// newen - shadow maps for the modern renderer
//
// Each chosen point light (r_shadow_lights of them: the test light first, then the brightest nearby ones that may
// cast) renders the world's depth into six 90-degree views, kept as six tiles of one depth texture (3 across, 2 down). The light shader picks the tile from the
// direction to the light and compares distances itself, so no cube-map shadow samplers are needed. The shadow
// only dims that light's own contribution: the baked lightmaps already have their shadows.
//
// The faces, in tile order (the light shader has the same table): +X -X +Y / -Y +Z -Z. Each view looks along
// F with R to the right and U up, so a point d from the light lands at 0.5 + 0.5 * (d.R, d.U) / |d.F|.
#include "bsp.h"
#include "model.h"

static const float face_f[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float face_r[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
static const float face_u[6][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 } };

#define SHADOW_NEAR 1.0f

static cvar_t *r_shadows, *r_shadow_size, *r_shadow_bias, *r_shadow_debug, *r_shadow_models, *r_shadow_lights;
static cvar_t *r_shadow_soft, *r_shadow_minradius;

// one shadow map per chosen light; the textures are kept between frames
#define MAX_SHADOW_SLOTS 8   // the most r_shadow_lights may ask for
typedef struct {
	GLuint fbo, tex;
	int size;                 // one view's size; the texture is 3x by 2x that
	qboolean failed;          // don't try again every frame
	int light;                // this frame's light, -1 = unused
	int id;                   // the light's id, for next frame's choice
	float zfar;
} shadowslot_t;
static shadowslot_t slots[MAX_SHADOW_SLOTS];
static int slots_used;
static int last_ids[MAX_SHADOW_SLOTS], last_ids_count;
static int warned_solid_frame = -1000;

// casters: faces collected for this frame's light, then sorted into batches (one per entity and '{' texture)
typedef struct {
	const msurface_t *surf;
	int ent;                    // -1 = the world, else an index into casterents
} castsurf_t;
typedef struct {
	int first, count;           // into casters
	const texture_t *tex;       // alpha-tested with this texture; NULL = opaque
	int ent;
} casterbatch_t;
typedef struct {
	vec3_t origin, angles;
} casterent_t;

static castsurf_t *castsurfs;
static int castsurfs_count, castsurfs_cap;
static casterbatch_t *batches;
static int batches_count, batches_cap;
static casterent_t *casterents;
static int casterents_count, casterents_cap;
static GLuint *casters;
static int casters_count, casters_cap;
static int c_casters;

// models: posed once per frame into world-space triangles, then drawn into all six views
typedef struct {
	int first, count;           // vertices in model_xyz/model_st
	const gltexture_t *masked;  // alpha-tested with this texture; NULL = solid
} modelrun_t;
static float *model_xyz, *model_st;
static int model_verts, model_cap;
static modelrun_t *modelruns;
static int modelruns_count, modelruns_cap;
static int c_models;

int R_ShadowSlot(int light)
{
	for (int i = 0; i < slots_used; i++)
		if (slots[i].light == light)
			return i;
	return -1;
}

GLuint R_ShadowTexture(int slot)
{
	return slots[slot].tex;
}

void R_ShadowParams(int slot, float *znear, float *zfar, float *bias, int *size, float *soft)
{
	*znear = SHADOW_NEAR;
	*zfar = slots[slot].zfar;
	*bias = r_shadow_bias->value;
	*size = slots[slot].size;
	*soft = r_shadow_soft->value ? 1.0f : 0.0f;
}

static void FreeSlot(shadowslot_t *sl)
{
	if (sl->fbo)
		qglDeleteFramebuffers(1, &sl->fbo);
	if (sl->tex)
		glDeleteTextures(1, &sl->tex);
	sl->fbo = sl->tex = 0;
	sl->size = 0;
}

static qboolean EnsureSlot(shadowslot_t *sl)
{
	int size = (int)r_shadow_size->value;
	if (size < 64) size = 64;
	if (size > 4096) size = 4096;
	if (size * 3 > gl_max_texture_size)
		size = gl_max_texture_size / 3;
	if (sl->fbo && size == sl->size)
		return true;
	if (sl->failed && size == sl->size)
		return false;
	FreeSlot(sl);
	sl->size = size;
	sl->failed = false;

	glGenTextures(1, &sl->tex);
	glBindTexture(GL_TEXTURE_2D, sl->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size * 3, size * 2, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	// the shader compares distances itself and filters by taking several samples
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	glBindTexture(GL_TEXTURE_2D, 0);

	qglGenFramebuffers(1, &sl->fbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, sl->fbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sl->tex, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	GLenum status = qglCheckFramebufferStatus(GL_FRAMEBUFFER);
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		Con_Warnf("Shadows are off: the shadow map framebuffer isn't usable (status 0x%x)\n", status);
		FreeSlot(sl);
		sl->size = size;
		sl->failed = true;
		return false;
	}
	Con_DPrintf("Shadow map: %dx%d\n", size * 3, size * 2);
	return true;
}

// ---------------------------------------------------------------- casters

static qboolean Masked(const castsurf_t *c)
{
	return c->surf->texinfo->texture->kind == TEXKIND_ALPHA;
}

static void AddCastSurf(const msurface_t *s, int ent)
{
	if (!s->numverts || (s->flags & (SURF_SKY | SURF_WATER)))
		return;
	if (castsurfs_count == castsurfs_cap) {
		castsurfs_cap = castsurfs_cap ? castsurfs_cap * 2 : 1024;
		castsurfs = Mem_Realloc(castsurfs, sizeof(*castsurfs) * castsurfs_cap);
	}
	castsurfs[castsurfs_count].surf = s;
	castsurfs[castsurfs_count].ent = ent;
	castsurfs_count++;
}

// Every world face the light's sphere reaches, whatever the camera sees: a wall behind the viewer still
// throws its shadow into the room
static void CollectWorldCasters(const vec3_t org, float radius, mnode_t *node, int depth)
{
	while (node && node->contents >= 0 && depth < 1024) {
		float dist = DotProduct(org, node->plane->normal) - node->plane->dist;
		if (dist > radius) {
			node = node->children[0];
		} else if (dist < -radius) {
			node = node->children[1];
		} else {
			for (int i = 0; i < node->numsurfaces; i++)
				AddCastSurf(&world.surfaces[node->firstsurface + i], -1);
			CollectWorldCasters(org, radius, node->children[0], depth + 1);
			node = node->children[1];
		}
		depth++;
	}
}

// Doors, lifts, trains and fences: the solid and alpha-tested brush entities near the light, on screen or not
static void CollectBrushCasters(const vec3_t org, float radius)
{
	casterents_count = 0;
	int n = R_BrushEntityCount();
	for (int i = 0; i < n; i++) {
		int model, rendermode;
		vec3_t origin, angles;
		R_GetBrushEntity(i, &model, origin, angles, &rendermode);
		if (rendermode != kRenderNormal && rendermode != kRenderTransAlpha)
			continue;   // see-through: light goes through it
		const msubmodel_t *m = &world.submodels[model];
		// the model's bounds round its origin (a sphere when it's turned), against the light's sphere
		float d2 = 0;
		if (angles[0] || angles[1] || angles[2]) {
			float r = 0;
			for (int j = 0; j < 3; j++) {
				float a = fabsf(m->mins[j]), b = fabsf(m->maxs[j]);
				r += (a > b ? a : b) * (a > b ? a : b);
			}
			vec3_t d;
			VectorSubtract(org, origin, d);
			float gap = VectorLength(d) - sqrtf(r);
			d2 = gap > 0 ? gap * gap : 0;
		} else {
			for (int j = 0; j < 3; j++) {
				float lo = origin[j] + m->mins[j], hi = origin[j] + m->maxs[j];
				float e = org[j] < lo ? lo - org[j] : org[j] > hi ? org[j] - hi : 0;
				d2 += e * e;
			}
		}
		if (d2 > radius * radius)
			continue;
		if (casterents_count == casterents_cap) {
			casterents_cap = casterents_cap ? casterents_cap * 2 : 64;
			casterents = Mem_Realloc(casterents, sizeof(*casterents) * casterents_cap);
		}
		casterent_t *ce = &casterents[casterents_count];
		VectorCopy(origin, ce->origin);
		VectorCopy(angles, ce->angles);
		for (int f = 0; f < m->numfaces; f++)
			AddCastSurf(&world.surfaces[m->firstface + f], casterents_count);
		casterents_count++;
	}
}

static void EmitModelTris(const float *xyz, const float *st, int n, const gltexture_t *masked)
{
	if (model_verts + n > model_cap) {
		model_cap = (model_verts + n) * 2;
		model_xyz = Mem_Realloc(model_xyz, sizeof(float) * 3 * model_cap);
		model_st = Mem_Realloc(model_st, sizeof(float) * 2 * model_cap);
	}
	memcpy(model_xyz + model_verts * 3, xyz, sizeof(float) * 3 * n);
	memcpy(model_st + model_verts * 2, st, sizeof(float) * 2 * n);
	if (!modelruns_count || modelruns[modelruns_count - 1].masked != masked || masked) {
		if (modelruns_count == modelruns_cap) {
			modelruns_cap = modelruns_cap ? modelruns_cap * 2 : 64;
			modelruns = Mem_Realloc(modelruns, sizeof(*modelruns) * modelruns_cap);
		}
		modelruns[modelruns_count].first = model_verts;
		modelruns[modelruns_count].count = 0;
		modelruns[modelruns_count].masked = masked;
		modelruns_count++;
	}
	modelruns[modelruns_count - 1].count += n;
	model_verts += n;
}

// Monsters, scientists, items: solid studio models near the light, whatever the camera sees
static void CollectModelCasters(const vec3_t org, float radius)
{
	model_verts = modelruns_count = c_models = 0;
	if (!r_shadow_models->value)
		return;
	int n = R_EntityTotal();
	for (int i = 0; i < n; i++) {
		rentity_t *e = R_EntityAt(i);
		if (!e->model || e->model->type != mod_studio || !e->model->studio.hdr)
			continue;
		if (e->rendermode != kRenderNormal && e->rendermode != kRenderTransAlpha)
			continue;
		float r = e->model->radius > 16 ? e->model->radius : 16;
		vec3_t d;
		VectorSubtract(e->origin, org, d);
		if (DotProduct(d, d) > (radius + r) * (radius + r))
			continue;
		R_StudioCasterGeometry(e, EmitModelTris);
		c_models++;
	}
}

// world first, then each entity; within each, opaque faces before the '{' ones grouped by texture
static int CompareCastSurfs(const void *pa, const void *pb)
{
	const castsurf_t *a = pa, *b = pb;
	if (a->ent != b->ent)
		return a->ent < b->ent ? -1 : 1;
	int ka = Masked(a) ? a->surf->texinfo->texture->index + 1 : 0;
	int kb = Masked(b) ? b->surf->texinfo->texture->index + 1 : 0;
	return ka < kb ? -1 : ka > kb;
}

static void BuildBatches(void)
{
	qsort(castsurfs, castsurfs_count, sizeof(*castsurfs), CompareCastSurfs);
	casters_count = batches_count = 0;
	for (int i = 0; i < castsurfs_count; i++) {
		const castsurf_t *c = &castsurfs[i];
		const texture_t *tex = Masked(c) ? c->surf->texinfo->texture : NULL;
		if (!batches_count || batches[batches_count - 1].ent != c->ent || batches[batches_count - 1].tex != tex) {
			if (batches_count == batches_cap) {
				batches_cap = batches_cap ? batches_cap * 2 : 64;
				batches = Mem_Realloc(batches, sizeof(*batches) * batches_cap);
			}
			casterbatch_t *b = &batches[batches_count++];
			b->first = casters_count;
			b->count = 0;
			b->tex = tex;
			b->ent = c->ent;
		}
		const msurface_t *s = c->surf;
		int need = casters_count + (s->numverts - 2) * 3;
		if (need > casters_cap) {
			casters_cap = need * 2;
			casters = Mem_Realloc(casters, sizeof(GLuint) * casters_cap);
		}
		for (int v = 2; v < s->numverts; v++) {
			casters[casters_count++] = s->firstvert;
			casters[casters_count++] = s->firstvert + v - 1;
			casters[casters_count++] = s->firstvert + v;
		}
		batches[batches_count - 1].count = casters_count - batches[batches_count - 1].first;
	}
	c_casters = castsurfs_count;
}

// ---------------------------------------------------------------- rendering

static void FaceModelview(int k, const vec3_t org, float m[16])
{
	const float *r = face_r[k], *u = face_u[k], *f = face_f[k];
	// rows R, U, -F (an OpenGL camera looks down -Z), then the light's position
	m[0] = r[0]; m[4] = r[1]; m[8] = r[2];   m[12] = -DotProduct(r, org);
	m[1] = u[0]; m[5] = u[1]; m[9] = u[2];   m[13] = -DotProduct(u, org);
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = DotProduct(f, org);
	m[3] = m[7] = m[11] = 0;
	m[15] = 1;
}

// One light's six views into its slot (the GL state is set up by R_RenderShadows)
static void RenderSlot(shadowslot_t *sl, const vec3_t org, float radius)
{
	int atlas_size = sl->size;
	castsurfs_count = 0;
	CollectWorldCasters(org, radius, &world.nodes[0], 0);
	CollectBrushCasters(org, radius);
	BuildBatches();
	CollectModelCasters(org, radius);
	sl->zfar = radius;

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-SHADOW_NEAR, SHADOW_NEAR, -SHADOW_NEAR, SHADOW_NEAR, SHADOW_NEAR, sl->zfar);
	glMatrixMode(GL_MODELVIEW);

	qglBindFramebuffer(GL_FRAMEBUFFER, sl->fbo);
	glViewport(0, 0, atlas_size * 3, atlas_size * 2);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);   // both sides: thin walls cast too, and the bias handles acne
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	qglActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	qglActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.5f, 2.0f);

	qglBindBuffer(GL_ARRAY_BUFFER, world.vbo);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(worldvert_t), (void *)offsetof(worldvert_t, xyz));
	// '{' faces need their texture's alpha, so fences cast shadows with holes
	qglClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(worldvert_t), (void *)offsetof(worldvert_t, st));
	glAlphaFunc(GL_GREATER, 0.25f);
	for (int k = 0; k < 6 && (casters_count || model_verts); k++) {
		float m[16];
		FaceModelview(k, org, m);
		glViewport((k % 3) * atlas_size, (k / 3) * atlas_size, atlas_size, atlas_size);
		int ent = -2;
		for (int i = 0; i < batches_count; i++) {
			const casterbatch_t *b = &batches[i];
			if (b->ent != ent) {
				// the same transform DrawBrushEntity uses
				ent = b->ent;
				glLoadMatrixf(m);
				if (ent >= 0) {
					const casterent_t *ce = &casterents[ent];
					glTranslatef(ce->origin[0], ce->origin[1], ce->origin[2]);
					if (ce->angles[0] || ce->angles[1] || ce->angles[2]) {
						glRotatef(ce->angles[1], 0, 0, 1);
						glRotatef(ce->angles[0], 0, 1, 0);
						glRotatef(ce->angles[2], 1, 0, 0);
					}
				}
			}
			if (b->tex) {
				glEnable(GL_TEXTURE_2D);
				glEnable(GL_ALPHA_TEST);
				glBindTexture(GL_TEXTURE_2D, b->tex->gl ? b->tex->gl->id : GL_MissingTexture()->id);
			}
			glDrawElements(GL_TRIANGLES, b->count, GL_UNSIGNED_INT, casters + b->first);
			if (b->tex) {
				glDisable(GL_ALPHA_TEST);
				glDisable(GL_TEXTURE_2D);
			}
		}
	}
	// the models' triangles are already in world space, in CPU memory
	if (model_verts) {
		qglBindBuffer(GL_ARRAY_BUFFER, 0);
		glVertexPointer(3, GL_FLOAT, 0, model_xyz);
		glTexCoordPointer(2, GL_FLOAT, 0, model_st);
		glAlphaFunc(GL_GREATER, 0.5f);   // the studio renderer's cut for masked meshes
		for (int k = 0; k < 6; k++) {
			float m[16];
			FaceModelview(k, org, m);
			glLoadMatrixf(m);
			glViewport((k % 3) * atlas_size, (k / 3) * atlas_size, atlas_size, atlas_size);
			for (int i = 0; i < modelruns_count; i++) {
				const modelrun_t *r = &modelruns[i];
				if (r->masked) {
					glEnable(GL_TEXTURE_2D);
					glEnable(GL_ALPHA_TEST);
					glBindTexture(GL_TEXTURE_2D, r->masked->id);
				}
				glDrawArrays(GL_TRIANGLES, r->first, r->count);
				if (r->masked) {
					glDisable(GL_ALPHA_TEST);
					glDisable(GL_TEXTURE_2D);
				}
			}
		}
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	qglBindBuffer(GL_ARRAY_BUFFER, 0);

	if (r_shadow_debug->value > 1)
		Con_Printf("shadow light %d id %d (slot %d): %d faces (%d brush entities) in %d batches, %d triangles; %d models, "
			"%d triangles; atlas %dx%d\n", sl->light, sl->id, (int)(sl - slots), c_casters, casterents_count, batches_count, casters_count / 3,
			c_models, model_verts / 3, atlas_size * 3, atlas_size * 2);
}

typedef struct {
	int light, id;
	float score;
} shadowpick_t;

static int ComparePicks(const void *a, const void *b)
{
	float sa = ((const shadowpick_t *)a)->score, sb = ((const shadowpick_t *)b)->score;
	return sa < sb ? 1 : sa > sb ? -1 : 0;
}

// Which lights get the shadow maps: the test light always, then the ones that light the most of what's near the
// viewer. A light that had a map last frame keeps a head start, so two similar lights don't trade it back and
// forth every frame (that flickers).
static int ChooseLights(shadowpick_t *picks, int budget)
{
	shadowpick_t cand[64];
	int n = 0;
	float minradius = r_shadow_minradius->value;
	for (int i = 0; i < R_DlightCount() && n < 64; i++) {
		int id = R_DlightShadowId(i);
		if (!id)
			continue;
		vec3_t org, color, d;
		float radius;
		R_GetDlightWorld(i, org, &radius, color);
		if (radius <= SHADOW_NEAR * 2 || (id != R_TESTLIGHT_ID && radius < minradius))
			continue;
		// inside a wall every direction is blocked: say so (for the test light) instead of guessing a better spot
		if (BSP_PointInLeaf(org)->contents == CONTENTS_SOLID) {
			if (id == R_TESTLIGHT_ID && r_refdef.framecount - warned_solid_frame > 300) {
				Con_Warnf("The test light at %.0f %.0f %.0f is inside a wall; it's drawn without shadows\n", org[0], org[1], org[2]);
				warned_solid_frame = r_refdef.framecount;
			}
			continue;
		}
		VectorSubtract(org, r_refdef.origin, d);
		float dist = VectorLength(d);
		float bright = color[0] > color[1] ? color[0] : color[1];
		if (color[2] > bright)
			bright = color[2];
		float score = bright * radius / (dist > radius ? dist : radius);
		for (int k = 0; k < last_ids_count; k++)
			if (last_ids[k] == id)
				score *= 1.5f;
		if (id == R_TESTLIGHT_ID)
			score = 1e30f;
		cand[n].light = i;
		cand[n].id = id;
		cand[n].score = score;
		n++;
	}
	qsort(cand, n, sizeof(cand[0]), ComparePicks);
	if (n > budget)
		n = budget;
	memcpy(picks, cand, sizeof(cand[0]) * n);
	return n;
}

void R_RenderShadows(void)
{
	slots_used = 0;
	if (!r_modern || !r_shadows->value || !gl_fbo || !world.numnodes)
		return;
	int budget = (int)r_shadow_lights->value;
	if (budget > MAX_SHADOW_SLOTS)
		budget = MAX_SHADOW_SLOTS;
	shadowpick_t picks[MAX_SHADOW_SLOTS];
	int n = budget > 0 ? ChooseLights(picks, budget) : 0;
	last_ids_count = 0;
	if (!n)
		return;

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	for (int i = 0; i < n; i++) {
		shadowslot_t *sl = &slots[slots_used];
		if (!EnsureSlot(sl))
			break;
		vec3_t org, color;
		float radius;
		R_GetDlightWorld(picks[i].light, org, &radius, color);
		sl->light = picks[i].light;
		sl->id = picks[i].id;
		RenderSlot(sl, org, radius);
		last_ids[last_ids_count++] = sl->id;
		slots_used++;
	}

	// back to the main view exactly as R_RenderView set it up
	glDisable(GL_POLYGON_OFFSET_FILL);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, vid_width, vid_height);
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glEnable(GL_TEXTURE_2D);
}

// r_shadow_debug 1: the shadow maps down the left side (depth as grey: near is dark)
void R_ShadowDrawDebug(void)
{
	if (!r_shadow_debug || !r_shadow_debug->value || !world.loaded)
		return;
	float w = 288, h = 192;
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
	for (int i = 0; i < slots_used; i++) {
		float y = 8 + i * (h + 8);
		glBindTexture(GL_TEXTURE_2D, slots[i].tex);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 1); glVertex2f(8, y);
		glTexCoord2f(1, 1); glVertex2f(8 + w, y);
		glTexCoord2f(1, 0); glVertex2f(8 + w, y + h);
		glTexCoord2f(0, 0); glVertex2f(8, y + h);
		glEnd();
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glEnable(GL_BLEND);
}

void R_InitShadows(void)
{
	r_shadows = Cvar_Get("r_shadows", "1", FCVAR_ARCHIVE, "modern: dynamic lights cast shadows");
	r_shadow_lights = Cvar_Get("r_shadow_lights", "2", FCVAR_ARCHIVE, va("modern: how many lights cast shadows at once (up to %d)", MAX_SHADOW_SLOTS));
	r_shadow_minradius = Cvar_Get("r_shadow_minradius", "64", FCVAR_ARCHIVE, "modern: smaller lights don't cast shadows (the test light always may)");
	r_shadow_soft = Cvar_Get("r_shadow_soft", "1", FCVAR_ARCHIVE, "modern: 1 = softer shadow edges (16 samples), 0 = 4");
	r_shadow_size = Cvar_Get("r_shadow_size", "256", FCVAR_ARCHIVE, "modern: pixels per side of each of a shadow's six views");
	r_shadow_bias = Cvar_Get("r_shadow_bias", "1", FCVAR_ARCHIVE, "modern: how far (units) a surface may be behind the shadow map and still be lit");
	r_shadow_models = Cvar_Get("r_shadow_models", "1", FCVAR_ARCHIVE, "modern: models (monsters, people, items) cast shadows");
	r_shadow_debug = Cvar_Get("r_shadow_debug", "0", 0, "1 = show the shadow map in the corner, 2 = also print caster counts");
}
