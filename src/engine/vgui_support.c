// newen - the engine half of the VGUI1 support interface (vguiapi_t), for client libraries that carry
// their own VGUI1 (hlsdk-portable's 64-bit and Android clients link FreeVGUI in, since Valve's vgui.so is
// 32-bit only). The client exports InitVGUISupportAPI; the engine hands it a table of drawing and input
// callbacks, the client fills in its own entry points (start, paint, mouse, keys), and from then on the
// root panel, painting and VGUI's mouse go through it instead of through vgui_bridge.c.
//
// The table's layout is the public-domain vgui_api.h (Xash3D's vgui_support interface), written here
// from that header's field order. VGUI1 colors are 0..255 with alpha meaning transparency (0 = opaque).
#include "gl_local.h"
#include "bsp.h"

typedef struct { float point[2], coord[2]; } vpoint_t;
enum { MA_PRESSED, MA_RELEASED, MA_DOUBLE, MA_WHEEL };
enum { dc_user, dc_none, dc_arrow };
enum {
	KM_LSHIFT = 1 << 0, KM_RSHIFT = 1 << 1, KM_LCTRL = 1 << 2, KM_RCTRL = 1 << 3,
	KM_LALT = 1 << 4, KM_RALT = 1 << 5, KM_LSUPER = 1 << 6, KM_RSUPER = 1 << 7,
	KM_NUMLOCK = 1 << 8, KM_CAPSLOCK = 1 << 9
};

typedef struct {
	int initialized;
	// the engine's, called by the client
	void (*DrawInit)(void);
	void (*DrawShutdown)(void);
	void (*SetupDrawingText)(int *color);
	void (*SetupDrawingRect)(int *color);
	void (*SetupDrawingImage)(int *color);
	void (*BindTexture)(int id);
	void (*EnableTexture)(int enable);
	void (*Reserved0)(int id, int width, int height);
	void (*UploadTexture)(int id, const char *rgba, int width, int height);
	void (*Reserved1)(int id, int x, int y, const unsigned char *rgba, int w, int h);
	void (*DrawQuad)(const vpoint_t *ul, const vpoint_t *lr);
	void (*GetTextureSizes)(int *width, int *height);
	int  (*GenerateTexture)(void);
	void *(*EngineMalloc)(size_t size);
	void (*CursorSelect)(int cursor);
	unsigned char (*GetColor)(int i, int j);
	int  (*IsInGame)(void);
	void (*EnableTextInput)(int enable, int force);
	void (*GetCursorPos)(int *x, int *y);
	int  (*ProcessUtfChar)(int ch);
	int  (*GetClipboardText)(char *buffer, size_t size);
	void (*SetClipboardText)(const char *text);
	int  (*GetKeyModifiers)(void);
	// the client's, called by the engine
	void (*Startup)(int width, int height);
	void (*Shutdown)(void);
	void *(*GetPanel)(void);
	void (*Paint)(void);
	void (*Mouse)(int action, int code);
	void (*Key)(int action, int code);
	void (*MouseMove)(int x, int y);
	void (*TextInput)(const char *text);
	// the engine's again (added later): moves the HUD's own 2D drawing inside a VGUI panel
	void (*SetPaintOffset)(int x, int y);
} vguiapi_t;

static vguiapi_t api;
static qboolean active, cursor_visible;
static int mouse_pos[2];
static int paint_offset[2];

typedef struct { int id, w, h; GLuint gl; } stex_t;
static stex_t *textures;
static int textures_count, textures_cap, next_id = 1, bound = -1;

static stex_t *Texture(int id)
{
	for (int i = 0; i < textures_count; i++)
		if (textures[i].id == id)
			return &textures[i];
	return NULL;
}

static void SetColor(const int *c)
{
	glColor4ub((GLubyte)c[0], (GLubyte)c[1], (GLubyte)c[2], (GLubyte)(255 - c[3]));
}

static void E_DrawInit(void) {}

static void E_DrawShutdown(void)
{
	for (int i = 0; i < textures_count; i++)
		glDeleteTextures(1, &textures[i].gl);
	textures_count = 0;
}

static void E_SetupDrawingText(int *color)
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	SetColor(color);
}

static void E_SetupDrawingRect(int *color)
{
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	SetColor(color);
}

static void E_SetupDrawingImage(int *color)
{
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	SetColor(color);
}

static void E_BindTexture(int id)
{
	stex_t *t = Texture(id);
	bound = id;
	glBindTexture(GL_TEXTURE_2D, t ? t->gl : 0);
}

static void E_EnableTexture(int enable)
{
	if (enable)
		glEnable(GL_TEXTURE_2D);
	else
		glDisable(GL_TEXTURE_2D);
}

static void E_Reserved0(int id, int w, int h) {}
static void E_Reserved1(int id, int x, int y, const unsigned char *rgba, int w, int h) {}

static void E_UploadTexture(int id, const char *rgba, int w, int h)
{
	stex_t *t = Texture(id);
	if (!t) {
		if (textures_count == textures_cap) {
			textures_cap = textures_cap ? textures_cap * 2 : 64;
			textures = Mem_Realloc(textures, sizeof(stex_t) * textures_cap);
		}
		t = &textures[textures_count++];
		t->id = id;
		glGenTextures(1, &t->gl);
	}
	t->w = w;
	t->h = h;
	glBindTexture(GL_TEXTURE_2D, t->gl);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	bound = id;
}

// VGUI's quads are already where they belong; only the HUD's own drawing takes the paint offset
static void E_DrawQuad(const vpoint_t *ul, const vpoint_t *lr)
{
	glPushMatrix();
	glLoadIdentity();
	glBegin(GL_QUADS);
	glTexCoord2f(ul->coord[0], ul->coord[1]); glVertex2f(ul->point[0], ul->point[1]);
	glTexCoord2f(lr->coord[0], ul->coord[1]); glVertex2f(lr->point[0], ul->point[1]);
	glTexCoord2f(lr->coord[0], lr->coord[1]); glVertex2f(lr->point[0], lr->point[1]);
	glTexCoord2f(ul->coord[0], lr->coord[1]); glVertex2f(ul->point[0], lr->point[1]);
	glEnd();
	glPopMatrix();
}

static void E_GetTextureSizes(int *w, int *h)
{
	stex_t *t = Texture(bound);
	*w = t ? t->w : 0;
	*h = t ? t->h : 0;
}

static int E_GenerateTexture(void) { return next_id++; }
static void *E_EngineMalloc(size_t size) { return malloc(size); }   // VGUI frees it with free()

static void E_CursorSelect(int cursor)
{
	qboolean visible = cursor != dc_none;
	if (visible && !cursor_visible)
		SDL_GetMouseState(&mouse_pos[0], &mouse_pos[1]);
	if (visible != cursor_visible)
		Con_DPrintf("VGUI cursor %s\n", visible ? "shown" : "hidden");
	cursor_visible = visible;
}

// Quake's ^0-^7 text colors
static unsigned char E_GetColor(int i, int j)
{
	static const unsigned char colors[8][4] = {
		{ 0, 0, 0, 255 }, { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 255, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 0, 255, 255, 255 }, { 255, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	return colors[i & 7][j & 3];
}

static int E_IsInGame(void) { return world.loaded; }

static void E_EnableTextInput(int enable, int force)
{
	if (enable)
		SDL_StartTextInput();
	else if (force)
		SDL_StopTextInput();
}

static void E_GetCursorPos(int *x, int *y)
{
	*x = mouse_pos[0];
	*y = mouse_pos[1];
}

static int E_ProcessUtfChar(int ch) { return ch; }

static int E_GetClipboardText(char *buffer, size_t size)
{
	char *text = SDL_GetClipboardText();
	if (!text)
		return 0;
	Q_strncpy(buffer, text, size);
	SDL_free(text);
	return (int)strlen(buffer);
}

static void E_SetClipboardText(const char *text) { SDL_SetClipboardText(text); }

static int E_GetKeyModifiers(void)
{
	SDL_Keymod m = SDL_GetModState();
	int out = 0;
	if (m & KMOD_LSHIFT) out |= KM_LSHIFT;
	if (m & KMOD_RSHIFT) out |= KM_RSHIFT;
	if (m & KMOD_LCTRL)  out |= KM_LCTRL;
	if (m & KMOD_RCTRL)  out |= KM_RCTRL;
	if (m & KMOD_LALT)   out |= KM_LALT;
	if (m & KMOD_RALT)   out |= KM_RALT;
	if (m & KMOD_LGUI)   out |= KM_LSUPER;
	if (m & KMOD_RGUI)   out |= KM_RSUPER;
	if (m & KMOD_NUM)    out |= KM_NUMLOCK;
	if (m & KMOD_CAPS)   out |= KM_CAPSLOCK;
	return out;
}

static void E_SetPaintOffset(int x, int y)
{
	paint_offset[0] = x;
	paint_offset[1] = y;
	glLoadIdentity();
	glTranslatef((float)x, (float)y, 0);
}

// ---------------------------------------------------------------- the engine's side

// call right after loading the client: it must have a root panel before HUD_VidInit
qboolean VGUIS_Init(void *client_lib)
{
	void (*init)(vguiapi_t *) = (void (*)(vguiapi_t *))Sys_GetProcAddress(client_lib, "InitVGUISupportAPI");
	if (!init)
		return false;
	memset(&api, 0, sizeof(api));
	api.DrawInit = E_DrawInit;
	api.DrawShutdown = E_DrawShutdown;
	api.SetupDrawingText = E_SetupDrawingText;
	api.SetupDrawingRect = E_SetupDrawingRect;
	api.SetupDrawingImage = E_SetupDrawingImage;
	api.BindTexture = E_BindTexture;
	api.EnableTexture = E_EnableTexture;
	api.Reserved0 = E_Reserved0;
	api.UploadTexture = E_UploadTexture;
	api.Reserved1 = E_Reserved1;
	api.DrawQuad = E_DrawQuad;
	api.GetTextureSizes = E_GetTextureSizes;
	api.GenerateTexture = E_GenerateTexture;
	api.EngineMalloc = E_EngineMalloc;
	api.CursorSelect = E_CursorSelect;
	api.GetColor = E_GetColor;
	api.IsInGame = E_IsInGame;
	api.EnableTextInput = E_EnableTextInput;
	api.GetCursorPos = E_GetCursorPos;
	api.ProcessUtfChar = E_ProcessUtfChar;
	api.GetClipboardText = E_GetClipboardText;
	api.SetClipboardText = E_SetClipboardText;
	api.GetKeyModifiers = E_GetKeyModifiers;
	api.SetPaintOffset = E_SetPaintOffset;
	init(&api);
	if (!api.Startup || !api.GetPanel || !api.Paint) {
		Con_Warnf("The client's VGUI support didn't fill in its entry points\n");
		return false;
	}
	api.Startup(vid_width, vid_height);
	api.initialized = 1;
	active = true;
	Con_DPrintf("VGUI (the client's own) started\n");
	return true;
}

qboolean VGUIS_Active(void) { return active; }
void *VGUIS_GetPanel(void) { return api.GetPanel(); }
qboolean VGUIS_WantsMouse(void) { return cursor_visible; }

void VGUIS_MouseMove(int x, int y)
{
	mouse_pos[0] = x;
	mouse_pos[1] = y;
	if (api.MouseMove)
		api.MouseMove(x, y);
}

// button: 0 left, 1 right, 2 middle
void VGUIS_MouseButton(int button, qboolean down, qboolean dbl)
{
	if (api.Mouse)
		api.Mouse(!down ? MA_RELEASED : dbl ? MA_DOUBLE : MA_PRESSED, button);
}

void VGUIS_MouseWheel(int delta)
{
	if (api.Mouse)
		api.Mouse(MA_WHEEL, delta);
}

void VGUIS_TextInput(const char *text)
{
	if (api.TextInput && cursor_visible)
		api.TextInput(text);
}

void VGUIS_Paint(void)
{
	static int last_w, last_h;
	if (last_w != vid_width || last_h != vid_height) {
		api.Startup(vid_width, vid_height);   // a second call only resizes the root panel
		last_w = vid_width;
		last_h = vid_height;
	}
	glPushMatrix();
	api.Paint();
	glPopMatrix();
	paint_offset[0] = paint_offset[1] = 0;
	glEnable(GL_TEXTURE_2D);
}

void VGUIS_Shutdown(void)
{
	if (!active)
		return;
	if (api.Shutdown)
		api.Shutdown();
	E_DrawShutdown();
	active = false;
}
