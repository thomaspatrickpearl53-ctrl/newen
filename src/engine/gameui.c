// newen - hosts Half-Life's own main menu: GameUI (gameui.so / GameUI.dll) on vgui2
//
// As GoldSrc does: vgui2 is loaded from the Half-Life folder and started with the engine's interfaces
// (the surface in vgui2_surface.c, the file system in vgui2_fs.c, and below: IEngineVGui, IGameUIFuncs,
// IBaseUI). The engine owns three full-screen panels: the root (the surface's embedded panel), one for
// the client DLL and one GameUI builds the menu on. Those are vgui2 panels whose client side
// (IClientPanel) is ours. GameUI then gets the same engine functions as the client DLL and runs its
// commands ("map c0a0", "load quick"...) through them.
//
// The menu shows at startup and whenever Escape is pressed; while it shows in a map, the game is
// paused (single player) and the mouse and keyboard go to vgui2.
#include "vgui2_local.h"

#ifdef _WIN32
#define VGUI2_LIB  "vgui2.dll"
#define GAMEUI_LIB "cl_dlls/GameUI.dll"
#define STEAM_LIB  "steam_api.dll"
#else
#define VGUI2_LIB  "vgui2.so"
#define GAMEUI_LIB "cl_dlls/gameui.so"
#define STEAM_LIB  "libsteam_api.so"
#endif

void *vg_ipanel, *vg_ivgui, *vg_input, *vg_scheme, *vg_localize;

static void *vgui2_lib, *gameui_lib;
static CreateInterfaceFn vgui2_factory, gameui_factory, client_factory;
static void *gameui, *gameconsole, *surface, *filesystem;
static qboolean loaded, active;
static cvar_t *gameui_cvar;

// ---------------------------------------------------------------- virtual tables

#ifdef _WIN32
static void *__fastcall Dtor(void *self, void *edx, unsigned flags) { return self; }
#else
static void Dtor(void *self) {}
#endif

static void Unfilled(void)
{
	Con_Warnf("VGUI2 called an engine method newen doesn't have\n");
}

void VG_InitVtable(void **mem, int methods)
{
	for (int i = 0; i < VT_PREFIX; i++)
		mem[i] = NULL;   // offset-to-top 0, no type info
	for (int i = 0; i < VT_DTORS; i++)
		mem[VT_PREFIX + i] = (void *)Dtor;
	for (int i = 0; i < methods; i++)
		mem[VT_PREFIX + VT_DTORS + i] = (void *)Unfilled;
}

// a method of one of vgui2's or GameUI's objects (they all have a virtual destructor first)
#define METHOD(obj, i) VFN(obj, VT_DTORS + (i))

// ---------------------------------------------------------------- the engine's panels (IClientPanel)

typedef struct {
	void **vtable;
	VPANEL vpanel;
	const char *name;
} clientpanel_t;

static clientpanel_t root_panel = { NULL, 0, "StaticPanel" };
static clientpanel_t client_panel = { NULL, 0, "BaseClientPanel" };
static clientpanel_t gameui_panel = { NULL, 0, "BaseGameUIPanel" };

#define CP ((clientpanel_t *)self)

static VPANEL THISCALL CP_GetVPanel(VTHIS) { return CP->vpanel; }
static void THISCALL CP_Nop(VTHIS) {}

// children are painted in order; popups are left to the surface (ShouldPaintChildPanel says so)
static void THISCALL CP_PaintTraverse(VTHIS, bool repaint, bool allowForce)
{
	if (!VP_IsVisible(CP->vpanel))
		return;
	int n = VP_GetChildCount(CP->vpanel);
	for (int i = 0; i < n; i++) {
		VPANEL child = VP_GetChild(CP->vpanel, i);
		if (!VGS_ShouldPaintChild(child))
			continue;
		VP_PaintTraverse(child, repaint, allowForce);
	}
}

// the topmost child under the point, or this panel
static VPANEL THISCALL CP_IsWithinTraverse(VTHIS, int x, int y, bool traversePopups)
{
	VPANEL p = CP->vpanel;
	if (!VP_IsVisible(p) || !VP_IsMouseInputEnabled(p))
		return 0;
	int n = VP_GetChildCount(p);
	if (traversePopups) {
		for (int i = n - 1; i >= 0; i--) {
			VPANEL child = VP_GetChild(p, i);
			if (VP_IsPopup(child)) {
				VPANEL hit = VP_IsWithinTraverse(child, x, y, true);
				if (hit)
					return hit;
			}
		}
	}
	for (int i = n - 1; i >= 0; i--) {
		VPANEL child = VP_GetChild(p, i);
		if (VP_IsPopup(child))
			continue;
		VPANEL hit = VP_IsWithinTraverse(child, x, y, false);
		if (hit)
			return hit;
	}
	int x0, y0, x1, y1;
	VP_GetClipRect(p, &x0, &y0, &x1, &y1);
	return x >= x0 && x < x1 && y >= y0 && y < y1 ? p : 0;
}

static void THISCALL CP_GetInset(VTHIS, int *l, int *t, int *r, int *b) { *l = *t = *r = *b = 0; }
static void THISCALL CP_GetClipRect(VTHIS, int *x0, int *y0, int *x1, int *y1)
{
	VP_GetClipRect(CP->vpanel, x0, y0, x1, y1);
}
static void THISCALL CP_OnChildAdded(VTHIS, VPANEL child) {}
static void THISCALL CP_OnSizeChanged(VTHIS, int w, int h) {}
static void THISCALL CP_InternalFocusChanged(VTHIS, bool lost) {}
static bool THISCALL CP_RequestInfo(VTHIS, void *kv) { return false; }
static void THISCALL CP_RequestFocus(VTHIS, int direction) {}
static bool THISCALL CP_RequestFocusPrevNext(VTHIS, VPANEL p) { return false; }
static void THISCALL CP_OnMessage(VTHIS, const void *kv, VPANEL from) {}
static VPANEL THISCALL CP_GetCurrentKeyFocus(VTHIS) { return 0; }
static int THISCALL CP_GetTabPosition(VTHIS) { return 0; }
static const char *THISCALL CP_GetName(VTHIS) { return CP->name; }
static const char *THISCALL CP_GetClassName(VTHIS) { return "Panel"; }
static HScheme THISCALL CP_GetScheme(VTHIS)
{
	HScheme (THISCALL *get)(VTHIS) = (void *)METHOD(vg_scheme, 2);   // GetDefaultScheme
	return get(VTHIS_ARG(vg_scheme));
}
static bool THISCALL CP_False(VTHIS) { return false; }
static void *THISCALL CP_QueryInterface(VTHIS, int id) { return NULL; }
static void *THISCALL CP_GetPanel(VTHIS) { return NULL; }
static const char *THISCALL CP_GetModuleName(VTHIS) { return "BaseUI"; }

// IClientPanel has no destructor and no overloads: the same slots on both compilers
static void *cp_vtable[VT_PREFIX + 27];

static void InitClientPanelVtable(void)
{
	void **m = cp_vtable + VT_PREFIX;
	m[0] = (void *)CP_GetVPanel;
	m[1] = (void *)CP_Nop;                    // Think
	m[2] = (void *)CP_Nop;                    // PerformApplySchemeSettings
	m[3] = (void *)CP_PaintTraverse;
	m[4] = (void *)CP_Nop;                    // Repaint
	m[5] = (void *)CP_IsWithinTraverse;
	m[6] = (void *)CP_GetInset;
	m[7] = (void *)CP_GetClipRect;
	m[8] = (void *)CP_OnChildAdded;
	m[9] = (void *)CP_OnSizeChanged;
	m[10] = (void *)CP_InternalFocusChanged;
	m[11] = (void *)CP_RequestInfo;
	m[12] = (void *)CP_RequestFocus;
	m[13] = (void *)CP_RequestFocusPrevNext;
	m[14] = (void *)CP_RequestFocusPrevNext;
	m[15] = (void *)CP_OnMessage;
	m[16] = (void *)CP_GetCurrentKeyFocus;
	m[17] = (void *)CP_GetTabPosition;
	m[18] = (void *)CP_GetName;
	m[19] = (void *)CP_GetClassName;
	m[20] = (void *)CP_GetScheme;
	m[21] = (void *)CP_False;                 // IsProportional
	m[22] = (void *)CP_False;                 // IsAutoDeleteSet
	m[23] = (void *)CP_Nop;                   // DeletePanel
	m[24] = (void *)CP_QueryInterface;
	m[25] = (void *)CP_GetPanel;
	m[26] = (void *)CP_GetModuleName;
}

static void MakePanel(clientpanel_t *cp, VPANEL parent, qboolean visible, qboolean input)
{
	VPANEL (THISCALL *alloc)(VTHIS) = (void *)METHOD(vg_ivgui, 7);   // AllocPanel
	void (THISCALL *init)(VTHIS, VPANEL, void *) = (void *)METHOD(vg_ipanel, IPANEL_Init);
	cp->vtable = cp_vtable + VT_PREFIX;
	cp->vpanel = alloc(VTHIS_ARG(vg_ivgui));
	init(VTHIS_ARG(vg_ipanel), cp->vpanel, cp);
	if (parent)
		VP_SetParent(cp->vpanel, parent);
	VP_SetPos(cp->vpanel, 0, 0);
	VP_SetSize(cp->vpanel, vid_width, vid_height);
	VP_SetVisible(cp->vpanel, visible);
	VP_SetMouseInputEnabled(cp->vpanel, input);
	VP_SetKeyBoardInputEnabled(cp->vpanel, input);
}

// ---------------------------------------------------------------- IEngineVGui ("VEngineVGui001")

static VPANEL THISCALL EV_GetPanel(VTHIS, int which)
{
	switch (which) {
	case 1: return client_panel.vpanel;   // PANEL_CLIENTDLL
	case 2: return gameui_panel.vpanel;   // PANEL_GAMEUIDLL
	default: return root_panel.vpanel;
	}
}
static bool THISCALL EV_SteamRefreshLogin(VTHIS, const char *password, bool secure) { return false; }
static bool THISCALL EV_SteamProcessCall(VTHIS, bool *finished, void *progress, void *error) { return false; }

static void *ev_vtable[VT_PREFIX + VT_DTORS + 3];
static cppobj_t ev_object;

// ---------------------------------------------------------------- IGameUIFuncs ("VENGINE_GAMEUIFUNCS_VERSION001")

// engine key numbers <-> vgui2's KeyCode
static int VGUIKeyCode(int key)
{
	if (key >= '0' && key <= '9') return 1 + key - '0';
	if (key >= 'a' && key <= 'z') return 11 + key - 'a';
	switch (key) {
	case K_KP_INS: return 37; case K_KP_END: return 38; case K_KP_DOWNARROW: return 39; case K_KP_PGDN: return 40;
	case K_KP_LEFTARROW: return 41; case K_KP_5: return 42; case K_KP_RIGHTARROW: return 43; case K_KP_HOME: return 44;
	case K_KP_UPARROW: return 45; case K_KP_PGUP: return 46; case K_KP_SLASH: return 47; case K_KP_MINUS: return 49;
	case K_KP_PLUS: return 50; case K_KP_ENTER: return 51; case K_KP_DEL: return 52;
	case '[': return 53; case ']': return 54; case ';': return 55; case '\'': return 56; case '`': return 57;
	case ',': return 58; case '.': return 59; case '/': return 60; case '\\': return 61; case '-': return 62;
	case '=': return 63; case K_ENTER: return 64; case K_SPACE: return 65; case K_BACKSPACE: return 66;
	case K_TAB: return 67; case K_CAPSLOCK: return 68; case K_ESCAPE: return 70; case K_INS: return 72;
	case K_DEL: return 73; case K_HOME: return 74; case K_END: return 75; case K_PGUP: return 76;
	case K_PGDN: return 77; case K_PAUSE: return 78; case K_SHIFT: return 79; case K_ALT: return 81;
	case K_CTRL: return 83; case K_UPARROW: return 88; case K_LEFTARROW: return 89; case K_DOWNARROW: return 90;
	case K_RIGHTARROW: return 91;
	}
	if (key >= K_F1 && key <= K_F12) return 92 + key - K_F1;
	return 0;
}

static bool THISCALL GF_IsKeyDown(VTHIS, const char *name, bool *down)
{
	int key = Key_StringToKeynum(name);
	*down = key >= 0 && Key_IsDown(key);
	return key >= 0;
}
static const char *THISCALL GF_Key_NameForKey(VTHIS, int key) { return Key_KeynumToString(key); }
static const char *THISCALL GF_Key_BindingForKey(VTHIS, int key) { return Key_GetBinding(key); }
static int THISCALL GF_GetVGUI2KeyCodeForBind(VTHIS, const char *bind)
{
	const char *name = Key_KeynameForBinding(bind);
	return name ? VGUIKeyCode(Key_StringToKeynum(name)) : 0;
}

typedef struct { int width, height, bpp; } vmode_t;
static vmode_t *modes;
static int modes_count;

static void THISCALL GF_GetVideoModes(VTHIS, vmode_t **out, int *count)
{
	if (!modes) {
		int n = SDL_GetNumDisplayModes(0);
		modes = Mem_Alloc(sizeof(*modes) * (n > 0 ? n : 1));
		for (int i = 0; i < n; i++) {
			SDL_DisplayMode m;
			if (SDL_GetDisplayMode(0, i, &m))
				continue;
			int dup = 0;
			for (int j = 0; j < modes_count; j++)
				if (modes[j].width == m.w && modes[j].height == m.h)
					dup = 1;
			if (dup)
				continue;
			modes[modes_count].width = m.w;
			modes[modes_count].height = m.h;
			modes[modes_count].bpp = 32;
			modes_count++;
		}
	}
	*out = modes;
	*count = modes_count;
}

static void THISCALL GF_GetCurrentVideoMode(VTHIS, int *w, int *h, int *bpp)
{
	*w = vid_width;
	*h = vid_height;
	*bpp = 32;
}

static void THISCALL GF_GetCurrentRenderer(VTHIS, char *name, int size, int *windowed, int *hdmodels, int *addons)
{
	// callers leave out what they don't need
	if (name && size > 0)
		Q_strncpy(name, "gl", (size_t)size);
	SDL_Window *w = SDL_GL_GetCurrentWindow();
	if (windowed)
		*windowed = !w || !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN);
	if (hdmodels)
		*hdmodels = COM_CheckParm("-hd") != 0;
	if (addons)
		*addons = 1;
}

static bool THISCALL GF_IsConnectedToVACSecureServer(VTHIS) { return false; }
static int THISCALL GF_Key_KeyStringToKeyNum(VTHIS, const char *name) { return Key_StringToKeynum(name); }

static void *gf_vtable[VT_PREFIX + VT_DTORS + 9];
static cppobj_t gf_object;

// ---------------------------------------------------------------- IBaseUI ("BaseUI001"): GameUI calls back

static void THISCALL BU_Initialize(VTHIS, CreateInterfaceFn *factories, int count) {}
static void THISCALL BU_Start(VTHIS, void *engfuncs, int version) {}
static void THISCALL BU_Shutdown(VTHIS) {}
static int  THISCALL BU_Key_Event(VTHIS, int down, int key, const char *binding) { return 1; }
static void THISCALL BU_CallEngineSurfaceAppHandler(VTHIS, void *event, void *data) {}
static void THISCALL BU_Paint(VTHIS, int x, int y, int right, int bottom) {}
static void THISCALL BU_HideGameUI(VTHIS) { GameUI_Hide(); }
static void THISCALL BU_ActivateGameUI(VTHIS) { GameUI_Activate(); }
static void THISCALL BU_HideConsole(VTHIS) { if (Con_IsVisible()) Con_ToggleConsole(); }
static void THISCALL BU_ShowConsole(VTHIS) { if (!Con_IsVisible()) Con_ToggleConsole(); }

static void *bu_vtable[VT_PREFIX + VT_DTORS + 10];
static cppobj_t bu_object;

// ---------------------------------------------------------------- the engine's factory

static void *EngineFactory(const char *name, int *ret)
{
	void *p = NULL;
	if (!strcmp(name, "VGUI_Surface026"))
		p = surface;
	else if (!strcmp(name, "VFileSystem009"))
		p = filesystem;
	else if (!strcmp(name, "VEngineVGui001"))
		p = &ev_object;
	else if (!strcmp(name, "VENGINE_GAMEUIFUNCS_VERSION001"))
		p = &gf_object;
	else if (!strcmp(name, "BaseUI001"))
		p = &bu_object;
	if (ret)
		*ret = p ? 0 : 1;   // IFACE_OK, IFACE_FAILED
	if (!p)
		Con_DPrintf("VGUI2 asked the engine for %s: not here\n", name);
	return p;
}

static void InitEngineObjects(void)
{
	void **vt = ev_vtable;
	VG_InitVtable(vt, 3);
	VG_METHOD(vt, 0) = (void *)EV_GetPanel;
	VG_METHOD(vt, 1) = (void *)EV_SteamRefreshLogin;
	VG_METHOD(vt, 2) = (void *)EV_SteamProcessCall;
	ev_object.vtable = vt + VT_PREFIX;

	vt = gf_vtable;
	VG_InitVtable(vt, 9);
	VG_METHOD(vt, 0) = (void *)GF_IsKeyDown;
	VG_METHOD(vt, 1) = (void *)GF_Key_NameForKey;
	VG_METHOD(vt, 2) = (void *)GF_Key_BindingForKey;
	VG_METHOD(vt, 3) = (void *)GF_GetVGUI2KeyCodeForBind;
	VG_METHOD(vt, 4) = (void *)GF_GetVideoModes;
	VG_METHOD(vt, 5) = (void *)GF_GetCurrentVideoMode;
	VG_METHOD(vt, 6) = (void *)GF_GetCurrentRenderer;
	VG_METHOD(vt, 7) = (void *)GF_IsConnectedToVACSecureServer;
	VG_METHOD(vt, 8) = (void *)GF_Key_KeyStringToKeyNum;
	gf_object.vtable = vt + VT_PREFIX;

	vt = bu_vtable;
	VG_InitVtable(vt, 10);
	VG_METHOD(vt, 0) = (void *)BU_Initialize;
	VG_METHOD(vt, 1) = (void *)BU_Start;
	VG_METHOD(vt, 2) = (void *)BU_Shutdown;
	VG_METHOD(vt, 3) = (void *)BU_Key_Event;
	VG_METHOD(vt, 4) = (void *)BU_CallEngineSurfaceAppHandler;
	VG_METHOD(vt, 5) = (void *)BU_Paint;
	VG_METHOD(vt, 6) = (void *)BU_HideGameUI;
	VG_METHOD(vt, 7) = (void *)BU_ActivateGameUI;
	VG_METHOD(vt, 8) = (void *)BU_HideConsole;
	VG_METHOD(vt, 9) = (void *)BU_ShowConsole;
	bu_object.vtable = vt + VT_PREFIX;

	InitClientPanelVtable();
}

// ---------------------------------------------------------------- IGameUI ("GameUI007") calls

enum {
	GUI_Initialize, GUI_Start, GUI_Shutdown, GUI_ActivateGameUI, GUI_ActivateDemoUI, GUI_HasExclusiveInput,
	GUI_RunFrame, GUI_ConnectToServer, GUI_DisconnectFromServer, GUI_HideGameUI, GUI_IsGameUIActive,
	GUI_LoadingStarted, GUI_LoadingFinished, GUI_StartProgressBar, GUI_ContinueProgressBar, GUI_StopProgressBar,
	GUI_SetProgressBarStatusText, GUI_SetSecondaryProgressBar, GUI_SetSecondaryProgressBarText, GUI_ValidateCDKey,
	GUI_OnDisconnectFromServer
};

typedef void (THISCALL *gui_v)(VTHIS);
typedef void (THISCALL *gui_ss)(VTHIS, const char *, const char *);

static void CallGameUI(int m)
{
	if (gameui)
		((gui_v)METHOD(gameui, m))(VTHIS_ARG(gameui));
}

// ---------------------------------------------------------------- vgui2's input (IInputInternal)

enum {
	IN_InternalCursorMoved = 24, IN_InternalMousePressed, IN_InternalMouseDoublePressed, IN_InternalMouseReleased,
	IN_InternalMouseWheeled, IN_InternalKeyCodePressed, IN_InternalKeyCodeTyped, IN_InternalKeyTyped,
	IN_InternalKeyCodeReleased
};

static void InputInt(int m, int v)
{
	((void (THISCALL *)(VTHIS, int))METHOD(vg_input, m))(VTHIS_ARG(vg_input), v);
}

// ---------------------------------------------------------------- setup

static void *Proc(void *lib, const char *name)
{
	return lib ? Sys_GetProcAddress(lib, name) : NULL;
}

static void *Need(CreateInterfaceFn f, const char *name)
{
	void *p = f ? f(name, NULL) : NULL;
	if (!p)
		Con_Warnf("VGUI2 has no %s\n", name);
	return p;
}

// GameUI pauses single player while it shows ("setpause" / "unpause"), as GoldSrc's engine does
static qboolean user_paused;
static void SetPause_f(void) { user_paused = true; }
static void Unpause_f(void) { user_paused = false; }
static void Nop_f(void) {}
qboolean GameUI_Paused(void) { return (user_paused || (loaded && active)) && R_CurrentMap()[0]; }

// test clicks: the cursor moves first and the button goes down two frames later, once vgui2 knows
// what's under it (as with a real mouse)
static int test_click_frames, test_click_button;

static void TestClick(int x, int y, int button)
{
	GameUI_MouseMove(x, y);
	test_click_button = button;
	test_click_frames = 2;
}

// test input: a click at a point, as if the mouse were there
static void Click_f(void)
{
	if (Cmd_Argc() < 3) {
		Con_Printf("usage: gameui_click <x> <y> [right]\n");
		return;
	}
	TestClick(atoi(Cmd_Argv(1)), atoi(Cmd_Argv(2)), Cmd_Argc() > 3 ? 1 : 0);
}

// the first visible panel with this name, depth first
static VPANEL FindPanel(VPANEL p, const char *name)
{
	if (!VP_IsVisible(p))
		return 0;
	const char *n = VP_GetName(p);
	if (n && !stricmp(n, name))
		return p;
	int count = VP_GetChildCount(p);
	for (int i = count - 1; i >= 0; i--) {
		VPANEL hit = FindPanel(VP_GetChild(p, i), name);
		if (hit)
			return hit;
	}
	return 0;
}

// test input: a click on the middle of a panel found by name ("PlayGame")
static void Press_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: gameui_press <panel name>\n");
		return;
	}
	VPANEL p = loaded ? FindPanel(root_panel.vpanel, Cmd_Argv(1)) : 0;
	if (!p) {
		Con_Printf("no visible panel \"%s\"\n", Cmd_Argv(1));
		return;
	}
	int x, y, w, h;
	VP_GetAbsPos(p, &x, &y);
	VP_GetSize(p, &w, &h);
	TestClick(x + w / 2, y + h / 2, 0);
}

static void ListPanels(VPANEL p, int depth)
{
	if (!VP_IsVisible(p))
		return;
	int x, y, w, h;
	VP_GetAbsPos(p, &x, &y);
	VP_GetSize(p, &w, &h);
	Con_Printf("%*s%s  %d,%d %dx%d%s\n", depth * 2, "", VP_GetName(p), x, y, w, h, VP_IsPopup(p) ? "  popup" : "");
	int n = VP_GetChildCount(p);
	for (int i = 0; i < n; i++)
		ListPanels(VP_GetChild(p, i), depth + 1);
}

static void Panels_f(void)
{
	if (loaded)
		ListPanels(root_panel.vpanel, 0);
}

// test input: a key press and release, by engine key name ("enter", "downarrow", "a")
static void Key_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: gameui_key <key>\n");
		return;
	}
	int key = Key_StringToKeynum(Cmd_Argv(1));
	if (key < 0) {
		Con_Printf("no key \"%s\"\n", Cmd_Argv(1));
		return;
	}
	GameUI_KeyEvent(key, true);
	GameUI_KeyEvent(key, false);
}

static void GameUI_f(void)
{
	if (!loaded) {
		Con_Printf("GameUI isn't loaded\n");
		return;
	}
	if (active)
		GameUI_Hide();
	else
		GameUI_Activate();
}

void GameUI_Init(void)
{
	gameui_cvar = Cvar_Get("gameui", "1", FCVAR_ARCHIVE, "Half-Life's own main menu (GameUI); 0 = the console instead, from the next start");
	Cmd_AddCommand("gameui_activate", GameUI_Activate, "shows the main menu");
	Cmd_AddCommand("gameui_hide", GameUI_Hide, "hides the main menu");
	Cmd_AddCommand("togglemenu", GameUI_f, "shows or hides the main menu");
	Cmd_AddCommand("gameui_click", Click_f, "gameui_click <x> <y> [right] - clicks the menu (for tests)");
	Cmd_AddCommand("gameui_press", Press_f, "gameui_press <panel name> - clicks a menu control by name (for tests)");
	Cmd_AddCommand("gameui_panels", Panels_f, "lists the visible menu panels");
	Cmd_AddCommand("gameui_key", Key_f, "gameui_key <key> - presses a key in the menu (for tests)");
	Cmd_AddCommand("setpause", SetPause_f, "pauses a single player game");
	Cmd_AddCommand("unpause", Unpause_f, "unpauses a single player game");
	Cmd_AddCommand("inputset_menu", Nop_f, "Steam Input action set (GameUI sends it; nothing to do here)");
	Cmd_AddCommand("inputset_game", Nop_f, "Steam Input action set (nothing to do here)");
	if (!gameui_cvar->value || COM_CheckParm("-nogameui"))
		return;

	char path[MAX_OSPATH];
	// GameUI can't run without Steam (it ends the process when the Steam API won't start)
	snprintf(path, sizeof(path), "%s/" STEAM_LIB, FS_BaseDir());
	void *steam = Sys_LoadLibrary(path);
	bool (*steam_running)(void) = steam ? (bool (*)(void))Proc(steam, "SteamAPI_IsSteamRunning") : NULL;
	if (!steam_running || !steam_running()) {
		Con_Warnf("Steam isn't running, so Half-Life's menu can't start; using the console instead\n");
		return;
	}
	// GameUI starts the Steam API itself and ends the process if Steam can't tell which game this is.
	// Steam's launcher sets these; Half-Life (and every mod run on it) is app 70.
	if (!getenv("SteamAppId")) {
#ifdef _WIN32
		_putenv("SteamAppId=70");
		_putenv("SteamGameId=70");
#else
		setenv("SteamAppId", "70", 0);
		setenv("SteamGameId", "70", 0);
#endif
	}

	snprintf(path, sizeof(path), "%s/" VGUI2_LIB, FS_BaseDir());
	vgui2_lib = Sys_LoadLibrary(path);
	if (!vgui2_lib) {
		Con_Warnf("Couldn't load %s (%s); no main menu\n", path, Sys_LibraryError());
		return;
	}
	if (!FS_FullPath(GAMEUI_LIB, path, sizeof(path))) {
		Con_Warnf("The game has no " GAMEUI_LIB "; no main menu\n");
		return;
	}
	gameui_lib = Sys_LoadLibrary(path);
	if (!gameui_lib) {
		Con_Warnf("Couldn't load %s (%s); no main menu\n", path, Sys_LibraryError());
		return;
	}
	vgui2_factory = (CreateInterfaceFn)Proc(vgui2_lib, "CreateInterface");
	gameui_factory = (CreateInterfaceFn)Proc(gameui_lib, "CreateInterface");
	client_factory = (CreateInterfaceFn)CL_InterfaceFactory();
	if (!vgui2_factory || !gameui_factory) {
		Con_Warnf("vgui2 or GameUI has no CreateInterface; no main menu\n");
		return;
	}

	// vgui2 and GameUI also read from Half-Life's platform folder (icons, fonts, Steam dialogs)
	FS_AddSearchPath(va("%s/platform", FS_BaseDir()), false);
	// the arrows and check marks are Marlett glyphs; Windows has the font, Linux gets the game's copy
	if (FS_FullPath("resource/marlett.ttf", path, sizeof(path)))
		Font_AddFile(path);

	InitEngineObjects();
	surface = VGS_Create();
	filesystem = VGFS_Create();
	// the order GoldSrc gives them: engine, vgui2, file system, (HTML), GameUI, client
	static CreateInterfaceFn factories[6];
	int count = 0;
	factories[count++] = EngineFactory;
	factories[count++] = vgui2_factory;
	factories[count++] = EngineFactory;
	factories[count++] = EngineFactory;
	factories[count++] = gameui_factory;
	if (client_factory)
		factories[count++] = client_factory;

	vg_ivgui = Need(vgui2_factory, "VGUI_ivgui006");
	vg_ipanel = Need(vgui2_factory, "VGUI_Panel007");
	vg_input = Need(vgui2_factory, "VGUI_InputInternal001");
	vg_scheme = Need(vgui2_factory, "VGUI_Scheme009");
	vg_localize = Need(vgui2_factory, "VGUI_Localize003");
	gameui = Need(gameui_factory, "GameUI007");
	gameconsole = Need(gameui_factory, "GameConsole003");
	if (!vg_ivgui || !vg_ipanel || !vg_input || !vg_scheme || !vg_localize || !gameui)
		return;

	// vgui2's KeyValues pool needs the object size before the first one is made (24 on both compilers)
	void *keyvalues = Need(vgui2_factory, "KeyValues003");
	if (!keyvalues)
		return;
	((void (THISCALL *)(VTHIS, int))METHOD(keyvalues, 0))(VTHIS_ARG(keyvalues), 24);   // RegisterSizeofKeyValues

	bool (THISCALL *vgui_init)(VTHIS, CreateInterfaceFn *, int) = (void *)METHOD(vg_ivgui, 0);
	if (!vgui_init(VTHIS_ARG(vg_ivgui), factories, count)) {
		Con_Warnf("vgui2 didn't start; no main menu\n");
		return;
	}

	MakePanel(&root_panel, 0, true, true);
	VGS_SetEmbeddedPanel(root_panel.vpanel);
	MakePanel(&client_panel, root_panel.vpanel, true, false);
	MakePanel(&gameui_panel, root_panel.vpanel, true, true);

	// the first scheme loaded is everyone's default
	HScheme (THISCALL *load_scheme)(VTHIS, const char *, const char *) = (void *)METHOD(vg_scheme, 0);
	load_scheme(VTHIS_ARG(vg_scheme), "resource/trackerscheme.res", "BaseUI");
	bool (THISCALL *add_file)(VTHIS, void *, const char *) = (void *)METHOD(vg_localize, 0);
	add_file(VTHIS_ARG(vg_localize), filesystem, "resource/valve_%language%.txt");
	((gui_v)METHOD(vg_ivgui, 2))(VTHIS_ARG(vg_ivgui));   // Start

	((void (THISCALL *)(VTHIS, CreateInterfaceFn *, int))METHOD(gameui, GUI_Initialize))(VTHIS_ARG(gameui),
		factories, count);
	((void (THISCALL *)(VTHIS, void *, int, void *))METHOD(gameui, GUI_Start))(VTHIS_ARG(gameui), CL_EngineFuncs(),
		7, NULL);   // CLDLL_INTERFACE_VERSION; no IBaseSystem (demo UI only)
	if (gameconsole)
		((gui_v)METHOD(gameconsole, 1))(VTHIS_ARG(gameconsole));   // Initialize

	loaded = true;
	Con_Printf("GameUI loaded\n");
	GameUI_Activate();
}

void GameUI_Shutdown(void)
{
	if (!loaded)
		return;
	CallGameUI(GUI_Shutdown);
	((gui_v)METHOD(vg_ivgui, 3))(VTHIS_ARG(vg_ivgui));   // Shutdown
	VGS_Shutdown();
	loaded = false;
	// the libraries stay loaded: their static destructors would run against a half-gone engine
}

qboolean GameUI_Loaded(void) { return loaded; }
qboolean GameUI_IsActive(void) { return loaded && active; }

void GameUI_Activate(void)
{
	if (!loaded)
		return;
	if (!active)
		Key_ClearStates();
	active = true;
	VP_SetVisible(gameui_panel.vpanel, true);
	CallGameUI(GUI_ActivateGameUI);
}

void GameUI_Hide(void)
{
	if (!loaded)
		return;
	// without a map there's nothing to go back to
	if (!R_CurrentMap()[0])
		return;
	CallGameUI(GUI_HideGameUI);
	active = false;
	VP_SetVisible(gameui_panel.vpanel, false);
}

void GameUI_Frame(void)
{
	if (!loaded)
		return;
	static int last_w, last_h;
	if (last_w != vid_width || last_h != vid_height) {
		VP_SetSize(root_panel.vpanel, vid_width, vid_height);
		VP_SetSize(client_panel.vpanel, vid_width, vid_height);
		VP_SetSize(gameui_panel.vpanel, vid_width, vid_height);
		last_w = vid_width;
		last_h = vid_height;
	}
	if (test_click_frames && !--test_click_frames) {
		GameUI_MouseButton(test_click_button, true, false);
		GameUI_MouseButton(test_click_button, false, false);
	}
	CallGameUI(GUI_RunFrame);
	((gui_v)METHOD(vg_ivgui, 5))(VTHIS_ARG(vg_ivgui));   // RunFrame: messages, ticks, input
	// GameUI hides itself (Resume Game) through HideGameUI
	bool (THISCALL *is_active)(VTHIS) = (void *)METHOD(gameui, GUI_IsGameUIActive);
	if (active && !is_active(VTHIS_ARG(gameui)) && R_CurrentMap()[0]) {
		active = false;
		VP_SetVisible(gameui_panel.vpanel, false);
	}
	Vid_SetTextInput(Con_IsVisible() || (active && VGS_WantsKeyboard()));
}

void GameUI_Paint(void)
{
	if (!loaded)
		return;
	VGS_Paint();
	VGS_UpdateCursor();
}

qboolean GameUI_WantsMouse(void)
{
	return loaded && active && VGS_WantsMouse() && !Con_IsVisible();
}

// ---------------------------------------------------------------- input

static qboolean keys_down[K_COUNT];

qboolean GameUI_KeyEvent(int key, qboolean down)
{
	if (!loaded || !active || key < 0 || key >= K_COUNT)
		return false;
	if (key >= K_MOUSE1 && key <= K_MOUSE5)
		return true;   // the mouse comes through GameUI_MouseButton
	if (key == K_MWHEELUP || key == K_MWHEELDOWN)
		return true;
	int code = VGUIKeyCode(key);
	if (!code)
		return true;
	if (down) {
		if (!keys_down[key])
			InputInt(IN_InternalKeyCodePressed, code);
		InputInt(IN_InternalKeyCodeTyped, code);
	} else if (keys_down[key]) {
		InputInt(IN_InternalKeyCodeReleased, code);
	}
	keys_down[key] = down;
	return true;
}

void GameUI_CharEvent(const char *s)
{
	if (!loaded || !active)
		return;
	// UTF-8 to code points
	while (*s) {
		unsigned c = (unsigned char)*s++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra)
			c &= 0x3F >> extra;
		for (; extra && (*s & 0xC0) == 0x80; extra--)
			c = (c << 6) | (*s++ & 0x3F);
		if (c >= 32 && c != 127)
			((void (THISCALL *)(VTHIS, wchar_t))METHOD(vg_input, IN_InternalKeyTyped))(VTHIS_ARG(vg_input), (wchar_t)c);
	}
}

void GameUI_MouseMove(int x, int y)
{
	if (!loaded)
		return;
	VGS_MouseMoved(x, y);
	((void (THISCALL *)(VTHIS, int, int))METHOD(vg_input, IN_InternalCursorMoved))(VTHIS_ARG(vg_input), x, y);
}

void GameUI_MouseButton(int button, qboolean down, qboolean dbl)
{
	if (!loaded)
		return;
	InputInt(!down ? IN_InternalMouseReleased : dbl ? IN_InternalMouseDoublePressed : IN_InternalMousePressed, button);
}

void GameUI_MouseWheel(int delta)
{
	if (loaded)
		InputInt(IN_InternalMouseWheeled, delta);
}

// ---------------------------------------------------------------- map loads

// the map loads in one go, so GameUI's loading dialog gets one frame of its own first (GoldSrc's plaque)
void GameUI_LoadingStarted(const char *map)
{
	if (!loaded)
		return;
	((gui_ss)METHOD(gameui, GUI_LoadingStarted))(VTHIS_ARG(gameui), "level", map);
	// the loading dialog is GameUI's progress bar, as when GoldSrc connects to its local server
	((void (THISCALL *)(VTHIS, const char *, int))METHOD(gameui, GUI_StartProgressBar))(VTHIS_ARG(gameui),
		"Connecting", 12);
	((int (THISCALL *)(VTHIS, int, float))METHOD(gameui, GUI_ContinueProgressBar))(VTHIS_ARG(gameui), 1, 0);
	for (int i = 0; i < 2; i++) {   // the first frame lays the dialog out, the second shows it
		GameUI_Frame();
		Vid_BeginFrame();
		Draw_Begin2D();
		GameUI_Paint();
		Vid_EndFrame();
	}
}

void GameUI_LoadingFinished(const char *map)
{
	if (!loaded)
		return;
	((void (THISCALL *)(VTHIS, bool, const char *, const char *))METHOD(gameui, GUI_StopProgressBar))(
		VTHIS_ARG(gameui), false, "", "");
	((gui_ss)METHOD(gameui, GUI_LoadingFinished))(VTHIS_ARG(gameui), "level", map);
	if (active)
		GameUI_Hide();   // GameUI usually does it itself (IBaseUI::HideGameUI)
}

void GameUI_Disconnected(void)
{
	if (!loaded)
		return;
	((void (THISCALL *)(VTHIS, bool, const char *, const char *))METHOD(gameui, GUI_StopProgressBar))(
		VTHIS_ARG(gameui), false, "", "");
	CallGameUI(GUI_DisconnectFromServer);
	GameUI_Activate();
}
