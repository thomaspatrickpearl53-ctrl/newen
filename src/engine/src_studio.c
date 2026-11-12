// newen - Source models: .mdl (versions 44-48) with their .vvd (vertices) and .vtx (triangles) files
//
// The three files carry the same checksum and are read together. Everything is range-checked against the
// file sizes; a model that fails a check is refused whole. Only the most detailed level (LOD 0) is used.
//
// The game and client DLLs read GoldSrc model files directly, so each Source model also gets a stand-in
// GoldSrc header (BuildStandIn) that describes it in the layout they expect.
#include "src_local.h"
#include <ctype.h>

// ---------------------------------------------------------------- file layouts (byte offsets)

enum {
	// .mdl header
	MDL_CHECKSUM = 8, MDL_NAME = 12, MDL_LENGTH = 76, MDL_HULL_MIN = 104, MDL_HULL_MAX = 116,
	MDL_VIEW_MIN = 128, MDL_VIEW_MAX = 140, MDL_FLAGS = 152, MDL_NUMBONES = 156, MDL_BONEINDEX = 160,
	MDL_NUMTEXTURES = 204, MDL_TEXTUREINDEX = 208, MDL_NUMCDTEXTURES = 212, MDL_CDTEXTUREINDEX = 216,
	MDL_NUMSKINREF = 220, MDL_NUMSKINFAMILIES = 224, MDL_SKININDEX = 228, MDL_NUMBODYPARTS = 232,
	MDL_BODYPARTINDEX = 236, MDL_HEADER_SIZE = 408,

	MDL_TEXTURE_SIZE = 64,       // name offset first
	MDL_BODYPART_SIZE = 16,      // name offset, model count, base, models offset
	MDL_MODEL_SIZE = 148,
	MDL_MODEL_NUMMESHES = 72, MDL_MODEL_MESHINDEX = 76, MDL_MODEL_NUMVERTS = 80, MDL_MODEL_VERTINDEX = 84,
	MDL_MESH_SIZE = 116,
	MDL_MESH_MATERIAL = 0, MDL_MESH_NUMVERTS = 8, MDL_MESH_VERTOFFSET = 12, MDL_MESH_MATERIALTYPE = 24,
	MDL_MESH_MATERIALPARAM = 28,   // type 1 = an eyeball, param = which
	MDL_MODEL_NUMEYEBALLS = 100, MDL_MODEL_EYEBALLINDEX = 104,
	MDL_EYEBALL_SIZE = 172,
	MDL_EYEBALL_BONE = 4, MDL_EYEBALL_ORG = 8, MDL_EYEBALL_ZOFFSET = 20, MDL_EYEBALL_RADIUS = 24,
	MDL_EYEBALL_UP = 28, MDL_EYEBALL_FORWARD = 40, MDL_EYEBALL_IRISSCALE = 60,

	// .vvd header
	VVD_VERSION = 4, VVD_CHECKSUM = 8, VVD_NUMLODS = 12, VVD_LODVERTS = 16, VVD_NUMFIXUPS = 48,
	VVD_FIXUPSTART = 52, VVD_VERTEXSTART = 56, VVD_TANGENTSTART = 60, VVD_HEADER_SIZE = 64,
	VVD_FIXUP_SIZE = 12,         // lod, first vertex, count

	// .vtx: a tree of body parts > models > LODs > meshes > strip groups > strips, each a count and an
	// offset from the start of its own entry. The file is byte-packed, hence the odd sizes.
	VTX_VERSION = 0, VTX_CHECKSUM = 16, VTX_NUMBODYPARTS = 28, VTX_BODYPARTOFFSET = 32, VTX_HEADER_SIZE = 36,
	VTX_BODYPART_SIZE = 8, VTX_MODEL_SIZE = 8, VTX_LOD_SIZE = 12, VTX_MESH_SIZE = 9,
	VTX_GROUP_SIZE = 25,         // verts, indices, strips (count + offset each), flags byte
	VTX_VERT_SIZE = 9, VTX_VERT_MESHVERT = 4,
	VTX_STRIP_SIZE = 27,         // index count, first index, vert count, first vert, bones, flags byte at 18
	VTX_STRIP_FLAGS = 18,
};
#define VTX_STRIP_IS_TRISTRIP 2

static int RdInt(const unsigned char *p)
{
	int v;
	memcpy(&v, p, 4);
	return v;
}

static unsigned RdU16(const unsigned char *p)
{
	return p[0] | p[1] << 8;
}

static float HalfToFloat(unsigned h)
{
	int e = h >> 10 & 31, mant = h & 1023;
	float v;
	if (e == 0)
		v = ldexpf((float)mant, -24);
	else if (e == 31)
		v = mant ? 0 : 65504.0f;
	else
		v = ldexpf((float)(mant + 1024), e - 25);
	return h & 0x8000 ? -v : v;
}

// true if count entries of elemsize at ofs lie inside a file of this size
static qboolean Has(size_t size, long long ofs, long long count, size_t elemsize)
{
	if (count == 0)
		return true;
	return ofs >= 0 && count > 0 && (unsigned long long)ofs <= size &&
		(unsigned long long)count <= (size - (size_t)ofs) / elemsize;
}

// a NUL-terminated string at ofs, or NULL if it runs off the file
static const char *FileString(const unsigned char *data, size_t size, long long ofs)
{
	if (ofs < 0 || (unsigned long long)ofs >= size || !memchr(data + ofs, 0, size - (size_t)ofs))
		return NULL;
	return (const char *)data + ofs;
}

// ---------------------------------------------------------------- vertices (.vvd)

static qboolean LoadVerts(const char *name, srcmodel_t *s, int checksum)
{
	size_t size;
	unsigned char *f = SrcFS_LoadFile(name, &size);
	if (!f) {
		Con_Warnf("%s not found\n", name);
		return false;
	}
	qboolean ok = false;
	if (size < VVD_HEADER_SIZE || memcmp(f, "IDSV", 4) || RdInt(f + VVD_VERSION) != 4) {
		Con_Warnf("%s isn't a version 4 vertex file\n", name);
		goto done;
	}
	if (RdInt(f + VVD_CHECKSUM) != checksum) {
		Con_Warnf("%s doesn't belong to its .mdl (different checksum)\n", name);
		goto done;
	}
	int total = RdInt(f + VVD_LODVERTS);   // LOD 0's count
	int numfixups = RdInt(f + VVD_NUMFIXUPS), fixups = RdInt(f + VVD_FIXUPSTART), vstart = RdInt(f + VVD_VERTEXSTART);
	if (total < 0 || !Has(size, fixups, numfixups, VVD_FIXUP_SIZE) || vstart < 0 || (size_t)vstart > size)
		goto damaged;
	size_t infile = (size - (size_t)vstart) / sizeof(srcvert_t);
	s->verts = Mem_Alloc(sizeof(srcvert_t) * (total ? total : 1));
	// tangents (16 bytes a vertex) follow the vertices, in the same order; they're optional here
	int tstart = RdInt(f + VVD_TANGENTSTART);
	const unsigned char *tans = NULL;
	if (tstart > vstart && (size_t)tstart <= size && (size - (size_t)tstart) / 16 >= ((size_t)tstart - vstart) / sizeof(srcvert_t)) {
		tans = f + tstart;
		if (((size_t)tstart - vstart) / sizeof(srcvert_t) < infile)
			infile = ((size_t)tstart - vstart) / sizeof(srcvert_t);
		s->tangents = Mem_Alloc(16 * (size_t)(total ? total : 1));
	}
	if (!numfixups) {
		if ((size_t)total > infile)
			goto damaged;
		memcpy(s->verts, f + vstart, sizeof(srcvert_t) * total);
		if (tans)
			memcpy(s->tangents, tans, 16 * (size_t)total);
	} else {
		// the file keeps vertices sorted for its lower detail levels; the fixups list the runs that
		// make up each level, in the order the triangles expect
		int n = 0;
		for (int i = 0; i < numfixups; i++) {
			const unsigned char *fx = f + fixups + i * VVD_FIXUP_SIZE;
			int lod = RdInt(fx), first = RdInt(fx + 4), count = RdInt(fx + 8);
			if (lod < 0)
				continue;
			if (first < 0 || count < 0 || (size_t)first > infile || (size_t)count > infile - first || count > total - n)
				goto damaged;
			memcpy(s->verts + n, f + vstart + (size_t)first * sizeof(srcvert_t), sizeof(srcvert_t) * count);
			if (tans)
				memcpy(s->tangents + n, tans + (size_t)first * 16, 16 * (size_t)count);
			n += count;
		}
		if (n != total)
			goto damaged;
	}
	s->numverts = total;
	ok = true;
	goto done;
damaged:
	Con_Warnf("%s is damaged\n", name);
done:
	Mem_Free(f);
	return ok;
}

// ---------------------------------------------------------------- triangles (.vtx)

// One mesh's strip groups into a triangle list. base: the mesh's first vertex in s->verts.
static qboolean LoadMeshTris(const unsigned char *f, size_t size, size_t mesh_ofs, srcmodel_t *s, int base,
	int meshverts, srcmesh_t *out)
{
	int numgroups = RdInt(f + mesh_ofs);
	long long groups = (long long)mesh_ofs + RdInt(f + mesh_ofs + 4);
	if (!Has(size, groups, numgroups, VTX_GROUP_SIZE))
		return false;
	int cap = 0;
	for (int g = 0; g < numgroups; g++) {
		size_t go = (size_t)groups + (size_t)g * VTX_GROUP_SIZE;
		int numverts = RdInt(f + go), numidx = RdInt(f + go + 8), numstrips = RdInt(f + go + 16);
		long long verts = (long long)go + RdInt(f + go + 4), idx = (long long)go + RdInt(f + go + 12);
		long long strips = (long long)go + RdInt(f + go + 20);
		if (!Has(size, verts, numverts, VTX_VERT_SIZE) || !Has(size, idx, numidx, 2) ||
			!Has(size, strips, numstrips, VTX_STRIP_SIZE))
			return false;
		for (int st = 0; st < numstrips; st++) {
			const unsigned char *sp = f + strips + (size_t)st * VTX_STRIP_SIZE;
			int count = RdInt(sp), first = RdInt(sp + 4);
			qboolean strip = (sp[VTX_STRIP_FLAGS] & VTX_STRIP_IS_TRISTRIP) != 0;
			if (count < 0 || first < 0 || first > numidx || count > numidx - first)
				return false;
			int tris = strip ? (count >= 3 ? count - 2 : 0) : count / 3;
			if (out->numindices + tris * 3 > cap) {
				cap = (out->numindices + tris * 3) * 2;
				out->indices = Mem_Realloc(out->indices, sizeof(unsigned) * cap);
			}
			for (int t = 0; t < tris; t++) {
				int c[3];
				if (!strip) {
					c[0] = t * 3; c[1] = t * 3 + 1; c[2] = t * 3 + 2;
				} else if (t & 1) {
					c[0] = t + 1; c[1] = t; c[2] = t + 2;   // odd strip triangles flip to keep the winding
				} else {
					c[0] = t; c[1] = t + 1; c[2] = t + 2;
				}
				for (int k = 0; k < 3; k++) {
					// index > the strip group's vertex > the mesh's vertex > the model's vertex
					unsigned gv = RdU16(f + idx + (size_t)(first + c[k]) * 2);
					if ((int)gv >= numverts)
						return false;
					int mv = (int)RdU16(f + verts + (size_t)gv * VTX_VERT_SIZE + VTX_VERT_MESHVERT);
					if (mv >= meshverts || base + mv >= s->numverts)
						return false;
					out->indices[out->numindices++] = (unsigned)(base + mv);
				}
			}
		}
	}
	s->numtris += out->numindices / 3;
	return true;
}

// Faces are animated by "flexes": lists of vertices and how far each moves when the flex is fully on.
// The one that drops the jaw opens the mouth; it's the only kind used (for talking). me: the mesh in the .mdl.
static void LoadJawMorphs(srcmodel_t *s, const unsigned char *me, int base, int meshverts)
{
	enum { H_FLAGS = 152, H_NUMFLEXDESC = 260, H_FLEXDESCINDEX = 264, H_FIXEDSCALE = 392,
		MESH_NUMFLEXES = 16, MESH_FLEXINDEX = 20, FLEX_SIZE = 60, FLEX_NUMVERTS = 20, FLEX_VERTINDEX = 24,
		FLEX_ANIMTYPE = 32 };
	const unsigned char *mdl = s->mdl;
	size_t size = s->mdlsize;
	int nflex = RdInt(me + MESH_NUMFLEXES), ndesc = RdInt(mdl + H_NUMFLEXDESC);
	long long flexes = (me - mdl) + RdInt(me + MESH_FLEXINDEX), descs = RdInt(mdl + H_FLEXDESCINDEX);
	if (nflex <= 0 || !Has(size, flexes, nflex, FLEX_SIZE) || !Has(size, descs, ndesc, 4))
		return;
	// version 48 packs the movement as fixed-point numbers, earlier ones as half floats
	float fixed = (RdInt(mdl + H_FLAGS) & 0x200000) ? 0 : 1.0f / 4096.0f;
	if (!fixed)
		memcpy(&fixed, mdl + H_FIXEDSCALE, 4);
	for (int i = 0; i < nflex; i++) {
		const unsigned char *fp = mdl + flexes + (size_t)i * FLEX_SIZE;
		int desc = RdInt(fp);
		if (desc < 0 || desc >= ndesc)
			continue;
		long long d = descs + (long long)desc * 4;
		const char *name = FileString(mdl, size, d + RdInt(mdl + d));
		// by its plain name, or its facial-action number in older models (26; "R" is the entry for a
		// left/right pair, and both halves open together). 25 parts the lips, which comes with it.
		float amount = 0;
		if (name && (!stricmp(name, "jaw_drop") || !stricmp(name, "AU26") || !stricmp(name, "AU26R")))
			amount = 1;
		else if (name && (!stricmp(name, "AU25") || !stricmp(name, "AU25R")))
			amount = 0.5f;
		if (!amount)
			continue;
		int n = RdInt(fp + FLEX_NUMVERTS);
		size_t step = fp[FLEX_ANIMTYPE] == 1 ? 18 : 16;   // type 1 carries a wrinkle value too
		long long verts = (fp - mdl) + RdInt(fp + FLEX_VERTINDEX);
		if (n <= 0 || !Has(size, verts, n, step))
			continue;
		s->morphs = Mem_Realloc(s->morphs, sizeof(srcmorph_t) * (s->nummorphs + n));
		for (int k = 0; k < n; k++) {
			// vertex (within the mesh), speed, side, then the movement
			const unsigned char *an = mdl + verts + (size_t)k * step;
			int v = (int)RdU16(an);
			if (v >= meshverts || base + v >= s->numverts)
				continue;
			srcmorph_t *mo = &s->morphs[s->nummorphs++];
			mo->vert = (unsigned)(base + v);
			for (int j = 0; j < 3; j++) {
				unsigned raw = RdU16(an + 4 + j * 2);
				mo->delta[j] = amount * (s->version >= 48 ? (float)(short)raw * fixed : HalfToFloat(raw));
			}
		}
	}
}

// Walks the .mdl's body parts, models and meshes side by side with the .vtx's
static qboolean LoadBodyParts(const char *vtxname, const unsigned char *f, size_t size, srcmodel_t *s)
{
	const unsigned char *mdl = s->mdl;
	size_t msize = s->mdlsize;
	int nparts = RdInt(mdl + MDL_NUMBODYPARTS);
	long long parts = RdInt(mdl + MDL_BODYPARTINDEX);
	long long vparts = RdInt(f + VTX_BODYPARTOFFSET);
	if (!Has(msize, parts, nparts, MDL_BODYPART_SIZE) || RdInt(f + VTX_NUMBODYPARTS) != nparts ||
		!Has(size, vparts, nparts, VTX_BODYPART_SIZE))
		return false;
	s->bodyparts = Mem_Alloc(sizeof(srcbodypart_t) * (nparts ? nparts : 1));
	s->numbodyparts = nparts;
	for (int i = 0; i < nparts; i++) {
		size_t po = (size_t)parts + (size_t)i * MDL_BODYPART_SIZE, vpo = (size_t)vparts + (size_t)i * VTX_BODYPART_SIZE;
		srcbodypart_t *bp = &s->bodyparts[i];
		const char *name = FileString(mdl, msize, (long long)po + RdInt(mdl + po));
		Q_strncpy(bp->name, name ? name : "", sizeof(bp->name));
		int nmodels = RdInt(mdl + po + 4);
		bp->base = RdInt(mdl + po + 8);
		long long models = (long long)po + RdInt(mdl + po + 12), vmodels = (long long)vpo + RdInt(f + vpo + 4);
		if (!Has(msize, models, nmodels, MDL_MODEL_SIZE) || RdInt(f + vpo) != nmodels ||
			!Has(size, vmodels, nmodels, VTX_MODEL_SIZE))
			return false;
		bp->models = Mem_Alloc(sizeof(srcsubmodel_t) * (nmodels ? nmodels : 1));
		bp->nummodels = nmodels;
		for (int j = 0; j < nmodels; j++) {
			size_t mo = (size_t)models + (size_t)j * MDL_MODEL_SIZE, vmo = (size_t)vmodels + (size_t)j * VTX_MODEL_SIZE;
			srcsubmodel_t *sm = &bp->models[j];
			memcpy(sm->name, mdl + mo, sizeof(sm->name) - 1);
			int nmeshes = RdInt(mdl + mo + MDL_MODEL_NUMMESHES);
			long long meshes = (long long)mo + RdInt(mdl + mo + MDL_MODEL_MESHINDEX);
			int modelverts = RdInt(mdl + mo + MDL_MODEL_NUMVERTS);
			int vertindex = RdInt(mdl + mo + MDL_MODEL_VERTINDEX);   // byte offset into the vertex data
			// the first LOD is the full-detail one
			long long vlod = (long long)vmo + RdInt(f + vmo + 4);
			if (!Has(msize, meshes, nmeshes, MDL_MESH_SIZE) || vertindex < 0 || modelverts < 0)
				return false;
			int neyes = RdInt(mdl + mo + MDL_MODEL_NUMEYEBALLS);
			long long eyes = (long long)mo + RdInt(mdl + mo + MDL_MODEL_EYEBALLINDEX);
			if (neyes > 0 && Has(msize, eyes, neyes, MDL_EYEBALL_SIZE)) {
				sm->eyeballs = Mem_Alloc(sizeof(srceyeball_t) * neyes);
				sm->numeyeballs = neyes;
				for (int e = 0; e < neyes; e++) {
					const unsigned char *ep = mdl + eyes + (size_t)e * MDL_EYEBALL_SIZE;
					srceyeball_t *eye = &sm->eyeballs[e];
					eye->bone = RdInt(ep + MDL_EYEBALL_BONE);
					memcpy(eye->org, ep + MDL_EYEBALL_ORG, 12);
					memcpy(&eye->zoffset, ep + MDL_EYEBALL_ZOFFSET, 4);
					memcpy(&eye->radius, ep + MDL_EYEBALL_RADIUS, 4);
					memcpy(eye->up, ep + MDL_EYEBALL_UP, 12);
					memcpy(eye->forward, ep + MDL_EYEBALL_FORWARD, 12);
					memcpy(&eye->iris_scale, ep + MDL_EYEBALL_IRISSCALE, 4);
					// older models store "forward" pointing into the head (the game aimed eyes at a target
					// and never used it). The eye sits in front of its bone, so forward is the way out.
					if (DotProduct(eye->forward, eye->org) < 0)
						VectorScale(eye->forward, -1, eye->forward);
				}
			}
			if (!nmeshes)
				continue;   // an empty choice ("no helmet")
			if (RdInt(f + vmo) < 1 || !Has(size, vlod, 1, VTX_LOD_SIZE) || RdInt(f + vlod) != nmeshes)
				return false;
			long long vmeshes = vlod + RdInt(f + vlod + 4);
			if (!Has(size, vmeshes, nmeshes, VTX_MESH_SIZE))
				return false;
			sm->meshes = Mem_Alloc(sizeof(srcmesh_t) * nmeshes);
			sm->nummeshes = nmeshes;
			for (int k = 0; k < nmeshes; k++) {
				const unsigned char *me = mdl + meshes + (size_t)k * MDL_MESH_SIZE;
				srcmesh_t *out = &sm->meshes[k];
				out->skinref = RdInt(me + MDL_MESH_MATERIAL);
				out->eyeball = RdInt(me + MDL_MESH_MATERIALTYPE) == 1 ? RdInt(me + MDL_MESH_MATERIALPARAM) : -1;
				if (out->eyeball >= sm->numeyeballs)
					out->eyeball = -1;
				int meshverts = RdInt(me + MDL_MESH_NUMVERTS), vertoffset = RdInt(me + MDL_MESH_VERTOFFSET);
				if (meshverts < 0 || vertoffset < 0 || vertoffset > modelverts || meshverts > modelverts - vertoffset)
					return false;
				int base = vertindex / (int)sizeof(srcvert_t) + vertoffset;
				if (!LoadMeshTris(f, size, (size_t)vmeshes + (size_t)k * VTX_MESH_SIZE, s, base, meshverts, out))
					return false;
				LoadJawMorphs(s, me, base, meshverts);
			}
		}
	}
	(void)vtxname;
	return true;
}

static qboolean LoadTris(const char *base, srcmodel_t *s, int checksum)
{
	// the same triangles built for different hardware; any one will do
	static const char *const exts[] = { ".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx" };
	char name[MAX_OSPATH] = "";
	size_t size = 0;
	unsigned char *f = NULL;
	for (int i = 0; i < 4 && !f; i++) {
		snprintf(name, sizeof(name), "%s%s", base, exts[i]);
		f = SrcFS_LoadFile(name, &size);
	}
	if (!f) {
		Con_Warnf("%s.dx90.vtx (or .dx80.vtx, .sw.vtx) not found\n", base);
		return false;
	}
	qboolean ok = false;
	if (size < VTX_HEADER_SIZE || RdInt(f + VTX_VERSION) != 7)
		Con_Warnf("%s isn't a version 7 triangle file\n", name);
	else if (RdInt(f + VTX_CHECKSUM) != checksum)
		Con_Warnf("%s doesn't belong to its .mdl (different checksum)\n", name);
	else if (!(ok = LoadBodyParts(name, f, size, s)))
		Con_Warnf("%s is damaged, or doesn't match its .mdl\n", name);
	Mem_Free(f);
	return ok;
}

// ---------------------------------------------------------------- materials and skins

static qboolean LoadMaterials(const model_t *m, srcmodel_t *s)
{
	const unsigned char *mdl = s->mdl;
	size_t size = s->mdlsize;
	int ntex = RdInt(mdl + MDL_NUMTEXTURES), ncd = RdInt(mdl + MDL_NUMCDTEXTURES);
	long long tex = RdInt(mdl + MDL_TEXTUREINDEX), cd = RdInt(mdl + MDL_CDTEXTUREINDEX);
	int nref = RdInt(mdl + MDL_NUMSKINREF), nfam = RdInt(mdl + MDL_NUMSKINFAMILIES);
	long long skins = RdInt(mdl + MDL_SKININDEX);
	if (!Has(size, tex, ntex, MDL_TEXTURE_SIZE) || !Has(size, cd, ncd, 4) || nref < 0 || nfam < 0 ||
		(nref && !Has(size, skins, (long long)nref * nfam, 2)))
		return false;
	s->materials = Mem_Alloc(sizeof(*s->materials) * (ntex ? ntex : 1));
	s->numtextures = ntex;
	for (int i = 0; i < ntex; i++) {
		size_t to = (size_t)tex + (size_t)i * MDL_TEXTURE_SIZE;
		const char *name = FileString(mdl, size, (long long)to + RdInt(mdl + to));
		if (!name)
			return false;
		// the model lists folders to look in; the first that has the material wins
		char path[256] = "";
		for (int c = 0; c < ncd; c++) {
			const char *dir = FileString(mdl, size, RdInt(mdl + cd + (size_t)c * 4));
			if (!dir)
				return false;
			snprintf(path, sizeof(path), "%s%s%s", dir, *dir && !strchr("/\\", dir[strlen(dir) - 1]) ? "/" : "", name);
			if (SrcFS_FileExists(va("%smaterials/%s.vmt", s->prefix, path)))
				break;
		}
		if (!*path)
			Q_strncpy(path, name, sizeof(path));
		s->materials[i] = SrcMat_Find(s->prefix, path, TF_MODEL);
		if (s->materials[i]->flags & SMAT_MISSING) {
			if (!s->nummissing++)
				Con_Warnf("%s: material %s not found\n", m->name, path);
		}
	}
	s->numskinref = nref;
	s->numskinfamilies = nfam;
	s->skins = Mem_Alloc(sizeof(short) * (nref && nfam ? nref * nfam : 1));
	if (nref && nfam)
		memcpy(s->skins, mdl + skins, sizeof(short) * nref * nfam);
	return true;
}

// ---------------------------------------------------------------- the stand-in GoldSrc header

// Half-Life's activity numbers, in the order the game DLL counts them (0 = none). Source models name their
// activities; the same name gets the same number, and names Half-Life doesn't have get 0.
static const char *const hl_activities[] = {
	"ACT_RESET", "ACT_IDLE", "ACT_GUARD", "ACT_WALK", "ACT_RUN", "ACT_FLY", "ACT_SWIM", "ACT_HOP", "ACT_LEAP",
	"ACT_FALL", "ACT_LAND", "ACT_STRAFE_LEFT", "ACT_STRAFE_RIGHT", "ACT_ROLL_LEFT", "ACT_ROLL_RIGHT",
	"ACT_TURN_LEFT", "ACT_TURN_RIGHT", "ACT_CROUCH", "ACT_CROUCHIDLE", "ACT_STAND", "ACT_USE", "ACT_SIGNAL1",
	"ACT_SIGNAL2", "ACT_SIGNAL3", "ACT_TWITCH", "ACT_COWER", "ACT_SMALL_FLINCH", "ACT_BIG_FLINCH",
	"ACT_RANGE_ATTACK1", "ACT_RANGE_ATTACK2", "ACT_MELEE_ATTACK1", "ACT_MELEE_ATTACK2", "ACT_RELOAD", "ACT_ARM",
	"ACT_DISARM", "ACT_EAT", "ACT_DIESIMPLE", "ACT_DIEBACKWARD", "ACT_DIEFORWARD", "ACT_DIEVIOLENT",
	"ACT_BARNACLE_HIT", "ACT_BARNACLE_PULL", "ACT_BARNACLE_CHOMP", "ACT_BARNACLE_CHEW", "ACT_SLEEP",
	"ACT_INSPECT_FLOOR", "ACT_INSPECT_WALL", "ACT_IDLE_ANGRY", "ACT_WALK_HURT", "ACT_RUN_HURT", "ACT_HOVER",
	"ACT_GLIDE", "ACT_FLY_LEFT", "ACT_FLY_RIGHT", "ACT_DETECT_SCENT", "ACT_SNIFF", "ACT_BITE",
	"ACT_THREAT_DISPLAY", "ACT_FEAR_DISPLAY", "ACT_EXCITED", "ACT_SPECIAL_ATTACK1", "ACT_SPECIAL_ATTACK2",
	"ACT_COMBAT_IDLE", "ACT_WALK_SCARED", "ACT_RUN_SCARED", "ACT_VICTORY_DANCE", "ACT_DIE_HEADSHOT",
	"ACT_DIE_CHESTSHOT", "ACT_DIE_GUTSHOT", "ACT_DIE_BACKSHOT", "ACT_FLINCH_HEAD", "ACT_FLINCH_CHEST",
	"ACT_FLINCH_STOMACH", "ACT_FLINCH_LEFTARM", "ACT_FLINCH_RIGHTARM", "ACT_FLINCH_LEFTLEG", "ACT_FLINCH_RIGHTLEG",
};
#define HL_NUM_ACTIVITIES ((int)(sizeof(hl_activities) / sizeof(hl_activities[0])))

// When the game asks for an activity the model has no sequence for, it plays sequence 0, which for Half-Life
// 2's characters is a T-pose. So a missing activity borrows the sequence of the nearest thing the model can
// do. Followed in chains (run scared > run > walk). Deaths aren't here: Source characters fall as ragdolls
// and have no death animation to borrow.
static const char *const activity_fallbacks[][2] = {
	{ "ACT_TURN_LEFT", "ACT_IDLE" }, { "ACT_TURN_RIGHT", "ACT_IDLE" }, { "ACT_WALK_SCARED", "ACT_WALK" },
	{ "ACT_RUN_SCARED", "ACT_RUN" }, { "ACT_WALK_HURT", "ACT_WALK" }, { "ACT_RUN_HURT", "ACT_RUN" },
	{ "ACT_RUN", "ACT_WALK" }, { "ACT_WALK", "ACT_RUN" }, { "ACT_CROUCHIDLE", "ACT_CROUCH" },
	{ "ACT_CROUCH", "ACT_IDLE" }, { "ACT_STAND", "ACT_IDLE" }, { "ACT_GUARD", "ACT_IDLE" },
	{ "ACT_COMBAT_IDLE", "ACT_IDLE_ANGRY" }, { "ACT_IDLE_ANGRY", "ACT_IDLE" }, { "ACT_THREAT_DISPLAY", "ACT_IDLE_ANGRY" },
	{ "ACT_FEAR_DISPLAY", "ACT_COWER" }, { "ACT_COWER", "ACT_IDLE" }, { "ACT_EXCITED", "ACT_IDLE" },
	{ "ACT_BIG_FLINCH", "ACT_SMALL_FLINCH" }, { "ACT_FLINCH_HEAD", "ACT_SMALL_FLINCH" },
	{ "ACT_FLINCH_CHEST", "ACT_SMALL_FLINCH" }, { "ACT_FLINCH_STOMACH", "ACT_SMALL_FLINCH" },
	{ "ACT_FLINCH_LEFTARM", "ACT_SMALL_FLINCH" }, { "ACT_FLINCH_RIGHTARM", "ACT_SMALL_FLINCH" },
	{ "ACT_FLINCH_LEFTLEG", "ACT_SMALL_FLINCH" }, { "ACT_FLINCH_RIGHTLEG", "ACT_SMALL_FLINCH" },
	{ "ACT_SMALL_FLINCH", "ACT_IDLE" }, { "ACT_MELEE_ATTACK2", "ACT_MELEE_ATTACK1" },
	{ "ACT_RANGE_ATTACK2", "ACT_RANGE_ATTACK1" }, { "ACT_SPECIAL_ATTACK2", "ACT_SPECIAL_ATTACK1" },
	{ "ACT_HOVER", "ACT_IDLE" }, { "ACT_USE", "ACT_IDLE" }, { "ACT_ARM", "ACT_IDLE" }, { "ACT_DISARM", "ACT_IDLE" },
	{ "ACT_SIGNAL1", "ACT_IDLE" }, { "ACT_SIGNAL2", "ACT_IDLE" }, { "ACT_SIGNAL3", "ACT_IDLE" },
	{ "ACT_TWITCH", "ACT_IDLE" }, { "ACT_EAT", "ACT_IDLE" }, { "ACT_SNIFF", "ACT_IDLE" },
	{ "ACT_DETECT_SCENT", "ACT_IDLE" }, { "ACT_INSPECT_FLOOR", "ACT_IDLE" }, { "ACT_INSPECT_WALL", "ACT_IDLE" },
	{ "ACT_VICTORY_DANCE", "ACT_IDLE" },
};

static int ActivityNumber(const char *name)
{
	for (int i = 1; i < HL_NUM_ACTIVITIES; i++)
		if (!stricmp(hl_activities[i], name))
			return i;
	return 0;
}

// the model's heaviest sequence for a Half-Life activity, following the fallbacks; -1 if none
static int SequenceForActivity(const srcmodel_t *s, int act)
{
	for (int hops = 0; hops < 8 && act > 0; hops++) {
		int best = -1, weight = -1;
		for (int i = 0; i < s->numvseqs; i++) {
			const srcseq_t *q = SrcAnim_Seq(s, i);
			if (q->actweight > weight && ActivityNumber(q->activity) == act) {
				best = i;
				weight = q->actweight;
			}
		}
		if (best >= 0)
			return best;
		int next = 0;
		for (size_t k = 0; k < sizeof(activity_fallbacks) / sizeof(activity_fallbacks[0]); k++)
			if (ActivityNumber(activity_fallbacks[k][0]) == act)
				next = ActivityNumber(activity_fallbacks[k][1]);
		act = next;
	}
	return -1;
}

static void AddAlias(srcmodel_t *s, int vseq, int activity, const char *label)
{
	s->aliases = Mem_Realloc(s->aliases, sizeof(srcalias_t) * (s->numaliases + 1));
	srcalias_t *al = &s->aliases[s->numaliases++];
	al->vseq = vseq;
	al->activity = activity;
	Q_strncpy(al->label, label, sizeof(al->label));
}

static int FindSequence(const srcmodel_t *s, const char *label)
{
	for (int i = 0; i < s->numvseqs; i++)
		if (!stricmp(SrcAnim_Seq(s, i)->label, label))
			return i;
	return -1;
}

static int FindPose(const srcmodel_t *s, const char *name)
{
	for (int i = 0; i < s->numpose; i++)
		if (!stricmp(s->pose[i].name, name))
			return i;
	return -1;
}

// events the settings file renumbers, for BuildStandIn: by number, or by name for Source's named events
typedef struct { char name[48]; int from, to; } eventremap_t;
static eventremap_t *event_remap;
static int event_remap_count;

int SrcStudio_StandInSeq(const srcmodel_t *s, int i, const srcalias_t **alias)
{
	if (alias)
		*alias = NULL;
	if (i < 0)
		return 0;
	if (i < s->numorder)
		return s->order[i];
	i -= s->numorder;
	if (i < s->numvseqs)
		return i;
	i -= s->numvseqs;
	if (i < s->numaliases) {
		if (alias)
			*alias = &s->aliases[i];
		return s->aliases[i].vseq;
	}
	return 0;
}

// The Half-Life number for one of a sequence's events (owner: the file it's in), or 0 to leave it out.
// Numbered events keep their number; of Source's named ones only the muzzle flash has a Half-Life twin.
static int EventNumber(const srcmodel_t *owner, const unsigned char *se, qboolean *muzzle)
{
	*muzzle = false;
	if (!(RdInt(se + 8) & SRC_EVENT_NAMED)) {
		int n = RdInt(se + 4);
		for (int r = 0; r < event_remap_count; r++)
			if (!event_remap[r].name[0] && event_remap[r].from == n)
				return event_remap[r].to;
		return n;
	}
	const char *name = FileString(owner->mdl, owner->mdlsize, (se - owner->mdl) + RdInt(se + 76));
	if (!name)
		return 0;
	for (int r = 0; r < event_remap_count; r++)
		if (event_remap[r].name[0] && !stricmp(event_remap[r].name, name))
			return event_remap[r].to;
	if (!stricmp(name, "AE_MUZZLEFLASH") || !stricmp(name, "AE_NPC_MUZZLEFLASH")) {
		*muzzle = true;
		return 5001;
	}
	return 0;
}

static const srcmodel_t *SeqOwner(const srcmodel_t *s, int vseq)
{
	return s->vseqs[vseq].include < 0 ? s : s->includes[s->vseqs[vseq].include].model;
}

// The model's settings file, modelcfg/<model path>.newen (hl2:models/barney.mdl > modelcfg/hl2/models/
// barney.newen), found through Half-Life's search paths. One setting a line:
//   controller <0-3> <pose parameter> [reverse]   what the game's head-turn controller drives
//   blend <0-1> <pose parameter> [reverse]        what the game's aim blend drives
//   activity <ACT_NAME> <sequence>                this sequence answers that Half-Life activity
//   sequence <name> <sequence>                    this sequence also answers to that name
//   event <number> <number>                       an animation event number becomes another
//   mouth <bone> <x|y|z> <degrees>                talking turns this bone, instead of the jaw_drop flex
//   head <bone> [reverse]                         the game's head turn (controller 0) turns this bone; "none" = off
//   order <sequence> <sequence> ...               the model's first sequences, by number (several lines add up)
//   viewmodel_fov <degrees>                       the field of view it's drawn with in the player's hands
// Without a file: controller 0 turns the bone named like a head (...Head1), else drives head_yaw.
static void LoadSettings(const model_t *m, const char *path, srcmodel_t *s)
{
	for (int i = 0; i < 4; i++)
		s->ctrlpose[i] = -1;
	s->blendpose[0] = s->blendpose[1] = -1;
	s->jawbone = -1;
	// the head: a bone to turn if there's one named like a head, else a head_yaw pose parameter
	s->headbone = -1;
	for (int b = 0; b < s->numbones && s->headbone < 0; b++) {
		const char *bn = s->bones[b].name;
		size_t len = strlen(bn);
		if (!stricmp(bn, "head") || (len >= 5 && !stricmp(bn + len - 5, "Head1")) || (len >= 5 && !stricmp(bn + len - 5, ".Head")))
			s->headbone = b;
	}
	if (s->headbone < 0)
		s->ctrlpose[0] = FindPose(s, "head_yaw");
	event_remap_count = 0;

	char name[MAX_OSPATH];
	snprintf(name, sizeof(name), "modelcfg/%s", path);
	for (char *c = name; *c; c++)
		if (*c == ':')
			*c = '/';
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = 0;
	Q_strncpy(name + strlen(name), ".newen", sizeof(name) - strlen(name));
	char *text = FS_LoadFile(name, NULL);
	if (!text)
		return;
	int lineno = 0;
	for (char *line = text; line && *line;) {
		char *eol = strchr(line, '\n');
		if (eol)
			*eol = 0;
		lineno++;
		char tok[24][64];
		int n = 0;
		for (const char *p = line; n < 24 && (p = COM_Parse(p)) != NULL;)
			Q_strncpy(tok[n++], com_token, sizeof(tok[0]));
		qboolean ok = n == 0;
		if (n >= 2 && !stricmp(tok[0], "order")) {
			ok = true;
			for (int k = 1; k < n; k++) {
				int seq = FindSequence(s, tok[k]);
				if (seq < 0) {
					Con_Warnf("%s line %d: no sequence %s\n", name, lineno, tok[k]);
					seq = 0;
				}
				s->order = Mem_Realloc(s->order, sizeof(int) * (s->numorder + 1));
				s->order[s->numorder++] = seq;
			}
		} else if (n >= 2 && !stricmp(tok[0], "viewmodel_fov")) {
			s->viewfov = (float)atof(tok[1]);
			ok = s->viewfov >= 10 && s->viewfov <= 150;
			if (!ok)
				s->viewfov = 0;
		} else
		if (n >= 3 && (!stricmp(tok[0], "controller") || !stricmp(tok[0], "blend"))) {
			qboolean ctrl = !stricmp(tok[0], "controller");
			int i = atoi(tok[1]), pose = FindPose(s, tok[2]);
			if (i >= 0 && i < (ctrl ? 4 : 2) && (pose >= 0 || !stricmp(tok[2], "none"))) {
				(ctrl ? s->ctrlpose : s->blendpose)[i] = pose;
				(ctrl ? s->ctrlrev : s->blendrev)[i] = n >= 4 && !stricmp(tok[3], "reverse");
				ok = true;
			}
		} else if (n >= 3 && !stricmp(tok[0], "activity")) {
			int act = ActivityNumber(tok[1]), seq = FindSequence(s, tok[2]);
			if (act > 0 && seq >= 0) {
				AddAlias(s, seq, act, "");
				ok = true;
			}
		} else if (n >= 3 && !stricmp(tok[0], "sequence")) {
			int seq = FindSequence(s, tok[2]);
			if (seq >= 0) {
				AddAlias(s, seq, 0, tok[1]);
				ok = true;
			}
		} else if (n >= 3 && !stricmp(tok[0], "event")) {
			// a number, or the name of one of Source's named events; to 0 = leave it out
			event_remap = Mem_Realloc(event_remap, sizeof(*event_remap) * (event_remap_count + 1));
			eventremap_t *er = &event_remap[event_remap_count++];
			qboolean number = tok[1][0] >= '0' && tok[1][0] <= '9';
			Q_strncpy(er->name, number ? "" : tok[1], sizeof(er->name));
			er->from = atoi(tok[1]);
			er->to = atoi(tok[2]);
			ok = true;
		} else if (n >= 2 && !stricmp(tok[0], "head")) {
			s->headbone = -1;
			for (int b = 0; b < s->numbones; b++)
				if (!stricmp(s->bones[b].name, tok[1]))
					s->headbone = b;
			s->headrev = n >= 3 && !stricmp(tok[2], "reverse");
			ok = s->headbone >= 0 || !stricmp(tok[1], "none");
			if (ok)
				s->ctrlpose[0] = -1;
		} else if (n >= 4 && !stricmp(tok[0], "mouth")) {
			for (int b = 0; b < s->numbones; b++)
				if (!stricmp(s->bones[b].name, tok[1]))
					s->jawbone = b;
			s->jawaxis = tolower((unsigned char)tok[2][0]) - 'x';
			s->jawdegrees = (float)atof(tok[3]);
			ok = s->jawbone >= 0 && s->jawaxis >= 0 && s->jawaxis <= 2;
			if (!ok)
				s->jawbone = -1;
		}
		if (!ok)
			Con_Warnf("%s line %d: \"%s\" isn't a setting this model can use\n", name, lineno, tok[0]);
		line = eol ? eol + 1 : NULL;
	}
	Mem_Free(text);
	Con_DPrintf("%s: settings from %s\n", m->name, name);
}

// What the DLLs and the engine's GoldSrc-shaped code read for this model: bones (names, parents, rest
// positions), sequences (name, speed, length, looping, box, activity, events, travel), attachments, hitboxes
// and body parts, with no vertices and no animation data. Every sequence points at one shared block of
// "no animation" records; the real animation is read from the Source file by SrcAnim_CalcPose.
static void BuildStandIn(model_t *m, srcmodel_t *s)
{
	const unsigned char *mdl = s->mdl;
	size_t msize = s->mdlsize;

	// activities Half-Life may ask for that the model can't answer (and the settings file didn't give)
	// get an extra sequence entry
	for (int act = 1; act < HL_NUM_ACTIVITIES && s->numvseqs > 0; act++) {
		qboolean have = false;
		for (int i = 0; i < s->numvseqs && !have; i++)
			have = ActivityNumber(SrcAnim_Seq(s, i)->activity) == act;
		for (int i = 0; i < s->numaliases && !have; i++)
			have = s->aliases[i].activity == act;
		int seq = have ? -1 : SequenceForActivity(s, act);
		if (seq >= 0)
			AddAlias(s, seq, act, "");
	}
	if (s->headbone >= 0)
		s->ctrlpose[0] = -1;   // controller 0 can only do one thing
	int nctrl = s->headbone >= 0;
	for (int i = 0; i < 4; i++)
		nctrl += s->ctrlpose[i] >= 0;

	// attachments and the first hitbox set, straight from the file
	enum { H_EYEPOS = 80, H_NUMHITBOXSETS = 172, H_HITBOXSETINDEX = 176, H_NUMATTACH = 240, H_ATTACHINDEX = 244,
		ATTACH_SIZE = 92, HITBOX_SIZE = 68 };
	int nattach = RdInt(mdl + H_NUMATTACH);
	long long attach = RdInt(mdl + H_ATTACHINDEX);
	if (nattach < 0 || !Has(msize, attach, nattach, ATTACH_SIZE))
		nattach = 0;
	int nboxes = 0;
	long long boxes = 0;
	long long sets = RdInt(mdl + H_HITBOXSETINDEX);
	if (RdInt(mdl + H_NUMHITBOXSETS) > 0 && Has(msize, sets, 1, 12)) {
		nboxes = RdInt(mdl + sets + 4);
		boxes = sets + RdInt(mdl + sets + 8);
		if (nboxes < 0 || !Has(msize, boxes, nboxes, HITBOX_SIZE))
			nboxes = 0;
	}

	int nmodels = 0, nevents = 0;
	for (int i = 0; i < s->numbodyparts; i++)
		nmodels += s->bodyparts[i].nummodels;
	int nseq = s->numorder + s->numvseqs + s->numaliases;
	if (nseq < 1)
		nseq = 1;
	for (int i = 0; i < nseq; i++) {
		int v = SrcStudio_StandInSeq(s, i, NULL);
		const srcseq_t *ss = SrcAnim_Seq(s, v);
		qboolean muzzle;
		for (int k = 0; ss && k < ss->numevents; k++)
			if (EventNumber(SeqOwner(s, v), ss->events + (size_t)k * SRC_EVENT_SIZE, &muzzle))
				nevents++;
	}
	size_t bones = sizeof(studiohdr_t);
	size_t ctrls = bones + sizeof(mstudiobone_t) * s->numbones;
	size_t seqs = ctrls + sizeof(mstudiobonecontroller_t) * nctrl;
	size_t events = seqs + sizeof(mstudioseqdesc_t) * nseq;
	size_t anims = events + sizeof(mstudioevent_t) * nevents;
	size_t groups = anims + sizeof(mstudioanim_t) * s->numbones;
	size_t attachs = groups + sizeof(mstudioseqgroup_t);
	size_t hitboxes = attachs + sizeof(mstudioattachment_t) * nattach;
	size_t parts = hitboxes + sizeof(mstudiobbox_t) * nboxes;
	size_t models = parts + sizeof(mstudiobodyparts_t) * s->numbodyparts;
	size_t total = models + sizeof(mstudiomodel_t) * nmodels;

	studiohdr_t *h = Mem_Alloc(total);
	h->id = IDSTUDIOHEADER;
	h->version = STUDIO_VERSION;
	Q_strncpy(h->name, m->name, sizeof(h->name));
	h->length = (int)total;
	memcpy(h->eyeposition, mdl + H_EYEPOS, 12);
	VectorCopy(m->mins, h->min);
	VectorCopy(m->maxs, h->max);
	VectorCopy(m->mins, h->bbmin);
	VectorCopy(m->maxs, h->bbmax);

	h->numbones = s->numbones;
	h->boneindex = (int)bones;
	mstudiobone_t *bone = STUDIO_PTR(h, bones, mstudiobone_t);
	for (int i = 0; i < s->numbones; i++) {
		Q_strncpy(bone[i].name, s->bones[i].name, sizeof(bone[i].name));
		bone[i].parent = s->bones[i].parent;
		for (int j = 0; j < 6; j++)
			bone[i].bonecontroller[j] = -1;
		for (int j = 0; j < 3; j++) {
			bone[i].value[j] = s->bones[i].pos[j];
			bone[i].value[j + 3] = s->bones[i].rot[j];
		}
	}

	// a controller per mapped pose parameter, with the parameter's range: the game converts its angle to
	// a 0-255 setting against this range, and the renderer reads the setting back as 0..1 of the parameter
	h->numbonecontrollers = nctrl;
	h->bonecontrollerindex = (int)ctrls;
	mstudiobonecontroller_t *bc = STUDIO_PTR(h, ctrls, mstudiobonecontroller_t);
	if (s->headbone >= 0) {
		bc->bone = s->headbone;
		bc->type = STUDIO_ZR;
		bc->start = -SRC_HEAD_TURN;
		bc->end = SRC_HEAD_TURN;
		bc++;   // index 0
	}
	for (int i = 0; i < 4; i++) {
		if (s->ctrlpose[i] < 0)
			continue;
		const srcposeparam_t *pp = &s->pose[s->ctrlpose[i]];
		bc->bone = -1;
		bc->type = STUDIO_YR;
		bc->start = pp->start < pp->end ? pp->start : pp->end;
		bc->end = pp->start < pp->end ? pp->end : pp->start;
		bc->index = i;
		bc++;
	}

	h->numseq = nseq;
	h->seqindex = (int)seqs;
	mstudioseqdesc_t *seq = STUDIO_PTR(h, seqs, mstudioseqdesc_t);
	mstudioevent_t *ev = STUDIO_PTR(h, events, mstudioevent_t);
	for (int i = 0; i < nseq; i++) {
		seq[i].fps = 30;
		seq[i].numframes = 1;
		seq[i].numblends = 1;
		seq[i].animindex = (int)anims;   // all zero: every bone at its default
		VectorCopy(m->mins, seq[i].bbmin);
		VectorCopy(m->maxs, seq[i].bbmax);
		const srcalias_t *al;
		int v = SrcStudio_StandInSeq(s, i, &al);
		const srcseq_t *ss = SrcAnim_Seq(s, v);
		for (int k = 0; k < 2; k++) {
			// the aim blends work like the controllers
			if (s->blendpose[k] < 0)
				continue;
			const srcposeparam_t *pp = &s->pose[s->blendpose[k]];
			seq[i].blendtype[k] = k ? STUDIO_YR : STUDIO_XR;
			seq[i].blendstart[k] = pp->start < pp->end ? pp->start : pp->end;
			seq[i].blendend[k] = pp->start < pp->end ? pp->end : pp->start;
		}
		if (!ss) {
			Q_strncpy(seq[i].label, "idle", sizeof(seq[i].label));   // a model with no sequences
			continue;
		}
		Q_strncpy(seq[i].label, al && al->label[0] ? al->label : ss->label, sizeof(seq[i].label));
		seq[i].fps = ss->fps > 0 ? ss->fps : 30;
		seq[i].numframes = ss->numframes > 0 ? ss->numframes : 1;
		if (ss->flags & SRCSEQ_LOOPING)
			seq[i].flags |= STUDIO_LOOPING;
		if (!VectorCompare(ss->bbmin, ss->bbmax)) {
			VectorCopy(ss->bbmin, seq[i].bbmin);
			VectorCopy(ss->bbmax, seq[i].bbmax);
		}
		seq[i].activity = al ? al->activity : ActivityNumber(ss->activity);
		seq[i].actweight = al ? 1 : ss->actweight;
		// how far the sequence walks; the game works its ground speed out from this
		const srcanim_t *sa = SrcAnim_SeqAnim(s, v);
		if (sa) {
			VectorCopy(sa->movement, seq[i].linearmovement);
			seq[i].motiontype = (sa->movement[0] ? STUDIO_X : 0) | (sa->movement[1] ? STUDIO_Y : 0) |
				(sa->movement[2] ? STUDIO_Z : 0);
		}
		// numbered events (sounds, attacks) keep their numbers; of the named ones the muzzle flash carries over
		seq[i].eventindex = (int)((unsigned char *)ev - (unsigned char *)h);
		for (int k = 0; k < ss->numevents; k++) {
			const unsigned char *se = ss->events + (size_t)k * SRC_EVENT_SIZE;
			qboolean muzzle;
			int number = EventNumber(SeqOwner(s, v), se, &muzzle);
			if (!number)
				continue;
			float cycle;
			memcpy(&cycle, se, 4);
			if (!(cycle >= 0 && cycle <= 1))
				cycle = 0;
			ev->frame = (int)(cycle * (float)(seq[i].numframes - 1) + 0.5f);
			ev->event = number;
			if (muzzle)
				Q_strncpy(ev->options, "11", sizeof(ev->options));   // Half-Life's flash: size 1, sprite 1
			else
				memcpy(ev->options, se + 12, sizeof(ev->options) - 1);
			ev++;
			seq[i].numevents++;
		}
	}
	h->numseqgroups = 1;
	h->seqgroupindex = (int)groups;
	Q_strncpy(STUDIO_PTR(h, groups, mstudioseqgroup_t)->label, "default", 32);

	h->numattachments = nattach;
	h->attachmentindex = (int)attachs;
	mstudioattachment_t *at = STUDIO_PTR(h, attachs, mstudioattachment_t);
	for (int i = 0; i < nattach; i++) {
		// name offset, flags, bone, then a 3x4 placement in that bone's space
		const unsigned char *sa = mdl + attach + (size_t)i * ATTACH_SIZE;
		const char *name = FileString(mdl, msize, (sa - mdl) + RdInt(sa));
		Q_strncpy(at[i].name, name ? name : "", sizeof(at[i].name));
		int b = RdInt(sa + 8);
		at[i].bone = b >= 0 && b < s->numbones ? b : 0;
		float local[3][4];
		memcpy(local, sa + 12, sizeof(local));
		for (int j = 0; j < 3; j++) {
			at[i].org[j] = local[j][3];
			for (int k = 0; k < 3; k++)
				at[i].vectors[k][j] = local[j][k];
		}
	}

	h->numhitboxes = nboxes;
	h->hitboxindex = (int)hitboxes;
	mstudiobbox_t *hb = STUDIO_PTR(h, hitboxes, mstudiobbox_t);
	for (int i = 0; i < nboxes; i++) {
		const unsigned char *sb = mdl + boxes + (size_t)i * HITBOX_SIZE;
		int b = RdInt(sb);
		hb[i].bone = b >= 0 && b < s->numbones ? b : 0;
		hb[i].group = RdInt(sb + 4);
		memcpy(hb[i].bbmin, sb + 8, 12);
		memcpy(hb[i].bbmax, sb + 20, 12);
	}

	h->numbodyparts = s->numbodyparts;
	h->bodypartindex = (int)parts;
	mstudiobodyparts_t *bp = STUDIO_PTR(h, parts, mstudiobodyparts_t);
	size_t mo = models;
	for (int i = 0; i < s->numbodyparts; i++) {
		Q_strncpy(bp[i].name, s->bodyparts[i].name, sizeof(bp[i].name));
		bp[i].nummodels = s->bodyparts[i].nummodels;
		bp[i].base = s->bodyparts[i].base;
		bp[i].modelindex = (int)mo;
		for (int j = 0; j < bp[i].nummodels; j++, mo += sizeof(mstudiomodel_t))
			Q_strncpy(STUDIO_PTR(h, mo, mstudiomodel_t)->name, s->bodyparts[i].models[j].name, 64);
	}
	h->numskinfamilies = s->numskinfamilies;

	studiomodel_t *st = &m->studio;
	st->hdr = st->texhdr = h;
	st->seqdata = Mem_Alloc(sizeof(*st->seqdata));
	st->seqsize = Mem_Alloc(sizeof(*st->seqsize));
	st->seqdata[0] = (unsigned char *)h;
	st->seqsize[0] = total;
}

// ---------------------------------------------------------------- loading

qboolean SrcStudio_Load(model_t *m, const char *path, unsigned char *data, size_t size)
{
	// from here on the model owns data, whatever happens (SrcStudio_Free)
	srcmodel_t *s = Mem_Alloc(sizeof(*s));
	m->studio.source = s;
	s->mdl = data;
	s->mdlsize = size;
	s->version = size >= 8 ? RdInt(data + 4) : 0;
	Q_strncpy(s->prefix, SrcFS_Prefix(path), sizeof(s->prefix));
	if (s->version < SRC_MDL_MIN_VERSION || s->version > SRC_MDL_MAX_VERSION) {
		Con_Warnf("%s is studio version %d; Half-Life's 10 and Source's %d to %d are supported\n", m->name, s->version,
			SRC_MDL_MIN_VERSION, SRC_MDL_MAX_VERSION);
		return false;
	}
	if (size < MDL_HEADER_SIZE) {
		Con_Warnf("%s is too small to be a model\n", m->name);
		return false;
	}

	char base[MAX_OSPATH];
	Q_strncpy(base, path, sizeof(base));
	char *dot = strrchr(base, '.');
	if (dot && !stricmp(dot, ".mdl"))
		*dot = 0;
	int checksum = RdInt(data + MDL_CHECKSUM);
	if (!LoadVerts(va("%s.vvd", base), s, checksum) || !LoadTris(base, s, checksum))
		return false;
	if (!SrcAnim_Load(m->name, s)) {
		Con_Warnf("%s is damaged (its bones or animations point outside the file)\n", m->name);
		return false;
	}
	SrcAnim_Link(m->name, s);
	for (int i = 0; i < s->numverts; i++) {
		const srcvert_t *v = &s->verts[i];
		if (v->numbones > 3 || (v->numbones > 0 && v->bone[0] >= s->numbones) ||
			(v->numbones > 1 && v->bone[1] >= s->numbones) || (v->numbones > 2 && v->bone[2] >= s->numbones)) {
			Con_Warnf("%s is damaged (a vertex names a bone the model doesn't have)\n", m->name);
			return false;
		}
	}
	if (!LoadMaterials(m, s)) {
		Con_Warnf("%s is damaged (its texture list points outside the file)\n", m->name);
		return false;
	}

	// the movement hull, else the view box, else the vertices themselves
	memcpy(m->mins, data + MDL_HULL_MIN, 12);
	memcpy(m->maxs, data + MDL_HULL_MAX, 12);
	if (VectorCompare(m->mins, m->maxs)) {
		memcpy(m->mins, data + MDL_VIEW_MIN, 12);
		memcpy(m->maxs, data + MDL_VIEW_MAX, 12);
	}
	if (VectorCompare(m->mins, m->maxs) && s->numverts) {
		VectorCopy(s->verts[0].pos, m->mins);
		VectorCopy(s->verts[0].pos, m->maxs);
	}
	// models are often bigger than their hull; culling needs the whole of it
	for (int i = 0; i < s->numverts; i++)
		for (int j = 0; j < 3; j++) {
			float v = s->verts[i].pos[j];
			if (!(v > -65536 && v < 65536)) {
				Con_Warnf("%s has a vertex far outside any map\n", m->name);
				return false;
			}
			if (v < m->mins[j]) m->mins[j] = v;
			if (v > m->maxs[j]) m->maxs[j] = v;
		}
	LoadSettings(m, path, s);
	BuildStandIn(m, s);
	return true;
}

void SrcStudio_Free(model_t *m)
{
	srcmodel_t *s = m->studio.source;
	if (!s)
		return;
	for (int i = 0; i < s->numbodyparts && s->bodyparts; i++) {
		srcbodypart_t *bp = &s->bodyparts[i];
		for (int j = 0; j < bp->nummodels && bp->models; j++) {
			for (int k = 0; k < bp->models[j].nummeshes; k++)
				Mem_Free(bp->models[j].meshes[k].indices);
			Mem_Free(bp->models[j].meshes);
			Mem_Free(bp->models[j].eyeballs);
		}
		Mem_Free(bp->models);
	}
	Mem_Free(s->bodyparts);
	Mem_Free(s->verts);
	Mem_Free(s->tangents);
	Mem_Free(s->materials);   // the materials themselves belong to the material cache
	Mem_Free(s->skins);
	Mem_Free(s->morphs);
	SrcAnim_Free(s);
	Mem_Free(s->mdl);
	Mem_Free(s);
	m->studio.source = NULL;
}

void SrcStudio_PrintInfo(const model_t *m)
{
	const srcmodel_t *s = m->studio.source;
	Con_Printf("%s: Source model version %d, %d vertices, %d triangles, %d bones, %d sequences, %d body parts, %d skins, %d materials",
		m->name, s->version, s->numverts, s->numtris, s->numbones, s->numvseqs, s->numbodyparts, s->numskinfamilies,
		s->numtextures);
	if (s->nummissing)
		Con_Printf(" (%d missing)", s->nummissing);
	Con_Printf("\n");
	for (int i = 0; i < s->numincludes; i++)
		Con_Printf("  animations from %s (%d sequences)\n", s->includes[i].model->mdl + 12, s->includes[i].model->numseqs);
	for (int i = 0; i < s->numpose; i++)
		Con_Printf("  pose parameter %d  %-16s %g to %g\n", i, s->pose[i].name, s->pose[i].start, s->pose[i].end);
	for (int i = 0; i < s->numvseqs; i++) {
		const srcseq_t *q = SrcAnim_Seq(s, i);
		const srcmodel_t *owner = s->vseqs[i].include < 0 ? s : s->includes[s->vseqs[i].include].model;
		Con_Printf("  seq %3d  %-28s %4d frames  %5.1f fps%s%s%s  %s\n", i, q->label, q->numframes, q->fps,
			q->flags & SRCSEQ_LOOPING ? "  loop" : "", q->groupsize[0] * q->groupsize[1] > 1 ? "  blend" : "",
			q->anim >= 0 && (owner->anims[q->anim].flags & SRCANIM_DELTA) ? "  delta" : "", q->activity);
		// its events: number@frame, or the name of one Half-Life has no number for
		for (int k = 0; k < q->numevents; k++) {
			const unsigned char *se = q->events + (size_t)k * SRC_EVENT_SIZE;
			float cycle;
			memcpy(&cycle, se, 4);
			if (!(cycle >= 0 && cycle <= 1))
				cycle = 0;
			int frame = (int)(cycle * (float)(q->numframes > 1 ? q->numframes - 1 : 0) + 0.5f);
			if (RdInt(se + 8) & SRC_EVENT_NAMED) {
				const char *en = FileString(owner->mdl, owner->mdlsize, (se - owner->mdl) + RdInt(se + 76));
				Con_Printf("%s%s@%d", k ? " " : "           events ", en ? en : "?", frame);
			} else
				Con_Printf("%s%d@%d", k ? " " : "           events ", RdInt(se + 4), frame);
		}
		if (q->numevents)
			Con_Printf("\n");
	}
	if (s->numorder)
		Con_Printf("  the settings file puts %d sequences first: the numbers above start at %d for the game\n",
			s->numorder, s->numorder);
	for (int i = 0; i < s->numorder; i++)
		Con_Printf("  seq %3d  %s\n", i, SrcAnim_Seq(s, s->order[i])->label);
	for (int i = 0; i < s->numaliases; i++)
		Con_Printf("  seq %3d  %-28s plays %s%s%s\n", s->numorder + s->numvseqs + i,
			s->aliases[i].label[0] ? s->aliases[i].label : SrcAnim_Seq(s, s->aliases[i].vseq)->label,
			SrcAnim_Seq(s, s->aliases[i].vseq)->label, s->aliases[i].activity ? " for " : "",
			s->aliases[i].activity ? hl_activities[s->aliases[i].activity] : "");
	if (s->headbone >= 0)
		Con_Printf("  head turn: bone %s\n", s->bones[s->headbone].name);
	Con_Printf("  mouth: %s\n", s->jawbone >= 0 ? s->bones[s->jawbone].name : s->nummorphs ? "jaw flex" : "none");
	for (int i = 0; i < s->numbodyparts; i++)
		Con_Printf("  body part %d  %-16s %d choice%s\n", i, s->bodyparts[i].name, s->bodyparts[i].nummodels,
			s->bodyparts[i].nummodels == 1 ? "" : "s");
	for (int i = 0; i < s->numtextures; i++) {
		const srcmaterial_t *mat = s->materials[i];
		Con_Printf("  material %-48s %4dx%-4d%s%s%s%s%s\n", mat->name, mat->width, mat->height,
			mat->flags & SMAT_MISSING ? " MISSING" : mat->flags & SMAT_NODRAW ? " not drawn" : "", mat->flags & SMAT_ALPHATEST ? " alphatest" : "",
			mat->flags & SMAT_TRANSLUCENT ? " translucent" : "", mat->flags & SMAT_ADDITIVE ? " additive" : "",
			mat->flags & SMAT_NOCULL ? " nocull" : "");
	}
}
