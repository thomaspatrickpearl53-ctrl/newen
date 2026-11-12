// newen - the drawing surface vgui2 paints through (ISurface, "VGUI_Surface026")
//
// Everything is drawn in 2D mode in window pixels. vgui2 colours are 0..255 with 255 opaque. Popups
// (menus, dialogs) are kept back to front; the tree under the embedded (root) panel is painted first,
// then each popup whose parents were painted (ShouldPaintChildPanel marks them), as Half-Life does.
// Fonts are rendered one glyph at a time (font_win.c / font_stb.c) into small textures.
#include "vgui2_local.h"

enum { dc_user, dc_none, dc_arrow, dc_ibeam, dc_hourglass, dc_waitarrow, dc_crosshair, dc_up, dc_sizenwse,
	dc_sizenesw, dc_sizewe, dc_sizens, dc_sizeall, dc_no, dc_hand, dc_blank, dc_last };

enum { FONTFLAG_ITALIC = 1, FONTFLAG_UNDERLINE = 2, FONTFLAG_STRIKEOUT = 4, FONTFLAG_SYMBOL = 8,
	FONTFLAG_ANTIALIAS = 16, FONTFLAG_GAUSSIANBLUR = 32, FONTFLAG_ROTARY = 64, FONTFLAG_DROPSHADOW = 128,
	FONTFLAG_ADDITIVE = 256 };

// ---------------------------------------------------------------- state

static VPANEL root;
static vcolor_t draw_color = { 255, 255, 255, 255 }, text_color = { 255, 255, 255, 255 };
static int text_pos[2];
static HFont text_font;

typedef struct { int ofs[2]; int clip[4]; } paintstate_t;
static paintstate_t *pstack;
static int pstack_count, pstack_cap;

typedef struct { GLuint gl; int w, h; char name[64]; } vtex_t;
static vtex_t *textures;   // indexed by id
static int textures_count;
static int current_texture;

typedef struct { int ch; int a, b, c; GLuint gl; } glyph_t;
typedef struct {
	fontface_t *face;
	char name[64];
	int tall, weight, blur, scanlines, flags, low, high;
	int height, ascent, shadow;
	glyph_t ascii[256];          // ch 0..255 by number
	qboolean ascii_done[256];
	glyph_t *more;               // everything else
	int more_count;
} vfont_t;
static vfont_t **fonts;          // HFont is the index, 0 unused
static int fonts_count;

static VPANEL *popups;           // back to front
static int popups_count;

static VPANEL restrict_panel, modal_panel;
static HCursor cursor = dc_arrow;
static qboolean need_mouse, need_kb, cursor_locked;
static int mouse_pos[2];
static char language[64] = "english";
static qboolean supports_esc = true;
static int prop_base[2] = { 640, 480 }, hd_prop_base[2] = { 1280, 720 };

// ---------------------------------------------------------------- calls into vgui2's IPanel

#define IPANEL(i) VFN(vg_ipanel, VT_DTORS + (i))
typedef void   (THISCALL *vp_v)(VTHIS, VPANEL);
typedef bool   (THISCALL *vp_b)(VTHIS, VPANEL);
typedef int    (THISCALL *vp_i)(VTHIS, VPANEL);
typedef VPANEL (THISCALL *vp_p)(VTHIS, VPANEL);
typedef void   (THISCALL *vp_vb)(VTHIS, VPANEL, bool);
typedef void   (THISCALL *vp_vi)(VTHIS, VPANEL, int);
typedef void   (THISCALL *vp_vii)(VTHIS, VPANEL, int, int);
typedef void   (THISCALL *vp_vpp)(VTHIS, VPANEL, int *, int *);
typedef void   (THISCALL *vp_vpppp)(VTHIS, VPANEL, int *, int *, int *, int *);

void VP_SetPos(VPANEL p, int x, int y) { ((vp_vii)IPANEL(IPANEL_SetPos))(VTHIS_ARG(vg_ipanel), p, x, y); }
void VP_SetSize(VPANEL p, int w, int h) { ((vp_vii)IPANEL(IPANEL_SetSize))(VTHIS_ARG(vg_ipanel), p, w, h); }
void VP_GetSize(VPANEL p, int *w, int *h) { ((vp_vpp)IPANEL(IPANEL_GetSize))(VTHIS_ARG(vg_ipanel), p, w, h); }
void VP_GetAbsPos(VPANEL p, int *x, int *y) { ((vp_vpp)IPANEL(IPANEL_GetAbsPos))(VTHIS_ARG(vg_ipanel), p, x, y); }
void VP_GetClipRect(VPANEL p, int *x0, int *y0, int *x1, int *y1)
{
	((vp_vpppp)IPANEL(IPANEL_GetClipRect))(VTHIS_ARG(vg_ipanel), p, x0, y0, x1, y1);
}
void VP_GetInset(VPANEL p, int *l, int *t, int *r, int *b)
{
	((vp_vpppp)IPANEL(IPANEL_GetInset))(VTHIS_ARG(vg_ipanel), p, l, t, r, b);
}
void VP_SetVisible(VPANEL p, bool on) { ((vp_vb)IPANEL(IPANEL_SetVisible))(VTHIS_ARG(vg_ipanel), p, on); }
bool VP_IsVisible(VPANEL p) { return ((vp_b)IPANEL(IPANEL_IsVisible))(VTHIS_ARG(vg_ipanel), p); }
void VP_SetParent(VPANEL p, VPANEL parent)
{
	((void (THISCALL *)(VTHIS, VPANEL, VPANEL))IPANEL(IPANEL_SetParent))(VTHIS_ARG(vg_ipanel), p, parent);
}
int VP_GetChildCount(VPANEL p) { return ((vp_i)IPANEL(IPANEL_GetChildCount))(VTHIS_ARG(vg_ipanel), p); }
VPANEL VP_GetChild(VPANEL p, int i)
{
	return ((VPANEL (THISCALL *)(VTHIS, VPANEL, int))IPANEL(IPANEL_GetChild))(VTHIS_ARG(vg_ipanel), p, i);
}
VPANEL VP_GetParent(VPANEL p) { return ((vp_p)IPANEL(IPANEL_GetParent))(VTHIS_ARG(vg_ipanel), p); }
void VP_MoveToFront(VPANEL p) { ((vp_v)IPANEL(IPANEL_MoveToFront))(VTHIS_ARG(vg_ipanel), p); }
bool VP_IsPopup(VPANEL p) { return ((vp_b)IPANEL(IPANEL_IsPopup))(VTHIS_ARG(vg_ipanel), p); }
void VP_SetPopup(VPANEL p, bool on) { ((vp_vb)IPANEL(IPANEL_SetPopup))(VTHIS_ARG(vg_ipanel), p, on); }
void VP_SetZPos(VPANEL p, int z) { ((vp_vi)IPANEL(IPANEL_SetZPos))(VTHIS_ARG(vg_ipanel), p, z); }
void VP_SetKeyBoardInputEnabled(VPANEL p, bool on)
{
	((vp_vb)IPANEL(IPANEL_SetKeyBoardInputEnabled))(VTHIS_ARG(vg_ipanel), p, on);
}
void VP_SetMouseInputEnabled(VPANEL p, bool on)
{
	((vp_vb)IPANEL(IPANEL_SetMouseInputEnabled))(VTHIS_ARG(vg_ipanel), p, on);
}
bool VP_IsKeyBoardInputEnabled(VPANEL p)
{
	return ((vp_b)IPANEL(IPANEL_IsKeyBoardInputEnabled))(VTHIS_ARG(vg_ipanel), p);
}
bool VP_IsMouseInputEnabled(VPANEL p)
{
	return ((vp_b)IPANEL(IPANEL_IsMouseInputEnabled))(VTHIS_ARG(vg_ipanel), p);
}
void VP_Solve(VPANEL p) { ((vp_v)IPANEL(IPANEL_Solve))(VTHIS_ARG(vg_ipanel), p); }
void VP_Think(VPANEL p) { ((vp_v)IPANEL(IPANEL_Think))(VTHIS_ARG(vg_ipanel), p); }
void VP_PerformApplySchemeSettings(VPANEL p)
{
	((vp_v)IPANEL(IPANEL_PerformApplySchemeSettings))(VTHIS_ARG(vg_ipanel), p);
}
void VP_PaintTraverse(VPANEL p, bool repaint, bool allowForce)
{
	((void (THISCALL *)(VTHIS, VPANEL, bool, bool))IPANEL(IPANEL_PaintTraverse))(VTHIS_ARG(vg_ipanel), p, repaint,
		allowForce);
}
VPANEL VP_IsWithinTraverse(VPANEL p, int x, int y, bool traversePopups)
{
	return ((VPANEL (THISCALL *)(VTHIS, VPANEL, int, int, bool))IPANEL(IPANEL_IsWithinTraverse))(
		VTHIS_ARG(vg_ipanel), p, x, y, traversePopups);
}
const char *VP_GetName(VPANEL p)
{
	return ((const char *(THISCALL *)(VTHIS, VPANEL))IPANEL(IPANEL_GetName))(VTHIS_ARG(vg_ipanel), p);
}
static bool VP_HasParent(VPANEL p, VPANEL parent)
{
	return ((bool (THISCALL *)(VTHIS, VPANEL, VPANEL))IPANEL(IPANEL_HasParent))(VTHIS_ARG(vg_ipanel), p, parent);
}
static bool VP_Render_GetPopupVisible(VPANEL p)
{
	return ((vp_b)IPANEL(IPANEL_Render_GetPopupVisible))(VTHIS_ARG(vg_ipanel), p);
}
static void VP_Render_SetPopupVisible(VPANEL p, bool on)
{
	((vp_vb)IPANEL(IPANEL_Render_SetPopupVisible))(VTHIS_ARG(vg_ipanel), p, on);
}

bool VP_IsFullyVisible(VPANEL p)
{
	for (; p; p = VP_GetParent(p))
		if (!VP_IsVisible(p))
			return false;
	return true;
}

// ---------------------------------------------------------------- drawing helpers

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

static void Blend(qboolean additive)
{
	glEnable(GL_BLEND);
	if (additive)
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	else
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void Quad(int x0, int y0, int x1, int y1, float s0, float t0, float s1, float t1)
{
	glBegin(GL_QUADS);
	glTexCoord2f(s0, t0); glVertex2i(x0, y0);
	glTexCoord2f(s1, t0); glVertex2i(x1, y0);
	glTexCoord2f(s1, t1); glVertex2i(x1, y1);
	glTexCoord2f(s0, t1); glVertex2i(x0, y1);
	glEnd();
}

static vtex_t *Texture(int id, qboolean create)
{
	if (id <= 0)
		return NULL;
	if (id >= textures_count) {
		if (!create)
			return NULL;
		int n = textures_count ? textures_count : 64;
		while (n <= id)
			n *= 2;
		textures = Mem_Realloc(textures, sizeof(*textures) * n);
		memset(textures + textures_count, 0, sizeof(*textures) * (n - textures_count));
		textures_count = n;
	}
	vtex_t *t = &textures[id];
	if (!t->gl && create)
		glGenTextures(1, &t->gl);
	return t->gl ? t : NULL;
}

static void Upload(vtex_t *t, const unsigned char *px, int w, int h, GLenum format, qboolean filter)
{
	glBindTexture(GL_TEXTURE_2D, t->gl);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, format, GL_UNSIGNED_BYTE, px);
	t->w = w;
	t->h = h;
}

// ---------------------------------------------------------------- fonts

static vfont_t *Font(HFont h)
{
	return h > 0 && (int)h < fonts_count ? fonts[h] : NULL;
}

// coverage -> white text with its black drop shadow, blurred or scanlined as the scheme asks
static unsigned char *GlyphRGBA(vfont_t *f, const unsigned char *alpha, int w, int h, int ow, int oh)
{
	float *cov = Mem_Alloc(sizeof(float) * ow * oh);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			cov[y * ow + x] = alpha[y * w + x] / 255.0f;
	if (f->blur > 0) {
		// a few box passes are close enough to a gaussian
		float *tmp = Mem_Alloc(sizeof(float) * ow * oh);
		int r = f->blur;
		for (int pass = 0; pass < 2; pass++) {
			for (int y = 0; y < oh; y++)
				for (int x = 0; x < ow; x++) {
					float sum = 0;
					for (int k = -r; k <= r; k++) {
						int xx = x + k;
						if (xx >= 0 && xx < ow)
							sum += cov[y * ow + xx];
					}
					tmp[y * ow + x] = sum / (2 * r + 1);
				}
			for (int y = 0; y < oh; y++)
				for (int x = 0; x < ow; x++) {
					float sum = 0;
					for (int k = -r; k <= r; k++) {
						int yy = y + k;
						if (yy >= 0 && yy < oh)
							sum += tmp[yy * ow + x];
					}
					cov[y * ow + x] = sum / (2 * r + 1);
				}
		}
		Mem_Free(tmp);
	}
	unsigned char *rgba = Mem_Alloc((size_t)ow * oh * 4);
	for (int y = 0; y < oh; y++)
		for (int x = 0; x < ow; x++) {
			float g = cov[y * ow + x];
			float s = 0;
			if (f->shadow && x >= f->shadow && y >= f->shadow)
				s = cov[(y - f->shadow) * ow + x - f->shadow];
			if (f->scanlines > 1 && (y % f->scanlines))
				g *= 0.7f;
			float a = g + s * (1 - g);
			unsigned char *o = &rgba[((size_t)y * ow + x) * 4];
			unsigned char v = a > 0 ? (unsigned char)(255 * g / a + 0.5f) : 255;
			o[0] = o[1] = o[2] = v;
			o[3] = (unsigned char)(a * 255 + 0.5f);
		}
	Mem_Free(cov);
	return rgba;
}

static glyph_t *Glyph(vfont_t *f, int ch)
{
	glyph_t *g = NULL;
	if (ch >= 0 && ch < 256) {
		g = &f->ascii[ch];
		if (f->ascii_done[ch])
			return g;
		f->ascii_done[ch] = true;
	} else {
		for (int i = 0; i < f->more_count; i++)
			if (f->more[i].ch == ch)
				return &f->more[i];
		f->more = Mem_Realloc(f->more, sizeof(*f->more) * (f->more_count + 1));
		g = &f->more[f->more_count++];
	}
	memset(g, 0, sizeof(*g));
	g->ch = ch;
	unsigned char *alpha;
	Font_Glyph(f->face, ch, &g->a, &g->b, &g->c, &alpha);
	if (alpha) {
		int w = g->b, h = Font_Height(f->face);
		int ow = w + f->shadow, oh = f->height;
		unsigned char *rgba = GlyphRGBA(f, alpha, w, h, ow, oh);
		glGenTextures(1, &g->gl);
		vtex_t tmp = { g->gl, 0, 0, "" };
		Upload(&tmp, rgba, ow, oh, GL_RGBA, false);
		Mem_Free(rgba);
		Mem_Free(alpha);
		g->b = ow;   // the shadow widens the glyph, as in GoldSrc
	}
	return g;
}

static void CharABC(vfont_t *f, int ch, int *a, int *b, int *c)
{
	glyph_t *g = Glyph(f, ch);
	*a = g->a;
	*b = g->b;
	*c = g->c;
}

// ---------------------------------------------------------------- methods

static void THISCALL S_Nop(VTHIS) {}
static void THISCALL S_NopP(VTHIS, VPANEL p) {}
static void THISCALL S_NopPB(VTHIS, VPANEL p, bool b) {}
static void THISCALL S_NopB(VTHIS, bool b) {}
static VPANEL THISCALL S_GetEmbeddedPanel(VTHIS) { return root; }
static void THISCALL S_SetEmbeddedPanel(VTHIS, VPANEL p) { root = p; }

static void THISCALL S_PushMakeCurrent(VTHIS, VPANEL panel, bool useInsets)
{
	int inset[4] = { 0 }, abs[2] = { 0 }, clip[4] = { 0 };
	VP_GetInset(panel, &inset[0], &inset[1], &inset[2], &inset[3]);
	VP_GetAbsPos(panel, &abs[0], &abs[1]);
	VP_GetClipRect(panel, &clip[0], &clip[1], &clip[2], &clip[3]);
	if (pstack_count == pstack_cap) {
		pstack_cap = pstack_cap ? pstack_cap * 2 : 32;
		pstack = Mem_Realloc(pstack, sizeof(*pstack) * pstack_cap);
	}
	paintstate_t *p = &pstack[pstack_count++];
	p->ofs[0] = abs[0] + (useInsets ? inset[0] : 0);
	p->ofs[1] = abs[1] + (useInsets ? inset[1] : 0);
	memcpy(p->clip, clip, sizeof(clip));
	ApplyClip();
}

static void THISCALL S_PopMakeCurrent(VTHIS, VPANEL panel)
{
	if (pstack_count)
		pstack_count--;
	ApplyClip();
}

static void THISCALL S_DrawSetColor(VTHIS, int r, int g, int b, int a)
{
	draw_color.r = (unsigned char)r; draw_color.g = (unsigned char)g;
	draw_color.b = (unsigned char)b; draw_color.a = (unsigned char)a;
}
static void THISCALL S_DrawSetColorC(VTHIS, vcolor_t c) { draw_color = c; }

static void THISCALL S_DrawFilledRect(VTHIS, int x0, int y0, int x1, int y1)
{
	if (!draw_color.a)
		return;
	Offset(&x0, &y0);
	Offset(&x1, &y1);
	glDisable(GL_TEXTURE_2D);
	Blend(false);
	glColor4ub(draw_color.r, draw_color.g, draw_color.b, draw_color.a);
	Quad(x0, y0, x1, y1, 0, 0, 0, 0);
	glEnable(GL_TEXTURE_2D);
}

static void THISCALL S_DrawOutlinedRect(VTHIS, int x0, int y0, int x1, int y1)
{
	S_DrawFilledRect(VTHIS_ARG(self), x0, y0, x1, y0 + 1);
	S_DrawFilledRect(VTHIS_ARG(self), x0, y1 - 1, x1, y1);
	S_DrawFilledRect(VTHIS_ARG(self), x0, y0 + 1, x0 + 1, y1 - 1);
	S_DrawFilledRect(VTHIS_ARG(self), x1 - 1, y0 + 1, x1, y1 - 1);
}

static void THISCALL S_DrawLine(VTHIS, int x0, int y0, int x1, int y1)
{
	Offset(&x0, &y0);
	Offset(&x1, &y1);
	glDisable(GL_TEXTURE_2D);
	Blend(false);
	glColor4ub(draw_color.r, draw_color.g, draw_color.b, draw_color.a);
	glBegin(GL_LINES);
	glVertex2f(x0 + 0.5f, y0 + 0.5f);
	glVertex2f(x1 + 0.5f, y1 + 0.5f);
	glEnd();
	glEnable(GL_TEXTURE_2D);
}

static void THISCALL S_DrawPolyLine(VTHIS, int *px, int *py, int n)
{
	for (int i = 0; i + 1 < n; i++)
		S_DrawLine(VTHIS_ARG(self), px[i], py[i], px[i + 1], py[i + 1]);
}

static void THISCALL S_DrawSetTextFont(VTHIS, HFont f) { text_font = f; }
static void THISCALL S_DrawSetTextColor(VTHIS, int r, int g, int b, int a)
{
	text_color.r = (unsigned char)r; text_color.g = (unsigned char)g;
	text_color.b = (unsigned char)b; text_color.a = (unsigned char)a;
}
static void THISCALL S_DrawSetTextColorC(VTHIS, vcolor_t c) { text_color = c; }
static void THISCALL S_DrawSetTextPos(VTHIS, int x, int y) { text_pos[0] = x; text_pos[1] = y; }
static void THISCALL S_DrawGetTextPos(VTHIS, int *x, int *y) { *x = text_pos[0]; *y = text_pos[1]; }

static void DrawChar(wchar_t ch, qboolean additive)
{
	vfont_t *f = Font(text_font);
	if (!f)
		return;
	glyph_t *g = Glyph(f, (int)ch);
	if (g->gl && text_color.a) {
		int x = text_pos[0] + g->a, y = text_pos[1];
		Offset(&x, &y);
		glEnable(GL_TEXTURE_2D);
		Blend(additive || (f->flags & FONTFLAG_ADDITIVE));
		glBindTexture(GL_TEXTURE_2D, g->gl);
		glColor4ub(text_color.r, text_color.g, text_color.b, text_color.a);
		Quad(x, y, x + g->b, y + f->height, 0, 0, 1, 1);
	}
	text_pos[0] += g->a + g->b + g->c;
}

static void THISCALL S_DrawPrintText(VTHIS, const wchar_t *text, int len)
{
	for (int i = 0; text && i < len && text[i]; i++)
		DrawChar(text[i], false);
}
static void THISCALL S_DrawUnicodeChar(VTHIS, wchar_t ch) { DrawChar(ch, false); }
static void THISCALL S_DrawUnicodeCharAdd(VTHIS, wchar_t ch) { DrawChar(ch, true); }

static void *THISCALL S_CreateHTMLWindow(VTHIS, void *events, VPANEL context) { return NULL; }
static void THISCALL S_HTMLWindow(VTHIS, void *html) {}

// texture files are named without an extension; GoldSrc's are TGAs
static void THISCALL S_DrawSetTextureFile(VTHIS, int id, const char *name, int filter, bool forceReload)
{
	vtex_t *t = Texture(id, true);
	current_texture = id;
	if (!t || (!forceReload && !stricmp(t->name, name) && t->w))
		return;
	Q_strncpy(t->name, name, sizeof(t->name));
	int w = 0, h = 0;
	unsigned char *px = NULL;
	const char *exts[] = { ".tga", ".bmp", ".png", "" };
	for (int i = 0; i < 4 && !px; i++) {
		const char *path = va("%s%s", name, exts[i]);
		if (FS_FileExists(path))
			px = Image_LoadRGBA(path, &w, &h);
	}
	if (!px) {
		Con_DPrintf("VGUI2: no texture %s\n", name);
		unsigned char white[4] = { 255, 255, 255, 255 };
		Upload(t, white, 1, 1, GL_RGBA, false);
		t->w = t->h = 0;   // reported as missing
		return;
	}
	Upload(t, px, w, h, GL_RGBA, filter != 0);
	free(px);   // stb_image's
	Con_DPrintf("VGUI2 texture %d: %s (%dx%d)\n", id, name, w, h);
}

static void THISCALL S_DrawSetTextureRGBA(VTHIS, int id, const unsigned char *rgba, int w, int h, int filter,
	bool forceReload)
{
	vtex_t *t = Texture(id, true);
	current_texture = id;
	if (t && rgba && w > 0 && h > 0)
		Upload(t, rgba, w, h, GL_RGBA, filter != 0);
}

static void THISCALL S_DrawSetTextureBGRA(VTHIS, int id, const unsigned char *bgra, int w, int h)
{
	vtex_t *t = Texture(id, true);
	current_texture = id;
	if (t && bgra && w > 0 && h > 0)
		Upload(t, bgra, w, h, GL_BGRA, true);
}

static void THISCALL S_DrawUpdateRegionTextureBGRA(VTHIS, int id, int x, int y, const unsigned char *bgra, int w,
	int h)
{
	vtex_t *t = Texture(id, false);
	if (!t || !bgra)
		return;
	glBindTexture(GL_TEXTURE_2D, t->gl);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
}

static void THISCALL S_DrawSetTexture(VTHIS, int id) { current_texture = id; }

static void THISCALL S_DrawGetTextureSize(VTHIS, int id, int *w, int *h)
{
	vtex_t *t = Texture(id, false);
	*w = t ? t->w : 0;
	*h = t ? t->h : 0;
}

static void TexturedRect(int x0, int y0, int x1, int y1, qboolean additive)
{
	vtex_t *t = Texture(current_texture, false);
	if (!t || !draw_color.a)
		return;
	Offset(&x0, &y0);
	Offset(&x1, &y1);
	glEnable(GL_TEXTURE_2D);
	Blend(additive);
	glBindTexture(GL_TEXTURE_2D, t->gl);
	glColor4ub(draw_color.r, draw_color.g, draw_color.b, draw_color.a);
	Quad(x0, y0, x1, y1, 0, 0, 1, 1);
}

static void THISCALL S_DrawTexturedRect(VTHIS, int x0, int y0, int x1, int y1) { TexturedRect(x0, y0, x1, y1, false); }
static void THISCALL S_DrawTexturedRectAdd(VTHIS, int x0, int y0, int x1, int y1) { TexturedRect(x0, y0, x1, y1, true); }

typedef struct { int x, y; float u, v; } vguivertex_t;
static void THISCALL S_DrawTexturedPolygon(VTHIS, vguivertex_t *v, int n)
{
	vtex_t *t = Texture(current_texture, false);
	if (!t || n < 3)
		return;
	glEnable(GL_TEXTURE_2D);
	Blend(false);
	glBindTexture(GL_TEXTURE_2D, t->gl);
	glColor4ub(draw_color.r, draw_color.g, draw_color.b, draw_color.a);
	glBegin(GL_POLYGON);
	for (int i = 0; i < n; i++) {
		int x = v[i].x, y = v[i].y;
		Offset(&x, &y);
		glTexCoord2f(v[i].u, v[i].v);
		glVertex2i(x, y);
	}
	glEnd();
}

static int next_texture = 1;
static bool THISCALL S_IsTextureIDValid(VTHIS, int id) { return id > 0 && id < next_texture; }
static int THISCALL S_CreateNewTextureID(VTHIS, bool procedural) { return next_texture++; }

static bool THISCALL S_DeleteTextureByID(VTHIS, int id)
{
	if (id <= 0 || id >= textures_count)
		return false;
	if (textures[id].gl)
		glDeleteTextures(1, &textures[id].gl);
	memset(&textures[id], 0, sizeof(textures[id]));
	return true;
}

static void THISCALL S_GetScreenSize(VTHIS, int *w, int *h) { *w = vid_width; *h = vid_height; }

// ---------------------------------------------------------------- popups

static int PopupIndex(VPANEL p)
{
	for (int i = 0; i < popups_count; i++)
		if (popups[i] == p)
			return i;
	return -1;
}

static void MovePopup(VPANEL p, qboolean front)
{
	int i = PopupIndex(p);
	if (i < 0)
		return;
	memmove(&popups[i], &popups[i + 1], sizeof(VPANEL) * (popups_count - i - 1));
	if (front) {
		popups[popups_count - 1] = p;
	} else {
		memmove(&popups[1], &popups[0], sizeof(VPANEL) * (popups_count - 1));
		popups[0] = p;
	}
}

static void THISCALL S_MovePopupToFront(VTHIS, VPANEL p) { MovePopup(p, true); }
static void THISCALL S_MovePopupToBack(VTHIS, VPANEL p) { MovePopup(p, false); }

static void THISCALL S_BringToFront(VTHIS, VPANEL p)
{
	VP_MoveToFront(p);
	if (VP_IsPopup(p))
		MovePopup(p, true);
}

static void THISCALL S_CreatePopup(VTHIS, VPANEL p, bool minimized, bool taskbar, bool disabled, bool mouse, bool kb)
{
	if (!VP_GetParent(p))
		VP_SetParent(p, root);
	VP_SetPopup(p, true);
	VP_SetKeyBoardInputEnabled(p, kb);
	VP_SetMouseInputEnabled(p, mouse);
	if (PopupIndex(p) < 0) {
		popups = Mem_Realloc(popups, sizeof(VPANEL) * (popups_count + 1));
		popups[popups_count++] = p;
	}
	MovePopup(p, true);
}

static void THISCALL S_ReleasePanel(VTHIS, VPANEL p)
{
	int i = PopupIndex(p);
	if (i >= 0) {
		memmove(&popups[i], &popups[i + 1], sizeof(VPANEL) * (popups_count - i - 1));
		popups_count--;
	}
	if (restrict_panel == p)
		restrict_panel = 0;
	if (modal_panel == p)
		modal_panel = 0;
}

static VPANEL THISCALL S_GetTopmostPopup(VTHIS) { return popups_count ? popups[popups_count - 1] : 0; }
static int THISCALL S_GetPopupCount(VTHIS) { return popups_count; }
static VPANEL THISCALL S_GetPopup(VTHIS, int i) { return i >= 0 && i < popups_count ? popups[i] : 0; }

// the popup that holds a panel comes to the front when it gets focus
static void THISCALL S_SetTopLevelFocus(VTHIS, VPANEL p)
{
	for (; p; p = VP_GetParent(p))
		if (VP_IsPopup(p)) {
			S_BringToFront(VTHIS_ARG(self), p);
			return;
		}
}

static bool THISCALL S_IsMinimized(VTHIS, VPANEL p) { return false; }
static void THISCALL S_SetTitle(VTHIS, VPANEL p, const wchar_t *t) {}
static void THISCALL S_SetCursor(VTHIS, HCursor c) { cursor = c; }
static bool THISCALL S_IsCursorVisible(VTHIS) { return cursor != dc_none && cursor != dc_blank; }
static bool THISCALL S_IsWithin(VTHIS, int x, int y) { return x >= 0 && y >= 0 && x < vid_width && y < vid_height; }
static bool THISCALL S_HasFocus(VTHIS) { return Vid_HasFocus(); }

// 1 antialiased fonts, 2 drop shadows, 3 the escape key closes dialogs (as in GoldSrc)
static bool THISCALL S_SupportsFeature(VTHIS, int feature)
{
	return feature == 1 || feature == 2 || (feature == 3 && supports_esc);
}

static void THISCALL S_RestrictPaintToSinglePanel(VTHIS, VPANEL p) { restrict_panel = p; }
static void THISCALL S_SetModalPanel(VTHIS, VPANEL p) { modal_panel = p; }
static VPANEL THISCALL S_GetModalPanel(VTHIS) { return modal_panel; }
static void THISCALL S_UnlockCursor(VTHIS) { cursor_locked = false; }
static void THISCALL S_LockCursor(VTHIS) { cursor_locked = true; }

// ---------------------------------------------------------------- font methods

static HFont THISCALL S_CreateFont(VTHIS)
{
	if (!fonts_count) {
		fonts = Mem_Alloc(sizeof(*fonts));
		fonts_count = 1;   // 0 is "no font"
	}
	fonts = Mem_Realloc(fonts, sizeof(*fonts) * (fonts_count + 1));
	fonts[fonts_count] = Mem_Alloc(sizeof(vfont_t));
	return (HFont)fonts_count++;
}

static void ClearFont(vfont_t *f)
{
	for (int i = 0; i < 256; i++)
		if (f->ascii[i].gl)
			glDeleteTextures(1, &f->ascii[i].gl);
	for (int i = 0; i < f->more_count; i++)
		if (f->more[i].gl)
			glDeleteTextures(1, &f->more[i].gl);
	Mem_Free(f->more);
	Font_Close(f->face);
	memset(f, 0, sizeof(*f));
}

static bool THISCALL S_AddGlyphSetToFont(VTHIS, HFont h, const char *name, int tall, int weight, int blur,
	int scanlines, int flags, int low, int high)
{
	vfont_t *f = Font(h);
	if (!f || !name)
		return false;
	if (f->face)
		return true;   // one face per font; later glyph sets (other ranges) share it
	ClearFont(f);
	f->face = Font_Open(name, tall, weight, (flags & FONTFLAG_ITALIC) != 0, (flags & FONTFLAG_ANTIALIAS) != 0);
	if (!f->face)
		return false;
	Q_strncpy(f->name, name, sizeof(f->name));
	f->tall = tall;
	f->weight = weight;
	f->blur = blur;
	f->scanlines = scanlines;
	f->flags = flags;
	f->low = low;
	f->high = high;
	f->shadow = (flags & FONTFLAG_DROPSHADOW) ? 1 : 0;
	f->height = Font_Height(f->face) + f->shadow;
	f->ascent = Font_Ascent(f->face);
	return true;
}

static bool THISCALL S_AddCustomFontFile(VTHIS, const char *name)
{
	char path[MAX_OSPATH];
	if (!FS_FullPath(name, path, sizeof(path)))
		return false;
	Font_AddFile(path);
	return true;
}

static int THISCALL S_GetFontTall(VTHIS, HFont h)
{
	vfont_t *f = Font(h);
	return f ? f->height : 0;
}

static void THISCALL S_GetCharABCwide(VTHIS, HFont h, int ch, int *a, int *b, int *c)
{
	vfont_t *f = Font(h);
	*a = *b = *c = 0;
	if (f && f->face)
		CharABC(f, ch, a, b, c);
}

static int THISCALL S_GetCharacterWidth(VTHIS, HFont h, int ch)
{
	int a, b, c;
	S_GetCharABCwide(VTHIS_ARG(self), h, ch, &a, &b, &c);
	return a + b + c;
}

static void THISCALL S_GetTextSize(VTHIS, HFont h, const wchar_t *text, int *w, int *tall)
{
	vfont_t *f = Font(h);
	*w = *tall = 0;
	if (!f || !text)
		return;
	int line = 0;
	*tall = f->height;
	for (; *text; text++) {
		if (*text == '\n') {
			*tall += f->height;
			line = 0;
			continue;
		}
		int a, b, c;
		CharABC(f, *text, &a, &b, &c);
		line += a + b + c;
		if (line > *w)
			*w = line;
	}
}

static int THISCALL S_GetFontAscent(VTHIS, HFont h, wchar_t ch)
{
	vfont_t *f = Font(h);
	return f ? f->ascent : 0;
}

static int THISCALL S_GetFontBlur(VTHIS, HFont h)
{
	vfont_t *f = Font(h);
	return f ? f->blur : 0;
}

static bool THISCALL S_IsAdditive(VTHIS, HFont h)
{
	vfont_t *f = Font(h);
	return f && (f->flags & FONTFLAG_ADDITIVE);
}

// ---------------------------------------------------------------- the rest

static VPANEL THISCALL S_GetNotifyPanel(VTHIS) { return 0; }
static void THISCALL S_SetNotifyIcon(VTHIS, VPANEL context, unsigned long icon, VPANEL panel, const char *text) {}

static void THISCALL S_PlaySound(VTHIS, const char *name)
{
	if (name && *name)
		S_LocalSound(name, 1);
}

// popups aren't painted as children: they come after the tree, in their own order
static bool THISCALL S_ShouldPaintChildPanel(VTHIS, VPANEL child)
{
	if (restrict_panel && !VP_HasParent(child, restrict_panel))
		return false;
	if (VP_IsPopup(child) && PopupIndex(child) >= 0) {
		VP_Render_SetPopupVisible(child, true);
		return false;
	}
	return true;
}

static bool THISCALL S_RecreateContext(VTHIS, VPANEL p) { return true; }

static void SchemeTraverse(VPANEL p)
{
	int n = VP_GetChildCount(p);
	for (int i = 0; i < n; i++)
		SchemeTraverse(VP_GetChild(p, i));
	VP_PerformApplySchemeSettings(p);
}

static void ThinkTraverse(VPANEL p)
{
	if (!VP_IsVisible(p))
		return;
	VP_Think(p);
	int n = VP_GetChildCount(p);
	for (int i = 0; i < n; i++)
		ThinkTraverse(VP_GetChild(p, i));
}

static void SolveTraverse(VPANEL p)
{
	VP_Solve(p);
	int n = VP_GetChildCount(p);
	for (int i = 0; i < n; i++)
		SolveTraverse(VP_GetChild(p, i));
}

static void THISCALL S_SolveTraverse(VTHIS, VPANEL p, bool force)
{
	SchemeTraverse(p);
	ThinkTraverse(p);
	SolveTraverse(p);
}

static void THISCALL S_PaintTraverse(VTHIS, VPANEL p)
{
	if (!VP_IsVisible(p))
		return;
	if (p != root) {
		VP_PaintTraverse(p, true, true);
		return;
	}
	VPANEL top = restrict_panel ? restrict_panel : p;
	for (int i = 0; i < popups_count; i++)
		VP_Render_SetPopupVisible(popups[i], false);
	VP_PaintTraverse(top, true, true);
	// the list can change while painting
	for (int i = 0; i < popups_count; i++) {
		VPANEL pop = popups[i];
		if (VP_Render_GetPopupVisible(pop) && VP_IsVisible(pop))
			VP_PaintTraverse(pop, true, true);
	}
}

static void THISCALL S_GetBounds(VTHIS, int *x, int *y, int *w, int *h)
{
	*x = *y = 0;
	*w = vid_width;
	*h = vid_height;
}

static void THISCALL S_GetProportionalBase(VTHIS, int *w, int *h)
{
	*w = vid_width < prop_base[0] ? vid_width : prop_base[0];
	*h = vid_height < prop_base[1] ? vid_height : prop_base[1];
}

static void THISCALL S_SetProportionalBase(VTHIS, int w, int h) { prop_base[0] = w; prop_base[1] = h; }

static void THISCALL S_GetHDProportionalBase(VTHIS, int *w, int *h)
{
	*w = vid_width < hd_prop_base[0] ? vid_width : hd_prop_base[0];
	*h = vid_height < hd_prop_base[1] ? vid_height : hd_prop_base[1];
}

static void THISCALL S_SetHDProportionalBase(VTHIS, int w, int h) { hd_prop_base[0] = w; hd_prop_base[1] = h; }

// the mouse belongs to vgui while a popup that takes it is showing
static void THISCALL S_CalculateMouseVisible(VTHIS)
{
	need_mouse = need_kb = false;
	for (int i = 0; i < popups_count; i++) {
		VPANEL p = popups[i];
		if (!VP_IsFullyVisible(p))
			continue;
		need_mouse |= VP_IsMouseInputEnabled(p);
		need_kb |= VP_IsKeyBoardInputEnabled(p);
	}
	if (need_mouse) {
		S_SetCursor(VTHIS_ARG(self), dc_arrow);
		cursor_locked = false;
	} else {
		S_SetCursor(VTHIS_ARG(self), dc_none);
		cursor_locked = true;
	}
}

static bool THISCALL S_NeedKBInput(VTHIS) { return need_kb; }
static bool THISCALL S_HasCursorPosFunctions(VTHIS) { return true; }
static void THISCALL S_SurfaceGetCursorPos(VTHIS, int *x, int *y) { *x = mouse_pos[0]; *y = mouse_pos[1]; }

static void THISCALL S_SurfaceSetCursorPos(VTHIS, int x, int y)
{
	mouse_pos[0] = x;
	mouse_pos[1] = y;
	SDL_Window *w = SDL_GL_GetCurrentWindow();
	if (w)
		SDL_WarpMouseInWindow(w, x, y);
}

static void THISCALL S_SetLanguage(VTHIS, const char *l) { Q_strncpy(language, l, sizeof(language)); }
static const char *THISCALL S_GetLanguage(VTHIS) { return language; }
static void THISCALL S_CreateBrowser(VTHIS, VPANEL p, void *responses, bool popup, const char *agent) {}
static void THISCALL S_RemoveBrowser(VTHIS, VPANEL p, void *responses) {}
static void *THISCALL S_AccessChromeHTMLController(VTHIS) { return NULL; }
static void THISCALL S_SetSupportsEsc(VTHIS, bool on) { supports_esc = on; }

// ---------------------------------------------------------------- the object

enum {
	SF_Shutdown, SF_RunFrame, SF_GetEmbeddedPanel, SF_SetEmbeddedPanel, SF_PushMakeCurrent, SF_PopMakeCurrent,
#ifdef _WIN32
	SF_DrawSetColorC, SF_DrawSetColor,   // overloads: reversed by MSVC
#else
	SF_DrawSetColor, SF_DrawSetColorC,
#endif
	SF_DrawFilledRect, SF_DrawOutlinedRect, SF_DrawLine, SF_DrawPolyLine, SF_DrawSetTextFont,
#ifdef _WIN32
	SF_DrawSetTextColorC, SF_DrawSetTextColor,
#else
	SF_DrawSetTextColor, SF_DrawSetTextColorC,
#endif
	SF_DrawSetTextPos, SF_DrawGetTextPos, SF_DrawPrintText, SF_DrawUnicodeChar, SF_DrawUnicodeCharAdd,
	SF_DrawFlushText, SF_CreateHTMLWindow, SF_PaintHTMLWindow, SF_DeleteHTMLWindow, SF_DrawSetTextureFile,
	SF_DrawSetTextureRGBA, SF_DrawSetTexture, SF_DrawGetTextureSize, SF_DrawTexturedRect, SF_IsTextureIDValid,
	SF_CreateNewTextureID, SF_GetScreenSize, SF_SetAsTopMost, SF_BringToFront, SF_SetForegroundWindow,
	SF_SetPanelVisible, SF_SetMinimized, SF_IsMinimized, SF_FlashWindow, SF_SetTitle, SF_SetAsToolBar,
	SF_CreatePopup, SF_SwapBuffers, SF_Invalidate, SF_SetCursor, SF_IsCursorVisible, SF_ApplyChanges, SF_IsWithin,
	SF_HasFocus, SF_SupportsFeature, SF_RestrictPaintToSinglePanel, SF_SetModalPanel, SF_GetModalPanel,
	SF_UnlockCursor, SF_LockCursor, SF_SetTranslateExtendedKeys, SF_GetTopmostPopup, SF_SetTopLevelFocus,
	SF_CreateFont, SF_AddGlyphSetToFont, SF_AddCustomFontFile, SF_GetFontTall, SF_GetCharABCwide,
	SF_GetCharacterWidth, SF_GetTextSize, SF_GetNotifyPanel, SF_SetNotifyIcon, SF_PlaySound, SF_GetPopupCount,
	SF_GetPopup, SF_ShouldPaintChildPanel, SF_RecreateContext, SF_AddPanel, SF_ReleasePanel, SF_MovePopupToFront,
	SF_MovePopupToBack, SF_SolveTraverse, SF_PaintTraverse, SF_EnableMouseCapture, SF_GetWorkspaceBounds,
	SF_GetAbsoluteWindowBounds, SF_GetProportionalBase, SF_CalculateMouseVisible, SF_NeedKBInput,
	SF_HasCursorPosFunctions, SF_SurfaceGetCursorPos, SF_SurfaceSetCursorPos, SF_DrawTexturedPolygon,
	SF_GetFontAscent, SF_SetAllowHTMLJavaScript, SF_SetLanguage, SF_GetLanguage, SF_DeleteTextureByID,
	SF_DrawUpdateRegionTextureBGRA, SF_DrawSetTextureBGRA, SF_CreateBrowser, SF_RemoveBrowser,
	SF_AccessChromeHTMLController, SF_DrawTexturedRectAdd, SF_SetSupportsEsc, SF_GetFontBlur, SF_IsAdditive,
	SF_SetProportionalBase, SF_GetHDProportionalBase, SF_SetHDProportionalBase, SF_COUNT
};

static void *surface_vtable[VT_PREFIX + VT_DTORS + SF_COUNT];
static cppobj_t surface_object;

void *VGS_Create(void)
{
	void **vt = surface_vtable;
	VG_InitVtable(vt, SF_COUNT);
#define M(name, fn) VG_METHOD(vt, SF_##name) = (void *)(fn)
	M(Shutdown, S_Nop);
	M(RunFrame, S_Nop);
	M(GetEmbeddedPanel, S_GetEmbeddedPanel);
	M(SetEmbeddedPanel, S_SetEmbeddedPanel);
	M(PushMakeCurrent, S_PushMakeCurrent);
	M(PopMakeCurrent, S_PopMakeCurrent);
	M(DrawSetColor, S_DrawSetColor);
	M(DrawSetColorC, S_DrawSetColorC);
	M(DrawFilledRect, S_DrawFilledRect);
	M(DrawOutlinedRect, S_DrawOutlinedRect);
	M(DrawLine, S_DrawLine);
	M(DrawPolyLine, S_DrawPolyLine);
	M(DrawSetTextFont, S_DrawSetTextFont);
	M(DrawSetTextColor, S_DrawSetTextColor);
	M(DrawSetTextColorC, S_DrawSetTextColorC);
	M(DrawSetTextPos, S_DrawSetTextPos);
	M(DrawGetTextPos, S_DrawGetTextPos);
	M(DrawPrintText, S_DrawPrintText);
	M(DrawUnicodeChar, S_DrawUnicodeChar);
	M(DrawUnicodeCharAdd, S_DrawUnicodeCharAdd);
	M(DrawFlushText, S_Nop);
	M(CreateHTMLWindow, S_CreateHTMLWindow);
	M(PaintHTMLWindow, S_HTMLWindow);
	M(DeleteHTMLWindow, S_HTMLWindow);
	M(DrawSetTextureFile, S_DrawSetTextureFile);
	M(DrawSetTextureRGBA, S_DrawSetTextureRGBA);
	M(DrawSetTexture, S_DrawSetTexture);
	M(DrawGetTextureSize, S_DrawGetTextureSize);
	M(DrawTexturedRect, S_DrawTexturedRect);
	M(IsTextureIDValid, S_IsTextureIDValid);
	M(CreateNewTextureID, S_CreateNewTextureID);
	M(GetScreenSize, S_GetScreenSize);
	M(SetAsTopMost, S_NopPB);
	M(BringToFront, S_BringToFront);
	M(SetForegroundWindow, S_BringToFront);
	M(SetPanelVisible, S_NopPB);
	M(SetMinimized, S_NopPB);
	M(IsMinimized, S_IsMinimized);
	M(FlashWindow, S_NopPB);
	M(SetTitle, S_SetTitle);
	M(SetAsToolBar, S_NopPB);
	M(CreatePopup, S_CreatePopup);
	M(SwapBuffers, S_NopP);
	M(Invalidate, S_NopP);
	M(SetCursor, S_SetCursor);
	M(IsCursorVisible, S_IsCursorVisible);
	M(ApplyChanges, S_Nop);
	M(IsWithin, S_IsWithin);
	M(HasFocus, S_HasFocus);
	M(SupportsFeature, S_SupportsFeature);
	M(RestrictPaintToSinglePanel, S_RestrictPaintToSinglePanel);
	M(SetModalPanel, S_SetModalPanel);
	M(GetModalPanel, S_GetModalPanel);
	M(UnlockCursor, S_UnlockCursor);
	M(LockCursor, S_LockCursor);
	M(SetTranslateExtendedKeys, S_NopB);
	M(GetTopmostPopup, S_GetTopmostPopup);
	M(SetTopLevelFocus, S_SetTopLevelFocus);
	M(CreateFont, S_CreateFont);
	M(AddGlyphSetToFont, S_AddGlyphSetToFont);
	M(AddCustomFontFile, S_AddCustomFontFile);
	M(GetFontTall, S_GetFontTall);
	M(GetCharABCwide, S_GetCharABCwide);
	M(GetCharacterWidth, S_GetCharacterWidth);
	M(GetTextSize, S_GetTextSize);
	M(GetNotifyPanel, S_GetNotifyPanel);
	M(SetNotifyIcon, S_SetNotifyIcon);
	M(PlaySound, S_PlaySound);
	M(GetPopupCount, S_GetPopupCount);
	M(GetPopup, S_GetPopup);
	M(ShouldPaintChildPanel, S_ShouldPaintChildPanel);
	M(RecreateContext, S_RecreateContext);
	M(AddPanel, S_NopP);
	M(ReleasePanel, S_ReleasePanel);
	M(MovePopupToFront, S_MovePopupToFront);
	M(MovePopupToBack, S_MovePopupToBack);
	M(SolveTraverse, S_SolveTraverse);
	M(PaintTraverse, S_PaintTraverse);
	M(EnableMouseCapture, S_NopPB);
	M(GetWorkspaceBounds, S_GetBounds);
	M(GetAbsoluteWindowBounds, S_GetBounds);
	M(GetProportionalBase, S_GetProportionalBase);
	M(CalculateMouseVisible, S_CalculateMouseVisible);
	M(NeedKBInput, S_NeedKBInput);
	M(HasCursorPosFunctions, S_HasCursorPosFunctions);
	M(SurfaceGetCursorPos, S_SurfaceGetCursorPos);
	M(SurfaceSetCursorPos, S_SurfaceSetCursorPos);
	M(DrawTexturedPolygon, S_DrawTexturedPolygon);
	M(GetFontAscent, S_GetFontAscent);
	M(SetAllowHTMLJavaScript, S_NopB);
	M(SetLanguage, S_SetLanguage);
	M(GetLanguage, S_GetLanguage);
	M(DeleteTextureByID, S_DeleteTextureByID);
	M(DrawUpdateRegionTextureBGRA, S_DrawUpdateRegionTextureBGRA);
	M(DrawSetTextureBGRA, S_DrawSetTextureBGRA);
	M(CreateBrowser, S_CreateBrowser);
	M(RemoveBrowser, S_RemoveBrowser);
	M(AccessChromeHTMLController, S_AccessChromeHTMLController);
	M(DrawTexturedRectAdd, S_DrawTexturedRectAdd);
	M(SetSupportsEsc, S_SetSupportsEsc);
	M(GetFontBlur, S_GetFontBlur);
	M(IsAdditive, S_IsAdditive);
	M(SetProportionalBase, S_SetProportionalBase);
	M(GetHDProportionalBase, S_GetHDProportionalBase);
	M(SetHDProportionalBase, S_SetHDProportionalBase);
#undef M
	surface_object.vtable = vt + VT_PREFIX;
	return &surface_object;
}

// ---------------------------------------------------------------- the engine's side

void VGS_SetEmbeddedPanel(VPANEL p) { root = p; }
qboolean VGS_ShouldPaintChild(VPANEL child) { return S_ShouldPaintChildPanel(VTHIS_ARG(&surface_object), child); }
VPANEL VGS_GetEmbeddedPanel(void) { return root; }

void VGS_Paint(void)
{
	if (!root)
		return;
	pstack_count = 0;
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	S_SolveTraverse(VTHIS_ARG(&surface_object), root, false);
	S_CalculateMouseVisible(VTHIS_ARG(&surface_object));   // who gets the mouse, every frame as in GoldSrc
	S_PaintTraverse(VTHIS_ARG(&surface_object), root);
	pstack_count = 0;
	glDisable(GL_SCISSOR_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, 1);
}

qboolean VGS_WantsMouse(void) { return need_mouse; }
qboolean VGS_WantsKeyboard(void) { return need_kb; }

void VGS_MouseMoved(int x, int y)
{
	mouse_pos[0] = x;
	mouse_pos[1] = y;
}

// vgui's cursor shapes as SDL's system cursors
void VGS_UpdateCursor(void)
{
	static SDL_Cursor *made[dc_last];
	static int shown = -1;
	int c = need_mouse ? (int)cursor : dc_none;
	if (c < 0 || c >= dc_last)
		c = dc_arrow;
	if (c == shown)
		return;
	shown = c;
	if (c == dc_none || c == dc_blank)
		return;   // the engine's own mouse handling hides it
	static const SDL_SystemCursor sys[dc_last] = {
		[dc_user] = SDL_SYSTEM_CURSOR_ARROW, [dc_arrow] = SDL_SYSTEM_CURSOR_ARROW, [dc_ibeam] = SDL_SYSTEM_CURSOR_IBEAM,
		[dc_hourglass] = SDL_SYSTEM_CURSOR_WAIT, [dc_waitarrow] = SDL_SYSTEM_CURSOR_WAITARROW,
		[dc_crosshair] = SDL_SYSTEM_CURSOR_CROSSHAIR, [dc_up] = SDL_SYSTEM_CURSOR_ARROW,
		[dc_sizenwse] = SDL_SYSTEM_CURSOR_SIZENWSE, [dc_sizenesw] = SDL_SYSTEM_CURSOR_SIZENESW,
		[dc_sizewe] = SDL_SYSTEM_CURSOR_SIZEWE, [dc_sizens] = SDL_SYSTEM_CURSOR_SIZENS,
		[dc_sizeall] = SDL_SYSTEM_CURSOR_SIZEALL, [dc_no] = SDL_SYSTEM_CURSOR_NO, [dc_hand] = SDL_SYSTEM_CURSOR_HAND,
	};
	if (!made[c])
		made[c] = SDL_CreateSystemCursor(sys[c]);
	if (made[c])
		SDL_SetCursor(made[c]);
}

void VGS_Shutdown(void)
{
	for (int i = 1; i < fonts_count; i++) {
		ClearFont(fonts[i]);
		Mem_Free(fonts[i]);
	}
	Mem_Free(fonts);
	fonts = NULL;
	fonts_count = 0;
	for (int i = 0; i < textures_count; i++)
		if (textures[i].gl)
			glDeleteTextures(1, &textures[i].gl);
	Mem_Free(textures);
	textures = NULL;
	textures_count = 0;
	Mem_Free(popups);
	popups = NULL;
	popups_count = 0;
	Mem_Free(pstack);
	pstack = NULL;
	pstack_count = pstack_cap = 0;
	root = 0;
}
