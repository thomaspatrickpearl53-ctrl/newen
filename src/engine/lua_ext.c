// newen - Lua: the smaller game calls (Lua API plan step 6): bones and attachments, body groups, playback,
// map triggers, finding entities in a cone or along a ray, player speed, freezing, stripping, messages, and
// the hooks KeyPress/KeyRelease, OnNPCKilled and CalcView
#include "lua_local.h"
#include "server.h"

#define EF_NODRAW 128

static int m_GetAttachment(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	int i = (int)luaL_checkinteger(L, 2);
	rentity_t r;
	if (!SV_EdictToRentity(e, &r))
		return 0;
	const studiohdr_t *h = Mod_StudioHeader(r.model);
	if (!h || i < 0 || i >= h->numattachments)
		return 0;
	const mstudioattachment_t *a = STUDIO_PTR(h, h->attachmentindex, mstudioattachment_t) + i;
	float (*bones)[3][4];
	int n = R_StudioSetupBones(&r, &bones);
	if (a->bone < 0 || a->bone >= n)
		return 0;
	vec3_t pos;
	for (int k = 0; k < 3; k++)
		pos[k] = DotProduct(a->org, bones[a->bone][k]) + bones[a->bone][k][3];
	lua_newtable(L);
	LuaG_PushVector(L, pos);
	lua_setfield(L, -2, "Pos");
	return 1;
}

static int FindNamed(const studiohdr_t *h, int count, int index, size_t size, size_t nameofs, const char *name)
{
	for (int i = 0; i < count; i++)
		if (!stricmp((const char *)h + index + (size_t)i * size + nameofs, name))
			return i;
	return -1;
}

// ent:LookupBone(name), ent:LookupAttachment(name): their numbers, or -1
static int m_LookupBone(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const studiohdr_t *h = Mod_StudioHeader(SV_ModelForIndex(e->v.modelindex));
	lua_pushinteger(L, h ? FindNamed(h, h->numbones, h->boneindex, sizeof(mstudiobone_t), 0, luaL_checkstring(L, 2)) : -1);
	return 1;
}

static int m_LookupAttachment(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const studiohdr_t *h = Mod_StudioHeader(SV_ModelForIndex(e->v.modelindex));
	lua_pushinteger(L, h ? FindNamed(h, h->numattachments, h->attachmentindex, sizeof(mstudioattachment_t), 0,
		luaL_checkstring(L, 2)) : -1);
	return 1;
}

// ent:GetBonePosition(bone number or name): where the bone is now
static int m_GetBonePosition(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	rentity_t r;
	if (!SV_EdictToRentity(e, &r))
		return 0;
	const studiohdr_t *h = Mod_StudioHeader(r.model);
	int b = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) :
		FindNamed(h, h->numbones, h->boneindex, sizeof(mstudiobone_t), 0, luaL_checkstring(L, 2));
	float (*bones)[3][4];
	int n = R_StudioSetupBones(&r, &bones);
	if (b < 0 || b >= n)
		return 0;
	vec3_t pos = { bones[b][0][3], bones[b][1][3], bones[b][2][3] };
	LuaG_PushVector(L, pos);
	return 1;
}

// ent:SetBodygroup(group, value) / GetBodygroup(group) / GetBodygroupCount(group): body parts, as the game's
// SetBodygroup packs them into the body number
static const mstudiobodyparts_t *BodyPart(edict_t *e, int group)
{
	const studiohdr_t *h = Mod_StudioHeader(SV_ModelForIndex(e->v.modelindex));
	if (!h || group < 0 || group >= h->numbodyparts)
		return NULL;
	return STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t) + group;
}

static int m_SetBodygroup(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const mstudiobodyparts_t *bp = BodyPart(e, (int)luaL_checkinteger(L, 2));
	int value = (int)luaL_checkinteger(L, 3);
	if (!bp || value < 0 || value >= bp->nummodels)
		return 0;
	int base = bp->base > 0 ? bp->base : 1;
	int current = (e->v.body / base) % bp->nummodels;
	e->v.body = e->v.body - current * base + value * base;
	return 0;
}

static int m_GetBodygroup(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const mstudiobodyparts_t *bp = BodyPart(e, (int)luaL_checkinteger(L, 2));
	lua_pushinteger(L, bp && bp->nummodels ? (e->v.body / (bp->base > 0 ? bp->base : 1)) % bp->nummodels : 0);
	return 1;
}

static int m_GetBodygroupCount(lua_State *L)
{
	const mstudiobodyparts_t *bp = BodyPart(LuaG_CheckEdict(L, 1), (int)luaL_checkinteger(L, 2));
	lua_pushinteger(L, bp ? bp->nummodels : 0);
	return 1;
}

static int m_GetNumBodyGroups(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const studiohdr_t *h = Mod_StudioHeader(SV_ModelForIndex(e->v.modelindex));
	lua_pushinteger(L, h ? h->numbodyparts : 0);
	return 1;
}

static int m_SetPlaybackRate(lua_State *L) { LuaG_CheckEdict(L, 1)->v.framerate = (float)luaL_checknumber(L, 2); return 0; }
static int m_GetPlaybackRate(lua_State *L) { lua_pushnumber(L, LuaG_CheckEdict(L, 1)->v.framerate); return 1; }

static int m_SetNoDraw(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	if (lua_toboolean(L, 2))
		e->v.effects |= EF_NODRAW;
	else
		e->v.effects &= ~EF_NODRAW;
	return 0;
}

static int m_GetNoDraw(lua_State *L) { lua_pushboolean(L, (LuaG_CheckEdict(L, 1)->v.effects & EF_NODRAW) != 0); return 1; }

// ---------------------------------------------------------------- players

static int m_Freeze(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	if (lua_toboolean(L, 2))
		e->v.flags |= FL_FROZEN;
	else
		e->v.flags &= ~FL_FROZEN;
	return 0;
}

static int m_IsFrozen(lua_State *L) { lua_pushboolean(L, (LuaG_CheckEdict(L, 1)->v.flags & FL_FROZEN) != 0); return 1; }
static int m_SetMaxSpeed(lua_State *L) { LuaG_CheckEdict(L, 1)->v.maxspeed = (float)luaL_checknumber(L, 2); return 0; }
static int m_GetMaxSpeed(lua_State *L) { lua_pushnumber(L, LuaG_CheckEdict(L, 1)->v.maxspeed); return 1; }
static int m_GetViewModel(lua_State *L) { lua_pushstring(L, SV_String(LuaG_CheckEdict(L, 1)->v.viewmodel)); return 1; }

// ply:StripWeapons(): through the game's own player_weaponstrip, as a map would
static int m_StripWeapons(lua_State *L)
{
	edict_t *pl = LuaG_CheckEdict(L, 1);
	edict_t *s = SV_CreateNamedEntity("player_weaponstrip");
	if (!s)
		return 0;
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnSpawn(s);
	gEntityInterface.pfnUse(s, pl);
	s->v.flags |= FL_KILLME;
	return 0;
}

// ply:PrintMessage(HUD_PRINTCENTER / HUD_PRINTTALK / HUD_PRINTCONSOLE / HUD_PRINTNOTIFY, text)
static int m_PrintMessage(lua_State *L)
{
	LuaG_CheckEdict(L, 1);
	int type = (int)luaL_checkinteger(L, 2);
	const char *text = luaL_checkstring(L, 3);
	if (type == 4) {
		CL_CenterPrint(text);
	} else if (type == 3) {
		// the client's chat line: its own SayText message (player number, text)
		int msg = SV_UserMsgIndex("SayText");
		unsigned char buf[192];
		size_t n = strlen(text);
		if (n > sizeof(buf) - 3)
			n = sizeof(buf) - 3;
		buf[0] = 1;
		memcpy(buf + 1, text, n);
		buf[n + 1] = '\n';
		buf[n + 2] = 0;
		if (msg)
			CL_QueueMessage(msg, buf, (int)n + 3);
		else
			Con_Printf("%s\n", text);
	} else {
		Con_Printf("%s\n", text);
	}
	return 0;
}

// ---------------------------------------------------------------- finding

static void EntityCenter(const edict_t *e, vec3_t c)
{
	for (int k = 0; k < 3; k++)
		c[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
}

// ents.FindInCone(origin, direction, range, cosine): entities whose centre is within range and the cone
static int l_FindInCone(lua_State *L)
{
	vec3_t org, dir;
	VectorCopy(LuaG_CheckVec(L, 1), org);
	VectorCopy(LuaG_CheckVec(L, 2), dir);
	VectorNormalize(dir);
	float range = (float)luaL_checknumber(L, 3), cosine = (float)luaL_checknumber(L, 4);
	lua_newtable(L);
	int n = 0;
	for (int i = 1; sv.active && i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free)
			continue;
		vec3_t c, d;
		EntityCenter(e, c);
		VectorSubtract(c, org, d);
		float len = VectorLength(d);
		if (len > range || (len > 0 && DotProduct(d, dir) / len < cosine))
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// ents.FindAlongRay(start, endpos, [mins], [maxs]): entities whose box the segment (grown by mins/maxs) touches
static int l_FindAlongRay(lua_State *L)
{
	vec3_t a, b, mins = { 0, 0, 0 }, maxs = { 0, 0, 0 };
	VectorCopy(LuaG_CheckVec(L, 1), a);
	VectorCopy(LuaG_CheckVec(L, 2), b);
	if (!lua_isnoneornil(L, 3))
		VectorCopy(LuaG_CheckVec(L, 3), mins);
	if (!lua_isnoneornil(L, 4))
		VectorCopy(LuaG_CheckVec(L, 4), maxs);
	lua_newtable(L);
	int n = 0;
	for (int i = 1; sv.active && i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || e->v.solid == 0)
			continue;
		// slab test against the box grown by the ray's own size
		float t0 = 0, t1 = 1;
		qboolean hit = true;
		for (int k = 0; k < 3 && hit; k++) {
			float lo = e->v.absmin[k] - maxs[k], hi = e->v.absmax[k] - mins[k];
			float d = b[k] - a[k];
			if (fabsf(d) < 1e-6f) {
				hit = a[k] >= lo && a[k] <= hi;
				continue;
			}
			float u = (lo - a[k]) / d, v = (hi - a[k]) / d;
			if (u > v) { float t = u; u = v; v = t; }
			if (u > t0) t0 = u;
			if (v < t1) t1 = v;
			hit = t0 <= t1;
		}
		if (!hit)
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// ents.FireTargets(targetname, [activator]): uses every entity with that name, as a map's trigger does
static int l_FireTargets(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	edict_t *activator = lua_isnoneornil(L, 2) ? SV_LocalPlayer() : LuaG_ToEdict(L, 2);
	int fired = 0;
	gGlobals.time = (float)sv.time;
	for (int i = 1; sv.active && *name && i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->pvPrivateData || stricmp(SV_String(e->v.targetname), name))
			continue;
		gEntityInterface.pfnUse(e, activator ? activator : sv.edicts);
		fired++;
	}
	lua_pushinteger(L, fired);
	return 1;
}

// ---------------------------------------------------------------- hooks

static int prev_buttons;
static unsigned char *prev_dead;
static int prev_dead_cap;

// KeyPress(ply, IN_*) / KeyRelease(ply, IN_*) when the player's buttons change, and OnNPCKilled(npc, attacker)
// when a monster starts dying; once per server frame
void LuaX_Frame(void)
{
	lua_State *L = Lua_GetState();
	if (!L || !sv.active)
		return;
	int top = lua_gettop(L);
	edict_t *pl = SV_LocalPlayer();
	if (pl) {
		int now = pl->v.button, changed = now ^ prev_buttons;
		for (int bit = 0; changed && bit < 32; bit++) {
			if (!(changed & (1 << bit)))
				continue;
			LuaG_PushEntity(L, pl);
			lua_pushinteger(L, 1 << bit);
			Lua_Hook((now & (1 << bit)) ? "KeyPress" : "KeyRelease", 2, 0);
			lua_settop(L, top);
		}
		prev_buttons = now;
	}
	if (prev_dead_cap < sv.max_edicts) {
		prev_dead = Mem_Realloc(prev_dead, (size_t)sv.max_edicts);
		memset(prev_dead + prev_dead_cap, 0, (size_t)(sv.max_edicts - prev_dead_cap));
		prev_dead_cap = sv.max_edicts;
	}
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		unsigned char dead = !e->free && (e->v.flags & FL_MONSTER) && e->v.deadflag != 0;
		if (dead && !prev_dead[i]) {
			edict_t *inf = e->v.dmg_inflictor;
			edict_t *attacker = inf && !inf->free && inf->v.owner ? inf->v.owner : inf;
			LuaG_PushEntity(L, e);
			LuaG_PushEntity(L, attacker);
			Lua_Hook("OnNPCKilled", 2, 0);
			lua_settop(L, top);
		}
		prev_dead[i] = dead;
	}
}

void LuaX_MapChange(void)
{
	if (prev_dead)
		memset(prev_dead, 0, (size_t)prev_dead_cap);
	prev_buttons = 0;
}

// CalcView(ply, origin, angles, fov): return a table { origin, angles, fov } to move the camera this frame
float LuaX_CalcView(float *origin, float *angles, float fov)
{
	lua_State *L = Lua_GetState();
	edict_t *pl = SV_LocalPlayer();
	if (!L || !pl)
		return fov;
	int top = lua_gettop(L);
	LuaG_PushEntity(L, pl);
	LuaG_PushVector(L, origin);
	LuaG_PushAngle(L, angles);
	lua_pushnumber(L, fov);
	if (Lua_Hook("CalcView", 4, 1) == 1 && lua_istable(L, -1)) {
		lua_getfield(L, -1, "origin");
		if (!lua_isnil(L, -1))
			VectorCopy(LuaG_CheckVec(L, -1), origin);
		lua_getfield(L, -2, "angles");
		if (!lua_isnil(L, -1))
			VectorCopy(LuaG_CheckVec(L, -1), angles);
		lua_getfield(L, -3, "fov");
		if (lua_isnumber(L, -1))
			fov = (float)lua_tonumber(L, -1);
	}
	lua_settop(L, top);
	return fov;
}

void LuaX_Open(lua_State *L)
{
	static const luaL_Reg methods[] = {
		{ "GetAttachment", m_GetAttachment }, { "LookupBone", m_LookupBone }, { "LookupAttachment", m_LookupAttachment },
		{ "GetBonePosition", m_GetBonePosition }, { "SetBodygroup", m_SetBodygroup }, { "GetBodygroup", m_GetBodygroup },
		{ "GetBodygroupCount", m_GetBodygroupCount }, { "GetNumBodyGroups", m_GetNumBodyGroups },
		{ "SetPlaybackRate", m_SetPlaybackRate }, { "GetPlaybackRate", m_GetPlaybackRate },
		{ "SetNoDraw", m_SetNoDraw }, { "GetNoDraw", m_GetNoDraw }, { "Freeze", m_Freeze }, { "IsFrozen", m_IsFrozen },
		{ "SetMaxSpeed", m_SetMaxSpeed }, { "GetMaxSpeed", m_GetMaxSpeed }, { "GetViewModel", m_GetViewModel },
		{ "StripWeapons", m_StripWeapons }, { "PrintMessage", m_PrintMessage }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);

	lua_getglobal(L, "ents");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, l_FindInCone);
		lua_setfield(L, -2, "FindInCone");
		lua_pushcfunction(L, l_FindAlongRay);
		lua_setfield(L, -2, "FindAlongRay");
		lua_pushcfunction(L, l_FireTargets);
		lua_setfield(L, -2, "FireTargets");
	}
	lua_pop(L, 1);

	static const struct { const char *name; int value; } consts[] = {
		{ "HUD_PRINTNOTIFY", 1 }, { "HUD_PRINTCONSOLE", 2 }, { "HUD_PRINTTALK", 3 }, { "HUD_PRINTCENTER", 4 },
		{ "IN_ATTACK", 1 << 0 }, { "IN_JUMP", 1 << 1 }, { "IN_DUCK", 1 << 2 }, { "IN_FORWARD", 1 << 3 },
		{ "IN_BACK", 1 << 4 }, { "IN_USE", 1 << 5 }, { "IN_CANCEL", 1 << 6 }, { "IN_LEFT", 1 << 7 },
		{ "IN_RIGHT", 1 << 8 }, { "IN_MOVELEFT", 1 << 9 }, { "IN_MOVERIGHT", 1 << 10 }, { "IN_ATTACK2", 1 << 11 },
		{ "IN_RUN", 1 << 12 }, { "IN_RELOAD", 1 << 13 }, { "IN_ALT1", 1 << 14 }, { "IN_SCORE", 1 << 15 },
	};
	for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++) {
		lua_pushinteger(L, consts[i].value);
		lua_setglobal(L, consts[i].name);
	}
}
