// newen - Source engine content (Half-Life 2's file formats), shared declarations
//
// Written from the formats' public documentation; structures and names here are newen's own.
#pragma once
#include "model.h"

// ---------------------------------------------------------------- files (src_fs.c)
// Source content has two origins, picked by the path:
//   "models/x.mdl"       loose files in Half-Life's own search paths (FS_LoadFile)
//   "hl2:models/x.mdl"   the installed Half-Life 2: its loose files, then its VPK archives
// Nothing without the prefix ever comes from Half-Life 2, because both games have files with the same names.
#define SRC_HL2_PREFIX "hl2:"

void        SrcFS_Init(void);
void        SrcFS_Shutdown(void);
void       *SrcFS_LoadFile(const char *path, size_t *len);   // NUL-terminated, Mem_Free it; NULL if missing
qboolean    SrcFS_FileExists(const char *path);
// "hl2:" when path has the prefix, else "": what a file's companions (materials, .vvd...) are looked up with
const char *SrcFS_Prefix(const char *path);

// ---------------------------------------------------------------- materials (src_material.c)
#define SMAT_ALPHATEST   (1<<0)
#define SMAT_TRANSLUCENT (1<<1)
#define SMAT_ADDITIVE    (1<<2)
#define SMAT_NOCULL      (1<<3)
#define SMAT_SELFILLUM   (1<<4)
#define SMAT_UNLIT       (1<<5)   // UnlitGeneric: drawn at full brightness
#define SMAT_MISSING     (1<<6)   // no .vmt, or its texture can't be loaded: the checkerboard
#define SMAT_NODRAW      (1<<7)   // an effect this renderer doesn't have (refraction): left out

typedef struct {
	char         name[128];      // prefix + lower-case path without "materials/" and ".vmt"
	gltexture_t *gl;             // $basetexture, or the checkerboard
	gltexture_t *normalmap;      // $bumpmap, loaded by SrcMat_LoadBump (modern renderer); may be NULL
	gltexture_t *specmap;        // shine strength, from the bump map's alpha when the material shines; may be NULL
	qboolean     shiny;          // $phong or an environment map masked by the bump map's alpha
	qboolean     base_shine;     // the shine strength is in the base texture's alpha instead
	char         basename[128];  // $basetexture's path
	qboolean     bump_tried;
	int          texflags;       // TF_MODEL or TF_WORLD, as it was loaded
	gltexture_t *iris;           // eyes: $iris, drawn over the eyeball where it looks; NULL for everything else
	int          flags;
	int          width, height;
	char         bumpname[128];  // $bumpmap's texture path, "" if none
} srcmaterial_t;

void           SrcMat_Init(void);
// prefix: "" or "hl2:"; name: path under materials/, with or without .vmt. texflags: TF_MODEL or TF_WORLD.
// Never NULL: a missing material comes back with SMAT_MISSING and the checkerboard.
srcmaterial_t *SrcMat_Find(const char *prefix, const char *name, int texflags);
// Loads the material's bump map (and shine map) the first time it's called; newen's normal maps have +Y up
// the image and Source's have it down, so the green channel is turned over
void           SrcMat_LoadBump(srcmaterial_t *m);
void           SrcMat_Free(int texflags);   // with GL_FreeTextures(texflags): forgets materials using those textures
// Decodes a .vtf's largest image (first frame) to RGBA. Mem_Free it. NULL with a warning if it can't.
unsigned char *VTF_Decode(const char *label, const unsigned char *file, size_t size, int *w, int *h, qboolean *alpha);

// ---------------------------------------------------------------- models (src_studio.c)
#define SRC_HEAD_TURN 60.0f   // degrees a head bone turns either way at controller 0's ends
#define SRC_MDL_MIN_VERSION 44
#define SRC_MDL_MAX_VERSION 48

// one vertex as the .vvd file stores it (48 bytes): positions and normals are in model space, in the
// bind pose, and move with up to three bones
typedef struct {
	float         weight[3];
	unsigned char bone[3], numbones;
	float         pos[3], normal[3], st[2];
} srcvert_t;
_Static_assert(sizeof(srcvert_t) == 48, "srcvert_t must match the .vvd vertex");

typedef struct {
	int       skinref;          // row into the skin table, which gives the material
	int       eyeball;          // which of the submodel's eyeballs this mesh is, or -1
	int       numindices;       // 3 per triangle
	unsigned *indices;          // into srcmodel_t::verts
} srcmesh_t;

// An eye: a ball fixed to a bone (the head). Its mesh is drawn with the white of the eye, then the iris
// texture is laid over it, centred where the eye looks.
typedef struct {
	int   bone;
	float org[3];               // centre, in the bone's space
	float up[3], forward[3];    // which way is up and straight ahead for this eye, in the bone's space
	float zoffset;              // how far the iris sits off straight ahead (eyes aren't quite parallel)
	float radius;
	float iris_scale;           // iris texture widths per unit
} srceyeball_t;

typedef struct {
	char          name[64];
	int           nummeshes;
	srcmesh_t    *meshes;
	int           numeyeballs;
	srceyeball_t *eyeballs;
} srcsubmodel_t;

typedef struct {
	char           name[64];
	int            nummodels;
	int            base;        // body value divisor for this part, as in GoldSrc
	srcsubmodel_t *models;
} srcbodypart_t;

typedef struct {
	char  name[64];
	int   parent;               // -1 = root; always a lower number than the bone itself
	float pos[3], quat[4];      // rest pose, relative to the parent
	float rot[3];               // the same rotation as angles (radians), which animation values add to
	float posscale[3], rotscale[3];   // multiply the compressed animation values
	float posetobone[3][4];     // model space (where the vertices are) to this bone's space, in the rest pose
} srcbone_t;

typedef struct {
	char   name[64];
	float  fps;
	int    flags;               // SRCANIM_*
	int    numframes;
	size_t ofs;                 // where its description is in the .mdl; data offsets count from there
	int    block, index;        // the data: block 0 is the .mdl itself, others are in the .ani file
	int    sectionofs, sectionframes;   // long animations are cut into sections of this many frames
	float  movement[3];         // how far the whole animation carries the model (walking); the pose stays in place
} srcanim_t;
#define SRCANIM_DELTA    0x0004   // adds to a pose instead of being one
#define SRCANIM_ALLZEROS 0x0020   // no data: the rest pose

typedef struct {
	char   label[64], activity[64];
	int    flags;               // SRCSEQ_*
	int    actweight;
	float  bbmin[3], bbmax[3];
	int    groupsize[2];        // a grid of animations to blend between, usually 1x1
	const short *anims;         // groupsize[0] x groupsize[1] animation numbers (in the .mdl)
	const float *weights;       // per bone, 0 = this sequence leaves the bone alone (in the .mdl); may be NULL
	int    anim;                // the grid's middle animation, which gives the timing; -1 = none
	int    paramindex[2];       // which of the model's own pose parameters picks along each side of the grid; -1 = none
	float  paramstart[2], paramend[2];   // the parameter values at the grid's first and last animation
	const float *posekeys;      // or the value at each animation: groupsize[0] then groupsize[1] of them; may be NULL
	int    numlayers;           // other sequences laid over this one while it plays (aiming, mostly)
	const unsigned char *layers;   // numlayers x 24 bytes in the .mdl: sequence, flags, start, peak, tail, end
	int    numevents;
	const unsigned char *events;   // numevents x SRC_EVENT_SIZE bytes in the .mdl: cycle, number, type, options[64], name
	float  fps;                 // that animation's speed and length
	int    numframes;
} srcseq_t;
#define SRC_EVENT_SIZE  80
#define SRC_EVENT_NAMED (1<<10)  // type flag: the event has a name instead of a number
#define SRCSEQ_LOOPING  0x0001
#define SRCSEQ_OVERRIDE 0x1000   // a placeholder: the same-named sequence of an include file replaces it

// A named dial (aim_pitch, move_yaw...) that picks where in a sequence's grid of animations to blend
typedef struct {
	char  name[64];
	float start, end;           // its range, in its own units (degrees mostly)
	float value;                // where it rests, 0..1 across the range: at 0 units when the range holds it
} srcposeparam_t;

// Another .mdl that holds animations for this skeleton ($includemodel), with its numbering mapped to ours
typedef struct {
	struct srcmodel_s *model;   // owned by the include cache, shared between models
	int   *bonemap;             // its bone number to ours, -1 = we have no such bone
	int   *posemap;             // its pose parameter number to ours
} srcinclude_t;

// One entry of the model's full sequence list: its own sequences, then the includes'
typedef struct {
	int include;                // -1 = the model itself
	int local;                  // the sequence number in that file
} srcvseq_t;

typedef struct {
	int  vseq;                  // which entry of the model's full sequence list plays
	int  activity;              // the Half-Life activity it answers, or 0
	char label[32];             // its name in the stand-in ("" = the sequence's own)
} srcalias_t;

typedef struct {
	unsigned vert;              // into srcmodel_t::verts
	float    delta[3];          // model space, rest pose
} srcmorph_t;

typedef struct srcmodel_s {
	unsigned char  *mdl;        // the whole .mdl file (bones and animation are read from it)
	size_t          mdlsize;
	int             version;
	char            prefix[8];  // "" or "hl2:": where its companions and materials come from
	int             numverts;
	srcvert_t      *verts;
	float         (*tangents)[4];   // per vertex, from the .vvd: the direction along the texture; NULL if it has none
	int             numbodyparts;
	srcbodypart_t  *bodyparts;
	int             numtextures;
	srcmaterial_t **materials;  // per texture; never NULL entries
	int             nummissing; // materials that fell back to the checkerboard
	int             numskinref, numskinfamilies;
	short          *skins;      // numskinfamilies x numskinref texture numbers
	int             numtris;    // LOD 0, every body part choice
	int             numbones;   // at least 1
	srcbone_t      *bones;
	int             numanims;
	srcanim_t      *anims;
	int             numseqs;
	srcseq_t       *seqs;
	unsigned char  *ani;        // the .ani file with the animation blocks, or NULL
	size_t          anisize;
	int             numlocalpose;   // the file's own pose parameters
	srcposeparam_t *localpose;
	// the model as a whole, with its include files (drawable models only; SrcAnim_Link)
	int             numincludes;
	srcinclude_t   *includes;
	int             numvseqs;   // what sequence numbers mean for this model
	srcvseq_t      *vseqs;
	int             numpose;    // own pose parameters plus the includes' new ones
	srcposeparam_t *pose;
	// stand-in sequences past numvseqs: extra entries that give a Half-Life activity or a sequence name the
	// model lacks one of its own sequences to play
	int             numaliases;
	srcalias_t     *aliases;
	// what the game's head-turn controllers (0-3) and aim blends (0-1) drive: a pose parameter, or -1
	int             ctrlpose[4], blendpose[2];
	qboolean        ctrlrev[4], blendrev[2];   // turned the other way round
	// settings file "order": the stand-in's first sequences, for code that plays sequences by number (the
	// client plays a weapon's animations that way). Then come the model's own list and the aliases.
	int             numorder;
	int            *order;      // entries of the model's full sequence list
	float           viewfov;    // settings file: the field of view it's drawn with as a view model; 0 = the cvar
	// the head bone the game's controller 0 turns left and right (as GoldSrc heads do), or -1
	int             headbone;
	qboolean        headrev;
	// talking: the vertices the open jaw moves, and how far at fully open (the "jaw_drop" flex)
	int             nummorphs;
	srcmorph_t     *morphs;
	// or a jaw bone turned about one of its axes (settings file)
	int             jawbone, jawaxis;
	float           jawdegrees;
} srcmodel_t;

// Loads a Source model into m. path is the .mdl's file (its companions sit beside it); it differs from
// m->name when the model stands in for another (mdl_replace). Keeps data on success.
qboolean SrcStudio_Load(model_t *m, const char *path, unsigned char *data, size_t size);
void     SrcStudio_Free(model_t *m);
void     SrcStudio_PrintInfo(const model_t *m);
// Which entry of the model's full sequence list stand-in sequence i plays (0 if out of range), and through
// *alias its alias entry if it is one (else NULL)
int      SrcStudio_StandInSeq(const srcmodel_t *s, int i, const srcalias_t **alias);

// ---------------------------------------------------------------- animation (src_anim.c)
// Reads the bones, animations and sequences out of s->mdl (and loads the .ani file). False if a table is damaged.
qboolean SrcAnim_Load(const char *name, srcmodel_t *s);
// After SrcAnim_Load, for a model that is drawn: loads its include files and builds the full sequence list
void     SrcAnim_Link(const char *name, srcmodel_t *s);
void     SrcAnim_Free(srcmodel_t *s);
void     SrcAnim_FreeIncludes(void);   // the shared include files; after every model using them is freed
// Sequence i of the model's full list, in whichever file holds it; NULL if out of range
const srcseq_t *SrcAnim_Seq(const srcmodel_t *s, int i);
const srcanim_t *SrcAnim_SeqAnim(const srcmodel_t *s, int i);   // its timing animation; NULL if it has none
// Each bone's rotation (quaternion) and position relative to its parent, for a sequence at cycle 0..1.
// pose: s->numpose values 0..1, or NULL for each parameter's resting value. A sequence out of range gives
// the rest pose. q and pos hold s->numbones entries.
void     SrcAnim_CalcPose(const srcmodel_t *s, int sequence, float cycle, const float *pose, float (*q)[4], vec3_t *pos);
