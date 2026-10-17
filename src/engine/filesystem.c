// newen - search paths over the Half-Life install
//
// Lookup order (first match wins):
//   userdata/<game>          newen's write folder (configs, logs, later saves/downloads)
//   <hl>/<game>_addon
//   <hl>/<game>_hd           only with -hd
//   <hl>/<game>_downloads
//   <hl>/<game>
//   <hl>/valve (+_hd)        when <game> isn't valve
//   base/                    newen's own content (Lua libraries, fonts...)
//
// newen never writes into the Half-Life folder.
#include "common.h"

typedef struct searchpath_s {
	char dir[MAX_OSPATH];
	struct searchpath_s *next;
} searchpath_t;

static searchpath_t *search_paths;
static char fs_basedir[MAX_OSPATH];
static char fs_gamedir[64];
static char fs_writedir[MAX_OSPATH];
static char fs_enginedir[MAX_OSPATH];

const char *FS_BaseDir(void)   { return fs_basedir; }
const char *FS_GameDir(void)   { return fs_gamedir; }
const char *FS_WriteDir(void)  { return fs_writedir; }
const char *FS_EngineDir(void) { return fs_enginedir; }

static void FixSlashes(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

static qboolean SafePath(const char *path)
{
	if (!path || !*path)
		return false;
	if (strstr(path, "..") || path[0] == '/' || path[0] == '\\' || strchr(path, ':'))
		return false;
	return true;
}

void FS_AddSearchPath(const char *dir, qboolean front)
{
	if (!Sys_DirExists(dir))
		return;
	for (searchpath_t *sp = search_paths; sp; sp = sp->next)
		if (!stricmp(sp->dir, dir))
			return;
	searchpath_t *sp = Mem_Alloc(sizeof(*sp));
	Q_strncpy(sp->dir, dir, sizeof(sp->dir));
	FixSlashes(sp->dir);
	if (front || !search_paths) {
		sp->next = search_paths;
		search_paths = sp;
	} else {
		searchpath_t *last = search_paths;
		while (last->next)
			last = last->next;
		last->next = sp;
	}
}

static qboolean IsHalfLifeDir(const char *dir)
{
	return Sys_FileExists(va("%s/valve/liblist.gam", dir));
}

// Steam keeps extra library folders in libraryfolders.vdf: "path" "D:\\SteamLibrary"
static qboolean FindInSteamLibraries(const char *steam, char *out, size_t outsize)
{
	if (IsHalfLifeDir(va("%s/steamapps/common/Half-Life", steam))) {
		snprintf(out, outsize, "%s/steamapps/common/Half-Life", steam);
		return true;
	}
	char vdf[MAX_OSPATH];
	snprintf(vdf, sizeof(vdf), "%s/steamapps/libraryfolders.vdf", steam);
	FILE *f = fopen(vdf, "rb");
	if (!f)
		return false;
	char line[2048];
	qboolean found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		const char *p = COM_Parse(line);
		if (!p || stricmp(com_token, "path"))
			continue;
		if (!COM_Parse(p))
			continue;
		// unescape \\ to /
		char lib[MAX_OSPATH];
		size_t n = 0;
		for (const char *s = com_token; *s && n < sizeof(lib) - 1; s++) {
			if (s[0] == '\\' && s[1] == '\\')
				s++;
			lib[n++] = *s == '\\' ? '/' : *s;
		}
		lib[n] = 0;
		if (IsHalfLifeDir(va("%s/steamapps/common/Half-Life", lib))) {
			snprintf(out, outsize, "%s/steamapps/common/Half-Life", lib);
			found = true;
		}
	}
	fclose(f);
	return found;
}

static qboolean FindBaseDir(void)
{
	const char *parm = COM_ParmValue("-basedir");
	if (parm) {
		Q_strncpy(fs_basedir, parm, sizeof(fs_basedir));
		return IsHalfLifeDir(fs_basedir);
	}
	// running from inside the Half-Life folder
	if (IsHalfLifeDir(Sys_ExeDir())) {
		Q_strncpy(fs_basedir, Sys_ExeDir(), sizeof(fs_basedir));
		return true;
	}
	// remembered from last time
	char *saved = NULL;
	FILE *f = fopen(va("%s/basedir.txt", Sys_ExeDir()), "rb");
	if (f) {
		char line[MAX_OSPATH] = { 0 };
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			saved = line;
			if (IsHalfLifeDir(saved)) {
				Q_strncpy(fs_basedir, saved, sizeof(fs_basedir));
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	const char *steam = Sys_SteamPath();
	if (steam && FindInSteamLibraries(steam, fs_basedir, sizeof(fs_basedir)))
		return true;
	return false;
}

static void AddGameDirs(const char *game, qboolean hd)
{
	FS_AddSearchPath(va("%s/%s_addon", fs_basedir, game), false);
	if (hd)
		FS_AddSearchPath(va("%s/%s_hd", fs_basedir, game), false);
	FS_AddSearchPath(va("%s/%s_downloads", fs_basedir, game), false);
	FS_AddSearchPath(va("%s/%s", fs_basedir, game), false);
}

static void Path_f(void)
{
	Con_Printf("Search paths:\n");
	for (searchpath_t *sp = search_paths; sp; sp = sp->next)
		Con_Printf("  %s\n", sp->dir);
	Con_Printf("Write folder: %s\n", fs_writedir);
}

static void Dir_f(void)
{
	const char *dir = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
	const char *pat = Cmd_Argc() > 2 ? Cmd_Argv(2) : "*";
	char **list;
	int n = FS_ListFiles(dir, pat, &list);
	for (int i = 0; i < n; i++)
		Con_Printf("%s\n", list[i]);
	Con_Printf("%d files\n", n);
	FS_FreeList(list, n);
}

void FS_Init(void)
{
	if (!FindBaseDir())
		Sys_Error("Couldn't find Half-Life.\n\nInstall it through Steam, or start " ENGINE_NAME
			" with -basedir \"C:\\path\\to\\Half-Life\".");
	FixSlashes(fs_basedir);

	// remember it so the next start is instant
	FILE *f = fopen(va("%s/basedir.txt", Sys_ExeDir()), "wb");
	if (f) {
		fprintf(f, "%s\n", fs_basedir);
		fclose(f);
	}

	const char *game = COM_ParmValue("-game");
	Q_strncpy(fs_gamedir, game ? game : "valve", sizeof(fs_gamedir));
	if (!SafePath(fs_gamedir) || !Sys_DirExists(va("%s/%s", fs_basedir, fs_gamedir))) {
		Con_Warnf("Game folder \"%s\" not found, using valve\n", fs_gamedir);
		Q_strncpy(fs_gamedir, "valve", sizeof(fs_gamedir));
	}

	snprintf(fs_writedir, sizeof(fs_writedir), "%s/userdata/%s", Sys_ExeDir(), fs_gamedir);
	snprintf(fs_enginedir, sizeof(fs_enginedir), "%s/base", Sys_ExeDir());
	Sys_MakeDirs(va("%s/", fs_writedir));

	qboolean hd = COM_CheckParm("-hd") != 0;
	FS_AddSearchPath(fs_writedir, false);
	AddGameDirs(fs_gamedir, hd);
	if (stricmp(fs_gamedir, "valve"))
		AddGameDirs("valve", hd);
	FS_AddSearchPath(fs_enginedir, false);

	Cmd_AddCommand("path", Path_f, "shows the file search paths");
	Cmd_AddCommand("dir", Dir_f, "dir <folder> [pattern] - lists files across search paths");
}

void FS_Shutdown(void)
{
	while (search_paths) {
		searchpath_t *next = search_paths->next;
		Mem_Free(search_paths);
		search_paths = next;
	}
}

qboolean FS_FullPath(const char *path, char *out, size_t outsize)
{
	if (!SafePath(path))
		return false;
	for (searchpath_t *sp = search_paths; sp; sp = sp->next) {
		snprintf(out, outsize, "%s/%s", sp->dir, path);
		if (Sys_FileExists(out))
			return true;
	}
	return false;
}

qboolean FS_FileExists(const char *path)
{
	char full[MAX_OSPATH];
	return FS_FullPath(path, full, sizeof(full));
}

void *FS_LoadFile(const char *path, size_t *len)
{
	char full[MAX_OSPATH];
	if (len)
		*len = 0;
	if (!FS_FullPath(path, full, sizeof(full)))
		return NULL;
	FILE *f = fopen(full, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	char *data = Mem_Alloc((size_t)size + 1);
	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);
	data[got] = 0;
	if (len)
		*len = got;
	return data;
}

FILE *FS_OpenWrite(const char *path)
{
	if (!SafePath(path))
		return NULL;
	char full[MAX_OSPATH];
	snprintf(full, sizeof(full), "%s/%s", fs_writedir, path);
	Sys_MakeDirs(full);
	return fopen(full, "wb");
}

typedef struct {
	const char *pattern;
	const char *dir;
	char      **list;
	int         count, cap;
} listctx_t;

static void ListOne(const char *name, qboolean isdir, void *p)
{
	listctx_t *ctx = p;
	if (isdir || !Q_WildcardMatch(ctx->pattern, name))
		return;
	char rel[MAX_OSPATH];
	if (*ctx->dir)
		snprintf(rel, sizeof(rel), "%s/%s", ctx->dir, name);
	else
		Q_strncpy(rel, name, sizeof(rel));
	for (int i = 0; i < ctx->count; i++)
		if (!stricmp(ctx->list[i], rel))
			return;
	if (ctx->count == ctx->cap) {
		ctx->cap = ctx->cap ? ctx->cap * 2 : 32;
		ctx->list = Mem_Realloc(ctx->list, sizeof(char *) * ctx->cap);
	}
	ctx->list[ctx->count++] = Mem_Strdup(rel);
}

static int CompareStrings(const void *a, const void *b)
{
	return stricmp(*(char *const *)a, *(char *const *)b);
}

int FS_ListFiles(const char *dir, const char *pattern, char ***out)
{
	listctx_t ctx = { pattern, dir, NULL, 0, 0 };
	*out = NULL;
	if (*dir && !SafePath(dir))
		return 0;
	for (searchpath_t *sp = search_paths; sp; sp = sp->next)
		Sys_ListDir(*dir ? va("%s/%s", sp->dir, dir) : sp->dir, ListOne, &ctx);
	if (ctx.count)
		qsort(ctx.list, ctx.count, sizeof(char *), CompareStrings);
	*out = ctx.list;
	return ctx.count;
}

void FS_FreeList(char **list, int count)
{
	for (int i = 0; i < count; i++)
		Mem_Free(list[i]);
	Mem_Free(list);
}
