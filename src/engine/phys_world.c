// newen - physics in the engine: the map in Jolt, brush entities following their edicts, test commands
//
// Jolt starts the first time something needs it, and a map gets its physics world the first time something
// in it does (phys_dropbox, later props and ragdolls), so a game that never uses physics never pays for it.
// The world is the map's world-model faces (water left out: it isn't solid), one static mesh. Every solid
// brush entity (doors, lifts, trains, walls, breakables) is a kinematic mesh of its own faces, moved each
// frame to where Half-Life put its edict, so it carries and pushes what lies on or in front of it.
#include "server.h"
#include "phys.h"

// ---------------------------------------------------------------- phys_test

static int failures;

static void Check(int ok, const char *what, const char *detail)
{
	Con_Printf("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
	if (!ok)
		failures++;
}

static float AngleDiff(float a, float b)
{
	float d = fmodf(a - b, 360);
	if (d > 180)
		d -= 360;
	if (d < -180)
		d += 360;
	return fabsf(d);
}

// a box dropped tumbling onto a floor must come to rest flat on one face, the right height above it,
// still, and asleep; angles and impulses must come back in the game's units
static void PhysTest_f(void)
{
	if (!Phys_Init()) {
		Con_Printf("phys_test: FAIL (Jolt didn't start)\n");
		return;
	}
	Con_Printf("phys_test: %s\n", Phys_Version());
	failures = 0;
	double t0 = Sys_FloatTime();
	physworld_t *w = Phys_CreateWorld(64, 800);
	char d[256];

	// the floor's top is at z = 0
	static const float floorhalf[3] = { 512, 512, 16 }, floororg[3] = { 0, 0, -16 };
	physbody_t floorb = Phys_AddBox(w, floorhalf, floororg, NULL, PHYS_STATIC, 0);
	Phys_Optimize(w);
	Check(floorb != PHYS_NOBODY, "static floor", NULL);

	// angles survive a round trip
	static const float rt_org[3] = { 0, 0, 500 }, rt_ang[3] = { 30, 45, -20 }, half8[3] = { 8, 8, 8 };
	physbody_t k = Phys_AddBox(w, half8, rt_org, rt_ang, PHYS_KINEMATIC, 0);
	float org[3], ang[3];
	Phys_GetPose(w, k, org, ang);
	float worst = 0;
	for (int i = 0; i < 3; i++)
		worst = fmaxf(worst, AngleDiff(ang[i], rt_ang[i]));
	snprintf(d, sizeof(d), "(%.3f %.3f %.3f), off by %.4f degrees", ang[0], ang[1], ang[2], worst);
	Check(worst < 0.01f && fabsf(org[2] - 500) < 0.01f, "angles round trip", d);
	Phys_RemoveBody(w, k);

	// a 20 kg box: an impulse of 2000 kg*in/s makes 100 in/s
	static const float side_org[3] = { 200, 0, 8 }, push[3] = { 2000, 0, 0 };
	physbody_t s = Phys_AddBox(w, half8, side_org, NULL, PHYS_DYNAMIC, 20);
	Phys_AddImpulse(w, s, push);
	float vel[3];
	Phys_GetVelocity(w, s, vel);
	snprintf(d, sizeof(d), "%.2f in/s", vel[0]);
	Check(fabsf(vel[0] - 100) < 0.5f, "impulse in game units", d);

	// the tumbling drop
	static const float drop_org[3] = { 0, 0, 200 }, drop_ang[3] = { 20, 30, 10 };
	physbody_t b = Phys_AddBox(w, half8, drop_org, drop_ang, PHYS_DYNAMIC, 20);
	Check(b != PHYS_NOBODY, "dynamic box", NULL);
	float lowest = 1e9f;
	int steps = 0;
	for (float t = 0; t < 4; t += 1.0f / 60) {   // 4 s of 60 fps frames
		Phys_Step(w, 1.0f / 60);
		Phys_GetPose(w, b, org, NULL);
		lowest = fminf(lowest, org[2]);
		steps++;
	}
	Phys_GetPose(w, b, org, ang);
	Phys_GetVelocity(w, b, vel);
	snprintf(d, sizeof(d), "z %.2f (lowest %.2f)", org[2], lowest);
	Check(fabsf(org[2] - 8) < 0.5f && lowest > 6, "rests on the floor", d);
	vec3_t f, r, u;
	AngleVectors(ang, f, r, u);
	float flat = fmaxf(fabsf(f[2]), fmaxf(fabsf(r[2]), fabsf(u[2])));
	snprintf(d, sizeof(d), "angles %.1f %.1f %.1f", ang[0], ang[1], ang[2]);
	Check(flat > 0.999f, "lies flat on a face", d);
	snprintf(d, sizeof(d), "%.3f in/s", VectorLength(vel));
	Check(VectorLength(vel) < 0.5f, "came to rest", d);
	Check(!Phys_IsAwake(w, b), "went to sleep", NULL);
	Phys_GetPose(w, s, org, NULL);
	snprintf(d, sizeof(d), "slid to x %.1f", org[0]);
	Check(org[0] > 210 && org[0] < 400 && !Phys_IsAwake(w, s), "pushed box slid and stopped", d);
	Check(Phys_BodyCount(w) == 3, "body count", NULL);

	Phys_DestroyWorld(w);
	Con_Printf("phys_test: %s (%d frames simulated in %.1f ms)\n", failures ? "FAIL" : "PASS", steps,
		(Sys_FloatTime() - t0) * 1000);
}

// ---------------------------------------------------------------- the map's physics world

typedef struct {
	int        serial;        // serialnumber + 1 of the edict it follows; 0 = none
	int        modelindex;
	physbody_t body;
	vec3_t     origin, angles;   // where it was put last
} brushbody_t;

typedef struct {
	physbody_t body;
	vec3_t     half;
} dropbox_t;

static physworld_t *svworld;
static physbody_t   worldbody = PHYS_NOBODY;
static brushbody_t *brushes;      // by edict number
static int          brushes_cap;
static dropbox_t   *boxes;
static int          box_count, box_cap;
static cvar_t      *phys_maxbodies, *phys_showtime;

// a brush model's faces as triangles in its own space (front faces counter-clockwise; BSP polygons run
// clockwise seen from the front)
static float *BrushTriangles(int submodel, int *numtris)
{
	*numtris = 0;
	if (submodel < 0 || submodel >= world.numsubmodels)
		return NULL;
	msubmodel_t *sm = &world.submodels[submodel];
	int count = 0;
	for (int i = 0; i < sm->numfaces; i++) {
		msurface_t *s = &world.surfaces[sm->firstface + i];
		if (!(s->flags & SURF_WATER) && s->numverts >= 3)
			count += s->numverts - 2;
	}
	if (!count)
		return NULL;
	float *tris = Mem_Alloc(sizeof(float) * 9 * (size_t)count), *t = tris;
	for (int i = 0; i < sm->numfaces; i++) {
		msurface_t *s = &world.surfaces[sm->firstface + i];
		if ((s->flags & SURF_WATER) || s->numverts < 3)
			continue;
		const worldvert_t *v = &world.verts[s->firstvert];
		for (int k = 1; k + 1 < s->numverts; k++, t += 9) {
			VectorCopy(v[0].xyz, t);
			VectorCopy(v[k + 1].xyz, t + 3);
			VectorCopy(v[k].xyz, t + 6);
		}
	}
	*numtris = count;
	return tris;
}

// ---------------------------------------------------------------- brush entities as convex pieces

// Jolt's meshes can't move, so a brush entity becomes convex pieces: the solid leaves of its own BSP tree
// (hull 0, the one bullets use). Each leaf is the space inside the planes on the way down to it, cut by
// the model's box; its corners are where three of those planes meet and no plane is crossed.
typedef struct { vec3_t n; float d; } halfspace_t;   // inside: n . p <= d

typedef struct {
	halfspace_t *planes;  int numplanes, planecap;
	float       *points;  int numpoints, pointcap;
	int         *counts;  int numpieces, piececap;
	vec3_t       mins, maxs;
} convexbuild_t;

static void PushPlane(convexbuild_t *cb, const vec3_t n, float d)
{
	if (cb->numplanes == cb->planecap) {
		cb->planecap = cb->planecap ? cb->planecap * 2 : 64;
		cb->planes = Mem_Realloc(cb->planes, sizeof(halfspace_t) * (size_t)cb->planecap);
	}
	VectorCopy(n, cb->planes[cb->numplanes].n);
	cb->planes[cb->numplanes++].d = d;
}

static void AddPoint(convexbuild_t *cb, const vec3_t p)
{
	if (cb->numpoints == cb->pointcap) {
		cb->pointcap = cb->pointcap ? cb->pointcap * 2 : 256;
		cb->points = Mem_Realloc(cb->points, sizeof(float) * 3 * (size_t)cb->pointcap);
	}
	VectorCopy(p, cb->points + 3 * cb->numpoints);
	cb->numpoints++;
}

static void SolidLeaf(convexbuild_t *cb)
{
	int first = cb->numpoints;
	const halfspace_t *h = cb->planes;
	int n = cb->numplanes;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) {
			vec3_t jk;
			for (int k = j + 1; k < n; k++) {
				vec3_t ij, ki, p;
				CrossProduct(h[j].n, h[k].n, jk);
				float det = DotProduct(h[i].n, jk);
				if (fabsf(det) < 1e-6f)
					continue;
				CrossProduct(h[k].n, h[i].n, ki);
				CrossProduct(h[i].n, h[j].n, ij);
				for (int a = 0; a < 3; a++)
					p[a] = (h[i].d * jk[a] + h[j].d * ki[a] + h[k].d * ij[a]) / det;
				qboolean inside = true;
				for (int m = 0; m < n && inside; m++)
					inside = DotProduct(h[m].n, p) <= h[m].d + 0.05f;
				for (int q = first; q < cb->numpoints && inside; q++) {
					vec3_t dd;
					VectorSubtract(cb->points + 3 * q, p, dd);
					inside = DotProduct(dd, dd) > 0.01f;   // not one we have
				}
				if (inside)
					AddPoint(cb, p);
			}
		}
	if (cb->numpoints - first < 4) {   // flat or nothing: not a piece
		cb->numpoints = first;
		return;
	}
	if (cb->numpieces == cb->piececap) {
		cb->piececap = cb->piececap ? cb->piececap * 2 : 16;
		cb->counts = Mem_Realloc(cb->counts, sizeof(int) * (size_t)cb->piececap);
	}
	cb->counts[cb->numpieces++] = cb->numpoints - first;
}

static void WalkHull(convexbuild_t *cb, const hull_t *hull, int num, int depth)
{
	if (num < 0) {
		if (num == CONTENTS_SOLID)
			SolidLeaf(cb);
		return;
	}
	if (num < hull->firstclipnode || num > hull->lastclipnode || depth > 1024)
		return;
	const mclipnode_t *node = &hull->clipnodes[num];
	const mplane_t *pl = &hull->planes[node->planenum];
	vec3_t neg;
	VectorScale(pl->normal, -1, neg);
	// front: n . p >= dist
	PushPlane(cb, neg, -pl->dist);
	WalkHull(cb, hull, node->children[0], depth + 1);
	cb->numplanes--;
	PushPlane(cb, pl->normal, pl->dist);
	WalkHull(cb, hull, node->children[1], depth + 1);
	cb->numplanes--;
}

static physbody_t AddBrushPieces(int submodel, const vec3_t origin, const vec3_t angles, physmotion_t motion)
{
	msubmodel_t *sm = &world.submodels[submodel];
	hull_t hull;
	SV_ModelHull(sm, 0, &hull);
	convexbuild_t cb;
	memset(&cb, 0, sizeof(cb));
	// the model's box, a little bigger, closes leaves that reach the edge of the tree
	static const vec3_t axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	for (int a = 0; a < 3; a++) {
		vec3_t neg;
		VectorScale(axes[a], -1, neg);
		PushPlane(&cb, axes[a], sm->maxs[a] + 1);
		PushPlane(&cb, neg, -(sm->mins[a] - 1));
	}
	WalkHull(&cb, &hull, hull.firstclipnode, 0);
	if (developer && developer->value > 1) {
		vec3_t lo = { 1e9f, 1e9f, 1e9f }, hi = { -1e9f, -1e9f, -1e9f };
		for (int i = 0; i < cb.numpoints; i++)
			for (int a = 0; a < 3; a++) {
				lo[a] = fminf(lo[a], cb.points[i * 3 + a]);
				hi[a] = fmaxf(hi[a], cb.points[i * 3 + a]);
			}
		Con_DPrintf("brush *%d: %d pieces, %d points, %.0f %.0f %.0f .. %.0f %.0f %.0f\n", submodel, cb.numpieces,
			cb.numpoints, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
	}
	physbody_t b = cb.numpieces ? Phys_AddConvexSet(svworld, cb.points, cb.counts, cb.numpieces, origin, angles, motion, 0)
		: PHYS_NOBODY;
	Mem_Free(cb.planes);
	Mem_Free(cb.points);
	Mem_Free(cb.counts);
	return b;
}

// the world is one static mesh of its faces (water left out); brush entities are convex pieces
static physbody_t AddBrush(int submodel, const vec3_t origin, const vec3_t angles, physmotion_t motion)
{
	if (submodel < 0 || submodel >= world.numsubmodels)
		return PHYS_NOBODY;
	if (motion != PHYS_STATIC)
		return AddBrushPieces(submodel, origin, angles, motion);
	int n;
	float *tris = BrushTriangles(submodel, &n);
	physbody_t b = tris ? Phys_AddMesh(svworld, tris, n, origin, angles, motion) : PHYS_NOBODY;
	Mem_Free(tris);
	return b;
}

static void RemoveBrush(brushbody_t *bb)
{
	if (bb->serial && bb->body != PHYS_NOBODY)
		Phys_RemoveBody(svworld, bb->body);
	memset(bb, 0, sizeof(*bb));
	bb->body = PHYS_NOBODY;
}

// solid brush entities follow their edicts; new ones join, gone ones leave
static void SyncBrushes(float dt)
{
	if (brushes_cap < sv.max_edicts) {
		brushes = Mem_Realloc(brushes, sizeof(*brushes) * (size_t)sv.max_edicts);
		memset(brushes + brushes_cap, 0, sizeof(*brushes) * (size_t)(sv.max_edicts - brushes_cap));
		brushes_cap = sv.max_edicts;
	}
	for (int i = 1; i < brushes_cap; i++) {
		brushbody_t *bb = &brushes[i];
		edict_t *e = i < sv.num_edicts ? EDICT_NUM(i) : NULL;
		model_t *m = e && !e->free && e->v.solid == SOLID_BSP ? SV_ModelForIndex(e->v.modelindex) : NULL;
		qboolean wanted = m && m->type == mod_brush && m->submodel > 0;
		if (bb->serial && (!wanted || bb->serial != e->serialnumber + 1 || bb->modelindex != e->v.modelindex))
			RemoveBrush(bb);
		if (!wanted)
			continue;
		if (!bb->serial) {
			bb->serial = e->serialnumber + 1;
			bb->modelindex = e->v.modelindex;
			bb->body = AddBrush(m->submodel, e->v.origin, e->v.angles, PHYS_KINEMATIC);
			VectorCopy(e->v.origin, bb->origin);
			VectorCopy(e->v.angles, bb->angles);
			continue;
		}
		// only moving ones are moved: a kinematic body told to stay keeps waking what rests on it
		if (bb->body != PHYS_NOBODY && (!VectorCompare(bb->origin, e->v.origin) || !VectorCompare(bb->angles, e->v.angles))) {
			Phys_MoveKinematic(svworld, bb->body, e->v.origin, e->v.angles, dt);
			VectorCopy(e->v.origin, bb->origin);
			VectorCopy(e->v.angles, bb->angles);
		} else if (bb->body != PHYS_NOBODY && Phys_IsAwake(svworld, bb->body)) {
			Phys_MoveKinematic(svworld, bb->body, e->v.origin, e->v.angles, dt);   // stops it
		}
	}
}

static physworld_t *ServerWorld(void);

physworld_t *Phys_ServerWorld(qboolean create)
{
	return create ? ServerWorld() : svworld;
}

// the map's physics world, made the first time something needs it
static physworld_t *ServerWorld(void)
{
	// also while a map loads: its own props spawn then, and a save's props come back then
	if (svworld || (!sv.active && !sv.loading) || !world.loaded)
		return svworld;
	double t0 = Sys_FloatTime();
	svworld = Phys_CreateWorld((int)phys_maxbodies->value, Cvar_VariableValue("sv_gravity"));
	if (!svworld)
		return NULL;
	static const vec3_t zero = { 0, 0, 0 };
	worldbody = AddBrush(0, zero, zero, PHYS_STATIC);
	SyncBrushes(0);
	Phys_Optimize(svworld);
	int n;
	Mem_Free(BrushTriangles(0, &n));
	Con_DPrintf("Physics world: %d triangles, %d bodies, %.1f ms\n", n, Phys_BodyCount(svworld),
		(Sys_FloatTime() - t0) * 1000);
	return svworld;
}

// ---------------------------------------------------------------- pushes

#define BLAST_SPEED   6.0f     // inches/s a light body at the middle gets per point of the explosion's damage
#define BLAST_REFMASS 20.0f    // kg; heavier bodies are thrown less
#define BLAST_MEMORY  0.3      // seconds an explosion still throws a ragdoll made after it (what it killed)
#define BULLET_PUSH   400.0f   // kg inches/s per point of a Lua bullet's damage, on the ragdoll part it hits

typedef struct {
	double time;
	vec3_t pos;
	float  radius, damage;
} blast_t;
static blast_t blasts[8];
static int     blast_next;

void Phys_ServerExplosion(const float pos[3], float radius, float damage)
{
	if (!sv.active || radius <= 0 || damage <= 0)
		return;
	blast_t *b = &blasts[blast_next++ % 8];
	b->time = sv.time;
	VectorCopy(pos, b->pos);
	b->radius = radius;
	b->damage = damage;
	if (!svworld)
		return;   // nothing to push
	Phys_Explode(svworld, pos, radius, damage * BLAST_SPEED, BLAST_REFMASS);
	Prop_Blasted(pos, radius);
}

void Phys_BlastVelocity(const float point[3], float mass, float addvel[3])
{
	VectorClear(addvel);
	for (int i = 0; i < 8; i++) {
		const blast_t *b = &blasts[i];
		if (b->radius <= 0 || sv.time - b->time > BLAST_MEMORY || sv.time < b->time)
			continue;
		vec3_t d;
		VectorSubtract(point, b->pos, d);
		float dist = VectorNormalize(d);
		if (dist >= b->radius)
			continue;
		float dv = b->damage * BLAST_SPEED * (1.0f - dist / b->radius);
		if (mass > BLAST_REFMASS)
			dv *= BLAST_REFMASS / mass;
		VectorMA(addvel, dv, d, addvel);
	}
}

void Phys_ServerBullet(const float from[3], const float to[3], float damage)
{
	if (!svworld || damage <= 0)
		return;
	vec3_t hit, dir;
	int tag = 0;
	physbody_t b = Phys_CastRay(svworld, from, to, hit, &tag);
	if (b == PHYS_NOBODY || tag >= 0)
		return;   // props hear of bullets through their damage; ragdoll parts have negative tags
	VectorSubtract(to, from, dir);
	VectorNormalize(dir);
	VectorScale(dir, damage * BULLET_PUSH, dir);
	Phys_AddImpulseAt(svworld, b, dir, hit);
}

// Each player as a body the ragdolls feel, so walking through a corpse shoves its limbs. A little
// smaller than the player's hull: it only meets ragdoll parts (PHYS_LAYER_PLAYER).
static physbody_t *playerbody;
static qboolean   *playerduck;
static int         playerbody_count;

static void SyncPlayers(float dt)
{
	if (playerbody_count < sv.maxclients + 1) {
		playerbody = Mem_Realloc(playerbody, sizeof(*playerbody) * (sv.maxclients + 1));
		playerduck = Mem_Realloc(playerduck, sizeof(*playerduck) * (sv.maxclients + 1));
		for (int i = playerbody_count; i <= sv.maxclients; i++)
			playerbody[i] = PHYS_NOBODY;
		playerbody_count = sv.maxclients + 1;
	}
	for (int c = 1; c <= sv.maxclients; c++) {
		edict_t *pl = EDICT_NUM(c);
		qboolean there = !pl->free && pl->v.health > 0 && pl->v.movetype != MOVETYPE_NOCLIP && pl->v.solid != SOLID_NOT;
		qboolean duck = (pl->v.flags & FL_DUCKING) != 0;
		if (playerbody[c] != PHYS_NOBODY && (!there || duck != playerduck[c])) {
			Phys_RemoveBody(svworld, playerbody[c]);
			playerbody[c] = PHYS_NOBODY;
		}
		if (!there)
			continue;
		if (playerbody[c] == PHYS_NOBODY) {
			float half[3] = { 14, 14, duck ? 16.0f : 34.0f };
			playerbody[c] = Phys_AddBox(svworld, half, pl->v.origin, NULL, PHYS_KINEMATIC, 0);
			if (playerbody[c] == PHYS_NOBODY)
				continue;
			Phys_SetLayer(svworld, playerbody[c], PHYS_LAYER_PLAYER);
			playerduck[c] = duck;
			continue;
		}
		// a jump in place (a teleport, a level's start) isn't a shove
		vec3_t at, d;
		Phys_GetPose(svworld, playerbody[c], at, NULL);
		VectorSubtract(pl->v.origin, at, d);
		if (DotProduct(d, d) > 64.0f * 64.0f)
			Phys_SetPose(svworld, playerbody[c], pl->v.origin, NULL);
		else
			Phys_MoveKinematic(svworld, playerbody[c], pl->v.origin, NULL, dt);
	}
}

// after the server's frame: Half-Life has moved its doors and trains; Jolt follows and steps
void Phys_ServerFrame(float dt)
{
	if (dt <= 0)
		return;
	double t0 = Sys_FloatTime();
	Ragdoll_Frame();   // may make the world
	if (!svworld)
		return;
	Phys_SetGravity(svworld, Cvar_VariableValue("sv_gravity"));
	SyncBrushes(dt);
	SyncPlayers(dt);
	Prop_BeforeStep(dt);
	Phys_Step(svworld, dt);
	Ragdoll_AfterStep();
	Prop_AfterStep();

	// phys_showtime: what physics cost, once a second
	static double total, worst, next;
	static int frames;
	double ms = (Sys_FloatTime() - t0) * 1000;
	total += ms;
	frames++;
	if (ms > worst)
		worst = ms;
	if (sv.time >= next || sv.time < next - 2) {
		if (phys_showtime->value)
			Con_Printf("physics: %.2f ms a frame, worst %.2f, %d bodies\n", total / frames, worst, Phys_BodyCount(svworld));
		total = worst = 0;
		frames = 0;
		next = sv.time + 1;
	}
}

void Phys_ServerStopped(void)
{
	Ragdoll_Clear();
	Prop_Clear();
	for (int i = 0; i < playerbody_count; i++)
		playerbody[i] = PHYS_NOBODY;
	memset(blasts, 0, sizeof(blasts));
	Phys_DestroyWorld(svworld);
	svworld = NULL;
	worldbody = PHYS_NOBODY;
	if (brushes)
		memset(brushes, 0, sizeof(*brushes) * (size_t)brushes_cap);
	box_count = 0;
}

// ---------------------------------------------------------------- test boxes

// phys_dropbox [half size 8] [mass 20]: a box in front of the player, drawn as a shaded box
static void DropBox_f(void)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl || !ServerWorld()) {
		Con_Printf("phys_dropbox: no map is running\n");
		return;
	}
	float h = Cmd_Argc() > 1 ? (float)atof(Cmd_Argv(1)) : 8;
	float mass = Cmd_Argc() > 2 ? (float)atof(Cmd_Argv(2)) : 20;
	if (h < 1) h = 1;
	vec3_t f, eye, pos;
	AngleVectors(pl->v.v_angle, f, NULL, NULL);
	VectorAdd(pl->v.origin, pl->v.view_ofs, eye);
	VectorMA(eye, 32 + h * 2, f, pos);
	vec3_t half = { h, h, h }, ang = { 0, pl->v.v_angle[1], 0 };
	physbody_t b = Phys_AddBox(svworld, half, pos, ang, PHYS_DYNAMIC, mass);
	if (b == PHYS_NOBODY)
		return;
	if (box_count == box_cap) {
		box_cap = box_cap ? box_cap * 2 : 16;
		boxes = Mem_Realloc(boxes, sizeof(*boxes) * (size_t)box_cap);
	}
	boxes[box_count].body = b;
	VectorCopy(half, boxes[box_count].half);
	box_count++;
}

// phys_boxat x y z [half 8] [mass 20]: a box at a point (for scripted tests)
static void BoxAt_f(void)
{
	if (Cmd_Argc() < 4 || !ServerWorld()) {
		Con_Printf("usage: phys_boxat x y z [half size] [mass]   (with a map running)\n");
		return;
	}
	float h = Cmd_Argc() > 4 ? (float)atof(Cmd_Argv(4)) : 8, mass = Cmd_Argc() > 5 ? (float)atof(Cmd_Argv(5)) : 20;
	vec3_t pos = { (float)atof(Cmd_Argv(1)), (float)atof(Cmd_Argv(2)), (float)atof(Cmd_Argv(3)) };
	vec3_t half = { h, h, h };
	physbody_t b = Phys_AddBox(svworld, half, pos, NULL, PHYS_DYNAMIC, mass);
	if (b == PHYS_NOBODY)
		return;
	if (box_count == box_cap) {
		box_cap = box_cap ? box_cap * 2 : 16;
		boxes = Mem_Realloc(boxes, sizeof(*boxes) * (size_t)box_cap);
	}
	boxes[box_count].body = b;
	VectorCopy(half, boxes[box_count].half);
	box_count++;
}

static void Boxes_f(void)
{
	for (int i = 0; i < box_count; i++) {
		vec3_t o, a, v;
		Phys_GetPose(svworld, boxes[i].body, o, a);
		Phys_GetVelocity(svworld, boxes[i].body, v);
		Con_Printf("box %d: at %.1f %.1f %.1f, angles %.1f %.1f %.1f, speed %.1f, %s\n", i, o[0], o[1], o[2], a[0], a[1], a[2],
			VectorLength(v), Phys_IsAwake(svworld, boxes[i].body) ? "awake" : "asleep");
	}
	for (int i = 1; Cmd_Argc() > 1 && i < brushes_cap; i++)
		if (brushes[i].serial && brushes[i].body != PHYS_NOBODY) {
			vec3_t o, a;
			Phys_GetPose(svworld, brushes[i].body, o, a);
			Con_Printf("brush %d (%s): at %.1f %.1f %.1f, angles %.1f %.1f %.1f, %s\n", i, SV_String(EDICT_NUM(i)->v.classname),
				o[0], o[1], o[2], a[0], a[1], a[2], Phys_IsAwake(svworld, brushes[i].body) ? "moving" : "still");
		}
	Con_Printf("%d box%s, %d bodies in the physics world\n", box_count, box_count == 1 ? "" : "es", Phys_BodyCount(svworld));
}

static void ClearBoxes_f(void)
{
	for (int i = 0; svworld && i < box_count; i++)
		Phys_RemoveBody(svworld, boxes[i].body);
	box_count = 0;
}

// the test boxes, shaded by which way each face looks (no textures), inside the world's view
void Phys_DrawBoxes(void)
{
	if (!svworld || !box_count)
		return;
	static const int faces[6][4] = { {0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5} };
	if (gl_shaders)
		qglUseProgram(0);
	glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	for (int i = 0; i < box_count; i++) {
		vec3_t o, a, f, r, u, c[8];
		Phys_GetPose(svworld, boxes[i].body, o, a);
		AngleVectors(a, f, r, u);
		const float *h = boxes[i].half;
		for (int k = 0; k < 8; k++) {
			float x = (k & 1) ? h[0] : -h[0], y = (k & 2) ? h[1] : -h[1], z = (k & 4) ? h[2] : -h[2];
			for (int j = 0; j < 3; j++)
				c[k][j] = o[j] + f[j] * x - r[j] * y + u[j] * z;
		}
		glBegin(GL_QUADS);
		for (int k = 0; k < 6; k++) {
			vec3_t e1, e2, n;
			VectorSubtract(c[faces[k][1]], c[faces[k][0]], e1);
			VectorSubtract(c[faces[k][2]], c[faces[k][0]], e2);
			CrossProduct(e1, e2, n);
			VectorNormalize(n);
			float l = 0.55f + 0.3f * fabsf(n[2]) + 0.15f * fabsf(n[0]);   // tops brightest
			glColor3f(0.85f * l, 0.6f * l, 0.25f * l);
			for (int j = 0; j < 4; j++)
				glVertex3fv(c[faces[k][j]]);
		}
		glEnd();
	}
	glPopAttrib();
}

void Phys_InitCommands(void)
{
	Cmd_AddCommand("phys_test", PhysTest_f, "checks the physics library: a box dropped on a floor (no map needed)");
	Cmd_AddCommand("phys_dropbox", DropBox_f, "drops a test box in front of you: [half size 8] [mass kg 20]");
	Cmd_AddCommand("phys_boxat", BoxAt_f, "a test box at x y z [half size] [mass]");
	Cmd_AddCommand("phys_boxes", Boxes_f, "lists the test boxes");
	Cmd_AddCommand("phys_clear", ClearBoxes_f, "removes the test boxes");
	phys_showtime = Cvar_Get("phys_showtime", "0", 0, "prints what physics costs each frame (milliseconds), once a second");
	Ragdoll_Init();
	Prop_Init();
	phys_maxbodies = Cvar_Get("phys_maxbodies", "8192", FCVAR_ARCHIVE, "most bodies a map's physics world holds (from the next map)");
}
