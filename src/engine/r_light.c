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
	int    id;         // stays the same while the light lives (the shadow choice sticks to it)
	qboolean shadow;   // may cast shadows (muzzle flashes and the classic flashlight don't)
	qboolean spot;     // modern only: a beam along dir (world axes), tan(half angle) wide; radius is its range
	vec3_t dir;
	float  tanhalf;
} rdlight_t;

static rdlight_t rdlights[MAX_DLIGHTS];
static int rdlight_count;
static int client_count;   // the client's lights; the test light goes after them each frame
static vec3_t rdlight_local[MAX_DLIGHTS];   // the lights in the space of what's being drawn

// r_testlight: one light the engine owns, so lighting can be checked without a game or client DLL
static qboolean testlight_on;
static vec3_t testlight_org;
static float testlight_radius, testlight_color[3];
static cvar_t *r_testlight_orbit;

// spotlights: the flashlight (set by the client each frame it's on) and r_testspot
static cvar_t *r_flashlight_fov, *r_flashlight_range, *r_flashlight_brightness, *r_flashlight_cone;
static int flashlight_frame = -1;
static vec3_t flashlight_org, flashlight_dir;
static int testspot_mode;   // 0 off, 1 fixed where it was placed, 2 follows the view
static vec3_t testspot_org, testspot_dir;
static float testspot_fov, testspot_range;

void R_ClearDlights(void)
{
	rdlight_count = client_count = 0;
}

void R_AddDlight(const float *origin, float radius, const float *color, float minlight, int id, qboolean shadow)
{
	if (rdlight_count >= MAX_DLIGHTS || radius <= 0)
		return;
	rdlight_t *l = &rdlights[rdlight_count++];
	VectorCopy(origin, l->origin);
	l->radius = radius;
	l->minlight = minlight;
	VectorCopy(color, l->color);
	l->id = id;
	l->shadow = shadow;
	l->spot = false;
	client_count = rdlight_count;
}

int R_DlightCount(void)
{
	return rdlight_count;
}

void R_GetDlight(int i, float *local_origin, float *radius, float *color)
{
	VectorCopy(rdlight_local[i], local_origin);
	*radius = rdlights[i].radius;
	VectorCopy(rdlights[i].color, color);
}

int R_DlightShadowId(int i)
{
	return rdlights[i].shadow ? rdlights[i].id : 0;
}

// the light in world space (R_GetDlight gives it in the space being drawn)
void R_GetDlightWorld(int i, float *origin, float *radius, float *color)
{
	VectorCopy(rdlights[i].origin, origin);
	*radius = rdlights[i].radius;
	VectorCopy(rdlights[i].color, color);
}

static void AddTestLight(void)
{
	if (!testlight_on || rdlight_count >= MAX_DLIGHTS)
		return;
	vec3_t org;
	VectorCopy(testlight_org, org);
	float orbit = r_testlight_orbit->value;
	if (orbit > 0) {
		// once round every 4 seconds, level with the point
		float a = (float)(r_refdef.time * M_PI_F * 0.5);
		org[0] += cosf(a) * orbit;
		org[1] += sinf(a) * orbit;
	}
	rdlight_t *l = &rdlights[rdlight_count++];
	VectorCopy(org, l->origin);
	l->radius = testlight_radius;
	l->minlight = 0;
	VectorCopy(testlight_color, l->color);
	l->id = R_TESTLIGHT_ID;
	l->shadow = true;
	l->spot = false;
}

qboolean R_DlightIsSpot(int i, float *dir, float *tanhalf)
{
	if (!rdlights[i].spot)
		return false;
	VectorCopy(rdlights[i].dir, dir);
	*tanhalf = rdlights[i].tanhalf;
	return true;
}

// right and up across a beam along dir (the shadow map's view and the shader use the same pair)
void R_SpotBasis(const float *dir, float *right, float *up)
{
	vec3_t worldup = { 0, 0, 1 };
	if (fabsf(dir[2]) > 0.99f)
		VectorSet(worldup, 1, 0, 0);
	CrossProduct(dir, worldup, right);
	VectorNormalize(right);
	CrossProduct(right, dir, up);
}

// The cone flashlight needs the modern renderer and a fifth texture unit for its pattern
qboolean R_FlashlightCone(void)
{
	return r_modern && r_flashlight_cone->value && R_SpotlightsAvailable();
}

void R_SetFlashlight(const float *org, const float *dir)
{
	VectorCopy(org, flashlight_org);
	VectorCopy(dir, flashlight_dir);
	VectorNormalize(flashlight_dir);
	flashlight_frame = r_refdef.framecount;
}

static void AddSpot(const vec3_t org, const vec3_t dir, float fov, float range, const float *color, int id)
{
	if (rdlight_count >= MAX_DLIGHTS || range <= 0)
		return;
	if (fov < 1) fov = 1;
	if (fov > 170) fov = 170;
	rdlight_t *l = &rdlights[rdlight_count++];
	VectorCopy(org, l->origin);
	l->radius = range;
	l->minlight = 0;
	VectorCopy(color, l->color);
	l->id = id;
	l->shadow = true;
	l->spot = true;
	VectorCopy(dir, l->dir);
	VectorNormalize(l->dir);
	l->tanhalf = tanf(DEG2RAD(fov * 0.5f));
}

static void AddSpots(void)
{
	if (!R_FlashlightCone())
		return;
	float b = r_flashlight_brightness->value;
	vec3_t color = { b, b * 0.97f, b * 0.9f };   // a little warm, like an incandescent bulb
	if (flashlight_frame == r_refdef.framecount)
		AddSpot(flashlight_org, flashlight_dir, r_flashlight_fov->value, r_flashlight_range->value, color, R_FLASHLIGHT_ID);
	if (testspot_mode == 2) {
		VectorCopy(r_refdef.origin, testspot_org);
		VectorCopy(r_refdef.forward, testspot_dir);
	}
	if (testspot_mode)
		AddSpot(testspot_org, testspot_dir, testspot_fov, testspot_range, color, R_TESTSPOT_ID);
}

static void TestSpot_f(void)
{
	int argc = Cmd_Argc();
	const char *what = argc > 1 ? Cmd_Argv(1) : "";
	if (!stricmp(what, "off")) {
		testspot_mode = 0;
	} else if (!stricmp(what, "here") || !stricmp(what, "follow")) {
		testspot_mode = !stricmp(what, "here") ? 1 : 2;
		VectorCopy(r_refdef.origin, testspot_org);
		VectorCopy(r_refdef.forward, testspot_dir);
		testspot_fov = argc > 2 ? (float)atof(Cmd_Argv(2)) : r_flashlight_fov->value;
		testspot_range = argc > 3 ? (float)atof(Cmd_Argv(3)) : r_flashlight_range->value;
	} else {
		Con_Printf("usage: r_testspot here|follow [angle range] | off   (modern renderer)\n");
		return;
	}
	if (testspot_mode && !R_FlashlightCone())
		Con_Warnf("Spotlights need r_renderer modern (and r_flashlight_cone 1)\n");
}

static void TestLight_f(void)
{
	int argc = Cmd_Argc();
	if (argc == 2 && !stricmp(Cmd_Argv(1), "off")) {
		testlight_on = false;
		return;
	}
	if (argc >= 2 && !stricmp(Cmd_Argv(1), "here")) {
		VectorCopy(r_refdef.origin, testlight_org);
		testlight_radius = argc > 2 ? (float)atof(Cmd_Argv(2)) : 300;
		VectorSet(testlight_color, 1, 1, 1);
		testlight_on = true;
	} else if (argc >= 5) {
		for (int i = 0; i < 3; i++)
			testlight_org[i] = (float)atof(Cmd_Argv(1 + i));
		testlight_radius = (float)atof(Cmd_Argv(4));
		VectorSet(testlight_color, 1, 1, 1);
		if (argc >= 8)
			for (int i = 0; i < 3; i++)
				testlight_color[i] = (float)atof(Cmd_Argv(5 + i));
		testlight_on = true;
	} else if (argc != 1) {
		Con_Printf("usage: r_testlight <x y z radius> [r g b 0-1] | here [radius] | off\n");
		return;
	}
	if (testlight_on)
		Con_Printf("test light at %.0f %.0f %.0f, radius %.0f, colour %.2f %.2f %.2f%s\n", testlight_org[0], testlight_org[1],
			testlight_org[2], testlight_radius, testlight_color[0], testlight_color[1], testlight_color[2],
			r_testlight_orbit->value > 0 ? " (orbiting)" : "");
	else
		Con_Printf("no test light\n");
}

void R_InitLights(void)
{
	r_testlight_orbit = Cvar_Get("r_testlight_orbit", "0", 0, "the test light circles its point at this distance");
	r_flashlight_cone = Cvar_Get("r_flashlight_cone", "1", FCVAR_ARCHIVE, "modern: the flashlight is a beam with a shadow (0 = classic's spot of light)");
	r_flashlight_fov = Cvar_Get("r_flashlight_fov", "30", FCVAR_ARCHIVE, "modern: the flashlight beam's width in degrees");
	r_flashlight_range = Cvar_Get("r_flashlight_range", "1000", FCVAR_ARCHIVE, "modern: how far the flashlight reaches");
	r_flashlight_brightness = Cvar_Get("r_flashlight_brightness", "1", FCVAR_ARCHIVE, "modern: the flashlight's brightness");
	Cmd_AddCommand("r_testspot", TestSpot_f, "r_testspot here|follow [angle range] | off - an engine-owned spotlight for testing (modern)");
	Cmd_AddCommand("r_testlight", TestLight_f, "r_testlight <x y z radius> [r g b] | here [radius] | off - an engine-owned dynamic light for testing");
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
	rdlight_count = client_count;
	AddTestLight();
	AddSpots();
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
		if (!(s->dlightbits & (1u << i)) || rdlights[i].spot)
			continue;   // a beam isn't a sphere: only the modern light passes draw spotlights
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
		if (l->spot)
			continue;
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
