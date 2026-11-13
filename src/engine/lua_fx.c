// newen - Lua: sound (sound.*, ent:EmitSound, the EntityEmitSound hook)
//
// Sound levels are given in decibels, as in Garry's Mod (75 = a normal sound, 0 = heard everywhere), and
// turned into Half-Life's attenuation the way Source does: 20 / (level - 50), so 75 is the game's ATTN_NORM.
#include "lua_local.h"
#include "server.h"

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

void LuaFX_Open(lua_State *L)
{
	static const luaL_Reg methods[] = {
		{ "EmitSound", m_EmitSound }, { "StopSound", m_StopSound }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);

	static const luaL_Reg sound[] = { { "Play", l_sound_Play }, { NULL, NULL } };
	luaL_newlib(L, sound);
	lua_setglobal(L, "sound");

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
