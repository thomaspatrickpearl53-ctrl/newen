// newen - rasterize a system font into an alpha atlas with stb_truetype (the non-Windows font_win.c)
//
// Face names are Windows ones ("Consolas", "Trebuchet MS"); fontconfig's fc-match turns them into a
// font file, substituting a similar installed font when the face itself isn't there.
#include <stdio.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ((void)(u), Mem_Alloc(x))
#define STBTT_free(x, u)   ((void)(u), Mem_Free(x))
#include "common.h"
#include "stb_truetype.h"

#define FIRST_CHAR 32
#define LAST_CHAR  126
#define COLUMNS    16

static unsigned char *ReadFontFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *data = len > 0 ? Mem_Alloc((size_t)len) : NULL;
	if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
		Mem_Free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

// fontconfig picks the file; style asks for a monospace or bold substitute when the face is missing.
// family (optional) gets the family it picked, to tell a real match from a substitute.
static unsigned char *LoadFaceFamily(const char *face, const char *style, char *family, size_t familysize)
{
	char cmd[512], path[MAX_OSPATH] = "";
	if (family)
		*family = 0;
	for (const char *s = face; *s; s++)
		if (*s == '\'' || *s == '\\' || *s == ':')
			return NULL;   // keep the shell command harmless
	snprintf(cmd, sizeof(cmd), "fc-match -f '%%{file}\\n%%{family}' '%s%s' 2>/dev/null", face, style);
	FILE *p = popen(cmd, "r");
	if (!p)
		return NULL;
	if (!fgets(path, sizeof(path), p))
		path[0] = 0;
	char fam[256] = "";
	if (!fgets(fam, sizeof(fam), p))
		fam[0] = 0;
	pclose(p);
	path[strcspn(path, "\n")] = 0;
	fam[strcspn(fam, ",\n")] = 0;
	if (family)
		Q_strncpy(family, fam, familysize);
	if (!path[0])
		return NULL;
	unsigned char *data = ReadFontFile(path);
	if (data)
		Con_DPrintf("Font %s: %s\n", face, path);
	return data;
}

static unsigned char *LoadFace(const char *face, const char *style)
{
	return LoadFaceFamily(face, style, NULL, 0);
}

static void AllocAtlas(fontatlas_t *out)
{
	int count = LAST_CHAR - FIRST_CHAR + 1;
	int rows = (count + COLUMNS - 1) / COLUMNS;
	int w = 1, h = 1;
	while (w < out->cell_w * COLUMNS) w <<= 1;
	while (h < out->cell_h * rows) h <<= 1;
	out->width = w;
	out->height = h;
	out->alpha = Mem_Alloc((size_t)w * h);
}

// draws one character with its pen at (x, baseline) of the atlas, clipped to its cell
static void DrawChar(const stbtt_fontinfo *info, float scale, int c, int cellx, int celly, int penx, int baseline,
	fontatlas_t *out)
{
	int x0, y0, x1, y1;
	stbtt_GetCodepointBitmapBox(info, c, scale, scale, &x0, &y0, &x1, &y1);
	int gw = x1 - x0, gh = y1 - y0;
	if (gw <= 0 || gh <= 0)
		return;
	unsigned char *glyph = Mem_Alloc((size_t)gw * gh);
	stbtt_MakeCodepointBitmap(info, glyph, gw, gh, gw, scale, scale, c);
	for (int y = 0; y < gh; y++) {
		int ay = baseline + y0 + y;
		if (ay < 0 || ay >= out->cell_h)
			continue;
		for (int x = 0; x < gw; x++) {
			int ax = penx + x0 + x;
			if (ax < 0 || ax >= out->cell_w)
				continue;
			unsigned char *d = &out->alpha[(size_t)(celly + ay) * out->width + cellx + ax];
			if (glyph[y * gw + x] > *d)
				*d = glyph[y * gw + x];
		}
	}
	Mem_Free(glyph);
}

qboolean Font_Rasterize(const char *face, int pixel_height, fontatlas_t *out)
{
	memset(out, 0, sizeof(*out));
	unsigned char *data = LoadFace(face, ":spacing=mono");
	stbtt_fontinfo info;
	if (!data || !stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
		Mem_Free(data);
		return false;
	}
	// like GDI's negative height: the em square is pixel_height tall
	float scale = stbtt_ScaleForMappingEmToPixels(&info, (float)pixel_height);
	int ascent, descent, gap, adv, lsb;
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
	stbtt_GetCodepointHMetrics(&info, 'M', &adv, &lsb);
	int base = (int)(ascent * scale + 0.5f);
	out->cell_w = (int)(adv * scale + 0.5f);
	out->cell_h = base + (int)(-descent * scale + 0.5f);
	AllocAtlas(out);
	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		int i = c - FIRST_CHAR;
		DrawChar(&info, scale, c, (i % COLUMNS) * out->cell_w, (i / COLUMNS) * out->cell_h, 0, base, out);
	}
	Mem_Free(data);
	return true;
}

qboolean Font_RasterizeProportional(const char *face, int tall, int weight, fontatlas_t *out)
{
	memset(out, 0, sizeof(*out));
	unsigned char *data = LoadFace(face, weight >= 600 ? ":weight=bold" : "");
	stbtt_fontinfo info;
	if (!data || !stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
		Mem_Free(data);
		return false;
	}
	// like GDI's positive height: ascent plus descent is tall
	float scale = stbtt_ScaleForPixelHeight(&info, (float)tall);
	int ascent, descent, gap;
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
	int base = (int)(ascent * scale + 0.5f);
	int widest = 0;
	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		int adv, lsb, x0, y0, x1, y1;
		stbtt_GetCodepointHMetrics(&info, c, &adv, &lsb);
		stbtt_GetCodepointBitmapBox(&info, c, scale, scale, &x0, &y0, &x1, &y1);
		int a = (int)(adv * scale + 0.5f);
		if (x1 > a)
			a = x1;   // keep the whole glyph inside its cell
		out->advance[c] = (short)a;
		if (a > widest)
			widest = a;
	}
	out->pad = tall / 4;   // italics and bold glyphs can reach outside their advance
	out->cell_w = widest + out->pad * 2;
	out->cell_h = tall;
	AllocAtlas(out);
	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		int i = c - FIRST_CHAR;
		DrawChar(&info, scale, c, (i % COLUMNS) * out->cell_w, (i / COLUMNS) * out->cell_h, out->pad, base, out);
	}
	Mem_Free(data);
	return true;
}

// ---------------------------------------------------------------- single glyphs

struct fontface_s {
	unsigned char *data;
	stbtt_fontinfo info;
	float scale;
	int tall, ascent;
	qboolean antialias;
};

// font files the game ships (VGUI2 schemes list them as CustomFontFiles)
static unsigned char **custom_fonts;
static int custom_count;

void Font_AddFile(const char *path)
{
	unsigned char *data = ReadFontFile(path);
	if (!data)
		return;
	custom_fonts = Mem_Realloc(custom_fonts, sizeof(*custom_fonts) * (custom_count + 1));
	custom_fonts[custom_count++] = data;
	Con_DPrintf("Font file %s\n", path);
}

// a copy of the custom font file with this family (and style, if one has it), or NULL
static unsigned char *FindCustom(const char *face, int macstyle)
{
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < custom_count; i++) {
			int ofs = stbtt_FindMatchingFont(custom_fonts[i], face, pass ? STBTT_MACSTYLE_DONTCARE : macstyle);
			if (ofs >= 0)
				return custom_fonts[i];   // stb_truetype only reads it, so faces can share it
		}
	return NULL;
}

fontface_t *Font_Open(const char *face, int tall, int weight, qboolean italic, qboolean antialias)
{
	char style[64], family[256];
	snprintf(style, sizeof(style), "%s%s", weight >= 600 ? ":weight=bold" : "", italic ? ":slant=italic" : "");
	int macstyle = (weight >= 600 ? STBTT_MACSTYLE_BOLD : 0) | (italic ? STBTT_MACSTYLE_ITALIC : 0);
	if (!macstyle)
		macstyle = STBTT_MACSTYLE_NONE;
	qboolean shared = false;
	unsigned char *data = FindCustom(face, macstyle);
	if (data) {
		shared = true;
	} else {
		data = LoadFaceFamily(face, style, family, sizeof(family));
		// Windows faces the system doesn't have: the fonts Half-Life ships for Linux
		if (data && stricmp(family, face)) {
			static const char *subs[][2] = {
				{ "Tahoma", "DejaVu Sans" }, { "Verdana", "DejaVu Sans" }, { "Arial", "Liberation Sans" },
				{ "Trebuchet MS", "DejaVu Sans" }, { "Courier", "Liberation Mono" },
				{ "Courier New", "Liberation Mono" }, { "Lucida Console", "Liberation Mono" },
			};
			for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
				if (stricmp(subs[i][0], face))
					continue;
				unsigned char *sub = FindCustom(subs[i][1], macstyle);
				if (sub) {
					Con_DPrintf("Font %s: the game's %s instead\n", face, subs[i][1]);
					Mem_Free(data);
					data = sub;
					shared = true;
				}
				break;
			}
		}
	}
	if (!data)
		return NULL;
	fontface_t *f = Mem_Alloc(sizeof(*f));
	int ofs = stbtt_GetFontOffsetForIndex(data, 0);
	if (shared) {
		int m = stbtt_FindMatchingFont(data, face, macstyle);
		if (m >= 0)
			ofs = m;
	}
	if (!stbtt_InitFont(&f->info, data, ofs)) {
		if (!shared)
			Mem_Free(data);
		Mem_Free(f);
		return NULL;
	}
	f->data = shared ? NULL : data;   // custom files stay loaded
	f->tall = tall > 0 ? tall : 1;
	// like GDI's positive height: ascent plus descent is tall
	f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)f->tall);
	int ascent, descent, gap;
	stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
	f->ascent = (int)(ascent * f->scale + 0.5f);
	f->antialias = antialias;
	return f;
}

void Font_Close(fontface_t *f)
{
	if (!f)
		return;
	Mem_Free(f->data);
	Mem_Free(f);
}

int Font_Height(fontface_t *f) { return f ? f->tall : 0; }
int Font_Ascent(fontface_t *f) { return f ? f->ascent : 0; }

void Font_Glyph(fontface_t *f, int ch, int *a, int *b, int *c, unsigned char **alpha)
{
	*a = *b = *c = 0;
	*alpha = NULL;
	if (!f)
		return;
	int adv, lsb, x0, y0, x1, y1;
	stbtt_GetCodepointHMetrics(&f->info, ch, &adv, &lsb);
	stbtt_GetCodepointBitmapBox(&f->info, ch, f->scale, f->scale, &x0, &y0, &x1, &y1);
	int advance = (int)(adv * f->scale + 0.5f);
	int gw = x1 - x0, gh = y1 - y0;
	if (gw <= 0 || gh <= 0) {
		*c = advance;
		return;
	}
	*a = x0;
	*b = gw;
	*c = advance - x1;
	unsigned char *glyph = Mem_Alloc((size_t)gw * gh);
	stbtt_MakeCodepointBitmap(&f->info, glyph, gw, gh, gw, f->scale, f->scale, ch);
	unsigned char *out = Mem_Alloc((size_t)gw * f->tall);
	for (int y = 0; y < gh; y++) {
		int oy = f->ascent + y0 + y;
		if (oy < 0 || oy >= f->tall)
			continue;
		for (int x = 0; x < gw; x++) {
			unsigned char v = glyph[y * gw + x];
			out[oy * gw + x] = f->antialias ? v : (v >= 128 ? 255 : 0);
		}
	}
	Mem_Free(glyph);
	*alpha = out;
}
