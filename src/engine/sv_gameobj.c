// newen - checked calls into the game DLL's C++ objects
#include "cl_local.h"   // the shared, nestable Linux crash guard

#ifdef _WIN32
#define OBJ_CALL __fastcall
#define OBJ_THIS void *self, void *edx_
#define OBJ_ARGS(p) (p), NULL
#else
#define OBJ_CALL
#define OBJ_THIS void *self
#define OBJ_ARGS(p) (p)
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#define OBJ_GUARDED(call, failure) \
	do { \
		__try { call; } \
		__except ((Sys_LogException(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)) { failure; } \
	} while (0)
#elif !defined(_WIN32)
#define OBJ_GUARDED(call, failure) SYS_GUARDED(call, failure)
#else
// The MinGW build has the same limitation as CLIENT_CALL: no 32-bit structured exception guard.
#define OBJ_GUARDED(call, failure) do { call; } while (0)
#endif

typedef int (OBJ_CALL *obj_int_t)(OBJ_THIS);

// CBaseEntity has no virtual destructor. These slots are the same in Valve's MSVC and GCC builds.
// Order checked against ValveSoftware/halflife's public dlls/cbase.h; no SDK code is used here.
static const struct {
	int classify, is_player;
} object_slots = { 8, 40 };

static cvar_t *gameobj_testbadslot;
static qboolean object_ready, session_failed, probe_ran;
static int player_answer, world_answer, scientist_answer;
static const char *object_reason = "no map probe yet";

static void ObjectFailed(const char *reason)
{
	object_ready = false;
	session_failed = true;
	object_reason = reason;
	Con_Warnf("Game objects: %s; method calls disabled for this session\n", reason);
}

static void ObjectQuarantine(edict_t *e)
{
	ObjectFailed("scientist cleanup failed");
	// A broken DLL cleanup must not run again at map unload. Leak this one allocation rather than retry it.
	e->pvPrivateData = NULL;
	ED_Free(e);
}

static int ObjectIntRaw(edict_t *e, int slot)
{
	obj_int_t *methods = *(obj_int_t **)e->pvPrivateData;
	if (!methods || !methods[slot])
		return -1;
	return methods[slot](OBJ_ARGS(e->pvPrivateData));
}

static qboolean ObjectInt(edict_t *e, int slot, int *answer)
{
	volatile qboolean ok = true;
	if (!e || e->free || !e->pvPrivateData)
		return false;
	OBJ_GUARDED(*answer = ObjectIntRaw(e, slot), ok = false);
	return ok;
}

void SV_GameObjClear(void)
{
	object_ready = false;
	probe_ran = false;
	object_reason = session_failed ? "disabled by an earlier probe" : "no map probe yet";
}

// A map need not contain a scientist. Construct one without spawning it, so it never thinks or draws.
static edict_t *ProbeScientist(qboolean *temporary)
{
	for (int i = sv.maxclients + 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (!e->free && e->pvPrivateData && !strcmp(SV_String(e->v.classname), "monster_scientist"))
			return e;
	}
	qboolean room = sv.num_edicts < sv.max_edicts;
	for (int i = sv.maxclients + 1; !room && i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		room = e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5);
	}
	if (!room)
		return NULL;
	ENTITYINIT init = SV_GetEntityInit("monster_scientist");
	if (!init)
		return NULL;
	edict_t *e = ED_Alloc();
	e->v.classname = SV_AllocString("monster_scientist");
	*temporary = true;
	OBJ_GUARDED(init(&e->v), ObjectFailed("scientist construction failed"));
	return e;
}

void SV_GameObjProbe(void)
{
	object_ready = false;
	probe_ran = true;
	player_answer = world_answer = scientist_answer = -1;
	if (session_failed) {
		object_reason = "disabled by an earlier probe";
		return;
	}
	edict_t *player = SV_LocalPlayer();
	// A deliberately wrong slot with the same signature tests refusal without invoking an unsafe method.
	int is_player = gameobj_testbadslot->value ? object_slots.classify : object_slots.is_player;
	if (!ObjectInt(player, is_player, &player_answer) || player_answer != 1 ||
		!ObjectInt(EDICT_NUM(0), is_player, &world_answer) || world_answer != 0) {
		ObjectFailed("IsPlayer probe failed");
		return;
	}
	qboolean temporary = false;
	edict_t *scientist = ProbeScientist(&temporary);
	qboolean ok = !session_failed && ObjectInt(scientist, object_slots.classify, &scientist_answer);
	if (temporary)
		OBJ_GUARDED(ED_Free(scientist), ObjectQuarantine(scientist));
	if (session_failed)
		return;
	if (!ok || scientist_answer != 3) {
		ObjectFailed("scientist Classify probe failed");
		return;
	}
	object_ready = true;
	object_reason = "probe passed";
	Con_Printf("Game objects: probe passed (player 1, world 0, scientist 3)\n");
}

static void GameObjInfo_f(void)
{
	Con_Printf("Game objects: %s (%s)\n", object_ready ? "ready" : "off", object_reason);
	Con_Printf("  slots: Classify %d, IsPlayer %d; %s ABI\n", object_slots.classify, object_slots.is_player,
#ifdef _WIN32
		"Windows thiscall");
#ifdef _MSC_VER
	Con_Printf("  calls guarded with structured exceptions\n");
#else
	Con_Printf("  calls unguarded (32-bit MinGW has no structured exception handling)\n");
#endif
#else
		"Linux cdecl");
	Con_Printf("  calls guarded with the engine's signal handler\n");
#endif
	if (probe_ran)
		Con_Printf("  answers: player %d, world %d, scientist %d\n", player_answer, world_answer, scientist_answer);
	if (gameobj_testbadslot->value)
		Con_Printf("  deliberate wrong-slot test enabled (applies at map start)\n");
}

void SV_InitGameObj(void)
{
	gameobj_testbadslot = Cvar_Get("gameobj_testbadslot", "0", 0,
		"tests refusal of an incorrect method slot at map start; restart to re-enable calls after a failure");
	Cmd_AddCommand("gameobj_info", GameObjInfo_f, "shows the game object's method probe and crash guard");
}
