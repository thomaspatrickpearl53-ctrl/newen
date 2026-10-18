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
