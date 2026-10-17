// newen - model cache and loaders for studio models (.mdl) and sprites (.spr)
//
// Studio files stay in memory whole; the renderer reads bones and animations straight from them.
// Everything that is decoded (textures, triangle lists) is sized from the file, with no fixed limits.
#include "model.h"
#include "bsp.h"

static model_t **models;
static int models_count, models_cap;

// ---------------------------------------------------------------- helpers

// true if count elements of elemsize at ofs lie inside the file
static qboolean InFile(size_t filesize, int ofs, int count, size_t elemsize)
{
	if (count == 0)
		return true;
	if (ofs < 0 || count < 0)
		return false;
	return (size_t)ofs + (size_t)count * elemsize <= filesize;
}

static void BaseName(const char *path, char *out, size_t size)
{
	const char *base = path;
	for (const char *s = path; *s; s++)
		if (*s == '/' || *s == '\\')
			base = s + 1;
	Q_strncpy(out, base, size);
	char *dot = strrchr(out, '.');
	if (dot)
		*dot = 0;
}

// ---------------------------------------------------------------- studio models

static qboolean CheckStudioHeader(const studiohdr_t *h, size_t size, const char *name, qboolean textures_only)
{
	if (size < sizeof(studiohdr_t)) {
		Con_Warnf("%s is too small to be a model\n", name);
		return false;
	}
	if (h->id != IDSTUDIOHEADER) {
		Con_Warnf("%s isn't a studio model\n", name);
		return false;
	}
	if (h->version != STUDIO_VERSION) {
		Con_Warnf("%s is studio version %d, expected %d (Half-Life)\n", name, h->version, STUDIO_VERSION);
		return false;
	}
	qboolean ok = InFile(size, h->textureindex, h->numtextures, sizeof(mstudiotexture_t)) &&
		InFile(size, h->skinindex, h->numskinref * h->numskinfamilies, sizeof(short));
	if (!textures_only)
		ok = ok && InFile(size, h->boneindex, h->numbones, sizeof(mstudiobone_t)) &&
			InFile(size, h->bonecontrollerindex, h->numbonecontrollers, sizeof(mstudiobonecontroller_t)) &&
			InFile(size, h->hitboxindex, h->numhitboxes, sizeof(mstudiobbox_t)) &&
			InFile(size, h->seqindex, h->numseq, sizeof(mstudioseqdesc_t)) &&
			InFile(size, h->seqgroupindex, h->numseqgroups, sizeof(mstudioseqgroup_t)) &&
			InFile(size, h->bodypartindex, h->numbodyparts, sizeof(mstudiobodyparts_t)) &&
			InFile(size, h->attachmentindex, h->numattachments, sizeof(mstudioattachment_t));
	if (!ok)
		Con_Warnf("%s is damaged (a table points outside the file)\n", name);
	return ok;
}

static gltexture_t *LoadStudioTexture(const model_t *mod, const studiohdr_t *th, size_t thsize,
	const mstudiotexture_t *t)
{
	// HD replacement: models/textures/<model>/<texture>.png
	char modelbase[64], texbase[64];
	BaseName(mod->name, modelbase, sizeof(modelbase));
	BaseName(t->name, texbase, sizeof(texbase));
	int flags = TF_MODEL | ((t->flags & STUDIO_NF_NOMIPS) ? 0 : TF_MIPMAP);
	if (t->flags & STUDIO_NF_MASKED)
		flags |= TF_ALPHA;
	int w, h;
	unsigned char *rgba = Image_LoadAny(va("models/textures/%s", modelbase), texbase, &w, &h);
	if (rgba) {
		gltexture_t *gl = GL_UploadTexture(texbase, rgba, w, h, flags);
		gl->hd = true;
		Mem_Free(rgba);
		return gl;
	}

	if (t->width <= 0 || t->height <= 0 || t->width > 8192 || t->height > 8192 ||
		!InFile(thsize, t->index, t->width * t->height + 768, 1)) {
		Con_Warnf("%s: texture %s is damaged\n", mod->name, t->name);
		return GL_MissingTexture();
	}
	const unsigned char *pix = (const unsigned char *)th + t->index;
	const unsigned char *pal = pix + t->width * t->height;
	size_t n = (size_t)t->width * t->height;
	rgba = Mem_Alloc(n * 4);
	for (size_t i = 0; i < n; i++) {
		int c = pix[i];
		rgba[i * 4 + 0] = pal[c * 3 + 0];
		rgba[i * 4 + 1] = pal[c * 3 + 1];
		rgba[i * 4 + 2] = pal[c * 3 + 2];
		rgba[i * 4 + 3] = ((t->flags & STUDIO_NF_MASKED) && c == 255) ? 0 : 255;
	}
	gltexture_t *gl = GL_UploadTexture(texbase, rgba, t->width, t->height, flags);
	Mem_Free(rgba);
	return gl;
}

// Strips and fans become one triangle list per mesh, so drawing is a single call
static qboolean DecodeMesh(const studiohdr_t *h, size_t size, const mstudiomodel_t *sm,
	const mstudiomesh_t *mesh, studiomesh_t *out)
{
	out->skinref = mesh->skinref;
	if (mesh->triindex < 0 || (size_t)mesh->triindex >= size)
		return false;
	const short *cmd = (const short *)((const unsigned char *)h + mesh->triindex);
	const short *end = (const short *)((const unsigned char *)h + size);

	int cap = 0;
	for (;;) {
		if (cmd >= end)
			return false;
		int count = *cmd++;
		if (!count)
			break;
		qboolean fan = count < 0;
		if (fan)
			count = -count;
		if (cmd + count * 4 > end)
			return false;
		const short *v = cmd;
		cmd += count * 4;
		for (int i = 0; i < count; i++)
			if (v[i * 4] < 0 || v[i * 4] >= sm->numverts || v[i * 4 + 1] < 0 || v[i * 4 + 1] >= sm->numnorms)
				return false;
		for (int i = 2; i < count; i++) {
			int a, b, c;
			if (fan) {
				a = 0; b = i - 1; c = i;
			} else if (i & 1) {
				a = i - 1; b = i - 2; c = i;   // odd strip triangles flip to keep the winding
			} else {
				a = i - 2; b = i - 1; c = i;
			}
			if (out->numtris == cap) {
				cap = cap ? cap * 2 : 64;
				out->tris = Mem_Realloc(out->tris, sizeof(short) * 12 * cap);
			}
			short *t = out->tris + out->numtris * 12;
			memcpy(t + 0, v + a * 4, sizeof(short) * 4);
			memcpy(t + 4, v + b * 4, sizeof(short) * 4);
			memcpy(t + 8, v + c * 4, sizeof(short) * 4);
			out->numtris++;
		}
	}
	return true;
}

static void *LoadSeqGroup(const model_t *mod, const mstudioseqgroup_t *g, int index, size_t *size)
{
	char path[MAX_OSPATH];
	Q_strncpy(path, g->name, sizeof(path));
	for (char *s = path; *s; s++)
		if (*s == '\\')
			*s = '/';
	unsigned char *data = *path ? FS_LoadFile(path, size) : NULL;
	if (!data) {
		// the stored name can come from the modeler's machine; fall back to <model>NN.mdl
		Q_strncpy(path, mod->name, sizeof(path));
		char *dot = strrchr(path, '.');
		if (dot)
			*dot = 0;
		data = FS_LoadFile(va("%s%02d.mdl", path, index), size);
	}
	if (!data) {
		Con_Warnf("%s: sequence group %d (%s) not found\n", mod->name, index, g->name);
		return NULL;
	}
	if (*size < sizeof(studioseqhdr_t) || ((studioseqhdr_t *)data)->id != IDSEQGRPHEADER) {
		Con_Warnf("%s: %s isn't a sequence group file\n", mod->name, path);
		Mem_Free(data);
		return NULL;
	}
	return data;
}

static void FreeStudio(model_t *m)
{
	studiomodel_t *s = &m->studio;
	if (s->meshes) {
		int sub = 0;
		studiohdr_t *h = s->hdr;
		mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
		for (int i = 0; i < h->numbodyparts; i++) {
			mstudiomodel_t *sm = STUDIO_PTR(h, bp[i].modelindex, mstudiomodel_t);
			for (int j = 0; j < bp[i].nummodels; j++, sub++) {
				if (!s->meshes[sub])
					continue;
				for (int k = 0; k < sm[j].nummesh; k++)
					Mem_Free(s->meshes[sub][k].tris);
				Mem_Free(s->meshes[sub]);
			}
		}
		Mem_Free(s->meshes);
	}
	if (s->seqdata && s->hdr)
		for (int i = 1; i < s->hdr->numseqgroups; i++)
			Mem_Free(s->seqdata[i]);
	Mem_Free(s->seqdata);
	Mem_Free(s->seqsize);
	Mem_Free(s->textures);
	if (s->texhdr != s->hdr)
		Mem_Free(s->texhdr);
	Mem_Free(s->hdr);
	memset(s, 0, sizeof(*s));
}

static qboolean LoadStudio(model_t *mod, unsigned char *data, size_t size)
{
	studiohdr_t *h = (studiohdr_t *)data;
	studiomodel_t *s = &mod->studio;
	s->hdr = h;
	s->texhdr = h;
	if (!CheckStudioHeader(h, size, mod->name, false))
		return false;

	// bodyparts, models and meshes must all be inside the file before anything reads them
	mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
	for (int i = 0; i < h->numbodyparts; i++) {
		if (!InFile(size, bp[i].modelindex, bp[i].nummodels, sizeof(mstudiomodel_t))) {
			Con_Warnf("%s is damaged (bodypart %d)\n", mod->name, i);
			return false;
		}
		mstudiomodel_t *sm = STUDIO_PTR(h, bp[i].modelindex, mstudiomodel_t);
		for (int j = 0; j < bp[i].nummodels; j++) {
			if (!InFile(size, sm[j].meshindex, sm[j].nummesh, sizeof(mstudiomesh_t)) ||
				!InFile(size, sm[j].vertinfoindex, sm[j].numverts, 1) ||
				!InFile(size, sm[j].vertindex, sm[j].numverts, sizeof(vec3_t)) ||
				!InFile(size, sm[j].norminfoindex, sm[j].numnorms, 1) ||
				!InFile(size, sm[j].normindex, sm[j].numnorms, sizeof(vec3_t))) {
				Con_Warnf("%s is damaged (model %s)\n", mod->name, sm[j].name);
				return false;
			}
			const unsigned char *vi = STUDIO_PTR(h, sm[j].vertinfoindex, unsigned char);
			const unsigned char *ni = STUDIO_PTR(h, sm[j].norminfoindex, unsigned char);
			for (int k = 0; k < sm[j].numverts; k++)
				if (vi[k] >= h->numbones) { Con_Warnf("%s: vertex bone out of range\n", mod->name); return false; }
			for (int k = 0; k < sm[j].numnorms; k++)
				if (ni[k] >= h->numbones) { Con_Warnf("%s: normal bone out of range\n", mod->name); return false; }
		}
		s->numsubmodels += bp[i].nummodels;
	}
	mstudiobone_t *bones = STUDIO_PTR(h, h->boneindex, mstudiobone_t);
	for (int i = 0; i < h->numbones; i++)
		if (bones[i].parent < -1 || bones[i].parent >= i) {
			// parents always come first; anything else would loop or read garbage
			Con_Warnf("%s: bone %s has a bad parent\n", mod->name, bones[i].name);
			return false;
		}

	// sequence groups (animations for group 0 live in this file)
	int ngroups = h->numseqgroups > 0 ? h->numseqgroups : 1;
	s->seqdata = Mem_Alloc(sizeof(unsigned char *) * ngroups);
	s->seqsize = Mem_Alloc(sizeof(size_t) * ngroups);
	s->seqdata[0] = data;
	s->seqsize[0] = size;
	mstudioseqgroup_t *groups = STUDIO_PTR(h, h->seqgroupindex, mstudioseqgroup_t);
	for (int i = 1; i < h->numseqgroups; i++)
		s->seqdata[i] = LoadSeqGroup(mod, &groups[i], i, &s->seqsize[i]);

	// textures may be in a separate <name>T.mdl
	size_t texsize = size;
	if (h->numtextures == 0) {
		char path[MAX_OSPATH];
		Q_strncpy(path, mod->name, sizeof(path));
		char *dot = strrchr(path, '.');
		if (dot)
			*dot = 0;
		unsigned char *tdata = FS_LoadFile(va("%sT.mdl", path), &texsize);
		if (tdata && CheckStudioHeader((studiohdr_t *)tdata, texsize, va("%sT.mdl", path), true))
			s->texhdr = (studiohdr_t *)tdata;
		else {
			Mem_Free(tdata);
			texsize = size;
		}
	}
	studiohdr_t *th = s->texhdr;
	mstudiotexture_t *tex = STUDIO_PTR(th, th->textureindex, mstudiotexture_t);
	s->textures = Mem_Alloc(sizeof(studiotex_t) * (th->numtextures ? th->numtextures : 1));
	int hd = 0;
	for (int i = 0; i < th->numtextures; i++) {
		s->textures[i].flags = tex[i].flags;
		s->textures[i].width = tex[i].width > 0 ? tex[i].width : 1;
		s->textures[i].height = tex[i].height > 0 ? tex[i].height : 1;
		s->textures[i].gl = LoadStudioTexture(mod, th, texsize, &tex[i]);
		hd += s->textures[i].gl->hd;
	}
	if (hd)
		Con_DPrintf("%s: %d HD texture%s\n", mod->name, hd, hd == 1 ? "" : "s");

	// triangle lists
	s->meshes = Mem_Alloc(sizeof(studiomesh_t *) * (s->numsubmodels ? s->numsubmodels : 1));
	int sub = 0;
	for (int i = 0; i < h->numbodyparts; i++) {
		mstudiomodel_t *sm = STUDIO_PTR(h, bp[i].modelindex, mstudiomodel_t);
		for (int j = 0; j < bp[i].nummodels; j++, sub++) {
			s->meshes[sub] = Mem_Alloc(sizeof(studiomesh_t) * (sm[j].nummesh ? sm[j].nummesh : 1));
			mstudiomesh_t *mesh = STUDIO_PTR(h, sm[j].meshindex, mstudiomesh_t);
			for (int k = 0; k < sm[j].nummesh; k++)
				if (!DecodeMesh(h, size, &sm[j], &mesh[k], &s->meshes[sub][k])) {
					Con_Warnf("%s: mesh %d of %s is damaged, skipping it\n", mod->name, k, sm[j].name);
					s->meshes[sub][k].numtris = 0;
				}
		}
	}

	// bounds for culling: the clipping box, or the first sequence's box
	VectorCopy(h->bbmin, mod->mins);
	VectorCopy(h->bbmax, mod->maxs);
	if (!(mod->mins[0] || mod->mins[1] || mod->mins[2] || mod->maxs[0] || mod->maxs[1] || mod->maxs[2]) && h->numseq) {
		mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t);
		VectorCopy(seq->bbmin, mod->mins);
		VectorCopy(seq->bbmax, mod->maxs);
	}
	mod->numframes = h->numseq;
	return true;
}

// ---------------------------------------------------------------- sprites

static qboolean LoadSprite(model_t *mod, unsigned char *data, size_t size)
{
	spritemodel_t *sp = &mod->sprite;
	if (size < sizeof(dsprite_t) + 2) {
		Con_Warnf("%s is too small to be a sprite\n", mod->name);
		return false;
	}
	dsprite_t *h = (dsprite_t *)data;
	if (h->ident != IDSPRITEHEADER || h->version != SPRITE_VERSION) {
		Con_Warnf("%s isn't a Half-Life sprite (version %d)\n", mod->name, h->version);
		return false;
	}
	sp->type = h->type;
	sp->texFormat = h->texFormat;
	sp->maxwidth = h->width;
	sp->maxheight = h->height;
	unsigned char *p = data + sizeof(dsprite_t);
	unsigned char *end = data + size;
	int palcount = *(short *)p;
	p += 2;
	if (palcount <= 0 || palcount > 256 || p + palcount * 3 > end) {
		Con_Warnf("%s has a bad palette\n", mod->name);
		return false;
	}
	unsigned char pal[256 * 3] = { 0 };
	memcpy(pal, p, palcount * 3);
	p += palcount * 3;

	if (h->numframes <= 0) {
		Con_Warnf("%s has no frames\n", mod->name);
		return false;
	}
	sp->numframes = h->numframes;
	sp->frames = Mem_Alloc(sizeof(spritegroup_t) * sp->numframes);
	float radius = 0;
	for (int f = 0; f < sp->numframes; f++) {
		if (p + 4 > end)
			goto damaged;
		int type = *(int *)p;
		p += 4;
		spritegroup_t *g = &sp->frames[f];
		g->numframes = 1;
		if (type != 0) {
			if (p + 4 > end)
				goto damaged;
			g->numframes = *(int *)p;
			p += 4;
			if (g->numframes <= 0 || p + g->numframes * 4 > end)
				goto damaged;
			g->intervals = Mem_Alloc(sizeof(float) * g->numframes);
			float total = 0;
			for (int i = 0; i < g->numframes; i++) {
				float iv = ((float *)p)[i];
				total += iv > 0 ? iv : 0.1f;
				g->intervals[i] = total;
			}
			p += g->numframes * 4;
		}
		g->frames = Mem_Alloc(sizeof(spriteframe_t) * g->numframes);
		for (int i = 0; i < g->numframes; i++) {
			if (p + sizeof(dspriteframe_t) > end)
				goto damaged;
			dspriteframe_t *df = (dspriteframe_t *)p;
			p += sizeof(dspriteframe_t);
			int w = df->width, hgt = df->height;
			if (w <= 0 || hgt <= 0 || w > 8192 || hgt > 8192 || p + (size_t)w * hgt > end)
				goto damaged;
			size_t n = (size_t)w * hgt;
			unsigned char *rgba = Mem_Alloc(n * 4);
			for (size_t k = 0; k < n; k++) {
				int c = p[k];
				unsigned char *o = rgba + k * 4;
				if (sp->texFormat == SPR_INDEXALPHA) {
					// the last palette color tints the whole sprite; the index is the opacity
					o[0] = pal[255 * 3 + 0]; o[1] = pal[255 * 3 + 1]; o[2] = pal[255 * 3 + 2];
					o[3] = (unsigned char)c;
				} else {
					o[0] = pal[c * 3 + 0]; o[1] = pal[c * 3 + 1]; o[2] = pal[c * 3 + 2];
					o[3] = (sp->texFormat == SPR_ALPHTEST && c == 255) ? 0 : 255;
				}
			}
			p += n;
			spriteframe_t *fr = &g->frames[i];
			int flags = TF_MODEL | TF_CLAMP | TF_MIPMAP;
			if (sp->texFormat == SPR_ALPHTEST || sp->texFormat == SPR_INDEXALPHA)
				flags |= TF_ALPHA;
			fr->gl = GL_UploadTexture(va("%s:%d", mod->name, f), rgba, w, hgt, flags);
			Mem_Free(rgba);
			fr->left = (float)df->origin[0];
			fr->right = (float)(df->origin[0] + w);
			fr->up = (float)df->origin[1];
			fr->down = (float)(df->origin[1] - hgt);
			float rx = fabsf(fr->left) > fabsf(fr->right) ? fabsf(fr->left) : fabsf(fr->right);
			float ry = fabsf(fr->up) > fabsf(fr->down) ? fabsf(fr->up) : fabsf(fr->down);
			float r = sqrtf(rx * rx + ry * ry);
			if (r > radius)
				radius = r;
		}
	}
	mod->numframes = sp->numframes;
	mod->radius = radius;
	VectorSet(mod->mins, -radius, -radius, -radius);
	VectorSet(mod->maxs, radius, radius, radius);
	return true;

damaged:
	Con_Warnf("%s is damaged\n", mod->name);
	return false;
}

static void FreeSprite(model_t *m)
{
	spritemodel_t *sp = &m->sprite;
	for (int f = 0; f < sp->numframes && sp->frames; f++) {
		Mem_Free(sp->frames[f].intervals);
		Mem_Free(sp->frames[f].frames);
	}
	Mem_Free(sp->frames);
	memset(sp, 0, sizeof(*sp));
}

// ---------------------------------------------------------------- cache

static void FreeModel(model_t *m)
{
	if (m->type == mod_studio)
		FreeStudio(m);
	else if (m->type == mod_sprite)
		FreeSprite(m);
	Mem_Free(m);
}

model_t *Mod_ForName(const char *name, qboolean crash)
{
	if (!name || !*name)
		return NULL;
	char fixed[64];
	Q_strncpy(fixed, name, sizeof(fixed));
	for (char *s = fixed; *s; s++)
		if (*s == '\\')
			*s = '/';
	for (int i = 0; i < models_count; i++)
		if (!stricmp(models[i]->name, fixed))
			return models[i]->type == mod_bad ? NULL : models[i];

	model_t *m = Mem_Alloc(sizeof(*m));
	Q_strncpy(m->name, fixed, sizeof(m->name));
	if (models_count == models_cap) {
		models_cap = models_cap ? models_cap * 2 : 64;
		models = Mem_Realloc(models, sizeof(*models) * models_cap);
	}
	models[models_count++] = m;   // failures stay cached as mod_bad, so they warn once

	if (fixed[0] == '*') {
		int n = atoi(fixed + 1);
		if (n > 0 && n < world.numsubmodels) {
			m->type = mod_brush;
			m->submodel = n;
			VectorCopy(world.submodels[n].mins, m->mins);
			VectorCopy(world.submodels[n].maxs, m->maxs);
			return m;
		}
		return NULL;
	}

	size_t size;
	unsigned char *data = FS_LoadFile(fixed, &size);
	if (!data) {
		if (crash)
			Sys_Error("Model %s not found", fixed);
		Con_Warnf("Model %s not found\n", fixed);
		return NULL;
	}
	qboolean ok = false;
	if (size >= 4 && *(int *)data == IDSTUDIOHEADER) {
		m->type = mod_studio;
		ok = LoadStudio(m, data, size);   // keeps data
	} else if (size >= 4 && *(int *)data == IDSPRITEHEADER) {
		m->type = mod_sprite;
		ok = LoadSprite(m, data, size);
		Mem_Free(data);
	} else {
		Con_Warnf("%s isn't a model or sprite this engine knows\n", fixed);
		Mem_Free(data);
	}
	if (!ok) {
		if (m->type == mod_studio)
			FreeStudio(m);   // frees data too
		else if (m->type == mod_sprite)
			FreeSprite(m);
		m->type = mod_bad;
		return NULL;
	}
	float r = 0;
	for (int i = 0; i < 3; i++) {
		float a = fabsf(m->mins[i]), b = fabsf(m->maxs[i]);
		float c = a > b ? a : b;
		r += c * c;
	}
	if (m->type == mod_studio)
		m->radius = sqrtf(r);
	return m;
}

void Mod_ClearAll(void)
{
	for (int i = 0; i < models_count; i++)
		FreeModel(models[i]);
	models_count = 0;
	GL_FreeTextures(TF_MODEL);
}

// ---------------------------------------------------------------- commands

void Mod_PrintInfo(const model_t *m)
{
	if (m->type == mod_sprite) {
		static const char *types[] = { "parallel upright", "facing upright", "parallel", "oriented", "parallel oriented" };
		static const char *formats[] = { "normal", "additive", "index alpha", "alpha test" };
		const spritemodel_t *sp = &m->sprite;
		Con_Printf("%s: sprite, %d frames, %dx%d, %s, %s\n", m->name, sp->numframes, sp->maxwidth, sp->maxheight,
			sp->type >= 0 && sp->type < 5 ? types[sp->type] : "?",
			sp->texFormat >= 0 && sp->texFormat < 4 ? formats[sp->texFormat] : "?");
		return;
	}
	if (m->type != mod_studio) {
		Con_Printf("%s: brush model *%d\n", m->name, m->submodel);
		return;
	}
	const studiohdr_t *h = m->studio.hdr;
	Con_Printf("%s: %d bones, %d controllers, %d sequences, %d body parts, %d skins, %d textures%s\n",
		m->name, h->numbones, h->numbonecontrollers, h->numseq, h->numbodyparts, m->studio.texhdr->numskinfamilies,
		m->studio.texhdr->numtextures, m->studio.texhdr != h ? " (from the T file)" : "");
	const mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t);
	for (int i = 0; i < h->numseq; i++)
		Con_Printf("  seq %3d  %-24s %4d frames  %5.1f fps%s%s\n", i, seq[i].label, seq[i].numframes, seq[i].fps,
			(seq[i].flags & STUDIO_LOOPING) ? "  loop" : "", seq[i].numblends > 1 ? "  blend" : "");
	const mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
	for (int i = 0; i < h->numbodyparts; i++)
		Con_Printf("  body part %d  %-16s %d choice%s\n", i, bp[i].name, bp[i].nummodels, bp[i].nummodels == 1 ? "" : "s");
	const studiohdr_t *th = m->studio.texhdr;
	const mstudiotexture_t *tex = STUDIO_PTR(th, th->textureindex, mstudiotexture_t);
	for (int i = 0; i < th->numtextures; i++)
		Con_Printf("  texture %-24s %4dx%-4d%s%s%s%s%s\n", tex[i].name, tex[i].width, tex[i].height,
			(tex[i].flags & STUDIO_NF_CHROME) ? " chrome" : "", (tex[i].flags & STUDIO_NF_FULLBRIGHT) ? " fullbright" : "",
			(tex[i].flags & STUDIO_NF_MASKED) ? " masked" : "", (tex[i].flags & STUDIO_NF_ADDITIVE) ? " additive" : "",
			m->studio.textures[i].gl->hd ? "  HD" : "");
}

static void ModelList_f(void)
{
	int n = 0;
	for (int i = 0; i < models_count; i++) {
		const model_t *m = models[i];
		static const char *types[] = { "bad", "brush", "studio", "sprite" };
		Con_Printf("%-7s %s\n", types[m->type], m->name);
		n++;
	}
	Con_Printf("%d models\n", n);
}

static void MdlInfo_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: mdlinfo <models/name.mdl | sprites/name.spr>\n");
		return;
	}
	const char *name = Cmd_Argv(1);
	model_t *m = Mod_ForName(strchr(name, '/') || name[0] == '*' ? name : va("models/%s.mdl", name), false);
	if (m)
		Mod_PrintInfo(m);
}

void Mod_Init(void)
{
	Cmd_AddCommand("modellist", ModelList_f, "lists loaded models and sprites");
	Cmd_AddCommand("mdlinfo", MdlInfo_f, "mdlinfo <model> - lists a model's sequences, body parts and textures");
}
