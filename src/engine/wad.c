// newen - WAD3 texture archives
#include "gl_local.h"

typedef struct {
	int  filepos, disksize, size;
	char type, compression, pad1, pad2;
	char name[16];
} wadlump_t;

typedef struct {
	char       name[64];
	unsigned char *data;
	size_t     size;
	wadlump_t *lumps;
	int        numlumps;
} wadfile_t;

#define TYP_MIPTEX 0x43

static wadfile_t *wads;
static int wads_count;

static qboolean ReadWad(const char *name, wadfile_t *w)
{
	size_t size;
	unsigned char *data = FS_LoadFile(name, &size);
	if (!data) {
		// maps often list WADs that were only on the mapper's machine; GoldSrc ignores them too
		Con_DPrintf("WAD %s not found\n", name);
		return false;
	}
	if (size < 12 || memcmp(data, "WAD3", 4)) {
		Con_Warnf("%s isn't a WAD3 file\n", name);
		Mem_Free(data);
		return false;
	}
	int numlumps = *(int *)(data + 4);
	int ofs = *(int *)(data + 8);
	if (numlumps < 0 || ofs < 0 || (size_t)ofs + (size_t)numlumps * sizeof(wadlump_t) > size) {
		Con_Warnf("%s is damaged\n", name);
		Mem_Free(data);
		return false;
	}
	Q_strncpy(w->name, name, sizeof(w->name));
	w->data = data;
	w->size = size;
	w->lumps = (wadlump_t *)(data + ofs);
	w->numlumps = numlumps;
	Con_DPrintf("Loaded %s (%d textures)\n", name, numlumps);
	return true;
}

static void LoadWad(const char *name)
{
	for (int i = 0; i < wads_count; i++)
		if (!stricmp(wads[i].name, name))
			return;
	wadfile_t w;
	if (!ReadWad(name, &w))
		return;
	wads = Mem_Realloc(wads, sizeof(*wads) * (wads_count + 1));
	wads[wads_count++] = w;
}

// "\half-life\valve\halflife.wad;\half-life\valve\liquids.wad" -> halflife.wad, liquids.wad
void Wad_LoadList(const char *wadlist)
{
	char item[MAX_OSPATH];
	const char *p = wadlist;
	while (*p) {
		size_t n = strcspn(p, ";");
		if (n && n < sizeof(item)) {
			memcpy(item, p, n);
			item[n] = 0;
			char *base = item;
			for (char *s = item; *s; s++)
				if (*s == '\\' || *s == '/')
					base = s + 1;
			if (*base)
				LoadWad(base);
		}
		p += n;
		if (*p == ';')
			p++;
	}
}

void Wad_FreeAll(void)
{
	for (int i = 0; i < wads_count; i++)
		Mem_Free(wads[i].data);
	Mem_Free(wads);
	wads = NULL;
	wads_count = 0;
}

static miptex_t *FindIn(const wadfile_t *w, const char *name, size_t *size)
{
	for (int j = 0; j < w->numlumps; j++) {
		wadlump_t *l = &w->lumps[j];
		if (l->type != TYP_MIPTEX || l->compression)
			continue;
		char lname[17];
		memcpy(lname, l->name, 16);
		lname[16] = 0;
		if (stricmp(lname, name))
			continue;
		if (l->filepos < 0 || l->disksize < (int)sizeof(miptex_t) || (size_t)l->filepos + l->disksize > w->size)
			return NULL;
		*size = (size_t)l->disksize;
		return (miptex_t *)(w->data + l->filepos);
	}
	return NULL;
}

// a WAD kept apart from the map's list, for the whole session (decals.wad)
void *Wad_Open(const char *name)
{
	wadfile_t *w = Mem_Alloc(sizeof(*w));
	if (ReadWad(name, w))
		return w;
	Mem_Free(w);
	return NULL;
}

miptex_t *Wad_FindIn(void *wad, const char *name, size_t *size)
{
	return wad ? FindIn(wad, name, size) : NULL;
}

miptex_t *Wad_FindMiptex(const char *name, size_t *size)
{
	for (int i = 0; i < wads_count; i++) {
		wadfile_t *w = &wads[i];
		for (int j = 0; j < w->numlumps; j++) {
			wadlump_t *l = &w->lumps[j];
			if (l->type != TYP_MIPTEX || l->compression)
				continue;
			char lname[17];
			memcpy(lname, l->name, 16);
			lname[16] = 0;
			if (stricmp(lname, name))
				continue;
			if (l->filepos < 0 || l->disksize < (int)sizeof(miptex_t) || (size_t)l->filepos + l->disksize > w->size)
				return NULL;
			*size = (size_t)l->disksize;
			return (miptex_t *)(w->data + l->filepos);
		}
	}
	return NULL;
}

unsigned char *Miptex_ToRGBA(const miptex_t *mt, size_t size)
{
	unsigned w = mt->width, h = mt->height;
	if (!w || !h || w > 16384 || h > 16384 || !mt->offsets[0])
		return NULL;
	size_t pixels = (size_t)w * h;
	// the palette follows the 4th mip: short count, then count*3 bytes
	size_t palofs = mt->offsets[3] + (size_t)(w / 8) * (h / 8);
	if (mt->offsets[0] + pixels > size || palofs + 2 + 768 > size)
		return NULL;
	const unsigned char *base = (const unsigned char *)mt;
	const unsigned char *idx = base + mt->offsets[0];
	const unsigned char *pal = base + palofs + 2;

	qboolean transparent = mt->name[0] == '{';
	unsigned char *rgba = Mem_Alloc(pixels * 4);
	for (size_t i = 0; i < pixels; i++) {
		int c = idx[i];
		unsigned char *p = rgba + i * 4;
		if (transparent && c == 255) {
			p[0] = p[1] = p[2] = p[3] = 0;
			continue;
		}
		p[0] = pal[c * 3 + 0];
		p[1] = pal[c * 3 + 1];
		p[2] = pal[c * 3 + 2];
		p[3] = 255;
	}

	if (transparent) {
		// give see-through pixels their neighbours' color so filtering doesn't draw dark outlines
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++) {
				unsigned char *p = rgba + ((size_t)y * w + x) * 4;
				if (p[3])
					continue;
				int r = 0, g = 0, b = 0, n = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						unsigned nx = (x + dx + w) % w, ny = (y + dy + h) % h;
						unsigned char *q = rgba + ((size_t)ny * w + nx) * 4;
						if (q[3]) { r += q[0]; g += q[1]; b += q[2]; n++; }
					}
				if (n) {
					p[0] = (unsigned char)(r / n);
					p[1] = (unsigned char)(g / n);
					p[2] = (unsigned char)(b / n);
				}
			}
	}
	return rgba;
}
