// newen - loaded BSP world
#pragma once
#include "gl_local.h"

#define CONTENTS_EMPTY  -1
#define CONTENTS_SOLID  -2
#define CONTENTS_WATER  -3
#define CONTENTS_SLIME  -4
#define CONTENTS_LAVA   -5
#define CONTENTS_SKY    -6

#define TEX_SPECIAL 1   // texinfo flag: sky/water, no lightmap

enum { TEXKIND_NORMAL, TEXKIND_SKY, TEXKIND_WATER, TEXKIND_ALPHA };

typedef struct {
	vec3_t normal;
	float  dist;
	int    type;
	int    signbits;
} mplane_t;

typedef struct texture_s {
	char  name[16];
	int   width, height;        // miptex size; texture coordinates are based on it
	gltexture_t *gl;
	int   kind;
	int   index;
	int   anim_total, anim_min, anim_max;   // '+' animated textures, 10 frames per second
	struct texture_s *anim_next;
	struct texture_s *alternate_anims;
} texture_t;

typedef struct {
	float      vecs[2][4];
	texture_t *texture;
	int        flags;
} mtexinfo_t;

typedef struct {
	float xyz[3];
	float st[2];
	float lm[2];
} worldvert_t;

#define SURF_PLANEBACK (1<<0)
#define SURF_SKY       (1<<1)
#define SURF_WATER     (1<<2)
#define SURF_NOLIGHT   (1<<3)

typedef struct msurface_s {
	mplane_t   *plane;
	int         flags;
	mtexinfo_t *texinfo;
	int         firstvert, numverts;   // into world.verts
	int         texturemins[2], extents[2];
	int         lm_w, lm_h, lm_page, lm_x, lm_y;
	unsigned char styles[4];
	unsigned char *samples;            // RGB, lm_w*lm_h per style; NULL = none
	int         cached_light[4];       // style values the uploaded lightmap was built with
	int         visframe;
} msurface_t;

// nodes and leafs share the first fields; contents 0 = node, negative = leaf
typedef struct mnode_s {
	int    contents;
	int    visframe;
	float  mins[3], maxs[3];
	struct mnode_s *parent;
	// node
	mplane_t *plane;
	struct mnode_s *children[2];
	int    firstsurface, numsurfaces;
	// leaf
	unsigned char *compressed_vis;
	msurface_t **marksurfaces;
	int    nummarksurfaces;
} mnode_t;

typedef struct {
	vec3_t mins, maxs, origin;
	int    headnode;
	int    headnodes[4];          // collision: hull 0 indexes world.clipnodes0, hulls 1-3 world.clipnodes
	int    visleafs;
	int    firstface, numfaces;
} msubmodel_t;

typedef struct {
	int    numpairs;
	char **keys;
	char **values;
} bspentity_t;

// collision tree node: children >= 0 are nodes, < 0 are contents
typedef struct {
	int planenum;
	int children[2];
} mclipnode_t;

#define LM_PAGE 1024   // lightmap atlas page size; as many pages as needed

typedef struct {
	char         name[64];        // "maps/c1a0.bsp"
	qboolean     loaded;
	mplane_t    *planes;          int numplanes;
	texture_t  **textures;        int numtextures;
	mtexinfo_t  *texinfo;         int numtexinfo;
	msurface_t  *surfaces;        int numsurfaces;
	mnode_t     *nodes;           int numnodes;
	mnode_t     *leafs;           int numleafs;
	msurface_t **marksurfaces;    int nummarksurfaces;
	msubmodel_t *submodels;       int numsubmodels;
	mclipnode_t *clipnodes;       int numclipnodes;    // hulls 1-3 (player, big monster, crouch)
	mclipnode_t *clipnodes0;                           // hull 0 (points), built from the render nodes
	worldvert_t *verts;           int numverts;
	GLuint       vbo;             // verts on the GPU
	unsigned char *visdata;       size_t vissize;
	unsigned char *lightdata;     size_t lightsize;
	bspentity_t *entities;        int numentities;
	GLuint      *lightmaps;       int numlightmaps;
	unsigned char **lightmap_pixels;   // CPU copy of each page (RGBA)
	char         skyname[64];
	gltexture_t *sky[6];
} world_t;

extern world_t world;

qboolean    BSP_Load(const char *mapname);   // "c1a0"
void        BSP_Free(void);
const char *BSP_ValueForKey(const bspentity_t *ent, const char *key);
mnode_t    *BSP_PointInLeaf(const vec3_t p);
const unsigned char *BSP_LeafPVS(const mnode_t *leaf);   // decompressed; all visible when no vis
void        BSP_BuildSurfaceLightmap(msurface_t *s, unsigned char *dest, int stride);
extern unsigned char lightgammatable[1024];   // summed light (0..1023) to displayed brightness

// lightstyles (r_world.c)
extern int  lightstyle_value[64];
void        R_SetLightStyle(int style, const char *pattern);
void        R_AnimateLightStyles(double time);
// true if any leaf the box touches is in the current view's PVS (r_world.c)
qboolean    R_BoxVisible(const vec3_t mins, const vec3_t maxs);
