// newen - the file system vgui2 and GameUI use (IFileSystem, "VFileSystem009")
//
// Reads go through the engine's search paths, plus any folder GameUI adds itself (AddSearchPath with a
// path ID such as "CONFIG"). Absolute paths are allowed for reading. Every write lands under
// userdata: a relative name in the game's write folder, an absolute path inside the Half-Life folder
// at the same place under userdata/ (so <hl>/platform/config/x.vdf becomes userdata/platform/config/x.vdf).
// Reads of such a path look there first, so what GameUI saved is what it reads back.
#include "vgui2_local.h"
#include <sys/stat.h>
#include <time.h>

typedef struct { char *dir, *id; } extrapath_t;
static extrapath_t *extra;
static int extra_count;

typedef struct { char **names; char *isdir; int count, pos; qboolean used; } findlist_t;
static findlist_t *finds;
static int finds_count;

static qboolean IsAbsolute(const char *p)
{
	return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

static void Slashes(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

// <hl>/x -> userdata/x; anything else outside userdata stays as it is
static qboolean MirrorPath(const char *abs, char *out, size_t size)
{
	const char *base = FS_BaseDir();
	size_t n = strlen(base);
	if (strnicmp(abs, base, n) || abs[n] != '/')
		return false;
	snprintf(out, size, "%s/userdata/%s", Sys_ExeDir(), abs + n + 1);
	return true;
}

static qboolean Exists(char *path)
{
	if (Sys_FileExists(path) || Sys_DirExists(path))
		return true;
#ifndef _WIN32
	return Sys_FixCase(path, 0);
#else
	return false;
#endif
}

// the file to read for a name, or false
static qboolean Resolve(const char *name, const char *id, char *out, size_t size)
{
	char fixed[MAX_OSPATH];
	Q_strncpy(fixed, name, sizeof(fixed));
	Slashes(fixed);
	if (IsAbsolute(fixed)) {
		if (MirrorPath(fixed, out, size) && Exists(out))
			return true;
		Q_strncpy(out, fixed, size);
		return Exists(out);
	}
	// folders GameUI added under this ID, then the engine's, then the rest
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < extra_count; i++) {
			if (pass == 0 && !(id && extra[i].id && !stricmp(id, extra[i].id)))
				continue;
			if (pass == 1 && id && extra[i].id && !stricmp(id, extra[i].id))
				continue;
			char abs[MAX_OSPATH], mirror[MAX_OSPATH];
			snprintf(abs, sizeof(abs), "%s/%s", extra[i].dir, fixed);
			if (MirrorPath(abs, mirror, sizeof(mirror)) && Exists(mirror)) {
				Q_strncpy(out, mirror, size);
				return true;
			}
			if (Exists(abs)) {
				Q_strncpy(out, abs, size);
				return true;
			}
		}
		if (pass == 0 && FS_FullPath(fixed, out, size))
			return true;
	}
	return false;
}

// where a write goes; false if it would go somewhere it shouldn't
static qboolean WritePath(const char *name, const char *id, char *out, size_t size)
{
	char fixed[MAX_OSPATH];
	Q_strncpy(fixed, name, sizeof(fixed));
	Slashes(fixed);
	if (strstr(fixed, ".."))
		return false;
	if (!IsAbsolute(fixed)) {
		// a folder GameUI added under this ID (inside the Half-Life folder: mirrored)
		for (int i = 0; id && i < extra_count; i++) {
			if (!extra[i].id || stricmp(id, extra[i].id))
				continue;
			char abs[MAX_OSPATH];
			snprintf(abs, sizeof(abs), "%s/%s", extra[i].dir, fixed);
			return MirrorPath(abs, out, size);
		}
		snprintf(out, size, "%s/%s", FS_WriteDir(), fixed);
		return true;
	}
	if (MirrorPath(fixed, out, size))
		return true;
	// only inside newen's own folder otherwise
	const char *exe = Sys_ExeDir();
	if (strnicmp(fixed, exe, strlen(exe)))
		return false;
	Q_strncpy(out, fixed, size);
	return true;
}

// ---------------------------------------------------------------- methods

static void THISCALL F_Nop(VTHIS) {}
static void THISCALL F_RemoveAllSearchPaths(VTHIS)
{
	for (int i = 0; i < extra_count; i++) {
		Mem_Free(extra[i].dir);
		Mem_Free(extra[i].id);
	}
	extra_count = 0;
}

static void THISCALL F_AddSearchPath(VTHIS, const char *path, const char *id)
{
	char dir[MAX_OSPATH];
	Q_strncpy(dir, path, sizeof(dir));
	Slashes(dir);
	size_t n = strlen(dir);
	while (n > 1 && dir[n - 1] == '/')
		dir[--n] = 0;
	if (!IsAbsolute(dir))
		snprintf(dir, sizeof(dir), "%s/%s", FS_BaseDir(), path);
	for (int i = 0; i < extra_count; i++)
		if (!stricmp(extra[i].dir, dir) && !stricmp(extra[i].id ? extra[i].id : "", id ? id : ""))
			return;
	extra = Mem_Realloc(extra, sizeof(*extra) * (extra_count + 1));
	extra[extra_count].dir = Mem_Strdup(dir);
	extra[extra_count].id = id ? Mem_Strdup(id) : NULL;
	extra_count++;
	Con_DPrintf("VGUI2 search path %s (%s)\n", dir, id ? id : "any");
}

static bool THISCALL F_RemoveSearchPath(VTHIS, const char *path)
{
	for (int i = 0; i < extra_count; i++) {
		if (stricmp(extra[i].dir, path))
			continue;
		Mem_Free(extra[i].dir);
		Mem_Free(extra[i].id);
		memmove(&extra[i], &extra[i + 1], sizeof(*extra) * (extra_count - i - 1));
		extra_count--;
		return true;
	}
	return false;
}

static void THISCALL F_RemoveFile(VTHIS, const char *name, const char *id)
{
	char path[MAX_OSPATH];
	if (WritePath(name, id, path, sizeof(path)))
		remove(path);
}

static void THISCALL F_CreateDirHierarchy(VTHIS, const char *name, const char *id)
{
	char path[MAX_OSPATH];
	if (WritePath(name, id, path, sizeof(path)))
		Sys_MakeDirs(va("%s/", path));
}

static bool THISCALL F_FileExists(VTHIS, const char *name)
{
	char path[MAX_OSPATH];
	return Resolve(name, NULL, path, sizeof(path));
}

static bool THISCALL F_IsDirectory(VTHIS, const char *name)
{
	char path[MAX_OSPATH];
	return Resolve(name, NULL, path, sizeof(path)) && Sys_DirExists(path);
}

static void *THISCALL F_Open(VTHIS, const char *name, const char *options, const char *id)
{
	char path[MAX_OSPATH];
	if (!name || !*name)
		return NULL;
	qboolean write = strchr(options, 'w') || strchr(options, 'a') || strchr(options, '+');
	if (write) {
		if (!WritePath(name, id, path, sizeof(path))) {
			Con_Warnf("VGUI2: not writing %s (outside userdata)\n", name);
			return NULL;
		}
		// "r+" of a file that's only in the Half-Life folder: start from a copy
		if (!strchr(options, 'w') && !Sys_FileExists(path)) {
			char src[MAX_OSPATH];
			if (Resolve(name, id, src, sizeof(src))) {
				size_t len;
				FILE *in = fopen(src, "rb");
				char *data = NULL;
				if (in) {
					fseek(in, 0, SEEK_END);
					len = (size_t)ftell(in);
					fseek(in, 0, SEEK_SET);
					data = Mem_Alloc(len + 1);
					len = fread(data, 1, len, in);
					fclose(in);
					Sys_MakeDirs(path);
					FILE *out = fopen(path, "wb");
					if (out) {
						fwrite(data, 1, len, out);
						fclose(out);
					}
					Mem_Free(data);
				}
			}
		}
		Sys_MakeDirs(path);
		return fopen(path, options);
	}
	if (!Resolve(name, id, path, sizeof(path)) || Sys_DirExists(path))
		return NULL;
	return fopen(path, options);
}

static void THISCALL F_Close(VTHIS, void *h)
{
	if (h)
		fclose(h);
}

static void THISCALL F_Seek(VTHIS, void *h, int pos, int type)
{
	static const int whence[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	if (h && type >= 0 && type < 3)
		fseek(h, pos, whence[type]);
}

static unsigned THISCALL F_Tell(VTHIS, void *h) { return h ? (unsigned)ftell(h) : 0; }

static unsigned THISCALL F_SizeHandle(VTHIS, void *h)
{
	if (!h)
		return 0;
	long cur = ftell(h);
	fseek(h, 0, SEEK_END);
	long end = ftell(h);
	fseek(h, cur, SEEK_SET);
	return (unsigned)end;
}

static unsigned THISCALL F_SizeName(VTHIS, const char *name)
{
	char path[MAX_OSPATH];
	struct stat st;
	if (!Resolve(name, NULL, path, sizeof(path)) || stat(path, &st))
		return 0;
	return (unsigned)st.st_size;
}

static long THISCALL F_GetFileTime(VTHIS, const char *name)
{
	char path[MAX_OSPATH];
	struct stat st;
	if (!Resolve(name, NULL, path, sizeof(path)) || stat(path, &st))
		return 0;
	return (long)st.st_mtime;
}

static void THISCALL F_FileTimeToString(VTHIS, char *out, int size, long t)
{
	time_t tt = (time_t)t;
	struct tm *tm = localtime(&tt);
	if (!tm || !strftime(out, (size_t)size, "%a %b %d %H:%M:%S %Y\n", tm))
		Q_strncpy(out, "", (size_t)size);
}

static bool THISCALL F_IsOk(VTHIS, void *h) { return h && !ferror((FILE *)h); }
static void THISCALL F_Flush(VTHIS, void *h) { if (h) fflush(h); }
static bool THISCALL F_EndOfFile(VTHIS, void *h) { return !h || feof((FILE *)h); }

static int THISCALL F_Read(VTHIS, void *out, int size, void *h)
{
	return h && size > 0 ? (int)fread(out, 1, (size_t)size, h) : 0;
}

static int THISCALL F_Write(VTHIS, const void *in, int size, void *h)
{
	return h && size > 0 ? (int)fwrite(in, 1, (size_t)size, h) : 0;
}

static char *THISCALL F_ReadLine(VTHIS, char *out, int size, void *h)
{
	return h ? fgets(out, size, h) : NULL;
}

// variadic: cdecl on both compilers, "this" first
static int F_FPrintf(void *self, void *h, char *fmt, ...)
{
	if (!h)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(h, fmt, ap);
	va_end(ap);
	return n;
}

static void *THISCALL F_GetReadBuffer(VTHIS, void *h, int *size, bool fail) { if (size) *size = 0; return NULL; }
static void THISCALL F_ReleaseReadBuffer(VTHIS, void *h, void *buf) {}

typedef struct { findlist_t *list; const char *pattern; } findctx_t;

static void FindOne(const char *name, qboolean isdir, void *p)
{
	findctx_t *ctx = p;
	findlist_t *l = ctx->list;
	if (!strcmp(name, ".") || !strcmp(name, "..") || !Q_WildcardMatch(ctx->pattern, name))
		return;
	for (int i = 0; i < l->count; i++)
		if (!stricmp(l->names[i], name))
			return;
	l->names = Mem_Realloc(l->names, sizeof(char *) * (l->count + 1));
	l->isdir = Mem_Realloc(l->isdir, l->count + 1);
	l->names[l->count] = Mem_Strdup(name);
	l->isdir[l->count] = (char)isdir;
	l->count++;
}

static void FreeFind(findlist_t *l)
{
	for (int i = 0; i < l->count; i++)
		Mem_Free(l->names[i]);
	Mem_Free(l->names);
	Mem_Free(l->isdir);
	memset(l, 0, sizeof(*l));
}

static const char *THISCALL F_FindFirst(VTHIS, const char *wild, int *handle, const char *id)
{
	char fixed[MAX_OSPATH], dir[MAX_OSPATH];
	Q_strncpy(fixed, wild, sizeof(fixed));
	Slashes(fixed);
	char *slash = strrchr(fixed, '/');
	const char *pattern = slash ? slash + 1 : fixed;
	if (slash)
		*slash = 0;
	const char *sub = slash ? fixed : "";

	int h;
	for (h = 0; h < finds_count && finds[h].used; h++)
		;
	if (h == finds_count) {
		finds = Mem_Realloc(finds, sizeof(*finds) * (finds_count + 1));
		memset(&finds[finds_count++], 0, sizeof(*finds));
	}
	findlist_t *l = &finds[h];
	l->used = true;
	findctx_t ctx = { l, pattern };
	if (IsAbsolute(fixed)) {
		if (MirrorPath(sub, dir, sizeof(dir)))
			Sys_ListDir(dir, FindOne, &ctx);
		Sys_ListDir(sub, FindOne, &ctx);
	} else {
		for (int i = 0; i < extra_count; i++) {
			char abs[MAX_OSPATH];
			snprintf(abs, sizeof(abs), "%s/%s", extra[i].dir, sub);
			if (id && extra[i].id && stricmp(id, extra[i].id))
				continue;
			if (MirrorPath(abs, dir, sizeof(dir)))
				Sys_ListDir(dir, FindOne, &ctx);
			Sys_ListDir(abs, FindOne, &ctx);
		}
		// the engine's search paths (its own listing skips folders)
		char path[MAX_OSPATH];
		for (int i = 0; FS_SearchPath(i, path, sizeof(path)); i++)
			Sys_ListDir(*sub ? va("%s/%s", path, sub) : path, FindOne, &ctx);
	}
	*handle = h;
	if (!l->count) {
		FreeFind(l);
		return NULL;
	}
	l->pos = 0;
	return l->names[0];
}

static const char *THISCALL F_FindNext(VTHIS, int h)
{
	if (h < 0 || h >= finds_count || !finds[h].used || finds[h].pos + 1 >= finds[h].count)
		return NULL;
	return finds[h].names[++finds[h].pos];
}

static bool THISCALL F_FindIsDirectory(VTHIS, int h)
{
	if (h < 0 || h >= finds_count || !finds[h].used || finds[h].pos >= finds[h].count)
		return false;
	return finds[h].isdir[finds[h].pos] != 0;
}

static void THISCALL F_FindClose(VTHIS, int h)
{
	if (h >= 0 && h < finds_count)
		FreeFind(&finds[h]);
}

static void THISCALL F_GetLocalCopy(VTHIS, const char *name) {}

static const char *THISCALL F_GetLocalPath(VTHIS, const char *name, char *out, int size)
{
	return Resolve(name, NULL, out, (size_t)size) ? out : NULL;
}

// one token: a quoted string, a brace, or a run of anything else
static char *THISCALL F_ParseFile(VTHIS, char *data, char *token, bool *quoted)
{
	*token = 0;
	if (quoted)
		*quoted = false;
	if (!data)
		return NULL;
	for (;;) {
		while (*data && (unsigned char)*data <= ' ')
			data++;
		if (!*data)
			return NULL;
		if (data[0] == '/' && data[1] == '/') {
			while (*data && *data != '\n')
				data++;
			continue;
		}
		break;
	}
	int n = 0;
	if (*data == '"') {
		if (quoted)
			*quoted = true;
		data++;
		while (*data && *data != '"' && n < 1023)
			token[n++] = *data++;
		if (*data == '"')
			data++;
	} else if (*data == '{' || *data == '}') {
		token[n++] = *data++;
	} else {
		while ((unsigned char)*data > ' ' && *data != '{' && *data != '}' && *data != '"' && n < 1023)
			token[n++] = *data++;
	}
	token[n] = 0;
	return data;
}

static bool THISCALL F_FullPathToRelativePath(VTHIS, const char *full, char *rel)
{
	char fixed[MAX_OSPATH];
	Q_strncpy(fixed, full, sizeof(fixed));
	Slashes(fixed);
	const char *roots[] = { FS_WriteDir(), va("%s/%s", FS_BaseDir(), FS_GameDir()), va("%s/valve", FS_BaseDir()),
		va("%s/platform", FS_BaseDir()), FS_BaseDir() };
	for (int i = 0; i < 5; i++) {
		size_t n = strlen(roots[i]);
		if (!strnicmp(fixed, roots[i], n) && fixed[n] == '/') {
			strcpy(rel, fixed + n + 1);
			return true;
		}
	}
	strcpy(rel, fixed);
	return false;
}

static bool THISCALL F_GetCurrentDirectory(VTHIS, char *out, int size)
{
	Q_strncpy(out, FS_BaseDir(), (size_t)size);
	return true;
}

static void THISCALL F_SetWarningFunc(VTHIS, void *fn) {}
static void THISCALL F_SetWarningLevel(VTHIS, int level) {}
static void THISCALL F_LogLevelLoad(VTHIS, const char *name) {}
static int  THISCALL F_HintResourceNeed(VTHIS, const char *name, int flags) { return 0; }
static int  THISCALL F_Preloading(VTHIS) { return 0; }
static int  THISCALL F_SetVBuf(VTHIS, void *h, char *buf, int mode, long size) { return 0; }
static void THISCALL F_GetInterfaceVersion(VTHIS, char *out, int size) { Q_strncpy(out, "Stdio", (size_t)size); }
static bool THISCALL F_IsFileImmediatelyAvailable(VTHIS, const char *name) { return true; }
static int  THISCALL F_WaitForResources(VTHIS, const char *list) { return 1; }
static bool THISCALL F_GetWaitForResourcesProgress(VTHIS, int h, float *progress, bool *complete)
{
	if (progress) *progress = 1;
	if (complete) *complete = true;
	return true;
}
static void THISCALL F_CancelWaitForResources(VTHIS, int h) {}
static bool THISCALL F_IsAppReadyForOfflinePlay(VTHIS, int app) { return true; }
static bool THISCALL F_AddPackFile(VTHIS, const char *path, const char *id) { return false; }
static void *THISCALL F_OpenFromCacheForRead(VTHIS, const char *name, const char *options, const char *id)
{
	return F_Open(VTHIS_ARG(self), name, options, id);
}

// ---------------------------------------------------------------- the object

enum {
	VFS_Mount, VFS_Unmount, VFS_RemoveAllSearchPaths, VFS_AddSearchPath, VFS_RemoveSearchPath, VFS_RemoveFile,
	VFS_CreateDirHierarchy, VFS_FileExists, VFS_IsDirectory, VFS_Open, VFS_Close, VFS_Seek, VFS_Tell,
#ifdef _WIN32
	VFS_SizeName, VFS_SizeHandle,   // overloads: reversed by MSVC
#else
	VFS_SizeHandle, VFS_SizeName,
#endif
	VFS_GetFileTime, VFS_FileTimeToString, VFS_IsOk, VFS_Flush, VFS_EndOfFile, VFS_Read, VFS_Write, VFS_ReadLine,
	VFS_FPrintf, VFS_GetReadBuffer, VFS_ReleaseReadBuffer, VFS_FindFirst, VFS_FindNext, VFS_FindIsDirectory,
	VFS_FindClose, VFS_GetLocalCopy, VFS_GetLocalPath, VFS_ParseFile, VFS_FullPathToRelativePath,
	VFS_GetCurrentDirectory, VFS_PrintOpenedFiles, VFS_SetWarningFunc, VFS_SetWarningLevel, VFS_LogLevelLoadStarted,
	VFS_LogLevelLoadFinished, VFS_HintResourceNeed, VFS_PauseResourcePreloading, VFS_ResumeResourcePreloading,
	VFS_SetVBuf, VFS_GetInterfaceVersion, VFS_IsFileImmediatelyAvailable, VFS_WaitForResources,
	VFS_GetWaitForResourcesProgress, VFS_CancelWaitForResources, VFS_IsAppReadyForOfflinePlay, VFS_AddPackFile,
	VFS_OpenFromCacheForRead, VFS_AddSearchPathNoWrite, VFS_GetFileModificationTime, VFS_COUNT
};

static void *fs_vtable[VT_PREFIX + VT_DTORS + VFS_COUNT];
static cppobj_t fs_object;

void *VGFS_Create(void)
{
	void **vt = fs_vtable;
	VG_InitVtable(vt, VFS_COUNT);
	VG_METHOD(vt, VFS_Mount) = (void *)F_Nop;
	VG_METHOD(vt, VFS_Unmount) = (void *)F_Nop;
	VG_METHOD(vt, VFS_RemoveAllSearchPaths) = (void *)F_RemoveAllSearchPaths;
	VG_METHOD(vt, VFS_AddSearchPath) = (void *)F_AddSearchPath;
	VG_METHOD(vt, VFS_RemoveSearchPath) = (void *)F_RemoveSearchPath;
	VG_METHOD(vt, VFS_RemoveFile) = (void *)F_RemoveFile;
	VG_METHOD(vt, VFS_CreateDirHierarchy) = (void *)F_CreateDirHierarchy;
	VG_METHOD(vt, VFS_FileExists) = (void *)F_FileExists;
	VG_METHOD(vt, VFS_IsDirectory) = (void *)F_IsDirectory;
	VG_METHOD(vt, VFS_Open) = (void *)F_Open;
	VG_METHOD(vt, VFS_Close) = (void *)F_Close;
	VG_METHOD(vt, VFS_Seek) = (void *)F_Seek;
	VG_METHOD(vt, VFS_Tell) = (void *)F_Tell;
	VG_METHOD(vt, VFS_SizeHandle) = (void *)F_SizeHandle;
	VG_METHOD(vt, VFS_SizeName) = (void *)F_SizeName;
	VG_METHOD(vt, VFS_GetFileTime) = (void *)F_GetFileTime;
	VG_METHOD(vt, VFS_FileTimeToString) = (void *)F_FileTimeToString;
	VG_METHOD(vt, VFS_IsOk) = (void *)F_IsOk;
	VG_METHOD(vt, VFS_Flush) = (void *)F_Flush;
	VG_METHOD(vt, VFS_EndOfFile) = (void *)F_EndOfFile;
	VG_METHOD(vt, VFS_Read) = (void *)F_Read;
	VG_METHOD(vt, VFS_Write) = (void *)F_Write;
	VG_METHOD(vt, VFS_ReadLine) = (void *)F_ReadLine;
	VG_METHOD(vt, VFS_FPrintf) = (void *)F_FPrintf;
	VG_METHOD(vt, VFS_GetReadBuffer) = (void *)F_GetReadBuffer;
	VG_METHOD(vt, VFS_ReleaseReadBuffer) = (void *)F_ReleaseReadBuffer;
	VG_METHOD(vt, VFS_FindFirst) = (void *)F_FindFirst;
	VG_METHOD(vt, VFS_FindNext) = (void *)F_FindNext;
	VG_METHOD(vt, VFS_FindIsDirectory) = (void *)F_FindIsDirectory;
	VG_METHOD(vt, VFS_FindClose) = (void *)F_FindClose;
	VG_METHOD(vt, VFS_GetLocalCopy) = (void *)F_GetLocalCopy;
	VG_METHOD(vt, VFS_GetLocalPath) = (void *)F_GetLocalPath;
	VG_METHOD(vt, VFS_ParseFile) = (void *)F_ParseFile;
	VG_METHOD(vt, VFS_FullPathToRelativePath) = (void *)F_FullPathToRelativePath;
	VG_METHOD(vt, VFS_GetCurrentDirectory) = (void *)F_GetCurrentDirectory;
	VG_METHOD(vt, VFS_PrintOpenedFiles) = (void *)F_Nop;
	VG_METHOD(vt, VFS_SetWarningFunc) = (void *)F_SetWarningFunc;
	VG_METHOD(vt, VFS_SetWarningLevel) = (void *)F_SetWarningLevel;
	VG_METHOD(vt, VFS_LogLevelLoadStarted) = (void *)F_LogLevelLoad;
	VG_METHOD(vt, VFS_LogLevelLoadFinished) = (void *)F_LogLevelLoad;
	VG_METHOD(vt, VFS_HintResourceNeed) = (void *)F_HintResourceNeed;
	VG_METHOD(vt, VFS_PauseResourcePreloading) = (void *)F_Preloading;
	VG_METHOD(vt, VFS_ResumeResourcePreloading) = (void *)F_Preloading;
	VG_METHOD(vt, VFS_SetVBuf) = (void *)F_SetVBuf;
	VG_METHOD(vt, VFS_GetInterfaceVersion) = (void *)F_GetInterfaceVersion;
	VG_METHOD(vt, VFS_IsFileImmediatelyAvailable) = (void *)F_IsFileImmediatelyAvailable;
	VG_METHOD(vt, VFS_WaitForResources) = (void *)F_WaitForResources;
	VG_METHOD(vt, VFS_GetWaitForResourcesProgress) = (void *)F_GetWaitForResourcesProgress;
	VG_METHOD(vt, VFS_CancelWaitForResources) = (void *)F_CancelWaitForResources;
	VG_METHOD(vt, VFS_IsAppReadyForOfflinePlay) = (void *)F_IsAppReadyForOfflinePlay;
	VG_METHOD(vt, VFS_AddPackFile) = (void *)F_AddPackFile;
	VG_METHOD(vt, VFS_OpenFromCacheForRead) = (void *)F_OpenFromCacheForRead;
	VG_METHOD(vt, VFS_AddSearchPathNoWrite) = (void *)F_AddSearchPath;
	VG_METHOD(vt, VFS_GetFileModificationTime) = (void *)F_GetFileTime;
	fs_object.vtable = vt + VT_PREFIX;
	return &fs_object;
}
