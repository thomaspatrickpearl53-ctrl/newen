// newen - level states: what carries the player (and anything near the landmark) from one map to the
// next, and what a map looks like when you come back to it
//
// The game DLL saves and restores its entities itself (pfnSave/pfnRestore); the engine provides a
// SAVERESTOREDATA with a data buffer, a token table for field names and one ENTITYTABLE entry per
// edict. Entity references in the data are table positions, which are the edict numbers at save
// time, so a restore builds its table at the same positions.
//
// On a level change the current map is saved (and kept, so going back finds it as it was). The game
// fills in its connections to other maps and marks every entity that's part of each transition
// (pfnParmsChangeLevel). The next map then either spawns from its entity lump or, if it was visited,
// restores its saved state; after that the marked entities of the old map, the player among them,
// are restored into it, moved by the difference between the two landmarks.
//
// The states live in memory for now. Field-name tokens point into the game DLL, which stays loaded.
#include "server.h"

typedef struct {
	int  location, size, flags;
	char classname[64];
} savedent_t;

typedef struct {
	char          map[64];
	unsigned char *data;
	int           size;
	char        **tokens;
	int           tokencount;
	savedent_t   *ents;            // indexed by edict number at save time
	int           numents;
	LEVELLIST     levels[MAX_LEVEL_CONNECTIONS];
	int           connections;
	char          styles[64][128];
} levelstate_t;

static levelstate_t *states;
static int states_count;
static cvar_t *sv_savebuffer;

static void FreeState(levelstate_t *ls)
{
	Mem_Free(ls->data);
	Mem_Free(ls->tokens);
	Mem_Free(ls->ents);
	memset(ls, 0, sizeof(*ls));
}

void SV_ClearLevelStates(void)
{
	for (int i = 0; i < states_count; i++)
		FreeState(&states[i]);
	Mem_Free(states);
	states = NULL;
	states_count = 0;
}

static levelstate_t *FindState(const char *map)
{
	for (int i = 0; i < states_count; i++)
		if (!stricmp(states[i].map, map))
			return &states[i];
	return NULL;
}

qboolean SV_HasLevelState(const char *map)
{
	return FindState(map) != NULL;
}

static SAVERESTOREDATA *NewSaveData(int bufsize, int tablecount, int tokencount)
{
	SAVERESTOREDATA *sd = Mem_Alloc(sizeof(*sd));
	sd->pBaseData = sd->pCurrentData = Mem_Alloc((size_t)bufsize);
	sd->bufferSize = bufsize;
	sd->tokenCount = tokencount;
	sd->pTokens = Mem_Alloc(sizeof(char *) * tokencount);
	sd->tableCount = tablecount;
	sd->pTable = Mem_Alloc(sizeof(ENTITYTABLE) * (tablecount > 0 ? tablecount : 1));
	sd->time = (float)sv.time;
	return sd;
}

static void FreeSaveData(SAVERESTOREDATA *sd)
{
	Mem_Free(sd->pBaseData);
	Mem_Free(sd->pTokens);
	Mem_Free(sd->pTable);
	Mem_Free(sd);
}

// ---------------------------------------------------------------- saving

static SAVERESTOREDATA *SaveEntities(int bufsize)
{
	SAVERESTOREDATA *sd = NewSaveData(bufsize, sv.num_edicts, 4096);
	Q_strncpy(sd->szCurrentMapName, sv.mapname, sizeof(sd->szCurrentMapName));
	for (int i = 0; i < sv.num_edicts; i++) {
		ENTITYTABLE *t = &sd->pTable[i];
		t->id = i;
		t->pent = EDICT_NUM(i);
		if (i >= 1 && i <= sv.maxclients)
			t->flags = FENTTABLE_PLAYER;
	}
	gGlobals.pSaveData = sd;
	gGlobals.time = (float)sv.time;
	// the game lists the maps this one connects to and marks the entities in each transition
	gEntityInterface.pfnParmsChangeLevel();
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->pvPrivateData)
			continue;
		sd->currentIndex = i;
		gEntityInterface.pfnSave(e, sd);
	}
	gGlobals.pSaveData = NULL;
	return sd;
}

void SV_SaveLevelState(void)
{
	if (!sv.active || !sv_game_loaded)
		return;
	int bufsize = sv_savebuffer->value > 0 ? (int)sv_savebuffer->value * 1024 * 1024 : 8 * 1024 * 1024;
	SAVERESTOREDATA *sd;
	// the game stops writing when the buffer is full: then do it again with a bigger one
	for (;;) {
		sd = SaveEntities(bufsize);
		if (sd->size < sd->bufferSize || bufsize >= 256 * 1024 * 1024)
			break;
		FreeSaveData(sd);
		bufsize *= 2;
	}

	levelstate_t *ls = FindState(sv.mapname);
	if (ls) {
		FreeState(ls);
	} else {
		states = Mem_Realloc(states, sizeof(levelstate_t) * (states_count + 1));
		ls = &states[states_count++];
		memset(ls, 0, sizeof(*ls));
	}
	Q_strncpy(ls->map, sv.mapname, sizeof(ls->map));
	ls->size = sd->size;
	ls->data = Mem_Alloc((size_t)sd->size + 1);
	memcpy(ls->data, sd->pBaseData, (size_t)sd->size);
	ls->tokencount = sd->tokenCount;
	ls->tokens = Mem_Alloc(sizeof(char *) * sd->tokenCount);
	memcpy(ls->tokens, sd->pTokens, sizeof(char *) * sd->tokenCount);
	ls->numents = sd->tableCount;
	ls->ents = Mem_Alloc(sizeof(savedent_t) * (ls->numents > 0 ? ls->numents : 1));
	for (int i = 0; i < sd->tableCount; i++) {
		ENTITYTABLE *t = &sd->pTable[i];
		savedent_t *s = &ls->ents[i];
		s->location = t->location;
		s->size = t->size;
		s->flags = t->flags;
		if (t->size && t->classname)
			Q_strncpy(s->classname, SV_String(t->classname), sizeof(s->classname));
	}
	ls->connections = sd->connectionCount < MAX_LEVEL_CONNECTIONS ? sd->connectionCount : MAX_LEVEL_CONNECTIONS;
	memcpy(ls->levels, sd->levelList, sizeof(LEVELLIST) * ls->connections);
	for (int i = 0; i < 64; i++)
		Q_strncpy(ls->styles[i], R_LightStylePattern(i), sizeof(ls->styles[i]));
	Con_DPrintf("Saved %s: %d KB, %d connections\n", ls->map, ls->size / 1024, ls->connections);
	FreeSaveData(sd);
}

// ---------------------------------------------------------------- restoring

typedef enum { RESTORE_LEVEL, RESTORE_TRANSITION } restoremode_t;

// returns how many entities came back; *player is set if the player was one of them
static int RestoreEntities(levelstate_t *ls, restoremode_t mode, int mask, const vec3_t offset, const char *landmark,
	qboolean *player)
{
	SAVERESTOREDATA *sd = NewSaveData(ls->size + 1, ls->numents, ls->tokencount);
	memcpy(sd->pBaseData, ls->data, (size_t)ls->size);
	sd->size = ls->size;
	memcpy(sd->pTokens, ls->tokens, sizeof(char *) * ls->tokencount);
	Q_strncpy(sd->szCurrentMapName, ls->map, sizeof(sd->szCurrentMapName));   // the level they come from
	if (mode == RESTORE_TRANSITION && landmark && *landmark) {
		sd->fUseLandmark = 1;
		Q_strncpy(sd->szLandmarkName, landmark, sizeof(sd->szLandmarkName));
		VectorCopy(offset, sd->vecLandmarkOffset);
	}

	// first every entity exists, so references between them resolve
	for (int i = 0; i < ls->numents; i++) {
		savedent_t *s = &ls->ents[i];
		ENTITYTABLE *t = &sd->pTable[i];
		t->id = i;
		t->location = s->location;
		t->size = s->size;
		t->flags = s->flags;
		if (!s->size || !s->classname[0] || (s->flags & FENTTABLE_REMOVED))
			continue;
		qboolean isplayer = (s->flags & FENTTABLE_PLAYER) != 0;
		if (mode == RESTORE_LEVEL ? isplayer : !(isplayer || (s->flags & mask)))
			continue;
		ENTITYINIT init = SV_GetEntityInit(s->classname);
		if (!init) {
			Con_DPrintf("Restore: no spawn function for %s\n", s->classname);
			continue;
		}
		edict_t *e;
		if (isplayer) {
			e = EDICT_NUM(i);   // single player: the player keeps slot 1
			ED_ClearEdict(e);
		} else if (i == 0) {
			e = EDICT_NUM(0);   // the world, when a whole level comes back
		} else {
			e = ED_Alloc();
		}
		t->classname = SV_AllocString(s->classname);
		e->v.classname = t->classname;
		init(&e->v);
		t->pent = e;
	}

	// then the game reads each one back
	int count = 0;
	gGlobals.pSaveData = sd;
	gGlobals.time = (float)sv.time;
	for (int i = 0; i < ls->numents; i++) {
		ENTITYTABLE *t = &sd->pTable[i];
		if (!t->pent)
			continue;
		sd->currentIndex = i;
		sd->size = t->location;
		sd->pCurrentData = sd->pBaseData + t->location;
		qboolean global = mode == RESTORE_TRANSITION && (t->flags & FENTTABLE_GLOBAL);
		int r = gEntityInterface.pfnRestore(t->pent, sd, global);
		if (global) {
			// the game updated this level's own copy of the global entity; the stand-in goes
			if (t->pent != sv.edicts)
				ED_Free(t->pent);
			t->pent = NULL;
			continue;
		}
		if (r < 0) {
			if (t->pent != sv.edicts)
				ED_Free(t->pent);
			t->pent = NULL;
			continue;
		}
		count++;
		if (t->flags & FENTTABLE_PLAYER)
			*player = true;
	}
	gGlobals.pSaveData = NULL;
	FreeSaveData(sd);
	return count;
}

// a level visited before comes back as it was left (without the player)
qboolean SV_RestoreLevelState(const char *map)
{
	levelstate_t *ls = FindState(map);
	if (!ls)
		return false;
	qboolean player = false;
	static const vec3_t zero = { 0, 0, 0 };
	int n = RestoreEntities(ls, RESTORE_LEVEL, 0, zero, NULL, &player);
	for (int i = 0; i < 64; i++)
		R_SetLightStyle(i, ls->styles[i]);
	Con_DPrintf("Restored %s: %d entities\n", map, n);
	return true;
}

static edict_t *FindLandmark(const char *name)
{
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (!e->free && e->v.targetname && !strcmp(SV_String(e->v.targetname), name) &&
			!strcmp(SV_String(e->v.classname), "info_landmark"))
			return e;
	}
	return NULL;
}

// the old map's entities that were part of this transition (the player among them); true if the
// player came along
qboolean SV_RestoreTransition(const char *oldmap, const char *landmark)
{
	levelstate_t *ls = FindState(oldmap);
	if (!ls)
		return false;
	int mask = 0;
	vec3_t offset = { 0, 0, 0 };
	if (landmark && *landmark) {
		edict_t *here = FindLandmark(landmark);
		for (int i = 0; i < ls->connections; i++) {
			if (stricmp(ls->levels[i].mapName, sv.mapname) || stricmp(ls->levels[i].landmarkName, landmark))
				continue;
			mask = 1 << i;
			if (here)
				VectorSubtract(here->v.origin, ls->levels[i].vecLandmarkOrigin, offset);
			break;
		}
		if (!here)
			Con_Warnf("Landmark %s isn't in %s\n", landmark, sv.mapname);
	}
	qboolean player = false;
	int n = RestoreEntities(ls, RESTORE_TRANSITION, mask, offset, landmark, &player);
	Con_DPrintf("Transition from %s at %s: %d entities%s\n", oldmap, landmark && *landmark ? landmark : "(none)", n,
		player ? ", the player" : "");
	return player;
}

static void LevelStates_f(void)
{
	for (int i = 0; i < states_count; i++)
		Con_Printf("%s: %d KB, %d entities saved, %d connections\n", states[i].map, states[i].size / 1024,
			states[i].numents, states[i].connections);
	Con_Printf("%d levels remembered\n", states_count);
}

void SV_InitSave(void)
{
	sv_savebuffer = Cvar_Get("sv_savebuffer", "8", 0, "starting size in MB of the buffer a level is saved into (it grows)");
	Cmd_AddCommand("levelstates", LevelStates_f, "lists the levels this game remembers");
}
