// newen - Source models' physics files (.phy): the convex pieces of each solid, and the text that follows
// them (each solid's bone and mass, the joints between them and how far each turns)
//
// A .phy is a 16-byte header, then each solid as a size and that many bytes, then text to the end of the
// file. A solid's bytes are a "compact surface": a header, convex pieces ("ledges": triangles naming points
// by number) and a tree of bounding spheres whose leaves point at the pieces. Offsets inside it count from
// the structure that holds them. Everything is range-checked; a file that fails a check is refused whole.
//
// Written from the format's public community documentation; structures and names here are newen's own.
#include "src_local.h"
#include <ctype.h>
#include <math.h>

enum {
	PHY_HEADER_SIZE = 16,        // header size, id, solid count, the .mdl's checksum
	PHY_NUMSOLIDS = 8, PHY_CHECKSUM = 12,
	// a solid after its size: this header, when it has the "VPHY" mark (older ones start at the surface)
	VPHY_ID = 0, VPHY_VERSION = 4, VPHY_TYPE = 6, VPHY_HEADER_SIZE = 28,
	// the surface: centre of mass (3 floats), inertia (3), radius, a byte + its 24-bit size, tree offset, "IVPS"
	SURF_MASSCENTER = 0, SURF_SIZE = 28, SURF_TREE = 32, SURF_ID = 44, SURF_HEADER_SIZE = 48,
	// a convex piece: offset to its points, client data, flags + size, triangle count; its triangles follow
	LEDGE_POINTS = 0, LEDGE_NUMTRIS = 12, LEDGE_SIZE = 16,
	TRI_SIZE = 16,               // 4 bytes of numbers, then three edges of 4 bytes, each starting with a point number
	POINT_SIZE = 16,             // x, y, z and a spare float
	// a tree node: offset to its right child (0 = a leaf; the left child follows the node), offset to its piece
	NODE_RIGHT = 0, NODE_LEDGE = 4, NODE_SIZE = 28,
};
#define VPHY_MARK 0x59485056   // "VPHY"
#define IVPS_MARK 0x53505649   // "IVPS"
#define METRES_TO_INCHES (1.0f / 0.0254f)

static int RdInt(const unsigned char *p)
{
	int v;
	memcpy(&v, p, 4);
	return v;
}

// true if size bytes at ofs lie inside len bytes
static qboolean Inside(size_t len, long long ofs, size_t size)
{
	return ofs >= 0 && (unsigned long long)ofs <= len && size <= len - (size_t)ofs;
}

// The file's points are in metres with Y down and Z where our Y is
static void ToInches(const unsigned char *p, float out[3])
{
	float v[3];
	memcpy(v, p, 12);
	out[0] = v[0] * METRES_TO_INCHES;
	out[1] = v[2] * METRES_TO_INCHES;
	out[2] = -v[1] * METRES_TO_INCHES;
}

typedef struct {
	float *points;
	int    numpoints, cappoints;
	int   *counts;
	int    numpieces, cappieces;
	int   *slot;                 // point number in the file -> its place in the piece being read, +1
	int    capslot;
} phybuild_t;

// One convex piece at ofs in the surface d (len bytes): the points its triangles use, each once
static qboolean ReadLedge(const unsigned char *d, size_t len, long long ofs, phybuild_t *b, int *numtris)
{
	if (!Inside(len, ofs, LEDGE_SIZE))
		return false;
	const unsigned char *ledge = d + ofs;
	long long points = ofs + RdInt(ledge + LEDGE_POINTS);
	int ntris = (short)(ledge[LEDGE_NUMTRIS] | ledge[LEDGE_NUMTRIS + 1] << 8);
	if (ntris < 0 || !Inside(len, ofs + LEDGE_SIZE, (size_t)ntris * TRI_SIZE) || !Inside(len, points, 0))
		return false;
	// how many points fit between the piece's point list and the end of the surface
	size_t maxpoints = (len - (size_t)points) / POINT_SIZE;
	int first = b->numpoints, used = 0;
	for (int t = 0; t < ntris; t++)
		for (int e = 0; e < 3; e++) {
			const unsigned char *edge = ledge + LEDGE_SIZE + (size_t)t * TRI_SIZE + 4 + e * 4;
			unsigned n = edge[0] | edge[1] << 8;
			if (n >= maxpoints)
				return false;
			if ((int)n >= b->capslot) {
				int cap = b->capslot ? b->capslot : 64;
				while (cap <= (int)n)
					cap *= 2;
				b->slot = Mem_Realloc(b->slot, sizeof(int) * cap);
				memset(b->slot + b->capslot, 0, sizeof(int) * (cap - b->capslot));
				b->capslot = cap;
			}
			if (b->slot[n])
				continue;
			if (b->numpoints == b->cappoints) {
				b->cappoints = b->cappoints ? b->cappoints * 2 : 256;
				b->points = Mem_Realloc(b->points, sizeof(float) * 3 * b->cappoints);
			}
			float *out = b->points + b->numpoints * 3;
			ToInches(d + points + (size_t)n * POINT_SIZE, out);
			for (int k = 0; k < 3; k++)
				if (!(out[k] > -65536 && out[k] < 65536))
					return false;
			b->slot[n] = ++used;
			b->numpoints++;
		}
	if (b->slot)
		memset(b->slot, 0, sizeof(int) * b->capslot);
	if (used < 4) {
		// not a solid shape; nothing for the physics to use
		b->numpoints = first;
		return true;
	}
	if (b->numpieces == b->cappieces) {
		b->cappieces = b->cappieces ? b->cappieces * 2 : 16;
		b->counts = Mem_Realloc(b->counts, sizeof(int) * b->cappieces);
	}
	b->counts[b->numpieces++] = used;
	*numtris += ntris;
	return true;
}

// The pieces of one solid, d being its len bytes. why says what's wrong when it returns false.
static qboolean ReadSolid(const unsigned char *d, size_t len, srcphysolid_t *s, phybuild_t *b, const char **why)
{
	*why = "a solid points outside the file";
	if (len >= VPHY_HEADER_SIZE && RdInt(d + VPHY_ID) == VPHY_MARK) {
		int type = (short)(d[VPHY_TYPE] | d[VPHY_TYPE + 1] << 8);
		if (type != 0) {
			// the other kinds are for map-sized meshes, which models don't have
			*why = "a solid isn't made of convex pieces";
			return false;
		}
		d += VPHY_HEADER_SIZE;
		len -= VPHY_HEADER_SIZE;
	}
	if (len < SURF_HEADER_SIZE)
		return false;
	if (RdInt(d + SURF_ID) != IVPS_MARK) {
		*why = "a solid's collision data isn't in the known format";
		return false;
	}
	ToInches(d + SURF_MASSCENTER, s->masscenter);
	for (int k = 0; k < 3; k++)
		if (!(s->masscenter[k] > -65536 && s->masscenter[k] < 65536))
			VectorClear(s->masscenter);

	// walk the tree to its leaves; a damaged tree could loop, so no more visits than nodes fit in the solid
	b->numpoints = b->numpieces = 0;
	s->numtris = 0;
	int *stack = NULL, depth = 0, cap = 0;
	size_t visits = 0, maxvisits = len / NODE_SIZE + 1;
	long long node = RdInt(d + SURF_TREE);
	qboolean ok = true;
	for (;;) {
		if (!Inside(len, node, NODE_SIZE) || ++visits > maxvisits) {
			ok = false;
			break;
		}
		int right = RdInt(d + node + NODE_RIGHT);
		if (right) {
			if (depth == cap) {
				cap = cap ? cap * 2 : 32;
				stack = Mem_Realloc(stack, sizeof(int) * cap);
			}
			long long r = node + right;
			if (r < 0 || r > (long long)len) {
				ok = false;
				break;
			}
			stack[depth++] = (int)r;
			node += NODE_SIZE;
			continue;
		}
		if (!ReadLedge(d, len, node + RdInt(d + node + NODE_LEDGE), b, &s->numtris)) {
			ok = false;
			break;
		}
		if (!depth)
			break;
		node = stack[--depth];
	}
	Mem_Free(stack);
	if (!ok)
		return false;

	s->numpieces = b->numpieces;
	s->numpoints = b->numpoints;
	if (b->numpieces) {
		s->counts = Mem_Alloc(sizeof(int) * b->numpieces);
		memcpy(s->counts, b->counts, sizeof(int) * b->numpieces);
		s->points = Mem_Alloc(sizeof(float) * 3 * b->numpoints);
		memcpy(s->points, b->points, sizeof(float) * 3 * b->numpoints);
		VectorCopy(s->points, s->mins);
		VectorCopy(s->points, s->maxs);
		for (int i = 1; i < s->numpoints; i++)
			for (int k = 0; k < 3; k++) {
				float v = s->points[i * 3 + k];
				if (v < s->mins[k]) s->mins[k] = v;
				if (v > s->maxs[k]) s->maxs[k] = v;
			}
	}
	return true;
}

// ---------------------------------------------------------------- the text

// skips a { } block p is just inside of
static const char *SkipBlock(const char *p)
{
	for (int level = 1; level && (p = COM_Parse(p)) != NULL;)
		if (com_token[0] == '{' && !com_token[1])
			level++;
		else if (com_token[0] == '}' && !com_token[1])
			level--;
	return p;
}

static float Number(const char *s)
{
	float v = (float)atof(s);
	return v > -1e9f && v < 1e9f ? v : 0;   // also turns NaN away
}

static void ParseSolid(srcphys_t *ph, const char *key, const char *value, srcphysolid_t *t)
{
	(void)ph;
	if (!stricmp(key, "index")) t->index = atoi(value);
	else if (!stricmp(key, "name")) Q_strncpy(t->name, value, sizeof(t->name));
	else if (!stricmp(key, "parent")) Q_strncpy(t->parent, value, sizeof(t->parent));
	else if (!stricmp(key, "surfaceprop")) Q_strncpy(t->surfaceprop, value, sizeof(t->surfaceprop));
	else if (!stricmp(key, "mass")) t->mass = Number(value);
	else if (!stricmp(key, "damping")) t->damping = Number(value);
	else if (!stricmp(key, "rotdamping")) t->rotdamping = Number(value);
	else if (!stricmp(key, "inertia")) t->inertia = Number(value);
	else if (!stricmp(key, "volume")) t->volume = Number(value);
}

static void ParseJoint(const char *key, const char *value, srcphyjoint_t *j)
{
	static const char axes[] = "xyz";
	if (!stricmp(key, "parent")) j->parent = atoi(value);
	else if (!stricmp(key, "child")) j->child = atoi(value);
	else if (key[0] && strchr(axes, tolower((unsigned char)key[0]))) {
		int a = (int)(strchr(axes, tolower((unsigned char)key[0])) - axes);
		if (!stricmp(key + 1, "min")) j->min[a] = Number(value);
		else if (!stricmp(key + 1, "max")) j->max[a] = Number(value);
		else if (!stricmp(key + 1, "friction")) j->friction[a] = Number(value);
	}
}

// "3,7": two solids that do collide when the rest of the model's solids don't
static void AddPair(srcphys_t *ph, const char *value)
{
	int a, b;
	if (sscanf(value, "%d ,%d", &a, &b) != 2 || a < 0 || b < 0 || a >= ph->numsolids || b >= ph->numsolids)
		return;
	ph->pairs = Mem_Realloc(ph->pairs, sizeof(*ph->pairs) * (ph->numpairs + 1));
	ph->pairs[ph->numpairs][0] = a;
	ph->pairs[ph->numpairs++][1] = b;
}

static void ParseText(srcphys_t *ph, const char *p)
{
	ph->selfcollide = true;
	while ((p = COM_Parse(p)) != NULL) {
		char block[64];
		Q_strncpy(block, com_token, sizeof(block));
		if ((p = COM_Parse(p)) == NULL)
			break;
		if (com_token[0] != '{' || com_token[1])
			continue;   // a stray pair of words outside any block
		srcphysolid_t solid;
		srcphyjoint_t joint;
		memset(&solid, 0, sizeof(solid));
		memset(&joint, 0, sizeof(joint));
		solid.index = joint.parent = joint.child = -1;
		while ((p = COM_Parse(p)) != NULL && !(com_token[0] == '}' && !com_token[1])) {
			char key[64];
			Q_strncpy(key, com_token, sizeof(key));
			if ((p = COM_Parse(p)) == NULL)
				break;
			if (com_token[0] == '{' && !com_token[1]) {
				p = SkipBlock(p);
				continue;
			}
			if (!stricmp(block, "solid")) ParseSolid(ph, key, com_token, &solid);
			else if (!stricmp(block, "ragdollconstraint")) ParseJoint(key, com_token, &joint);
			else if (!stricmp(block, "editparams")) {
				if (!stricmp(key, "totalmass")) ph->totalmass = Number(com_token);
				else if (!stricmp(key, "rootname")) Q_strncpy(ph->rootname, com_token, sizeof(ph->rootname));
			}
			else if (!stricmp(block, "collisionrules")) {
				if (!stricmp(key, "selfcollisions")) ph->selfcollide = atoi(com_token) != 0;
				else if (!stricmp(key, "collisionpair")) AddPair(ph, com_token);
			}
		}
		if (!stricmp(block, "solid") && solid.index >= 0 && solid.index < ph->numsolids) {
			// the text describes the shape already read: keep that
			srcphysolid_t *t = &ph->solids[solid.index];
			Q_strncpy(t->name, solid.name, sizeof(t->name));
			Q_strncpy(t->parent, solid.parent, sizeof(t->parent));
			Q_strncpy(t->surfaceprop, solid.surfaceprop, sizeof(t->surfaceprop));
			t->mass = solid.mass;
			t->damping = solid.damping;
			t->rotdamping = solid.rotdamping;
			t->inertia = solid.inertia;
			t->volume = solid.volume;
			t->described = true;
		}
		else if (!stricmp(block, "ragdollconstraint") && joint.parent >= 0 && joint.parent < ph->numsolids &&
			joint.child >= 0 && joint.child < ph->numsolids && joint.child != joint.parent) {
			// a joint that can't turn at all on an axis has min above max in some files: make it a range
			for (int a = 0; a < 3; a++)
				if (joint.min[a] > joint.max[a]) {
					float v = joint.min[a];
					joint.min[a] = joint.max[a];
					joint.max[a] = v;
				}
			ph->joints = Mem_Realloc(ph->joints, sizeof(*ph->joints) * (ph->numjoints + 1));
			ph->joints[ph->numjoints++] = joint;
		}
		if (!p)
			break;
	}
}

// ---------------------------------------------------------------- loading

srcphys_t *SrcPhys_Parse(const char *label, const unsigned char *data, size_t size)
{
	const char *why = "it is too small";
	srcphys_t *ph = NULL;
	phybuild_t build;
	memset(&build, 0, sizeof(build));
	if (size < PHY_HEADER_SIZE)
		goto bad;
	int hdrsize = RdInt(data), numsolids = RdInt(data + PHY_NUMSOLIDS);
	why = "its header is damaged";
	// every solid takes at least its size and a surface header
	if (hdrsize < PHY_HEADER_SIZE || (size_t)hdrsize > size || numsolids < 0 ||
		(size_t)numsolids > (size - (size_t)hdrsize) / (4 + SURF_HEADER_SIZE))
		goto bad;
	ph = Mem_Alloc(sizeof(*ph));
	ph->checksum = RdInt(data + PHY_CHECKSUM);
	ph->numsolids = numsolids;
	ph->solids = Mem_Alloc(sizeof(*ph->solids) * (numsolids ? numsolids : 1));
	size_t pos = (size_t)hdrsize;
	for (int i = 0; i < numsolids; i++) {
		why = "a solid points outside the file";
		if (!Inside(size, (long long)pos, 4))
			goto bad;
		int len = RdInt(data + pos);
		pos += 4;
		if (len < 0 || !Inside(size, (long long)pos, (size_t)len))
			goto bad;
		ph->solids[i].index = i;
		if (!ReadSolid(data + pos, (size_t)len, &ph->solids[i], &build, &why))
			goto bad;
		pos += (size_t)len;
	}
	// the loaders NUL-terminate files, but a caller's buffer might not be: parse a copy that is
	char *text = Mem_Alloc(size - pos + 1);
	memcpy(text, data + pos, size - pos);
	ParseText(ph, text);
	Mem_Free(text);
	Mem_Free(build.points);
	Mem_Free(build.counts);
	Mem_Free(build.slot);
	return ph;
bad:
	Con_Warnf("%s can't be used: %s\n", label, why);
	Mem_Free(build.points);
	Mem_Free(build.counts);
	Mem_Free(build.slot);
	SrcPhys_Free(ph);
	return NULL;
}

// "x.mdl" or "x.phy" (or neither) to "x.phy"
static void PhyPath(const char *name, char *out, size_t size)
{
	Q_strncpy(out, name, size);
	char *dot = strrchr(out, '.');
	if (dot && (!stricmp(dot, ".mdl") || !stricmp(dot, ".phy")))
		*dot = 0;
	Q_strncpy(out + strlen(out), ".phy", size - strlen(out));
}

srcphys_t *SrcPhys_Load(const char *name)
{
	char path[MAX_OSPATH];
	PhyPath(name, path, sizeof(path));
	size_t size;
	unsigned char *data = SrcFS_LoadFile(path, &size);
	if (!data)
		return NULL;
	srcphys_t *ph = SrcPhys_Parse(path, data, size);
	Mem_Free(data);
	return ph;
}

void SrcPhys_Free(srcphys_t *ph)
{
	if (!ph)
		return;
	for (int i = 0; i < ph->numsolids; i++) {
		Mem_Free(ph->solids[i].points);
		Mem_Free(ph->solids[i].counts);
	}
	Mem_Free(ph->solids);
	Mem_Free(ph->joints);
	Mem_Free(ph->pairs);
	Mem_Free(ph);
}

// ---------------------------------------------------------------- phyinfo

static void PrintInfo(const char *path, const srcphys_t *ph)
{
	int pieces = 0;
	float mass = 0;
	for (int i = 0; i < ph->numsolids; i++) {
		pieces += ph->solids[i].numpieces;
		mass += ph->solids[i].mass;
	}
	Con_Printf("%s: %d solid%s, %d convex piece%s, %d joint%s, %g kg", path, ph->numsolids, ph->numsolids == 1 ? "" : "s",
		pieces, pieces == 1 ? "" : "s", ph->numjoints, ph->numjoints == 1 ? "" : "s", mass);
	if (ph->totalmass && fabsf(ph->totalmass - mass) > 0.01f)
		Con_Printf(" (the file's total says %g)", ph->totalmass);
	if (!ph->selfcollide)
		Con_Printf(", its solids don't collide with each other%s", ph->numpairs ? " except the pairs below" : "");
	Con_Printf("\n");

	// the model it belongs to, if it's there: the same checksum, and a bone for every solid
	char mdl[MAX_OSPATH];
	Q_strncpy(mdl, path, sizeof(mdl));
	strcpy(mdl + strlen(mdl) - 4, ".mdl");
	const srcmodel_t *s = NULL;
	if (SrcFS_FileExists(mdl)) {
		model_t *m = Mod_ForName(mdl, false);
		s = m && m->type == mod_studio ? m->studio.source : NULL;
	}
	if (s && s->mdlsize >= 12) {
		int sum;
		memcpy(&sum, s->mdl + 8, 4);
		if (sum != ph->checksum)
			Con_Printf("  made for another version of %s (the checksums differ)\n", mdl);
	}
	for (int i = 0; i < ph->numsolids; i++) {
		const srcphysolid_t *t = &ph->solids[i];
		const char *note = "";
		if (!t->described)
			note = "  NOT DESCRIBED in the text";
		else if (s && ph->numsolids > 1) {
			int b = 0;
			while (b < s->numbones && stricmp(s->bones[b].name, t->name))
				b++;
			if (b == s->numbones)
				note = "  NO SUCH BONE in the model";
		}
		Con_Printf("  solid %2d  %-28s %7.2f kg  %2d piece%s %4d points  size %.0f x %.0f x %.0f  %s%s\n", i,
			t->name[0] ? t->name : "(no name)", t->mass, t->numpieces, t->numpieces == 1 ? " " : "s", t->numpoints,
			t->maxs[0] - t->mins[0], t->maxs[1] - t->mins[1], t->maxs[2] - t->mins[2], t->surfaceprop, note);
	}
	for (int i = 0; i < ph->numjoints; i++) {
		const srcphyjoint_t *j = &ph->joints[i];
		Con_Printf("  joint %2d  %s > %s   x %g to %g  y %g to %g  z %g to %g\n", i, ph->solids[j->parent].name,
			ph->solids[j->child].name, j->min[0], j->max[0], j->min[1], j->max[1], j->min[2], j->max[2]);
	}
	for (int i = 0; i < ph->numpairs; i++)
		Con_Printf("  these collide: %s and %s\n", ph->solids[ph->pairs[i][0]].name, ph->solids[ph->pairs[i][1]].name);
}

typedef struct {
	int files, bad, solids, pieces, joints, jointed;
} phytotals_t;

static void CheckOne(const char *name, void *ctx)
{
	phytotals_t *t = ctx;
	t->files++;
	srcphys_t *ph = SrcPhys_Load(name);   // says why if it can't
	if (!ph) {
		t->bad++;
		return;
	}
	t->solids += ph->numsolids;
	t->joints += ph->numjoints;
	t->jointed += ph->numjoints > 0;
	for (int i = 0; i < ph->numsolids; i++) {
		t->pieces += ph->solids[i].numpieces;
		if (!ph->solids[i].numpieces)
			Con_Printf("%s: solid %d has no convex pieces\n", name, i);
		if (!ph->solids[i].described)
			Con_Printf("%s: solid %d isn't described in the text\n", name, i);
	}
	SrcPhys_Free(ph);
}

static void PhyInfo_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: phyinfo <model>   e.g. phyinfo hl2:models/combine_soldier.mdl\n"
			"       phyinfo <pattern> reads every match and counts, e.g. phyinfo hl2:models/*.phy\n");
		return;
	}
	const char *arg = Cmd_Argv(1);
	if (strchr(arg, '*') || strchr(arg, '?')) {
		phytotals_t t;
		memset(&t, 0, sizeof(t));
		SrcFS_Find(arg, CheckOne, &t);
		Con_Printf("%d physics files: %d read, %d refused; %d solids, %d convex pieces, %d joints in %d jointed models\n",
			t.files, t.files - t.bad, t.bad, t.solids, t.pieces, t.joints, t.jointed);
		return;
	}
	char path[MAX_OSPATH];
	PhyPath(strchr(arg, '/') ? arg : va("models/%s", arg), path, sizeof(path));
	if (!SrcFS_FileExists(path)) {
		Con_Printf("%s: not found (Half-Life 2's files need the hl2: prefix)\n", path);
		return;
	}
	srcphys_t *ph = SrcPhys_Load(path);
	if (ph)
		PrintInfo(path, ph);
	SrcPhys_Free(ph);
}

void SrcPhys_Init(void)
{
	Cmd_AddCommand("phyinfo", PhyInfo_f, "phyinfo <model or pattern> - lists a Source model's physics solids, masses and joints");
}
