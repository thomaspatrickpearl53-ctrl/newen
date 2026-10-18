// newen - BSP v30 loader (Half-Life maps)
//
// Nothing here has a fixed limit: arrays are sized from the file. Coordinates are floats, so maps
// beyond GoldSrc's +-4096 load fine; node/leaf bounds are 16-bit in the format (+-32767).
#include "bsp.h"
#include <ctype.h>

world_t world;

#define BSPVERSION 30

enum {
	LUMP_ENTITIES, LUMP_PLANES, LUMP_TEXTURES, LUMP_VERTEXES, LUMP_VISIBILITY, LUMP_NODES,
	LUMP_TEXINFO, LUMP_FACES, LUMP_LIGHTING, LUMP_CLIPNODES, LUMP_LEAFS, LUMP_MARKSURFACES,
	LUMP_EDGES, LUMP_SURFEDGES, LUMP_MODELS, HEADER_LUMPS
};

typedef struct { int fileofs, filelen; } lump_t;
typedef struct { int version; lump_t lumps[HEADER_LUMPS]; } dheader_t;

// newen's extended format (NBSP, docs/NBSP.md): BSP30's lumps with 32-bit indices and float bounds, and a lump
// count so more lumps can follow. Header: "NBSP", version, number of lumps, then the lumps.
#define NBSP_IDENT   (('P' << 24) | ('S' << 16) | ('B' << 8) | 'N')
#define NBSP_VERSION 1

#pragma pack(push, 1)
typedef struct { float normal[3]; float dist; int type; } dplane_t;
typedef struct { int planenum; short children[2]; short mins[3], maxs[3]; unsigned short firstface, numfaces; } dnode_t;
typedef struct { float vecs[2][4]; int miptex; int flags; } dtexinfo_t;
typedef struct { unsigned short planenum; short side; int firstedge; short numedges; short texinfo;
                 unsigned char styles[4]; int lightofs; } dface_t;
typedef struct { int contents; int visofs; short mins[3], maxs[3];
                 unsigned short firstmarksurface, nummarksurfaces; unsigned char ambient_level[4]; } dleaf_t;
typedef struct { unsigned short v[2]; } dedge_t;
typedef struct { int planenum; short children[2]; } dclipnode_t;
typedef struct { float mins[3], maxs[3]; float origin[3]; int headnode[4]; int visleafs; int firstface, numfaces; } dmodel_t;

// the wide versions: NBSP stores these; BSP30's are widened into them as they load
typedef struct { int planenum; int children[2]; float mins[3], maxs[3]; int firstface, numfaces; } wnode_t;
typedef struct { int planenum; int side; int firstedge; int numedges; int texinfo;
                 unsigned char styles[4]; int lightofs; } wface_t;
typedef struct { int contents; int visofs; float mins[3], maxs[3];
                 int firstmarksurface, nummarksurfaces; unsigned char ambient_level[4]; } wleaf_t;
typedef struct { unsigned int v[2]; } wedge_t;
typedef struct { int planenum; int children[2]; } wclipnode_t;
#pragma pack(pop)

static unsigned char *file_base;
static size_t file_size;
static lump_t lumps[HEADER_LUMPS];
static qboolean wide;   // NBSP
static const char *load_error;

// widened copies of BSP30 lumps, freed when the load is over
static void **scratch;
static int scratch_count;

static void *Scratch(size_t size)
{
	scratch = Mem_Realloc(scratch, sizeof(void *) * (scratch_count + 1));
	return scratch[scratch_count++] = Mem_Alloc(size ? size : 1);
}

static void FreeScratch(void)
{
	for (int i = 0; i < scratch_count; i++)
		Mem_Free(scratch[i]);
	Mem_Free(scratch);
	scratch = NULL;
	scratch_count = 0;
}

static void *LumpData(int lump, size_t elemsize, int *count)
{
	lump_t *l = &lumps[lump];
	if (l->fileofs < 0 || l->filelen < 0 || (size_t)l->fileofs + (size_t)l->filelen > file_size) {
		load_error = va("lump %d is outside the file", lump);
		*count = 0;
		return NULL;
	}
	if (elemsize && l->filelen % elemsize) {
		load_error = va("lump %d has a bad size", lump);
		*count = 0;
		return NULL;
	}
	*count = elemsize ? (int)(l->filelen / elemsize) : l->filelen;
	return file_base + l->fileofs;
}

static wnode_t *GetNodes(int *n)
{
	if (wide)
		return LumpData(LUMP_NODES, sizeof(wnode_t), n);
	dnode_t *d = LumpData(LUMP_NODES, sizeof(dnode_t), n);
	if (!d)
		return NULL;
	wnode_t *w = Scratch(sizeof(wnode_t) * *n);
	for (int i = 0; i < *n; i++) {
		w[i].planenum = d[i].planenum;
		w[i].children[0] = d[i].children[0];
		w[i].children[1] = d[i].children[1];
		for (int j = 0; j < 3; j++) {
			w[i].mins[j] = d[i].mins[j];
			w[i].maxs[j] = d[i].maxs[j];
		}
		w[i].firstface = d[i].firstface;
		w[i].numfaces = d[i].numfaces;
	}
	return w;
}

static wface_t *GetFaces(int *n)
{
	if (wide)
		return LumpData(LUMP_FACES, sizeof(wface_t), n);
	dface_t *d = LumpData(LUMP_FACES, sizeof(dface_t), n);
	if (!d)
		return NULL;
	wface_t *w = Scratch(sizeof(wface_t) * *n);
	for (int i = 0; i < *n; i++) {
		w[i].planenum = d[i].planenum;
		w[i].side = d[i].side;
		w[i].firstedge = d[i].firstedge;
		w[i].numedges = d[i].numedges;
		w[i].texinfo = d[i].texinfo;
		memcpy(w[i].styles, d[i].styles, 4);
		w[i].lightofs = d[i].lightofs;
	}
	return w;
}

static wleaf_t *GetLeafs(int *n)
{
	if (wide)
		return LumpData(LUMP_LEAFS, sizeof(wleaf_t), n);
	dleaf_t *d = LumpData(LUMP_LEAFS, sizeof(dleaf_t), n);
	if (!d)
		return NULL;
	wleaf_t *w = Scratch(sizeof(wleaf_t) * *n);
	for (int i = 0; i < *n; i++) {
		w[i].contents = d[i].contents;
		w[i].visofs = d[i].visofs;
		for (int j = 0; j < 3; j++) {
			w[i].mins[j] = d[i].mins[j];
			w[i].maxs[j] = d[i].maxs[j];
		}
		w[i].firstmarksurface = d[i].firstmarksurface;
		w[i].nummarksurfaces = d[i].nummarksurfaces;
		memcpy(w[i].ambient_level, d[i].ambient_level, 4);
	}
	return w;
}

static wedge_t *GetEdges(int *n)
{
	if (wide)
		return LumpData(LUMP_EDGES, sizeof(wedge_t), n);
	dedge_t *d = LumpData(LUMP_EDGES, sizeof(dedge_t), n);
	if (!d)
		return NULL;
	wedge_t *w = Scratch(sizeof(wedge_t) * *n);
	for (int i = 0; i < *n; i++) {
		w[i].v[0] = d[i].v[0];
		w[i].v[1] = d[i].v[1];
	}
	return w;
}

static int *GetMarkSurfaces(int *n)
{
	if (wide)
		return LumpData(LUMP_MARKSURFACES, sizeof(int), n);
	unsigned short *d = LumpData(LUMP_MARKSURFACES, sizeof(unsigned short), n);
	if (!d)
		return NULL;
	int *w = Scratch(sizeof(int) * *n);
	for (int i = 0; i < *n; i++)
		w[i] = d[i];
	return w;
}

static wclipnode_t *GetClipnodes(int *n)
{
	if (wide)
		return LumpData(LUMP_CLIPNODES, sizeof(wclipnode_t), n);
	dclipnode_t *d = LumpData(LUMP_CLIPNODES, sizeof(dclipnode_t), n);
	if (!d)
		return NULL;
	wclipnode_t *w = Scratch(sizeof(wclipnode_t) * *n);
	for (int i = 0; i < *n; i++) {
		w[i].planenum = d[i].planenum;
		for (int j = 0; j < 2; j++) {
			int ch = d[i].children[j];
			// children are 16-bit; positive values past 32767 come out negative but aren't contents
			if (ch < -15 && *n > 32767)
				ch = (unsigned short)ch;
			w[i].children[j] = ch;
		}
	}
	return w;
}

// ---------------------------------------------------------------- entities

static void ParseEntities(const char *text, size_t len)
{
	char *copy = Mem_Alloc(len + 1);
	memcpy(copy, text, len);
	const char *p = copy;
	int cap = 0;
	for (;;) {
		p = COM_Parse(p);
		if (!p)
			break;
		if (com_token[0] != '{') {
			Con_Warnf("Entity lump: expected {, got %s\n", com_token);
			break;
		}
		if (world.numentities == cap) {
			cap = cap ? cap * 2 : 256;
			world.entities = Mem_Realloc(world.entities, sizeof(*world.entities) * cap);
		}
		bspentity_t *e = &world.entities[world.numentities++];
		memset(e, 0, sizeof(*e));
		int paircap = 0;
		for (;;) {
			p = COM_Parse(p);
			if (!p || com_token[0] == '}')
				break;
			char key[256];
			Q_strncpy(key, com_token, sizeof(key));
			p = COM_Parse(p);
			if (!p)
				break;
			if (e->numpairs == paircap) {
				paircap = paircap ? paircap * 2 : 16;
				e->keys = Mem_Realloc(e->keys, sizeof(char *) * paircap);
				e->values = Mem_Realloc(e->values, sizeof(char *) * paircap);
			}
			e->keys[e->numpairs] = Mem_Strdup(key);
			e->values[e->numpairs] = Mem_Strdup(com_token);
			e->numpairs++;
		}
		if (!p)
			break;
	}
	Mem_Free(copy);
}

const char *BSP_ValueForKey(const bspentity_t *ent, const char *key)
{
	for (int i = 0; i < ent->numpairs; i++)
		if (!strcmp(ent->keys[i], key))
			return ent->values[i];
	return "";
}

// ---------------------------------------------------------------- textures

static qboolean IsSkyName(const char *name)
{
	return !strnicmp(name, "sky", 3);
}

static gltexture_t *LoadTextureImage(texture_t *tx, const miptex_t *embedded, size_t embedded_size)
{
	int flags = TF_MIPMAP | TF_WORLD;
	if (tx->kind == TEXKIND_ALPHA)
		flags |= TF_ALPHA;

	// HD replacement: textures/<name>.png/.tga/.jpg/.bmp
	int w, h;
	unsigned char *rgba = Image_LoadAny("textures", tx->name, &w, &h);
	if (rgba) {
		gltexture_t *t = GL_UploadTexture(tx->name, rgba, w, h, flags);
		t->hd = true;
		Mem_Free(rgba);
		return t;
	}

	const miptex_t *mt = embedded;
	size_t size = embedded_size;
	if (!mt)
		mt = Wad_FindMiptex(tx->name, &size);
	if (!mt) {
		Con_Warnf("Texture %s not found\n", tx->name);
		return GL_MissingTexture();
	}
	rgba = Miptex_ToRGBA(mt, size);
	if (!rgba) {
		Con_Warnf("Texture %s is damaged\n", tx->name);
		return GL_MissingTexture();
	}
	gltexture_t *t = GL_UploadTexture(tx->name, rgba, (int)mt->width, (int)mt->height, flags);
	Mem_Free(rgba);
	return t;
}

// '+0name'..'+9name' animate in order; '+aname'..'+jname' are the alternate set (toggled by entities)
static void LinkAnimations(void)
{
	for (int i = 0; i < world.numtextures; i++) {
		texture_t *tx = world.textures[i];
		if (!tx || tx->name[0] != '+' || tx->anim_next)
			continue;
		texture_t *anims[10] = { 0 }, *altanims[10] = { 0 };
		int max = 0, altmax = 0;
		for (int j = i; j < world.numtextures; j++) {
			texture_t *t2 = world.textures[j];
			if (!t2 || t2->name[0] != '+' || stricmp(t2->name + 2, tx->name + 2))
				continue;
			int c = tolower((unsigned char)t2->name[1]);
			if (c >= '0' && c <= '9') {
				anims[c - '0'] = t2;
				if (c - '0' + 1 > max) max = c - '0' + 1;
			} else if (c >= 'a' && c <= 'j') {
				altanims[c - 'a'] = t2;
				if (c - 'a' + 1 > altmax) altmax = c - 'a' + 1;
			}
		}
		for (int j = 0; j < max; j++) {
			if (!anims[j]) { Con_Warnf("Animation %s is missing frame %d\n", tx->name, j); max = j; break; }
		}
		for (int j = 0; j < altmax; j++) {
			if (!altanims[j]) { altmax = j; break; }
		}
		for (int j = 0; j < max; j++) {
			anims[j]->anim_total = max;
			anims[j]->anim_min = j;
			anims[j]->anim_max = j + 1;
			anims[j]->anim_next = anims[(j + 1) % max];
			if (altmax)
				anims[j]->alternate_anims = altanims[0];
		}
		for (int j = 0; j < altmax; j++) {
			altanims[j]->anim_total = altmax;
			altanims[j]->anim_min = j;
			altanims[j]->anim_max = j + 1;
			altanims[j]->anim_next = altanims[(j + 1) % altmax];
			if (max)
				altanims[j]->alternate_anims = anims[0];
		}
	}
}

static qboolean LoadTextures(void)
{
	int len;
	unsigned char *data = LumpData(LUMP_TEXTURES, 0, &len);
	if (!data)
		return false;
	if (len < 4) {
		world.numtextures = 0;
		return true;
	}
	int count = *(int *)data;
	if (count < 0 || 4 + (size_t)count * 4 > (size_t)len) {
		load_error = "texture lump is damaged";
		return false;
	}
	int *offsets = (int *)(data + 4);
	world.numtextures = count;
	world.textures = Mem_Alloc(sizeof(texture_t *) * (count ? count : 1));
	int hd = 0;
	for (int i = 0; i < count; i++) {
		if (offsets[i] < 0 || (size_t)offsets[i] + sizeof(miptex_t) > (size_t)len)
			continue;   // texture slot unused
		miptex_t *mt = (miptex_t *)(data + offsets[i]);
		texture_t *tx = Mem_Alloc(sizeof(*tx));
		memcpy(tx->name, mt->name, 15);
		tx->name[15] = 0;
		tx->width = mt->width ? (int)mt->width : 16;
		tx->height = mt->height ? (int)mt->height : 16;
		tx->index = i;
		if (IsSkyName(tx->name))
			tx->kind = TEXKIND_SKY;
		else if (tx->name[0] == '!' || tx->name[0] == '*' || !strnicmp(tx->name, "water", 5))
			tx->kind = TEXKIND_WATER;
		else if (tx->name[0] == '{')
			tx->kind = TEXKIND_ALPHA;
		world.textures[i] = tx;
		if (tx->kind == TEXKIND_SKY)
			continue;   // drawn from the skybox
		const miptex_t *embedded = mt->offsets[0] ? mt : NULL;
		tx->gl = LoadTextureImage(tx, embedded, (size_t)len - offsets[i]);
		if (tx->gl->hd)
			hd++;
	}
	LinkAnimations();
	if (hd)
		Con_Printf("%d HD texture replacement%s\n", hd, hd == 1 ? "" : "s");
	return true;
}

// ---------------------------------------------------------------- surfaces

static int CompareBlock(int a, int b) { return a > b ? a : b; }

typedef struct {
	int heights[LM_PAGE];
} lmpage_alloc_t;
static lmpage_alloc_t *lm_alloc;

// Quake-style skyline packer; opens a new page when the current ones are full
static void AllocLightmap(msurface_t *s)
{
	int w = s->lm_w, h = s->lm_h;
	if (w > LM_PAGE || h > LM_PAGE) {
		Con_Warnf("Lightmap %dx%d is too big, drawing that face unlit\n", w, h);
		s->flags |= SURF_NOLIGHT;
		return;
	}
	for (int page = 0;; page++) {
		if (page == world.numlightmaps) {
			world.numlightmaps++;
			lm_alloc = Mem_Realloc(lm_alloc, sizeof(*lm_alloc) * world.numlightmaps);
			memset(&lm_alloc[page], 0, sizeof(*lm_alloc));
			world.lightmap_pixels = Mem_Realloc(world.lightmap_pixels, sizeof(char *) * world.numlightmaps);
			world.lightmap_pixels[page] = Mem_Alloc(LM_PAGE * LM_PAGE * 4);
		}
		int *heights = lm_alloc[page].heights;
		int best = LM_PAGE, bestx = -1;
		for (int i = 0; i <= LM_PAGE - w; i++) {
			int best2 = 0, j;
			for (j = 0; j < w; j++) {
				if (heights[i + j] >= best)
					break;
				best2 = CompareBlock(best2, heights[i + j]);
			}
			if (j == w) {
				bestx = i;
				best = best2;
			}
		}
		if (bestx < 0 || best + h > LM_PAGE)
			continue;
		for (int i = 0; i < w; i++)
			heights[bestx + i] = best + h;
		s->lm_page = page;
		s->lm_x = bestx;
		s->lm_y = best;
		return;
	}
}

static qboolean LoadFaces(void)
{
	int nverts, nedges, nsurfedges, nfaces, ntexinfo;
	float   (*verts)[3] = LumpData(LUMP_VERTEXES, sizeof(float) * 3, &nverts);
	wedge_t  *edges = GetEdges(&nedges);
	int      *surfedges = LumpData(LUMP_SURFEDGES, sizeof(int), &nsurfedges);
	wface_t  *faces = GetFaces(&nfaces);
	dtexinfo_t *ti = LumpData(LUMP_TEXINFO, sizeof(dtexinfo_t), &ntexinfo);
	if (load_error)
		return false;

	world.numtexinfo = ntexinfo;
	world.texinfo = Mem_Alloc(sizeof(mtexinfo_t) * (ntexinfo ? ntexinfo : 1));
	for (int i = 0; i < ntexinfo; i++) {
		memcpy(world.texinfo[i].vecs, ti[i].vecs, sizeof(ti[i].vecs));
		world.texinfo[i].flags = ti[i].flags;
		int m = ti[i].miptex;
		texture_t *tx = (m >= 0 && m < world.numtextures) ? world.textures[m] : NULL;
		if (!tx) {
			// a face pointing at a missing texture still draws (with the checkerboard)
			static texture_t notex = { "*notexture", 16, 16 };
			notex.gl = GL_MissingTexture();
			tx = &notex;
		}
		world.texinfo[i].texture = tx;
	}

	// count vertices first so the array is allocated once
	int total = 0;
	for (int i = 0; i < nfaces; i++)
		if (faces[i].numedges >= 3)
			total += faces[i].numedges;
	world.verts = Mem_Alloc(sizeof(worldvert_t) * (total ? total : 1));
	world.numsurfaces = nfaces;
	world.surfaces = Mem_Alloc(sizeof(msurface_t) * (nfaces ? nfaces : 1));

	for (int i = 0; i < nfaces; i++) {
		wface_t *f = &faces[i];
		msurface_t *s = &world.surfaces[i];
		if (f->planenum < 0 || f->planenum >= world.numplanes || f->texinfo < 0 || f->texinfo >= ntexinfo || f->numedges < 3 ||
			f->firstedge < 0 || f->firstedge + f->numedges > nsurfedges) {
			s->numverts = 0;
			continue;
		}
		s->plane = &world.planes[f->planenum];
		if (f->side)
			s->flags |= SURF_PLANEBACK;
		s->texinfo = &world.texinfo[f->texinfo];
		memcpy(s->styles, f->styles, 4);
		texture_t *tx = s->texinfo->texture;
		if (tx->kind == TEXKIND_SKY)
			s->flags |= SURF_SKY;
		if (tx->kind == TEXKIND_WATER)
			s->flags |= SURF_WATER;
		if (s->texinfo->flags & TEX_SPECIAL)
			s->flags |= SURF_NOLIGHT;

		// polygon vertices, and texture extents for the lightmap
		s->firstvert = world.numverts;
		s->numverts = f->numedges;
		double mins[2] = { 1e30, 1e30 }, maxs[2] = { -1e30, -1e30 };
		for (int j = 0; j < f->numedges; j++) {
			int e = surfedges[f->firstedge + j];
			unsigned v;
			if (e >= 0)
				v = (e < nedges) ? edges[e].v[0] : 0;
			else
				v = (-e < nedges) ? edges[-e].v[1] : 0;
			if (v >= (unsigned)nverts)
				v = 0;
			worldvert_t *wv = &world.verts[world.numverts++];
			VectorCopy(verts[v], wv->xyz);
			for (int k = 0; k < 2; k++) {
				const float *vec = s->texinfo->vecs[k];
				double val = (double)wv->xyz[0] * vec[0] + (double)wv->xyz[1] * vec[1] +
				             (double)wv->xyz[2] * vec[2] + vec[3];
				if (val < mins[k]) mins[k] = val;
				if (val > maxs[k]) maxs[k] = val;
			}
		}
		for (int k = 0; k < 2; k++) {
			int bmin = (int)floor(mins[k] / 16.0);
			int bmax = (int)ceil(maxs[k] / 16.0);
			s->texturemins[k] = bmin * 16;
			s->extents[k] = (bmax - bmin) * 16;
		}

		if (!(s->flags & SURF_NOLIGHT)) {
			s->lm_w = (s->extents[0] >> 4) + 1;
			s->lm_h = (s->extents[1] >> 4) + 1;
			if (f->lightofs >= 0 && world.lightdata) {
				int styles = 0;
				while (styles < 4 && s->styles[styles] != 255)
					styles++;
				size_t need = (size_t)s->lm_w * s->lm_h * 3 * (styles ? styles : 1);
				if ((size_t)f->lightofs + need <= world.lightsize)
					s->samples = world.lightdata + f->lightofs;
			}
			AllocLightmap(s);
		}

		// final texture and lightmap coordinates
		for (int j = 0; j < s->numverts; j++) {
			worldvert_t *wv = &world.verts[s->firstvert + j];
			float sv = DotProduct(wv->xyz, s->texinfo->vecs[0]) + s->texinfo->vecs[0][3];
			float tv = DotProduct(wv->xyz, s->texinfo->vecs[1]) + s->texinfo->vecs[1][3];
			wv->st[0] = sv / tx->width;
			wv->st[1] = tv / tx->height;
			if (!(s->flags & SURF_NOLIGHT)) {
				wv->lm[0] = (sv - s->texturemins[0] + s->lm_x * 16 + 8) / (LM_PAGE * 16.0f);
				wv->lm[1] = (tv - s->texturemins[1] + s->lm_y * 16 + 8) / (LM_PAGE * 16.0f);
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------- lightmaps

unsigned char lightgammatable[1024];   // also used to light models (r_light.c)
static cvar_t *r_lightgamma;

static void BuildLightGamma(void)
{
	float g = r_lightgamma->value > 0.1f ? r_lightgamma->value : 1.0f;
	for (int i = 0; i < 1024; i++) {
		double v = 255.0 * pow((i > 255 ? 255 : i) / 255.0, 1.0 / g);
		// above the normal range (bright light styles) keep scaling linearly
		if (i > 255)
			v *= i / 255.0;
		lightgammatable[i] = (unsigned char)(v > 255 ? 255 : v);
	}
}

void BSP_BuildSurfaceLightmap(msurface_t *s, unsigned char *dest, int stride)
{
	int size = s->lm_w * s->lm_h;
	static int *blocks;
	static int blocks_cap;
	if (size * 3 > blocks_cap) {
		blocks_cap = size * 3;
		blocks = Mem_Realloc(blocks, sizeof(int) * blocks_cap);
	}

	if (!s->samples) {
		// maps compiled without light are fully bright; unlit faces in lit maps are black
		int v = world.lightdata ? 0 : 255;
		for (int i = 0; i < size * 3; i++)
			blocks[i] = v * 256;
	} else {
		memset(blocks, 0, sizeof(int) * size * 3);
		const unsigned char *lm = s->samples;
		for (int m = 0; m < 4 && s->styles[m] != 255; m++) {
			int scale = lightstyle_value[s->styles[m] & 63];
			s->cached_light[m] = scale;
			for (int i = 0; i < size * 3; i++)
				blocks[i] += lm[i] * scale;
			lm += size * 3;
		}
	}
	if (s->dlightframe == r_refdef.framecount)
		R_SurfaceDlights(s, blocks);

	const int *b = blocks;
	for (int y = 0; y < s->lm_h; y++) {
		unsigned char *d = dest + y * stride;
		for (int x = 0; x < s->lm_w; x++, b += 3, d += 4) {
			for (int c = 0; c < 3; c++) {
				int v = b[c] >> 8;
				d[c] = lightgammatable[v > 1023 ? 1023 : v];
			}
			d[3] = 255;
		}
	}
}

static void BuildLightmaps(void)
{
	BuildLightGamma();
	for (int i = 0; i < world.numsurfaces; i++) {
		msurface_t *s = &world.surfaces[i];
		if ((s->flags & SURF_NOLIGHT) || !s->numverts)
			continue;
		unsigned char *page = world.lightmap_pixels[s->lm_page];
		BSP_BuildSurfaceLightmap(s, page + (s->lm_y * LM_PAGE + s->lm_x) * 4, LM_PAGE * 4);
	}
	world.lightmaps = Mem_Alloc(sizeof(GLuint) * (world.numlightmaps ? world.numlightmaps : 1));
	glGenTextures(world.numlightmaps, world.lightmaps);
	for (int i = 0; i < world.numlightmaps; i++) {
		glBindTexture(GL_TEXTURE_2D, world.lightmaps[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, LM_PAGE, LM_PAGE, 0, GL_RGBA, GL_UNSIGNED_BYTE, world.lightmap_pixels[i]);
	}
	Mem_Free(lm_alloc);
	lm_alloc = NULL;
}

// ---------------------------------------------------------------- tree

static void SetNodeParent(mnode_t *node, mnode_t *parent, int depth)
{
	// iterative on the leaf side keeps deep trees from overflowing the stack
	while (node) {
		node->parent = parent;
		if (node->contents < 0 || depth > 4096)
			return;
		SetNodeParent(node->children[0], node, depth + 1);
		parent = node;
		node = node->children[1];
		depth++;
	}
}

static qboolean LoadTree(void)
{
	int nplanes, nnodes, nleafs, nmark, nvis, nmodels;
	dplane_t *planes = LumpData(LUMP_PLANES, sizeof(dplane_t), &nplanes);
	if (load_error)
		return false;
	world.numplanes = nplanes;
	world.planes = Mem_Alloc(sizeof(mplane_t) * (nplanes ? nplanes : 1));
	for (int i = 0; i < nplanes; i++) {
		mplane_t *p = &world.planes[i];
		VectorCopy(planes[i].normal, p->normal);
		p->dist = planes[i].dist;
		p->type = planes[i].type;
		p->signbits = 0;
		for (int j = 0; j < 3; j++)
			if (p->normal[j] < 0)
				p->signbits |= 1 << j;
	}

	unsigned char *vis = LumpData(LUMP_VISIBILITY, 0, &nvis);
	if (nvis > 0) {
		world.vissize = (size_t)nvis;
		world.visdata = Mem_Alloc(world.vissize);
		memcpy(world.visdata, vis, world.vissize);
	}
	int nlight;
	unsigned char *light = LumpData(LUMP_LIGHTING, 0, &nlight);
	if (nlight > 0) {
		world.lightsize = (size_t)nlight;
		world.lightdata = Mem_Alloc(world.lightsize);
		memcpy(world.lightdata, light, world.lightsize);
	}
	if (load_error)
		return false;

	if (!LoadTextures() || !LoadFaces())
		return false;

	int *mark = GetMarkSurfaces(&nmark);
	wleaf_t *leafs = GetLeafs(&nleafs);
	wnode_t *nodes = GetNodes(&nnodes);
	dmodel_t *models = LumpData(LUMP_MODELS, sizeof(dmodel_t), &nmodels);
	if (load_error)
		return false;
	if (!nleafs || !nmodels) {
		load_error = "map has no leafs or models";
		return false;
	}

	world.nummarksurfaces = nmark;
	world.marksurfaces = Mem_Alloc(sizeof(msurface_t *) * (nmark ? nmark : 1));
	for (int i = 0; i < nmark; i++)
		world.marksurfaces[i] = mark[i] >= 0 && mark[i] < world.numsurfaces ? &world.surfaces[mark[i]] : &world.surfaces[0];

	world.numleafs = nleafs;
	world.leafs = Mem_Alloc(sizeof(mnode_t) * nleafs);
	for (int i = 0; i < nleafs; i++) {
		mnode_t *l = &world.leafs[i];
		l->contents = leafs[i].contents < 0 ? leafs[i].contents : CONTENTS_EMPTY;
		for (int j = 0; j < 3; j++) {
			l->mins[j] = leafs[i].mins[j];
			l->maxs[j] = leafs[i].maxs[j];
		}
		int first = leafs[i].firstmarksurface, n = leafs[i].nummarksurfaces;
		if (first < 0 || n < 0)
			first = n = 0;
		if (first + n > nmark)
			n = first < nmark ? nmark - first : 0;
		l->marksurfaces = world.marksurfaces + (first < nmark ? first : 0);
		l->nummarksurfaces = n;
		l->compressed_vis = (leafs[i].visofs >= 0 && (size_t)leafs[i].visofs < world.vissize)
			? world.visdata + leafs[i].visofs : NULL;
	}

	world.numnodes = nnodes;
	world.nodes = Mem_Alloc(sizeof(mnode_t) * (nnodes ? nnodes : 1));
	for (int i = 0; i < nnodes; i++) {
		mnode_t *n = &world.nodes[i];
		n->contents = 0;
		for (int j = 0; j < 3; j++) {
			n->mins[j] = nodes[i].mins[j];
			n->maxs[j] = nodes[i].maxs[j];
		}
		n->plane = (nodes[i].planenum >= 0 && nodes[i].planenum < nplanes) ? &world.planes[nodes[i].planenum] : &world.planes[0];
		n->firstsurface = nodes[i].firstface;
		n->numsurfaces = nodes[i].numfaces;
		if (n->firstsurface < 0 || n->numsurfaces < 0 || n->firstsurface + n->numsurfaces > world.numsurfaces)
			n->firstsurface = n->numsurfaces = 0;
		for (int j = 0; j < 2; j++) {
			int c = nodes[i].children[j];
			if (c >= 0)
				n->children[j] = c < nnodes ? &world.nodes[c] : &world.leafs[0];
			else
				n->children[j] = (-1 - c) < nleafs ? &world.leafs[-1 - c] : &world.leafs[0];
		}
	}
	if (nnodes)
		SetNodeParent(&world.nodes[0], NULL, 0);

	// collision hulls: 1-3 from the clipnodes lump, 0 from the render nodes
	int nclip;
	wclipnode_t *clip = GetClipnodes(&nclip);
	if (load_error)
		return false;
	world.numclipnodes = nclip;
	world.clipnodes = Mem_Alloc(sizeof(mclipnode_t) * (nclip ? nclip : 1));
	for (int i = 0; i < nclip; i++) {
		mclipnode_t *c = &world.clipnodes[i];
		c->planenum = (clip[i].planenum >= 0 && clip[i].planenum < nplanes) ? clip[i].planenum : 0;
		for (int j = 0; j < 2; j++) {
			int ch = clip[i].children[j];
			c->children[j] = (ch >= nclip || ch < -15) ? CONTENTS_SOLID : ch;
		}
	}
	world.clipnodes0 = Mem_Alloc(sizeof(mclipnode_t) * (nnodes ? nnodes : 1));
	for (int i = 0; i < nnodes; i++) {
		mclipnode_t *c = &world.clipnodes0[i];
		c->planenum = (int)(world.nodes[i].plane - world.planes);
		for (int j = 0; j < 2; j++) {
			mnode_t *ch = world.nodes[i].children[j];
			c->children[j] = ch->contents < 0 ? ch->contents : (int)(ch - world.nodes);
		}
	}

	world.numsubmodels = nmodels;
	world.submodels = Mem_Alloc(sizeof(msubmodel_t) * nmodels);
	for (int i = 0; i < nmodels; i++) {
		msubmodel_t *m = &world.submodels[i];
		VectorCopy(models[i].mins, m->mins);
		VectorCopy(models[i].maxs, m->maxs);
		VectorCopy(models[i].origin, m->origin);
		m->headnode = models[i].headnode[0];
		for (int j = 0; j < 4; j++)
			m->headnodes[j] = models[i].headnode[j];
		m->visleafs = models[i].visleafs;
		m->firstface = models[i].firstface;
		m->numfaces = models[i].numfaces;
		if (m->firstface < 0 || m->firstface + m->numfaces > world.numsurfaces)
			m->numfaces = 0;
	}
	return true;
}

// ---------------------------------------------------------------- queries

mnode_t *BSP_PointInLeaf(const vec3_t p)
{
	if (!world.numnodes)
		return &world.leafs[0];
	mnode_t *node = &world.nodes[0];
	while (node->contents == 0) {
		mplane_t *pl = node->plane;
		float d = DotProduct(p, pl->normal) - pl->dist;
		node = node->children[d > 0 ? 0 : 1];
	}
	return node;
}

const unsigned char *BSP_LeafPVS(const mnode_t *leaf)
{
	static unsigned char *pvs;
	static int pvs_size;
	int row = (world.submodels[0].visleafs + 7) >> 3;
	if (row + 4 > pvs_size) {
		pvs_size = row + 4;
		pvs = Mem_Realloc(pvs, pvs_size);
	}
	const unsigned char *in = leaf->compressed_vis;
	if (!in || leaf == &world.leafs[0]) {
		memset(pvs, 0xff, row);
		return pvs;
	}
	const unsigned char *end = world.visdata + world.vissize;
	unsigned char *out = pvs;
	while (out < pvs + row && in < end) {
		if (*in) {
			*out++ = *in++;
			continue;
		}
		int c = in + 1 < end ? in[1] : 0;
		in += 2;
		while (c-- && out < pvs + row)
			*out++ = 0;
	}
	while (out < pvs + row)
		*out++ = 0;
	return pvs;
}

// ---------------------------------------------------------------- load / free

static void LoadSky(void)
{
	static const char *suffix[6] = { "rt", "bk", "lf", "ft", "up", "dn" };
	const char *name = world.numentities ? BSP_ValueForKey(&world.entities[0], "skyname") : "";
	Q_strncpy(world.skyname, *name ? name : "desert", sizeof(world.skyname));
	for (int i = 0; i < 6; i++) {
		int w, h;
		const char *face = va("%s%s", world.skyname, suffix[i]);
		unsigned char *rgba = Image_LoadAny("gfx/env", face, &w, &h);
		if (!rgba) {
			Con_Warnf("Sky image gfx/env/%s not found\n", face);
			world.sky[i] = GL_MissingTexture();
			continue;
		}
		world.sky[i] = GL_UploadTexture(face, rgba, w, h, TF_CLAMP | TF_WORLD);
		Mem_Free(rgba);
	}
}

static void SetupLightStyles(void)
{
	// GoldSrc's default animated styles (hl.dll sets the same when it runs)
	static const char *styles[] = {
		"m", "mmnmmommommnonmmonqnmmo", "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
		"mmmmmaaaaammmmmaaaaaabcdefgabcdefg", "mamamamamama", "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
		"nmonqnmomnmomomno", "mmmaaaabcdefgmmmmaaaammmaamm", "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
		"aaaaaaaazzzzzzzz", "mmamammmmammamamaaamammma", "abcdefghijklmnopqrrqponmlkjihgfedcba",
		"mmnnmmnnnmmnn",
	};
	for (int i = 0; i < 64; i++)
		R_SetLightStyle(i, i < (int)(sizeof(styles) / sizeof(styles[0])) ? styles[i] : "m");
	// switchable lights (styles 32+) that start off
	for (int i = 0; i < world.numentities; i++) {
		bspentity_t *e = &world.entities[i];
		if (strnicmp(BSP_ValueForKey(e, "classname"), "light", 5))
			continue;
		int style = atoi(BSP_ValueForKey(e, "style"));
		if (style >= 32 && style < 64 && (atoi(BSP_ValueForKey(e, "spawnflags")) & 1))
			R_SetLightStyle(style, "a");
	}
}

void BSP_Free(void)
{
	if (world.vbo)
		qglDeleteBuffers(1, &world.vbo);
	if (world.lightmaps && world.numlightmaps)
		glDeleteTextures(world.numlightmaps, world.lightmaps);
	for (int i = 0; i < world.numlightmaps; i++)
		Mem_Free(world.lightmap_pixels[i]);
	for (int i = 0; i < world.numtextures; i++)
		Mem_Free(world.textures[i]);
	for (int i = 0; i < world.numentities; i++) {
		for (int j = 0; j < world.entities[i].numpairs; j++) {
			Mem_Free(world.entities[i].keys[j]);
			Mem_Free(world.entities[i].values[j]);
		}
		Mem_Free(world.entities[i].keys);
		Mem_Free(world.entities[i].values);
	}
	Mem_Free(world.lightmap_pixels);
	Mem_Free(world.lightmaps);
	Mem_Free(world.textures);
	Mem_Free(world.entities);
	Mem_Free(world.entitytext);
	Mem_Free(world.planes);
	Mem_Free(world.texinfo);
	Mem_Free(world.surfaces);
	Mem_Free(world.nodes);
	Mem_Free(world.leafs);
	Mem_Free(world.marksurfaces);
	Mem_Free(world.submodels);
	Mem_Free(world.clipnodes);
	Mem_Free(world.clipnodes0);
	Mem_Free(world.verts);
	Mem_Free(world.visdata);
	Mem_Free(world.lightdata);
	GL_FreeTextures(TF_WORLD);
	Wad_FreeAll();
	memset(&world, 0, sizeof(world));
}

qboolean BSP_Load(const char *mapname)
{
	if (!r_lightgamma)
		r_lightgamma = Cvar_Get("r_lightgamma", "1.6", FCVAR_ARCHIVE, "lightmap brightness curve (higher is brighter; applies on map load)");

	char path[MAX_OSPATH];
	snprintf(path, sizeof(path), "maps/%s.bsp", mapname);
	size_t size;
	unsigned char *data = FS_LoadFile(path, &size);
	if (!data) {
		Con_Errorf("Map %s not found\n", path);
		return false;
	}
	if (size < sizeof(dheader_t) && !(size >= 12 && *(int *)data == NBSP_IDENT)) {
		Mem_Free(data);
		Con_Errorf("%s is too small to be a map\n", path);
		return false;
	}

	BSP_Free();
	double start = Sys_FloatTime();
	file_base = data;
	file_size = size;
	load_error = NULL;

	const int *h = (const int *)data;
	if (h[0] == NBSP_IDENT) {
		// "NBSP", version, lump count, lumps; lumps past the ones known here are for later features
		if (h[1] != NBSP_VERSION || h[2] < HEADER_LUMPS || 12 + (size_t)h[2] * sizeof(lump_t) > size) {
			Con_Errorf("%s is NBSP version %d with %d lumps; this engine reads version %d\n", path, h[1], h[2], NBSP_VERSION);
			Mem_Free(data);
			return false;
		}
		wide = true;
		memcpy(lumps, data + 12, sizeof(lumps));
	} else if (h[0] == BSPVERSION) {
		wide = false;
		memcpy(lumps, ((dheader_t *)data)->lumps, sizeof(lumps));
	} else {
		Con_Errorf("%s is BSP version %d, expected %d (Half-Life) or NBSP\n", path, h[0], BSPVERSION);
		Mem_Free(data);
		return false;
	}
	// Blue Shift stores the planes and entities lumps the other way round
	if (!wide) {
		lump_t *l0 = &lumps[LUMP_ENTITIES], *l1 = &lumps[LUMP_PLANES];
		if (l0->filelen % sizeof(dplane_t) == 0 && l1->fileofs >= 0 && (size_t)l1->fileofs < size &&
			data[l1->fileofs] == '{' && l0->fileofs >= 0 && (size_t)l0->fileofs < size && data[l0->fileofs] != '{') {
			lump_t tmp = *l0;
			*l0 = *l1;
			*l1 = tmp;
			Con_DPrintf("Blue Shift lump order detected\n");
		}
	}

	Q_strncpy(world.name, path, sizeof(world.name));
	int entlen;
	const char *ents = LumpData(LUMP_ENTITIES, 0, &entlen);
	if (ents) {
		ParseEntities(ents, (size_t)entlen);
		world.entitytext = Mem_Alloc((size_t)entlen + 1);
		memcpy(world.entitytext, ents, (size_t)entlen);
	}
	if (world.numentities)
		Wad_LoadList(BSP_ValueForKey(&world.entities[0], "wad"));

	qboolean ok = LoadTree();
	FreeScratch();
	Mem_Free(data);
	file_base = NULL;
	if (!ok) {
		Con_Errorf("Couldn't load %s: %s\n", path, load_error ? load_error : "unknown error");
		BSP_Free();
		return false;
	}
	SetupLightStyles();
	R_AnimateLightStyles(host_realtime);
	BuildLightmaps();
	LoadSky();
	qglGenBuffers(1, &world.vbo);
	qglBindBuffer(GL_ARRAY_BUFFER, world.vbo);
	qglBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)sizeof(worldvert_t) * world.numverts, world.verts, GL_STATIC_DRAW);
	qglBindBuffer(GL_ARRAY_BUFFER, 0);
	world.loaded = true;

	Con_Printf("Loaded %s in %.0f ms: %d faces, %d leafs, %d textures, %d brush models, %d entities, %d lightmap page%s\n",
		path, (Sys_FloatTime() - start) * 1000, world.numsurfaces, world.numleafs, world.numtextures,
		world.numsubmodels, world.numentities, world.numlightmaps, world.numlightmaps == 1 ? "" : "s");
	return true;
}
