// newen - scripted entities: ENT tables in Lua (scripted_ents.Register, lua/entities/*.lua)
//
// A Lua entity is a real edict with a real, harmless game object behind it: the game DLL's info_target
// (no model, no thinking of its own). The game's bullets and explosions assume everything they hit has
// an object, and with one they damage a Lua entity through its ordinary health handling. Its think,
// touch and use come here instead of going to the game DLL. The game's +use never finds it (an
// info_target isn't usable), so the player's use button is handled here too.
//
// Each Lua entity has a table of its own, keyed by edict number in the registry; its metatable leads to
// its class's ENT table, so self.anything reads the entity's own field, then its class's. Plain data in
// that table (numbers, strings, booleans, vectors, angles, entities, tables of those) goes into saved
// games and level states, and survives lua_reload.
#include "lua_local.h"
#include "server.h"

#define SENTS_KEY   "newen.sents"     // class name -> ENT
#define SMETA_KEY   "newen.smeta"     // class name -> the metatable every entity of that class gets
#define ENTDATA_KEY "newen.entdata"   // edict number -> the entity's own table
#define DMGINFO_MT  "DamageInfo"

// GMod's use types; SIMPLE_USE is once per press, the others every frame the button is held
enum { CONTINUOUS_USE = 0, ONOFF_USE = 1, DIRECTIONAL_USE = 2, SIMPLE_USE = 3 };
enum { USE_OFF = 0, USE_ON = 1, USE_SET = 2, USE_TOGGLE = 3 };

typedef struct {
	int   serial;      // serialnumber + 1 of the edict that is a Lua entity; 0 = not one
	int   usetype;
	float health;      // last frame's, to notice the game's damage
} sentslot_t;

static sentslot_t *slots;
static void ResolveAllBases(lua_State *L);
static int slots_cap;
static qboolean use_held;

static void ClearSlots(void)
{
	if (slots)
		memset(slots, 0, sizeof(*slots) * (size_t)slots_cap);
	use_held = false;
}

static sentslot_t *Slot(const edict_t *e)
{
	if (!e || !sv.edicts || e->free || !slots)
		return NULL;
	int i = NUM_FOR_EDICT(e);
	if (i < 0 || i >= slots_cap || slots[i].serial != e->serialnumber + 1)
		return NULL;
	return &slots[i];
}

qboolean LuaE_IsScripted(edict_t *e)
{
	return Slot(e) != NULL;
}

// ---------------------------------------------------------------- the entity's table

// pushes registry[key], creating it
static void PushRegTable(lua_State *L, const char *key)
{
	if (lua_getfield(L, LUA_REGISTRYINDEX, key) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
	}
}

// pushes the entity's own table; false (nothing pushed) if it isn't a Lua entity or has none
qboolean LuaE_PushTable(lua_State *L, struct edict_s *e)
{
	if (!Slot(e))
		return false;
	PushRegTable(L, ENTDATA_KEY);
	lua_rawgeti(L, -1, NUM_FOR_EDICT(e));
	lua_remove(L, -2);
	if (lua_istable(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

// a new, empty table for the entity, with its class's metatable
static void NewTable(lua_State *L, edict_t *e)
{
	PushRegTable(L, ENTDATA_KEY);
	lua_newtable(L);
	PushRegTable(L, SMETA_KEY);
	lua_getfield(L, -1, SV_String(e->v.classname));
	lua_remove(L, -2);
	if (lua_istable(L, -1))
		lua_setmetatable(L, -2);
	else
		lua_pop(L, 1);
	lua_rawseti(L, -2, NUM_FOR_EDICT(e));
	lua_pop(L, 1);
}

// pushes the class's function and the entity (as self); false (nothing pushed) if there's no such function
static qboolean PushMethod(lua_State *L, edict_t *e, const char *name)
{
	if (!LuaE_PushTable(L, e))
		return false;
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	LuaG_PushEntity(L, e);
	return true;
}

// self:name(...) with nargs already pushed after the method and self
static void CallMethod(edict_t *e, const char *name, int nargs)
{
	gGlobals.time = (float)sv.time;
	Lua_Call(nargs + 1, 0, va("%s:%s", SV_String(e->v.classname), name));
}

// ---------------------------------------------------------------- creation

static ENTITYINIT base_init;

// the class function for a Lua class: the game's info_target, then the entity becomes a Lua one
static void ScriptedInit(entvars_t *pev)
{
	base_init(pev);
	edict_t *e = pev->pContainingEntity;
	lua_State *L = Lua_GetState();
	if (!e || !L || !slots)
		return;
	int i = NUM_FOR_EDICT(e);
	if (i <= 0 || i >= slots_cap)
		return;
	slots[i].serial = e->serialnumber + 1;
	slots[i].usetype = CONTINUOUS_USE;
	slots[i].health = e->v.health;
	int top = lua_gettop(L);
	NewTable(L, e);
	lua_settop(L, top);
}

ENTITYINIT LuaE_EntityInit(const char *classname)
{
	lua_State *L = Lua_GetState();
	if (!L || !classname || !*classname)
		return NULL;
	int top = lua_gettop(L);
	PushRegTable(L, SENTS_KEY);
	lua_getfield(L, -1, classname);
	qboolean found = lua_istable(L, -1);
	lua_settop(L, top);
	if (!found)
		return NULL;
	if (!base_init)
		base_init = (ENTITYINIT)SV_GetProcAddress("info_target");
	return base_init ? ScriptedInit : NULL;
}

void LuaE_KeyValue(edict_t *e, const char *key, const char *value)
{
	lua_State *L = Lua_GetState();
	if (!L || !strcmp(key, "classname"))
		return;
	int top = lua_gettop(L);
	if (PushMethod(L, e, "KeyValue")) {
		lua_pushstring(L, key);
		lua_pushstring(L, value);
		CallMethod(e, "KeyValue", 2);
	}
	lua_settop(L, top);
}

// after the game's Spawn: Initialize, and Think from the next frame if the class has one
void LuaE_Spawned(edict_t *e)
{
	lua_State *L = Lua_GetState();
	sentslot_t *s = Slot(e);
	if (!L || !s)
		return;
	int top = lua_gettop(L);
	if (PushMethod(L, e, "Think")) {
		e->v.nextthink = (float)sv.time + 0.001f;
		lua_settop(L, top);
	}
	if (PushMethod(L, e, "Initialize"))
		CallMethod(e, "Initialize", 0);
	lua_settop(L, top);
	if ((s = Slot(e)) != NULL)
		s->health = e->v.health;
}

// ---------------------------------------------------------------- think, touch, use

// true if e is a Lua entity (then the game DLL isn't asked)
qboolean LuaE_Think(edict_t *e)
{
	if (!Slot(e))
		return false;
	lua_State *L = Lua_GetState();
	int top = L ? lua_gettop(L) : 0;
	if (L && PushMethod(L, e, "Think")) {
		CallMethod(e, "Think", 0);
		// without self:NextThink, it thinks again next frame (RunThink cleared nextthink before calling)
		if (!e->free && e->v.nextthink <= 0)
			e->v.nextthink = (float)sv.time + 0.001f;
	}
	if (L)
		lua_settop(L, top);
	return true;
}

qboolean LuaE_Touch(edict_t *e, edict_t *other)
{
	if (!Slot(e))
		return false;
	lua_State *L = Lua_GetState();
	int top = L ? lua_gettop(L) : 0;
	if (L && PushMethod(L, e, "Touch")) {
		LuaG_PushEntity(L, other);
		CallMethod(e, "Touch", 1);
	}
	if (L)
		lua_settop(L, top);
	return true;
}

static void CallUse(lua_State *L, edict_t *e, edict_t *activator, edict_t *caller, int type, float value)
{
	int top = lua_gettop(L);
	if (PushMethod(L, e, "Use")) {
		LuaG_PushEntity(L, activator);
		LuaG_PushEntity(L, caller);
		lua_pushinteger(L, type);
		lua_pushnumber(L, value);
		CallMethod(e, "Use", 4);
	}
	lua_settop(L, top);
}

qboolean LuaE_Use(edict_t *e, edict_t *activator)
{
	if (!Slot(e))
		return false;
	lua_State *L = Lua_GetState();
	if (L)
		CallUse(L, e, activator, activator, USE_TOGGLE, 0);
	return true;
}

static qboolean HasMethod(lua_State *L, edict_t *e, const char *name)
{
	int top = lua_gettop(L);
	qboolean has = PushMethod(L, e, name);
	lua_settop(L, top);
	return has;
}

// The Lua entity with a Use function the player is looking at within reach, or NULL. Reach and the
// view cone are close to what Half-Life's own +use allows.
static edict_t *FindUseTarget(lua_State *L, edict_t *pl, const vec3_t viewangles)
{
	vec3_t eye, forward;
	VectorAdd(pl->v.origin, pl->v.view_ofs, eye);
	AngleVectors(viewangles, forward, NULL, NULL);
	edict_t *best = NULL;
	float bestdot = 0.7f;
	for (int i = 1; i < sv.num_edicts && i < slots_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		if (!Slot(e))
			continue;
		// nearest point of its box to the eye
		vec3_t nearest, center, dir;
		for (int k = 0; k < 3; k++) {
			float v = eye[k];
			nearest[k] = v < e->v.absmin[k] ? e->v.absmin[k] : v > e->v.absmax[k] ? e->v.absmax[k] : v;
			center[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
		}
		VectorSubtract(nearest, eye, dir);
		if (VectorLength(dir) > 80)
			continue;
		VectorSubtract(center, eye, dir);
		VectorNormalize(dir);
		float dot = DotProduct(dir, forward);
		if (dot <= bestdot || !HasMethod(L, e, "Use"))
			continue;
		// not through a wall
		static const vec3_t zero = { 0, 0, 0 };
		trace_t tr = SV_Move(eye, zero, zero, center, MOVE_NOMONSTERS, pl);
		vec3_t hit;
		VectorSubtract(tr.endpos, eye, hit);
		VectorSubtract(nearest, eye, dir);
		if (tr.fraction < 1 && VectorLength(hit) < VectorLength(dir) - 1)
			continue;
		best = e;
		bestdot = dot;
	}
	return best;
}

// before the game sees the command: +use on a Lua entity goes to it, and the game doesn't see the button
// (it would find nothing of its own to use and play its "denied" sound)
void LuaE_PlayerUse(edict_t *pl, usercmd_t *cmd)
{
	qboolean held = (cmd->buttons & IN_USE) != 0, pressed = held && !use_held;
	use_held = held;
	lua_State *L = Lua_GetState();
	if (!held || !L || !sv.active || !slots)
		return;
	edict_t *e = FindUseTarget(L, pl, cmd->viewangles);
	if (!e)
		return;
	cmd->buttons &= (unsigned short)~IN_USE;
	sentslot_t *s = Slot(e);
	if (s->usetype == SIMPLE_USE && !pressed)
		return;
	CallUse(L, e, pl, pl, s->usetype == SIMPLE_USE ? USE_TOGGLE : USE_SET, 1);
}

// ---------------------------------------------------------------- damage

static void PushDamageInfo(lua_State *L, float amount, edict_t *attacker, edict_t *inflictor, int type)
{
	lua_createtable(L, 0, 4);
	lua_pushnumber(L, amount);
	lua_setfield(L, -2, "Damage");
	LuaG_PushEntity(L, attacker);
	lua_setfield(L, -2, "Attacker");
	LuaG_PushEntity(L, inflictor);
	lua_setfield(L, -2, "Inflictor");
	lua_pushinteger(L, type);
	lua_setfield(L, -2, "DamageType");
	luaL_setmetatable(L, DMGINFO_MT);
}

static void CallDamage(edict_t *e, float amount, edict_t *attacker, edict_t *inflictor, int type)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	if (PushMethod(L, e, "OnTakeDamage")) {
		PushDamageInfo(L, amount, attacker, inflictor, type);
		CallMethod(e, "OnTakeDamage", 1);
	}
	lua_settop(L, top);
}

static edict_t *Live(edict_t *e)
{
	return e && !e->free ? e : NULL;
}

// damage the engine or Lua dealt (it knows who and how); the health it took is already gone
void LuaE_Damaged(edict_t *e, float amount, edict_t *attacker, edict_t *inflictor, int type)
{
	sentslot_t *s = Slot(e);
	if (!s)
		return;
	s->health = e->v.health;
	CallDamage(e, amount, Live(attacker), Live(inflictor), type);
}

// the game's own damage shows only as lost health; dmg_inflictor says who, when the game set it
static void CheckHealth(edict_t *e, sentslot_t *s)
{
	float lost = s->health - e->v.health;
	s->health = e->v.health;
	if (lost <= 0)
		return;
	edict_t *inf = Live(e->v.dmg_inflictor);
	edict_t *attacker = inf && Live(inf->v.owner) ? inf->v.owner : inf;
	CallDamage(e, lost, attacker, inf, 0);
}

void LuaE_Removed(edict_t *e)
{
	sentslot_t *s = Slot(e);
	if (!s)
		return;
	lua_State *L = Lua_GetState();
	if (L) {
		CheckHealth(e, s);   // the shot that killed it removed it in the same frame
		int top = lua_gettop(L);
		if (PushMethod(L, e, "OnRemove"))
			CallMethod(e, "OnRemove", 0);
		PushRegTable(L, ENTDATA_KEY);
		lua_pushnil(L);
		lua_rawseti(L, -2, NUM_FOR_EDICT(e));
		lua_settop(L, top);
	}
	if ((s = Slot(e)) != NULL)
		s->serial = 0;
}

void LuaE_Frame(void)
{
	if (!Lua_GetState() || !sv.active || !slots)
		return;
	for (int i = 1; i < sv.num_edicts && i < slots_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		sentslot_t *s = Slot(e);
		if (s)
			CheckHealth(e, s);
	}
}

// ---------------------------------------------------------------- drawing

void LuaE_Draw(void)
{
	lua_State *L = Lua_GetState();
	if (!L || !sv.active || !slots)
		return;
	int top = lua_gettop(L);
	for (int i = 1; i < sv.num_edicts && i < slots_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		if (Slot(e) && PushMethod(L, e, "Draw"))
			CallMethod(e, "Draw", 0);
		lua_settop(L, top);
	}
}

// ---------------------------------------------------------------- saving the entities' tables

// The format: per entity its edict number, use type and table. A value is a tag byte and its data; a
// table is key/value pairs up to 'z'. Entities are stored by edict number: a restore maps the numbers of
// the saved level to the edicts that came back.
typedef struct {
	unsigned char *data;
	int size, cap;
} wbuf_t;

static void Put(wbuf_t *b, const void *p, int n)
{
	if (b->size + n > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 1024;
		while (b->cap < b->size + n)
			b->cap *= 2;
		b->data = Mem_Realloc(b->data, (size_t)b->cap);
	}
	memcpy(b->data + b->size, p, (size_t)n);
	b->size += n;
}

static void PutByte(wbuf_t *b, int c) { unsigned char u = (unsigned char)c; Put(b, &u, 1); }
static void PutInt(wbuf_t *b, int v) { Put(b, &v, 4); }

static qboolean Storable(lua_State *L, int idx, int depth)
{
	switch (lua_type(L, idx)) {
	case LUA_TNUMBER: case LUA_TSTRING: case LUA_TBOOLEAN: return true;
	case LUA_TTABLE: return depth < 32;
	case LUA_TUSERDATA:
		return luaL_testudata(L, idx, "Vector") || luaL_testudata(L, idx, "Angle") || luaL_testudata(L, idx, "Entity");
	default: return false;
	}
}

static void PutTable(lua_State *L, wbuf_t *b, int idx, int depth);

static void PutValue(lua_State *L, wbuf_t *b, int idx, int depth)
{
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx)) {
			long long v = (long long)lua_tointeger(L, idx);
			PutByte(b, 'i');
			Put(b, &v, 8);
		} else {
			double v = (double)lua_tonumber(L, idx);
			PutByte(b, 'n');
			Put(b, &v, 8);
		}
		break;
	case LUA_TSTRING: {
		size_t n;
		const char *s = lua_tolstring(L, idx, &n);
		PutByte(b, 's');
		PutInt(b, (int)n);
		Put(b, s, (int)n);
		break;
	}
	case LUA_TBOOLEAN:
		PutByte(b, 'b');
		PutByte(b, lua_toboolean(L, idx));
		break;
	case LUA_TTABLE:
		PutTable(L, b, idx, depth + 1);
		break;
	case LUA_TUSERDATA: {
		float *v = luaL_testudata(L, idx, "Vector");
		float *a = luaL_testudata(L, idx, "Angle");
		if (v || a) {
			PutByte(b, v ? 'v' : 'a');
			Put(b, v ? v : a, 12);
		} else {
			edict_t *e = LuaG_ToEdict(L, idx);
			PutByte(b, 'e');
			PutInt(b, e ? NUM_FOR_EDICT(e) : -1);
		}
		break;
	}
	}
}

// Marks or unmarks a table in the set of tables open on the branch being written
static void MarkSaving(lua_State *L, int idx, qboolean on)
{
	idx = lua_absindex(L, idx);
	lua_getfield(L, LUA_REGISTRYINDEX, "newen.saving");
	lua_pushvalue(L, idx);
	if (on)
		lua_pushboolean(L, 1);
	else
		lua_pushnil(L);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

static qboolean IsSaving(lua_State *L, int idx)
{
	idx = lua_absindex(L, idx);
	lua_getfield(L, LUA_REGISTRYINDEX, "newen.saving");
	lua_pushvalue(L, idx);
	qboolean open = lua_rawget(L, -2) != LUA_TNIL;
	lua_pop(L, 2);
	return open;
}

// a table seen higher up the same branch is left out (a loop), as is anything that isn't plain data
static void PutTable(lua_State *L, wbuf_t *b, int idx, int depth)
{
	PutByte(b, 't');
	idx = lua_absindex(L, idx);
	MarkSaving(L, idx, true);
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		int kt = lua_type(L, -2);
		if ((kt == LUA_TSTRING || kt == LUA_TNUMBER || kt == LUA_TBOOLEAN) && Storable(L, -1, depth) &&
			!(lua_istable(L, -1) && IsSaving(L, -1))) {
			PutValue(L, b, -2, depth);
			PutValue(L, b, -1, depth);
		}
		lua_pop(L, 1);
	}
	MarkSaving(L, idx, false);
	PutByte(b, 'z');
}

// every Lua entity's table; NULL if there are none
unsigned char *LuaE_SaveData(int *size)
{
	*size = 0;
	lua_State *L = Lua_GetState();
	if (!L || !sv.active || !slots)
		return NULL;
	int top = lua_gettop(L);
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "newen.saving");
	wbuf_t b = { NULL, 0, 0 };
	int count = 0;
	PutInt(&b, 0);   // count, filled in below
	for (int i = 1; i < sv.num_edicts && i < slots_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		sentslot_t *s = Slot(e);
		if (!s || !LuaE_PushTable(L, e))
			continue;
		PutInt(&b, i);
		PutInt(&b, s->usetype);
		PutTable(L, &b, -1, 0);
		lua_settop(L, top);
		count++;
	}
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "newen.saving");
	lua_settop(L, top);
	if (!count) {
		Mem_Free(b.data);
		return NULL;
	}
	memcpy(b.data, &count, 4);
	*size = b.size;
	return b.data;
}

typedef struct {
	const unsigned char *p, *end;
	qboolean bad;
	edict_t **byindex;   // saved edict number -> edict now; NULL: the numbers are still right
	int count;
} rbuf_t;

static qboolean Get(rbuf_t *r, void *out, int n)
{
	if (r->bad || r->end - r->p < n) {
		r->bad = true;
		memset(out, 0, (size_t)n);
		return false;
	}
	memcpy(out, r->p, (size_t)n);
	r->p += n;
	return true;
}

static int GetInt(rbuf_t *r) { int v; Get(r, &v, 4); return v; }

static edict_t *MapIndex(rbuf_t *r, int i)
{
	if (r->byindex)
		return i >= 0 && i < r->count ? Live(r->byindex[i]) : NULL;
	return i >= 0 && i < sv.num_edicts ? Live(EDICT_NUM(i)) : NULL;
}

// pushes one value (nil for an end marker or damage; *end says which)
static void GetValue(lua_State *L, rbuf_t *r, qboolean *end, int depth)
{
	unsigned char tag = 0;
	*end = false;
	Get(r, &tag, 1);
	switch (tag) {
	case 'i': { long long v; Get(r, &v, 8); lua_pushinteger(L, (lua_Integer)v); return; }
	case 'n': { double v; Get(r, &v, 8); lua_pushnumber(L, (lua_Number)v); return; }
	case 'b': { unsigned char v; Get(r, &v, 1); lua_pushboolean(L, v); return; }
	case 's': {
		int n = GetInt(r);
		if (n < 0 || r->end - r->p < n) {
			r->bad = true;
			break;
		}
		lua_pushlstring(L, (const char *)r->p, (size_t)n);
		r->p += n;
		return;
	}
	case 'v': case 'a': { float v[3]; Get(r, v, 12); (tag == 'v' ? LuaG_PushVector : LuaG_PushAngle)(L, v); return; }
	case 'e': LuaG_PushEntity(L, MapIndex(r, GetInt(r))); return;
	case 't': {
		if (depth > 32) {
			r->bad = true;
			break;
		}
		lua_newtable(L);
		for (;;) {
			qboolean kend, vend;
			GetValue(L, r, &kend, depth + 1);
			if (kend || r->bad) {
				lua_pop(L, 1);
				break;
			}
			GetValue(L, r, &vend, depth + 1);
			if (vend || r->bad || lua_isnil(L, -2)) {
				lua_pop(L, 2);
				if (vend || r->bad)
					break;
				continue;
			}
			lua_rawset(L, -3);
		}
		return;
	}
	case 'z': *end = true; break;
	default: r->bad = true; break;
	}
	lua_pushnil(L);
}

// the data goes into the tables of the Lua entities that came back; then OnRestore (or after
// lua_reload, OnReloaded)
static void Restore(const unsigned char *data, int size, edict_t **byindex, int count, const char *after)
{
	lua_State *L = Lua_GetState();
	if (!L || !slots)
		return;
	int top = lua_gettop(L);
	rbuf_t r = { data, data + size, false, byindex, count };
	int n = data ? GetInt(&r) : 0;
	for (int k = 0; k < n && !r.bad; k++) {
		edict_t *e = MapIndex(&r, GetInt(&r));
		int usetype = GetInt(&r);
		qboolean end;
		GetValue(L, &r, &end, 0);
		if (r.bad || !lua_istable(L, -1) || !LuaE_PushTable(L, e)) {
			lua_settop(L, top);
			continue;
		}
		Slot(e)->usetype = usetype;
		lua_pushnil(L);
		while (lua_next(L, -3)) {
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			lua_rawset(L, -4);
		}
		lua_settop(L, top);
	}
	if (r.bad)
		Con_Warnf("Lua entities: saved data is damaged; some entities lost theirs\n");
	for (int i = 1; i < sv.num_edicts && i < slots_cap; i++) {
		edict_t *e = EDICT_NUM(i);
		sentslot_t *s = Slot(e);
		if (!s)
			continue;
		s->health = e->v.health;
		if (PushMethod(L, e, after))
			CallMethod(e, after, 0);
		lua_settop(L, top);
	}
}

void LuaE_RestoreData(const unsigned char *data, int size, edict_t **byindex, int count)
{
	Restore(data, size, byindex, count, "OnRestore");
}

// lua_reload: the tables are kept across the new Lua state
static unsigned char *reload_data;
static int reload_size;

void LuaE_StateStopping(void)
{
	Mem_Free(reload_data);
	reload_data = LuaE_SaveData(&reload_size);
}

void LuaE_StateStarted(void)
{
	lua_State *L = Lua_GetState();
	if (L)
		ResolveAllBases(L);
	if (L && sv.active && slots) {
		int top = lua_gettop(L);
		for (int i = 1; i < sv.num_edicts && i < slots_cap; i++)
			if (Slot(EDICT_NUM(i)))
				NewTable(L, EDICT_NUM(i));
		lua_settop(L, top);
		Restore(reload_data, reload_size, NULL, 0, "OnReloaded");
	}
	Mem_Free(reload_data);
	reload_data = NULL;
	reload_size = 0;
}

void LuaE_ServerStarted(void)
{
	if (slots_cap < sv.max_edicts) {
		slots_cap = sv.max_edicts;
		slots = Mem_Realloc(slots, sizeof(*slots) * (size_t)slots_cap);
	}
	ClearSlots();
	lua_State *L = Lua_GetState();
	if (L) {
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, ENTDATA_KEY);
	}
}

// ---------------------------------------------------------------- Lua

// ENT.Base names another class: ENT reads what it lacks from it, and ENT.BaseClass is it. False if the base
// isn't registered (yet: the files load alphabetically, so the bases are settled again after all of them)
static qboolean ResolveBase(lua_State *L, int ent)
{
	ent = lua_absindex(L, ent);
	int top = lua_gettop(L);
	qboolean ok = true;
	if (lua_getfield(L, ent, "Base") == LUA_TSTRING) {
		PushRegTable(L, SENTS_KEY);
		lua_getfield(L, -1, lua_tostring(L, -2));
		if (lua_istable(L, -1) && !lua_rawequal(L, -1, ent)) {
			lua_pushvalue(L, -1);
			lua_setfield(L, ent, "BaseClass");
			lua_createtable(L, 0, 1);
			lua_pushvalue(L, -2);
			lua_setfield(L, -2, "__index");
			lua_setmetatable(L, ent);
		} else {
			ok = false;
		}
	}
	lua_settop(L, top);
	return ok;
}

// scripted_ents.Register(ENT, classname)
static int l_Register(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	const char *cls = luaL_checkstring(L, 2);
	lua_settop(L, 2);
	lua_pushvalue(L, 2);
	lua_setfield(L, 1, "ClassName");
	ResolveBase(L, 1);
	PushRegTable(L, SENTS_KEY);
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, cls);
	// entities of this class that already exist pick up the new table (a file run again)
	PushRegTable(L, SMETA_KEY);
	if (lua_getfield(L, -1, cls) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_createtable(L, 0, 1);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, cls);
	}
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "__index");
	return 0;
}

// after lua/entities/*.lua: bases registered after the classes that use them
static void ResolveAllBases(lua_State *L)
{
	int top = lua_gettop(L);
	PushRegTable(L, SENTS_KEY);
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		if (lua_istable(L, -1) && !ResolveBase(L, -1)) {
			lua_getfield(L, -1, "Base");
			Con_Warnf("[Lua] %s: base class %s isn't registered\n", lua_tostring(L, -3), lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_settop(L, top);
}

static int l_Get(lua_State *L)
{
	PushRegTable(L, SENTS_KEY);
	lua_getfield(L, -1, luaL_checkstring(L, 1));
	return 1;
}

static int l_GetList(lua_State *L)
{
	lua_newtable(L);
	PushRegTable(L, SENTS_KEY);
	int n = 0;
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
		lua_rawseti(L, -4, ++n);
	}
	lua_pop(L, 1);
	return 1;
}

// self:NextThink(CurTime() + delay), as in GMod: CurTime is newen's clock, the entity thinks on the
// server's, so only the delay is kept
static int m_NextThink(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	double delay = luaL_checknumber(L, 2) - host_realtime;
	e->v.nextthink = (float)(sv.time + (delay > 0.001 ? delay : 0.001));
	return 0;
}

// the running map's clock: stops while the game is paused and comes back with a saved game, so times
// kept in an entity's table stay right (CurTime is the engine's, from the start of the program)
static int l_ServerTime(lua_State *L)
{
	lua_pushnumber(L, sv.active ? sv.time : 0);
	return 1;
}

static int m_SetUseType(lua_State *L)
{
	sentslot_t *s = Slot(LuaG_CheckEdict(L, 1));
	if (s)
		s->usetype = (int)luaL_checkinteger(L, 2);
	return 0;
}

static int m_IsScripted(lua_State *L)
{
	lua_pushboolean(L, Slot(LuaG_ToEdict(L, 1)) != NULL);
	return 1;
}

static int m_GetTable(lua_State *L)
{
	if (!LuaE_PushTable(L, LuaG_CheckEdict(L, 1)))
		lua_pushnil(L);
	return 1;
}

static int d_GetDamage(lua_State *L)     { luaL_checktype(L, 1, LUA_TTABLE); lua_getfield(L, 1, "Damage"); return 1; }
static int d_GetAttacker(lua_State *L)   { luaL_checktype(L, 1, LUA_TTABLE); lua_getfield(L, 1, "Attacker"); return 1; }
static int d_GetInflictor(lua_State *L)  { luaL_checktype(L, 1, LUA_TTABLE); lua_getfield(L, 1, "Inflictor"); return 1; }
static int d_GetDamageType(lua_State *L) { luaL_checktype(L, 1, LUA_TTABLE); lua_getfield(L, 1, "DamageType"); return 1; }

void LuaE_Open(lua_State *L)
{
	static const luaL_Reg lib[] = { { "Register", l_Register }, { "Get", l_Get }, { "GetList", l_GetList }, { NULL, NULL } };
	luaL_newlib(L, lib);
	lua_setglobal(L, "scripted_ents");
	lua_register(L, "ServerTime", l_ServerTime);

	static const luaL_Reg methods[] = {
		{ "NextThink", m_NextThink }, { "SetUseType", m_SetUseType }, { "IsScripted", m_IsScripted },
		{ "GetTable", m_GetTable }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);

	luaL_newmetatable(L, DMGINFO_MT);
	static const luaL_Reg dmethods[] = {
		{ "GetDamage", d_GetDamage }, { "GetAttacker", d_GetAttacker }, { "GetInflictor", d_GetInflictor },
		{ "GetDamageType", d_GetDamageType }, { NULL, NULL }
	};
	luaL_newlib(L, dmethods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	static const struct { const char *name; int v; } consts[] = {
		{ "CONTINUOUS_USE", CONTINUOUS_USE }, { "ONOFF_USE", ONOFF_USE }, { "DIRECTIONAL_USE", DIRECTIONAL_USE },
		{ "SIMPLE_USE", SIMPLE_USE }, { "USE_OFF", USE_OFF }, { "USE_ON", USE_ON }, { "USE_SET", USE_SET },
		{ "USE_TOGGLE", USE_TOGGLE }, { "DAMAGE_NO", 0 }, { "DAMAGE_YES", 1 }, { "DAMAGE_AIM", 2 },
	};
	for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++) {
		lua_pushinteger(L, consts[i].v);
		lua_setglobal(L, consts[i].name);
	}
}
