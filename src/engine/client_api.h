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

// event flags (PlaybackEvent)
#define FEV_NOTHOST   (1<<0)
#define FEV_RELIABLE  (1<<1)
#define FEV_GLOBAL    (1<<2)
#define FEV_UPDATE    (1<<3)
#define FEV_HOSTONLY  (1<<4)
#define FEV_SERVER    (1<<5)
#define FEV_CLIENT    (1<<6)

// ---------------------------------------------------------------- client-side effects

// temp entity flags
#define FTENT_SINEWAVE       0x00000001
#define FTENT_GRAVITY        0x00000002
#define FTENT_ROTATE         0x00000004
#define FTENT_SLOWGRAVITY    0x00000008
#define FTENT_SMOKETRAIL     0x00000010
#define FTENT_COLLIDEWORLD   0x00000020
#define FTENT_FLICKER        0x00000040
#define FTENT_FADEOUT        0x00000080
#define FTENT_SPRANIMATE     0x00000100
#define FTENT_HITSOUND       0x00000200
#define FTENT_SPIRAL         0x00000400
#define FTENT_SPRCYCLE       0x00000800
#define FTENT_COLLIDEALL     0x00001000
#define FTENT_PERSIST        0x00002000
#define FTENT_COLLIDEKILL    0x00004000
#define FTENT_PLYRATTACHMENT 0x00008000
#define FTENT_SPRANIMATELOOP 0x00010000
#define FTENT_SPARKSHOWER    0x00020000
#define FTENT_NOMODEL        0x00040000
#define FTENT_CLIENTCUSTOM   0x00080000
#define FTENT_SCALE          0x00100000

typedef struct tempent_s {
	int      flags;
	float    die;
	float    frameMax;
	float    x, y, z;
	float    fadeSpeed;
	float    bounceFactor;
	int      hitSound;
	void   (*hitcallback)(struct tempent_s *ent, struct pmtrace_s *ptr);
	void   (*callback)(struct tempent_s *ent, float frametime, float currenttime);
	struct tempent_s *next;
	int      priority;
	short    clientIndex;
	vec3f_t  tentOffset;
	cl_entity_t entity;
} TEMPENTITY;

#define TENTPRIORITY_LOW  0
#define TENTPRIORITY_HIGH 1

typedef enum { pt_static, pt_grav, pt_slowgrav, pt_fire, pt_explode, pt_explode2, pt_blob, pt_blob2,
	pt_vox_slowgrav, pt_vox_grav, pt_clientcustom } ptype_t;

typedef struct particle_s {
	vec3f_t  org;
	short    color;
	short    packedColor;
	struct particle_s *next;
	vec3f_t  vel;
	float    ramp;
	float    die;
	ptype_t  type;
	void   (*deathfunc)(struct particle_s *particle);
	void   (*callback)(struct particle_s *particle, float frametime);
	unsigned char context;
} particle_t;

// beam flags (a beam's type is its TE_ number)
#define FBEAM_STARTENTITY 0x00000001
#define FBEAM_ENDENTITY   0x00000002
#define FBEAM_FADEIN      0x00000004
#define FBEAM_FADEOUT     0x00000008
#define FBEAM_SINENOISE   0x00000010
#define FBEAM_SOLID       0x00000020
#define FBEAM_SHADEIN     0x00000040
#define FBEAM_SHADEOUT    0x00000080
#define FBEAM_STARTVISIBLE 0x10000000
#define FBEAM_ENDVISIBLE  0x20000000
#define FBEAM_ISACTIVE    0x40000000
#define FBEAM_FOREVER     0x80000000

typedef struct beam_s {
	struct beam_s *next;
	int      type;
	int      flags;
	vec3f_t  source, target, delta;
	float    t;              // 0..1 over the beam's life
	float    freq;
	float    die;
	float    width;
	float    amplitude;
	float    r, g, b;
	float    brightness;
	float    speed;
	float    frameRate;
	float    frame;
	int      segments;
	int      startEntity, endEntity;
	int      modelIndex;
	int      frameCount;
	struct model_s *pFollowModel;
	particle_t *particles;
} BEAM;

typedef struct dlight_s {
	vec3f_t  origin;
	float    radius;
	color24  color;
	float    die;
	float    decay;
	float    minlight;
	int      key;
	qboolean dark;
} dlight_t;

// ---------------------------------------------------------------- the extra API tables

typedef struct {
	int     version;
	void  (*RenderMode)(int mode);
	void  (*Begin)(int primitiveCode);
	void  (*End)(void);
	void  (*Color4f)(float r, float g, float b, float a);
	void  (*Color4ub)(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
	void  (*TexCoord2f)(float u, float v);
	void  (*Vertex3fv)(float *worldPnt);
	void  (*Vertex3f)(float x, float y, float z);
	void  (*Brightness)(float brightness);
	void  (*CullFace)(int style);
	int   (*SpriteTexture)(struct model_s *pSpriteModel, int frame);
	int   (*WorldToScreen)(float *world, float *screen);
	void  (*Fog)(float flFogColor[3], float flStart, float flEnd, int bOn);
	void  (*ScreenToWorld)(float *screen, float *world);
	void  (*GetMatrix)(const int pname, float *matrix);
	int   (*BoxInPVS)(float *mins, float *maxs);
	void  (*LightAtPoint)(float *pos, float *value);
	void  (*Color4fRendermode)(float r, float g, float b, float a, int rendermode);
	void  (*FogParams)(float flDensity, int iFogSkybox);
	void   *reserved[16];
} triangleapi_t;

#define TRI_TRIANGLES      0
#define TRI_TRIANGLE_FAN   1
#define TRI_QUADS          2
#define TRI_POLYGON        3
#define TRI_LINES          4
#define TRI_TRIANGLE_STRIP 5
#define TRI_QUAD_STRIP     6
#define TRI_FRONT 0
#define TRI_NONE  1

typedef struct {
	particle_t *(*R_AllocParticle)(void (*callback)(particle_t *particle, float frametime));
	void   (*R_BlobExplosion)(float *org);
	void   (*R_Blood)(float *org, float *dir, int pcolor, int speed);
	void   (*R_BloodSprite)(float *org, int colorindex, int modelIndex, int modelIndex2, float size);
	void   (*R_BloodStream)(float *org, float *dir, int pcolor, int speed);
	void   (*R_BreakModel)(float *pos, float *size, float *dir, float random, float life, int count, int modelIndex, char flags);
	void   (*R_Bubbles)(float *mins, float *maxs, float height, int modelIndex, int count, float speed);
	void   (*R_BubbleTrail)(float *start, float *end, float height, int modelIndex, int count, float speed);
	void   (*R_BulletImpactParticles)(float *pos);
	void   (*R_EntityParticles)(cl_entity_t *ent);
	void   (*R_Explosion)(float *pos, int model, float scale, float framerate, int flags);
	void   (*R_FizzEffect)(cl_entity_t *pent, int modelIndex, int density);
	void   (*R_FireField)(float *org, int radius, int modelIndex, int count, int flags, float life);
	void   (*R_FlickerParticles)(float *org);
	void   (*R_FunnelSprite)(float *org, int modelIndex, int reverse);
	void   (*R_Implosion)(float *end, float radius, int count, float life);
	void   (*R_LargeFunnel)(float *org, int reverse);
	void   (*R_LavaSplash)(float *org);
	void   (*R_MultiGunshot)(float *org, float *dir, float *noise, int count, int decalCount, int *decalIndices);
	void   (*R_MuzzleFlash)(float *pos1, int type);
	void   (*R_ParticleBox)(float *mins, float *maxs, unsigned char r, unsigned char g, unsigned char b, float life);
	void   (*R_ParticleBurst)(float *pos, int size, int color, float life);
	void   (*R_ParticleExplosion)(float *org);
	void   (*R_ParticleExplosion2)(float *org, int colorStart, int colorLength);
	void   (*R_ParticleLine)(float *start, float *end, unsigned char r, unsigned char g, unsigned char b, float life);
	void   (*R_PlayerSprites)(int client, int modelIndex, int count, int size);
	void   (*R_Projectile)(float *origin, float *velocity, int modelIndex, int life, int owner,
	                       void (*hitcallback)(TEMPENTITY *ent, struct pmtrace_s *ptr));
	void   (*R_RicochetSound)(float *pos);
	void   (*R_RicochetSprite)(float *pos, struct model_s *pmodel, float duration, float scale);
	void   (*R_RocketFlare)(float *pos);
	void   (*R_RocketTrail)(float *start, float *end, int type);
	void   (*R_RunParticleEffect)(float *org, float *dir, int color, int count);
	void   (*R_ShowLine)(float *start, float *end);
	void   (*R_SparkEffect)(float *pos, int count, int velocityMin, int velocityMax);
	void   (*R_SparkShower)(float *pos);
	void   (*R_SparkStreaks)(float *pos, int count, int velocityMin, int velocityMax);
	void   (*R_Spray)(float *pos, float *dir, int modelIndex, int count, int speed, int spread, int rendermode);
	void   (*R_Sprite_Explode)(TEMPENTITY *pTemp, float scale, int flags);
	void   (*R_Sprite_Smoke)(TEMPENTITY *pTemp, float scale);
	void   (*R_Sprite_Spray)(float *pos, float *dir, int modelIndex, int count, int speed, int iRand);
	void   (*R_Sprite_Trail)(int type, float *start, float *end, int modelIndex, int count, float life, float size,
	                         float amplitude, int renderamt, float speed);
	void   (*R_Sprite_WallPuff)(TEMPENTITY *pTemp, float scale);
	void   (*R_StreakSplash)(float *pos, float *dir, int color, int count, float speed, int velocityMin, int velocityMax);
	void   (*R_TracerEffect)(float *start, float *end);
	void   (*R_UserTracerParticle)(float *org, float *vel, float life, int colorIndex, float length,
	                               unsigned char deathcontext, void (*deathfunc)(particle_t *particle));
	particle_t *(*R_TracerParticles)(float *org, float *vel, float life);
	void   (*R_TeleportSplash)(float *org);
	void   (*R_TempSphereModel)(float *pos, float speed, float life, int count, int modelIndex);
	TEMPENTITY *(*R_TempModel)(float *pos, float *dir, float *angles, float life, int modelIndex, int soundtype);
	TEMPENTITY *(*R_DefaultSprite)(float *pos, int spriteIndex, float framerate);
	TEMPENTITY *(*R_TempSprite)(float *pos, float *dir, float scale, int modelIndex, int rendermode, int renderfx,
	                            float a, float life, int flags);
	int    (*Draw_DecalIndex)(int id);
	int    (*Draw_DecalIndexFromName)(char *name);
	void   (*R_DecalShoot)(int textureIndex, int entity, int modelIndex, float *position, int flags);
	void   (*R_AttachTentToPlayer)(int client, int modelIndex, float zoffset, float life);
	void   (*R_KillAttachedTents)(int client);
	BEAM  *(*R_BeamCirclePoints)(int type, float *start, float *end, int modelIndex, float life, float width, float amplitude,
	                             float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	BEAM  *(*R_BeamEntPoint)(int startEnt, float *end, int modelIndex, float life, float width, float amplitude,
	                         float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	BEAM  *(*R_BeamEnts)(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	                     float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	BEAM  *(*R_BeamFollow)(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness);
	void   (*R_BeamKill)(int deadEntity);
	BEAM  *(*R_BeamLightning)(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	                          float brightness, float speed);
	BEAM  *(*R_BeamPoints)(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	                       float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	BEAM  *(*R_BeamRing)(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	                     float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	dlight_t *(*CL_AllocDlight)(int key);
	dlight_t *(*CL_AllocElight)(int key);
	TEMPENTITY *(*CL_TempEntAlloc)(float *org, struct model_s *model);
	TEMPENTITY *(*CL_TempEntAllocNoModel)(float *org);
	TEMPENTITY *(*CL_TempEntAllocHigh)(float *org, struct model_s *model);
	TEMPENTITY *(*CL_TentEntAllocCustom)(float *origin, struct model_s *model, int high,
	                                     void (*callback)(TEMPENTITY *ent, float frametime, float currenttime));
	void   (*R_GetPackedColor)(short *packed, short color);
	short  (*R_LookupColor)(unsigned char r, unsigned char g, unsigned char b);
	void   (*R_DecalRemoveAll)(int textureIndex);
	void   (*R_FireCustomDecal)(int textureIndex, int entity, int modelIndex, float *position, int flags, float scale);
	void   *reserved[16];
} efx_api_t;

typedef struct {
	int     version;
	void  (*EV_PlaySound)(int ent, float *origin, int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch);
	void  (*EV_StopSound)(int ent, int channel, const char *sample);
	int   (*EV_FindModelIndex)(const char *pmodel);
	int   (*EV_IsLocal)(int playernum);
	int   (*EV_LocalPlayerDucking)(void);
	void  (*EV_LocalPlayerViewheight)(float *viewheight);
	void  (*EV_LocalPlayerBounds)(int hull, float *mins, float *maxs);
	int   (*EV_IndexFromTrace)(struct pmtrace_s *pTrace);
	physent_t *(*EV_GetPhysent)(int idx);
	void  (*EV_SetUpPlayerPrediction)(int dopred, int bIncludeLocalClient);
	void  (*EV_PushPMStates)(void);
	void  (*EV_PopPMStates)(void);
	void  (*EV_SetSolidPlayers)(int playernum);
	void  (*EV_SetTraceHull)(int hull);
	void  (*EV_PlayerTrace)(float *start, float *end, int traceFlags, int ignore_pe, struct pmtrace_s *tr);
	void  (*EV_WeaponAnimation)(int sequence, int body);
	unsigned short (*EV_PrecacheEvent)(int type, const char *psz);
	void  (*EV_PlaybackEvent)(int flags, const edict_t *pInvoker, unsigned short eventindex, float delay, float *origin,
	                          float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2);
	const char *(*EV_TraceTexture)(int ground, float *vstart, float *vend);
	void  (*EV_StopAllSounds)(int entnum, int entchannel);
	void  (*EV_KillEvents)(int entnum, const char *eventname);
	void   *reserved[16];
} event_api_t;

// The studio model API the engine gives the client (HUD_GetStudioModelInterface). The client keeps a
// copy and uses parts of it outside model drawing too (the egon asks IsHardware).
typedef struct {
	void   *(*Mem_Calloc)(int number, size_t size);
	void   *(*Cache_Check)(void *c);
	void    (*LoadCacheFile)(char *path, void *cu);
	struct model_s *(*Mod_ForName)(const char *name, int crash_if_missing);
	void   *(*Mod_Extradata)(struct model_s *mod);
	struct model_s *(*GetModelByIndex)(int index);
	cl_entity_t *(*GetCurrentEntity)(void);
	void   *(*PlayerInfo)(int index);
	entity_state_t *(*GetPlayerState)(int index);
	cl_entity_t *(*GetViewEntity)(void);
	void    (*GetTimes)(int *framecount, double *current, double *old);
	cvar_t *(*GetCvar)(const char *name);
	void    (*GetViewInfo)(float *origin, float *upv, float *rightv, float *vpnv);
	struct model_s *(*GetChromeSprite)(void);
	void    (*GetModelCounters)(int **s, int **a);
	void    (*GetAliasScale)(float *x, float *y);
	float ****(*StudioGetBoneTransform)(void);
	float ****(*StudioGetLightTransform)(void);
	float ***(*StudioGetAliasTransform)(void);
	float ***(*StudioGetRotationMatrix)(void);
	void    (*StudioSetupModel)(int bodypart, void **ppbodypart, void **ppsubmodel);
	int     (*StudioCheckBBox)(void);
	void    (*StudioDynamicLight)(cl_entity_t *ent, void *plight);
	void    (*StudioEntityLight)(void *plight);
	void    (*StudioSetupLighting)(void *plighting);
	void    (*StudioDrawPoints)(void);
	void    (*StudioDrawHulls)(void);
	void    (*StudioDrawAbsBBox)(void);
	void    (*StudioDrawBones)(void);
	void    (*StudioSetupSkin)(void *ptexturehdr, int index);
	void    (*StudioSetRemapColors)(int top, int bottom);
	struct model_s *(*SetupPlayerModel)(int index);
	void    (*StudioClientEvents)(void);
	int     (*GetForceFaceFlags)(void);
	void    (*SetForceFaceFlags)(int flags);
	void    (*StudioSetHeader)(void *header);
	void    (*SetRenderModel)(struct model_s *model);
	void    (*SetupRenderer)(int rendermode);
	void    (*RestoreRenderer)(void);
	void    (*SetChromeOrigin)(void);
	int     (*IsHardware)(void);
	void    (*GL_StudioDrawShadow)(void);
	void    (*GL_SetRenderMode)(int mode);
	void    (*StudioSetRenderamt)(int iRenderamt);
	void    (*StudioSetCullState)(int iCull);
	void    (*StudioRenderShadow)(int iSprite, float *p1, float *p2, float *p3, float *p4);
} engine_studio_api_t;

#define STUDIO_INTERFACE_VERSION 1

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
_Static_assert(offsetof(TEMPENTITY, entity) == 68, "TEMPENTITY layout");
// the tables are arrays of pointers in a fixed order; these catch an entry added or lost by mistake
_Static_assert(offsetof(efx_api_t, R_FireCustomDecal) == 73 * 4, "efx_api_t layout");
_Static_assert(offsetof(event_api_t, EV_KillEvents) == 21 * 4, "event_api_t layout");
_Static_assert(offsetof(triangleapi_t, FogParams) == 19 * 4, "triangleapi_t layout");
_Static_assert(offsetof(engine_studio_api_t, IsHardware) == 40 * 4, "engine_studio_api_t layout");
