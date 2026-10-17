// newen - sprite drawing (.spr): orientation types, frame groups and render modes
#include "model.h"

static const spriteframe_t *GetFrame(const rentity_t *e)
{
	const spritemodel_t *sp = &e->model->sprite;
	// until hl.dll animates sprites, the renderer advances the frame at framerate frames per second
	float f = e->frame + (float)((r_refdef.time - e->animtime) * e->framerate);
	int n = (int)f % sp->numframes;
	if (n < 0)
		n += sp->numframes;
	const spritegroup_t *g = &sp->frames[n];
	if (g->numframes <= 1 || !g->intervals)
		return &g->frames[0];
	// groups pick a frame by time, looping over the summed intervals
	float total = g->intervals[g->numframes - 1];
	float t = (float)fmod(r_refdef.time, total);
	for (int i = 0; i < g->numframes; i++)
		if (t < g->intervals[i])
			return &g->frames[i];
	return &g->frames[g->numframes - 1];
}

static float SpriteScale(const rentity_t *e)
{
	float scale = e->scale > 0 ? e->scale : 1.0f;
	if (e->rendermode == kRenderGlow) {
		// glows keep their size on screen
		vec3_t d;
		VectorSubtract(e->origin, r_refdef.origin, d);
		scale *= VectorLength(d) * 0.005f;
	}
	return scale;
}

qboolean R_SpriteCull(const rentity_t *e)
{
	float r = e->model->radius * SpriteScale(e);
	vec3_t mins = { e->origin[0] - r, e->origin[1] - r, e->origin[2] - r };
	vec3_t maxs = { e->origin[0] + r, e->origin[1] + r, e->origin[2] + r };
	return R_CullBox(mins, maxs);
}

void R_SpriteDrawModel(rentity_t *e)
{
	const spritemodel_t *sp = &e->model->sprite;
	if (e->rendermode == kRenderGlow) {
		// a glow behind a wall isn't seen at all; the last 8 units are allowed, since glows usually
		// sit inside the light fixture they belong to (GoldSrc has the same tolerance)
		vec3_t d, end;
		VectorSubtract(r_refdef.origin, e->origin, d);
		float len = VectorNormalize(d);
		VectorMA(e->origin, len > 8 ? 8 : len, d, end);
		if (R_LineBlocked(r_refdef.origin, end))
			return;
	}
	const spriteframe_t *fr = GetFrame(e);

	vec3_t up, right;
	switch (sp->type) {
	case SPR_VP_PARALLEL_UPRIGHT: {
		// stands upright and faces the view plane
		VectorSet(up, 0, 0, 1);
		VectorSet(right, r_refdef.forward[1], -r_refdef.forward[0], 0);
		if (VectorNormalize(right) == 0)
			VectorCopy(r_refdef.right, right);
		break;
	}
	case SPR_FACING_UPRIGHT: {
		// stands upright and turns toward the viewer
		vec3_t d;
		VectorSubtract(e->origin, r_refdef.origin, d);
		VectorSet(up, 0, 0, 1);
		VectorSet(right, d[1], -d[0], 0);
		if (VectorNormalize(right) == 0)
			VectorCopy(r_refdef.right, right);
		break;
	}
	case SPR_ORIENTED: {
		vec3_t f;
		AngleVectors(e->angles, f, right, up);
		break;
	}
	case SPR_VP_PARALLEL_ORIENTED: {
		// faces the view plane, rotated by the entity's roll
		float a = DEG2RAD(e->angles[2]), s = sinf(a), c = cosf(a);
		for (int i = 0; i < 3; i++) {
			right[i] = r_refdef.right[i] * c + r_refdef.up[i] * s;
			up[i] = r_refdef.right[i] * -s + r_refdef.up[i] * c;
		}
		break;
	}
	default:
		VectorCopy(r_refdef.up, up);
		VectorCopy(r_refdef.right, right);
		break;
	}

	float scale = SpriteScale(e);
	float alpha = e->renderamt / 255.0f;
	float color[3];
	qboolean nocolor = !e->rendercolor[0] && !e->rendercolor[1] && !e->rendercolor[2];
	for (int i = 0; i < 3; i++)
		color[i] = nocolor ? 1.0f : e->rendercolor[i] / 255.0f;

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, fr->gl->id);
	switch (e->rendermode) {
	case kRenderNormal:
		alpha = 1;
		if (fr->gl->flags & TF_ALPHA) {
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, 0.5f);
		}
		break;
	case kRenderTransAlpha:
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.25f);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		break;
	case kRenderGlow:
		glDisable(GL_DEPTH_TEST);
		// fall through
	case kRenderTransAdd:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDepthMask(GL_FALSE);
		break;
	default:   // kRenderTransColor, kRenderTransTexture
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		break;
	}
	glColor4f(color[0], color[1], color[2], alpha);

	vec3_t p;
	glBegin(GL_QUADS);
	glTexCoord2f(0, 1);
	VectorMA(e->origin, fr->down * scale, up, p);
	VectorMA(p, fr->left * scale, right, p);
	glVertex3fv(p);
	glTexCoord2f(0, 0);
	VectorMA(e->origin, fr->up * scale, up, p);
	VectorMA(p, fr->left * scale, right, p);
	glVertex3fv(p);
	glTexCoord2f(1, 0);
	VectorMA(e->origin, fr->up * scale, up, p);
	VectorMA(p, fr->right * scale, right, p);
	glVertex3fv(p);
	glTexCoord2f(1, 1);
	VectorMA(e->origin, fr->down * scale, up, p);
	VectorMA(p, fr->right * scale, right, p);
	glVertex3fv(p);
	glEnd();

	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, 1);
}
