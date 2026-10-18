// newen - light and line queries against the world: what lights a model, what blocks a glow
#include "bsp.h"
#include "model.h"

// Finds where the segment first crosses a lit surface and reads the lightmap there.
// Returns 1 on a hit (color filled), 0 for an unlit hit, -1 if nothing was hit.
static int RecursiveLightPoint(mnode_t *node, const vec3_t start, const vec3_t end, int color[3], int depth)
{
	if (node->contents < 0 || depth > 1024)
		return -1;
	float front = DotProduct(start, node->plane->normal) - node->plane->dist;
	float back = DotProduct(end, node->plane->normal) - node->plane->dist;
	int side = front < 0;
	if ((back < 0) == side)
		return RecursiveLightPoint(node->children[side], start, end, color, depth + 1);

	float frac = front / (front - back);
	vec3_t mid;
	for (int i = 0; i < 3; i++)
		mid[i] = start[i] + (end[i] - start[i]) * frac;

	int r = RecursiveLightPoint(node->children[side], start, mid, color, depth + 1);
	if (r >= 0)
		return r;

	for (int i = 0; i < node->numsurfaces; i++) {
		msurface_t *s = &world.surfaces[node->firstsurface + i];
		if (s->flags & (SURF_SKY | SURF_WATER | SURF_NOLIGHT) || !s->numverts)
			continue;
		const mtexinfo_t *ti = s->texinfo;
		int ds = (int)(DotProduct(mid, ti->vecs[0]) + ti->vecs[0][3]);
		int dt = (int)(DotProduct(mid, ti->vecs[1]) + ti->vecs[1][3]);
		if (ds < s->texturemins[0] || dt < s->texturemins[1])
			continue;
		ds -= s->texturemins[0];
		dt -= s->texturemins[1];
		if (ds > s->extents[0] || dt > s->extents[1])
			continue;
		if (!s->samples) {
			int v = world.lightdata ? 0 : 255 * 256;
			color[0] = color[1] = color[2] = v;
			return 0;
		}
		ds >>= 4;
		dt >>= 4;
		const unsigned char *lm = s->samples + (dt * s->lm_w + ds) * 3;
		int size = s->lm_w * s->lm_h * 3;
		color[0] = color[1] = color[2] = 0;
		for (int m = 0; m < 4 && s->styles[m] != 255; m++, lm += size) {
			int scale = lightstyle_value[s->styles[m] & 63];
			for (int c = 0; c < 3; c++)
				color[c] += lm[c] * scale;
		}
		return 1;
	}
	return RecursiveLightPoint(node->children[!side], mid, end, color, depth + 1);
}

void R_LightPoint(const vec3_t p, vec3_t out)
{
	if (!world.loaded || !world.numnodes || !world.lightdata) {
		VectorSet(out, 1, 1, 1);
		return;
	}
	vec3_t end = { p[0], p[1], p[2] - 2048 };
	int color[3] = { 0, 0, 0 };
	if (RecursiveLightPoint(&world.nodes[0], p, end, color, 0) < 0) {
		VectorSet(out, 0.5f, 0.5f, 0.5f);   // nothing below (the sky or a hole): mid grey
		return;
	}
	// same curve as the lightmaps, so models match the floor they stand on
	for (int c = 0; c < 3; c++) {
		int v = color[c] >> 8;
		out[c] = lightgammatable[v > 1023 ? 1023 : v] / 255.0f;
	}
}

static qboolean RecursiveLineBlocked(mnode_t *node, const vec3_t p1, const vec3_t p2, int depth)
{
	if (node->contents < 0)
		return node->contents == CONTENTS_SOLID;
	if (depth > 1024)
		return false;
	float d1 = DotProduct(p1, node->plane->normal) - node->plane->dist;
	float d2 = DotProduct(p2, node->plane->normal) - node->plane->dist;
	if (d1 >= 0 && d2 >= 0)
		return RecursiveLineBlocked(node->children[0], p1, p2, depth + 1);
	if (d1 < 0 && d2 < 0)
		return RecursiveLineBlocked(node->children[1], p1, p2, depth + 1);
	float frac = d1 / (d1 - d2);
	vec3_t mid;
	for (int i = 0; i < 3; i++)
		mid[i] = p1[i] + (p2[i] - p1[i]) * frac;
	int side = d1 < 0;
	return RecursiveLineBlocked(node->children[side], p1, mid, depth + 1) ||
		RecursiveLineBlocked(node->children[!side], mid, p2, depth + 1);
}

qboolean R_LineBlocked(const vec3_t start, const vec3_t end)
{
	if (!world.loaded || !world.numnodes)
		return false;
	return RecursiveLineBlocked(&world.nodes[0], start, end, 0);
}

// ---------------------------------------------------------------- dynamic lights

#define MAX_DLIGHTS 32   // one bit each in a surface's dlightbits

typedef struct {
	vec3_t origin;
	float  radius, minlight;
	float  color[3];
} rdlight_t;

static rdlight_t rdlights[MAX_DLIGHTS];
static int rdlight_count;
static vec3_t rdlight_local[MAX_DLIGHTS];   // the lights in the space of what's being drawn

void R_ClearDlights(void)
{
	rdlight_count = 0;
}

void R_AddDlight(const float *origin, float radius, const float *color, float minlight)
{
	if (rdlight_count >= MAX_DLIGHTS || radius <= 0)
		return;
	rdlight_t *l = &rdlights[rdlight_count++];
	VectorCopy(origin, l->origin);
	l->radius = radius;
	l->minlight = minlight;
	VectorCopy(color, l->color);
}

static void MarkLights(const rdlight_t *l, unsigned bit, mnode_t *node, int depth)
{
	while (node && node->contents >= 0 && depth < 1024) {
		float dist = DotProduct(l->origin, node->plane->normal) - node->plane->dist;
		if (dist > l->radius) {
			node = node->children[0];
		} else if (dist < -l->radius) {
			node = node->children[1];
		} else {
			for (int i = 0; i < node->numsurfaces; i++) {
				msurface_t *s = &world.surfaces[node->firstsurface + i];
				if (s->dlightframe != r_refdef.framecount) {
					s->dlightbits = 0;
					s->dlightframe = r_refdef.framecount;
				}
				s->dlightbits |= bit;
			}
			MarkLights(l, bit, node->children[0], depth + 1);
			node = node->children[1];
		}
		depth++;
	}
}

// Brush entities keep their faces in their own space; their lightmaps are built while they draw,
// so the lights are moved into that space first (NULL: back to the world's).
void R_DlightSpace(const float *origin, const float *angles)
{
	for (int i = 0; i < rdlight_count; i++) {
		float *o = rdlight_local[i];
		VectorCopy(rdlights[i].origin, o);
		if (!origin)
			continue;
		VectorSubtract(o, origin, o);
		if (angles && (angles[0] || angles[1] || angles[2])) {
			vec3_t f, r, u, tmp;
			AngleVectors(angles, f, r, u);
			VectorCopy(o, tmp);
			o[0] = DotProduct(tmp, f);
			o[1] = -DotProduct(tmp, r);
			o[2] = DotProduct(tmp, u);
		}
	}
}

// a brush entity's face: marked by plane distance (its faces aren't in the world's tree)
void R_MarkSurfaceDlights(msurface_t *s)
{
	for (int i = 0; i < rdlight_count; i++) {
		float dist = DotProduct(rdlight_local[i], s->plane->normal) - s->plane->dist;
		if (fabsf(dist) >= rdlights[i].radius)
			continue;
		if (s->dlightframe != r_refdef.framecount) {
			s->dlightbits = 0;
			s->dlightframe = r_refdef.framecount;
		}
		s->dlightbits |= 1u << i;
	}
}

void R_PushDlights(void)
{
	R_DlightSpace(NULL, NULL);
	if (!world.numnodes)
		return;
	for (int i = 0; i < rdlight_count; i++)
		MarkLights(&rdlights[i], 1u << i, &world.nodes[0], 0);
}

// adds light falling off with distance across the face, like Quake's dynamic lights
void R_SurfaceDlights(const msurface_t *s, int *blocks)
{
	const mtexinfo_t *ti = s->texinfo;
	for (int i = 0; i < rdlight_count; i++) {
		if (!(s->dlightbits & (1u << i)))
			continue;
		const rdlight_t *l = &rdlights[i];
		const float *lo = rdlight_local[i];
		float dist = DotProduct(lo, s->plane->normal) - s->plane->dist;
		float rad = l->radius - fabsf(dist);
		if (rad < l->minlight)
			continue;
		float minlight = rad - l->minlight;
		vec3_t impact;
		VectorMA(lo, -dist, s->plane->normal, impact);
		float ls = DotProduct(impact, ti->vecs[0]) + ti->vecs[0][3] - s->texturemins[0];
		float lt = DotProduct(impact, ti->vecs[1]) + ti->vecs[1][3] - s->texturemins[1];
		int *b = blocks;
		for (int t = 0; t < s->lm_h; t++) {
			float td = fabsf(lt - t * 16);
			for (int u = 0; u < s->lm_w; u++, b += 3) {
				float sd = fabsf(ls - u * 16);
				float d = sd > td ? sd + td * 0.5f : td + sd * 0.5f;   // cheap distance, as Quake does
				if (d < minlight) {
					float add = (rad - d) * 256;
					for (int c = 0; c < 3; c++)
						b[c] += (int)(add * l->color[c]);
				}
			}
		}
	}
}

// models: the lights near them add to the floor light
void R_DlightPoint(const vec3_t p, vec3_t color)
{
	for (int i = 0; i < rdlight_count; i++) {
		const rdlight_t *l = &rdlights[i];
		vec3_t d;
		VectorSubtract(p, l->origin, d);
		float dist = VectorLength(d);
		if (dist >= l->radius)
			continue;
		float add = (l->radius - dist) / 255.0f;
		for (int c = 0; c < 3; c++)
			color[c] += add * l->color[c];
	}
}
