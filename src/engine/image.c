// newen - image files and the GL texture list
#include "gl_local.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_MALLOC(sz)        Mem_Alloc(sz)
#define STBI_REALLOC(p, sz)    Mem_Realloc(p, sz)
#define STBI_FREE(p)           Mem_Free(p)
#pragma warning(push, 0)
#include "stb_image.h"
#pragma warning(pop)

qglActiveTexture_t qglActiveTexture;
qglActiveTexture_t qglClientActiveTexture;
qglGenBuffers_t qglGenBuffers;
qglGenBuffers_t qglDeleteBuffers;
qglBindBuffer_t qglBindBuffer;
qglBufferData_t qglBufferData;
float gl_max_anisotropy = 1.0f;
int   gl_max_texture_size = 2048;

unsigned char texgammatable[256];

static gltexture_t **textures;
static int  textures_count, textures_cap;
static gltexture_t *missing;
static cvar_t *r_texturemode, *r_anisotropy, *r_texgamma;

void GL_LoadExtensions(void)
{
	qglActiveTexture = (qglActiveTexture_t)SDL_GL_GetProcAddress("glActiveTexture");
	qglClientActiveTexture = (qglActiveTexture_t)SDL_GL_GetProcAddress("glClientActiveTexture");
	qglGenBuffers = (qglGenBuffers_t)SDL_GL_GetProcAddress("glGenBuffers");
	qglDeleteBuffers = (qglGenBuffers_t)SDL_GL_GetProcAddress("glDeleteBuffers");
	qglBindBuffer = (qglBindBuffer_t)SDL_GL_GetProcAddress("glBindBuffer");
	qglBufferData = (qglBufferData_t)SDL_GL_GetProcAddress("glBufferData");
	if (!qglActiveTexture || !qglClientActiveTexture || !qglGenBuffers || !qglBindBuffer || !qglBufferData)
		Sys_Error("Your OpenGL driver is too old (needs OpenGL 1.5)");
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max_texture_size);
	if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic"))
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl_max_anisotropy);
	Con_Printf("Max texture size %d, anisotropy %gx\n", gl_max_texture_size, gl_max_anisotropy);
}

static void BuildGammaTable(void)
{
	// GoldSrc brightens textures with texgamma/gamma (2.0/2.5); the exponent is a cvar here
	float e = r_texgamma->value > 0.1f ? r_texgamma->value : 1.0f;
	for (int i = 0; i < 256; i++) {
		int v = (int)(255.0 * pow(i / 255.0, e) + 0.5);
		texgammatable[i] = (unsigned char)(v > 255 ? 255 : v);
	}
}

static void TextureMode_Changed(void)
{
	GL_ApplyTextureFilters();
}

void Image_Init(void)
{
	r_texturemode = Cvar_Get("r_texturemode", "trilinear", FCVAR_ARCHIVE,
		"texture filtering: trilinear, bilinear, or nearest for the crunchy pixel look");
	r_anisotropy = Cvar_Get("r_anisotropy", "8", FCVAR_ARCHIVE, "anisotropic filtering level (1 = off)");
	r_texgamma = Cvar_Get("r_texgamma", "0.8", FCVAR_ARCHIVE, "texture brightness curve (lower is brighter; applies on map load)");
	Cmd_AddCommand("r_applyfilters", TextureMode_Changed, "applies r_texturemode and r_anisotropy now");
	BuildGammaTable();

	// magenta/black checkerboard for textures that can't be found
	unsigned char px[16 * 16 * 4];
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			unsigned char *p = px + (y * 16 + x) * 4;
			qboolean on = ((x >> 3) ^ (y >> 3)) & 1;
			p[0] = on ? 255 : 0;
			p[1] = 0;
			p[2] = on ? 255 : 0;
			p[3] = 255;
		}
	missing = GL_UploadTexture("*missing", px, 16, 16, TF_MIPMAP | TF_NEAREST);
}

void Image_Shutdown(void)
{
	GL_FreeTextures(~0);
	Mem_Free(textures);
	textures = NULL;
	textures_cap = 0;
	missing = NULL;
}

gltexture_t *GL_MissingTexture(void)
{
	return missing;
}

unsigned char *Image_LoadRGBA(const char *path, int *w, int *h)
{
	size_t len;
	unsigned char *file = FS_LoadFile(path, &len);
	if (!file)
		return NULL;
	int comp;
	unsigned char *rgba = stbi_load_from_memory(file, (int)len, w, h, &comp, 4);
	Mem_Free(file);
	if (!rgba)
		Con_Warnf("Couldn't decode %s: %s\n", path, stbi_failure_reason());
	return rgba;
}

unsigned char *Image_LoadAny(const char *dir, const char *name, int *w, int *h)
{
	static const char *exts[] = { "png", "tga", "jpg", "bmp" };
	for (int i = 0; i < 4; i++) {
		const char *path = va("%s/%s.%s", dir, name, exts[i]);
		if (FS_FileExists(path))
			return Image_LoadRGBA(path, w, h);
	}
	return NULL;
}

static void SetFilter(gltexture_t *t)
{
	glBindTexture(GL_TEXTURE_2D, t->id);
	GLint minf, magf;
	const char *mode = r_texturemode->string;
	if ((t->flags & TF_NEAREST) || !stricmp(mode, "nearest")) {
		minf = (t->flags & TF_MIPMAP) ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
		magf = GL_NEAREST;
	} else if (!stricmp(mode, "bilinear")) {
		minf = (t->flags & TF_MIPMAP) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
		magf = GL_LINEAR;
	} else {
		minf = (t->flags & TF_MIPMAP) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
		magf = GL_LINEAR;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
	if (gl_max_anisotropy > 1.0f && (t->flags & TF_MIPMAP)) {
		float a = r_anisotropy->value;
		if (a < 1) a = 1;
		if (a > gl_max_anisotropy) a = gl_max_anisotropy;
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
	}
}

void GL_ApplyTextureFilters(void)
{
	for (int i = 0; i < textures_count; i++)
		SetFilter(textures[i]);
}

gltexture_t *GL_UploadTexture(const char *name, const unsigned char *rgba, int w, int h, int flags)
{
	gltexture_t *t = Mem_Alloc(sizeof(*t));
	Q_strncpy(t->name, name, sizeof(t->name));
	t->width = w;
	t->height = h;
	t->flags = flags;

	const unsigned char *src = rgba;
	unsigned char *tmp = NULL;
	if (flags & (TF_WORLD | TF_MODEL)) {
		size_t n = (size_t)w * h;
		tmp = Mem_Alloc(n * 4);
		for (size_t i = 0; i < n; i++) {
			tmp[i * 4 + 0] = texgammatable[rgba[i * 4 + 0]];
			tmp[i * 4 + 1] = texgammatable[rgba[i * 4 + 1]];
			tmp[i * 4 + 2] = texgammatable[rgba[i * 4 + 2]];
			tmp[i * 4 + 3] = rgba[i * 4 + 3];
		}
		src = tmp;
	}
	if (w > gl_max_texture_size || h > gl_max_texture_size)
		Con_Warnf("%s is %dx%d, larger than this GPU allows (%d)\n", name, w, h, gl_max_texture_size);

	glGenTextures(1, &t->id);
	glBindTexture(GL_TEXTURE_2D, t->id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (flags & TF_MIPMAP)
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	GLint wrap = (flags & TF_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	SetFilter(t);
	Mem_Free(tmp);

	if (textures_count == textures_cap) {
		textures_cap = textures_cap ? textures_cap * 2 : 256;
		textures = Mem_Realloc(textures, sizeof(*textures) * textures_cap);
	}
	textures[textures_count++] = t;
	return t;
}

void GL_FreeTextures(int flagmask)
{
	int out = 0;
	for (int i = 0; i < textures_count; i++) {
		gltexture_t *t = textures[i];
		if (t->flags & flagmask) {
			glDeleteTextures(1, &t->id);
			Mem_Free(t);
		} else {
			textures[out++] = t;
		}
	}
	textures_count = out;
	// rebuild the gamma table so r_texgamma changes apply to the next map
	BuildGammaTable();
}
