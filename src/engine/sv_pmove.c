// newen - player movement: the engine side of the game DLL's pm_shared code
//
// Movement itself (walking, jumping, ducking, swimming, ladders, noclip) lives in the game DLL. The
// engine fills a playermove_t with the player's state, the nearby solid entities ("physents") and
// a set of callbacks for traces and contents, calls PM_Move, copies the result back, and touches
// whatever the move ran into. Order per command, as in GoldSrc: CmdStart, PlayerPreThink, the
// player's own think, PM_Move, touches, PlayerPostThink, CmdEnd.
#include "server.h"

static playermove_t *pm;     // ~330 KB, allocated once
static movevars_t movevars;
static unsigned random_seed;

// pm hulls are numbered stand, duck, point, large; the BSP's are point, stand, large, duck
static const int pm_to_bsp_hull[4] = { 1, 3, 0, 2 };

static int UseHull(void)
{
	return (pm->usehull >= 0 && pm->usehull < 4) ? pm->usehull : 0;
}

// ---------------------------------------------------------------- collision against physents

static hull_t *HullForPhysent(const physent_t *pe, int usehull, vec3_t offset)
{
	static hull_t hull;
	model_t *m = pe->model;
	if (m && m->type == mod_brush && pe->solid == SOLID_BSP) {
		SV_ModelHull(&world.submodels[m->submodel], pm_to_bsp_hull[usehull], &hull);
		VectorSubtract(hull.clip_mins, pm->player_mins[usehull], offset);
		VectorAdd(offset, pe->origin, offset);
		return &hull;
	}
	vec3_t mins, maxs;
	VectorSubtract(pe->mins, pm->player_maxs[usehull], mins);
	VectorSubtract(pe->maxs, pm->player_mins[usehull], maxs);
	VectorCopy(pe->origin, offset);
	return SV_HullForBox(mins, maxs);
}

static qboolean Rotated(const physent_t *pe)
{
	return pe->solid == SOLID_BSP && (pe->angles[0] || pe->angles[1] || pe->angles[2]);
}

static void ToLocal(const physent_t *pe, const vec3_t offset, const vec3_t in, vec3_t out)
{
	VectorSubtract(in, offset, out);
	if (Rotated(pe)) {
		vec3_t f, r, u, t;
		AngleVectors(pe->angles, f, r, u);
		VectorCopy(out, t);
		out[0] = DotProduct(t, f);
		out[1] = -DotProduct(t, r);
		out[2] = DotProduct(t, u);
	}
}

static trace_t ClipToPhysent(const physent_t *pe, int usehull, const vec3_t start, const vec3_t end)
{
	trace_t t;
	memset(&t, 0, sizeof(t));
	t.fraction = 1;
	t.allsolid = true;
	VectorCopy(end, t.endpos);
	vec3_t offset, start_l, end_l;
	hull_t *hull = HullForPhysent(pe, usehull, offset);
	ToLocal(pe, offset, start, start_l);
	ToLocal(pe, offset, end, end_l);
	SV_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l, &t, 0);
	if (Rotated(pe) && t.fraction != 1) {
		vec3_t f, r, u, n;
		AngleVectors(pe->angles, f, r, u);
		VectorCopy(t.plane.normal, n);
		for (int i = 0; i < 3; i++)
			t.plane.normal[i] = f[i] * n[0] - r[i] * n[1] + u[i] * n[2];
	}
	if (t.fraction != 1)
		for (int i = 0; i < 3; i++)
			t.endpos[i] = start[i] + t.fraction * (end[i] - start[i]);
	return t;
}

static pmtrace_t ToPmTrace(const trace_t *t, int ent)
{
	pmtrace_t p;
	memset(&p, 0, sizeof(p));
	p.allsolid = t->allsolid;
	p.startsolid = t->startsolid;
	p.inopen = t->inopen;
	p.inwater = t->inwater;
	p.fraction = t->fraction;
	VectorCopy(t->endpos, p.endpos);
	VectorCopy(t->plane.normal, p.plane.normal);
	p.plane.dist = t->plane.dist;
	p.ent = ent;
	p.hitgroup = t->hitgroup;
	return p;
}

static pmtrace_t PlayerTrace(const vec3_t start, const vec3_t end, int flags, int usehull, int ignore_pe,
	int (*ignorefn)(physent_t *))
{
	trace_t total = ClipToPhysent(&pm->physents[0], usehull, start, end);
	int totalent = (total.fraction < 1 || total.startsolid) ? 0 : -1;
	if (!(flags & PM_WORLD_ONLY)) {
		for (int i = 1; i < pm->numphysent; i++) {
			physent_t *pe = &pm->physents[i];
			if (i == ignore_pe || (ignorefn && ignorefn(pe)))
				continue;
			if ((flags & PM_GLASS_IGNORE) && pe->rendermode != kRenderNormal)
				continue;
			trace_t t = ClipToPhysent(pe, usehull, start, end);
			if (t.allsolid || t.startsolid || t.fraction < total.fraction) {
				qboolean wasstartsolid = total.startsolid;
				total = t;
				totalent = i;
				if (wasstartsolid)
					total.startsolid = true;
			} else if (t.startsolid) {
				total.startsolid = true;
			}
		}
	}
	return ToPmTrace(&total, totalent);
}

static pmtrace_t PM_PlayerTrace(float *start, float *end, int flags, int ignore_pe)
{
	return PlayerTrace(start, end, flags, UseHull(), ignore_pe, NULL);
}

static pmtrace_t PM_PlayerTraceEx(float *start, float *end, int flags, int (*ignore)(physent_t *))
{
	return PlayerTrace(start, end, flags, UseHull(), -1, ignore);
}

static pmtrace_t *PM_TraceLine(float *start, float *end, int flags, int usehull, int ignore_pe)
{
	static pmtrace_t t;
	if (usehull < 0 || usehull > 3)
		usehull = 2;
	t = PlayerTrace(start, end, PM_NORMAL, usehull, ignore_pe, NULL);
	return &t;
}

// for the client DLL's view of player movement
pmtrace_t *SV_PM_TraceLine(float *start, float *end, int flags, int usehull, int ignore_pe)
{
	if (!pm)
		return NULL;
	return PM_TraceLine(start, end, flags, usehull, ignore_pe);
}

movevars_t *SV_MoveVars(void)
{
	return &movevars;
}

static pmtrace_t *PM_TraceLineEx(float *start, float *end, int flags, int usehull, int (*ignore)(physent_t *))
{
	static pmtrace_t t;
	if (usehull < 0 || usehull > 3)
		usehull = 2;
	t = PlayerTrace(start, end, PM_NORMAL, usehull, -1, ignore);
	return &t;
}

static int TestPosition(const float *pos, pmtrace_t *ptrace, int (*ignore)(physent_t *))
{
	int usehull = UseHull();
	for (int i = 0; i < pm->numphysent; i++) {
		physent_t *pe = &pm->physents[i];
		if (ignore && ignore(pe))
			continue;
		vec3_t offset, test;
		hull_t *hull = HullForPhysent(pe, usehull, offset);
		ToLocal(pe, offset, pos, test);
		if (SV_HullPointContents(hull, hull->firstclipnode, test) == CONTENTS_SOLID) {
			if (ptrace)
				*ptrace = PlayerTrace(pos, pos, PM_NORMAL, usehull, -1, ignore);
			return i;
		}
	}
	return -1;   // free
}

static int PM_TestPlayerPosition(float *pos, pmtrace_t *ptrace) { return TestPosition(pos, ptrace, NULL); }
static int PM_TestPlayerPositionEx(float *pos, pmtrace_t *ptrace, int (*ignore)(physent_t *)) { return TestPosition(pos, ptrace, ignore); }

// world contents, then water and other content brushes (func_water, ladders) among the moveents
static int PM_PointContents(float *p, int *truecontents)
{
	int c = SV_TruePointContents(p);
	if (truecontents)
		*truecontents = c;
	if (c != CONTENTS_EMPTY)
		return (c <= CONTENTS_CURRENT_0 && c >= CONTENTS_CURRENT_DOWN) ? CONTENTS_WATER : c;
	for (int i = 0; i < pm->nummoveent; i++) {
		physent_t *pe = &pm->moveents[i];
		if (!pe->model || pe->model->type != mod_brush || pe->skin >= CONTENTS_EMPTY || pe->skin == CONTENTS_LADDER)
			continue;
		hull_t hull;
		SV_ModelHull(&world.submodels[pe->model->submodel], 0, &hull);
		vec3_t test;
		VectorSubtract(p, pe->origin, test);
		if (SV_HullPointContents(&hull, hull.firstclipnode, test) != CONTENTS_EMPTY) {
			if (truecontents)
				*truecontents = pe->skin;
			return pe->skin;
		}
	}
	return c;
}

static int PM_TruePointContents(float *p) { return SV_TruePointContents(p); }
static int PM_HullPointContents(void *hull, int num, float *p) { return SV_HullPointContents(hull, num, p); }

static void *PM_HullForBsp(physent_t *pe, float *offset)
{
	static hull_t hull;
	int usehull = UseHull();
	int sub = (pe->model && pe->model->type == mod_brush) ? pe->model->submodel : 0;
	SV_ModelHull(&world.submodels[sub], pm_to_bsp_hull[usehull], &hull);
	VectorSubtract(hull.clip_mins, pm->player_mins[usehull], offset);
	VectorAdd(offset, pe->origin, offset);
	return &hull;
}

static float PM_TraceModel(physent_t *pe, float *start, float *end, gs_trace_t *trace)
{
	vec3_t offset, start_l, end_l;
	hull_t *hull = PM_HullForBsp(pe, offset);
	VectorSubtract(start, offset, start_l);
	VectorSubtract(end, offset, end_l);
	trace_t t;
	memset(&t, 0, sizeof(t));
	t.fraction = 1;
	t.allsolid = true;
	VectorCopy(end, t.endpos);
	SV_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l, &t, 0);
	memset(trace, 0, sizeof(*trace));
	trace->allsolid = t.allsolid;
	trace->startsolid = t.startsolid;
	trace->inopen = t.inopen;
	trace->inwater = t.inwater;
	trace->fraction = t.fraction;
	for (int i = 0; i < 3; i++)
		trace->endpos[i] = start[i] + t.fraction * (end[i] - start[i]);
	VectorCopy(t.plane.normal, trace->plane.normal);
	trace->plane.dist = t.plane.dist;
	return t.fraction;
}

// model_t::type already uses GoldSrc's numbering (brush, sprite, alias, studio)
static int PM_GetModelType(struct model_s *m)
{
	return (m && m->type != mod_bad) ? m->type : mod_alias;
}

static void PM_GetModelBounds(struct model_s *m, float *mins, float *maxs)
{
	if (!m) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	VectorCopy(m->mins, mins);
	VectorCopy(m->maxs, maxs);
}

static void PM_StuckTouch(int hitent, pmtrace_t *tr)
{
	for (int i = 0; i < pm->numtouch; i++)
		if (pm->touchindex[i].ent == hitent)
			return;
	if (pm->numtouch >= MAX_PHYSENTS)
		return;
	tr->deltavelocity[0] = tr->deltavelocity[1] = tr->deltavelocity[2] = 0;
	pm->touchindex[pm->numtouch] = *tr;
	pm->touchindex[pm->numtouch].ent = hitent;
	pm->numtouch++;
}

static const char *PM_TraceTexture(int ground, float *vstart, float *vend)
{
	if (ground < 0 || ground >= pm->numphysent)
		return NULL;
	return SV_TraceTexture(EDICT_NUM(pm->physents[ground].info), vstart, vend);
}

// ---------------------------------------------------------------- the rest of the callbacks

static const char *PM_Info_ValueForKey(const char *s, const char *key) { return SV_InfoValueForKey(s, key); }
static void PM_Particle(float *origin, int color, float life, int zpos, int zvel) {}

static void PM_Con_NPrintf(int idx, const char *fmt, ...) {}

static void PM_Con_DPrintf(const char *fmt, ...)
{
	if (!developer->value)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("%s", buf);
}

static void PM_Con_Printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("%s", buf);
}

static double PM_Sys_FloatTime(void) { return Sys_FloatTime(); }

static long PM_RandomLong(long lo, long hi)
{
	if (hi <= lo)
		return lo;
	return lo + (long)(rand() % (hi - lo + 1));
}

static float PM_RandomFloat(float lo, float hi)
{
	return lo + (hi - lo) * (rand() / (float)RAND_MAX);
}

static int PM_COM_FileSize(const char *name)
{
	size_t n;
	void *data = FS_LoadFile(name, &n);
	if (!data)
		return -1;
	Mem_Free(data);
	return (int)n;
}

static unsigned char *PM_COM_LoadFile(const char *path, int usehunk, int *len)
{
	size_t n = 0;
	unsigned char *data = FS_LoadFile(path, &n);
	if (len)
		*len = data ? (int)n : 0;
	return data;
}

static void PM_COM_FreeFile(void *buffer) { Mem_Free(buffer); }

// reads a line from a file in memory, like fgets
static char *PM_memfgets(unsigned char *mem, int size, int *pos, char *out, int outsize)
{
	if (!mem || !out || !pos || *pos >= size || outsize < 2)
		return NULL;
	int i = *pos, last = size;
	if (last - *pos > outsize - 1)
		last = *pos + outsize - 1;
	while (i < last) {
		if (mem[i++] == '\n')
			break;
	}
	if (i == *pos)
		return NULL;
	int n = i - *pos;
	memcpy(out, mem + *pos, n);
	out[n] = 0;
	*pos = i;
	return out;
}

// footsteps, jumps, ladders, water: the player's own sounds
static void PM_PlaySound(int channel, const char *sample, float volume, float attenuation, int flags, int pitch)
{
	S_StartSound(pm->player_index + 1, channel, sample, NULL, volume, attenuation, flags, pitch);
}
void CL_PlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, const float *origin,
	const float *angles, float f1, float f2, int i1, int i2, int b1, int b2);

static void PM_PlaybackEventFull(int flags, int clientindex, unsigned short eventindex, float delay, float *origin,
	float *angles, float f1, float f2, int i1, int i2, int b1, int b2)
{
	edict_t *invoker = (clientindex >= 0 && clientindex < sv.maxclients) ? EDICT_NUM(clientindex + 1) : NULL;
	CL_PlaybackEvent(flags & ~(1 << 6), invoker, eventindex, delay, origin, angles, f1, f2, i1, i2, b1, b2);
}
static struct msurface_s *PM_TraceSurface(int ground, float *vstart, float *vend) { return NULL; }

// ---------------------------------------------------------------- setup

void SV_InitPlayerMove(void)
{
	if (!pm)
		pm = Mem_Alloc(sizeof(*pm));
	pm->PM_Info_ValueForKey = PM_Info_ValueForKey;
	pm->PM_Particle = PM_Particle;
	pm->PM_TestPlayerPosition = PM_TestPlayerPosition;
	pm->Con_NPrintf = PM_Con_NPrintf;
	pm->Con_DPrintf = PM_Con_DPrintf;
	pm->Con_Printf = PM_Con_Printf;
	pm->Sys_FloatTime = PM_Sys_FloatTime;
	pm->PM_StuckTouch = PM_StuckTouch;
	pm->PM_PointContents = PM_PointContents;
	pm->PM_TruePointContents = PM_TruePointContents;
	pm->PM_HullPointContents = PM_HullPointContents;
	pm->PM_PlayerTrace = PM_PlayerTrace;
	pm->PM_TraceLine = PM_TraceLine;
	pm->RandomLong = PM_RandomLong;
	pm->RandomFloat = PM_RandomFloat;
	pm->PM_GetModelType = PM_GetModelType;
	pm->PM_GetModelBounds = PM_GetModelBounds;
	pm->PM_HullForBsp = PM_HullForBsp;
	pm->PM_TraceModel = PM_TraceModel;
	pm->COM_FileSize = PM_COM_FileSize;
	pm->COM_LoadFile = PM_COM_LoadFile;
	pm->COM_FreeFile = PM_COM_FreeFile;
	pm->memfgets = PM_memfgets;
	pm->runfuncs = true;
	pm->PM_PlaySound = PM_PlaySound;
	pm->PM_TraceTexture = PM_TraceTexture;
	pm->PM_PlaybackEventFull = PM_PlaybackEventFull;
	pm->PM_PlayerTraceEx = PM_PlayerTraceEx;
	pm->PM_TestPlayerPositionEx = PM_TestPlayerPositionEx;
	pm->PM_TraceLineEx = PM_TraceLineEx;
	pm->PM_TraceSurface = PM_TraceSurface;
	pm->movevars = &movevars;
	pm->server = true;

	// player sizes: the defaults, then whatever the game says (the stock DLL's version leaves them alone)
	static const float mins[4][3] = { { -16, -16, -36 }, { -16, -16, -18 }, { 0, 0, 0 }, { -32, -32, -32 } };
	static const float maxs[4][3] = { { 16, 16, 36 }, { 16, 16, 18 }, { 0, 0, 0 }, { 32, 32, 32 } };
	for (int i = 0; i < 4; i++) {
		VectorCopy(mins[i], pm->player_mins[i]);
		VectorCopy(maxs[i], pm->player_maxs[i]);
		if (gEntityInterface.pfnGetHullBounds)
			gEntityInterface.pfnGetHullBounds(i, pm->player_mins[i], pm->player_maxs[i]);
	}
	if (gEntityInterface.pfnPM_Init)
		gEntityInterface.pfnPM_Init(pm);
}

static void SetMoveVars(void)
{
	movevars.gravity = sv_gravity->value;
	movevars.stopspeed = sv_stopspeed->value;
	movevars.maxspeed = Cvar_VariableValue("sv_maxspeed");
	movevars.spectatormaxspeed = Cvar_VariableValue("sv_spectatormaxspeed");
	movevars.accelerate = Cvar_VariableValue("sv_accelerate");
	movevars.airaccelerate = Cvar_VariableValue("sv_airaccelerate");
	movevars.wateraccelerate = Cvar_VariableValue("sv_wateraccelerate");
	movevars.friction = sv_friction->value;
	movevars.edgefriction = Cvar_VariableValue("edgefriction");
	movevars.waterfriction = Cvar_VariableValue("sv_waterfriction");
	movevars.entgravity = 1.0f;
	movevars.bounce = sv_bounce->value;
	movevars.stepsize = sv_stepsize->value;
	movevars.maxvelocity = sv_maxvelocity->value;
	movevars.zmax = Cvar_VariableValue("sv_zmax");
	movevars.waveHeight = Cvar_VariableValue("sv_wateramp");
	movevars.footsteps = Cvar_VariableValue("mp_footsteps") != 0;
	Q_strncpy(movevars.skyName, Cvar_VariableString("sv_skyname"), sizeof(movevars.skyName));
	movevars.rollangle = Cvar_VariableValue("sv_rollangle");
	movevars.rollspeed = Cvar_VariableValue("sv_rollspeed");
}

static void FillPhysent(physent_t *pe, edict_t *e)
{
	memset(pe, 0, sizeof(*pe));
	Q_strncpy(pe->name, SV_String(e->v.classname), sizeof(pe->name));
	int n = NUM_FOR_EDICT(e);
	pe->player = (n >= 1 && n <= sv.maxclients) ? n : 0;
	VectorCopy(e->v.origin, pe->origin);
	model_t *m = SV_ModelForIndex(e->v.modelindex);
	if (m && m->type == mod_brush)
		pe->model = m;
	else if (m && m->type == mod_studio)
		pe->studiomodel = m;
	VectorCopy(e->v.mins, pe->mins);
	VectorCopy(e->v.maxs, pe->maxs);
	pe->info = n;
	VectorCopy(e->v.angles, pe->angles);
	pe->solid = e->v.solid;
	pe->skin = e->v.skin;
	pe->rendermode = e->v.rendermode;
	pe->frame = e->v.frame;
	pe->sequence = e->v.sequence;
	memcpy(pe->controller, e->v.controller, 4);
	memcpy(pe->blending, e->v.blending, 2);
	pe->movetype = e->v.movetype;
	pe->takedamage = (int)e->v.takedamage;
	pe->team = e->v.team;
	pe->classnumber = e->v.playerclass;
	pe->iuser1 = e->v.iuser1; pe->iuser2 = e->v.iuser2; pe->iuser3 = e->v.iuser3; pe->iuser4 = e->v.iuser4;
	pe->fuser1 = e->v.fuser1; pe->fuser2 = e->v.fuser2; pe->fuser3 = e->v.fuser3; pe->fuser4 = e->v.fuser4;
	VectorCopy(e->v.vuser1, pe->vuser1); VectorCopy(e->v.vuser2, pe->vuser2);
	VectorCopy(e->v.vuser3, pe->vuser3); VectorCopy(e->v.vuser4, pe->vuser4);
}

// the world, every solid entity near the player, and content brushes (water, ladders)
static void AddPhysents(edict_t *pl)
{
	pm->numphysent = 0;
	pm->nummoveent = 0;
	pm->numvisent = 0;
	FillPhysent(&pm->physents[pm->numphysent++], sv.edicts);

	vec3_t mins, maxs;
	for (int i = 0; i < 3; i++) {
		float reach = 256 + fabsf(pl->v.velocity[i]) * 0.25f;
		mins[i] = pl->v.origin[i] - reach;
		maxs[i] = pl->v.origin[i] + reach;
	}
	for (int i = 1; i < sv.num_edicts; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e == pl || e->free || !ED_Linked(e) || e->v.owner == pl)
			continue;
		if (e->v.absmin[0] > maxs[0] || e->v.absmin[1] > maxs[1] || e->v.absmin[2] > maxs[2] ||
			e->v.absmax[0] < mins[0] || e->v.absmax[1] < mins[1] || e->v.absmax[2] < mins[2])
			continue;
		model_t *m = SV_ModelForIndex(e->v.modelindex);
		qboolean brush = m && m->type == mod_brush;
		if (brush && e->v.skin < CONTENTS_EMPTY && e->v.skin >= CONTENTS_LADDER && pm->nummoveent < MAX_MOVEENTS)
			FillPhysent(&pm->moveents[pm->nummoveent++], e);
		if (e->v.solid == SOLID_NOT || e->v.solid == SOLID_TRIGGER)
			continue;
		if (e->v.flags & FL_MONSTERCLIP)
			continue;   // monster clip brushes don't block players
		if (pm->numphysent >= MAX_PHYSENTS)
			break;
		FillPhysent(&pm->physents[pm->numphysent++], e);
	}
}

void SV_RunPlayerCmd(edict_t *pl, usercmd_t *cmd)
{
	if (!pm)
		return;
	float frametime = cmd->msec / 1000.0f;
	gGlobals.time = (float)sv.time;
	gGlobals.frametime = frametime;

	LuaG_StartCommand(pl, cmd);   // Lua may change the input
	gEntityInterface.pfnCmdStart(pl, cmd, random_seed++);
	if (!pl->v.fixangle)
		VectorCopy(cmd->viewangles, pl->v.v_angle);
	pl->v.button = cmd->buttons;
	if (cmd->impulse)
		pl->v.impulse = cmd->impulse;
	vec3_t light;
	R_LightPoint(pl->v.origin, light);
	pl->v.light_level = (int)((light[0] + light[1] + light[2]) * (255.0f / 3.0f));

	gEntityInterface.pfnPlayerPreThink(pl);
	if (pl->free)
		return;
	SV_RunThink(pl);
	if (pl->free)
		return;

	SetMoveVars();
	pm->player_index = NUM_FOR_EDICT(pl) - 1;
	pm->multiplayer = sv.maxclients > 1;
	pm->time = (float)(sv.time * 1000.0);
	pm->frametime = frametime;
	VectorCopy(pl->v.origin, pm->origin);
	VectorCopy(pl->v.v_angle, pm->angles);
	VectorCopy(pl->v.v_angle, pm->oldangles);
	VectorCopy(pl->v.velocity, pm->velocity);
	VectorCopy(pl->v.movedir, pm->movedir);
	VectorCopy(pl->v.basevelocity, pm->basevelocity);
	VectorCopy(pl->v.view_ofs, pm->view_ofs);
	pm->flDuckTime = (float)pl->v.flDuckTime;
	pm->bInDuck = pl->v.bInDuck;
	pm->flTimeStepSound = pl->v.flTimeStepSound;
	pm->iStepLeft = pl->v.iStepLeft;
	pm->flFallVelocity = pl->v.flFallVelocity;
	VectorCopy(pl->v.punchangle, pm->punchangle);
	pm->flSwimTime = (float)pl->v.flSwimTime;
	pm->flNextPrimaryAttack = 0;
	pm->effects = pl->v.effects;
	pm->flags = pl->v.flags;
	pm->usehull = (pl->v.flags & FL_DUCKING) ? 1 : 0;
	pm->gravity = pl->v.gravity;
	pm->friction = pl->v.friction;
	pm->oldbuttons = pl->v.oldbuttons;
	pm->waterjumptime = pl->v.teleport_time;
	pm->dead = pl->v.health <= 0;
	pm->deadflag = pl->v.deadflag;
	pm->spectator = 0;
	pm->movetype = pl->v.movetype;
	pm->onground = -1;
	pm->waterlevel = pl->v.waterlevel;
	pm->watertype = pl->v.watertype;
	pm->oldwaterlevel = pl->v.waterlevel;
	pm->maxspeed = movevars.maxspeed;
	pm->clientmaxspeed = pl->v.maxspeed;
	pm->iuser1 = pl->v.iuser1; pm->iuser2 = pl->v.iuser2; pm->iuser3 = pl->v.iuser3; pm->iuser4 = pl->v.iuser4;
	pm->fuser1 = pl->v.fuser1; pm->fuser2 = pl->v.fuser2; pm->fuser3 = pl->v.fuser3; pm->fuser4 = pl->v.fuser4;
	VectorCopy(pl->v.vuser1, pm->vuser1); VectorCopy(pl->v.vuser2, pm->vuser2);
	VectorCopy(pl->v.vuser3, pm->vuser3); VectorCopy(pl->v.vuser4, pm->vuser4);
	pm->cmd = *cmd;
	pm->numtouch = 0;
	Q_strncpy(pm->physinfo, SV_PhysInfo(), sizeof(pm->physinfo));
	AddPhysents(pl);

	gEntityInterface.pfnPM_Move(pm, true);

	VectorCopy(pm->origin, pl->v.origin);
	VectorCopy(pm->velocity, pl->v.velocity);
	VectorCopy(pm->basevelocity, pl->v.basevelocity);
	VectorCopy(pm->view_ofs, pl->v.view_ofs);
	VectorCopy(pm->movedir, pl->v.movedir);
	pl->v.flDuckTime = (int)pm->flDuckTime;
	pl->v.bInDuck = pm->bInDuck;
	pl->v.flTimeStepSound = pm->flTimeStepSound;
	pl->v.iStepLeft = pm->iStepLeft;
	pl->v.flFallVelocity = pm->flFallVelocity;
	VectorCopy(pm->punchangle, pl->v.punchangle);
	pl->v.flSwimTime = (int)pm->flSwimTime;
	pl->v.flags = pm->flags;
	pl->v.friction = pm->friction;
	pl->v.oldbuttons = pm->oldbuttons;
	pl->v.teleport_time = pm->waterjumptime;
	pl->v.waterlevel = pm->waterlevel;
	pl->v.watertype = pm->watertype;
	pl->v.deadflag = pm->deadflag;
	pl->v.iuser1 = pm->iuser1; pl->v.iuser2 = pm->iuser2; pl->v.iuser3 = pm->iuser3; pl->v.iuser4 = pm->iuser4;
	pl->v.fuser1 = pm->fuser1; pl->v.fuser2 = pm->fuser2; pl->v.fuser3 = pm->fuser3; pl->v.fuser4 = pm->fuser4;
	VectorCopy(pm->vuser1, pl->v.vuser1); VectorCopy(pm->vuser2, pl->v.vuser2);
	VectorCopy(pm->vuser3, pl->v.vuser3); VectorCopy(pm->vuser4, pl->v.vuser4);
	if (pm->onground >= 0 && pm->onground < pm->numphysent) {
		pl->v.flags |= FL_ONGROUND;
		pl->v.groundentity = EDICT_NUM(pm->physents[pm->onground].info);
	} else {
		pl->v.flags &= ~FL_ONGROUND;
		pl->v.groundentity = NULL;
	}
	// the model leans with a third of the view pitch
	if (!pl->v.fixangle) {
		pl->v.angles[0] = -pl->v.v_angle[0] / 3;
		pl->v.angles[1] = pl->v.v_angle[1];
		pl->v.angles[2] = pm->angles[2];
	}

	SV_LinkEdict(pl, true);   // triggers
	for (int i = 0; i < pm->numtouch && !pl->free; i++) {
		int idx = pm->touchindex[i].ent;
		if (idx <= 0 || idx >= pm->numphysent)
			continue;
		edict_t *e = EDICT_NUM(pm->physents[idx].info);
		if (!e->free)
			SV_Impact(e, pl);
	}
	if (pl->free)
		return;
	gGlobals.time = (float)sv.time;
	gEntityInterface.pfnPlayerPostThink(pl);
	gEntityInterface.pfnCmdEnd(pl);
}

// ---------------------------------------------------------------- traces for client events and temp entities

// The client DLL's event code and its temp entity physics trace through the same playermove
// structure (EV_PushPMStates, EV_SetSolidPlayers, EV_PlayerTrace). Here the "client's view" is the
// server's edicts: every solid entity at any distance, not just the ones near the player.
static physent_t *saved_physents;
static int saved_num, saved_cap, saved_hull;
static qboolean pushed;

void SV_PM_PushClientState(void)
{
	if (!pm)
		return;
	if (saved_cap < pm->numphysent) {
		saved_cap = pm->numphysent;
		saved_physents = Mem_Realloc(saved_physents, sizeof(physent_t) * saved_cap);
	}
	memcpy(saved_physents, pm->physents, sizeof(physent_t) * pm->numphysent);
	saved_num = pm->numphysent;
	saved_hull = pm->usehull;
	pushed = true;

	pm->numphysent = 0;
	FillPhysent(&pm->physents[pm->numphysent++], sv.edicts);
	for (int i = sv.maxclients + 1; i < sv.num_edicts && pm->numphysent < MAX_PHYSENTS; i++) {
		edict_t *e = EDICT_NUM(i);
		if (e->free || !ED_Linked(e) || e->v.solid == SOLID_NOT || e->v.solid == SOLID_TRIGGER)
			continue;
		FillPhysent(&pm->physents[pm->numphysent++], e);
	}
}

void SV_PM_PopClientState(void)
{
	if (!pm || !pushed)
		return;
	memcpy(pm->physents, saved_physents, sizeof(physent_t) * saved_num);
	pm->numphysent = saved_num;
	pm->usehull = saved_hull;
	pushed = false;
}

// players become solid, except one (0-based player number; -1 = none skipped)
void SV_PM_SetSolidPlayers(int skip)
{
	if (!pm)
		return;
	for (int i = 1; i <= sv.maxclients && pm->numphysent < MAX_PHYSENTS; i++) {
		edict_t *e = EDICT_NUM(i);
		if (i - 1 == skip || e->free || !ED_Linked(e) || e->v.solid == SOLID_NOT || e->v.health <= 0)
			continue;
		FillPhysent(&pm->physents[pm->numphysent++], e);
	}
}

void SV_PM_SetTraceHull(int hull)
{
	if (pm)
		pm->usehull = (hull >= 0 && hull < 4) ? hull : 2;
}

void SV_PM_ClientTrace(const float *start, const float *end, int flags, int ignore_pe, pmtrace_t *tr)
{
	if (!pm) {
		memset(tr, 0, sizeof(*tr));
		tr->fraction = 1;
		tr->ent = -1;
		VectorCopy(end, tr->endpos);
		return;
	}
	*tr = PlayerTrace(start, end, flags, UseHull(), ignore_pe, NULL);
}

physent_t *SV_PM_GetPhysent(int i)
{
	return (pm && i >= 0 && i < pm->numphysent) ? &pm->physents[i] : NULL;
}

void SV_PM_HullBounds(int hull, float *mins, float *maxs)
{
	if (!pm || hull < 0 || hull > 3) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	VectorCopy(pm->player_mins[hull], mins);
	VectorCopy(pm->player_maxs[hull], maxs);
}

// the client DLL's own pm_shared (texture types for impact sounds) is set up with the same structure
playermove_t *SV_PlayerMove(void)
{
	return pm;
}
