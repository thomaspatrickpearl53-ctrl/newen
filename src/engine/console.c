// newen - console: scrollback, input line, history, completion, log file
#include "common.h"
#include <ctype.h>

typedef struct {
	char  *text;
	int    color;
	double time;    // when printed, for notify lines
} conline_t;

static conline_t *lines;
static int  lines_count, lines_cap, lines_first;   // ring buffer
static strbuf_t partial;                            // line still being printed
static int  partial_color;

static FILE *logfile;
static qboolean con_visible, con_forced, con_ready;
static float con_frac;      // slide animation 0..1
static int   con_scroll;    // lines scrolled up from the bottom

static char input[1024];
static int  input_cursor;
static char **history;
static int  history_count, history_pos;

static cvar_t *con_maxlines, *con_notifytime, *con_notifylines, *con_height, *con_speed;

// ---------------------------------------------------------------- output

static void AddLine(const char *text, int color)
{
	int max = con_maxlines ? (int)con_maxlines->value : 8192;
	if (max < 64)
		max = 64;
	if (lines_cap != max) {
		// resize: keep the newest lines
		conline_t *nl = Mem_Alloc(sizeof(*nl) * max);
		int keep = lines_count < max ? lines_count : max;
		for (int i = 0; i < keep; i++)
			nl[i] = lines[(lines_first + lines_count - keep + i) % lines_cap];
		for (int i = 0; i < lines_count - keep; i++)
			Mem_Free(lines[(lines_first + i) % lines_cap].text);
		Mem_Free(lines);
		lines = nl;
		lines_cap = max;
		lines_count = keep;
		lines_first = 0;
	}
	conline_t *l;
	if (lines_count == lines_cap) {
		l = &lines[lines_first];
		Mem_Free(l->text);
		lines_first = (lines_first + 1) % lines_cap;
	} else {
		l = &lines[(lines_first + lines_count) % lines_cap];
		lines_count++;
	}
	l->text = Mem_Strdup(text);
	l->color = color;
	l->time = host_realtime;
	if (con_scroll)
		con_scroll++;   // keep the view where the reader left it
}

void Con_ColorPrint(int color, const char *text)
{
	fputs(text, stdout);
	if (logfile) {
		fputs(text, logfile);
		fflush(logfile);
	}
	for (const char *p = text; *p; p++) {
		if (*p == '\n') {
			AddLine(partial.data ? partial.data : "", partial.len ? partial_color : color);
			SB_Clear(&partial);
		} else if (*p != '\r') {
			if (!partial.len)
				partial_color = color;
			SB_Append(&partial, p, 1);
		}
	}
}

static void VPrint(int color, const char *fmt, va_list ap)
{
	char buf[16384];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	Con_ColorPrint(color, buf);
}

void Con_Printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	VPrint(COLOR_DEFAULT, fmt, ap);
	va_end(ap);
}

void Con_DPrintf(const char *fmt, ...)
{
	if (!developer || developer->value <= 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	VPrint(COLOR_DEFAULT, fmt, ap);
	va_end(ap);
}

void Con_Warnf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	VPrint(COLOR_WARN, fmt, ap);
	va_end(ap);
}

void Con_Errorf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	VPrint(COLOR_ERROR, fmt, ap);
	va_end(ap);
}

// ---------------------------------------------------------------- visibility

void Con_ToggleConsole(void)
{
	if (con_forced)
		return;
	con_visible = !con_visible;
	con_scroll = 0;
	Key_ClearStates();
	Vid_SetTextInput(con_visible);
}

qboolean Con_IsVisible(void)
{
	return con_visible || con_forced;
}

void Con_SetForced(qboolean forced)
{
	con_forced = forced;
	Vid_SetTextInput(Con_IsVisible());
}

static void ToggleConsole_f(void) { Con_ToggleConsole(); }

static void Clear_f(void)
{
	for (int i = 0; i < lines_count; i++)
		Mem_Free(lines[(lines_first + i) % lines_cap].text);
	lines_count = lines_first = con_scroll = 0;
}

static void Condump_f(void)
{
	const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "condump.txt";
	FILE *f = FS_OpenWrite(name);
	if (!f) {
		Con_Errorf("condump: couldn't write %s\n", name);
		return;
	}
	for (int i = 0; i < lines_count; i++)
		fprintf(f, "%s\n", lines[(lines_first + i) % lines_cap].text);
	fclose(f);
	Con_Printf("Dumped console to %s/%s\n", FS_WriteDir(), name);
}

// ---------------------------------------------------------------- input line

static void HistoryAdd(const char *s)
{
	if (history_count && !strcmp(history[history_count - 1], s))
		return;
	history = Mem_Realloc(history, sizeof(*history) * (history_count + 1));
	history[history_count++] = Mem_Strdup(s);
}

typedef struct {
	char   first[256];
	char   common[256];
	int    count;
	char **names;
} completion_t;

static void CollectName(const char *name, void *ctx)
{
	completion_t *c = ctx;
	for (int i = 0; i < c->count; i++)
		if (!stricmp(c->names[i], name))
			return;
	c->names = Mem_Realloc(c->names, sizeof(char *) * (c->count + 1));
	c->names[c->count++] = (char *)name;
	if (c->count == 1) {
		Q_strncpy(c->common, name, sizeof(c->common));
	} else {
		int i = 0;
		while (c->common[i] && tolower((unsigned char)c->common[i]) == tolower((unsigned char)name[i]))
			i++;
		c->common[i] = 0;
	}
}

static int CompareNames(const void *a, const void *b)
{
	return stricmp(*(char *const *)a, *(char *const *)b);
}

static void CompleteInput(void)
{
	// only complete the first word
	char *start = input;
	while (*start == ' ' || *start == '/' || *start == '\\')
		start++;
	if (strchr(start, ' ') || !*start)
		return;

	completion_t c = { 0 };
	Cmd_CompleteNames(start, CollectName, &c);
	if (c.count == 1) {
		snprintf(input, sizeof(input), "%s ", c.names[0]);
	} else if (c.count > 1) {
		Con_Printf("]%s\n", input);
		qsort(c.names, c.count, sizeof(char *), CompareNames);
		for (int i = 0; i < c.count; i++) {
			cvar_t *v = Cvar_Find(c.names[i]);
			if (v)
				Con_Printf("  %s \"%s\"\n", c.names[i], v->string);
			else
				Con_Printf("  %s\n", c.names[i]);
		}
		Q_strncpy(input, c.common, sizeof(input));
	}
	input_cursor = (int)strlen(input);
	Mem_Free(c.names);
}

void Con_KeyEvent(int key)
{
	int len = (int)strlen(input);
	qboolean ctrl = Key_IsDown(K_CTRL);

	switch (key) {
	case K_ENTER:
	case K_KP_ENTER: {
		const char *cmd = input;
		while (*cmd == '/' || *cmd == '\\')
			cmd++;
		Con_ColorPrint(COLOR_ECHO, va("]%s\n", input));
		if (*cmd) {
			HistoryAdd(input);
			Cbuf_AddText(cmd);
			Cbuf_AddText("\n");
		}
		history_pos = history_count;
		input[0] = 0;
		input_cursor = 0;
		con_scroll = 0;
		return;
	}
	case K_TAB:
		CompleteInput();
		return;
	case K_BACKSPACE:
		if (input_cursor > 0) {
			memmove(input + input_cursor - 1, input + input_cursor, len - input_cursor + 1);
			input_cursor--;
		}
		return;
	case K_DEL:
		if (input_cursor < len)
			memmove(input + input_cursor, input + input_cursor + 1, len - input_cursor);
		return;
	case K_LEFTARROW:
		if (input_cursor > 0)
			input_cursor--;
		return;
	case K_RIGHTARROW:
		if (input_cursor < len)
			input_cursor++;
		return;
	case K_HOME:
		if (ctrl)
			con_scroll = lines_count;
		else
			input_cursor = 0;
		return;
	case K_END:
		if (ctrl)
			con_scroll = 0;
		else
			input_cursor = len;
		return;
	case K_UPARROW:
		if (history_pos > 0) {
			history_pos--;
			Q_strncpy(input, history[history_pos], sizeof(input));
			input_cursor = (int)strlen(input);
		}
		return;
	case K_DOWNARROW:
		if (history_pos < history_count) {
			history_pos++;
			Q_strncpy(input, history_pos < history_count ? history[history_pos] : "", sizeof(input));
			input_cursor = (int)strlen(input);
		}
		return;
	case K_PGUP:
	case K_MWHEELUP:
		con_scroll += (key == K_PGUP) ? 10 : 3;
		if (con_scroll > lines_count - 1)
			con_scroll = lines_count > 0 ? lines_count - 1 : 0;
		return;
	case K_PGDN:
	case K_MWHEELDOWN:
		con_scroll -= (key == K_PGDN) ? 10 : 3;
		if (con_scroll < 0)
			con_scroll = 0;
		return;
	case K_ESCAPE:
		if (con_visible && !con_forced)
			Con_ToggleConsole();
		return;
	}

	if (ctrl && (key == 'l')) {
		Clear_f();
		return;
	}
	if (ctrl && (key == 'v')) {
		char *clip = Vid_GetClipboard();
		if (clip) {
			for (char *p = clip; *p; p++)
				if ((unsigned char)*p >= 32)
					Con_CharEvent((unsigned char)*p);
			Mem_Free(clip);
		}
		return;
	}
}

void Con_CharEvent(int ch)
{
	if (ch == '`' || ch == '~')
		return;   // the toggle key never types
	if (ch < 32 || ch > 126)
		return;
	int len = (int)strlen(input);
	if (len >= (int)sizeof(input) - 1)
		return;
	memmove(input + input_cursor + 1, input + input_cursor, len - input_cursor + 1);
	input[input_cursor++] = (char)ch;
}

// ---------------------------------------------------------------- drawing

static const int colors[][3] = {
	{ 230, 230, 230 },  // default
	{ 255, 200,  80 },  // warn
	{ 255,  90,  90 },  // error
	{ 120, 200, 255 },  // lua
	{ 160, 160, 160 },  // echo
};

static void DrawLine(float x, float y, const conline_t *l)
{
	const int *c = colors[l->color < 5 ? l->color : 0];
	Draw_String(x, y, l->text, c[0], c[1], c[2], 255);
}

static void DrawNotify(void)
{
	int max = (int)con_notifylines->value;
	float lh = (float)Draw_LineHeight();
	float y = 4;
	int start = lines_count - max;
	if (start < 0)
		start = 0;
	for (int i = start; i < lines_count; i++) {
		conline_t *l = &lines[(lines_first + i) % lines_cap];
		if (host_realtime - l->time > con_notifytime->value)
			continue;
		DrawLine(6, y, l);
		y += lh;
	}
}

void Con_Draw(void)
{
	float target = Con_IsVisible() ? 1.0f : 0.0f;
	float speed = con_speed->value > 0 ? con_speed->value : 1000.0f;
	float step = (float)(host_frametime * speed / 100.0);
	if (con_forced)
		con_frac = 1.0f;
	else if (con_frac < target)
		con_frac = con_frac + step > target ? target : con_frac + step;
	else if (con_frac > target)
		con_frac = con_frac - step < target ? target : con_frac - step;

	if (con_frac <= 0.0f) {
		DrawNotify();
		return;
	}

	float h = con_forced ? (float)vid_height : vid_height * con_height->value * con_frac;
	float lh = (float)Draw_LineHeight();
	Draw_Fill(0, 0, (float)vid_width, h, 12, 14, 18, 235);
	Draw_Fill(0, h - 2, (float)vid_width, 2, 255, 140, 0, 255);

	// version tag
	const char *tag = ENGINE_NAME " " ENGINE_VERSION;
	Draw_String(vid_width - Draw_StringWidth(tag) - 8.0f, h - lh * 2 - 6, tag, 255, 140, 0, 200);

	// input line with cursor
	float y = h - lh - 6;
	char shown[1100];
	snprintf(shown, sizeof(shown), "]%s", input);
	Draw_String(6, y, shown, 255, 255, 255, 255);
	if (((int)(host_realtime * 3)) & 1) {
		char pre[1100];
		Q_strncpy(pre, shown, (size_t)input_cursor + 2);
		float cx = 6.0f + Draw_StringWidth(pre);
		Draw_Fill(cx, y + lh - 3, (float)Draw_CharWidth(), 2, 255, 255, 255, 255);
	}

	// scrollback, newest at the bottom
	y -= lh + 4;
	if (con_scroll) {
		Draw_String(6, y, "^ ^ ^ ^ ^ ^ ^ ^", 255, 140, 0, 255);
		y -= lh;
	}
	for (int i = lines_count - 1 - con_scroll; i >= 0 && y > -lh; i--, y -= lh)
		DrawLine(6, y, &lines[(lines_first + i) % lines_cap]);
}

// ---------------------------------------------------------------- init

void Con_Init(void)
{
	con_maxlines = Cvar_Get("con_maxlines", "8192", FCVAR_ARCHIVE, "scrollback size in lines");
	con_notifytime = Cvar_Get("con_notifytime", "4", FCVAR_ARCHIVE, "seconds a printed line stays on screen");
	con_notifylines = Cvar_Get("con_notifylines", "4", FCVAR_ARCHIVE, "lines shown at the top while the console is closed");
	con_height = Cvar_Get("con_height", "0.5", FCVAR_ARCHIVE, "console height as a fraction of the screen");
	con_speed = Cvar_Get("con_speed", "600", FCVAR_ARCHIVE, "console slide speed");

	Cmd_AddCommand("toggleconsole", ToggleConsole_f, "opens or closes the console");
	Cmd_AddCommand("clear", Clear_f, "clears the console");
	Cmd_AddCommand("condump", Condump_f, "writes the console text to a file");
	con_ready = true;
}

void Con_OpenLog(const char *path)
{
	if (logfile)
		fclose(logfile);
	logfile = fopen(path, "w");
	// write what was printed before the log opened
	if (logfile)
		for (int i = 0; i < lines_count; i++)
			fprintf(logfile, "%s\n", lines[(lines_first + i) % lines_cap].text);
}

void Con_Shutdown(void)
{
	if (partial.len)
		Con_ColorPrint(COLOR_DEFAULT, "\n");
	if (logfile) {
		fclose(logfile);
		logfile = NULL;
	}
}
