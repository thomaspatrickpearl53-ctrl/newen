// newen - engine-level Lua 5.4
//
// Lua lives in the engine, so it works with any game. Script tree (merged, first match wins):
//   userdata/<game>/lua
//   userdata/<game>/addons/<id>/lua
//   base/addons/<id>/lua
//   base/lua                       core libraries (lua/includes/init.lua)
// On start: includes/init.lua, then every autorun/*.lua (merged, alphabetical), then hook "Initialize".
#include "common.h"
#include <ctype.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static lua_State *L;
static cvar_t *lua_timeout;
static double  call_deadline;
static int     call_depth;

static char  **roots;
static int     roots_count;

static char    last_error[1024];
static double  last_error_time;

#define CONCOMMANDS "newen.concommands"
#define CONVAR_MT   "ConVar"

// ---------------------------------------------------------------- calls and errors

static void TimeoutHook(lua_State *Ls, lua_Debug *ar)
{
	(void)ar;
	if (call_deadline > 0 && Sys_FloatTime() > call_deadline)
		luaL_error(Ls, "script ran longer than lua_timeout (%s s) and was stopped", lua_timeout->string);
}

static int Traceback(lua_State *Ls)
{
	const char *msg = lua_tostring(Ls, 1);
	if (!msg)
		msg = luaL_tolstring(Ls, 1, NULL);
	luaL_traceback(Ls, Ls, msg, 1);
	return 1;
}

static void ReportError(const char *where, const char *msg)
{
	// the same error every frame (a broken Think hook) is shown once every few seconds
	if (!strcmp(msg, last_error) && host_realtime - last_error_time < 5.0)
		return;
	Q_strncpy(last_error, msg, sizeof(last_error));
	last_error_time = host_realtime;
	Con_Errorf("[Lua] %s: %s\n", where, msg);
}

// Calls the function below nargs arguments. Leaves nres results on success; returns false on error.
static qboolean SafeCall(int nargs, int nres, const char *where)
{
	int base = lua_gettop(L) - nargs;
	lua_pushcfunction(L, Traceback);
	lua_insert(L, base);
	if (call_depth++ == 0) {
		double t = lua_timeout->value;
		call_deadline = t > 0 ? Sys_FloatTime() + t : 0;
	}
	int r = lua_pcall(L, nargs, nres, base);
	if (--call_depth == 0)
		call_deadline = 0;
	if (r != LUA_OK) {
		ReportError(where, lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error object isn't a string)");
		lua_pop(L, 2);
		return false;
	}
	lua_remove(L, base);
	return true;
}

// ---------------------------------------------------------------- script tree

static void AddRoot(const char *dir)
{
	if (!Sys_DirExists(dir))
		return;
	roots = Mem_Realloc(roots, sizeof(char *) * (roots_count + 1));
	roots[roots_count++] = Mem_Strdup(dir);
}

typedef struct { const char *base; } addonctx_t;

static int CompareStrings(const void *a, const void *b)
{
	return stricmp(*(char *const *)a, *(char *const *)b);
}

typedef struct { char **names; int count; } namelist_t;

static void CollectDir(const char *name, qboolean isdir, void *p)
{
	namelist_t *nl = p;
	if (!isdir)
		return;
	nl->names = Mem_Realloc(nl->names, sizeof(char *) * (nl->count + 1));
	nl->names[nl->count++] = Mem_Strdup(name);
}

static void AddAddonRoots(const char *addons_dir)
{
	namelist_t nl = { 0 };
	Sys_ListDir(addons_dir, CollectDir, &nl);
	if (nl.count)
		qsort(nl.names, nl.count, sizeof(char *), CompareStrings);
	for (int i = 0; i < nl.count; i++) {
		AddRoot(va("%s/%s/lua", addons_dir, nl.names[i]));
		Mem_Free(nl.names[i]);
	}
	Mem_Free(nl.names);
}

static void BuildRoots(void)
{
	for (int i = 0; i < roots_count; i++)
		Mem_Free(roots[i]);
	Mem_Free(roots);
	roots = NULL;
	roots_count = 0;

	AddRoot(va("%s/lua", FS_WriteDir()));
	AddAddonRoots(va("%s/addons", FS_WriteDir()));
	AddAddonRoots(va("%s/addons", FS_EngineDir()));
	AddRoot(va("%s/lua", FS_EngineDir()));
}

static qboolean SafeRel(const char *p)
{
	return p && *p && !strstr(p, "..") && p[0] != '/' && p[0] != '\\' && !strchr(p, ':');
}

static qboolean FindScript(const char *rel, char *out, size_t outsize)
{
	if (!SafeRel(rel))
		return false;
	for (int i = 0; i < roots_count; i++) {
		snprintf(out, outsize, "%s/%s", roots[i], rel);
		if (Sys_FileExists(out))
			return true;
	}
	return false;
}

typedef struct { const char *pattern; const char *dir; char **list; int count; } scriptlist_t;

static void CollectScript(const char *name, qboolean isdir, void *p)
{
	scriptlist_t *sl = p;
	if (isdir || !Q_WildcardMatch(sl->pattern, name))
		return;
	for (int i = 0; i < sl->count; i++)
		if (!stricmp(sl->list[i], name))
			return;
	sl->list = Mem_Realloc(sl->list, sizeof(char *) * (sl->count + 1));
	sl->list[sl->count++] = Mem_Strdup(name);
}

// File names (not paths) in dir across the tree, sorted
static int ListScripts(const char *dir, const char *pattern, char ***out)
{
	scriptlist_t sl = { pattern, dir, NULL, 0 };
	for (int i = 0; i < roots_count; i++)
		Sys_ListDir(va("%s/%s", roots[i], dir), CollectScript, &sl);
	if (sl.count)
		qsort(sl.list, sl.count, sizeof(char *), CompareStrings);
	*out = sl.list;
	return sl.count;
}

static void FreeList(char **list, int n)
{
	for (int i = 0; i < n; i++)
		Mem_Free(list[i]);
	Mem_Free(list);
}

// current file stack, for include() relative to the including file
static char  include_stack[32][MAX_OSPATH];
static int   include_depth;

// Loads and runs lua/<rel>. Pushes the chunk's results; returns count, or -1 on error (error already reported)
static int RunScript(const char *rel, qboolean report_missing)
{
	char full[MAX_OSPATH];
	char tried[MAX_OSPATH] = "";

	// relative to the including file first
	if (include_depth > 0) {
		const char *cur = include_stack[include_depth - 1];
		const char *slash = strrchr(cur, '/');
		if (slash) {
			snprintf(tried, sizeof(tried), "%.*s/%s", (int)(slash - cur), cur, rel);
			if (!FindScript(tried, full, sizeof(full)))
				tried[0] = 0;
		}
	}
	if (!tried[0]) {
		if (!FindScript(rel, full, sizeof(full))) {
			if (report_missing)
				ReportError("include", va("couldn't find lua/%s", rel));
			return -1;
		}
		Q_strncpy(tried, rel, sizeof(tried));
	}
	if (include_depth >= 32) {
		ReportError("include", "includes nested too deep");
		return -1;
	}

	FILE *f = fopen(full, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = Mem_Alloc((size_t)len + 1);
	len = (long)fread(src, 1, (size_t)len, f);
	fclose(f);

	int top = lua_gettop(L);
	int r = luaL_loadbuffer(L, src, (size_t)len, va("@lua/%s", tried));
	Mem_Free(src);
	if (r != LUA_OK) {
		ReportError("load", lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	Q_strncpy(include_stack[include_depth++], tried, MAX_OSPATH);
	qboolean ok = SafeCall(0, LUA_MULTRET, tried);
	include_depth--;
	return ok ? lua_gettop(L) - top : -1;
}

// ---------------------------------------------------------------- base functions

static int l_print(lua_State *Ls)
{
	strbuf_t sb = { 0 };
	int n = lua_gettop(Ls);
	for (int i = 1; i <= n; i++) {
		size_t len;
		const char *s = luaL_tolstring(Ls, i, &len);
		if (i > 1)
			SB_Append(&sb, "\t", 1);
		SB_Append(&sb, s, len);
		lua_pop(Ls, 1);
	}
	SB_Append(&sb, "\n", 1);
	Con_ColorPrint(COLOR_LUA, sb.data);
	SB_Free(&sb);
	return 0;
}

static int l_Msg(lua_State *Ls)
{
	int n = lua_gettop(Ls);
	for (int i = 1; i <= n; i++) {
		Con_ColorPrint(COLOR_LUA, luaL_tolstring(Ls, i, NULL));
		lua_pop(Ls, 1);
	}
	return 0;
}

static int l_MsgN(lua_State *Ls)
{
	l_Msg(Ls);
	Con_ColorPrint(COLOR_LUA, "\n");
	return 0;
}

static int l_include(lua_State *Ls)
{
	const char *rel = luaL_checkstring(Ls, 1);
	int n = RunScript(rel, true);
	return n < 0 ? 0 : n;
}

static int l_CurTime(lua_State *Ls)   { lua_pushnumber(Ls, host_realtime); return 1; }
static int l_RealTime(lua_State *Ls)  { lua_pushnumber(Ls, host_realtime); return 1; }
static int l_FrameTime(lua_State *Ls) { lua_pushnumber(Ls, host_frametime); return 1; }
static int l_SysTime(lua_State *Ls)   { lua_pushnumber(Ls, Sys_FloatTime()); return 1; }
static int l_ScrW(lua_State *Ls)      { lua_pushinteger(Ls, vid_width); return 1; }
static int l_ScrH(lua_State *Ls)      { lua_pushinteger(Ls, vid_height); return 1; }

static int l_RunConsoleCommand(lua_State *Ls)
{
	strbuf_t sb = { 0 };
	const char *cmd = luaL_checkstring(Ls, 1);
	if (strpbrk(cmd, ";\n\r\"")) {
		return luaL_error(Ls, "RunConsoleCommand: bad command name");
	}
	SB_Append(&sb, cmd, strlen(cmd));
	for (int i = 2; i <= lua_gettop(Ls); i++) {
		const char *a = luaL_tolstring(Ls, i, NULL);
		SB_Append(&sb, " \"", 2);
		for (const char *p = a; *p; p++)
			if (*p != '"' && *p != '\n' && *p != '\r')
				SB_Append(&sb, p, 1);
		SB_Append(&sb, "\"", 1);
		lua_pop(Ls, 1);
	}
	SB_Append(&sb, "\n", 1);
	Cbuf_AddText(sb.data);
	SB_Free(&sb);
	return 0;
}

// ---------------------------------------------------------------- concommand

static void LowerName(char *out, size_t size, const char *name)
{
	Q_strncpy(out, name, size);
	for (char *p = out; *p; p++)
		*p = (char)tolower((unsigned char)*p);
}

static void LuaCommand_f(void)
{
	char lower[256];
	LowerName(lower, sizeof(lower), Cmd_Argv(0));
	lua_getfield(L, LUA_REGISTRYINDEX, CONCOMMANDS);
	lua_getfield(L, -1, lower);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	// GMod signature: (ply, cmd, args, argStr); ply is nil for the engine console
	lua_pushnil(L);
	lua_pushstring(L, Cmd_Argv(0));
	lua_createtable(L, Cmd_Argc() - 1, 0);
	for (int i = 1; i < Cmd_Argc(); i++) {
		lua_pushstring(L, Cmd_Argv(i));
		lua_rawseti(L, -2, i);
	}
	lua_pushstring(L, Cmd_Args());
	SafeCall(4, 0, va("concommand %s", Cmd_Argv(0)));
}

static int l_concommand_Add(lua_State *Ls)
{
	const char *name = luaL_checkstring(Ls, 1);
	luaL_checktype(Ls, 2, LUA_TFUNCTION);
	const char *help = luaL_optstring(Ls, 4, NULL);
	if (!*name || strpbrk(name, " \t;\"\n"))
		return luaL_error(Ls, "concommand.Add: bad name \"%s\"", name);
	if (Cvar_Find(name))
		return luaL_error(Ls, "concommand.Add: \"%s\" is a cvar", name);
	char lower[256];
	LowerName(lower, sizeof(lower), name);
	if (Cmd_Exists(name)) {
		// only replace our own
		lua_getfield(Ls, LUA_REGISTRYINDEX, CONCOMMANDS);
		lua_getfield(Ls, -1, lower);
		qboolean ours = lua_isfunction(Ls, -1);
		lua_pop(Ls, 2);
		if (!ours)
			return luaL_error(Ls, "concommand.Add: \"%s\" is an engine command", name);
	}
	lua_getfield(Ls, LUA_REGISTRYINDEX, CONCOMMANDS);
	lua_pushvalue(Ls, 2);
	lua_setfield(Ls, -2, lower);
	lua_pop(Ls, 1);
	Cmd_AddCommandFlags(name, LuaCommand_f, help, CMD_LUA);
	return 0;
}

static int l_concommand_Remove(lua_State *Ls)
{
	const char *name = luaL_checkstring(Ls, 1);
	char lower[256];
	LowerName(lower, sizeof(lower), name);
	lua_getfield(Ls, LUA_REGISTRYINDEX, CONCOMMANDS);
	lua_getfield(Ls, -1, lower);
	qboolean ours = lua_isfunction(Ls, -1);
	lua_pop(Ls, 1);
	if (ours) {
		lua_pushnil(Ls);
		lua_setfield(Ls, -2, lower);
		Cmd_RemoveCommand(name);
	}
	lua_pop(Ls, 1);
	return 0;
}

// ---------------------------------------------------------------- ConVar

static void PushConVar(lua_State *Ls, const char *name)
{
	char *ud = lua_newuserdatauv(Ls, strlen(name) + 1, 0);
	strcpy(ud, name);
	luaL_setmetatable(Ls, CONVAR_MT);
}

static cvar_t *CheckConVar(lua_State *Ls)
{
	const char *name = luaL_checkudata(Ls, 1, CONVAR_MT);
	cvar_t *v = Cvar_Find(name);
	if (!v)
		luaL_error(Ls, "convar \"%s\" no longer exists", name);
	return v;
}

static int l_CreateConVar(lua_State *Ls)
{
	const char *name = luaL_checkstring(Ls, 1);
	const char *def = luaL_tolstring(Ls, 2, NULL);
	int flags = 0;
	if (lua_isinteger(Ls, 3))
		flags = (int)lua_tointeger(Ls, 3);
	const char *help = luaL_optstring(Ls, 4, NULL);
	if (!*name || strpbrk(name, " \t;\"\n"))
		return luaL_error(Ls, "CreateConVar: bad name \"%s\"", name);
	if (Cmd_Exists(name))
		return luaL_error(Ls, "CreateConVar: \"%s\" is a command", name);
	// FCVAR_ARCHIVE values are saved; the config keeps them until the script creates the cvar again
	if (!Cvar_Get(name, def, (flags & FCVAR_ARCHIVE) | FCVAR_LUA, help))
		return luaL_error(Ls, "CreateConVar: couldn't create \"%s\"", name);
	PushConVar(Ls, name);
	return 1;
}

static int l_GetConVar(lua_State *Ls)
{
	cvar_t *v = Cvar_Find(luaL_checkstring(Ls, 1));
	if (!v)
		return 0;
	PushConVar(Ls, v->name);
	return 1;
}

static int cv_GetString(lua_State *Ls)  { lua_pushstring(Ls, CheckConVar(Ls)->string); return 1; }
static int cv_GetFloat(lua_State *Ls)   { lua_pushnumber(Ls, CheckConVar(Ls)->value); return 1; }
static int cv_GetInt(lua_State *Ls)     { lua_pushinteger(Ls, (lua_Integer)CheckConVar(Ls)->value); return 1; }
static int cv_GetBool(lua_State *Ls)    { lua_pushboolean(Ls, CheckConVar(Ls)->value != 0); return 1; }
static int cv_GetName(lua_State *Ls)    { lua_pushstring(Ls, CheckConVar(Ls)->name); return 1; }
static int cv_GetDefault(lua_State *Ls) { lua_pushstring(Ls, Cvar_Default(CheckConVar(Ls))); return 1; }
static int cv_GetHelpText(lua_State *Ls){ lua_pushstring(Ls, Cvar_Description(CheckConVar(Ls))); return 1; }

static int cv_SetString(lua_State *Ls)
{
	cvar_t *v = CheckConVar(Ls);
	if (v->flags & FCVAR_READONLY)
		return luaL_error(Ls, "%s is read-only", v->name);
	Cvar_DirectSet(v, luaL_tolstring(Ls, 2, NULL));
	return 0;
}

static int cv_SetBool(lua_State *Ls)
{
	cvar_t *v = CheckConVar(Ls);
	if (v->flags & FCVAR_READONLY)
		return luaL_error(Ls, "%s is read-only", v->name);
	Cvar_DirectSet(v, lua_toboolean(Ls, 2) ? "1" : "0");
	return 0;
}

static int cv_tostring(lua_State *Ls)
{
	const char *name = luaL_checkudata(Ls, 1, CONVAR_MT);
	lua_pushfstring(Ls, "ConVar [%s]", name);
	return 1;
}

// ---------------------------------------------------------------- surface (2D drawing)

static int draw_r = 255, draw_g = 255, draw_b = 255, draw_a = 255;
static int text_r = 255, text_g = 255, text_b = 255, text_a = 255;
static float text_x, text_y;
static qboolean in_draw;

static void ReadColor(lua_State *Ls, int idx, int *r, int *g, int *b, int *a)
{
	if (lua_istable(Ls, idx)) {
		lua_getfield(Ls, idx, "r"); *r = (int)luaL_optinteger(Ls, -1, 255);
		lua_getfield(Ls, idx, "g"); *g = (int)luaL_optinteger(Ls, -1, 255);
		lua_getfield(Ls, idx, "b"); *b = (int)luaL_optinteger(Ls, -1, 255);
		lua_getfield(Ls, idx, "a"); *a = (int)luaL_optinteger(Ls, -1, 255);
		lua_pop(Ls, 4);
	} else {
		*r = (int)luaL_checknumber(Ls, idx);
		*g = (int)luaL_checknumber(Ls, idx + 1);
		*b = (int)luaL_checknumber(Ls, idx + 2);
		*a = (int)luaL_optnumber(Ls, idx + 3, 255);
	}
}

static int CheckDrawing(lua_State *Ls)
{
	if (!in_draw)
		return luaL_error(Ls, "surface functions only work inside a drawing hook (DrawOverlay)");
	return 0;
}

static int l_SetDrawColor(lua_State *Ls) { ReadColor(Ls, 1, &draw_r, &draw_g, &draw_b, &draw_a); return 0; }
static int l_SetTextColor(lua_State *Ls) { ReadColor(Ls, 1, &text_r, &text_g, &text_b, &text_a); return 0; }

static int l_SetTextPos(lua_State *Ls)
{
	text_x = (float)luaL_checknumber(Ls, 1);
	text_y = (float)luaL_checknumber(Ls, 2);
	return 0;
}

static int l_DrawRect(lua_State *Ls)
{
	CheckDrawing(Ls);
	Draw_Fill((float)luaL_checknumber(Ls, 1), (float)luaL_checknumber(Ls, 2),
		(float)luaL_checknumber(Ls, 3), (float)luaL_checknumber(Ls, 4), draw_r, draw_g, draw_b, draw_a);
	return 0;
}

static int l_DrawOutlinedRect(lua_State *Ls)
{
	CheckDrawing(Ls);
	float x = (float)luaL_checknumber(Ls, 1), y = (float)luaL_checknumber(Ls, 2);
	float w = (float)luaL_checknumber(Ls, 3), h = (float)luaL_checknumber(Ls, 4);
	float t = (float)luaL_optnumber(Ls, 5, 1);
	Draw_Fill(x, y, w, t, draw_r, draw_g, draw_b, draw_a);
	Draw_Fill(x, y + h - t, w, t, draw_r, draw_g, draw_b, draw_a);
	Draw_Fill(x, y + t, t, h - 2 * t, draw_r, draw_g, draw_b, draw_a);
	Draw_Fill(x + w - t, y + t, t, h - 2 * t, draw_r, draw_g, draw_b, draw_a);
	return 0;
}

static int l_DrawText(lua_State *Ls)
{
	CheckDrawing(Ls);
	const char *s = luaL_checkstring(Ls, 1);
	text_x += Draw_String(text_x, text_y, s, text_r, text_g, text_b, text_a);
	return 0;
}

static int l_GetTextSize(lua_State *Ls)
{
	const char *s = luaL_checkstring(Ls, 1);
	lua_pushinteger(Ls, Draw_StringWidth(s));
	lua_pushinteger(Ls, Draw_LineHeight());
	return 2;
}

// ---------------------------------------------------------------- input

static int KeyArg(lua_State *Ls, int idx)
{
	if (lua_isinteger(Ls, idx))
		return (int)lua_tointeger(Ls, idx);
	return Key_StringToKeynum(luaL_checkstring(Ls, idx));
}

static int l_IsKeyDown(lua_State *Ls)    { lua_pushboolean(Ls, Key_IsDown(KeyArg(Ls, 1))); return 1; }
static int l_GetKeyName(lua_State *Ls)   { lua_pushstring(Ls, Key_KeynumToString((int)luaL_checkinteger(Ls, 1))); return 1; }
static int l_GetKeyCode(lua_State *Ls)   { lua_pushinteger(Ls, Key_StringToKeynum(luaL_checkstring(Ls, 1))); return 1; }

// ---------------------------------------------------------------- file

static int l_file_Read(lua_State *Ls)
{
	const char *path = luaL_checkstring(Ls, 1);
	const char *where = luaL_optstring(Ls, 2, "GAME");
	size_t len = 0;
	char *data = NULL;
	if (!stricmp(where, "LUA")) {
		char full[MAX_OSPATH];
		if (FindScript(path, full, sizeof(full))) {
			FILE *f = fopen(full, "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				len = (size_t)ftell(f);
				fseek(f, 0, SEEK_SET);
				data = Mem_Alloc(len + 1);
				len = fread(data, 1, len, f);
				fclose(f);
			}
		}
	} else if (!stricmp(where, "DATA")) {
		data = FS_LoadFile(va("data/%s", path), &len);
	} else {
		data = FS_LoadFile(path, &len);
	}
	if (!data)
		return 0;
	lua_pushlstring(Ls, data, len);
	Mem_Free(data);
	return 1;
}

static int l_file_Exists(lua_State *Ls)
{
	const char *path = luaL_checkstring(Ls, 1);
	const char *where = luaL_optstring(Ls, 2, "GAME");
	char full[MAX_OSPATH];
	if (!stricmp(where, "LUA"))
		lua_pushboolean(Ls, FindScript(path, full, sizeof(full)));
	else if (!stricmp(where, "DATA"))
		lua_pushboolean(Ls, FS_FileExists(va("data/%s", path)));
	else
		lua_pushboolean(Ls, FS_FileExists(path));
	return 1;
}

// file.Write(path, text) writes into userdata/<game>/data/
static int l_file_Write(lua_State *Ls)
{
	const char *path = luaL_checkstring(Ls, 1);
	size_t len;
	const char *text = luaL_checklstring(Ls, 2, &len);
	FILE *f = FS_OpenWrite(va("data/%s", path));
	if (!f)
		return luaL_error(Ls, "file.Write: can't write \"%s\"", path);
	fwrite(text, 1, len, f);
	fclose(f);
	return 0;
}

// file.Find("maps/*.bsp", "GAME") -> sorted list of file names
static int l_file_Find(lua_State *Ls)
{
	const char *pattern = luaL_checkstring(Ls, 1);
	const char *where = luaL_optstring(Ls, 2, "GAME");
	char dir[MAX_OSPATH];
	Q_strncpy(dir, pattern, sizeof(dir));
	char *slash = strrchr(dir, '/');
	const char *mask = pattern;
	if (slash) {
		*slash = 0;
		mask = pattern + (slash - dir) + 1;
	} else {
		dir[0] = 0;
	}
	char **list;
	int n;
	if (!stricmp(where, "LUA"))
		n = ListScripts(dir, mask, &list);
	else
		n = FS_ListFiles(dir, mask, &list);
	lua_createtable(Ls, n, 0);
	for (int i = 0; i < n; i++) {
		const char *s = list[i];
		const char *base = strrchr(s, '/');
		lua_pushstring(Ls, base ? base + 1 : s);
		lua_rawseti(Ls, -2, i + 1);
	}
	FreeList(list, n);
	return 1;
}

// ---------------------------------------------------------------- engine

static int l_engine_GetGameDir(lua_State *Ls) { lua_pushstring(Ls, FS_GameDir()); return 1; }
static int l_engine_Version(lua_State *Ls)    { lua_pushstring(Ls, ENGINE_VERSION); return 1; }
static int l_engine_GetMap(lua_State *Ls)     { lua_pushstring(Ls, R_CurrentMap()); return 1; }

// ---------------------------------------------------------------- state

static void Register(const char *lib, const luaL_Reg *fns)
{
	if (lib) {
		lua_getglobal(L, lib);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			lua_newtable(L);
		}
		luaL_setfuncs(L, fns, 0);
		lua_setglobal(L, lib);
	} else {
		lua_pushglobaltable(L);
		luaL_setfuncs(L, fns, 0);
		lua_pop(L, 1);
	}
}

static void OpenLibs(void)
{
	luaL_openlibs(L);

	// scripts load through include(); no loading arbitrary DLLs or files outside the tree
	lua_pushnil(L); lua_setglobal(L, "dofile");
	lua_pushnil(L); lua_setglobal(L, "loadfile");
	lua_getglobal(L, "package");
	lua_pushstring(L, ""); lua_setfield(L, -2, "path");
	lua_pushstring(L, ""); lua_setfield(L, -2, "cpath");
	lua_pop(L, 1);
	lua_getglobal(L, "os");
	lua_pushnil(L); lua_setfield(L, -2, "execute");
	lua_pushnil(L); lua_setfield(L, -2, "remove");
	lua_pushnil(L); lua_setfield(L, -2, "rename");
	lua_pushnil(L); lua_setfield(L, -2, "exit");
	lua_pushnil(L); lua_setfield(L, -2, "tmpname");
	lua_pop(L, 1);
	lua_pushnil(L); lua_setglobal(L, "io");

	static const luaL_Reg base[] = {
		{ "print", l_print }, { "Msg", l_Msg }, { "MsgN", l_MsgN }, { "include", l_include },
		{ "CurTime", l_CurTime }, { "RealTime", l_RealTime }, { "FrameTime", l_FrameTime },
		{ "SysTime", l_SysTime }, { "ScrW", l_ScrW }, { "ScrH", l_ScrH },
		{ "RunConsoleCommand", l_RunConsoleCommand },
		{ "CreateConVar", l_CreateConVar }, { "GetConVar", l_GetConVar },
		{ NULL, NULL }
	};
	Register(NULL, base);

	static const luaL_Reg concommand[] = {
		{ "Add", l_concommand_Add }, { "Remove", l_concommand_Remove }, { NULL, NULL }
	};
	Register("concommand", concommand);

	static const luaL_Reg surface[] = {
		{ "SetDrawColor", l_SetDrawColor }, { "SetTextColor", l_SetTextColor },
		{ "SetTextPos", l_SetTextPos }, { "DrawRect", l_DrawRect },
		{ "DrawOutlinedRect", l_DrawOutlinedRect }, { "DrawText", l_DrawText },
		{ "GetTextSize", l_GetTextSize }, { NULL, NULL }
	};
	Register("surface", surface);

	static const luaL_Reg input[] = {
		{ "IsKeyDown", l_IsKeyDown }, { "GetKeyName", l_GetKeyName }, { "GetKeyCode", l_GetKeyCode },
		{ NULL, NULL }
	};
	Register("input", input);

	static const luaL_Reg file[] = {
		{ "Read", l_file_Read }, { "Exists", l_file_Exists }, { "Write", l_file_Write },
		{ "Find", l_file_Find }, { NULL, NULL }
	};
	Register("file", file);

	static const luaL_Reg engine[] = {
		{ "GetGameDir", l_engine_GetGameDir }, { "Version", l_engine_Version }, { "GetMap", l_engine_GetMap },
		{ NULL, NULL }
	};
	Register("engine", engine);

	static const luaL_Reg convar[] = {
		{ "GetString", cv_GetString }, { "GetFloat", cv_GetFloat }, { "GetInt", cv_GetInt },
		{ "GetBool", cv_GetBool }, { "GetName", cv_GetName }, { "GetDefault", cv_GetDefault },
		{ "GetHelpText", cv_GetHelpText }, { "SetString", cv_SetString }, { "SetFloat", cv_SetString },
		{ "SetInt", cv_SetString }, { "SetBool", cv_SetBool }, { NULL, NULL }
	};
	luaL_newmetatable(L, CONVAR_MT);
	lua_newtable(L);
	luaL_setfuncs(L, convar, 0);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, cv_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	lua_pushinteger(L, FCVAR_ARCHIVE);
	lua_setglobal(L, "FCVAR_ARCHIVE");

	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, CONCOMMANDS);
}

// hook.Run(name, ...) with the given args already pushed; returns results count or -1
static int RunHook(const char *name, int nargs, int nres)
{
	lua_getglobal(L, "hook");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1 + nargs);
		return -1;
	}
	lua_getfield(L, -1, "Run");
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1 + nargs);
		return -1;
	}
	lua_insert(L, -1 - nargs);
	lua_pushstring(L, name);
	lua_insert(L, -1 - nargs);
	return SafeCall(nargs + 1, nres, va("hook %s", name)) ? nres : -1;
}

static void StartState(void)
{
	L = luaL_newstate();
	if (!L)
		Sys_Error("Couldn't create the Lua state");
	lua_sethook(L, TimeoutHook, LUA_MASKCOUNT, 10000);
	OpenLibs();
	BuildRoots();

	if (RunScript("includes/init.lua", true) < 0)
		Con_Errorf("[Lua] core library failed to load; scripts may not work\n");
	lua_settop(L, 0);

	char **list;
	int n = ListScripts("autorun", "*.lua", &list);
	for (int i = 0; i < n; i++) {
		RunScript(va("autorun/%s", list[i]), true);
		lua_settop(L, 0);
	}
	FreeList(list, n);

	RunHook("Initialize", 0, 0);
	lua_settop(L, 0);
	Con_Printf("Lua: %s, %d script folder%s, %d autorun file%s\n", LUA_RELEASE,
		roots_count, roots_count == 1 ? "" : "s", n, n == 1 ? "" : "s");
}

static void StopState(void)
{
	if (!L)
		return;
	RunHook("ShutDown", 0, 0);
	Cmd_RemoveFlagged(CMD_LUA);
	lua_close(L);
	L = NULL;
	in_draw = false;
	include_depth = 0;
	call_depth = 0;
}

// ---------------------------------------------------------------- console commands

static void LuaRun_f(void)
{
	if (!L)
		return;
	const char *code = Cmd_Args();
	if (!*code) {
		Con_Printf("usage: lua_run <code>   (an expression prints its value: lua_run 2+2)\n");
		return;
	}
	// try as an expression first so lua_run 2+2 prints 4
	int top = lua_gettop(L);
	strbuf_t expr = { 0 };
	SB_Append(&expr, "return ", 7);
	SB_Append(&expr, code, strlen(code));
	int r = luaL_loadbuffer(L, expr.data, expr.len, "=lua_run");
	SB_Free(&expr);
	if (r != LUA_OK) {
		lua_pop(L, 1);
		if (luaL_loadbuffer(L, code, strlen(code), "=lua_run") != LUA_OK) {
			ReportError("lua_run", lua_tostring(L, -1));
			lua_settop(L, top);
			return;
		}
	}
	if (SafeCall(0, LUA_MULTRET, "lua_run")) {
		int n = lua_gettop(L) - top;
		if (n > 0) {
			lua_getglobal(L, "print");
			lua_insert(L, top + 1);
			SafeCall(n, 0, "lua_run print");
		}
	}
	lua_settop(L, top);
}

static void LuaOpenScript_f(void)
{
	if (!L || Cmd_Argc() < 2) {
		Con_Printf("usage: lua_openscript <file>   (relative to lua/)\n");
		return;
	}
	RunScript(Cmd_Argv(1), true);
	lua_settop(L, 0);
}

static void LuaReload_f(void)
{
	StopState();
	StartState();
	Con_Printf("Lua reloaded\n");
}

static void LuaPaths_f(void)
{
	for (int i = 0; i < roots_count; i++)
		Con_Printf("  %s\n", roots[i]);
}

static void LuaMem_f(void)
{
	if (L)
		Con_Printf("Lua memory: %d KB\n", lua_gc(L, LUA_GCCOUNT, 0));
}

// ---------------------------------------------------------------- engine hooks

void Lua_Init(void)
{
	lua_timeout = Cvar_Get("lua_timeout", "5", FCVAR_ARCHIVE, "seconds one call into Lua may run, 0 = no limit");
	Cmd_AddCommand("lua_run", LuaRun_f, "runs Lua code; expressions print their value");
	Cmd_AddCommand("lua_openscript", LuaOpenScript_f, "runs lua/<file>");
	Cmd_AddCommand("lua_reload", LuaReload_f, "restarts Lua and reruns every script");
	Cmd_AddCommand("lua_paths", LuaPaths_f, "lists the Lua script folders");
	Cmd_AddCommand("lua_mem", LuaMem_f, "shows Lua memory use");
	StartState();
}

void Lua_Shutdown(void)
{
	StopState();
}

void Lua_Frame(void)
{
	if (!L)
		return;
	RunHook("Think", 0, 0);
	lua_settop(L, 0);
}

void Lua_DrawOverlay(void)
{
	if (!L)
		return;
	in_draw = true;
	RunHook("DrawOverlay", 0, 0);
	in_draw = false;
	lua_settop(L, 0);
}

void Lua_RunHookString(const char *hook, const char *arg)
{
	if (!L)
		return;
	lua_pushstring(L, arg ? arg : "");
	RunHook(hook, 1, 0);
	lua_settop(L, 0);
}

qboolean Lua_KeyEvent(int key, qboolean down)
{
	if (!L)
		return false;
	lua_pushstring(L, Key_KeynumToString(key));
	lua_pushboolean(L, down);
	lua_pushinteger(L, key);
	qboolean eaten = false;
	if (RunHook("KeyEvent", 3, 1) == 1)
		eaten = lua_toboolean(L, -1);
	lua_settop(L, 0);
	return eaten;
}
