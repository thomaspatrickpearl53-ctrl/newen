// newen - closed captions
//
// Every sound reaches the mixer through S_StartSound, which tells us here what started, for how
// long and how loud it is where the listener stands. A caption comes from the caption file
// (captions/<cc_lang>.txt, then captions/<cc_lang>_*.txt from addons, later entries win) by the
// sentence name ("HG_ALERT0") or the sample path ("weapons/explode3.wav", wildcards allowed);
// otherwise sentences built from word files (VOX, the HEV suit, soldiers' radio) are captioned
// from their words. The file's layout copies the idea of Source's closecaption files:
//
//   "lang" { "Language" "english"
//     "Tokens"   { "HG_ALERT0" "<clr:200,220,140>Squad, we got Freeman!"  "weapons/explode*" "<sfx>[Explosion]" }
//     "Speakers" { "hgrunt" "<clr:200,220,140>" }   word directories that get automatic captions, and their colour
//     "Words"    { "onehundred" "one hundred"  "clik" "" }   respellings; "" drops the word
//   }
//
// Tags: <clr:r,g,b>, <sfx> (a sound effect, hidden by cc_subtitles 1), <len:seconds>, <cr> (new
// line). Captions queue up while sounds start and are shown at the next frame, after the Lua
// hook CaptionShow(key, text, isSfx) may change (return a string) or hide (return false) them.
#include "snd_local.h"
#include "gl_local.h"
#include "lua_local.h"
#include <ctype.h>

typedef struct {
	char *key, *value;
} ccpair_t;

typedef struct {
	ccpair_t *items;
	int count, cap;
} cclist_t;

typedef struct {
	char    *key;          // what it was found by, so a sound repeating doesn't stack copies
	char    *text;         // without tags
	int      r, g, b;
	qboolean sfx;
	float    seconds;      // how long the sound plays
	double   end;          // when it starts fading (active captions)
} caption_t;

static cclist_t tokens, words, speakers;
static caption_t *pending, *active;
static int pending_count, pending_cap, active_count, active_cap;
static char loaded_lang[64];
static cvar_t *closecaption, *cc_subtitles, *cc_lang, *cc_lines, *cc_linger, *cc_minvolume, *cc_font;

#define FADE_TIME 0.5   // seconds a caption takes to fade out
#define MIN_TIME  1.0   // short sounds still stay up long enough to read

// ---------------------------------------------------------------- the caption file

static void AddPair(cclist_t *l, const char *key, const char *value)
{
	if (l->count == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->items = Mem_Realloc(l->items, sizeof(*l->items) * l->cap);
	}
	l->items[l->count].key = Mem_Strdup(key);
	l->items[l->count].value = Mem_Strdup(value);
	l->count++;
}

static void FreeList(cclist_t *l)
{
	for (int i = 0; i < l->count; i++) {
		Mem_Free(l->items[i].key);
		Mem_Free(l->items[i].value);
	}
	Mem_Free(l->items);
	memset(l, 0, sizeof(*l));
}

// the last entry wins, so addon files and later lines override earlier ones; exact names first
static const char *Find(const cclist_t *l, const char *key)
{
	for (int i = l->count - 1; i >= 0; i--)
		if (!stricmp(l->items[i].key, key))
			return l->items[i].value;
	for (int i = l->count - 1; i >= 0; i--)
		if (strpbrk(l->items[i].key, "*?") && Q_WildcardMatch(l->items[i].key, key))
			return l->items[i].value;
	return NULL;
}

static void ParseFile(const char *text)
{
	char key[256], section[64] = "";
	const char *p = text;
	while ((p = COM_Parse(p)) != NULL) {
		if (!strcmp(com_token, "{"))
			continue;
		if (!strcmp(com_token, "}")) {
			section[0] = 0;
			continue;
		}
		Q_strncpy(key, com_token, sizeof(key));
		if ((p = COM_Parse(p)) == NULL)
			break;
		if (!strcmp(com_token, "{")) {
			Q_strncpy(section, key, sizeof(section));
			continue;
		}
		if (!stricmp(section, "Tokens"))
			AddPair(&tokens, key, com_token);
		else if (!stricmp(section, "Words"))
			AddPair(&words, key, com_token);
		else if (!stricmp(section, "Speakers"))
			AddPair(&speakers, key, com_token);
	}
}

static void LoadFile(const char *path)
{
	char *text = FS_LoadFile(path, NULL);
	if (!text)
		return;
	ParseFile(text);
	Mem_Free(text);
}

static void LoadCaptions(void)
{
	FreeList(&tokens);
	FreeList(&words);
	FreeList(&speakers);
	Q_strncpy(loaded_lang, cc_lang->string, sizeof(loaded_lang));
	LoadFile(va("captions/%s.txt", loaded_lang));
	char **list;
	int n = FS_ListFiles("captions", va("%s_*.txt", loaded_lang), &list);
	for (int i = 0; i < n; i++)
		LoadFile(list[i]);   // already "captions/<name>"
	FS_FreeList(list, n);
	Con_DPrintf("Captions (%s): %d lines, %d word spellings, %d speakers\n", loaded_lang, tokens.count, words.count,
		speakers.count);
}

// ---------------------------------------------------------------- making captions

// strips the tags into out; returns false when there's nothing left to show
static qboolean ParseTags(const char *in, strbuf_t *out, caption_t *c)
{
	SB_Clear(out);
	while (*in) {
		if (*in == '<') {
			const char *end = strchr(in, '>');
			if (end) {
				char tag[64];
				size_t n = (size_t)(end - in - 1);
				if (n >= sizeof(tag))
					n = sizeof(tag) - 1;
				memcpy(tag, in + 1, n);
				tag[n] = 0;
				if (!strnicmp(tag, "clr:", 4))
					sscanf(tag + 4, "%d,%d,%d", &c->r, &c->g, &c->b);
				else if (!stricmp(tag, "sfx"))
					c->sfx = true;
				else if (!strnicmp(tag, "len:", 4))
					c->seconds = (float)atof(tag + 4);
				else if (!stricmp(tag, "cr"))
					SB_Append(out, "\n", 1);
				// tags we don't know (Source's <B>, <I>, <norepeat:...>) are dropped
				in = end + 1;
				continue;
			}
		}
		SB_Append(out, in, 1);
		in++;
	}
	return out->len > 0;
}

// "hgrunt/squad!.wav" -> directory "hgrunt", word "squad"
static void SplitSample(const char *sample, char *dir, size_t dirsize, char *word, size_t wordsize)
{
	const char *slash = strrchr(sample, '/');
	size_t d = slash ? (size_t)(slash - sample) : 0;
	if (d >= dirsize)
		d = dirsize - 1;
	memcpy(dir, sample, d);
	dir[d] = 0;
	Q_strncpy(word, slash ? slash + 1 : sample, wordsize);
	char *dot = strrchr(word, '.');
	if (dot)
		*dot = 0;
	size_t n = strlen(word);
	if (n && word[n - 1] == '!')   // the soldiers' stressed variant of the same word
		word[n - 1] = 0;
}

// a caption from the sentence's words, if they're all from directories the file lists as speech
static qboolean AutoCaption(const sentence_t *s, strbuf_t *out)
{
	SB_Clear(out);
	for (int i = 0; i < s->numwords; i++) {
		char dir[64], word[64];
		SplitSample(s->words[i].sample, dir, sizeof(dir), word, sizeof(word));
		if (!Find(&speakers, dir))
			return false;
		if (!stricmp(word, "_comma") || !stricmp(word, "_period")) {
			if (out->len)
				SB_Append(out, word[1] == 'c' ? "," : ".", 1);
			continue;
		}
		const char *spelling = Find(&words, word);
		if (spelling && !*spelling)
			continue;   // a beep or a click
		char text[128];
		Q_strncpy(text, spelling ? spelling : word, sizeof(text));
		for (char *p = text; *p; p++)
			if (*p == '_')
				*p = ' ';
		if (out->len)
			SB_Append(out, " ", 1);
		SB_Append(out, text, strlen(text));
	}
	// leading punctuation from dropped words would look odd
	while (out->len && (out->data[out->len - 1] == ',' || out->data[out->len - 1] == ' '))
		out->data[--out->len] = 0;
	if (!out->len)
		return false;
	out->data[0] = (char)toupper((unsigned char)out->data[0]);
	return true;
}

static void Queue(const char *key, const char *text, const caption_t *c)
{
	if (pending_count == pending_cap) {
		pending_cap = pending_cap ? pending_cap * 2 : 16;
		pending = Mem_Realloc(pending, sizeof(*pending) * pending_cap);
	}
	caption_t *p = &pending[pending_count++];
	*p = *c;
	p->key = Mem_Strdup(key);
	p->text = Mem_Strdup(text);
}

void CC_SoundStarted(const char *key, const sentence_t *sentence, float seconds, float gain)
{
	if (!closecaption || !closecaption->value || gain < cc_minvolume->value || !key || !*key)
		return;
	if (strcmp(loaded_lang, cc_lang->string))
		LoadCaptions();
	while (*key == '*' || *key == '#' || *key == '^' || *key == '@' || *key == ')' || *key == '<' || *key == '>')
		key++;

	caption_t c;
	memset(&c, 0, sizeof(c));
	c.r = c.g = c.b = 255;
	c.seconds = seconds;
	// the speaker's colour first, so the line's own tags win
	char dir[64], word[64];
	SplitSample(sentence && sentence->numwords ? sentence->words[0].sample : key, dir, sizeof(dir), word, sizeof(word));
	const char *speaker = Find(&speakers, dir);
	strbuf_t text = { 0 }, ignored = { 0 };
	if (speaker)
		ParseTags(speaker, &ignored, &c);

	const char *line = Find(&tokens, key);
	// a sentence of one recording can be captioned by that recording's path too
	if (!line && sentence && sentence->numwords == 1)
		line = Find(&tokens, sentence->words[0].sample);
	qboolean ok = false;
	if (line) {
		ok = ParseTags(line, &text, &c);
	} else if (sentence && AutoCaption(sentence, &ignored)) {
		SB_Append(&text, ignored.data, ignored.len);
		ok = true;
	}
	if (ok && !(c.sfx && cc_subtitles->value))
		Queue(key, text.data, &c);
	SB_Free(&text);
	SB_Free(&ignored);
}

void CC_Add(const char *text, float seconds, int r, int g, int b)
{
	if (!text || !*text)
		return;
	caption_t c;
	memset(&c, 0, sizeof(c));
	c.r = r;
	c.g = g;
	c.b = b;
	c.seconds = seconds;
	strbuf_t out = { 0 };
	if (ParseTags(text, &out, &c))
		Queue(text, out.data, &c);
	SB_Free(&out);
}

static void FreeCaption(caption_t *c)
{
	Mem_Free(c->key);
	Mem_Free(c->text);
}

void CC_Clear(void)
{
	for (int i = 0; i < pending_count; i++)
		FreeCaption(&pending[i]);
	for (int i = 0; i < active_count; i++)
		FreeCaption(&active[i]);
	pending_count = active_count = 0;
}

// queued captions go through the Lua hook, then on screen; a caption already showing is kept up longer
static void ShowPending(void)
{
	// detached: the hook may start sounds of its own, which queue for the next frame
	caption_t *list = pending;
	int count = pending_count;
	pending = NULL;
	pending_count = pending_cap = 0;
	for (int i = 0; i < count; i++) {
		caption_t *c = &list[i];
		lua_State *L = Lua_GetState();
		if (L) {
			int top = lua_gettop(L);
			lua_pushstring(L, c->key);
			lua_pushstring(L, c->text);
			lua_pushboolean(L, c->sfx);
			if (Lua_Hook("CaptionShow", 3, 1) == 1) {
				if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
					lua_settop(L, top);
					FreeCaption(c);
					continue;
				}
				if (lua_type(L, -1) == LUA_TSTRING && *lua_tostring(L, -1)) {
					Mem_Free(c->text);
					c->text = Mem_Strdup(lua_tostring(L, -1));
				}
			}
			lua_settop(L, top);
		}
		double end = host_realtime + (c->seconds > MIN_TIME ? c->seconds : MIN_TIME) + cc_linger->value;
		int j;
		for (j = 0; j < active_count; j++)
			if (!strcmp(active[j].key, c->key) && !strcmp(active[j].text, c->text))
				break;
		if (j < active_count) {
			if (end > active[j].end)
				active[j].end = end;
			FreeCaption(c);
			continue;
		}
		if (active_count == active_cap) {
			active_cap = active_cap ? active_cap * 2 : 16;
			active = Mem_Realloc(active, sizeof(*active) * active_cap);
		}
		c->end = end;
		active[active_count++] = *c;
	}
	Mem_Free(list);
}

// ---------------------------------------------------------------- drawing

// a proportional font like the titles', but drawn with normal blending so it reads on any background
static struct {
	int built_for;
	char face[64];
	fontatlas_t atlas;
	GLuint tex;
} ccfont;

static qboolean CaptionFont(void)
{
	if (ccfont.built_for == vid_height && !strcmp(ccfont.face, cc_font->string))
		return ccfont.tex != 0;
	ccfont.built_for = vid_height;
	Q_strncpy(ccfont.face, cc_font->string, sizeof(ccfont.face));
	Mem_Free(ccfont.atlas.alpha);
	memset(&ccfont.atlas, 0, sizeof(ccfont.atlas));
	int tall = (18 * vid_height + 240) / 480;
	if (tall < 12)
		tall = 12;
	if (!Font_RasterizeProportional(cc_font->string, tall, 600, &ccfont.atlas) &&
		!Font_RasterizeProportional("Arial", tall, 600, &ccfont.atlas)) {
		if (ccfont.tex)
			glDeleteTextures(1, &ccfont.tex);
		ccfont.tex = 0;
		return false;
	}
	if (!ccfont.tex)
		glGenTextures(1, &ccfont.tex);
	glBindTexture(GL_TEXTURE_2D, ccfont.tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, ccfont.atlas.width, ccfont.atlas.height, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		ccfont.atlas.alpha);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

static int CharWidth(int ch)
{
	return ch >= 32 && ch < 127 ? ccfont.atlas.advance[ch] : 0;
}

static int TextWidth(const char *s, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++)
		w += CharWidth((unsigned char)s[i]);
	return w;
}

static void DrawCaptionText(int x, int y, const char *s, int n, int r, int g, int b, int a)
{
	const fontatlas_t *f = &ccfont.atlas;
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, ccfont.tex);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	for (int i = 0; i < n; i++) {
		int ch = (unsigned char)s[i];
		if (ch > 32 && ch < 127) {
			int k = ch - 32, cx = (k % 16) * f->cell_w, cy = (k / 16) * f->cell_h;
			float s0 = (float)cx / f->width, s1 = (float)(cx + f->cell_w) / f->width;
			float t0 = (float)cy / f->height, t1 = (float)(cy + f->cell_h) / f->height;
			int x0 = x - f->pad;
			glTexCoord2f(s0, t0); glVertex2i(x0, y);
			glTexCoord2f(s1, t0); glVertex2i(x0 + f->cell_w, y);
			glTexCoord2f(s1, t1); glVertex2i(x0 + f->cell_w, y + f->cell_h);
			glTexCoord2f(s0, t1); glVertex2i(x0, y + f->cell_h);
		}
		x += CharWidth(ch);
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

typedef struct {
	const caption_t *c;
	int start, len;      // into c->text
	float alpha;
} ccline_t;

static ccline_t *lines;
static int lines_cap;

// splits a caption into lines that fit; returns the new line count
static int WrapCaption(const caption_t *c, float alpha, int width, int count)
{
	const char *t = c->text;
	int n = (int)strlen(t), at = 0;
	while (at < n) {
		int end = at, last_space = -1, w = 0;
		while (end < n && t[end] != '\n') {
			w += CharWidth((unsigned char)t[end]);
			if (w > width && end > at)
				break;
			if (t[end] == ' ')
				last_space = end;
			end++;
		}
		if (end < n && t[end] != '\n' && last_space > at)
			end = last_space;
		if (count == lines_cap) {
			lines_cap = lines_cap ? lines_cap * 2 : 16;
			lines = Mem_Realloc(lines, sizeof(*lines) * lines_cap);
		}
		lines[count].c = c;
		lines[count].start = at;
		lines[count].len = end - at;
		lines[count].alpha = alpha;
		count++;
		at = end;
		while (at < n && (t[at] == ' ' || t[at] == '\n'))
			at++;
	}
	return count;
}

void CC_Draw(void)
{
	ShowPending();
	// captions that have faded go
	int kept = 0;
	for (int i = 0; i < active_count; i++) {
		if (host_realtime >= active[i].end + FADE_TIME)
			FreeCaption(&active[i]);
		else
			active[kept++] = active[i];
	}
	active_count = kept;
	if (!active_count || !closecaption->value || !CaptionFont())
		return;

	int pad = vid_height / 80 + 4;
	int boxw = vid_width * 3 / 5;
	if (boxw < 320)
		boxw = vid_width - 32;
	int count = 0;
	for (int i = 0; i < active_count; i++) {
		double left = active[i].end + FADE_TIME - host_realtime;
		float alpha = left < FADE_TIME ? (float)(left / FADE_TIME) : 1;
		count = WrapCaption(&active[i], alpha, boxw - 2 * pad, count);
	}
	// newest at the bottom; the oldest lines go when there are too many
	int max = (int)cc_lines->value;
	if (max < 1)
		max = 1;
	int first = count > max ? count - max : 0;
	int lh = ccfont.atlas.cell_h;
	int boxh = (count - first) * lh + 2 * pad;
	int x = (vid_width - boxw) / 2;
	int y = vid_height * 4 / 5 - boxh;   // above the HUD's health and ammo
	float box_alpha = 0;
	for (int i = first; i < count; i++)
		if (lines[i].alpha > box_alpha)
			box_alpha = lines[i].alpha;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	Draw_Fill((float)x, (float)y, (float)boxw, (float)boxh, 0, 0, 0, (int)(150 * box_alpha));
	for (int i = first; i < count; i++) {
		const ccline_t *l = &lines[i];
		const char *s = l->c->text + l->start;
		int lx = (vid_width - TextWidth(s, l->len)) / 2;
		DrawCaptionText(lx, y + pad + (i - first) * lh, s, l->len, l->c->r, l->c->g, l->c->b, (int)(255 * l->alpha));
	}
	glColor4f(1, 1, 1, 1);
}

// ---------------------------------------------------------------- commands

static void Reload_f(void)
{
	LoadCaptions();
	Con_Printf("Captions (%s): %d lines, %d word spellings, %d speakers\n", loaded_lang, tokens.count, words.count,
		speakers.count);
}

// cc_emit <sentence, sound path or text>: shows a caption as if the sound had played
static void Emit_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: cc_emit <sentence name, sound path or text>\n");
		return;
	}
	const char *arg = Cmd_Argc() == 2 ? Cmd_Argv(1) : Cmd_Args();
	sentence_t *s = S_FindSentence(arg);
	const char *line = Find(&tokens, arg);
	if (s || line)
		CC_SoundStarted(s ? s->name : arg, s, 3, 1);
	else
		CC_Add(arg, 3, 255, 255, 255);
}

void CC_Init(void)
{
	closecaption = Cvar_Get("closecaption", "1", FCVAR_ARCHIVE, "shows captions for speech and sounds");
	cc_subtitles = Cvar_Get("cc_subtitles", "0", FCVAR_ARCHIVE, "1 = speech only, no sound-effect captions");
	cc_lang = Cvar_Get("cc_lang", "english", FCVAR_ARCHIVE, "caption language (captions/<lang>.txt)");
	cc_lines = Cvar_Get("cc_lines", "4", FCVAR_ARCHIVE, "most caption lines on screen");
	cc_linger = Cvar_Get("cc_linger", "1", FCVAR_ARCHIVE, "seconds a caption stays after its sound ends");
	cc_minvolume = Cvar_Get("cc_minvolume", "0.05", FCVAR_ARCHIVE, "quietest sound that gets a caption (0..1, before the volume setting)");
	cc_font = Cvar_Get("cc_font", "Trebuchet MS", FCVAR_ARCHIVE, "caption font face");
	Cmd_AddCommand("cc_reload", Reload_f, "reloads the caption files");
	Cmd_AddCommand("cc_emit", Emit_f, "cc_emit <sentence, sound path or text> - shows a caption");
	Cmd_AddCommand("cc_flush", CC_Clear, "clears the captions on screen");
	LoadCaptions();
}

void CC_Shutdown(void)
{
	CC_Clear();
	Mem_Free(pending);
	Mem_Free(active);
	Mem_Free(lines);
	pending = active = NULL;
	lines = NULL;
	pending_cap = active_cap = lines_cap = 0;
	FreeList(&tokens);
	FreeList(&words);
	FreeList(&speakers);
	if (ccfont.tex)
		glDeleteTextures(1, &ccfont.tex);
	Mem_Free(ccfont.atlas.alpha);
	memset(&ccfont, 0, sizeof(ccfont));
}
