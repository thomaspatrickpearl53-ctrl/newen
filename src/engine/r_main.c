// newen - view setup, frustum, map loading commands and the noclip camera
#include "bsp.h"
#include "model.h"
#include "server.h"

refdef_t r_refdef;

void R_InitWorld(void);
void R_NewMap(void);
void R_DrawWorld(void);

static mplane_t frustum[4];
static cvar_t *fov, *r_nearclip, *r_farclip;
static cvar_t *cam_speed, *sensitivity, *m_pitch, *m_yaw;
static char current_map[64];

// ---------------------------------------------------------------- frustum

qboolean R_CullBox(const vec3_t mins, const vec3_t maxs)
{
	for (int i = 0; i < 4; i++) {
		const mplane_t *p = &frustum[i];
		vec3_t pv;
		for (int j = 0; j < 3; j++)
			pv[j] = p->normal[j] >= 0 ? maxs[j] : mins[j];
		if (DotProduct(pv, p->normal) - p->dist < 0)
			return true;
	}
	return false;
}

static void SetPlane(mplane_t *p, const vec3_t a, float sa, const vec3_t b, float sb)
{
	for (int i = 0; i < 3; i++)
		p->normal[i] = a[i] * sa + b[i] * sb;
	VectorNormalize(p->normal);
	p->dist = DotProduct(r_refdef.origin, p->normal);
}

static void SetupFrustum(void)
{
	float hx = DEG2RAD(r_refdef.fov_x * 0.5f), hy = DEG2RAD(r_refdef.fov_y * 0.5f);
	SetPlane(&frustum[0], r_refdef.forward, sinf(hx), r_refdef.right, cosf(hx));    // left
	SetPlane(&frustum[1], r_refdef.forward, sinf(hx), r_refdef.right, -cosf(hx));   // right
	SetPlane(&frustum[2], r_refdef.forward, sinf(hy), r_refdef.up, -cosf(hy));      // top
	SetPlane(&frustum[3], r_refdef.forward, sinf(hy), r_refdef.up, cosf(hy));       // bottom
}

// ---------------------------------------------------------------- view

// GoldSrc's fov is horizontal for 4:3. Keep the 4:3 vertical angle and widen for wider screens (Hor+).
static void ComputeFov(void)
{
	float fx = fov->value;
	if (fx < 10) fx = 10;
	if (fx > 170) fx = 170;
	float aspect = vid_height ? (float)vid_width / vid_height : 4.0f / 3.0f;
	float tan_y = tanf(DEG2RAD(fx * 0.5f)) * 0.75f;
	r_refdef.fov_y = 2.0f * atanf(tan_y) * 180.0f / M_PI_F;
	r_refdef.fov_x = 2.0f * atanf(tan_y * aspect) * 180.0f / M_PI_F;
}

static float FarClip(void)
{
	if (r_farclip->value > 0)
		return r_farclip->value;
	// big enough for the whole map, whatever its size
	msubmodel_t *m = &world.submodels[0];
	vec3_t size;
	VectorSubtract(m->maxs, m->mins, size);
	float d = VectorLength(size) * 1.1f;
	return d < 4096 ? 4096 : d;
}

static void SetupGL(void)
{
	float znear = r_nearclip->value > 0.5f ? r_nearclip->value : 4.0f;
	float zfar = FarClip();
	float ymax = znear * tanf(DEG2RAD(r_refdef.fov_y * 0.5f));
	float xmax = znear * tanf(DEG2RAD(r_refdef.fov_x * 0.5f));

	glViewport(0, 0, vid_width, vid_height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-xmax, xmax, -ymax, ymax, znear, zfar);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatef(-90, 1, 0, 0);   // z up
	glRotatef(90, 0, 0, 1);    // x forward
	glRotatef(-r_refdef.angles[2], 1, 0, 0);
	glRotatef(-r_refdef.angles[0], 0, 1, 0);
	glRotatef(-r_refdef.angles[1], 0, 0, 1);
	glTranslatef(-r_refdef.origin[0], -r_refdef.origin[1], -r_refdef.origin[2]);
}

void R_RenderView(void)
{
	if (!world.loaded)
		return;
	r_refdef.framecount++;
	r_refdef.time = host_realtime;
	// the view: the player's eyes when there is one, otherwise the free camera
	edict_t *pl = SV_LocalPlayer();
	if (pl && CL_CalcRefdef(r_refdef.origin, r_refdef.angles)) {
		// the client DLL placed the view
	} else if (pl) {
		VectorAdd(pl->v.origin, pl->v.view_ofs, r_refdef.origin);
		VectorAdd(cl_viewangles, pl->v.punchangle, r_refdef.angles);
	} else {
		VectorCopy(cl_viewangles, r_refdef.angles);
	}
	AngleVectors(r_refdef.angles, r_refdef.forward, r_refdef.right, r_refdef.up);
	ComputeFov();
	SetupFrustum();
	SetupGL();
	if (SV_Active())
		SV_SetupRenderEntities();
	R_DrawWorld();
	R_DrawEntities();
	CL_DrawViewModel();
}

// ---------------------------------------------------------------- noclip camera

void R_UpdateCamera(void)
{
	qboolean active = world.loaded && !Con_IsVisible() && Vid_HasFocus();
	// with m_rawinput 0 the client DLL reads the cursor and recenters it, so relative mode must be off
	qboolean cursor_mode = active && CL_HasClient() && SV_LocalPlayer() && Cvar_VariableValue("m_rawinput") == 0;
	Vid_SetRelativeMouse(active && !cursor_mode);
	Vid_SetCapturedCursor(cursor_mode);
	int dx, dy;
	Vid_GetMouseDelta(&dx, &dy);
	if (!active)
		return;

	float sens = sensitivity->value;
	if (CL_HasClient() && SV_LocalPlayer())
		return;   // the client DLL turns the view (CL_CreateMove)
	cl_viewangles[1] -= dx * sens * m_yaw->value;
	cl_viewangles[0] += dy * sens * m_pitch->value;
	if (cl_viewangles[0] > 89) cl_viewangles[0] = 89;
	if (cl_viewangles[0] < -89) cl_viewangles[0] = -89;
	cl_viewangles[1] = fmodf(cl_viewangles[1], 360.0f);
	if (SV_LocalPlayer())
		return;   // the player moves through the game's movement code

	vec3_t f, r, u, move = { 0, 0, 0 };
	AngleVectors(cl_viewangles, f, r, u);
	if (Key_IsDown('w')) VectorAdd(move, f, move);
	if (Key_IsDown('s')) VectorSubtract(move, f, move);
	if (Key_IsDown('d')) VectorAdd(move, r, move);
	if (Key_IsDown('a')) VectorSubtract(move, r, move);
	if (Key_IsDown(K_SPACE)) move[2] += 1;
	if (Key_IsDown(K_CTRL)) move[2] -= 1;
	if (VectorNormalize(move) > 0) {
		float speed = cam_speed->value * (Key_IsDown(K_SHIFT) ? 3.0f : 1.0f);
		VectorMA(r_refdef.origin, (float)(speed * host_frametime), move, r_refdef.origin);
	}
}

// ---------------------------------------------------------------- commands

static void PlaceCamera(void)
{
	VectorClear(r_refdef.origin);
	VectorClear(cl_viewangles);
	static const char *starts[] = { "info_player_start", "info_player_deathmatch", "info_player_coop", NULL };
	for (int s = 0; starts[s]; s++) {
		for (int i = 0; i < world.numentities; i++) {
			bspentity_t *e = &world.entities[i];
			if (stricmp(BSP_ValueForKey(e, "classname"), starts[s]))
				continue;
			sscanf(BSP_ValueForKey(e, "origin"), "%f %f %f", &r_refdef.origin[0], &r_refdef.origin[1], &r_refdef.origin[2]);
			const char *ang = BSP_ValueForKey(e, "angles");
			if (*ang)
				sscanf(ang, "%f %f %f", &cl_viewangles[0], &cl_viewangles[1], &cl_viewangles[2]);
			else
				cl_viewangles[1] = (float)atof(BSP_ValueForKey(e, "angle"));
			cl_viewangles[2] = 0;
			VectorCopy(cl_viewangles, r_refdef.angles);
			r_refdef.origin[2] += 28;   // eye height above the player origin
			return;
		}
	}
}

static void Map_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: map <name>   (\"maps\" lists them)\n");
		return;
	}
	char name[64];
	Q_strncpy(name, Cmd_Argv(1), sizeof(name));
	size_t n = strlen(name);
	if (n > 4 && !stricmp(name + n - 4, ".bsp"))
		name[n - 4] = 0;

	Lua_RunHookString("WorldUnloaded", current_map);
	// models belong to the map they were loaded for (as in GoldSrc)
	CL_MapEnded();
	SV_Deactivate();
	R_ClearEntities();
	Mod_ClearAll();
	if (!BSP_Load(name)) {
		current_map[0] = 0;
		Con_SetForced(false);
		return;
	}
	Q_strncpy(current_map, name, sizeof(current_map));
	R_NewMap();
	CL_MapStarted();   // the client gets ready before the spawn messages reach it, as in GoldSrc
	SV_SpawnServer(name);
	PlaceCamera();
	if (Con_IsVisible())
		Con_ToggleConsole();
	Lua_RunHookString("WorldLoaded", current_map);
}

static void Disconnect_f(void)
{
	if (!world.loaded)
		return;
	Lua_RunHookString("WorldUnloaded", current_map);
	CL_MapEnded();
	SV_Deactivate();
	R_ClearEntities();
	Mod_ClearAll();
	BSP_Free();
	R_NewMap();
	current_map[0] = 0;
	if (!Con_IsVisible())
		Con_ToggleConsole();
}

static void Maps_f(void)
{
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL;
	char **list;
	int count = FS_ListFiles("maps", "*.bsp", &list);
	int shown = 0;
	for (int i = 0; i < count; i++) {
		const char *base = list[i] + 5;   // skip "maps/"
		if (filter && !Q_WildcardMatch(filter, base) && strnicmp(base, filter, strlen(filter)))
			continue;
		Con_Printf("%s\n", base);
		shown++;
	}
	Con_Printf("%d maps\n", shown);
	FS_FreeList(list, count);
}

static void Viewpos_f(void)
{
	Con_Printf("origin %.1f %.1f %.1f  angles %.1f %.1f %.1f  leaf %d\n",
		r_refdef.origin[0], r_refdef.origin[1], r_refdef.origin[2],
		r_refdef.angles[0], r_refdef.angles[1], r_refdef.angles[2],
		world.loaded ? (int)(BSP_PointInLeaf(r_refdef.origin) - world.leafs) : -1);
}

static void Setpos_f(void)
{
	if (Cmd_Argc() < 4) {
		Con_Printf("usage: setpos <x> <y> <z> [pitch yaw roll]\n");
		return;
	}
	vec3_t pos, ang;
	for (int i = 0; i < 3; i++)
		pos[i] = (float)atof(Cmd_Argv(1 + i));
	qboolean hasangles = Cmd_Argc() >= 7;
	if (hasangles)
		for (int i = 0; i < 3; i++)
			ang[i] = (float)atof(Cmd_Argv(4 + i));
	// with a game running this moves the player (its eyes end up at the position)
	SV_SetPlayerPos(pos, hasangles ? ang : NULL);
	VectorCopy(pos, r_refdef.origin);
	if (hasangles) {
		VectorCopy(ang, cl_viewangles);
		VectorCopy(ang, r_refdef.angles);
	}
}

static void Texlist_f(void)
{
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL;
	int hd = 0;
	for (int i = 0; i < world.numtextures; i++) {
		texture_t *t = world.textures[i];
		if (!t || (filter && !Q_WildcardMatch(filter, t->name) && strnicmp(t->name, filter, strlen(filter))))
			continue;
		qboolean is_hd = t->gl && t->gl->hd;
		hd += is_hd;
		if (is_hd)
			Con_Printf("%-16s %4dx%-4d  HD %dx%d\n", t->name, t->width, t->height, t->gl->width, t->gl->height);
		else
			Con_Printf("%-16s %4dx%-4d\n", t->name, t->width, t->height);
	}
	Con_Printf("%d textures, %d HD replacements (put them in textures/<name>.png)\n", world.numtextures, hd);
}

const char *R_CurrentMap(void)
{
	return current_map;
}

void R_Init(void)
{
	fov = Cvar_Get("fov", "90", FCVAR_ARCHIVE, "horizontal field of view at 4:3; wider screens see more (Hor+)");
	r_nearclip = Cvar_Get("r_nearclip", "4", 0, "near clip distance");
	r_farclip = Cvar_Get("r_farclip", "0", 0, "far clip distance, 0 = fit the map");
	cam_speed = Cvar_Get("cam_speed", "400", FCVAR_ARCHIVE, "noclip camera speed (Shift = 3x)");
	sensitivity = Cvar_Get("sensitivity", "3", FCVAR_ARCHIVE, "mouse sensitivity");
	m_pitch = Cvar_Get("m_pitch", "0.022", FCVAR_ARCHIVE, "mouse pitch scale (negative inverts)");
	m_yaw = Cvar_Get("m_yaw", "0.022", FCVAR_ARCHIVE, "mouse yaw scale");
	Cmd_AddCommand("map", Map_f, "map <name> - loads a map");
	Cmd_AddCommand("maps", Maps_f, "lists maps, optionally filtered");
	Cmd_AddCommand("disconnect", Disconnect_f, "unloads the map");
	Cmd_AddCommand("viewpos", Viewpos_f, "prints the camera position");
	Cmd_AddCommand("setpos", Setpos_f, "moves the camera");
	Cmd_AddCommand("texlist", Texlist_f, "lists the map's textures and which have HD replacements");
	GL_LoadExtensions();
	Image_Init();
	R_InitWorld();
	Mod_Init();
	R_StudioInit();
	R_InitEntities();
}

void R_Shutdown(void)
{
	SV_Deactivate();
	R_ClearEntities();
	Mod_ClearAll();
	BSP_Free();
	Image_Shutdown();
}
