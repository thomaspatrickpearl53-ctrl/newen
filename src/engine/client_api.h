// newen - the interface between the engine and a GoldSrc client DLL (client.dll)
//
// Written from the publicly documented layout of the Half-Life SDK's client interface. As with the
// game DLL, these structures are shared memory: order and sizes must match exactly (see the asserts).
#pragma once
#include "game_api.h"

typedef int HSPRITE_t;

typedef struct { int left, right, top, bottom; } wrect_t;

#define SCRINFO_SCREENFLASH 1
#define SCRINFO_STRETCHED   2

typedef struct {
	int   iSize;
	int   iWidth, iHeight;
	int   iFlags;
	int   iCharHeight;
	short charWidths[256];
} SCREENINFO;

typedef struct {
	char    szName[64];
	char    szSprite[64];
	int     hspr;
	int     iRes;
	wrect_t rc;
} client_sprite_t;

typedef struct {
	char          *name;
	short          ping;
	unsigned char  thisplayer;
	unsigned char  spectator;
	unsigned char  packetloss;
	char          *model;
	short          topcolor, bottomcolor;
	unsigned long long m_nSteamID;
} hud_player_info_t;

typedef struct {
	int   effect;
	unsigned char r1, g1, b1, a1;
	unsigned char r2, g2, b2, a2;
	float x, y;
	float fadein, fadeout, holdtime, fxtime;
	const char *pName;
	const char *pMessage;
} client_textmessage_t;

typedef struct con_nprint_s {
	int   index;
	float time_to_live;
	float color[3];
} con_nprint_t;

typedef struct screenfade_s {
	float fadeSpeed, fadeEnd, fadeTotalEnd, fadeReset;
	unsigned char fader, fadeg, fadeb, fadealpha;
	int   fadeFlags;
} screenfade_t;

// ---------------------------------------------------------------- entities as the client sees them

typedef struct { unsigned char r, g, b; } color24;
typedef struct { unsigned r, g, b, a; } colorVec;

typedef struct entity_state_s {
	int      entityType;
	int      number;
	float    msg_time;
	int      messagenum;
	vec3f_t  origin, angles;
	int      modelindex;
	int      sequence;
	float    frame;
	int      colormap;
	short    skin, solid;
	int      effects;
	float    scale;
	unsigned char eflags;
	int      rendermode;
	int      renderamt;
	color24  rendercolor;
	int      renderfx;
	int      movetype;
	float    animtime, framerate;
	int      body;
	unsigned char controller[4], blending[4];
	vec3f_t  velocity;
	vec3f_t  mins, maxs;
	int      aiment, owner;
	float    friction, gravity;
	int      team, playerclass, health;
	qboolean spectator;
	int      weaponmodel;
	int      gaitsequence;
	vec3f_t  basevelocity;
	int      usehull, oldbuttons, onground, iStepLeft;
	float    flFallVelocity;
	float    fov;
	int      weaponanim;
	vec3f_t  startpos, endpos;
	float    impacttime, starttime;
	int      iuser1, iuser2, iuser3, iuser4;
	float    fuser1, fuser2, fuser3, fuser4;
	vec3f_t  vuser1, vuser2, vuser3, vuser4;
} entity_state_t;

#define HISTORY_MAX 64

typedef struct {
	float   animtime;
	vec3f_t origin, angles;
} position_history_t;

typedef struct {
	unsigned char mouthopen, sndcount;
	int sndavg;
} mouth_t;

typedef struct {
	float   prevanimtime, sequencetime;
	unsigned char prevseqblending[2];
	vec3f_t prevorigin, prevangles;
	int     prevsequence;
	float   prevframe;
	unsigned char prevcontroller[4], prevblending[2];
} latchedvars_t;

typedef struct cl_entity_s {
	int            index;
	qboolean       player;
	entity_state_t baseline, prevstate, curstate;
	int            current_position;
	position_history_t ph[HISTORY_MAX];
	mouth_t        mouth;
	latchedvars_t  latched;
	float          lastmove;
	vec3f_t        origin, angles;
	vec3f_t        attachment[4];
	int            trivial_accept;
	struct model_s *model;
	void          *efrag;
	void          *topnode;
	float          syncbase;
	int            visframe;
	colorVec       cvFloorColor;
} cl_entity_t;

typedef struct {
	vec3f_t origin;
	vec3f_t viewangles;
	int     iWeaponBits;
	float   fov;
} client_data_t;

typedef struct ref_params_s {
	vec3f_t vieworg, viewangles;
	vec3f_t forward, right, up;
	float   frametime, time;
	int     intermission, paused, spectator, onground, waterlevel;
	vec3f_t simvel, simorg, viewheight;
	float   idealpitch;
	vec3f_t cl_viewangles;
	int     health;
	vec3f_t crosshairangle;
	float   viewsize;
	vec3f_t punchangle;
	int     maxclients, viewentity, playernum, max_entities, demoplayback, hardware, smoothing;
	usercmd_t  *cmd;
	movevars_t *movevars;
	int     viewport[4];
	int     nextView;
	int     onlyClientDraw;
} ref_params_t;

typedef struct event_args_s {
	int     flags;
	int     entindex;
	vec3f_t origin, angles, velocity;
	int     ducking;
	float   fparam1, fparam2;
	int     iparam1, iparam2;
	int     bparam1, bparam2;
} event_args_t;

typedef int (*pfnUserMsgHook)(const char *pszName, int iSize, void *pbuf);
typedef void (*pfnEventHook)(event_args_t *args);

// the extra API tables; most entries are stubs for now, so they're generous arrays of pointers
typedef struct { int version; void *fn[48]; } triangleapi_t;
typedef struct { void *fn[128]; } efx_api_t;
typedef struct { int version; void *fn[48]; } event_api_t;
typedef struct { void *fn[16]; } demo_api_t;
typedef struct { void *fn[32]; } net_api_t;
typedef struct { void *fn[16]; } voicetweak_t;

// ---------------------------------------------------------------- engine functions (engine -> client)

typedef struct {
	HSPRITE_t (*pfnSPR_Load)(const char *szPicName);
	int       (*pfnSPR_Frames)(HSPRITE_t hPic);
	int       (*pfnSPR_Height)(HSPRITE_t hPic, int frame);
	int       (*pfnSPR_Width)(HSPRITE_t hPic, int frame);
	void      (*pfnSPR_Set)(HSPRITE_t hPic, int r, int g, int b);
	void      (*pfnSPR_Draw)(int frame, int x, int y, const wrect_t *prc);
	void      (*pfnSPR_DrawHoles)(int frame, int x, int y, const wrect_t *prc);
	void      (*pfnSPR_DrawAdditive)(int frame, int x, int y, const wrect_t *prc);
	void      (*pfnSPR_EnableScissor)(int x, int y, int width, int height);
	void      (*pfnSPR_DisableScissor)(void);
	client_sprite_t *(*pfnSPR_GetList)(const char *psz, int *piCount);
	void      (*pfnFillRGBA)(int x, int y, int width, int height, int r, int g, int b, int a);
	int       (*pfnGetScreenInfo)(SCREENINFO *pscrinfo);
	void      (*pfnSetCrosshair)(HSPRITE_t hspr, wrect_t rc, int r, int g, int b);
	cvar_t   *(*pfnRegisterVariable)(const char *szName, const char *szValue, int flags);
	float     (*pfnGetCvarFloat)(const char *szName);
	char     *(*pfnGetCvarString)(const char *szName);
	int       (*pfnAddCommand)(const char *cmd_name, void (*function)(void));
	int       (*pfnHookUserMsg)(const char *szMsgName, pfnUserMsgHook pfn);
	int       (*pfnServerCmd)(const char *szCmdString);
	int       (*pfnClientCmd)(const char *szCmdString);
	void      (*pfnGetPlayerInfo)(int ent_num, hud_player_info_t *pinfo);
	void      (*pfnPlaySoundByName)(const char *szSound, float volume);
	void      (*pfnPlaySoundByIndex)(int iSound, float volume);
	void      (*pfnAngleVectors)(const float *vecAngles, float *forward, float *right, float *up);
	client_textmessage_t *(*pfnTextMessageGet)(const char *pName);
	int       (*pfnDrawCharacter)(int x, int y, int number, int r, int g, int b);
	int       (*pfnDrawConsoleString)(int x, int y, const char *string);
	void      (*pfnDrawSetTextColor)(float r, float g, float b);
	void      (*pfnDrawConsoleStringLen)(const char *string, int *length, int *height);
	void      (*pfnConsolePrint)(const char *string);
	void      (*pfnCenterPrint)(const char *string);
	int       (*GetWindowCenterX)(void);
	int       (*GetWindowCenterY)(void);
	void      (*GetViewAngles)(float *);
	void      (*SetViewAngles)(float *);
	int       (*GetMaxClients)(void);
	void      (*Cvar_SetValue)(const char *cvar, float value);
	int       (*Cmd_Argc)(void);
	const char *(*Cmd_Argv)(int arg);
	void      (*Con_Printf)(const char *fmt, ...);
	void      (*Con_DPrintf)(const char *fmt, ...);
	void      (*Con_NPrintf)(int pos, const char *fmt, ...);
	void      (*Con_NXPrintf)(con_nprint_t *info, const char *fmt, ...);
	const char *(*PhysInfo_ValueForKey)(const char *key);
	const char *(*ServerInfo_ValueForKey)(const char *key);
	float     (*GetClientMaxspeed)(void);
	int       (*CheckParm)(const char *parm, char **ppnext);
	void      (*Key_Event)(int key, int down);
	void      (*GetMousePosition)(int *mx, int *my);
	int       (*IsNoClipping)(void);
	cl_entity_t *(*GetLocalPlayer)(void);
	cl_entity_t *(*GetViewModel)(void);
	cl_entity_t *(*GetEntityByIndex)(int idx);
	float     (*GetClientTime)(void);
	void      (*V_CalcShake)(void);
	void      (*V_ApplyShake)(float *origin, float *angles, float factor);
	int       (*PM_PointContents)(float *point, int *truecontents);
	int       (*PM_WaterEntity)(float *p);
	pmtrace_t *(*PM_TraceLine)(float *start, float *end, int flags, int usehull, int ignore_pe);
	struct model_s *(*CL_LoadModel)(const char *modelname, int *index);
	int       (*CL_CreateVisibleEntity)(int type, cl_entity_t *ent);
	const struct model_s *(*GetSpritePointer)(HSPRITE_t hSprite);
	void      (*pfnPlaySoundByNameAtLocation)(const char *szSound, float volume, float *origin);
	unsigned short (*pfnPrecacheEvent)(int type, const char *psz);
	void      (*pfnPlaybackEvent)(int flags, const edict_t *pInvoker, unsigned short eventindex, float delay, float *origin,
	                              float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2);
	void      (*pfnWeaponAnim)(int iAnim, int body);
	float     (*pfnRandomFloat)(float flLow, float flHigh);
	long      (*pfnRandomLong)(long lLow, long lHigh);
	void      (*pfnHookEvent)(const char *name, pfnEventHook pfnEvent);
	int       (*Con_IsVisible)(void);
	const char *(*pfnGetGameDirectory)(void);
	cvar_t   *(*pfnGetCvarPointer)(const char *szName);
	const char *(*Key_LookupBinding)(const char *pBinding);
	const char *(*pfnGetLevelName)(void);
	void      (*pfnGetScreenFade)(screenfade_t *fade);
	void      (*pfnSetScreenFade)(screenfade_t *fade);
	void     *(*VGui_GetPanel)(void);
	void      (*VGui_ViewportPaintBackground)(int extents[4]);
	unsigned char *(*COM_LoadFile)(const char *path, int usehunk, int *pLength);
	char     *(*COM_ParseFile)(char *data, char *token);
	void      (*COM_FreeFile)(void *buffer);
	triangleapi_t *pTriAPI;
	efx_api_t     *pEfxAPI;
	event_api_t   *pEventAPI;
	demo_api_t    *pDemoAPI;
	net_api_t     *pNetAPI;
	voicetweak_t  *pVoiceTweak;
	int       (*IsSpectateOnly)(void);
	struct model_s *(*LoadMapSprite)(const char *filename);
	void      (*COM_AddAppDirectoryToSearchPath)(const char *pszBaseDir, const char *appName);
	int       (*COM_ExpandFilename)(const char *fileName, char *nameOutBuffer, int nameOutBufferSize);
	const char *(*PlayerInfo_ValueForKey)(int playerNum, const char *key);
	void      (*PlayerInfo_SetValueForKey)(const char *key, const char *value);
	qboolean  (*GetPlayerUniqueID)(int iPlayer, char playerID[16]);
	int       (*GetTrackerIDForPlayer)(int playerSlot);
	int       (*GetPlayerForTrackerID)(int trackerID);
	int       (*pfnServerCmdUnreliable)(const char *szCmdString);
	void      (*pfnGetMousePos)(void *ppt);
	void      (*pfnSetMousePos)(int x, int y);
	void      (*pfnSetMouseEnable)(qboolean fEnable);
	// later additions (Steam builds and the 25th anniversary update)
	cvar_t   *(*GetFirstCvarPtr)(void);
	unsigned  (*GetFirstCmdFunctionHandle)(void);
	unsigned  (*GetNextCmdFunctionHandle)(unsigned cmdhandle);
	const char *(*GetCmdFunctionName)(unsigned cmdhandle);
	float     (*hudGetClientOldTime)(void);
	float     (*hudGetServerGravityValue)(void);
	struct model_s *(*hudGetModelByIndex)(int index);
	void      (*pfnSetFilterMode)(int mode);
	void      (*pfnSetFilterColor)(float r, float g, float b);
	void      (*pfnSetFilterBrightness)(float brightness);
	void     *(*pfnSequenceGet)(const char *fileName, const char *entryName);
	void      (*pfnSPR_DrawGeneric)(int frame, int x, int y, const wrect_t *prc, int src, int dest, int w, int h);
	void     *(*pfnSequencePickSentence)(const char *sentenceName, int pickMethod, int *entryPicked);
	int       (*pfnDrawString)(int x, int y, const char *str, int r, int g, int b);
	int       (*pfnDrawStringReverse)(int x, int y, const char *str, int r, int g, int b);
	const char *(*LocalPlayerInfo_ValueForKey)(const char *key);
	int       (*pfnVGUI2DrawCharacter)(int x, int y, int ch, unsigned font);
	int       (*pfnVGUI2DrawCharacterAdditive)(int x, int y, int ch, int r, int g, int b, unsigned font);
	unsigned  (*pfnGetApproxWavePlayLen)(const char *filename);
	void     *(*GetCareerGameUI)(void);
	void      (*Cvar_Set)(const char *cvar, const char *value);
	int       (*pfnIsPlayingCareerMatch)(void);
	double    (*pfnGetAbsoluteTime)(void);
	void      (*pfnProcessTutorMessageDecayBuffer)(int *buffer, int bufferLength);
	void      (*pfnConstructTutorMessageDecayBuffer)(int *buffer, int bufferLength);
	void      (*pfnResetTutorMessageDecayData)(void);
	void      (*pfnFillRGBABlend)(int x, int y, int width, int height, int r, int g, int b, int a);
	int       (*pfnGetAppID)(void);
	void     *(*pfnGetAliases)(void);
	void      (*pfnVguiWrap2_GetMouseDelta)(int *x, int *y);
	int       (*pfnFilteredClientCmd)(const char *cmd);
	void      (*pfnReserved[32])(void);
} cl_enginefunc_t;

#define CLDLL_INTERFACE_VERSION 7

// ---------------------------------------------------------------- client exports (client -> engine)

typedef struct {
	int   (*Initialize)(cl_enginefunc_t *pEnginefuncs, int iVersion);
	void  (*HUD_Init)(void);
	int   (*HUD_VidInit)(void);
	int   (*HUD_Redraw)(float time, int intermission);
	int   (*HUD_UpdateClientData)(client_data_t *pcldata, float flTime);
	void  (*HUD_Reset)(void);
	void  (*HUD_PlayerMove)(playermove_t *ppmove, int server);
	void  (*HUD_PlayerMoveInit)(playermove_t *ppmove);
	char  (*HUD_PlayerMoveTexture)(char *name);
	void  (*IN_ActivateMouse)(void);
	void  (*IN_DeactivateMouse)(void);
	void  (*IN_MouseEvent)(int mstate);
	void  (*IN_ClearStates)(void);
	void  (*IN_Accumulate)(void);
	void  (*CL_CreateMove)(float frametime, usercmd_t *cmd, int active);
	int   (*CL_IsThirdPerson)(void);
	void  (*CL_CameraOffset)(float *ofs);
	void *(*KB_Find)(const char *name);
	void  (*CAM_Think)(void);
	void  (*V_CalcRefdef)(ref_params_t *pparams);
	int   (*HUD_AddEntity)(int type, cl_entity_t *ent, const char *modelname);
	void  (*HUD_CreateEntities)(void);
	void  (*HUD_DrawNormalTriangles)(void);
	void  (*HUD_DrawTransparentTriangles)(void);
	void  (*HUD_StudioEvent)(const void *event, const cl_entity_t *entity);
	void  (*HUD_Shutdown)(void);
	void  (*HUD_Frame)(double time);
	int   (*HUD_Key_Event)(int down, int keynum, const char *pszCurrentBinding);
	void  (*HUD_TempEntUpdate)(double frametime, double client_time, double cl_gravity, void **ppTempEntFree,
	                           void **ppTempEntActive, int (*Callback_AddVisibleEntity)(cl_entity_t *pEntity),
	                           void (*Callback_TempEntPlaySound)(void *pTemp, float damp));
	cl_entity_t *(*HUD_GetUserEntity)(int index);
	int   (*HUD_GetStudioModelInterface)(int version, void **ppinterface, void *pstudio);
	void  (*HUD_DirectorMessage)(int iSize, void *pbuf);
} cldll_exports_t;

_Static_assert(sizeof(SCREENINFO) == 532, "SCREENINFO layout");
_Static_assert(sizeof(entity_state_t) == 340, "entity_state_t layout");
_Static_assert(sizeof(cl_entity_t) == 3000, "cl_entity_t layout");
_Static_assert(sizeof(client_sprite_t) == 152, "client_sprite_t layout");
