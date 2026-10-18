// newen - the client side: loads client.dll and gives it the engine functions it needs
//
// The client DLL draws the HUD, turns input into movement commands, computes the view (bob, punch,
// view model placement) and receives the game's user messages. The engine and the server run in
// one process, so "network" messages from the game DLL are queued here and handed to the client's
// message hooks at the start of each client frame.
//
// Events, temp entities and the other effects are in cl_event.c, cl_tent.c, r_part.c, r_beam.c,
// r_decal.c; titles, screen fade and shake in cl_view.c; the triangle API in cl_tri.c.
// Not yet: sound, the studio model interface (the engine draws models itself), dynamic lights.
#include "cl_local.h"

#pragma warning(disable: 4152)   // GetProcAddress results become function pointers

qboolean VGUI_Init(void);
void    *VGUI_GetPanel(void);
void     VGUI_Paint(void);
void     CL_InitInput(void);
void     CL_CreateCmdEngine(float frametime, usercmd_t *cmd);
const char *SV_UserMsgName(int type);
pmtrace_t *SV_PM_TraceLine(float *start, float *end, int flags, int usehull, int ignore_pe);
movevars_t *SV_MoveVars(void);
const char *Key_KeynameForBinding(const char *binding);

static HMODULE client_dll;
cldll_exports_t cl;
static cl_enginefunc_t cl_funcs;
qboolean client_ok;
static qboolean map_ready;   // HUD_VidInit has run for the current map

static triangleapi_t tri_api;
static efx_api_t efx_api;
static event_api_t event_api;
static demo_api_t demo_api;
static net_api_t net_api;
static voicetweak_t voice_api;

static cl_entity_t *cl_entities;
static int cl_entities_count;
cl_entity_t cl_viewent;
static usercmd_t last_cmd;
double cl_time, cl_oldtime;
static cvar_t *crosshair, *cl_drawhud;

qboolean CL_HasClient(void) { return client_ok; }

// ---------------------------------------------------------------- guarded calls into the DLL

char cl_crash_where[256];

// remembers where a crash happened: module and offset, for finding the engine function at fault
int CL_CrashFilter(EXCEPTION_POINTERS *ep)
{
	void *addr = ep->ExceptionRecord->ExceptionAddress;
	HMODULE mod = NULL;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod))
		GetModuleFileNameA(mod, name, sizeof(name));
	const char *base = strrchr(name, 92);   // backslash
	snprintf(cl_crash_where, sizeof(cl_crash_where), "%s+0x%x (reading 0x%08lx)", base ? base + 1 : name,
		(unsigned)((unsigned char *)addr - (unsigned char *)mod),
		ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul);
	Con_Errorf("Crash inside the client (exception 0x%08lx), call stack:\n", ep->ExceptionRecord->ExceptionCode);
	Sys_LogException(ep);
	return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------- sprites (HUD)

static model_t **hud_sprites;
static int hud_sprites_count;
static model_t *spr_current;
static int spr_color[3] = { 255, 255, 255 };
static qboolean scissor_on;
static int scissor[4];

void CL_ClearSprites(void)
{
	hud_sprites_count = 0;   // the models themselves go with Mod_ClearAll
	spr_current = NULL;
}

static HSPRITE_t SPR_Load(const char *name)
{
	if (!name || !*name)
		return 0;
	for (int i = 0; i < hud_sprites_count; i++)
		if (hud_sprites[i] && !stricmp(hud_sprites[i]->name, name))
			return i + 1;
	model_t *m = Mod_ForName(name, false);
	if (!m || m->type != mod_sprite) {
		Con_DPrintf("SPR_Load: %s isn't a sprite\n", name);
		return 0;
	}
	hud_sprites = Mem_Realloc(hud_sprites, sizeof(model_t *) * (hud_sprites_count + 1));
	hud_sprites[hud_sprites_count++] = m;
	return hud_sprites_count;
}

static model_t *SpriteForHandle(HSPRITE_t h)
{
	return (h > 0 && h <= hud_sprites_count) ? hud_sprites[h - 1] : NULL;
}

static const spriteframe_t *SpriteFrame(model_t *m, int frame)
{
	if (!m || m->type != mod_sprite || !m->sprite.numframes)
		return NULL;
	if (frame < 0 || frame >= m->sprite.numframes)
		frame = 0;
	return &m->sprite.frames[frame].frames[0];
}

static int SPR_Frames(HSPRITE_t h) { model_t *m = SpriteForHandle(h); return m ? m->sprite.numframes : 0; }

static int SPR_Height(HSPRITE_t h, int frame)
{
	const spriteframe_t *f = SpriteFrame(SpriteForHandle(h), frame);
	return f ? (int)(f->up - f->down) : 0;
}

static int SPR_Width(HSPRITE_t h, int frame)
{
	const spriteframe_t *f = SpriteFrame(SpriteForHandle(h), frame);
	return f ? (int)(f->right - f->left) : 0;
}

static void SPR_Set(HSPRITE_t h, int r, int g, int b)
{
	spr_current = SpriteForHandle(h);
	spr_color[0] = r; spr_color[1] = g; spr_color[2] = b;
}

enum { SPRMODE_NORMAL, SPRMODE_HOLES, SPRMODE_ADDITIVE };

static void DrawSpriteFrame(model_t *m, int frame, int x, int y, const wrect_t *prc, int mode, const int color[3], int dw, int dh)
{
	const spriteframe_t *f = SpriteFrame(m, frame);
	if (!f)
		return;
	int w = (int)(f->right - f->left), h = (int)(f->up - f->down);
	wrect_t rc = { 0, w, 0, h };
	if (prc)
		rc = *prc;
	if (rc.right <= rc.left || rc.bottom <= rc.top || w <= 0 || h <= 0)
		return;
	float s0 = (float)rc.left / w, s1 = (float)rc.right / w, t0 = (float)rc.top / h, t1 = (float)rc.bottom / h;
	int qw = dw > 0 ? dw : rc.right - rc.left, qh = dh > 0 ? dh : rc.bottom - rc.top;

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, f->gl->id);
	glDisable(GL_ALPHA_TEST);
	if (mode == SPRMODE_ADDITIVE) {
		// weighted by opacity: alpha-test sprites (the crosshairs) keep a colour in their see-through pixels
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	} else if (mode == SPRMODE_HOLES) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.5f);
	} else {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	glColor4ub((GLubyte)color[0], (GLubyte)color[1], (GLubyte)color[2], 255);
	glBegin(GL_QUADS);
	glTexCoord2f(s0, t0); glVertex2i(x, y);
	glTexCoord2f(s1, t0); glVertex2i(x + qw, y);
	glTexCoord2f(s1, t1); glVertex2i(x + qw, y + qh);
	glTexCoord2f(s0, t1); glVertex2i(x, y + qh);
	glEnd();
	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, 1);
}

static void SPR_Draw(int frame, int x, int y, const wrect_t *prc)         { DrawSpriteFrame(spr_current, frame, x, y, prc, SPRMODE_NORMAL, spr_color, 0, 0); }
static void SPR_DrawHoles(int frame, int x, int y, const wrect_t *prc)    { DrawSpriteFrame(spr_current, frame, x, y, prc, SPRMODE_HOLES, spr_color, 0, 0); }
static void SPR_DrawAdditive(int frame, int x, int y, const wrect_t *prc) { DrawSpriteFrame(spr_current, frame, x, y, prc, SPRMODE_ADDITIVE, spr_color, 0, 0); }

static void SPR_DrawGeneric(int frame, int x, int y, const wrect_t *prc, int src, int dest, int w, int h)
{
	DrawSpriteFrame(spr_current, frame, x, y, prc, SPRMODE_NORMAL, spr_color, w, h);
}

static void SPR_EnableScissor(int x, int y, int w, int h)
{
	scissor_on = true;
	scissor[0] = x; scissor[1] = y; scissor[2] = w; scissor[3] = h;
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, vid_height - (y + h), w, h);
}

static void SPR_DisableScissor(void)
{
	scissor_on = false;
	glDisable(GL_SCISSOR_TEST);
}

// hud.txt: count, then "name resolution sprite x y w h" per line
static client_sprite_t *SPR_GetList(const char *psz, int *count)
{
	*count = 0;
	char *data = FS_LoadFile(psz, NULL);
	if (!data)
		return NULL;
	const char *p = COM_Parse(data);
	int n = p ? atoi(com_token) : 0;
	client_sprite_t *list = NULL;
	if (n > 0) {
		list = Mem_Alloc(sizeof(client_sprite_t) * n);   // the client keeps it for the session
		int i;
		for (i = 0; i < n; i++) {
			client_sprite_t *s = &list[i];
			if (!(p = COM_Parse(p))) break;
			Q_strncpy(s->szName, com_token, sizeof(s->szName));
			if (!(p = COM_Parse(p))) break;
			s->iRes = atoi(com_token);
			if (!(p = COM_Parse(p))) break;
			Q_strncpy(s->szSprite, com_token, sizeof(s->szSprite));
			if (!(p = COM_Parse(p))) break;
			s->rc.left = atoi(com_token);
			if (!(p = COM_Parse(p))) break;
			s->rc.top = atoi(com_token);
			if (!(p = COM_Parse(p))) break;
			s->rc.right = s->rc.left + atoi(com_token);
			if (!(p = COM_Parse(p))) break;
			s->rc.bottom = s->rc.top + atoi(com_token);
		}
		*count = i;
	}
	Mem_Free(data);
	return list;
}

static const struct model_s *GetSpritePointer(HSPRITE_t h) { return SpriteForHandle(h); }

// ---------------------------------------------------------------- 2D drawing and text

static HSPRITE_t crosshair_sprite;
static wrect_t crosshair_rect;
static int crosshair_color[3];
static float text_color[3] = { 1, 1, 1 };
static char center_text[1024];
static double center_time;

static void FillRGBA(int x, int y, int w, int h, int r, int g, int b, int a)
{
	// GoldSrc's FillRGBA is additive
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	glVertex2i(x, y); glVertex2i(x + w, y); glVertex2i(x + w, y + h); glVertex2i(x, y + h);
	glEnd();
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

static void FillRGBABlend(int x, int y, int w, int h, int r, int g, int b, int a)
{
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
	glBegin(GL_QUADS);
	glVertex2i(x, y); glVertex2i(x + w, y); glVertex2i(x + w, y + h); glVertex2i(x, y + h);
	glEnd();
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

// ---------------------------------------------------------------- the title font

// The client draws titles.txt messages and the credits one character at a time. Steam GoldSrc uses
// its "CreditsFont" for that: Trebuchet MS, 18 tall at 480 lines and scaled with the screen,
// semi-bold, antialiased. It's drawn additively: the client fades text by darkening its colour.
static struct {
	int built_for;   // screen height the atlas was made for
	char face[64];
	fontatlas_t atlas;
	GLuint tex;
} tfont;

static cvar_t *cl_titlefont;

static qboolean TitleFont(void)
{
	if (!cl_titlefont)
		return false;
	if (tfont.built_for == vid_height && !strcmp(tfont.face, cl_titlefont->string))
		return tfont.tex != 0;
	tfont.built_for = vid_height;
	Q_strncpy(tfont.face, cl_titlefont->string, sizeof(tfont.face));
	Mem_Free(tfont.atlas.alpha);
	if (!tfont.tex)
		glGenTextures(1, &tfont.tex);
	int tall = (18 * vid_height + 240) / 480;
	if (tall < 12)
		tall = 12;
	if (!Font_RasterizeProportional(cl_titlefont->string, tall, 600, &tfont.atlas)) {
		glDeleteTextures(1, &tfont.tex);
		tfont.tex = 0;
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, tfont.tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, tfont.atlas.width, tfont.atlas.height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, tfont.atlas.alpha);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

static int TitleCharWidth(int ch)
{
	return ch >= 32 && ch < 127 ? tfont.atlas.advance[ch] : 0;
}

static int GetScreenInfo(SCREENINFO *s)
{
	if (!s || s->iSize != sizeof(SCREENINFO))
		return 0;
	s->iWidth = vid_width;
	s->iHeight = vid_height;
	s->iFlags = SCRINFO_SCREENFLASH;
	if (TitleFont()) {
		s->iCharHeight = tfont.atlas.cell_h;
		for (int i = 0; i < 256; i++)
			s->charWidths[i] = (short)TitleCharWidth(i);
	} else {
		s->iCharHeight = Draw_LineHeight();
		for (int i = 0; i < 256; i++)
			s->charWidths[i] = (short)Draw_CharWidth();
	}
	return 1;
}

static void SetCrosshair(HSPRITE_t h, wrect_t rc, int r, int g, int b)
{
	crosshair_sprite = h;
	crosshair_rect = rc;
	crosshair_color[0] = r; crosshair_color[1] = g; crosshair_color[2] = b;
}

static int DrawCharacter(int x, int y, int ch, int r, int g, int b)
{
	ch &= 255;
	if (!TitleFont()) {
		char s[2] = { (char)ch, 0 };
		if (ch > 32 && ch < 127)
			Draw_String((float)x, (float)y, s, r, g, b, 255);
		return Draw_CharWidth();
	}
	if (ch > 32 && ch < 127) {
		const fontatlas_t *a = &tfont.atlas;
		int i = ch - 32, cx = (i % 16) * a->cell_w, cy = (i / 16) * a->cell_h;
		float s0 = (float)cx / a->width, s1 = (float)(cx + a->cell_w) / a->width;
		float t0 = (float)cy / a->height, t1 = (float)(cy + a->cell_h) / a->height;
		int x0 = x - a->pad;
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tfont.tex);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, 255);
		glBegin(GL_QUADS);
		glTexCoord2f(s0, t0); glVertex2i(x0, y);
		glTexCoord2f(s1, t0); glVertex2i(x0 + a->cell_w, y);
		glTexCoord2f(s1, t1); glVertex2i(x0 + a->cell_w, y + a->cell_h);
		glTexCoord2f(s0, t1); glVertex2i(x0, y + a->cell_h);
		glEnd();
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4f(1, 1, 1, 1);
	}
	return TitleCharWidth(ch);
}

static int DrawConsoleString(int x, int y, const char *s)
{
	if (!s)
		return x;
	int w = Draw_String((float)x, (float)y, s, (int)(text_color[0] * 255), (int)(text_color[1] * 255), (int)(text_color[2] * 255), 255);
	return x + w;
}

static void DrawSetTextColor(float r, float g, float b) { text_color[0] = r; text_color[1] = g; text_color[2] = b; }

static void DrawConsoleStringLen(const char *s, int *len, int *h)
{
	if (len) *len = s ? Draw_StringWidth(s) : 0;
	if (h) *h = Draw_LineHeight();
}

static int DrawString(int x, int y, const char *s, int r, int g, int b)
{
	return s ? x + Draw_String((float)x, (float)y, s, r, g, b, 255) : x;
}

static int DrawStringReverse(int x, int y, const char *s, int r, int g, int b)
{
	if (!s)
		return x;
	int w = Draw_StringWidth(s);
	Draw_String((float)(x - w), (float)y, s, r, g, b, 255);
	return x - w;
}

static int VGUI2DrawCharacter(int x, int y, int ch, unsigned font) { return DrawCharacter(x, y, ch, 255, 255, 255); }
static int VGUI2DrawCharacterAdditive(int x, int y, int ch, int r, int g, int b, unsigned font) { return DrawCharacter(x, y, ch, r, g, b); }

static void ConsolePrint(const char *s) { if (s) Con_Printf("%s", s); }

static void CenterPrint(const char *s)
{
	Q_strncpy(center_text, s ? s : "", sizeof(center_text));
	center_time = host_realtime;
}

static void GetScreenFade(screenfade_t *f) { if (f) CL_GetScreenFade(f); }
static void SetScreenFade(screenfade_t *f) { if (f) CL_SetScreenFade(f); }

// ---------------------------------------------------------------- cvars, commands, messages

static cvar_t *RegisterVariable(const char *name, const char *value, int flags)
{
	return Cvar_Get(name, value, flags | FCVAR_CLIENTDLL, NULL);
}

static float GetCvarFloat(const char *n) { cvar_t *v = Cvar_Find(n); return v ? v->value : 0.0f; }
static char *GetCvarString(const char *n) { cvar_t *v = Cvar_Find(n); return v ? v->string : ""; }
static void ClCvar_SetValue(const char *n, float f) { Cvar_SetValue(n, f); }
static void ClCvar_Set(const char *n, const char *v) { Cvar_Set(n, v); }
static cvar_t *GetCvarPointer(const char *n)
{
	cvar_t *v = Cvar_Find(n);
	if (!v)
		Con_DPrintf("client.dll asked for cvar %s, which doesn't exist\n", n);
	return v;
}

static int AddCommand(const char *name, void (*fn)(void))
{
	if (!name || !fn)
		return 0;
	if (Cmd_Exists(name)) {
		Con_DPrintf("client.dll: command %s already exists\n", name);
		return 0;
	}
	Cmd_AddCommand(name, fn, "client DLL command");
	return 1;
}

typedef struct { char name[32]; pfnUserMsgHook fn; } msghook_t;
static msghook_t *msghooks;
static int msghooks_count;

static int HookUserMsg(const char *name, pfnUserMsgHook fn)
{
	for (int i = 0; i < msghooks_count; i++)
		if (!strcmp(msghooks[i].name, name)) {
			msghooks[i].fn = fn;
			return 1;
		}
	msghooks = Mem_Realloc(msghooks, sizeof(msghook_t) * (msghooks_count + 1));
	Q_strncpy(msghooks[msghooks_count].name, name, sizeof(msghooks[0].name));
	msghooks[msghooks_count].fn = fn;
	msghooks_count++;
	return 1;
}

static int ServerCmd(const char *s)
{
	// the server is local: run it as a console command, which reaches the game's ClientCommand
	if (s) {
		Cbuf_AddText(s);
		Cbuf_AddText("\n");
	}
	return 1;
}

static int ClientCmd(const char *s) { return ServerCmd(s); }

static void GetPlayerInfo(int ent_num, hud_player_info_t *info)
{
	if (!info)
		return;
	static char name[64], model[64];
	edict_t *pl = SV_LocalPlayer();
	if (ent_num != 1 || !pl) {
		info->name = NULL;
		return;
	}
	Q_strncpy(name, SV_InfoValueForKey(sv.clientinfo[0], "name"), sizeof(name));
	Q_strncpy(model, SV_InfoValueForKey(sv.clientinfo[0], "model"), sizeof(model));
	info->name = name;
	info->ping = 0;
	info->thisplayer = 1;
	info->spectator = 0;
	info->packetloss = 0;
	info->model = model;
	info->topcolor = (short)atoi(SV_InfoValueForKey(sv.clientinfo[0], "topcolor"));
	info->bottomcolor = (short)atoi(SV_InfoValueForKey(sv.clientinfo[0], "bottomcolor"));
	info->m_nSteamID = 0;
}

static void PlaySoundByName(const char *s, float vol) { S_LocalSound(s, vol); }

static void PlaySoundByIndex(int i, float vol)
{
	if (i > 0 && i < sv.num_sounds)
		S_LocalSound(sv.sound_precache[i], vol);
}

static void PlaySoundByNameAtLocation(const char *s, float vol, float *org)
{
	S_StartSound(0, 0, s, org, vol, 0.8f, 0, 100);   // ATTN_NORM
}
static client_textmessage_t *TextMessageGet(const char *name) { return CL_TextMessageGet(name); }

static void ClAngleVectors(const float *a, float *f, float *r, float *u) { AngleVectors(a, f, r, u); }

static int GetWindowCenterX(void) { int x, y; Vid_GetWindowCenter(&x, &y); return x; }
static int GetWindowCenterY(void) { int x, y; Vid_GetWindowCenter(&x, &y); return y; }
static void GetViewAngles(float *a) { VectorCopy(cl_viewangles, a); }
static void SetViewAngles(float *a) { VectorCopy(a, cl_viewangles); }
static int GetMaxClients(void) { return sv.maxclients > 0 ? sv.maxclients : 1; }
static int ClCmd_Argc(void) { return Cmd_Argc(); }
static const char *ClCmd_Argv(int i) { return Cmd_Argv(i); }

static void ClCon_Printf(const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("%s", buf);
}

static void ClCon_DPrintf(const char *fmt, ...)
{
	if (!developer->value)
		return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("%s", buf);
}

static void ClCon_NPrintf(int pos, const char *fmt, ...) {}
static void ClCon_NXPrintf(con_nprint_t *info, const char *fmt, ...) {}

static const char *PhysInfo_ValueForKey(const char *key) { return SV_InfoValueForKey(SV_PhysInfo(), key); }
static const char *ServerInfo_ValueForKey(const char *key) { return SV_InfoValueForKey(sv.serverinfo, key); }
static float GetClientMaxspeed(void) { edict_t *pl = SV_LocalPlayer(); return pl ? pl->v.maxspeed : 0.0f; }

static int ClCheckParm(const char *parm, char **next)
{
	int i = COM_CheckParm(parm);
	if (next)
		*next = (i && i + 1 < com_argc) ? com_argv[i + 1] : NULL;
	return i;
}

static void ClKey_Event(int key, int down) { Key_Event(key, down != 0); }
static void GetMousePosition(int *x, int *y) { SDL_GetMouseState(x, y); }
static int IsNoClipping(void) { edict_t *pl = SV_LocalPlayer(); return pl && pl->v.movetype == MOVETYPE_NOCLIP; }

cl_entity_t *CL_EntityByIndex(int i)
{
	if (i < 0 || i >= cl_entities_count)
		return NULL;
	return &cl_entities[i];
}

static cl_entity_t *GetEntityByIndex(int i) { return CL_EntityByIndex(i); }
static cl_entity_t *GetLocalPlayer(void) { return CL_EntityByIndex(1); }
static cl_entity_t *GetViewModel(void) { return &cl_viewent; }
static float GetClientTime(void) { return (float)cl_time; }
static float GetClientOldTime(void) { return (float)cl_oldtime; }
static void V_CalcShake(void) { CL_CalcShake(); }
static void V_ApplyShake(float *origin, float *angles, float factor) { CL_ApplyShake(origin, angles, factor); }

static int ClPM_PointContents(float *p, int *truecontents)
{
	int c = SV_TruePointContents(p);
	if (truecontents)
		*truecontents = c;
	return SV_PointContents(p);
}

static int PM_WaterEntity(float *p) { return -1; }
static pmtrace_t *ClPM_TraceLine(float *start, float *end, int flags, int usehull, int ignore_pe)
{
	return SV_PM_TraceLine(start, end, flags, usehull, ignore_pe);
}

static struct model_s *CL_LoadModel(const char *name, int *index)
{
	if (index)
		*index = sv.active ? SV_ModelIndex(name) : 0;
	return Mod_ForName(name, false);
}

int CL_AddVisibleEntity(cl_entity_t *ent)
{
	if (!ent || !ent->model || ent->model->type == mod_brush)
		return 0;
	rentity_t *r = R_AddFrameEntity();
	r->model = ent->model;
	VectorCopy(ent->origin, r->origin);
	VectorCopy(ent->angles, r->angles);
	r->sequence = ent->curstate.sequence;
	r->frame = ent->curstate.frame;
	r->framerate = ent->model->type == mod_studio ? ent->curstate.framerate : 0;
	r->animtime = r_refdef.time - (cl_time - ent->curstate.animtime);
	r->body = ent->curstate.body;
	r->skin = ent->curstate.skin;
	r->rendermode = ent->curstate.rendermode;
	r->renderamt = r->rendermode == kRenderNormal ? 255 : ent->curstate.renderamt;
	r->rendercolor[0] = ent->curstate.rendercolor.r;
	r->rendercolor[1] = ent->curstate.rendercolor.g;
	r->rendercolor[2] = ent->curstate.rendercolor.b;
	r->scale = ent->curstate.scale;
	return 1;
}

static int CL_CreateVisibleEntity(int type, cl_entity_t *ent) { return CL_AddVisibleEntity(ent); }

static unsigned short ClPrecacheEvent(int type, const char *name) { return SV_PrecacheEvent(type, name); }
static void ClPlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, float *origin, float *angles,
	float f1, float f2, int i1, int i2, int b1, int b2)
{
	CL_PlaybackEvent(flags | FEV_CLIENT, invoker, index, delay, origin, angles, f1, f2, i1, i2, b1, b2);
}

void CL_SetViewModelAnim(int anim, int body)
{
	cl_viewent.curstate.sequence = anim;
	cl_viewent.curstate.body = body;
	cl_viewent.curstate.frame = 0;
	cl_viewent.curstate.framerate = 1;
	cl_viewent.curstate.animtime = (float)cl_time;
}

static void WeaponAnim(int anim, int body) { CL_SetViewModelAnim(anim, body); }

static unsigned rng = 0x2545F491u;
static unsigned NextRand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float ClRandomFloat(float lo, float hi) { return lo + (hi - lo) * ((NextRand() >> 8) / 16777216.0f); }
static long ClRandomLong(long lo, long hi) { return hi <= lo ? lo : lo + (long)(NextRand() % (unsigned)(hi - lo + 1)); }

static int ClCon_IsVisible(void) { return Con_IsVisible(); }
static const char *GetGameDirectory(void) { return FS_GameDir(); }
static const char *ClKey_LookupBinding(const char *b) { const char *k = Key_KeynameForBinding(b); return k ? k : "<not bound>"; }
static const char *GetLevelName(void) { return world.loaded ? world.name : ""; }
static void *ClVGui_GetPanel(void) { return VGUI_GetPanel(); }
static void VGui_ViewportPaintBackground(int extents[4]) {}

static unsigned char *ClCOM_LoadFile(const char *path, int usehunk, int *len)
{
	size_t n = 0;
	unsigned char *data = FS_LoadFile(path, &n);
	if (len)
		*len = data ? (int)n : 0;
	return data;
}

static char *ClCOM_ParseFile(char *data, char *token)
{
	const char *p = COM_Parse(data);
	if (token) {
		size_t n = strlen(com_token);
		memcpy(token, com_token, n + 1);
	}
	return (char *)p;
}

static void ClCOM_FreeFile(void *p) { Mem_Free(p); }
static int IsSpectateOnly(void) { return 0; }
static struct model_s *LoadMapSprite(const char *name) { return Mod_ForName(name, false); }
static void COM_AddAppDirectoryToSearchPath(const char *base, const char *app) {}

static int COM_ExpandFilename(const char *name, char *out, int size)
{
	return FS_FullPath(name, out, (size_t)size);
}

static const char *PlayerInfo_ValueForKey(int n, const char *key)
{
	if (n != 1 || !SV_LocalPlayer())
		return NULL;
	return SV_InfoValueForKey(sv.clientinfo[0], key);
}

static void PlayerInfo_SetValueForKey(const char *key, const char *value) {}
static const char *LocalPlayerInfo_ValueForKey(const char *key) { return SV_InfoValueForKey(sv.clientinfo[0], key); }
static qboolean GetPlayerUniqueID(int i, char id[16]) { return false; }
static int GetTrackerIDForPlayer(int slot) { return 0; }
static int GetPlayerForTrackerID(int id) { return 0; }
static void ClGetMousePos(void *pt) { if (pt) { int *p = pt; SDL_GetGlobalMouseState(&p[0], &p[1]); } }
static void ClSetMousePos(int x, int y) {}
static void ClSetMouseEnable(qboolean on) {}
static cvar_t *GetFirstCvarPtr(void) { return Cvar_First(); }
static unsigned GetFirstCmdFunctionHandle(void) { return 0; }
static unsigned GetNextCmdFunctionHandle(unsigned h) { return 0; }
static const char *GetCmdFunctionName(unsigned h) { return ""; }
static float GetServerGravityValue(void) { return sv_gravity ? sv_gravity->value : 800.0f; }
static struct model_s *GetModelByIndex(int i) { return SV_ModelForIndex(i); }
static void SetFilterMode(int mode) {}
static void SetFilterColor(float r, float g, float b) {}
static void SetFilterBrightness(float b) {}
static void *ClSequenceGet(const char *file, const char *entry) { return NULL; }
static void *ClSequencePickSentence(const char *name, int method, int *picked) { if (picked) *picked = 0; return NULL; }
static unsigned ClGetApproxWavePlayLen(const char *file) { return 0; }
static void *GetCareerGameUI(void) { return NULL; }
static int IsPlayingCareerMatch(void) { return 0; }
static double GetAbsoluteTime(void) { return Sys_FloatTime(); }
static void TutorStub(int *buffer, int len) {}
static void TutorReset(void) {}
static int GetAppID(void) { return 70; }
static void *GetAliases(void) { return NULL; }
static void GetMouseDelta(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; }

static void ApiStub(void)
{
	static qboolean warned;
	if (!warned)
		Con_DPrintf("client.dll used a part of the engine API that isn't written yet\n");
	warned = true;
}

static void InitEngineFuncs(void)
{
	cl_enginefunc_t *f = &cl_funcs;
	memset(f, 0, sizeof(*f));
	f->pfnSPR_Load = SPR_Load;
	f->pfnSPR_Frames = SPR_Frames;
	f->pfnSPR_Height = SPR_Height;
	f->pfnSPR_Width = SPR_Width;
	f->pfnSPR_Set = SPR_Set;
	f->pfnSPR_Draw = SPR_Draw;
	f->pfnSPR_DrawHoles = SPR_DrawHoles;
	f->pfnSPR_DrawAdditive = SPR_DrawAdditive;
	f->pfnSPR_EnableScissor = SPR_EnableScissor;
	f->pfnSPR_DisableScissor = SPR_DisableScissor;
	f->pfnSPR_GetList = SPR_GetList;
	f->pfnFillRGBA = FillRGBA;
	f->pfnGetScreenInfo = GetScreenInfo;
	f->pfnSetCrosshair = SetCrosshair;
	f->pfnRegisterVariable = RegisterVariable;
	f->pfnGetCvarFloat = GetCvarFloat;
	f->pfnGetCvarString = GetCvarString;
	f->pfnAddCommand = AddCommand;
	f->pfnHookUserMsg = HookUserMsg;
	f->pfnServerCmd = ServerCmd;
	f->pfnClientCmd = ClientCmd;
	f->pfnGetPlayerInfo = GetPlayerInfo;
	f->pfnPlaySoundByName = PlaySoundByName;
	f->pfnPlaySoundByIndex = PlaySoundByIndex;
	f->pfnAngleVectors = ClAngleVectors;
	f->pfnTextMessageGet = TextMessageGet;
	f->pfnDrawCharacter = DrawCharacter;
	f->pfnDrawConsoleString = DrawConsoleString;
	f->pfnDrawSetTextColor = DrawSetTextColor;
	f->pfnDrawConsoleStringLen = DrawConsoleStringLen;
	f->pfnConsolePrint = ConsolePrint;
	f->pfnCenterPrint = CenterPrint;
	f->GetWindowCenterX = GetWindowCenterX;
	f->GetWindowCenterY = GetWindowCenterY;
	f->GetViewAngles = GetViewAngles;
	f->SetViewAngles = SetViewAngles;
	f->GetMaxClients = GetMaxClients;
	f->Cvar_SetValue = ClCvar_SetValue;
	f->Cmd_Argc = ClCmd_Argc;
	f->Cmd_Argv = ClCmd_Argv;
	f->Con_Printf = ClCon_Printf;
	f->Con_DPrintf = ClCon_DPrintf;
	f->Con_NPrintf = ClCon_NPrintf;
	f->Con_NXPrintf = ClCon_NXPrintf;
	f->PhysInfo_ValueForKey = PhysInfo_ValueForKey;
	f->ServerInfo_ValueForKey = ServerInfo_ValueForKey;
	f->GetClientMaxspeed = GetClientMaxspeed;
	f->CheckParm = ClCheckParm;
	f->Key_Event = ClKey_Event;
	f->GetMousePosition = GetMousePosition;
	f->IsNoClipping = IsNoClipping;
	f->GetLocalPlayer = GetLocalPlayer;
	f->GetViewModel = GetViewModel;
	f->GetEntityByIndex = GetEntityByIndex;
	f->GetClientTime = GetClientTime;
	f->V_CalcShake = V_CalcShake;
	f->V_ApplyShake = V_ApplyShake;
	f->PM_PointContents = ClPM_PointContents;
	f->PM_WaterEntity = PM_WaterEntity;
	f->PM_TraceLine = ClPM_TraceLine;
	f->CL_LoadModel = CL_LoadModel;
	f->CL_CreateVisibleEntity = CL_CreateVisibleEntity;
	f->GetSpritePointer = GetSpritePointer;
	f->pfnPlaySoundByNameAtLocation = PlaySoundByNameAtLocation;
	f->pfnPrecacheEvent = ClPrecacheEvent;
	f->pfnPlaybackEvent = ClPlaybackEvent;
	f->pfnWeaponAnim = WeaponAnim;
	f->pfnRandomFloat = ClRandomFloat;
	f->pfnRandomLong = ClRandomLong;
	f->pfnHookEvent = CL_HookEvent;
	f->Con_IsVisible = ClCon_IsVisible;
	f->pfnGetGameDirectory = GetGameDirectory;
	f->pfnGetCvarPointer = GetCvarPointer;
	f->Key_LookupBinding = ClKey_LookupBinding;
	f->pfnGetLevelName = GetLevelName;
	f->pfnGetScreenFade = GetScreenFade;
	f->pfnSetScreenFade = SetScreenFade;
	f->VGui_GetPanel = ClVGui_GetPanel;
	f->VGui_ViewportPaintBackground = VGui_ViewportPaintBackground;
	f->COM_LoadFile = ClCOM_LoadFile;
	f->COM_ParseFile = ClCOM_ParseFile;
	f->COM_FreeFile = ClCOM_FreeFile;
	f->pTriAPI = &tri_api;
	f->pEfxAPI = &efx_api;
	f->pEventAPI = &event_api;
	f->pDemoAPI = &demo_api;
	f->pNetAPI = &net_api;
	f->pVoiceTweak = &voice_api;
	f->IsSpectateOnly = IsSpectateOnly;
	f->LoadMapSprite = LoadMapSprite;
	f->COM_AddAppDirectoryToSearchPath = COM_AddAppDirectoryToSearchPath;
	f->COM_ExpandFilename = COM_ExpandFilename;
	f->PlayerInfo_ValueForKey = PlayerInfo_ValueForKey;
	f->PlayerInfo_SetValueForKey = PlayerInfo_SetValueForKey;
	f->GetPlayerUniqueID = GetPlayerUniqueID;
	f->GetTrackerIDForPlayer = GetTrackerIDForPlayer;
	f->GetPlayerForTrackerID = GetPlayerForTrackerID;
	f->pfnServerCmdUnreliable = ServerCmd;
	f->pfnGetMousePos = ClGetMousePos;
	f->pfnSetMousePos = ClSetMousePos;
	f->pfnSetMouseEnable = ClSetMouseEnable;
	f->GetFirstCvarPtr = GetFirstCvarPtr;
	f->GetFirstCmdFunctionHandle = GetFirstCmdFunctionHandle;
	f->GetNextCmdFunctionHandle = GetNextCmdFunctionHandle;
	f->GetCmdFunctionName = GetCmdFunctionName;
	f->hudGetClientOldTime = GetClientOldTime;
	f->hudGetServerGravityValue = GetServerGravityValue;
	f->hudGetModelByIndex = GetModelByIndex;
	f->pfnSetFilterMode = SetFilterMode;
	f->pfnSetFilterColor = SetFilterColor;
	f->pfnSetFilterBrightness = SetFilterBrightness;
	f->pfnSequenceGet = ClSequenceGet;
	f->pfnSPR_DrawGeneric = SPR_DrawGeneric;
	f->pfnSequencePickSentence = ClSequencePickSentence;
	f->pfnDrawString = DrawString;
	f->pfnDrawStringReverse = DrawStringReverse;
	f->LocalPlayerInfo_ValueForKey = LocalPlayerInfo_ValueForKey;
	f->pfnVGUI2DrawCharacter = VGUI2DrawCharacter;
	f->pfnVGUI2DrawCharacterAdditive = VGUI2DrawCharacterAdditive;
	f->pfnGetApproxWavePlayLen = ClGetApproxWavePlayLen;
	f->GetCareerGameUI = GetCareerGameUI;
	f->Cvar_Set = ClCvar_Set;
	f->pfnIsPlayingCareerMatch = IsPlayingCareerMatch;
	f->pfnGetAbsoluteTime = GetAbsoluteTime;
	f->pfnProcessTutorMessageDecayBuffer = TutorStub;
	f->pfnConstructTutorMessageDecayBuffer = TutorStub;
	f->pfnResetTutorMessageDecayData = TutorReset;
	f->pfnFillRGBABlend = FillRGBABlend;
	f->pfnGetAppID = GetAppID;
	f->pfnGetAliases = GetAliases;
	f->pfnVguiWrap2_GetMouseDelta = GetMouseDelta;
	f->pfnFilteredClientCmd = ClientCmd;
	for (int i = 0; i < 32; i++)
		f->pfnReserved[i] = ApiStub;

	CL_InitTriAPI(&tri_api);
	CL_InitEfxAPI(&efx_api);
	CL_InitEventAPI(&event_api);
	// the rest: harmless stubs until each part is written
	for (int i = 0; i < 16; i++) {
		demo_api.fn[i] = (void *)ApiStub;
		voice_api.fn[i] = (void *)ApiStub;
	}
	for (int i = 0; i < 32; i++)
		net_api.fn[i] = (void *)ApiStub;
}

// ---------------------------------------------------------------- messages from the game DLL

typedef struct { int type, len; unsigned char *data; } clmsg_t;
static clmsg_t *msgqueue;
static int msgqueue_count, msgqueue_cap;

void CL_QueueMessage(int type, const unsigned char *data, int len)
{
	if (!client_ok)
		return;
	if (msgqueue_count == msgqueue_cap) {
		msgqueue_cap = msgqueue_cap ? msgqueue_cap * 2 : 64;
		msgqueue = Mem_Realloc(msgqueue, sizeof(clmsg_t) * msgqueue_cap);
	}
	clmsg_t *m = &msgqueue[msgqueue_count++];
	m->type = type;
	m->len = len;
	m->data = Mem_Alloc(len > 0 ? len : 1);
	if (len > 0)
		memcpy(m->data, data, len);
}

#define SVC_TEMPENTITY 23
#define SVC_WEAPONANIM 35
#define SVC_ROOMTYPE   37

pfnUserMsgHook CL_FindMessageHook(const char *name)
{
	for (int k = 0; name && k < msghooks_count; k++)
		if (!strcmp(msghooks[k].name, name))
			return msghooks[k].fn;
	return NULL;
}

static void DispatchMessages(void)
{
	// hooks can queue more messages; walk by index
	for (int i = 0; i < msgqueue_count; i++) {
		clmsg_t *m = &msgqueue[i];
		int type = m->type, len = m->len;
		unsigned char *data = m->data;
		if (type >= 64) {
			const char *name = SV_UserMsgName(type);
			pfnUserMsgHook fn = CL_FindMessageHook(name);
			// a few messages are the engine's own in GoldSrc (ScreenFade, ScreenShake)
			qboolean engine = !fn && name && CL_EngineMessage(name, data, len);
			if (developer->value > 1)
				Con_Printf("client message %s (%d bytes)%s\n", name ? name : "?", len, fn || engine ? "" : ", not hooked");
			if (fn)
				CLIENT_CALL(name, fn(name, len, data));
		} else if (type == SVC_WEAPONANIM && len >= 2) {
			CL_SetViewModelAnim(data[0], data[1]);
		} else if (type == SVC_ROOMTYPE && len >= 2) {
			Cvar_SetValue("room_type", (float)(short)(data[0] | (data[1] << 8)));
		} else if (type == SVC_TEMPENTITY) {
			CL_ParseTempEntity(data, len);
		}
		Mem_Free(msgqueue[i].data);
	}
	msgqueue_count = 0;
}

// ---------------------------------------------------------------- entities and the view

static void UpdateEntities(void)
{
	if (!sv.active)
		return;
	if (cl_entities_count < sv.max_edicts) {
		cl_entities = Mem_Realloc(cl_entities, sizeof(cl_entity_t) * sv.max_edicts);
		memset(cl_entities + cl_entities_count, 0, sizeof(cl_entity_t) * (sv.max_edicts - cl_entities_count));
		cl_entities_count = sv.max_edicts;
	}
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		cl_entity_t *c = &cl_entities[i];
		c->index = i;
		c->player = i >= 1 && i <= sv.maxclients;
		if (e->free) {
			c->model = NULL;
			c->curstate.modelindex = 0;
			continue;
		}
		c->prevstate = c->curstate;
		entity_state_t *s = &c->curstate;
		s->number = i;
		s->msg_time = (float)cl_time;
		VectorCopy(e->v.origin, s->origin);
		VectorCopy(e->v.angles, s->angles);
		s->modelindex = e->v.modelindex;
		s->sequence = e->v.sequence;
		s->frame = e->v.frame;
		s->colormap = e->v.colormap;
		s->skin = (short)e->v.skin;
		s->solid = (short)e->v.solid;
		s->effects = e->v.effects;
		s->scale = e->v.scale;
		s->rendermode = e->v.rendermode;
		s->renderamt = (int)e->v.renderamt;
		s->rendercolor.r = (unsigned char)e->v.rendercolor[0];
		s->rendercolor.g = (unsigned char)e->v.rendercolor[1];
		s->rendercolor.b = (unsigned char)e->v.rendercolor[2];
		s->renderfx = e->v.renderfx;
		s->movetype = e->v.movetype;
		s->animtime = e->v.animtime;
		s->framerate = e->v.framerate;
		s->body = e->v.body;
		memcpy(s->controller, e->v.controller, 4);
		memcpy(s->blending, e->v.blending, 2);
		VectorCopy(e->v.velocity, s->velocity);
		VectorCopy(e->v.mins, s->mins);
		VectorCopy(e->v.maxs, s->maxs);
		s->aiment = e->v.aiment ? NUM_FOR_EDICT(e->v.aiment) : 0;
		s->owner = e->v.owner ? NUM_FOR_EDICT(e->v.owner) : 0;
		s->friction = e->v.friction;
		s->gravity = e->v.gravity;
		s->team = e->v.team;
		s->playerclass = e->v.playerclass;
		s->health = (int)e->v.health;
		s->weaponmodel = e->v.weaponmodel;
		s->gaitsequence = e->v.gaitsequence;
		VectorCopy(e->v.basevelocity, s->basevelocity);
		s->usehull = (e->v.flags & FL_DUCKING) ? 1 : 0;
		s->oldbuttons = e->v.oldbuttons;
		s->onground = (e->v.flags & FL_ONGROUND) ? 0 : -1;
		s->iStepLeft = e->v.iStepLeft;
		s->flFallVelocity = e->v.flFallVelocity;
		s->fov = e->v.fov;
		s->weaponanim = e->v.weaponanim;
		VectorCopy(e->v.origin, c->origin);
		VectorCopy(e->v.angles, c->angles);
		c->model = SV_ModelForIndex(e->v.modelindex);
	}

	// the view model follows the player's weapon
	edict_t *pl = SV_LocalPlayer();
	model_t *vm = pl ? Mod_ForName(SV_String(pl->v.viewmodel), false) : NULL;
	if (pl && !pl->v.viewmodel)
		vm = NULL;
	if (vm != cl_viewent.model) {
		cl_viewent.model = vm;
		cl_viewent.curstate.modelindex = vm ? SV_ModelIndex(vm->name) : 0;
	}
}

// V_CalcRefdef places the view (and the view model) with bob, punch and smoothing
qboolean CL_CalcRefdef(vec3_t vieworg, vec3_t viewangles)
{
	edict_t *pl = SV_LocalPlayer();
	if (!client_ok || !pl || !cl.V_CalcRefdef)
		return false;
	static ref_params_t rp;
	memset(&rp, 0, sizeof(rp));
	VectorAdd(pl->v.origin, pl->v.view_ofs, rp.vieworg);
	VectorCopy(cl_viewangles, rp.viewangles);
	AngleVectors(cl_viewangles, rp.forward, rp.right, rp.up);
	rp.frametime = (float)host_frametime;
	rp.time = (float)cl_time;
	rp.onground = (pl->v.flags & FL_ONGROUND) != 0;
	rp.waterlevel = pl->v.waterlevel;
	VectorCopy(pl->v.velocity, rp.simvel);
	VectorCopy(pl->v.origin, rp.simorg);
	VectorCopy(pl->v.view_ofs, rp.viewheight);
	rp.idealpitch = pl->v.idealpitch;
	VectorCopy(cl_viewangles, rp.cl_viewangles);
	rp.health = (int)pl->v.health;
	rp.viewsize = 120;
	VectorCopy(pl->v.punchangle, rp.punchangle);
	rp.maxclients = sv.maxclients;
	rp.viewentity = 1;
	rp.playernum = 0;
	rp.max_entities = sv.max_edicts;
	rp.hardware = 1;
	rp.cmd = &last_cmd;
	rp.movevars = SV_MoveVars();
	rp.viewport[2] = vid_width;
	rp.viewport[3] = vid_height;
	qboolean ok = false;
	CLIENT_CALL("V_CalcRefdef", { cl.V_CalcRefdef(&rp); ok = true; });
	if (!ok)
		return false;
	VectorCopy(rp.vieworg, vieworg);
	VectorCopy(rp.viewangles, viewangles);
	return true;
}

// the weapon in your hands, as the renderer draws it
qboolean CL_BuildViewModel(rentity_t *r)
{
	edict_t *pl = SV_LocalPlayer();
	if (!client_ok || !pl || !cl_viewent.model || pl->v.health <= 0 || cl_viewent.model->type != mod_studio)
		return false;
	memset(r, 0, sizeof(*r));
	r->model = cl_viewent.model;
	VectorCopy(cl_viewent.origin, r->origin);
	VectorCopy(cl_viewent.angles, r->angles);   // the client's V_CalcRefdef already set the studio pitch
	r->sequence = cl_viewent.curstate.sequence;
	r->frame = 0;
	r->framerate = cl_viewent.curstate.framerate ? cl_viewent.curstate.framerate : 1;
	r->animtime = r_refdef.time - (cl_time - cl_viewent.curstate.animtime);
	r->body = cl_viewent.curstate.body;
	r->skin = cl_viewent.curstate.skin;
	r->renderamt = 255;
	R_StudioInitControllers(r->model, r);
	return true;
}

// drawn last and squashed into the near depth range so it never pokes into walls
void CL_DrawViewModel(void)
{
	rentity_t r;
	if (!CL_BuildViewModel(&r) || Cvar_VariableValue("r_drawviewmodel") == 0)
		return;
	glDepthRange(0, 0.3);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	R_StudioDrawModel(&r);
	glDisable(GL_CULL_FACE);
	glDepthRange(0, 1);
}

// ---------------------------------------------------------------- per frame

void CL_CreateCmd(float frametime, usercmd_t *cmd)
{
	if (!client_ok || !cl.CL_CreateMove) {
		CL_CreateCmdEngine(frametime, cmd);
		last_cmd = *cmd;
		return;
	}
	memset(cmd, 0, sizeof(*cmd));
	int active = !Con_IsVisible();
	CLIENT_CALL("CL_CreateMove", cl.CL_CreateMove(frametime, cmd, active));
	// the client clears the command; the frame time is the engine's to fill in
	int msec = (int)(frametime * 1000.0f + 0.5f);
	cmd->msec = (unsigned char)(msec < 1 ? 1 : msec > 255 ? 255 : msec);
	last_cmd = *cmd;
}

void CL_Frame(double frametime)
{
	if (!client_ok)
		return;
	cl_oldtime = cl_time;
	cl_time = sv.active ? sv.time : host_realtime;

	// mouse look belongs to the client while playing
	static int mouse_on = -1;
	int want = sv.active && !Con_IsVisible() && Vid_HasFocus();   // let go of the mouse when alt-tabbed
	if (want != mouse_on) {
		if (want)
			CLIENT_CALL("IN_ActivateMouse", cl.IN_ActivateMouse());
		else
			CLIENT_CALL("IN_DeactivateMouse", cl.IN_DeactivateMouse());
		mouse_on = want;
	}
	UpdateEntities();
	DispatchMessages();
	CL_FireEvents();
	CLIENT_CALL("HUD_Frame", cl.HUD_Frame(cl_time));
}

void CL_DrawHUD(void)
{
	if (!client_ok)
		return;
	if (sv.active && map_ready)
		CL_DrawScreenFade();
	if (sv.active && map_ready && cl_drawhud->value) {
		edict_t *pl = SV_LocalPlayer();
		if (pl) {
			client_data_t cd;
			memset(&cd, 0, sizeof(cd));
			VectorCopy(pl->v.origin, cd.origin);
			VectorCopy(cl_viewangles, cd.viewangles);
			cd.iWeaponBits = pl->v.weapons;
			cd.fov = pl->v.fov ? pl->v.fov : Cvar_VariableValue("default_fov");
			int changed = 0;
			CLIENT_CALL("HUD_UpdateClientData", changed = cl.HUD_UpdateClientData(&cd, (float)cl_time));
			if (changed)
				VectorCopy(cd.viewangles, cl_viewangles);
		}
		CLIENT_CALL("HUD_Redraw", cl.HUD_Redraw((float)cl_time, 0));
		if (scissor_on)
			SPR_DisableScissor();

		// the crosshair the client asked for
		if (crosshair_sprite && crosshair->value) {
			model_t *m = SpriteForHandle(crosshair_sprite);
			int w = crosshair_rect.right - crosshair_rect.left, h = crosshair_rect.bottom - crosshair_rect.top;
			if (m)
				DrawSpriteFrame(m, 0, (vid_width - w) / 2, (vid_height - h) / 2, &crosshair_rect, SPRMODE_ADDITIVE, crosshair_color, 0, 0);
		}
	}
	if (center_text[0] && host_realtime - center_time < 3.0) {
		int w = Draw_StringWidth(center_text);
		Draw_String((float)((vid_width - w) / 2), vid_height * 0.35f, center_text, 255, 255, 255, 255);
	}
	__try {
		VGUI_Paint();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con_Errorf("vgui.dll crashed while painting (exception 0x%08lx)\n", GetExceptionCode());
		client_ok = false;
	}
	glDisable(GL_SCISSOR_TEST);
	glColor4f(1, 1, 1, 1);
}

// a key press goes to the client first (weapon menus); false means it ate it
qboolean CL_KeyEvent(int key, qboolean down, const char *binding)
{
	if (!client_ok || !cl.HUD_Key_Event)
		return true;
	int pass = 1;
	CLIENT_CALL("HUD_Key_Event", pass = cl.HUD_Key_Event(down, key, binding));
	return pass != 0;
}

void CL_MapStarted(void)
{
	if (!client_ok)
		return;
	memset(&cl_viewent, 0, sizeof(cl_viewent));
	// the client's pm_shared needs its texture types before any event looks one up (GoldSrc does
	// this once at startup; here the playermove structure exists once the game DLL is loaded)
	static qboolean pm_ready;
	if (!pm_ready && cl.HUD_PlayerMoveInit && SV_PlayerMove()) {
		CLIENT_CALL("HUD_PlayerMoveInit", cl.HUD_PlayerMoveInit(SV_PlayerMove()));
		pm_ready = true;
	}
	CLIENT_CALL("HUD_VidInit", cl.HUD_VidInit());
	CLIENT_CALL("HUD_Reset", cl.HUD_Reset());
	map_ready = client_ok;
}

void CL_MapEnded(void)
{
	map_ready = false;
	CL_ClearSprites();
	CL_ClearTempEnts();
	CL_ClearEvents();
	CL_ClearView();
	S_StopAllSounds();
	R_ClearParticles();
	R_ClearBeams();
	for (int i = 0; i < msgqueue_count; i++)
		Mem_Free(msgqueue[i].data);
	msgqueue_count = 0;
}

// ---------------------------------------------------------------- startup

static void *Export(const char *name)
{
	return (void *)GetProcAddress(client_dll, name);
}

// Steam features the client asks for; nothing to do here
static void NoOp_f(void) {}

void CL_Init(void)
{
	Cmd_AddCommand("richpresence_gamemode", NoOp_f, "Steam rich presence (ignored)");
	Cmd_AddCommand("richpresence_update", NoOp_f, "Steam rich presence (ignored)");
	Cvar_Get("sv_allow_autoaim", "1", FCVAR_SERVER, NULL);
	Cvar_Get("sv_allow_shaders", "1", FCVAR_SERVER, NULL);
	// the 25th anniversary client reads raw mouse movement from SDL when this is on (an engine cvar there)
	Cvar_Get("m_rawinput", "1", FCVAR_ARCHIVE, "1 = the client reads raw mouse movement (SDL relative mode)");
	cl_titlefont = Cvar_Get("cl_titlefont", "Trebuchet MS", FCVAR_ARCHIVE, "font face for titles and credits (applies at the next map)");
	// client-side weapon prediction: not supported yet, so off (the server's weapon effects are shown)
	Cvar_Get("cl_lw", "0", FCVAR_READONLY, "client-side weapon prediction (not supported yet)");
	crosshair = Cvar_Get("crosshair", "1", FCVAR_ARCHIVE, "draws the crosshair");
	cl_drawhud = Cvar_Get("hud_draw", "1", FCVAR_ARCHIVE, "draws the client DLL's HUD");
	Cvar_Get("r_drawviewmodel", "1", FCVAR_ARCHIVE, "draws the weapon in your hands");
	Cvar_Get("default_fov", "90", FCVAR_ARCHIVE, "field of view the HUD asks for");
	CL_InitView();
	CL_InitTempEnts();
	R_InitParticles();
	R_InitBeams();

	char path[MAX_OSPATH];
	if (COM_CheckParm("-noclientdll") || COM_CheckParm("-nogamedll") || !FS_FullPath("cl_dlls/client.dll", path, sizeof(path))) {
		CL_InitInput();
		return;
	}
	if (!VGUI_Init()) {
		CL_InitInput();
		return;
	}
	client_dll = LoadLibraryA(path);
	if (!client_dll) {
		Con_Errorf("Couldn't load %s (error %lu)\n", path, GetLastError());
		CL_InitInput();
		return;
	}
	cl.Initialize = Export("Initialize");
	cl.HUD_Init = Export("HUD_Init");
	cl.HUD_VidInit = Export("HUD_VidInit");
	cl.HUD_Redraw = Export("HUD_Redraw");
	cl.HUD_UpdateClientData = Export("HUD_UpdateClientData");
	cl.HUD_Reset = Export("HUD_Reset");
	cl.HUD_PlayerMove = Export("HUD_PlayerMove");
	cl.HUD_PlayerMoveInit = Export("HUD_PlayerMoveInit");
	cl.HUD_PlayerMoveTexture = Export("HUD_PlayerMoveTexture");
	cl.IN_ActivateMouse = Export("IN_ActivateMouse");
	cl.IN_DeactivateMouse = Export("IN_DeactivateMouse");
	cl.IN_MouseEvent = Export("IN_MouseEvent");
	cl.IN_ClearStates = Export("IN_ClearStates");
	cl.IN_Accumulate = Export("IN_Accumulate");
	cl.CL_CreateMove = Export("CL_CreateMove");
	cl.CL_IsThirdPerson = Export("CL_IsThirdPerson");
	cl.CL_CameraOffset = Export("CL_CameraOffset");
	cl.KB_Find = Export("KB_Find");
	cl.CAM_Think = Export("CAM_Think");
	cl.V_CalcRefdef = Export("V_CalcRefdef");
	cl.HUD_AddEntity = Export("HUD_AddEntity");
	cl.HUD_CreateEntities = Export("HUD_CreateEntities");
	cl.HUD_DrawNormalTriangles = Export("HUD_DrawNormalTriangles");
	cl.HUD_DrawTransparentTriangles = Export("HUD_DrawTransparentTriangles");
	cl.HUD_StudioEvent = Export("HUD_StudioEvent");
	cl.HUD_Shutdown = Export("HUD_Shutdown");
	cl.HUD_Frame = Export("HUD_Frame");
	cl.HUD_Key_Event = Export("HUD_Key_Event");
	cl.HUD_TempEntUpdate = Export("HUD_TempEntUpdate");
	cl.HUD_GetUserEntity = Export("HUD_GetUserEntity");
	cl.HUD_GetStudioModelInterface = Export("HUD_GetStudioModelInterface");
	cl.HUD_DirectorMessage = Export("HUD_DirectorMessage");
	if (!cl.Initialize || !cl.HUD_Init || !cl.HUD_VidInit || !cl.HUD_Redraw || !cl.HUD_Frame || !cl.CL_CreateMove) {
		Con_Errorf("%s isn't a GoldSrc client DLL\n", path);
		CL_InitInput();
		return;
	}

	InitEngineFuncs();
	client_ok = true;
	int ok = 0;
	CLIENT_CALL("Initialize", ok = cl.Initialize(&cl_funcs, CLDLL_INTERFACE_VERSION));
	if (!ok) {
		Con_Errorf("client.dll refused interface version %d\n", CLDLL_INTERFACE_VERSION);
		client_ok = false;
	}
	CLIENT_CALL("HUD_Init", cl.HUD_Init());
	if (!client_ok) {
		CL_InitInput();
		return;
	}
	CL_LoadTitles();
	CL_InitStudioAPI();
	Con_Printf("Client DLL: cl_dlls/client.dll\n");
}

void CL_Shutdown(void)
{
	CLIENT_CALL("HUD_Shutdown", if (cl.HUD_Shutdown) cl.HUD_Shutdown());
}
