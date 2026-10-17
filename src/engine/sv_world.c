// newen - server collision: point contents, hull traces against the world and entities, linking
//
// Traces walk the map's collision hulls (the BSP clipnodes, one tree per player size) the way
// Quake-family engines do. Entities that aren't brush models get a temporary box hull. There is
// no spatial tree for entities yet: a trace checks every linked edict, which is cheap at
// Half-Life's entity counts.
#include "server.h"

vec3_t hull_mins[MAX_HULLS], hull_maxs[MAX_HULLS];

static hull_t box_hull;
static mclipnode_t box_clipnodes[6];
static mplane_t box_planes[6];
static link_t linked_marker;   // area.prev points here while an edict is linked

#define DIST_EPSILON 0.03125f

// ---------------------------------------------------------------- hulls

static void InitBoxHull(void)
{
	box_hull.clipnodes = box_clipnodes;
	box_hull.planes = box_planes;
	box_hull.firstclipnode = 0;
	box_hull.lastclipnode = 5;
	for (int i = 0; i < 6; i++) {
		box_clipnodes[i].planenum = i;
		int side = i & 1;
		box_clipnodes[i].children[side] = CONTENTS_EMPTY;
		box_clipnodes[i].children[side ^ 1] = i != 5 ? i + 1 : CONTENTS_SOLID;
		memset(&box_planes[i], 0, sizeof(box_planes[i]));
		box_planes[i].type = i >> 1;
		box_planes[i].normal[i >> 1] = 1;
	}
}

hull_t *SV_HullForBox(const vec3_t mins, const vec3_t maxs)
{
	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];
	return &box_hull;
}

void SV_InitWorld(void)
{
	static const float defmins[4][3] = { { 0, 0, 0 }, { -16, -16, -36 }, { -32, -32, -32 }, { -16, -16, -18 } };
	static const float defmaxs[4][3] = { { 0, 0, 0 }, { 16, 16, 36 }, { 32, 32, 32 }, { 16, 16, 18 } };
	// the map's clip hulls were compiled for these sizes; the game's GetHullBounds is for player
	// movement (different numbering) and must not change them
	for (int i = 0; i < MAX_HULLS; i++) {
		VectorCopy(defmins[i], hull_mins[i]);
		VectorCopy(defmaxs[i], hull_maxs[i]);
	}
	InitBoxHull();
}

// a brush model's hull, with the bounds of that hull's player size
void SV_ModelHull(const msubmodel_t *sm, int h, hull_t *out)
{
	out->clipnodes = h == 0 ? world.clipnodes0 : world.clipnodes;
	out->planes = world.planes;
	out->firstclipnode = sm->headnodes[h];
	out->lastclipnode = (h == 0 ? world.numnodes : world.numclipnodes) - 1;
	VectorCopy(hull_mins[h], out->clip_mins);
	VectorCopy(hull_maxs[h], out->clip_maxs);
}

static int HullForSize(const vec3_t mins, const vec3_t maxs)
{
	vec3_t size;
	VectorSubtract(maxs, mins, size);
	if (size[0] <= 8)
		return 0;
	if (size[0] <= 36)
		return size[2] <= 36 ? 3 : 1;
	return 2;
}

hull_t *SV_HullForBsp(edict_t *ent, const vec3_t mins, const vec3_t maxs, vec3_t offset)
{
	static hull_t hull;
	model_t *m = SV_ModelForIndex(ent->v.modelindex);
	int sub = (m && m->type == mod_brush) ? m->submodel : 0;
	int h = HullForSize(mins, maxs);
	SV_ModelHull(&world.submodels[sub], h, &hull);
	VectorSubtract(hull.clip_mins, mins, offset);
	VectorAdd(offset, ent->v.origin, offset);
	return &hull;
}

static hull_t *HullForEntity(edict_t *ent, const vec3_t mins, const vec3_t maxs, vec3_t offset)
{
	if (ent->v.solid == SOLID_BSP) {
		model_t *m = SV_ModelForIndex(ent->v.modelindex);
		if (m && m->type == mod_brush)
			return SV_HullForBsp(ent, mins, maxs, offset);
		// a non-brush model marked SOLID_BSP: GoldSrc errors out, a box is kinder
	}
	vec3_t hullmins, hullmaxs;
	VectorSubtract(ent->v.mins, maxs, hullmins);
	VectorSubtract(ent->v.maxs, mins, hullmaxs);
	VectorCopy(ent->v.origin, offset);
	return SV_HullForBox(hullmins, hullmaxs);
}

// ---------------------------------------------------------------- point contents

int SV_HullPointContents(const hull_t *hull, int num, const vec3_t p)
{
	int guard = 0;
	while (num >= 0) {
		if (num < hull->firstclipnode || num > hull->lastclipnode || ++guard > 65536)
			return CONTENTS_SOLID;
		const mclipnode_t *node = &hull->clipnodes[num];
		const mplane_t *plane = &hull->planes[node->planenum];
		float d = plane->type < 3 ? p[plane->type] - plane->dist : DotProduct(plane->normal, p) - plane->dist;
		num = node->children[d < 0];
	}
	return num;
}

int SV_TruePointContents(const vec3_t p)
{
	if (!world.loaded || !world.numnodes)
		return CONTENTS_EMPTY;
	hull_t h;
	SV_ModelHull(&world.submodels[0], 0, &h);
	return SV_HullPointContents(&h, h.firstclipnode, p);
}

int SV_PointContents(const vec3_t p)
{
	int c = SV_TruePointContents(p);
	if (c <= CONTENTS_CURRENT_0 && c >= CONTENTS_CURRENT_DOWN)
		c = CONTENTS_WATER;   // currents are water to the game
	return c;
}

// ---------------------------------------------------------------- tracing

qboolean SV_RecursiveHullCheck(const hull_t *hull, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2,
	trace_t *trace, int depth)
{
	if (num < 0) {
		if (num != CONTENTS_SOLID) {
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		} else {
			trace->startsolid = true;
		}
		return true;
	}
	if (num < hull->firstclipnode || num > hull->lastclipnode || depth > 2048)
		return true;   // damaged tree: treat as open rather than loop

	const mclipnode_t *node = &hull->clipnodes[num];
	const mplane_t *plane = &hull->planes[node->planenum];
	float t1, t2;
	if (plane->type < 3) {
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	} else {
		t1 = DotProduct(plane->normal, p1) - plane->dist;
		t2 = DotProduct(plane->normal, p2) - plane->dist;
	}
	if (t1 >= 0 && t2 >= 0)
		return SV_RecursiveHullCheck(hull, node->children[0], p1f, p2f, p1, p2, trace, depth + 1);
	if (t1 < 0 && t2 < 0)
		return SV_RecursiveHullCheck(hull, node->children[1], p1f, p2f, p1, p2, trace, depth + 1);

	// split, keeping the impact point a hair on the near side of the plane
	float frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	float midf = p1f + (p2f - p1f) * frac;
	vec3_t mid;
	for (int i = 0; i < 3; i++)
		mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	int side = t1 < 0;

	if (!SV_RecursiveHullCheck(hull, node->children[side], p1f, midf, p1, mid, trace, depth + 1))
		return false;
	if (SV_HullPointContents(hull, node->children[side ^ 1], mid) != CONTENTS_SOLID)
		return SV_RecursiveHullCheck(hull, node->children[side ^ 1], midf, p2f, mid, p2, trace, depth + 1);
	if (trace->allsolid)
		return false;   // never got out of the solid area

	// the other side is solid: this is the impact
	if (!side) {
		VectorCopy(plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	} else {
		VectorScale(plane->normal, -1, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}
	while (SV_HullPointContents(hull, hull->firstclipnode, mid) == CONTENTS_SOLID) {
		// rounding put the point inside; back off
		frac -= 0.1f;
		if (frac < 0) {
			trace->fraction = midf;
			VectorCopy(mid, trace->endpos);
			return false;
		}
		midf = p1f + (p2f - p1f) * frac;
		for (int i = 0; i < 3; i++)
			mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	}
	trace->fraction = midf;
	VectorCopy(mid, trace->endpos);
	return false;
}

trace_t SV_ClipMoveToEntity(edict_t *ent, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end)
{
	trace_t trace;
	memset(&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.allsolid = true;
	VectorCopy(end, trace.endpos);

	vec3_t offset, start_l, end_l;
	hull_t *hull = HullForEntity(ent, mins, maxs, offset);
	VectorSubtract(start, offset, start_l);
	VectorSubtract(end, offset, end_l);

	// rotated brush entities: trace in their own space
	qboolean rotated = ent->v.solid == SOLID_BSP && (ent->v.angles[0] || ent->v.angles[1] || ent->v.angles[2]);
	vec3_t f, r, u;
	if (rotated) {
		AngleVectors(ent->v.angles, f, r, u);
		vec3_t t;
		VectorCopy(start_l, t);
		start_l[0] = DotProduct(t, f); start_l[1] = -DotProduct(t, r); start_l[2] = DotProduct(t, u);
		VectorCopy(end_l, t);
		end_l[0] = DotProduct(t, f); end_l[1] = -DotProduct(t, r); end_l[2] = DotProduct(t, u);
	}

	SV_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l, &trace, 0);

	if (rotated && trace.fraction != 1) {
		vec3_t n;
		VectorCopy(trace.plane.normal, n);
		for (int i = 0; i < 3; i++)
			trace.plane.normal[i] = f[i] * n[0] - r[i] * n[1] + u[i] * n[2];
	}
	if (trace.fraction != 1) {
		for (int i = 0; i < 3; i++)
			trace.endpos[i] = start[i] + trace.fraction * (end[i] - start[i]);
	}
	if (trace.fraction < 1 || trace.startsolid)
		trace.ent = ent;
	return trace;
}

trace_t SV_Move(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int type, edict_t *passedict)
{
	// 0x100 asks traces to pass through glass (translucent brush entities)
	qboolean ignoreglass = (type & 0x100) != 0;
	type &= 0xff;
	trace_t tr = SV_ClipMoveToEntity(sv.edicts, start, mins, maxs, end);
	if (tr.allsolid)
		return tr;

	// missiles hit monsters with a bigger box
	vec3_t mins2, maxs2;
	if (type == MOVE_MISSILE) {
		VectorSet(mins2, -15, -15, -15);
		VectorSet(maxs2, 15, 15, 15);
	} else {
		VectorCopy(mins, mins2);
		VectorCopy(maxs, maxs2);
	}
	vec3_t boxmins, boxmaxs;
	for (int i = 0; i < 3; i++) {
		float lo = start[i] < end[i] ? start[i] : end[i];
		float hi = start[i] < end[i] ? end[i] : start[i];
		boxmins[i] = lo + (mins2[i] < mins[i] ? mins2[i] : mins[i]) - 1;
		boxmaxs[i] = hi + (maxs2[i] > maxs[i] ? maxs2[i] : maxs[i]) + 1;
	}

	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *touch = EDICT_NUM(i);
		if (touch->free || !ED_Linked(touch) || touch == passedict)
			continue;
		if (touch->v.solid == SOLID_NOT || touch->v.solid == SOLID_TRIGGER)
			continue;
		if (type == MOVE_NOMONSTERS && touch->v.solid != SOLID_BSP)
			continue;
		if (ignoreglass && touch->v.rendermode != kRenderNormal && !(touch->v.flags & FL_WORLDBRUSH))
			continue;
		if (boxmins[0] > touch->v.absmax[0] || boxmins[1] > touch->v.absmax[1] || boxmins[2] > touch->v.absmax[2] ||
			boxmaxs[0] < touch->v.absmin[0] || boxmaxs[1] < touch->v.absmin[1] || boxmaxs[2] < touch->v.absmin[2])
			continue;
		if (passedict && passedict->v.size[0] && !touch->v.size[0])
			continue;   // points never interact
		if (passedict && (touch->v.owner == passedict || passedict->v.owner == touch))
			continue;   // a missile doesn't hit its shooter
		// monster clip brushes only block monsters
		if ((touch->v.flags & FL_MONSTERCLIP) && !(passedict && (passedict->v.flags & FL_MONSTERCLIP)))
			continue;
		if (gNewDLLFunctions.pfnShouldCollide && passedict && !gNewDLLFunctions.pfnShouldCollide(touch, passedict))
			continue;

		trace_t t;
		if (touch->v.flags & FL_MONSTER)
			t = SV_ClipMoveToEntity(touch, start, mins2, maxs2, end);
		else
			t = SV_ClipMoveToEntity(touch, start, mins, maxs, end);
		if (t.allsolid || t.startsolid || t.fraction < tr.fraction) {
			t.ent = touch;
			if (tr.startsolid) {
				tr = t;
				tr.startsolid = true;
			} else {
				tr = t;
			}
		} else if (t.startsolid) {
			tr.startsolid = true;
		}
		if (tr.allsolid)
			break;
	}
	return tr;
}

edict_t *SV_TestEntityPosition(edict_t *e)
{
	trace_t tr = SV_Move(e->v.origin, e->v.mins, e->v.maxs, e->v.origin, MOVE_NORMAL, e);
	if (tr.startsolid)
		return tr.ent ? tr.ent : sv.edicts;
	return NULL;
}

// ---------------------------------------------------------------- linking

void SV_UnlinkEdict(edict_t *e)
{
	e->area.prev = e->area.next = NULL;
}

static void FindTouchedLeafs(edict_t *e, mnode_t *node, int depth)
{
	while (node && depth < 2048) {
		if (node->contents == CONTENTS_SOLID)
			return;
		if (node->contents < 0) {
			if (e->num_leafs >= MAX_ENT_LEAFS) {
				e->num_leafs = MAX_ENT_LEAFS + 1;   // too many: fall back to the headnode
				return;
			}
			e->leafnums[e->num_leafs++] = (short)(node - world.leafs - 1);
			return;
		}
		const mplane_t *p = node->plane;
		vec3_t nearp, farp;
		for (int i = 0; i < 3; i++) {
			nearp[i] = p->normal[i] >= 0 ? e->v.absmin[i] : e->v.absmax[i];
			farp[i] = p->normal[i] >= 0 ? e->v.absmax[i] : e->v.absmin[i];
		}
		float dnear = DotProduct(nearp, p->normal) - p->dist;
		float dfar = DotProduct(farp, p->normal) - p->dist;
		if (dnear >= 0) {
			node = node->children[0];
		} else if (dfar < 0) {
			node = node->children[1];
		} else {
			if (e->headnode == -1)
				e->headnode = (int)(node - world.nodes);   // the node that splits the box
			FindTouchedLeafs(e, node->children[0], depth + 1);
			node = node->children[1];
		}
		depth++;
	}
}

static void TouchLinks(edict_t *ent)
{
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *touch = EDICT_NUM(i);
		if (touch == ent || touch->free || !ED_Linked(touch) || touch->v.solid != SOLID_TRIGGER)
			continue;
		if (ent->v.absmin[0] > touch->v.absmax[0] || ent->v.absmin[1] > touch->v.absmax[1] ||
			ent->v.absmin[2] > touch->v.absmax[2] || ent->v.absmax[0] < touch->v.absmin[0] ||
			ent->v.absmax[1] < touch->v.absmin[1] || ent->v.absmax[2] < touch->v.absmin[2])
			continue;
		// brush triggers can be any shape, not just their box
		model_t *m = SV_ModelForIndex(touch->v.modelindex);
		if (m && m->type == mod_brush) {
			vec3_t offset, test;
			hull_t *hull = SV_HullForBsp(touch, ent->v.mins, ent->v.maxs, offset);
			VectorSubtract(ent->v.origin, offset, test);
			if (SV_HullPointContents(hull, hull->firstclipnode, test) != CONTENTS_SOLID)
				continue;
		}
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnTouch(touch, ent);
		if (ent->free)
			return;
	}
}

void SV_LinkEdict(edict_t *e, qboolean touch_triggers)
{
	if (e == sv.edicts || e->free)
		return;
	SV_UnlinkEdict(e);
	gEntityInterface.pfnSetAbsBox(e);   // the game computes absmin/absmax

	// leafs, for PVS checks
	e->num_leafs = 0;
	e->headnode = -1;
	if (e->v.movetype == MOVETYPE_FOLLOW && e->v.aiment && !e->v.aiment->free) {
		edict_t *a = e->v.aiment;
		e->num_leafs = a->num_leafs;
		e->headnode = a->headnode;
		memcpy(e->leafnums, a->leafnums, sizeof(e->leafnums));
	} else if (e->v.modelindex && world.numnodes) {
		FindTouchedLeafs(e, &world.nodes[0], 0);
		if (e->num_leafs <= MAX_ENT_LEAFS)
			e->headnode = -1;
	}

	e->area.prev = e->area.next = &linked_marker;
	if (touch_triggers && e->v.solid != SOLID_TRIGGER)
		TouchLinks(e);
}
