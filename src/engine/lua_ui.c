// newen - Lua: fonts for surface.* (surface.CreateFont / SetFont), proportional and antialiased, made from the
// system's (or the game's) font files the same way the captions' font is
#include "lua_local.h"
#include "gl_local.h"

typedef struct {
	char        name[64];
	char        face[64];
	int         size, weight;
	fontatlas_t atlas;
	GLuint      tex;
} luafont_t;

static luafont_t *fonts;
static int fonts_count, fonts_cap;
static int current = -1;   // -1 = the engine's own text (the console's)

static qboolean BuildFont(luafont_t *f)
{
	if (!Font_RasterizeProportional(f->face, f->size, f->weight, &f->atlas) &&
		!Font_RasterizeProportional("Arial", f->size, f->weight, &f->atlas) &&
		!Font_RasterizeProportional("DejaVu Sans", f->size, f->weight, &f->atlas)) {
		Con_Warnf("Font %s (%s %d): no font file found\n", f->name, f->face, f->size);
		return false;
	}
	glGenTextures(1, &f->tex);
	glBindTexture(GL_TEXTURE_2D, f->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, f->atlas.width, f->atlas.height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, f->atlas.alpha);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

// surface.CreateFont(name, { font = "Arial", size = 20, weight = 500 })
static int l_CreateFont(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	int i;
	for (i = 0; i < fonts_count; i++)
		if (!strcmp(fonts[i].name, name))
			break;
	if (i == fonts_count) {
		if (fonts_count == fonts_cap) {
			fonts_cap = fonts_cap ? fonts_cap * 2 : 8;
			fonts = Mem_Realloc(fonts, sizeof(*fonts) * fonts_cap);
		}
		memset(&fonts[fonts_count++], 0, sizeof(*fonts));
	} else {
		// made again with new settings
		Mem_Free(fonts[i].atlas.alpha);
		if (fonts[i].tex)
			glDeleteTextures(1, &fonts[i].tex);
		memset(&fonts[i], 0, sizeof(*fonts));
	}
	luafont_t *f = &fonts[i];
	Q_strncpy(f->name, name, sizeof(f->name));
	lua_getfield(L, 2, "font");
	Q_strncpy(f->face, luaL_optstring(L, -1, "Arial"), sizeof(f->face));
	lua_getfield(L, 2, "size");
	f->size = (int)luaL_optinteger(L, -1, 20);
	lua_getfield(L, 2, "weight");
	f->weight = (int)luaL_optinteger(L, -1, 500);
	lua_pop(L, 3);
	if (f->size < 6) f->size = 6;
	if (f->size > 128) f->size = 128;
	BuildFont(f);
	return 0;
}

// surface.SetFont(name): the font DrawText and GetTextSize use; an unknown name goes back to the engine's text
static int l_SetFont(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	current = -1;
	for (int i = 0; i < fonts_count; i++)
		if (!strcmp(fonts[i].name, name) && fonts[i].tex)
			current = i;
	return 0;
}

static int Advance(const luafont_t *f, int ch)
{
	return ch >= 32 && ch < 127 ? f->atlas.advance[ch] : 0;
}

int LuaUI_DrawText(float x, float y, const char *s, int r, int g, int b, int a)
{
	if (current < 0)
		return -1;
	const luafont_t *f = &fonts[current];
	const fontatlas_t *at = &f->atlas;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, f->tex);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	float x0 = x;
	glBegin(GL_QUADS);
	for (; *s; s++) {
		int ch = (unsigned char)*s;
		if (ch > 32 && ch < 127) {
			int k = ch - 32, cx = (k % 16) * at->cell_w, cy = (k / 16) * at->cell_h;
			float s0 = (float)cx / at->width, s1 = (float)(cx + at->cell_w) / at->width;
			float t0 = (float)cy / at->height, t1 = (float)(cy + at->cell_h) / at->height;
			float gx = (float)(int)x - at->pad, gy = (float)(int)y;
			glTexCoord2f(s0, t0); glVertex2f(gx, gy);
			glTexCoord2f(s1, t0); glVertex2f(gx + at->cell_w, gy);
			glTexCoord2f(s1, t1); glVertex2f(gx + at->cell_w, gy + at->cell_h);
			glTexCoord2f(s0, t1); glVertex2f(gx, gy + at->cell_h);
		}
		x += Advance(f, ch);
	}
	glEnd();
	glColor4f(1, 1, 1, 1);
	return (int)(x - x0);
}

qboolean LuaUI_TextSize(const char *s, int *w, int *h)
{
	if (current < 0)
		return false;
	const luafont_t *f = &fonts[current];
	int width = 0;
	for (; *s; s++)
		width += Advance(f, (unsigned char)*s);
	*w = width;
	*h = f->atlas.cell_h;
	return true;
}

void LuaUI_TexturedRect(unsigned int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
	int r, int g, int b, int a)
{
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, tex);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	glTexCoord2f(u0, v0); glVertex2f(x, y);
	glTexCoord2f(u1, v0); glVertex2f(x + w, y);
	glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
	glTexCoord2f(u0, v1); glVertex2f(x, y + h);
	glEnd();
	glColor4f(1, 1, 1, 1);
}

void LuaUI_Open(lua_State *L)
{
	lua_getglobal(L, "surface");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, l_CreateFont);
		lua_setfield(L, -2, "CreateFont");
		lua_pushcfunction(L, l_SetFont);
		lua_setfield(L, -2, "SetFont");
	}
	lua_pop(L, 1);
	current = -1;   // a restarted Lua starts on the engine's text; the fonts themselves stay made
}
