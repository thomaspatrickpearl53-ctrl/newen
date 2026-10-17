// newen - the studio model API given to the client DLL
//
// GoldSrc hands the client this table at startup (HUD_GetStudioModelInterface) so its own
// StudioModelRenderer can draw models. newen still draws models itself and never calls the
// client's renderer, but the client keeps a copy of the table and uses it elsewhere (the egon's
// fire event asks IsHardware), and its renderer's Init stores the pointers the getters return.
// So the queries are real and the drawing entries do nothing.
#include "cl_local.h"

#define MAXSTUDIOBONES 128

static float bone_transform[MAXSTUDIOBONES][3][4];
static float light_transform[MAXSTUDIOBONES][3][4];
static float alias_transform[3][4];
static float rotation_matrix[3][4];
static int model_count, models_drawn;
static int force_face_flags;
static unsigned char player_info[1024];   // GoldSrc's player_info_t; nothing in it yet
static cl_entity_t dummy_entity;
static engine_studio_api_t studio_api;

static void *St_Mem_Calloc(int number, size_t size) { return Mem_Alloc((size_t)number * size); }
static void *St_Cache_Check(void *c) { return c ? *(void **)c : NULL; }
static void St_LoadCacheFile(char *path, void *cu) {}
static struct model_s *St_Mod_ForName(const char *name, int crash) { return Mod_ForName(name, crash != 0); }
static void *St_Mod_Extradata(struct model_s *m) { return Mod_StudioHeader(m); }
static struct model_s *St_GetModelByIndex(int index) { return CL_ModelForIndex(index); }
static cl_entity_t *St_GetCurrentEntity(void) { return &dummy_entity; }
static void *St_PlayerInfo(int index) { return player_info; }

static entity_state_t *St_GetPlayerState(int index)
{
	cl_entity_t *e = CL_EntityByIndex(index + 1);
	return e ? &e->curstate : &dummy_entity.curstate;
}

static cl_entity_t *St_GetViewEntity(void)
{
	cl_entity_t *e = CL_EntityByIndex(1);
	return e ? e : &dummy_entity;
}

static void St_GetTimes(int *framecount, double *current, double *old)
{
	if (framecount) *framecount = r_refdef.framecount;
	if (current) *current = cl_time;
	if (old) *old = cl_oldtime;
}

static cvar_t *St_GetCvar(const char *name) { return Cvar_Find(name); }

static void St_GetViewInfo(float *origin, float *up, float *right, float *vpn)
{
	if (origin) VectorCopy(r_refdef.origin, origin);
	if (up) VectorCopy(r_refdef.up, up);
	if (right) VectorCopy(r_refdef.right, right);
	if (vpn) VectorCopy(r_refdef.forward, vpn);
}

static struct model_s *St_GetChromeSprite(void) { return NULL; }

static void St_GetModelCounters(int **s, int **a)
{
	if (s) *s = &model_count;
	if (a) *a = &models_drawn;
}

static void St_GetAliasScale(float *x, float *y)
{
	if (x) *x = 1;
	if (y) *y = 1;
}

static float ****St_BoneTransform(void) { return (float ****)bone_transform; }
static float ****St_LightTransform(void) { return (float ****)light_transform; }
static float ***St_AliasTransform(void) { return (float ***)alias_transform; }
static float ***St_RotationMatrix(void) { return (float ***)rotation_matrix; }

static void St_SetupModel(int bodypart, void **ppbodypart, void **ppsubmodel)
{
	if (ppbodypart) *ppbodypart = NULL;
	if (ppsubmodel) *ppsubmodel = NULL;
}

static int St_CheckBBox(void) { return 1; }
static int St_GetForceFaceFlags(void) { return force_face_flags; }
static void St_SetForceFaceFlags(int flags) { force_face_flags = flags; }
static int St_IsHardware(void) { return 1; }   // OpenGL
static struct model_s *St_SetupPlayerModel(int index) { return NULL; }
static void St_Nothing(void) {}

void CL_InitStudioAPI(void)
{
	engine_studio_api_t *s = &studio_api;
	// the drawing entries do nothing (newen draws models itself)
	void (**slot)(void) = (void (**)(void))s;
	for (size_t i = 0; i < sizeof(*s) / sizeof(void *); i++)
		slot[i] = St_Nothing;
	s->Mem_Calloc = St_Mem_Calloc;
	s->Cache_Check = St_Cache_Check;
	s->LoadCacheFile = St_LoadCacheFile;
	s->Mod_ForName = St_Mod_ForName;
	s->Mod_Extradata = St_Mod_Extradata;
	s->GetModelByIndex = St_GetModelByIndex;
	s->GetCurrentEntity = St_GetCurrentEntity;
	s->PlayerInfo = St_PlayerInfo;
	s->GetPlayerState = St_GetPlayerState;
	s->GetViewEntity = St_GetViewEntity;
	s->GetTimes = St_GetTimes;
	s->GetCvar = St_GetCvar;
	s->GetViewInfo = St_GetViewInfo;
	s->GetChromeSprite = St_GetChromeSprite;
	s->GetModelCounters = St_GetModelCounters;
	s->GetAliasScale = St_GetAliasScale;
	s->StudioGetBoneTransform = St_BoneTransform;
	s->StudioGetLightTransform = St_LightTransform;
	s->StudioGetAliasTransform = St_AliasTransform;
	s->StudioGetRotationMatrix = St_RotationMatrix;
	s->StudioSetupModel = St_SetupModel;
	s->StudioCheckBBox = St_CheckBBox;
	s->SetupPlayerModel = St_SetupPlayerModel;
	s->GetForceFaceFlags = St_GetForceFaceFlags;
	s->SetForceFaceFlags = St_SetForceFaceFlags;
	s->IsHardware = St_IsHardware;

	if (!cl.HUD_GetStudioModelInterface)
		return;
	void *client_interface = NULL;   // its renderer; not used
	int ok = 0;
	CLIENT_CALL("HUD_GetStudioModelInterface", ok = cl.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION,
		&client_interface, &studio_api));
	if (!ok)
		Con_DPrintf("client.dll declined the studio interface\n");
}
