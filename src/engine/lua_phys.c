// newen - physics props: entities moved by Jolt, and Lua's physics calls
//
// Any entity can be given a physics body (ent:PhysicsInit): from then on Jolt moves it, and each frame its
// edict gets the body's position and angles and an upright box round its shape, which is all Half-Life's
// own collision understands, so the player and monsters bump into it, stand on it and shoot it.
// prop_physics (base/lua/entities/prop_physics.lua) is the scripted entity built on this.
//
// The player pushes a prop by walking into it: Half-Life stops the player at the prop's box, so the push
// is a force along the way the player is trying to go (phys_pushforce), which heavy things resist.
#include "server.h"
#include "phys.h"
#include "src_local.h"
#include "lua_local.h"

#define PHYSOBJ_MT    "newen.PhysObj"
#define PROP_FRICTION 0.6f
#define PROP_DENSITY  400.0f     // kg per cubic metre, for a model with no physics file (wood)
#define NEWTON        (1.0f / 0.0254f)   // one newton in the game's units (kg inches/s^2)

typedef struct {
	int        serial;          // serialnumber + 1 of the edict it belongs to; 0 = none
	physbody_t body;
	qboolean   motion;          // false: held still (EnableMotion)
	qboolean   wasawake;
	double     blasted;         // the map's time when an explosion last threw it; 0 = never
	double     nexthit;         // it hurts nobody before this (one blow per impact, not one per frame)
} prop_t;

static prop_t *props;
static int     props_cap;
static cvar_t *phys_pushforce, *phys_impactspeed, *phys_impactdamage;

static prop_t *PropFor(const edict_t *e)
{
	if (!e || e->free || !props || !sv.edicts)
		return NULL;
	int i = NUM_FOR_EDICT(e);
	if (i <= 0 || i >= props_cap || props[i].serial != e->serialnumber + 1)
		return NULL;
	return &props[i];
}

// Studio models are drawn with their pitch the other way round (as GoldSrc does); the physics has it straight
static void BodyAngles(const vec3_t entangles, vec3_t out)
{
	out[0] = -entangles[0];
	out[1] = entangles[1];
	out[2] = entangles[2];
}

// the body's place into the edict: position, angles, speed, and the upright box round its shape
static void SyncEdict(edict_t *e, prop_t *p, physworld_t *w)
{
	vec3_t org, ang, mins, maxs;
	Phys_GetPose(w, p->body, org, ang);
	VectorCopy(org, e->v.origin);
	BodyAngles(ang, e->v.angles);
	if (p->motion) {
		Phys_GetVelocity(w, p->body, e->v.velocity);
		Phys_GetSpin(w, p->body, e->v.avelocity);   // kept for saves; MOVETYPE_NONE doesn't use it
	} else {
		VectorClear(e->v.velocity);
		VectorClear(e->v.avelocity);
	}
	Phys_GetBounds(w, p->body, mins, maxs);
	for (int k = 0; k < 3; k++) {
		mins[k] -= org[k];
		maxs[k] -= org[k];
	}
	SV_SetMinMaxSize(e, mins, maxs);   // links it
}

static void DestroyProp(prop_t *p)
{
	physworld_t *w = Phys_ServerWorld(false);
	if (w && p->serial)
		Phys_RemoveBody(w, p->body);
	memset(p, 0, sizeof(*p));
}

// A body for e from its model's physics file, else from a box (boxmins NULL: the model's own).
// mass 0 = the file's, or from the box's size.
static qboolean CreateProp(edict_t *e, float mass, const float *boxmins, const float *boxmaxs)
{
	physworld_t *w = Phys_ServerWorld(true);
	if (!w)
		return false;
	int num = NUM_FOR_EDICT(e);
	if (num >= props_cap) {
		int cap = sv.max_edicts > num ? sv.max_edicts : num + 1;
		props = Mem_Realloc(props, sizeof(*props) * (size_t)cap);
		memset(props + props_cap, 0, sizeof(*props) * (size_t)(cap - props_cap));
		props_cap = cap;
	}
	prop_t *p = &props[num];
	if (p->serial)
		DestroyProp(p);

	model_t *m = SV_ModelForIndex(e->v.modelindex);
	const srcphys_t *ph = !boxmins && m && m->type == mod_studio && m->studio.source ? m->studio.source->phys : NULL;
	vec3_t ang;
	BodyAngles(e->v.angles, ang);
	physbody_t b = PHYS_NOBODY;
	// one solid in the model's own space; a jointed model's solids are in their bones' spaces (a ragdoll)
	if (ph && ph->numsolids == 1 && ph->solids[0].numpieces) {
		const srcphysolid_t *so = &ph->solids[0];
		if (mass <= 0)
			mass = so->mass;
		b = Phys_AddConvexSet(w, so->points, so->counts, so->numpieces, e->v.origin, ang, PHYS_DYNAMIC, mass > 0 ? mass : 10);
	}
	if (b == PHYS_NOBODY) {
		vec3_t mins, maxs;
		if (boxmins) {
			VectorCopy(boxmins, mins);
			VectorCopy(boxmaxs, maxs);
		} else if (m && m->type != mod_brush) {
			VectorCopy(m->mins, mins);
			VectorCopy(m->maxs, maxs);
		} else {
			VectorCopy(e->v.mins, mins);
			VectorCopy(e->v.maxs, maxs);
		}
		for (int k = 0; k < 3; k++)
			if (maxs[k] - mins[k] < 1) {
				mins[k] -= 4;
				maxs[k] += 4;
			}
		float pts[24];
		for (int c = 0; c < 8; c++)
			for (int k = 0; k < 3; k++)
				pts[c * 3 + k] = c & (1 << k) ? maxs[k] : mins[k];
		int count = 8;
		if (mass <= 0) {
			float vol = (maxs[0] - mins[0]) * (maxs[1] - mins[1]) * (maxs[2] - mins[2]) * (0.0254f * 0.0254f * 0.0254f);
			mass = vol * PROP_DENSITY;
			mass = mass < 1 ? 1 : mass > 500 ? 500 : mass;
		}
		b = Phys_AddConvexSet(w, pts, &count, 1, e->v.origin, ang, PHYS_DYNAMIC, mass);
	}
	if (b == PHYS_NOBODY)
		return false;
	p->serial = e->serialnumber + 1;
	p->body = b;
	p->motion = true;
	p->wasawake = true;
	Phys_SetTag(w, b, num);
	Phys_SetFriction(w, b, PROP_FRICTION);
	// it starts with the speed the entity has (a thrown prop, a restored save)
	Phys_SetMotion(w, b, e->v.velocity, e->v.avelocity);
	// Half-Life leaves it alone: the physics moves it
	e->v.movetype = MOVETYPE_NONE;
	if (e->v.solid == SOLID_NOT)
		e->v.solid = SOLID_BBOX;
	SyncEdict(e, p, w);
	return true;
}

// ---------------------------------------------------------------- each frame

// the way a player is trying to walk, flat; false if no movement key is down
static qboolean WishDir(const edict_t *pl, vec3_t dir)
{
	int b = pl->v.button;
	float fwd = (b & IN_FORWARD ? 1.0f : 0) - (b & IN_BACK ? 1.0f : 0);
	float side = (b & IN_MOVERIGHT ? 1.0f : 0) - (b & IN_MOVELEFT ? 1.0f : 0);
	if (fwd == 0 && side == 0)
		return false;
	float yaw = DEG2RAD(pl->v.v_angle[1]);
	dir[0] = cosf(yaw) * fwd + sinf(yaw) * side;
	dir[1] = sinf(yaw) * fwd - cosf(yaw) * side;
	dir[2] = 0;
	VectorNormalize(dir);
	return true;
}

// before the step: players lean on the props in their way
void Prop_BeforeStep(float dt)
{
	physworld_t *w = Phys_ServerWorld(false);
	if (!w || !props || phys_pushforce->value <= 0)
		return;
	for (int c = 1; c <= sv.maxclients; c++) {
		edict_t *pl = EDICT_NUM(c);
		vec3_t dir;
		if (pl->free || pl->v.health <= 0 || pl->v.movetype == MOVETYPE_NOCLIP || !WishDir(pl, dir))
			continue;
		for (int i = sv.maxclients + 1; i < sv.num_edicts && i < props_cap; i++) {
			edict_t *e = EDICT_NUM(i);
			prop_t *p = PropFor(e);
			if (!p || !p->motion)
				continue;
			// touching the player's box sideways (not one they stand on, not one overhead)
			qboolean touching = true;   // ("near" is a word Windows headers define away)
			for (int k = 0; k < 3 && touching; k++)
				touching = e->v.absmin[k] <= pl->v.absmax[k] + 2 && e->v.absmax[k] >= pl->v.absmin[k] - 2;
			if (!touching || e->v.absmax[2] <= pl->v.absmin[2] + 4 || e->v.absmin[2] >= pl->v.absmax[2] - 4)
				continue;
			vec3_t to, vel;
			for (int k = 0; k < 2; k++)
				to[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f - pl->v.origin[k];
			to[2] = 0;
			if (VectorNormalize(to) < 0.01f || DotProduct(to, dir) < 0.3f)
				continue;
			// no faster than the player walks
			Phys_GetVelocity(w, p->body, vel);
			if (DotProduct(vel, dir) > pl->v.maxspeed && pl->v.maxspeed > 0)
				continue;
			vec3_t push;
			VectorScale(dir, phys_pushforce->value * NEWTON * dt, push);
			Phys_AddImpulse(w, p->body, push);
		}
	}
}

// A prop flying into a monster hurts it: Jolt knows nothing of Half-Life's monsters, so this looks for
// living ones whose box the prop's box has reached while it moves towards them. The damage grows with the
// prop's mass and its speed over phys_impactspeed; the prop loses most of its speed, as if it had hit.
static void HitMonsters(edict_t *e, prop_t *p, physworld_t *w)
{
	if (!p->motion || phys_impactdamage->value <= 0 || sv.time < p->nexthit || !SV_GameObjReady())
		return;
	vec3_t vel, centre;
	Phys_GetVelocity(w, p->body, vel);
	float speed = VectorLength(vel);
	if (speed <= phys_impactspeed->value)
		return;
	for (int k = 0; k < 3; k++)
		centre[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
	float damage = Phys_GetMass(w, p->body) * (speed - phys_impactspeed->value) * 0.01f * phys_impactdamage->value;
	qboolean hit = false;
	for (int i = sv.maxclients + 1; i < sv.num_edicts; i++) {
		edict_t *m = EDICT_NUM(i);
		if (m->free || m == e || !(m->v.flags & FL_MONSTER) || m->v.health <= 0 || m->v.deadflag != 0 ||
			m->v.takedamage == 0 || !m->pvPrivateData)
			continue;
		qboolean touching = true;
		vec3_t to;
		for (int k = 0; k < 3; k++) {
			if (e->v.absmin[k] > m->v.absmax[k] + 2 || e->v.absmax[k] < m->v.absmin[k] - 2)
				touching = false;
			to[k] = (m->v.absmin[k] + m->v.absmax[k]) * 0.5f - centre[k];
		}
		if (!touching || DotProduct(to, vel) <= 0)
			continue;
		// never gibbed: what it kills goes limp and flies on with the prop's speed
		// (a 30 kg crate doesn't carry an 80 kg body off at its own speed)
		vec3_t kick;
		float mass = Phys_GetMass(w, p->body);
		VectorScale(vel, mass / (mass + 60.0f), kick);
		Ragdoll_Kick(i, kick);
		SV_GameObjTakeDamage(m, e, e, damage, DMG_CRUSH | DMG_NEVERGIB);
		hit = true;
		if (e->free)
			return;
	}
	if (hit) {
		VectorScale(vel, 0.3f, vel);
		Phys_SetVelocity(w, p->body, vel);
		p->nexthit = sv.time + 0.3;
	}
}

// ENT:PhysicsCollide(data) on a scripted entity, then the PhysicsCollide(ent, data) hook
static void TellCollide(lua_State *L, edict_t *e, edict_t *other, const physcontact_t *c, float sign)
{
	int top = lua_gettop(L);
	lua_createtable(L, 0, 4);
	LuaG_PushEntity(L, other ? other : EDICT_NUM(0));
	lua_setfield(L, -2, "HitEntity");
	LuaG_PushVector(L, c->pos);
	lua_setfield(L, -2, "HitPos");
	vec3_t n = { c->normal[0] * sign, c->normal[1] * sign, c->normal[2] * sign };
	LuaG_PushVector(L, n);
	lua_setfield(L, -2, "HitNormal");
	lua_pushnumber(L, c->speed);
	lua_setfield(L, -2, "Speed");
	int data = lua_gettop(L);
	if (LuaE_PushTable(L, e)) {
		lua_getfield(L, -1, "PhysicsCollide");
		if (lua_isfunction(L, -1)) {
			LuaG_PushEntity(L, e);
			lua_pushvalue(L, data);
			gGlobals.time = (float)sv.time;
			Lua_Call(2, 0, "ENT:PhysicsCollide");
		}
	}
	lua_settop(L, data);
	if (!e->free) {
		LuaG_PushEntity(L, e);
		lua_pushvalue(L, data);
		Lua_Hook("PhysicsCollide", 2, 0);
	}
	lua_settop(L, top);
}

// after the step: the edicts follow their bodies; Lua hears what hit what
void Prop_AfterStep(void)
{
	physworld_t *w = Phys_ServerWorld(false);
	if (!w)
		return;
	for (int i = 1; i < sv.num_edicts && i < props_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		prop_t *p = PropFor(e);
		if (!p) {
			if (i < props_cap && props[i].serial)
				DestroyProp(&props[i]);   // the edict went without telling us
			continue;
		}
		qboolean awake = Phys_IsAwake(w, p->body);
		if (awake || p->wasawake)
			SyncEdict(e, p, w);
		p->wasawake = awake;
		if (awake) {
			HitMonsters(e, p, w);
			if (e->free || !(p = PropFor(e)))
				continue;
		}
		// one that left the map (spawned outside it, or squeezed through a wall) would fall for ever
		const model_t *wm = SV_ModelForIndex(1);
		for (int k = 0; k < 3 && wm; k++)
			if (e->v.origin[k] < wm->mins[k] - 512 || e->v.origin[k] > wm->maxs[k] + 512) {
				Con_DPrintf("Physics: %s (entity %d) left the map and was removed\n", SV_String(e->v.classname), i);
				ED_Free(e);
				break;
			}
	}
	physcontact_t hits[64];
	int n = Phys_TakeContacts(w, hits, 64);
	lua_State *L = Lua_GetState();
	for (int i = 0; i < n && L; i++) {
		const physcontact_t *c = &hits[i];
		if (c->speed < 30)
			continue;   // resting and scraping aren't hits
		edict_t *a = c->tag1 > 0 && c->tag1 < sv.num_edicts ? EDICT_NUM(c->tag1) : NULL;
		edict_t *b = c->tag2 > 0 && c->tag2 < sv.num_edicts ? EDICT_NUM(c->tag2) : NULL;
		if (!PropFor(a))
			a = NULL;
		if (!PropFor(b))
			b = NULL;
		if (a)
			TellCollide(L, a, b, c, 1);
		if (b && PropFor(b))
			TellCollide(L, b, a && PropFor(a) ? a : NULL, c, -1);
	}
}

void Prop_Blasted(const float pos[3], float radius)
{
	for (int i = 1; i < sv.num_edicts && i < props_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		prop_t *p = PropFor(e);
		if (!p)
			continue;
		vec3_t d;
		for (int k = 0; k < 3; k++)
			d[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f - pos[k];
		if (VectorLength(d) < radius + 64)
			p->blasted = sv.time > 0 ? sv.time : 0.001;
	}
}

void Prop_EdictFreed(edict_t *e)
{
	if (!props || !sv.edicts)
		return;
	int i = NUM_FOR_EDICT(e);
	if (i > 0 && i < props_cap && props[i].serial)
		DestroyProp(&props[i]);
}

// the map is going (its physics world with it)
void Prop_Clear(void)
{
	if (props)
		memset(props, 0, sizeof(*props) * (size_t)props_cap);
}

// ---------------------------------------------------------------- Lua: physics objects

typedef struct {
	int num, serial;
} physobj_t;

// the prop behind a physics object, or NULL once its entity or body is gone
static prop_t *ObjProp(lua_State *L, int idx, edict_t **ent)
{
	physobj_t *o = luaL_checkudata(L, idx, PHYSOBJ_MT);
	if ((!sv.active && !sv.loading) || o->num <= 0 || o->num >= sv.num_edicts)
		return NULL;
	edict_t *e = EDICT_NUM(o->num);
	prop_t *p = PropFor(e);
	if (!p || p->serial != o->serial || !Phys_ServerWorld(false))
		return NULL;
	if (ent)
		*ent = e;
	return p;
}

static prop_t *CheckObj(lua_State *L, edict_t **ent)
{
	prop_t *p = ObjProp(L, 1, ent);
	if (!p)
		luaL_error(L, "the physics object is no longer valid");
	return p;
}

#define W Phys_ServerWorld(false)

static int o_IsValid(lua_State *L)
{
	lua_pushboolean(L, ObjProp(L, 1, NULL) != NULL);
	return 1;
}

static int o_GetEntity(lua_State *L)
{
	edict_t *e = NULL;
	ObjProp(L, 1, &e);
	LuaG_PushEntity(L, e);
	return 1;
}

static int o_ApplyForceCenter(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	if (p->motion)
		Phys_AddImpulse(W, p->body, LuaG_CheckVec(L, 2));
	return 0;
}

static int o_ApplyForceOffset(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	if (p->motion)
		Phys_AddImpulseAt(W, p->body, LuaG_CheckVec(L, 2), LuaG_CheckVec(L, 3));
	return 0;
}

static int o_SetVelocity(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	if (p->motion) {
		Phys_SetVelocity(W, p->body, LuaG_CheckVec(L, 2));
		Phys_Wake(W, p->body);
	}
	return 0;
}

static int o_AddVelocity(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	if (p->motion) {
		vec3_t v;
		Phys_GetVelocity(W, p->body, v);
		VectorAdd(v, LuaG_CheckVec(L, 2), v);
		Phys_SetVelocity(W, p->body, v);
		Phys_Wake(W, p->body);
	}
	return 0;
}

static int o_GetVelocity(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	vec3_t v = { 0, 0, 0 };
	if (p->motion)
		Phys_GetVelocity(W, p->body, v);
	LuaG_PushVector(L, v);
	return 1;
}

static int o_EnableMotion(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	qboolean on = lua_toboolean(L, 2);
	if (on != p->motion) {
		Phys_EnableMotion(W, p->body, on);
		p->motion = on;
		p->wasawake = true;
	}
	return 0;
}

static int o_IsMotionEnabled(lua_State *L)
{
	lua_pushboolean(L, CheckObj(L, NULL)->motion);
	return 1;
}

static int o_Wake(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	if (p->motion)
		Phys_Wake(W, p->body);
	return 0;
}

static int o_Sleep(lua_State *L)
{
	Phys_Sleep(W, CheckObj(L, NULL)->body);
	return 0;
}

static int o_IsAsleep(lua_State *L)
{
	lua_pushboolean(L, !Phys_IsAwake(W, CheckObj(L, NULL)->body));
	return 1;
}

static int o_GetMass(lua_State *L)
{
	lua_pushnumber(L, Phys_GetMass(W, CheckObj(L, NULL)->body));
	return 1;
}

static int o_SetMass(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	float mass = (float)luaL_checknumber(L, 2);
	luaL_argcheck(L, mass > 0, 2, "a mass above 0");
	Phys_SetMass(W, p->body, mass);
	return 0;
}

static int o_GetPos(lua_State *L)
{
	vec3_t v;
	Phys_GetPose(W, CheckObj(L, NULL)->body, v, NULL);
	LuaG_PushVector(L, v);
	return 1;
}

static int o_GetMassCenter(lua_State *L)
{
	vec3_t v;
	Phys_GetMassCenter(W, CheckObj(L, NULL)->body, v);
	LuaG_PushVector(L, v);
	return 1;
}

// the map's time (ServerTime) when an explosion last threw it, or -1: its damage in the same frame is
// the blast's, not a shot's
static int o_LastBlastTime(lua_State *L)
{
	prop_t *p = CheckObj(L, NULL);
	lua_pushnumber(L, p->blasted > 0 ? p->blasted : -1);
	return 1;
}

static int o_tostring(lua_State *L)
{
	physobj_t *o = luaL_checkudata(L, 1, PHYSOBJ_MT);
	lua_pushfstring(L, ObjProp(L, 1, NULL) ? "PhysObj [%d]" : "PhysObj [NULL]", o->num);
	return 1;
}

// ---------------------------------------------------------------- Lua: entity methods

static void TeleportProp(edict_t *e)
{
	prop_t *p = PropFor(e);
	physworld_t *w = Phys_ServerWorld(false);
	if (!p || !w)
		return;
	vec3_t ang;
	BodyAngles(e->v.angles, ang);
	Phys_SetPose(w, p->body, e->v.origin, ang);
	p->wasawake = true;
}

// ent:SetPos / SetAngles on an entity with a body move the body (lua_game.c calls this)
void Prop_EdictMoved(edict_t *e)
{
	TeleportProp(e);
}

static int m_PhysicsInit(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	luaL_argcheck(L, NUM_FOR_EDICT(e) > sv.maxclients, 1, "not the world or a player");
	// GMod passes a solid type first; a number here is the mass in kilograms (0 or nothing: the model's)
	float mass = (float)luaL_optnumber(L, 2, 0);
	lua_pushboolean(L, CreateProp(e, mass, NULL, NULL));
	return 1;
}

static int m_PhysicsInitBox(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	luaL_argcheck(L, NUM_FOR_EDICT(e) > sv.maxclients, 1, "not the world or a player");
	const float *mins = LuaG_CheckVec(L, 2), *maxs = LuaG_CheckVec(L, 3);
	for (int k = 0; k < 3; k++)
		luaL_argcheck(L, maxs[k] > mins[k], 3, "maxs above mins");
	lua_pushboolean(L, CreateProp(e, (float)luaL_optnumber(L, 4, 0), mins, maxs));
	return 1;
}

static int m_PhysicsDestroy(lua_State *L)
{
	prop_t *p = PropFor(LuaG_CheckEdict(L, 1));
	if (p)
		DestroyProp(p);
	return 0;
}

static int m_GetPhysicsObject(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	prop_t *p = PropFor(e);
	physobj_t *o = lua_newuserdatauv(L, sizeof(*o), 0);
	o->num = p ? NUM_FOR_EDICT(e) : 0;
	o->serial = p ? p->serial : 0;
	luaL_setmetatable(L, PHYSOBJ_MT);
	return 1;
}

static int m_BecomeRagdoll(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	lua_pushboolean(L, Ragdoll_Force(NUM_FOR_EDICT(e)));
	return 1;
}

static int m_WorldSpaceCenter(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	vec3_t c;
	for (int k = 0; k < 3; k++)
		c[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
	LuaG_PushVector(L, c);
	return 1;
}

void LuaP_Open(lua_State *L)
{
	static const luaL_Reg obj[] = {
		{ "IsValid", o_IsValid }, { "GetEntity", o_GetEntity }, { "ApplyForceCenter", o_ApplyForceCenter },
		{ "ApplyForceOffset", o_ApplyForceOffset }, { "SetVelocity", o_SetVelocity }, { "AddVelocity", o_AddVelocity },
		{ "GetVelocity", o_GetVelocity }, { "EnableMotion", o_EnableMotion }, { "IsMotionEnabled", o_IsMotionEnabled },
		{ "Wake", o_Wake }, { "Sleep", o_Sleep }, { "IsAsleep", o_IsAsleep }, { "GetMass", o_GetMass },
		{ "SetMass", o_SetMass }, { "GetPos", o_GetPos }, { "GetMassCenter", o_GetMassCenter },
		{ "LastBlastTime", o_LastBlastTime }, { NULL, NULL }
	};
	luaL_newmetatable(L, PHYSOBJ_MT);
	lua_newtable(L);
	luaL_setfuncs(L, obj, 0);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, o_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	static const luaL_Reg methods[] = {
		{ "PhysicsInit", m_PhysicsInit }, { "PhysicsInitBox", m_PhysicsInitBox }, { "PhysicsDestroy", m_PhysicsDestroy },
		{ "GetPhysicsObject", m_GetPhysicsObject }, { "BecomeRagdoll", m_BecomeRagdoll },
		{ "WorldSpaceCenter", m_WorldSpaceCenter }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);

	// physics.SpawnProp is added by prop_physics.lua
	lua_newtable(L);
	lua_setglobal(L, "physics");
}

void Prop_Init(void)
{
	phys_impactspeed = Cvar_Get("phys_impactspeed", "250", FCVAR_ARCHIVE,
		"a prop moving faster than this (units a second) hurts the monsters it flies into");
	phys_impactdamage = Cvar_Get("phys_impactdamage", "1", FCVAR_ARCHIVE,
		"scales that damage: the prop's mass in kilograms x each 100 units a second over the limit (0 = none)");
	phys_pushforce = Cvar_Get("phys_pushforce", "500", FCVAR_ARCHIVE,
		"how hard the player pushes a physics prop by walking into it, in newtons (0 = not at all)");
}
