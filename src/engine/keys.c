// newen - key bindings and key event routing
#include "common.h"
#include <ctype.h>

static char    *bindings[K_COUNT];
static qboolean keydown[K_COUNT];

typedef struct { const char *name; int key; } keyname_t;

static const keyname_t keynames[] = {
	{ "TAB", K_TAB }, { "ENTER", K_ENTER }, { "ESCAPE", K_ESCAPE }, { "SPACE", K_SPACE },
	{ "BACKSPACE", K_BACKSPACE },
	{ "UPARROW", K_UPARROW }, { "DOWNARROW", K_DOWNARROW },
	{ "LEFTARROW", K_LEFTARROW }, { "RIGHTARROW", K_RIGHTARROW },
	{ "ALT", K_ALT }, { "CTRL", K_CTRL }, { "SHIFT", K_SHIFT },
	{ "F1", K_F1 }, { "F2", K_F2 }, { "F3", K_F3 }, { "F4", K_F4 }, { "F5", K_F5 }, { "F6", K_F6 },
	{ "F7", K_F7 }, { "F8", K_F8 }, { "F9", K_F9 }, { "F10", K_F10 }, { "F11", K_F11 }, { "F12", K_F12 },
	{ "INS", K_INS }, { "DEL", K_DEL }, { "PGDN", K_PGDN }, { "PGUP", K_PGUP },
	{ "HOME", K_HOME }, { "END", K_END },
	{ "KP_HOME", K_KP_HOME }, { "KP_UPARROW", K_KP_UPARROW }, { "KP_PGUP", K_KP_PGUP },
	{ "KP_LEFTARROW", K_KP_LEFTARROW }, { "KP_5", K_KP_5 }, { "KP_RIGHTARROW", K_KP_RIGHTARROW },
	{ "KP_END", K_KP_END }, { "KP_DOWNARROW", K_KP_DOWNARROW }, { "KP_PGDN", K_KP_PGDN },
	{ "KP_ENTER", K_KP_ENTER }, { "KP_INS", K_KP_INS }, { "KP_DEL", K_KP_DEL },
	{ "KP_SLASH", K_KP_SLASH }, { "KP_MINUS", K_KP_MINUS }, { "KP_PLUS", K_KP_PLUS },
	{ "CAPSLOCK", K_CAPSLOCK },
	{ "MWHEELUP", K_MWHEELUP }, { "MWHEELDOWN", K_MWHEELDOWN },
	{ "MOUSE1", K_MOUSE1 }, { "MOUSE2", K_MOUSE2 }, { "MOUSE3", K_MOUSE3 },
	{ "MOUSE4", K_MOUSE4 }, { "MOUSE5", K_MOUSE5 },
	{ "PAUSE", K_PAUSE },
	{ "SEMICOLON", ';' },
	{ NULL, 0 }
};

int Key_StringToKeynum(const char *str)
{
	if (!str || !*str)
		return -1;
	if (!str[1])
		return tolower((unsigned char)str[0]);
	for (const keyname_t *kn = keynames; kn->name; kn++)
		if (!stricmp(str, kn->name))
			return kn->key;
	return -1;
}

const char *Key_KeynumToString(int key)
{
	static char single[2];
	if (key < 0 || key >= K_COUNT)
		return "<KEY NOT FOUND>";
	for (const keyname_t *kn = keynames; kn->name; kn++)
		if (kn->key == key)
			return kn->name;
	if (key > 32 && key < 127) {
		single[0] = (char)key;
		single[1] = 0;
		return single;
	}
	return "<UNKNOWN KEYNUM>";
}

static void SetBinding(int key, const char *cmd)
{
	Mem_Free(bindings[key]);
	bindings[key] = (cmd && *cmd) ? Mem_Strdup(cmd) : NULL;
}

static void Bind_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: bind <key> [command]\n");
		return;
	}
	int key = Key_StringToKeynum(Cmd_Argv(1));
	if (key < 0) {
		Con_Printf("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}
	if (Cmd_Argc() == 2) {
		if (bindings[key])
			Con_Printf("\"%s\" = \"%s\"\n", Cmd_Argv(1), bindings[key]);
		else
			Con_Printf("\"%s\" is not bound\n", Cmd_Argv(1));
		return;
	}
	strbuf_t sb = { 0 };
	for (int i = 2; i < Cmd_Argc(); i++) {
		if (i > 2)
			SB_Append(&sb, " ", 1);
		SB_Append(&sb, Cmd_Argv(i), strlen(Cmd_Argv(i)));
	}
	SetBinding(key, sb.data);
	SB_Free(&sb);
}

static void Unbind_f(void)
{
	int key = Cmd_Argc() > 1 ? Key_StringToKeynum(Cmd_Argv(1)) : -1;
	if (key < 0) {
		Con_Printf("usage: unbind <key>\n");
		return;
	}
	SetBinding(key, NULL);
}

static char *default_bindings[K_COUNT];

static void Unbindall_f(void)
{
	// configs written before the movement keys existed start with "unbindall"; while the saved
	// config loads, start from the defaults instead so new default keys survive
	for (int i = 0; i < K_COUNT; i++)
		SetBinding(i, cmd_defer_cvars ? default_bindings[i] : NULL);
}

static void Bindlist_f(void)
{
	for (int i = 0; i < K_COUNT; i++)
		if (bindings[i])
			Con_Printf("%-12s \"%s\"\n", Key_KeynumToString(i), bindings[i]);
}

// the first key bound to a command, for the client DLL's "press [key]" hints
const char *Key_KeynameForBinding(const char *binding)
{
	if (!binding)
		return NULL;
	for (int i = 0; i < K_COUNT; i++)
		if (bindings[i] && !stricmp(bindings[i], binding))
			return Key_KeynumToString(i);
	return NULL;
}

// remembers the engine's default keys, so the config only has to store changes
void Key_SnapshotDefaults(void)
{
	for (int i = 0; i < K_COUNT; i++) {
		Mem_Free(default_bindings[i]);
		default_bindings[i] = bindings[i] ? Mem_Strdup(bindings[i]) : NULL;
	}
}

// Only differences from the defaults are written, so keys added to the defaults later still show up
// for people with an older config
void Key_WriteBindings(FILE *f)
{
	for (int i = 0; i < K_COUNT; i++) {
		const char *def = default_bindings[i];
		if (bindings[i] && (!def || strcmp(def, bindings[i])))
			fprintf(f, "bind \"%s\" \"%s\"\n", Key_KeynumToString(i), bindings[i]);
		else if (!bindings[i] && def)
			fprintf(f, "unbind \"%s\"\n", Key_KeynumToString(i));
	}
}

qboolean Key_IsDown(int key)
{
	return key >= 0 && key < K_COUNT && keydown[key];
}

void Key_ClearStates(void)
{
	for (int i = 0; i < K_COUNT; i++) {
		if (keydown[i] && bindings[i] && bindings[i][0] == '+')
			Cbuf_AddText(va("-%s %d\n", bindings[i] + 1, i));
		keydown[i] = false;
	}
}

void Key_Event(int key, qboolean down)
{
	if (key < 0 || key >= K_COUNT)
		return;
	qboolean repeat = down && keydown[key];
	keydown[key] = down;

	// the console key always works
	if (key == '`' && down && !repeat) {
		Con_ToggleConsole();
		return;
	}

	if (Con_IsVisible()) {
		if (down)
			Con_KeyEvent(key);
		return;
	}

	if (Lua_KeyEvent(key, down))
		return;

	if (key == K_ESCAPE && down && !bindings[key]) {
		Con_ToggleConsole();   // until there's a menu
		return;
	}

	const char *b = bindings[key];
	// the client DLL sees keys first (weapon menus use the number keys)
	if (!repeat && !CL_KeyEvent(key, down, b ? b : ""))
		return;
	if (!b || repeat)
		return;
	if (b[0] == '+') {
		// GoldSrc passes the key number so +/- commands can track several keys
		if (down)
			Cbuf_AddText(va("%s %d\n", b, key));
		else
			Cbuf_AddText(va("-%s %d\n", b + 1, key));
	} else if (down) {
		Cbuf_AddText(b);
		Cbuf_AddText("\n");
	}
}

void Key_Init(void)
{
	Cmd_AddCommand("bind", Bind_f, "bind <key> <command>");
	Cmd_AddCommand("unbind", Unbind_f, "removes a key's binding");
	Cmd_AddCommand("unbindall", Unbindall_f, "removes every binding");
	Cmd_AddCommand("bindlist", Bindlist_f, "lists key bindings");
}
