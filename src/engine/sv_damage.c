// newen - damage the engine deals for Lua (and later Lua weapons), through the game's own rules
//
// Half-Life's bullets go TraceAttack > AddMultiDamage > ApplyMultiDamage inside hl.dll, and the last two
// aren't reachable from outside. So the engine does what that chain does around the one call it can make
// (TakeDamage): the trace (with hitboxes), the hit group's multiplier from the game's own skill cvars, blood,
// decals and impact effects. Armor, god mode, pain, death and gibs are the game's, through TakeDamage.
#include "cl_local.h"   // the effect functions: newen's game and client share one process

static const vec3_t zero = { 0, 0, 0 };

// Hit group multipliers: the game's sk_<monster|player>_<part><skill> cvars, read live (the game sets
// them from skill.cfg); Half-Life's defaults when a cvar is missing
static float HitGroupScale(const edict_t *victim, int hitgroup)
{
	static const char *const parts[8] = { NULL, "head", "chest", "stomach", "arm", "arm", "leg", "leg" };
	static const float defaults[8] = { 1, 3, 1, 1, 1, 1, 1, 1 };
	if (hitgroup <= 0 || hitgroup > 7 || !(victim->v.flags & (FL_MONSTER | FL_CLIENT)))
		return 1;
	int skill = (int)Cvar_VariableValue("skill");
	if (skill < 1 || skill > 3)
		skill = 1;
	float v = Cvar_VariableValue(va("sk_%s_%s%d", (victim->v.flags & FL_CLIENT) ? "player" : "monster", parts[hitgroup], skill));
	return v > 0 ? v : defaults[hitgroup];
}

// The decal on the surface behind a bleeding body, or the bullet hole on a wall
static void ImpactDecal(const vec3_t pos, const vec3_t dir, edict_t *ignore, qboolean blood, int color)
{
	vec3_t end;
	VectorMA(pos, blood ? 172 : 8, dir, end);
	vec3_t start;
	VectorMA(pos, -2, dir, start);
	trace_t tr = SV_Move(start, zero, zero, end, MOVE_NOMONSTERS, ignore);
	if (tr.fraction == 1 || !tr.ent || tr.ent->v.solid != SOLID_BSP)
		return;
	const char *name = blood ? va("{%sblood%d", color == 247 ? "" : "y", 1 + rand() % 6) : va("{shot%d", 1 + rand() % 5);
	int tex = R_DecalIndex(name);
	if (tex >= 0)
		R_DecalShoot(tex, NUM_FOR_EDICT(tr.ent), 0, tr.endpos, 0, 1.0f);
}

void SV_FireBullets(const svbullets_t *b, svbullet_hit_t on_hit, void *ctx)
{
	vec3_t dir, right, up;
	VectorCopy(b->dir, dir);
	if (VectorNormalize(dir) == 0)
		return;
	// any two directions square to the shot
	vec3_t any = { 0, 0, 1 };
	if (fabsf(dir[2]) > 0.9f)
		VectorSet(any, 1, 0, 0);
	CrossProduct(dir, any, right);
	VectorNormalize(right);
	CrossProduct(right, dir, up);
	float distance = b->distance > 0 ? b->distance : 8192;
	int num = b->num > 0 ? b->num : 1;

	for (int i = 0; i < num; i++) {
		// two random numbers added: more shots near the middle of the cone, as Half-Life spreads them
		float x, y, z;
		do {
			x = ((float)rand() / RAND_MAX - 0.5f) + ((float)rand() / RAND_MAX - 0.5f);
			y = ((float)rand() / RAND_MAX - 0.5f) + ((float)rand() / RAND_MAX - 0.5f);
			z = x * x + y * y;
		} while (z > 1);
		vec3_t shot, end;
		for (int k = 0; k < 3; k++)
			shot[k] = dir[k] + x * b->spread_x * right[k] + y * b->spread_y * up[k];
		VectorNormalize(shot);
		VectorMA(b->src, distance, shot, end);
		trace_t tr = SV_Move(b->src, zero, zero, end, MOVE_NORMAL | 0x100, b->ignore ? b->ignore : b->attacker);
		if (b->tracer_every > 0 && i % b->tracer_every == 0) {
			vec3_t from;
			VectorMA(b->src, 32, shot, from);
			R_TracerEffect(from, tr.endpos);
		}
		if (tr.fraction == 1)
			continue;
		edict_t *hit = tr.ent;
		float damage = b->damage;
		if (hit && hit != sv.edicts && hit->v.takedamage != 0)
			damage *= HitGroupScale(hit, tr.hitgroup);
		int blood = hit ? SV_GameObjBloodColor(hit) : -1;
		if (blood >= 0 && damage > 0) {
			vec3_t back;
			VectorScale(shot, -1, back);
			R_Blood(tr.endpos, back, blood, (int)(damage > 40 ? 120 : damage * 3));
			ImpactDecal(tr.endpos, shot, hit, true, blood);
		} else {
			R_BulletImpactParticles(tr.endpos);
			ImpactDecal(tr.endpos, shot, NULL, false, 0);
		}
		if (on_hit)
			on_hit(&tr, damage, ctx);
		if (hit && hit != sv.edicts && damage > 0)
			SV_GameObjTakeDamage(hit, b->attacker, b->attacker, damage, DMG_BULLET | DMG_NEVERGIB);
	}
}

// Damage to everything within radius that can see pos, falling off with distance, as Half-Life's explosions
// do. Returns how many things were hurt.
// Without method calls into the game, a hidden env_explosion (fireball, smoke, sparks and decal turned
// off) does the damage instead. Its radius is the game's own: 2.5 times its magnitude.
static void BlastWithExplosion(const vec3_t pos, float damage)
{
	static qboolean warned;
	if (!warned) {
		Con_Warnf("Blast damage: game method calls are off, using env_explosion (its radius is 2.5 x damage)\n");
		warned = true;
	}
	edict_t *e = SV_CreateNamedEntity("env_explosion");
	if (!e)
		return;
	VectorCopy(pos, e->v.origin);
	const char *keys[][2] = { { "iMagnitude", va("%d", (int)damage) }, { "spawnflags", "60" } };   // no fireball, smoke, decal, sparks
	for (int i = 0; i < 2; i++) {
		char cls[] = "env_explosion", key[32], value[32];
		Q_strncpy(key, keys[i][0], sizeof(key));
		Q_strncpy(value, keys[i][1], sizeof(value));
		KeyValueData kvd = { cls, key, value, 0 };
		gEntityInterface.pfnKeyValue(e, &kvd);
	}
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnSpawn(e);
	gEntityInterface.pfnUse(e, sv.edicts);
}

int SV_BlastDamage(edict_t *inflictor, edict_t *attacker, const vec3_t pos, float radius, float damage)
{
	if (radius <= 0 || damage <= 0)
		return 0;
	if (!SV_GameObjReady()) {
		BlastWithExplosion(pos, damage);
		return -1;
	}
	// the victims first: damage can kill and free entities while the list is walked
	int count = 0, cap = 0;
	edict_t **victims = NULL;
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || e->v.takedamage == 0 || !e->pvPrivateData)
			continue;
		vec3_t c;
		for (int k = 0; k < 3; k++)
			c[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
		vec3_t d;
		VectorSubtract(c, pos, d);
		if (VectorLength(d) > radius + 32)
			continue;
		if (count == cap) {
			cap = cap ? cap * 2 : 16;
			victims = Mem_Realloc(victims, sizeof(*victims) * cap);
		}
		victims[count++] = e;
	}
	int hurt = 0;
	for (int i = 0; i < count; i++) {
		edict_t *e = victims[i];
		if (e->free)
			continue;
		vec3_t c;
		for (int k = 0; k < 3; k++)
			c[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
		trace_t tr = SV_Move(pos, zero, zero, c, MOVE_NOMONSTERS, inflictor);
		if (tr.fraction < 1 && tr.ent != e)
			continue;   // a wall in the way
		vec3_t d;
		VectorSubtract(tr.endpos, pos, d);
		float amount = damage * (1 - VectorLength(d) / radius);
		if (amount <= 0)
			continue;
		if (SV_GameObjTakeDamage(e, inflictor, attacker, amount, DMG_BLAST))
			hurt++;
	}
	Mem_Free(victims);
	return hurt;
}
