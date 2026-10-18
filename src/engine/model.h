// newen - models: studio (.mdl v10), sprites (.spr v2) and the renderer's entity description
//
// The on-disk structures are written from the documented GoldSrc formats (field order and sizes
// matter, everything is little-endian, offsets are from the start of the file).
#pragma once
#include "gl_local.h"

// ---------------------------------------------------------------- studio file format

#define IDSTUDIOHEADER   (('T' << 24) | ('S' << 16) | ('D' << 8) | 'I')   // "IDST"
#define IDSEQGRPHEADER   (('Q' << 24) | ('S' << 16) | ('D' << 8) | 'I')   // "IDSQ"
#define STUDIO_VERSION   10

// texture flags
#define STUDIO_NF_FLATSHADE  0x0001
#define STUDIO_NF_CHROME     0x0002
#define STUDIO_NF_FULLBRIGHT 0x0004
#define STUDIO_NF_NOMIPS     0x0008
#define STUDIO_NF_ALPHA      0x0010
#define STUDIO_NF_ADDITIVE   0x0020
#define STUDIO_NF_MASKED     0x0040

// sequence flags and motion types
#define STUDIO_LOOPING 0x0001
#define STUDIO_X       0x0001
#define STUDIO_Y       0x0002
#define STUDIO_Z       0x0004
#define STUDIO_XR      0x0008
#define STUDIO_YR      0x0010
#define STUDIO_ZR      0x0020
#define STUDIO_TYPES   0x7fff
#define STUDIO_RLOOP   0x8000

typedef struct {
	int   id, version;
	char  name[64];
	int   length;
	float eyeposition[3];
	float min[3], max[3];        // ideal movement hull
	float bbmin[3], bbmax[3];    // clipping box
	int   flags;
	int   numbones, boneindex;
	int   numbonecontrollers, bonecontrollerindex;
	int   numhitboxes, hitboxindex;
	int   numseq, seqindex;
	int   numseqgroups, seqgroupindex;
	int   numtextures, textureindex, texturedataindex;
	int   numskinref, numskinfamilies, skinindex;
	int   numbodyparts, bodypartindex;
	int   numattachments, attachmentindex;
	int   soundtable, soundindex, soundgroups, soundgroupindex;
	int   numtransitions, transitionindex;
} studiohdr_t;

typedef struct {
	int  id, version;
	char name[64];
	int  length;
} studioseqhdr_t;

typedef struct {
	char  name[32];
	int   parent;               // -1 = root
	int   flags;
	int   bonecontroller[6];    // -1 = none
	float value[6];             // default position (x y z) and rotation (x y z, radians)
	float scale[6];             // multiplies the compressed animation values
} mstudiobone_t;

typedef struct {
	int   bone;                 // -1 = none
	int   type;                 // STUDIO_X..STUDIO_ZR, optionally | STUDIO_RLOOP
	float start, end;
	int   rest;                 // byte value at rest
	int   index;                // 0-3 = user controllers, 4 = mouth
} mstudiobonecontroller_t;

typedef struct {
	int   bone, group;
	float bbmin[3], bbmax[3];
} mstudiobbox_t;

typedef struct {
	char label[32];
	char name[64];              // file name for groups other than 0
	int  unused1, unused2;      // a cache pointer and a data offset in the original engine
} mstudioseqgroup_t;

typedef struct {
	char  label[32];
	float fps;
	int   flags;
	int   activity, actweight;
	int   numevents, eventindex;
	int   numframes;
	int   numpivots, pivotindex;
	int   motiontype, motionbone;
	float linearmovement[3];
	int   automoveposindex, automoveangleindex;
	float bbmin[3], bbmax[3];
	int   numblends;
	int   animindex;            // mstudioanim_t[numblends][numbones], in the sequence group's file
	int   blendtype[2];
	float blendstart[2], blendend[2];
	int   blendparent;
	int   seqgroup;
	int   entrynode, exitnode, nodeflags;
	int   nextseq;
} mstudioseqdesc_t;

typedef struct {
	int  frame, event, type;
	char options[64];
} mstudioevent_t;

typedef struct {
	unsigned short offset[6];   // from this struct to the run-length values; 0 = the bone's default
} mstudioanim_t;

// run-length encoded animation values: a header {valid, total} followed by "valid" shorts,
// the last of which repeats until "total" frames are covered
typedef union {
	struct { unsigned char valid, total; } num;
	short value;
} mstudioanimvalue_t;

typedef struct {
	char  name[32];
	int   type, bone;
	float org[3];
	float vectors[3][3];
} mstudioattachment_t;

typedef struct {
	char name[64];
	int  nummodels;
	int  base;                  // body value divisor for this part
	int  modelindex;
} mstudiobodyparts_t;

typedef struct {
	char name[64];
	int  flags;
	int  width, height;
	int  index;                 // 8-bit pixels followed by a 256-color palette
} mstudiotexture_t;

typedef struct {
	char  name[64];
	int   type;
	float boundingradius;
	int   nummesh, meshindex;
	int   numverts, vertinfoindex, vertindex;   // vertinfo = bone per vertex
	int   numnorms, norminfoindex, normindex;
	int   numgroups, groupindex;
} mstudiomodel_t;

typedef struct {
	int numtris, triindex;      // triangle commands: count (>0 strip, <0 fan), then count x {vert, norm, s, t}
	int skinref;
	int numnorms, normindex;
} mstudiomesh_t;

// ---------------------------------------------------------------- sprite file format

#define IDSPRITEHEADER (('P' << 24) | ('S' << 16) | ('D' << 8) | 'I')   // "IDSP"
#define SPRITE_VERSION 2

enum { SPR_VP_PARALLEL_UPRIGHT, SPR_FACING_UPRIGHT, SPR_VP_PARALLEL, SPR_ORIENTED, SPR_VP_PARALLEL_ORIENTED };
enum { SPR_NORMAL, SPR_ADDITIVE, SPR_INDEXALPHA, SPR_ALPHTEST };

typedef struct {
	int   ident, version;
	int   type;                 // orientation
	int   texFormat;
	float boundingradius;
	int   width, height;
	int   numframes;
	float beamlength;
	int   synctype;
} dsprite_t;

typedef struct {
	int origin[2];              // left, top
	int width, height;
} dspriteframe_t;

// ---------------------------------------------------------------- loaded models

// GoldSrc's numbering (client.dll reads model_t::type); mod_bad is newen's own
typedef enum { mod_brush, mod_sprite, mod_alias, mod_studio, mod_bad } modtype_t;

// one triangle-list mesh, decoded from the strips and fans at load time
typedef struct {
	int   numtris;
	short *tris;                // numtris * 3 * {vert, norm, s, t}
	int   skinref;
} studiomesh_t;

typedef struct {
	gltexture_t *gl;
	int   flags;
	int   width, height;        // original size; texture coordinates are based on it
} studiotex_t;

typedef struct {
	studiohdr_t *hdr;           // the whole .mdl file
	studiohdr_t *texhdr;        // where textures and skins come from (hdr or the <name>T.mdl file)
	unsigned char **seqdata;    // file data per sequence group (group 0 = hdr)
	size_t       *seqsize;
	studiotex_t  *textures;     // texhdr->numtextures
	studiomesh_t **meshes;      // per studio model (flattened bodypart index), per mesh
	int          numsubmodels;
} studiomodel_t;

typedef struct {
	gltexture_t *gl;
	float left, right, up, down;   // quad corners relative to the origin
} spriteframe_t;

typedef struct {
	int   numframes;              // 1 for a single frame
	float *intervals;             // cumulative end times, for groups
	spriteframe_t *frames;
} spritegroup_t;

typedef struct {
	int   type, texFormat;
	int   maxwidth, maxheight;
	int   numframes;
	spritegroup_t *frames;
} spritemodel_t;

// The first fields match GoldSrc's model_t, because the client DLL reads them directly
typedef struct model_s {
	char      name[64];
	qboolean  needload;
	modtype_t type;
	int       numframes;
	int       synctype;
	int       flags;
	vec3_t    mins, maxs;
	float     radius;
	// newen's own from here on
	int       submodel;           // brush models ("*n")
	union {
		studiomodel_t studio;
		spritemodel_t sprite;
		// brush models: GoldSrc keeps the map's entity text here, and the client DLL reads it
		// (its spectator code looks for info_player_start in the world model's entities)
		struct { unsigned char unused[384 - 116]; char *entities; void *cache; } brush;
	};
} model_t;

_Static_assert(offsetof(model_t, type) == 68 && offsetof(model_t, mins) == 84 && offsetof(model_t, radius) == 108,
	"model_t must start like GoldSrc's");
_Static_assert(offsetof(model_t, brush.entities) == 384, "brush models keep the entity text where GoldSrc does");

void     Mod_Init(void);
model_t *Mod_ForName(const char *name, qboolean crash);   // cached; "*n" for brush submodels
void     Mod_ClearAll(void);                              // on map change: frees every model and its textures
void     Mod_PrintInfo(const model_t *m);

static inline studiohdr_t *Mod_StudioHeader(const model_t *m)
{
	return m && m->type == mod_studio ? m->studio.hdr : NULL;
}

#define STUDIO_PTR(hdr, ofs, type) ((type *)((unsigned char *)(hdr) + (ofs)))

// ---------------------------------------------------------------- what the renderer draws

// render modes and effects use GoldSrc's numbers (entities from hl.dll will set them)
enum { kRenderNormal, kRenderTransColor, kRenderTransTexture, kRenderGlow, kRenderTransAlpha, kRenderTransAdd };

typedef struct {
	model_t *model;
	vec3_t   origin, angles;
	int      sequence;
	float    frame;               // starting frame
	float    framerate;           // 1 = the sequence's own speed (sprites: frames per second)
	double   animtime;            // when frame was set
	int      body, skin;
	unsigned char controller[4], mouth, blending[2];
	int      rendermode, renderamt, renderfx;
	int      rendercolor[3];
	float    scale;
	float    dist;                // for sorting
} rentity_t;

// r_studio.c
void     R_StudioInit(void);
void     R_StudioDrawModel(rentity_t *e);
float    R_StudioFrame(const rentity_t *e);             // current frame of the sequence
void     R_StudioInitControllers(const model_t *m, rentity_t *e);   // every controller to 0 degrees
int      R_StudioLookupSequence(const model_t *m, const char *label);   // -1 if none
qboolean R_StudioCull(const rentity_t *e);
// sets up the bone matrices (3x4, model space to world) for e; returns the bone count
int      R_StudioSetupBones(const rentity_t *e, float (**bones)[3][4]);

// r_sprite.c
void     R_SpriteDrawModel(rentity_t *e);
qboolean R_SpriteCull(const rentity_t *e);

// r_light.c
void     R_LightPoint(const vec3_t p, vec3_t color);    // 0..1+, lightmap light at the floor below p
qboolean R_LineBlocked(const vec3_t start, const vec3_t end);   // true if world geometry is in the way
void     R_DlightPoint(const vec3_t p, vec3_t color);          // adds this frame's dynamic lights near p

// r_ents.c
void     R_InitEntities(void);
void     R_BuildMapEntities(void);
void     R_ClearEntities(void);
void     R_DrawEntities(void);
rentity_t *R_AddEntity(void);   // a new static entity (cleared on map change)
void     R_ClearFrameEntities(void);
rentity_t *R_AddFrameEntity(void);   // rebuilt every frame (server edicts, client entities)
