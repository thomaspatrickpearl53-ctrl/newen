// newen - the engine functions table handed to the game DLL (enginefuncs_t)
//
// Most functions are complete. Sound, networking (messages, deltas, events) and save/restore arrive
// with the client in later steps; until then they are accepted and do nothing.
#include "server.h"
#include <sys/stat.h>

// ---------------------------------------------------------------- helpers

static void SetGlobalTrace(const trace_t *t)
{
	gGlobals.trace_allsolid = (float)t->allsolid;
	gGlobals.trace_startsolid = (float)t->startsolid;
	gGlobals.trace_fraction = t->fraction;
	gGlobals.trace_inwater = (float)t->inwater;
	gGlobals.trace_inopen = (float)t->inopen;
	VectorCopy(t->endpos, gGlobals.trace_endpos);
	VectorCopy(t->plane.normal, gGlobals.trace_plane_normal);
	gGlobals.trace_plane_dist = t->plane.dist;
	gGlobals.trace_ent = t->ent ? t->ent : sv.edicts;
	gGlobals.trace_hitgroup = t->hitgroup;
}

static void ToTraceResult(const trace_t *t, TraceResult *tr)
{
	tr->fAllSolid = t->allsolid;
	tr->fStartSolid = t->startsolid;
	tr->fInOpen = t->inopen;
	tr->fInWater = t->inwater;
	tr->flFraction = t->fraction;
	VectorCopy(t->endpos, tr->vecEndPos);
	tr->flPlaneDist = t->plane.dist;
	VectorCopy(t->plane.normal, tr->vecPlaneNormal);
	tr->pHit = t->ent ? t->ent : sv.edicts;
	tr->iHitgroup = t->hitgroup;
	SetGlobalTrace(t);
}

static const vec3_t vec3_origin = { 0, 0, 0 };

static float AngleMod(float a)
{
	a = fmodf(a, 360.0f);
	return a < 0 ? a + 360.0f : a;
}

void SV_MakeVectors(const vec3_t angles)
{
	AngleVectors(angles, gGlobals.v_forward, gGlobals.v_right, gGlobals.v_up);
}

float SV_VecToYaw(const vec3_t v)
{
	if (v[0] == 0 && v[1] == 0)
		return 0;
	float yaw = atan2f(v[1], v[0]) * 180.0f / M_PI_F;
	return yaw < 0 ? yaw + 360.0f : yaw;
}

// the edict as the renderer sees it, frozen at its current frame (for bone queries)
static qboolean EdictToRentity(const edict_t *e, rentity_t *r)
{
	model_t *m = SV_ModelForIndex(e->v.modelindex);
	if (!m || m->type != mod_studio)
		return false;
	memset(r, 0, sizeof(*r));
	r->model = m;
	VectorCopy(e->v.origin, r->origin);
	VectorCopy(e->v.angles, r->angles);
	r->sequence = e->v.sequence;
	r->frame = e->v.frame;
	r->framerate = 0;
	r->animtime = r_refdef.time;
	r->body = e->v.body;
	r->skin = e->v.skin;
	memcpy(r->controller, e->v.controller, 4);
	memcpy(r->blending, e->v.blending, 2);
	return true;
}

// ---------------------------------------------------------------- info strings ("\key\value\key\value")

const char *SV_InfoValueForKey(const char *s, const char *key)
{
	static char value[4][256];
	static int which;
	which = (which + 1) & 3;
	char *out = value[which];
	if (*s == '\\')
		s++;
	while (*s) {
		char k[256];
		int n = 0;
		while (*s && *s != '\\' && n < 255)
			k[n++] = *s++;
		k[n] = 0;
		if (*s == '\\')
			s++;
		n = 0;
		while (*s && *s != '\\' && n < 255)
			out[n++] = *s++;
		out[n] = 0;
		if (!stricmp(k, key))
			return out;
		if (*s == '\\')
			s++;
	}
	out[0] = 0;
	return out;
}

static void Info_RemoveKey(char *s, const char *key)
{
	char *start = s;
	while (*s) {
		char *pair = s;
		if (*s == '\\')
			s++;
		char k[256];
		int n = 0;
		while (*s && *s != '\\' && n < 255)
			k[n++] = *s++;
		k[n] = 0;
		if (*s == '\\')
			s++;
		while (*s && *s != '\\')
			s++;
		if (!stricmp(k, key)) {
			memmove(pair, s, strlen(s) + 1);
			return;
		}
	}
	(void)start;
}

static void Info_SetValueForKey(char *s, const char *key, const char *value, size_t maxsize)
{
	if (strchr(key, '\\') || strchr(value, '\\'))
		return;
	Info_RemoveKey(s, key);
	if (!*value)
		return;
	size_t len = strlen(s), add = strlen(key) + strlen(value) + 2;
	if (len + add + 1 > maxsize) {
		Con_DPrintf("Info string full, can't add %s\n", key);
		return;
	}
	snprintf(s + len, maxsize - len, "\\%s\\%s", key, value);
}

// ---------------------------------------------------------------- models and precache

static int PF_PrecacheModel(const char *s) { return SV_PrecacheModel(s); }
static int PF_PrecacheSound(const char *s) { return SV_PrecacheSound(s); }

void SV_SetModel(edict_t *e, const char *m)
{
	if (!e || !m)
		return;
	int idx = SV_ModelIndex(m);
	if (!idx && *m) {
		Con_DPrintf("SetModel: %s wasn't precached\n", m);
		idx = SV_PrecacheModel(m);
	}
	e->v.model = idx ? SV_MakeString(sv.model_precache[idx]) : 0;
	e->v.modelindex = idx;
	model_t *mod = SV_ModelForIndex(idx);
	if (mod)
		SV_SetMinMaxSize(e, mod->mins, mod->maxs);
	else
		SV_SetMinMaxSize(e, vec3_origin, vec3_origin);
}

static int PF_ModelIndex(const char *m)
{
	int idx = SV_ModelIndex(m);
	if (!idx && m && *m)
		Con_DPrintf("ModelIndex: %s wasn't precached\n", m);
	return idx;
}

static int PF_ModelFrames(int modelIndex)
{
	model_t *m = SV_ModelForIndex(modelIndex);
	if (!m)
		return 1;
	if (m->type == mod_sprite)
		return m->sprite.numframes;
	if (m->type == mod_studio) {
		// studio "frames" are body combinations
		const studiohdr_t *h = m->studio.hdr;
		const mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
		int count = 1;
		for (int i = 0; i < h->numbodyparts; i++)
			count *= bp[i].nummodels > 0 ? bp[i].nummodels : 1;
		return count;
	}
	return 1;
}

static void PF_SetSize(edict_t *e, const float *mins, const float *maxs)
{
	if (e)
		SV_SetMinMaxSize(e, mins, maxs);
}

static void PF_ChangeLevel(const char *s1, const char *s2)
{
	static double last;
	if (sv.time - last < 1)
		return;
	last = sv.time;
	Con_Printf("The game asked for a level change to %s (landmark %s); level changes arrive in step 6\n",
		s1 ? s1 : "?", s2 ? s2 : "none");
}

static void PF_GetSpawnParms(edict_t *ent) {}
static void PF_SaveSpawnParms(edict_t *ent) {}

static float PF_VecToYaw(const float *v) { return SV_VecToYaw(v); }

static void PF_VecToAngles(const float *in, float *out)
{
	float yaw, pitch;
	if (in[0] == 0 && in[1] == 0) {
		yaw = 0;
		pitch = in[2] > 0 ? 90.0f : 270.0f;
	} else {
		yaw = atan2f(in[1], in[0]) * 180.0f / M_PI_F;
		if (yaw < 0)
			yaw += 360;
		float forward = sqrtf(in[0] * in[0] + in[1] * in[1]);
		pitch = atan2f(in[2], forward) * 180.0f / M_PI_F;
		if (pitch < 0)
			pitch += 360;
	}
	out[0] = pitch;
	out[1] = yaw;
	out[2] = 0;
}

static void PF_MoveToOrigin(edict_t *ent, const float *goal, float dist, int movetype)
{
	if (ent)
		SV_MoveToOrigin(ent, goal, dist, movetype);
}

static void PF_ChangeYaw(edict_t *ent)
{
	float current = AngleMod(ent->v.angles[1]);
	float ideal = ent->v.ideal_yaw, speed = ent->v.yaw_speed;
	if (current == ideal)
		return;
	float move = ideal - current;
	if (ideal > current) {
		if (move >= 180) move -= 360;
	} else {
		if (move <= -180) move += 360;
	}
	if (move > 0) {
		if (move > speed) move = speed;
	} else {
		if (move < -speed) move = -speed;
	}
	ent->v.angles[1] = AngleMod(current + move);
}

static void PF_ChangePitch(edict_t *ent)
{
	float current = AngleMod(ent->v.angles[0]);
	float ideal = ent->v.idealpitch, speed = ent->v.pitch_speed;
	if (current == ideal)
		return;
	float move = ideal - current;
	if (ideal > current) {
		if (move >= 180) move -= 360;
	} else {
		if (move <= -180) move += 360;
	}
	if (move > 0) {
		if (move > speed) move = speed;
	} else {
		if (move < -speed) move = -speed;
	}
	ent->v.angles[0] = AngleMod(current + move);
}

// ---------------------------------------------------------------- finding entities

static int StringFieldOffset(const char *field)
{
	static const struct { const char *name; int ofs; } fields[] = {
		{ "classname", offsetof(entvars_t, classname) }, { "globalname", offsetof(entvars_t, globalname) },
		{ "model", offsetof(entvars_t, model) }, { "target", offsetof(entvars_t, target) },
		{ "targetname", offsetof(entvars_t, targetname) }, { "netname", offsetof(entvars_t, netname) },
		{ "message", offsetof(entvars_t, message) }, { "noise", offsetof(entvars_t, noise) },
		{ "noise1", offsetof(entvars_t, noise1) }, { "noise2", offsetof(entvars_t, noise2) },
		{ "noise3", offsetof(entvars_t, noise3) },
	};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		if (!strcmp(fields[i].name, field))
			return fields[i].ofs;
	return -1;
}

static edict_t *PF_FindEntityByString(edict_t *start, const char *field, const char *value)
{
	int ofs = StringFieldOffset(field);
	if (ofs < 0 || !value) {
		Con_DPrintf("FindEntityByString: unknown field %s\n", field ? field : "(null)");
		return sv.edicts;
	}
	int first = start ? NUM_FOR_EDICT(start) + 1 : 0;
	for (int i = first; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free)
			continue;
		string_t s = *(string_t *)((unsigned char *)&e->v + ofs);
		if (s && !strcmp(SV_String(s), value))
			return e;
	}
	return sv.edicts;   // the game checks FNullEnt, which treats the world as "none"
}

static int PF_GetEntityIllum(edict_t *e)
{
	if (!e)
		return 0;
	vec3_t c;
	R_LightPoint(e->v.origin, c);
	float m = c[0] > c[1] ? c[0] : c[1];
	if (c[2] > m) m = c[2];
	int v = (int)(m * 255);
	return v > 255 ? 255 : v;
}

static edict_t *PF_FindEntityInSphere(edict_t *start, const float *org, float rad)
{
	int first = start ? NUM_FOR_EDICT(start) + 1 : 1;
	float r2 = rad * rad;
	for (int i = first; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.classname)
			continue;
		// distance to the nearest point of the entity's box
		float d2 = 0;
		for (int j = 0; j < 3; j++) {
			float v = org[j];
			if (v < e->v.absmin[j]) v = e->v.absmin[j] - v;
			else if (v > e->v.absmax[j]) v = v - e->v.absmax[j];
			else v = 0;
			d2 += v * v;
		}
		if (d2 <= r2)
			return e;
	}
	return sv.edicts;
}

static qboolean EdictInPVS(const edict_t *e, const unsigned char *pvs)
{
	if (e->num_leafs > MAX_ENT_LEAFS)
		return true;   // too big to list: assume visible
	for (int i = 0; i < e->num_leafs; i++) {
		int l = e->leafnums[i];
		if (l >= 0 && (pvs[l >> 3] & (1 << (l & 7))))
			return true;
	}
	return false;
}

static const unsigned char *PVSForPoint(const vec3_t p)
{
	return BSP_LeafPVS(BSP_PointInLeaf(p));
}

static edict_t *PF_FindClientInPVS(edict_t *ed)
{
	edict_t *pl = SV_LocalPlayer();
	if (!pl || !ed || (pl->v.flags & FL_NOTARGET))
		return sv.edicts;
	vec3_t view;
	VectorAdd(ed->v.origin, ed->v.view_ofs, view);
	if (!EdictInPVS(pl, PVSForPoint(view)))
		return sv.edicts;
	return pl;
}

static edict_t *PF_EntitiesInPVS(edict_t *pplayer)
{
	edict_t *chain = sv.edicts;
	if (!pplayer)
		return chain;
	vec3_t view;
	VectorAdd(pplayer->v.origin, pplayer->v.view_ofs, view);
	// the PVS buffer is shared; copy it before walking entities
	int row = (world.submodels[0].visleafs + 7) >> 3;
	unsigned char *pvs = Mem_Alloc(row + 4);
	memcpy(pvs, PVSForPoint(view), row);
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !e->v.modelindex)
			continue;
		if (!EdictInPVS(e, pvs))
			continue;
		e->v.chain = chain;
		chain = e;
	}
	Mem_Free(pvs);
	return chain;
}

static void PF_MakeVectors(const float *v) { SV_MakeVectors(v); }

static void PF_AngleVectors(const float *v, float *f, float *r, float *u)
{
	AngleVectors(v, f, r, u);
}

// ---------------------------------------------------------------- creating and removing

static edict_t *PF_CreateEntity(void) { return ED_Alloc(); }

static void PF_RemoveEntity(edict_t *e)
{
	if (e && e != sv.edicts)
		ED_Free(e);
}

edict_t *SV_CreateNamedEntity(const char *name)
{
	ENTITYINIT init = SV_GetEntityInit(name);
	if (!init) {
		Con_DPrintf("CreateNamedEntity: no class %s\n", name);
		return NULL;
	}
	edict_t *e = ED_Alloc();
	e->v.classname = SV_AllocString(name);
	init(&e->v);
	return e;
}

static edict_t *PF_CreateNamedEntity(int className)
{
	const char *name = SV_String(className);
	ENTITYINIT init = SV_GetEntityInit(name);
	if (!init) {
		Con_DPrintf("CreateNamedEntity: no class %s\n", name);
		return NULL;
	}
	edict_t *e = ED_Alloc();
	e->v.classname = className;
	init(&e->v);
	return e;
}

// static entities become part of the scenery: drawn, never thought about again
static void PF_MakeStatic(edict_t *ent)
{
	model_t *m = SV_ModelForIndex(ent->v.modelindex);
	if (!m || m->type == mod_brush)
		return;   // brush statics stay as they are
	rentity_t *r = R_AddEntity();
	r->model = m;
	VectorCopy(ent->v.origin, r->origin);
	VectorCopy(ent->v.angles, r->angles);
	r->sequence = ent->v.sequence;
	r->frame = ent->v.frame;
	r->framerate = m->type == mod_studio ? ent->v.framerate : 0;
	r->body = ent->v.body;
	r->skin = ent->v.skin;
	r->rendermode = ent->v.rendermode;
	r->renderamt = r->rendermode == kRenderNormal ? 255 : (int)ent->v.renderamt;
	for (int i = 0; i < 3; i++)
		r->rendercolor[i] = (int)ent->v.rendercolor[i];
	r->scale = ent->v.scale;
	ED_Free(ent);
}

static int PF_EntIsOnFloor(edict_t *e) { return e ? SV_CheckBottom(e) : 0; }
static int PF_DropToFloor(edict_t *e) { return e ? SV_DropToFloor(e) : 0; }

static int PF_WalkMove(edict_t *ent, float yaw, float dist, int mode)
{
	if (!ent || !(ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return 0;
	float r = DEG2RAD(yaw);
	vec3_t move = { cosf(r) * dist, sinf(r) * dist, 0 };
	return SV_movestep(ent, move, true, mode);
}

static void PF_SetOrigin(edict_t *e, const float *org)
{
	if (!e)
		return;
	VectorCopy(org, e->v.origin);
	SV_LinkEdict(e, false);
}

// sound arrives in step 6
static void PF_EmitSound(edict_t *entity, int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch) {}
static void PF_EmitAmbientSound(edict_t *entity, float *pos, const char *samp, float vol, float attenuation, int fFlags, int pitch) {}

// ---------------------------------------------------------------- traces

static void PF_TraceLine(const float *v1, const float *v2, int fNoMonsters, edict_t *skip, TraceResult *ptr)
{
	trace_t t = SV_Move(v1, vec3_origin, vec3_origin, v2, fNoMonsters, skip);
	ToTraceResult(&t, ptr);
}

static void PF_TraceToss(edict_t *pent, edict_t *ignore, TraceResult *ptr)
{
	// follow the entity's arc in small steps until it hits something
	vec3_t org, vel;
	VectorCopy(pent->v.origin, org);
	VectorCopy(pent->v.velocity, vel);
	float grav = (pent->v.gravity ? pent->v.gravity : 1.0f) * sv_gravity->value;
	trace_t t;
	memset(&t, 0, sizeof(t));
	t.fraction = 1;
	for (int i = 0; i < 200; i++) {
		vel[2] -= grav * 0.05f;
		vec3_t end;
		VectorMA(org, 0.05f, vel, end);
		t = SV_Move(org, pent->v.mins, pent->v.maxs, end, MOVE_NORMAL, pent);
		VectorCopy(t.endpos, org);
		if (t.fraction < 1 || t.startsolid)
			break;
		if (t.ent && t.ent == ignore)
			break;
	}
	ToTraceResult(&t, ptr);
}

static int PF_TraceMonsterHull(edict_t *e, const float *v1, const float *v2, int fNoMonsters, edict_t *skip, TraceResult *ptr)
{
	trace_t t = SV_Move(v1, e->v.mins, e->v.maxs, v2, fNoMonsters, skip);
	if (ptr)
		ToTraceResult(&t, ptr);
	else
		SetGlobalTrace(&t);
	return t.allsolid || t.fraction != 1.0f;
}

static void PF_TraceHull(const float *v1, const float *v2, int fNoMonsters, int hullNumber, edict_t *skip, TraceResult *ptr)
{
	if (hullNumber < 0 || hullNumber >= MAX_HULLS)
		hullNumber = 0;
	trace_t t = SV_Move(v1, hull_mins[hullNumber], hull_maxs[hullNumber], v2, fNoMonsters, skip);
	ToTraceResult(&t, ptr);
}

static void PF_TraceModel(const float *v1, const float *v2, int hullNumber, edict_t *pent, TraceResult *ptr)
{
	if (hullNumber < 0 || hullNumber >= MAX_HULLS)
		hullNumber = 0;
	int oldsolid = pent->v.solid;
	model_t *m = SV_ModelForIndex(pent->v.modelindex);
	if (m && m->type == mod_brush)
		pent->v.solid = SOLID_BSP;
	trace_t t = SV_ClipMoveToEntity(pent, v1, hull_mins[hullNumber], hull_maxs[hullNumber], v2);
	pent->v.solid = oldsolid;
	ToTraceResult(&t, ptr);
}

// the texture of the surface a line hits (footstep and impact sounds)
static msurface_t *SurfaceAtPoint(mnode_t *node, const vec3_t start, const vec3_t end, int depth)
{
	if (node->contents < 0 || depth > 2048)
		return NULL;
	float front = DotProduct(start, node->plane->normal) - node->plane->dist;
	float back = DotProduct(end, node->plane->normal) - node->plane->dist;
	int side = front < 0;
	if ((back < 0) == side)
		return SurfaceAtPoint(node->children[side], start, end, depth + 1);
	float frac = front / (front - back);
	vec3_t mid;
	for (int i = 0; i < 3; i++)
		mid[i] = start[i] + (end[i] - start[i]) * frac;
	msurface_t *s = SurfaceAtPoint(node->children[side], start, mid, depth + 1);
	if (s)
		return s;
	for (int i = 0; i < node->numsurfaces; i++) {
		msurface_t *surf = &world.surfaces[node->firstsurface + i];
		if (!surf->numverts)
			continue;
		const mtexinfo_t *ti = surf->texinfo;
		int ds = (int)(DotProduct(mid, ti->vecs[0]) + ti->vecs[0][3]);
		int dt = (int)(DotProduct(mid, ti->vecs[1]) + ti->vecs[1][3]);
		if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
			continue;
		if (ds - surf->texturemins[0] > surf->extents[0] || dt - surf->texturemins[1] > surf->extents[1])
			continue;
		return surf;
	}
	return SurfaceAtPoint(node->children[!side], mid, end, depth + 1);
}

const char *SV_TraceTexture(edict_t *texent, const float *v1, const float *v2)
{
	if (!world.numnodes)
		return NULL;
	int sub = 0;
	vec3_t start, end;
	VectorCopy(v1, start);
	VectorCopy(v2, end);
	if (texent && texent != sv.edicts) {
		model_t *m = SV_ModelForIndex(texent->v.modelindex);
		if (!m || m->type != mod_brush)
			return NULL;
		sub = m->submodel;
		VectorSubtract(start, texent->v.origin, start);
		VectorSubtract(end, texent->v.origin, end);
	}
	int head = world.submodels[sub].headnode;
	if (head < 0 || head >= world.numnodes)
		return NULL;
	msurface_t *s = SurfaceAtPoint(&world.nodes[head], start, end, 0);
	return s ? s->texinfo->texture->name : NULL;
}

static void PF_TraceSphere(const float *v1, const float *v2, int fNoMonsters, float radius, edict_t *skip, TraceResult *ptr)
{
	// GoldSrc never implemented this; a line is the closest thing
	PF_TraceLine(v1, v2, fNoMonsters, skip, ptr);
}

static void PF_GetAimVector(edict_t *ent, float speed, float *out)
{
	vec3_t f;
	AngleVectors(ent->v.v_angle, f, NULL, NULL);
	VectorCopy(f, out);
}

// ---------------------------------------------------------------- commands and console

// The game's own commands have a buffer of their own: ServerExecute must run only what the game
// queued, not whatever the user typed after "map" (that could even be "quit")
static strbuf_t server_cmds;

static void PF_ServerCommand(const char *str)
{
	if (str)
		SB_Append(&server_cmds, str, strlen(str));
}

void SV_ExecuteServerCommands(void)
{
	int guard = 0;
	while (server_cmds.len && ++guard < 10000) {
		size_t i, quotes = 0;
		for (i = 0; i < server_cmds.len; i++) {
			char c = server_cmds.data[i];
			if (c == '"')
				quotes++;
			if ((!(quotes & 1) && c == ';') || c == '\n' || c == '\r')
				break;
		}
		char *line = Mem_Alloc(i + 1);
		memcpy(line, server_cmds.data, i);
		if (i < server_cmds.len)
			i++;
		memmove(server_cmds.data, server_cmds.data + i, server_cmds.len - i);
		server_cmds.len -= i;
		server_cmds.data[server_cmds.len] = 0;
		if (developer->value > 1)
			Con_Printf("game command: %s\n", line);
		Cmd_ExecuteString(line);
		Mem_Free(line);
	}
}

static void PF_ServerExecute(void) { SV_ExecuteServerCommands(); }

static void PF_ClientCommand(edict_t *e, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (e && e == SV_LocalPlayer())
		Cbuf_AddText(buf);
}

static void PF_ParticleEffect(const float *org, const float *dir, float color, float count) {}
static void PF_LightStyle(int style, const char *val) { R_SetLightStyle(style, val); }

static char **decal_names;
static int decal_count;

static int PF_DecalIndex(const char *name)
{
	for (int i = 0; i < decal_count; i++)
		if (!stricmp(decal_names[i], name))
			return i;
	decal_names = Mem_Realloc(decal_names, sizeof(char *) * (decal_count + 1));
	decal_names[decal_count] = Mem_Strdup(name);
	return decal_count++;
}

static int PF_PointContents(const float *v) { return SV_PointContents(v); }

// ---------------------------------------------------------------- messages (sent to clients in step 5)

// Messages are built in a buffer and, if the local player should get them, queued for the client
// DLL (there's one client, in this process)
void CL_QueueMessage(int type, const unsigned char *data, int len);
static int msg_dest, msg_type, msg_active;
static edict_t *msg_ent;
static unsigned char msg_buf[2048];
static int msg_len;

static void PF_MessageBegin(int dest, int type, const float *origin, edict_t *ed)
{
	if (msg_active)
		Con_DPrintf("MessageBegin: a message (%d) was still open\n", msg_type);
	msg_dest = dest;
	msg_type = type;
	msg_ent = ed;
	msg_len = 0;
	msg_active = 1;
}

static void PF_MessageEnd(void)
{
	if (!msg_active)
		return;
	msg_active = 0;
	edict_t *pl = SV_LocalPlayer();
	qboolean deliver;
	switch (msg_dest) {
	case MSG_ONE:
	case MSG_ONE_UNRELIABLE:
		deliver = pl && msg_ent == pl;
		break;
	case MSG_SPEC:
		deliver = false;
		break;
	default:   // broadcast, all, init, PVS/PAS (one player: always in range)
		deliver = true;
		break;
	}
	if (deliver)
		CL_QueueMessage(msg_type, msg_buf, msg_len);
}

static void MsgWrite(const void *data, int n)
{
	if (msg_len + n > (int)sizeof(msg_buf)) {
		Con_DPrintf("Message %d is too long\n", msg_type);
		return;
	}
	memcpy(msg_buf + msg_len, data, n);
	msg_len += n;
}

static void PF_WriteByte(int v)   { unsigned char b = (unsigned char)v; MsgWrite(&b, 1); }
static void PF_WriteChar(int v)   { signed char c = (signed char)v; MsgWrite(&c, 1); }
static void PF_WriteShort(int v)  { short s = (short)v; MsgWrite(&s, 2); }
static void PF_WriteLong(int v)   { MsgWrite(&v, 4); }
static void PF_WriteAngle(float v) { PF_WriteByte((int)(v * 256.0f / 360.0f) & 255); }
static void PF_WriteCoord(float v) { PF_WriteShort((int)(v * 8.0f)); }
static void PF_WriteString(const char *s) { if (!s) s = ""; MsgWrite(s, (int)strlen(s) + 1); }
static void PF_WriteEntity(int v) { PF_WriteShort(v); }

// ---------------------------------------------------------------- cvars

static void PF_CVarRegister(cvar_t *var) { Cvar_RegisterExternal(var); }
static float PF_CVarGetFloat(const char *n) { cvar_t *v = Cvar_Find(n); return v ? v->value : 0.0f; }
static const char *PF_CVarGetString(const char *n) { cvar_t *v = Cvar_Find(n); return v ? v->string : ""; }
static void PF_CVarSetFloat(const char *n, float f) { if (Cvar_Find(n)) Cvar_SetValue(n, f); }
static void PF_CVarSetString(const char *n, const char *s) { if (Cvar_Find(n)) Cvar_Set(n, s); }

static cvar_t *PF_CVarGetPointer(const char *n)
{
	cvar_t *v = Cvar_Find(n);
	if (!v)
		Con_DPrintf("The game looked for cvar %s, which doesn't exist\n", n);
	return v;
}

static void PF_AlertMessage(ALERT_TYPE type, const char *fmt, ...)
{
	if ((type == at_console || type == at_aiconsole) && developer->value < (type == at_aiconsole ? 2 : 1))
		return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (type == at_warning)
		Con_Warnf("%s", buf);
	else if (type == at_error)
		Con_Errorf("%s", buf);
	else
		Con_Printf("%s", buf);
}

// the FILE belongs to the DLL's C runtime, which isn't ours; only used for debug dumps
static void PF_EngineFprintf(void *pfile, const char *fmt, ...) {}

// ---------------------------------------------------------------- edicts and private data

static void *PF_PvAllocEntPrivateData(edict_t *e, int cb)
{
	if (!e)
		return NULL;
	if (e->pvPrivateData) {
		if (gNewDLLFunctions.pfnOnFreeEntPrivateData)
			gNewDLLFunctions.pfnOnFreeEntPrivateData(e);
		Mem_Free(e->pvPrivateData);
	}
	e->pvPrivateData = Mem_Alloc(cb > 0 ? cb : 1);   // zeroed: the game relies on it
	return e->pvPrivateData;
}

static void *PF_PvEntPrivateData(edict_t *e) { return e ? e->pvPrivateData : NULL; }

static void PF_FreeEntPrivateData(edict_t *e)
{
	if (!e || !e->pvPrivateData)
		return;
	if (gNewDLLFunctions.pfnOnFreeEntPrivateData)
		gNewDLLFunctions.pfnOnFreeEntPrivateData(e);
	Mem_Free(e->pvPrivateData);
	e->pvPrivateData = NULL;
}

static const char *PF_SzFromIndex(int i) { return SV_String(i); }
static int PF_AllocString(const char *s) { return SV_AllocString(s); }
static entvars_t *PF_GetVarsOfEnt(edict_t *e) { return e ? &e->v : NULL; }
static edict_t *PF_PEntityOfEntOffset(int ofs) { return (edict_t *)((unsigned char *)sv.edicts + ofs); }
static int PF_EntOffsetOfPEntity(const edict_t *e) { return e ? (int)((const unsigned char *)e - (const unsigned char *)sv.edicts) : 0; }

static int PF_IndexOfEdict(const edict_t *e)
{
	if (!e || !sv.edicts)
		return 0;
	int n = NUM_FOR_EDICT(e);
	return (n >= 0 && n < sv.max_edicts) ? n : 0;
}

static edict_t *PF_PEntityOfEntIndex(int i)
{
	if (!sv.edicts || i < 0 || i >= sv.max_edicts)
		return NULL;
	edict_t *e = EDICT_NUM(i);
	if (e->free && i > sv.maxclients)
		return NULL;
	return e;
}

static edict_t *PF_PEntityOfEntIndexAllEntities(int i)
{
	if (!sv.edicts || i < 0 || i >= sv.max_edicts)
		return NULL;
	return EDICT_NUM(i);
}

static edict_t *PF_FindEntityByVars(entvars_t *vars)
{
	for (int i = 0; i < sv.num_edicts; i++)
		if (&EDICT_NUM(i)->v == vars)
			return EDICT_NUM(i);
	return NULL;
}

static void *PF_GetModelPtr(edict_t *e)
{
	if (!e)
		return NULL;
	return Mod_StudioHeader(SV_ModelForIndex(e->v.modelindex));
}

static char **usermsg_names;
static int usermsg_count;

const char *SV_UserMsgName(int type)
{
	int i = type - 64;
	return (i >= 0 && i < usermsg_count) ? usermsg_names[i] : NULL;
}

static int PF_RegUserMsg(const char *name, int size)
{
	for (int i = 0; i < usermsg_count; i++)
		if (!strcmp(usermsg_names[i], name))
			return 64 + i;
	if (64 + usermsg_count > 255) {
		Con_Warnf("Too many user messages, %s dropped\n", name);
		return 0;
	}
	usermsg_names = Mem_Realloc(usermsg_names, sizeof(char *) * (usermsg_count + 1));
	usermsg_names[usermsg_count] = Mem_Strdup(name);
	return 64 + usermsg_count++;
}

static void PF_AnimationAutomove(const edict_t *e, float t) {}

static void PF_GetBonePosition(const edict_t *e, int bone, float *org, float *angles)
{
	rentity_t r;
	if (!e || !EdictToRentity(e, &r))
		return;
	float (*bones)[3][4];
	int n = R_StudioSetupBones(&r, &bones);
	if (bone < 0 || bone >= n)
		return;
	for (int i = 0; i < 3; i++)
		org[i] = bones[bone][i][3];
}

static void PF_GetAttachment(const edict_t *e, int att, float *org, float *angles)
{
	rentity_t r;
	if (!e || !EdictToRentity(e, &r))
		return;
	const studiohdr_t *h = r.model->studio.hdr;
	if (att < 0 || att >= h->numattachments)
		return;
	const mstudioattachment_t *a = STUDIO_PTR(h, h->attachmentindex, mstudioattachment_t) + att;
	float (*bones)[3][4];
	int n = R_StudioSetupBones(&r, &bones);
	if (a->bone < 0 || a->bone >= n)
		return;
	for (int i = 0; i < 3; i++)
		org[i] = DotProduct(a->org, bones[a->bone][i]) + bones[a->bone][i][3];
}

static unsigned PF_FunctionFromName(const char *name)
{
	return (unsigned)(uintptr_t)SV_GetProcAddress(name);
}

static const char *PF_NameForFunction(unsigned f)
{
	const char *n = SV_NameForFunction(f);
	return n ? n : "";
}

static void PF_ClientPrintf(edict_t *e, PRINT_TYPE type, const char *msg) { if (msg) Con_Printf("%s", msg); }
static void PF_ServerPrint(const char *msg) { if (msg) Con_Printf("%s", msg); }
static const char *PF_Cmd_Args(void) { return Cmd_Args(); }
static const char *PF_Cmd_Argv(int i) { return Cmd_Argv(i); }
static int PF_Cmd_Argc(void) { return Cmd_Argc(); }

// ---------------------------------------------------------------- CRC32 and random numbers

static unsigned crc_table[256];

static void PF_CRC32_Init(CRC32_t *crc)
{
	if (!crc_table[1])
		for (unsigned i = 0; i < 256; i++) {
			unsigned c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	*crc = 0xFFFFFFFFu;
}
static void PF_CRC32_ProcessByte(CRC32_t *crc, unsigned char ch)
{
	if (!crc_table[1]) { CRC32_t t; PF_CRC32_Init(&t); }
	*crc = crc_table[(*crc ^ ch) & 0xff] ^ (*crc >> 8);
}
static void PF_CRC32_ProcessBuffer(CRC32_t *crc, void *p, int len)
{
	const unsigned char *b = p;
	for (int i = 0; i < len; i++)
		PF_CRC32_ProcessByte(crc, b[i]);
}
static CRC32_t PF_CRC32_Final(CRC32_t crc) { return crc ^ 0xFFFFFFFFu; }

static unsigned rng_state = 0x9E3779B9u;
static unsigned NextRandom(void)
{
	// xorshift32: fast, and good enough for gameplay
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static int PF_RandomLong(int lo, int hi)
{
	if (hi <= lo)
		return lo;
	unsigned range = (unsigned)(hi - lo) + 1;
	return lo + (int)(range ? NextRandom() % range : NextRandom());
}

static float PF_RandomFloat(float lo, float hi)
{
	float u = (NextRandom() >> 8) / 16777216.0f;
	return lo + (hi - lo) * u;
}

static void PF_SetView(const edict_t *client, const edict_t *viewent) {}
static float PF_Time(void) { return (float)host_realtime; }
static void PF_CrosshairAngle(const edict_t *client, float pitch, float yaw) {}

static unsigned char *PF_LoadFileForMe(const char *filename, int *len)
{
	size_t n = 0;
	unsigned char *data = FS_LoadFile(filename, &n);
	if (len)
		*len = data ? (int)n : 0;
	return data;
}

static void PF_FreeFile(void *buffer) { Mem_Free(buffer); }

static void PF_EndSection(const char *name)
{
	Con_Printf("End of section %s\n", name ? name : "");
}

static int PF_CompareFileTime(const char *f1, const char *f2, int *cmp)
{
	char p1[MAX_OSPATH], p2[MAX_OSPATH];
	struct _stat s1, s2;
	*cmp = 0;
	if (!FS_FullPath(f1, p1, sizeof(p1)) || !FS_FullPath(f2, p2, sizeof(p2)) || _stat(p1, &s1) || _stat(p2, &s2))
		return 0;
	*cmp = s1.st_mtime < s2.st_mtime ? -1 : s1.st_mtime > s2.st_mtime ? 1 : 0;
	return 1;
}

static void PF_GetGameDir(char *out) { strcpy(out, FS_GameDir()); }
static void PF_Cvar_RegisterVariable(cvar_t *var) { Cvar_RegisterExternal(var); }
static void PF_FadeClientVolume(const edict_t *e, int fadePercent, int fadeOutSeconds, int holdTime, int fadeInSeconds) {}
static void PF_SetClientMaxspeed(const edict_t *e, float speed) { if (e) ((edict_t *)e)->v.maxspeed = speed; }
static edict_t *PF_CreateFakeClient(const char *name) { return NULL; }
static void PF_RunPlayerMove(edict_t *fake, const float *va, float fm, float sm, float um, unsigned short buttons,
	unsigned char impulse, unsigned char msec) {}

static int PF_NumberOfEntities(void)
{
	int n = 0;
	for (int i = 0; i < sv.num_edicts; i++)
		n += !EDICT_NUM(i)->free;
	return n;
}

static char *PF_GetInfoKeyBuffer(edict_t *e)
{
	static char empty[1];
	if (!e)
		return sv.localinfo;
	int n = NUM_FOR_EDICT(e);
	if (n == 0)
		return sv.serverinfo;
	if (n >= 1 && n <= sv.maxclients)
		return sv.clientinfo[n - 1];
	empty[0] = 0;
	return empty;
}

static char *PF_InfoKeyValue(char *buf, const char *key) { return (char *)SV_InfoValueForKey(buf ? buf : "", key); }

static size_t InfoBufferSize(const char *buf)
{
	// every buffer we hand out is 512 bytes; others are left alone
	if (buf == sv.localinfo || buf == sv.serverinfo)
		return 512;
	for (int i = 0; i < sv.maxclients; i++)
		if (buf == sv.clientinfo[i])
			return sizeof(sv.clientinfo[i]);
	return 0;
}

static void PF_SetKeyValue(char *buf, const char *key, const char *value)
{
	size_t size = InfoBufferSize(buf);
	if (size)
		Info_SetValueForKey(buf, key, value, size);
}

static void PF_SetClientKeyValue(int client, char *buf, const char *key, const char *value)
{
	PF_SetKeyValue(buf, key, value);
}

static int PF_IsMapValid(const char *name) { return name && FS_FileExists(va("maps/%s.bsp", name)); }
static void PF_StaticDecal(const float *origin, int decalIndex, int entityIndex, int modelIndex) {}
static int PF_PrecacheGeneric(const char *s) { return SV_PrecacheGeneric(s); }

static int PF_GetPlayerUserId(edict_t *e)
{
	int n = e ? NUM_FOR_EDICT(e) : 0;
	return (n >= 1 && n <= sv.maxclients) ? n : -1;
}

static void PF_BuildSoundMsg(edict_t *entity, int channel, const char *sample, float volume, float attenuation, int fFlags,
	int pitch, int dest, int type, const float *origin, edict_t *ed) {}
static int PF_IsDedicatedServer(void) { return 0; }
static unsigned PF_GetPlayerWONId(edict_t *e) { return 0xFFFFFFFFu; }
static void PF_Info_RemoveKey(char *s, const char *key) { if (s && InfoBufferSize(s)) Info_RemoveKey(s, key); }

static char physinfo[512];
static const char *PF_GetPhysicsKeyValue(const edict_t *e, const char *key) { return SV_InfoValueForKey(physinfo, key); }
static void PF_SetPhysicsKeyValue(const edict_t *e, const char *key, const char *value)
{
	Info_SetValueForKey(physinfo, key, value, sizeof(physinfo));
}
static const char *PF_GetPhysicsInfoString(const edict_t *e) { return physinfo; }
const char *SV_PhysInfo(void) { return physinfo; }

static unsigned short PF_PrecacheEvent(int type, const char *name) { return SV_PrecacheEvent(type, name); }
static void PF_PlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, float *origin, float *angles,
	float f1, float f2, int i1, int i2, int b1, int b2) {}

static unsigned char *fatpvs;
static int fatpvs_size;

static unsigned char *PF_SetFatPVS(float *org)
{
	int row = (world.submodels[0].visleafs + 7) >> 3;
	if (row + 4 > fatpvs_size) {
		fatpvs_size = row + 4;
		fatpvs = Mem_Realloc(fatpvs, fatpvs_size);
	}
	memcpy(fatpvs, PVSForPoint(org), row);
	return fatpvs;
}

static unsigned char *PF_SetFatPAS(float *org)
{
	// no PAS yet: the PVS is a smaller, safe stand-in
	return PF_SetFatPVS(org);
}

static int PF_CheckVisibility(const edict_t *e, unsigned char *pset)
{
	if (!pset || !e)
		return 1;
	return EdictInPVS(e, pset);
}

static void PF_DeltaSetField(struct delta_s *f, const char *name) {}
static void PF_DeltaUnsetField(struct delta_s *f, const char *name) {}
static void PF_DeltaAddEncoder(const char *name, void (*fn)(struct delta_s *, const unsigned char *, const unsigned char *)) {}
static int PF_GetCurrentPlayer(void) { return -1; }
static int PF_CanSkipPlayer(const edict_t *player) { return 0; }
static int PF_DeltaFindField(struct delta_s *f, const char *name) { return -1; }
static void PF_DeltaSetFieldByIndex(struct delta_s *f, int n) {}
static void PF_DeltaUnsetFieldByIndex(struct delta_s *f, int n) {}
static void PF_SetGroupMask(int mask, int op) {}
static int PF_CreateInstancedBaseline(int classname, struct entity_state_s *baseline) { return 0; }
static void PF_Cvar_DirectSet(cvar_t *var, const char *value) { Cvar_DirectSet(var, value); }
static void PF_ForceUnmodified(FORCE_TYPE type, float *mins, float *maxs, const char *filename) {}
static void PF_GetPlayerStats(const edict_t *e, int *ping, int *loss) { if (ping) *ping = 0; if (loss) *loss = 0; }
static void PF_AddServerCommand(const char *name, void (*fn)(void)) { if (name && fn && !Cmd_Exists(name)) Cmd_AddCommand(name, fn, "game DLL command"); }
static qboolean PF_Voice_GetClientListening(int r, int s) { return false; }
static qboolean PF_Voice_SetClientListening(int r, int s, qboolean listen) { return false; }

static const char *PF_GetPlayerAuthId(edict_t *e)
{
	int n = e ? NUM_FOR_EDICT(e) : 0;
	return (n >= 1 && n <= sv.maxclients) ? "STEAM_ID_LOOPBACK" : "";
}

static void *PF_SequenceGet(const char *file, const char *entry) { return NULL; }
static void *PF_SequencePickSentence(const char *group, int method, int *picked) { if (picked) *picked = 0; return NULL; }

static int PF_GetFileSize(const char *filename)
{
	char path[MAX_OSPATH];
	struct _stat st;
	if (!FS_FullPath(filename, path, sizeof(path)) || _stat(path, &st))
		return -1;
	return (int)st.st_size;
}

static unsigned PF_GetApproxWavePlayLen(const char *filepath) { return 0; }
static int PF_IsCareerMatch(void) { return 0; }
static int PF_GetLocalizedStringLength(const char *label) { return 0; }
static void PF_RegisterTutorMessageShown(int mid) {}
static int PF_GetTimesTutorMessageShown(int mid) { return 0; }
static void PF_ProcessTutorMessageDecayBuffer(int *buffer, int len) {}
static void PF_ConstructTutorMessageDecayBuffer(int *buffer, int len) {}
static void PF_ResetTutorMessageDecayData(void) {}
static void PF_QueryClientCvarValue(const edict_t *player, const char *name) {}
static void PF_QueryClientCvarValue2(const edict_t *player, const char *name, int id) {}

static int PF_CheckParm(const char *parm, char **next)
{
	int i = COM_CheckParm(parm);
	if (next)
		*next = (i && i + 1 < com_argc) ? com_argv[i + 1] : NULL;
	return i;
}

static void PF_Reserved(void)
{
	static qboolean warned;
	if (!warned)
		Con_Warnf("The game DLL called an engine function newen doesn't know about\n");
	warned = true;
}

// ---------------------------------------------------------------- the table

void SV_InitEngineFuncs(enginefuncs_t *ef)
{
	memset(ef, 0, sizeof(*ef));
	ef->pfnPrecacheModel = PF_PrecacheModel;
	ef->pfnPrecacheSound = PF_PrecacheSound;
	ef->pfnSetModel = SV_SetModel;
	ef->pfnModelIndex = PF_ModelIndex;
	ef->pfnModelFrames = PF_ModelFrames;
	ef->pfnSetSize = PF_SetSize;
	ef->pfnChangeLevel = PF_ChangeLevel;
	ef->pfnGetSpawnParms = PF_GetSpawnParms;
	ef->pfnSaveSpawnParms = PF_SaveSpawnParms;
	ef->pfnVecToYaw = PF_VecToYaw;
	ef->pfnVecToAngles = PF_VecToAngles;
	ef->pfnMoveToOrigin = PF_MoveToOrigin;
	ef->pfnChangeYaw = PF_ChangeYaw;
	ef->pfnChangePitch = PF_ChangePitch;
	ef->pfnFindEntityByString = PF_FindEntityByString;
	ef->pfnGetEntityIllum = PF_GetEntityIllum;
	ef->pfnFindEntityInSphere = PF_FindEntityInSphere;
	ef->pfnFindClientInPVS = PF_FindClientInPVS;
	ef->pfnEntitiesInPVS = PF_EntitiesInPVS;
	ef->pfnMakeVectors = PF_MakeVectors;
	ef->pfnAngleVectors = PF_AngleVectors;
	ef->pfnCreateEntity = PF_CreateEntity;
	ef->pfnRemoveEntity = PF_RemoveEntity;
	ef->pfnCreateNamedEntity = PF_CreateNamedEntity;
	ef->pfnMakeStatic = PF_MakeStatic;
	ef->pfnEntIsOnFloor = PF_EntIsOnFloor;
	ef->pfnDropToFloor = PF_DropToFloor;
	ef->pfnWalkMove = PF_WalkMove;
	ef->pfnSetOrigin = PF_SetOrigin;
	ef->pfnEmitSound = PF_EmitSound;
	ef->pfnEmitAmbientSound = PF_EmitAmbientSound;
	ef->pfnTraceLine = PF_TraceLine;
	ef->pfnTraceToss = PF_TraceToss;
	ef->pfnTraceMonsterHull = PF_TraceMonsterHull;
	ef->pfnTraceHull = PF_TraceHull;
	ef->pfnTraceModel = PF_TraceModel;
	ef->pfnTraceTexture = SV_TraceTexture;
	ef->pfnTraceSphere = PF_TraceSphere;
	ef->pfnGetAimVector = PF_GetAimVector;
	ef->pfnServerCommand = PF_ServerCommand;
	ef->pfnServerExecute = PF_ServerExecute;
	ef->pfnClientCommand = PF_ClientCommand;
	ef->pfnParticleEffect = PF_ParticleEffect;
	ef->pfnLightStyle = PF_LightStyle;
	ef->pfnDecalIndex = PF_DecalIndex;
	ef->pfnPointContents = PF_PointContents;
	ef->pfnMessageBegin = PF_MessageBegin;
	ef->pfnMessageEnd = PF_MessageEnd;
	ef->pfnWriteByte = PF_WriteByte;
	ef->pfnWriteChar = PF_WriteChar;
	ef->pfnWriteShort = PF_WriteShort;
	ef->pfnWriteLong = PF_WriteLong;
	ef->pfnWriteAngle = PF_WriteAngle;
	ef->pfnWriteCoord = PF_WriteCoord;
	ef->pfnWriteString = PF_WriteString;
	ef->pfnWriteEntity = PF_WriteEntity;
	ef->pfnCVarRegister = PF_CVarRegister;
	ef->pfnCVarGetFloat = PF_CVarGetFloat;
	ef->pfnCVarGetString = PF_CVarGetString;
	ef->pfnCVarSetFloat = PF_CVarSetFloat;
	ef->pfnCVarSetString = PF_CVarSetString;
	ef->pfnAlertMessage = PF_AlertMessage;
	ef->pfnEngineFprintf = PF_EngineFprintf;
	ef->pfnPvAllocEntPrivateData = PF_PvAllocEntPrivateData;
	ef->pfnPvEntPrivateData = PF_PvEntPrivateData;
	ef->pfnFreeEntPrivateData = PF_FreeEntPrivateData;
	ef->pfnSzFromIndex = PF_SzFromIndex;
	ef->pfnAllocString = PF_AllocString;
	ef->pfnGetVarsOfEnt = PF_GetVarsOfEnt;
	ef->pfnPEntityOfEntOffset = PF_PEntityOfEntOffset;
	ef->pfnEntOffsetOfPEntity = PF_EntOffsetOfPEntity;
	ef->pfnIndexOfEdict = PF_IndexOfEdict;
	ef->pfnPEntityOfEntIndex = PF_PEntityOfEntIndex;
	ef->pfnFindEntityByVars = PF_FindEntityByVars;
	ef->pfnGetModelPtr = PF_GetModelPtr;
	ef->pfnRegUserMsg = PF_RegUserMsg;
	ef->pfnAnimationAutomove = PF_AnimationAutomove;
	ef->pfnGetBonePosition = PF_GetBonePosition;
	ef->pfnFunctionFromName = PF_FunctionFromName;
	ef->pfnNameForFunction = PF_NameForFunction;
	ef->pfnClientPrintf = PF_ClientPrintf;
	ef->pfnServerPrint = PF_ServerPrint;
	ef->pfnCmd_Args = PF_Cmd_Args;
	ef->pfnCmd_Argv = PF_Cmd_Argv;
	ef->pfnCmd_Argc = PF_Cmd_Argc;
	ef->pfnGetAttachment = PF_GetAttachment;
	ef->pfnCRC32_Init = PF_CRC32_Init;
	ef->pfnCRC32_ProcessBuffer = PF_CRC32_ProcessBuffer;
	ef->pfnCRC32_ProcessByte = PF_CRC32_ProcessByte;
	ef->pfnCRC32_Final = PF_CRC32_Final;
	ef->pfnRandomLong = PF_RandomLong;
	ef->pfnRandomFloat = PF_RandomFloat;
	ef->pfnSetView = PF_SetView;
	ef->pfnTime = PF_Time;
	ef->pfnCrosshairAngle = PF_CrosshairAngle;
	ef->pfnLoadFileForMe = PF_LoadFileForMe;
	ef->pfnFreeFile = PF_FreeFile;
	ef->pfnEndSection = PF_EndSection;
	ef->pfnCompareFileTime = PF_CompareFileTime;
	ef->pfnGetGameDir = PF_GetGameDir;
	ef->pfnCvar_RegisterVariable = PF_Cvar_RegisterVariable;
	ef->pfnFadeClientVolume = PF_FadeClientVolume;
	ef->pfnSetClientMaxspeed = PF_SetClientMaxspeed;
	ef->pfnCreateFakeClient = PF_CreateFakeClient;
	ef->pfnRunPlayerMove = PF_RunPlayerMove;
	ef->pfnNumberOfEntities = PF_NumberOfEntities;
	ef->pfnGetInfoKeyBuffer = PF_GetInfoKeyBuffer;
	ef->pfnInfoKeyValue = PF_InfoKeyValue;
	ef->pfnSetKeyValue = PF_SetKeyValue;
	ef->pfnSetClientKeyValue = PF_SetClientKeyValue;
	ef->pfnIsMapValid = PF_IsMapValid;
	ef->pfnStaticDecal = PF_StaticDecal;
	ef->pfnPrecacheGeneric = PF_PrecacheGeneric;
	ef->pfnGetPlayerUserId = PF_GetPlayerUserId;
	ef->pfnBuildSoundMsg = PF_BuildSoundMsg;
	ef->pfnIsDedicatedServer = PF_IsDedicatedServer;
	ef->pfnCVarGetPointer = PF_CVarGetPointer;
	ef->pfnGetPlayerWONId = PF_GetPlayerWONId;
	ef->pfnInfo_RemoveKey = PF_Info_RemoveKey;
	ef->pfnGetPhysicsKeyValue = PF_GetPhysicsKeyValue;
	ef->pfnSetPhysicsKeyValue = PF_SetPhysicsKeyValue;
	ef->pfnGetPhysicsInfoString = PF_GetPhysicsInfoString;
	ef->pfnPrecacheEvent = PF_PrecacheEvent;
	ef->pfnPlaybackEvent = PF_PlaybackEvent;
	ef->pfnSetFatPVS = PF_SetFatPVS;
	ef->pfnSetFatPAS = PF_SetFatPAS;
	ef->pfnCheckVisibility = PF_CheckVisibility;
	ef->pfnDeltaSetField = PF_DeltaSetField;
	ef->pfnDeltaUnsetField = PF_DeltaUnsetField;
	ef->pfnDeltaAddEncoder = PF_DeltaAddEncoder;
	ef->pfnGetCurrentPlayer = PF_GetCurrentPlayer;
	ef->pfnCanSkipPlayer = PF_CanSkipPlayer;
	ef->pfnDeltaFindField = PF_DeltaFindField;
	ef->pfnDeltaSetFieldByIndex = PF_DeltaSetFieldByIndex;
	ef->pfnDeltaUnsetFieldByIndex = PF_DeltaUnsetFieldByIndex;
	ef->pfnSetGroupMask = PF_SetGroupMask;
	ef->pfnCreateInstancedBaseline = PF_CreateInstancedBaseline;
	ef->pfnCvar_DirectSet = PF_Cvar_DirectSet;
	ef->pfnForceUnmodified = PF_ForceUnmodified;
	ef->pfnGetPlayerStats = PF_GetPlayerStats;
	ef->pfnAddServerCommand = PF_AddServerCommand;
	ef->pfnVoice_GetClientListening = PF_Voice_GetClientListening;
	ef->pfnVoice_SetClientListening = PF_Voice_SetClientListening;
	ef->pfnGetPlayerAuthId = PF_GetPlayerAuthId;
	ef->pfnSequenceGet = PF_SequenceGet;
	ef->pfnSequencePickSentence = PF_SequencePickSentence;
	ef->pfnGetFileSize = PF_GetFileSize;
	ef->pfnGetApproxWavePlayLen = PF_GetApproxWavePlayLen;
	ef->pfnIsCareerMatch = PF_IsCareerMatch;
	ef->pfnGetLocalizedStringLength = PF_GetLocalizedStringLength;
	ef->pfnRegisterTutorMessageShown = PF_RegisterTutorMessageShown;
	ef->pfnGetTimesTutorMessageShown = PF_GetTimesTutorMessageShown;
	ef->pfnProcessTutorMessageDecayBuffer = PF_ProcessTutorMessageDecayBuffer;
	ef->pfnConstructTutorMessageDecayBuffer = PF_ConstructTutorMessageDecayBuffer;
	ef->pfnResetTutorMessageDecayData = PF_ResetTutorMessageDecayData;
	ef->pfnQueryClientCvarValue = PF_QueryClientCvarValue;
	ef->pfnQueryClientCvarValue2 = PF_QueryClientCvarValue2;
	ef->pfnCheckParm = PF_CheckParm;
	ef->pfnPEntityOfEntIndexAllEntities = PF_PEntityOfEntIndexAllEntities;
	for (int i = 0; i < 32; i++)
		ef->pfnReserved[i] = PF_Reserved;
}
