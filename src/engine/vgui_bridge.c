// newen - VGUI1 bridge: hosts Half-Life's vgui.dll so the stock client.dll can build its viewport
//
// vgui.dll is C++. Its classes are created here by calling their exported constructors on memory we
// allocate, and the engine's drawing surface is a SurfaceBase whose virtual table we fill ourselves.
// The slot order comes from vgui.dll's own exported vtables (SurfaceBase has 35 virtual functions;
// slots 8-16 and 18-34 are the platform part GoldSrc's engine implements). Methods use thiscall:
// "this" in ECX, arguments on the stack, callee pops; MSVC's __fastcall with a dummy EDX argument
// matches that from C. VGUI1 colors are 0..255 with alpha meaning transparency (0 = opaque).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "gl_local.h"

typedef void (__fastcall *ctor_bool_t)(void *self, void *edx, int b);
typedef void (__fastcall *ctor_4i_t)(void *self, void *edx, int x, int y, int w, int h);
typedef void (__fastcall *ctor_ptr_t)(void *self, void *edx, void *p);
typedef void (__fastcall *void_t)(void *self, void *edx);
typedef void (__fastcall *get4_t)(void *self, void *edx, int *a, int *b, int *c, int *d);
typedef void (__fastcall *set4_t)(void *self, void *edx, int a, int b, int c, int d);
typedef int  (__fastcall *int_t)(void *self, void *edx);
typedef void (__fastcall *abc_t)(void *self, void *edx, int ch, int *a, int *b, int *c);
typedef void (__fastcall *rgba_t)(void *self, void *edx, int ch, int x, int y, int w, int h, unsigned char *rgba);

static HMODULE vgui;
static unsigned char *app, *root, *surface;
static void *app_vtable[64];
static void *surface_vtable[40];

static get4_t panel_getAbsExtents, panel_getClipRect, panel_getInset;
static set4_t panel_setBounds;
static void_t panel_solveTraverse, panel_paintTraverse, app_externalTick;
static abc_t  font_getCharABCwide;
static rgba_t font_getCharRGBA;
static int_t  font_getTall, font_getId;

static void *Proc(const char *name)
{
	void *p = vgui ? (void *)GetProcAddress(vgui, name) : NULL;
	if (!p)
		Con_DPrintf("vgui.dll has no %s\n", name);
	return p;
}

// ---------------------------------------------------------------- the surface

static int draw_color[4] = { 255, 255, 255, 255 }, text_color[4] = { 255, 255, 255, 255 };
static int text_pos[2];
static void *text_font;
static int current_texture;

typedef struct { int ofs[2]; int clip[4]; } paintstate_t;
static paintstate_t *pstack;
static int pstack_count, pstack_cap;

typedef struct { int id; GLuint gl; } vtex_t;
static vtex_t *textures;
static int textures_count, next_texture = 1;

typedef struct { int font, ch; GLuint gl; int a, b, c, tall; } glyph_t;
static glyph_t *glyphs;
static int glyphs_count, glyphs_cap;

static void ApplyClip(void)
{
	if (!pstack_count) {
		glDisable(GL_SCISSOR_TEST);
		return;
	}
	const paintstate_t *s = &pstack[pstack_count - 1];
	int w = s->clip[2] - s->clip[0], h = s->clip[3] - s->clip[1];
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	glEnable(GL_SCISSOR_TEST);
	glScissor(s->clip[0], vid_height - s->clip[3], w, h);
}

static void Offset(int *x, int *y)
{
	if (pstack_count) {
		*x += pstack[pstack_count - 1].ofs[0];
		*y += pstack[pstack_count - 1].ofs[1];
	}
}

static void SetGLColor(const int c[4])
{
	glColor4ub((GLubyte)c[0], (GLubyte)c[1], (GLubyte)c[2], (GLubyte)(255 - c[3]));
}

static void __fastcall S_setTitle(void *s, void *e, const char *t) {}
static int  __fastcall S_setFullscreenMode(void *s, void *e, int w, int h, int bpp) { return 0; }
static void __fastcall S_setWindowedMode(void *s, void *e) {}
static void __fastcall S_setAsTopMost(void *s, void *e, int b) {}
static void __fastcall S_createPopup(void *s, void *e, void *panel) {}
static int  __fastcall S_hasFocus(void *s, void *e) { return 1; }
static int  __fastcall S_isWithin(void *s, void *e, int x, int y) { return 1; }
static int  __fastcall S_createNewTextureID(void *s, void *e) { return next_texture++; }
static void __fastcall S_GetMousePos(void *s, void *e, int *x, int *y) { SDL_GetMouseState(x, y); }
static void __fastcall S_invalidate(void *s, void *e, void *panel) {}
static void __fastcall S_enableMouseCapture(void *s, void *e, int b) {}
static void __fastcall S_setCursor(void *s, void *e, void *cursor) {}
static void __fastcall S_swapBuffers(void *s, void *e) {}
static void __fastcall S_applyChanges(void *s, void *e) {}
static int  __fastcall S_createPlat(void *s, void *e) { return 1; }
static int  __fastcall S_recreateContext(void *s, void *e) { return 1; }

static void __fastcall S_drawSetColor(void *s, void *e, int r, int g, int b, int a)
{
	draw_color[0] = r; draw_color[1] = g; draw_color[2] = b; draw_color[3] = a;
}

static void __fastcall S_drawFilledRect(void *s, void *e, int x0, int y0, int x1, int y1)
{
	if (draw_color[3] >= 255)
		return;   // fully transparent
	Offset(&x0, &y0);
	Offset(&x1, &y1);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	SetGLColor(draw_color);
	glBegin(GL_QUADS);
	glVertex2i(x0, y0); glVertex2i(x1, y0); glVertex2i(x1, y1); glVertex2i(x0, y1);
	glEnd();
	glEnable(GL_TEXTURE_2D);
}

static void __fastcall S_drawOutlinedRect(void *s, void *e, int x0, int y0, int x1, int y1)
{
	S_drawFilledRect(s, e, x0, y0, x1, y0 + 1);
	S_drawFilledRect(s, e, x0, y1 - 1, x1, y1);
	S_drawFilledRect(s, e, x0, y0 + 1, x0 + 1, y1 - 1);
	S_drawFilledRect(s, e, x1 - 1, y0 + 1, x1, y1 - 1);
}

static void __fastcall S_drawSetTextFont(void *s, void *e, void *font) { text_font = font; }

static void __fastcall S_drawSetTextColor(void *s, void *e, int r, int g, int b, int a)
{
	text_color[0] = r; text_color[1] = g; text_color[2] = b; text_color[3] = a;
}

static void __fastcall S_drawSetTextPos(void *s, void *e, int x, int y) { text_pos[0] = x; text_pos[1] = y; }

// glyphs come from vgui's own font code (GDI underneath), one small texture each
static glyph_t *GetGlyph(void *font, int ch)
{
	int id = font_getId ? font_getId(font, NULL) : (int)(intptr_t)font;
	for (int i = 0; i < glyphs_count; i++)
		if (glyphs[i].font == id && glyphs[i].ch == ch)
			return &glyphs[i];
	if (!font_getCharABCwide || !font_getCharRGBA || !font_getTall)
		return NULL;
	if (glyphs_count == glyphs_cap) {
		glyphs_cap = glyphs_cap ? glyphs_cap * 2 : 256;
		glyphs = Mem_Realloc(glyphs, sizeof(glyph_t) * glyphs_cap);
	}
	glyph_t *g = &glyphs[glyphs_count++];
	memset(g, 0, sizeof(*g));
	g->font = id;
	g->ch = ch;
	font_getCharABCwide(font, NULL, ch, &g->a, &g->b, &g->c);
	g->tall = font_getTall(font, NULL);
	if (g->b > 0 && g->tall > 0 && g->b < 512 && g->tall < 512) {
		unsigned char *rgba = Mem_Alloc((size_t)g->b * g->tall * 4);
		font_getCharRGBA(font, NULL, ch, 0, 0, g->b, g->tall, rgba);
		glGenTextures(1, &g->gl);
		glBindTexture(GL_TEXTURE_2D, g->gl);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->b, g->tall, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		Mem_Free(rgba);
	}
	return g;
}

static void __fastcall S_drawPrintText(void *s, void *e, const char *text, int len)
{
	if (!text_font || !text || text_color[3] >= 255)
		return;
	int x = text_pos[0], y = text_pos[1];
	Offset(&x, &y);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	SetGLColor(text_color);
	for (int i = 0; i < len && text[i]; i++) {
		glyph_t *g = GetGlyph(text_font, (unsigned char)text[i]);
		if (!g)
			break;
		if (g->gl) {
			int gx = x + g->a;
			glBindTexture(GL_TEXTURE_2D, g->gl);
			glBegin(GL_QUADS);
			glTexCoord2f(0, 0); glVertex2i(gx, y);
			glTexCoord2f(1, 0); glVertex2i(gx + g->b, y);
			glTexCoord2f(1, 1); glVertex2i(gx + g->b, y + g->tall);
			glTexCoord2f(0, 1); glVertex2i(gx, y + g->tall);
			glEnd();
		}
		x += g->a + g->b + g->c;
		text_pos[0] += g->a + g->b + g->c;
	}
}

static GLuint FindTexture(int id, qboolean create)
{
	for (int i = 0; i < textures_count; i++)
		if (textures[i].id == id)
			return textures[i].gl;
	if (!create)
		return 0;
	textures = Mem_Realloc(textures, sizeof(vtex_t) * (textures_count + 1));
	textures[textures_count].id = id;
	glGenTextures(1, &textures[textures_count].gl);
	return textures[textures_count++].gl;
}

static void __fastcall S_drawSetTextureRGBA(void *s, void *e, int id, const char *rgba, int w, int h)
{
	GLuint t = FindTexture(id, true);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	current_texture = id;
}

static void __fastcall S_drawSetTexture(void *s, void *e, int id) { current_texture = id; }

static void __fastcall S_drawTexturedRect(void *s, void *e, int x0, int y0, int x1, int y1)
{
	GLuint t = FindTexture(current_texture, false);
	if (!t)
		return;
	Offset(&x0, &y0);
	Offset(&x1, &y1);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, t);
	SetGLColor(draw_color);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2i(x0, y0);
	glTexCoord2f(1, 0); glVertex2i(x1, y0);
	glTexCoord2f(1, 1); glVertex2i(x1, y1);
	glTexCoord2f(0, 1); glVertex2i(x0, y1);
	glEnd();
}

// before vgui paints a panel: where it is on screen and what it may draw over
static void __fastcall S_pushMakeCurrent(void *s, void *e, void *panel, int useInsets)
{
	int inset[4] = { 0, 0, 0, 0 }, abs[4] = { 0, 0, 0, 0 }, clip[4] = { 0, 0, 0, 0 };
	if (panel_getInset)
		panel_getInset(panel, NULL, &inset[0], &inset[1], &inset[2], &inset[3]);
	if (panel_getAbsExtents)
		panel_getAbsExtents(panel, NULL, &abs[0], &abs[1], &abs[2], &abs[3]);
	if (panel_getClipRect)
		panel_getClipRect(panel, NULL, &clip[0], &clip[1], &clip[2], &clip[3]);
	if (pstack_count == pstack_cap) {
		pstack_cap = pstack_cap ? pstack_cap * 2 : 32;
		pstack = Mem_Realloc(pstack, sizeof(*pstack) * pstack_cap);
	}
	paintstate_t *p = &pstack[pstack_count++];
	p->ofs[0] = abs[0] + ((useInsets & 0xff) ? inset[0] : 0);
	p->ofs[1] = abs[1] + ((useInsets & 0xff) ? inset[1] : 0);
	memcpy(p->clip, clip, sizeof(clip));
	ApplyClip();
}

static void __fastcall S_popMakeCurrent(void *s, void *e, void *panel)
{
	if (pstack_count)
		pstack_count--;
	ApplyClip();
}

static void __fastcall App_main(void *s, void *e, int argc, char **argv) {}
static void __fastcall App_platTick(void *s, void *e) {}

// ---------------------------------------------------------------- setup and use

qboolean VGUI_Init(void)
{
	char path[MAX_OSPATH];
	snprintf(path, sizeof(path), "%s/vgui.dll", FS_BaseDir());
	vgui = LoadLibraryA(path);
	if (!vgui) {
		Con_Warnf("Couldn't load %s; the client DLL can't run without it\n", path);
		return false;
	}
	ctor_bool_t app_ctor = (ctor_bool_t)Proc("??0App@vgui@@QAE@_N@Z");
	ctor_4i_t panel_ctor = (ctor_4i_t)Proc("??0Panel@vgui@@QAE@HHHH@Z");
	ctor_ptr_t surface_ctor = (ctor_ptr_t)Proc("??0SurfaceBase@vgui@@QAE@PAVPanel@1@@Z");
	panel_getAbsExtents = (get4_t)Proc("?getAbsExtents@Panel@vgui@@UAEXAAH000@Z");
	panel_getClipRect = (get4_t)Proc("?getClipRect@Panel@vgui@@UAEXAAH000@Z");
	panel_getInset = (get4_t)Proc("?getInset@Panel@vgui@@UAEXAAH000@Z");
	panel_setBounds = (set4_t)Proc("?setBounds@Panel@vgui@@UAEXHHHH@Z");
	panel_solveTraverse = (void_t)Proc("?solveTraverse@Panel@vgui@@UAEXXZ");
	panel_paintTraverse = (void_t)Proc("?paintTraverse@Panel@vgui@@UAEXXZ");
	app_externalTick = (void_t)Proc("?externalTick@App@vgui@@UAEXXZ");
	font_getCharABCwide = (abc_t)Proc("?getCharABCwide@Font@vgui@@UAEXHAAH00@Z");
	font_getCharRGBA = (rgba_t)Proc("?getCharRGBA@Font@vgui@@UAEXHHHHHPAE@Z");
	font_getTall = (int_t)Proc("?getTall@Font@vgui@@UAEHXZ");
	font_getId = (int_t)Proc("?getId@Font@vgui@@UAEHXZ");
	if (!app_ctor || !panel_ctor || !surface_ctor || !panel_paintTraverse) {
		Con_Warnf("%s isn't the VGUI this engine knows\n", path);
		return false;
	}

	// the objects' real sizes aren't known outside vgui.dll; these are far bigger than needed
	app = Mem_Alloc(65536);
	app_ctor(app, NULL, 1);   // external main loop: the engine drives it
	memcpy(app_vtable, *(void ***)app, sizeof(void *) * 56);
	app_vtable[38] = (void *)App_main;       // pure virtual main()
	app_vtable[53] = (void *)App_platTick;   // the platform message pump belongs to the engine
	*(void ***)app = app_vtable;

	root = Mem_Alloc(65536);
	panel_ctor(root, NULL, 0, 0, vid_width, vid_height);

	surface = Mem_Alloc(65536);
	surface_ctor(surface, NULL, root);
	memcpy(surface_vtable, *(void ***)surface, sizeof(void *) * 35);
	surface_vtable[8] = (void *)S_setTitle;
	surface_vtable[9] = (void *)S_setFullscreenMode;
	surface_vtable[10] = (void *)S_setWindowedMode;
	surface_vtable[11] = (void *)S_setAsTopMost;
	surface_vtable[12] = (void *)S_createPopup;
	surface_vtable[13] = (void *)S_hasFocus;
	surface_vtable[14] = (void *)S_isWithin;
	surface_vtable[15] = (void *)S_createNewTextureID;
	surface_vtable[16] = (void *)S_GetMousePos;
	surface_vtable[18] = (void *)S_drawSetColor;
	surface_vtable[19] = (void *)S_drawFilledRect;
	surface_vtable[20] = (void *)S_drawOutlinedRect;
	surface_vtable[21] = (void *)S_drawSetTextFont;
	surface_vtable[22] = (void *)S_drawSetTextColor;
	surface_vtable[23] = (void *)S_drawSetTextPos;
	surface_vtable[24] = (void *)S_drawPrintText;
	surface_vtable[25] = (void *)S_drawSetTextureRGBA;
	surface_vtable[26] = (void *)S_drawSetTexture;
	surface_vtable[27] = (void *)S_drawTexturedRect;
	surface_vtable[28] = (void *)S_invalidate;
	surface_vtable[29] = (void *)S_enableMouseCapture;
	surface_vtable[30] = (void *)S_setCursor;
	surface_vtable[31] = (void *)S_swapBuffers;
	surface_vtable[32] = (void *)S_pushMakeCurrent;
	surface_vtable[33] = (void *)S_popMakeCurrent;
	surface_vtable[34] = (void *)S_applyChanges;
	surface_vtable[35] = (void *)S_createPlat;
	surface_vtable[36] = (void *)S_recreateContext;
	*(void ***)surface = surface_vtable;
	Con_DPrintf("VGUI started\n");
	return true;
}

void *VGUI_GetPanel(void)
{
	return root;
}

// paints every VGUI panel; call in 2D mode after the HUD
void VGUI_Paint(void)
{
	if (!root)
		return;
	static int last_w, last_h;
	if ((last_w != vid_width || last_h != vid_height) && panel_setBounds) {
		panel_setBounds(root, NULL, 0, 0, vid_width, vid_height);
		last_w = vid_width;
		last_h = vid_height;
	}
	if (app_externalTick)
		app_externalTick(app, NULL);
	if (panel_solveTraverse)
		panel_solveTraverse(root, NULL);
	pstack_count = 0;
	panel_paintTraverse(root, NULL);
	pstack_count = 0;
	glDisable(GL_SCISSOR_TEST);
	glColor4f(1, 1, 1, 1);
}
