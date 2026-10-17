// newen - the interface between the engine and a GoldSrc game DLL (hl.dll and mods)
//
// Written from the publicly documented layout of the Half-Life SDK's engine interface. Every structure
// here is shared memory with the DLL, so field order, types and sizes must match exactly; the
// static asserts at the bottom check the sizes the DLLs were built with. Everything is 32-bit and
// uses the default (cdecl) calling convention, except GiveFnptrsToDll, which is stdcall.
#pragma once
#include "common.h"

typedef int string_t;   // offset from globalvars_t.pStringBase
typedef float vec3f_t[3];

typedef struct edict_s edict_t;
typedef struct link_s { struct link_s *prev, *next; } link_t;

// opaque to the engine for now (networking, prediction, save/restore arrive in later steps)
struct playermove_s;
struct clientdata_s;
struct entity_state_s;
struct usercmd_s;
struct weapon_data_s;
struct customization_s;
struct delta_s;
struct netadr_s;
struct saverestore_s;
struct typedescription_s;

// ---------------------------------------------------------------- entity variables

typedef struct entvars_s {
	string_t classname;
	string_t globalname;
	vec3f_t  origin, oldorigin, velocity, basevelocity, clbasevelocity, movedir;
	vec3f_t  angles, avelocity, punchangle, v_angle;
	vec3f_t  endpos, startpos;
	float    impacttime, starttime;
	int      fixangle;
	float    idealpitch, pitch_speed, ideal_yaw, yaw_speed;
	int      modelindex;
	string_t model;
	int      viewmodel, weaponmodel;
	vec3f_t  absmin, absmax, mins, maxs, size;
	float    ltime, nextthink;
	int      movetype, solid, skin, body, effects;
	float    gravity, friction;
	int      light_level;
	int      sequence, gaitsequence;
	float    frame, animtime, framerate;
	unsigned char controller[4], blending[2];
	float    scale;
	int      rendermode;
	float    renderamt;
	vec3f_t  rendercolor;
	int      renderfx;
	float    health, frags;
	int      weapons;
	float    takedamage;
	int      deadflag;
	vec3f_t  view_ofs;
	int      button, impulse;
	edict_t *chain, *dmg_inflictor, *enemy, *aiment, *owner, *groundentity;
	int      spawnflags, flags;
	int      colormap, team;
	float    max_health, teleport_time, armortype, armorvalue;
	int      waterlevel, watertype;
	string_t target, targetname, netname, message;
	float    dmg_take, dmg_save, dmg, dmgtime;
	string_t noise, noise1, noise2, noise3;
	float    speed, air_finished, pain_finished, radsuit_finished;
	edict_t *pContainingEntity;
	int      playerclass;
	float    maxspeed;
	float    fov;
	int      weaponanim;
	int      pushmsec;
	int      bInDuck, flTimeStepSound, flSwimTime, flDuckTime, iStepLeft;
	float    flFallVelocity;
	int      gamestate;
	int      oldbuttons;
	int      groupinfo;
	int      iuser1, iuser2, iuser3, iuser4;
	float    fuser1, fuser2, fuser3, fuser4;
	vec3f_t  vuser1, vuser2, vuser3, vuser4;
	edict_t *euser1, *euser2, *euser3, *euser4;
} entvars_t;

#define MAX_ENT_LEAFS 48

struct edict_s {
	qboolean  free;
	int       serialnumber;
	link_t    area;
	int       headnode;            // -1 when the leaf list below is complete
	int       num_leafs;
	short     leafnums[MAX_ENT_LEAFS];
	float     freetime;
	void     *pvPrivateData;       // the game's C++ object
	entvars_t v;
};

typedef struct {
	float    time, frametime, force_retouch;
	string_t mapname, startspot;
	float    deathmatch, coop, teamplay, serverflags, found_secrets;
	vec3f_t  v_forward, v_up, v_right;
	float    trace_allsolid, trace_startsolid, trace_fraction;
	vec3f_t  trace_endpos, trace_plane_normal;
	float    trace_plane_dist;
	edict_t *trace_ent;
	float    trace_inopen, trace_inwater;
	int      trace_hitgroup, trace_flags;
	int      msg_entity;
	int      cdAudioTrack;
	int      maxClients, maxEntities;
	const char *pStringBase;
	void    *pSaveData;
	vec3f_t  vecLandmarkOffset;
} globalvars_t;

typedef struct {
	int      fAllSolid, fStartSolid, fInOpen, fInWater;
	float    flFraction;
	vec3f_t  vecEndPos;
	float    flPlaneDist;
	vec3f_t  vecPlaneNormal;
	edict_t *pHit;
	int      iHitgroup;
} TraceResult;

typedef struct {
	char *szClassName, *szKeyName, *szValue;
	int   fHandled;
} KeyValueData;

// ---------------------------------------------------------------- constants

// movetype
#define MOVETYPE_NONE          0
#define MOVETYPE_WALK          3
#define MOVETYPE_STEP          4
#define MOVETYPE_FLY           5
#define MOVETYPE_TOSS          6
#define MOVETYPE_PUSH          7
#define MOVETYPE_NOCLIP        8
#define MOVETYPE_FLYMISSILE    9
#define MOVETYPE_BOUNCE        10
#define MOVETYPE_BOUNCEMISSILE 11
#define MOVETYPE_FOLLOW        12
#define MOVETYPE_PUSHSTEP      13

// solid
#define SOLID_NOT      0
#define SOLID_TRIGGER  1
#define SOLID_BBOX     2
#define SOLID_SLIDEBOX 3
#define SOLID_BSP      4

// flags
#define FL_FLY            (1<<0)
#define FL_SWIM           (1<<1)
#define FL_CONVEYOR       (1<<2)
#define FL_CLIENT         (1<<3)
#define FL_INWATER        (1<<4)
#define FL_MONSTER        (1<<5)
#define FL_GODMODE        (1<<6)
#define FL_NOTARGET       (1<<7)
#define FL_SKIPLOCALHOST  (1<<8)
#define FL_ONGROUND       (1<<9)
#define FL_PARTIALGROUND  (1<<10)
#define FL_WATERJUMP      (1<<11)
#define FL_FROZEN         (1<<12)
#define FL_FAKECLIENT     (1<<13)
#define FL_DUCKING        (1<<14)
#define FL_FLOAT          (1<<15)
#define FL_GRAPHED        (1<<16)
#define FL_IMMUNE_WATER   (1<<17)
#define FL_IMMUNE_SLIME   (1<<18)
#define FL_IMMUNE_LAVA    (1<<19)
#define FL_PROXY          (1<<20)
#define FL_ALWAYSTHINK    (1<<21)
#define FL_BASEVELOCITY   (1<<22)
#define FL_MONSTERCLIP    (1<<23)
#define FL_ONTRAIN        (1<<24)
#define FL_WORLDBRUSH     (1<<25)
#define FL_SPECTATOR      (1<<26)
#define FL_CUSTOMENTITY   (1<<29)
#define FL_KILLME         (1<<30)
#define FL_DORMANT        (1<<31)

// effects
#define EF_BRIGHTFIELD 1
#define EF_MUZZLEFLASH 2
#define EF_BRIGHTLIGHT 4
#define EF_DIMLIGHT    8
#define EF_INVLIGHT    16
#define EF_NOINTERP    32
#define EF_LIGHT       64
#define EF_NODRAW      128

// pfnWalkMove modes
#define WALKMOVE_NORMAL    0
#define WALKMOVE_WORLDONLY 1
#define WALKMOVE_CHECKONLY 2

// pfnMoveToOrigin types
#define MOVE_NORMAL 0
#define MOVE_STRAFE 1

// trace types (fNoMonsters)
#define MOVE_NOMONSTERS 1
#define MOVE_MISSILE    2

// pfnAlertMessage
typedef enum { at_notice, at_console, at_aiconsole, at_warning, at_error, at_logged } ALERT_TYPE;
// pfnClientPrintf
typedef enum { print_console, print_center, print_chat } PRINT_TYPE;
typedef enum { force_exactfile, force_model_samebounds, force_model_specifybounds, force_model_specifybounds_if_avail } FORCE_TYPE;

// message destinations
#define MSG_BROADCAST      0
#define MSG_ONE            1
#define MSG_ALL            2
#define MSG_INIT           3
#define MSG_PVS            4
#define MSG_PAS            5
#define MSG_PVS_R          6
#define MSG_PAS_R          7
#define MSG_ONE_UNRELIABLE 8
#define MSG_SPEC           9

typedef unsigned int CRC32_t;

// ---------------------------------------------------------------- engine functions (engine -> DLL)

typedef struct {
	int         (*pfnPrecacheModel)(const char *s);
	int         (*pfnPrecacheSound)(const char *s);
	void        (*pfnSetModel)(edict_t *e, const char *m);
	int         (*pfnModelIndex)(const char *m);
	int         (*pfnModelFrames)(int modelIndex);
	void        (*pfnSetSize)(edict_t *e, const float *rgflMin, const float *rgflMax);
	void        (*pfnChangeLevel)(const char *s1, const char *s2);
	void        (*pfnGetSpawnParms)(edict_t *ent);
	void        (*pfnSaveSpawnParms)(edict_t *ent);
	float       (*pfnVecToYaw)(const float *rgflVector);
	void        (*pfnVecToAngles)(const float *rgflVectorIn, float *rgflVectorOut);
	void        (*pfnMoveToOrigin)(edict_t *ent, const float *pflGoal, float dist, int iMoveType);
	void        (*pfnChangeYaw)(edict_t *ent);
	void        (*pfnChangePitch)(edict_t *ent);
	edict_t    *(*pfnFindEntityByString)(edict_t *pEdictStartSearchAfter, const char *pszField, const char *pszValue);
	int         (*pfnGetEntityIllum)(edict_t *pEnt);
	edict_t    *(*pfnFindEntityInSphere)(edict_t *pEdictStartSearchAfter, const float *org, float rad);
	edict_t    *(*pfnFindClientInPVS)(edict_t *pEdict);
	edict_t    *(*pfnEntitiesInPVS)(edict_t *pplayer);
	void        (*pfnMakeVectors)(const float *rgflVector);
	void        (*pfnAngleVectors)(const float *rgflVector, float *forward, float *right, float *up);
	edict_t    *(*pfnCreateEntity)(void);
	void        (*pfnRemoveEntity)(edict_t *e);
	edict_t    *(*pfnCreateNamedEntity)(int className);
	void        (*pfnMakeStatic)(edict_t *ent);
	int         (*pfnEntIsOnFloor)(edict_t *e);
	int         (*pfnDropToFloor)(edict_t *e);
	int         (*pfnWalkMove)(edict_t *ent, float yaw, float dist, int iMode);
	void        (*pfnSetOrigin)(edict_t *e, const float *rgflOrigin);
	void        (*pfnEmitSound)(edict_t *entity, int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch);
	void        (*pfnEmitAmbientSound)(edict_t *entity, float *pos, const char *samp, float vol, float attenuation, int fFlags, int pitch);
	void        (*pfnTraceLine)(const float *v1, const float *v2, int fNoMonsters, edict_t *pentToSkip, TraceResult *ptr);
	void        (*pfnTraceToss)(edict_t *pent, edict_t *pentToIgnore, TraceResult *ptr);
	int         (*pfnTraceMonsterHull)(edict_t *pEdict, const float *v1, const float *v2, int fNoMonsters, edict_t *pentToSkip, TraceResult *ptr);
	void        (*pfnTraceHull)(const float *v1, const float *v2, int fNoMonsters, int hullNumber, edict_t *pentToSkip, TraceResult *ptr);
	void        (*pfnTraceModel)(const float *v1, const float *v2, int hullNumber, edict_t *pent, TraceResult *ptr);
	const char *(*pfnTraceTexture)(edict_t *pTextureEntity, const float *v1, const float *v2);
	void        (*pfnTraceSphere)(const float *v1, const float *v2, int fNoMonsters, float radius, edict_t *pentToSkip, TraceResult *ptr);
	void        (*pfnGetAimVector)(edict_t *ent, float speed, float *rgflReturn);
	void        (*pfnServerCommand)(const char *str);
	void        (*pfnServerExecute)(void);
	void        (*pfnClientCommand)(edict_t *pEdict, const char *szFmt, ...);
	void        (*pfnParticleEffect)(const float *org, const float *dir, float color, float count);
	void        (*pfnLightStyle)(int style, const char *val);
	int         (*pfnDecalIndex)(const char *name);
	int         (*pfnPointContents)(const float *rgflVector);
	void        (*pfnMessageBegin)(int msg_dest, int msg_type, const float *pOrigin, edict_t *ed);
	void        (*pfnMessageEnd)(void);
	void        (*pfnWriteByte)(int iValue);
	void        (*pfnWriteChar)(int iValue);
	void        (*pfnWriteShort)(int iValue);
	void        (*pfnWriteLong)(int iValue);
	void        (*pfnWriteAngle)(float flValue);
	void        (*pfnWriteCoord)(float flValue);
	void        (*pfnWriteString)(const char *sz);
	void        (*pfnWriteEntity)(int iValue);
	void        (*pfnCVarRegister)(cvar_t *pCvar);
	float       (*pfnCVarGetFloat)(const char *szVarName);
	const char *(*pfnCVarGetString)(const char *szVarName);
	void        (*pfnCVarSetFloat)(const char *szVarName, float flValue);
	void        (*pfnCVarSetString)(const char *szVarName, const char *szValue);
	void        (*pfnAlertMessage)(ALERT_TYPE atype, const char *szFmt, ...);
	void        (*pfnEngineFprintf)(void *pfile, const char *szFmt, ...);
	void       *(*pfnPvAllocEntPrivateData)(edict_t *pEdict, int cb);
	void       *(*pfnPvEntPrivateData)(edict_t *pEdict);
	void        (*pfnFreeEntPrivateData)(edict_t *pEdict);
	const char *(*pfnSzFromIndex)(int iString);
	int         (*pfnAllocString)(const char *szValue);
	entvars_t  *(*pfnGetVarsOfEnt)(edict_t *pEdict);
	edict_t    *(*pfnPEntityOfEntOffset)(int iEntOffset);
	int         (*pfnEntOffsetOfPEntity)(const edict_t *pEdict);
	int         (*pfnIndexOfEdict)(const edict_t *pEdict);
	edict_t    *(*pfnPEntityOfEntIndex)(int iEntIndex);
	edict_t    *(*pfnFindEntityByVars)(entvars_t *pvars);
	void       *(*pfnGetModelPtr)(edict_t *pEdict);
	int         (*pfnRegUserMsg)(const char *pszName, int iSize);
	void        (*pfnAnimationAutomove)(const edict_t *pEdict, float flTime);
	void        (*pfnGetBonePosition)(const edict_t *pEdict, int iBone, float *rgflOrigin, float *rgflAngles);
	unsigned    (*pfnFunctionFromName)(const char *pName);
	const char *(*pfnNameForFunction)(unsigned function);
	void        (*pfnClientPrintf)(edict_t *pEdict, PRINT_TYPE ptype, const char *szMsg);
	void        (*pfnServerPrint)(const char *szMsg);
	const char *(*pfnCmd_Args)(void);
	const char *(*pfnCmd_Argv)(int argc);
	int         (*pfnCmd_Argc)(void);
	void        (*pfnGetAttachment)(const edict_t *pEdict, int iAttachment, float *rgflOrigin, float *rgflAngles);
	void        (*pfnCRC32_Init)(CRC32_t *pulCRC);
	void        (*pfnCRC32_ProcessBuffer)(CRC32_t *pulCRC, void *p, int len);
	void        (*pfnCRC32_ProcessByte)(CRC32_t *pulCRC, unsigned char ch);
	CRC32_t     (*pfnCRC32_Final)(CRC32_t pulCRC);
	int         (*pfnRandomLong)(int lLow, int lHigh);
	float       (*pfnRandomFloat)(float flLow, float flHigh);
	void        (*pfnSetView)(const edict_t *pClient, const edict_t *pViewent);
	float       (*pfnTime)(void);
	void        (*pfnCrosshairAngle)(const edict_t *pClient, float pitch, float yaw);
	unsigned char *(*pfnLoadFileForMe)(const char *filename, int *pLength);
	void        (*pfnFreeFile)(void *buffer);
	void        (*pfnEndSection)(const char *pszSectionName);
	int         (*pfnCompareFileTime)(const char *filename1, const char *filename2, int *iCompare);
	void        (*pfnGetGameDir)(char *szGetGameDir);
	void        (*pfnCvar_RegisterVariable)(cvar_t *variable);
	void        (*pfnFadeClientVolume)(const edict_t *pEdict, int fadePercent, int fadeOutSeconds, int holdTime, int fadeInSeconds);
	void        (*pfnSetClientMaxspeed)(const edict_t *pEdict, float fNewMaxspeed);
	edict_t    *(*pfnCreateFakeClient)(const char *netname);
	void        (*pfnRunPlayerMove)(edict_t *fakeclient, const float *viewangles, float forwardmove, float sidemove, float upmove,
	                                unsigned short buttons, unsigned char impulse, unsigned char msec);
	int         (*pfnNumberOfEntities)(void);
	char       *(*pfnGetInfoKeyBuffer)(edict_t *e);
	char       *(*pfnInfoKeyValue)(char *infobuffer, const char *key);
	void        (*pfnSetKeyValue)(char *infobuffer, const char *key, const char *value);
	void        (*pfnSetClientKeyValue)(int clientIndex, char *infobuffer, const char *key, const char *value);
	int         (*pfnIsMapValid)(const char *filename);
	void        (*pfnStaticDecal)(const float *origin, int decalIndex, int entityIndex, int modelIndex);
	int         (*pfnPrecacheGeneric)(const char *s);
	int         (*pfnGetPlayerUserId)(edict_t *e);
	void        (*pfnBuildSoundMsg)(edict_t *entity, int channel, const char *sample, float volume, float attenuation, int fFlags,
	                                int pitch, int msg_dest, int msg_type, const float *pOrigin, edict_t *ed);
	int         (*pfnIsDedicatedServer)(void);
	cvar_t     *(*pfnCVarGetPointer)(const char *szVarName);
	unsigned    (*pfnGetPlayerWONId)(edict_t *e);
	void        (*pfnInfo_RemoveKey)(char *s, const char *key);
	const char *(*pfnGetPhysicsKeyValue)(const edict_t *pClient, const char *key);
	void        (*pfnSetPhysicsKeyValue)(const edict_t *pClient, const char *key, const char *value);
	const char *(*pfnGetPhysicsInfoString)(const edict_t *pClient);
	unsigned short (*pfnPrecacheEvent)(int type, const char *psz);
	void        (*pfnPlaybackEvent)(int flags, const edict_t *pInvoker, unsigned short eventindex, float delay, float *origin,
	                                float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2);
	unsigned char *(*pfnSetFatPVS)(float *org);
	unsigned char *(*pfnSetFatPAS)(float *org);
	int         (*pfnCheckVisibility)(const edict_t *entity, unsigned char *pset);
	void        (*pfnDeltaSetField)(struct delta_s *pFields, const char *fieldname);
	void        (*pfnDeltaUnsetField)(struct delta_s *pFields, const char *fieldname);
	void        (*pfnDeltaAddEncoder)(const char *name, void (*conditionalencode)(struct delta_s *pFields, const unsigned char *from, const unsigned char *to));
	int         (*pfnGetCurrentPlayer)(void);
	int         (*pfnCanSkipPlayer)(const edict_t *player);
	int         (*pfnDeltaFindField)(struct delta_s *pFields, const char *fieldname);
	void        (*pfnDeltaSetFieldByIndex)(struct delta_s *pFields, int fieldNumber);
	void        (*pfnDeltaUnsetFieldByIndex)(struct delta_s *pFields, int fieldNumber);
	void        (*pfnSetGroupMask)(int mask, int op);
	int         (*pfnCreateInstancedBaseline)(int classname, struct entity_state_s *baseline);
	void        (*pfnCvar_DirectSet)(cvar_t *var, const char *value);
	void        (*pfnForceUnmodified)(FORCE_TYPE type, float *mins, float *maxs, const char *filename);
	void        (*pfnGetPlayerStats)(const edict_t *pClient, int *ping, int *packet_loss);
	void        (*pfnAddServerCommand)(const char *cmd_name, void (*function)(void));
	qboolean    (*pfnVoice_GetClientListening)(int iReceiver, int iSender);
	qboolean    (*pfnVoice_SetClientListening)(int iReceiver, int iSender, qboolean bListen);
	const char *(*pfnGetPlayerAuthId)(edict_t *e);
	void       *(*pfnSequenceGet)(const char *fileName, const char *entryName);
	void       *(*pfnSequencePickSentence)(const char *groupName, int pickMethod, int *picked);
	int         (*pfnGetFileSize)(const char *filename);
	unsigned    (*pfnGetApproxWavePlayLen)(const char *filepath);
	int         (*pfnIsCareerMatch)(void);
	int         (*pfnGetLocalizedStringLength)(const char *label);
	void        (*pfnRegisterTutorMessageShown)(int mid);
	int         (*pfnGetTimesTutorMessageShown)(int mid);
	void        (*pfnProcessTutorMessageDecayBuffer)(int *buffer, int bufferLength);
	void        (*pfnConstructTutorMessageDecayBuffer)(int *buffer, int bufferLength);
	void        (*pfnResetTutorMessageDecayData)(void);
	void        (*pfnQueryClientCvarValue)(const edict_t *player, const char *cvarName);
	void        (*pfnQueryClientCvarValue2)(const edict_t *player, const char *cvarName, int requestID);
	int         (*pfnCheckParm)(const char *pchCmdLineToken, char **ppnext);
	edict_t    *(*pfnPEntityOfEntIndexAllEntities)(int iEntIndex);   // added in the 25th anniversary update
	// newer DLLs may copy a larger table than they know about; these stay harmless
	void        (*pfnReserved[32])(void);
} enginefuncs_t;

// ---------------------------------------------------------------- game functions (DLL -> engine)

#define INTERFACE_VERSION      140
#define NEW_DLL_FUNCTIONS_VERSION 1

typedef struct {
	void        (*pfnGameInit)(void);
	int         (*pfnSpawn)(edict_t *pent);
	void        (*pfnThink)(edict_t *pent);
	void        (*pfnUse)(edict_t *pentUsed, edict_t *pentOther);
	void        (*pfnTouch)(edict_t *pentTouched, edict_t *pentOther);
	void        (*pfnBlocked)(edict_t *pentBlocked, edict_t *pentOther);
	void        (*pfnKeyValue)(edict_t *pentKeyvalue, KeyValueData *pkvd);
	void        (*pfnSave)(edict_t *pent, struct saverestore_s *pSaveData);
	int         (*pfnRestore)(edict_t *pent, struct saverestore_s *pSaveData, int globalEntity);
	void        (*pfnSetAbsBox)(edict_t *pent);
	void        (*pfnSaveWriteFields)(struct saverestore_s *, const char *, void *, struct typedescription_s *, int);
	void        (*pfnSaveReadFields)(struct saverestore_s *, const char *, void *, struct typedescription_s *, int);
	void        (*pfnSaveGlobalState)(struct saverestore_s *);
	void        (*pfnRestoreGlobalState)(struct saverestore_s *);
	void        (*pfnResetGlobalState)(void);
	qboolean    (*pfnClientConnect)(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128]);
	void        (*pfnClientDisconnect)(edict_t *pEntity);
	void        (*pfnClientKill)(edict_t *pEntity);
	void        (*pfnClientPutInServer)(edict_t *pEntity);
	void        (*pfnClientCommand)(edict_t *pEntity);
	void        (*pfnClientUserInfoChanged)(edict_t *pEntity, char *infobuffer);
	void        (*pfnServerActivate)(edict_t *pEdictList, int edictCount, int clientMax);
	void        (*pfnServerDeactivate)(void);
	void        (*pfnPlayerPreThink)(edict_t *pEntity);
	void        (*pfnPlayerPostThink)(edict_t *pEntity);
	void        (*pfnStartFrame)(void);
	void        (*pfnParmsNewLevel)(void);
	void        (*pfnParmsChangeLevel)(void);
	const char *(*pfnGetGameDescription)(void);
	void        (*pfnPlayerCustomization)(edict_t *pEntity, struct customization_s *pCustom);
	void        (*pfnSpectatorConnect)(edict_t *pEntity);
	void        (*pfnSpectatorDisconnect)(edict_t *pEntity);
	void        (*pfnSpectatorThink)(edict_t *pEntity);
	void        (*pfnSys_Error)(const char *error_string);
	void        (*pfnPM_Move)(struct playermove_s *ppmove, qboolean server);
	void        (*pfnPM_Init)(struct playermove_s *ppmove);
	char        (*pfnPM_FindTextureType)(const char *name);
	void        (*pfnSetupVisibility)(edict_t *pViewEntity, edict_t *pClient, unsigned char **pvs, unsigned char **pas);
	void        (*pfnUpdateClientData)(const edict_t *ent, int sendweapons, struct clientdata_s *cd);
	int         (*pfnAddToFullPack)(struct entity_state_s *state, int e, edict_t *ent, edict_t *host, int hostflags, int player,
	                                unsigned char *pSet);
	void        (*pfnCreateBaseline)(int player, int eindex, struct entity_state_s *baseline, edict_t *entity, int playermodelindex,
	                                 float *player_mins, float *player_maxs);
	void        (*pfnRegisterEncoders)(void);
	int         (*pfnGetWeaponData)(edict_t *player, struct weapon_data_s *info);
	void        (*pfnCmdStart)(const edict_t *player, const struct usercmd_s *cmd, unsigned random_seed);
	void        (*pfnCmdEnd)(const edict_t *player);
	int         (*pfnConnectionlessPacket)(const struct netadr_s *net_from, const char *args, char *response_buffer, int *response_buffer_size);
	int         (*pfnGetHullBounds)(int hullnumber, float *mins, float *maxs);
	void        (*pfnCreateInstancedBaselines)(void);
	int         (*pfnInconsistentFile)(const edict_t *player, const char *filename, char *disconnect_message);
	int         (*pfnAllowLagCompensation)(void);
} DLL_FUNCTIONS;

typedef struct {
	void (*pfnOnFreeEntPrivateData)(edict_t *pEnt);
	void (*pfnGameShutdown)(void);
	int  (*pfnShouldCollide)(edict_t *pentTouched, edict_t *pentOther);
	void (*pfnCvarValue)(const edict_t *pEnt, const char *value);
	void (*pfnCvarValue2)(const edict_t *pEnt, int requestID, const char *cvarName, const char *value);
} NEW_DLL_FUNCTIONS;

typedef void (__stdcall *GIVEFNPTRSTODLL)(enginefuncs_t *engfuncs, globalvars_t *globals);
typedef int  (*APIFUNCTION)(DLL_FUNCTIONS *functionTable, int interfaceVersion);
typedef int  (*APIFUNCTION2)(DLL_FUNCTIONS *functionTable, int *interfaceVersion);
typedef int  (*NEW_DLL_FUNCTIONS_FN)(NEW_DLL_FUNCTIONS *functionTable, int *interfaceVersion);
typedef void (*ENTITYINIT)(entvars_t *pev);   // exported per classname by LINK_ENTITY_TO_CLASS

// sizes the shipped DLLs were compiled with
_Static_assert(sizeof(entvars_t) == 676, "entvars_t layout");
_Static_assert(sizeof(edict_t) == 804, "edict_t layout");
_Static_assert(sizeof(TraceResult) == 56, "TraceResult layout");
_Static_assert(offsetof(enginefuncs_t, pfnCheckParm) == 157 * 4, "enginefuncs_t order");
_Static_assert(sizeof(DLL_FUNCTIONS) == 50 * 4, "DLL_FUNCTIONS layout");
