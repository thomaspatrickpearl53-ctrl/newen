// newen - window, OpenGL context and input events (SDL2)
#include <SDL.h>
#include <SDL_opengl.h>
#include "common.h"

int vid_width, vid_height;
void Vid_RegisterCommands(void);

static SDL_Window   *window;
static SDL_GLContext glctx;
static cvar_t *vid_mode_width, *vid_mode_height, *vid_fullscreen, *vid_vsync, *vid_borderless;
static int applied_vsync = -1;

static int MapKey(SDL_Keycode k)
{
	if (k >= 'a' && k <= 'z')
		return k;
	if (k >= 32 && k < 127)
		return k;
	switch (k) {
	case SDLK_TAB:       return K_TAB;
	case SDLK_RETURN:    return K_ENTER;
	case SDLK_ESCAPE:    return K_ESCAPE;
	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP:        return K_UPARROW;
	case SDLK_DOWN:      return K_DOWNARROW;
	case SDLK_LEFT:      return K_LEFTARROW;
	case SDLK_RIGHT:     return K_RIGHTARROW;
	case SDLK_LALT: case SDLK_RALT:     return K_ALT;
	case SDLK_LCTRL: case SDLK_RCTRL:   return K_CTRL;
	case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
	case SDLK_F1: return K_F1;   case SDLK_F2: return K_F2;   case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;   case SDLK_F5: return K_F5;   case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;   case SDLK_F8: return K_F8;   case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10; case SDLK_F11: return K_F11; case SDLK_F12: return K_F12;
	case SDLK_INSERT:   return K_INS;
	case SDLK_DELETE:   return K_DEL;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_PAGEUP:   return K_PGUP;
	case SDLK_HOME:     return K_HOME;
	case SDLK_END:      return K_END;
	case SDLK_KP_7: return K_KP_HOME;      case SDLK_KP_8: return K_KP_UPARROW;
	case SDLK_KP_9: return K_KP_PGUP;      case SDLK_KP_4: return K_KP_LEFTARROW;
	case SDLK_KP_5: return K_KP_5;         case SDLK_KP_6: return K_KP_RIGHTARROW;
	case SDLK_KP_1: return K_KP_END;       case SDLK_KP_2: return K_KP_DOWNARROW;
	case SDLK_KP_3: return K_KP_PGDN;      case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_KP_0: return K_KP_INS;       case SDLK_KP_PERIOD: return K_KP_DEL;
	case SDLK_KP_DIVIDE: return K_KP_SLASH; case SDLK_KP_MINUS: return K_KP_MINUS;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_CAPSLOCK: return K_CAPSLOCK;
	case SDLK_PAUSE:    return K_PAUSE;
	}
	return -1;
}

static int MapMouseButton(Uint8 b)
{
	switch (b) {
	case SDL_BUTTON_LEFT:   return K_MOUSE1;
	case SDL_BUTTON_RIGHT:  return K_MOUSE2;
	case SDL_BUTTON_MIDDLE: return K_MOUSE3;
	case SDL_BUTTON_X1:     return K_MOUSE4;
	case SDL_BUTTON_X2:     return K_MOUSE5;
	}
	return -1;
}

static void UpdateSize(void)
{
	SDL_GL_GetDrawableSize(window, &vid_width, &vid_height);
}

static Uint32 WindowFlags(void)
{
	Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (vid_fullscreen->value)
		flags |= vid_borderless->value ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
	return flags;
}

static void Vid_Restart_f(void)
{
	SDL_SetWindowSize(window, (int)vid_mode_width->value, (int)vid_mode_height->value);
	if (vid_fullscreen->value)
		SDL_SetWindowFullscreen(window, vid_borderless->value ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN);
	else {
		SDL_SetWindowFullscreen(window, 0);
		SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	}
	UpdateSize();
	Con_Printf("Video mode: %dx%d%s\n", vid_width, vid_height, vid_fullscreen->value ? " fullscreen" : "");
}

void Vid_Init(void)
{
	vid_mode_width = Cvar_Get("vid_width", "1280", FCVAR_ARCHIVE, "window width (vid_restart to apply)");
	vid_mode_height = Cvar_Get("vid_height", "720", FCVAR_ARCHIVE, "window height (vid_restart to apply)");
	vid_fullscreen = Cvar_Get("vid_fullscreen", "0", FCVAR_ARCHIVE, "1 = fullscreen (vid_restart to apply)");
	vid_borderless = Cvar_Get("vid_borderless", "1", FCVAR_ARCHIVE, "fullscreen uses a borderless desktop-sized window");
	vid_vsync = Cvar_Get("vid_vsync", "0", FCVAR_ARCHIVE, "0 off, 1 on, -1 adaptive");
	Cmd_AddCommand("vid_restart", Vid_Restart_f, "applies vid_width/vid_height/vid_fullscreen");
	Vid_RegisterCommands();

	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
	SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");   // raw input for mouse look
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0)
		Sys_Error("SDL_Init failed: %s", SDL_GetError());

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	int w = (int)vid_mode_width->value, h = (int)vid_mode_height->value;
	if (w < 320) w = 320;
	if (h < 200) h = 200;
	window = SDL_CreateWindow(ENGINE_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, WindowFlags());
	if (!window)
		Sys_Error("Couldn't create a window: %s", SDL_GetError());
	glctx = SDL_GL_CreateContext(window);
	if (!glctx)
		Sys_Error("Couldn't create an OpenGL context: %s", SDL_GetError());
	SDL_GL_MakeCurrent(window, glctx);
	UpdateSize();

	Con_Printf("OpenGL: %s / %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
	Con_Printf("Video mode: %dx%d%s\n", vid_width, vid_height, vid_fullscreen->value ? " fullscreen" : "");
}

void Vid_Shutdown(void)
{
	if (glctx)
		SDL_GL_DeleteContext(glctx);
	if (window)
		SDL_DestroyWindow(window);
	glctx = NULL;
	window = NULL;
	SDL_Quit();
}

void Vid_SetTextInput(qboolean on)
{
	if (on)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
}

char *Vid_GetClipboard(void)
{
	if (!SDL_HasClipboardText())
		return NULL;
	char *t = SDL_GetClipboardText();
	if (!t)
		return NULL;
	char *copy = Mem_Strdup(t);
	SDL_free(t);
	return copy;
}

static int mouse_dx, mouse_dy;
static qboolean relative_mouse;

void Vid_SetRelativeMouse(qboolean on)
{
	if (on == relative_mouse)
		return;
	relative_mouse = on;
	SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
	mouse_dx = mouse_dy = 0;
}

void Vid_GetMouseDelta(int *dx, int *dy)
{
	*dx = mouse_dx;
	*dy = mouse_dy;
	mouse_dx = mouse_dy = 0;
}

void Vid_PumpEvents(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_MOUSEMOTION:
			if (relative_mouse) {
				mouse_dx += e.motion.xrel;
				mouse_dy += e.motion.yrel;
			}
			break;
		case SDL_QUIT:
			Host_Quit();
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
				UpdateSize();
			else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				Key_ClearStates();
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			// the key left of 1 toggles the console on any keyboard layout
			int key = e.key.keysym.scancode == SDL_SCANCODE_GRAVE ? '`' : MapKey(e.key.keysym.sym);
			if (key >= 0)
				Key_Event(key, e.type == SDL_KEYDOWN);
			break;
		}
		case SDL_TEXTINPUT:
			if (Con_IsVisible())
				for (const char *p = e.text.text; *p; p++)
					Con_CharEvent((unsigned char)*p);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int key = MapMouseButton(e.button.button);
			if (key >= 0)
				Key_Event(key, e.type == SDL_MOUSEBUTTONDOWN);
			break;
		}
		case SDL_MOUSEWHEEL:
			if (e.wheel.y) {
				int key = e.wheel.y > 0 ? K_MWHEELUP : K_MWHEELDOWN;
				Key_Event(key, true);
				Key_Event(key, false);
			}
			break;
		}
	}
}

void Vid_BeginFrame(void)
{
	int vs = (int)vid_vsync->value;
	if (vs != applied_vsync) {
		if (SDL_GL_SetSwapInterval(vs) < 0 && vs == -1)
			SDL_GL_SetSwapInterval(1);
		applied_vsync = vs;
	}
	glViewport(0, 0, vid_width, vid_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static qboolean screenshot_pending;

// Writes the finished frame to userdata/<game>/screenshots/shotNNNN.tga
static void WriteScreenshot(void)
{
	char name[64];
	int i;
	for (i = 0; i < 10000; i++) {
		snprintf(name, sizeof(name), "screenshots/shot%04d.tga", i);
		if (!Sys_FileExists(va("%s/%s", FS_WriteDir(), name)))
			break;
	}
	if (i == 10000) {
		Con_Errorf("screenshot: too many screenshots\n");
		return;
	}
	size_t size = (size_t)vid_width * vid_height * 3;
	unsigned char *px = Mem_Alloc(size);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, vid_width, vid_height, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);

	FILE *f = FS_OpenWrite(name);
	if (!f) {
		Mem_Free(px);
		Con_Errorf("screenshot: couldn't write %s\n", name);
		return;
	}
	unsigned char header[18] = { 0 };
	header[2] = 2;   // uncompressed true-color, rows bottom-up like glReadPixels
	header[12] = (unsigned char)(vid_width & 255);
	header[13] = (unsigned char)(vid_width >> 8);
	header[14] = (unsigned char)(vid_height & 255);
	header[15] = (unsigned char)(vid_height >> 8);
	header[16] = 24;
	fwrite(header, 1, sizeof(header), f);
	fwrite(px, 1, size, f);
	fclose(f);
	Mem_Free(px);
	Con_Printf("Wrote %s\n", name);
}

static void Screenshot_f(void)
{
	screenshot_pending = true;
}

void Vid_RegisterCommands(void)
{
	Cmd_AddCommand("screenshot", Screenshot_f, "saves the next frame to screenshots/");
}

void Vid_EndFrame(void)
{
	if (screenshot_pending) {
		screenshot_pending = false;
		WriteScreenshot();
	}
	SDL_GL_SwapWindow(window);
}
