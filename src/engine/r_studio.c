// newen - studio model animation and drawing
//
// Bone setup follows GoldSrc: each bone's rotation and position come from run-length encoded
// animation values (scaled and added to the bone's defaults), are interpolated between frames
// (quaternion slerp), blended between a sequence's blend animations, and then chained parent to
// child. Vertices are skinned on the CPU (one bone per vertex) and lit like GoldSrc: ambient
// light plus a shade light from the lightmap under the model, with a wrapped ("lambert") falloff.
//
// In step 5 the client DLL's StudioModelRenderer drives this through the engine studio interface;
// R_StudioSetupBones and R_StudioDrawModel are the pieces it will call.
#include "model.h"
#include "bsp.h"

static cvar_t *r_studio_lambert, *r_studio_ambient, *r_studio_shade, *r_fullbright_ref;

// per-frame scratch, grown to the biggest model seen
static float (*bonetransform)[3][4];
static float (*q1)[4], (*q2)[4];
static vec3_t *pos1, *pos2;
static int bones_cap;
static vec3_t *xverts, *xnorms;
static float (*chromeuv)[2];
static int verts_cap, norms_cap;
static float *arr_xyz, *arr_st, *arr_rgba;
static int arr_cap;

static void EnsureBones(int n)
{
	if (n <= bones_cap)
		return;
	bones_cap = n;
	bonetransform = Mem_Realloc(bonetransform, sizeof(*bonetransform) * n);
	q1 = Mem_Realloc(q1, sizeof(*q1) * n);
	q2 = Mem_Realloc(q2, sizeof(*q2) * n);
	pos1 = Mem_Realloc(pos1, sizeof(*pos1) * n);
	pos2 = Mem_Realloc(pos2, sizeof(*pos2) * n);
}

// ---------------------------------------------------------------- math

static void AngleQuaternion(const float angles[3], float q[4])
{
	// rotations about x, then y, then z (radians)
	float sr = sinf(angles[0] * 0.5f), cr = cosf(angles[0] * 0.5f);
	float sp = sinf(angles[1] * 0.5f), cp = cosf(angles[1] * 0.5f);
	float sy = sinf(angles[2] * 0.5f), cy = cosf(angles[2] * 0.5f);
	q[0] = sr * cp * cy - cr * sp * sy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
	q[3] = cr * cp * cy + sr * sp * sy;
}

static void QuaternionSlerp(const float p[4], const float qin[4], float t, float out[4])
{
	float q[4];
	// take the short way round
	float a = 0, b = 0;
	for (int i = 0; i < 4; i++) {
		a += (p[i] - qin[i]) * (p[i] - qin[i]);
		b += (p[i] + qin[i]) * (p[i] + qin[i]);
	}
	for (int i = 0; i < 4; i++)
		q[i] = a > b ? -qin[i] : qin[i];

	float cosom = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
	float sclp, sclq;
	if (1.0f + cosom > 0.000001f) {
		if (1.0f - cosom > 0.000001f) {
			float omega = acosf(cosom);
			float sinom = sinf(omega);
			sclp = sinf((1.0f - t) * omega) / sinom;
			sclq = sinf(t * omega) / sinom;
		} else {
			sclp = 1.0f - t;
			sclq = t;
		}
		for (int i = 0; i < 4; i++)
			out[i] = sclp * p[i] + sclq * q[i];
	} else {
		// opposite: rotate through a perpendicular quaternion
		float perp[4] = { -q[1], q[0], -q[3], q[2] };
		sclp = sinf((1.0f - t) * 0.5f * M_PI_F);
		sclq = sinf(t * 0.5f * M_PI_F);
		for (int i = 0; i < 3; i++)
			out[i] = sclp * p[i] + sclq * perp[i];
		out[3] = perp[3];
	}
}

static void QuaternionMatrix(const float q[4], float m[3][4])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	m[0][0] = 1 - 2 * (y * y + z * z);
	m[1][0] = 2 * (x * y + w * z);
	m[2][0] = 2 * (x * z - w * y);
	m[0][1] = 2 * (x * y - w * z);
	m[1][1] = 1 - 2 * (x * x + z * z);
	m[2][1] = 2 * (y * z + w * x);
	m[0][2] = 2 * (x * z + w * y);
	m[1][2] = 2 * (y * z - w * x);
	m[2][2] = 1 - 2 * (x * x + y * y);
}

static void ConcatTransforms(const float a[3][4], const float b[3][4], float out[3][4])
{
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		out[i][3] += a[i][3];
	}
}

// entity angles to a matrix whose columns are forward, left, up
static void AngleMatrix(const vec3_t angles, float m[3][4])
{
	vec3_t f, r, u;
	AngleVectors(angles, f, r, u);
	for (int i = 0; i < 3; i++) {
		m[i][0] = f[i];
		m[i][1] = -r[i];
		m[i][2] = u[i];
		m[i][3] = 0;
	}
}

// ---------------------------------------------------------------- animation

static qboolean SequenceAnims(const model_t *m, const mstudioseqdesc_t *seq, mstudioanim_t **anim, const unsigned char **end)
{
	const studiohdr_t *h = m->studio.hdr;
	int g = seq->seqgroup;
	if (g < 0 || g >= (h->numseqgroups > 0 ? h->numseqgroups : 1) || !m->studio.seqdata[g])
		return false;
	size_t size = m->studio.seqsize[g];
	int blends = seq->numblends > 0 ? seq->numblends : 1;
	if (seq->animindex < 0 || (size_t)seq->animindex + (size_t)blends * h->numbones * sizeof(mstudioanim_t) > size)
		return false;
	*anim = (mstudioanim_t *)(m->studio.seqdata[g] + seq->animindex);
	*end = m->studio.seqdata[g] + size;
	return true;
}

// reads the values for frame and frame+1 of one channel; false if the data runs out
static qboolean AnimValues(const mstudioanim_t *anim, int channel, int frame, const unsigned char *end, float *v1, float *v2)
{
	const mstudioanimvalue_t *av = (const mstudioanimvalue_t *)((const unsigned char *)anim + anim->offset[channel]);
	const mstudioanimvalue_t *last = (const mstudioanimvalue_t *)end - 1;
	int k = frame;
	while (av <= last && av->num.total <= k) {
		if (!av->num.total)
			return false;
		k -= av->num.total;
		av += av->num.valid + 1;
	}
	if (av > last || av + av->num.valid > last)
		return false;
	// the first value of the next run, for interpolating across runs
	short next = av + av->num.valid + 2 <= last ? av[av->num.valid + 2].value : av[av->num.valid].value;
	if (av->num.valid > k) {
		*v1 = av[k + 1].value;
		if (av->num.valid > k + 1)
			*v2 = av[k + 2].value;
		else
			*v2 = av->num.total > k + 1 ? *v1 : next;
	} else {
		*v1 = av[av->num.valid].value;   // the last value repeats
		*v2 = av->num.total > k + 1 ? *v1 : next;
	}
	return true;
}

static void CalcBoneQuaternion(int frame, float s, const mstudiobone_t *bone, const mstudioanim_t *anim,
	const unsigned char *end, const float *adj, float q[4])
{
	float a1[3], a2[3];
	for (int j = 0; j < 3; j++) {
		float v1 = 0, v2 = 0;
		if (anim && anim->offset[j + 3] && AnimValues(anim, j + 3, frame, end, &v1, &v2)) {
			a1[j] = bone->value[j + 3] + v1 * bone->scale[j + 3];
			a2[j] = bone->value[j + 3] + v2 * bone->scale[j + 3];
		} else {
			a1[j] = a2[j] = bone->value[j + 3];
		}
		if (bone->bonecontroller[j + 3] != -1 && adj) {
			a1[j] += adj[bone->bonecontroller[j + 3]];
			a2[j] += adj[bone->bonecontroller[j + 3]];
		}
	}
	if (a1[0] != a2[0] || a1[1] != a2[1] || a1[2] != a2[2]) {
		float qa[4], qb[4];
		AngleQuaternion(a1, qa);
		AngleQuaternion(a2, qb);
		QuaternionSlerp(qa, qb, s, q);
	} else {
		AngleQuaternion(a1, q);
	}
}

static void CalcBonePosition(int frame, float s, const mstudiobone_t *bone, const mstudioanim_t *anim,
	const unsigned char *end, const float *adj, float pos[3])
{
	for (int j = 0; j < 3; j++) {
		pos[j] = bone->value[j];
		float v1, v2;
		if (anim && anim->offset[j] && AnimValues(anim, j, frame, end, &v1, &v2))
			pos[j] += (v1 * (1.0f - s) + v2 * s) * bone->scale[j];
		if (bone->bonecontroller[j] != -1 && adj)
			pos[j] += adj[bone->bonecontroller[j]];
	}
}

static void CalcBoneAdj(const model_t *m, const rentity_t *e, float *adj)
{
	const studiohdr_t *h = m->studio.hdr;
	const mstudiobonecontroller_t *bc = STUDIO_PTR(h, h->bonecontrollerindex, mstudiobonecontroller_t);
	for (int j = 0; j < h->numbonecontrollers; j++) {
		int i = bc[j].index;
		float value;
		if (i == 4) {
			// mouth: 0..64 opens it fully
			value = e->mouth / 64.0f;
			if (value > 1)
				value = 1;
			value = (1.0f - value) * bc[j].start + value * bc[j].end;
		} else if (i >= 0 && i < 4) {
			if (bc[j].type & STUDIO_RLOOP) {
				value = e->controller[i] * (360.0f / 256.0f) + bc[j].start;
			} else {
				value = e->controller[i] / 255.0f;
				value = (1.0f - value) * bc[j].start + value * bc[j].end;
			}
		} else {
			value = 0;
		}
		switch (bc[j].type & STUDIO_TYPES) {
		case STUDIO_XR: case STUDIO_YR: case STUDIO_ZR:
			adj[j] = DEG2RAD(value);
			break;
		default:
			adj[j] = value;
			break;
		}
	}
}

static void CalcRotations(const model_t *m, const mstudioseqdesc_t *seq, const mstudioanim_t *anim,
	const unsigned char *end, float f, const float *adj, float (*q)[4], vec3_t *pos)
{
	const studiohdr_t *h = m->studio.hdr;
	const mstudiobone_t *bone = STUDIO_PTR(h, h->boneindex, mstudiobone_t);
	int frame = (int)f;
	float s = f - frame;
	for (int i = 0; i < h->numbones; i++) {
		CalcBoneQuaternion(frame, s, &bone[i], anim ? &anim[i] : NULL, end, adj, q[i]);
		CalcBonePosition(frame, s, &bone[i], anim ? &anim[i] : NULL, end, adj, pos[i]);
	}
	// sequences with movement keep the model in place; the entity itself moves
	if (seq && seq->motionbone >= 0 && seq->motionbone < h->numbones) {
		if (seq->motiontype & STUDIO_X) pos[seq->motionbone][0] = 0;
		if (seq->motiontype & STUDIO_Y) pos[seq->motionbone][1] = 0;
		if (seq->motiontype & STUDIO_Z) pos[seq->motionbone][2] = 0;
	}
}

float R_StudioFrame(const rentity_t *e)
{
	const studiohdr_t *h = Mod_StudioHeader(e->model);
	if (!h || !h->numseq)
		return 0;
	int s = e->sequence >= 0 && e->sequence < h->numseq ? e->sequence : 0;
	const mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t) + s;
	if (seq->numframes <= 1)
		return 0;
	float last = (float)(seq->numframes - 1);
	// frame is a 0..255 cycle like GoldSrc's entity frame
	float f = e->frame * last / 256.0f;
	f += (float)((r_refdef.time - e->animtime) * seq->fps * e->framerate);
	if (seq->flags & STUDIO_LOOPING) {
		f -= (int)(f / last) * last;
		if (f < 0)
			f += last;
	} else {
		if (f >= last - 0.001f)
			f = last - 0.001f;
		if (f < 0)
			f = 0;
	}
	return f;
}

int R_StudioSetupBones(const rentity_t *e, float (**bones)[3][4])
{
	const model_t *m = e->model;
	const studiohdr_t *h = m->studio.hdr;
	EnsureBones(h->numbones);
	*bones = bonetransform;
	if (!h->numbones)
		return 0;

	float adjbuf[8], *adj = adjbuf;
	float *adjheap = NULL;
	if (h->numbonecontrollers > 8)
		adj = adjheap = Mem_Alloc(sizeof(float) * h->numbonecontrollers);
	CalcBoneAdj(m, e, adj);

	const mstudioseqdesc_t *seq = NULL;
	mstudioanim_t *anim = NULL;
	const unsigned char *end = NULL;
	if (h->numseq) {
		int s = e->sequence >= 0 && e->sequence < h->numseq ? e->sequence : 0;
		seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t) + s;
		if (!SequenceAnims(m, seq, &anim, &end))
			anim = NULL;   // missing group file: bind pose
	}
	float f = R_StudioFrame(e);
	CalcRotations(m, seq, anim, end, f, adj, q1, pos1);

	if (seq && anim && seq->numblends > 1) {
		CalcRotations(m, seq, anim + h->numbones, end, f, adj, q2, pos2);
		float s = e->blending[0] / 255.0f;
		for (int i = 0; i < h->numbones; i++) {
			float q[4];
			QuaternionSlerp(q1[i], q2[i], s, q);
			memcpy(q1[i], q, sizeof(q));
			for (int j = 0; j < 3; j++)
				pos1[i][j] = pos1[i][j] * (1.0f - s) + pos2[i][j] * s;
		}
	}
	Mem_Free(adjheap);

	// model to world; studio models are drawn with their pitch flipped, as in GoldSrc
	float toworld[3][4];
	vec3_t angles = { -e->angles[0], e->angles[1], e->angles[2] };
	AngleMatrix(angles, toworld);
	for (int i = 0; i < 3; i++)
		toworld[i][3] = e->origin[i];

	const mstudiobone_t *bone = STUDIO_PTR(h, h->boneindex, mstudiobone_t);
	for (int i = 0; i < h->numbones; i++) {
		float local[3][4];
		QuaternionMatrix(q1[i], local);
		for (int j = 0; j < 3; j++)
			local[j][3] = pos1[i][j];
		if (bone[i].parent == -1)
			ConcatTransforms(toworld, local, bonetransform[i]);
		else
			ConcatTransforms(bonetransform[bone[i].parent], local, bonetransform[i]);
	}
	return h->numbones;
}

// ---------------------------------------------------------------- controllers and sequences

void R_StudioInitControllers(const model_t *m, rentity_t *e)
{
	memset(e->controller, 0, sizeof(e->controller));
	const studiohdr_t *h = Mod_StudioHeader(m);
	if (!h)
		return;
	const mstudiobonecontroller_t *bc = STUDIO_PTR(h, h->bonecontrollerindex, mstudiobonecontroller_t);
	for (int j = 0; j < h->numbonecontrollers; j++) {
		int i = bc[j].index;
		if (i < 0 || i > 3)
			continue;
		float start = bc[j].start, end = bc[j].end, v = 0;
		int setting;
		if (bc[j].type & STUDIO_RLOOP) {
			setting = (int)((v - start) * 256.0f / 360.0f) & 255;
		} else {
			if (end == start)
				continue;
			setting = (int)(255.0f * (v - start) / (end - start));
			if (setting < 0) setting = 0;
			if (setting > 255) setting = 255;
		}
		e->controller[i] = (unsigned char)setting;
	}
}

int R_StudioLookupSequence(const model_t *m, const char *label)
{
	const studiohdr_t *h = Mod_StudioHeader(m);
	if (!h)
		return -1;
	const mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t);
	for (int i = 0; i < h->numseq; i++)
		if (!stricmp(seq[i].label, label))
			return i;
	return -1;
}

// ---------------------------------------------------------------- drawing

qboolean R_StudioCull(const rentity_t *e)
{
	const model_t *m = e->model;
	const studiohdr_t *h = m->studio.hdr;
	float r = m->radius;
	if (h->numseq) {
		int s = e->sequence >= 0 && e->sequence < h->numseq ? e->sequence : 0;
		const mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t) + s;
		for (int i = 0; i < 3; i++) {
			float a = fabsf(seq->bbmin[i]), b = fabsf(seq->bbmax[i]);
			if (a > r) r = a;
			if (b > r) r = b;
		}
	}
	if (r < 16)
		r = 16;
	vec3_t mins = { e->origin[0] - r, e->origin[1] - r, e->origin[2] - r };
	vec3_t maxs = { e->origin[0] + r, e->origin[1] + r, e->origin[2] + r };
	return R_CullBox(mins, maxs);
}

static void EnsureArrays(int nverts, int nnorms, int ntris)
{
	if (nverts > verts_cap) {
		verts_cap = nverts;
		xverts = Mem_Realloc(xverts, sizeof(*xverts) * nverts);
	}
	if (nnorms > norms_cap) {
		norms_cap = nnorms;
		xnorms = Mem_Realloc(xnorms, sizeof(*xnorms) * nnorms);
		chromeuv = Mem_Realloc(chromeuv, sizeof(*chromeuv) * nnorms);
	}
	if (ntris * 3 > arr_cap) {
		arr_cap = ntris * 3;
		arr_xyz = Mem_Realloc(arr_xyz, sizeof(float) * 3 * arr_cap);
		arr_st = Mem_Realloc(arr_st, sizeof(float) * 2 * arr_cap);
		arr_rgba = Mem_Realloc(arr_rgba, sizeof(float) * 4 * arr_cap);
	}
}

typedef struct {
	vec3_t color;               // the light at the floor below, as the world shows it (0..1)
	float  ambient, shade;      // shares of it: everywhere, and from above
	float  invgamma;            // the share is light, not brightness: it goes through the light curve
	vec3_t lightvec;            // direction the light travels
	float  alpha;
	qboolean fullbright;
} studiolight_t;

static void SetupLighting(const rentity_t *e, studiolight_t *l)
{
	vec3_t c;
	l->fullbright = r_fullbright_ref->value != 0 || !world.lightdata;
	vec3_t p = { e->origin[0], e->origin[1], e->origin[2] + 1 };
	R_LightPoint(p, c);
	VectorCopy(c, l->color);
	l->ambient = r_studio_ambient->value;
	l->shade = r_studio_shade->value;
	float g = Cvar_VariableValue("r_lightgamma");
	l->invgamma = g > 0.1f ? 1.0f / g : 1.0f;
	// GoldSrc lights models from straight above when there are no dynamic lights
	VectorSet(l->lightvec, 0, 0, -1);
}

static void LightNormal(const studiolight_t *l, const vec3_t n, int texflags, float out[3])
{
	if (l->fullbright || (texflags & STUDIO_NF_FULLBRIGHT)) {
		VectorSet(out, 1, 1, 1);
		return;
	}
	float k;
	if (texflags & STUDIO_NF_FLATSHADE) {
		k = 0.8f;
	} else {
		// wrapped falloff: lambert 1.5 lights the sides and only the underside gets just ambient
		float r = r_studio_lambert->value > 1 ? r_studio_lambert->value : 1;
		float lightcos = DotProduct(n, l->lightvec);
		if (lightcos > 1) lightcos = 1;
		lightcos = (lightcos + (r - 1.0f)) / r;
		k = 1.0f - (lightcos > 0 ? lightcos : 0);
	}
	float f = l->ambient + l->shade * k;
	f = f > 0 ? powf(f, l->invgamma) : 0;
	for (int i = 0; i < 3; i++) {
		float v = l->color[i] * f;
		out[i] = v > 1 ? 1 : v;
	}
}

static void DrawMeshes(const model_t *m, const rentity_t *e, const mstudiomodel_t *sm, studiomesh_t *meshes,
	const studiolight_t *light, qboolean additive_pass)
{
	const studiomodel_t *st = &m->studio;
	const studiohdr_t *th = st->texhdr;
	const short *skins = STUDIO_PTR(th, th->skinindex, short);
	int skin = (e->skin >= 0 && e->skin < th->numskinfamilies) ? e->skin : 0;

	for (int k = 0; k < sm->nummesh; k++) {
		studiomesh_t *mesh = &meshes[k];
		if (!mesh->numtris)
			continue;
		int ref = mesh->skinref;
		if (ref >= 0 && ref < th->numskinref && th->numskinfamilies > 0)
			ref = skins[skin * th->numskinref + ref];
		if (ref < 0 || ref >= th->numtextures)
			continue;
		const studiotex_t *tex = &st->textures[ref];
		qboolean additive = (tex->flags & STUDIO_NF_ADDITIVE) != 0;
		if (additive != additive_pass)
			continue;

		float iw = 1.0f / tex->width, ih = 1.0f / tex->height;
		int n = 0;
		for (int t = 0; t < mesh->numtris * 3; t++) {
			const short *v = mesh->tris + t * 4;
			VectorCopy(xverts[v[0]], arr_xyz + n * 3);
			if (tex->flags & STUDIO_NF_CHROME) {
				arr_st[n * 2 + 0] = chromeuv[v[1]][0];
				arr_st[n * 2 + 1] = chromeuv[v[1]][1];
			} else {
				arr_st[n * 2 + 0] = v[2] * iw;
				arr_st[n * 2 + 1] = v[3] * ih;
			}
			float *c = arr_rgba + n * 4;
			LightNormal(light, xnorms[v[1]], tex->flags, c);
			c[3] = light->alpha;
			n++;
		}

		glBindTexture(GL_TEXTURE_2D, tex->gl->id);
		if (tex->flags & STUDIO_NF_MASKED) {
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, 0.5f);
		}
		if (additive) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glDepthMask(GL_FALSE);
		}
		glDrawArrays(GL_TRIANGLES, 0, n);
		if (tex->flags & STUDIO_NF_MASKED)
			glDisable(GL_ALPHA_TEST);
		if (additive) {
			glDepthMask(GL_TRUE);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			if (light->alpha >= 1)
				glDisable(GL_BLEND);
		}
	}
}

void R_StudioDrawModel(rentity_t *e)
{
	model_t *m = e->model;
	const studiohdr_t *h = m->studio.hdr;
	float (*bones)[3][4];
	R_StudioSetupBones(e, &bones);

	studiolight_t light;
	SetupLighting(e, &light);
	light.alpha = 1;

	// render modes
	switch (e->rendermode) {
	case kRenderTransColor:
	case kRenderTransTexture:
		light.alpha = e->renderamt / 255.0f;
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		light.alpha = e->renderamt / 255.0f;
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDepthMask(GL_FALSE);
		break;
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnable(GL_TEXTURE_2D);

	const mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
	int sub = 0;
	for (int i = 0; i < h->numbodyparts; i++) {
		int choice = 0;
		if (bp[i].nummodels > 0) {
			int base = bp[i].base > 0 ? bp[i].base : 1;
			int body = e->body >= 0 ? e->body : 0;
			choice = (body / base) % bp[i].nummodels;
		}
		const mstudiomodel_t *sm = STUDIO_PTR(h, bp[i].modelindex, mstudiomodel_t) + choice;
		studiomesh_t *meshes = m->studio.meshes[sub + choice];
		sub += bp[i].nummodels;
		if (!sm->numverts)
			continue;

		int ntris = 0;
		for (int k = 0; k < sm->nummesh; k++)
			if (meshes[k].numtris > ntris)
				ntris = meshes[k].numtris;
		EnsureArrays(sm->numverts, sm->numnorms, ntris);

		const unsigned char *vbone = STUDIO_PTR(h, sm->vertinfoindex, unsigned char);
		const vec3_t *verts = STUDIO_PTR(h, sm->vertindex, vec3_t);
		const unsigned char *nbone = STUDIO_PTR(h, sm->norminfoindex, unsigned char);
		const vec3_t *norms = STUDIO_PTR(h, sm->normindex, vec3_t);
		for (int v = 0; v < sm->numverts; v++) {
			float (*b)[4] = bones[vbone[v]];
			for (int j = 0; j < 3; j++)
				xverts[v][j] = DotProduct(verts[v], b[j]) + b[j][3];
		}
		for (int v = 0; v < sm->numnorms; v++) {
			float (*b)[4] = bones[nbone[v]];
			for (int j = 0; j < 3; j++)
				xnorms[v][j] = DotProduct(norms[v], b[j]);
			// chrome follows the view: texture coordinates from the normal against the camera axes
			chromeuv[v][0] = (DotProduct(xnorms[v], r_refdef.right) + 1.0f) * 0.5f;
			chromeuv[v][1] = (1.0f - DotProduct(xnorms[v], r_refdef.up)) * 0.5f;
		}

		glVertexPointer(3, GL_FLOAT, 0, arr_xyz);
		glTexCoordPointer(2, GL_FLOAT, 0, arr_st);
		glColorPointer(4, GL_FLOAT, 0, arr_rgba);
		DrawMeshes(m, e, sm, meshes, &light, false);
		DrawMeshes(m, e, sm, meshes, &light, true);
	}

	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, 1);
}

void R_StudioInit(void)
{
	r_studio_lambert = Cvar_Get("r_studio_lambert", "1.5", 0, "how far model lighting wraps around the sides (GoldSrc: 1.5)");
	// GoldSrc's split: a tenth of the light reaches every side, the rest comes from above
	r_studio_ambient = Cvar_Get("r_studio_ambient", "0.1", 0, "share of the floor light that lights every side of a model");
	r_studio_shade = Cvar_Get("r_studio_shade", "0.9", 0, "share of the floor light that comes from above");
	r_fullbright_ref = Cvar_Get("r_fullbright", "0", 0, "1 = no lightmaps");
}
