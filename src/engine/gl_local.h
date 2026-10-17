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

// ---------------------------------------------------------------- textures (image.c)
#define TF_MIPMAP   (1<<0)
#define TF_CLAMP    (1<<1)
#define TF_NEAREST  (1<<2)   // never filtered (UI, lightmaps use their own)
#define TF_WORLD    (1<<3)   // freed when the map changes
#define TF_ALPHA    (1<<4)   // has transparent pixels
#define TF_MODEL    (1<<5)   // studio model or sprite skin; freed with the model cache

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
