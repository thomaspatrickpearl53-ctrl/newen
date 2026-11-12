// newen - Source materials: .vmt (which texture and how to draw it) and .vtf (the texture)
#include "src_local.h"
#include <ctype.h>

static srcmaterial_t **materials;
static int materials_count, materials_cap;

// ---------------------------------------------------------------- VTF

// image formats, numbered as in the file
enum {
	VF_RGBA8888, VF_ABGR8888, VF_RGB888, VF_BGR888, VF_RGB565, VF_I8, VF_IA88, VF_P8, VF_A8,
	VF_RGB888_BLUESCREEN, VF_BGR888_BLUESCREEN, VF_ARGB8888, VF_BGRA8888, VF_DXT1, VF_DXT3, VF_DXT5,
	VF_BGRX8888, VF_BGR565, VF_BGRX5551, VF_BGRA4444, VF_DXT1_ONEBITALPHA, VF_BGRA5551, VF_UV88,
	VF_UVWQ8888, VF_RGBA16161616F, VF_RGBA16161616, VF_UVLX8888, VF_COUNT
};
static const unsigned char vf_bytes[VF_COUNT] = {
	4, 4, 3, 3, 2, 1, 2, 1, 1, 3, 3, 4, 4, 0, 0, 0, 4, 2, 2, 2, 0, 2, 2, 4, 8, 8, 4
};
static const char *const vf_names[VF_COUNT] = {
	"RGBA8888", "ABGR8888", "RGB888", "BGR888", "RGB565", "I8", "IA88", "P8", "A8", "RGB888_BLUESCREEN",
	"BGR888_BLUESCREEN", "ARGB8888", "BGRA8888", "DXT1", "DXT3", "DXT5", "BGRX8888", "BGR565", "BGRX5551",
	"BGRA4444", "DXT1_ONEBITALPHA", "BGRA5551", "UV88", "UVWQ8888", "RGBA16161616F", "RGBA16161616", "UVLX8888"
};
#define VTF_FLAG_ENVMAP 0x4000

static qboolean IsDXT(int fmt)
{
	return fmt == VF_DXT1 || fmt == VF_DXT3 || fmt == VF_DXT5 || fmt == VF_DXT1_ONEBITALPHA;
}

static size_t ImageBytes(int fmt, int w, int h)
{
	if (IsDXT(fmt))
		return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == VF_DXT1 || fmt == VF_DXT1_ONEBITALPHA ? 8 : 16);
	return (size_t)w * h * vf_bytes[fmt];
}

static void Color565(unsigned c, unsigned char *out)
{
	unsigned r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
	out[0] = (unsigned char)(r << 3 | r >> 2);
	out[1] = (unsigned char)(g << 2 | g >> 4);
	out[2] = (unsigned char)(b << 3 | b >> 2);
	out[3] = 255;
}

// one 4x4 colour block (8 bytes) into pal[4]; DXT1 alone has the 3-colour + transparent mode
static void DXTPalette(const unsigned char *b, qboolean dxt1, unsigned char pal[4][4])
{
	unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
	Color565(c0, pal[0]);
	Color565(c1, pal[1]);
	if (c0 > c1 || !dxt1) {
		for (int i = 0; i < 3; i++) {
			pal[2][i] = (unsigned char)((2 * pal[0][i] + pal[1][i] + 1) / 3);
			pal[3][i] = (unsigned char)((pal[0][i] + 2 * pal[1][i] + 1) / 3);
		}
		pal[2][3] = pal[3][3] = 255;
	} else {
		for (int i = 0; i < 3; i++) {
			pal[2][i] = (unsigned char)((pal[0][i] + pal[1][i]) / 2);
			pal[3][i] = 0;
		}
		pal[2][3] = 255;
		pal[3][3] = 0;
	}
}

static void DecodeDXT(int fmt, const unsigned char *src, int w, int h, unsigned char *out)
{
	qboolean dxt1 = fmt == VF_DXT1 || fmt == VF_DXT1_ONEBITALPHA;
	for (int by = 0; by < h; by += 4)
		for (int bx = 0; bx < w; bx += 4) {
			unsigned char alpha[16];
			memset(alpha, 255, sizeof(alpha));
			if (fmt == VF_DXT3) {
				for (int i = 0; i < 16; i++)
					alpha[i] = (unsigned char)((src[i / 2] >> (i & 1) * 4 & 15) * 17);
				src += 8;
			} else if (fmt == VF_DXT5) {
				unsigned a[8] = { src[0], src[1] };
				if (a[0] > a[1])
					for (int i = 1; i < 7; i++)
						a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
				else {
					for (int i = 1; i < 5; i++)
						a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
					a[6] = 0;
					a[7] = 255;
				}
				unsigned long long bits = 0;
				for (int i = 0; i < 6; i++)
					bits |= (unsigned long long)src[2 + i] << (8 * i);
				for (int i = 0; i < 16; i++)
					alpha[i] = (unsigned char)a[bits >> (3 * i) & 7];
				src += 8;
			}
			unsigned char pal[4][4];
			DXTPalette(src, dxt1, pal);
			unsigned idx = src[4] | src[5] << 8 | src[6] << 16 | (unsigned)src[7] << 24;
			src += 8;
			for (int i = 0; i < 16; i++) {
				int x = bx + (i & 3), y = by + (i >> 2);
				if (x >= w || y >= h)
					continue;
				const unsigned char *c = pal[idx >> (2 * i) & 3];
				unsigned char *p = out + ((size_t)y * w + x) * 4;
				p[0] = c[0];
				p[1] = c[1];
				p[2] = c[2];
				p[3] = dxt1 ? c[3] : alpha[i];
			}
		}
}

// false for a format this can't turn into RGBA
static qboolean DecodePixels(int fmt, const unsigned char *s, int w, int h, unsigned char *out)
{
	if (IsDXT(fmt)) {
		DecodeDXT(fmt, s, w, h, out);
		return true;
	}
	size_t n = (size_t)w * h;
	int step = vf_bytes[fmt];
	for (size_t i = 0; i < n; i++, s += step, out += 4) {
		unsigned v;
		out[3] = 255;
		switch (fmt) {
		case VF_RGBA8888: out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; out[3] = s[3]; break;
		case VF_ABGR8888: out[0] = s[3]; out[1] = s[2]; out[2] = s[1]; out[3] = s[0]; break;
		case VF_ARGB8888: out[0] = s[1]; out[1] = s[2]; out[2] = s[3]; out[3] = s[0]; break;
		case VF_BGRA8888: out[0] = s[2]; out[1] = s[1]; out[2] = s[0]; out[3] = s[3]; break;
		case VF_BGRX8888: out[0] = s[2]; out[1] = s[1]; out[2] = s[0]; break;
		case VF_RGB888:   out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; break;
		case VF_BGR888:   out[0] = s[2]; out[1] = s[1]; out[2] = s[0]; break;
		// pure blue means transparent
		case VF_RGB888_BLUESCREEN: out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
			if (!s[0] && !s[1] && s[2] == 255) out[2] = out[3] = 0;
			break;
		case VF_BGR888_BLUESCREEN: out[0] = s[2]; out[1] = s[1]; out[2] = s[0];
			if (!s[2] && !s[1] && s[0] == 255) out[2] = out[3] = 0;
			break;
		case VF_I8:   out[0] = out[1] = out[2] = s[0]; break;
		case VF_IA88: out[0] = out[1] = out[2] = s[0]; out[3] = s[1]; break;
		case VF_A8:   out[0] = out[1] = out[2] = 255; out[3] = s[0]; break;
		case VF_RGB565:   // red in the low bits
			v = s[0] | s[1] << 8;
			Color565((v & 31) << 11 | (v & 0x7e0) | v >> 11, out);
			break;
		case VF_BGR565:
			Color565(s[0] | s[1] << 8, out);
			break;
		case VF_BGRA5551: case VF_BGRX5551:
			v = s[0] | s[1] << 8;
			out[0] = (unsigned char)((v >> 10 & 31) * 255 / 31);
			out[1] = (unsigned char)((v >> 5 & 31) * 255 / 31);
			out[2] = (unsigned char)((v & 31) * 255 / 31);
			if (fmt == VF_BGRA5551) out[3] = v & 0x8000 ? 255 : 0;
			break;
		case VF_BGRA4444:
			v = s[0] | s[1] << 8;
			out[0] = (unsigned char)((v >> 8 & 15) * 17);
			out[1] = (unsigned char)((v >> 4 & 15) * 17);
			out[2] = (unsigned char)((v & 15) * 17);
			out[3] = (unsigned char)((v >> 12) * 17);
			break;
		case VF_UV88:   // a du/dv map: shown as a normal-ish colour
			out[0] = (unsigned char)(s[0] + 128); out[1] = (unsigned char)(s[1] + 128); out[2] = 255;
			break;
		default:
			return false;
		}
	}
	return true;
}

unsigned char *VTF_Decode(const char *label, const unsigned char *f, size_t size, int *w, int *h, qboolean *alpha)
{
	if (size < 64 || memcmp(f, "VTF", 4)) {
		Con_Warnf("%s isn't a VTF texture\n", label);
		return NULL;
	}
	unsigned major, minor, header_size, flags, fmt, lowfmt;
	memcpy(&major, f + 4, 4);
	memcpy(&minor, f + 8, 4);
	memcpy(&header_size, f + 12, 4);
	memcpy(&flags, f + 20, 4);
	memcpy(&fmt, f + 52, 4);
	memcpy(&lowfmt, f + 57, 4);
	int width = f[16] | f[17] << 8, height = f[18] | f[19] << 8;
	int frames = f[24] | f[25] << 8, first_frame = f[26] | f[27] << 8;
	int mips = f[56], low_w = f[61], low_h = f[62];
	int depth = minor >= 2 && size >= 65 ? f[63] | f[64] << 8 : 1;
	if (major != 7 || minor > 5) {
		Con_Warnf("%s: VTF version %u.%u isn't supported\n", label, major, minor);
		return NULL;
	}
	if (fmt >= VF_COUNT || (!IsDXT((int)fmt) && !vf_bytes[fmt]) || !width || !height || !mips || depth > 1) {
		Con_Warnf("%s: VTF image format %u (%dx%d, depth %d) isn't supported\n", label, fmt, width, height, depth);
		return NULL;
	}
	if (!frames)
		frames = 1;
	// environment maps hold 6 faces a frame, and before 7.5 often a 7th (sphere map)
	int faces = 1;
	if (flags & VTF_FLAG_ENVMAP)
		faces = minor < 5 && first_frame != 0xffff ? 7 : 6;

	// where the full-size images start: after the header and the small preview, or where the resource
	// list (7.3 and later) says
	size_t start = header_size;
	if (lowfmt < VF_COUNT && low_w && low_h)
		start += ImageBytes((int)lowfmt, low_w, low_h);
	if (minor >= 3 && size >= 80) {
		unsigned count;
		memcpy(&count, f + 68, 4);
		for (unsigned i = 0; i < count && 80 + (size_t)(i + 1) * 8 <= size; i++) {
			const unsigned char *r = f + 80 + i * 8;
			if (r[0] == 0x30 && !r[1] && !r[2]) {
				unsigned ofs;
				memcpy(&ofs, r + 4, 4);
				start = ofs;
			}
		}
	}
	// mips are stored smallest first; skip to the largest
	for (int m = mips - 1; m >= 1; m--) {
		int mw = width >> m, mh = height >> m;
		start += ImageBytes((int)fmt, mw ? mw : 1, mh ? mh : 1) * frames * faces;
	}
	size_t need = ImageBytes((int)fmt, width, height);
	if (start > size || need > size - start) {
		Con_Warnf("%s: VTF image data runs past the end of the file\n", label);
		return NULL;
	}
	unsigned char *rgba = Mem_Alloc((size_t)width * height * 4);
	if (!DecodePixels((int)fmt, f + start, width, height, rgba)) {
		Con_Warnf("%s: VTF image format %s isn't supported\n", label, vf_names[fmt]);
		Mem_Free(rgba);
		return NULL;
	}
	*w = width;
	*h = height;
	if (alpha) {
		*alpha = false;
		for (size_t i = 0, n = (size_t)width * height; i < n; i++)
			if (rgba[i * 4 + 3] < 255) {
				*alpha = true;
				break;
			}
	}
	return rgba;
}

// ---------------------------------------------------------------- VMT

typedef struct {
	char shader[64];
	char basetexture[128], bumpmap[128], iris[128];
	int  flags;
	qboolean shiny, base_shine;
} vmtinfo_t;

// lower case, forward slashes, no "materials/" in front, no extension
static void CleanMaterialName(const char *in, char *out, size_t size)
{
	while (*in == '/' || *in == '\\')
		in++;
	size_t n = 0;
	for (; *in && n < size - 1; in++) {
		char c = *in == '\\' ? '/' : (char)tolower((unsigned char)*in);
		if (c == '/' && n && out[n - 1] == '/')
			continue;   // models list folders with and without a trailing slash
		out[n++] = c;
	}
	out[n] = 0;
	if (!strncmp(out, "materials/", 10))
		memmove(out, out + 10, strlen(out + 10) + 1);
	char *dot = strrchr(out, '.');
	if (dot && (!strcmp(dot, ".vmt") || !strcmp(dot, ".vtf")))
		*dot = 0;
}

static void SetFlag(vmtinfo_t *v, int flag, const char *value)
{
	if (atoi(value))
		v->flags |= flag;
	else
		v->flags &= ~flag;
}

static qboolean ParseVMTFile(const char *prefix, const char *name, vmtinfo_t *v, int depth);

// the keys between { and }; p is just past the {. Returns the position past the closing }.
static const char *ParseVMTBlock(const char *prefix, const char *p, vmtinfo_t *v, int depth)
{
	while ((p = COM_Parse(p)) != NULL && com_token[0] != '}') {
		char key[128];
		Q_strncpy(key, com_token, sizeof(key));
		if ((p = COM_Parse(p)) == NULL)
			break;
		if (com_token[0] == '{' && !com_token[1]) {
			// a patch's insert/replace blocks change the included material; proxies, fallbacks and
			// the rest are skipped
			// and a ">=dx90" block holds the settings for newer graphics hardware, which is what we are
			if (!stricmp(key, "insert") || !stricmp(key, "replace") || (key[0] == '>' && strstr(key, "dx"))) {
				p = ParseVMTBlock(prefix, p, v, depth);
				continue;
			}
			for (int level = 1; level && (p = COM_Parse(p)) != NULL;)
				if (com_token[0] == '{')
					level++;
				else if (com_token[0] == '}')
					level--;
			continue;
		}
		if (!stricmp(key, "include")) {
			char inc[128];
			CleanMaterialName(com_token, inc, sizeof(inc));
			if (depth < 8)
				ParseVMTFile(prefix, inc, v, depth + 1);
		}
		else if (!stricmp(key, "$basetexture")) CleanMaterialName(com_token, v->basetexture, sizeof(v->basetexture));
		else if (!stricmp(key, "$bumpmap") || !stricmp(key, "$normalmap")) CleanMaterialName(com_token, v->bumpmap, sizeof(v->bumpmap));
		else if (!stricmp(key, "$iris")) CleanMaterialName(com_token, v->iris, sizeof(v->iris));
		else if (!stricmp(key, "$alphatest")) SetFlag(v, SMAT_ALPHATEST, com_token);
		else if (!stricmp(key, "$translucent")) SetFlag(v, SMAT_TRANSLUCENT, com_token);
		else if (!stricmp(key, "$additive")) SetFlag(v, SMAT_ADDITIVE, com_token);
		else if (!stricmp(key, "$nocull")) SetFlag(v, SMAT_NOCULL, com_token);
		else if (!stricmp(key, "$selfillum")) SetFlag(v, SMAT_SELFILLUM, com_token);
		else if (!stricmp(key, "$phong") || !stricmp(key, "$normalmapalphaenvmapmask")) v->shiny = atoi(com_token) != 0;
		else if (!stricmp(key, "$basealphaenvmapmask") || !stricmp(key, "$basemapalphaphongmask")) v->base_shine = atoi(com_token) != 0;
	}
	return p;
}

static qboolean ParseVMTFile(const char *prefix, const char *name, vmtinfo_t *v, int depth)
{
	char *text = SrcFS_LoadFile(va("%smaterials/%s.vmt", prefix, name), NULL);
	if (!text)
		return false;
	const char *p = COM_Parse(text);
	if (p) {
		// a patch keeps the shader of the file it includes
		if (stricmp(com_token, "patch"))
			Q_strncpy(v->shader, com_token, sizeof(v->shader));
		if ((p = COM_Parse(p)) != NULL && com_token[0] == '{')
			ParseVMTBlock(prefix, p, v, depth);
	}
	Mem_Free(text);
	return true;
}

// the material's picture as RGBA (Mem_Free it), or NULL. Fills info either way.
static unsigned char *LoadMaterialImage(const char *prefix, const char *clean, vmtinfo_t *info, int *w, int *h,
	qboolean *alpha, qboolean *found)
{
	memset(info, 0, sizeof(*info));
	*found = ParseVMTFile(prefix, clean, info, 0);
	if (!*found)
		return NULL;
	if (!stricmp(info->shader, "UnlitGeneric"))
		info->flags |= SMAT_UNLIT;
	// screen-bending effects (force fields, heat haze) have no picture of their own
	if (!stricmp(info->shader, "Refract") || !stricmp(info->shader, "Water"))
		info->flags |= SMAT_NODRAW;
	// eyes keep their picture in $iris
	const char *tex = info->basetexture[0] ? info->basetexture : info->iris;
	if (!*tex)
		return NULL;
	const char *path = va("%smaterials/%s.vtf", prefix, tex);
	size_t size;
	unsigned char *file = SrcFS_LoadFile(path, &size);
	if (!file) {
		Con_Warnf("Material %s%s: texture %s.vtf not found\n", prefix, clean, tex);
		return NULL;
	}
	unsigned char *rgba = VTF_Decode(path, file, size, w, h, alpha);
	Mem_Free(file);
	return rgba;
}

srcmaterial_t *SrcMat_Find(const char *prefix, const char *name, int texflags)
{
	char clean[128], key[128];
	CleanMaterialName(name, clean, sizeof(clean));
	snprintf(key, sizeof(key), "%s%s", prefix, clean);
	for (int i = 0; i < materials_count; i++)
		if (!strcmp(materials[i]->name, key))
			return materials[i];

	srcmaterial_t *m = Mem_Alloc(sizeof(*m));
	Q_strncpy(m->name, key, sizeof(m->name));
	vmtinfo_t info;
	int w = 0, h = 0;
	qboolean alpha = false, found;
	unsigned char *rgba = LoadMaterialImage(prefix, clean, &info, &w, &h, &alpha, &found);
	m->flags = info.flags;
	m->shiny = info.shiny;
	m->base_shine = info.base_shine;
	Q_strncpy(m->basename, info.basetexture[0] ? info.basetexture : info.iris, sizeof(m->basename));
	m->texflags = texflags;
	Q_strncpy(m->bumpname, info.bumpmap, sizeof(m->bumpname));
	if (rgba && info.basetexture[0] && info.iris[0]) {
		// an eye: the iris is its own picture, with a see-through surround
		const char *path = va("%smaterials/%s.vtf", prefix, info.iris);
		size_t size;
		unsigned char *file = SrcFS_LoadFile(path, &size);
		int iw, ih;
		unsigned char *iris = file ? VTF_Decode(path, file, size, &iw, &ih, NULL) : NULL;
		if (iris)
			m->iris = GL_UploadTexture(va("%s#iris", key), iris, iw, ih, texflags | TF_MIPMAP | TF_CLAMP | TF_ALPHA);
		Mem_Free(iris);
		Mem_Free(file);
	}
	if (rgba) {
		qboolean see_through = (m->flags & (SMAT_ALPHATEST | SMAT_TRANSLUCENT)) && alpha;
		// alpha that isn't transparency is something else (a shine mask, mostly): the picture is solid
		if (!see_through && !(m->flags & SMAT_ADDITIVE))
			for (size_t i = 0, n = (size_t)w * h; i < n; i++)
				rgba[i * 4 + 3] = 255;
		m->gl = GL_UploadTexture(key, rgba, w, h, texflags | TF_MIPMAP | (see_through ? TF_ALPHA : 0));
		m->width = w;
		m->height = h;
		Mem_Free(rgba);
	} else {
		m->gl = GL_MissingTexture();
		// a material that names no texture at all isn't an error: it's drawn with nothing
		if (!found || info.basetexture[0] || info.iris[0])
			m->flags |= SMAT_MISSING;
		else
			m->flags |= SMAT_NODRAW;
		m->width = m->height = 16;
	}
	if (materials_count == materials_cap) {
		materials_cap = materials_cap ? materials_cap * 2 : 256;
		materials = Mem_Realloc(materials, sizeof(*materials) * materials_cap);
	}
	materials[materials_count++] = m;
	return m;
}

void SrcMat_LoadBump(srcmaterial_t *m)
{
	if (m->bump_tried)
		return;
	m->bump_tried = true;
	if (m->flags & SMAT_MISSING)
		return;
	const char *path;
	size_t size;
	unsigned char *file;
	int w, h;
	qboolean alpha;
	if (m->base_shine && m->basename[0] && !(m->flags & (SMAT_ALPHATEST | SMAT_TRANSLUCENT))) {
		// the base texture's alpha says how much each spot shines
		path = va("%smaterials/%s.vtf", SrcFS_Prefix(m->name), m->basename);
		file = SrcFS_LoadFile(path, &size);
		unsigned char *base = file ? VTF_Decode(path, file, size, &w, &h, &alpha) : NULL;
		Mem_Free(file);
		if (base && alpha) {
			for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
				base[i * 4] = base[i * 4 + 1] = base[i * 4 + 2] = base[i * 4 + 3];
				base[i * 4 + 3] = 255;
			}
			m->specmap = GL_UploadTexture(va("%s#s", m->name), base, w, h, m->texflags | TF_MIPMAP | TF_DATA);
		}
		Mem_Free(base);
	}
	if (!m->bumpname[0])
		return;
	path = va("%smaterials/%s.vtf", SrcFS_Prefix(m->name), m->bumpname);
	file = SrcFS_LoadFile(path, &size);
	if (!file)
		return;
	unsigned char *rgba = VTF_Decode(path, file, size, &w, &h, &alpha);
	Mem_Free(file);
	if (!rgba)
		return;
	size_t n = (size_t)w * h;
	unsigned char *spec = m->shiny && alpha && !m->specmap ? Mem_Alloc(n * 4) : NULL;
	for (size_t i = 0; i < n; i++) {
		if (spec) {
			// the bump map's alpha says how much each spot shines
			spec[i * 4] = spec[i * 4 + 1] = spec[i * 4 + 2] = rgba[i * 4 + 3];
			spec[i * 4 + 3] = 255;
		}
		rgba[i * 4 + 1] = (unsigned char)(255 - rgba[i * 4 + 1]);
		rgba[i * 4 + 3] = 255;
	}
	m->normalmap = GL_UploadTexture(va("%s#n", m->name), rgba, w, h, m->texflags | TF_MIPMAP | TF_DATA);
	if (spec)
		m->specmap = GL_UploadTexture(va("%s#s", m->name), spec, w, h, m->texflags | TF_MIPMAP | TF_DATA);
	Mem_Free(spec);
	Mem_Free(rgba);
}

void SrcMat_Free(int texflags)
{
	int keep = 0;
	for (int i = 0; i < materials_count; i++) {
		srcmaterial_t *m = materials[i];
		// the checkerboard is shared and never freed
		if (m->gl == GL_MissingTexture() || (m->gl->flags & texflags))
			Mem_Free(m);
		else
			materials[keep++] = m;
	}
	materials_count = keep;
}

// srcmat_dump <material>: decodes a material's texture and writes it as a TGA into the write folder
static void SrcMatDump_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("srcmat_dump <material>, e.g. srcmat_dump hl2:models/barney/barneyface\n");
		return;
	}
	const char *prefix = SrcFS_Prefix(Cmd_Argv(1));
	char clean[128];
	CleanMaterialName(Cmd_Argv(1) + strlen(prefix), clean, sizeof(clean));
	vmtinfo_t info;
	int w = 0, h = 0;
	qboolean alpha = false, found;
	unsigned char *rgba = LoadMaterialImage(prefix, clean, &info, &w, &h, &alpha, &found);
	if (!found) {
		Con_Printf("%smaterials/%s.vmt not found\n", prefix, clean);
		return;
	}
	Con_Printf("%s%s: shader %s, texture %s, bump map %s, flags%s%s%s%s%s%s\n", prefix, clean, info.shader,
		info.basetexture[0] ? info.basetexture : info.iris[0] ? info.iris : "(none)",
		info.bumpmap[0] ? info.bumpmap : "(none)",
		info.flags & SMAT_ALPHATEST ? " alphatest" : "", info.flags & SMAT_TRANSLUCENT ? " translucent" : "",
		info.flags & SMAT_ADDITIVE ? " additive" : "", info.flags & SMAT_NOCULL ? " nocull" : "",
		info.flags & SMAT_SELFILLUM ? " selfillum" : "", info.flags & SMAT_UNLIT ? " unlit" : "");
	if (!rgba)
		return;
	const char *base = strrchr(clean, '/');
	const char *out = va("srcdump/%s.tga", base ? base + 1 : clean);
	FILE *f = FS_OpenWrite(out);
	if (f) {
		unsigned char hdr[18] = { 0 };
		hdr[2] = 2;
		hdr[12] = (unsigned char)w; hdr[13] = (unsigned char)(w >> 8);
		hdr[14] = (unsigned char)h; hdr[15] = (unsigned char)(h >> 8);
		hdr[16] = 32;
		hdr[17] = 0x28;   // top row first, 8 alpha bits
		fwrite(hdr, 1, 18, f);
		for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
			unsigned char bgra[4] = { rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4], rgba[i * 4 + 3] };
			fwrite(bgra, 1, 4, f);
		}
		fclose(f);
		Con_Printf("%dx%d%s, written to %s/%s\n", w, h, alpha ? " with alpha" : "", FS_WriteDir(), out);
	}
	Mem_Free(rgba);
}

void SrcMat_Init(void)
{
	Cmd_AddCommand("srcmat_dump", SrcMatDump_f, "srcmat_dump <material> - shows a Source material's settings and writes its texture as a TGA");
}
