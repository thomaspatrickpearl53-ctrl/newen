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
// A saved game (save/<name>.sav in the game's userdata folder) is every remembered level plus the
// game's global state (env_global and the like). Loading one brings all of it back and restores the
// current level with the player in it. Field-name tokens are kept as our own copies of the strings,
// so a state read from a file works the same as one made a moment ago.
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
static char last_save[64];         // what "reload" (after dying) loads

// the game's global state, from a loaded save until the map is up
static unsigned char *global_data;
static int global_size;
static char **global_tokens;
static int global_tokencount;

static void FreeTokens(char **tokens, int count)
{
	for (int i = 0; tokens && i < count; i++)
		Mem_Free(tokens[i]);
	Mem_Free(tokens);
}

static char **CopyTokens(char **src, int count)
{
	char **t = Mem_Alloc(sizeof(char *) * (count > 0 ? count : 1));
	for (int i = 0; i < count; i++)
		if (src[i])
			t[i] = Mem_Strdup(src[i]);
	return t;
}

static void FreeState(levelstate_t *ls)
{
	Mem_Free(ls->data);
	FreeTokens(ls->tokens, ls->tokencount);
	Mem_Free(ls->ents);
	memset(ls, 0, sizeof(*ls));
}

static void FreeGlobal(void)
{
	Mem_Free(global_data);
	FreeTokens(global_tokens, global_tokencount);
	global_data = NULL;
	global_tokens = NULL;
	global_size = global_tokencount = 0;
}

void SV_ClearLevelStates(void)
{
	for (int i = 0; i < states_count; i++)
		FreeState(&states[i]);
	Mem_Free(states);
	states = NULL;
	states_count = 0;
	last_save[0] = 0;   // a new game: dying restarts the map
}

const char *SV_LastSave(void)
{
	return last_save[0] ? last_save : NULL;
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
	ls->tokens = CopyTokens(sd->pTokens, sd->tokenCount);
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

// a level visited before (without the player), a transition's entities, or a whole saved level
typedef enum { RESTORE_LEVEL, RESTORE_TRANSITION, RESTORE_SAVEGAME } restoremode_t;

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
	} else if (mode == RESTORE_SAVEGAME) {
		sd->fUseLandmark = 1;   // with no offset: everything (the player too) stays exactly where it was
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
		if (mode == RESTORE_LEVEL && isplayer)
			continue;
		if (mode == RESTORE_TRANSITION && !(isplayer || (s->flags & mask)))
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

// a saved game's current level, the player in it; true if the player came back
qboolean SV_RestoreSavedLevel(const char *map)
{
	levelstate_t *ls = FindState(map);
	if (!ls)
		return false;
	qboolean player = false;
	static const vec3_t zero = { 0, 0, 0 };
	int n = RestoreEntities(ls, RESTORE_SAVEGAME, 0, zero, NULL, &player);
	for (int i = 0; i < 64; i++)
		R_SetLightStyle(i, ls->styles[i]);
	Con_DPrintf("Loaded %s: %d entities\n", map, n);
	return player;
}

// the global state from the loaded save goes back to the game (after its ResetGlobalState)
void SV_RestoreGlobalState(void)
{
	if (!global_data)
		return;
	SAVERESTOREDATA *sd = NewSaveData(global_size + 1, 0, global_tokencount);
	memcpy(sd->pBaseData, global_data, (size_t)global_size);
	for (int i = 0; i < global_tokencount; i++)
		sd->pTokens[i] = global_tokens[i];
	sd->bufferSize = global_size;
	sd->size = 0;
	gGlobals.pSaveData = sd;
	gEntityInterface.pfnRestoreGlobalState(sd);
	gGlobals.pSaveData = NULL;
	memset(sd->pTokens, 0, sizeof(char *) * global_tokencount);   // still ours
	FreeSaveData(sd);
	FreeGlobal();
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

// ---------------------------------------------------------------- saved games

#define SAVE_MAGIC   "NEWENSAV"
#define SAVE_VERSION 1

static void WriteInt(FILE *f, int v) { fwrite(&v, 4, 1, f); }
static void WriteBytes(FILE *f, const void *p, int n) { WriteInt(f, n); if (n > 0) fwrite(p, 1, (size_t)n, f); }
static void WriteString(FILE *f, const char *s) { WriteBytes(f, s, (int)strlen(s)); }

static void WriteTokens(FILE *f, char **tokens, int count)
{
	int used = 0;
	for (int i = 0; i < count; i++)
		used += tokens[i] != NULL;
	WriteInt(f, count);
	WriteInt(f, used);
	for (int i = 0; i < count; i++)
		if (tokens[i]) {
			WriteInt(f, i);
			WriteString(f, tokens[i]);
		}
}

typedef struct {
	const unsigned char *p, *end;
	qboolean bad;
} reader_t;

static int ReadInt(reader_t *r)
{
	if (r->end - r->p < 4) {
		r->bad = true;
		return 0;
	}
	int v;
	memcpy(&v, r->p, 4);
	r->p += 4;
	return v;
}

// a block: a fresh copy (zero-terminated) and its length
static void *ReadBytes(reader_t *r, int *len)
{
	int n = ReadInt(r);
	if (n < 0 || r->end - r->p < n) {
		r->bad = true;
		*len = 0;
		return Mem_Alloc(1);
	}
	unsigned char *b = Mem_Alloc((size_t)n + 1);
	memcpy(b, r->p, (size_t)n);
	r->p += n;
	*len = n;
	return b;
}

static void ReadStringTo(reader_t *r, char *out, size_t size)
{
	int n;
	char *s = ReadBytes(r, &n);
	Q_strncpy(out, s, size);
	Mem_Free(s);
}

static char **ReadTokens(reader_t *r, int *count)
{
	*count = ReadInt(r);
	int used = ReadInt(r);
	if (*count <= 0 || *count > 1 << 20 || used < 0 || used > *count) {
		r->bad = true;
		*count = 0;
		return Mem_Alloc(sizeof(char *));
	}
	char **t = Mem_Alloc(sizeof(char *) * *count);
	for (int k = 0; k < used && !r->bad; k++) {
		int i = ReadInt(r), n;
		char *s = ReadBytes(r, &n);
		if (i >= 0 && i < *count && !t[i])
			t[i] = s;
		else
			Mem_Free(s);
	}
	return t;
}

static qboolean SaveGame(const char *name, qboolean quiet)
{
	edict_t *pl = SV_LocalPlayer();
	if (!sv.active || !sv_game_loaded || !pl) {
		Con_Printf("Not playing a map\n");
		return false;
	}
	if (pl->v.health <= 0 || pl->v.deadflag) {
		Con_Printf("Can't save while dead\n");
		return false;
	}
	SV_SaveLevelState();   // the level you're in joins the ones you've been to

	SAVERESTOREDATA *gd = NewSaveData(1024 * 1024, 0, 4096);
	gGlobals.pSaveData = gd;
	gEntityInterface.pfnSaveGlobalState(gd);
	gGlobals.pSaveData = NULL;

	FILE *f = FS_OpenWrite(va("save/%s.sav", name));
	if (!f) {
		Con_Printf("Couldn't write save/%s.sav\n", name);
		FreeSaveData(gd);
		return false;
	}
	fwrite(SAVE_MAGIC, 1, 8, f);
	WriteInt(f, SAVE_VERSION);
	WriteString(f, sv.mapname);
	float skill = Cvar_VariableValue("skill");
	fwrite(&skill, 4, 1, f);
	WriteBytes(f, gd->pBaseData, gd->size);
	WriteTokens(f, gd->pTokens, gd->tokenCount);
	WriteInt(f, states_count);
	for (int i = 0; i < states_count; i++) {
		levelstate_t *ls = &states[i];
		WriteString(f, ls->map);
		WriteBytes(f, ls->data, ls->size);
		WriteTokens(f, ls->tokens, ls->tokencount);
		WriteInt(f, ls->numents);
		for (int k = 0; k < ls->numents; k++) {
			WriteInt(f, ls->ents[k].location);
			WriteInt(f, ls->ents[k].size);
			WriteInt(f, ls->ents[k].flags);
			WriteString(f, ls->ents[k].classname);
		}
		WriteInt(f, ls->connections);
		for (int k = 0; k < ls->connections; k++) {
			WriteString(f, ls->levels[k].mapName);
			WriteString(f, ls->levels[k].landmarkName);
			fwrite(ls->levels[k].vecLandmarkOrigin, 4, 3, f);
		}
		for (int k = 0; k < 64; k++)
			WriteString(f, ls->styles[k]);
	}
	fclose(f);
	FreeSaveData(gd);
	Q_strncpy(last_save, name, sizeof(last_save));
	if (!quiet)
		Con_Printf("Saved %s\n", name);
	return true;
}

static qboolean LoadGame(const char *name)
{
	size_t len = 0;
	unsigned char *file = FS_LoadFile(va("save/%s.sav", name), &len);
	if (!file) {
		Con_Printf("No saved game called %s\n", name);
		return false;
	}
	reader_t rd = { file + 8, file + len, false }, *r = &rd;
	if (len < 12 || memcmp(file, SAVE_MAGIC, 8) || ReadInt(r) != SAVE_VERSION) {
		Con_Printf("%s isn't a newen saved game (or it's from another version)\n", name);
		Mem_Free(file);
		return false;
	}
	char map[64];
	ReadStringTo(r, map, sizeof(map));
	float skill = 1;
	if (rd.end - rd.p >= 4) {
		memcpy(&skill, rd.p, 4);
		rd.p += 4;
	}
	unsigned char *gdata;
	int gsize, gcount;
	gdata = ReadBytes(r, &gsize);
	char **gtokens = ReadTokens(r, &gcount);

	int n = ReadInt(r);
	levelstate_t *loaded = Mem_Alloc(sizeof(levelstate_t) * (n > 0 && n < 4096 ? n : 1));
	int count = 0;
	for (int i = 0; i < n && n < 4096 && !rd.bad; i++) {
		levelstate_t *ls = &loaded[count++];
		ReadStringTo(r, ls->map, sizeof(ls->map));
		ls->data = ReadBytes(r, &ls->size);
		ls->tokens = ReadTokens(r, &ls->tokencount);
		ls->numents = ReadInt(r);
		if (ls->numents < 0 || ls->numents > 1 << 20) {
			rd.bad = true;
			ls->numents = 0;
		}
		ls->ents = Mem_Alloc(sizeof(savedent_t) * (ls->numents > 0 ? ls->numents : 1));
		for (int k = 0; k < ls->numents && !rd.bad; k++) {
			ls->ents[k].location = ReadInt(r);
			ls->ents[k].size = ReadInt(r);
			ls->ents[k].flags = ReadInt(r);
			ReadStringTo(r, ls->ents[k].classname, sizeof(ls->ents[k].classname));
			if (ls->ents[k].location < 0 || ls->ents[k].size < 0 || ls->ents[k].location + ls->ents[k].size > ls->size)
				rd.bad = true;
		}
		ls->connections = ReadInt(r);
		if (ls->connections < 0 || ls->connections > MAX_LEVEL_CONNECTIONS) {
			rd.bad = true;
			ls->connections = 0;
		}
		for (int k = 0; k < ls->connections && !rd.bad; k++) {
			ReadStringTo(r, ls->levels[k].mapName, sizeof(ls->levels[k].mapName));
			ReadStringTo(r, ls->levels[k].landmarkName, sizeof(ls->levels[k].landmarkName));
			if (rd.end - rd.p < 12) {
				rd.bad = true;
				break;
			}
			memcpy(ls->levels[k].vecLandmarkOrigin, rd.p, 12);
			rd.p += 12;
		}
		for (int k = 0; k < 64 && !rd.bad; k++)
			ReadStringTo(r, ls->styles[k], sizeof(ls->styles[k]));
	}
	Mem_Free(file);
	qboolean found = false;
	for (int i = 0; i < count; i++)
		found |= !stricmp(loaded[i].map, map);
	if (rd.bad || !found) {
		Con_Printf("save/%s.sav is damaged\n", name);
		for (int i = 0; i < count; i++)
			FreeState(&loaded[i]);
		Mem_Free(loaded);
		Mem_Free(gdata);
		FreeTokens(gtokens, gcount);
		return false;
	}

	// it's good: it replaces the game in progress
	SV_ClearLevelStates();
	states = loaded;
	states_count = count;
	FreeGlobal();
	global_data = gdata;
	global_size = gsize;
	global_tokens = gtokens;
	global_tokencount = gcount;
	Cvar_SetValue("skill", skill);
	Q_strncpy(last_save, name, sizeof(last_save));
	R_LoadMap(map, NULL, NULL, true);
	return true;
}

static void Save_f(void)
{
	const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "quick";
	if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
		Con_Printf("A save name can't have / or ..\n");
		return;
	}
	SaveGame(name, false);
}

static void Load_f(void)
{
	const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "quick";
	if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
		Con_Printf("A save name can't have / or ..\n");
		return;
	}
	char copy[64];
	Q_strncpy(copy, name, sizeof(copy));   // the map load runs commands; Cmd_Argv won't last
	LoadGame(copy);
}

// at each level change, and when the game asks (trigger_autosave)
void SV_AutoSave(void)
{
	if (SV_LocalPlayer() && SV_LocalPlayer()->v.health > 0)
		SaveGame("autosave", true);
}

static void AutoSave_f(void) { SV_AutoSave(); }

static void Saves_f(void)
{
	char **list;
	int n = FS_ListFiles("save", "*.sav", &list);
	for (int i = 0; i < n; i++)
		Con_Printf("%s\n", list[i]);
	Con_Printf("%d saved games\n", n);
	FS_FreeList(list, n);
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
	Cmd_AddCommand("save", Save_f, "save [name] - saves the game (\"quick\" if no name; F6)");
	Cmd_AddCommand("load", Load_f, "load [name] - loads a saved game (\"quick\" if no name; F9)");
	Cmd_AddCommand("autosave", AutoSave_f, "saves the game as \"autosave\"");
	Cmd_AddCommand("saves", Saves_f, "lists the saved games");
}
