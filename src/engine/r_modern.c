// newen - the optional modern renderer (r_renderer modern): per-pixel dynamic lights with normal and specular
// maps, added on top of the classic lightmapped world. Classic stays the default and draws exactly as before.
// See docs/RENDERER_PLAN.md.
#include "bsp.h"

qboolean r_modern;
static cvar_t *r_renderer, *r_modern_forcefail, *r_dlightscale, *r_gloss, *r_specscale;
static char fallback[160];   // why modern was asked for but classic is drawing

static GLuint light_prog;
static GLint u_lightorg, u_vieworg, u_lightcolor, u_radius, u_gloss, u_specscale;
static gltexture_t *flat_normal, *no_specular;
static int materials_n, materials_s;

static qboolean ModernInit(void)
{
	if (!gl_shaders) {
		snprintf(fallback, sizeof(fallback), "%s", gl_shaders_missing);
		return false;
	}
	// compiled again at every map load, so shader edits show up with "reload"
	R_FreeProgram(&light_prog);
	light_prog = R_LoadProgram("modern/light");
	if (!light_prog) {
		snprintf(fallback, sizeof(fallback), "the light shader failed (the error is above)");
		return false;
	}
	u_lightorg = qglGetUniformLocation(light_prog, "u_lightorg");
	u_vieworg = qglGetUniformLocation(light_prog, "u_vieworg");
	u_lightcolor = qglGetUniformLocation(light_prog, "u_lightcolor");
	u_radius = qglGetUniformLocation(light_prog, "u_radius");
	u_gloss = qglGetUniformLocation(light_prog, "u_gloss");
	u_specscale = qglGetUniformLocation(light_prog, "u_specscale");
	qglUseProgram(light_prog);
	qglUniform1i(qglGetUniformLocation(light_prog, "u_diffuse"), 0);
	qglUniform1i(qglGetUniformLocation(light_prog, "u_normalmap"), 1);
	qglUniform1i(qglGetUniformLocation(light_prog, "u_specmap"), 2);
	qglUseProgram(0);

	if (!flat_normal) {
		static const unsigned char up[4] = { 128, 128, 255, 255 }, black[4] = { 0, 0, 0, 255 };
		flat_normal = GL_UploadTexture("*flatnormal", up, 1, 1, TF_DATA | TF_NEAREST);
		no_specular = GL_UploadTexture("*nospecular", black, 1, 1, TF_DATA | TF_NEAREST);
	}
	return true;
}

// Chosen when a map loads, so the lightmaps and material textures belong to one renderer for the whole map
void R_SelectRenderer(void)
{
	const char *want = r_renderer->string;
	qboolean modern;
	if (!stricmp(want, "classic"))
		modern = false;
	else if (!stricmp(want, "modern"))
		modern = true;
	else {
		Con_Warnf("r_renderer \"%s\" isn't classic or modern; staying %s\n", want, r_modern ? "modern" : "classic");
		modern = r_modern;
	}
	fallback[0] = 0;
	if (modern && r_modern_forcefail->value) {
		snprintf(fallback, sizeof(fallback), "r_modern_forcefail is set");
		modern = false;
	} else if (modern && !ModernInit()) {
		modern = false;
	}
	r_modern = modern;
	materials_n = materials_s = 0;
	if (fallback[0])
		Con_Warnf("Renderer: classic, because modern isn't available: %s\n", fallback);
	else
		Con_Printf("Renderer: %s\n", r_modern ? "modern" : "classic");
}

static void RendererInfo_f(void)
{
	Con_Printf("requested: %s\ndrawing: %s%s%s\n", r_renderer->string, r_modern ? "modern" : "classic",
		fallback[0] ? " - modern isn't available: " : "", fallback);
	Con_Printf("shaders: %s\n", gl_shaders ? "available" : gl_shaders_missing);
	if (r_modern)
		Con_Printf("this map: %d normal maps, %d specular maps\n", materials_n, materials_s);
	Con_Printf("r_renderer applies at the next map load (\"reload\")\n");
}

void R_ModernInitCvars(void)
{
	r_renderer = Cvar_Get("r_renderer", "classic", FCVAR_ARCHIVE,
		"classic, or modern for per-pixel dynamic lights and normal/specular maps (applies at the next map load)");
	r_modern_forcefail = Cvar_Get("r_modern_forcefail", "0", 0, "debug: pretend the modern renderer can't start");
	r_dlightscale = Cvar_Get("r_dlightscale", "1", FCVAR_ARCHIVE, "modern: brightness of dynamic lights");
	r_gloss = Cvar_Get("r_gloss", "32", FCVAR_ARCHIVE, "modern: specular exponent (higher is a smaller, sharper shine)");
	r_specscale = Cvar_Get("r_specscale", "1", FCVAR_ARCHIVE, "modern: strength of specular maps");
	Cmd_AddCommand("r_rendererinfo", RendererInfo_f, "shows which renderer is drawing and why");
}

// ---------------------------------------------------------------- materials

static gltexture_t *LoadAux(const texture_t *tx, const char *suffix)
{
	char name[64];
	snprintf(name, sizeof(name), "%s%s", tx->name, suffix);
	int w, h;
	unsigned char *rgba = Image_LoadAny("textures", name, &w, &h);
	if (!rgba)
		return NULL;
	gltexture_t *t = GL_UploadTexture(name, rgba, w, h, TF_MIPMAP | TF_WORLD | TF_DATA);
	Mem_Free(rgba);
	return t;
}

// textures/<name>_n (tangent-space normals, +Y up the image) and textures/<name>_s (specular strength)
void R_LoadMaterial(texture_t *tx)
{
	if (!r_modern)
		return;
	tx->normalmap = LoadAux(tx, "_n");
	tx->specmap = LoadAux(tx, "_s");
	materials_n += tx->normalmap != NULL;
	materials_s += tx->specmap != NULL;
}

// ---------------------------------------------------------------- light passes

// The surfaces are already drawn with the lightmap (which has no dynamic light in modern);
// each light adds its own pass, depth-tested against them.
void R_ModernBeginLights(const texture_t *tx, const float *vieworg)
{
	// normals and tangents come from the second stream; positions and st stay on the world's buffer
	qglBindBuffer(GL_ARRAY_BUFFER, world.modern_vbo);
	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, sizeof(modernvert_t), (void *)offsetof(modernvert_t, normal));
	qglClientActiveTexture(GL_TEXTURE2);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(4, GL_FLOAT, sizeof(modernvert_t), (void *)offsetof(modernvert_t, tangent));
	qglClientActiveTexture(GL_TEXTURE0);
	qglBindBuffer(GL_ARRAY_BUFFER, world.vbo);

	qglActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tx->normalmap ? tx->normalmap->id : flat_normal->id);
	qglActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, tx->specmap ? tx->specmap->id : no_specular->id);
	qglActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tx->gl ? tx->gl->id : GL_MissingTexture()->id);

	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	qglUseProgram(light_prog);
	qglUniform3f(u_vieworg, vieworg[0], vieworg[1], vieworg[2]);
	qglUniform1f(u_gloss, r_gloss->value > 1 ? r_gloss->value : 1);
	qglUniform1f(u_specscale, r_specscale->value);
}

void R_ModernSetLight(int i)
{
	vec3_t org, color;
	float radius;
	R_GetDlight(i, org, &radius, color);
	float scale = r_dlightscale->value;
	qglUniform3f(u_lightorg, org[0], org[1], org[2]);
	qglUniform1f(u_radius, radius);
	qglUniform3f(u_lightcolor, color[0] * scale, color[1] * scale, color[2] * scale);
}

// Back to the fixed-function state the classic drawing expects (the caller rebinds its lightmap)
void R_ModernEndLights(void)
{
	qglUseProgram(0);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	qglActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	qglActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_NORMAL_ARRAY);
	qglClientActiveTexture(GL_TEXTURE2);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	qglClientActiveTexture(GL_TEXTURE0);
}
