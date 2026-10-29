// newen - world rendering: PVS, frustum culling, batched lightmapped surfaces, sky, water, brush entities
#include "bsp.h"
#include "model.h"

int lightstyle_value[64];
static char lightstyle_pattern[64][128];

static cvar_t *r_novis, *r_drawworld, *r_drawentities, *r_fullbright, *r_lightmap, *r_speeds, *r_wateralpha;

static int visframecount;
static mnode_t *viewleaf, *oldviewleaf;
static int old_novis = -1;

// visible surfaces collected each frame, then sorted into batches
typedef struct {
	msurface_t *surf;
	texture_t  *tex;     // after animation
	int         key;
} drawsurf_t;

static drawsurf_t *drawsurfs;
static int drawsurfs_count, drawsurfs_cap;
static GLuint *indices;
static int indices_cap;
static int c_surfaces, c_batches, c_lmupdates, c_lightpasses;

// modern: whether what DrawBatches draws now takes light passes (opaque or alpha-tested), and the viewer
// in its space
static qboolean lightpass_ok;
static vec3_t lightpass_viewer;

// ---------------------------------------------------------------- light styles

void R_SetLightStyle(int style, const char *pattern)
{
	if (style < 0 || style >= 64)
		return;
	Q_strncpy(lightstyle_pattern[style], pattern ? pattern : "", sizeof(lightstyle_pattern[0]));
}

const char *R_LightStylePattern(int style)
{
	return style >= 0 && style < 64 ? lightstyle_pattern[style] : "";
}

void R_AnimateLightStyles(double time)
{
	int frame = (int)(time * 10.0);
	for (int i = 0; i < 64; i++) {
		const char *p = lightstyle_pattern[i];
		int len = (int)strlen(p);
		lightstyle_value[i] = len ? (p[frame % len] - 'a') * 22 : 256;
		if (lightstyle_value[i] < 0)
			lightstyle_value[i] = 0;
	}
}

static void LightStyle_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: lightstyle <0-63> [pattern a-z]\n");
		return;
	}
	int s = atoi(Cmd_Argv(1));
	if (Cmd_Argc() == 2)
		Con_Printf("style %d: \"%s\"\n", s, (s >= 0 && s < 64) ? lightstyle_pattern[s] : "");
	else
		R_SetLightStyle(s, Cmd_Argv(2));
}

static void UpdateLightmap(msurface_t *s)
{
	if (!s->samples)
		return;
	// modern adds dynamic lights in their own passes, so its lightmaps only follow the light styles
	qboolean lit = !r_modern && s->dlightframe == r_refdef.framecount;
	qboolean changed = lit || s->dlit;   // a dynamic light is on it, or was last time
	for (int m = 0; m < 4 && s->styles[m] != 255; m++)
		if (s->cached_light[m] != lightstyle_value[s->styles[m] & 63])
			changed = true;
	if (!changed)
		return;
	s->dlit = lit;
	unsigned char *page = world.lightmap_pixels[s->lm_page];
	unsigned char *dest = page + (s->lm_y * LM_PAGE + s->lm_x) * 4;
	BSP_BuildSurfaceLightmap(s, dest, LM_PAGE * 4);
	glBindTexture(GL_TEXTURE_2D, world.lightmaps[s->lm_page]);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, LM_PAGE);
	glTexSubImage2D(GL_TEXTURE_2D, 0, s->lm_x, s->lm_y, s->lm_w, s->lm_h, GL_RGBA, GL_UNSIGNED_BYTE, dest);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	c_lmupdates++;
}

// ---------------------------------------------------------------- visibility

static void MarkLeaves(void)
{
	int novis = (int)r_novis->value;
	if (viewleaf == oldviewleaf && novis == old_novis)
		return;
	oldviewleaf = viewleaf;
	old_novis = novis;
	visframecount++;

	int visleafs = world.submodels[0].visleafs;
	const unsigned char *pvs = NULL;
	if (!novis && viewleaf->contents != CONTENTS_SOLID && world.visdata)
		pvs = BSP_LeafPVS(viewleaf);

	for (int i = 0; i < visleafs && i + 1 < world.numleafs; i++) {
		if (pvs && !(pvs[i >> 3] & (1 << (i & 7))))
			continue;
		mnode_t *node = &world.leafs[i + 1];
		do {
			if (node->visframe == visframecount)
				break;
			node->visframe = visframecount;
			node = node->parent;
		} while (node);
	}
}

static texture_t *TextureAnimation(texture_t *base, int frame)
{
	if (frame && base->alternate_anims)
		base = base->alternate_anims;
	if (!base->anim_total)
		return base;
	int reletive = (int)(r_refdef.time * 10.0) % base->anim_total;
	int count = 0;
	while (base->anim_min > reletive || base->anim_max <= reletive) {
		base = base->anim_next;
		if (!base || ++count > 100)
			return base ? base : NULL;
	}
	return base;
}

static void AddDrawSurf(msurface_t *s, int frame)
{
	if (drawsurfs_count == drawsurfs_cap) {
		drawsurfs_cap = drawsurfs_cap ? drawsurfs_cap * 2 : 4096;
		drawsurfs = Mem_Realloc(drawsurfs, sizeof(*drawsurfs) * drawsurfs_cap);
	}
	texture_t *tx = TextureAnimation(s->texinfo->texture, frame);
	if (!tx)
		tx = s->texinfo->texture;
	drawsurf_t *d = &drawsurfs[drawsurfs_count++];
	d->surf = s;
	d->tex = tx;
	int page = (s->flags & SURF_NOLIGHT) ? 0xffff : s->lm_page;
	d->key = (tx->index << 16) | (page & 0xffff);
}

static qboolean SurfaceFacing(const msurface_t *s, const vec3_t org)
{
	float d = DotProduct(org, s->plane->normal) - s->plane->dist;
	return (s->flags & SURF_PLANEBACK) ? d < 0.01f : d > -0.01f;
}

static void RecursiveWorldNode(mnode_t *node)
{
	while (node) {
		if (node->contents == CONTENTS_SOLID || node->visframe != visframecount)
			return;
		if (R_CullBox(node->mins, node->maxs))
			return;
		if (node->contents < 0) {
			for (int i = 0; i < node->nummarksurfaces; i++)
				node->marksurfaces[i]->visframe = r_refdef.framecount;
			return;
		}
		float d = DotProduct(r_refdef.origin, node->plane->normal) - node->plane->dist;
		int side = d >= 0 ? 0 : 1;
		RecursiveWorldNode(node->children[side]);

		for (int i = 0; i < node->numsurfaces; i++) {
			msurface_t *s = &world.surfaces[node->firstsurface + i];
			if (s->visframe != r_refdef.framecount || !s->numverts)
				continue;
			if (!SurfaceFacing(s, r_refdef.origin))
				continue;
			AddDrawSurf(s, 0);
		}
		node = node->children[!side];
	}
}

// ---------------------------------------------------------------- drawing

static int CompareDrawSurfs(const void *a, const void *b)
{
	int ka = ((const drawsurf_t *)a)->key, kb = ((const drawsurf_t *)b)->key;
	return ka < kb ? -1 : ka > kb;
}

static void SetupArrays(void)
{
	// the vertices live in a buffer on the GPU; pointers are offsets into it
	qglBindBuffer(GL_ARRAY_BUFFER, world.vbo);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(worldvert_t), (void *)offsetof(worldvert_t, xyz));
	qglClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(worldvert_t), (void *)offsetof(worldvert_t, st));
	qglClientActiveTexture(GL_TEXTURE1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(worldvert_t), (void *)offsetof(worldvert_t, lm));
	qglClientActiveTexture(GL_TEXTURE0);
}

static void ResetArrays(void)
{
	qglBindBuffer(GL_ARRAY_BUFFER, 0);
	qglClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	qglClientActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

static void EnsureIndices(int n)
{
	if (n > indices_cap) {
		indices_cap = n * 2;
		indices = Mem_Realloc(indices, sizeof(GLuint) * indices_cap);
	}
}

static void BindLightmap(int page)
{
	qglActiveTexture(GL_TEXTURE1);
	if (page == 0xffff || r_fullbright->value) {
		glDisable(GL_TEXTURE_2D);
	} else {
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, world.lightmaps[page]);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
	qglActiveTexture(GL_TEXTURE0);
}

static void BindDiffuse(const texture_t *tx)
{
	if (r_lightmap->value) {
		glDisable(GL_TEXTURE_2D);
		return;
	}
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tx->gl ? tx->gl->id : GL_MissingTexture()->id);
}

// Water: unlit, with a gentle turbulent warp of the texture coordinates
static void DrawWaterSurface(const msurface_t *s, const texture_t *tx, float alpha)
{
	BindLightmap(0xffff);
	BindDiffuse(tx);
	float t = (float)r_refdef.time;
	glColor4f(1, 1, 1, alpha);
	glBegin(GL_POLYGON);
	for (int i = 0; i < s->numverts; i++) {
		const worldvert_t *v = &world.verts[s->firstvert + i];
		float os = v->st[0], ot = v->st[1];
		float ws = os + sinf(ot * 3.0f + t) * 0.0625f;
		float wt = ot + sinf(os * 3.0f + t) * 0.0625f;
		glTexCoord2f(ws, wt);
		glVertex3fv(v->xyz);
	}
	glEnd();
	glColor4f(1, 1, 1, 1);
}

// Modern: one additive pass per dynamic light that reaches any surface of the batch [first, last)
static void DrawLightPasses(int first, int last, const texture_t *tx)
{
	unsigned bits = 0;
	for (int k = first; k < last; k++) {
		const msurface_t *s = drawsurfs[k].surf;
		if (!(s->flags & (SURF_WATER | SURF_SKY)) && s->dlightframe == r_refdef.framecount)
			bits |= s->dlightbits;
	}
	if (!bits)
		return;
	// normals and tangents come from the second stream; positions and st stay on the world's buffer
	qglBindBuffer(GL_ARRAY_BUFFER, world.modern_vbo);
	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, sizeof(modernvert_t), (void *)offsetof(modernvert_t, normal));
	qglClientActiveTexture(GL_TEXTURE2);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(4, GL_FLOAT, sizeof(modernvert_t), (void *)offsetof(modernvert_t, tangent));
	qglClientActiveTexture(GL_TEXTURE0);
	qglBindBuffer(GL_ARRAY_BUFFER, world.vbo);
	R_ModernBeginLights(tx->gl ? tx->gl->id : GL_MissingTexture()->id, tx->normalmap, tx->specmap, lightpass_viewer);
	int count = R_DlightCount();
	for (int i = 0; i < count; i++) {
		unsigned bit = 1u << i;
		if (!(bits & bit))
			continue;
		int n = 0;
		for (int k = first; k < last; k++) {
			const msurface_t *s = drawsurfs[k].surf;
			if ((s->flags & (SURF_WATER | SURF_SKY)) || s->dlightframe != r_refdef.framecount || !(s->dlightbits & bit))
				continue;
			for (int v = 2; v < s->numverts; v++) {
				indices[n++] = s->firstvert;
				indices[n++] = s->firstvert + v - 1;
				indices[n++] = s->firstvert + v;
			}
		}
		if (!n)
			continue;
		R_ModernSetLight(i);
		glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, indices);
		c_lightpasses++;
	}
	R_ModernEndLights();
}

// Draws the collected surfaces in texture/lightmap batches
static void DrawBatches(float alpha)
{
	if (!drawsurfs_count)
		return;
	qsort(drawsurfs, drawsurfs_count, sizeof(drawsurf_t), CompareDrawSurfs);
	c_surfaces += drawsurfs_count;

	for (int i = 0; i < drawsurfs_count;) {
		int key = drawsurfs[i].key;
		texture_t *tx = drawsurfs[i].tex;
		int j = i;
		int nidx = 0;
		while (j < drawsurfs_count && drawsurfs[j].key == key) {
			msurface_t *s = drawsurfs[j].surf;
			if (!(s->flags & (SURF_WATER | SURF_SKY)))
				nidx += (s->numverts - 2) * 3;
			j++;
		}

		if (tx->kind == TEXKIND_WATER) {
			for (int k = i; k < j; k++)
				DrawWaterSurface(drawsurfs[k].surf, tx, r_wateralpha->value < 1 ? r_wateralpha->value * alpha : alpha);
			SetupArrays();
			i = j;
			c_batches++;
			continue;
		}

		EnsureIndices(nidx);
		int n = 0;
		for (int k = i; k < j; k++) {
			msurface_t *s = drawsurfs[k].surf;
			if (s->flags & (SURF_WATER | SURF_SKY))
				continue;
			UpdateLightmap(s);
			for (int v = 2; v < s->numverts; v++) {
				indices[n++] = s->firstvert;
				indices[n++] = s->firstvert + v - 1;
				indices[n++] = s->firstvert + v;
			}
		}
		if (n) {
			BindLightmap(key & 0xffff);
			BindDiffuse(tx);
			if (tx->kind == TEXKIND_ALPHA) {
				glEnable(GL_ALPHA_TEST);
				glAlphaFunc(GL_GREATER, 0.25f);
			}
			glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, indices);
			if (tx->kind == TEXKIND_ALPHA)
				glDisable(GL_ALPHA_TEST);
			c_batches++;
			if (r_modern && lightpass_ok && !r_fullbright->value && !r_lightmap->value) {
				DrawLightPasses(i, j, tx);
				BindLightmap(key & 0xffff);
			}
		}
		i = j;
	}
	drawsurfs_count = 0;
}

// ---------------------------------------------------------------- sky

static void DrawSkyBox(void)
{
	// +x rt, +y bk, -x lf, -y ft, +z up, -z dn; same orientation as GoldSrc/Quake 2
	static const int axis_tex[6] = { 0, 2, 1, 3, 4, 5 };   // index into world.sky (rt bk lf ft up dn)
	static const int st_to_vec[6][3] = {
		{ 3, -1, 2 }, { -3, 1, 2 }, { 1, 3, 2 }, { -1, -3, 2 }, { -2, -1, 3 }, { 2, -1, -3 }
	};
	static const float corners[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
	const float size = 64.0f;

	glDepthMask(GL_FALSE);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
	glPushMatrix();
	glTranslatef(r_refdef.origin[0], r_refdef.origin[1], r_refdef.origin[2]);
	for (int axis = 0; axis < 6; axis++) {
		gltexture_t *t = world.sky[axis_tex[axis]];
		glBindTexture(GL_TEXTURE_2D, t ? t->id : GL_MissingTexture()->id);
		glBegin(GL_QUADS);
		for (int c = 0; c < 4; c++) {
			float v[3] = { corners[c][0] * size, corners[c][1] * size, size };
			float b[3];
			for (int j = 0; j < 3; j++) {
				int k = st_to_vec[axis][j];
				b[j] = k < 0 ? -v[-k - 1] : v[k - 1];
			}
			float s = (corners[c][0] + 1) * 0.5f;
			float tt = 1.0f - (corners[c][1] + 1) * 0.5f;
			glTexCoord2f(s, tt);
			glVertex3fv(b);
		}
		glEnd();
	}
	glPopMatrix();
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

// Sky faces only write depth, so nothing behind them shows through the skybox
static void DrawSkyDepth(void)
{
	int n = 0;
	for (int i = 0; i < world.numsurfaces; i++) {
		msurface_t *s = &world.surfaces[i];
		if ((s->flags & SURF_SKY) && s->visframe == r_refdef.framecount && s->numverts)
			n += (s->numverts - 2) * 3;
	}
	if (!n)
		return;
	EnsureIndices(n);
	n = 0;
	for (int i = 0; i < world.numsurfaces; i++) {
		msurface_t *s = &world.surfaces[i];
		if (!(s->flags & SURF_SKY) || s->visframe != r_refdef.framecount || !s->numverts)
			continue;
		for (int v = 2; v < s->numverts; v++) {
			indices[n++] = s->firstvert;
			indices[n++] = s->firstvert + v - 1;
			indices[n++] = s->firstvert + v;
		}
	}
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDisable(GL_TEXTURE_2D);
	glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, indices);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// ---------------------------------------------------------------- brush entities


typedef struct {
	int    model;
	vec3_t origin, angles;
	int    rendermode, renderamt;
	int    color[3];
	int    frame;       // non-zero: the "+a" alternate textures (a pressed button, a lit sign)
	float  dist;
} brushent_t;

static brushent_t *brushents;
static int brushents_count;

static qboolean HiddenClass(const char *cls)
{
	static const char *hidden[] = {
		"func_ladder", "func_monsterclip", "env_bubbles", "game_zone_player", "func_traincontrols",
		"func_tankcontrols", "func_mortar_field", "func_buyzone", "func_bomb_target", "func_hostage_rescue",
		NULL
	};
	if (!strnicmp(cls, "trigger_", 8))
		return true;
	for (int i = 0; hidden[i]; i++)
		if (!stricmp(cls, hidden[i]))
			return true;
	return false;
}

static int brushents_cap;

void R_ClearBrushEntities(void)
{
	brushents_count = 0;
}

void R_AddBrushEntity(int submodel, const vec3_t origin, const vec3_t angles, int rendermode, int renderamt, const int color[3],
	int frame)
{
	if (submodel <= 0 || submodel >= world.numsubmodels)
		return;
	if (brushents_count == brushents_cap) {
		brushents_cap = brushents_cap ? brushents_cap * 2 : 256;
		brushents = Mem_Realloc(brushents, sizeof(brushent_t) * brushents_cap);
	}
	brushent_t *b = &brushents[brushents_count++];
	memset(b, 0, sizeof(*b));
	b->model = submodel;
	VectorCopy(origin, b->origin);
	VectorCopy(angles, b->angles);
	b->rendermode = rendermode;
	b->renderamt = rendermode == kRenderNormal ? 255 : renderamt;
	b->frame = frame;
	for (int i = 0; i < 3; i++)
		b->color[i] = color[i];
}

// Without a game DLL, brush entities are placed from the map's entity lump
void R_BuildBrushEntities(void)
{
	brushents_count = 0;
	if (!world.loaded || SV_GameLoaded())
		return;
	for (int i = 1; i < world.numentities; i++) {
		bspentity_t *e = &world.entities[i];
		const char *model = BSP_ValueForKey(e, "model");
		if (model[0] != '*' || HiddenClass(BSP_ValueForKey(e, "classname")))
			continue;
		int m = atoi(model + 1);
		if (m <= 0 || m >= world.numsubmodels)
			continue;
		vec3_t origin = { 0, 0, 0 }, angles = { 0, 0, 0 };
		int color[3] = { 255, 255, 255 };
		sscanf(BSP_ValueForKey(e, "origin"), "%f %f %f", &origin[0], &origin[1], &origin[2]);
		sscanf(BSP_ValueForKey(e, "angles"), "%f %f %f", &angles[0], &angles[1], &angles[2]);
		sscanf(BSP_ValueForKey(e, "rendercolor"), "%d %d %d", &color[0], &color[1], &color[2]);
		const char *amt = BSP_ValueForKey(e, "renderamt");
		R_AddBrushEntity(m, origin, angles, atoi(BSP_ValueForKey(e, "rendermode")), *amt ? atoi(amt) : 255, color, 0);
	}
}

static qboolean BrushEntityVisible(const brushent_t *b)
{
	msubmodel_t *m = &world.submodels[b->model];
	vec3_t mins, maxs;
	if (b->angles[0] || b->angles[1] || b->angles[2]) {
		float r = 0;
		for (int i = 0; i < 3; i++) {
			float a = fabsf(m->mins[i]), c = fabsf(m->maxs[i]);
			r += (a > c ? a : c) * (a > c ? a : c);
		}
		r = sqrtf(r);
		for (int i = 0; i < 3; i++) {
			mins[i] = b->origin[i] - r;
			maxs[i] = b->origin[i] + r;
		}
	} else {
		VectorAdd(m->mins, b->origin, mins);
		VectorAdd(m->maxs, b->origin, maxs);
	}
	return !R_CullBox(mins, maxs);
}

static void DrawBrushEntity(const brushent_t *b)
{
	msubmodel_t *m = &world.submodels[b->model];
	// the camera in the entity's local space, for back-face tests
	vec3_t local;
	VectorSubtract(r_refdef.origin, b->origin, local);
	qboolean rotated = b->angles[0] || b->angles[1] || b->angles[2];
	if (rotated) {
		vec3_t f, r, u, tmp;
		AngleVectors(b->angles, f, r, u);
		VectorCopy(local, tmp);
		local[0] = DotProduct(tmp, f);
		local[1] = -DotProduct(tmp, r);
		local[2] = DotProduct(tmp, u);
	}

	glPushMatrix();
	glTranslatef(b->origin[0], b->origin[1], b->origin[2]);
	if (rotated) {
		glRotatef(b->angles[1], 0, 0, 1);
		glRotatef(b->angles[0], 0, 1, 0);
		glRotatef(b->angles[2], 1, 0, 0);
	}

	R_DlightSpace(b->origin, b->angles);
	for (int i = 0; i < m->numfaces; i++) {
		msurface_t *s = &world.surfaces[m->firstface + i];
		if (!s->numverts || (s->flags & SURF_SKY) || !SurfaceFacing(s, local))
			continue;
		R_MarkSurfaceDlights(s);
		AddDrawSurf(s, b->frame);
	}

	float alpha = 1.0f;
	switch (b->rendermode) {
	case kRenderTransColor:
		glDisable(GL_TEXTURE_2D);
		glColor4ub((GLubyte)b->color[0], (GLubyte)b->color[1], (GLubyte)b->color[2], (GLubyte)b->renderamt);
		glEnable(GL_BLEND);
		glDepthMask(GL_FALSE);
		break;
	case kRenderTransTexture:
	case kRenderGlow:
		alpha = b->renderamt / 255.0f;
		glColor4f(1, 1, 1, alpha);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		break;
	case kRenderTransAdd:
		alpha = b->renderamt / 255.0f;
		glColor4f(alpha, alpha, alpha, 1);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthMask(GL_FALSE);
		break;
	case kRenderTransAlpha:
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.25f);
		break;
	}
	// see-through modes blend, so a light added on top would show twice; they stay without dynamic light
	lightpass_ok = b->rendermode == kRenderNormal || b->rendermode == kRenderTransAlpha;
	VectorCopy(local, lightpass_viewer);
	DrawBatches(alpha);
	lightpass_ok = false;
	if (b->rendermode == kRenderNormal || b->rendermode == kRenderTransAlpha) {
		R_DrawDecals(m->firstface, m->numfaces, local, false);
		SetupArrays();
	}
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glColor4f(1, 1, 1, 1);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glPopMatrix();
	R_DlightSpace(NULL, NULL);
}

static int CompareBrushDist(const void *a, const void *b)
{
	float da = ((const brushent_t *const *)a)[0]->dist, db = ((const brushent_t *const *)b)[0]->dist;
	return da < db ? 1 : da > db ? -1 : 0;   // far to near
}

static void DrawBrushEntities(void)
{
	if (!r_drawentities->value || !brushents_count)
		return;
	static const brushent_t **trans;
	static int trans_cap;
	if (trans_cap < brushents_count) {
		trans_cap = brushents_count;
		trans = Mem_Realloc((void *)trans, sizeof(*trans) * trans_cap);
	}
	int ntrans = 0;
	for (int i = 0; i < brushents_count; i++) {
		brushent_t *b = &brushents[i];
		if (b->rendermode != kRenderNormal && b->rendermode != kRenderTransAlpha) {
			if (b->renderamt <= 0)
				continue;   // fully invisible
		}
		if (!BrushEntityVisible(b))
			continue;
		if (b->rendermode == kRenderNormal || b->rendermode == kRenderTransAlpha) {
			DrawBrushEntity(b);
		} else {
			msubmodel_t *m = &world.submodels[b->model];
			vec3_t center, d;
			for (int j = 0; j < 3; j++)
				center[j] = b->origin[j] + (m->mins[j] + m->maxs[j]) * 0.5f;
			VectorSubtract(center, r_refdef.origin, d);
			b->dist = DotProduct(d, d);
			trans[ntrans++] = b;
		}
	}
	qsort((void *)trans, ntrans, sizeof(*trans), CompareBrushDist);
	for (int i = 0; i < ntrans; i++)
		DrawBrushEntity(trans[i]);
}

static qboolean BoxVisibleNode(const mnode_t *node, const vec3_t mins, const vec3_t maxs, int depth)
{
	while (node) {
		if (node->contents < 0)
			return node->contents != CONTENTS_SOLID && node->visframe == visframecount;
		if (node->visframe != visframecount || depth > 1024)
			return false;
		const mplane_t *p = node->plane;
		vec3_t near_pt, far_pt;
		for (int i = 0; i < 3; i++) {
			near_pt[i] = p->normal[i] >= 0 ? mins[i] : maxs[i];
			far_pt[i] = p->normal[i] >= 0 ? maxs[i] : mins[i];
		}
		float dfar = DotProduct(far_pt, p->normal) - p->dist;
		float dnear = DotProduct(near_pt, p->normal) - p->dist;
		if (dnear >= 0) {
			node = node->children[0];
		} else if (dfar < 0) {
			node = node->children[1];
		} else {
			if (BoxVisibleNode(node->children[0], mins, maxs, depth + 1))
				return true;
			node = node->children[1];
		}
		depth++;
	}
	return false;
}

qboolean R_BoxVisible(const vec3_t mins, const vec3_t maxs)
{
	if (!world.numnodes)
		return true;
	return BoxVisibleNode(&world.nodes[0], mins, maxs, 0);
}

// ---------------------------------------------------------------- entry points

void R_InitWorld(void)
{
	r_novis = Cvar_Get("r_novis", "0", 0, "1 = ignore the map's visibility data");
	r_drawworld = Cvar_Get("r_drawworld", "1", 0, "draws the world");
	r_drawentities = Cvar_Get("r_drawentities", "1", 0, "draws brush entities, models and sprites");
	r_fullbright = Cvar_Get("r_fullbright", "0", 0, "1 = no lightmaps");
	r_lightmap = Cvar_Get("r_lightmap", "0", 0, "1 = show only the lightmaps");
	r_speeds = Cvar_Get("r_speeds", "0", 0, "prints surface and batch counts");
	r_wateralpha = Cvar_Get("r_wateralpha", "1", FCVAR_ARCHIVE, "water transparency (1 = opaque like GoldSrc)");
	Cmd_AddCommand("lightstyle", LightStyle_f, "lightstyle <n> [pattern] - shows or sets an animated light style");
	for (int i = 0; i < 64; i++)
		R_SetLightStyle(i, "m");
	R_InitDecals();
}

void R_NewMap(void)
{
	viewleaf = oldviewleaf = NULL;
	old_novis = -1;
	R_ClearDecals();
	R_BuildBrushEntities();
	R_BuildMapEntities();
}

void R_DrawWorld(void)
{
	c_surfaces = c_batches = c_lmupdates = c_lightpasses = 0;
	R_AnimateLightStyles(r_refdef.time);
	viewleaf = BSP_PointInLeaf(r_refdef.origin);
	MarkLeaves();
	R_PushDlights();

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);
	qglActiveTexture(GL_TEXTURE0);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	DrawSkyBox();
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);   // Quake winding with the Z-up modelview
	SetupArrays();

	if (r_drawworld->value && world.numnodes) {
		RecursiveWorldNode(&world.nodes[0]);
		DrawSkyDepth();
		lightpass_ok = true;
		VectorCopy(r_refdef.origin, lightpass_viewer);
		DrawBatches(1.0f);
		lightpass_ok = false;
		R_DrawDecals(world.submodels[0].firstface, world.submodels[0].numfaces, r_refdef.origin, true);
		SetupArrays();
	}
	DrawBrushEntities();

	ResetArrays();
	qglActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	qglActiveTexture(GL_TEXTURE0);
	glDisable(GL_CULL_FACE);

	if (r_speeds->value) {
		if (r_modern)
			Con_Printf("%d surfaces, %d batches, %d lightmap updates, %d light passes (%d lights)\n", c_surfaces, c_batches,
				c_lmupdates, c_lightpasses, R_DlightCount());
		else
			Con_Printf("%d surfaces, %d batches, %d lightmap updates\n", c_surfaces, c_batches, c_lmupdates);
	}
}
