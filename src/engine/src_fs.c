// newen - reading files from an installed Half-Life 2: loose files and VPK archives
//
// Half-Life 2 is found through Steam (or -hl2dir <folder>) the first time an "hl2:" path is asked for.
// Its search paths come from hl2/gameinfo.txt. Everything is read only.
#include "src_local.h"
#include <ctype.h>

#define VPK_SIGNATURE   0x55aa1234u
#define VPK_IN_DIR      0x7fff      // archive index meaning "inside the _dir file, after the tree"

typedef struct {
	char    *name;          // lower case, forward slashes
	unsigned hash;
	int      vpk;           // which archive set
	unsigned archive;       // VPK_IN_DIR or the number in <base>_NNN.vpk
	unsigned offset, length;
	unsigned preload_ofs, preload_len;   // bytes kept in the _dir file itself
} vpkentry_t;

typedef struct {
	char     base[MAX_OSPATH];   // path without "_dir.vpk"
	unsigned data_start;         // where VPK_IN_DIR data begins in the _dir file
} vpkfile_t;

static qboolean    hl2_tried, hl2_found;
static char        hl2_dir[MAX_OSPATH];
static vpkfile_t  *vpks;
static int         vpks_count, vpks_cap;
static vpkentry_t *entries;
static int         entries_count, entries_cap;
static int        *buckets;            // entry index + 1, open addressing
static int         buckets_size;
static char      (*loose_dirs)[MAX_OSPATH];   // searched after the VPKs
static int         loose_count, loose_cap;

// a whole file by its full path; NUL-terminated, NULL if it can't be read
static void *ReadWholeFile(const char *full, size_t *len)
{
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
	if (len)
		*len = got;
	return data;
}

static unsigned HashName(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

// lower case, forward slashes, no leading slash
static void CleanName(const char *in, char *out, size_t size)
{
	size_t n = 0;
	while (*in == '/' || *in == '\\')
		in++;
	for (; *in && n < size - 1; in++) {
		char c = *in == '\\' ? '/' : (char)tolower((unsigned char)*in);
		if (c == '/' && n && out[n - 1] == '/')
			continue;
		out[n++] = c;
	}
	out[n] = 0;
}

static void RebuildBuckets(void)
{
	Mem_Free(buckets);
	buckets_size = 1024;
	while (buckets_size < entries_count * 2)
		buckets_size *= 2;
	buckets = Mem_Alloc(sizeof(int) * buckets_size);
	// earlier archives win, like earlier search paths
	for (int i = 0; i < entries_count; i++) {
		unsigned b = entries[i].hash & (buckets_size - 1);
		qboolean dup = false;
		while (buckets[b]) {
			const vpkentry_t *e = &entries[buckets[b] - 1];
			if (e->hash == entries[i].hash && !strcmp(e->name, entries[i].name)) {
				dup = true;
				break;
			}
			b = (b + 1) & (buckets_size - 1);
		}
		if (!dup)
			buckets[b] = i + 1;
	}
}

static const vpkentry_t *FindEntry(const char *clean)
{
	if (!buckets)
		return NULL;
	unsigned h = HashName(clean);
	for (unsigned b = h & (buckets_size - 1); buckets[b]; b = (b + 1) & (buckets_size - 1)) {
		const vpkentry_t *e = &entries[buckets[b] - 1];
		if (e->hash == h && !strcmp(e->name, clean))
			return e;
	}
	return NULL;
}

// reads a NUL-terminated string from the tree; false if it runs off the end
static qboolean TreeString(const unsigned char *tree, size_t size, size_t *pos, const char **out)
{
	const unsigned char *end = memchr(tree + *pos, 0, size - *pos);
	if (!end)
		return false;
	*out = (const char *)tree + *pos;
	*pos = (size_t)(end - tree) + 1;
	return true;
}

// dirfile: full path of a "<base>_dir.vpk". Adds its entries; false (with a warning) if it's damaged.
static qboolean MountVPK(const char *dirfile)
{
	FILE *f = fopen(dirfile, "rb");
	if (!f)
		return false;
	unsigned hdr[7];
	if (fread(hdr, 4, 3, f) != 3 || hdr[0] != VPK_SIGNATURE || (hdr[1] != 1 && hdr[1] != 2)) {
		Con_Warnf("%s isn't a VPK version 1 or 2 archive\n", dirfile);
		fclose(f);
		return false;
	}
	unsigned header_size = 12;
	if (hdr[1] == 2) {
		if (fread(hdr + 3, 4, 4, f) != 4) {
			fclose(f);
			return false;
		}
		header_size = 28;
	}
	size_t tree_size = hdr[2];
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	if (file_size < 0 || tree_size > (size_t)file_size - header_size) {
		Con_Warnf("%s: directory is larger than the file\n", dirfile);
		fclose(f);
		return false;
	}
	unsigned char *tree = Mem_Alloc(tree_size + 1);
	fseek(f, (long)header_size, SEEK_SET);
	size_t got = fread(tree, 1, tree_size, f);
	fclose(f);
	if (got != tree_size) {
		Mem_Free(tree);
		return false;
	}

	if (vpks_count == vpks_cap) {
		vpks_cap = vpks_cap ? vpks_cap * 2 : 8;
		vpks = Mem_Realloc(vpks, sizeof(*vpks) * vpks_cap);
	}
	vpkfile_t *v = &vpks[vpks_count];
	Q_strncpy(v->base, dirfile, sizeof(v->base));
	v->base[strlen(v->base) - strlen("_dir.vpk")] = 0;
	v->data_start = header_size + (unsigned)tree_size;

	// three levels: extension, folder, file name; each list ends with an empty string
	size_t pos = 0;
	int added = 0;
	qboolean ok = true;
	const char *ext, *dir, *name;
	while (ok && (ok = TreeString(tree, tree_size, &pos, &ext)) && *ext) {
		while (ok && (ok = TreeString(tree, tree_size, &pos, &dir)) && *dir) {
			while (ok && (ok = TreeString(tree, tree_size, &pos, &name)) && *name) {
				// crc u32, preload u16, archive u16, offset u32, length u32, terminator u16
				if (tree_size - pos < 18) {
					ok = false;
					break;
				}
				const unsigned char *p = tree + pos;
				unsigned preload = p[4] | p[5] << 8;
				unsigned archive = p[6] | p[7] << 8;
				unsigned offset, length;
				memcpy(&offset, p + 8, 4);
				memcpy(&length, p + 12, 4);
				pos += 18;
				if (tree_size - pos < preload) {
					ok = false;
					break;
				}
				char full[MAX_OSPATH], clean[MAX_OSPATH];
				// a single space stands for "none" (files in the root, or without an extension)
				snprintf(full, sizeof(full), "%s%s%s%s%s", strcmp(dir, " ") ? dir : "", strcmp(dir, " ") ? "/" : "",
					name, strcmp(ext, " ") ? "." : "", strcmp(ext, " ") ? ext : "");
				CleanName(full, clean, sizeof(clean));
				if (entries_count == entries_cap) {
					entries_cap = entries_cap ? entries_cap * 2 : 16384;
					entries = Mem_Realloc(entries, sizeof(*entries) * entries_cap);
				}
				vpkentry_t *e = &entries[entries_count++];
				e->name = Mem_Strdup(clean);
				e->hash = HashName(clean);
				e->vpk = vpks_count;
				e->archive = archive;
				e->offset = offset;
				e->length = length;
				e->preload_ofs = header_size + (unsigned)pos;
				e->preload_len = preload;
				pos += preload;
				added++;
			}
		}
	}
	Mem_Free(tree);
	if (!ok)
		Con_Warnf("%s: directory ends early, %d files read\n", dirfile, added);
	vpks_count++;
	Con_Printf("Source: mounted %s (%d files)\n", dirfile, added);
	return true;
}

static void AddLooseDir(const char *dir)
{
	if (!Sys_DirExists(dir))
		return;
	for (int i = 0; i < loose_count; i++)
		if (!stricmp(loose_dirs[i], dir))
			return;
	if (loose_count == loose_cap) {
		loose_cap = loose_cap ? loose_cap * 2 : 8;
		loose_dirs = Mem_Realloc(loose_dirs, sizeof(*loose_dirs) * loose_cap);
	}
	Q_strncpy(loose_dirs[loose_count++], dir, MAX_OSPATH);
}

static qboolean IsHL2Dir(const char *dir)
{
	return Sys_FileExists(va("%s/hl2/gameinfo.txt", dir));
}

static qboolean FindHL2(void)
{
	const char *parm = COM_ParmValue("-hl2dir");
	if (parm) {
		Q_strncpy(hl2_dir, parm, sizeof(hl2_dir));
		return IsHL2Dir(hl2_dir);
	}
	const char *cache = va("%s/basedir_hl2.txt", Sys_ExeDir());
	FILE *f = fopen(cache, "rb");
	if (f) {
		char line[MAX_OSPATH] = { 0 };
		if (fgets(line, sizeof(line), f))
			line[strcspn(line, "\r\n")] = 0;
		fclose(f);
		if (*line && IsHL2Dir(line)) {
			Q_strncpy(hl2_dir, line, sizeof(hl2_dir));
			return true;
		}
	}
	if (!FS_FindSteamGame("Half-Life 2", "hl2/gameinfo.txt", hl2_dir, sizeof(hl2_dir)))
		return false;
	f = fopen(cache, "wb");
	if (f) {
		fprintf(f, "%s\n", hl2_dir);
		fclose(f);
	}
	return true;
}

// gameinfo.txt's SearchPaths block: "<kinds> <path>" lines. A path ending in .vpk is an archive set
// (hl2/hl2_misc.vpk means hl2/hl2_misc_dir.vpk + _000...), anything else a folder of loose files.
static void MountGameInfo(void)
{
	char *text = ReadWholeFile(va("%s/hl2/gameinfo.txt", hl2_dir), NULL);
	qboolean in_paths = false;
	const char *p = text;
	while (p && (p = COM_Parse(p)) != NULL) {
		if (!in_paths) {
			if (!stricmp(com_token, "SearchPaths") && (p = COM_Parse(p)) != NULL && com_token[0] == '{')
				in_paths = true;
			continue;
		}
		if (com_token[0] == '}')
			break;
		// the kinds token ("game+mod") is in hand; read the path up to the end of the line, because
		// COM_Parse splits at characters paths may hold
		while (*p == ' ' || *p == '\t')
			p++;
		char path[MAX_OSPATH];
		size_t n = 0;
		qboolean quoted = *p == '"';
		if (quoted)
			p++;
		while (*p && *p != '\n' && *p != '\r' && *p != '"' && (quoted || (unsigned char)*p > ' ') && n < sizeof(path) - 1)
			path[n++] = *p++;
		path[n] = 0;
		if (*p == '"')
			p++;
		// |all_source_engine_paths| and |gameinfo_path| are both the install here
		char *rel = path;
		if (!strnicmp(rel, "|all_source_engine_paths|", 25))
			rel += 25;
		else if (!strnicmp(rel, "|gameinfo_path|", 15)) {
			AddLooseDir(va("%s/hl2", hl2_dir));
			continue;
		}
		size_t len = strlen(rel);
		if (!len || strstr(rel, "..") || strchr(rel, '*'))
			continue;   // hl2/custom/* (user add-ons) isn't mounted
		if (len > 4 && !stricmp(rel + len - 4, ".vpk")) {
			rel[len - 4] = 0;
			const char *dirfile = va("%s/%s_dir.vpk", hl2_dir, rel);
			if (Sys_FileExists(dirfile))
				MountVPK(dirfile);
		} else
			AddLooseDir(va("%s/%s", hl2_dir, rel));
	}
	Mem_Free(text);
	if (!vpks_count && !loose_count) {
		// an unreadable gameinfo.txt: the standard layout
		AddLooseDir(va("%s/hl2", hl2_dir));
		MountVPK(va("%s/hl2/hl2_textures_dir.vpk", hl2_dir));
		MountVPK(va("%s/hl2/hl2_misc_dir.vpk", hl2_dir));
	}
	RebuildBuckets();
}

static qboolean MountHL2(void)
{
	if (hl2_tried)
		return hl2_found;
	hl2_tried = true;
	if (!FindHL2()) {
		Con_Warnf("Half-Life 2 not found; start with -hl2dir <folder>\n");
		return false;
	}
	for (char *s = hl2_dir; *s; s++)
		if (*s == '\\')
			*s = '/';
	Con_Printf("Source: Half-Life 2 at %s\n", hl2_dir);
	MountGameInfo();
	hl2_found = true;
	return true;
}

static qboolean LooseFullPath(const char *clean, char *out, size_t outsize)
{
	for (int i = 0; i < loose_count; i++) {
		snprintf(out, outsize, "%s/%s", loose_dirs[i], clean);
		if (Sys_FileExists(out))
			return true;
#ifndef _WIN32
		if (Sys_FixCase(out, strlen(loose_dirs[i])))
			return true;
#endif
	}
	return false;
}

static void *ReadEntry(const vpkentry_t *e, size_t *len)
{
	const vpkfile_t *v = &vpks[e->vpk];
	size_t total = (size_t)e->preload_len + e->length;
	unsigned char *data = Mem_Alloc(total + 1);
	qboolean ok = true;
	if (e->preload_len) {
		FILE *f = fopen(va("%s_dir.vpk", v->base), "rb");
		ok = f && !fseek(f, (long)e->preload_ofs, SEEK_SET) && fread(data, 1, e->preload_len, f) == e->preload_len;
		if (f)
			fclose(f);
	}
	if (ok && e->length) {
		const char *file = e->archive == VPK_IN_DIR ? va("%s_dir.vpk", v->base) : va("%s_%03u.vpk", v->base, e->archive);
		unsigned start = e->archive == VPK_IN_DIR ? v->data_start + e->offset : e->offset;
		FILE *f = fopen(file, "rb");
		ok = f && !fseek(f, (long)start, SEEK_SET) && fread(data + e->preload_len, 1, e->length, f) == e->length;
		if (f)
			fclose(f);
		if (!ok)
			Con_Warnf("Couldn't read %s from %s\n", e->name, file);
	}
	if (!ok) {
		Mem_Free(data);
		return NULL;
	}
	if (len)
		*len = total;
	return data;
}

const char *SrcFS_Prefix(const char *path)
{
	return !strnicmp(path, SRC_HL2_PREFIX, strlen(SRC_HL2_PREFIX)) ? SRC_HL2_PREFIX : "";
}

void *SrcFS_LoadFile(const char *path, size_t *len)
{
	if (len)
		*len = 0;
	if (!*SrcFS_Prefix(path))
		return FS_LoadFile(path, len);
	char clean[MAX_OSPATH], full[MAX_OSPATH];
	CleanName(path + strlen(SRC_HL2_PREFIX), clean, sizeof(clean));
	if (strstr(clean, "..") || strchr(clean, ':') || !MountHL2())
		return NULL;
	// archives before loose files, the order gameinfo.txt gives (Steam leaves empty placeholder files
	// beside the archives)
	const vpkentry_t *e = FindEntry(clean);
	if (e)
		return ReadEntry(e, len);
	return LooseFullPath(clean, full, sizeof(full)) ? ReadWholeFile(full, len) : NULL;
}

qboolean SrcFS_FileExists(const char *path)
{
	if (!*SrcFS_Prefix(path))
		return FS_FileExists(path);
	char clean[MAX_OSPATH], full[MAX_OSPATH];
	CleanName(path + strlen(SRC_HL2_PREFIX), clean, sizeof(clean));
	if (strstr(clean, "..") || strchr(clean, ':') || !MountHL2())
		return false;
	return FindEntry(clean) || LooseFullPath(clean, full, sizeof(full));
}

int SrcFS_Find(const char *pattern, void (*fn)(const char *name, void *ctx), void *ctx)
{
	if (!strchr(pattern, '*') && !strchr(pattern, '?')) {
		if (!SrcFS_FileExists(pattern))
			return 0;
		fn(pattern, ctx);
		return 1;
	}
	if (!*SrcFS_Prefix(pattern) || !MountHL2())
		return 0;
	char clean[MAX_OSPATH];
	CleanName(pattern + strlen(SRC_HL2_PREFIX), clean, sizeof(clean));
	int n = 0;
	for (int i = 0; i < entries_count; i++)
		if (Q_WildcardMatch(clean, entries[i].name) && FindEntry(entries[i].name) == &entries[i]) {
			fn(va(SRC_HL2_PREFIX "%s", entries[i].name), ctx);
			n++;
		}
	return n;
}

// srcfs_find <pattern>: which files match, and where each comes from
static void SrcFSFind_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("srcfs_find <path or pattern>, e.g. srcfs_find hl2:models/barney*.mdl\n");
		return;
	}
	const char *arg = Cmd_Argv(1);
	if (!*SrcFS_Prefix(arg)) {
		char full[MAX_OSPATH];
		if (FS_FullPath(arg, full, sizeof(full)))
			Con_Printf("%s\n", full);
		else
			Con_Printf("%s: not found in Half-Life's folders (Half-Life 2's files need the hl2: prefix)\n", arg);
		return;
	}
	if (!MountHL2())
		return;
	char clean[MAX_OSPATH], full[MAX_OSPATH];
	CleanName(arg + strlen(SRC_HL2_PREFIX), clean, sizeof(clean));
	if (!strchr(clean, '*') && !strchr(clean, '?')) {
		const vpkentry_t *e = FindEntry(clean);
		if (e)
			Con_Printf("%s: %u bytes in %s\n", e->name, e->preload_len + e->length, vpks[e->vpk].base);
		else if (LooseFullPath(clean, full, sizeof(full)))
			Con_Printf("%s\n", full);
		else
			Con_Printf("%s: not found\n", arg);
		return;
	}
	int n = 0;
	for (int i = 0; i < entries_count; i++)
		if (Q_WildcardMatch(clean, entries[i].name) && FindEntry(entries[i].name) == &entries[i]) {
			if (n++ < 200)
				Con_Printf("%s\n", entries[i].name);
		}
	Con_Printf("%d files in the archives%s\n", n, n > 200 ? " (first 200 shown)" : "");
}

void SrcFS_Init(void)
{
	Cmd_AddCommand("srcfs_find", SrcFSFind_f, "srcfs_find <path or pattern> - finds a Source content file (hl2: for Half-Life 2's)");
}

void SrcFS_Shutdown(void)
{
	for (int i = 0; i < entries_count; i++)
		Mem_Free(entries[i].name);
	Mem_Free(entries);
	Mem_Free(vpks);
	Mem_Free(buckets);
	Mem_Free(loose_dirs);
	entries = NULL;
	vpks = NULL;
	buckets = NULL;
	loose_dirs = NULL;
	entries_count = entries_cap = vpks_count = vpks_cap = loose_count = loose_cap = buckets_size = 0;
	hl2_tried = hl2_found = false;
}
