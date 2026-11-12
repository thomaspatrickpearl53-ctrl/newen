// newen - Source model animation: bones, animations and sequences out of a .mdl (versions 44-48)
//
// An animation is a chain of per-bone records. Each record holds the bone's rotation and position either as
// one fixed value (packed quaternion, half-float vector) or as run-length encoded values per frame and axis,
// the same scheme GoldSrc uses. Bones without a record stay in the rest pose. Long animations are cut into
// sections, and big models keep their animation data in "blocks" in a separate .ani file.
#include "src_local.h"

enum {
	MDL_NUMBONES = 156, MDL_BONEINDEX = 160, MDL_NUMLOCALANIM = 180, MDL_LOCALANIMINDEX = 184,
	MDL_NUMLOCALSEQ = 188, MDL_LOCALSEQINDEX = 192, MDL_ANIMBLOCKNAME = 348, MDL_NUMANIMBLOCKS = 352,
	MDL_ANIMBLOCKINDEX = 356, MDL_NUMPOSEPARAMS = 300, MDL_POSEPARAMINDEX = 304, MDL_NUMINCLUDES = 336,
	MDL_INCLUDEINDEX = 340,
	POSEPARAM_SIZE = 20,    // name offset, flags, start, end, loop
	INCLUDE_SIZE = 8,       // label offset, file name offset

	BONE_SIZE = 216,
	BONE_NAME = 0, BONE_PARENT = 4, BONE_POS = 32, BONE_QUAT = 44, BONE_ROT = 60, BONE_POSSCALE = 72,
	BONE_ROTSCALE = 84, BONE_POSETOBONE = 96,

	ANIM_SIZE = 100,
	ANIM_NAME = 4, ANIM_FPS = 8, ANIM_FLAGS = 12, ANIM_NUMFRAMES = 16, ANIM_NUMMOVEMENTS = 20,
	ANIM_MOVEMENTINDEX = 24, ANIM_BLOCK = 52, ANIM_INDEX = 56,
	MOVEMENT_SIZE = 44, MOVEMENT_POSITION = 32,   // one stretch of travel; position = where it ends
	ANIM_SECTIONINDEX = 80, ANIM_SECTIONFRAMES = 84,

	SEQ_SIZE = 212,
	SEQ_LABEL = 4, SEQ_ACTIVITYNAME = 8, SEQ_FLAGS = 12, SEQ_ACTWEIGHT = 20, SEQ_NUMEVENTS = 24,
	SEQ_EVENTINDEX = 28, SEQ_BBMIN = 32, SEQ_BBMAX = 44,
	SEQ_ANIMINDEXINDEX = 60, SEQ_GROUPSIZE = 68, SEQ_PARAMINDEX = 76, SEQ_PARAMSTART = 84, SEQ_PARAMEND = 92,
	SEQ_NUMAUTOLAYERS = 148, SEQ_AUTOLAYERINDEX = 152, SEQ_WEIGHTLIST = 156, SEQ_POSEKEYINDEX = 160,
	LAYER_SIZE = 24,        // sequence (16 bits), pose parameter (16 bits), flags, start, peak, tail, end
	LAYER_SPLINE = 0x0040, LAYER_XFADE = 0x0080, LAYER_NOBLEND = 0x0200, LAYER_POSE = 0x4000,

	// per-bone record: bone, flags, offset to the next record (0 = last), then the data
	REC_RAWPOS = 0x01,      // position: three half floats
	REC_RAWROT = 0x02,      // rotation: a quaternion in 48 bits
	REC_ANIMPOS = 0x04,     // position: run-length values per axis
	REC_ANIMROT = 0x08,     // rotation: run-length angle values per axis
	REC_DELTA = 0x10,
	REC_RAWROT2 = 0x20,     // rotation: a quaternion in 64 bits (version 48)
};

static int RdInt(const unsigned char *p)
{
	int v;
	memcpy(&v, p, 4);
	return v;
}

static float RdFloat(const unsigned char *p)
{
	float v;
	memcpy(&v, p, 4);
	return v;
}

static unsigned RdU16(const unsigned char *p)
{
	return p[0] | p[1] << 8;
}

static qboolean Has(size_t size, long long ofs, long long count, size_t elemsize)
{
	if (count == 0)
		return true;
	return ofs >= 0 && count > 0 && (unsigned long long)ofs <= size &&
		(unsigned long long)count <= (size - (size_t)ofs) / elemsize;
}

static void CopyString(const unsigned char *data, size_t size, long long ofs, char *out, size_t outsize)
{
	out[0] = 0;
	if (ofs > 0 && (unsigned long long)ofs < size && memchr(data + ofs, 0, size - (size_t)ofs))
		Q_strncpy(out, (const char *)data + ofs, outsize);
}

// ---------------------------------------------------------------- loading

qboolean SrcAnim_Load(const char *modelname, srcmodel_t *s)
{
	const unsigned char *mdl = s->mdl;
	size_t size = s->mdlsize;
	int nbones = RdInt(mdl + MDL_NUMBONES), nanims = RdInt(mdl + MDL_NUMLOCALANIM), nseqs = RdInt(mdl + MDL_NUMLOCALSEQ);
	long long bones = RdInt(mdl + MDL_BONEINDEX), anims = RdInt(mdl + MDL_LOCALANIMINDEX), seqs = RdInt(mdl + MDL_LOCALSEQINDEX);
	if (!Has(size, bones, nbones, BONE_SIZE) || !Has(size, anims, nanims, ANIM_SIZE) || !Has(size, seqs, nseqs, SEQ_SIZE))
		return false;

	s->numbones = nbones > 0 ? nbones : 1;
	s->bones = Mem_Alloc(sizeof(srcbone_t) * s->numbones);
	if (nbones <= 0) {
		// nothing to hang the vertices on: one bone that does nothing
		srcbone_t *b = &s->bones[0];
		Q_strncpy(b->name, "root", sizeof(b->name));
		b->parent = -1;
		b->quat[3] = 1;
		b->posetobone[0][0] = b->posetobone[1][1] = b->posetobone[2][2] = 1;
	}
	for (int i = 0; i < nbones; i++) {
		const unsigned char *p = mdl + bones + (size_t)i * BONE_SIZE;
		srcbone_t *b = &s->bones[i];
		CopyString(mdl, size, (p - mdl) + RdInt(p + BONE_NAME), b->name, sizeof(b->name));
		b->parent = RdInt(p + BONE_PARENT);
		if (b->parent < -1 || b->parent >= i)
			return false;   // parents come first, so one pass sets up the whole skeleton
		memcpy(b->pos, p + BONE_POS, 12);
		memcpy(b->quat, p + BONE_QUAT, 16);
		memcpy(b->rot, p + BONE_ROT, 12);
		memcpy(b->posscale, p + BONE_POSSCALE, 12);
		memcpy(b->rotscale, p + BONE_ROTSCALE, 12);
		memcpy(b->posetobone, p + BONE_POSETOBONE, 48);
	}

	s->numanims = nanims;
	s->anims = Mem_Alloc(sizeof(srcanim_t) * (nanims ? nanims : 1));
	for (int i = 0; i < nanims; i++) {
		const unsigned char *p = mdl + anims + (size_t)i * ANIM_SIZE;
		srcanim_t *a = &s->anims[i];
		a->ofs = (size_t)(p - mdl);
		CopyString(mdl, size, (long long)a->ofs + RdInt(p + ANIM_NAME), a->name, sizeof(a->name));
		a->fps = RdFloat(p + ANIM_FPS);
		a->flags = RdInt(p + ANIM_FLAGS);
		a->numframes = RdInt(p + ANIM_NUMFRAMES);
		a->block = RdInt(p + ANIM_BLOCK);
		a->index = RdInt(p + ANIM_INDEX);
		a->sectionofs = RdInt(p + ANIM_SECTIONINDEX);
		a->sectionframes = RdInt(p + ANIM_SECTIONFRAMES);
		if (a->numframes < 0 || a->sectionframes < 0)
			return false;
		int nmove = RdInt(p + ANIM_NUMMOVEMENTS);
		long long moves = (long long)a->ofs + RdInt(p + ANIM_MOVEMENTINDEX);
		if (nmove > 0 && Has(size, moves, nmove, MOVEMENT_SIZE))
			memcpy(a->movement, mdl + moves + (size_t)(nmove - 1) * MOVEMENT_SIZE + MOVEMENT_POSITION, 12);
		if (a->sectionframes) {
			// one entry per section, one more for the last frame and one spare
			long long count = a->numframes / a->sectionframes + 2;
			if (!Has(size, (long long)a->ofs + a->sectionofs, count, 8))
				return false;
		}
	}

	s->numseqs = nseqs;
	s->seqs = Mem_Alloc(sizeof(srcseq_t) * (nseqs ? nseqs : 1));
	for (int i = 0; i < nseqs; i++) {
		const unsigned char *p = mdl + seqs + (size_t)i * SEQ_SIZE;
		long long base = p - mdl;
		srcseq_t *q = &s->seqs[i];
		CopyString(mdl, size, base + RdInt(p + SEQ_LABEL), q->label, sizeof(q->label));
		CopyString(mdl, size, base + RdInt(p + SEQ_ACTIVITYNAME), q->activity, sizeof(q->activity));
		q->flags = RdInt(p + SEQ_FLAGS);
		q->actweight = RdInt(p + SEQ_ACTWEIGHT);
		memcpy(q->bbmin, p + SEQ_BBMIN, 12);
		memcpy(q->bbmax, p + SEQ_BBMAX, 12);
		int nev = RdInt(p + SEQ_NUMEVENTS);
		if (nev > 0 && Has(size, base + RdInt(p + SEQ_EVENTINDEX), nev, SRC_EVENT_SIZE)) {
			q->numevents = nev;
			q->events = mdl + base + RdInt(p + SEQ_EVENTINDEX);
		}
		int nlay = RdInt(p + SEQ_NUMAUTOLAYERS);
		if (nlay > 0 && Has(size, base + RdInt(p + SEQ_AUTOLAYERINDEX), nlay, LAYER_SIZE)) {
			q->numlayers = nlay;
			q->layers = mdl + base + RdInt(p + SEQ_AUTOLAYERINDEX);
		}
		q->groupsize[0] = RdInt(p + SEQ_GROUPSIZE);
		q->groupsize[1] = RdInt(p + SEQ_GROUPSIZE + 4);
		if (q->groupsize[0] < 0 || q->groupsize[1] < 0 || q->groupsize[0] > 64 || q->groupsize[1] > 64)
			return false;
		if (!q->groupsize[0] || !q->groupsize[1]) {
			// a sequence with no animation of its own (it only stacks other sequences): the rest pose
			q->groupsize[0] = q->groupsize[1] = 0;
			q->anim = -1;
			q->fps = 30;
			q->numframes = 1;
			continue;
		}
		long long grid = base + RdInt(p + SEQ_ANIMINDEXINDEX);
		int cells = q->groupsize[0] * q->groupsize[1];
		if (!Has(size, grid, cells, 2))
			return false;
		q->anims = (const short *)(mdl + grid);
		for (int k = 0; k < 2; k++) {
			q->paramindex[k] = RdInt(p + SEQ_PARAMINDEX + k * 4);
			q->paramstart[k] = RdFloat(p + SEQ_PARAMSTART + k * 4);
			q->paramend[k] = RdFloat(p + SEQ_PARAMEND + k * 4);
		}
		int keys = RdInt(p + SEQ_POSEKEYINDEX);
		if (keys && Has(size, base + keys, q->groupsize[0] + q->groupsize[1], 4))
			q->posekeys = (const float *)(mdl + base + keys);
		int wl = RdInt(p + SEQ_WEIGHTLIST);
		if (wl && nbones > 0 && Has(size, base + wl, nbones, 4))
			q->weights = (const float *)(mdl + base + wl);
		q->anim = q->anims[(q->groupsize[1] / 2) * q->groupsize[0] + q->groupsize[0] / 2];
		if (q->anim < 0 || q->anim >= nanims) {
			// some shipped models name an animation they don't hold: that sequence is the rest pose
			q->anim = -1;
			q->fps = 30;
			q->numframes = 1;
			continue;
		}
		q->fps = s->anims[q->anim].fps;
		q->numframes = s->anims[q->anim].numframes;
	}

	int npose = RdInt(mdl + MDL_NUMPOSEPARAMS);
	long long poses = RdInt(mdl + MDL_POSEPARAMINDEX);
	if (!Has(size, poses, npose, POSEPARAM_SIZE))
		return false;
	s->numlocalpose = npose > 0 ? npose : 0;
	s->localpose = Mem_Alloc(sizeof(srcposeparam_t) * (s->numlocalpose ? s->numlocalpose : 1));
	for (int i = 0; i < s->numlocalpose; i++) {
		const unsigned char *p = mdl + poses + (size_t)i * POSEPARAM_SIZE;
		srcposeparam_t *pp = &s->localpose[i];
		CopyString(mdl, size, (p - mdl) + RdInt(p), pp->name, sizeof(pp->name));
		pp->start = RdFloat(p + 8);
		pp->end = RdFloat(p + 12);
		// at rest a dial sits at 0 (looking straight ahead, moving forward) when its range holds 0
		float range = pp->end - pp->start;
		pp->value = range != 0 ? -pp->start / range : 0;
		if (!(pp->value >= 0 && pp->value <= 1))
			pp->value = 0;
	}

	// animation blocks live in their own file, named in the header
	int nblocks = RdInt(mdl + MDL_NUMANIMBLOCKS);
	if (nblocks > 1) {
		char name[MAX_OSPATH];
		CopyString(mdl, size, RdInt(mdl + MDL_ANIMBLOCKNAME), name, sizeof(name));
		if (!Has(size, RdInt(mdl + MDL_ANIMBLOCKINDEX), nblocks, 8))
			return false;
		if (*name)
			s->ani = SrcFS_LoadFile(va("%s%s", s->prefix, name), &s->anisize);
		if (!s->ani)
			Con_Warnf("%s: animation file %s not found; those animations stay in the rest pose\n", modelname, name);
	}
	return true;
}

void SrcAnim_Free(srcmodel_t *s)
{
	for (int i = 0; i < s->numincludes; i++) {
		Mem_Free(s->includes[i].bonemap);
		Mem_Free(s->includes[i].posemap);
	}
	Mem_Free(s->includes);
	Mem_Free(s->aliases);
	s->aliases = NULL;
	s->numaliases = 0;
	Mem_Free(s->order);
	s->order = NULL;
	s->numorder = 0;
	Mem_Free(s->vseqs);
	Mem_Free(s->pose);
	Mem_Free(s->localpose);
	Mem_Free(s->bones);
	Mem_Free(s->anims);
	Mem_Free(s->seqs);
	Mem_Free(s->ani);
	s->includes = NULL;
	s->vseqs = NULL;
	s->pose = s->localpose = NULL;
	s->bones = NULL;
	s->anims = NULL;
	s->seqs = NULL;
	s->ani = NULL;
	s->numincludes = s->numvseqs = s->numpose = 0;
}

// ---------------------------------------------------------------- include files

// Characters keep most of their animations in files shared by every model with the same skeleton
// (models/humans/male_shared.mdl). Those files are loaded once and stay until the model cache is cleared.
typedef struct {
	char        name[MAX_OSPATH];   // with its prefix
	srcmodel_t *model;              // NULL = tried and failed
} includefile_t;
static includefile_t *include_files;
static int include_count, include_cap;

static srcmodel_t *LoadInclude(const char *prefix, const char *file)
{
	char name[MAX_OSPATH];
	snprintf(name, sizeof(name), "%s%s", prefix, file);
	for (char *c = name; *c; c++)
		if (*c == '\\')
			*c = '/';
	for (int i = 0; i < include_count; i++)
		if (!stricmp(include_files[i].name, name))
			return include_files[i].model;
	if (include_count == include_cap) {
		include_cap = include_cap ? include_cap * 2 : 16;
		include_files = Mem_Realloc(include_files, sizeof(*include_files) * include_cap);
	}
	includefile_t *f = &include_files[include_count++];
	Q_strncpy(f->name, name, sizeof(f->name));
	f->model = NULL;

	size_t size;
	unsigned char *data = SrcFS_LoadFile(name, &size);
	if (!data) {
		Con_Warnf("Animation file %s not found\n", name);
		return NULL;
	}
	srcmodel_t *s = Mem_Alloc(sizeof(*s));
	s->mdl = data;
	s->mdlsize = size;
	s->version = size >= 8 ? RdInt(data + 4) : 0;
	Q_strncpy(s->prefix, prefix, sizeof(s->prefix));
	if (size < 408 || memcmp(data, "IDST", 4) || s->version < SRC_MDL_MIN_VERSION || s->version > SRC_MDL_MAX_VERSION ||
		!SrcAnim_Load(name, s)) {
		Con_Warnf("Animation file %s is damaged or not a Source model version %d to %d\n", name, SRC_MDL_MIN_VERSION,
			SRC_MDL_MAX_VERSION);
		SrcAnim_Free(s);
		Mem_Free(data);
		Mem_Free(s);
		return NULL;
	}
	f->model = s;
	return s;
}

void SrcAnim_FreeIncludes(void)
{
	for (int i = 0; i < include_count; i++) {
		srcmodel_t *s = include_files[i].model;
		if (!s)
			continue;
		SrcAnim_Free(s);
		Mem_Free(s->mdl);
		Mem_Free(s);
	}
	Mem_Free(include_files);
	include_files = NULL;
	include_count = include_cap = 0;
}

// which = -1 for the model itself, else the include whose file src is
static void AppendSequences(srcmodel_t *s, const srcmodel_t *src, int which)
{
	s->vseqs = Mem_Realloc(s->vseqs, sizeof(srcvseq_t) * (s->numvseqs + src->numseqs + 1));
	for (int i = 0; i < src->numseqs; i++) {
		// a name already in the list stays, unless that entry is only a placeholder for this one
		int k;
		for (k = 0; k < s->numvseqs; k++)
			if (!stricmp(SrcAnim_Seq(s, k)->label, src->seqs[i].label))
				break;
		if (k < s->numvseqs && !(SrcAnim_Seq(s, k)->flags & SRCSEQ_OVERRIDE))
			continue;
		s->vseqs[k].include = which;
		s->vseqs[k].local = i;
		if (k == s->numvseqs)
			s->numvseqs++;
	}
}

static int AddPoseParam(srcmodel_t *s, const srcposeparam_t *pp)
{
	for (int i = 0; i < s->numpose; i++)
		if (!stricmp(s->pose[i].name, pp->name))
			return i;
	s->pose = Mem_Realloc(s->pose, sizeof(srcposeparam_t) * (s->numpose + 1));
	s->pose[s->numpose] = *pp;
	return s->numpose++;
}

static void AppendIncludes(const char *name, srcmodel_t *s, const srcmodel_t *from, int depth)
{
	const unsigned char *mdl = from->mdl;
	int count = RdInt(mdl + MDL_NUMINCLUDES);
	long long list = RdInt(mdl + MDL_INCLUDEINDEX);
	if (count <= 0 || !Has(from->mdlsize, list, count, INCLUDE_SIZE))
		return;
	for (int i = 0; i < count; i++) {
		char file[MAX_OSPATH];
		long long entry = list + (long long)i * INCLUDE_SIZE;
		CopyString(mdl, from->mdlsize, entry + RdInt(mdl + entry + 4), file, sizeof(file));
		srcmodel_t *inc = *file ? LoadInclude(s->prefix, file) : NULL;
		if (!inc)
			continue;
		qboolean have = false;
		for (int k = 0; k < s->numincludes; k++)
			if (s->includes[k].model == inc)
				have = true;
		if (have)
			continue;
		s->includes = Mem_Realloc(s->includes, sizeof(srcinclude_t) * (s->numincludes + 1));
		srcinclude_t *in = &s->includes[s->numincludes];
		in->model = inc;
		// the two files number their bones and dials on their own: match them by name
		in->bonemap = Mem_Alloc(sizeof(int) * inc->numbones);
		for (int b = 0; b < inc->numbones; b++) {
			in->bonemap[b] = -1;
			for (int k = 0; k < s->numbones; k++)
				if (!stricmp(s->bones[k].name, inc->bones[b].name)) {
					in->bonemap[b] = k;
					break;
				}
		}
		in->posemap = Mem_Alloc(sizeof(int) * (inc->numlocalpose ? inc->numlocalpose : 1));
		for (int k = 0; k < inc->numlocalpose; k++)
			in->posemap[k] = AddPoseParam(s, &inc->localpose[k]);
		AppendSequences(s, inc, s->numincludes);
		s->numincludes++;
		if (depth < 4)
			AppendIncludes(name, s, inc, depth + 1);
	}
}

void SrcAnim_Link(const char *name, srcmodel_t *s)
{
	for (int i = 0; i < s->numlocalpose; i++)
		AddPoseParam(s, &s->localpose[i]);
	AppendSequences(s, s, -1);
	AppendIncludes(name, s, s, 0);
}

// the animation that gives sequence i its length, speed and travel; NULL if it has none
const srcanim_t *SrcAnim_SeqAnim(const srcmodel_t *s, int i)
{
	const srcseq_t *seq = SrcAnim_Seq(s, i);
	if (!seq || seq->anim < 0)
		return NULL;
	const srcvseq_t *v = &s->vseqs[i];
	return &(v->include < 0 ? s : s->includes[v->include].model)->anims[seq->anim];
}

const srcseq_t *SrcAnim_Seq(const srcmodel_t *s, int i)
{
	if (i < 0 || i >= s->numvseqs)
		return NULL;
	const srcvseq_t *v = &s->vseqs[i];
	return v->include < 0 ? &s->seqs[v->local] : &s->includes[v->include].model->seqs[v->local];
}

// ---------------------------------------------------------------- decoding

static float HalfFloat(unsigned h)
{
	int e = h >> 10 & 31, mant = h & 1023;
	float v;
	if (e == 0)
		v = ldexpf((float)mant, -24);
	else if (e == 31)
		v = mant ? 0 : 65504.0f;   // infinity is stored for "as large as it gets"
	else
		v = ldexpf((float)(mant + 1024), e - 25);
	return h & 0x8000 ? -v : v;
}

// x, y in 16 bits each, z in 15, and the sign of w
static void Quat48(const unsigned char *p, float q[4])
{
	unsigned z = RdU16(p + 4);
	q[0] = ((int)RdU16(p) - 32768) * (1.0f / 32768.0f);
	q[1] = ((int)RdU16(p + 2) - 32768) * (1.0f / 32768.0f);
	q[2] = ((int)(z & 0x7fff) - 16384) * (1.0f / 16384.0f);
	float w = 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
	q[3] = w > 0 ? sqrtf(w) : 0;
	if (z & 0x8000)
		q[3] = -q[3];
}

// x, y, z in 21 bits each, and the sign of w
static void Quat64(const unsigned char *p, float q[4])
{
	unsigned long long v;
	memcpy(&v, p, 8);
	q[0] = ((int)(v & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
	q[1] = ((int)(v >> 21 & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
	q[2] = ((int)(v >> 42 & 0x1fffff) - 1048576) * (1.0f / 1048576.5f);
	float w = 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
	q[3] = w > 0 ? sqrtf(w) : 0;
	if (v >> 63)
		q[3] = -q[3];
}

// rotations about x, then y, then z (radians), as GoldSrc's
static void AngleQuat(const float a[3], float q[4])
{
	float sr = sinf(a[0] * 0.5f), cr = cosf(a[0] * 0.5f);
	float sp = sinf(a[1] * 0.5f), cp = cosf(a[1] * 0.5f);
	float sy = sinf(a[2] * 0.5f), cy = cosf(a[2] * 0.5f);
	q[0] = sr * cp * cy - cr * sp * sy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
	q[3] = cr * cp * cy + sr * sp * sy;
}

static void QuatMult(const float a[4], const float b[4], float out[4])
{
	float r[4];
	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

// a to b by t along the short way, renormalised: frames are close together, so this is as good as a slerp
static void QuatBlend(const float a[4], const float b[4], float t, float out[4])
{
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float sign = dot < 0 ? -1.0f : 1.0f, len = 0;
	for (int i = 0; i < 4; i++) {
		out[i] = a[i] * (1.0f - t) + b[i] * sign * t;
		len += out[i] * out[i];
	}
	len = len > 1e-12f ? 1.0f / sqrtf(len) : 0;
	for (int i = 0; i < 4; i++)
		out[i] *= len;
	if (len == 0)
		out[3] = 1;
}

// The values at frame and frame + 1 from a run-length list: {valid, total} then "valid" values, the last
// of which holds until "total" frames are covered, then the next span. False if the list runs past end.
static qboolean AnimValues(const unsigned char *p, const unsigned char *end, int frame, float scale, float *v1, float *v2)
{
	*v1 = *v2 = 0;
	for (int guard = 0; guard < 65536; guard++) {
		if (p + 2 > end)
			return false;
		int valid = p[0], total = p[1];
		if (!total)
			return true;   // a list shorter than the animation: the bone's default
		if (total <= frame) {
			frame -= total;
			p += (valid + 1) * 2;
			continue;
		}
		// far enough for every read below: this span and the next one's header and first value
		if (p + (valid + 3) * 2 > end)
			return false;
#define VAL(i) ((float)(short)RdU16(p + (i) * 2) * scale)
		if (valid > frame) {
			*v1 = VAL(frame + 1);
			if (valid > frame + 1)
				*v2 = VAL(frame + 2);
			else if (total > frame + 1)
				*v2 = *v1;
			else
				*v2 = VAL(valid + 2);
		} else {
			*v1 = VAL(valid);
			if (total > frame + 1)
				*v2 = *v1;
			else
				*v2 = VAL(valid + 2);
		}
#undef VAL
		return true;
	}
	return false;
}

// One bone's record into its rotation and position
static void DecodeRecord(const srcbone_t *b, const unsigned char *rec, const unsigned char *end, int frame, float t,
	float q[4], float pos[3])
{
	int flags = rec[1];
	const unsigned char *data = rec + 4;
	qboolean delta = (flags & REC_DELTA) != 0;

	if (flags & REC_RAWROT) {
		if (data + 6 <= end)
			Quat48(data, q);
	} else if (flags & REC_RAWROT2) {
		if (data + 8 <= end)
			Quat64(data, q);
	} else if (flags & REC_ANIMROT) {
		// three offsets (from here) to the lists for x, y and z; 0 = that angle doesn't change
		float a1[3], a2[3], q1[4], q2[4];
		for (int i = 0; i < 3 && data + 6 <= end; i++) {
			int ofs = (short)RdU16(data + i * 2);
			a1[i] = a2[i] = 0;
			if (ofs > 0)
				AnimValues(data + ofs, end, frame, b->rotscale[i], &a1[i], &a2[i]);
			if (!delta) {
				a1[i] += b->rot[i];
				a2[i] += b->rot[i];
			}
		}
		if (data + 6 <= end) {
			AngleQuat(a1, q1);
			AngleQuat(a2, q2);
			QuatBlend(q1, q2, t, q);
		}
	} else if (delta) {
		q[0] = q[1] = q[2] = 0;
		q[3] = 1;
	}

	if (flags & REC_RAWPOS) {
		const unsigned char *p = data + ((flags & REC_RAWROT) ? 6 : 0) + ((flags & REC_RAWROT2) ? 8 : 0);
		if (p + 6 <= end)
			for (int i = 0; i < 3; i++)
				pos[i] = HalfFloat(RdU16(p + i * 2));
	} else if (flags & REC_ANIMPOS) {
		// the position's offsets follow the rotation's, when both are lists
		const unsigned char *p = data + ((flags & REC_ANIMROT) ? 6 : 0);
		for (int i = 0; i < 3 && p + 6 <= end; i++) {
			int ofs = (short)RdU16(p + i * 2);
			float v1 = 0, v2 = 0;
			if (ofs > 0)
				AnimValues(p + ofs, end, frame, b->posscale[i], &v1, &v2);
			pos[i] = v1 * (1.0f - t) + v2 * t + (delta ? 0 : b->pos[i]);
		}
	} else if (delta) {
		pos[0] = pos[1] = pos[2] = 0;
	}
}

// Where an animation's records for this frame are: the buffer, its end, and the frame within its section
static const unsigned char *AnimData(const srcmodel_t *s, const srcanim_t *a, int *frame, const unsigned char **end)
{
	int block = a->block, index = a->index;
	if (a->sectionframes > 0) {
		int section;
		if (a->numframes > a->sectionframes && *frame == a->numframes - 1) {
			// the last frame has a section to itself
			*frame = 0;
			section = a->numframes / a->sectionframes + 1;
		} else {
			section = *frame / a->sectionframes;
			*frame -= section * a->sectionframes;
		}
		const unsigned char *sec = s->mdl + a->ofs + a->sectionofs + (size_t)section * 8;
		block = RdInt(sec);
		index = RdInt(sec + 4);
	}
	if (block == 0) {
		if (index < 0 || a->ofs + (size_t)index >= s->mdlsize)
			return NULL;
		*end = s->mdl + s->mdlsize;
		return s->mdl + a->ofs + index;
	}
	// a block: its table (start and end in the .ani file) is in the .mdl
	int nblocks = RdInt(s->mdl + MDL_NUMANIMBLOCKS);
	if (!s->ani || block < 0 || block >= nblocks)
		return NULL;
	const unsigned char *tab = s->mdl + RdInt(s->mdl + MDL_ANIMBLOCKINDEX) + (size_t)block * 8;
	int start = RdInt(tab), stop = RdInt(tab + 4);
	if (start < 0 || stop < start || (size_t)stop > s->anisize || index < 0 || index >= stop - start)
		return NULL;
	*end = s->ani + stop;
	return s->ani + start + index;
}

// One animation of a sequence at a cycle, into the model's bones. owner is the file holding the animation
// and bonemap its bone numbers in the model's (NULL when it is the model's own file). Bones the animation
// doesn't move get the rest pose, or for a delta animation "no change" (no rotation, no offset).
static void CalcAnimation(const srcmodel_t *s, const srcmodel_t *owner, const int *bonemap, const srcseq_t *seq,
	int anim, float cycle, float (*q)[4], vec3_t *pos)
{
	const srcanim_t *a = anim >= 0 && anim < owner->numanims ? &owner->anims[anim] : NULL;
	qboolean delta = a && (a->flags & SRCANIM_DELTA);
	for (int i = 0; i < s->numbones; i++) {
		if (delta) {
			q[i][0] = q[i][1] = q[i][2] = 0;
			q[i][3] = 1;
			VectorClear(pos[i]);
		} else {
			memcpy(q[i], s->bones[i].quat, sizeof(q[i]));
			VectorCopy(s->bones[i].pos, pos[i]);
		}
	}
	if (!a || (a->flags & SRCANIM_ALLZEROS) || a->numframes < 1)
		return;

	float f = cycle * (float)(a->numframes - 1);
	int frame = (int)f;
	float t = f - (float)frame;
	if (frame >= a->numframes - 1) {
		frame = a->numframes - 1;
		t = 0;
	}
	const unsigned char *end = NULL;
	const unsigned char *rec = AnimData(owner, a, &frame, &end);

	// the records come in bone order, each pointing at the next
	for (int guard = 0; rec && guard < 256; guard++) {
		if (rec + 4 > end)
			break;
		int local = rec[0], next = (short)RdU16(rec + 2);
		int bone = local < owner->numbones ? (bonemap ? bonemap[local] : local) : -1;
		if (bone >= 0 && bone < s->numbones && (!seq->weights || seq->weights[local] > 0)) {
			// the values are packed against the bone as the animation's own file describes it
			const srcbone_t *ob = &owner->bones[local];
			if (!delta) {
				memcpy(q[bone], ob->quat, sizeof(q[bone]));
				VectorCopy(ob->pos, pos[bone]);
			}
			DecodeRecord(ob, rec, end, frame, t, q[bone], pos[bone]);
		}
		if (next <= 0)
			break;
		rec += next;
	}
}

// Where a pose parameter puts us along one side of a sequence's grid: the animation before, and how far
// (0..1) towards the next one
static void GridPosition(const srcmodel_t *s, const srcseq_t *seq, int side, int param, const float *pose,
	int *index, float *frac)
{
	*index = 0;
	*frac = 0;
	int n = seq->groupsize[side];
	if (n < 2 || param < 0 || param >= s->numpose)
		return;
	const srcposeparam_t *pp = &s->pose[param];
	float v = pose ? pose[param] : pp->value;   // 0..1 across the parameter's range
	float range = pp->end - pp->start;
	if (seq->posekeys) {
		// the value each animation sits at is listed
		const float *keys = seq->posekeys + (side ? seq->groupsize[0] : 0);
		float value = v * range + pp->start;
		int i = 0;
		float f;
		for (;;) {
			float span = keys[i + 1] - keys[i];
			f = span != 0 ? (value - keys[i]) / span : 0;
			if (i < n - 2 && f > 1.0f) {
				i++;
				continue;
			}
			break;
		}
		*index = i;
		*frac = f < 0 ? 0 : f > 1 ? 1 : f;
		return;
	}
	// or the animations are spread evenly between the sequence's start and end values
	if (range == 0)
		return;
	float lo = (seq->paramstart[side] - pp->start) / range, hi = (seq->paramend[side] - pp->start) / range;
	float f = hi != lo ? (v - lo) / (hi - lo) : 0;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	int i = (int)(f * (float)(n - 1));
	if (i > n - 2)
		i = n - 2;
	*index = i;
	*frac = f * (float)(n - 1) - (float)i;
}

static void BlendPose(int n, float (*q)[4], vec3_t *pos, float (*q2)[4], vec3_t *pos2, float t)
{
	for (int i = 0; i < n; i++) {
		float out[4];
		QuatBlend(q[i], q2[i], t, out);
		memcpy(q[i], out, sizeof(out));
		for (int j = 0; j < 3; j++)
			pos[i][j] = pos[i][j] * (1.0f - t) + pos2[i][j] * t;
	}
}

// scratch poses, grown to the biggest skeleton seen: 0-2 for blending a grid, 3 for a layer
static float (*scratch_q[4])[4];
static vec3_t *scratch_pos[4];
static float *scratch_weight;
static int scratch_cap;

static void EnsureScratch(int bones)
{
	if (bones <= scratch_cap)
		return;
	scratch_cap = bones;
	for (int i = 0; i < 4; i++) {
		scratch_q[i] = Mem_Realloc(scratch_q[i], sizeof(*scratch_q[i]) * bones);
		scratch_pos[i] = Mem_Realloc(scratch_pos[i], sizeof(*scratch_pos[i]) * bones);
	}
	scratch_weight = Mem_Realloc(scratch_weight, sizeof(float) * bones);
}

// the model's number for pose parameter `local` of the file that holds sequence v; -1 if none
static int PoseIndex(const srcmodel_t *s, const srcvseq_t *v, int local)
{
	const srcinclude_t *inc = v->include < 0 ? NULL : &s->includes[v->include];
	const srcmodel_t *owner = inc ? inc->model : s;
	if (local < 0 || local >= owner->numlocalpose)
		return -1;
	if (inc)
		return inc->posemap[local];
	for (int k = 0; k < s->numpose; k++)
		if (!stricmp(s->pose[k].name, owner->localpose[local].name))
			return k;
	return -1;
}

// A sequence's own pose at a cycle: its grid of animations blended by the pose parameters. A delta
// sequence comes out as its changes alone. False if the sequence has no animation.
static qboolean CalcSeqPose(const srcmodel_t *s, int sequence, float cycle, const float *pose, float (*q)[4],
	vec3_t *pos, qboolean *is_delta)
{
	const srcseq_t *seq = SrcAnim_Seq(s, sequence);
	if (!seq || seq->anim < 0)
		return false;
	const srcvseq_t *v = &s->vseqs[sequence];
	const srcinclude_t *inc = v->include < 0 ? NULL : &s->includes[v->include];
	const srcmodel_t *owner = inc ? inc->model : s;
	const int *bonemap = inc ? inc->bonemap : NULL;
	if (cycle < 0) cycle = 0;
	if (cycle > 1) cycle = 1;
	if (is_delta)
		*is_delta = (owner->anims[seq->anim].flags & SRCANIM_DELTA) != 0;

	int g0 = seq->groupsize[0], g1 = seq->groupsize[1];
	int i0 = 0, i1 = 0;
	float f0 = 0, f1 = 0;
	GridPosition(s, seq, 0, PoseIndex(s, v, seq->paramindex[0]), pose, &i0, &f0);
	GridPosition(s, seq, 1, PoseIndex(s, v, seq->paramindex[1]), pose, &i1, &f1);
#define CELL(x, y) seq->anims[(y) * g0 + (x)]
	CalcAnimation(s, owner, bonemap, seq, CELL(i0, i1), cycle, q, pos);
	qboolean across = f0 > 0.001f && i0 + 1 < g0, down = f1 > 0.001f && i1 + 1 < g1;
	// along the first side, then between the two rows
	if (across) {
		CalcAnimation(s, owner, bonemap, seq, CELL(i0 + 1, i1), cycle, scratch_q[0], scratch_pos[0]);
		BlendPose(s->numbones, q, pos, scratch_q[0], scratch_pos[0], f0);
	}
	if (down) {
		CalcAnimation(s, owner, bonemap, seq, CELL(i0, i1 + 1), cycle, scratch_q[1], scratch_pos[1]);
		if (across) {
			CalcAnimation(s, owner, bonemap, seq, CELL(i0 + 1, i1 + 1), cycle, scratch_q[2], scratch_pos[2]);
			BlendPose(s->numbones, scratch_q[1], scratch_pos[1], scratch_q[2], scratch_pos[2], f0);
		}
		BlendPose(s->numbones, q, pos, scratch_q[1], scratch_pos[1], f1);
	}
#undef CELL
	return true;
}

// how much sequence v moves each of the model's bones (0 = leaves it alone)
static void SeqBoneWeights(const srcmodel_t *s, int sequence, float *w)
{
	const srcseq_t *seq = SrcAnim_Seq(s, sequence);
	const srcvseq_t *v = &s->vseqs[sequence];
	const srcinclude_t *inc = v->include < 0 ? NULL : &s->includes[v->include];
	const srcmodel_t *owner = inc ? inc->model : s;
	for (int i = 0; i < s->numbones; i++)
		w[i] = inc ? 0 : (seq->weights ? seq->weights[i] : 1);
	for (int i = 0; inc && i < owner->numbones; i++)
		if (inc->bonemap[i] >= 0)
			w[inc->bonemap[i]] = seq->weights ? seq->weights[i] : 1;
}

// The sequences a sequence lays over itself. Aiming works this way: the standing animation carries a layer
// that is a grid of small changes, picked by the aim parameters and added on top.
static void AddLayers(const srcmodel_t *s, int sequence, float cycle, const float *pose, float (*q)[4], vec3_t *pos)
{
	const srcseq_t *seq = SrcAnim_Seq(s, sequence);
	const srcvseq_t *v = &s->vseqs[sequence];
	for (int k = 0; k < seq->numlayers; k++) {
		const unsigned char *lp = seq->layers + (size_t)k * LAYER_SIZE;
		int local = (short)RdU16(lp), flags = RdInt(lp + 4);
		float start = RdFloat(lp + 8), peak = RdFloat(lp + 12), tail = RdFloat(lp + 16), end = RdFloat(lp + 20);
		// the layer is numbered within the same file as the sequence
		int layer = -1;
		for (int i = 0; i < s->numvseqs && layer < 0; i++)
			if (s->vseqs[i].include == v->include && s->vseqs[i].local == local)
				layer = i;
		if (layer < 0 || layer == sequence)
			continue;
		float weight = 1, layer_cycle = cycle;
		if (start != end) {
			// fades in from start to peak, holds to tail, fades out by end: over the cycle, or over a
			// pose parameter's value
			float index = cycle;
			if (flags & LAYER_POSE) {
				int param = PoseIndex(s, v, (short)RdU16(lp + 2));
				if (param < 0)
					continue;
				const srcposeparam_t *pp = &s->pose[param];
				index = (pose ? pose[param] : pp->value) * (pp->end - pp->start) + pp->start;
			} else
				layer_cycle = (cycle - start) / (end - start);
			if (index < start || index >= end)
				continue;
			float ramp = 1;
			if (index < peak && start != peak)
				ramp = (index - start) / (peak - start);
			else if (index > tail && end != tail)
				ramp = (end - index) / (end - tail);
			if (flags & LAYER_SPLINE)
				ramp = ramp * ramp * (3.0f - 2.0f * ramp);
			weight = ramp;
		}
		qboolean delta = false;
		if (weight <= 0 || !CalcSeqPose(s, layer, layer_cycle, pose, scratch_q[3], scratch_pos[3], &delta))
			continue;
		SeqBoneWeights(s, layer, scratch_weight);
		for (int i = 0; i < s->numbones; i++) {
			float w = weight * scratch_weight[i];
			if (w <= 0)
				continue;
			if (w > 1)
				w = 1;
			if (delta) {
				// part of the change: from "no change" towards it, then on top of what's there
				static const float none[4] = { 0, 0, 0, 1 };
				float part[4];
				QuatBlend(none, scratch_q[3][i], w, part);
				QuatMult(q[i], part, q[i]);
				VectorMA(pos[i], w, scratch_pos[3][i], pos[i]);
			} else {
				float out[4];
				QuatBlend(q[i], scratch_q[3][i], w, out);
				memcpy(q[i], out, sizeof(out));
				for (int j = 0; j < 3; j++)
					pos[i][j] = pos[i][j] * (1.0f - w) + scratch_pos[3][i][j] * w;
			}
		}
	}
}

void SrcAnim_CalcPose(const srcmodel_t *s, int sequence, float cycle, const float *pose, float (*q)[4], vec3_t *pos)
{
	EnsureScratch(s->numbones);
	qboolean delta = false;
	if (!CalcSeqPose(s, sequence, cycle, pose, q, pos, &delta)) {
		for (int i = 0; i < s->numbones; i++) {
			memcpy(q[i], s->bones[i].quat, sizeof(q[i]));
			VectorCopy(s->bones[i].pos, pos[i]);
		}
		return;
	}
	if (delta) {
		// a gesture or posture on its own: shown added to the rest pose
		for (int i = 0; i < s->numbones; i++) {
			QuatMult(s->bones[i].quat, q[i], q[i]);
			VectorAdd(s->bones[i].pos, pos[i], pos[i]);
		}
		return;
	}
	if (cycle < 0) cycle = 0;
	if (cycle > 1) cycle = 1;
	AddLayers(s, sequence, cycle, pose, q, pos);
}
