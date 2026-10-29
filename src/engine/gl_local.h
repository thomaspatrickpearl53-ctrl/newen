// newen - renderer-internal declarations
#pragma once
#include <SDL.h>
#include <SDL_opengl.h>
#include "common.h"
#include "mathlib.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// GL 1.3+ entry points (opengl32.dll only exports 1.1)
typedef void (APIENTRY *qglActiveTexture_t)(GLenum texture);
extern qglActiveTexture_t qglActiveTexture;
extern qglActiveTexture_t qglClientActiveTexture;
typedef void (APIENTRY *qglGenBuffers_t)(GLsizei n, GLuint *buffers);
typedef void (APIENTRY *qglBindBuffer_t)(GLenum target, GLuint buffer);
typedef void (APIENTRY *qglBufferData_t)(GLenum target, ptrdiff_t size, const void *data, GLenum usage);
extern qglGenBuffers_t qglGenBuffers;
extern qglGenBuffers_t qglDeleteBuffers;
extern qglBindBuffer_t qglBindBuffer;
extern qglBufferData_t qglBufferData;
void GL_LoadExtensions(void);
extern float gl_max_anisotropy;
extern int   gl_max_texture_size;

// GLSL entry points, loaded when the driver has them (only the modern renderer uses them)
extern PFNGLCREATESHADERPROC      qglCreateShader;
extern PFNGLSHADERSOURCEPROC      qglShaderSource;
extern PFNGLCOMPILESHADERPROC     qglCompileShader;
extern PFNGLGETSHADERIVPROC       qglGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC  qglGetShaderInfoLog;
extern PFNGLDELETESHADERPROC      qglDeleteShader;
extern PFNGLCREATEPROGRAMPROC     qglCreateProgram;
extern PFNGLATTACHSHADERPROC      qglAttachShader;
extern PFNGLLINKPROGRAMPROC       qglLinkProgram;
extern PFNGLGETPROGRAMIVPROC      qglGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC qglGetProgramInfoLog;
extern PFNGLDELETEPROGRAMPROC     qglDeleteProgram;
extern PFNGLUSEPROGRAMPROC        qglUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC qglGetUniformLocation;
extern PFNGLUNIFORM1IPROC         qglUniform1i;
extern PFNGLUNIFORM1FPROC         qglUniform1f;
extern PFNGLUNIFORM3FPROC         qglUniform3f;
extern qboolean gl_shaders;            // everything the modern renderer needs is there
extern char     gl_shaders_missing[128];   // why not, when it isn't

// ---------------------------------------------------------------- shaders (r_shader.c)
GLuint R_LoadProgram(const char *name);   // shaders/<name>.vert + .frag; 0 on failure (the log says why)
void   R_FreeProgram(GLuint *prog);

// ---------------------------------------------------------------- modern renderer (r_modern.c)
extern qboolean r_modern;   // the loaded map is drawn by the modern renderer (chosen at map load)
void R_ModernInitCvars(void);
void R_SelectRenderer(void);

// ---------------------------------------------------------------- textures (image.c)
#define TF_MIPMAP   (1<<0)
#define TF_CLAMP    (1<<1)
#define TF_NEAREST  (1<<2)   // never filtered (UI, lightmaps use their own)
#define TF_WORLD    (1<<3)   // freed when the map changes
#define TF_ALPHA    (1<<4)   // has transparent pixels
#define TF_MODEL    (1<<5)   // studio model or sprite skin; freed with the model cache
#define TF_DATA     (1<<6)   // not a colour (normal/specular map): no texture gamma

typedef struct gltexture_s {
	char   name[64];
	GLuint id;
	int    width, height;   // size of the uploaded image
	int    flags;
	qboolean hd;            // came from an HD replacement file
} gltexture_t;

void         Image_Init(void);
void         Image_Shutdown(void);
// Loads PNG/TGA/JPG/BMP through the filesystem. Returns RGBA (Mem_Free it) or NULL.
unsigned char *Image_LoadRGBA(const char *path, int *w, int *h);
// Tries name.png/.tga/.jpg/.bmp under dir; returns RGBA or NULL
unsigned char *Image_LoadAny(const char *dir, const char *name, int *w, int *h);
gltexture_t *GL_UploadTexture(const char *name, const unsigned char *rgba, int w, int h, int flags);
void         GL_FreeTextures(int flagmask);
void         GL_ApplyTextureFilters(void);
gltexture_t *GL_MissingTexture(void);
extern unsigned char texgammatable[256];

// ---------------------------------------------------------------- WADs (wad.c)
typedef struct {
	char     name[16];
	unsigned width, height;
	unsigned offsets[4];
} miptex_t;

void      Wad_LoadList(const char *wadlist);   // worldspawn "wad" key
void      Wad_FreeAll(void);
miptex_t *Wad_FindMiptex(const char *name, size_t *size);
void     *Wad_Open(const char *name);                  // kept apart from the map's WADs (decals.wad)
miptex_t *Wad_FindIn(void *wad, const char *name, size_t *size);
// Converts a miptex (with its palette) to RGBA; '{' textures get transparency
unsigned char *Miptex_ToRGBA(const miptex_t *mt, size_t size);

// ---------------------------------------------------------------- view (r_main.c)
typedef struct {
	vec3_t origin, angles;
	vec3_t forward, right, up;
	float  fov_x, fov_y;
	int    framecount;
	double time;
} refdef_t;
extern refdef_t r_refdef;
qboolean R_CullBox(const vec3_t mins, const vec3_t maxs);

// cl_main.c: the client DLL's view and the weapon in your hands
qboolean CL_CalcRefdef(vec3_t vieworg, vec3_t viewangles);
void     CL_DrawViewModel(void);
void     CL_CaptureViewMatrices(void);
void     CL_AddEntities(void);
void     CL_DrawEffects(void);
