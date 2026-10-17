// newen - command buffer, commands and aliases
#include "common.h"

typedef struct cmd_s {
	char      *name;
	char      *desc;
	xcommand_t fn;
	int        flags;
	struct cmd_s *next;
} cmd_t;

typedef struct alias_s {
	char *name;
	char *value;
	struct alias_s *next;
} alias_t;

static cmd_t   *cmd_list;
static alias_t *alias_list;
static strbuf_t cmd_text;
static int      cmd_wait;
static int      alias_depth;

qboolean (*cmd_fallback)(void);
qboolean cmd_defer_cvars;

// ---------------------------------------------------------------- buffer

void Cbuf_AddText(const char *text)
{
	SB_Append(&cmd_text, text, strlen(text));
}

void Cbuf_InsertText(const char *text)
{
	strbuf_t sb = { 0 };
	size_t n = strlen(text);
	SB_Append(&sb, text, n);
	if (n && text[n - 1] != '\n')
		SB_Append(&sb, "\n", 1);
	if (cmd_text.len)
		SB_Append(&sb, cmd_text.data, cmd_text.len);
	SB_Free(&cmd_text);
	cmd_text = sb;
}

void Cbuf_Execute(void)
{
	int guard = 0;
	while (cmd_text.len) {
		if (cmd_wait) {
			cmd_wait--;
			break;
		}
		if (++guard > 100000) {
			Con_Errorf("Cbuf_Execute: runaway command loop, buffer cleared\n");
			SB_Clear(&cmd_text);
			break;
		}

		// find the end of one command: newline, or ';' outside quotes
		size_t i, quotes = 0;
		for (i = 0; i < cmd_text.len; i++) {
			char c = cmd_text.data[i];
			if (c == '"')
				quotes++;
			if (!(quotes & 1) && c == ';')
				break;
			if (c == '\n' || c == '\r')
				break;
		}

		char *line = Mem_Alloc(i + 1);
		memcpy(line, cmd_text.data, i);
		line[i] = 0;

		// remove it before executing, the command may insert more text
		if (i < cmd_text.len)
			i++;
		memmove(cmd_text.data, cmd_text.data + i, cmd_text.len - i);
		cmd_text.len -= i;
		cmd_text.data[cmd_text.len] = 0;

		Cmd_ExecuteString(line);
		Mem_Free(line);
	}
	alias_depth = 0;
}

// ---------------------------------------------------------------- arguments

static char   **cmd_argv;
static int      cmd_argc, cmd_argcap;
static strbuf_t cmd_args;

void Cmd_TokenizeString(const char *text)
{
	for (int i = 0; i < cmd_argc; i++)
		Mem_Free(cmd_argv[i]);
	cmd_argc = 0;
	SB_Clear(&cmd_args);

	for (;;) {
		while (*text && *text != '\n' && (unsigned char)*text <= ' ')
			text++;
		if (!*text || *text == '\n')
			break;
		if (text[0] == '/' && text[1] == '/')
			break;   // comment to end of line
		if (cmd_argc == 1) {
			const char *end = text + strlen(text);
			while (end > text && (unsigned char)end[-1] <= ' ')
				end--;
			SB_Append(&cmd_args, text, end - text);
		}
		// commands are not split on ':' or '(' like map files are
		const char *start = text;
		size_t len = 0;
		char tok[8192];
		if (*text == '"') {
			text++;
			while (*text && *text != '"' && len < sizeof(tok) - 1)
				tok[len++] = *text++;
			if (*text == '"')
				text++;
		} else {
			while ((unsigned char)*text > ' ' && len < sizeof(tok) - 1)
				tok[len++] = *text++;
		}
		tok[len] = 0;
		if (text == start)
			break;
		if (cmd_argc == cmd_argcap) {
			cmd_argcap = cmd_argcap ? cmd_argcap * 2 : 16;
			cmd_argv = Mem_Realloc(cmd_argv, sizeof(*cmd_argv) * cmd_argcap);
		}
		cmd_argv[cmd_argc++] = Mem_Strdup(tok);
	}
}

int Cmd_Argc(void)
{
	return cmd_argc;
}

const char *Cmd_Argv(int i)
{
	return (i >= 0 && i < cmd_argc) ? cmd_argv[i] : "";
}

const char *Cmd_Args(void)
{
	return cmd_args.data ? cmd_args.data : "";
}

// ---------------------------------------------------------------- commands

static cmd_t *FindCmd(const char *name)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!stricmp(c->name, name))
			return c;
	return NULL;
}

qboolean Cmd_Exists(const char *name)
{
	return FindCmd(name) != NULL;
}

void Cmd_AddCommandFlags(const char *name, xcommand_t fn, const char *desc, int flags)
{
	if (Cvar_Find(name)) {
		Con_Warnf("Cmd_AddCommand: \"%s\" is already a cvar\n", name);
		return;
	}
	cmd_t *c = FindCmd(name);
	if (c) {
		// Lua may re-register its own commands; engine commands can't be replaced
		if ((c->flags & CMD_LUA) && (flags & CMD_LUA)) {
			c->fn = fn;
			return;
		}
		Con_Warnf("Cmd_AddCommand: \"%s\" already defined\n", name);
		return;
	}
	c = Mem_Alloc(sizeof(*c));
	c->name = Mem_Strdup(name);
	c->desc = desc ? Mem_Strdup(desc) : NULL;
	c->fn = fn;
	c->flags = flags;

	// keep the list sorted for cmdlist and completion
	cmd_t **prev = &cmd_list;
	while (*prev && stricmp((*prev)->name, name) < 0)
		prev = &(*prev)->next;
	c->next = *prev;
	*prev = c;
}

void Cmd_AddCommand(const char *name, xcommand_t fn, const char *desc)
{
	Cmd_AddCommandFlags(name, fn, desc, 0);
}

static void FreeCmd(cmd_t *c)
{
	Mem_Free(c->name);
	Mem_Free(c->desc);
	Mem_Free(c);
}

void Cmd_RemoveCommand(const char *name)
{
	for (cmd_t **prev = &cmd_list; *prev; prev = &(*prev)->next) {
		if (!stricmp((*prev)->name, name)) {
			cmd_t *c = *prev;
			*prev = c->next;
			FreeCmd(c);
			return;
		}
	}
}

void Cmd_RemoveFlagged(int flags)
{
	cmd_t **prev = &cmd_list;
	while (*prev) {
		cmd_t *c = *prev;
		if (c->flags & flags) {
			*prev = c->next;
			FreeCmd(c);
		} else {
			prev = &c->next;
		}
	}
}

void Cmd_CompleteNames(const char *prefix, cmd_completefn_t fn, void *ctx)
{
	size_t n = strlen(prefix);
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!strnicmp(c->name, prefix, n))
			fn(c->name, ctx);
	for (alias_t *a = alias_list; a; a = a->next)
		if (!strnicmp(a->name, prefix, n))
			fn(a->name, ctx);
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (!strnicmp(v->name, prefix, n))
			fn(v->name, ctx);
}

void Cmd_ExecuteString(const char *text)
{
	Cmd_TokenizeString(text);
	if (!Cmd_Argc())
		return;

	cmd_t *c = FindCmd(cmd_argv[0]);
	if (c) {
		if (c->fn)
			c->fn();
		return;
	}

	for (alias_t *a = alias_list; a; a = a->next) {
		if (!stricmp(a->name, cmd_argv[0])) {
			if (++alias_depth > 256) {
				Con_Errorf("alias \"%s\" recursed too deep\n", a->name);
				return;
			}
			Cbuf_InsertText(a->value);
			return;
		}
	}

	if (Cvar_Command())
		return;
	if (cmd_fallback && cmd_fallback())
		return;
	// While the saved config runs, settings for cvars that don't exist yet
	// (created later by a subsystem or by Lua) are kept until they appear.
	if (cmd_defer_cvars && Cmd_Argc() == 2) {
		Cvar_SetPending(cmd_argv[0], cmd_argv[1]);
		return;
	}

	Con_Printf("Unknown command \"%s\"\n", cmd_argv[0]);
}

// ---------------------------------------------------------------- built-ins

void Cmd_ExecFile(const char *path, qboolean quiet)
{
	size_t len;
	char *data = FS_LoadFile(path, &len);
	if (!data) {
		if (!quiet)
			Con_Printf("couldn't exec %s\n", path);
		return;
	}
	if (!quiet)
		Con_DPrintf("execing %s\n", path);
	Cbuf_InsertText(data);
	Mem_Free(data);
}

static void Exec_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: exec <file.cfg>\n");
		return;
	}
	const char *name = Cmd_Argv(1);
	if (strstr(name, "..")) {
		Con_Printf("exec: relative paths aren't allowed\n");
		return;
	}
	Cmd_ExecFile(name, false);
}

static void Echo_f(void)
{
	Con_ColorPrint(COLOR_ECHO, va("%s\n", Cmd_Args()));
}

static void Wait_f(void)
{
	cmd_wait = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 1;
	if (cmd_wait < 1)
		cmd_wait = 1;
}

static void Alias_f(void)
{
	if (Cmd_Argc() == 1) {
		for (alias_t *a = alias_list; a; a = a->next)
			Con_Printf("%s : %s\n", a->name, a->value);
		return;
	}
	const char *name = Cmd_Argv(1);
	alias_t *a;
	for (a = alias_list; a; a = a->next)
		if (!stricmp(a->name, name))
			break;
	if (Cmd_Argc() == 2) {
		if (a)
			Con_Printf("%s : %s\n", a->name, a->value);
		return;
	}
	// rebuild the value from argv so quotes inside behave like GoldSrc
	strbuf_t sb = { 0 };
	for (int i = 2; i < Cmd_Argc(); i++) {
		if (i > 2)
			SB_Append(&sb, " ", 1);
		SB_Append(&sb, Cmd_Argv(i), strlen(Cmd_Argv(i)));
	}
	SB_Append(&sb, "\n", 1);
	if (!a) {
		a = Mem_Alloc(sizeof(*a));
		a->name = Mem_Strdup(name);
		a->next = alias_list;
		alias_list = a;
	} else {
		Mem_Free(a->value);
	}
	a->value = sb.data;
}

static void Cmdlist_f(void)
{
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL;
	int n = 0;
	for (cmd_t *c = cmd_list; c; c = c->next) {
		if (filter && !Q_WildcardMatch(filter, c->name) && strnicmp(c->name, filter, strlen(filter)))
			continue;
		Con_Printf("%-24s %s\n", c->name, c->desc ? c->desc : "");
		n++;
	}
	Con_Printf("%d commands\n", n);
}

static void Find_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: find <text>  (searches command and cvar names and descriptions)\n");
		return;
	}
	const char *needle = Cmd_Argv(1);
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (strstr(c->name, needle) || (c->desc && strstr(c->desc, needle)))
			Con_Printf("%-24s %s\n", c->name, c->desc ? c->desc : "");
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (strstr(v->name, needle) || strstr(Cvar_Description(v), needle))
			Con_Printf("%-24s \"%s\" %s\n", v->name, v->string, Cvar_Description(v));
}

void Cmd_Init(void)
{
	Cmd_AddCommand("exec", Exec_f, "runs a config file");
	Cmd_AddCommand("echo", Echo_f, "prints text to the console");
	Cmd_AddCommand("wait", Wait_f, "delays the rest of the command buffer by N frames");
	Cmd_AddCommand("alias", Alias_f, "alias <name> <commands>");
	Cmd_AddCommand("cmdlist", Cmdlist_f, "lists commands, optionally filtered");
	Cmd_AddCommand("find", Find_f, "searches commands and cvars");
}
