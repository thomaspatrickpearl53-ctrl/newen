// newen - ragdolls: a dying entity whose model has a physics file goes limp
//
// When hl.dll marks an entity as dying (deadflag) and its Source model has a jointed .phy, each of the
// file's solids becomes a Jolt body where its bone is, joined as the file's ragdollconstraints say, and
// from then on the entity is drawn from those bodies instead of its animation. hl.dll still owns the
// corpse: when it fades it the ragdoll fades, when it removes it the ragdoll goes.
//
// Visual only: nothing in Half-Life's own collision knows about them. They aren't saved.
#include "server.h"
#include "phys.h"
#include "src_local.h"
#include <ctype.h>

// torque (newton metres) every joint resists turning with, so limbs don't swing like chains
#define RAGDOLL_JOINT_FRICTION 3.0f
#define RAGDOLL_SETTLE 3.0          // seconds a ragdoll over the limit gets to fall before it's stopped

static cvar_t *phys_ragdolls, *phys_ragdoll_max, *phys_ragdoll_time, *phys_ragdoll_hl;

// How an entity looked on one frame: enough to pose its bones again later
typedef struct {
	qboolean valid;
	double   time;              // server time
	int      modelindex;
	vec3_t   origin, angles;
	int      sequence;
	float    frame, framerate, animtime;
	unsigned char controller[4], blending[2];
} snap_t;

typedef struct {
	snap_t snap[2];             // the last frame and the one before, while it was alive
	int    ragdoll;             // index + 1, 0 = none
	qboolean dead;              // seen dead: don't try again each frame if it couldn't become one
	qboolean forced;            // limp though alive (ent:BecomeRagdoll), until the edict goes
	vec3_t   kick;              // what hit it (a flying prop) was going this fast...
	double   kicktime;          // ...at this time
} track_t;

typedef struct {
	qboolean     used;
	int          edict;
	model_t     *model;
	int          modelindex;
	double       born;
	qboolean     frozen;        // its bodies are gone; it keeps its last pose
	int          numbones;
	float      (*bones)[3][4];  // every bone in the world, as the renderer wants them
	float      (*local)[3][4];  // a bone with no body: where it is in its parent's space (or the root body's)
	physbody_t  *body;          // per bone
	const int   *parent;        // per bone, the model's (its ragskel_t's)
	int          rootbone;      // the bone of the first body
	int          numjoints;
	physjoint_t *joints;
	physgroup_t  group;
} ragdoll_t;

static track_t   *tracks;
static int        tracks_cap;
static ragdoll_t *ragdolls;
static int        ragdolls_count;

// ---------------------------------------------------------------- 3x4 matrices

static void MatMul(const float a[3][4], const float b[3][4], float out[3][4])
{
	float r[3][4];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		r[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
	}
	memcpy(out, r, sizeof(r));
}

// a rotation and a move: the inverse is the rotation turned over and the move taken back through it
static void MatInvert(const float m[3][4], float out[3][4])
{
	float r[3][4];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			r[i][j] = m[j][i];
		r[i][3] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
	}
	memcpy(out, r, sizeof(r));
}

// how fast a body turned to get from one rotation to another in dt: degrees per second about the world's axes
static void SpinBetween(const float from[3][4], const float to[3][4], float dt, float spin[3])
{
	// d = to x from turned over; its axis and angle
	float d[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			d[i][j] = to[i][0] * from[j][0] + to[i][1] * from[j][1] + to[i][2] * from[j][2];
	float axis[3] = { d[2][1] - d[1][2], d[0][2] - d[2][0], d[1][0] - d[0][1] };
	float s = sqrtf(DotProduct(axis, axis)) * 0.5f, c = (d[0][0] + d[1][1] + d[2][2] - 1) * 0.5f;
	VectorClear(spin);
	if (s < 1e-5f)
		return;   // no turn (or a half turn, which a frame never is)
	float angle = atan2f(s, c) * (180.0f / M_PI_F);
	for (int i = 0; i < 3; i++)
		spin[i] = axis[i] / (2 * s) * angle / dt;
}

// ---------------------------------------------------------------- what a model's ragdoll is made of

// A model's bones as the ragdoll code needs them, and its solids and joints: a Source model's own physics
// file, or for a Half-Life model (phys_ragdoll_hl) ones made here from its hitboxes. Kept per model until
// the map goes (the models go then too).
typedef struct {
	model_t         *model;
	const srcphys_t *phys;       // NULL: this model can't be a ragdoll
	srcphys_t       *made;       // phys when it was made here (ours to free)
	int              numbones;
	int             *parent;
	float          (*rest)[3][4];   // each bone in the model's space in its rest pose: where joint angles are zero
} ragskel_t;

static ragskel_t *skels;
static int        skels_count;

// How far a joint turns, guessed from the child bone's name. Half-Life's characters were made with an
// earlier version of the 3ds Max skeleton Half-Life 2 used: a bone's X runs along it and knees and elbows
// bend about Z, so the ranges are Half-Life 2's own, rounded. Its names differ ("L Leg" is the thigh,
// "L Leg1" the calf, "L Arm" the collar, "L Arm1" the upper arm, "L Arm2" the forearm) and its legs turn
// the other way about Z (checked on scientist.mdl's rest pose: a calf turned +90 puts the foot in front).
// The first word found wins, so the longer names come first. Anything else gets a modest cone.
static void GuessLimits(const char *bone, srcphyjoint_t *j)
{
	static const struct { const char *word; float min[3], max[3]; } table[] = {
		{ "leg1",     {   0,   0, -125 }, { 0,  0,  10 } },
		{ "arm2",     {   0,   0, -120 }, { 0,  0,   5 } },
		{ "arm1",     { -50, -90, -90 }, { 55, 90,  30 } },
		{ "calf",     {   0,   0, -10 }, {  0,  0, 125 } },
		{ "forearm",  {   0,   0, -120 }, { 0,  0,   5 } },
		{ "thigh",    { -25, -45, -95 }, { 35, 45,  30 } },
		{ "upperarm", { -50, -90, -90 }, { 55, 90,  30 } },
		{ "clavicle", { -10, -10, -10 }, { 10, 10,  10 } },
		{ "head",     { -20, -25, -15 }, { 20, 25,  30 } },
		{ "neck",     { -15, -20, -15 }, { 15, 20,  25 } },
		{ "spine",    { -30, -20, -20 }, { 30, 20,  40 } },
		{ "foot",     {   0, -20, -15 }, {  0, 20,  35 } },
		{ "toe",      {   0,   0, -20 }, {  0,  0,  20 } },
		{ "hand",     { -60,   0, -55 }, { 60,  0,  70 } },
		{ "finger",   {   0,   0, -60 }, {  0,  0,  10 } },
		{ "leg",      { -25, -45, -30 }, { 35, 45,  95 } },
		{ "arm",      { -10, -10, -10 }, { 10, 10,  10 } },
	};
	char name[64];
	size_t n = 0;
	for (; bone[n] && n < sizeof(name) - 1; n++)
		name[n] = (char)tolower((unsigned char)bone[n]);
	name[n] = 0;
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (strstr(name, table[i].word)) {
			VectorCopy(table[i].min, j->min);
			VectorCopy(table[i].max, j->max);
			return;
		}
	VectorSet(j->min, -35, -35, -35);
	VectorSet(j->max, 35, 35, 35);
}

// A Half-Life model's solids and joints from its hitboxes: a bone's hitboxes are its solid, joined to the
// nearest bone above it that has one. NULL if it has fewer than two such bones.
static srcphys_t *HitboxPhys(const studiohdr_t *h, const ragskel_t *sk)
{
	const mstudiobbox_t *hb = STUDIO_PTR(h, h->hitboxindex, mstudiobbox_t);
	const mstudiobone_t *bone = STUDIO_PTR(h, h->boneindex, mstudiobone_t);
	int *solidof = Mem_Alloc(sizeof(int) * (h->numbones ? h->numbones : 1));
	int numsolids = 0;
	for (int i = 0; i < h->numbones; i++)
		solidof[i] = -1;
	for (int i = 0; i < h->numhitboxes; i++)
		if (hb[i].bone >= 0 && hb[i].bone < h->numbones && solidof[hb[i].bone] < 0)
			solidof[hb[i].bone] = numsolids++;
	if (numsolids < 2) {
		Mem_Free(solidof);
		return NULL;
	}
	srcphys_t *ph = Mem_Alloc(sizeof(*ph));
	ph->numsolids = numsolids;
	ph->solids = Mem_Alloc(sizeof(*ph->solids) * numsolids);
	ph->joints = Mem_Alloc(sizeof(*ph->joints) * numsolids);
	ph->selfcollide = false;   // hitboxes overlap their neighbours generously
	for (int b = 0; b < h->numbones; b++) {
		if (solidof[b] < 0)
			continue;
		srcphysolid_t *so = &ph->solids[solidof[b]];
		so->index = solidof[b];
		so->bone = b;
		Q_strncpy(so->name, bone[b].name, sizeof(so->name));
		Q_strncpy(so->surfaceprop, "flesh", sizeof(so->surfaceprop));
		so->described = true;
		int n = 0;
		for (int i = 0; i < h->numhitboxes; i++)
			n += hb[i].bone == b;
		so->counts = Mem_Alloc(sizeof(int) * n);
		so->points = Mem_Alloc(sizeof(float) * 24 * n);
		float volume = 0;
		for (int i = 0; i < h->numhitboxes; i++) {
			if (hb[i].bone != b)
				continue;
			vec3_t mins, maxs;
			for (int k = 0; k < 3; k++) {
				mins[k] = hb[i].bbmin[k] < hb[i].bbmax[k] ? hb[i].bbmin[k] : hb[i].bbmax[k];
				maxs[k] = hb[i].bbmin[k] < hb[i].bbmax[k] ? hb[i].bbmax[k] : hb[i].bbmin[k];
				if (!(mins[k] > -4096 && maxs[k] < 4096))
					mins[k] = maxs[k] = 0;
				if (maxs[k] - mins[k] < 1) {   // a flat box has no shape
					mins[k] -= 0.5f;
					maxs[k] += 0.5f;
				}
			}
			float *pts = so->points + so->numpoints * 3;
			for (int c = 0; c < 8; c++)
				for (int k = 0; k < 3; k++)
					pts[c * 3 + k] = c & (1 << k) ? maxs[k] : mins[k];
			so->counts[so->numpieces++] = 8;
			so->numpoints += 8;
			volume += (maxs[0] - mins[0]) * (maxs[1] - mins[1]) * (maxs[2] - mins[2]);
		}
		// about half of a box that size is flesh, as heavy as water
		so->volume = volume;
		so->mass = volume * (0.0254f * 0.0254f * 0.0254f) * 500.0f;
		if (so->mass < 0.5f)
			so->mass = 0.5f;
		so->damping = 0.05f;
		so->rotdamping = 1.5f;
		so->inertia = 3;
		// joined to the nearest bone above that has a solid
		int up = sk->parent[b];
		while (up >= 0 && solidof[up] < 0)
			up = sk->parent[up];
		if (up >= 0) {
			srcphyjoint_t *j = &ph->joints[ph->numjoints++];
			j->parent = solidof[up];
			j->child = solidof[b];
			GuessLimits(bone[b].name, j);
			Q_strncpy(so->parent, bone[up].name, sizeof(so->parent));
		}
	}
	Mem_Free(solidof);
	if (!ph->numjoints) {
		SrcPhys_Free(ph);
		return NULL;
	}
	return ph;
}

// rotations about X, then Y, then Z (radians) and a position: how Half-Life's models store a bone
static void BoneMatrix(const float value[6], float m[3][4])
{
	float sx = sinf(value[3]), cx = cosf(value[3]), sy = sinf(value[4]), cy = cosf(value[4]);
	float sz = sinf(value[5]), cz = cosf(value[5]);
	m[0][0] = cz * cy;  m[0][1] = cz * sy * sx - sz * cx;  m[0][2] = cz * sy * cx + sz * sx;
	m[1][0] = sz * cy;  m[1][1] = sz * sy * sx + cz * cx;  m[1][2] = sz * sy * cx - cz * sx;
	m[2][0] = -sy;      m[2][1] = cy * sx;                 m[2][2] = cy * cx;
	for (int k = 0; k < 3; k++)
		m[k][3] = value[k];
}

static const ragskel_t *SkelFor(model_t *m)
{
	if (!m || m->type != mod_studio)
		return NULL;
	for (int i = 0; i < skels_count; i++)
		if (skels[i].model == m)
			return skels[i].phys ? &skels[i] : NULL;
	const srcmodel_t *s = m->studio.source;
	const studiohdr_t *h = m->studio.hdr;
	// a Half-Life model is only looked at while the cvar is on (so turning it on later still works)
	if (!s && (!phys_ragdoll_hl->value || !h || h->numbones < 2 || h->numhitboxes < 2))
		return NULL;
	skels = Mem_Realloc(skels, sizeof(*skels) * (skels_count + 1));
	ragskel_t *sk = &skels[skels_count++];
	memset(sk, 0, sizeof(*sk));
	sk->model = m;
	sk->numbones = s ? s->numbones : h->numbones;
	sk->parent = Mem_Alloc(sizeof(int) * sk->numbones);
	sk->rest = Mem_Alloc(sizeof(*sk->rest) * sk->numbones);
	if (s) {
		for (int i = 0; i < sk->numbones; i++) {
			sk->parent[i] = s->bones[i].parent;
			MatInvert(s->bones[i].posetobone, sk->rest[i]);
		}
		sk->phys = s->phys && s->phys->numjoints ? s->phys : NULL;
	} else {
		const mstudiobone_t *bone = STUDIO_PTR(h, h->boneindex, mstudiobone_t);
		for (int i = 0; i < sk->numbones; i++) {
			int parent = bone[i].parent;
			sk->parent[i] = parent >= 0 && parent < i ? parent : -1;   // parents come first; anything else is a root
			float local[3][4];
			BoneMatrix(bone[i].value, local);
			if (sk->parent[i] < 0)
				memcpy(sk->rest[i], local, sizeof(local));
			else
				MatMul(sk->rest[sk->parent[i]], local, sk->rest[i]);
		}
		sk->phys = sk->made = HitboxPhys(h, sk);
	}
	return sk->phys ? sk : NULL;
}

static void FreeSkels(void)
{
	for (int i = 0; i < skels_count; i++) {
		SrcPhys_Free(skels[i].made);
		Mem_Free(skels[i].parent);
		Mem_Free(skels[i].rest);
	}
	Mem_Free(skels);
	skels = NULL;
	skels_count = 0;
}

// ---------------------------------------------------------------- making and removing

static void FreeBodies(ragdoll_t *rd)
{
	physworld_t *w = Phys_ServerWorld(false);
	if (w) {
		for (int i = 0; i < rd->numjoints; i++)
			Phys_RemoveJoint(w, rd->joints[i]);
		for (int i = 0; i < rd->numbones; i++)
			if (rd->body[i] != PHYS_NOBODY)
				Phys_RemoveBody(w, rd->body[i]);
		Phys_FreeGroup(w, rd->group);
	}
	for (int i = 0; i < rd->numbones; i++)
		rd->body[i] = PHYS_NOBODY;
	rd->numjoints = 0;
	rd->group = 0;
	rd->frozen = true;
}

static void RemoveRagdoll(ragdoll_t *rd)
{
	if (!rd->used)
		return;
	FreeBodies(rd);
	Mem_Free(rd->bones);
	Mem_Free(rd->local);
	Mem_Free(rd->body);
	Mem_Free(rd->joints);
	if (rd->edict < tracks_cap)
		tracks[rd->edict].ragdoll = 0;
	memset(rd, 0, sizeof(*rd));
}

// the entity's bones in the world as it stood at a snapshot; out holds the model's bone count
static qboolean PoseAt(const snap_t *sn, model_t *m, float (*out)[3][4], int numbones)
{
	rentity_t r;
	memset(&r, 0, sizeof(r));
	r.model = m;
	VectorCopy(sn->origin, r.origin);
	VectorCopy(sn->angles, r.angles);
	r.sequence = sn->sequence;
	r.frame = sn->frame;
	r.framerate = sn->framerate;
	// the renderer runs animations on its own clock: set that back to the snapshot's moment
	r.animtime = r_refdef.time - (sn->time - sn->animtime);
	memcpy(r.controller, sn->controller, 4);
	memcpy(r.blending, sn->blending, 2);
	float (*bones)[3][4];
	if (R_StudioSetupBones(&r, &bones) != numbones)
		return false;
	memcpy(out, bones, sizeof(*out) * numbones);
	return true;
}

static void TakeSnap(const edict_t *e, snap_t *sn)
{
	sn->valid = true;
	sn->time = sv.time;
	sn->modelindex = e->v.modelindex;
	VectorCopy(e->v.origin, sn->origin);
	VectorCopy(e->v.angles, sn->angles);
	sn->sequence = e->v.sequence;
	sn->frame = e->v.frame;
	sn->framerate = e->v.framerate;
	sn->animtime = e->v.animtime;
	memcpy(sn->controller, e->v.controller, 4);
	memcpy(sn->blending, e->v.blending, 2);
}

// every bone from the bodies: theirs straight, the others carried by their parents as they were at death
static void UpdateBones(ragdoll_t *rd, physworld_t *w)
{
	for (int i = 0; i < rd->numbones; i++)
		if (rd->body[i] != PHYS_NOBODY)
			Phys_GetMatrix(w, rd->body[i], rd->bones[i]);
	for (int i = 0; i < rd->numbones; i++) {
		if (rd->body[i] != PHYS_NOBODY)
			continue;
		int parent = rd->parent[i];
		MatMul(rd->bones[parent < 0 ? rd->rootbone : parent], rd->local[i], rd->bones[i]);
	}
}

static qboolean Awake(const ragdoll_t *rd, physworld_t *w)
{
	for (int i = 0; i < rd->numbones; i++)
		if (rd->body[i] != PHYS_NOBODY && Phys_IsAwake(w, rd->body[i]))
			return true;
	return false;
}

// More simulated than the limit: the oldest stop being simulated and keep their pose. One still falling
// would freeze in mid-air, so a ragdoll is only taken once it lies still or has had RAGDOLL_SETTLE seconds;
// many dying at once are over the limit for that long.
static void EnforceLimit(physworld_t *w)
{
	int limit = (int)phys_ragdoll_max->value;
	if (limit < 1)
		limit = 1;
	for (;;) {
		int live = 0, oldest = -1;
		for (int i = 0; i < ragdolls_count; i++) {
			const ragdoll_t *rd = &ragdolls[i];
			if (!rd->used || rd->frozen)
				continue;
			live++;
			if ((oldest < 0 || rd->born < ragdolls[oldest].born) && (sv.time - rd->born > RAGDOLL_SETTLE || !Awake(rd, w)))
				oldest = i;
		}
		if (live <= limit || oldest < 0)
			return;
		FreeBodies(&ragdolls[oldest]);
	}
}

static qboolean MakeRagdoll(int num, edict_t *e, track_t *t, model_t *m)
{
	const ragskel_t *s = SkelFor(m);
	if (!s)
		return false;
	const srcphys_t *ph = s->phys;
	physworld_t *w = Phys_ServerWorld(true);
	if (!w)
		return false;

	ragdoll_t *rd = NULL;
	for (int i = 0; i < ragdolls_count && !rd; i++)
		if (!ragdolls[i].used)
			rd = &ragdolls[i];
	if (!rd) {
		ragdolls = Mem_Realloc(ragdolls, sizeof(*ragdolls) * (ragdolls_count + 1));
		rd = &ragdolls[ragdolls_count++];
	}
	memset(rd, 0, sizeof(*rd));
	rd->used = true;
	rd->edict = num;
	rd->model = m;
	rd->modelindex = e->v.modelindex;
	rd->born = sv.time;
	rd->numbones = s->numbones;
	rd->parent = s->parent;
	rd->bones = Mem_Alloc(sizeof(*rd->bones) * s->numbones);
	rd->local = Mem_Alloc(sizeof(*rd->local) * s->numbones);
	rd->body = Mem_Alloc(sizeof(*rd->body) * s->numbones);
	rd->joints = Mem_Alloc(sizeof(*rd->joints) * (ph->numjoints ? ph->numjoints : 1));
	for (int i = 0; i < s->numbones; i++)
		rd->body[i] = PHYS_NOBODY;

	// the pose it had while still alive: by now hl.dll may have switched it to a death animation the
	// model doesn't have. One that was never seen alive (a corpse placed in the map) starts as it is.
	snap_t now;
	TakeSnap(e, &now);
	const snap_t *last = t->snap[0].valid && t->snap[0].modelindex == e->v.modelindex ? &t->snap[0] : &now;
	const snap_t *before = t->snap[1].valid && t->snap[1].modelindex == e->v.modelindex && last != &now ? &t->snap[1] : NULL;
	float (*prev)[3][4] = Mem_Alloc(sizeof(*prev) * s->numbones);
	float dt = before ? (float)(last->time - before->time) : 0;
	qboolean moving = before && dt > 0.001f && dt < 0.25f && PoseAt(before, m, prev, s->numbones);
	if (!PoseAt(last, m, rd->bones, s->numbones)) {
		Mem_Free(prev);
		RemoveRagdoll(rd);
		return false;
	}

	rd->group = Phys_NewGroup(w, ph->numsolids);
	rd->rootbone = -1;
	int made = 0;
	for (int i = 0; i < ph->numsolids; i++) {
		const srcphysolid_t *so = &ph->solids[i];
		if (so->bone < 0 || !so->numpieces || rd->body[so->bone] != PHYS_NOBODY)
			continue;
		physbody_t b = Phys_AddConvexSetAt(w, so->points, so->counts, so->numpieces, rd->bones[so->bone], PHYS_DYNAMIC,
			so->mass > 0.1f ? so->mass : 0.1f, so->inertia);
		if (b == PHYS_NOBODY)
			continue;
		rd->body[so->bone] = b;
		if (rd->rootbone < 0)
			rd->rootbone = so->bone;
		Phys_SetGroup(w, b, rd->group, i);
		Phys_SetDamping(w, b, so->damping, so->rotdamping);
		Phys_SetLayer(w, b, PHYS_LAYER_RAGDOLL);
		Phys_SetTag(w, b, -1 - (int)(rd - ragdolls));   // negative: a ragdoll part, for bullets
		// its speed from the last two frames; a jump (a teleport, a new sequence) isn't speed
		vec3_t vel, spin = { 0, 0, 0 };
		VectorCopy(e->v.velocity, vel);
		if (moving) {
			vec3_t v;
			for (int k = 0; k < 3; k++)
				v[k] = (rd->bones[so->bone][k][3] - prev[so->bone][k][3]) / dt;
			if (DotProduct(v, v) < 1000.0f * 1000.0f) {
				VectorCopy(v, vel);
				SpinBetween(prev[so->bone], rd->bones[so->bone], dt, spin);
				if (DotProduct(spin, spin) > 720.0f * 720.0f)
					VectorClear(spin);
			}
		}
		// what killed it may have been an explosion a moment ago
		vec3_t at = { rd->bones[so->bone][0][3], rd->bones[so->bone][1][3], rd->bones[so->bone][2][3] }, blast;
		Phys_BlastVelocity(at, so->mass, blast);
		VectorAdd(vel, blast, vel);
		// or a flying prop
		if (t->kicktime > 0 && sv.time - t->kicktime < 0.5 && sv.time >= t->kicktime)
			VectorAdd(vel, t->kick, vel);
		Phys_SetMotion(w, b, vel, spin);
		made++;
	}
	Mem_Free(prev);
	if (!made) {
		RemoveRagdoll(rd);
		return false;
	}

	// the joints: the child's bone axes are the joint's, placed on the parent where the child's bone sits
	// in the model's rest pose, which is where the file's angles are zero
	static const float identity[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
	for (int i = 0; i < ph->numjoints; i++) {
		const srcphyjoint_t *j = &ph->joints[i];
		int pb = ph->solids[j->parent].bone, cb = ph->solids[j->child].bone;
		if (pb < 0 || cb < 0 || rd->body[pb] == PHYS_NOBODY || rd->body[cb] == PHYS_NOBODY)
			continue;
		float modeltoparent[3][4], frame1[3][4];
		MatInvert(s->rest[pb], modeltoparent);
		MatMul(modeltoparent, s->rest[cb], frame1);
		physjoint_t pj = Phys_AddJoint(w, rd->body[pb], rd->body[cb], frame1, identity, j->min, j->max, RAGDOLL_JOINT_FRICTION);
		if (pj)
			rd->joints[rd->numjoints++] = pj;
	}
	// which of its own solids collide: the file's pairs if it has rules, else all that aren't joined
	if (ph->numpairs || !ph->selfcollide) {
		for (int i = 0; i < ph->numpairs; i++)
			Phys_GroupCollide(w, rd->group, ph->pairs[i][0], ph->pairs[i][1]);
	} else {
		for (int a = 0; a < ph->numsolids; a++)
			for (int b = a + 1; b < ph->numsolids; b++) {
				qboolean joined = false;
				for (int i = 0; i < ph->numjoints && !joined; i++)
					joined = (ph->joints[i].parent == a && ph->joints[i].child == b) ||
						(ph->joints[i].parent == b && ph->joints[i].child == a);
				if (!joined)
					Phys_GroupCollide(w, rd->group, a, b);
			}
	}

	// bones without a body keep their place against their parent (fingers, the spine between two solids)
	for (int i = 0; i < s->numbones; i++) {
		if (rd->body[i] != PHYS_NOBODY)
			continue;
		int parent = s->parent[i];
		float inv[3][4];
		MatInvert(rd->bones[parent < 0 ? rd->rootbone : parent], inv);
		MatMul(inv, rd->bones[i], rd->local[i]);
	}
	t->ragdoll = (int)(rd - ragdolls) + 1;
	return true;
}

// ---------------------------------------------------------------- each frame

// before the physics step: who died, who's gone
void Ragdoll_Frame(void)
{
	if (!sv.active)
		return;
	if (sv.num_edicts > tracks_cap) {
		tracks = Mem_Realloc(tracks, sizeof(*tracks) * sv.num_edicts);
		memset(tracks + tracks_cap, 0, sizeof(*tracks) * (sv.num_edicts - tracks_cap));
		tracks_cap = sv.num_edicts;
	}
	for (int i = sv.maxclients + 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		track_t *t = &tracks[i];
		ragdoll_t *rd = t->ragdoll ? &ragdolls[t->ragdoll - 1] : NULL;
		model_t *m = e->free || !e->v.modelindex ? NULL : SV_ModelForIndex(e->v.modelindex);
		// (the cvar is asked first: with it off, Half-Life models aren't even looked at)
		const ragskel_t *s = m && m->type == mod_studio && (m->studio.source || phys_ragdoll_hl->value) ? SkelFor(m) : NULL;
		if (e->free) {
			t->forced = false;
			t->kicktime = 0;
		}
		qboolean dead = s && (e->v.deadflag != 0 || t->forced);
		// gone, alive again, or another model: the ragdoll goes with it
		if (rd && (!dead || rd->modelindex != e->v.modelindex || rd->model != m)) {
			RemoveRagdoll(rd);
			rd = NULL;
		}
		if (!s) {
			t->snap[0].valid = t->snap[1].valid = false;
			t->dead = false;
			continue;
		}
		if (!dead) {
			t->dead = false;
			t->snap[1] = t->snap[0];
			TakeSnap(e, &t->snap[0]);
			continue;
		}
		if (!rd && !t->dead && phys_ragdolls->value && !(e->v.effects & EF_NODRAW))
			MakeRagdoll(i, e, t, m);
		// The corpse's box stays where it died while the body lies elsewhere, so shots and blasts there
		// would bleed and gib thin air. A corpse with a ragdoll takes no damage: explosions throw it instead.
		if (t->ragdoll && e->v.deadflag != 0)
			e->v.takedamage = 0;
		t->dead = true;
	}
	// edicts past the end are gone too
	for (int i = 0; i < ragdolls_count; i++)
		if (ragdolls[i].used && ragdolls[i].edict >= sv.num_edicts)
			RemoveRagdoll(&ragdolls[i]);
}

// after the physics step: the bones for the renderer; old ragdolls stop being simulated
void Ragdoll_AfterStep(void)
{
	physworld_t *w = Phys_ServerWorld(false);
	if (!w)
		return;
	float keep = phys_ragdoll_time->value;
	for (int i = 0; i < ragdolls_count; i++) {
		ragdoll_t *rd = &ragdolls[i];
		if (!rd->used || rd->frozen)
			continue;
		UpdateBones(rd, w);
		if (keep > 0 && sv.time - rd->born > keep)
			FreeBodies(rd);
	}
	EnforceLimit(w);
}

void Ragdoll_Kick(int num, const float velocity[3])
{
	if (num <= 0 || num >= tracks_cap)
		return;
	VectorCopy(velocity, tracks[num].kick);
	tracks[num].kicktime = sv.time > 0 ? sv.time : 0.001;
}

qboolean Ragdoll_Force(int num)
{
	if (!sv.active || num <= sv.maxclients || num >= sv.num_edicts || !phys_ragdolls->value)
		return false;
	edict_t *e = EDICT_NUM(num);
	model_t *m = e->free ? NULL : SV_ModelForIndex(e->v.modelindex);
	if (!SkelFor(m))
		return false;
	Ragdoll_Frame();   // makes sure it's tracked
	if (num >= tracks_cap)
		return false;
	tracks[num].forced = true;
	Ragdoll_Frame();
	return tracks[num].ragdoll != 0;
}

void Ragdoll_Clear(void)
{
	for (int i = 0; i < ragdolls_count; i++)
		RemoveRagdoll(&ragdolls[i]);
	FreeSkels();
	Mem_Free(ragdolls);
	Mem_Free(tracks);
	ragdolls = NULL;
	tracks = NULL;
	ragdolls_count = tracks_cap = 0;
}

// ---------------------------------------------------------------- for the renderer

int Ragdoll_ForEdict(int num, vec3_t origin)
{
	if (num < 0 || num >= tracks_cap || !tracks[num].ragdoll)
		return 0;
	const ragdoll_t *rd = &ragdolls[tracks[num].ragdoll - 1];
	// drawn, lit and culled from where its body lies, not where it died
	for (int k = 0; k < 3; k++)
		origin[k] = rd->bones[rd->rootbone][k][3];
	return tracks[num].ragdoll;
}

qboolean Ragdoll_Bones(int handle, const model_t *m, float (*out)[3][4], int numbones)
{
	if (handle < 1 || handle > ragdolls_count)
		return false;
	const ragdoll_t *rd = &ragdolls[handle - 1];
	if (!rd->used || rd->model != m || rd->numbones != numbones)
		return false;
	memcpy(out, rd->bones, sizeof(*out) * numbones);
	return true;
}

// ---------------------------------------------------------------- commands

// in a joint's own terms: how far the child has turned about axis from the rest pose, for a joint
// that turns about one axis only (knees, elbows)
static float HingeAngle(const ragdoll_t *rd, const ragskel_t *s, int pb, int cb, int axis)
{
	float modeltoparent[3][4], frame1[3][4], world1[3][4], inv[3][4], rel[3][4];
	MatInvert(s->rest[pb], modeltoparent);
	MatMul(modeltoparent, s->rest[cb], frame1);
	MatMul(rd->bones[pb], frame1, world1);
	MatInvert(world1, inv);
	MatMul(inv, rd->bones[cb], rel);
	int a = (axis + 1) % 3, b = (axis + 2) % 3;
	return atan2f(rel[b][a], rel[a][a]) * (180.0f / M_PI_F);
}

static void Ragdolls_f(void)
{
	physworld_t *w = Phys_ServerWorld(false);
	int n = 0, live = 0, awake = 0;
	for (int i = 0; i < ragdolls_count; i++) {
		const ragdoll_t *rd = &ragdolls[i];
		if (!rd->used)
			continue;
		n++;
		int bodies = 0, moving = 0;
		for (int k = 0; k < rd->numbones; k++)
			if (rd->body[k] != PHYS_NOBODY) {
				bodies++;
				moving += w && Phys_IsAwake(w, rd->body[k]);
			}
		live += !rd->frozen;
		awake += moving > 0;
		const float (*root)[4] = rd->bones[rd->rootbone];
		Con_Printf("  edict %4d  %-40s %2d bodies %2d joints  %s  at %.0f %.0f %.0f  %.1f s old\n", rd->edict,
			rd->model->name, bodies, rd->numjoints, rd->frozen ? "kept still" : moving ? "moving" : "asleep",
			root[0][3], root[1][3], root[2][3], sv.time - rd->born);
		if (Cmd_Argc() > 1 && !rd->frozen) {
			// joints that bend one way only, against their limits
			const ragskel_t *s = SkelFor(rd->model);
			const srcphys_t *ph = s ? s->phys : NULL;
			for (int k = 0; ph && k < ph->numjoints; k++) {
				const srcphyjoint_t *j = &ph->joints[k];
				int free = 0, axis = 0;
				for (int a = 0; a < 3; a++)
					if (j->max[a] - j->min[a] >= 0.5f) {
						free++;
						axis = a;
					}
				int pb = ph->solids[j->parent].bone, cb = ph->solids[j->child].bone;
				if (free != 1 || pb < 0 || cb < 0 || rd->body[pb] == PHYS_NOBODY || rd->body[cb] == PHYS_NOBODY)
					continue;
				float ang = HingeAngle(rd, s, pb, cb, axis);
				Con_Printf("    %-28s %c %6.1f  (%g to %g)%s\n", ph->solids[j->child].name, "xyz"[axis], ang, j->min[axis],
					j->max[axis], ang < j->min[axis] - 5 || ang > j->max[axis] + 5 ? "  OUTSIDE" : "");
			}
		}
	}
	Con_Printf("%d ragdolls: %d simulated (%d moving), %d kept still; limit %d\n", n, live, awake, n - live,
		(int)phys_ragdoll_max->value);
}

void Ragdoll_Init(void)
{
	phys_ragdolls = Cvar_Get("phys_ragdolls", "1", FCVAR_ARCHIVE, "dying characters with a physics file fall limp (Source models)");
	phys_ragdoll_max = Cvar_Get("phys_ragdoll_max", "16", FCVAR_ARCHIVE, "most ragdolls simulated at once; the oldest beyond it keeps its pose and stops moving");
	phys_ragdoll_time = Cvar_Get("phys_ragdoll_time", "0", FCVAR_ARCHIVE, "seconds a ragdoll is simulated before it keeps its pose (0 = until the map changes)");
	phys_ragdoll_hl = Cvar_Get("phys_ragdoll_hl", "0", FCVAR_ARCHIVE, "Half-Life's own characters also fall limp, with joints guessed from their hitboxes, instead of playing their death animations");
	Cmd_AddCommand("phys_ragdolls_list", Ragdolls_f, "lists the ragdolls (with any argument: their knees and elbows against their limits)");
}
