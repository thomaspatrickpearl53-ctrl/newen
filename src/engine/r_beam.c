// newen - beams: textured strips between two points or entities (the egon and gauss guns,
// vortigaunt lightning, lasers, shock rings, trails), from the efx API, temp entity messages and the
// game's beam entities (CBeam, flagged FL_CUSTOMENTITY)
//
// A beam is drawn as a camera-facing strip of segments, jittered by its noise amplitude (or a
// moving sine wave with FBEAM_SINENOISE), with its sprite's texture scrolling along it. Beams add
// light (GL_ONE, GL_ONE) unless they're solid.
#include "cl_local.h"

#define BEAM_BLOCK 64
#define TRAIL_MAX  64

// the client gets the BEAM part; the trail is for R_BeamFollow
typedef struct cbeam_s {
	BEAM   b;
	vec3_t trail[TRAIL_MAX];
	float  trail_time[TRAIL_MAX];
	int    trail_count;
	float  life;
	struct cbeam_s *link;
} cbeam_t;

static cbeam_t *beam_free, *beam_active;
static cbeam_t **beam_blocks;
static int beam_nblocks;
static BEAM *frame_beams;          // the game's beam entities, rebuilt every frame
static int frame_beams_count, frame_beams_cap;
static cvar_t *r_drawbeams;
static double last_update;

static model_t *BeamSprite(int modelIndex)
{
	model_t *m = CL_ModelForIndex(modelIndex);
	return m && m->type == mod_sprite && m->sprite.numframes ? m : NULL;
}

static cbeam_t *Alloc(int modelIndex, float life, float width, float amplitude, float brightness, float speed,
	int startFrame, float framerate, float r, float g, float b)
{
	model_t *m = BeamSprite(modelIndex);
	if (!m)
		return NULL;
	if (!beam_free) {
		cbeam_t *block = Mem_Alloc(sizeof(cbeam_t) * BEAM_BLOCK);
		for (int i = 0; i < BEAM_BLOCK; i++) {
			block[i].link = beam_free;
			beam_free = &block[i];
		}
		beam_blocks = Mem_Realloc(beam_blocks, sizeof(*beam_blocks) * (beam_nblocks + 1));
		beam_blocks[beam_nblocks++] = block;
	}
	cbeam_t *c = beam_free;
	beam_free = c->link;
	memset(c, 0, sizeof(*c));
	BEAM *bm = &c->b;
	bm->modelIndex = modelIndex;
	bm->frameCount = m->sprite.numframes;
	bm->die = (float)cl_time + life;
	bm->width = width;
	bm->amplitude = amplitude;
	bm->brightness = brightness;
	bm->speed = speed;
	bm->frame = (float)startFrame;
	bm->frameRate = framerate;
	bm->r = r;
	bm->g = g;
	bm->b = b;
	bm->flags = FBEAM_ISACTIVE;
	if (life == 0)
		bm->flags |= FBEAM_FOREVER;   // until it's killed (R_BeamKill, or the client sets die)
	c->life = life;
	c->link = beam_active;
	beam_active = c;
	return c;
}

void R_ClearBeams(void)
{
	beam_free = beam_active = NULL;
	for (int b = 0; b < beam_nblocks; b++)
		for (int i = 0; i < BEAM_BLOCK; i++) {
			beam_blocks[b][i].link = beam_free;
			beam_free = &beam_blocks[b][i];
		}
	frame_beams_count = 0;
}

BEAM *R_BeamPoints(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	cbeam_t *c = Alloc(modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b);
	if (!c)
		return NULL;
	c->b.type = TE_BEAMPOINTS;
	if (life == 0)
		c->b.flags &= ~FBEAM_FOREVER;   // points never move; a zero life is one frame
	VectorCopy(start, c->b.source);
	VectorCopy(end, c->b.target);
	return &c->b;
}

BEAM *R_BeamEntPoint(int startEnt, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	cbeam_t *c = Alloc(modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b);
	if (!c)
		return NULL;
	c->b.type = TE_BEAMENTPOINT;
	c->b.flags |= FBEAM_STARTENTITY;
	c->b.startEntity = startEnt;
	VectorCopy(end, c->b.target);
	CL_GetEntityPoint(startEnt, c->b.source);
	return &c->b;
}

BEAM *R_BeamEnts(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	cbeam_t *c = Alloc(modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b);
	if (!c)
		return NULL;
	c->b.type = TE_BEAMENTS;
	c->b.flags |= FBEAM_STARTENTITY | FBEAM_ENDENTITY;
	c->b.startEntity = startEnt;
	c->b.endEntity = endEnt;
	CL_GetEntityPoint(startEnt, c->b.source);
	CL_GetEntityPoint(endEnt, c->b.target);
	return &c->b;
}

BEAM *R_BeamRing(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	BEAM *bm = R_BeamEnts(startEnt, endEnt, modelIndex, life, width, amplitude, brightness, speed, startFrame,
		framerate, r, g, b);
	if (bm)
		bm->type = TE_BEAMRING;
	return bm;
}

BEAM *R_BeamCirclePoints(int type, float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	BEAM *bm = R_BeamPoints(start, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b);
	if (bm)
		bm->type = type;
	return bm;
}

BEAM *R_BeamFollow(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness)
{
	cbeam_t *c = Alloc(modelIndex, life, width, 0, brightness, 0, 0, 0, r, g, b);
	if (!c)
		return NULL;
	c->b.type = TE_BEAMFOLLOW;
	c->b.flags |= FBEAM_STARTENTITY | FBEAM_FOREVER;   // lives as long as what it follows
	c->b.startEntity = startEnt;
	c->life = life > 0 ? life : 1;
	return &c->b;
}

BEAM *R_BeamLightning(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed)
{
	return R_BeamPoints(start, end, modelIndex, life, width, amplitude, brightness, speed, 0, 0, 1, 1, 1);
}

void R_BeamKill(int deadEntity)
{
	for (cbeam_t *c = beam_active; c; c = c->link) {
		BEAM *b = &c->b;
		if (((b->flags & FBEAM_STARTENTITY) && (b->startEntity & 0xfff) == deadEntity) ||
			((b->flags & FBEAM_ENDENTITY) && (b->endEntity & 0xfff) == deadEntity)) {
			b->flags &= ~FBEAM_FOREVER;
			b->die = (float)cl_time - 1;
		}
	}
}

// ---------------------------------------------------------------- the game's beam entities

#define BEAM_ENTPOINT 1
#define BEAM_ENTS     2

void R_AddBeamEntity(const edict_t *e, model_t *sprite)
{
	if (frame_beams_count == frame_beams_cap) {
		frame_beams_cap = frame_beams_cap ? frame_beams_cap * 2 : 32;
		frame_beams = Mem_Realloc(frame_beams, sizeof(BEAM) * frame_beams_cap);
	}
	BEAM *b = &frame_beams[frame_beams_count++];
	memset(b, 0, sizeof(*b));
	int kind = e->v.rendermode & 0x0f, fl = e->v.rendermode & 0xf0;
	b->type = TE_BEAMPOINTS;
	b->flags = FBEAM_ISACTIVE;
	if (fl & 0x10) b->flags |= FBEAM_SINENOISE;
	if (fl & 0x20) b->flags |= FBEAM_SOLID;
	if (fl & 0x40) b->flags |= FBEAM_SHADEIN;
	if (fl & 0x80) b->flags |= FBEAM_SHADEOUT;
	// sequence and skin pack the end entities like "entity:attachment"
	VectorCopy(e->v.origin, b->source);
	VectorCopy(e->v.angles, b->target);
	// an entity end overrides the point stored for it (PointEntInit keeps a point start and an
	// entity end, e.g. the egon's beam from the player's gun)
	// (resolved when drawn, once the view model's attachments are up to date)
	if ((kind == BEAM_ENTPOINT || kind == BEAM_ENTS) && (e->v.sequence & 0x0fff)) {
		b->startEntity = e->v.sequence;
		b->flags |= FBEAM_STARTENTITY;
	}
	if ((kind == BEAM_ENTPOINT || kind == BEAM_ENTS) && (e->v.skin & 0x0fff)) {
		b->endEntity = e->v.skin;
		b->flags |= FBEAM_ENDENTITY;
	}
	b->modelIndex = e->v.modelindex;
	b->frameCount = sprite->sprite.numframes;
	b->frame = e->v.frame;
	b->width = e->v.scale * 0.1f;          // in tenths, like the temp entity messages
	b->amplitude = e->v.body * 0.01f;
	b->r = e->v.rendercolor[0] / 255.0f;
	b->g = e->v.rendercolor[1] / 255.0f;
	b->b = e->v.rendercolor[2] / 255.0f;
	b->brightness = e->v.renderamt / 255.0f;
	b->speed = e->v.animtime * 0.1f;       // the game keeps the scroll rate in animtime
}

// ---------------------------------------------------------------- drawing

static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void BindFrame(const BEAM *b)
{
	model_t *m = BeamSprite(b->modelIndex);
	int n = m ? m->sprite.numframes : 0;
	if (!n) {
		glDisable(GL_TEXTURE_2D);
		return;
	}
	int f = (int)b->frame % n;
	if (f < 0)
		f += n;
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, m->sprite.frames[f].frames[0].gl->id);
}

// one strip through points[0..n-1], each with a brightness factor
static void DrawStrip(const BEAM *b, vec3_t *pts, const float *bright, int n, float vstart, float vstep)
{
	float w = b->width * 0.5f;
	glBegin(GL_QUAD_STRIP);
	for (int i = 0; i < n; i++) {
		vec3_t dir, view, side;
		if (i < n - 1)
			VectorSubtract(pts[i + 1], pts[i], dir);
		else
			VectorSubtract(pts[i], pts[i - 1], dir);
		VectorSubtract(pts[i], r_refdef.origin, view);
		CrossProduct(dir, view, side);
		VectorNormalize(side);
		float br = Clamp01(b->brightness) * bright[i];
		glColor4f(Clamp01(b->r * br), Clamp01(b->g * br), Clamp01(b->b * br), br);
		float v = vstart + vstep * i;
		glTexCoord2f(0, v);
		glVertex3f(pts[i][0] - side[0] * w, pts[i][1] - side[1] * w, pts[i][2] - side[2] * w);
		glTexCoord2f(1, v);
		glVertex3f(pts[i][0] + side[0] * w, pts[i][1] + side[1] * w, pts[i][2] + side[2] * w);
	}
	glEnd();
}

static vec3_t *pts;
static float *brights;
static int pts_cap;

static void EnsurePoints(int n)
{
	if (n > pts_cap) {
		pts_cap = n * 2;
		pts = Mem_Realloc(pts, sizeof(vec3_t) * pts_cap);
		brights = Mem_Realloc(brights, sizeof(float) * pts_cap);
	}
}

static void DrawSegments(const BEAM *b, const vec3_t source, const vec3_t target)
{
	vec3_t delta;
	VectorSubtract(target, source, delta);
	float length = VectorLength(delta);
	if (length < 0.5f)
		return;
	int segs = b->amplitude > 0 ? (int)(length / 12) : (int)(length / 64);
	if (segs < 2)
		segs = 2;
	if (segs > 128)
		segs = 128;
	EnsurePoints(segs + 1);

	// noise moves sideways to the beam, in the plane facing the viewer
	vec3_t dir, view, side, up;
	VectorCopy(delta, dir);
	VectorNormalize(dir);
	VectorSubtract(source, r_refdef.origin, view);
	CrossProduct(dir, view, side);
	VectorNormalize(side);
	CrossProduct(dir, side, up);
	float amp = b->amplitude * length * 0.05f;
	float t = (float)cl_time;
	for (int i = 0; i <= segs; i++) {
		float f = (float)i / segs;
		VectorMA(source, f, delta, pts[i]);
		if (amp > 0 && i > 0 && i < segs) {
			float a, c;
			if (b->flags & FBEAM_SINENOISE) {
				a = sinf(f * M_PI_F * 4 + t * b->speed) * amp;
				c = cosf(f * M_PI_F * 3 + t * b->speed * 0.7f) * amp * 0.5f;
			} else {
				a = CL_Random(-amp, amp);
				c = CL_Random(-amp, amp) * 0.5f;
			}
			// taper the noise towards the ends
			float taper = sinf(f * M_PI_F);
			VectorMA(pts[i], a * taper, side, pts[i]);
			VectorMA(pts[i], c * taper, up, pts[i]);
		}
		float br = 1;
		if (b->flags & (FBEAM_FADEIN | FBEAM_SHADEIN))
			br *= f;
		if (b->flags & (FBEAM_FADEOUT | FBEAM_SHADEOUT))
			br *= 1 - f;
		brights[i] = br;
	}
	float vstart = (float)fmod(t * b->speed * 0.1, 1.0);
	DrawStrip(b, pts, brights, segs + 1, vstart, length * 0.01f / segs);
}

// rings, disks and cylinders: TE_BEAMTORUS/DISK/CYLINDER grow their radius over time
static void DrawCircle(const BEAM *b, const vec3_t center, float radius, float height)
{
	int segs = 32;
	EnsurePoints(segs + 1);
	for (int i = 0; i <= segs; i++) {
		float a = i * 2 * M_PI_F / segs;
		pts[i][0] = center[0] + cosf(a) * radius;
		pts[i][1] = center[1] + sinf(a) * radius;
		pts[i][2] = center[2];
		brights[i] = 1;
	}
	if (b->type == TE_BEAMCYLINDER) {
		float br = Clamp01(b->brightness);
		glBegin(GL_QUAD_STRIP);
		for (int i = 0; i <= segs; i++) {
			glColor4f(Clamp01(b->r * br), Clamp01(b->g * br), Clamp01(b->b * br), br);
			glTexCoord2f((float)i / segs * 4, 0);
			glVertex3f(pts[i][0], pts[i][1], pts[i][2]);
			glTexCoord2f((float)i / segs * 4, 1);
			glVertex3f(pts[i][0], pts[i][1], pts[i][2] + height);
		}
		glEnd();
	} else if (b->type == TE_BEAMDISK) {
		float br = Clamp01(b->brightness);
		glBegin(GL_TRIANGLE_FAN);
		glColor4f(Clamp01(b->r * br), Clamp01(b->g * br), Clamp01(b->b * br), br);
		glTexCoord2f(0.5f, 0.5f);
		glVertex3fv(center);
		for (int i = 0; i <= segs; i++) {
			float a = i * 2 * M_PI_F / segs;
			glTexCoord2f(0.5f + cosf(a) * 0.5f, 0.5f + sinf(a) * 0.5f);
			glVertex3fv(pts[i]);
		}
		glEnd();
	} else {
		DrawStrip(b, pts, brights, segs + 1, 0, 0.25f);
	}
}

static void DrawBeam(cbeam_t *c, BEAM *b)
{
	if (b->type == TE_BEAMFOLLOW) {
		if (!c || c->trail_count < 2)
			return;
		EnsurePoints(c->trail_count);
		for (int i = 0; i < c->trail_count; i++) {
			VectorCopy(c->trail[i], pts[i]);
			brights[i] = Clamp01(1 - (float)(cl_time - c->trail_time[i]) / c->life);
		}
		DrawStrip(b, pts, brights, c->trail_count, 0, 0.1f);
		return;
	}
	if (b->type == TE_BEAMTORUS || b->type == TE_BEAMDISK || b->type == TE_BEAMCYLINDER) {
		vec3_t d;
		VectorSubtract(b->target, b->source, d);
		float elapsed = c ? c->life - (b->die - (float)cl_time) : 0;
		float radius = VectorLength(d) * (elapsed > 0 ? elapsed : 0);
		DrawCircle(b, b->source, radius, b->width);
		return;
	}
	if (b->type == TE_BEAMRING) {
		vec3_t center, d;
		VectorAdd(b->source, b->target, center);
		VectorScale(center, 0.5f, center);
		VectorSubtract(b->target, b->source, d);
		DrawCircle(b, center, VectorLength(d) * 0.5f, b->width);
		return;
	}
	DrawSegments(b, b->source, b->target);
}

static qboolean EntityGone(int packed)
{
	int index = packed & 0x0fff;
	return !sv.active || index <= 0 || index >= sv.num_edicts || EDICT_NUM(index)->free;
}

static void UpdateBeams(float ft)
{
	cbeam_t *prev = NULL;
	for (cbeam_t *c = beam_active; c;) {
		cbeam_t *next = c->link;
		BEAM *b = &c->b;
		qboolean dead = !(b->flags & FBEAM_FOREVER) && b->die < cl_time;
		if (!dead && (b->flags & FBEAM_STARTENTITY)) {
			if (EntityGone(b->startEntity))
				dead = true;
			else
				CL_GetEntityPoint(b->startEntity, b->source);
		}
		if (!dead && (b->flags & FBEAM_ENDENTITY)) {
			if (EntityGone(b->endEntity))
				dead = true;
			else
				CL_GetEntityPoint(b->endEntity, b->target);
		}
		if (!dead && b->type == TE_BEAMFOLLOW) {
			// a new trail point when it moved; old ones fade out after the beam's life
			if (!c->trail_count || DotProduct(c->trail[c->trail_count - 1], c->trail[c->trail_count - 1]) == 0 ||
				fabsf(c->trail[c->trail_count - 1][0] - b->source[0]) + fabsf(c->trail[c->trail_count - 1][1] - b->source[1]) +
				fabsf(c->trail[c->trail_count - 1][2] - b->source[2]) > 4) {
				if (c->trail_count == TRAIL_MAX) {
					memmove(c->trail, c->trail + 1, sizeof(vec3_t) * (TRAIL_MAX - 1));
					memmove(c->trail_time, c->trail_time + 1, sizeof(float) * (TRAIL_MAX - 1));
					c->trail_count--;
				}
				VectorCopy(b->source, c->trail[c->trail_count]);
				c->trail_time[c->trail_count] = (float)cl_time;
				c->trail_count++;
			}
			int keep = 0;
			while (keep < c->trail_count && cl_time - c->trail_time[keep] > c->life)
				keep++;
			if (keep) {
				memmove(c->trail, c->trail + keep, sizeof(vec3_t) * (c->trail_count - keep));
				memmove(c->trail_time, c->trail_time + keep, sizeof(float) * (c->trail_count - keep));
				c->trail_count -= keep;
			}
		}
		if (dead) {
			if (prev)
				prev->link = next;
			else
				beam_active = next;
			c->link = beam_free;
			beam_free = c;
		} else {
			b->frame += b->frameRate * ft;
			prev = c;
		}
		c = next;
	}
}

void R_DrawBeams(void)
{
	double ft = cl_time - last_update;
	last_update = cl_time;
	if (ft < 0 || ft > 0.25)
		ft = 0;
	UpdateBeams((float)ft);
	if ((!beam_active && !frame_beams_count) || !r_drawbeams->value) {
		frame_beams_count = 0;
		return;
	}
	glEnable(GL_BLEND);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	for (int pass = 0; pass < 2; pass++) {
		int n = pass ? frame_beams_count : 0;
		cbeam_t *c = pass ? NULL : beam_active;
		for (int i = 0; pass ? i < n : c != NULL; i++) {
			BEAM *b = pass ? &frame_beams[i] : &c->b;
			if (pass && (b->flags & FBEAM_STARTENTITY))
				CL_GetEntityPoint(b->startEntity, b->source);
			if (pass && (b->flags & FBEAM_ENDENTITY))
				CL_GetEntityPoint(b->endEntity, b->target);
			if (b->flags & FBEAM_SOLID)
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			else
				glBlendFunc(GL_ONE, GL_ONE);
			BindFrame(b);
			DrawBeam(pass ? NULL : c, b);
			if (!pass)
				c = c->link;
		}
	}
	frame_beams_count = 0;
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

void R_InitBeams(void)
{
	r_drawbeams = Cvar_Get("r_drawbeams", "1", 0, "draws beams (lasers, lightning, the egon and gauss guns)");
}
