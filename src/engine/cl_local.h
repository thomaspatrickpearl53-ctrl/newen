// newen - what the client-side files share: the client DLL's exports, client time and entities,
// and the effect systems (events, temp entities, particles, beams, decals, triangles, view effects)
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "client_api.h"
#include "server.h"

// ---------------------------------------------------------------- cl_main.c

extern cldll_exports_t cl;
extern qboolean        client_ok;
extern double          cl_time, cl_oldtime;   // the server's clock, one process
extern cl_entity_t     cl_viewent;
extern char            cl_crash_where[256];
int          CL_CrashFilter(EXCEPTION_POINTERS *ep);
cl_entity_t *CL_EntityByIndex(int i);         // NULL if out of range
void         CL_SetViewModelAnim(int anim, int body);
qboolean     CL_BuildViewModel(rentity_t *r);  // false when no view model should be drawn
pfnUserMsgHook CL_FindMessageHook(const char *name);
int          CL_AddVisibleEntity(cl_entity_t *ent);    // a client entity becomes a frame entity for the renderer

// effects only need cheap randomness
static inline float CL_Random(float lo, float hi) { return lo + (hi - lo) * (rand() / (float)RAND_MAX); }
static inline int   CL_RandomInt(int lo, int hi) { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); }

// A crash in the client is turned into an error and the client is switched off, instead of
// taking the engine down with it
#define CLIENT_CALL(what, call) \
	do { \
		if (client_ok) { \
			__try { call; } \
			__except (CL_CrashFilter(GetExceptionInformation())) { \
				Con_Errorf("client.dll crashed in %s at %s; the client is now off\n", what, cl_crash_where); \
				client_ok = false; \
			} \
		} \
	} while (0)

// ---------------------------------------------------------------- cl_event.c

void CL_InitEventAPI(event_api_t *api);
void CL_HookEvent(const char *name, pfnEventHook fn);
void CL_PlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, const float *origin,
	const float *angles, float f1, float f2, int i1, int i2, int b1, int b2);   // from the game or the client
void CL_FireEvents(void);
void CL_ClearEvents(void);

// ---------------------------------------------------------------- cl_tent.c

// temp entity message numbers (svc_temp_entity)
enum {
	TE_BEAMPOINTS = 0, TE_BEAMENTPOINT = 1, TE_GUNSHOT = 2, TE_EXPLOSION = 3, TE_TAREXPLOSION = 4, TE_SMOKE = 5,
	TE_TRACER = 6, TE_LIGHTNING = 7, TE_BEAMENTS = 8, TE_SPARKS = 9, TE_LAVASPLASH = 10, TE_TELEPORT = 11,
	TE_EXPLOSION2 = 12, TE_BSPDECAL = 13, TE_IMPLOSION = 14, TE_SPRITETRAIL = 15, TE_SPRITE = 17,
	TE_BEAMSPRITE = 18, TE_BEAMTORUS = 19, TE_BEAMDISK = 20, TE_BEAMCYLINDER = 21, TE_BEAMFOLLOW = 22,
	TE_GLOWSPRITE = 23, TE_BEAMRING = 24, TE_STREAK_SPLASH = 25, TE_DLIGHT = 27, TE_ELIGHT = 28,
	TE_TEXTMESSAGE = 29, TE_LINE = 30, TE_BOX = 31, TE_KILLBEAM = 99, TE_LARGEFUNNEL = 100,
	TE_BLOODSTREAM = 101, TE_SHOWLINE = 102, TE_BLOOD = 103, TE_DECAL = 104, TE_FIZZ = 105, TE_MODEL = 106,
	TE_EXPLODEMODEL = 107, TE_BREAKMODEL = 108, TE_GUNSHOTDECAL = 109, TE_SPRITE_SPRAY = 110,
	TE_ARMOR_RICOCHET = 111, TE_PLAYERDECAL = 112, TE_BUBBLES = 113, TE_BUBBLETRAIL = 114,
	TE_BLOODSPRITE = 115, TE_WORLDDECAL = 116, TE_WORLDDECALHIGH = 117, TE_DECALHIGH = 118,
	TE_PROJECTILE = 119, TE_SPRAY = 120, TE_PLAYERSPRITES = 121, TE_PARTICLEBURST = 122, TE_FIREFIELD = 123,
	TE_PLAYERATTACHMENT = 124, TE_KILLPLAYERATTACHMENTS = 125, TE_MULTIGUNSHOT = 126, TE_USERTRACER = 127,
};

#define TE_EXPLFLAG_NOADDITIVE  1
#define TE_EXPLFLAG_NODLIGHTS   2
#define TE_EXPLFLAG_NOSOUND     4
#define TE_EXPLFLAG_NOPARTICLES 8

// R_BreakModel flags (the material picks the bounce sound)
#define BREAK_GLASS    0x01
#define BREAK_METAL    0x02
#define BREAK_FLESH    0x04
#define BREAK_WOOD     0x08
#define BREAK_SMOKE    0x10
#define BREAK_TRANS    0x20
#define BREAK_CONCRETE 0x40

// temp entity bounce sounds (TEMPENTITY hitSound)
enum { BOUNCE_GLASS = 1, BOUNCE_METAL, BOUNCE_FLESH, BOUNCE_WOOD, BOUNCE_SHRAP, BOUNCE_SHELL, BOUNCE_CONCRETE,
	BOUNCE_SHOTSHELL };

void        CL_InitTempEnts(void);
void        CL_InitEfxAPI(efx_api_t *api);
void        CL_ClearTempEnts(void);                         // map change
void        CL_ParseTempEntity(const unsigned char *data, int len);
void        CL_AddEntities(void);                           // per frame, after the server's entities
void        CL_StudioEvents(cl_entity_t *ent, const rentity_t *r);   // client events (5000+) of one model
TEMPENTITY *CL_TempEntAlloc(const float *org, model_t *model);
TEMPENTITY *CL_TempEntAllocHigh(const float *org, model_t *model);
model_t    *CL_ModelForIndex(int index);
void        CL_GetEntityPoint(int entnum, vec3_t out);      // origin, or an attachment ("entity:attachment" packing)

// ---------------------------------------------------------------- r_part.c

void        R_InitParticles(void);
void        R_ClearParticles(void);
void        R_DrawParticles(void);
extern unsigned char r_palette[768];                         // gfx/palette.lmp
particle_t *R_AllocParticle(void (*callback)(particle_t *p, float frametime));
void        R_BlobExplosion(float *org);
void        R_Blood(float *org, float *dir, int pcolor, int speed);
void        R_BloodStream(float *org, float *dir, int pcolor, int speed);
void        R_BulletImpactParticles(float *pos);
void        R_EntityParticles(cl_entity_t *ent);
void        R_FlickerParticles(float *org);
void        R_Implosion(float *end, float radius, int count, float life);
void        R_LargeFunnel(float *org, int reverse);
void        R_LavaSplash(float *org);
void        R_ParticleBox(float *mins, float *maxs, unsigned char r, unsigned char g, unsigned char b, float life);
void        R_ParticleBurst(float *pos, int size, int color, float life);
void        R_ParticleExplosion(float *org);
void        R_ParticleExplosion2(float *org, int colorStart, int colorLength);
void        R_ParticleLine(float *start, float *end, unsigned char r, unsigned char g, unsigned char b, float life);
void        R_RocketTrail(float *start, float *end, int type);
void        R_RunParticleEffect(float *org, float *dir, int color, int count);
void        R_ShowLine(float *start, float *end);
void        R_SparkStreaks(float *pos, int count, int velocityMin, int velocityMax);
void        R_StreakSplash(float *pos, float *dir, int color, int count, float speed, int velocityMin, int velocityMax);
void        R_TeleportSplash(float *org);
void        R_TracerEffect(float *start, float *end);
particle_t *R_TracerParticles(float *org, float *vel, float life);
void        R_UserTracerParticle(float *org, float *vel, float life, int colorIndex, float length,
	unsigned char deathcontext, void (*deathfunc)(particle_t *p));
short       R_LookupColor(unsigned char r, unsigned char g, unsigned char b);
void        R_GetPackedColor(short *packed, short color);

// ---------------------------------------------------------------- r_beam.c

void  R_InitBeams(void);
void  R_ClearBeams(void);
void  R_DrawBeams(void);                                    // efx beams and the game's beam entities
void  R_AddBeamEntity(const edict_t *e, model_t *sprite);   // an edict with FL_CUSTOMENTITY
BEAM *R_BeamCirclePoints(int type, float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
BEAM *R_BeamEntPoint(int startEnt, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
BEAM *R_BeamEnts(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
BEAM *R_BeamFollow(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness);
void  R_BeamKill(int deadEntity);
BEAM *R_BeamLightning(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed);
BEAM *R_BeamPoints(float *start, float *end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
BEAM *R_BeamRing(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b);

// ---------------------------------------------------------------- cl_studio.c

void CL_InitStudioAPI(void);   // hands the client the studio model API table

// ---------------------------------------------------------------- cl_tri.c

void CL_InitTriAPI(triangleapi_t *api);
void CL_CaptureViewMatrices(void);                          // after the 3D view is set up

// ---------------------------------------------------------------- cl_view.c (titles, fade, shake)

void CL_InitView(void);
void CL_LoadTitles(void);
client_textmessage_t *CL_TextMessageGet(const char *name);
void CL_ShowNetMessage(const client_textmessage_t *msg);   // TE_TEXTMESSAGE: through the client's HudText
qboolean CL_EngineMessage(const char *name, const unsigned char *data, int len);   // ScreenFade, ScreenShake
void CL_GetScreenFade(screenfade_t *f);
void CL_SetScreenFade(const screenfade_t *f);
void CL_DrawScreenFade(void);
void CL_CalcShake(void);
void CL_ApplyShake(float *origin, float *angles, float factor);
void CL_ClearView(void);                                    // map change
