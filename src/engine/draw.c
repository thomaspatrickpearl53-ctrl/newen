// newen - 2D drawing: filled rectangles and console text
#include <SDL.h>
#include <SDL_opengl.h>
#include "common.h"

#define FIRST_CHAR 32
#define LAST_CHAR  126
#define COLUMNS    16

static fontatlas_t font;
static GLuint font_tex;
static cvar_t *con_font, *con_fontsize;

void Draw_Init(void)
{
	con_font = Cvar_Get("con_font", "Consolas", FCVAR_ARCHIVE, "console font face (restart to apply)");
	con_fontsize = Cvar_Get("con_fontsize", "16", FCVAR_ARCHIVE, "console font height in pixels (restart to apply)");

	int size = (int)con_fontsize->value;
	if (size < 8)
		size = 8;
	if (!Font_Rasterize(con_font->string, size, &font) && !Font_Rasterize("Courier New", size, &font))
		Sys_Error("Couldn't create the console font");

	glGenTextures(1, &font_tex);
	glBindTexture(GL_TEXTURE_2D, font_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, font.width, font.height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, font.alpha);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
}

void Draw_Shutdown(void)
{
	if (font_tex)
		glDeleteTextures(1, &font_tex);
	Mem_Free(font.alpha);
	memset(&font, 0, sizeof(font));
	font_tex = 0;
}

void Draw_Begin2D(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, vid_width, vid_height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

void Draw_Fill(float x, float y, float w, float h, int r, int g, int b, int a)
{
	glDisable(GL_TEXTURE_2D);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	glVertex2f(x, y);
	glVertex2f(x + w, y);
	glVertex2f(x + w, y + h);
	glVertex2f(x, y + h);
	glEnd();
}

int Draw_CharWidth(void)  { return font.cell_w; }
int Draw_LineHeight(void) { return font.cell_h; }

int Draw_StringWidth(const char *s)
{
	return (int)strlen(s) * font.cell_w;
}

int Draw_String(float x, float y, const char *s, int r, int g, int b, int a)
{
	float sx = x;
	float iw = 1.0f / font.width, ih = 1.0f / font.height;
	x = (float)(int)x;
	y = (float)(int)y;
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, font_tex);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	for (; *s; s++, x += font.cell_w) {
		int c = (unsigned char)*s;
		if (c == '\t')
			c = ' ';
		if (c <= FIRST_CHAR || c > LAST_CHAR)
			continue;   // spaces and unknown characters just advance
		int i = c - FIRST_CHAR;
		float u = (float)((i % COLUMNS) * font.cell_w) * iw;
		float v = (float)((i / COLUMNS) * font.cell_h) * ih;
		float u2 = u + font.cell_w * iw, v2 = v + font.cell_h * ih;
		glTexCoord2f(u, v);   glVertex2f(x, y);
		glTexCoord2f(u2, v);  glVertex2f(x + font.cell_w, y);
		glTexCoord2f(u2, v2); glVertex2f(x + font.cell_w, y + font.cell_h);
		glTexCoord2f(u, v2);  glVertex2f(x, y + font.cell_h);
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
	return (int)(x - sx);
}
