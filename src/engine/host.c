// newen - startup, main loop, shutdown
#include "gl_local.h"   // the view (r_refdef) for sound

double   host_realtime;
double   host_frametime;
qboolean host_initialized;
cvar_t  *developer;

static cvar_t *fps_max, *cl_showfps, *host_speeds;
static double  last_time;
static double  fps_time;
static int     fps_frames;
static float   fps_shown;

#define CONFIG_NAME "newen_config.cfg"

static void WriteConfig(void)
{
	FILE *f = FS_OpenWrite(CONFIG_NAME);
	if (!f) {
		Con_Warnf("Couldn't write %s\n", CONFIG_NAME);
		return;
	}
	fprintf(f, "// written by " ENGINE_NAME " - changes here are kept, but it is rewritten on exit\n");
	Key_WriteBindings(f);
	Cvar_WriteArchived(f);
	fclose(f);
}

void Host_Quit(void)
{
	Host_Shutdown();
	exit(0);
}

static void Quit_f(void)    { Host_Quit(); }

static void Version_f(void)
{
	Con_Printf("%s %s (built %s %s)\n", ENGINE_NAME, ENGINE_VERSION, __DATE__, __TIME__);
}

static void WriteConfig_f(void)
{
	WriteConfig();
	Con_Printf("Wrote %s\n", CONFIG_NAME);
}

static void DefaultBindings(void)
{
	Cbuf_AddText("bind ` toggleconsole\n");
	// GoldSrc's default movement keys (newen's own +commands until client.dll provides them)
	Cbuf_AddText("bind w +forward; bind s +back; bind a +moveleft; bind d +moveright; bind SPACE +jump; "
		"bind CTRL +duck; bind SHIFT +speed; bind e +use; bind r +reload; bind MOUSE1 +attack; bind MOUSE2 +attack2; "
		"bind TAB +showscores; bind v noclip; bind f \"impulse 100\"; bind q lastinv; bind t \"impulse 201\"; "
		"bind MWHEELUP invprev; bind MWHEELDOWN invnext; bind 1 slot1; bind 2 slot2; bind 3 slot3; bind 4 slot4; "
		"bind 5 slot5; bind 6 slot6; bind 7 slot7; bind 8 slot8; bind 9 slot9; bind 0 slot10; "
		"bind F6 \"save quick\"; bind F9 \"load quick\"\n");
}

void Host_Init(int argc, char **argv)
{
	com_argc = argc;
	com_argv = argv;

	Cmd_Init();
	Cvar_Init();
	developer = Cvar_Get("developer", COM_CheckParm("-dev") ? "1" : "0", 0, "shows developer messages");
	fps_max = Cvar_Get("fps_max", "300", FCVAR_ARCHIVE, "frame rate cap, 0 = unlimited");
	cl_showfps = Cvar_Get("cl_showfps", "0", FCVAR_ARCHIVE, "shows frames per second");
	host_speeds = Cvar_Get("host_speeds", "0", 0, "prints frame timing");

	Con_Init();
	Key_Init();
	Cmd_AddCommand("quit", Quit_f, "exits the engine");
	Cmd_AddCommand("exit", Quit_f, "exits the engine");
	Cmd_AddCommand("version", Version_f, "prints the engine version");
	Cmd_AddCommand("writeconfig", WriteConfig_f, "saves bindings and settings now");

	FS_Init();
	Con_OpenLog(va("%s/console.log", FS_WriteDir()));
	Con_Printf("%s %s\n", ENGINE_NAME, ENGINE_VERSION);
	Con_Printf("Half-Life: %s\n", FS_BaseDir());
	Con_Printf("Game: %s\n", FS_GameDir());

	// settings: defaults, then the saved config, then the user's autoexec
	DefaultBindings();
	Cbuf_Execute();
	Key_SnapshotDefaults();
	cmd_defer_cvars = true;
	Cmd_ExecFile(CONFIG_NAME, true);
	Cbuf_Execute();
	cmd_defer_cvars = false;
	Cmd_ExecFile("newen_autoexec.cfg", true);
	Cbuf_Execute();

	Vid_Init();
	Draw_Init();
	R_Init();
	S_Init();
	CC_Init();
	Lua_Init();
	CL_Init();
	SV_Init();
	// Half-Life's default config turns on mouse look; without it the mouse's up/down walks you
	if (CL_HasClient())
		Cbuf_AddText("+mlook\n+jlook\n");

	// +commands from the command line, like GoldSrc: +map c1a0 +developer 1
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '+')
			continue;
		strbuf_t sb = { 0 };
		SB_Append(&sb, argv[i] + 1, strlen(argv[i] + 1));
		// arguments run until the next +command or -option (negative numbers are arguments)
		while (i + 1 < argc && argv[i + 1][0] != '+' &&
			!(argv[i + 1][0] == '-' && !(argv[i + 1][1] >= '0' && argv[i + 1][1] <= '9') && argv[i + 1][1] != '.')) {
			i++;
			// quote only arguments that contain spaces, so "+lua_run 2+2" stays code
			qboolean quote = strchr(argv[i], ' ') != NULL;
			SB_Append(&sb, quote ? " \"" : " ", quote ? 2 : 1);
			SB_Append(&sb, argv[i], strlen(argv[i]));
			if (quote)
				SB_Append(&sb, "\"", 1);
		}
		SB_Append(&sb, "\n", 1);
		Cbuf_AddText(sb.data);
		SB_Free(&sb);
	}

	Con_ToggleConsole();   // start with the console open; there's no world or menu yet
	host_initialized = true;
	last_time = Sys_FloatTime();
	Con_Printf("Ready. Type \"cmdlist\" or \"cvarlist\" to look around, \"lua_run <code>\" to run Lua.\n");
}

static void DrawFPS(void)
{
	if (!cl_showfps->value)
		return;
	const char *s = va("%.0f fps", fps_shown);
	Draw_String((float)(vid_width - Draw_StringWidth(s) - 8), 4, s, 255, 255, 255, 255);
}

void Host_Frame(void)
{
	// frame rate cap: sleep most of the wait, spin the last millisecond
	double now = Sys_FloatTime();
	double max = fps_max->value;
	if (max > 0) {
		double target = 1.0 / max;
		while (now - last_time < target) {
			double left = target - (now - last_time);
			if (left > 0.002)
				Sys_Sleep(1);
			now = Sys_FloatTime();
		}
	}
	host_frametime = now - last_time;
	if (host_frametime > 0.25)
		host_frametime = 0.25;   // after a hitch, don't jump the simulation
	last_time = now;
	host_realtime = now;

	fps_frames++;
	if (now - fps_time >= 0.5) {
		fps_shown = (float)(fps_frames / (now - fps_time));
		fps_frames = 0;
		fps_time = now;
	}

	double t0 = Sys_FloatTime();
	Vid_PumpEvents();
	Cbuf_Execute();
	R_UpdateCamera();
	CL_Frame(host_frametime);
	SV_Frame(host_frametime);
	Lua_Frame();
	double t1 = Sys_FloatTime();

	Vid_BeginFrame();
	R_RenderView();
	S_Update(r_refdef.origin, r_refdef.right);
	Draw_Begin2D();
	R_ShadowDrawDebug();
	CL_DrawHUD();
	CC_Draw();
	Lua_DrawOverlay();
	Con_Draw();
	DrawFPS();
	Vid_EndFrame();
	double t2 = Sys_FloatTime();

	if (host_speeds->value)
		Con_Printf("frame %5.2f ms  logic %5.2f ms  render %5.2f ms\n",
			host_frametime * 1000, (t1 - t0) * 1000, (t2 - t1) * 1000);
}

void Host_Shutdown(void)
{
	static qboolean done;
	if (done)
		return;
	done = true;
	if (host_initialized)
		WriteConfig();
	Lua_Shutdown();
	CC_Shutdown();
	CL_Shutdown();
	SV_Shutdown();
	S_Shutdown();
	R_Shutdown();
	Draw_Shutdown();
	Vid_Shutdown();
	FS_Shutdown();
	Con_Shutdown();
}
