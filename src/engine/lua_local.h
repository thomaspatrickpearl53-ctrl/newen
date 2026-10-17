// newen - shared between the Lua host (lua_host.c) and the game bindings (lua_game.c)
#pragma once
#include "common.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// lua_host.c
lua_State *Lua_GetState(void);                          // NULL when Lua isn't running
qboolean   Lua_Call(int nargs, int nres, const char *where);   // protected call with timeout and traceback
int        Lua_Hook(const char *name, int nargs, int nres);    // hook.Run; results left, or -1

// lua_game.c
void       LuaG_Open(lua_State *L);                     // Vector, Angle, Entity, ents, player, util
struct edict_s;
void       LuaG_PushEntity(lua_State *L, struct edict_s *e);   // NULL becomes the invalid entity
struct edict_s *SV_LocalPlayer(void);
