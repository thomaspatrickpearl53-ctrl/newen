// newen - Lua bindings for the game: Vector, Angle, Entity, ents, player, util, and server hooks
//
// Names follow Garry's Mod and Half-Life: Lua so addons move over. An Entity is an edict index plus
// its serial number, so a handle to a removed entity stops being valid instead of pointing at
// whatever reuses the slot. Entity fields (ent.health, ent.target...) read and write the game's
// entvars directly; methods (ent:GetPos()...) cover the common cases.
#include "lua_local.h"
#include "server.h"

#define VECTOR_MT "Vector"
#define ANGLE_MT  "Angle"
#define ENTITY_MT "Entity"

// ---------------------------------------------------------------- Vector and Angle

static float *NewVec(lua_State *L, const char *mt, float x, float y, float z)
{
	float *v = lua_newuserdatauv(L, sizeof(float) * 3, 0);
	v[0] = x; v[1] = y; v[2] = z;
	luaL_setmetatable(L, mt);
	return v;
}

static void PushVector(lua_State *L, const float *v) { NewVec(L, VECTOR_MT, v[0], v[1], v[2]); }
static void PushAngle(lua_State *L, const float *v)  { NewVec(L, ANGLE_MT, v[0], v[1], v[2]); }

// a Vector or an Angle (both are three floats)
static float *TestVec(lua_State *L, int idx)
{
	float *v = luaL_testudata(L, idx, VECTOR_MT);
	return v ? v : luaL_testudata(L, idx, ANGLE_MT);
}

static float *CheckVec(lua_State *L, int idx)
{
	float *v = TestVec(L, idx);
	if (!v)
		luaL_typeerror(L, idx, "Vector");
	return v;
}

static int l_Vector(lua_State *L)
{
	float *src = TestVec(L, 1);
	if (src)
		NewVec(L, VECTOR_MT, src[0], src[1], src[2]);
	else
		NewVec(L, VECTOR_MT, (float)luaL_optnumber(L, 1, 0), (float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0));
	return 1;
}

static int l_Angle(lua_State *L)
{
	float *src = TestVec(L, 1);
	if (src)
		NewVec(L, ANGLE_MT, src[0], src[1], src[2]);
	else
		NewVec(L, ANGLE_MT, (float)luaL_optnumber(L, 1, 0), (float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0));
	return 1;
}

static int ComponentIndex(const char *k, qboolean angle)
{
	if (!k || k[1])
		return angle ? (!strcmp(k, "pitch") ? 0 : !strcmp(k, "yaw") ? 1 : !strcmp(k, "roll") ? 2 : -1) : -1;
	switch (k[0]) {
	case 'x': case 'p': case '1': return 0;
	case 'y': case '2': return 1;
	case 'z': case 'r': case '3': return 2;
	}
	return -1;
}

static int vec_index(lua_State *L, qboolean angle)
{
	float *v = CheckVec(L, 1);
	if (lua_type(L, 2) == LUA_TNUMBER) {
		lua_Integer i = lua_tointeger(L, 2);
		if (i >= 1 && i <= 3) {
			lua_pushnumber(L, v[i - 1]);
			return 1;
		}
	}
	const char *k = lua_tostring(L, 2);
	int c = k ? ComponentIndex(k, angle) : -1;
	if (c >= 0) {
		lua_pushnumber(L, v[c]);
		return 1;
	}
	luaL_getmetatable(L, angle ? ANGLE_MT : VECTOR_MT);
	lua_getfield(L, -1, "methods");
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	return 1;
}

static int vec_newindex(lua_State *L, qboolean angle)
{
	float *v = CheckVec(L, 1);
	int c = -1;
	if (lua_type(L, 2) == LUA_TNUMBER) {
		lua_Integer i = lua_tointeger(L, 2);
		c = (i >= 1 && i <= 3) ? (int)i - 1 : -1;
	} else {
		c = ComponentIndex(lua_tostring(L, 2), angle);
	}
	if (c < 0)
		return luaL_error(L, "%s has no field %s", angle ? "Angle" : "Vector", lua_tostring(L, 2));
	v[c] = (float)luaL_checknumber(L, 3);
	return 0;
}

static int v_index(lua_State *L)    { return vec_index(L, false); }
static int a_index(lua_State *L)    { return vec_index(L, true); }
static int v_newindex(lua_State *L) { return vec_newindex(L, false); }
static int a_newindex(lua_State *L) { return vec_newindex(L, true); }

// arithmetic keeps the type of whichever operand is a Vector/Angle
static const char *ResultType(lua_State *L)
{
	if (luaL_testudata(L, 1, ANGLE_MT) || luaL_testudata(L, 2, ANGLE_MT))
		return ANGLE_MT;
	return VECTOR_MT;
}

static int v_add(lua_State *L)
{
	float *a = CheckVec(L, 1), *b = CheckVec(L, 2);
	NewVec(L, ResultType(L), a[0] + b[0], a[1] + b[1], a[2] + b[2]);
	return 1;
}

static int v_sub(lua_State *L)
{
	float *a = CheckVec(L, 1), *b = CheckVec(L, 2);
	NewVec(L, ResultType(L), a[0] - b[0], a[1] - b[1], a[2] - b[2]);
	return 1;
}

static int v_mul(lua_State *L)
{
	float *a = TestVec(L, 1), *b = TestVec(L, 2);
	if (a && b)
		NewVec(L, ResultType(L), a[0] * b[0], a[1] * b[1], a[2] * b[2]);
	else if (a)
		{ float s = (float)luaL_checknumber(L, 2); NewVec(L, ResultType(L), a[0] * s, a[1] * s, a[2] * s); }
	else
		{ float s = (float)luaL_checknumber(L, 1); b = CheckVec(L, 2); NewVec(L, ResultType(L), b[0] * s, b[1] * s, b[2] * s); }
	return 1;
}

static int v_div(lua_State *L)
{
	float *a = CheckVec(L, 1), *b = TestVec(L, 2);
	if (b)
		NewVec(L, ResultType(L), a[0] / b[0], a[1] / b[1], a[2] / b[2]);
	else
		{ float s = (float)luaL_checknumber(L, 2); NewVec(L, ResultType(L), a[0] / s, a[1] / s, a[2] / s); }
	return 1;
}

static int v_unm(lua_State *L)
{
	float *a = CheckVec(L, 1);
	NewVec(L, ResultType(L), -a[0], -a[1], -a[2]);
	return 1;
}

static int v_eq(lua_State *L)
{
	float *a = TestVec(L, 1), *b = TestVec(L, 2);
	lua_pushboolean(L, a && b && a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
	return 1;
}

static int v_tostring(lua_State *L)
{
	float *a = CheckVec(L, 1);
	lua_pushfstring(L, "%f %f %f", (double)a[0], (double)a[1], (double)a[2]);
	return 1;
}

static int v_Length(lua_State *L)    { float *a = CheckVec(L, 1); lua_pushnumber(L, VectorLength(a)); return 1; }
static int v_LengthSqr(lua_State *L) { float *a = CheckVec(L, 1); lua_pushnumber(L, DotProduct(a, a)); return 1; }
static int v_Length2D(lua_State *L)  { float *a = CheckVec(L, 1); lua_pushnumber(L, sqrtf(a[0] * a[0] + a[1] * a[1])); return 1; }
static int v_Dot(lua_State *L)       { lua_pushnumber(L, DotProduct(CheckVec(L, 1), CheckVec(L, 2))); return 1; }
static int v_IsZero(lua_State *L)    { float *a = CheckVec(L, 1); lua_pushboolean(L, !a[0] && !a[1] && !a[2]); return 1; }
static int v_Unpack(lua_State *L)    { float *a = CheckVec(L, 1); for (int i = 0; i < 3; i++) lua_pushnumber(L, a[i]); return 3; }
static int v_Zero(lua_State *L)      { float *a = CheckVec(L, 1); VectorClear(a); return 0; }

static int v_Distance(lua_State *L)
{
	vec3_t d;
	VectorSubtract(CheckVec(L, 1), CheckVec(L, 2), d);
	lua_pushnumber(L, VectorLength(d));
	return 1;
}

static int v_DistToSqr(lua_State *L)
{
	vec3_t d;
	VectorSubtract(CheckVec(L, 1), CheckVec(L, 2), d);
	lua_pushnumber(L, DotProduct(d, d));
	return 1;
}

static int v_Cross(lua_State *L)
{
	vec3_t c;
	CrossProduct(CheckVec(L, 1), CheckVec(L, 2), c);
	PushVector(L, c);
	return 1;
}

static int v_GetNormalized(lua_State *L)
{
	vec3_t n;
	VectorCopy(CheckVec(L, 1), n);
	VectorNormalize(n);
	PushVector(L, n);
	return 1;
}

static int v_Normalize(lua_State *L) { VectorNormalize(CheckVec(L, 1)); return 0; }

static int v_Set(lua_State *L)
{
	float *a = CheckVec(L, 1), *b = TestVec(L, 2);
	if (b)
		VectorCopy(b, a);
	else
		for (int i = 0; i < 3; i++)
			a[i] = (float)luaL_optnumber(L, 2 + i, 0);
	return 0;
}

static int v_Add(lua_State *L) { float *a = CheckVec(L, 1), *b = CheckVec(L, 2); VectorAdd(a, b, a); return 0; }
static int v_Sub(lua_State *L) { float *a = CheckVec(L, 1), *b = CheckVec(L, 2); VectorSubtract(a, b, a); return 0; }
static int v_Mul(lua_State *L) { float *a = CheckVec(L, 1); float s = (float)luaL_checknumber(L, 2); VectorScale(a, s, a); return 0; }

// direction to angles (pitch up is negative, as in the engine)
static int v_Angle(lua_State *L)
{
	float *a = CheckVec(L, 1);
	float ang[3] = { 0, 0, 0 };
	if (a[0] || a[1]) {
		ang[1] = atan2f(a[1], a[0]) * 180.0f / M_PI_F;
		if (ang[1] < 0)
			ang[1] += 360;
	}
	float fwd = sqrtf(a[0] * a[0] + a[1] * a[1]);
	ang[0] = -atan2f(a[2], fwd) * 180.0f / M_PI_F;
	PushAngle(L, ang);
	return 1;
}

static int a_Forward(lua_State *L) { vec3_t f; AngleVectors(CheckVec(L, 1), f, NULL, NULL); PushVector(L, f); return 1; }
static int a_Right(lua_State *L)   { vec3_t r; AngleVectors(CheckVec(L, 1), NULL, r, NULL); PushVector(L, r); return 1; }
static int a_Up(lua_State *L)      { vec3_t u; AngleVectors(CheckVec(L, 1), NULL, NULL, u); PushVector(L, u); return 1; }

static int a_Normalize(lua_State *L)
{
	float *a = CheckVec(L, 1);
	for (int i = 0; i < 3; i++) {
		a[i] = fmodf(a[i] + 180.0f, 360.0f);
		if (a[i] < 0)
			a[i] += 360.0f;
		a[i] -= 180.0f;
	}
	return 0;
}

static void RegisterVecType(lua_State *L, const char *mt, lua_CFunction index, lua_CFunction newindex, const luaL_Reg *methods)
{
	luaL_newmetatable(L, mt);
	static const luaL_Reg meta[] = {
		{ "__add", v_add }, { "__sub", v_sub }, { "__mul", v_mul }, { "__div", v_div }, { "__unm", v_unm },
		{ "__eq", v_eq }, { "__tostring", v_tostring }, { NULL, NULL }
	};
	luaL_setfuncs(L, meta, 0);
	lua_pushcfunction(L, index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, newindex);
	lua_setfield(L, -2, "__newindex");
	lua_newtable(L);
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "methods");
	lua_pop(L, 1);
}

// ---------------------------------------------------------------- Entity

typedef struct {
	int index, serial;
} luaent_t;

void LuaG_PushEntity(lua_State *L, edict_t *e)
{
	luaent_t *le = lua_newuserdatauv(L, sizeof(luaent_t), 0);
	if (e && sv.active && sv.edicts && !e->free) {
		le->index = NUM_FOR_EDICT(e);
		le->serial = e->serialnumber;
	} else {
		le->index = -1;
		le->serial = 0;
	}
	luaL_setmetatable(L, ENTITY_MT);
}

static edict_t *ToEdict(lua_State *L, int idx)
{
	luaent_t *le = luaL_testudata(L, idx, ENTITY_MT);
	if (!le || le->index < 0 || !sv.active || !sv.edicts || le->index >= sv.num_edicts)
		return NULL;
	edict_t *e = EDICT_NUM(le->index);
	if (e->free || e->serialnumber != le->serial)
		return NULL;
	return e;
}

static edict_t *CheckEdict(lua_State *L, int idx)
{
	edict_t *e = ToEdict(L, idx);
	if (!e) {
		if (!luaL_testudata(L, idx, ENTITY_MT))
			luaL_typeerror(L, idx, "Entity");
		luaL_error(L, "tried to use a NULL entity");
	}
	return e;
}

static qboolean IsPlayerEdict(const edict_t *e)
{
	int n = NUM_FOR_EDICT(e);
	return n >= 1 && n <= sv.maxclients;
}

// entvars fields reachable as ent.name
enum { F_FLOAT, F_INT, F_VEC, F_STRING, F_EDICT };
typedef struct { const char *name; int ofs, type; } entfield_t;
#define FLD(n, t) { #n, offsetof(entvars_t, n), t }
static const entfield_t entfields[] = {
	FLD(classname, F_STRING), FLD(globalname, F_STRING), FLD(model, F_STRING), FLD(target, F_STRING),
	FLD(targetname, F_STRING), FLD(netname, F_STRING), FLD(message, F_STRING), FLD(noise, F_STRING),
	FLD(noise1, F_STRING), FLD(noise2, F_STRING), FLD(noise3, F_STRING),
	FLD(origin, F_VEC), FLD(oldorigin, F_VEC), FLD(velocity, F_VEC), FLD(basevelocity, F_VEC), FLD(movedir, F_VEC),
	FLD(angles, F_VEC), FLD(avelocity, F_VEC), FLD(punchangle, F_VEC), FLD(v_angle, F_VEC), FLD(view_ofs, F_VEC),
	FLD(absmin, F_VEC), FLD(absmax, F_VEC), FLD(mins, F_VEC), FLD(maxs, F_VEC), FLD(size, F_VEC),
	FLD(rendercolor, F_VEC), FLD(vuser1, F_VEC), FLD(vuser2, F_VEC), FLD(vuser3, F_VEC), FLD(vuser4, F_VEC),
	FLD(health, F_FLOAT), FLD(max_health, F_FLOAT), FLD(armorvalue, F_FLOAT), FLD(armortype, F_FLOAT),
	FLD(takedamage, F_FLOAT), FLD(gravity, F_FLOAT), FLD(friction, F_FLOAT), FLD(frame, F_FLOAT),
	FLD(animtime, F_FLOAT), FLD(framerate, F_FLOAT), FLD(scale, F_FLOAT), FLD(renderamt, F_FLOAT),
	FLD(nextthink, F_FLOAT), FLD(ltime, F_FLOAT), FLD(speed, F_FLOAT), FLD(dmg, F_FLOAT), FLD(frags, F_FLOAT),
	FLD(ideal_yaw, F_FLOAT), FLD(yaw_speed, F_FLOAT), FLD(maxspeed, F_FLOAT), FLD(fov, F_FLOAT),
	FLD(fuser1, F_FLOAT), FLD(fuser2, F_FLOAT), FLD(fuser3, F_FLOAT), FLD(fuser4, F_FLOAT),
	FLD(modelindex, F_INT), FLD(movetype, F_INT), FLD(solid, F_INT), FLD(skin, F_INT), FLD(body, F_INT),
	FLD(effects, F_INT), FLD(sequence, F_INT), FLD(rendermode, F_INT), FLD(renderfx, F_INT), FLD(weapons, F_INT),
	FLD(deadflag, F_INT), FLD(button, F_INT), FLD(impulse, F_INT), FLD(spawnflags, F_INT), FLD(flags, F_INT),
	FLD(team, F_INT), FLD(waterlevel, F_INT), FLD(watertype, F_INT), FLD(light_level, F_INT), FLD(weaponanim, F_INT),
	FLD(iuser1, F_INT), FLD(iuser2, F_INT), FLD(iuser3, F_INT), FLD(iuser4, F_INT),
	FLD(owner, F_EDICT), FLD(enemy, F_EDICT), FLD(aiment, F_EDICT), FLD(groundentity, F_EDICT), FLD(chain, F_EDICT),
	FLD(dmg_inflictor, F_EDICT), FLD(euser1, F_EDICT), FLD(euser2, F_EDICT), FLD(euser3, F_EDICT), FLD(euser4, F_EDICT),
};

static const entfield_t *FindField(const char *name)
{
	for (size_t i = 0; i < sizeof(entfields) / sizeof(entfields[0]); i++)
		if (!strcmp(entfields[i].name, name))
			return &entfields[i];
	return NULL;
}

static int PushField(lua_State *L, edict_t *e, const entfield_t *f)
{
	unsigned char *p = (unsigned char *)&e->v + f->ofs;
	switch (f->type) {
	case F_FLOAT:  lua_pushnumber(L, *(float *)p); break;
	case F_INT:    lua_pushinteger(L, *(int *)p); break;
	case F_VEC:    if (!strcmp(f->name, "angles") || !strcmp(f->name, "v_angle") || !strcmp(f->name, "punchangle") ||
	                   !strcmp(f->name, "avelocity"))
	                   PushAngle(L, (float *)p);
	               else
	                   PushVector(L, (float *)p);
	               break;
	case F_STRING: lua_pushstring(L, SV_String(*(string_t *)p)); break;
	case F_EDICT:  LuaG_PushEntity(L, *(edict_t **)p); break;
	}
	return 1;
}

static void SetField(lua_State *L, edict_t *e, const entfield_t *f, int idx)
{
	unsigned char *p = (unsigned char *)&e->v + f->ofs;
	switch (f->type) {
	case F_FLOAT:  *(float *)p = (float)luaL_checknumber(L, idx); break;
	case F_INT:    *(int *)p = (int)luaL_checkinteger(L, idx); break;
	case F_VEC:    VectorCopy(CheckVec(L, idx), (float *)p); break;
	case F_STRING: *(string_t *)p = SV_AllocString(luaL_checkstring(L, idx)); break;
	case F_EDICT:  *(edict_t **)p = lua_isnil(L, idx) ? NULL : ToEdict(L, idx); break;
	}
	// moving or resizing needs relinking so collision and triggers see it
	if (!strcmp(f->name, "origin") || !strcmp(f->name, "mins") || !strcmp(f->name, "maxs") || !strcmp(f->name, "solid"))
		SV_LinkEdict(e, false);
}

static int e_index(lua_State *L)
{
	luaL_getmetatable(L, ENTITY_MT);
	lua_getfield(L, -1, "methods");
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
		return 1;
	const char *k = lua_tostring(L, 2);
	const entfield_t *f = k ? FindField(k) : NULL;
	edict_t *e = ToEdict(L, 1);
	if (f && e)
		return PushField(L, e, f);
	lua_pushnil(L);
	return 1;
}

static int e_newindex(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	const char *k = luaL_checkstring(L, 2);
	const entfield_t *f = FindField(k);
	if (!f)
		return luaL_error(L, "entities have no field '%s'", k);
	SetField(L, e, f, 3);
	return 0;
}

static int e_eq(lua_State *L)
{
	luaent_t *a = luaL_testudata(L, 1, ENTITY_MT), *b = luaL_testudata(L, 2, ENTITY_MT);
	lua_pushboolean(L, a && b && a->index == b->index && a->serial == b->serial);
	return 1;
}

static int e_tostring(lua_State *L)
{
	edict_t *e = ToEdict(L, 1);
	if (!e)
		lua_pushstring(L, "[NULL Entity]");
	else
		lua_pushfstring(L, "%s [%d][%s]", IsPlayerEdict(e) ? "Player" : "Entity", NUM_FOR_EDICT(e), SV_String(e->v.classname));
	return 1;
}

static int m_IsValid(lua_State *L)  { lua_pushboolean(L, ToEdict(L, 1) != NULL); return 1; }
static int m_EntIndex(lua_State *L) { edict_t *e = ToEdict(L, 1); lua_pushinteger(L, e ? NUM_FOR_EDICT(e) : -1); return 1; }
static int m_GetClass(lua_State *L) { lua_pushstring(L, SV_String(CheckEdict(L, 1)->v.classname)); return 1; }
static int m_GetPos(lua_State *L)   { PushVector(L, CheckEdict(L, 1)->v.origin); return 1; }
static int m_GetAngles(lua_State *L){ PushAngle(L, CheckEdict(L, 1)->v.angles); return 1; }
static int m_GetVelocity(lua_State *L) { PushVector(L, CheckEdict(L, 1)->v.velocity); return 1; }
static int m_GetModel(lua_State *L) { lua_pushstring(L, SV_String(CheckEdict(L, 1)->v.model)); return 1; }
static int m_Health(lua_State *L)   { lua_pushnumber(L, CheckEdict(L, 1)->v.health); return 1; }
static int m_GetMaxHealth(lua_State *L) { lua_pushnumber(L, CheckEdict(L, 1)->v.max_health); return 1; }
static int m_Armor(lua_State *L)    { lua_pushnumber(L, CheckEdict(L, 1)->v.armorvalue); return 1; }
static int m_GetName(lua_State *L)  { lua_pushstring(L, SV_String(CheckEdict(L, 1)->v.targetname)); return 1; }
static int m_GetOwner(lua_State *L) { LuaG_PushEntity(L, CheckEdict(L, 1)->v.owner); return 1; }
static int m_GetMoveType(lua_State *L) { lua_pushinteger(L, CheckEdict(L, 1)->v.movetype); return 1; }
static int m_GetSolid(lua_State *L) { lua_pushinteger(L, CheckEdict(L, 1)->v.solid); return 1; }
static int m_GetSkin(lua_State *L)  { lua_pushinteger(L, CheckEdict(L, 1)->v.skin); return 1; }
static int m_GetBody(lua_State *L)  { lua_pushinteger(L, CheckEdict(L, 1)->v.body); return 1; }
static int m_GetSequence(lua_State *L) { lua_pushinteger(L, CheckEdict(L, 1)->v.sequence); return 1; }
static int m_GetFlags(lua_State *L) { lua_pushinteger(L, CheckEdict(L, 1)->v.flags); return 1; }
static int m_OBBMins(lua_State *L)  { PushVector(L, CheckEdict(L, 1)->v.mins); return 1; }
static int m_OBBMaxs(lua_State *L)  { PushVector(L, CheckEdict(L, 1)->v.maxs); return 1; }
static int m_WaterLevel(lua_State *L) { lua_pushinteger(L, CheckEdict(L, 1)->v.waterlevel); return 1; }
static int m_OnGround(lua_State *L) { lua_pushboolean(L, (CheckEdict(L, 1)->v.flags & FL_ONGROUND) != 0); return 1; }
static int m_IsPlayer(lua_State *L) { edict_t *e = ToEdict(L, 1); lua_pushboolean(L, e && IsPlayerEdict(e)); return 1; }
static int m_IsNPC(lua_State *L)    { edict_t *e = ToEdict(L, 1); lua_pushboolean(L, e && !IsPlayerEdict(e) && (e->v.flags & FL_MONSTER)); return 1; }
static int m_IsWorld(lua_State *L)  { edict_t *e = ToEdict(L, 1); lua_pushboolean(L, e && e == sv.edicts); return 1; }
static int m_Alive(lua_State *L)    { edict_t *e = CheckEdict(L, 1); lua_pushboolean(L, e->v.health > 0 && e->v.deadflag == 0); return 1; }

static int m_EyePos(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	vec3_t p;
	VectorAdd(e->v.origin, e->v.view_ofs, p);
	PushVector(L, p);
	return 1;
}

static int m_EyeAngles(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	PushAngle(L, IsPlayerEdict(e) ? e->v.v_angle : e->v.angles);
	return 1;
}

static int m_GetAimVector(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	vec3_t f;
	AngleVectors(IsPlayerEdict(e) ? e->v.v_angle : e->v.angles, f, NULL, NULL);
	PushVector(L, f);
	return 1;
}

static int m_GetForward(lua_State *L) { vec3_t f; AngleVectors(CheckEdict(L, 1)->v.angles, f, NULL, NULL); PushVector(L, f); return 1; }
static int m_GetRight(lua_State *L)   { vec3_t r; AngleVectors(CheckEdict(L, 1)->v.angles, NULL, r, NULL); PushVector(L, r); return 1; }
static int m_GetUp(lua_State *L)      { vec3_t u; AngleVectors(CheckEdict(L, 1)->v.angles, NULL, NULL, u); PushVector(L, u); return 1; }

static int m_SetPos(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	VectorCopy(CheckVec(L, 2), e->v.origin);
	SV_LinkEdict(e, false);
	return 0;
}

static int m_SetAngles(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	VectorCopy(CheckVec(L, 2), e->v.angles);
	if (IsPlayerEdict(e)) {
		VectorCopy(e->v.angles, e->v.v_angle);
		e->v.fixangle = 1;   // turns the view too
	}
	return 0;
}

static int m_SetVelocity(lua_State *L) { VectorCopy(CheckVec(L, 2), CheckEdict(L, 1)->v.velocity); return 0; }
static int m_SetHealth(lua_State *L)   { CheckEdict(L, 1)->v.health = (float)luaL_checknumber(L, 2); return 0; }
static int m_SetMaxHealth(lua_State *L){ CheckEdict(L, 1)->v.max_health = (float)luaL_checknumber(L, 2); return 0; }
static int m_SetArmor(lua_State *L)    { CheckEdict(L, 1)->v.armorvalue = (float)luaL_checknumber(L, 2); return 0; }
static int m_SetName(lua_State *L)     { CheckEdict(L, 1)->v.targetname = SV_AllocString(luaL_checkstring(L, 2)); return 0; }
static int m_SetOwner(lua_State *L)    { CheckEdict(L, 1)->v.owner = ToEdict(L, 2); return 0; }
static int m_SetMoveType(lua_State *L) { CheckEdict(L, 1)->v.movetype = (int)luaL_checkinteger(L, 2); return 0; }
static int m_SetSkin(lua_State *L)     { CheckEdict(L, 1)->v.skin = (int)luaL_checkinteger(L, 2); return 0; }
static int m_SetBody(lua_State *L)     { CheckEdict(L, 1)->v.body = (int)luaL_checkinteger(L, 2); return 0; }
static int m_AddFlags(lua_State *L)    { CheckEdict(L, 1)->v.flags |= (int)luaL_checkinteger(L, 2); return 0; }
static int m_RemoveFlags(lua_State *L) { CheckEdict(L, 1)->v.flags &= ~(int)luaL_checkinteger(L, 2); return 0; }

static int m_SetSolid(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	e->v.solid = (int)luaL_checkinteger(L, 2);
	SV_LinkEdict(e, false);
	return 0;
}

static int m_SetSize(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	SV_SetMinMaxSize(e, CheckVec(L, 2), CheckVec(L, 3));
	return 0;
}

static int m_SetModel(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	SV_SetModel(e, luaL_checkstring(L, 2));
	return 0;
}

static int m_SetColor(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	const char *keys[4] = { "r", "g", "b", "a" };
	float c[4];
	for (int i = 0; i < 4; i++) {
		lua_getfield(L, 2, keys[i]);
		c[i] = (float)luaL_optnumber(L, -1, 255);
		lua_pop(L, 1);
	}
	VectorCopy(c, e->v.rendercolor);
	e->v.renderamt = c[3];
	return 0;
}

static int m_SetRenderMode(lua_State *L) { CheckEdict(L, 1)->v.rendermode = (int)luaL_checkinteger(L, 2); return 0; }

static int m_SetSequence(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	int seq;
	if (lua_type(L, 2) == LUA_TSTRING) {
		seq = R_StudioLookupSequence(SV_ModelForIndex(e->v.modelindex), lua_tostring(L, 2));
		if (seq < 0)
			return luaL_error(L, "no sequence named %s", lua_tostring(L, 2));
	} else {
		seq = (int)luaL_checkinteger(L, 2);
	}
	e->v.sequence = seq;
	e->v.frame = 0;
	e->v.animtime = (float)sv.time;
	if (!e->v.framerate)
		e->v.framerate = 1;
	return 0;
}

static int m_LookupSequence(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	lua_pushinteger(L, R_StudioLookupSequence(SV_ModelForIndex(e->v.modelindex), luaL_checkstring(L, 2)));
	return 1;
}

// the game's KeyValue, as if the key had been in the map
static int m_SetKeyValue(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	char cls[256], key[256], value[2048];
	Q_strncpy(cls, SV_String(e->v.classname), sizeof(cls));
	Q_strncpy(key, luaL_checkstring(L, 2), sizeof(key));
	Q_strncpy(value, luaL_tolstring(L, 3, NULL), sizeof(value));
	lua_pop(L, 1);
	KeyValueData kvd = { cls, key, value, 0 };
	gEntityInterface.pfnKeyValue(e, &kvd);
	lua_pushboolean(L, kvd.fHandled);
	return 1;
}

static int m_Spawn(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	gGlobals.time = (float)sv.time;
	if (gEntityInterface.pfnSpawn(e) < 0 || (e->v.flags & FL_KILLME)) {
		ED_Free(e);
		lua_pushboolean(L, false);
		return 1;
	}
	lua_pushboolean(L, true);
	return 1;
}

static int m_Use(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	edict_t *other = lua_isnoneornil(L, 2) ? SV_LocalPlayer() : ToEdict(L, 2);
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnUse(e, other ? other : sv.edicts);
	return 0;
}

static int m_Touch(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1), *other = CheckEdict(L, 2);
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnTouch(e, other);
	return 0;
}

// removed at the end of the frame, the way the game removes things (UTIL_Remove)
static int m_Remove(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	if (e == sv.edicts || IsPlayerEdict(e))
		return luaL_error(L, "can't remove the world or a player");
	e->v.flags |= FL_KILLME;
	e->v.targetname = 0;
	return 0;
}

static int m_Kill(lua_State *L)
{
	edict_t *e = CheckEdict(L, 1);
	if (IsPlayerEdict(e)) {
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnClientKill(e);
	}
	return 0;
}

static int m_Nick(lua_State *L) { lua_pushstring(L, SV_String(CheckEdict(L, 1)->v.netname)); return 1; }

// like the game's GiveNamedItem: spawn the item on the player and touch it
static int m_Give(lua_State *L)
{
	edict_t *pl = CheckEdict(L, 1);
	if (!IsPlayerEdict(pl))
		return luaL_error(L, "Give only works on players");
	edict_t *item = SV_CreateNamedEntity(luaL_checkstring(L, 2));
	if (!item) {
		LuaG_PushEntity(L, NULL);
		return 1;
	}
	VectorCopy(pl->v.origin, item->v.origin);
	item->v.spawnflags |= 1 << 30;   // SF_NORESPAWN
	gGlobals.time = (float)sv.time;
	if (gEntityInterface.pfnSpawn(item) < 0) {
		ED_Free(item);
		LuaG_PushEntity(L, NULL);
		return 1;
	}
	gEntityInterface.pfnTouch(item, pl);
	LuaG_PushEntity(L, item);
	return 1;
}

// ---------------------------------------------------------------- ents and player

static int l_Entity(lua_State *L)
{
	lua_Integer i = luaL_checkinteger(L, 1);
	edict_t *e = (sv.active && i >= 0 && i < sv.num_edicts) ? EDICT_NUM(i) : NULL;
	LuaG_PushEntity(L, e && !e->free ? e : NULL);
	return 1;
}

static int l_LocalPlayer(lua_State *L) { LuaG_PushEntity(L, SV_LocalPlayer()); return 1; }

static void RequireServer(lua_State *L)
{
	if (!sv.active)
		luaL_error(L, "no map is running");
}

static int l_ents_Create(lua_State *L)
{
	RequireServer(L);
	edict_t *e = SV_CreateNamedEntity(luaL_checkstring(L, 1));
	LuaG_PushEntity(L, e);
	return 1;
}

static int l_ents_GetAll(lua_State *L)
{
	lua_newtable(L);
	if (!sv.active)
		return 1;
	int n = 0;
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free)
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int FindByStringField(lua_State *L, int ofs)
{
	const char *pattern = luaL_checkstring(L, 1);
	lua_newtable(L);
	if (!sv.active)
		return 1;
	int n = 0;
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free)
			continue;
		const char *s = SV_String(*(string_t *)((unsigned char *)&e->v + ofs));
		if (!*s || !Q_WildcardMatch(pattern, s))
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int l_ents_FindByClass(lua_State *L) { return FindByStringField(L, offsetof(entvars_t, classname)); }
static int l_ents_FindByName(lua_State *L)  { return FindByStringField(L, offsetof(entvars_t, targetname)); }
static int l_ents_FindByModel(lua_State *L) { return FindByStringField(L, offsetof(entvars_t, model)); }

static int l_ents_FindInSphere(lua_State *L)
{
	float *org = CheckVec(L, 1);
	float r = (float)luaL_checknumber(L, 2);
	lua_newtable(L);
	if (!sv.active)
		return 1;
	int n = 0;
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.classname)
			continue;
		float d2 = 0;
		for (int j = 0; j < 3; j++) {
			float v = org[j];
			v = v < e->v.absmin[j] ? e->v.absmin[j] - v : v > e->v.absmax[j] ? v - e->v.absmax[j] : 0;
			d2 += v * v;
		}
		if (d2 > r * r)
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int l_ents_FindInBox(lua_State *L)
{
	float *mins = CheckVec(L, 1), *maxs = CheckVec(L, 2);
	lua_newtable(L);
	if (!sv.active)
		return 1;
	int n = 0;
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.classname)
			continue;
		if (e->v.absmin[0] > maxs[0] || e->v.absmin[1] > maxs[1] || e->v.absmin[2] > maxs[2] ||
			e->v.absmax[0] < mins[0] || e->v.absmax[1] < mins[1] || e->v.absmax[2] < mins[2])
			continue;
		LuaG_PushEntity(L, e);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int l_ents_GetCount(lua_State *L)
{
	int n = 0;
	if (sv.active)
		for (int i = 0; i < sv.num_edicts; i++)
			n += !EDICT_NUM(i)->free;
	lua_pushinteger(L, n);
	return 1;
}

static int l_player_GetAll(lua_State *L)
{
	lua_newtable(L);
	edict_t *pl = SV_LocalPlayer();
	if (pl) {
		LuaG_PushEntity(L, pl);
		lua_rawseti(L, -2, 1);
	}
	return 1;
}

// ---------------------------------------------------------------- util

// util.TraceLine{ start, endpos, filter, ignoremonsters } / util.TraceHull{ ..., mins, maxs }
static int DoTrace(lua_State *L, qboolean hull)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	RequireServer(L);
	vec3_t start, end, mins = { 0, 0, 0 }, maxs = { 0, 0, 0 };
	lua_getfield(L, 1, "start");
	VectorCopy(CheckVec(L, -1), start);
	lua_getfield(L, 1, "endpos");
	VectorCopy(CheckVec(L, -1), end);
	if (hull) {
		lua_getfield(L, 1, "mins");
		VectorCopy(CheckVec(L, -1), mins);
		lua_getfield(L, 1, "maxs");
		VectorCopy(CheckVec(L, -1), maxs);
	}
	lua_getfield(L, 1, "filter");
	edict_t *filter = ToEdict(L, -1);
	lua_getfield(L, 1, "ignoremonsters");
	int type = lua_toboolean(L, -1) ? MOVE_NOMONSTERS : MOVE_NORMAL;
	lua_settop(L, 1);

	trace_t t = SV_Move(start, mins, maxs, end, type, filter);
	lua_newtable(L);
	lua_pushboolean(L, t.fraction < 1 || t.startsolid);
	lua_setfield(L, -2, "Hit");
	lua_pushboolean(L, t.ent == sv.edicts);
	lua_setfield(L, -2, "HitWorld");
	lua_pushnumber(L, t.fraction);
	lua_setfield(L, -2, "Fraction");
	PushVector(L, t.endpos);
	lua_setfield(L, -2, "HitPos");
	PushVector(L, t.plane.normal);
	lua_setfield(L, -2, "HitNormal");
	PushVector(L, start);
	lua_setfield(L, -2, "StartPos");
	LuaG_PushEntity(L, t.ent);
	lua_setfield(L, -2, "Entity");
	lua_pushboolean(L, t.startsolid);
	lua_setfield(L, -2, "StartSolid");
	lua_pushboolean(L, t.allsolid);
	lua_setfield(L, -2, "AllSolid");
	if (t.fraction < 1 && t.ent) {
		// the texture name, for footstep-style material checks
		vec3_t dir, past;
		VectorSubtract(end, start, dir);
		VectorNormalize(dir);
		VectorMA(t.endpos, 8, dir, past);
		const char *tex = SV_TraceTexture(t.ent, start, past);
		if (tex) {
			lua_pushstring(L, tex);
			lua_setfield(L, -2, "HitTexture");
		}
	}
	return 1;
}

static int l_util_TraceLine(lua_State *L) { return DoTrace(L, false); }
static int l_util_TraceHull(lua_State *L) { return DoTrace(L, true); }
static int l_util_PointContents(lua_State *L) { lua_pushinteger(L, SV_PointContents(CheckVec(L, 1))); return 1; }

static int l_util_PrecacheModel(lua_State *L)
{
	RequireServer(L);
	lua_pushinteger(L, SV_PrecacheModel(luaL_checkstring(L, 1)));
	return 1;
}

static int l_util_PrecacheSound(lua_State *L)
{
	RequireServer(L);
	lua_pushinteger(L, SV_PrecacheSound(luaL_checkstring(L, 1)));
	return 1;
}

// ---------------------------------------------------------------- registration

static void SetIntGlobal(lua_State *L, const char *name, int v)
{
	lua_pushinteger(L, v);
	lua_setglobal(L, name);
}

void LuaG_Open(lua_State *L)
{
	static const luaL_Reg vmethods[] = {
		{ "Length", v_Length }, { "LengthSqr", v_LengthSqr }, { "Length2D", v_Length2D }, { "Dot", v_Dot },
		{ "Cross", v_Cross }, { "Distance", v_Distance }, { "DistToSqr", v_DistToSqr }, { "GetNormalized", v_GetNormalized },
		{ "Normalize", v_Normalize }, { "IsZero", v_IsZero }, { "Zero", v_Zero }, { "Set", v_Set }, { "Add", v_Add },
		{ "Sub", v_Sub }, { "Mul", v_Mul }, { "Angle", v_Angle }, { "Unpack", v_Unpack }, { NULL, NULL }
	};
	static const luaL_Reg amethods[] = {
		{ "Forward", a_Forward }, { "Right", a_Right }, { "Up", a_Up }, { "Normalize", a_Normalize },
		{ "IsZero", v_IsZero }, { "Zero", v_Zero }, { "Set", v_Set }, { "Unpack", v_Unpack }, { NULL, NULL }
	};
	RegisterVecType(L, VECTOR_MT, v_index, v_newindex, vmethods);
	RegisterVecType(L, ANGLE_MT, a_index, a_newindex, amethods);

	static const luaL_Reg emethods[] = {
		{ "IsValid", m_IsValid }, { "EntIndex", m_EntIndex }, { "GetClass", m_GetClass }, { "GetPos", m_GetPos },
		{ "SetPos", m_SetPos }, { "GetAngles", m_GetAngles }, { "SetAngles", m_SetAngles },
		{ "GetVelocity", m_GetVelocity }, { "SetVelocity", m_SetVelocity }, { "GetModel", m_GetModel },
		{ "SetModel", m_SetModel }, { "Health", m_Health }, { "SetHealth", m_SetHealth },
		{ "GetMaxHealth", m_GetMaxHealth }, { "SetMaxHealth", m_SetMaxHealth }, { "Armor", m_Armor },
		{ "SetArmor", m_SetArmor }, { "GetName", m_GetName }, { "SetName", m_SetName }, { "GetOwner", m_GetOwner },
		{ "SetOwner", m_SetOwner }, { "GetMoveType", m_GetMoveType }, { "SetMoveType", m_SetMoveType },
		{ "GetSolid", m_GetSolid }, { "SetSolid", m_SetSolid }, { "GetSkin", m_GetSkin }, { "SetSkin", m_SetSkin },
		{ "GetBody", m_GetBody }, { "SetBody", m_SetBody }, { "GetSequence", m_GetSequence },
		{ "SetSequence", m_SetSequence }, { "LookupSequence", m_LookupSequence }, { "GetFlags", m_GetFlags },
		{ "AddFlags", m_AddFlags }, { "RemoveFlags", m_RemoveFlags }, { "OBBMins", m_OBBMins }, { "OBBMaxs", m_OBBMaxs },
		{ "SetSize", m_SetSize }, { "SetColor", m_SetColor }, { "SetRenderMode", m_SetRenderMode },
		{ "WaterLevel", m_WaterLevel }, { "OnGround", m_OnGround }, { "IsPlayer", m_IsPlayer }, { "IsNPC", m_IsNPC },
		{ "IsWorld", m_IsWorld }, { "Alive", m_Alive }, { "EyePos", m_EyePos }, { "EyeAngles", m_EyeAngles },
		{ "GetAimVector", m_GetAimVector }, { "GetForward", m_GetForward }, { "GetRight", m_GetRight },
		{ "GetUp", m_GetUp }, { "SetKeyValue", m_SetKeyValue }, { "Spawn", m_Spawn }, { "Use", m_Use },
		{ "Touch", m_Touch }, { "Remove", m_Remove }, { "Kill", m_Kill }, { "Nick", m_Nick }, { "Name", m_Nick },
		{ "Give", m_Give }, { NULL, NULL }
	};
	luaL_newmetatable(L, ENTITY_MT);
	lua_pushcfunction(L, e_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, e_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, e_eq);
	lua_setfield(L, -2, "__eq");
	lua_pushcfunction(L, e_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_newtable(L);
	luaL_setfuncs(L, emethods, 0);
	lua_setfield(L, -2, "methods");
	lua_pop(L, 1);

	lua_pushcfunction(L, l_Vector);
	lua_setglobal(L, "Vector");
	lua_pushcfunction(L, l_Angle);
	lua_setglobal(L, "Angle");
	lua_pushcfunction(L, l_Entity);
	lua_setglobal(L, "Entity");
	lua_pushcfunction(L, l_LocalPlayer);
	lua_setglobal(L, "LocalPlayer");
	LuaG_PushEntity(L, NULL);
	lua_setglobal(L, "NULL");

	static const luaL_Reg ents[] = {
		{ "Create", l_ents_Create }, { "GetAll", l_ents_GetAll }, { "FindByClass", l_ents_FindByClass },
		{ "FindByName", l_ents_FindByName }, { "FindByModel", l_ents_FindByModel },
		{ "FindInSphere", l_ents_FindInSphere }, { "FindInBox", l_ents_FindInBox }, { "GetByIndex", l_Entity },
		{ "GetCount", l_ents_GetCount }, { NULL, NULL }
	};
	luaL_newlib(L, ents);
	lua_setglobal(L, "ents");

	static const luaL_Reg player[] = {
		{ "GetAll", l_player_GetAll }, { "GetByID", l_Entity }, { NULL, NULL }
	};
	luaL_newlib(L, player);
	lua_setglobal(L, "player");

	static const luaL_Reg util[] = {
		{ "TraceLine", l_util_TraceLine }, { "TraceHull", l_util_TraceHull },
		{ "PointContents", l_util_PointContents }, { "PrecacheModel", l_util_PrecacheModel },
		{ "PrecacheSound", l_util_PrecacheSound }, { NULL, NULL }
	};
	luaL_newlib(L, util);
	lua_setglobal(L, "util");

	// one Lua state runs both sides of the game
	lua_pushboolean(L, 1);
	lua_setglobal(L, "SERVER");
	lua_pushboolean(L, 1);
	lua_setglobal(L, "CLIENT");

	SetIntGlobal(L, "MOVETYPE_NONE", MOVETYPE_NONE);
	SetIntGlobal(L, "MOVETYPE_WALK", MOVETYPE_WALK);
	SetIntGlobal(L, "MOVETYPE_STEP", MOVETYPE_STEP);
	SetIntGlobal(L, "MOVETYPE_FLY", MOVETYPE_FLY);
	SetIntGlobal(L, "MOVETYPE_TOSS", MOVETYPE_TOSS);
	SetIntGlobal(L, "MOVETYPE_PUSH", MOVETYPE_PUSH);
	SetIntGlobal(L, "MOVETYPE_NOCLIP", MOVETYPE_NOCLIP);
	SetIntGlobal(L, "MOVETYPE_FLYMISSILE", MOVETYPE_FLYMISSILE);
	SetIntGlobal(L, "MOVETYPE_BOUNCE", MOVETYPE_BOUNCE);
	SetIntGlobal(L, "MOVETYPE_FOLLOW", MOVETYPE_FOLLOW);
	SetIntGlobal(L, "SOLID_NOT", SOLID_NOT);
	SetIntGlobal(L, "SOLID_TRIGGER", SOLID_TRIGGER);
	SetIntGlobal(L, "SOLID_BBOX", SOLID_BBOX);
	SetIntGlobal(L, "SOLID_SLIDEBOX", SOLID_SLIDEBOX);
	SetIntGlobal(L, "SOLID_BSP", SOLID_BSP);
	SetIntGlobal(L, "FL_ONGROUND", FL_ONGROUND);
	SetIntGlobal(L, "FL_GODMODE", FL_GODMODE);
	SetIntGlobal(L, "FL_NOTARGET", FL_NOTARGET);
	SetIntGlobal(L, "FL_MONSTER", FL_MONSTER);
	SetIntGlobal(L, "FL_CLIENT", FL_CLIENT);
	SetIntGlobal(L, "FL_DUCKING", FL_DUCKING);
	SetIntGlobal(L, "FL_FROZEN", FL_FROZEN);
	SetIntGlobal(L, "EF_NODRAW", EF_NODRAW);
	SetIntGlobal(L, "EF_BRIGHTLIGHT", EF_BRIGHTLIGHT);
	SetIntGlobal(L, "EF_DIMLIGHT", EF_DIMLIGHT);
	SetIntGlobal(L, "kRenderNormal", kRenderNormal);
	SetIntGlobal(L, "kRenderTransColor", kRenderTransColor);
	SetIntGlobal(L, "kRenderTransTexture", kRenderTransTexture);
	SetIntGlobal(L, "kRenderGlow", kRenderGlow);
	SetIntGlobal(L, "kRenderTransAlpha", kRenderTransAlpha);
	SetIntGlobal(L, "kRenderTransAdd", kRenderTransAdd);
	SetIntGlobal(L, "CONTENTS_EMPTY", CONTENTS_EMPTY);
	SetIntGlobal(L, "CONTENTS_SOLID", CONTENTS_SOLID);
	SetIntGlobal(L, "CONTENTS_WATER", CONTENTS_WATER);
	SetIntGlobal(L, "CONTENTS_SLIME", CONTENTS_SLIME);
	SetIntGlobal(L, "CONTENTS_LAVA", CONTENTS_LAVA);
	SetIntGlobal(L, "CONTENTS_SKY", CONTENTS_SKY);
	SetIntGlobal(L, "IN_ATTACK", IN_ATTACK);
	SetIntGlobal(L, "IN_JUMP", IN_JUMP);
	SetIntGlobal(L, "IN_DUCK", IN_DUCK);
	SetIntGlobal(L, "IN_FORWARD", IN_FORWARD);
	SetIntGlobal(L, "IN_BACK", IN_BACK);
	SetIntGlobal(L, "IN_USE", IN_USE);
	SetIntGlobal(L, "IN_MOVELEFT", IN_MOVELEFT);
	SetIntGlobal(L, "IN_MOVERIGHT", IN_MOVERIGHT);
	SetIntGlobal(L, "IN_ATTACK2", IN_ATTACK2);
	SetIntGlobal(L, "IN_RELOAD", IN_RELOAD);
	SetIntGlobal(L, "IN_SPEED", IN_RUN);
}

// ---------------------------------------------------------------- hooks fired by the server

// each edict slot remembers the serial it was announced with, so OnEntityCreated fires once per entity
static int *announced;
static int announced_cap;

typedef struct {
	luaent_t player, inflictor, attacker;
	float amount;
	unsigned int bits;
} playerhurt_t;

static playerhurt_t *hurtqueue;
static int hurt_count, hurt_cap;
static unsigned int hurt_generation;

static luaent_t HurtHandle(edict_t *e)
{
	luaent_t h = { -1, 0 };
	uintptr_t address = (uintptr_t)e, base = (uintptr_t)sv.edicts;
	if (!e || !sv.edicts || address < base || address - base >= (size_t)sv.num_edicts * sizeof(*e)
		|| (address - base) % sizeof(*e) || e->free)
		return h;
	h.index = (int)((address - base) / sizeof(*e));
	h.serial = e->serialnumber;
	return h;
}

static edict_t *HurtEntity(luaent_t h)
{
	if (!sv.active || !sv.edicts || h.index < 0 || h.index >= sv.num_edicts)
		return NULL;
	edict_t *e = EDICT_NUM(h.index);
	return !e->free && e->serialnumber == h.serial ? e : NULL;
}

void LuaG_ClearPlayerHurt(void)
{
	Mem_Free(hurtqueue);
	hurtqueue = NULL;
	hurt_count = hurt_cap = 0;
	hurt_generation++;
}

void LuaG_QueuePlayerHurt(edict_t *pl, unsigned int damagebits)
{
	// The DLL clears these HUD accumulators as soon as MessageEnd returns.
	if (!Lua_GetState() || !sv.active || pl->v.dmg_take <= 0)
		return;
	if (hurt_count == hurt_cap) {
		hurt_cap = hurt_cap ? hurt_cap * 2 : 16;
		hurtqueue = Mem_Realloc(hurtqueue, sizeof(*hurtqueue) * hurt_cap);
	}
	playerhurt_t *h = &hurtqueue[hurt_count++];
	h->player = HurtHandle(pl);
	h->inflictor = HurtHandle(pl->v.dmg_inflictor);
	edict_t *inflictor = HurtEntity(h->inflictor);
	h->attacker = inflictor && inflictor->v.owner ? HurtHandle(inflictor->v.owner) : h->inflictor;
	h->amount = pl->v.dmg_take;
	h->bits = damagebits;
}

void LuaG_FlushPlayerHurt(void)
{
	// Detach the batch: Lua may cause more DLL calls, removals or a world change.
	playerhurt_t *batch = hurtqueue;
	int count = hurt_count;
	unsigned int generation = hurt_generation;
	hurtqueue = NULL;
	hurt_count = hurt_cap = 0;
	for (int i = 0; i < count && generation == hurt_generation; i++) {
		lua_State *L = Lua_GetState();
		playerhurt_t *h = &batch[i];
		edict_t *pl = HurtEntity(h->player);
		if (!L || !pl)
			continue;
		int top = lua_gettop(L);
		LuaG_PushEntity(L, pl);
		lua_pushnumber(L, h->amount);
		LuaG_PushEntity(L, HurtEntity(h->inflictor));
		LuaG_PushEntity(L, HurtEntity(h->attacker));
		lua_pushinteger(L, (lua_Integer)h->bits);
		Lua_Hook("PlayerHurt", 5, 0);
		lua_settop(L, top);
	}
	Mem_Free(batch);
}

static void FireEntityHook(const char *name, edict_t *e)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	LuaG_PushEntity(L, e);
	Lua_Hook(name, 1, 0);
	lua_settop(L, top);
}

void LuaG_ServerStarted(void)
{
	if (announced_cap < sv.max_edicts) {
		announced_cap = sv.max_edicts;
		announced = Mem_Realloc(announced, sizeof(int) * announced_cap);
	}
	memset(announced, 0, sizeof(int) * announced_cap);
}

// new entities since the last check (the game creates many itself, without telling the engine)
void LuaG_CheckNewEntities(void)
{
	if (!Lua_GetState() || !sv.active || !announced)
		return;
	for (int i = 0; i < sv.num_edicts && i < announced_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.classname || announced[i] == e->serialnumber + 1)
			continue;
		announced[i] = e->serialnumber + 1;
		FireEntityHook("OnEntityCreated", e);
	}
}

void LuaG_EntityRemoved(edict_t *e)
{
	int i = NUM_FOR_EDICT(e);
	if (!announced || i < 0 || i >= announced_cap || announced[i] != e->serialnumber + 1)
		return;
	announced[i] = 0;
	FireEntityHook("EntityRemoved", e);
}

void LuaG_PlayerSpawned(edict_t *pl)
{
	FireEntityHook("PlayerInitialSpawn", pl);
	FireEntityHook("PlayerSpawn", pl);
}

void LuaG_PlayerDied(edict_t *pl)
{
	FireEntityHook("PlayerDeath", pl);
}

void LuaG_InitPostEntity(void)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	Lua_Hook("InitPostEntity", 0, 0);
	lua_settop(L, top);
}

void LuaG_Tick(void)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	Lua_Hook("Tick", 0, 0);
	lua_settop(L, top);
}

// PlayerCommand(ply, name, args): return true to keep the game from seeing it
qboolean LuaG_PlayerCommand(edict_t *pl)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return false;
	int top = lua_gettop(L);
	LuaG_PushEntity(L, pl);
	lua_pushstring(L, Cmd_Argv(0));
	lua_pushstring(L, Cmd_Args());
	qboolean eaten = false;
	if (Lua_Hook("PlayerCommand", 3, 1) == 1)
		eaten = lua_toboolean(L, -1);
	lua_settop(L, top);
	return eaten;
}

// StartCommand(ply, cmd): cmd is a table the hook may change (forwardmove, sidemove, upmove, buttons,
// impulse, viewangles)
void LuaG_StartCommand(edict_t *pl, usercmd_t *cmd)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	LuaG_PushEntity(L, pl);
	lua_createtable(L, 0, 6);
	int t = lua_gettop(L);
	lua_pushnumber(L, cmd->forwardmove); lua_setfield(L, t, "forwardmove");
	lua_pushnumber(L, cmd->sidemove);    lua_setfield(L, t, "sidemove");
	lua_pushnumber(L, cmd->upmove);      lua_setfield(L, t, "upmove");
	lua_pushinteger(L, cmd->buttons);    lua_setfield(L, t, "buttons");
	lua_pushinteger(L, cmd->impulse);    lua_setfield(L, t, "impulse");
	PushAngle(L, cmd->viewangles);       lua_setfield(L, t, "viewangles");
	lua_pushvalue(L, t);
	lua_insert(L, top + 1);   // keep a reference below the hook arguments
	int ref = top + 1;
	if (Lua_Hook("StartCommand", 2, 0) >= 0) {
		lua_getfield(L, ref, "forwardmove"); cmd->forwardmove = (float)lua_tonumber(L, -1);
		lua_getfield(L, ref, "sidemove");    cmd->sidemove = (float)lua_tonumber(L, -1);
		lua_getfield(L, ref, "upmove");      cmd->upmove = (float)lua_tonumber(L, -1);
		lua_getfield(L, ref, "buttons");     cmd->buttons = (unsigned short)lua_tointeger(L, -1);
		lua_getfield(L, ref, "impulse");     cmd->impulse = (unsigned char)lua_tointeger(L, -1);
		lua_getfield(L, ref, "viewangles");
		float *va = TestVec(L, -1);
		if (va)
			VectorCopy(va, cmd->viewangles);
	}
	lua_settop(L, top);
}

// EntityKeyValue(classname, key, value) while a map spawns: return a string to replace the value
const char *LuaG_EntityKeyValue(const char *classname, const char *key, const char *value)
{
	static char replaced[2048];
	lua_State *L = Lua_GetState();
	if (!L)
		return NULL;
	int top = lua_gettop(L);
	lua_pushstring(L, classname);
	lua_pushstring(L, key);
	lua_pushstring(L, value);
	const char *out = NULL;
	if (Lua_Hook("EntityKeyValue", 3, 1) == 1 && lua_type(L, -1) == LUA_TSTRING) {
		Q_strncpy(replaced, lua_tostring(L, -1), sizeof(replaced));
		out = replaced;
	}
	lua_settop(L, top);
	return out;
}
