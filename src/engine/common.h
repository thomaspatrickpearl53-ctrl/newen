// newen - shared engine declarations
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#define ENGINE_NAME    "newen"
#define ENGINE_VERSION "0.1.0"

typedef int qboolean;
#ifndef true
#define true 1
#define false 0
#endif

#define MAX_OSPATH 1024

#ifdef _MSC_VER
#define NORETURN __declspec(noreturn)
#define stricmp _stricmp
#define strnicmp _strnicmp
#else
#define NORETURN __attribute__((noreturn))
#define stricmp strcasecmp
#define strnicmp strncasecmp
#endif

// ---------------------------------------------------------------- common.c
void *Mem_Alloc(size_t size);               // zeroed, never returns NULL
void *Mem_Realloc(void *ptr, size_t size);
void  Mem_Free(void *ptr);
char *Mem_Strdup(const char *s);

void  Q_strncpy(char *dst, const char *src, size_t size);
char *va(const char *fmt, ...);              // rotating static buffers
qboolean Q_WildcardMatch(const char *pattern, const char *s);

// Growable byte buffer
typedef struct { char *data; size_t len, cap; } strbuf_t;
void SB_Append(strbuf_t *sb, const char *s, size_t n);
void SB_Clear(strbuf_t *sb);
void SB_Free(strbuf_t *sb);

// Tokenizer: returns pointer past token, or NULL at end. Token in com_token.
extern char com_token[8192];
const char *COM_Parse(const char *data);

extern int    com_argc;
extern char **com_argv;
int         COM_CheckParm(const char *parm);     // index or 0
const char *COM_ParmValue(const char *parm);     // value after parm, or NULL

// ---------------------------------------------------------------- sys_win.c
NORETURN void Sys_Error(const char *fmt, ...);
double      Sys_FloatTime(void);
void        Sys_Sleep(int ms);
const char *Sys_ExeDir(void);
qboolean    Sys_DirExists(const char *path);
qboolean    Sys_FileExists(const char *path);
void        Sys_MakeDirs(const char *path);    // creates every directory in the path to a file
const char *Sys_SteamPath(void);               // NULL if unknown
typedef void (*sys_listfn_t)(const char *name, qboolean isdir, void *ctx);
void        Sys_ListDir(const char *dir, sys_listfn_t fn, void *ctx);

// ---------------------------------------------------------------- cvar.c
// Layout matches GoldSrc's cvar_t so game DLLs can register their own.
typedef struct cvar_s {
	char  *name;
	char  *string;
	int    flags;
	float  value;
	struct cvar_s *next;
} cvar_t;

#define FCVAR_ARCHIVE       (1<<0)
#define FCVAR_USERINFO      (1<<1)
#define FCVAR_SERVER        (1<<2)
#define FCVAR_EXTDLL        (1<<3)
#define FCVAR_CLIENTDLL     (1<<4)
#define FCVAR_PROTECTED     (1<<5)
#define FCVAR_SPONLY        (1<<6)
#define FCVAR_PRINTABLEONLY (1<<7)
#define FCVAR_UNLOGGED      (1<<8)
// newen extensions
#define FCVAR_READONLY      (1<<16)
#define FCVAR_LUA           (1<<17)  // created by Lua, removed on lua_reload

void        Cvar_Init(void);
cvar_t     *Cvar_Get(const char *name, const char *value, int flags, const char *desc);
cvar_t     *Cvar_Find(const char *name);
void        Cvar_Set(const char *name, const char *value);
void        Cvar_SetValue(const char *name, float value);
void        Cvar_DirectSet(cvar_t *var, const char *value);
float       Cvar_VariableValue(const char *name);
const char *Cvar_VariableString(const char *name);
const char *Cvar_Description(const cvar_t *var);
const char *Cvar_Default(const cvar_t *var);
qboolean    Cvar_Command(void);                 // handles "name [value]" from the console
void        Cvar_WriteArchived(FILE *f);
void        Cvar_RemoveFlagged(int flag);
void        Cvar_SetPending(const char *name, const char *value);  // applied when the cvar is created
cvar_t     *Cvar_First(void);

// ---------------------------------------------------------------- cmd.c
typedef void (*xcommand_t)(void);

void        Cmd_Init(void);
void        Cbuf_AddText(const char *text);
void        Cbuf_InsertText(const char *text);
void        Cbuf_Execute(void);
void        Cmd_AddCommand(const char *name, xcommand_t fn, const char *desc);
void        Cmd_AddCommandFlags(const char *name, xcommand_t fn, const char *desc, int flags);
void        Cmd_RemoveCommand(const char *name);
void        Cmd_RemoveFlagged(int flags);
qboolean    Cmd_Exists(const char *name);
void        Cmd_TokenizeString(const char *text);
int         Cmd_Argc(void);
const char *Cmd_Argv(int i);
const char *Cmd_Args(void);                     // everything after argv[0]
void        Cmd_ExecuteString(const char *text);
void        Cmd_ExecFile(const char *path, qboolean quiet);
// For console completion: calls fn for every command, alias and cvar name starting with prefix
typedef void (*cmd_completefn_t)(const char *name, void *ctx);
void        Cmd_CompleteNames(const char *prefix, cmd_completefn_t fn, void *ctx);
// Fallback for unknown commands; returns true if handled
extern qboolean (*cmd_fallback)(void);
extern qboolean cmd_defer_cvars;   // unknown "name value" lines become pending cvar values

#define CMD_LUA (1<<0)

// ---------------------------------------------------------------- console.c
#define COLOR_DEFAULT 0
#define COLOR_WARN    1
#define COLOR_ERROR   2
#define COLOR_LUA     3
#define COLOR_ECHO    4

void Con_Init(void);
void Con_Shutdown(void);
void Con_Printf(const char *fmt, ...);
void Con_DPrintf(const char *fmt, ...);         // only when "developer" > 0
void Con_Warnf(const char *fmt, ...);
void Con_Errorf(const char *fmt, ...);
void Con_ColorPrint(int color, const char *text);
void Con_ToggleConsole(void);
qboolean Con_IsVisible(void);
void Con_KeyEvent(int key);
void Con_CharEvent(int ch);
void Con_Draw(void);
void Con_SetForced(qboolean forced);            // full-screen console while there is no world
void Con_OpenLog(const char *path);

// ---------------------------------------------------------------- keys.c
// GoldSrc key numbers (client.dll uses them)
enum {
	K_TAB = 9, K_ENTER = 13, K_ESCAPE = 27, K_SPACE = 32, K_BACKSPACE = 127,
	K_UPARROW = 128, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW,
	K_ALT, K_CTRL, K_SHIFT,
	K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
	K_INS, K_DEL, K_PGDN, K_PGUP, K_HOME, K_END,
	K_KP_HOME = 160, K_KP_UPARROW, K_KP_PGUP, K_KP_LEFTARROW, K_KP_5, K_KP_RIGHTARROW,
	K_KP_END, K_KP_DOWNARROW, K_KP_PGDN, K_KP_ENTER, K_KP_INS, K_KP_DEL,
	K_KP_SLASH, K_KP_MINUS, K_KP_PLUS, K_CAPSLOCK,
	K_MWHEELDOWN = 239, K_MWHEELUP = 240,
	K_MOUSE1 = 241, K_MOUSE2, K_MOUSE3, K_MOUSE4, K_MOUSE5,
	K_PAUSE = 255,
	K_COUNT = 256
};

void        Key_Init(void);
void        Key_Event(int key, qboolean down);
qboolean    Key_IsDown(int key);
int         Key_StringToKeynum(const char *str);
const char *Key_KeynumToString(int key);
void        Key_WriteBindings(FILE *f);
void        Key_ClearStates(void);

// ---------------------------------------------------------------- filesystem.c
void        FS_Init(void);
void        FS_Shutdown(void);
const char *FS_BaseDir(void);    // Half-Life install folder
const char *FS_GameDir(void);    // e.g. "valve"
const char *FS_WriteDir(void);   // where configs and saves go
const char *FS_EngineDir(void);  // newen's own content ("base")
void       *FS_LoadFile(const char *path, size_t *len);  // NUL-terminated, Mem_Free it
qboolean    FS_FileExists(const char *path);
qboolean    FS_FullPath(const char *path, char *out, size_t outsize);
FILE       *FS_OpenWrite(const char *path);
// Lists files under dir (relative) matching wildcard, merged over all search paths,
// sorted, without duplicates. Returns count; free with FS_FreeList.
int         FS_ListFiles(const char *dir, const char *pattern, char ***out);
void        FS_FreeList(char **list, int count);
void        FS_AddSearchPath(const char *dir, qboolean front);

// ---------------------------------------------------------------- vid_sdl.c
extern int vid_width, vid_height;  // drawable size in pixels
void     Vid_Init(void);
void     Vid_Shutdown(void);
void     Vid_PumpEvents(void);
void     Vid_BeginFrame(void);
void     Vid_EndFrame(void);
void     Vid_SetTextInput(qboolean on);
char    *Vid_GetClipboard(void);   // Mem_Free it; NULL if empty
void     Vid_SetRelativeMouse(qboolean on);
void     Vid_GetMouseDelta(int *dx, int *dy);   // since the last call

// ---------------------------------------------------------------- r_main.c
void        R_Init(void);
void        R_Shutdown(void);
void        R_UpdateCamera(void);
void        R_RenderView(void);
const char *R_CurrentMap(void);

// ---------------------------------------------------------------- draw.c
void Draw_Init(void);
void Draw_Shutdown(void);
void Draw_Begin2D(void);
void Draw_Fill(float x, float y, float w, float h, int r, int g, int b, int a);
int  Draw_String(float x, float y, const char *s, int r, int g, int b, int a);  // returns width
int  Draw_StringWidth(const char *s);
int  Draw_CharWidth(void);
int  Draw_LineHeight(void);

// font_win.c - rasterizes a system font into an 8-bit alpha atlas (ASCII 32..126)
typedef struct {
	int width, height;          // atlas size
	int cell_w, cell_h;         // monospace cell
	unsigned char *alpha;       // width*height
} fontatlas_t;
qboolean Font_Rasterize(const char *face, int pixel_height, fontatlas_t *out);

// ---------------------------------------------------------------- lua_host.c
void     Lua_Init(void);
void     Lua_Shutdown(void);
void     Lua_Frame(void);
void     Lua_DrawOverlay(void);
qboolean Lua_KeyEvent(int key, qboolean down);
void     Lua_RunHookString(const char *hook, const char *arg);

// ---------------------------------------------------------------- host.c
extern double host_realtime;    // seconds since start
extern double host_frametime;   // last frame's duration
extern qboolean host_initialized;
void Host_Init(int argc, char **argv);
void Host_Frame(void);
void Host_Shutdown(void);
NORETURN void Host_Quit(void);
extern cvar_t *developer;
