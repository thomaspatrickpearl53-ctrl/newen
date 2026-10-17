// newen - the triangle API: immediate-mode drawing for the client DLL (HUD_DrawNormalTriangles and
// HUD_DrawTransparentTriangles run inside the 3D view), plus world/screen projection
#include "cl_local.h"

static float view_modelview[16], view_projection[16];
static int view_viewport[4];
static float tri_brightness = 1;
static float tri_color[4] = { 1, 1, 1, 1 };
static int tri_rendermode;

// called once the 3D view is set up, so WorldToScreen works from the HUD too
void CL_CaptureViewMatrices(void)
{
	glGetFloatv(GL_MODELVIEW_MATRIX, view_modelview);
	glGetFloatv(GL_PROJECTION_MATRIX, view_projection);
	glGetIntegerv(GL_VIEWPORT, view_viewport);
}

static void ApplyColor(void)
{
	glColor4f(tri_color[0] * tri_brightness, tri_color[1] * tri_brightness, tri_color[2] * tri_brightness, tri_color[3]);
}

static void Tri_RenderMode(int mode)
{
	tri_rendermode = mode;
	switch (mode) {
	case kRenderNormal:
		glDisable(GL_BLEND);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_TRUE);
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_FALSE);
		break;
	case kRenderTransAlpha:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.25f);
		glDepthMask(GL_TRUE);
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_FALSE);
		break;
	}
}

static void Tri_Begin(int prim)
{
	static const GLenum modes[] = { GL_TRIANGLES, GL_TRIANGLE_FAN, GL_QUADS, GL_POLYGON, GL_LINES,
		GL_TRIANGLE_STRIP, GL_QUAD_STRIP };
	ApplyColor();
	glBegin(prim >= 0 && prim < 7 ? modes[prim] : GL_TRIANGLES);
}

static void Tri_End(void) { glEnd(); }

static void Tri_Color4f(float r, float g, float b, float a)
{
	tri_color[0] = r; tri_color[1] = g; tri_color[2] = b; tri_color[3] = a;
	ApplyColor();
}

static void Tri_Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	Tri_Color4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static void Tri_Color4fRendermode(float r, float g, float b, float a, int mode)
{
	// additive modes carry the alpha in the colour
	if (mode == kRenderTransAdd || mode == kRenderGlow)
		Tri_Color4f(r * a, g * a, b * a, 1);
	else
		Tri_Color4f(r, g, b, a);
}

static void Tri_TexCoord2f(float u, float v) { glTexCoord2f(u, v); }
static void Tri_Vertex3fv(float *p) { glVertex3fv(p); }
static void Tri_Vertex3f(float x, float y, float z) { glVertex3f(x, y, z); }

static void Tri_Brightness(float b)
{
	tri_brightness = b;
	ApplyColor();
}

static void Tri_CullFace(int style)
{
	if (style == TRI_FRONT) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);   // Quake winding with the Z-up modelview
	} else {
		glDisable(GL_CULL_FACE);
	}
}

static int Tri_SpriteTexture(struct model_s *m, int frame)
{
	if (!m || m->type != mod_sprite || !m->sprite.numframes)
		return 0;
	if (frame < 0 || frame >= m->sprite.numframes)
		frame = 0;
	const spriteframe_t *f = &m->sprite.frames[frame].frames[0];
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, f->gl->id);
	return 1;
}

static void Transform(const float *m, const float *in, float *out)
{
	for (int i = 0; i < 4; i++)
		out[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i] * in[3];
}

// to normalised device coordinates (-1..1); returns 1 if the point is behind the viewer
static int Tri_WorldToScreen(float *wpos, float *screen)
{
	float in[4] = { wpos[0], wpos[1], wpos[2], 1 }, eye[4], clip[4];
	Transform(view_modelview, in, eye);
	Transform(view_projection, eye, clip);
	if (clip[3] == 0) {
		screen[0] = screen[1] = screen[2] = 0;
		return 1;
	}
	float w = 1.0f / clip[3];
	screen[0] = clip[0] * w;
	screen[1] = clip[1] * w;
	screen[2] = clip[2] * w;
	return clip[3] < 0;
}

// a 4x4 inverse (column-major, like GL's)
static qboolean Invert(const float *m, float *inv)
{
	float t[16];
	t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
	if (det == 0)
		return false;
	for (int i = 0; i < 16; i++)
		inv[i] = t[i] / det;
	return true;
}

static void Tri_ScreenToWorld(float *screen, float *wpos)
{
	float mvp[16], inv[16];
	// projection * modelview, column-major
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += view_projection[k * 4 + r] * view_modelview[c * 4 + k];
			mvp[c * 4 + r] = s;
		}
	if (!Invert(mvp, inv)) {
		VectorClear(wpos);
		return;
	}
	float in[4] = { screen[0], screen[1], screen[2], 1 }, out[4];
	Transform(inv, in, out);
	float w = out[3] ? 1.0f / out[3] : 0;
	wpos[0] = out[0] * w;
	wpos[1] = out[1] * w;
	wpos[2] = out[2] * w;
}

static void Tri_GetMatrix(const int pname, float *matrix) { glGetFloatv((GLenum)pname, matrix); }

static int Tri_BoxInPVS(float *mins, float *maxs) { return R_BoxVisible(mins, maxs); }

static void Tri_LightAtPoint(float *pos, float *value)
{
	vec3_t c;
	R_LightPoint(pos, c);
	for (int i = 0; i < 3; i++)
		value[i] = c[i] * 255;
}

static void Tri_Fog(float color[3], float start, float end, int on)
{
	if (!on) {
		glDisable(GL_FOG);
		return;
	}
	float c[4] = { color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1 };
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogfv(GL_FOG_COLOR, c);
	glFogf(GL_FOG_START, start);
	glFogf(GL_FOG_END, end);
}

static void Tri_FogParams(float density, int skybox) { glFogf(GL_FOG_DENSITY, density); }

void CL_InitTriAPI(triangleapi_t *api)
{
	memset(api, 0, sizeof(*api));
	api->version = 1;
	api->RenderMode = Tri_RenderMode;
	api->Begin = Tri_Begin;
	api->End = Tri_End;
	api->Color4f = Tri_Color4f;
	api->Color4ub = Tri_Color4ub;
	api->TexCoord2f = Tri_TexCoord2f;
	api->Vertex3fv = Tri_Vertex3fv;
	api->Vertex3f = Tri_Vertex3f;
	api->Brightness = Tri_Brightness;
	api->CullFace = Tri_CullFace;
	api->SpriteTexture = Tri_SpriteTexture;
	api->WorldToScreen = Tri_WorldToScreen;
	api->Fog = Tri_Fog;
	api->ScreenToWorld = Tri_ScreenToWorld;
	api->GetMatrix = Tri_GetMatrix;
	api->BoxInPVS = Tri_BoxInPVS;
	api->LightAtPoint = Tri_LightAtPoint;
	api->Color4fRendermode = Tri_Color4fRendermode;
	api->FogParams = Tri_FogParams;
}
