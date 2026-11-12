// newen - VGUI1 bridge: hosts Half-Life's vgui.dll so the stock client.dll can build its viewport
//
// vgui.dll is C++. Its classes are created here by calling their exported constructors on memory we
// allocate, and the engine's drawing surface is a SurfaceBase whose virtual table we fill ourselves.
// The slot order comes from vgui.dll's own exported vtables (SurfaceBase has 35 virtual functions;
// slots 8-16 and 18-34 are the platform part GoldSrc's engine implements). Linux's vgui.so (GCC) has
// the same slots: no class here has a virtual destructor or overloaded virtuals, where the two
// compilers differ. Only the method names and the calling convention change.
//
// Windows methods use thiscall: "this" in ECX, arguments on the stack, callee pops; MSVC's
// __fastcall with a dummy EDX argument matches that from C. GCC passes "this" as the first stack
// argument, a plain C call. VGUI1 colors are 0..255 with alpha meaning transparency (0 = opaque).
#include "gl_local.h"

#ifdef _WIN32
#define THISCALL __fastcall
#define VTHIS     void *self, void *edx_
#define VTHIS_ARG(p) (p), NULL
#define VT_PREFIX 0           // the vtable copies start at slot 0
#define VGUI_LIB "vgui.dll"
#define SYM(msvc, gcc) msvc
#else
#define THISCALL
#define VTHIS     void *self
#define VTHIS_ARG(p) (p)
#define VT_PREFIX 2           // offset-to-top and type info sit before slot 0; copied too for RTTI
#define VGUI_LIB "vgui.so"
#define SYM(msvc, gcc) gcc
#endif

typedef void (THISCALL *ctor_bool_t)(VTHIS, int b);
typedef void (THISCALL *ctor_4i_t)(VTHIS, int x, int y, int w, int h);
typedef void (THISCALL *ctor_ptr_t)(VTHIS, void *p);
typedef void (THISCALL *void_t)(VTHIS);
typedef void (THISCALL *get4_t)(VTHIS, int *a, int *b, int *c, int *d);
typedef void (THISCALL *set4_t)(VTHIS, int a, int b, int c, int d);
typedef int  (THISCALL *int_t)(VTHIS);
typedef void (THISCALL *abc_t)(VTHIS, int ch, int *a, int *b, int *c);
typedef void (THISCALL *rgba_t)(VTHIS, int ch, int x, int y, int w, int h, unsigned char *rgba);
typedef void (THISCALL *int_ptr_t)(VTHIS, int a, void *p);
typedef void (THISCALL *int2_ptr_t)(VTHIS, int a, int b, void *p);

static void *vgui;
static unsigned char *app, *root, *surface;
static void *app_vtable_mem[VT_PREFIX + 64];
static void *surface_vtable_mem[VT_PREFIX + 40];
#define app_vtable     (app_vtable_mem + VT_PREFIX)
#define surface_vtable (surface_vtable_mem + VT_PREFIX)

static get4_t panel_getAbsExtents, panel_getClipRect, panel_getInset;
static set4_t panel_setBounds;
static void_t panel_solveTraverse, panel_paintTraverse, app_externalTick;
static abc_t  font_getCharABCwide;
static rgba_t font_getCharRGBA;
static int_t  font_getTall, font_getId;
static int_t  cursor_getDefaultCursor;
static int2_ptr_t app_cursorMoved;
static int_ptr_t  app_mousePressed, app_mouseDoublePressed, app_mouseReleased, app_mouseWheeled;
static qboolean cursor_visible;

static void *Proc(const char *name)
{
	void *p = Sys_GetProcAddress(vgui, name);
	if (!p)
		Con_DPrintf(VGUI_LIB " has no %s\n", name);
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

static void THISCALL S_setTitle(VTHIS, const char *t) {}
static int  THISCALL S_setFullscreenMode(VTHIS, int w, int h, int bpp) { return 0; }
static void THISCALL S_setWindowedMode(VTHIS) {}
static void THISCALL S_setAsTopMost(VTHIS, int b) {}
static void THISCALL S_createPopup(VTHIS, void *panel) {}
static int  THISCALL S_hasFocus(VTHIS) { return 1; }
static int  THISCALL S_isWithin(VTHIS, int x, int y) { return 1; }
static int  THISCALL S_createNewTextureID(VTHIS) { return next_texture++; }
static int mouse_pos[2];   // where the engine last told vgui the cursor is
static void THISCALL S_GetMousePos(VTHIS, int *x, int *y) { *x = mouse_pos[0]; *y = mouse_pos[1]; }
static void THISCALL S_invalidate(VTHIS, void *panel) {}
static void THISCALL S_enableMouseCapture(VTHIS, int b) {}
// vgui sets the cursor every tick; the client hides it (dc_none) unless a menu wants the mouse
static void THISCALL S_setCursor(VTHIS, void *cursor)
{
	enum { dc_user, dc_none };
	qboolean visible = cursor && cursor_getDefaultCursor && cursor_getDefaultCursor(VTHIS_ARG(cursor)) != dc_none;
	if (visible && !cursor_visible)
		SDL_GetMouseState(&mouse_pos[0], &mouse_pos[1]);   // it may not move before the first click
	if (visible != cursor_visible)
		Con_DPrintf("VGUI cursor %s\n", visible ? "shown" : "hidden");
	cursor_visible = visible;
}
static void THISCALL S_swapBuffers(VTHIS) {}
static void THISCALL S_applyChanges(VTHIS) {}
static int  THISCALL S_createPlat(VTHIS) { return 1; }
static int  THISCALL S_recreateContext(VTHIS) { return 1; }

static void THISCALL S_drawSetColor(VTHIS, int r, int g, int b, int a)
{
	draw_color[0] = r; draw_color[1] = g; draw_color[2] = b; draw_color[3] = a;
}

static void THISCALL S_drawFilledRect(VTHIS, int x0, int y0, int x1, int y1)
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

static void THISCALL S_drawOutlinedRect(VTHIS, int x0, int y0, int x1, int y1)
{
	S_drawFilledRect(VTHIS_ARG(self), x0, y0, x1, y0 + 1);
	S_drawFilledRect(VTHIS_ARG(self), x0, y1 - 1, x1, y1);
	S_drawFilledRect(VTHIS_ARG(self), x0, y0 + 1, x0 + 1, y1 - 1);
	S_drawFilledRect(VTHIS_ARG(self), x1 - 1, y0 + 1, x1, y1 - 1);
}

static void THISCALL S_drawSetTextFont(VTHIS, void *font) { text_font = font; }

static void THISCALL S_drawSetTextColor(VTHIS, int r, int g, int b, int a)
{
	text_color[0] = r; text_color[1] = g; text_color[2] = b; text_color[3] = a;
}

static void THISCALL S_drawSetTextPos(VTHIS, int x, int y) { text_pos[0] = x; text_pos[1] = y; }

// glyphs come from vgui's own font code (GDI underneath), one small texture each
static glyph_t *GetGlyph(void *font, int ch)
{
	int id = font_getId ? font_getId(VTHIS_ARG(font)) : (int)(intptr_t)font;
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
	font_getCharABCwide(VTHIS_ARG(font), ch, &g->a, &g->b, &g->c);
	g->tall = font_getTall(VTHIS_ARG(font));
	if (g->b > 0 && g->tall > 0 && g->b < 512 && g->tall < 512) {
		unsigned char *rgba = Mem_Alloc((size_t)g->b * g->tall * 4);
		font_getCharRGBA(VTHIS_ARG(font), ch, 0, 0, g->b, g->tall, rgba);
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

static void THISCALL S_drawPrintText(VTHIS, const char *text, int len)
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

static void THISCALL S_drawSetTextureRGBA(VTHIS, int id, const char *rgba, int w, int h)
{
	GLuint t = FindTexture(id, true);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	current_texture = id;
}

static void THISCALL S_drawSetTexture(VTHIS, int id) { current_texture = id; }

static void THISCALL S_drawTexturedRect(VTHIS, int x0, int y0, int x1, int y1)
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
static void THISCALL S_pushMakeCurrent(VTHIS, void *panel, int useInsets)
{
	int inset[4] = { 0, 0, 0, 0 }, abs[4] = { 0, 0, 0, 0 }, clip[4] = { 0, 0, 0, 0 };
	if (panel_getInset)
		panel_getInset(VTHIS_ARG(panel), &inset[0], &inset[1], &inset[2], &inset[3]);
	if (panel_getAbsExtents)
		panel_getAbsExtents(VTHIS_ARG(panel), &abs[0], &abs[1], &abs[2], &abs[3]);
	if (panel_getClipRect)
		panel_getClipRect(VTHIS_ARG(panel), &clip[0], &clip[1], &clip[2], &clip[3]);
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

static void THISCALL S_popMakeCurrent(VTHIS, void *panel)
{
	if (pstack_count)
		pstack_count--;
	ApplyClip();
}

static void THISCALL App_main(VTHIS, int argc, char **argv) {}
static void THISCALL App_platTick(VTHIS) {}

// ---------------------------------------------------------------- setup and use

qboolean VGUI_Init(void)
{
	char path[MAX_OSPATH];
	snprintf(path, sizeof(path), "%s/" VGUI_LIB, FS_BaseDir());
	vgui = Sys_LoadLibrary(path);
	if (!vgui) {
		Con_Warnf("Couldn't load %s; the client DLL can't run without it\n", path);
		return false;
	}
	ctor_bool_t app_ctor = (ctor_bool_t)Proc(SYM("??0App@vgui@@QAE@_N@Z",
		"_ZN4vgui3AppC1Eb"));
	ctor_4i_t panel_ctor = (ctor_4i_t)Proc(SYM("??0Panel@vgui@@QAE@HHHH@Z",
		"_ZN4vgui5PanelC1Eiiii"));
	ctor_ptr_t surface_ctor = (ctor_ptr_t)Proc(SYM("??0SurfaceBase@vgui@@QAE@PAVPanel@1@@Z",
		"_ZN4vgui11SurfaceBaseC1EPNS_5PanelE"));
	panel_getAbsExtents = (get4_t)Proc(SYM("?getAbsExtents@Panel@vgui@@UAEXAAH000@Z",
		"_ZN4vgui5Panel13getAbsExtentsERiS1_S1_S1_"));
	panel_getClipRect = (get4_t)Proc(SYM("?getClipRect@Panel@vgui@@UAEXAAH000@Z",
		"_ZN4vgui5Panel11getClipRectERiS1_S1_S1_"));
	panel_getInset = (get4_t)Proc(SYM("?getInset@Panel@vgui@@UAEXAAH000@Z",
		"_ZN4vgui5Panel8getInsetERiS1_S1_S1_"));
	panel_setBounds = (set4_t)Proc(SYM("?setBounds@Panel@vgui@@UAEXHHHH@Z",
		"_ZN4vgui5Panel9setBoundsEiiii"));
	panel_solveTraverse = (void_t)Proc(SYM("?solveTraverse@Panel@vgui@@UAEXXZ",
		"_ZN4vgui5Panel13solveTraverseEv"));
	panel_paintTraverse = (void_t)Proc(SYM("?paintTraverse@Panel@vgui@@UAEXXZ",
		"_ZN4vgui5Panel13paintTraverseEv"));
	app_externalTick = (void_t)Proc(SYM("?externalTick@App@vgui@@UAEXXZ",
		"_ZN4vgui3App12externalTickEv"));
	font_getCharABCwide = (abc_t)Proc(SYM("?getCharABCwide@Font@vgui@@UAEXHAAH00@Z",
		"_ZN4vgui4Font14getCharABCwideEiRiS1_S1_"));
	font_getCharRGBA = (rgba_t)Proc(SYM("?getCharRGBA@Font@vgui@@UAEXHHHHHPAE@Z",
		"_ZN4vgui4Font11getCharRGBAEiiiiiPh"));
	font_getTall = (int_t)Proc(SYM("?getTall@Font@vgui@@UAEHXZ",
		"_ZN4vgui4Font7getTallEv"));
	font_getId = (int_t)Proc(SYM("?getId@Font@vgui@@UAEHXZ",
		"_ZN4vgui4Font5getIdEv"));
	cursor_getDefaultCursor = (int_t)Proc(SYM("?getDefaultCursor@Cursor@vgui@@UAE?AW4DefaultCursor@12@XZ",
		"_ZN4vgui6Cursor16getDefaultCursorEv"));
	app_cursorMoved = (int2_ptr_t)Proc(SYM("?internalCursorMoved@App@vgui@@UAEXHHPAVSurfaceBase@2@@Z",
		"_ZN4vgui3App19internalCursorMovedEiiPNS_11SurfaceBaseE"));
	app_mousePressed = (int_ptr_t)Proc(SYM("?internalMousePressed@App@vgui@@UAEXW4MouseCode@2@PAVSurfaceBase@2@@Z",
		"_ZN4vgui3App20internalMousePressedENS_9MouseCodeEPNS_11SurfaceBaseE"));
	app_mouseDoublePressed = (int_ptr_t)Proc(SYM("?internalMouseDoublePressed@App@vgui@@UAEXW4MouseCode@2@PAVSurfaceBase@2@@Z",
		"_ZN4vgui3App26internalMouseDoublePressedENS_9MouseCodeEPNS_11SurfaceBaseE"));
	app_mouseReleased = (int_ptr_t)Proc(SYM("?internalMouseReleased@App@vgui@@UAEXW4MouseCode@2@PAVSurfaceBase@2@@Z",
		"_ZN4vgui3App21internalMouseReleasedENS_9MouseCodeEPNS_11SurfaceBaseE"));
	app_mouseWheeled = (int_ptr_t)Proc(SYM("?internalMouseWheeled@App@vgui@@UAEXHPAVSurfaceBase@2@@Z",
		"_ZN4vgui3App20internalMouseWheeledEiPNS_11SurfaceBaseE"));
	if (!app_ctor || !panel_ctor || !surface_ctor || !panel_paintTraverse) {
		Con_Warnf("%s isn't the VGUI this engine knows\n", path);
		return false;
	}

	// the objects' real sizes aren't known outside vgui.dll; these are far bigger than needed
	app = Mem_Alloc(65536);
	app_ctor(VTHIS_ARG(app), 1);   // external main loop: the engine drives it
	memcpy(app_vtable_mem, *(void ***)app - VT_PREFIX, sizeof(void *) * (VT_PREFIX + 56));
	app_vtable[38] = (void *)App_main;       // pure virtual main()
	app_vtable[53] = (void *)App_platTick;   // the platform message pump belongs to the engine
	*(void ***)app = app_vtable;

	root = Mem_Alloc(65536);
	panel_ctor(VTHIS_ARG(root), 0, 0, vid_width, vid_height);

	surface = Mem_Alloc(65536);
	surface_ctor(VTHIS_ARG(surface), root);
	memcpy(surface_vtable_mem, *(void ***)surface - VT_PREFIX, sizeof(void *) * (VT_PREFIX + 35));
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

// ---------------------------------------------------------------- input

// true while a VGUI menu shows the cursor: the mouse then belongs to VGUI, not to mouse look
qboolean VGUI_WantsMouse(void)
{
	return root && cursor_visible;
}

void VGUI_MouseMove(int x, int y)
{
	mouse_pos[0] = x;
	mouse_pos[1] = y;
	if (root && app_cursorMoved)
		app_cursorMoved(VTHIS_ARG(app), x, y, surface);
}

// button: 0 left, 1 right, 2 middle (vgui's MouseCode)
void VGUI_MouseButton(int button, qboolean down, qboolean dbl)
{
	if (!root)
		return;
	int_ptr_t f = !down ? app_mouseReleased : dbl ? app_mouseDoublePressed : app_mousePressed;
	if (f)
		f(VTHIS_ARG(app), button, surface);
}

void VGUI_MouseWheel(int delta)
{
	if (root && app_mouseWheeled)
		app_mouseWheeled(VTHIS_ARG(app), delta, surface);
}

// paints every VGUI panel; call in 2D mode after the HUD
void VGUI_Paint(void)
{
	if (!root)
		return;
	static int last_w, last_h;
	if ((last_w != vid_width || last_h != vid_height) && panel_setBounds) {
		panel_setBounds(VTHIS_ARG(root), 0, 0, vid_width, vid_height);
		last_w = vid_width;
		last_h = vid_height;
	}
	if (app_externalTick)
		app_externalTick(VTHIS_ARG(app));
	if (panel_solveTraverse)
		panel_solveTraverse(VTHIS_ARG(root));
	pstack_count = 0;
	panel_paintTraverse(VTHIS_ARG(root));
	pstack_count = 0;
	glDisable(GL_SCISSOR_TEST);
	glColor4f(1, 1, 1, 1);
}
