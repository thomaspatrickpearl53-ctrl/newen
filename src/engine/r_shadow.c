// newen - shadow maps for the modern renderer
//
// One point light (the shadow-casting one: the test light for now) renders the world's depth into six 90-degree
// views, kept as six tiles of one depth texture (3 across, 2 down). The light shader picks the tile from the
// direction to the light and compares distances itself, so no cube-map shadow samplers are needed. The shadow
// only dims that light's own contribution: the baked lightmaps already have their shadows.
//
// The faces, in tile order (the light shader has the same table): +X -X +Y / -Y +Z -Z. Each view looks along
// F with R to the right and U up, so a point d from the light lands at 0.5 + 0.5 * (d.R, d.U) / |d.F|.
#include "bsp.h"

static const float face_f[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float face_r[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
static const float face_u[6][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 } };

#define SHADOW_NEAR 1.0f

static cvar_t *r_shadows, *r_shadow_size, *r_shadow_bias, *r_shadow_debug;
static GLuint fbo, atlas;
static int atlas_size;            // one tile's size; the texture is 3x by 2x that
static qboolean atlas_failed;     // don't try again every frame
static int shadow_light = -1;
static float shadow_far;
static int warned_solid_frame = -1000;

static GLuint *casters;
static int casters_count, casters_cap;
static int c_casters;

int R_ShadowLight(void)
{
	return shadow_light;
}

GLuint R_ShadowTexture(void)
{
	return atlas;
}

void R_ShadowParams(float *znear, float *zfar, float *bias, int *size)
{
	*znear = SHADOW_NEAR;
	*zfar = shadow_far;
	*bias = r_shadow_bias->value;
	*size = atlas_size;
}

static void FreeAtlas(void)
{
	if (fbo)
		qglDeleteFramebuffers(1, &fbo);
	if (atlas)
		glDeleteTextures(1, &atlas);
	fbo = atlas = 0;
	atlas_size = 0;
}

static qboolean EnsureAtlas(void)
{
	int size = (int)r_shadow_size->value;
	if (size < 64) size = 64;
	if (size > 4096) size = 4096;
	if (size * 3 > gl_max_texture_size)
		size = gl_max_texture_size / 3;
	if (fbo && size == atlas_size)
		return true;
	if (atlas_failed && size == atlas_size)
		return false;
	FreeAtlas();
	atlas_size = size;
	atlas_failed = false;

	glGenTextures(1, &atlas);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size * 3, size * 2, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	// the shader compares distances itself and filters by taking several samples
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	glBindTexture(GL_TEXTURE_2D, 0);

	qglGenFramebuffers(1, &fbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, fbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, atlas, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	GLenum status = qglCheckFramebufferStatus(GL_FRAMEBUFFER);
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		Con_Warnf("Shadows are off: the shadow map framebuffer isn't usable (status 0x%x)\n", status);
		FreeAtlas();
		atlas_size = size;
		atlas_failed = true;
		return false;
	}
	Con_DPrintf("Shadow map: %dx%d\n", size * 3, size * 2);
	return true;
}

// ---------------------------------------------------------------- casters

static void AddCaster(const msurface_t *s)
{
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
	c_casters++;
}

// Every opaque world face the light's sphere reaches, whatever the camera sees: a wall behind the viewer still
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
			for (int i = 0; i < node->numsurfaces; i++) {
				const msurface_t *s = &world.surfaces[node->firstsurface + i];
				if (!s->numverts || (s->flags & (SURF_SKY | SURF_WATER)) || s->texinfo->texture->kind == TEXKIND_ALPHA)
					continue;
				AddCaster(s);
			}
			CollectWorldCasters(org, radius, node->children[0], depth + 1);
			node = node->children[1];
		}
		depth++;
	}
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

void R_RenderShadows(void)
{
	shadow_light = -1;
	if (!r_modern || !r_shadows->value || !gl_fbo || !world.numnodes)
		return;
	int light = -1;
	for (int i = 0; i < R_DlightCount(); i++)
		if (R_DlightShadows(i)) {
			light = i;
			break;
		}
	if (light < 0)
		return;
	vec3_t org;
	float radius;
	R_GetDlightWorld(light, org, &radius);
	if (radius <= SHADOW_NEAR * 2)
		return;
	// inside a wall every direction is blocked: say so instead of guessing a better spot
	if (BSP_PointInLeaf(org)->contents == CONTENTS_SOLID) {
		if (r_refdef.framecount - warned_solid_frame > 300) {
			Con_Warnf("The shadow-casting light at %.0f %.0f %.0f is inside a wall; it's drawn without shadows\n", org[0], org[1], org[2]);
			warned_solid_frame = r_refdef.framecount;
		}
		return;
	}
	if (!EnsureAtlas())
		return;

	casters_count = 0;
	c_casters = 0;
	CollectWorldCasters(org, radius, &world.nodes[0], 0);

	shadow_light = light;
	shadow_far = radius;

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glFrustum(-SHADOW_NEAR, SHADOW_NEAR, -SHADOW_NEAR, SHADOW_NEAR, SHADOW_NEAR, shadow_far);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();

	qglBindFramebuffer(GL_FRAMEBUFFER, fbo);
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
	for (int k = 0; k < 6 && casters_count; k++) {
		float m[16];
		FaceModelview(k, org, m);
		glLoadMatrixf(m);
		glViewport((k % 3) * atlas_size, (k / 3) * atlas_size, atlas_size, atlas_size);
		glDrawElements(GL_TRIANGLES, casters_count, GL_UNSIGNED_INT, casters);
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	qglBindBuffer(GL_ARRAY_BUFFER, 0);

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

	if (r_shadow_debug->value > 1)
		Con_Printf("shadow light %d: %d casters, %d triangles, atlas %dx%d\n", light, c_casters, casters_count / 3,
			atlas_size * 3, atlas_size * 2);
}

// r_shadow_debug 1: the atlas in the top left corner (depth as grey: near is dark)
void R_ShadowDrawDebug(void)
{
	if (!r_shadow_debug || !r_shadow_debug->value || shadow_light < 0 || !atlas || !world.loaded)
		return;
	float w = 384, h = 256;
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glColor4f(1, 1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 1); glVertex2f(8, 8);
	glTexCoord2f(1, 1); glVertex2f(8 + w, 8);
	glTexCoord2f(1, 0); glVertex2f(8 + w, 8 + h);
	glTexCoord2f(0, 0); glVertex2f(8, 8 + h);
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glEnable(GL_BLEND);
}

void R_InitShadows(void)
{
	r_shadows = Cvar_Get("r_shadows", "1", FCVAR_ARCHIVE, "modern: the shadow-casting light (the test light, for now) casts shadows");
	r_shadow_size = Cvar_Get("r_shadow_size", "256", FCVAR_ARCHIVE, "modern: pixels per side of each of a shadow's six views");
	r_shadow_bias = Cvar_Get("r_shadow_bias", "1", FCVAR_ARCHIVE, "modern: how far (units) a surface may be behind the shadow map and still be lit");
	r_shadow_debug = Cvar_Get("r_shadow_debug", "0", 0, "1 = show the shadow map in the corner, 2 = also print caster counts");
}
