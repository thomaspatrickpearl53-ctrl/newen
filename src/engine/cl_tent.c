// newen - temp entities: short-lived models and sprites the client spawns (shells, gibs, explosions,
// smoke, muzzle flashes), the efx API the client DLL calls, the game's svc_temp_entity messages and
// the client events (5000+) in model animations
//
// Temp entities live in blocks that never move, since the client DLL keeps pointers into them, and
// sit on two linked lists, free and active. Each frame the client's HUD_TempEntUpdate moves them
// (gravity, bounces, fading, sprite frames), puts dead ones back on the free list and hands the
// visible ones back to the engine, which turns them into frame entities for the renderer.
#include "cl_local.h"

#define TENT_BLOCK 256

static TEMPENTITY *tent_free, *tent_active;
static TEMPENTITY **tent_blocks;
static int tent_nblocks;
static cvar_t *tempents_max;
static vec3_t zero_vec;

model_t *CL_ModelForIndex(int index)
{
	return SV_ModelForIndex(index);
}

static model_t *SpriteByName(const char *name)
{
	model_t *m = Mod_ForName(name, false);
	return m && m->type != mod_bad ? m : NULL;
}

// sprite frames, or the number of body combinations of a studio model (gibs pick one at random)
static int ModelFrameCount(const model_t *m)
{
	if (!m)
		return 1;
	if (m->type == mod_sprite)
		return m->sprite.numframes > 0 ? m->sprite.numframes : 1;
	const studiohdr_t *h = Mod_StudioHeader(m);
	if (!h)
		return 1;
	int n = 1;
	const mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
	for (int i = 0; i < h->numbodyparts; i++)
		if (bp[i].nummodels > 0)
			n *= bp[i].nummodels;
	return n;
}

// ---------------------------------------------------------------- allocation

static void GrowPool(void)
{
	TEMPENTITY *block = Mem_Alloc(sizeof(TEMPENTITY) * TENT_BLOCK);
	for (int i = 0; i < TENT_BLOCK; i++) {
		block[i].next = tent_free;
		tent_free = &block[i];
	}
	tent_blocks = Mem_Realloc(tent_blocks, sizeof(*tent_blocks) * (tent_nblocks + 1));
	tent_blocks[tent_nblocks++] = block;
}

static TEMPENTITY *TentAlloc(const float *org, model_t *model, qboolean high)
{
	if (!tent_free) {
		if (tent_nblocks * TENT_BLOCK < tempents_max->value || !tent_nblocks) {
			GrowPool();
		} else if (high) {
			// full: a high priority one (muzzle flash) takes the oldest low priority one's place
			TEMPENTITY *prev = NULL, *found = NULL, *foundprev = NULL;
			for (TEMPENTITY *t = tent_active; t; prev = t, t = t->next)
				if (t->priority == TENTPRIORITY_LOW) {
					found = t;
					foundprev = prev;
				}
			if (!found)
				return NULL;
			if (foundprev)
				foundprev->next = found->next;
			else
				tent_active = found->next;
			found->next = tent_free;
			tent_free = found;
		} else {
			return NULL;
		}
	}
	TEMPENTITY *t = tent_free;
	tent_free = t->next;
	memset(t, 0, sizeof(*t));
	t->priority = high ? TENTPRIORITY_HIGH : TENTPRIORITY_LOW;
	t->die = (float)cl_time + 0.75f;
	t->fadeSpeed = 0.5f;
	t->bounceFactor = 1;
	t->clientIndex = -1;
	t->frameMax = (float)(ModelFrameCount(model) - 1);
	cl_entity_t *e = &t->entity;
	e->model = model;
	e->curstate.modelindex = model && sv.active ? SV_ModelIndex(model->name) : 0;
	e->curstate.rendermode = kRenderNormal;
	e->curstate.renderamt = e->baseline.renderamt = 255;
	e->curstate.rendercolor.r = e->curstate.rendercolor.g = e->curstate.rendercolor.b = 255;
	e->curstate.scale = 1;
	if (org) {
		VectorCopy(org, e->origin);
		VectorCopy(org, e->curstate.origin);
		VectorCopy(org, e->prevstate.origin);
	}
	t->next = tent_active;
	tent_active = t;
	return t;
}

TEMPENTITY *CL_TempEntAlloc(const float *org, model_t *model) { return model ? TentAlloc(org, model, false) : NULL; }
TEMPENTITY *CL_TempEntAllocHigh(const float *org, model_t *model) { return model ? TentAlloc(org, model, true) : NULL; }
static TEMPENTITY *TempEntAlloc(float *org, model_t *model) { return CL_TempEntAlloc(org, model); }
static TEMPENTITY *TempEntAllocHigh(float *org, model_t *model) { return CL_TempEntAllocHigh(org, model); }

static TEMPENTITY *TempEntAllocNoModel(float *org)
{
	TEMPENTITY *t = TentAlloc(org, NULL, false);
	if (t)
		t->flags |= FTENT_NOMODEL;
	return t;
}

static TEMPENTITY *TempEntAllocCustom(float *org, model_t *model, int high,
	void (*callback)(TEMPENTITY *ent, float frametime, float currenttime))
{
	TEMPENTITY *t = TentAlloc(org, model, high != 0);
	if (t && callback) {
		t->callback = callback;
		t->flags |= FTENT_CLIENTCUSTOM;
	}
	return t;
}

void CL_ClearTempEnts(void)
{
	// everything back to the free list; the blocks stay
	tent_free = NULL;
	tent_active = NULL;
	for (int b = 0; b < tent_nblocks; b++)
		for (int i = 0; i < TENT_BLOCK; i++) {
			tent_blocks[b][i].next = tent_free;
			tent_free = &tent_blocks[b][i];
		}
}

// ---------------------------------------------------------------- per frame

static int AddVisibleTempEnt(cl_entity_t *e)
{
	if (!e || !e->model)
		return 0;
	return CL_AddVisibleEntity(e);
}

static void TempEntPlaySound(void *t, float damp) {}   // sound arrives in step 6

static void UpdateTempEnts(void)
{
	if (!tent_active)
		return;
	double ft = cl_time - cl_oldtime;
	if (cl.HUD_TempEntUpdate) {
		CLIENT_CALL("HUD_TempEntUpdate", cl.HUD_TempEntUpdate(ft, cl_time, sv_gravity ? sv_gravity->value : 800,
			(void **)&tent_free, (void **)&tent_active, AddVisibleTempEnt, TempEntPlaySound));
		return;
	}
	// a client without the export: no physics, just lifetimes
	TEMPENTITY *prev = NULL;
	for (TEMPENTITY *t = tent_active; t;) {
		TEMPENTITY *next = t->next;
		if (t->die < cl_time && !(t->flags & FTENT_PERSIST)) {
			if (prev)
				prev->next = next;
			else
				tent_active = next;
			t->next = tent_free;
			tent_free = t;
		} else {
			if (!(t->flags & FTENT_NOMODEL))
				AddVisibleTempEnt(&t->entity);
			prev = t;
		}
		t = next;
	}
}

static void ComputeAttachments(cl_entity_t *ent, const rentity_t *r);

void CL_AddEntities(void)
{
	if (!client_ok || !sv.active)
		return;
	// the view model's events first, so a muzzle flash shows in the frame that fired it
	rentity_t vr;
	if (CL_BuildViewModel(&vr)) {
		// the gun's attachments (barrel, shell port) every frame: beams and effects start there
		ComputeAttachments(&cl_viewent, &vr);
		CL_StudioEvents(&cl_viewent, &vr);
	}
	UpdateTempEnts();
}

void CL_DrawEffects(void)
{
	R_DrawBeams();
	R_DrawParticles();
	if (client_ok && sv.active) {
		CLIENT_CALL("HUD_DrawNormalTriangles", if (cl.HUD_DrawNormalTriangles) cl.HUD_DrawNormalTriangles());
		CLIENT_CALL("HUD_DrawTransparentTriangles", if (cl.HUD_DrawTransparentTriangles) cl.HUD_DrawTransparentTriangles());
	}
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

// ---------------------------------------------------------------- client events in model animations

static void ComputeAttachments(cl_entity_t *ent, const rentity_t *r)
{
	const studiohdr_t *h = Mod_StudioHeader(r->model);
	float (*bones)[3][4];
	int nb = R_StudioSetupBones(r, &bones);
	const mstudioattachment_t *a = STUDIO_PTR(h, h->attachmentindex, mstudioattachment_t);
	for (int i = 0; i < 4; i++) {
		if (i >= h->numattachments || a[i].bone < 0 || a[i].bone >= nb) {
			VectorCopy(r->origin, ent->attachment[i]);
			continue;
		}
		for (int k = 0; k < 3; k++)
			ent->attachment[i][k] = DotProduct(a[i].org, bones[a[i].bone][k]) + bones[a[i].bone][k][3];
	}
}

// the frame r would show at time t (the renderer's clock)
static float FrameAt(const rentity_t *r, double t)
{
	rentity_t c = *r;
	c.animtime += r_refdef.time - t;
	return R_StudioFrame(&c);
}

// Fires the client events (muzzle flashes, sparks) whose frame the animation passed since the last
// frame. The last frame looked at is kept in the entity's latched fields.
void CL_StudioEvents(cl_entity_t *ent, const rentity_t *r)
{
	const studiohdr_t *h = Mod_StudioHeader(r->model);
	if (!client_ok || !cl.HUD_StudioEvent || !h || !h->numseq || !ent)
		return;
	int s = r->sequence >= 0 && r->sequence < h->numseq ? r->sequence : 0;
	const mstudioseqdesc_t *seq = STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t) + s;
	float f2 = R_StudioFrame(r);
	qboolean continuing = ent->latched.prevsequence == s && ent->latched.prevanimtime == (float)(r_refdef.framecount - 1);
	float f1 = continuing ? ent->latched.prevframe : FrameAt(r, r_refdef.time - host_frametime);
	ent->latched.prevsequence = s;
	ent->latched.prevframe = f2;
	ent->latched.prevanimtime = (float)r_refdef.framecount;
	if (!seq->numevents)
		return;

	qboolean looping = (seq->flags & STUDIO_LOOPING) != 0;
	qboolean wrapped = false;
	if (f2 < f1) {
		if (looping)
			wrapped = true;
		else
			f1 = -0.01f;   // a one-shot sequence started over (the same weapon animation again)
	} else if (!continuing && r->frame == 0 && r->animtime > r_refdef.time - host_frametime) {
		f1 = -0.01f;       // started this frame: the first frame's events count too
	}
	if (f1 == f2 && !wrapped)
		return;

	const mstudioevent_t *ev = STUDIO_PTR(h, seq->eventindex, mstudioevent_t);
	qboolean attached = false;
	for (int i = 0; i < seq->numevents; i++) {
		if (ev[i].event < 5000)
			continue;   // the game's own events (server side)
		float fr = (float)ev[i].frame;
		qboolean fire = wrapped ? (fr > f1 || fr <= f2) : (fr > f1 && fr <= f2);
		if (!fire)
			continue;
		if (!attached) {
			ComputeAttachments(ent, r);
			attached = true;
		}
		CLIENT_CALL("HUD_StudioEvent", cl.HUD_StudioEvent(&ev[i], ent));
	}
}

void CL_EntityStudioEvents(int index, const rentity_t *r)
{
	cl_entity_t *e = CL_EntityByIndex(index);
	if (e)
		CL_StudioEvents(e, r);
}

// "entity:attachment" packing used by beams and messages: low 12 bits entity, next 4 attachment
void CL_GetEntityPoint(int entnum, vec3_t out)
{
	int index = entnum & 0x0fff, att = (entnum >> 12) & 0xf;
	edict_t *pl = SV_LocalPlayer();
	// the local player's attachments are the view model's (its gun barrel), in first person
	if (att && pl && index == NUM_FOR_EDICT(pl)) {
		VectorCopy(cl_viewent.attachment[att - 1], out);
		return;
	}
	cl_entity_t *e = CL_EntityByIndex(index);
	if (!e || !sv.active || index >= sv.num_edicts) {
		VectorClear(out);
		return;
	}
	if (att && e->model && e->model->type == mod_studio) {
		edict_t *ed = EDICT_NUM(index);
		rentity_t r;
		memset(&r, 0, sizeof(r));
		r.model = e->model;
		VectorCopy(ed->v.origin, r.origin);
		VectorCopy(ed->v.angles, r.angles);
		r.sequence = ed->v.sequence;
		r.frame = ed->v.frame;
		r.framerate = ed->v.framerate;
		r.animtime = r_refdef.time - (sv.time - ed->v.animtime);
		memcpy(r.controller, ed->v.controller, 4);
		memcpy(r.blending, ed->v.blending, 2);
		ComputeAttachments(e, &r);
		VectorCopy(e->attachment[att - 1], out);
		return;
	}
	edict_t *ed = EDICT_NUM(index);
	if (e->model && e->model->type == mod_brush) {
		// brush entities: their centre
		for (int i = 0; i < 3; i++)
			out[i] = ed->v.origin[i] + (ed->v.mins[i] + ed->v.maxs[i]) * 0.5f;
		return;
	}
	VectorCopy(ed->v.origin, out);
}

// ---------------------------------------------------------------- the efx API: models and sprites

static TEMPENTITY *R_TempModel(float *pos, float *dir, float *angles, float life, int modelIndex, int soundtype)
{
	TEMPENTITY *t = CL_TempEntAlloc(pos, CL_ModelForIndex(modelIndex));
	if (!t)
		return NULL;
	t->flags = FTENT_COLLIDEWORLD | FTENT_GRAVITY | FTENT_ROTATE;
	t->hitSound = soundtype;
	VectorCopy(dir, t->entity.baseline.origin);   // velocity
	VectorCopy(angles, t->entity.angles);
	for (int i = 0; i < 3; i++)
		t->entity.baseline.angles[i] = CL_Random(-255, 255);   // spin
	t->die = (float)cl_time + life;
	return t;
}

static TEMPENTITY *R_DefaultSprite(float *pos, int spriteIndex, float framerate)
{
	model_t *m = CL_ModelForIndex(spriteIndex);
	if (!m || m->type != mod_sprite)
		return NULL;
	TEMPENTITY *t = CL_TempEntAlloc(pos, m);
	if (!t)
		return NULL;
	if (framerate <= 0)
		framerate = 10;
	t->flags |= FTENT_SPRANIMATE;
	t->entity.curstate.framerate = framerate;
	t->die = (float)cl_time + (t->frameMax + 1) / framerate;
	return t;
}

static TEMPENTITY *R_TempSprite(float *pos, float *dir, float scale, int modelIndex, int rendermode, int renderfx,
	float a, float life, int flags)
{
	TEMPENTITY *t = CL_TempEntAlloc(pos, CL_ModelForIndex(modelIndex));
	if (!t)
		return NULL;
	t->flags = flags;
	t->entity.curstate.framerate = 10;
	t->entity.curstate.rendermode = rendermode;
	t->entity.curstate.renderfx = renderfx;
	t->entity.curstate.scale = scale;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = (int)(a * 255);
	if (dir)
		VectorCopy(dir, t->entity.baseline.origin);
	t->die = (float)cl_time + life;
	return t;
}

static void R_Sprite_Explode(TEMPENTITY *t, float scale, int flags)
{
	if (!t)
		return;
	if (flags & TE_EXPLFLAG_NOADDITIVE) {
		t->entity.curstate.rendermode = kRenderNormal;
	} else {
		t->entity.curstate.rendermode = kRenderTransAdd;
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 180;
	}
	t->entity.curstate.scale = scale;
	t->entity.origin[2] += 10;
	t->entity.baseline.origin[2] = 8;   // rises a little
}

static void R_Sprite_Smoke(TEMPENTITY *t, float scale)
{
	if (!t)
		return;
	int c = CL_RandomInt(20, 35);
	t->entity.curstate.rendermode = kRenderTransAlpha;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
	t->entity.curstate.rendercolor.r = t->entity.curstate.rendercolor.g = t->entity.curstate.rendercolor.b = (unsigned char)c;
	t->entity.origin[2] += 20;
	t->entity.baseline.origin[2] = 30;
	t->entity.curstate.scale = scale;
}

static void R_Sprite_WallPuff(TEMPENTITY *t, float scale)
{
	if (!t)
		return;
	t->entity.curstate.rendermode = kRenderTransAlpha;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
	t->entity.curstate.scale = scale;
	t->entity.angles[2] = (float)CL_RandomInt(0, 359);
	t->die = (float)cl_time + 0.01f;
}

static void R_Explosion(float *pos, int model, float scale, float framerate, int flags)
{
	TEMPENTITY *t = R_DefaultSprite(pos, model, framerate);
	R_Sprite_Explode(t, scale, flags);
	if (!(flags & TE_EXPLFLAG_NOPARTICLES))
		R_FlickerParticles(pos);
}

static void R_MuzzleFlash(float *pos, int type)
{
	int index = type % 10;
	float scale = (type / 10) * 0.1f;
	if (scale == 0)
		scale = 0.5f;
	if (index < 0 || index > 2)
		index = 0;
	TEMPENTITY *t = CL_TempEntAllocHigh(pos, SpriteByName(va("sprites/muzzleflash%d.spr", index + 1)));
	if (!t)
		return;
	t->entity.curstate.rendermode = kRenderTransAdd;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
	t->entity.curstate.scale = scale;
	t->entity.curstate.frame = (float)CL_RandomInt(0, (int)t->frameMax);
	t->entity.angles[2] = (float)CL_RandomInt(0, 359);
	t->die = (float)cl_time + 0.01f;   // one frame
}

static void R_RicochetSprite(float *pos, model_t *m, float duration, float scale)
{
	TEMPENTITY *t = CL_TempEntAlloc(pos, m);
	if (!t)
		return;
	t->entity.curstate.rendermode = kRenderGlow;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 200;
	t->entity.curstate.scale = scale;
	t->flags = FTENT_FADEOUT;
	t->die = (float)cl_time + duration;
	t->entity.curstate.frame = 0;
	t->entity.angles[2] = (float)(45 * CL_RandomInt(0, 7));
}

static void R_SparkEffect(float *pos, int count, int vmin, int vmax)
{
	R_RicochetSprite(pos, SpriteByName("sprites/richo1.spr"), 0.1f, CL_Random(0.5f, 1.0f));
	R_SparkStreaks(pos, count, vmin, vmax);
}

static void R_SparkShower(float *pos)
{
	TEMPENTITY *t = TempEntAllocNoModel(pos);
	if (!t)
		return;
	t->flags |= FTENT_SPARKSHOWER | FTENT_COLLIDEWORLD | FTENT_SLOWGRAVITY;
	t->entity.baseline.origin[0] = CL_Random(-300, 300);
	t->entity.baseline.origin[1] = CL_Random(-300, 300);
	t->entity.baseline.origin[2] = CL_Random(-200, 200);
	t->entity.baseline.framerate = CL_Random(0.5f, 1.5f);   // the client uses these as spark rate and scale
	t->entity.baseline.scale = (float)cl_time;
	t->die = (float)cl_time + 0.5f;
}

static void R_Spray(float *pos, float *dir, int modelIndex, int count, int speed, int spread, int rendermode)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = 0; i < count; i++) {
		TEMPENTITY *t = CL_TempEntAlloc(pos, m);
		if (!t)
			return;
		t->entity.curstate.rendermode = rendermode;
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
		t->entity.curstate.renderfx = 0;
		t->entity.curstate.scale = 0.5f;
		t->flags |= FTENT_FADEOUT | FTENT_SLOWGRAVITY | FTENT_COLLIDEWORLD;
		t->fadeSpeed = 2;
		float noise = spread / 100.0f;
		for (int k = 0; k < 3; k++)
			t->entity.baseline.origin[k] = (dir[k] + CL_Random(-noise, noise)) * CL_Random(speed * 0.8f, speed * 1.2f);
		t->entity.curstate.frame = (float)CL_RandomInt(0, (int)t->frameMax);
		t->die = (float)cl_time + 0.35f;
	}
}

static void R_Sprite_Spray(float *pos, float *dir, int modelIndex, int count, int speed, int rnd)
{
	R_Spray(pos, dir, modelIndex, count, speed, rnd, kRenderTransAlpha);
}

static void R_Sprite_Trail(int type, float *start, float *end, int modelIndex, int count, float life, float size,
	float amplitude, int renderamt, float speed)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	vec3_t delta, dir;
	VectorSubtract(end, start, delta);
	VectorCopy(delta, dir);
	VectorNormalize(dir);
	for (int i = 0; i < count; i++) {
		vec3_t p;
		VectorMA(start, (float)i / (count > 1 ? count - 1 : 1), delta, p);
		TEMPENTITY *t = CL_TempEntAlloc(p, m);
		if (!t)
			return;
		t->flags = FTENT_COLLIDEWORLD | FTENT_SPRCYCLE | FTENT_FADEOUT | FTENT_SLOWGRAVITY;
		for (int k = 0; k < 3; k++)
			t->entity.baseline.origin[k] = dir[k] * speed + CL_Random(-amplitude, amplitude);
		t->entity.curstate.rendermode = kRenderGlow;
		t->entity.curstate.renderfx = 14;   // kRenderFxNoDissipation
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = renderamt;
		t->entity.curstate.scale = size;
		t->entity.curstate.framerate = 10;
		t->die = (float)cl_time + life + CL_Random(0, 4);
	}
}

static void R_BloodSprite(float *org, int colorindex, int modelIndex, int modelIndex2, float size)
{
	colorindex &= 255;
	unsigned char *c = &r_palette[colorindex * 3];
	// the spray
	TEMPENTITY *t = CL_TempEntAlloc(org, CL_ModelForIndex(modelIndex));
	if (t) {
		t->entity.curstate.rendermode = kRenderTransTexture;
		t->entity.curstate.renderfx = 0;
		t->entity.curstate.scale = CL_Random(size / 25.0f, size / 35.0f);
		t->entity.curstate.rendercolor.r = c[0];
		t->entity.curstate.rendercolor.g = c[1];
		t->entity.curstate.rendercolor.b = c[2];
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 250;
		t->entity.curstate.framerate = (t->frameMax + 1) * 4;
		t->flags = FTENT_SPRANIMATE;
		t->die = (float)cl_time + (t->frameMax + 1) / t->entity.curstate.framerate;
	}
	// drops
	model_t *drop = CL_ModelForIndex(modelIndex2);
	int n = CL_RandomInt(1, 8);
	for (int i = 0; drop && i < n; i++) {
		TEMPENTITY *d = CL_TempEntAlloc(org, drop);
		if (!d)
			break;
		d->flags = FTENT_COLLIDEWORLD | FTENT_SLOWGRAVITY;
		d->entity.curstate.rendermode = kRenderTransTexture;
		d->entity.curstate.rendercolor.r = c[0];
		d->entity.curstate.rendercolor.g = c[1];
		d->entity.curstate.rendercolor.b = c[2];
		d->entity.curstate.renderamt = d->entity.baseline.renderamt = 250;
		d->entity.curstate.scale = CL_Random(size / 15.0f, size / 25.0f);
		d->entity.baseline.origin[0] = CL_Random(-96, 95);
		d->entity.baseline.origin[1] = CL_Random(-96, 95);
		d->entity.baseline.origin[2] = CL_Random(-32, 95);
		d->entity.curstate.frame = (float)CL_RandomInt(0, (int)d->frameMax);
		d->die = (float)cl_time + CL_Random(1, 3);
	}
}

static void R_BreakModel(float *pos, float *size, float *dir, float random, float life, int count, int modelIndex, char flags)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	if (!m)
		return;
	int frames = ModelFrameCount(m);
	if (count <= 0)
		count = (int)((size[0] * size[1] + size[1] * size[2] + size[2] * size[0]) / (3 * 64 * 64));
	if (count < 1)
		count = 1;
	if (count > 100)
		count = 100;
	for (int i = 0; i < count; i++) {
		vec3_t p;
		for (int k = 0; k < 3; k++)
			p[k] = pos[k] + CL_Random(-0.5f, 0.5f) * size[k];
		TEMPENTITY *t = CL_TempEntAlloc(p, m);
		if (!t)
			return;
		t->hitSound = flags & 0x0f;
		if (m->type == mod_sprite)
			t->entity.curstate.frame = (float)CL_RandomInt(0, frames - 1);
		else
			t->entity.curstate.body = CL_RandomInt(0, frames - 1);
		t->flags |= FTENT_COLLIDEWORLD | FTENT_FADEOUT | FTENT_SLOWGRAVITY;
		if (CL_RandomInt(0, 255) < 200) {
			t->flags |= FTENT_ROTATE;
			for (int k = 0; k < 3; k++)
				t->entity.baseline.angles[k] = CL_Random(-256, 255);
		}
		if (CL_RandomInt(0, 255) < 100 && (flags & 0x04))   // BREAK_SMOKE
			t->flags |= FTENT_SMOKETRAIL;
		if (flags & BREAK_TRANS) {
			t->entity.curstate.rendermode = kRenderTransTexture;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = 128;
		}
		for (int k = 0; k < 3; k++)
			t->entity.baseline.origin[k] = dir[k] + CL_Random(-random, random);
		t->die = (float)cl_time + life + CL_Random(0, 1);
	}
}

static void R_TempSphereModel(float *pos, float speed, float life, int count, int modelIndex)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = 0; m && i < count; i++) {
		TEMPENTITY *t = CL_TempEntAlloc(pos, m);
		if (!t)
			return;
		t->entity.curstate.body = CL_RandomInt(0, ModelFrameCount(m) - 1);
		t->flags |= FTENT_COLLIDEWORLD | FTENT_SMOKETRAIL | FTENT_FLICKER | FTENT_GRAVITY | FTENT_ROTATE;
		for (int k = 0; k < 3; k++) {
			t->entity.baseline.angles[k] = CL_Random(-255, 255);
			t->entity.baseline.origin[k] = CL_Random(-1, 1) * speed;
		}
		t->die = (float)cl_time + life;
	}
}

static void R_Bubbles(float *mins, float *maxs, float height, int modelIndex, int count, float speed)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = 0; m && i < count; i++) {
		vec3_t p;
		for (int k = 0; k < 3; k++)
			p[k] = CL_Random(mins[k], maxs[k]);
		TEMPENTITY *t = CL_TempEntAlloc(p, m);
		if (!t)
			return;
		t->flags |= FTENT_SINEWAVE;
		t->x = p[0];
		t->y = p[1];
		float angle = CL_Random(0, M_PI_F * 2);
		t->entity.baseline.origin[0] = speed * cosf(angle);
		t->entity.baseline.origin[1] = speed * sinf(angle);
		t->entity.baseline.origin[2] = CL_Random(80, 100);
		t->entity.curstate.frame = (float)CL_RandomInt(0, (int)t->frameMax);
		t->entity.curstate.rendermode = kRenderTransAlpha;
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
		t->entity.curstate.scale = 0.25f;
		t->die = (float)cl_time + (height - (p[2] - mins[2])) / t->entity.baseline.origin[2] - 0.1f;
	}
}

static void R_BubbleTrail(float *start, float *end, float height, int modelIndex, int count, float speed)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = 0; m && i < count; i++) {
		float d = CL_Random(0, 1);
		vec3_t p;
		for (int k = 0; k < 3; k++)
			p[k] = start[k] + d * (end[k] - start[k]);
		TEMPENTITY *t = CL_TempEntAlloc(p, m);
		if (!t)
			return;
		t->flags |= FTENT_SINEWAVE;
		t->x = p[0];
		t->y = p[1];
		float angle = CL_Random(0, M_PI_F * 2);
		t->entity.baseline.origin[0] = speed * cosf(angle);
		t->entity.baseline.origin[1] = speed * sinf(angle);
		t->entity.baseline.origin[2] = CL_Random(80, 100);
		t->entity.curstate.frame = (float)CL_RandomInt(0, (int)t->frameMax);
		t->entity.curstate.rendermode = kRenderTransAlpha;
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
		t->entity.curstate.scale = 0.25f;
		t->die = (float)cl_time + (height - (p[2] - start[2])) / t->entity.baseline.origin[2] - 0.1f;
	}
}

static void R_FizzEffect(cl_entity_t *pent, int modelIndex, int density)
{
	if (!pent || !pent->model || pent->index <= 0 || pent->index >= sv.num_edicts)
		return;
	edict_t *e = EDICT_NUM(pent->index);
	vec3_t mins, maxs;
	VectorAdd(e->v.origin, e->v.mins, mins);
	VectorAdd(e->v.origin, e->v.maxs, maxs);
	int count = density + 1;
	float height = maxs[2] - mins[2];
	vec3_t top = { maxs[0], maxs[1], mins[2] };
	R_Bubbles(mins, top, height, modelIndex, count, 6);
}

static void R_FireField(float *org, int radius, int modelIndex, int count, int flags, float life)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = 0; m && i < count; i++) {
		vec3_t p;
		VectorCopy(org, p);
		p[0] += CL_Random(-(float)radius, (float)radius);
		p[1] += CL_Random(-(float)radius, (float)radius);
		if (!(flags & 1))   // TEFIRE_FLAG_PLANAR
			p[2] += CL_Random(-(float)radius, (float)radius);
		TEMPENTITY *t = CL_TempEntAlloc(p, m);
		if (!t)
			return;
		t->entity.curstate.framerate = 10;
		t->flags |= FTENT_SPRANIMATE | FTENT_SPRANIMATELOOP;
		if (flags & 2) {   // TEFIRE_FLAG_ALLFLOAT
			t->entity.baseline.origin[2] = 30;
		}
		if (flags & 8) {   // TEFIRE_FLAG_ALPHA
			t->entity.curstate.rendermode = kRenderTransAlpha;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = 128;
		} else if (flags & 16) {   // TEFIRE_FLAG_ADDITIVE
			t->entity.curstate.rendermode = kRenderTransAdd;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = 80;
		}
		t->die = (float)cl_time + life;
	}
}

static void R_FunnelSprite(float *org, int modelIndex, int reverse)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	for (int i = -256; m && i <= 256; i += 32)
		for (int j = -256; j <= 256; j += 32) {
			vec3_t p, d;
			if (reverse) {
				VectorCopy(org, p);
				d[0] = (float)i; d[1] = (float)j; d[2] = CL_Random(100, 800);
				VectorAdd(org, d, d);
			} else {
				p[0] = org[0] + i; p[1] = org[1] + j; p[2] = org[2] + CL_Random(100, 800);
				VectorCopy(org, d);
			}
			TEMPENTITY *t = CL_TempEntAlloc(p, m);
			if (!t)
				return;
			t->flags = FTENT_ROTATE | FTENT_FADEOUT;
			vec3_t dir;
			VectorSubtract(d, p, dir);
			float dist = VectorNormalize(dir);
			float vel = d[2] / 8;
			if (vel < 64)
				vel = 64;
			vel += CL_Random(64, 128);
			VectorScale(dir, vel, t->entity.baseline.origin);
			t->entity.curstate.rendermode = kRenderTransAdd;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = 200;
			t->entity.curstate.scale = 0.1f;
			t->die = (float)cl_time + dist / vel;
		}
}

static void R_Projectile(float *origin, float *velocity, int modelIndex, int life, int owner,
	void (*hitcallback)(TEMPENTITY *ent, struct pmtrace_s *ptr))
{
	TEMPENTITY *t = CL_TempEntAllocHigh(origin, CL_ModelForIndex(modelIndex));
	if (!t)
		return;
	VectorCopy(velocity, t->entity.baseline.origin);
	vec3_t dir;
	VectorCopy(velocity, dir);
	VectorNormalize(dir);
	t->entity.angles[1] = SV_VecToYaw(dir);
	t->entity.angles[0] = -atan2f(dir[2], sqrtf(dir[0] * dir[0] + dir[1] * dir[1])) * 180.0f / M_PI_F;
	t->entity.curstate.body = 0;
	t->flags = FTENT_COLLIDEALL | FTENT_COLLIDEKILL;
	t->clientIndex = (short)owner;
	t->entity.baseline.renderamt = 255;
	t->hitcallback = hitcallback;
	t->die = (float)cl_time + life;
}

static void R_PlayerSprites(int client, int modelIndex, int count, int size)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	cl_entity_t *e = CL_EntityByIndex(client);
	if (!m || !e)
		return;
	for (int i = 0; i < count; i++) {
		TEMPENTITY *t = CL_TempEntAlloc(e->origin, m);
		if (!t)
			return;
		t->entity.curstate.rendermode = kRenderTransAlpha;
		t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
		t->entity.curstate.scale = CL_Random(0.1f, 0.25f) * size;
		t->flags = FTENT_COLLIDEWORLD | FTENT_GRAVITY;
		for (int k = 0; k < 3; k++)
			t->entity.baseline.origin[k] = CL_Random(-1, 1) * 128;
		t->die = (float)cl_time + CL_Random(1, 2);
	}
}

static void R_AttachTentToPlayer(int client, int modelIndex, float zoffset, float life)
{
	cl_entity_t *e = CL_EntityByIndex(client);
	model_t *m = CL_ModelForIndex(modelIndex);
	if (!e || !m)
		return;
	vec3_t p;
	VectorCopy(e->origin, p);
	p[2] += zoffset;
	TEMPENTITY *t = CL_TempEntAllocHigh(p, m);
	if (!t)
		return;
	t->entity.curstate.rendermode = kRenderNormal;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 192;
	t->entity.curstate.renderfx = 0;
	t->clientIndex = (short)client;
	t->tentOffset[2] = zoffset;
	t->die = (float)cl_time + life;
	t->flags |= FTENT_PLYRATTACHMENT | FTENT_PERSIST;
	if (m->type == mod_sprite) {
		t->flags |= FTENT_SPRANIMATE | FTENT_SPRANIMATELOOP;
		t->entity.curstate.framerate = 10;
	}
}

static void R_KillAttachedTents(int client)
{
	for (TEMPENTITY *t = tent_active; t; t = t->next)
		if ((t->flags & FTENT_PLYRATTACHMENT) && t->clientIndex == client)
			t->die = (float)cl_time;   // the client removes it on its next update
}

static void R_RicochetSound(float *pos) {}   // sound arrives in step 6
static void R_RocketFlare(float *pos)
{
	TEMPENTITY *t = CL_TempEntAlloc(pos, SpriteByName("sprites/animglow01.spr"));
	if (!t)
		return;
	t->entity.curstate.rendermode = kRenderGlow;
	t->entity.curstate.renderfx = 14;
	t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
	t->entity.curstate.scale = 1;
	t->entity.curstate.frame = (float)CL_RandomInt(0, (int)t->frameMax);
	t->die = (float)cl_time + 0.01f;
}

// ---------------------------------------------------------------- decals and lights

static int Draw_DecalIndex(int id) { return id; }   // one list, shared with the game's DecalIndex
static int Draw_DecalIndexFromName(char *name) { return name ? R_DecalIndex(name) : -1; }

static void EfxDecalShoot(int tex, int entity, int modelIndex, float *pos, int flags)
{
	R_DecalShoot(tex, entity, modelIndex, pos, flags, 1.0f);
}

static void EfxFireCustomDecal(int tex, int entity, int modelIndex, float *pos, int flags, float scale)
{
	R_DecalShoot(tex, entity, modelIndex, pos, flags, scale);
}

static void EfxDecalRemoveAll(int tex) { R_DecalRemoveAll(tex); }

// dynamic lights aren't drawn yet; the client gets somewhere harmless to write
static dlight_t dlights[32];
static int dlight_next;

static dlight_t *CL_AllocDlight(int key)
{
	if (key)
		for (int i = 0; i < 32; i++)
			if (dlights[i].key == key) {
				memset(&dlights[i], 0, sizeof(dlights[i]));
				dlights[i].key = key;
				return &dlights[i];
			}
	dlight_t *d = &dlights[dlight_next++ & 31];
	memset(d, 0, sizeof(*d));
	d->key = key;
	return d;
}

static void R_MultiGunshot(float *org, float *dir, float *noise, int count, int decalCount, int *decals)
{
	vec3_t right, up;
	vec3_t angles;
	angles[0] = -atan2f(dir[2], sqrtf(dir[0] * dir[0] + dir[1] * dir[1])) * 180.0f / M_PI_F;
	angles[1] = SV_VecToYaw(dir);
	angles[2] = 0;
	vec3_t fwd;
	AngleVectors(angles, fwd, right, up);
	for (int i = 0; i < count; i++) {
		float x, y, z;
		do {
			x = CL_Random(-0.5f, 0.5f) + CL_Random(-0.5f, 0.5f);
			y = CL_Random(-0.5f, 0.5f) + CL_Random(-0.5f, 0.5f);
			z = x * x + y * y;
		} while (z > 1);
		vec3_t d, end;
		for (int k = 0; k < 3; k++)
			d[k] = dir[k] + x * noise[0] * right[k] + y * noise[1] * up[k];
		VectorMA(org, 2048, d, end);
		trace_t tr = SV_Move(org, zero_vec, zero_vec, end, 0, NULL);
		if (tr.fraction == 1)
			continue;
		R_BulletImpactParticles(tr.endpos);
		if (decalCount && decals && tr.ent && tr.ent->v.solid == SOLID_BSP)
			R_DecalShoot(decals[CL_RandomInt(0, decalCount - 1)], NUM_FOR_EDICT(tr.ent), 0, tr.endpos, 0, 1.0f);
	}
}

void CL_InitEfxAPI(efx_api_t *api)
{
	memset(api, 0, sizeof(*api));
	api->R_AllocParticle = R_AllocParticle;
	api->R_BlobExplosion = R_BlobExplosion;
	api->R_Blood = R_Blood;
	api->R_BloodSprite = R_BloodSprite;
	api->R_BloodStream = R_BloodStream;
	api->R_BreakModel = R_BreakModel;
	api->R_Bubbles = R_Bubbles;
	api->R_BubbleTrail = R_BubbleTrail;
	api->R_BulletImpactParticles = R_BulletImpactParticles;
	api->R_EntityParticles = R_EntityParticles;
	api->R_Explosion = R_Explosion;
	api->R_FizzEffect = R_FizzEffect;
	api->R_FireField = R_FireField;
	api->R_FlickerParticles = R_FlickerParticles;
	api->R_FunnelSprite = R_FunnelSprite;
	api->R_Implosion = R_Implosion;
	api->R_LargeFunnel = R_LargeFunnel;
	api->R_LavaSplash = R_LavaSplash;
	api->R_MultiGunshot = R_MultiGunshot;
	api->R_MuzzleFlash = R_MuzzleFlash;
	api->R_ParticleBox = R_ParticleBox;
	api->R_ParticleBurst = R_ParticleBurst;
	api->R_ParticleExplosion = R_ParticleExplosion;
	api->R_ParticleExplosion2 = R_ParticleExplosion2;
	api->R_ParticleLine = R_ParticleLine;
	api->R_PlayerSprites = R_PlayerSprites;
	api->R_Projectile = R_Projectile;
	api->R_RicochetSound = R_RicochetSound;
	api->R_RicochetSprite = R_RicochetSprite;
	api->R_RocketFlare = R_RocketFlare;
	api->R_RocketTrail = R_RocketTrail;
	api->R_RunParticleEffect = R_RunParticleEffect;
	api->R_ShowLine = R_ShowLine;
	api->R_SparkEffect = R_SparkEffect;
	api->R_SparkShower = R_SparkShower;
	api->R_SparkStreaks = R_SparkStreaks;
	api->R_Spray = R_Spray;
	api->R_Sprite_Explode = R_Sprite_Explode;
	api->R_Sprite_Smoke = R_Sprite_Smoke;
	api->R_Sprite_Spray = R_Sprite_Spray;
	api->R_Sprite_Trail = R_Sprite_Trail;
	api->R_Sprite_WallPuff = R_Sprite_WallPuff;
	api->R_StreakSplash = R_StreakSplash;
	api->R_TracerEffect = R_TracerEffect;
	api->R_UserTracerParticle = R_UserTracerParticle;
	api->R_TracerParticles = R_TracerParticles;
	api->R_TeleportSplash = R_TeleportSplash;
	api->R_TempSphereModel = R_TempSphereModel;
	api->R_TempModel = R_TempModel;
	api->R_DefaultSprite = R_DefaultSprite;
	api->R_TempSprite = R_TempSprite;
	api->Draw_DecalIndex = Draw_DecalIndex;
	api->Draw_DecalIndexFromName = Draw_DecalIndexFromName;
	api->R_DecalShoot = EfxDecalShoot;
	api->R_AttachTentToPlayer = R_AttachTentToPlayer;
	api->R_KillAttachedTents = R_KillAttachedTents;
	api->R_BeamCirclePoints = R_BeamCirclePoints;
	api->R_BeamEntPoint = R_BeamEntPoint;
	api->R_BeamEnts = R_BeamEnts;
	api->R_BeamFollow = R_BeamFollow;
	api->R_BeamKill = R_BeamKill;
	api->R_BeamLightning = R_BeamLightning;
	api->R_BeamPoints = R_BeamPoints;
	api->R_BeamRing = R_BeamRing;
	api->CL_AllocDlight = CL_AllocDlight;
	api->CL_AllocElight = CL_AllocDlight;
	api->CL_TempEntAlloc = TempEntAlloc;
	api->CL_TempEntAllocNoModel = TempEntAllocNoModel;
	api->CL_TempEntAllocHigh = TempEntAllocHigh;
	api->CL_TentEntAllocCustom = TempEntAllocCustom;
	api->R_GetPackedColor = R_GetPackedColor;
	api->R_LookupColor = R_LookupColor;
	api->R_DecalRemoveAll = EfxDecalRemoveAll;
	api->R_FireCustomDecal = EfxFireCustomDecal;
}

void CL_InitTempEnts(void)
{
	tempents_max = Cvar_Get("cl_tempents_max", "2048", FCVAR_ARCHIVE, "most temp entities (shells, gibs, sprites) at once");
}

// ---------------------------------------------------------------- svc_temp_entity

typedef struct {
	const unsigned char *data;
	int len, pos;
	qboolean bad;
} reader_t;

static int ReadByte(reader_t *r)
{
	if (r->pos + 1 > r->len) {
		r->bad = true;
		return 0;
	}
	return r->data[r->pos++];
}

static int ReadShort(reader_t *r)
{
	if (r->pos + 2 > r->len) {
		r->bad = true;
		return 0;
	}
	short s;
	memcpy(&s, r->data + r->pos, 2);
	r->pos += 2;
	return s;
}

static float ReadCoord(reader_t *r) { return ReadShort(r) / 8.0f; }

static void ReadVec(reader_t *r, vec3_t v)
{
	v[0] = ReadCoord(r);
	v[1] = ReadCoord(r);
	v[2] = ReadCoord(r);
}

static const char *ReadString(reader_t *r)
{
	static char buf[2048];
	int n = 0;
	while (r->pos < r->len && r->data[r->pos]) {
		if (n < (int)sizeof(buf) - 1)
			buf[n++] = (char)r->data[r->pos];
		r->pos++;
	}
	if (r->pos < r->len)
		r->pos++;
	else
		r->bad = true;
	buf[n] = 0;
	return buf;
}

// the beam messages share a tail: sprite, frame, framerate, life, width, noise, color, brightness, speed
typedef struct { int sprite, startframe; float framerate, life, width, noise, r, g, b, brightness, speed; } beamparms_t;

static void ReadBeamTail(reader_t *r, beamparms_t *b)
{
	b->sprite = ReadShort(r);
	b->startframe = ReadByte(r);
	b->framerate = ReadByte(r) * 0.1f;
	b->life = ReadByte(r) * 0.1f;
	b->width = ReadByte(r) * 0.1f;
	b->noise = ReadByte(r) * 0.01f;
	b->r = ReadByte(r) / 255.0f;
	b->g = ReadByte(r) / 255.0f;
	b->b = ReadByte(r) / 255.0f;
	b->brightness = ReadByte(r) / 255.0f;
	b->speed = ReadByte(r) * 0.1f;
}

static float FixedSigned16(int v, float scale) { return (short)v / scale; }
static float FixedUnsigned16(int v, float scale) { return (unsigned short)v / scale; }

void CL_ParseTempEntity(const unsigned char *data, int len)
{
	reader_t rd = { data, len, 0, false }, *r = &rd;
	int type = ReadByte(r);
	vec3_t pos, pos2, dir;
	beamparms_t b;
	TEMPENTITY *t;
	int ent, model, count;
	float scale, life;

	switch (type) {
	case TE_BEAMPOINTS:
		ReadVec(r, pos);
		ReadVec(r, pos2);
		ReadBeamTail(r, &b);
		R_BeamPoints(pos, pos2, b.sprite, b.life, b.width, b.noise, b.brightness, b.speed, b.startframe, b.framerate, b.r, b.g, b.b);
		break;
	case TE_BEAMENTPOINT:
		ent = ReadShort(r);
		ReadVec(r, pos2);
		ReadBeamTail(r, &b);
		R_BeamEntPoint(ent, pos2, b.sprite, b.life, b.width, b.noise, b.brightness, b.speed, b.startframe, b.framerate, b.r, b.g, b.b);
		break;
	case TE_BEAMENTS: {
		int e1 = ReadShort(r), e2 = ReadShort(r);
		ReadBeamTail(r, &b);
		R_BeamEnts(e1, e2, b.sprite, b.life, b.width, b.noise, b.brightness, b.speed, b.startframe, b.framerate, b.r, b.g, b.b);
		break;
	}
	case TE_BEAMRING: {
		int e1 = ReadShort(r), e2 = ReadShort(r);
		ReadBeamTail(r, &b);
		R_BeamRing(e1, e2, b.sprite, b.life, b.width, b.noise, b.brightness, b.speed, b.startframe, b.framerate, b.r, b.g, b.b);
		break;
	}
	case TE_BEAMTORUS:
	case TE_BEAMDISK:
	case TE_BEAMCYLINDER:
		ReadVec(r, pos);
		ReadVec(r, pos2);
		ReadBeamTail(r, &b);
		R_BeamCirclePoints(type, pos, pos2, b.sprite, b.life, b.width, b.noise, b.brightness, b.speed, b.startframe, b.framerate, b.r, b.g, b.b);
		break;
	case TE_BEAMFOLLOW: {
		ent = ReadShort(r);
		model = ReadShort(r);
		life = ReadByte(r) * 0.1f;
		float width = ReadByte(r) * 0.1f;
		float cr = ReadByte(r) / 255.0f, cg = ReadByte(r) / 255.0f, cb = ReadByte(r) / 255.0f;
		float br = ReadByte(r) / 255.0f;
		R_BeamFollow(ent, model, life, width, cr, cg, cb, br);
		break;
	}
	case TE_LIGHTNING: {
		ReadVec(r, pos);
		ReadVec(r, pos2);
		life = ReadByte(r) * 0.1f;
		float width = ReadByte(r) * 0.1f;
		float noise = ReadByte(r) * 0.01f;
		model = ReadShort(r);
		R_BeamLightning(pos, pos2, model, life, width, noise, 0.6f, 3.5f);
		break;
	}
	case TE_BEAMSPRITE: {
		ReadVec(r, pos);
		ReadVec(r, pos2);
		int beam = ReadShort(r), spr = ReadShort(r);
		BEAM *bm = R_BeamPoints(pos, pos2, beam, 0.01f, 0.4f, 0, CL_Random(0.5f, 0.655f), 5.0f, 0, 0, 1, 1, 1);
		if (bm)
			bm->flags |= FBEAM_SINENOISE;
		t = R_DefaultSprite(pos2, spr, 1);
		if (t) {
			t->entity.curstate.scale = 0.1f;
			t->entity.curstate.rendermode = kRenderTransAdd;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = 255;
			t->die = (float)cl_time + 0.01f;
		}
		break;
	}
	case TE_KILLBEAM:
		R_BeamKill(ReadShort(r));
		break;
	case TE_GUNSHOT:
		ReadVec(r, pos);
		R_RunParticleEffect(pos, zero_vec, 0, 20);
		break;
	case TE_EXPLOSION: {
		ReadVec(r, pos);
		model = ReadShort(r);
		scale = ReadByte(r) * 0.1f;
		float fr = (float)ReadByte(r);
		int flags = ReadByte(r);
		R_Explosion(pos, model, scale, fr, flags);
		break;
	}
	case TE_TAREXPLOSION:
		ReadVec(r, pos);
		R_BlobExplosion(pos);
		break;
	case TE_SMOKE: {
		ReadVec(r, pos);
		model = ReadShort(r);
		scale = ReadByte(r) * 0.1f;
		float fr = (float)ReadByte(r);
		t = R_DefaultSprite(pos, model, fr);
		R_Sprite_Smoke(t, scale);
		break;
	}
	case TE_TRACER:
		ReadVec(r, pos);
		ReadVec(r, pos2);
		R_TracerEffect(pos, pos2);
		break;
	case TE_SPARKS:
		ReadVec(r, pos);
		R_SparkShower(pos);
		break;
	case TE_LAVASPLASH:
		ReadVec(r, pos);
		R_LavaSplash(pos);
		break;
	case TE_TELEPORT:
		ReadVec(r, pos);
		R_TeleportSplash(pos);
		break;
	case TE_EXPLOSION2: {
		ReadVec(r, pos);
		int start = ReadByte(r), n = ReadByte(r);
		R_ParticleExplosion2(pos, start, n);
		break;
	}
	case TE_BSPDECAL: {
		ReadVec(r, pos);
		int tex = ReadShort(r);
		ent = ReadShort(r);
		model = ent ? ReadShort(r) : 0;
		R_DecalShoot(tex, ent, model, pos, 1, 1.0f);   // permanent
		break;
	}
	case TE_IMPLOSION: {
		ReadVec(r, pos);
		float radius = (float)ReadByte(r);
		count = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		R_Implosion(pos, radius, count, life);
		break;
	}
	case TE_SPRITETRAIL: {
		ReadVec(r, pos);
		ReadVec(r, pos2);
		model = ReadShort(r);
		count = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		scale = ReadByte(r) * 0.1f;
		float vel = ReadByte(r) * 10.0f, rnd = ReadByte(r) * 10.0f;
		R_Sprite_Trail(type, pos, pos2, model, count, life, scale, rnd, 255, vel);
		break;
	}
	case TE_SPRITE: {
		ReadVec(r, pos);
		model = ReadShort(r);
		scale = ReadByte(r) * 0.1f;
		int a = ReadByte(r);
		t = R_DefaultSprite(pos, model, 0);
		if (t) {
			t->entity.curstate.scale = scale;
			t->entity.curstate.rendermode = kRenderTransAdd;
			t->entity.curstate.renderamt = t->entity.baseline.renderamt = a;
		}
		break;
	}
	case TE_GLOWSPRITE: {
		ReadVec(r, pos);
		model = ReadShort(r);
		life = ReadByte(r) * 0.1f;
		scale = ReadByte(r) * 0.1f;
		int a = ReadByte(r);
		t = R_TempSprite(pos, NULL, scale, model, kRenderGlow, 14, a / 255.0f, life, FTENT_FADEOUT);
		if (t)
			t->fadeSpeed = 2;
		break;
	}
	case TE_STREAK_SPLASH: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		int color = ReadByte(r);
		count = ReadShort(r);
		int speed = ReadShort(r), vel = ReadShort(r);
		R_StreakSplash(pos, dir, color, count, (float)speed, -vel, vel);
		break;
	}
	case TE_DLIGHT:
		ReadVec(r, pos);
		for (int i = 0; i < 6; i++)
			ReadByte(r);   // radius, color, life, decay: dynamic lights aren't drawn yet
		break;
	case TE_ELIGHT:
		ReadShort(r);
		ReadVec(r, pos);
		ReadCoord(r);
		ReadByte(r); ReadByte(r); ReadByte(r); ReadByte(r);
		ReadCoord(r);
		break;
	case TE_TEXTMESSAGE: {
		client_textmessage_t m;
		memset(&m, 0, sizeof(m));
		ReadByte(r);   // channel
		m.x = FixedSigned16(ReadShort(r), 1 << 13);
		m.y = FixedSigned16(ReadShort(r), 1 << 13);
		m.effect = ReadByte(r);
		m.r1 = (unsigned char)ReadByte(r); m.g1 = (unsigned char)ReadByte(r);
		m.b1 = (unsigned char)ReadByte(r); m.a1 = (unsigned char)ReadByte(r);
		m.r2 = (unsigned char)ReadByte(r); m.g2 = (unsigned char)ReadByte(r);
		m.b2 = (unsigned char)ReadByte(r); m.a2 = (unsigned char)ReadByte(r);
		m.fadein = FixedUnsigned16(ReadShort(r), 1 << 8);
		m.fadeout = FixedUnsigned16(ReadShort(r), 1 << 8);
		m.holdtime = FixedUnsigned16(ReadShort(r), 1 << 8);
		if (m.effect == 2)
			m.fxtime = FixedUnsigned16(ReadShort(r), 1 << 8);
		m.pMessage = ReadString(r);
		if (!rd.bad)
			CL_ShowNetMessage(&m);
		break;
	}
	case TE_LINE:
	case TE_BOX: {
		ReadVec(r, pos);
		ReadVec(r, pos2);
		life = ReadShort(r) * 0.1f;
		int cr = ReadByte(r), cg = ReadByte(r), cb = ReadByte(r);
		if (type == TE_LINE)
			R_ParticleLine(pos, pos2, (unsigned char)cr, (unsigned char)cg, (unsigned char)cb, life);
		else
			R_ParticleBox(pos, pos2, (unsigned char)cr, (unsigned char)cg, (unsigned char)cb, life);
		break;
	}
	case TE_LARGEFUNNEL: {
		ReadVec(r, pos);
		model = ReadShort(r);
		int flags = ReadShort(r);
		R_LargeFunnel(pos, flags & 1);
		R_FunnelSprite(pos, model, flags & 1);
		break;
	}
	case TE_BLOODSTREAM:
	case TE_BLOOD: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		int color = ReadByte(r), speed = ReadByte(r);
		if (type == TE_BLOOD)
			R_Blood(pos, dir, color, speed);
		else
			R_BloodStream(pos, dir, color, speed);
		break;
	}
	case TE_SHOWLINE:
		ReadVec(r, pos);
		ReadVec(r, pos2);
		R_ShowLine(pos, pos2);
		break;
	case TE_DECAL:
	case TE_DECALHIGH:
	case TE_WORLDDECAL:
	case TE_WORLDDECALHIGH: {
		ReadVec(r, pos);
		int tex = ReadByte(r);
		if (type == TE_DECALHIGH || type == TE_WORLDDECALHIGH)
			tex += 256;
		ent = (type == TE_DECAL || type == TE_DECALHIGH) ? ReadShort(r) : 0;
		R_DecalShoot(tex, ent, 0, pos, 0, 1.0f);
		break;
	}
	case TE_FIZZ: {
		ent = ReadShort(r);
		model = ReadShort(r);
		int density = ReadByte(r);
		R_FizzEffect(CL_EntityByIndex(ent), model, density);
		break;
	}
	case TE_MODEL: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		vec3_t ang = { 0, ReadByte(r) * (360.0f / 256.0f), 0 };
		model = ReadShort(r);
		int sound = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		R_TempModel(pos, dir, ang, life, model, sound);
		break;
	}
	case TE_EXPLODEMODEL: {
		ReadVec(r, pos);
		float speed = ReadCoord(r);
		model = ReadShort(r);
		count = ReadShort(r);
		life = ReadByte(r) * 0.1f;
		R_TempSphereModel(pos, speed, life, count, model);
		break;
	}
	case TE_BREAKMODEL: {
		vec3_t size;
		ReadVec(r, pos);
		ReadVec(r, size);
		ReadVec(r, dir);
		float rnd = ReadByte(r) * 10.0f;
		model = ReadShort(r);
		count = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		int flags = ReadByte(r);
		R_BreakModel(pos, size, dir, rnd, life, count, model, (char)flags);
		break;
	}
	case TE_GUNSHOTDECAL: {
		ReadVec(r, pos);
		ent = ReadShort(r);
		int tex = ReadByte(r);
		R_DecalShoot(tex, ent, 0, pos, 0, 1.0f);
		R_BulletImpactParticles(pos);
		break;
	}
	case TE_SPRITE_SPRAY:
	case TE_SPRAY: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		model = ReadShort(r);
		count = ReadByte(r);
		int speed = ReadByte(r), noise = ReadByte(r);
		if (type == TE_SPRAY) {
			int mode = ReadByte(r);
			R_Spray(pos, dir, model, count, speed, noise, mode);
		} else {
			R_Sprite_Spray(pos, dir, model, count, speed, noise);
		}
		break;
	}
	case TE_ARMOR_RICOCHET:
		ReadVec(r, pos);
		scale = ReadByte(r) * 0.1f;
		R_RicochetSprite(pos, SpriteByName("sprites/richo1.spr"), 0.1f, scale);
		break;
	case TE_PLAYERDECAL: {
		ReadByte(r);
		ReadVec(r, pos);
		ent = ReadShort(r);
		int tex = ReadByte(r);
		model = ReadShort(r);
		R_DecalShoot(tex, ent, model, pos, 0, 1.0f);   // spray logos aren't loaded; this uses the decal list
		break;
	}
	case TE_BUBBLES:
	case TE_BUBBLETRAIL: {
		ReadVec(r, pos);
		ReadVec(r, pos2);
		float height = ReadCoord(r);
		model = ReadShort(r);
		count = ReadByte(r);
		float speed = ReadCoord(r);
		if (type == TE_BUBBLES)
			R_Bubbles(pos, pos2, height, model, count, speed);
		else
			R_BubbleTrail(pos, pos2, height, model, count, speed);
		break;
	}
	case TE_BLOODSPRITE: {
		ReadVec(r, pos);
		int m1 = ReadShort(r), m2 = ReadShort(r);
		int color = ReadByte(r);
		scale = (float)ReadByte(r);
		R_BloodSprite(pos, color, m1, m2, scale);
		break;
	}
	case TE_PROJECTILE: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		model = ReadShort(r);
		int l = ReadByte(r), owner = ReadByte(r);
		R_Projectile(pos, dir, model, l, owner, NULL);
		break;
	}
	case TE_PLAYERSPRITES: {
		int pl = ReadByte(r);
		model = ReadShort(r);
		count = ReadByte(r);
		int variance = ReadByte(r);
		R_PlayerSprites(pl, model, count, variance);
		break;
	}
	case TE_PARTICLEBURST: {
		ReadVec(r, pos);
		int radius = ReadShort(r), color = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		R_ParticleBurst(pos, radius, color, life);
		break;
	}
	case TE_FIREFIELD: {
		ReadVec(r, pos);
		int radius = ReadShort(r);
		model = ReadShort(r);
		count = ReadByte(r);
		int flags = ReadByte(r);
		life = ReadByte(r) * 0.1f;
		R_FireField(pos, radius, model, count, flags, life);
		break;
	}
	case TE_PLAYERATTACHMENT: {
		int pl = ReadByte(r);
		float z = ReadCoord(r);
		model = ReadShort(r);
		life = ReadShort(r) * 0.1f;
		R_AttachTentToPlayer(pl, model, z, life);
		break;
	}
	case TE_KILLPLAYERATTACHMENTS:
		R_KillAttachedTents(ReadByte(r));
		break;
	case TE_MULTIGUNSHOT: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		float noise[2] = { ReadCoord(r) * 0.01f, ReadCoord(r) * 0.01f };
		count = ReadByte(r);
		int tex = ReadByte(r);
		R_MultiGunshot(pos, dir, noise, count, 1, &tex);
		break;
	}
	case TE_USERTRACER: {
		ReadVec(r, pos);
		ReadVec(r, dir);
		life = ReadByte(r) * 0.1f;
		int color = ReadByte(r);
		float length = ReadByte(r) * 0.1f;
		R_UserTracerParticle(pos, dir, life, color, length, 0, NULL);
		break;
	}
	default:
		Con_DPrintf("temp entity %d isn't handled\n", type);
		return;
	}
	if (rd.bad)
		Con_DPrintf("temp entity %d was short\n", type);
}
