// newen - the local server: loads the game DLL, owns the edicts, spawns maps and runs frames
//
// Startup follows GoldSrc: GiveFnptrsToDll hands the DLL the engine functions and the globals,
// GetEntityAPI2 returns the game's functions, GameInit runs once. For each map the engine parses
// the entity lump, calls the DLL's exported class function for every classname (that allocates the
// game's C++ object), passes each key with KeyValue, then calls Spawn. ServerActivate follows, two
// settling frames run, and the local player connects.
//
// Each frame the local player's input runs through the game's player movement (sv_pmove.c), then
// every other entity moves and thinks (sv_phys.c).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "server.h"

server_t          sv;
globalvars_t      gGlobals;
DLL_FUNCTIONS     gEntityInterface;
NEW_DLL_FUNCTIONS gNewDLLFunctions;
qboolean          sv_game_loaded;
void             *sv_game_dll;
static enginefuncs_t g_engfuncs;
static char       game_dll_path[MAX_OSPATH];

cvar_t *sv_gravity, *sv_stepsize, *sv_maxvelocity, *sv_friction, *sv_stopspeed, *sv_bounce;
static cvar_t *sv_maxedicts, *deathmatch, *coop, *teamplay;

// ---------------------------------------------------------------- strings

// string_t is an offset from pStringBase. The base is an empty string, so offset 0 means "".
// Strings the DLL makes from its own constants (MAKE_STRING) are offsets to its memory; that works
// because everything is 32-bit and offsets wrap.
static const char string_base[1] = "";
static char **strings;
static int strings_count, strings_cap;

const char *SV_String(string_t s)
{
	return string_base + s;
}

string_t SV_MakeString(const char *s)
{
	return (string_t)(s - string_base);
}

string_t SV_AllocString(const char *s)
{
	if (!s || !*s)
		return 0;
	// the game treats strings as immutable but sometimes writes "\n" escapes; GoldSrc converts them
	size_t n = strlen(s);
	char *copy = Mem_Alloc(n + 1);
	char *o = copy;
	for (const char *p = s; *p; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			*o++ = '\n';
			p++;
		} else {
			*o++ = *p;
		}
	}
	*o = 0;
	if (strings_count == strings_cap) {
		strings_cap = strings_cap ? strings_cap * 2 : 1024;
		strings = Mem_Realloc(strings, sizeof(char *) * strings_cap);
	}
	strings[strings_count++] = copy;
	return SV_MakeString(copy);
}

static void FreeStrings(void)
{
	for (int i = 0; i < strings_count; i++)
		Mem_Free(strings[i]);
	strings_count = 0;
}

// ---------------------------------------------------------------- edicts

static void FreePrivateData(edict_t *e)
{
	if (!e->pvPrivateData)
		return;
	if (gNewDLLFunctions.pfnOnFreeEntPrivateData)
		gNewDLLFunctions.pfnOnFreeEntPrivateData(e);
	Mem_Free(e->pvPrivateData);
	e->pvPrivateData = NULL;
}

void ED_ClearEdict(edict_t *e)
{
	FreePrivateData(e);
	memset(&e->v, 0, sizeof(e->v));
	e->free = false;
	e->v.pContainingEntity = e;
}

edict_t *ED_Alloc(void)
{
	for (int i = sv.maxclients + 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		// don't reuse a slot for half a second, so the client doesn't lerp between two entities
		if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5)) {
			ED_ClearEdict(e);
			return e;
		}
	}
	if (sv.num_edicts >= sv.max_edicts)
		Sys_Error("Out of edicts (%d); raise sv_maxedicts", sv.max_edicts);
	edict_t *e = EDICT_NUM(sv.num_edicts++);
	ED_ClearEdict(e);
	return e;
}

void ED_Free(edict_t *e)
{
	if (e->free)
		return;
	LuaG_EntityRemoved(e);
	SV_UnlinkEdict(e);
	FreePrivateData(e);
	e->serialnumber++;
	e->freetime = (float)sv.time;
	e->free = true;
	memset(&e->v, 0, sizeof(e->v));
	e->v.pContainingEntity = e;
	e->v.nextthink = -1;
}

edict_t *SV_LocalPlayer(void)
{
	if (!sv.active || !sv.client_spawned)
		return NULL;
	edict_t *e = EDICT_NUM(1);
	return e->free ? NULL : e;
}

// ---------------------------------------------------------------- precache

static int FindOrAdd(char ***list, int *count, const char *name, qboolean *added)
{
	*added = false;
	for (int i = 1; i < *count; i++)
		if (!stricmp((*list)[i], name))
			return i;
	if (*count == 0) {
		*list = Mem_Alloc(sizeof(char *) * 64);
		(*list)[0] = Mem_Strdup("");
		*count = 1;
	} else if ((*count & (*count - 1)) == 0 && *count >= 64) {
		*list = Mem_Realloc(*list, sizeof(char *) * *count * 2);
	}
	(*list)[*count] = Mem_Strdup(name);
	*added = true;
	return (*count)++;
}

int SV_ModelIndex(const char *name)
{
	if (!name || !*name)
		return 0;
	for (int i = 1; i < sv.num_models; i++)
		if (!stricmp(sv.model_precache[i], name))
			return i;
	return 0;
}

int SV_PrecacheModel(const char *name)
{
	if (!name || !*name)
		return 0;
	qboolean added;
	int idx = FindOrAdd(&sv.model_precache, &sv.num_models, name, &added);
	if (added) {
		if (!sv.loading)
			Con_DPrintf("Late precache of %s\n", name);
		sv.models = Mem_Realloc(sv.models, sizeof(model_t *) * (sv.num_models + 1));
		sv.models[0] = NULL;
		sv.models[idx] = Mod_ForName(name, false);
	}
	return idx;
}

int SV_PrecacheSound(const char *name)
{
	if (!name || !*name)
		return 0;
	qboolean added;
	return FindOrAdd(&sv.sound_precache, &sv.num_sounds, name, &added);
}

int SV_PrecacheGeneric(const char *name)
{
	if (!name || !*name)
		return 0;
	qboolean added;
	return FindOrAdd(&sv.generic_precache, &sv.num_generic, name, &added);
}

unsigned short SV_PrecacheEvent(int type, const char *name)
{
	if (!name || !*name)
		return 0;
	qboolean added;
	return (unsigned short)FindOrAdd(&sv.event_precache, &sv.num_events, name, &added);
}

model_t *SV_ModelForIndex(int index)
{
	if (index <= 0 || index >= sv.num_models)
		return NULL;
	return sv.models[index];
}

static void FreeList(char ***list, int *count)
{
	for (int i = 0; i < *count; i++)
		Mem_Free((*list)[i]);
	Mem_Free(*list);
	*list = NULL;
	*count = 0;
}

void SV_SetMinMaxSize(edict_t *e, const vec3_t mins, const vec3_t maxs)
{
	for (int i = 0; i < 3; i++)
		if (mins[i] > maxs[i])
			Con_DPrintf("%s: backwards mins/maxs\n", SV_String(e->v.classname));
	VectorCopy(mins, e->v.mins);
	VectorCopy(maxs, e->v.maxs);
	VectorSubtract(maxs, mins, e->v.size);
	SV_LinkEdict(e, false);
}

// ---------------------------------------------------------------- game DLL

void *SV_GetProcAddress(const char *name)
{
	return sv_game_dll ? (void *)GetProcAddress((HMODULE)sv_game_dll, name) : NULL;
}

ENTITYINIT SV_GetEntityInit(const char *classname)
{
	return (ENTITYINIT)SV_GetProcAddress(classname);
}

// Save/restore stores think and touch functions by their exported (decorated) names
const char *SV_NameForFunction(unsigned function)
{
	if (!sv_game_dll)
		return NULL;
	unsigned char *base = (unsigned char *)sv_game_dll;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (!dir->VirtualAddress)
		return NULL;
	IMAGE_EXPORT_DIRECTORY *exp = (IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
	DWORD *names = (DWORD *)(base + exp->AddressOfNames);
	WORD *ordinals = (WORD *)(base + exp->AddressOfNameOrdinals);
	DWORD *funcs = (DWORD *)(base + exp->AddressOfFunctions);
	for (DWORD i = 0; i < exp->NumberOfNames; i++)
		if ((unsigned)(uintptr_t)(base + funcs[ordinals[i]]) == function)
			return (const char *)(base + names[i]);
	return NULL;
}

static char *LiblistValue(const char *key)
{
	static char value[MAX_OSPATH];
	char *data = FS_LoadFile("liblist.gam", NULL);
	if (!data)
		return NULL;
	const char *p = data;
	qboolean found = false;
	while ((p = COM_Parse(p)) != NULL) {
		char k[64];
		Q_strncpy(k, com_token, sizeof(k));
		p = COM_Parse(p);
		if (!p)
			break;
		if (!stricmp(k, key)) {
			Q_strncpy(value, com_token, sizeof(value));
			found = true;
			break;
		}
	}
	Mem_Free(data);
	return found ? value : NULL;
}

static qboolean LoadGameDLL(void)
{
	const char *rel = LiblistValue("gamedll");
	if (!rel)
		rel = "dlls/hl.dll";
	char fixed[MAX_OSPATH];
	Q_strncpy(fixed, rel, sizeof(fixed));
	for (char *s = fixed; *s; s++)
		if (*s == '\\')
			*s = '/';
	if (!FS_FullPath(fixed, game_dll_path, sizeof(game_dll_path))) {
		Con_Warnf("Game DLL %s not found; maps load without game logic\n", fixed);
		return false;
	}
	HMODULE dll = LoadLibraryA(game_dll_path);
	if (!dll) {
		Con_Errorf("Couldn't load %s (error %lu)\n", game_dll_path, GetLastError());
		return false;
	}
	sv_game_dll = dll;

	GIVEFNPTRSTODLL give = (GIVEFNPTRSTODLL)GetProcAddress(dll, "GiveFnptrsToDll");
	if (!give)
		give = (GIVEFNPTRSTODLL)GetProcAddress(dll, "_GiveFnptrsToDll@8");
	if (!give) {
		Con_Errorf("%s isn't a GoldSrc game DLL (no GiveFnptrsToDll)\n", game_dll_path);
		FreeLibrary(dll);
		sv_game_dll = NULL;
		return false;
	}
	SV_InitEngineFuncs(&g_engfuncs);
	gGlobals.pStringBase = string_base;
	give(&g_engfuncs, &gGlobals);

	memset(&gEntityInterface, 0, sizeof(gEntityInterface));
	int version = INTERFACE_VERSION;
	APIFUNCTION2 api2 = (APIFUNCTION2)GetProcAddress(dll, "GetEntityAPI2");
	APIFUNCTION api = (APIFUNCTION)GetProcAddress(dll, "GetEntityAPI");
	qboolean ok = false;
	if (api2)
		ok = api2(&gEntityInterface, &version);
	if (!ok && api)
		ok = api(&gEntityInterface, INTERFACE_VERSION);
	if (!ok) {
		Con_Errorf("%s: GetEntityAPI failed (interface version %d)\n", game_dll_path, version);
		FreeLibrary(dll);
		sv_game_dll = NULL;
		return false;
	}
	memset(&gNewDLLFunctions, 0, sizeof(gNewDLLFunctions));
	NEW_DLL_FUNCTIONS_FN newapi = (NEW_DLL_FUNCTIONS_FN)GetProcAddress(dll, "GetNewDLLFunctions");
	if (newapi) {
		int nv = NEW_DLL_FUNCTIONS_VERSION;
		if (!newapi(&gNewDLLFunctions, &nv))
			memset(&gNewDLLFunctions, 0, sizeof(gNewDLLFunctions));
	}

	gEntityInterface.pfnGameInit();
	SV_ExecuteServerCommands();   // "exec skill.cfg" and friends, before any map reads them
	sv_game_loaded = true;
	const char *desc = gEntityInterface.pfnGetGameDescription ? gEntityInterface.pfnGetGameDescription() : "";
	Con_Printf("Game DLL: %s (%s)\n", fixed, desc ? desc : "");
	return true;
}

qboolean SV_GameLoaded(void) { return sv_game_loaded; }
qboolean SV_Active(void)     { return sv.active; }

// ---------------------------------------------------------------- spawning

static void ParseEntity(const bspentity_t *be, qboolean isworld)
{
	const char *classname = BSP_ValueForKey(be, "classname");
	if (!*classname)
		return;
	if (gGlobals.deathmatch && (atoi(BSP_ValueForKey(be, "spawnflags")) & 2048))
		return;   // "not in deathmatch"

	edict_t *e = isworld ? sv.edicts : ED_Alloc();
	ENTITYINIT init = SV_GetEntityInit(classname);
	if (!init) {
		Con_DPrintf("No spawn function for %s\n", classname);
		if (!isworld)
			ED_Free(e);
		return;
	}
	e->v.classname = SV_AllocString(classname);
	init(&e->v);

	char key[256], value[2048], cls[256];
	Q_strncpy(cls, classname, sizeof(cls));
	for (int i = 0; i < be->numpairs; i++) {
		Q_strncpy(key, be->keys[i], sizeof(key));
		Q_strncpy(value, be->values[i], sizeof(value));
		// "angle" is a yaw; -1 and -2 mean straight up and down
		if (!strcmp(key, "angle")) {
			float y = (float)atof(value);
			if (y >= 0)
				snprintf(value, sizeof(value), "%f %f %f", e->v.angles[0], y, e->v.angles[2]);
			else if ((int)y == -1)
				Q_strncpy(value, "-90 0 0", sizeof(value));
			else
				Q_strncpy(value, "90 0 0", sizeof(value));
			Q_strncpy(key, "angles", sizeof(key));
		}
		const char *replaced = LuaG_EntityKeyValue(cls, key, value);
		if (replaced)
			Q_strncpy(value, replaced, sizeof(value));
		KeyValueData kvd = { cls, key, value, 0 };
		gEntityInterface.pfnKeyValue(e, &kvd);
	}

	if (gEntityInterface.pfnSpawn(e) < 0 || (e->v.flags & FL_KILLME)) {
		if (!isworld)
			ED_Free(e);
	}
}

static void ConnectLocalClient(void)
{
	edict_t *pl = EDICT_NUM(1);
	ED_ClearEdict(pl);
	pl->v.netname = SV_AllocString("Player");
	pl->v.colormap = 1;
	Q_strncpy(sv.clientinfo[0], "\\name\\Player\\model\\gordon\\topcolor\\30\\bottomcolor\\6", sizeof(sv.clientinfo[0]));
	char reject[128] = "";
	if (!gEntityInterface.pfnClientConnect(pl, "Player", "loopback", reject)) {
		Con_Warnf("The game refused the local player: %s\n", reject);
		ED_Free(pl);
		return;
	}
	gEntityInterface.pfnClientUserInfoChanged(pl, sv.clientinfo[0]);
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnClientPutInServer(pl);
	sv.client_spawned = true;
}

void SV_Deactivate(void)
{
	if (!sv.edicts)
		return;
	if (sv.active && sv_game_loaded)
		gEntityInterface.pfnServerDeactivate();
	for (int i = 0; i < sv.num_edicts; i++)
		FreePrivateData(EDICT_NUM(i));
	Mem_Free(sv.edicts);
	sv.edicts = NULL;
	FreeList(&sv.model_precache, &sv.num_models);
	Mem_Free(sv.models);
	sv.models = NULL;
	FreeList(&sv.sound_precache, &sv.num_sounds);
	FreeList(&sv.generic_precache, &sv.num_generic);
	FreeList(&sv.event_precache, &sv.num_events);
	FreeStrings();
	sv.active = false;
	sv.client_spawned = false;
}

qboolean SV_SpawnServer(const char *mapname)
{
	SV_Deactivate();
	if (!sv_game_loaded || !world.loaded)
		return false;
	double start = Sys_FloatTime();

	sv.time = 1.0;
	sv.maxclients = 1;
	sv.restarting = false;
	Q_strncpy(sv.mapname, mapname, sizeof(sv.mapname));
	Q_strncpy(sv.modelname, world.name, sizeof(sv.modelname));
	sv.max_edicts = (int)sv_maxedicts->value;
	if (sv.max_edicts < 64)
		sv.max_edicts = 64;
	sv.edicts = Mem_Alloc(sizeof(edict_t) * sv.max_edicts);
	for (int i = 0; i < sv.max_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		e->v.pContainingEntity = e;
		e->free = i > 0;
	}
	sv.num_edicts = sv.maxclients + 1;
	LuaG_ServerStarted();

	memset(&gGlobals, 0, sizeof(gGlobals));
	gGlobals.pStringBase = string_base;
	gGlobals.time = (float)sv.time;
	gGlobals.maxClients = sv.maxclients;
	gGlobals.maxEntities = sv.max_edicts;
	gGlobals.mapname = SV_AllocString(mapname);
	gGlobals.deathmatch = deathmatch->value;
	gGlobals.coop = coop->value;
	gGlobals.teamplay = teamplay->value;

	// model 1 is the world, 2.. its brush models, like GoldSrc
	sv.loading = true;
	SV_PrecacheModel(sv.modelname);
	for (int i = 1; i < world.numsubmodels; i++)
		SV_PrecacheModel(va("*%d", i));

	edict_t *ent = sv.edicts;
	ED_ClearEdict(ent);
	ent->v.model = SV_AllocString(sv.modelname);
	ent->v.modelindex = 1;
	ent->v.solid = SOLID_BSP;
	ent->v.movetype = MOVETYPE_PUSH;

	gEntityInterface.pfnResetGlobalState();
	SV_ExecuteServerCommands();
	SV_InitWorld();
	for (int i = 0; i < world.numentities; i++)
		ParseEntity(&world.entities[i], i == 0);

	gEntityInterface.pfnServerActivate(sv.edicts, sv.num_edicts, sv.maxclients);
	sv.active = true;
	// two frames so everything settles (doors close, monsters drop to the floor)
	for (int i = 0; i < 2; i++) {
		sv.frametime = 0.1f;
		gGlobals.frametime = 0.1f;
		SV_Physics();
	}
	ConnectLocalClient();
	sv.loading = false;
	LuaG_CheckNewEntities();
	if (SV_LocalPlayer())
		LuaG_PlayerSpawned(SV_LocalPlayer());
	LuaG_InitPostEntity();

	int used = 0;
	for (int i = 0; i < sv.num_edicts; i++)
		used += !EDICT_NUM(i)->free;
	Con_Printf("Server: %s, %d entities, %d models, %d sounds precached (%.0f ms)\n", mapname, used,
		sv.num_models - 1, sv.num_sounds > 0 ? sv.num_sounds - 1 : 0, (Sys_FloatTime() - start) * 1000);
	return true;
}

// ---------------------------------------------------------------- frames

// The game set the view (spawning, teleporting, cameras): GoldSrc sends the client its angles
static void CheckFixAngle(edict_t *pl)
{
	if (!pl->v.fixangle)
		return;
	VectorCopy(pl->v.angles, cl_viewangles);
	VectorCopy(pl->v.angles, pl->v.v_angle);
	pl->v.fixangle = 0;
}

void SV_Frame(double frametime)
{
	if (!sv.active)
		return;
	float ft = (float)frametime;
	if (ft > 0.1f)
		ft = 0.1f;
	sv.frametime = ft;
	gGlobals.frametime = ft;
	SV_ExecuteServerCommands();
	if (!sv.active)
		return;

	edict_t *pl = SV_LocalPlayer();
	if (pl) {
		CheckFixAngle(pl);
		usercmd_t cmd;
		CL_CreateCmd(ft, &cmd);
		SV_RunPlayerCmd(pl, &cmd);
	}
	sv.frametime = ft;
	gGlobals.frametime = ft;
	SV_Physics();
	if ((pl = SV_LocalPlayer()) != NULL)
		CheckFixAngle(pl);

	LuaG_CheckNewEntities();
	static float last_health = 1;
	if (pl) {
		if (last_health > 0 && pl->v.health <= 0)
			LuaG_PlayerDied(pl);
		last_health = pl->v.health;
	}
	LuaG_Tick();
}

// ---------------------------------------------------------------- rendering bridge

void R_ClearBrushEntities(void);
void R_AddBrushEntity(int submodel, const vec3_t origin, const vec3_t angles, int rendermode, int renderamt, const int color[3]);
void R_ClearFrameEntities(void);
rentity_t *R_AddFrameEntity(void);
extern refdef_t r_refdef;
void R_AddBeamEntity(const edict_t *e, model_t *sprite);   // r_beam.c
void CL_EntityStudioEvents(int index, const rentity_t *r);  // cl_tent.c

void SV_SetupRenderEntities(void)
{
	R_ClearBrushEntities();
	R_ClearFrameEntities();
	if (!sv.active)
		return;
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.modelindex || (e->v.effects & EF_NODRAW))
			continue;
		if (i <= sv.maxclients)
			continue;   // first person: the local player isn't drawn
		model_t *m = SV_ModelForIndex(e->v.modelindex);
		if (!m)
			continue;
		// the game's beams (CBeam): a sprite drawn between two points or entities
		if (e->v.flags & FL_CUSTOMENTITY) {
			if (m->type == mod_sprite)
				R_AddBeamEntity(e, m);
			continue;
		}
		int color[3] = { (int)e->v.rendercolor[0], (int)e->v.rendercolor[1], (int)e->v.rendercolor[2] };
		if (m->type == mod_brush) {
			R_AddBrushEntity(m->submodel, e->v.origin, e->v.angles, e->v.rendermode, (int)e->v.renderamt, color);
			continue;
		}
		rentity_t *r = R_AddFrameEntity();
		r->model = m;
		VectorCopy(e->v.origin, r->origin);
		VectorCopy(e->v.angles, r->angles);
		r->sequence = e->v.sequence;
		r->frame = e->v.frame;
		r->body = e->v.body;
		r->skin = e->v.skin;
		memcpy(r->controller, e->v.controller, 4);
		memcpy(r->blending, e->v.blending, 2);
		r->rendermode = e->v.rendermode;
		r->renderamt = (int)e->v.renderamt;
		r->renderfx = e->v.renderfx;
		memcpy(r->rendercolor, color, sizeof(color));
		r->scale = e->v.scale;
		if (m->type == mod_studio) {
			// the game sets frame at animtime; the renderer runs it forward on its own clock
			r->framerate = e->v.framerate;
			r->animtime = r_refdef.time - (sv.time - e->v.animtime);
			CL_EntityStudioEvents(i, r);   // muzzle flashes and sparks from the model's animation
		} else {
			r->framerate = 0;   // the game animates sprites itself
			r->animtime = r_refdef.time;
		}
		// in GoldSrc, normal-mode entities don't use renderamt
		if (r->rendermode == kRenderNormal)
			r->renderamt = 255;
	}
}

// ---------------------------------------------------------------- commands

// unknown console commands go to the game (give, say, impulse...)
static qboolean ClientCommandFallback(void)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl)
		return false;
	if (LuaG_PlayerCommand(pl))
		return true;
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnClientCommand(pl);
	return true;
}

static void Edicts_f(void)
{
	if (!sv.active) {
		Con_Printf("No server running\n");
		return;
	}
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL;
	int n = 0;
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free)
			continue;
		const char *cls = SV_String(e->v.classname);
		if (filter && !Q_WildcardMatch(filter, cls) && strnicmp(cls, filter, strlen(filter)))
			continue;
		// brush entities sit at 0 0 0 with their geometry elsewhere: show where they really are
		vec3_t c;
		for (int j = 0; j < 3; j++)
			c[j] = (e->v.absmin[j] + e->v.absmax[j]) * 0.5f;
		Con_Printf("%4d %-24s %-20s at %6.0f %6.0f %6.0f  center %6.0f %6.0f %6.0f  move %d solid %d\n", i, cls,
			SV_String(e->v.model), e->v.origin[0], e->v.origin[1], e->v.origin[2], c[0], c[1], c[2], e->v.movetype, e->v.solid);
		n++;
	}
	Con_Printf("%d of %d edicts in use (max %d)\n", n, sv.num_edicts, sv.max_edicts);
}

// tracetest [hull]: what a box of that hull size hits straight down from the camera
static void TraceTest_f(void)
{
	if (!sv.active)
		return;
	int h = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 1;
	if (h < 0 || h >= MAX_HULLS)
		h = 1;
	vec3_t start, end;
	VectorCopy(r_refdef.origin, start);
	VectorCopy(start, end);
	end[2] -= 512;
	trace_t t = SV_Move(start, hull_mins[h], hull_maxs[h], end, MOVE_NORMAL, SV_LocalPlayer());
	Con_Printf("contents %d; hull %d down: allsolid %d startsolid %d fraction %.3f end %.1f %.1f %.1f hit %s\n",
		SV_PointContents(start), h, t.allsolid, t.startsolid, t.fraction, t.endpos[0], t.endpos[1], t.endpos[2],
		t.ent ? SV_String(t.ent->v.classname) : "nothing");
}

static void ToggleFlag(int flag, const char *name)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl) {
		Con_Printf("No player\n");
		return;
	}
	pl->v.flags ^= flag;
	Con_Printf("%s %s\n", name, (pl->v.flags & flag) ? "ON" : "OFF");
}

static void God_f(void)      { ToggleFlag(FL_GODMODE, "godmode"); }
static void Notarget_f(void) { ToggleFlag(FL_NOTARGET, "notarget"); }

// the game's movement code flies through walls when the player's movetype is noclip
static void Noclip_f(void)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl) {
		Con_Printf("No player\n");
		return;
	}
	if (pl->v.movetype != MOVETYPE_NOCLIP) {
		pl->v.movetype = MOVETYPE_NOCLIP;
		Con_Printf("noclip ON\n");
	} else {
		pl->v.movetype = MOVETYPE_WALK;
		Con_Printf("noclip OFF\n");
	}
}

static void Kill_f(void)
{
	edict_t *pl = SV_LocalPlayer();
	if (pl && pl->v.health > 0) {
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnClientKill(pl);
	}
}

// single player restarts the map when the player dies; deferred so the map isn't freed mid-frame
static void Reload_f(void)
{
	if (sv.active && !sv.restarting) {
		sv.restarting = true;
		Cbuf_InsertText(va("map %s\n", sv.mapname));
	}
}

// moves the player (or, without one, the free camera)
qboolean SV_SetPlayerPos(const vec3_t eye, const vec3_t angles)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl)
		return false;
	VectorSubtract(eye, pl->v.view_ofs, pl->v.origin);
	VectorClear(pl->v.velocity);
	SV_LinkEdict(pl, false);
	if (angles)
		VectorCopy(angles, cl_viewangles);
	return true;
}

static void Status_f(void)
{
	Con_Printf("game DLL: %s\n", sv_game_loaded ? game_dll_path : "none");
	if (sv.active)
		Con_Printf("map: %s, time %.1f, %d edicts\n", sv.mapname, sv.time, sv.num_edicts);
	edict_t *pl = SV_LocalPlayer();
	if (pl)
		Con_Printf("player: health %.0f, armor %.0f, weapons 0x%x, deadflag %d, button %d, at %.0f %.0f %.0f\n", pl->v.health,
			pl->v.armorvalue, pl->v.weapons, pl->v.deadflag, pl->v.button, pl->v.origin[0], pl->v.origin[1], pl->v.origin[2]);
}

void SV_Init(void)
{
	sv_gravity = Cvar_Get("sv_gravity", "800", FCVAR_SERVER, "world gravity");
	sv_stepsize = Cvar_Get("sv_stepsize", "18", FCVAR_SERVER, "highest step that can be walked up");
	sv_maxvelocity = Cvar_Get("sv_maxvelocity", "2000", 0, "speed limit for physics");
	sv_friction = Cvar_Get("sv_friction", "4", FCVAR_SERVER, "ground friction");
	sv_stopspeed = Cvar_Get("sv_stopspeed", "100", FCVAR_SERVER, "speed below which friction stops you");
	sv_bounce = Cvar_Get("sv_bounce", "1", FCVAR_SERVER, "bounce factor for MOVETYPE_BOUNCE");
	sv_maxedicts = Cvar_Get("sv_maxedicts", "2048", FCVAR_ARCHIVE, "edicts per map (GoldSrc: 900); applies on map load");
	deathmatch = Cvar_Get("deathmatch", "0", FCVAR_SERVER, "deathmatch game");
	coop = Cvar_Get("coop", "0", FCVAR_SERVER, "cooperative game");
	teamplay = Cvar_Get("teamplay", "0", FCVAR_SERVER, "team deathmatch");
	// engine cvars game DLLs look up by pointer; a missing one would crash them
	static const char *engine_cvars[][2] = {
		{ "skill", "1" }, { "sv_maxspeed", "320" }, { "sv_accelerate", "10" }, { "sv_airaccelerate", "10" },
		{ "sv_wateraccelerate", "10" }, { "sv_waterfriction", "1" }, { "edgefriction", "2" },
		{ "sv_cheats", "0" }, { "sv_footsteps", "1" }, { "sv_rollangle", "2" }, { "sv_rollspeed", "200" },
		{ "sv_aim", "1" }, { "sv_skyname", "desert" }, { "sv_skycolor_r", "0" }, { "sv_skycolor_g", "0" },
		{ "sv_skycolor_b", "0" }, { "sv_skyvec_x", "0" }, { "sv_skyvec_y", "0" }, { "sv_skyvec_z", "0" },
		{ "sv_wateramp", "0" }, { "sv_zmax", "4096" }, { "sv_clienttrace", "1" }, { "violence_hblood", "1" },
		{ "violence_ablood", "1" }, { "violence_hgibs", "1" }, { "violence_agibs", "1" }, { "hostname", "newen" },
		{ "sv_password", "" }, { "maxplayers", "1" }, { "sv_language", "0" }, { "sv_allowupload", "1" },
		{ "sv_allowdownload", "1" }, { "sv_lan", "1" }, { "pausable", "1" }, { "mp_logecho", "0" },
		{ "sv_contact", "" }, { "sv_region", "-1" }, { "sv_visiblemaxplayers", "-1" }, { "sv_spectatormaxspeed", "500" },
	};
	for (size_t i = 0; i < sizeof(engine_cvars) / sizeof(engine_cvars[0]); i++)
		Cvar_Get(engine_cvars[i][0], engine_cvars[i][1], FCVAR_SERVER, NULL);
	Cmd_AddCommand("edicts", Edicts_f, "lists the game's entities, optionally filtered by classname");
	Cmd_AddCommand("tracetest", TraceTest_f, "tracetest [hull] - traces a box straight down from the camera");
	Cmd_AddCommand("god", God_f, "toggles invulnerability for the player");
	Cmd_AddCommand("notarget", Notarget_f, "toggles whether monsters notice the player");
	Cmd_AddCommand("status", Status_f, "shows the game DLL and the running map");
	Cmd_AddCommand("noclip", Noclip_f, "toggles flying through walls");
	Cmd_AddCommand("kill", Kill_f, "kills the player");
	Cmd_AddCommand("reload", Reload_f, "restarts the current map");

	if (COM_CheckParm("-nogamedll")) {
		Con_Printf("Game DLL disabled (-nogamedll)\n");
		return;
	}
	if (LoadGameDLL()) {
		cmd_fallback = ClientCommandFallback;
		SV_InitPlayerMove();
	}
}

void SV_Shutdown(void)
{
	SV_Deactivate();
	if (sv_game_loaded && gNewDLLFunctions.pfnGameShutdown)
		gNewDLLFunctions.pfnGameShutdown();
	// the DLL stays loaded until the process exits: it may have registered cvars and commands
	// that point into it
}
