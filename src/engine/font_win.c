// newen - rasterize a Windows font into an alpha atlas with GDI
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "common.h"

#define FIRST_CHAR 32
#define LAST_CHAR  126
#define COLUMNS    16

qboolean Font_Rasterize(const char *face, int pixel_height, fontatlas_t *out)
{
	memset(out, 0, sizeof(*out));
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return false;
	HFONT font = CreateFontA(-pixel_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, face);
	if (!font) {
		DeleteDC(dc);
		return false;
	}
	SelectObject(dc, font);

	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	SIZE sz;
	GetTextExtentPoint32A(dc, "M", 1, &sz);
	out->cell_w = sz.cx;
	out->cell_h = tm.tmHeight;

	int count = LAST_CHAR - FIRST_CHAR + 1;
	int rows = (count + COLUMNS - 1) / COLUMNS;
	int w = 1, h = 1;
	while (w < out->cell_w * COLUMNS) w <<= 1;
	while (h < out->cell_h * rows) h <<= 1;
	out->width = w;
	out->height = h;

	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bmp) {
		DeleteObject(font);
		DeleteDC(dc);
		return false;
	}
	SelectObject(dc, bmp);
	SelectObject(dc, font);
	SetBkMode(dc, OPAQUE);
	SetBkColor(dc, RGB(0, 0, 0));
	SetTextColor(dc, RGB(255, 255, 255));
	RECT all = { 0, 0, w, h };
	FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));

	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		int i = c - FIRST_CHAR;
		char ch = (char)c;
		TextOutA(dc, (i % COLUMNS) * out->cell_w, (i / COLUMNS) * out->cell_h, &ch, 1);
	}
	GdiFlush();

	out->alpha = Mem_Alloc((size_t)w * h);
	const unsigned char *px = bits;
	for (int i = 0; i < w * h; i++)
		out->alpha[i] = px[i * 4 + 1];   // green channel

	DeleteObject(bmp);
	DeleteObject(font);
	DeleteDC(dc);
	return true;
}

qboolean Font_RasterizeProportional(const char *face, int tall, int weight, fontatlas_t *out)
{
	memset(out, 0, sizeof(*out));
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return false;
	HFONT font = CreateFontA(tall, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
	if (!font) {
		DeleteDC(dc);
		return false;
	}
	SelectObject(dc, font);

	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	int widest = 0;
	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		INT adv = 0;
		ABC abc = { 0, 0, 0 };
		GetCharWidth32A(dc, c, c, &adv);
		if (GetCharABCWidthsA(dc, c, c, &abc) && (int)abc.abcB + (abc.abcA > 0 ? abc.abcA : 0) > adv)
			adv = abc.abcA + abc.abcB;   // keep the whole glyph inside its cell
		out->advance[c] = (short)adv;
		if (adv > widest)
			widest = adv;
	}
	out->pad = tm.tmHeight / 4;   // italics and bold glyphs can reach outside their advance
	out->cell_w = widest + out->pad * 2;
	out->cell_h = tm.tmHeight;

	int count = LAST_CHAR - FIRST_CHAR + 1;
	int rows = (count + COLUMNS - 1) / COLUMNS;
	int w = 1, h = 1;
	while (w < out->cell_w * COLUMNS) w <<= 1;
	while (h < out->cell_h * rows) h <<= 1;
	out->width = w;
	out->height = h;

	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bmp) {
		DeleteObject(font);
		DeleteDC(dc);
		return false;
	}
	SelectObject(dc, bmp);
	SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	RECT all = { 0, 0, w, h };
	FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));

	for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
		int i = c - FIRST_CHAR;
		char ch = (char)c;
		TextOutA(dc, (i % COLUMNS) * out->cell_w + out->pad, (i / COLUMNS) * out->cell_h, &ch, 1);
	}
	GdiFlush();

	out->alpha = Mem_Alloc((size_t)w * h);
	const unsigned char *px = bits;
	for (int i = 0; i < w * h; i++)
		out->alpha[i] = px[i * 4 + 1];   // green channel

	DeleteObject(bmp);
	DeleteObject(font);
	DeleteDC(dc);
	return true;
}

// ---------------------------------------------------------------- single glyphs

// font files the game ships (VGUI2 schemes list them as CustomFontFiles), for this process only
void Font_AddFile(const char *path)
{
	if (!AddFontResourceExA(path, FR_PRIVATE, 0))
		Con_DPrintf("Couldn't add font %s\n", path);
}

struct fontface_s {
	HDC dc;
	HFONT font;
	HBITMAP bmp;
	void *bits;
	int bmp_w, tall, ascent;
};

fontface_t *Font_Open(const char *face, int tall, int weight, qboolean italic, qboolean antialias)
{
	if (tall < 1)
		tall = 1;
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return NULL;
	HFONT font = CreateFontA(tall, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, antialias ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, face);
	if (!font) {
		DeleteDC(dc);
		return NULL;
	}
	fontface_t *f = Mem_Alloc(sizeof(*f));
	f->dc = dc;
	f->font = font;
	SelectObject(dc, font);
	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	f->tall = tm.tmHeight;
	f->ascent = tm.tmAscent;
	// room for the widest glyph plus overhangs on both sides
	f->bmp_w = tm.tmMaxCharWidth * 2 + f->tall * 2;
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = f->bmp_w;
	bi.bmiHeader.biHeight = -f->tall;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	f->bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &f->bits, NULL, 0);
	if (!f->bmp) {
		Font_Close(f);
		return NULL;
	}
	SelectObject(dc, f->bmp);
	SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	return f;
}

void Font_Close(fontface_t *f)
{
	if (!f)
		return;
	if (f->bmp)
		DeleteObject(f->bmp);
	DeleteObject(f->font);
	DeleteDC(f->dc);
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
	WCHAR wc[2];
	int len = 1;
	if (ch > 0xFFFF) {   // outside the BMP: a surrogate pair
		ch -= 0x10000;
		wc[0] = (WCHAR)(0xD800 + (ch >> 10));
		wc[1] = (WCHAR)(0xDC00 + (ch & 0x3FF));
		len = 2;
	} else {
		wc[0] = (WCHAR)ch;
	}
	ABC abc;
	if (len == 1 && GetCharABCWidthsW(f->dc, ch, ch, &abc)) {
		*a = abc.abcA;
		*b = (int)abc.abcB;
		*c = abc.abcC;
	} else {
		SIZE sz;
		GetTextExtentPoint32W(f->dc, wc, len, &sz);
		*b = sz.cx;
	}
	if (*b <= 0 || *b > f->bmp_w - f->tall * 2)
		return;
	// the glyph's black box starts at x = pad
	int pad = f->tall;
	RECT all = { 0, 0, f->bmp_w, f->tall };
	FillRect(f->dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
	TextOutW(f->dc, pad - *a, 0, wc, len);
	GdiFlush();
	unsigned char *out = Mem_Alloc((size_t)*b * f->tall);
	const unsigned char *px = f->bits;
	for (int y = 0; y < f->tall; y++)
		for (int x = 0; x < *b; x++)
			out[y * *b + x] = px[((size_t)y * f->bmp_w + pad + x) * 4 + 1];   // green channel
	*alpha = out;
}
