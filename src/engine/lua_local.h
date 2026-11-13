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
struct edict_s *LuaG_ToEdict(lua_State *L, int idx);      // NULL if not a valid entity
struct edict_s *LuaG_CheckEdict(lua_State *L, int idx);   // a Lua error if not
float          *LuaG_CheckVec(lua_State *L, int idx);
void            LuaG_PushVector(lua_State *L, const float *v);
void            LuaG_PushAngle(lua_State *L, const float *v);
void            LuaG_AddEntityMethods(lua_State *L, const luaL_Reg *regs);

// lua_render.c: render.*, ClientsideModel, Material, Vector:ToScreen; opened by LuaG_Open
void            LuaR_Open(lua_State *L);
unsigned int    LuaR_MaterialTexture(lua_State *L, int idx);   // a Material's GL texture

// lua_ext.c: the smaller entity/player calls and the KeyPress, OnNPCKilled, CalcView hooks; opened by LuaG_Open
void            LuaX_Open(lua_State *L);
// lua_ents.c: scripted_ents, the entities' own tables; opened by LuaG_Open
void            LuaE_Open(lua_State *L);
qboolean        LuaE_PushTable(lua_State *L, struct edict_s *e);   // false (nothing pushed) if not a Lua entity
void            LuaE_Draw(void);           // ENT:Draw, inside PostDrawOpaque
void            LuaE_StateStopping(void);  // lua_reload: the tables are kept...
void            LuaE_StateStarted(void);   // ...and put back after entities/*.lua ran
// lua_ui.c: fonts and textured rectangles for surface.*; opened by the host after its surface table
void            LuaUI_Open(lua_State *L);
int             LuaUI_DrawText(float x, float y, const char *s, int r, int g, int b, int a);   // -1 = no font set
qboolean        LuaUI_TextSize(const char *s, int *w, int *h);                                  // false = no font set
void            LuaUI_TexturedRect(unsigned int tex, float x, float y, float w, float h, float u0, float v0, float u1,
	float v1, int r, int g, int b, int a);

// lua_fx.c: sound and effects; opened by LuaG_Open
void            LuaFX_Open(lua_State *L);
// The game's own sounds pass here first: hook EntityEmitSound may change or block them.
// False = blocked. sample/volume/attn/pitch/channel may be changed (sample points at static storage).
qboolean        LuaFX_GameSound(struct edict_s *e, int *channel, const char **sample, float *volume, float *attn,
	int flags, int *pitch, const float *pos);
