// newen - Lua: drawing in the 3D world (render.*, inside the PostDrawOpaque / PostDrawTranslucent hooks),
// models Lua places itself (ClientsideModel), materials (Material) and Vector:ToScreen()
//
// render.* draws with the world's camera, after the solid things (PostDrawOpaque) or after the see-through
// things and effects (PostDrawTranslucent), in both renderers. Outside those hooks the calls are errors:
// there is no camera to draw with.
#include "lua_local.h"
#include "gl_local.h"
#include "model.h"

#define CSMODEL_MT  "CSModel"
#define MATERIAL_MT "IMaterial"

static qboolean in_draw;   // inside a draw hook: render.* may draw

// ---------------------------------------------------------------- materials

typedef struct {
	char name[128];
	gltexture_t *gl;         // pictures: loaded once, kept until quit
	qboolean sprite;         // sprites: looked up each use (the model cache is cleared at map change)
} material_t;

static material_t *materials;
static int materials_count, materials_cap;

static gltexture_t *MaterialTexture(material_t *m)
{
	if (!m->sprite)
		return m->gl;
	model_t *mod = Mod_ForName(m->name, false);
	if (!mod || mod->type != mod_sprite || !mod->sprite.numframes)
		return GL_MissingTexture();
	return mod->sprite.frames[0].frames[0].gl;
}

// Material("sprites/glow01.spr") or Material("materials/x.png"): a picture for render.* and surface.*
static int l_Material(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	int i;
	for (i = 0; i < materials_count; i++)
		if (!stricmp(materials[i].name, name))
			break;
	if (i == materials_count) {
		if (materials_count == materials_cap) {
			materials_cap = materials_cap ? materials_cap * 2 : 16;
			materials = Mem_Realloc(materials, sizeof(*materials) * materials_cap);
		}
		material_t *m = &materials[materials_count++];
		memset(m, 0, sizeof(*m));
		Q_strncpy(m->name, name, sizeof(m->name));
		size_t len = strlen(name);
		m->sprite = len > 4 && !stricmp(name + len - 4, ".spr");
		if (!m->sprite) {
			int w, h;
			unsigned char *rgba = Image_LoadRGBA(name, &w, &h);
			m->gl = rgba ? GL_UploadTexture(name, rgba, w, h, TF_MIPMAP | TF_CLAMP | TF_ALPHA) : GL_MissingTexture();
			if (!rgba)
				Con_Warnf("Material %s not found\n", name);
			Mem_Free(rgba);
		}
	}
	int *u = lua_newuserdatauv(L, sizeof(int), 0);
	*u = i;
	luaL_setmetatable(L, MATERIAL_MT);
	return 1;
}

static material_t *CheckMaterial(lua_State *L, int idx)
{
	int *u = luaL_checkudata(L, idx, MATERIAL_MT);
	return &materials[*u];
}

static int mat_GetName(lua_State *L) { lua_pushstring(L, CheckMaterial(L, 1)->name); return 1; }
static int mat_Width(lua_State *L)   { lua_pushinteger(L, MaterialTexture(CheckMaterial(L, 1))->width); return 1; }
static int mat_Height(lua_State *L)  { lua_pushinteger(L, MaterialTexture(CheckMaterial(L, 1))->height); return 1; }

// for surface.DrawTexturedRect in the host bindings
unsigned int LuaR_MaterialTexture(lua_State *L, int idx)
{
	return MaterialTexture(CheckMaterial(L, idx))->id;
}

// ---------------------------------------------------------------- clientside models

typedef struct {
	qboolean used, nodraw;
	int      serial;
	char     name[64];   // looked up each frame: the model cache is cleared at map change
	rentity_t r;
} csmodel_t;

static csmodel_t *csm;
static int csm_count, csm_cap, csm_serial;

typedef struct { int index, serial; } csref_t;

static csmodel_t *ToCS(lua_State *L, int idx, qboolean must)
{
	csref_t *u = luaL_checkudata(L, idx, CSMODEL_MT);
	if (u->index >= 0 && u->index < csm_count && csm[u->index].used && csm[u->index].serial == u->serial)
		return &csm[u->index];
	if (must)
		luaL_error(L, "the clientside model was removed");
	return NULL;
}

// ClientsideModel(model): a model only Lua knows, drawn every frame where Lua puts it, until :Remove() or
// the map ends
static int l_ClientsideModel(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	if (strlen(name) >= sizeof(csm[0].name))
		return luaL_error(L, "model name too long");
	model_t *m = Mod_ForName(name, false);
	if (!m || m->type == mod_brush) {
		lua_pushnil(L);
		return 1;
	}
	int i;
	for (i = 0; i < csm_count && csm[i].used; i++)
		;
	if (i == csm_count) {
		if (csm_count == csm_cap) {
			csm_cap = csm_cap ? csm_cap * 2 : 16;
			csm = Mem_Realloc(csm, sizeof(*csm) * csm_cap);
		}
		csm_count++;
	}
	csmodel_t *c = &csm[i];
	memset(c, 0, sizeof(*c));
	c->used = true;
	c->serial = ++csm_serial;
	Q_strncpy(c->name, name, sizeof(c->name));
	c->r.model = m;
	c->r.renderamt = 255;
	c->r.framerate = 1;
	c->r.animtime = r_refdef.time;
	c->r.scale = 1;
	if (m->type == mod_studio)
		R_StudioInitControllers(m, &c->r);
	csref_t *u = lua_newuserdatauv(L, sizeof(csref_t), 0);
	u->index = i;
	u->serial = c->serial;
	luaL_setmetatable(L, CSMODEL_MT);
	return 1;
}

static int cs_IsValid(lua_State *L)  { lua_pushboolean(L, ToCS(L, 1, false) != NULL); return 1; }
static int cs_Remove(lua_State *L)   { csmodel_t *c = ToCS(L, 1, false); if (c) c->used = false; return 0; }
static int cs_SetPos(lua_State *L)   { VectorCopy(LuaG_CheckVec(L, 2), ToCS(L, 1, true)->r.origin); return 0; }
static int cs_GetPos(lua_State *L)   { LuaG_PushVector(L, ToCS(L, 1, true)->r.origin); return 1; }
static int cs_SetAngles(lua_State *L) { VectorCopy(LuaG_CheckVec(L, 2), ToCS(L, 1, true)->r.angles); return 0; }
static int cs_GetAngles(lua_State *L) { LuaG_PushAngle(L, ToCS(L, 1, true)->r.angles); return 1; }
static int cs_SetBody(lua_State *L)  { ToCS(L, 1, true)->r.body = (int)luaL_checkinteger(L, 2); return 0; }
static int cs_SetSkin(lua_State *L)  { ToCS(L, 1, true)->r.skin = (int)luaL_checkinteger(L, 2); return 0; }
static int cs_SetNoDraw(lua_State *L) { ToCS(L, 1, true)->nodraw = lua_toboolean(L, 2); return 0; }
static int cs_GetModel(lua_State *L) { lua_pushstring(L, ToCS(L, 1, true)->name); return 1; }

static int cs_SetModel(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	const char *name = luaL_checkstring(L, 2);
	model_t *m = Mod_ForName(name, false);
	if (!m || m->type == mod_brush || strlen(name) >= sizeof(c->name))
		return luaL_error(L, "can't use model %s", name);
	Q_strncpy(c->name, name, sizeof(c->name));
	c->r.model = m;
	c->r.sequence = 0;
	return 0;
}

// :SetSequence(number or name): starts it from its beginning
static int cs_SetSequence(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	int seq = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : R_StudioLookupSequence(c->r.model, luaL_checkstring(L, 2));
	if (seq < 0)
		return luaL_error(L, "%s has no sequence %s", c->name, lua_tostring(L, 2));
	c->r.sequence = seq;
	c->r.frame = 0;
	c->r.animtime = r_refdef.time;
	return 0;
}

static int cs_GetSequence(lua_State *L) { lua_pushinteger(L, ToCS(L, 1, true)->r.sequence); return 1; }

// :SetPlaybackRate(rate): 1 = the sequence's own speed, 0 = holds the current frame
static int cs_SetPlaybackRate(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	// keep the frame where it is now, then go on at the new speed
	if (c->r.model->type == mod_studio) {
		float f = R_StudioFrame(&c->r);
		const studiohdr_t *h = Mod_StudioHeader(c->r.model);
		int frames = h && c->r.sequence >= 0 && c->r.sequence < h->numseq ?
			STUDIO_PTR(h, h->seqindex, mstudioseqdesc_t)[c->r.sequence].numframes : 1;
		c->r.frame = frames > 1 ? f * 256.0f / (float)(frames - 1) : 0;
		c->r.animtime = r_refdef.time;
	}
	c->r.framerate = (float)luaL_checknumber(L, 2);
	return 0;
}

// :SetCycle(0..1): where in the sequence
static int cs_SetCycle(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	float v = (float)luaL_checknumber(L, 2);
	c->r.frame = (v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f;
	c->r.animtime = r_refdef.time;
	return 0;
}

// :SetColor(Color(r, g, b, a)): a below 255 makes it see-through
static int cs_SetColor(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	luaL_checktype(L, 2, LUA_TTABLE);
	const char *k[4] = { "r", "g", "b", "a" };
	int v[4];
	for (int i = 0; i < 4; i++) {
		lua_getfield(L, 2, k[i]);
		v[i] = (int)luaL_optinteger(L, -1, 255);
		lua_pop(L, 1);
	}
	for (int i = 0; i < 3; i++)
		c->r.rendercolor[i] = v[i];
	c->r.renderamt = v[3];
	if (v[3] < 255 && c->r.rendermode == kRenderNormal)
		c->r.rendermode = kRenderTransTexture;
	return 0;
}

static int cs_SetRenderMode(lua_State *L) { ToCS(L, 1, true)->r.rendermode = (int)luaL_checkinteger(L, 2); return 0; }

// :DrawModel(): draws it now, inside a draw hook (for models with :SetNoDraw(true) drawn by Lua's own rules)
static int cs_DrawModel(lua_State *L)
{
	csmodel_t *c = ToCS(L, 1, true);
	if (!in_draw)
		return luaL_error(L, "DrawModel only works inside PostDrawOpaque / PostDrawTranslucent");
	c->r.model = Mod_ForName(c->name, false);
	if (!c->r.model)
		return 0;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	if (c->r.model->type == mod_studio) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		R_StudioDrawModel(&c->r);
		glDisable(GL_CULL_FACE);
	} else if (c->r.model->type == mod_sprite) {
		R_SpriteDrawModel(&c->r);
	}
	return 0;
}

void LuaR_AddModels(void)
{
	for (int i = 0; i < csm_count; i++) {
		csmodel_t *c = &csm[i];
		if (!c->used || c->nodraw)
			continue;
		model_t *m = Mod_ForName(c->name, false);
		if (!m)
			continue;
		c->r.model = m;
		rentity_t *r = R_AddFrameEntity();
		*r = c->r;
	}
}

void LuaR_ClearModels(void)
{
	csm_count = 0;   // the userdata Lua still holds see "removed" through their serial
}

// ---------------------------------------------------------------- render.*

static void NeedDraw(lua_State *L)
{
	if (!in_draw)
		luaL_error(L, "render.* only works inside the PostDrawOpaque and PostDrawTranslucent hooks");
}

// colour from a Color table (or white); returns whether it's see-through
static qboolean SetColor(lua_State *L, int idx)
{
	float c[4] = { 1, 1, 1, 1 };
	if (lua_istable(L, idx)) {
		const char *k[4] = { "r", "g", "b", "a" };
		for (int i = 0; i < 4; i++) {
			lua_getfield(L, idx, k[i]);
			c[i] = (float)luaL_optnumber(L, -1, 255) / 255.0f;
			lua_pop(L, 1);
		}
	}
	glColor4f(c[0], c[1], c[2], c[3]);
	return c[3] < 1;
}

static void BeginShape(lua_State *L, int coloridx, int depthidx, qboolean textured)
{
	NeedDraw(L);
	if (textured)
		glEnable(GL_TEXTURE_2D);
	else
		glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	// whatever the passes before left on: shapes are seen from both sides and never cut by alpha
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	if (depthidx && !lua_isnoneornil(L, depthidx) && !lua_toboolean(L, depthidx))
		glDisable(GL_DEPTH_TEST);   // drawn over everything
	else
		glEnable(GL_DEPTH_TEST);
	SetColor(L, coloridx);
}

static void EndShape(void)
{
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

// render.DrawLine(from, to, [color], [depth true]): depth false draws it over everything
static int r_DrawLine(lua_State *L)
{
	vec3_t a, b;
	VectorCopy(LuaG_CheckVec(L, 1), a);
	VectorCopy(LuaG_CheckVec(L, 2), b);
	BeginShape(L, 3, 4, false);
	glBegin(GL_LINES);
	glVertex3fv(a);
	glVertex3fv(b);
	glEnd();
	EndShape();
	return 0;
}

// the 8 corners of a box turned by angles and placed at pos
static void BoxCorners(const vec3_t pos, const vec3_t ang, const vec3_t mins, const vec3_t maxs, vec3_t out[8])
{
	vec3_t f, r, u;
	AngleVectors(ang, f, r, u);
	for (int i = 0; i < 8; i++) {
		float x = (i & 1) ? maxs[0] : mins[0], y = (i & 2) ? maxs[1] : mins[1], z = (i & 4) ? maxs[2] : mins[2];
		for (int k = 0; k < 3; k++)
			out[i][k] = pos[k] + f[k] * x - r[k] * y + u[k] * z;
	}
}

static void ReadBox(lua_State *L, vec3_t pos, vec3_t ang, vec3_t mins, vec3_t maxs)
{
	VectorCopy(LuaG_CheckVec(L, 1), pos);
	VectorCopy(LuaG_CheckVec(L, 2), ang);
	VectorCopy(LuaG_CheckVec(L, 3), mins);
	VectorCopy(LuaG_CheckVec(L, 4), maxs);
}

// render.DrawWireframeBox(pos, angles, mins, maxs, [color], [depth true])
static int r_DrawWireframeBox(lua_State *L)
{
	vec3_t pos, ang, mins, maxs, c[8];
	ReadBox(L, pos, ang, mins, maxs);
	BoxCorners(pos, ang, mins, maxs, c);
	static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
	BeginShape(L, 5, 6, false);
	glBegin(GL_LINES);
	for (int i = 0; i < 12; i++) {
		glVertex3fv(c[edges[i][0]]);
		glVertex3fv(c[edges[i][1]]);
	}
	glEnd();
	EndShape();
	return 0;
}

// render.DrawBox(pos, angles, mins, maxs, [color]): solid faces (a colour with alpha for a see-through box)
static int r_DrawBox(lua_State *L)
{
	vec3_t pos, ang, mins, maxs, c[8];
	ReadBox(L, pos, ang, mins, maxs);
	BoxCorners(pos, ang, mins, maxs, c);
	static const int faces[6][4] = { {0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5} };
	BeginShape(L, 5, 0, false);
	glBegin(GL_QUADS);
	for (int i = 0; i < 6; i++)
		for (int k = 0; k < 4; k++)
			glVertex3fv(c[faces[i][k]]);
	glEnd();
	EndShape();
	return 0;
}

static void Sphere(const vec3_t pos, float radius, int segs, qboolean wire)
{
	if (segs < 4) segs = 4;
	if (segs > 64) segs = 64;
	for (int i = 0; i < segs; i++) {
		float a1 = M_PI_F * (float)i / segs - M_PI_F / 2, a2 = M_PI_F * (float)(i + 1) / segs - M_PI_F / 2;
		glBegin(wire ? GL_LINE_STRIP : GL_QUAD_STRIP);
		for (int j = 0; j <= segs; j++) {
			float b = 2 * M_PI_F * (float)j / segs;
			glVertex3f(pos[0] + radius * cosf(a1) * cosf(b), pos[1] + radius * cosf(a1) * sinf(b), pos[2] + radius * sinf(a1));
			glVertex3f(pos[0] + radius * cosf(a2) * cosf(b), pos[1] + radius * cosf(a2) * sinf(b), pos[2] + radius * sinf(a2));
		}
		glEnd();
	}
}

// render.DrawSphere(pos, radius, [segments 12], [color]) and render.DrawWireframeSphere(..., [depth true])
static int r_DrawSphere(lua_State *L)
{
	vec3_t pos;
	VectorCopy(LuaG_CheckVec(L, 1), pos);
	BeginShape(L, 4, 0, false);
	Sphere(pos, (float)luaL_checknumber(L, 2), (int)luaL_optinteger(L, 3, 12), false);
	EndShape();
	return 0;
}

static int r_DrawWireframeSphere(lua_State *L)
{
	vec3_t pos;
	VectorCopy(LuaG_CheckVec(L, 1), pos);
	BeginShape(L, 4, 5, false);
	Sphere(pos, (float)luaL_checknumber(L, 2), (int)luaL_optinteger(L, 3, 12), true);
	EndShape();
	return 0;
}

static int current_material = -1;

// render.SetMaterial(Material(...)): the picture DrawSprite and DrawQuad use
static int r_SetMaterial(lua_State *L)
{
	int *u = luaL_checkudata(L, 1, MATERIAL_MT);
	current_material = *u;
	return 0;
}

// render.DrawSprite(pos, width, height, [color]): the current material, facing the camera, added on (glows)
static int r_DrawSprite(lua_State *L)
{
	vec3_t pos;
	VectorCopy(LuaG_CheckVec(L, 1), pos);
	float w = (float)luaL_checknumber(L, 2) * 0.5f, h = (float)luaL_checknumber(L, 3) * 0.5f;
	BeginShape(L, 4, 0, true);
	glBindTexture(GL_TEXTURE_2D, current_material >= 0 ? MaterialTexture(&materials[current_material])->id : GL_MissingTexture()->id);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	const float *r = r_refdef.right, *u = r_refdef.up;
	glBegin(GL_QUADS);
	glTexCoord2f(0, 1); glVertex3f(pos[0] - r[0] * w - u[0] * h, pos[1] - r[1] * w - u[1] * h, pos[2] - r[2] * w - u[2] * h);
	glTexCoord2f(0, 0); glVertex3f(pos[0] - r[0] * w + u[0] * h, pos[1] - r[1] * w + u[1] * h, pos[2] - r[2] * w + u[2] * h);
	glTexCoord2f(1, 0); glVertex3f(pos[0] + r[0] * w + u[0] * h, pos[1] + r[1] * w + u[1] * h, pos[2] + r[2] * w + u[2] * h);
	glTexCoord2f(1, 1); glVertex3f(pos[0] + r[0] * w - u[0] * h, pos[1] + r[1] * w - u[1] * h, pos[2] + r[2] * w - u[2] * h);
	glEnd();
	EndShape();
	return 0;
}

// render.GetViewPos(), render.GetViewAngles(): the camera of the frame being drawn
static int r_GetViewPos(lua_State *L)    { LuaG_PushVector(L, r_refdef.origin); return 1; }
static int r_GetViewAngles(lua_State *L) { LuaG_PushAngle(L, r_refdef.angles); return 1; }

// Vector:ToScreen(): { x, y, visible } where the point is on screen, from the last frame's camera
static int v_ToScreen(lua_State *L)
{
	const float *p = LuaG_CheckVec(L, 1);
	vec3_t d;
	VectorSubtract(p, r_refdef.origin, d);
	float z = DotProduct(d, r_refdef.forward);
	float tx = tanf(DEG2RAD(r_refdef.fov_x * 0.5f)), ty = tanf(DEG2RAD(r_refdef.fov_y * 0.5f));
	float x = 0, y = 0;
	if (z > 0.001f) {
		x = DotProduct(d, r_refdef.right) / z / tx;
		y = DotProduct(d, r_refdef.up) / z / ty;
	}
	lua_newtable(L);
	lua_pushnumber(L, (1 + x) * 0.5f * vid_width);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, (1 - y) * 0.5f * vid_height);
	lua_setfield(L, -2, "y");
	lua_pushboolean(L, z > 0.001f && x >= -1 && x <= 1 && y >= -1 && y <= 1);
	lua_setfield(L, -2, "visible");
	return 1;
}

// Runs a draw hook with render.* allowed; called from R_RenderView
void LuaR_DrawHook(const char *name)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	if (gl_shaders)
		qglUseProgram(0);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT | GL_LINE_BIT);
	glLineWidth(1.5f);
	in_draw = true;
	int top = lua_gettop(L);
	Lua_Hook(name, 0, 0);
	lua_settop(L, top);
	in_draw = false;
	glPopAttrib();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
}

void LuaR_Open(lua_State *L)
{
	luaL_newmetatable(L, MATERIAL_MT);
	static const luaL_Reg mmethods[] = { { "GetName", mat_GetName }, { "Width", mat_Width }, { "Height", mat_Height }, { NULL, NULL } };
	luaL_newlib(L, mmethods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, CSMODEL_MT);
	static const luaL_Reg cmethods[] = {
		{ "IsValid", cs_IsValid }, { "Remove", cs_Remove }, { "SetPos", cs_SetPos }, { "GetPos", cs_GetPos },
		{ "SetAngles", cs_SetAngles }, { "GetAngles", cs_GetAngles }, { "SetModel", cs_SetModel },
		{ "GetModel", cs_GetModel }, { "SetSequence", cs_SetSequence }, { "GetSequence", cs_GetSequence },
		{ "SetPlaybackRate", cs_SetPlaybackRate }, { "SetCycle", cs_SetCycle }, { "SetBody", cs_SetBody },
		{ "SetSkin", cs_SetSkin }, { "SetColor", cs_SetColor }, { "SetRenderMode", cs_SetRenderMode },
		{ "SetNoDraw", cs_SetNoDraw }, { "DrawModel", cs_DrawModel }, { NULL, NULL }
	};
	luaL_newlib(L, cmethods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_pushcfunction(L, l_Material);
	lua_setglobal(L, "Material");
	lua_pushcfunction(L, l_ClientsideModel);
	lua_setglobal(L, "ClientsideModel");

	static const luaL_Reg render[] = {
		{ "DrawLine", r_DrawLine }, { "DrawWireframeBox", r_DrawWireframeBox }, { "DrawBox", r_DrawBox },
		{ "DrawSphere", r_DrawSphere }, { "DrawWireframeSphere", r_DrawWireframeSphere },
		{ "SetMaterial", r_SetMaterial }, { "DrawSprite", r_DrawSprite }, { "GetViewPos", r_GetViewPos },
		{ "GetViewAngles", r_GetViewAngles }, { NULL, NULL }
	};
	luaL_newlib(L, render);
	lua_setglobal(L, "render");

	// Vector:ToScreen
	luaL_getmetatable(L, "Vector");
	lua_getfield(L, -1, "methods");
	lua_pushcfunction(L, v_ToScreen);
	lua_setfield(L, -2, "ToScreen");
	lua_pop(L, 2);
}
