// newen - particles: small dots and streaks coloured from the Quake palette (bullet impacts, sparks,
// blood, explosions, tracers, debug lines)
//
// Particles live in blocks that never move (R_AllocParticle hands the client a pointer it may keep
// until the particle dies). Each frame they move, age by type and are drawn as camera-facing dots;
// sparks and tracers are streaks along their velocity.
#include "cl_local.h"

#define PART_BLOCK 1024

typedef struct cparticle_s {
	particle_t p;            // first: the part the client sees
	float      tail;         // > 0: a streak, this many seconds of travel long
	qboolean   rgb_set;      // colour below instead of the palette
	unsigned char rgb[3];
	struct cparticle_s *link;
} cparticle_t;

unsigned char r_palette[768];

static cparticle_t *part_free, *part_active;
static cparticle_t **part_blocks;
static int part_nblocks;
static cvar_t *r_particles, *r_particles_max;
static gltexture_t *dot_texture;
static double last_update;

// Quake's colour ramps (palette indices) for fire and explosions
static const int ramp1[8] = { 0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61 };
static const int ramp2[8] = { 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66 };
static const int ramp3[8] = { 0x6d, 0x6b, 6, 5, 4, 3, 3, 3 };

// tracer colours by index (TE_USERTRACER, R_UserTracerParticle)
static const unsigned char tracer_colors[][3] = {
	{ 255, 255, 255 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 167, 17 }, { 255, 255, 0 },
	{ 255, 0, 255 }, { 0, 255, 255 }, { 255, 128, 0 }, { 128, 128, 128 }, { 255, 64, 64 }, { 64, 64, 255 },
};

static cparticle_t *Alloc(void)
{
	if (!r_particles->value)
		return NULL;
	if (!part_free) {
		if (part_nblocks * PART_BLOCK >= r_particles_max->value && part_nblocks)
			return NULL;
		cparticle_t *block = Mem_Alloc(sizeof(cparticle_t) * PART_BLOCK);
		for (int i = 0; i < PART_BLOCK; i++) {
			block[i].link = part_free;
			part_free = &block[i];
		}
		part_blocks = Mem_Realloc(part_blocks, sizeof(*part_blocks) * (part_nblocks + 1));
		part_blocks[part_nblocks++] = block;
	}
	cparticle_t *c = part_free;
	part_free = c->link;
	memset(c, 0, sizeof(*c));
	c->p.die = (float)cl_time + 1;
	c->link = part_active;
	part_active = c;
	return c;
}

static cparticle_t *New(const float *org, int color, ptype_t type, float life)
{
	cparticle_t *c = Alloc();
	if (!c)
		return NULL;
	VectorCopy(org, c->p.org);
	c->p.color = (short)(color & 255);
	c->p.type = type;
	c->p.die = (float)cl_time + life;
	return c;
}

static void SetRGB(cparticle_t *c, int r, int g, int b)
{
	c->rgb_set = true;
	c->rgb[0] = (unsigned char)r;
	c->rgb[1] = (unsigned char)g;
	c->rgb[2] = (unsigned char)b;
}

particle_t *R_AllocParticle(void (*callback)(particle_t *p, float frametime))
{
	cparticle_t *c = Alloc();
	if (!c)
		return NULL;
	c->p.callback = callback;
	c->p.type = callback ? pt_clientcustom : pt_static;
	return &c->p;
}

void R_ClearParticles(void)
{
	part_free = part_active = NULL;
	for (int b = 0; b < part_nblocks; b++)
		for (int i = 0; i < PART_BLOCK; i++) {
			part_blocks[b][i].link = part_free;
			part_free = &part_blocks[b][i];
		}
}

short R_LookupColor(unsigned char r, unsigned char g, unsigned char b)
{
	int best = 0, bestd = 1 << 30;
	for (int i = 0; i < 255; i++) {
		int dr = r_palette[i * 3] - r, dg = r_palette[i * 3 + 1] - g, db = r_palette[i * 3 + 2] - b;
		int d = dr * dr + dg * dg + db * db;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return (short)best;
}

void R_GetPackedColor(short *packed, short color)
{
	if (packed)
		*packed = color;   // particles keep the palette index
}

// ---------------------------------------------------------------- effects

static void RandomDir(vec3_t v)
{
	do {
		v[0] = CL_Random(-1, 1);
		v[1] = CL_Random(-1, 1);
		v[2] = CL_Random(-1, 1);
	} while (DotProduct(v, v) > 1 || DotProduct(v, v) < 0.01f);
	VectorNormalize(v);
}

void R_RunParticleEffect(float *org, float *dir, int color, int count)
{
	if (count == 1024) {
		R_ParticleExplosion(org);
		return;
	}
	for (int i = 0; i < count; i++) {
		vec3_t p;
		for (int k = 0; k < 3; k++)
			p[k] = org[k] + CL_RandomInt(-8, 7);
		cparticle_t *c = New(p, (color & ~7) + CL_RandomInt(0, 7), pt_slowgrav, 0.1f * CL_RandomInt(1, 5));
		if (!c)
			return;
		VectorScale(dir, 15, c->p.vel);
	}
}

void R_SparkStreaks(float *pos, int count, int vmin, int vmax)
{
	for (int i = 0; i < count; i++) {
		cparticle_t *c = New(pos, 0, pt_grav, CL_Random(0.1f, 0.5f));
		if (!c)
			return;
		SetRGB(c, 255, CL_RandomInt(160, 230), CL_RandomInt(40, 90));
		for (int k = 0; k < 3; k++)
			c->p.vel[k] = CL_Random((float)vmin, (float)vmax);
		c->tail = 0.025f;
	}
}

void R_BulletImpactParticles(float *pos)
{
	R_SparkStreaks(pos, 2, -200, 200);
	for (int i = 0; i < 12; i++) {
		cparticle_t *c = New(pos, 0, pt_grav, CL_Random(0.3f, 0.6f));
		if (!c)
			return;
		int g = CL_RandomInt(90, 150);
		SetRGB(c, g, g, g);
		c->p.org[0] += CL_Random(-2, 2);
		c->p.org[1] += CL_Random(-2, 2);
		c->p.vel[0] = CL_Random(-60, 60);
		c->p.vel[1] = CL_Random(-60, 60);
		c->p.vel[2] = CL_Random(20, 120);
	}
}

void R_StreakSplash(float *pos, float *dir, int color, int count, float speed, int vmin, int vmax)
{
	for (int i = 0; i < count; i++) {
		cparticle_t *c = New(pos, color, pt_grav, CL_Random(0.1f, 0.5f));
		if (!c)
			return;
		for (int k = 0; k < 3; k++)
			c->p.vel[k] = dir[k] * speed + CL_Random((float)vmin, (float)vmax);
		c->tail = 0.02f;
	}
}

particle_t *R_TracerParticles(float *org, float *vel, float life)
{
	cparticle_t *c = New(org, 0, pt_static, life);
	if (!c)
		return NULL;
	SetRGB(c, 255, 200, 90);
	VectorCopy(vel, c->p.vel);
	c->tail = 0.01f;
	return &c->p;
}

void R_TracerEffect(float *start, float *end)
{
	vec3_t dir;
	VectorSubtract(end, start, dir);
	float len = VectorNormalize(dir);
	const float speed = 6000;
	vec3_t vel;
	VectorScale(dir, speed, vel);
	R_TracerParticles(start, vel, len / speed);
}

void R_UserTracerParticle(float *org, float *vel, float life, int colorIndex, float length,
	unsigned char deathcontext, void (*deathfunc)(particle_t *p))
{
	cparticle_t *c = New(org, 0, pt_static, life);
	if (!c)
		return;
	int n = (int)(sizeof(tracer_colors) / sizeof(tracer_colors[0]));
	const unsigned char *rgb = tracer_colors[colorIndex >= 0 && colorIndex < n ? colorIndex : 4];
	SetRGB(c, rgb[0], rgb[1], rgb[2]);
	VectorCopy(vel, c->p.vel);
	c->tail = length > 0 ? length * 0.01f : 0.01f;
	c->p.context = deathcontext;
	c->p.deathfunc = deathfunc;
}

void R_Blood(float *org, float *dir, int pcolor, int speed)
{
	vec3_t d;
	VectorCopy(dir, d);
	VectorNormalize(d);
	int n = speed > 0 ? speed * 2 : 20;
	if (n > 200)
		n = 200;
	for (int i = 0; i < n; i++) {
		cparticle_t *c = New(org, pcolor + CL_RandomInt(0, 3), pt_vox_grav, CL_Random(0.8f, 1.5f));
		if (!c)
			return;
		float s = CL_Random(0.5f, 1.5f) * speed * 2;
		for (int k = 0; k < 3; k++) {
			c->p.org[k] += CL_Random(-2, 2);
			c->p.vel[k] = d[k] * s + CL_Random(-40, 40);
		}
	}
}

void R_BloodStream(float *org, float *dir, int pcolor, int speed)
{
	vec3_t d;
	VectorCopy(dir, d);
	VectorNormalize(d);
	for (int i = 0; i < 100; i++) {
		cparticle_t *c = New(org, pcolor + CL_RandomInt(0, 3), pt_vox_grav, CL_Random(1.5f, 2.5f));
		if (!c)
			return;
		float s = speed * (1 + i * 0.02f);
		for (int k = 0; k < 3; k++)
			c->p.vel[k] = d[k] * s + CL_Random(-10, 10);
	}
}

void R_BlobExplosion(float *org)
{
	for (int i = 0; i < 1024; i++) {
		qboolean a = i & 1;
		cparticle_t *c = New(org, a ? 66 + CL_RandomInt(0, 5) : 150 + CL_RandomInt(0, 5), a ? pt_blob : pt_blob2,
			1 + CL_Random(0, 0.4f));
		if (!c)
			return;
		for (int k = 0; k < 3; k++) {
			c->p.org[k] += CL_RandomInt(-16, 15);
			c->p.vel[k] = (float)CL_RandomInt(-256, 255);
		}
	}
}

void R_ParticleExplosion(float *org)
{
	for (int i = 0; i < 1024; i++) {
		cparticle_t *c = New(org, ramp1[0], (i & 1) ? pt_explode : pt_explode2, 5);
		if (!c)
			return;
		c->p.ramp = (float)CL_RandomInt(0, 3);
		for (int k = 0; k < 3; k++) {
			c->p.org[k] += CL_RandomInt(-16, 15);
			c->p.vel[k] = (float)CL_RandomInt(-256, 255);
		}
	}
}

void R_ParticleExplosion2(float *org, int colorStart, int colorLength)
{
	if (colorLength < 1)
		colorLength = 1;
	for (int i = 0; i < 512; i++) {
		cparticle_t *c = New(org, colorStart + (i % colorLength), pt_blob, 0.3f);
		if (!c)
			return;
		for (int k = 0; k < 3; k++) {
			c->p.org[k] += CL_RandomInt(-16, 15);
			c->p.vel[k] = (float)CL_RandomInt(-256, 255);
		}
	}
}

void R_LavaSplash(float *org)
{
	for (int i = -16; i < 16; i++)
		for (int j = -16; j < 16; j++) {
			cparticle_t *c = New(org, 224 + CL_RandomInt(0, 7), pt_slowgrav, 2 + CL_RandomInt(0, 31) * 0.02f);
			if (!c)
				return;
			vec3_t d = { j * 8.0f + CL_RandomInt(0, 7), i * 8.0f + CL_RandomInt(0, 7), 256 };
			c->p.org[0] += d[0];
			c->p.org[1] += d[1];
			c->p.org[2] += CL_RandomInt(0, 63);
			VectorNormalize(d);
			VectorScale(d, 50.0f + CL_RandomInt(0, 63), c->p.vel);
		}
}

void R_TeleportSplash(float *org)
{
	for (int i = -16; i < 16; i += 4)
		for (int j = -16; j < 16; j += 4)
			for (int k = -24; k < 32; k += 4) {
				cparticle_t *c = New(org, 7 + CL_RandomInt(0, 7), pt_slowgrav, 0.2f + CL_RandomInt(0, 7) * 0.02f);
				if (!c)
					return;
				vec3_t d = { j * 8.0f, i * 8.0f, k * 8.0f };
				c->p.org[0] += i + CL_RandomInt(0, 3);
				c->p.org[1] += j + CL_RandomInt(0, 3);
				c->p.org[2] += k + CL_RandomInt(0, 3);
				VectorNormalize(d);
				VectorScale(d, 50.0f + CL_RandomInt(0, 63), c->p.vel);
			}
}

void R_ParticleBurst(float *pos, int size, int color, float life)
{
	for (int i = 0; i < 64; i++) {
		cparticle_t *c = New(pos, color, pt_static, life);
		if (!c)
			return;
		vec3_t d;
		RandomDir(d);
		VectorScale(d, life > 0 ? size / life : (float)size, c->p.vel);
	}
}

void R_FlickerParticles(float *org)
{
	for (int i = 0; i < 16; i++) {
		cparticle_t *c = New(org, 0, pt_grav, 0.5f + CL_Random(0, 0.5f));
		if (!c)
			return;
		SetRGB(c, 255, CL_RandomInt(150, 220), 40);
		c->p.vel[0] = CL_Random(-32, 32);
		c->p.vel[1] = CL_Random(-32, 32);
		c->p.vel[2] = CL_Random(80, 143);
	}
}

void R_Implosion(float *end, float radius, int count, float life)
{
	if (life <= 0)
		life = 0.1f;
	for (int i = 0; i < count; i++) {
		vec3_t d;
		RandomDir(d);
		vec3_t p;
		VectorMA(end, radius, d, p);
		cparticle_t *c = New(p, 0, pt_static, life);
		if (!c)
			return;
		SetRGB(c, 255, 255, 255);
		VectorScale(d, -radius / life, c->p.vel);
		c->tail = 0.02f;
	}
}

void R_LargeFunnel(float *org, int reverse)
{
	for (int i = -256; i <= 256; i += 32)
		for (int j = -256; j <= 256; j += 32) {
			vec3_t p = { org[0] + i, org[1] + j, org[2] + CL_Random(100, 800) }, d;
			cparticle_t *c = New(reverse ? org : p, 244, pt_static, 1);
			if (!c)
				return;
			if (reverse)
				VectorSubtract(p, org, d);
			else
				VectorSubtract(org, p, d);
			float dist = VectorNormalize(d);
			float vel = CL_Random(64, 128) + (p[2] - org[2]) / 8;
			VectorScale(d, vel, c->p.vel);
			c->p.die = (float)cl_time + dist / vel;
		}
}

void R_ParticleLine(float *start, float *end, unsigned char r, unsigned char g, unsigned char b, float life)
{
	vec3_t d;
	VectorSubtract(end, start, d);
	float len = VectorNormalize(d);
	for (float x = 0; x <= len; x += 2) {
		vec3_t p;
		VectorMA(start, x, d, p);
		cparticle_t *c = New(p, 0, pt_static, life);
		if (!c)
			return;
		SetRGB(c, r, g, b);
	}
}

void R_ParticleBox(float *mins, float *maxs, unsigned char r, unsigned char g, unsigned char b, float life)
{
	// the 12 edges
	for (int i = 0; i < 8; i++)
		for (int k = 0; k < 3; k++) {
			if (i & (1 << k))
				continue;
			vec3_t a, c;
			for (int j = 0; j < 3; j++)
				a[j] = (i & (1 << j)) ? maxs[j] : mins[j];
			VectorCopy(a, c);
			c[k] = maxs[k];
			R_ParticleLine(a, c, r, g, b, life);
		}
}

void R_ShowLine(float *start, float *end)
{
	vec3_t d;
	VectorSubtract(end, start, d);
	float len = VectorNormalize(d);
	for (float x = 0; x <= len; x += 5) {
		vec3_t p;
		VectorMA(start, x, d, p);
		if (!New(p, 75, pt_static, 30))
			return;
	}
}

void R_RocketTrail(float *start, float *end, int type)
{
	vec3_t d;
	VectorSubtract(end, start, d);
	float len = VectorNormalize(d);
	float step = type < 128 ? 3.0f : 6.0f;
	for (float x = 0; x < len; x += step) {
		vec3_t p;
		VectorMA(start, x, d, p);
		cparticle_t *c = New(p, 0, pt_static, 2);
		if (!c)
			return;
		switch (type & 7) {
		case 0:   // rocket
			c->p.ramp = (float)CL_RandomInt(0, 3);
			c->p.color = (short)ramp3[(int)c->p.ramp];
			c->p.type = pt_fire;
			break;
		case 1:   // smoke
			c->p.ramp = (float)(CL_RandomInt(0, 3) + 2);
			c->p.color = (short)ramp3[(int)c->p.ramp];
			c->p.type = pt_fire;
			break;
		case 2:   // blood
		case 4:
			c->p.type = pt_grav;
			c->p.color = (short)(67 + CL_RandomInt(0, 3));
			break;
		case 3:   // tracers
		case 5:
			c->p.die = (float)cl_time + 0.5f;
			c->p.color = (short)((type & 7) == 3 ? 52 : 230);
			c->p.vel[0] = d[1] * 30 * ((int)x & 1 ? 1 : -1);
			c->p.vel[1] = -d[0] * 30 * ((int)x & 1 ? 1 : -1);
			break;
		default:  // purple
			c->p.color = (short)(9 * 16 + 8 + CL_RandomInt(0, 3));
			c->p.die = (float)cl_time + 0.3f;
			break;
		}
		for (int k = 0; k < 3; k++)
			c->p.org[k] += CL_Random(-3, 3);
	}
}

void R_EntityParticles(cl_entity_t *ent)
{
	if (!ent)
		return;
	for (int i = 0; i < 64; i++) {
		vec3_t d;
		RandomDir(d);
		vec3_t p;
		VectorMA(ent->origin, 64, d, p);
		cparticle_t *c = New(p, 0x6f, pt_explode, 0.01f);
		if (!c)
			return;
	}
}

// ---------------------------------------------------------------- update and drawing

static void Update(float ft)
{
	float grav = (sv_gravity ? sv_gravity->value : 800) * ft;
	cparticle_t *prev = NULL;
	for (cparticle_t *c = part_active; c;) {
		cparticle_t *next = c->link;
		particle_t *p = &c->p;
		qboolean dead = p->die < cl_time;
		if (!dead) {
			switch (p->type) {
			case pt_static:
				break;
			case pt_grav:
			case pt_vox_grav:
				p->vel[2] -= grav * 0.5f;
				break;
			case pt_slowgrav:
			case pt_vox_slowgrav:
				p->vel[2] -= grav * 0.05f;
				break;
			case pt_fire:
				p->ramp += ft * 5;
				if (p->ramp >= 6)
					dead = true;
				else
					p->color = (short)ramp3[(int)p->ramp];
				p->vel[2] += grav * 0.05f;
				break;
			case pt_explode:
				p->ramp += ft * 10;
				if (p->ramp >= 8)
					dead = true;
				else
					p->color = (short)ramp1[(int)p->ramp];
				for (int k = 0; k < 3; k++)
					p->vel[k] += p->vel[k] * 4 * ft;
				p->vel[2] -= grav * 0.05f;
				break;
			case pt_explode2:
				p->ramp += ft * 15;
				if (p->ramp >= 8)
					dead = true;
				else
					p->color = (short)ramp2[(int)p->ramp];
				for (int k = 0; k < 3; k++)
					p->vel[k] -= p->vel[k] * ft;
				p->vel[2] -= grav * 0.05f;
				break;
			case pt_blob:
				for (int k = 0; k < 3; k++)
					p->vel[k] += p->vel[k] * 4 * ft;
				p->vel[2] -= grav * 0.05f;
				break;
			case pt_blob2:
				for (int k = 0; k < 2; k++)
					p->vel[k] -= p->vel[k] * 4 * ft;
				p->vel[2] -= grav * 0.05f;
				break;
			case pt_clientcustom:
				if (p->callback)
					CLIENT_CALL("particle callback", p->callback(p, ft));
				break;
			}
			VectorMA(p->org, ft, p->vel, p->org);
		}
		if (dead) {
			if (p->deathfunc)
				CLIENT_CALL("particle deathfunc", p->deathfunc(p));
			if (prev)
				prev->link = next;
			else
				part_active = next;
			c->link = part_free;
			part_free = c;
		} else {
			prev = c;
		}
		c = next;
	}
}

void R_DrawParticles(void)
{
	double ft = cl_time - last_update;
	last_update = cl_time;
	if (!part_active)
		return;
	if (ft < 0 || ft > 0.25)
		ft = 0;
	Update((float)ft);
	if (!part_active)
		return;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, dot_texture->id);
	glBegin(GL_QUADS);
	for (cparticle_t *c = part_active; c; c = c->link) {
		particle_t *p = &c->p;
		vec3_t d;
		VectorSubtract(p->org, r_refdef.origin, d);
		float dist = DotProduct(d, r_refdef.forward);
		if (dist < 4)
			continue;   // behind or at the eye
		const unsigned char *rgb = c->rgb_set ? c->rgb : &r_palette[(p->color & 255) * 3];
		glColor4ub(rgb[0], rgb[1], rgb[2], 255);
		float size = 0.75f + dist * 0.003f;
		if (c->tail > 0) {
			// a streak from where it was a moment ago to where it is
			vec3_t back, side, dir;
			VectorMA(p->org, -c->tail, p->vel, back);
			VectorSubtract(p->org, back, dir);
			CrossProduct(dir, d, side);
			if (VectorNormalize(side) == 0)
				continue;
			float w = size * 0.4f;
			glTexCoord2f(0.5f, 0); glVertex3f(back[0] - side[0] * w, back[1] - side[1] * w, back[2] - side[2] * w);
			glTexCoord2f(0.5f, 1); glVertex3f(back[0] + side[0] * w, back[1] + side[1] * w, back[2] + side[2] * w);
			glTexCoord2f(0.5f, 1); glVertex3f(p->org[0] + side[0] * w, p->org[1] + side[1] * w, p->org[2] + side[2] * w);
			glTexCoord2f(0.5f, 0); glVertex3f(p->org[0] - side[0] * w, p->org[1] - side[1] * w, p->org[2] - side[2] * w);
			continue;
		}
		const float *u = r_refdef.up, *r = r_refdef.right;
		glTexCoord2f(0, 0); glVertex3f(p->org[0] - (r[0] - u[0]) * size, p->org[1] - (r[1] - u[1]) * size, p->org[2] - (r[2] - u[2]) * size);
		glTexCoord2f(1, 0); glVertex3f(p->org[0] + (r[0] + u[0]) * size, p->org[1] + (r[1] + u[1]) * size, p->org[2] + (r[2] + u[2]) * size);
		glTexCoord2f(1, 1); glVertex3f(p->org[0] + (r[0] - u[0]) * size, p->org[1] + (r[1] - u[1]) * size, p->org[2] + (r[2] - u[2]) * size);
		glTexCoord2f(0, 1); glVertex3f(p->org[0] - (r[0] + u[0]) * size, p->org[1] - (r[1] + u[1]) * size, p->org[2] - (r[2] + u[2]) * size);
	}
	glEnd();
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);
}

void R_InitParticles(void)
{
	r_particles = Cvar_Get("r_particles", "1", FCVAR_ARCHIVE, "draws particles (sparks, dust, blood)");
	r_particles_max = Cvar_Get("r_particles_max", "16384", FCVAR_ARCHIVE, "most particles at once");

	size_t n = 0;
	unsigned char *pal = FS_LoadFile("gfx/palette.lmp", &n);
	if (pal && n >= 768) {
		memcpy(r_palette, pal, 768);
	} else {
		for (int i = 0; i < 256; i++)
			r_palette[i * 3] = r_palette[i * 3 + 1] = r_palette[i * 3 + 2] = (unsigned char)i;
		Con_DPrintf("gfx/palette.lmp not found; particles are grey\n");
	}
	if (pal)
		Mem_Free(pal);

	// a soft round dot
	unsigned char dot[16 * 16 * 4];
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			float dx = x - 7.5f, dy = y - 7.5f;
			float a = 1.0f - sqrtf(dx * dx + dy * dy) / 8.0f;
			a = a < 0 ? 0 : a > 0.5f ? 1 : a * 2;
			unsigned char *p = dot + (y * 16 + x) * 4;
			p[0] = p[1] = p[2] = 255;
			p[3] = (unsigned char)(a * 255);
		}
	dot_texture = GL_UploadTexture("*particle", dot, 16, 16, TF_CLAMP);
}
