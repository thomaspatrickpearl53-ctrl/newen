// newen - the local server: game DLL, edicts, collision and physics
#pragma once
#include "game_api.h"
#include "bsp.h"
#include "model.h"

// ---------------------------------------------------------------- collision

typedef struct {
	mclipnode_t *clipnodes;
	mplane_t    *planes;
	int          firstclipnode, lastclipnode;
	vec3_t       clip_mins, clip_maxs;
} hull_t;

typedef struct {
	qboolean allsolid, startsolid, inopen, inwater;
	float    fraction;
	vec3_t   endpos;
	mplane_t plane;
	edict_t *ent;
	int      hitgroup;
} trace_t;

#define MAX_HULLS 4
extern vec3_t hull_mins[MAX_HULLS], hull_maxs[MAX_HULLS];   // point, player, big monster, crouch

void     SV_InitWorld(void);                     // hull sizes and the box hull; per map
void     SV_LinkEdict(edict_t *e, qboolean touch_triggers);
void     SV_UnlinkEdict(edict_t *e);
int      SV_PointContents(const vec3_t p);
int      SV_TruePointContents(const vec3_t p);  // keeps the current directions
trace_t  SV_Move(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int type, edict_t *passedict);
trace_t  SV_ClipMoveToEntity(edict_t *ent, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end);
edict_t *SV_TestEntityPosition(edict_t *e);     // what e is stuck in, or NULL
hull_t  *SV_HullForBsp(edict_t *ent, const vec3_t mins, const vec3_t maxs, vec3_t offset);
int      SV_HullPointContents(const hull_t *hull, int num, const vec3_t p);
hull_t  *SV_HullForBox(const vec3_t mins, const vec3_t maxs);   // shared storage, valid until the next call
void     SV_ModelHull(const msubmodel_t *sm, int h, hull_t *out);
qboolean SV_RecursiveHullCheck(const hull_t *hull, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2,
	trace_t *trace, int depth);

#define CONTENTS_CURRENT_0    -9
#define CONTENTS_CURRENT_DOWN -14
#define CONTENTS_TRANSLUCENT  -15
#define CONTENTS_LADDER       -16

// ---------------------------------------------------------------- server state

typedef struct {
	qboolean   active;
	double     time;
	float      frametime;
	char       mapname[64];
	char       modelname[64];     // "maps/c1a0.bsp"
	edict_t   *edicts;
	int        num_edicts, max_edicts;
	int        maxclients;
	char     **model_precache;    model_t **models;   int num_models;
	char     **sound_precache;    int num_sounds;
	char     **generic_precache;  int num_generic;
	char     **event_precache;    int num_events;
	qboolean   loading;           // precaching is allowed
	char       clientinfo[1][512];   // userinfo per client ("\name\Player\model\gordon")
	char       serverinfo[512];
	char       localinfo[512];
	qboolean   client_spawned;
	qboolean   restarting;        // a reload is queued; the game repeats the request every frame
	qboolean   changing;          // a level change is queued (the game asks every frame it's touched)
} server_t;

extern server_t          sv;
extern globalvars_t      gGlobals;
extern DLL_FUNCTIONS     gEntityInterface;
extern NEW_DLL_FUNCTIONS gNewDLLFunctions;
extern qboolean          sv_game_loaded;
extern void             *sv_game_dll;   // HMODULE
extern cvar_t           *sv_gravity, *sv_stepsize, *sv_maxvelocity, *sv_friction, *sv_stopspeed, *sv_bounce;

#define EDICT_NUM(n)     (&sv.edicts[n])
#define NUM_FOR_EDICT(e) ((int)((e) - sv.edicts))

static inline qboolean ED_Linked(const edict_t *e) { return e->area.prev != NULL; }

// sv_main.c
void        SV_Init(void);
void        SV_Shutdown(void);
// after BSP_Load. oldmap NULL: a new game. Otherwise a level change from oldmap: the map comes back
// as it was left if it was visited, and the old map's entities at the landmark (the player) follow.
qboolean    SV_SpawnServer(const char *mapname, const char *oldmap, const char *landmark, qboolean loadgame);
void        SV_Deactivate(void);
void        SV_Frame(double frametime);
const char *SV_String(string_t s);
string_t    SV_AllocString(const char *s);
string_t    SV_MakeString(const char *s);          // for strings that already live forever
edict_t    *ED_Alloc(void);
void        ED_Free(edict_t *e);
void        ED_ClearEdict(edict_t *e);
int         SV_ModelIndex(const char *name);       // 0 if not precached
int         SV_PrecacheModel(const char *name);
int         SV_PrecacheSound(const char *name);
int         SV_PrecacheGeneric(const char *name);
unsigned short SV_PrecacheEvent(int type, const char *name);
model_t    *SV_ModelForIndex(int index);
void        SV_SetMinMaxSize(edict_t *e, const vec3_t mins, const vec3_t maxs);
void        SV_SetupRenderEntities(void);          // fills the renderer's lists from the edicts
ENTITYINIT  SV_GetEntityInit(const char *classname);
void       *SV_GetProcAddress(const char *name);
const char *SV_NameForFunction(unsigned function);
edict_t    *SV_LocalPlayer(void);

// sv_save.c
void        SV_InitSave(void);
void        SV_ClearLevelStates(void);              // a new game forgets the levels
void        SV_SaveLevelState(void);                // the current level, before leaving it
qboolean    SV_HasLevelState(const char *map);
qboolean    SV_RestoreLevelState(const char *map);
qboolean    SV_RestoreTransition(const char *oldmap, const char *landmark);   // true if the player came
qboolean    SV_RestoreSavedLevel(const char *map);     // a loaded save's level, the player in it
void        SV_RestoreGlobalState(void);               // from a loaded save
void        SV_AutoSave(void);
const char *SV_LastSave(void);                          // NULL in a new game
// r_main.c: loads a map and starts the server on it (oldmap: a level change; loadgame: from a save)
void        R_LoadMap(const char *mapname, const char *oldmap, const char *landmark, qboolean loadgame);

// sv_engfuncs.c
void        SV_InitEngineFuncs(enginefuncs_t *ef);
void        SV_ExecuteServerCommands(void);   // what the game queued with ServerCommand
const char *SV_InfoValueForKey(const char *s, const char *key);
const char *SV_TraceTexture(edict_t *texent, const float *v1, const float *v2);
const char *SV_PhysInfo(void);
void        SV_SetModel(edict_t *e, const char *name);
edict_t    *SV_CreateNamedEntity(const char *classname);   // allocates and runs the class function; NULL if unknown

// lua_game.c: hooks the server fires into engine Lua
void        LuaG_ServerStarted(void);
void        LuaG_CheckNewEntities(void);
void        LuaG_EntityRemoved(edict_t *e);
void        LuaG_PlayerSpawned(edict_t *pl);
void        LuaG_PlayerDied(edict_t *pl);
void        LuaG_InitPostEntity(void);
void        LuaG_Tick(void);
qboolean    LuaG_PlayerCommand(edict_t *pl);
void        LuaG_StartCommand(edict_t *pl, usercmd_t *cmd);
const char *LuaG_EntityKeyValue(const char *classname, const char *key, const char *value);

// sv_pmove.c
void        SV_InitPlayerMove(void);                      // after the game DLL loads
playermove_t *SV_PlayerMove(void);                        // NULL before then
void        SV_RunPlayerCmd(edict_t *pl, usercmd_t *cmd);
qboolean    SV_SetPlayerPos(const vec3_t eye, const vec3_t angles);   // false if there's no player
// the client's traces (event API): push swaps in every solid entity, pop restores the movement set
void        SV_PM_PushClientState(void);
void        SV_PM_PopClientState(void);
void        SV_PM_SetSolidPlayers(int skip);
void        SV_PM_SetTraceHull(int hull);
void        SV_PM_ClientTrace(const float *start, const float *end, int flags, int ignore_pe, pmtrace_t *tr);
physent_t  *SV_PM_GetPhysent(int i);
void        SV_PM_HullBounds(int hull, float *mins, float *maxs);

// cl_input.c (temporary: client.dll owns movement input in GoldSrc)
void        CL_InitInput(void);
void        CL_CreateCmd(float frametime, usercmd_t *cmd);        // cl_main.c: the client DLL's, or the engine's
void        CL_CreateCmdEngine(float frametime, usercmd_t *cmd);
extern vec3_t cl_viewangles;
void        SV_MakeVectors(const vec3_t angles);
float       SV_VecToYaw(const vec3_t v);

// sv_phys.c
void        SV_Physics(void);
void        SV_RunThink(edict_t *ent);
void        SV_Impact(edict_t *e1, edict_t *e2);
qboolean    SV_movestep(edict_t *ent, const vec3_t move, qboolean relink, int mode);
qboolean    SV_CheckBottom(edict_t *ent);
void        SV_MoveToOrigin(edict_t *ent, const vec3_t goal, float dist, int movetype);
int         SV_DropToFloor(edict_t *e);
void        SV_CheckVelocity(edict_t *ent);
