// newen - hosting Half-Life's VGUI2 libraries (vgui2 and GameUI): shared declarations
//
// GameUI (the main menu, its dialogs and the loading screen) is C++ built on vgui2. The engine hands it
// C++ interfaces of its own: a drawing surface, a file system, a few engine services and the panels
// everything hangs from. Here they are C structs whose first word points at a virtual table we fill.
// The slot order comes from Half-Life's own Linux binaries, which carry their debug information
// (hw.so for the engine's side, vgui2.so and gameui.so for theirs).
//
// The two compilers lay virtual tables out differently:
//   GCC:  two destructor slots when the class has a virtual destructor (IBaseInterface does), and two
//         words before slot 0 (offset-to-top, type info). "this" is the first stack argument.
//   MSVC: one destructor slot (scalar deleting, takes a flags argument), and overloaded functions are
//         grouped in reverse order of declaration. thiscall: "this" in ECX, callee pops the
//         arguments; __fastcall with a dummy EDX argument matches it from C.
// Methods with "..." are cdecl on both, with "this" as the first stack argument.
#pragma once
#include <stdbool.h>
#include <wchar.h>
#include "gl_local.h"

#ifdef _WIN32
#define THISCALL __fastcall
#define VTHIS     void *self, void *edx_
#define VTHIS_ARG(p) (p), NULL
#define VT_PREFIX 0
#define VT_DTORS  1
#else
#define THISCALL
#define VTHIS     void *self
#define VTHIS_ARG(p) (p)
#define VT_PREFIX 2
#define VT_DTORS  2
#endif

// a slot of an object's virtual table
#define VFN(obj, slot) ((*(void ***)(obj))[slot])

// an object of ours: one virtual table pointer
typedef struct { void **vtable; } cppobj_t;

// fills the destructor slots (they're never called: nothing deletes the engine's objects)
void   VG_InitVtable(void **mem, int methods);
#define VG_METHOD(vt, i) (vt)[VT_PREFIX + VT_DTORS + (i)]   // i counts from the first method after the destructor

typedef void *(*CreateInterfaceFn)(const char *name, int *ret);

typedef unsigned int  VPANEL;
typedef unsigned long HFont;
typedef unsigned long HScheme;
typedef unsigned long HCursor;
typedef struct { unsigned char r, g, b, a; } vcolor_t;

// ---------------------------------------------------------------- vgui2.so's interfaces (calls into it)

// IPanel ("VGUI_Panel007"), method numbers
enum {
	IPANEL_Init, IPANEL_SetPos, IPANEL_GetPos, IPANEL_SetSize, IPANEL_GetSize, IPANEL_SetMinimumSize,
	IPANEL_GetMinimumSize, IPANEL_SetZPos, IPANEL_GetZPos, IPANEL_GetAbsPos, IPANEL_GetClipRect, IPANEL_SetInset,
	IPANEL_GetInset, IPANEL_SetVisible, IPANEL_IsVisible, IPANEL_SetParent, IPANEL_GetChildCount, IPANEL_GetChild,
	IPANEL_GetParent, IPANEL_MoveToFront, IPANEL_MoveToBack, IPANEL_HasParent, IPANEL_IsPopup, IPANEL_SetPopup,
	IPANEL_Render_GetPopupVisible, IPANEL_Render_SetPopupVisible, IPANEL_GetScheme, IPANEL_IsProportional,
	IPANEL_IsAutoDeleteSet, IPANEL_DeletePanel, IPANEL_SetKeyBoardInputEnabled, IPANEL_SetMouseInputEnabled,
	IPANEL_IsKeyBoardInputEnabled, IPANEL_IsMouseInputEnabled, IPANEL_Solve, IPANEL_GetName, IPANEL_GetClassName,
	IPANEL_SendMessage, IPANEL_Think, IPANEL_PerformApplySchemeSettings, IPANEL_PaintTraverse, IPANEL_Repaint,
	IPANEL_IsWithinTraverse, IPANEL_OnChildAdded, IPANEL_OnSizeChanged, IPANEL_InternalFocusChanged,
	IPANEL_RequestInfo, IPANEL_RequestFocus, IPANEL_RequestFocusPrev, IPANEL_RequestFocusNext,
	IPANEL_GetCurrentKeyFocus, IPANEL_GetTabPosition, IPANEL_Plat, IPANEL_SetPlat, IPANEL_GetPanel,
	IPANEL_IsEnabled, IPANEL_SetEnabled
};

extern void *vg_ipanel, *vg_ivgui, *vg_input, *vg_scheme, *vg_localize;

// thin wrappers over IPanel
void   VP_SetPos(VPANEL p, int x, int y);
void   VP_SetSize(VPANEL p, int w, int h);
void   VP_GetSize(VPANEL p, int *w, int *h);
void   VP_GetAbsPos(VPANEL p, int *x, int *y);
void   VP_GetClipRect(VPANEL p, int *x0, int *y0, int *x1, int *y1);
void   VP_GetInset(VPANEL p, int *l, int *t, int *r, int *b);
void   VP_SetVisible(VPANEL p, bool on);
bool   VP_IsVisible(VPANEL p);
void   VP_SetParent(VPANEL p, VPANEL parent);
int    VP_GetChildCount(VPANEL p);
VPANEL VP_GetChild(VPANEL p, int i);
VPANEL VP_GetParent(VPANEL p);
void   VP_MoveToFront(VPANEL p);
bool   VP_IsPopup(VPANEL p);
void   VP_SetPopup(VPANEL p, bool on);
void   VP_SetZPos(VPANEL p, int z);
void   VP_SetKeyBoardInputEnabled(VPANEL p, bool on);
void   VP_SetMouseInputEnabled(VPANEL p, bool on);
bool   VP_IsKeyBoardInputEnabled(VPANEL p);
bool   VP_IsMouseInputEnabled(VPANEL p);
void   VP_Solve(VPANEL p);
void   VP_Think(VPANEL p);
void   VP_PerformApplySchemeSettings(VPANEL p);
void   VP_PaintTraverse(VPANEL p, bool repaint, bool allowForce);
VPANEL VP_IsWithinTraverse(VPANEL p, int x, int y, bool traversePopups);
const char *VP_GetName(VPANEL p);
bool   VP_IsFullyVisible(VPANEL p);   // it and every parent

// ---------------------------------------------------------------- vgui2_surface.c (ISurface, "VGUI_Surface026")
void  *VGS_Create(void);
void   VGS_SetEmbeddedPanel(VPANEL root);
VPANEL VGS_GetEmbeddedPanel(void);
qboolean VGS_ShouldPaintChild(VPANEL child);   // false for popups: they're painted after the tree
void   VGS_Paint(void);                 // the whole tree, popups on top; in 2D mode
qboolean VGS_WantsMouse(void);          // a visible popup takes the mouse
qboolean VGS_WantsKeyboard(void);
void   VGS_MouseMoved(int x, int y);    // where the cursor is (for SurfaceGetCursorPos)
void   VGS_UpdateCursor(void);          // shows the system cursor vgui asked for
void   VGS_Shutdown(void);

// ---------------------------------------------------------------- vgui2_fs.c (IFileSystem, "VFileSystem009")
void  *VGFS_Create(void);
