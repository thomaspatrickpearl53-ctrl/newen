// newen - Lua: sound (sound.*, ent:EmitSound, the EntityEmitSound hook) and effects (effects.*, screen shake
// and fade)
//
// Effects are built as the same temporary-entity messages the game DLL sends, and handed to the client's own
// message reader (CL_ParseTempEntity), so they look, sound and fade exactly as the game's do.
//
// Sound levels are given in decibels, as in Garry's Mod (75 = a normal sound, 0 = heard everywhere), and
// turned into Half-Life's attenuation the way Source does: 20 / (level - 50), so 75 is the game's ATTN_NORM.
#include "lua_local.h"
#include "server.h"
#include "cl_local.h"

#define SND_STOP (1 << 5)

static float LevelToAttn(float level)
{
	if (level <= 0)
		return 0;   // everywhere
	return level > 50 ? 20.0f / (level - 50) : 4.0f;
}

static float AttnToLevel(float attn)
{
	return attn > 0 ? 50 + 20.0f / attn : 0;
}

static int EntNum(const edict_t *e)
{
	return e ? NUM_FOR_EDICT(e) : 0;
}

// ent:EmitSound(name, [level 75], [pitch 100], [volume 1], [channel CHAN_AUTO]); "!NAME" plays a sentence
static int m_EmitSound(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	const char *name = luaL_checkstring(L, 2);
	float attn = LevelToAttn((float)luaL_optnumber(L, 3, 75));
	int pitch = (int)luaL_optinteger(L, 4, 100);
	float vol = (float)luaL_optnumber(L, 5, 1);
	int chan = (int)luaL_optinteger(L, 6, 0);
	if (!LuaFX_GameSound(e, &chan, &name, &vol, &attn, 0, &pitch, NULL))
		return 0;
	S_StartSound(EntNum(e), chan, name, NULL, vol, attn, 0, pitch);
	return 0;
}

// ent:StopSound(name)
static int m_StopSound(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	S_StopSound(EntNum(e), -1, luaL_optstring(L, 2, NULL));
	return 0;
}

// sound.Play(name, pos, [level 75], [pitch 100], [volume 1]): a sound at a place in the world
static int l_sound_Play(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	vec3_t pos;
	VectorCopy(LuaG_CheckVec(L, 2), pos);
	float attn = LevelToAttn((float)luaL_optnumber(L, 3, 75));
	int pitch = (int)luaL_optinteger(L, 4, 100);
	float vol = (float)luaL_optnumber(L, 5, 1);
	int chan = 0;
	if (!LuaFX_GameSound(NULL, &chan, &name, &vol, &attn, 0, &pitch, pos))
		return 0;
	S_StartSound(0, chan, name, pos, vol, attn, 0, pitch);
	return 0;
}

// surface.PlaySound(name): a sound for the player alone, not placed in the world (menus, HUD)
static int l_surface_PlaySound(lua_State *L)
{
	S_LocalSound(luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 1));
	return 0;
}

// The hook can't call itself through a sound it plays
static qboolean in_hook;

qboolean LuaFX_GameSound(edict_t *e, int *channel, const char **sample, float *volume, float *attn, int flags,
	int *pitch, const float *pos)
{
	static char replaced[256];
	lua_State *L = Lua_GetState();
	if (!L || in_hook || (flags & SND_STOP) || !*sample)
		return true;
	int top = lua_gettop(L);
	lua_newtable(L);
	lua_pushstring(L, *sample);
	lua_setfield(L, -2, "SoundName");
	lua_pushstring(L, *sample);
	lua_setfield(L, -2, "OriginalSoundName");
	LuaG_PushEntity(L, e);
	lua_setfield(L, -2, "Entity");
	lua_pushinteger(L, *channel);
	lua_setfield(L, -2, "Channel");
	lua_pushnumber(L, *volume);
	lua_setfield(L, -2, "Volume");
	lua_pushnumber(L, AttnToLevel(*attn));
	lua_setfield(L, -2, "SoundLevel");
	lua_pushinteger(L, *pitch);
	lua_setfield(L, -2, "Pitch");
	lua_pushinteger(L, flags);
	lua_setfield(L, -2, "Flags");
	if (pos) {
		LuaG_PushVector(L, pos);
		lua_setfield(L, -2, "Pos");
	}
	lua_pushvalue(L, -1);   // keep the table to read changes back
	in_hook = true;
	int n = Lua_Hook("EntityEmitSound", 1, 1);
	in_hook = false;
	qboolean play = true;
	if (n == 1 && lua_type(L, -1) == LUA_TBOOLEAN) {
		if (!lua_toboolean(L, -1)) {
			play = false;
		} else {
			// true: the table's changes apply
			lua_getfield(L, -2, "SoundName");
			if (lua_type(L, -1) == LUA_TSTRING) {
				Q_strncpy(replaced, lua_tostring(L, -1), sizeof(replaced));
				*sample = replaced;
			}
			lua_getfield(L, -3, "Volume");
			if (lua_isnumber(L, -1))
				*volume = (float)lua_tonumber(L, -1);
			lua_getfield(L, -4, "SoundLevel");
			if (lua_isnumber(L, -1))
				*attn = LevelToAttn((float)lua_tonumber(L, -1));
			lua_getfield(L, -5, "Pitch");
			if (lua_isnumber(L, -1))
				*pitch = (int)lua_tointeger(L, -1);
			lua_getfield(L, -6, "Channel");
			if (lua_isnumber(L, -1))
				*channel = (int)lua_tointeger(L, -1);
		}
	}
	lua_settop(L, top);
	return play;
}

// ---------------------------------------------------------------- effects

typedef struct { unsigned char data[128]; int len; } temsg_t;
static void PutByte(temsg_t *m, int v)  { if (m->len < (int)sizeof(m->data)) m->data[m->len++] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void PutShort(temsg_t *m, int v) { short s = (short)v; if (m->len + 2 <= (int)sizeof(m->data)) { memcpy(m->data + m->len, &s, 2); m->len += 2; } }
static void PutCoord(temsg_t *m, float v) { if (m->len + 4 <= (int)sizeof(m->data)) { memcpy(m->data + m->len, &v, 4); m->len += 4; } }
static void PutVec(temsg_t *m, const float *v) { PutCoord(m, v[0]); PutCoord(m, v[1]); PutCoord(m, v[2]); }

static void SendTE(lua_State *L, const temsg_t *m)
{
	if (!sv.active)
		luaL_error(L, "effects need a running map");
	CL_ParseTempEntity(m->data, m->len);
}

// A sprite's model number, precaching it if the game hasn't
static int SpriteIndex(const char *name)
{
	return SV_PrecacheModel(name);
}

// Color(r, g, b, a) tables, or three numbers from idx
static void ReadColor(lua_State *L, int idx, int *r, int *g, int *b, int dr, int dg, int db)
{
	*r = dr; *g = dg; *b = db;
	if (lua_istable(L, idx)) {
		lua_getfield(L, idx, "r"); *r = (int)luaL_optinteger(L, -1, dr);
		lua_getfield(L, idx, "g"); *g = (int)luaL_optinteger(L, -1, dg);
		lua_getfield(L, idx, "b"); *b = (int)luaL_optinteger(L, -1, db);
		lua_pop(L, 3);
	}
}

static float FieldNum(lua_State *L, int t, const char *k, float def)
{
	lua_getfield(L, t, k);
	float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return v;
}

// effects.Explosion(pos, [scale 1], [nosound], [nolight]): the game's fireball, with its sound and light
static int l_fx_Explosion(lua_State *L)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_EXPLOSION);
	PutVec(&m, LuaG_CheckVec(L, 1));
	PutShort(&m, SpriteIndex("sprites/zerogxplode.spr"));
	PutByte(&m, (int)(luaL_optnumber(L, 2, 1) * 30));
	PutByte(&m, 15);
	PutByte(&m, (lua_toboolean(L, 3) ? 4 : 0) | (lua_toboolean(L, 4) ? 2 : 0));   // TE_EXPLFLAG_NOSOUND, _NODLIGHTS
	SendTE(L, &m);
	return 0;
}

// effects.Smoke(pos, [scale 1])
static int l_fx_Smoke(lua_State *L)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_SMOKE);
	PutVec(&m, LuaG_CheckVec(L, 1));
	PutShort(&m, SpriteIndex("sprites/steam1.spr"));
	PutByte(&m, (int)(luaL_optnumber(L, 2, 1) * 20));
	PutByte(&m, 10);
	SendTE(L, &m);
	return 0;
}

// effects.Sparks(pos), effects.Ricochet(pos) (a bullet's puff), effects.Teleport(pos)
static int PointTE(lua_State *L, int type)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, type);
	PutVec(&m, LuaG_CheckVec(L, 1));
	SendTE(L, &m);
	return 0;
}
static int l_fx_Sparks(lua_State *L)   { return PointTE(L, TE_SPARKS); }
static int l_fx_Ricochet(lua_State *L) { return PointTE(L, TE_GUNSHOT); }
static int l_fx_Teleport(lua_State *L) { return PointTE(L, TE_TELEPORT); }

// effects.Tracer(from, to)
static int l_fx_Tracer(lua_State *L)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_TRACER);
	PutVec(&m, LuaG_CheckVec(L, 1));
	PutVec(&m, LuaG_CheckVec(L, 2));
	SendTE(L, &m);
	return 0;
}

// effects.Blood(pos, dir, [color 247 red, 195 yellow], [amount 50])
static int l_fx_Blood(lua_State *L)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_BLOOD);
	PutVec(&m, LuaG_CheckVec(L, 1));
	PutVec(&m, LuaG_CheckVec(L, 2));
	PutByte(&m, (int)luaL_optinteger(L, 3, 247));
	PutByte(&m, (int)luaL_optinteger(L, 4, 50));
	SendTE(L, &m);
	return 0;
}

// effects.Light{pos, radius 200, color Color(255, 200, 150), time 0.5, decay 0}: a dynamic light
static int l_fx_Light(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "pos");
	vec3_t pos;
	VectorCopy(LuaG_CheckVec(L, -1), pos);
	lua_getfield(L, 1, "color");
	int r, g, b;
	ReadColor(L, lua_gettop(L), &r, &g, &b, 255, 200, 150);
	lua_settop(L, 1);
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_DLIGHT);
	PutVec(&m, pos);
	PutByte(&m, (int)(FieldNum(L, 1, "radius", 200) / 10));
	PutByte(&m, r);
	PutByte(&m, g);
	PutByte(&m, b);
	PutByte(&m, (int)(FieldNum(L, 1, "time", 0.5f) * 10));
	PutByte(&m, (int)(FieldNum(L, 1, "decay", 0) / 10));
	SendTE(L, &m);
	return 0;
}

// effects.Beam{start, endpos, sprite "sprites/laserbeam.spr", width 2, noise 0, life 0.5, color, brightness 255,
// speed 0}: a beam between two points
static int l_fx_Beam(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	vec3_t a, b;
	lua_getfield(L, 1, "start");
	VectorCopy(LuaG_CheckVec(L, -1), a);
	lua_getfield(L, 1, "endpos");
	VectorCopy(LuaG_CheckVec(L, -1), b);
	lua_getfield(L, 1, "sprite");
	int sprite = SpriteIndex(luaL_optstring(L, -1, "sprites/laserbeam.spr"));
	lua_getfield(L, 1, "color");
	int r, g, bl;
	ReadColor(L, lua_gettop(L), &r, &g, &bl, 255, 255, 255);
	lua_settop(L, 1);
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_BEAMPOINTS);
	PutVec(&m, a);
	PutVec(&m, b);
	PutShort(&m, sprite);
	PutByte(&m, 0);                                            // start frame
	PutByte(&m, 10);                                           // frame rate
	PutByte(&m, (int)(FieldNum(L, 1, "life", 0.5f) * 10));
	PutByte(&m, (int)(FieldNum(L, 1, "width", 2) * 10));
	PutByte(&m, (int)(FieldNum(L, 1, "noise", 0) * 100));
	PutByte(&m, r);
	PutByte(&m, g);
	PutByte(&m, bl);
	PutByte(&m, (int)FieldNum(L, 1, "brightness", 255));
	PutByte(&m, (int)(FieldNum(L, 1, "speed", 0) * 10));
	SendTE(L, &m);
	return 0;
}

// effects.Sprite(pos, sprite, [scale 1], [brightness 255]): a sprite shown once, additive
static int l_fx_Sprite(lua_State *L)
{
	temsg_t m = { { 0 }, 0 };
	PutByte(&m, TE_SPRITE);
	PutVec(&m, LuaG_CheckVec(L, 1));
	PutShort(&m, SpriteIndex(luaL_checkstring(L, 2)));
	PutByte(&m, (int)(luaL_optnumber(L, 3, 1) * 10));
	PutByte(&m, (int)luaL_optinteger(L, 4, 255));
	SendTE(L, &m);
	return 0;
}

// util.Decal(name, start, endpos, [filter]): the decal where a line from start to endpos hits the world or a
// brush entity, as Garry's Mod's
static int l_util_Decal(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	vec3_t a, b;
	VectorCopy(LuaG_CheckVec(L, 2), a);
	VectorCopy(LuaG_CheckVec(L, 3), b);
	edict_t *filter = lua_isnoneornil(L, 4) ? NULL : LuaG_ToEdict(L, 4);
	if (!sv.active)
		return luaL_error(L, "util.Decal needs a running map");
	static const vec3_t zero = { 0, 0, 0 };
	trace_t tr = SV_Move(a, zero, zero, b, MOVE_NOMONSTERS, filter);
	int tex = R_DecalIndex(name);
	qboolean ok = tr.fraction < 1 && tr.ent && tr.ent->v.solid == SOLID_BSP && tex >= 0;
	if (ok)
		R_DecalShoot(tex, NUM_FOR_EDICT(tr.ent), 0, tr.endpos, 0, 1.0f);
	lua_pushboolean(L, ok);
	return 1;
}

static void U16(unsigned char *p, float v)
{
	int i = v < 0 ? 0 : v > 65535 ? 65535 : (int)v;
	p[0] = (unsigned char)i;
	p[1] = (unsigned char)(i >> 8);
}

// util.ScreenShake(pos, amplitude, frequency, duration, radius): shakes the view if the player is within
// radius (weaker further away), as Half-Life's own shakes; radius 0 = everywhere
static int l_util_ScreenShake(lua_State *L)
{
	const float *pos = LuaG_CheckVec(L, 1);
	float amp = (float)luaL_checknumber(L, 2), freq = (float)luaL_checknumber(L, 3);
	float dur = (float)luaL_checknumber(L, 4), radius = (float)luaL_optnumber(L, 5, 0);
	edict_t *pl = SV_LocalPlayer();
	if (!pl)
		return 0;
	if (radius > 0) {
		vec3_t d;
		VectorSubtract(pl->v.origin, pos, d);
		float dist = VectorLength(d);
		if (dist >= radius)
			return 0;
		amp *= 1 - dist / radius;
	}
	unsigned char msg[6];
	U16(msg, amp * 4096);
	U16(msg + 2, dur * 4096);
	U16(msg + 4, freq * 256);
	CL_EngineMessage("ScreenShake", msg, 6);
	return 0;
}

// ply:ScreenFade(flags, color, fadetime, holdtime): SCREENFADE.IN fades from the colour, OUT to it
static int m_ScreenFade(lua_State *L)
{
	edict_t *e = LuaG_CheckEdict(L, 1);
	if (e != SV_LocalPlayer())
		return 0;
	int flags = (int)luaL_checkinteger(L, 2);
	int r, g, b;
	ReadColor(L, 3, &r, &g, &b, 0, 0, 0);
	int a = 255;
	if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "a");
		a = (int)luaL_optinteger(L, -1, 255);
		lua_pop(L, 1);
	}
	unsigned char msg[10];
	U16(msg, (float)luaL_checknumber(L, 4) * 4096);
	U16(msg + 2, (float)luaL_optnumber(L, 5, 0) * 4096);
	msg[4] = (unsigned char)flags;
	msg[5] = (unsigned char)(flags >> 8);
	msg[6] = (unsigned char)r;
	msg[7] = (unsigned char)g;
	msg[8] = (unsigned char)b;
	msg[9] = (unsigned char)(a < 0 ? 0 : a > 255 ? 255 : a);
	CL_EngineMessage("ScreenFade", msg, 10);
	return 0;
}

void LuaFX_Open(lua_State *L)
{
	static const luaL_Reg methods[] = {
		{ "EmitSound", m_EmitSound }, { "StopSound", m_StopSound }, { "ScreenFade", m_ScreenFade }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);

	static const luaL_Reg sound[] = { { "Play", l_sound_Play }, { NULL, NULL } };
	luaL_newlib(L, sound);
	lua_setglobal(L, "sound");

	static const luaL_Reg fx[] = {
		{ "Explosion", l_fx_Explosion }, { "Smoke", l_fx_Smoke }, { "Sparks", l_fx_Sparks },
		{ "Ricochet", l_fx_Ricochet }, { "Teleport", l_fx_Teleport }, { "Tracer", l_fx_Tracer },
		{ "Blood", l_fx_Blood }, { "Light", l_fx_Light }, { "Beam", l_fx_Beam }, { "Sprite", l_fx_Sprite },
		{ NULL, NULL }
	};
	luaL_newlib(L, fx);
	lua_setglobal(L, "effects");

	// util is the game bindings' table, made before this
	lua_getglobal(L, "util");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, l_util_Decal);
		lua_setfield(L, -2, "Decal");
		lua_pushcfunction(L, l_util_ScreenShake);
		lua_setfield(L, -2, "ScreenShake");
	}
	lua_pop(L, 1);

	lua_newtable(L);
	static const struct { const char *name; int value; } fade[] = { { "IN", 0 }, { "OUT", 1 }, { "MODULATE", 2 }, { "STAYOUT", 4 } };
	for (size_t i = 0; i < 4; i++) {
		lua_pushinteger(L, fade[i].value);
		lua_setfield(L, -2, fade[i].name);
	}
	lua_setglobal(L, "SCREENFADE");

	// surface is the host's drawing table, made before this
	lua_getglobal(L, "surface");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, l_surface_PlaySound);
		lua_setfield(L, -2, "PlaySound");
	}
	lua_pop(L, 1);

	static const struct { const char *name; int value; } chans[] = {
		{ "CHAN_AUTO", 0 }, { "CHAN_WEAPON", 1 }, { "CHAN_VOICE", 2 }, { "CHAN_ITEM", 3 }, { "CHAN_BODY", 4 },
		{ "CHAN_STREAM", 5 }, { "CHAN_STATIC", 6 },
	};
	for (size_t i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
		lua_pushinteger(L, chans[i].value);
		lua_setglobal(L, chans[i].name);
	}
}
