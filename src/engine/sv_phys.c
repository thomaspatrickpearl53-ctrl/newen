// newen - server physics: thinking and the movetypes (push, step, toss, fly, noclip, follow)
//
// The same model as Quake and GoldSrc: every frame the game's StartFrame runs, then each entity
// moves according to its movetype and thinks when its nextthink comes up. Players are skipped here;
// they move through player movement (or, for now, follow the camera).
#include "server.h"

static const vec3_t vec3_origin = { 0, 0, 0 };

void SV_CheckVelocity(edict_t *ent)
{
	float max = sv_maxvelocity->value;
	for (int i = 0; i < 3; i++) {
		if (ent->v.velocity[i] != ent->v.velocity[i]) {
			Con_DPrintf("%s got a NaN velocity\n", SV_String(ent->v.classname));
			ent->v.velocity[i] = 0;
		}
		if (ent->v.origin[i] != ent->v.origin[i]) {
			Con_DPrintf("%s got a NaN origin\n", SV_String(ent->v.classname));
			ent->v.origin[i] = 0;
		}
		if (ent->v.velocity[i] > max)
			ent->v.velocity[i] = max;
		else if (ent->v.velocity[i] < -max)
			ent->v.velocity[i] = -max;
	}
}

// false if the entity was removed
static qboolean RunThink(edict_t *ent)
{
	float thinktime = ent->v.nextthink;
	if (thinktime <= 0 || thinktime > sv.time + sv.frametime)
		return true;
	if (thinktime < sv.time)
		thinktime = (float)sv.time;   // don't let things stay in the past
	ent->v.nextthink = 0;
	gGlobals.time = thinktime;
	gEntityInterface.pfnThink(ent);
	if (ent->v.flags & FL_KILLME)
		ED_Free(ent);
	return !ent->free;
}

void SV_RunThink(edict_t *ent)
{
	RunThink(ent);
}

void SV_Impact(edict_t *e1, edict_t *e2)
{
	gGlobals.time = (float)sv.time;
	if (!e1->free && !e2->free && e1->v.solid != SOLID_NOT)
		gEntityInterface.pfnTouch(e1, e2);
	if (!e1->free && !e2->free && e2->v.solid != SOLID_NOT)
		gEntityInterface.pfnTouch(e2, e1);
}

static int ClipVelocity(const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
	int blocked = 0;
	if (normal[2] > 0)
		blocked |= 1;   // floor
	if (!normal[2])
		blocked |= 2;   // step or wall
	float backoff = DotProduct(in, normal) * overbounce;
	for (int i = 0; i < 3; i++) {
		out[i] = in[i] - normal[i] * backoff;
		if (out[i] > -0.1f && out[i] < 0.1f)
			out[i] = 0;
	}
	return blocked;
}

static void CheckWater(edict_t *ent)
{
	vec3_t p = { ent->v.origin[0], ent->v.origin[1], ent->v.origin[2] + ent->v.mins[2] + 1 };
	ent->v.waterlevel = 0;
	ent->v.watertype = CONTENTS_EMPTY;
	int c = SV_PointContents(p);
	if (c > CONTENTS_WATER || c < CONTENTS_LAVA)
		return;
	ent->v.watertype = c;
	ent->v.waterlevel = 1;
	p[2] = ent->v.origin[2] + (ent->v.mins[2] + ent->v.maxs[2]) * 0.5f;
	c = SV_PointContents(p);
	if (c <= CONTENTS_WATER && c >= CONTENTS_LAVA) {
		ent->v.waterlevel = 2;
		p[2] = ent->v.origin[2] + ent->v.view_ofs[2];
		c = SV_PointContents(p);
		if (c <= CONTENTS_WATER && c >= CONTENTS_LAVA)
			ent->v.waterlevel = 3;
	}
}

static void AddGravity(edict_t *ent)
{
	float g = ent->v.gravity ? ent->v.gravity : 1.0f;
	ent->v.velocity[2] -= g * sv_gravity->value * sv.frametime;
	ent->v.velocity[2] += ent->v.basevelocity[2] * sv.frametime;
	ent->v.basevelocity[2] = 0;
	SV_CheckVelocity(ent);
}

// slides along whatever it hits; returns 1 = floor, 2 = wall, 4 = dead stop
#define MAX_CLIP_PLANES 5
static int FlyMove(edict_t *ent, float time)
{
	vec3_t planes[MAX_CLIP_PLANES], primal, original, newvel;
	int numplanes = 0, blocked = 0;
	VectorCopy(ent->v.velocity, primal);
	VectorCopy(ent->v.velocity, original);
	float time_left = time;
	int type = ent->v.movetype == MOVETYPE_FLYMISSILE || ent->v.movetype == MOVETYPE_BOUNCEMISSILE ? MOVE_MISSILE : MOVE_NORMAL;

	for (int bump = 0; bump < 4; bump++) {
		if (!ent->v.velocity[0] && !ent->v.velocity[1] && !ent->v.velocity[2])
			break;
		vec3_t end;
		VectorMA(ent->v.origin, time_left, ent->v.velocity, end);
		trace_t trace = SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, end, type, ent);
		if (trace.allsolid) {
			VectorClear(ent->v.velocity);
			return 4;
		}
		if (trace.fraction > 0) {
			VectorCopy(trace.endpos, ent->v.origin);
			VectorCopy(ent->v.velocity, original);
			numplanes = 0;
		}
		if (trace.fraction == 1)
			break;
		edict_t *hit = trace.ent ? trace.ent : sv.edicts;
		if (trace.plane.normal[2] > 0.7f) {
			blocked |= 1;
			if (hit->v.solid == SOLID_BSP || hit->v.solid == SOLID_BBOX || hit->v.solid == SOLID_SLIDEBOX) {
				ent->v.flags |= FL_ONGROUND;
				ent->v.groundentity = hit;
			}
		}
		if (!trace.plane.normal[2])
			blocked |= 2;
		SV_Impact(ent, hit);
		if (ent->free)
			break;
		time_left -= time_left * trace.fraction;
		if (numplanes >= MAX_CLIP_PLANES) {
			VectorClear(ent->v.velocity);
			return blocked | 4;
		}
		VectorCopy(trace.plane.normal, planes[numplanes]);
		numplanes++;

		// find a velocity that runs along every plane hit so far
		int i, j;
		for (i = 0; i < numplanes; i++) {
			ClipVelocity(original, planes[i], newvel, 1);
			for (j = 0; j < numplanes; j++)
				if (j != i && DotProduct(newvel, planes[j]) < 0)
					break;
			if (j == numplanes)
				break;
		}
		if (i != numplanes) {
			VectorCopy(newvel, ent->v.velocity);
		} else {
			if (numplanes != 2) {
				VectorClear(ent->v.velocity);
				return blocked | 4;
			}
			vec3_t dir;
			CrossProduct(planes[0], planes[1], dir);
			float d = DotProduct(dir, ent->v.velocity);
			VectorScale(dir, d, ent->v.velocity);
		}
		// don't turn back and bounce around in corners
		if (DotProduct(ent->v.velocity, primal) <= 0) {
			VectorClear(ent->v.velocity);
			return blocked;
		}
	}
	return blocked;
}

static trace_t PushEntity(edict_t *ent, const vec3_t push)
{
	vec3_t end;
	VectorAdd(ent->v.origin, push, end);
	int type;
	if (ent->v.movetype == MOVETYPE_FLYMISSILE || ent->v.movetype == MOVETYPE_BOUNCEMISSILE)
		type = MOVE_MISSILE;
	else if (ent->v.solid == SOLID_TRIGGER || ent->v.solid == SOLID_NOT)
		type = MOVE_NOMONSTERS;
	else
		type = MOVE_NORMAL;
	trace_t trace = SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, end, type, ent);
	if (trace.fraction != 0)
		VectorCopy(trace.endpos, ent->v.origin);
	SV_LinkEdict(ent, true);
	if (trace.ent && !ent->free)
		SV_Impact(ent, trace.ent);
	return trace;
}

// ---------------------------------------------------------------- pushers (doors, plats, trains)

typedef struct { edict_t *ent; vec3_t origin; } pushed_t;
static pushed_t *pushed;
static int pushed_cap;

static qboolean Pushable(const edict_t *check, const edict_t *pusher)
{
	if (check->free || check == pusher || !ED_Linked(check))
		return false;
	int mt = check->v.movetype;
	if (mt == MOVETYPE_PUSH || mt == MOVETYPE_NONE || mt == MOVETYPE_NOCLIP || mt == MOVETYPE_FOLLOW)
		return false;
	return check->v.solid != SOLID_TRIGGER;
}

static qboolean PushMove(edict_t *pusher, float movetime)
{
	if (!pusher->v.velocity[0] && !pusher->v.velocity[1] && !pusher->v.velocity[2]) {
		pusher->v.ltime += movetime;
		return true;
	}
	vec3_t move, mins, maxs, pushorig;
	VectorScale(pusher->v.velocity, movetime, move);
	VectorAdd(pusher->v.absmin, move, mins);
	VectorAdd(pusher->v.absmax, move, maxs);
	VectorCopy(pusher->v.origin, pushorig);
	VectorAdd(pusher->v.origin, move, pusher->v.origin);
	pusher->v.ltime += movetime;
	SV_LinkEdict(pusher, false);
	if (pusher->v.solid == SOLID_NOT)
		return true;   // nothing to push

	if (pushed_cap < sv.num_edicts) {
		pushed_cap = sv.max_edicts;
		pushed = Mem_Realloc(pushed, sizeof(*pushed) * pushed_cap);
	}
	int npushed = 0;
	for (int e = 1; e < sv.num_edicts; e++) {
		edict_t *check = EDICT_NUM(e);
		if (!Pushable(check, pusher))
			continue;
		// riders always move; anything else only if the pusher now overlaps it
		if (!((check->v.flags & FL_ONGROUND) && check->v.groundentity == pusher)) {
			if (check->v.absmin[0] >= maxs[0] || check->v.absmin[1] >= maxs[1] || check->v.absmin[2] >= maxs[2] ||
				check->v.absmax[0] <= mins[0] || check->v.absmax[1] <= mins[1] || check->v.absmax[2] <= mins[2])
				continue;
			if (!SV_TestEntityPosition(check))
				continue;
		}
		if (check->v.movetype != MOVETYPE_WALK)
			check->v.flags &= ~FL_ONGROUND;

		pushed[npushed].ent = check;
		VectorCopy(check->v.origin, pushed[npushed].origin);
		npushed++;

		int solid = pusher->v.solid;
		pusher->v.solid = SOLID_NOT;
		PushEntity(check, move);
		pusher->v.solid = solid;

		edict_t *block = SV_TestEntityPosition(check);
		if (!block)
			continue;
		if (check->v.mins[0] == check->v.maxs[0])
			continue;   // points don't block
		if (check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER) {
			// corpses get squashed flat instead
			VectorClear(check->v.mins);
			VectorClear(check->v.maxs);
			continue;
		}
		// blocked: put everything back and tell the game
		VectorCopy(pushorig, pusher->v.origin);
		SV_LinkEdict(pusher, false);
		pusher->v.ltime -= movetime;
		for (int i = npushed - 1; i >= 0; i--) {
			VectorCopy(pushed[i].origin, pushed[i].ent->v.origin);
			SV_LinkEdict(pushed[i].ent, false);
		}
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnBlocked(pusher, check);
		return false;
	}
	return true;
}

// rotating pushers turn, and stop when something is in the way (riders aren't carried round yet)
static qboolean PushRotate(edict_t *pusher, float movetime)
{
	if (!pusher->v.avelocity[0] && !pusher->v.avelocity[1] && !pusher->v.avelocity[2]) {
		pusher->v.ltime += movetime;
		return true;
	}
	vec3_t amove, oldangles;
	VectorScale(pusher->v.avelocity, movetime, amove);
	VectorCopy(pusher->v.angles, oldangles);
	VectorAdd(pusher->v.angles, amove, pusher->v.angles);
	pusher->v.ltime += movetime;
	SV_LinkEdict(pusher, false);
	if (pusher->v.solid == SOLID_NOT)
		return true;

	for (int e = 1; e < sv.num_edicts; e++) {
		edict_t *check = EDICT_NUM(e);
		if (!Pushable(check, pusher) || check->v.solid == SOLID_NOT)
			continue;
		if (check->v.absmin[0] >= pusher->v.absmax[0] || check->v.absmin[1] >= pusher->v.absmax[1] ||
			check->v.absmin[2] >= pusher->v.absmax[2] || check->v.absmax[0] <= pusher->v.absmin[0] ||
			check->v.absmax[1] <= pusher->v.absmin[1] || check->v.absmax[2] <= pusher->v.absmin[2])
			continue;
		if (SV_TestEntityPosition(check) != pusher)
			continue;
		VectorCopy(oldangles, pusher->v.angles);
		SV_LinkEdict(pusher, false);
		pusher->v.ltime -= movetime;
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnBlocked(pusher, check);
		return false;
	}
	return true;
}

static void Physics_Pusher(edict_t *ent)
{
	float oldltime = ent->v.ltime;
	float thinktime = ent->v.nextthink;
	float movetime;
	if (thinktime < ent->v.ltime + sv.frametime) {
		movetime = thinktime - ent->v.ltime;
		if (movetime < 0)
			movetime = 0;
	} else {
		movetime = sv.frametime;
	}
	if (movetime) {
		qboolean rotating = ent->v.avelocity[0] || ent->v.avelocity[1] || ent->v.avelocity[2];
		qboolean moving = ent->v.velocity[0] || ent->v.velocity[1] || ent->v.velocity[2];
		if (rotating && moving) {
			if (PushRotate(ent, movetime)) {
				float lt = ent->v.ltime;
				ent->v.ltime -= movetime;   // both moves share the same time slice
				PushMove(ent, movetime);
				ent->v.ltime = lt;
			}
		} else if (rotating) {
			PushRotate(ent, movetime);
		} else {
			PushMove(ent, movetime);
		}
	}
	if (thinktime > oldltime && thinktime <= ent->v.ltime) {
		ent->v.nextthink = 0;
		gGlobals.time = (float)sv.time;
		gEntityInterface.pfnThink(ent);
	}
}

// ---------------------------------------------------------------- other movetypes

static void Physics_None(edict_t *ent)
{
	RunThink(ent);
}

static void Physics_Follow(edict_t *ent)
{
	if (!RunThink(ent))
		return;
	edict_t *a = ent->v.aiment;
	if (!a || a->free) {
		ent->v.movetype = MOVETYPE_NONE;
		return;
	}
	VectorAdd(a->v.origin, ent->v.v_angle, ent->v.origin);
	VectorCopy(a->v.angles, ent->v.angles);
	SV_LinkEdict(ent, true);
}

static void Physics_Noclip(edict_t *ent)
{
	if (!RunThink(ent))
		return;
	VectorMA(ent->v.angles, sv.frametime, ent->v.avelocity, ent->v.angles);
	VectorMA(ent->v.origin, sv.frametime, ent->v.velocity, ent->v.origin);
	SV_LinkEdict(ent, false);
}

static void Physics_Toss(edict_t *ent)
{
	if (!RunThink(ent))
		return;
	if (ent->v.velocity[2] > 0 || !ent->v.groundentity || ent->v.groundentity->free)
		ent->v.flags &= ~FL_ONGROUND;
	if ((ent->v.flags & FL_ONGROUND) && !ent->v.velocity[0] && !ent->v.velocity[1] && !ent->v.velocity[2]) {
		VectorClear(ent->v.avelocity);
		return;   // at rest
	}
	SV_CheckVelocity(ent);
	if (ent->v.movetype != MOVETYPE_FLY && ent->v.movetype != MOVETYPE_FLYMISSILE && ent->v.movetype != MOVETYPE_BOUNCEMISSILE)
		AddGravity(ent);
	VectorMA(ent->v.angles, sv.frametime, ent->v.avelocity, ent->v.angles);

	vec3_t move;
	VectorScale(ent->v.velocity, sv.frametime, move);
	trace_t trace = PushEntity(ent, move);
	if (ent->free || trace.fraction == 1)
		return;
	if (trace.allsolid) {
		// stuck in something: stop dead
		VectorClear(ent->v.velocity);
		VectorClear(ent->v.avelocity);
		return;
	}
	float backoff = 1;
	if (ent->v.movetype == MOVETYPE_BOUNCE)
		backoff = 2.0f - ent->v.friction;
	else if (ent->v.movetype == MOVETYPE_BOUNCEMISSILE)
		backoff = 2.0f;
	ClipVelocity(ent->v.velocity, trace.plane.normal, ent->v.velocity, backoff);

	// stop on the ground
	if (trace.plane.normal[2] > 0.7f) {
		if (ent->v.velocity[2] < 60 || (ent->v.movetype != MOVETYPE_BOUNCE && ent->v.movetype != MOVETYPE_BOUNCEMISSILE)) {
			ent->v.flags |= FL_ONGROUND;
			ent->v.groundentity = trace.ent ? trace.ent : sv.edicts;
			VectorClear(ent->v.velocity);
			VectorClear(ent->v.avelocity);
		}
	}
	CheckWater(ent);
}

static void Physics_Step(edict_t *ent)
{
	CheckWater(ent);
	if (ent->v.groundentity && ent->v.groundentity->free)
		ent->v.flags &= ~FL_ONGROUND;

	if (!(ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM))) {
		AddGravity(ent);
		FlyMove(ent, sv.frametime);
		if (!ent->free)
			SV_LinkEdict(ent, true);
	} else if (ent->v.velocity[0] || ent->v.velocity[1] || ((ent->v.flags & (FL_FLY | FL_SWIM)) && ent->v.velocity[2])) {
		// sliding on the ground slows down
		if (ent->v.flags & FL_ONGROUND) {
			float speed = sqrtf(ent->v.velocity[0] * ent->v.velocity[0] + ent->v.velocity[1] * ent->v.velocity[1]);
			if (speed) {
				float friction = sv_friction->value * (ent->v.friction ? ent->v.friction : 1.0f);
				float control = speed < sv_stopspeed->value ? sv_stopspeed->value : speed;
				float newspeed = speed - sv.frametime * control * friction;
				if (newspeed < 0)
					newspeed = 0;
				newspeed /= speed;
				ent->v.velocity[0] *= newspeed;
				ent->v.velocity[1] *= newspeed;
			}
		}
		SV_CheckVelocity(ent);
		FlyMove(ent, sv.frametime);
		if (!ent->free)
			SV_LinkEdict(ent, true);
	}
	if (!ent->free)
		RunThink(ent);
}

void SV_Physics(void)
{
	gGlobals.time = (float)sv.time;
	gGlobals.frametime = sv.frametime;
	gEntityInterface.pfnStartFrame();

	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *ent = EDICT_NUM(i);
		if (ent->free)
			continue;
		if (gGlobals.force_retouch)
			SV_LinkEdict(ent, true);   // retouch even if not moving
		if (i > 0 && i <= sv.maxclients)
			continue;   // players move in their own path

		switch (ent->v.movetype) {
		case MOVETYPE_PUSH:
			Physics_Pusher(ent);
			break;
		case MOVETYPE_NONE:
			Physics_None(ent);
			break;
		case MOVETYPE_FOLLOW:
			Physics_Follow(ent);
			break;
		case MOVETYPE_NOCLIP:
			Physics_Noclip(ent);
			break;
		case MOVETYPE_STEP:
		case MOVETYPE_PUSHSTEP:
			Physics_Step(ent);
			break;
		case MOVETYPE_TOSS:
		case MOVETYPE_BOUNCE:
		case MOVETYPE_FLY:
		case MOVETYPE_FLYMISSILE:
		case MOVETYPE_BOUNCEMISSILE:
			Physics_Toss(ent);
			break;
		case MOVETYPE_WALK:
			Physics_None(ent);   // walking non-players just think
			break;
		default:
			Con_DPrintf("%s has unknown movetype %d\n", SV_String(ent->v.classname), ent->v.movetype);
			break;
		}
		if (!ent->free && (ent->v.flags & FL_KILLME))
			ED_Free(ent);
	}
	if (gGlobals.force_retouch)
		gGlobals.force_retouch--;
	sv.time += sv.frametime;
}

// ---------------------------------------------------------------- monster movement

int SV_DropToFloor(edict_t *e)
{
	vec3_t end;
	VectorCopy(e->v.origin, end);
	end[2] -= 256;
	trace_t trace = SV_Move(e->v.origin, e->v.mins, e->v.maxs, end, MOVE_NORMAL, e);
	if (trace.allsolid)
		return -1;
	if (trace.fraction == 1)
		return 0;
	VectorCopy(trace.endpos, e->v.origin);
	SV_LinkEdict(e, false);
	e->v.flags |= FL_ONGROUND;
	e->v.groundentity = trace.ent ? trace.ent : sv.edicts;
	return 1;
}

// true if the box is standing on something under all four corners (or close enough)
qboolean SV_CheckBottom(edict_t *ent)
{
	vec3_t mins, maxs, start, stop;
	VectorAdd(ent->v.origin, ent->v.mins, mins);
	VectorAdd(ent->v.origin, ent->v.maxs, maxs);

	// all four corners over solid world: no need for the slower check
	start[2] = mins[2] - 1;
	for (int x = 0; x <= 1; x++)
		for (int y = 0; y <= 1; y++) {
			start[0] = x ? maxs[0] : mins[0];
			start[1] = y ? maxs[1] : mins[1];
			if (SV_PointContents(start) != CONTENTS_SOLID)
				goto realcheck;
		}
	return true;

realcheck:;
	float stepsize = sv_stepsize->value;
	start[2] = mins[2];
	start[0] = stop[0] = (mins[0] + maxs[0]) * 0.5f;
	start[1] = stop[1] = (mins[1] + maxs[1]) * 0.5f;
	stop[2] = start[2] - 2 * stepsize;
	trace_t trace = SV_Move(start, vec3_origin, vec3_origin, stop, MOVE_NOMONSTERS, ent);
	if (trace.fraction == 1)
		return false;
	float mid = trace.endpos[2], bottom = mid;
	for (int x = 0; x <= 1; x++)
		for (int y = 0; y <= 1; y++) {
			start[0] = stop[0] = x ? maxs[0] : mins[0];
			start[1] = stop[1] = y ? maxs[1] : mins[1];
			trace = SV_Move(start, vec3_origin, vec3_origin, stop, MOVE_NOMONSTERS, ent);
			if (trace.fraction != 1 && trace.endpos[2] > bottom)
				bottom = trace.endpos[2];
			if (trace.fraction == 1 || mid - trace.endpos[2] > stepsize)
				return false;
		}
	return true;
}

qboolean SV_movestep(edict_t *ent, const vec3_t move, qboolean relink, int mode)
{
	vec3_t oldorg, neworg, end;
	VectorCopy(ent->v.origin, oldorg);
	VectorAdd(ent->v.origin, move, neworg);
	int type = mode == WALKMOVE_WORLDONLY ? MOVE_NOMONSTERS : MOVE_NORMAL;

	// flying and swimming monsters don't step
	if (ent->v.flags & (FL_SWIM | FL_FLY)) {
		for (int i = 0; i < 2; i++) {
			VectorAdd(ent->v.origin, move, neworg);
			edict_t *enemy = ent->v.enemy;
			if (i == 0 && enemy && !enemy->free) {
				float dz = ent->v.origin[2] - enemy->v.origin[2];
				if (dz > 40) neworg[2] -= 8;
				if (dz < 30) neworg[2] += 8;
			}
			trace_t trace = SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, neworg, type, ent);
			if (trace.fraction == 1) {
				if ((ent->v.flags & FL_SWIM) && SV_PointContents(trace.endpos) == CONTENTS_EMPTY)
					return false;   // a fish can't leave the water
				if (mode != WALKMOVE_CHECKONLY) {
					VectorCopy(trace.endpos, ent->v.origin);
					if (relink)
						SV_LinkEdict(ent, true);
				}
				return true;
			}
			if (!enemy || enemy->free)
				break;
		}
		return false;
	}

	// walk: from a step above the target, trace down to the floor
	float stepsize = sv_stepsize->value;
	neworg[2] += stepsize;
	VectorCopy(neworg, end);
	end[2] -= stepsize * 2;
	trace_t trace = SV_Move(neworg, ent->v.mins, ent->v.maxs, end, type, ent);
	if (trace.allsolid)
		return false;
	if (trace.startsolid) {
		neworg[2] -= stepsize;
		trace = SV_Move(neworg, ent->v.mins, ent->v.maxs, end, type, ent);
		if (trace.allsolid || trace.startsolid)
			return false;
	}
	if (trace.fraction == 1) {
		// walked off an edge
		if (ent->v.flags & FL_PARTIALGROUND) {
			if (mode != WALKMOVE_CHECKONLY) {
				VectorAdd(ent->v.origin, move, ent->v.origin);
				if (relink)
					SV_LinkEdict(ent, true);
				ent->v.flags &= ~FL_ONGROUND;
			}
			return true;
		}
		return false;
	}

	VectorCopy(trace.endpos, ent->v.origin);
	if (!SV_CheckBottom(ent)) {
		if (ent->v.flags & FL_PARTIALGROUND) {
			// it had its floor mostly pulled out and is trying to get back on
			if (mode == WALKMOVE_CHECKONLY)
				VectorCopy(oldorg, ent->v.origin);
			else if (relink)
				SV_LinkEdict(ent, true);
			return true;
		}
		VectorCopy(oldorg, ent->v.origin);
		return false;
	}
	if (mode == WALKMOVE_CHECKONLY) {
		VectorCopy(oldorg, ent->v.origin);
		return true;
	}
	ent->v.flags &= ~FL_PARTIALGROUND;
	ent->v.groundentity = trace.ent ? trace.ent : sv.edicts;
	if (relink)
		SV_LinkEdict(ent, true);
	return true;
}

static qboolean StepDirection(edict_t *ent, float yaw, float dist)
{
	float r = DEG2RAD(yaw);
	vec3_t move = { cosf(r) * dist, sinf(r) * dist, 0 };
	qboolean ok = SV_movestep(ent, move, false, WALKMOVE_NORMAL);
	SV_LinkEdict(ent, true);
	return ok;
}

void SV_MoveToOrigin(edict_t *ent, const vec3_t goal, float dist, int movetype)
{
	if (!(ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return;
	vec3_t dir;
	VectorSubtract(goal, ent->v.origin, dir);
	if (!(ent->v.flags & (FL_FLY | FL_SWIM)))
		dir[2] = 0;
	VectorNormalize(dir);

	if (movetype == MOVE_NORMAL) {
		float yaw = SV_VecToYaw(dir);
		if (StepDirection(ent, yaw, dist))
			return;
		// blocked: try turning a little either way, like the chase code in Quake
		static const float tries[] = { 45, -45, 90, -90 };
		for (int i = 0; i < 4; i++)
			if (StepDirection(ent, yaw + tries[i], dist))
				return;
	} else {
		vec3_t move;
		VectorScale(dir, dist, move);
		SV_movestep(ent, move, true, WALKMOVE_NORMAL);
	}
}
