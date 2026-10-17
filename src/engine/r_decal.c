// newen - decals: bullet holes, blood, scorch marks and the map's infodecals
//
// A decal is cut out of the surfaces it lands on: a rectangle the size of its texture, laid along
// the surface's texture axes and clipped to each polygon it touches. It belongs to that surface
// (in the brush model's own space, so decals on doors move with them) and is drawn right after
// it, lit by the surface's lightmap. Textures come from decals.wad (then the map's WADs): each
// pixel's palette index is its opacity, and the last palette colour is the decal's colour.
#include "bsp.h"
#include "model.h"
#include "server.h"

typedef struct {
	char         name[16];
	gltexture_t *gl;
	int          w, h;
	qboolean     tried;
} decaltex_t;

typedef struct {
	msurface_t  *surf;
	int          tex;
	worldvert_t *verts;
	int          numverts;
	qboolean     permanent;
} decal_t;

static decaltex_t *dtex;
static int dtex_count;
static void *decal_wad;
static qboolean decal_wad_tried;

static decal_t *decals;
static int decals_count, decals_cap;
static cvar_t *r_decals;

// one list for the game (DecalIndex), the client (Draw_DecalIndexFromName) and messages
int R_DecalIndex(const char *name)
{
	if (!name || !*name)
		return -1;
	for (int i = 0; i < dtex_count; i++)
		if (!stricmp(dtex[i].name, name))
			return i;
	dtex = Mem_Realloc(dtex, sizeof(decaltex_t) * (dtex_count + 1));
	memset(&dtex[dtex_count], 0, sizeof(decaltex_t));
	Q_strncpy(dtex[dtex_count].name, name, sizeof(dtex[0].name));
	return dtex_count++;
}

static decaltex_t *DecalTexture(int index)
{
	if (index < 0 || index >= dtex_count)
		return NULL;
	decaltex_t *d = &dtex[index];
	if (d->tried)
		return d->gl ? d : NULL;
	d->tried = true;
	if (!decal_wad_tried) {
		decal_wad_tried = true;
		decal_wad = Wad_Open("decals.wad");
	}
	size_t size = 0;
	miptex_t *mt = Wad_FindIn(decal_wad, d->name, &size);
	if (!mt)
		mt = Wad_FindMiptex(d->name, &size);
	if (!mt) {
		Con_DPrintf("Decal %s not found\n", d->name);
		return NULL;
	}
	unsigned w = mt->width, h = mt->height;
	size_t pixels = (size_t)w * h;
	size_t palofs = mt->offsets[3] + (size_t)(w / 8) * (h / 8);
	if (!w || !h || w > 4096 || h > 4096 || mt->offsets[0] + pixels > size || palofs + 2 + 768 > size)
		return NULL;
	const unsigned char *base = (const unsigned char *)mt;
	const unsigned char *idx = base + mt->offsets[0];
	const unsigned char *color = base + palofs + 2 + 255 * 3;
	unsigned char *rgba = Mem_Alloc(pixels * 4);
	for (size_t i = 0; i < pixels; i++) {
		rgba[i * 4 + 0] = color[0];
		rgba[i * 4 + 1] = color[1];
		rgba[i * 4 + 2] = color[2];
		rgba[i * 4 + 3] = idx[i];
	}
	d->gl = GL_UploadTexture(va("*decal/%s", d->name), rgba, (int)w, (int)h, TF_MIPMAP | TF_CLAMP | TF_ALPHA);
	d->w = (int)w;
	d->h = (int)h;
	Mem_Free(rgba);
	return d;
}

// ---------------------------------------------------------------- placing

static void FreeDecal(int i)
{
	Mem_Free(decals[i].verts);
	memmove(&decals[i], &decals[i + 1], sizeof(decal_t) * (decals_count - i - 1));
	decals_count--;
}

void R_ClearDecals(void)
{
	for (int i = 0; i < decals_count; i++)
		Mem_Free(decals[i].verts);
	decals_count = 0;
}

void R_DecalRemoveAll(int tex)
{
	for (int i = decals_count - 1; i >= 0; i--)
		if (decals[i].tex == tex)
			FreeDecal(i);
}

// keeps the part of poly in front of the plane (n, d), at most max points; returns the new count
static int ClipPoly(vec3_t *in, int n, vec3_t *out, int max, const vec3_t normal, float dist)
{
	int count = 0;
	for (int i = 0; i < n && count < max - 1; i++) {
		const float *a = in[i], *b = in[(i + 1) % n];
		float da = DotProduct(a, normal) - dist, db = DotProduct(b, normal) - dist;
		if (da >= 0) {
			VectorCopy(a, out[count]);   // a macro: no ++ inside it
			count++;
		}
		if ((da >= 0) != (db >= 0)) {
			float t = da / (da - db);
			for (int k = 0; k < 3; k++)
				out[count][k] = a[k] + t * (b[k] - a[k]);
			count++;
		}
	}
	return count;
}

typedef struct {
	vec3_t pos;
	int    tex;
	float  w, h, radius;
	qboolean permanent;
} decalshot_t;

static void AddToSurface(msurface_t *s, const decalshot_t *shot)
{
	if (!s->numverts || (s->flags & (SURF_SKY | SURF_WATER)))
		return;
	vec3_t normal;
	float pdist = s->plane->dist;
	VectorCopy(s->plane->normal, normal);
	if (s->flags & SURF_PLANEBACK) {
		VectorScale(normal, -1, normal);
		pdist = -pdist;
	}
	// the shot must be on the surface or just in front of it, not behind (the far side of a wall)
	float d = DotProduct(shot->pos, normal) - pdist;
	if (d < -1 || d > 4)
		return;

	// the decal's axes follow the surface's texture
	const mtexinfo_t *ti = s->texinfo;
	vec3_t sa = { ti->vecs[0][0], ti->vecs[0][1], ti->vecs[0][2] };
	vec3_t ta = { ti->vecs[1][0], ti->vecs[1][1], ti->vecs[1][2] };
	VectorMA(sa, -DotProduct(sa, normal), normal, sa);
	VectorMA(ta, -DotProduct(ta, normal), normal, ta);
	if (VectorNormalize(sa) == 0 || VectorNormalize(ta) == 0)
		return;
	vec3_t c;
	VectorMA(shot->pos, -d, normal, c);

	// quick reject: the decal's centre far outside the polygon's box
	vec3_t mins = { 1e9f, 1e9f, 1e9f }, maxs = { -1e9f, -1e9f, -1e9f };
	for (int i = 0; i < s->numverts; i++)
		for (int k = 0; k < 3; k++) {
			float v = world.verts[s->firstvert + i].xyz[k];
			if (v < mins[k]) mins[k] = v;
			if (v > maxs[k]) maxs[k] = v;
		}
	for (int k = 0; k < 3; k++)
		if (c[k] < mins[k] - shot->radius || c[k] > maxs[k] + shot->radius)
			return;

	int cap = 2 * s->numverts + 16;   // clipping a convex polygon adds at most one point per edge
	vec3_t *a = Mem_Alloc(sizeof(vec3_t) * cap * 2), *b = a + cap;
	float hw = shot->w * 0.5f, hh = shot->h * 0.5f;
	for (int k = 0; k < 3; k++) {
		a[0][k] = c[k] - sa[k] * hw - ta[k] * hh;
		a[1][k] = c[k] + sa[k] * hw - ta[k] * hh;
		a[2][k] = c[k] + sa[k] * hw + ta[k] * hh;
		a[3][k] = c[k] - sa[k] * hw + ta[k] * hh;
	}
	int n = 4;
	// clip by each edge of the surface, keeping the inside
	vec3_t centre = { (mins[0] + maxs[0]) * 0.5f, (mins[1] + maxs[1]) * 0.5f, (mins[2] + maxs[2]) * 0.5f };
	for (int i = 0; i < s->numverts && n >= 3; i++) {
		const float *p0 = world.verts[s->firstvert + i].xyz;
		const float *p1 = world.verts[s->firstvert + (i + 1) % s->numverts].xyz;
		vec3_t edge, en;
		VectorSubtract(p1, p0, edge);
		CrossProduct(normal, edge, en);
		if (VectorNormalize(en) == 0)
			continue;
		vec3_t tc;
		VectorSubtract(centre, p0, tc);
		if (DotProduct(tc, en) < 0)
			VectorScale(en, -1, en);
		n = ClipPoly(a, n, b, cap, en, DotProduct(p0, en) - 0.01f);
		vec3_t *tmp = a; a = b; b = tmp;
	}
	if (n < 3) {
		Mem_Free(a < b ? a : b);
		return;
	}

	if (decals_count >= (int)r_decals->value) {
		for (int i = 0; i < decals_count; i++)
			if (!decals[i].permanent) {
				FreeDecal(i);
				break;
			}
		if (decals_count >= (int)r_decals->value && decals_count > 0) {
			Mem_Free(a < b ? a : b);
			return;
		}
	}
	if (decals_count == decals_cap) {
		decals_cap = decals_cap ? decals_cap * 2 : 256;
		decals = Mem_Realloc(decals, sizeof(decal_t) * decals_cap);
	}
	decal_t *dc = &decals[decals_count++];
	dc->surf = s;
	dc->tex = shot->tex;
	dc->permanent = shot->permanent;
	dc->numverts = n;
	dc->verts = Mem_Alloc(sizeof(worldvert_t) * n);
	for (int i = 0; i < n; i++) {
		worldvert_t *v = &dc->verts[i];
		VectorCopy(a[i], v->xyz);
		vec3_t r;
		VectorSubtract(a[i], c, r);
		v->st[0] = DotProduct(r, sa) / shot->w + 0.5f;
		v->st[1] = DotProduct(r, ta) / shot->h + 0.5f;
		if (!(s->flags & SURF_NOLIGHT)) {
			float su = DotProduct(v->xyz, ti->vecs[0]) + ti->vecs[0][3];
			float tv = DotProduct(v->xyz, ti->vecs[1]) + ti->vecs[1][3];
			v->lm[0] = (su - s->texturemins[0] + s->lm_x * 16 + 8) / (LM_PAGE * 16.0f);
			v->lm[1] = (tv - s->texturemins[1] + s->lm_y * 16 + 8) / (LM_PAGE * 16.0f);
		}
	}
	Mem_Free(a < b ? a : b);
}

static void DecalNode(mnode_t *node, const decalshot_t *shot, int depth)
{
	while (node && node->contents == 0 && depth < 1024) {
		float d = DotProduct(shot->pos, node->plane->normal) - node->plane->dist;
		if (d > shot->radius) {
			node = node->children[0];
		} else if (d < -shot->radius) {
			node = node->children[1];
		} else {
			// the surfaces on this node's plane
			if (fabsf(d) < 4)
				for (int i = 0; i < node->numsurfaces; i++)
					AddToSurface(&world.surfaces[node->firstsurface + i], shot);
			DecalNode(node->children[0], shot, depth + 1);
			node = node->children[1];
		}
		depth++;
	}
}

void R_DecalShoot(int tex, int entity, int modelIndex, const float *pos, int flags, float scale)
{
	if (!world.loaded || !world.numnodes)
		return;
	decaltex_t *dt = DecalTexture(tex);
	if (!dt)
		return;
	decalshot_t shot;
	VectorCopy(pos, shot.pos);
	int submodel = 0;
	if (entity > 0) {
		if (!sv.active || entity >= sv.num_edicts)
			return;
		edict_t *e = EDICT_NUM(entity);
		model_t *m = SV_ModelForIndex(modelIndex ? modelIndex : e->v.modelindex);
		if (!m || m->type != mod_brush || e->free)
			return;
		submodel = m->submodel;
		// into the brush model's own space, where its surfaces are
		VectorSubtract(pos, e->v.origin, shot.pos);
		if (e->v.angles[0] || e->v.angles[1] || e->v.angles[2]) {
			vec3_t f, r, u, t;
			AngleVectors(e->v.angles, f, r, u);
			VectorCopy(shot.pos, t);
			shot.pos[0] = DotProduct(t, f);
			shot.pos[1] = -DotProduct(t, r);
			shot.pos[2] = DotProduct(t, u);
		}
	}
	if (submodel < 0 || submodel >= world.numsubmodels)
		return;
	if (scale <= 0)
		scale = 1;
	shot.tex = tex;
	shot.w = dt->w * scale;
	shot.h = dt->h * scale;
	shot.radius = sqrtf(shot.w * shot.w + shot.h * shot.h) * 0.5f;
	shot.permanent = (flags & 1) != 0;   // FDECAL_PERMANENT: map decals stay when the limit is reached
	int head = world.submodels[submodel].headnode;
	if (head >= 0 && head < world.numnodes)
		DecalNode(&world.nodes[head], &shot, 0);
}

// ---------------------------------------------------------------- drawing

// Draws the decals of faces [first, first + count) that are visible this frame; the caller restores
// its vertex arrays afterwards. viewer is the camera in the faces' own space.
void R_DrawDecals(int first, int count, const float *viewer, qboolean world_pass)
{
	if (!decals_count)
		return;
	qboolean fullbright = Cvar_VariableValue("r_fullbright") != 0;
	qboolean started = false;
	for (int i = 0; i < decals_count; i++) {
		decal_t *dc = &decals[i];
		int sn = (int)(dc->surf - world.surfaces);
		if (sn < first || sn >= first + count)
			continue;
		msurface_t *s = dc->surf;
		if (world_pass && s->visframe != r_refdef.framecount)
			continue;
		float d = DotProduct(viewer, s->plane->normal) - s->plane->dist;
		if ((s->flags & SURF_PLANEBACK) ? d > 0 : d < 0)
			continue;
		decaltex_t *dt = DecalTexture(dc->tex);
		if (!dt)
			continue;
		if (!started) {
			started = true;
			qglBindBuffer(GL_ARRAY_BUFFER, 0);
			glEnableClientState(GL_VERTEX_ARRAY);
			qglClientActiveTexture(GL_TEXTURE0);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			qglClientActiveTexture(GL_TEXTURE1);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			qglClientActiveTexture(GL_TEXTURE0);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1, -2);
			glColor4f(1, 1, 1, 1);
		}
		glVertexPointer(3, GL_FLOAT, sizeof(worldvert_t), dc->verts[0].xyz);
		glTexCoordPointer(2, GL_FLOAT, sizeof(worldvert_t), dc->verts[0].st);
		qglClientActiveTexture(GL_TEXTURE1);
		glTexCoordPointer(2, GL_FLOAT, sizeof(worldvert_t), dc->verts[0].lm);
		qglClientActiveTexture(GL_TEXTURE0);

		qglActiveTexture(GL_TEXTURE1);
		if ((s->flags & SURF_NOLIGHT) || fullbright) {
			glDisable(GL_TEXTURE_2D);
		} else {
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, world.lightmaps[s->lm_page]);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
		qglActiveTexture(GL_TEXTURE0);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, dt->gl->id);
		glDrawArrays(GL_TRIANGLE_FAN, 0, dc->numverts);
	}
	if (started) {
		glDisable(GL_POLYGON_OFFSET_FILL);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
	}
}

static void DecalCount_f(void)
{
	Con_Printf("%d decals, %d decal textures known\n", decals_count, dtex_count);
}

void R_InitDecals(void)
{
	r_decals = Cvar_Get("r_decals", "4096", FCVAR_ARCHIVE, "most decals (bullet holes, blood) at once");
	Cmd_AddCommand("decalcount", DecalCount_f, "shows how many decals there are");
}
