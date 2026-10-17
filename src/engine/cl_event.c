// newen - client events: the game's PlaybackEvent calls reach the client DLL's event hooks
//
// The game DLL fires events by precache index; the client hooks them by name ("events/glock1.sc")
// and plays the effects: muzzle flash, shells, bullet holes, tracers. Events can be delayed, and
// FEV_UPDATE replaces one that is still waiting. The event API gives the hooks traces against the
// world and entities as the client sees them, which here are the server's edicts (one process).
//
// The engine never asks the game to skip the host (CanSkipPlayer is 0, no client-side weapon
// prediction), so FEV_NOTHOST events are played here too.
#include "cl_local.h"

typedef struct { char name[64]; pfnEventHook fn; } evhook_t;
static evhook_t *evhooks;
static int evhooks_count;

typedef struct {
	unsigned short index;
	double         time;
	event_args_t   args;
} qevent_t;
static qevent_t *events;
static int events_count, events_cap;

void CL_HookEvent(const char *name, pfnEventHook fn)
{
	if (!name)
		return;
	for (int i = 0; i < evhooks_count; i++)
		if (!stricmp(evhooks[i].name, name)) {
			evhooks[i].fn = fn;
			return;
		}
	evhooks = Mem_Realloc(evhooks, sizeof(evhook_t) * (evhooks_count + 1));
	Q_strncpy(evhooks[evhooks_count].name, name, sizeof(evhooks[0].name));
	evhooks[evhooks_count].fn = fn;
	evhooks_count++;
}

static const char *EventName(unsigned short index)
{
	return (index > 0 && index < sv.num_events) ? sv.event_precache[index] : NULL;
}

static qboolean IsNull(const float *v)
{
	return !v || (v[0] == 0 && v[1] == 0 && v[2] == 0);
}

void CL_PlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, const float *origin,
	const float *angles, float f1, float f2, int i1, int i2, int b1, int b2)
{
	if (!client_ok || !EventName(index))
		return;
	event_args_t a;
	memset(&a, 0, sizeof(a));
	a.flags = flags;
	// from the client itself: the local player fired it (its predicted weapons pass their own fake edict)
	qboolean real = invoker && sv.edicts && invoker >= sv.edicts && invoker < sv.edicts + sv.num_edicts;
	if (!real)
		invoker = (flags & FEV_CLIENT) ? SV_LocalPlayer() : NULL;
	if (invoker) {
		int n = NUM_FOR_EDICT(invoker);
		a.entindex = n;
		qboolean player = n >= 1 && n <= sv.maxclients;
		if (IsNull(origin))
			VectorCopy(invoker->v.origin, a.origin);
		// a player shoots where it looks
		if (IsNull(angles))
			VectorCopy(player ? invoker->v.v_angle : invoker->v.angles, a.angles);
		VectorCopy(invoker->v.velocity, a.velocity);
		a.ducking = player && (invoker->v.flags & FL_DUCKING) ? 1 : 0;
	}
	if (!IsNull(origin))
		VectorCopy(origin, a.origin);
	if (!IsNull(angles))
		VectorCopy(angles, a.angles);
	a.fparam1 = f1;
	a.fparam2 = f2;
	a.iparam1 = i1;
	a.iparam2 = i2;
	a.bparam1 = b1;
	a.bparam2 = b2;

	if (flags & FEV_UPDATE) {
		for (int i = 0; i < events_count; i++)
			if (events[i].index == index && events[i].args.entindex == a.entindex) {
				events[i].args = a;
				events[i].time = cl_time + delay;
				return;
			}
	}
	if (events_count == events_cap) {
		events_cap = events_cap ? events_cap * 2 : 64;
		events = Mem_Realloc(events, sizeof(qevent_t) * events_cap);
	}
	qevent_t *e = &events[events_count++];
	e->index = index;
	e->time = cl_time + delay;
	e->args = a;
}

void CL_FireEvents(void)
{
	// hooks may queue more events: walk by index, keeping the ones that aren't due yet
	int keep = 0;
	for (int i = 0; i < events_count; i++) {
		qevent_t ev = events[i];
		if (!ev.index)
			continue;   // killed (EV_KillEvents) while queued
		if (ev.time > cl_time) {
			events[keep++] = ev;
			continue;
		}
		const char *name = EventName(ev.index);
		pfnEventHook fn = NULL;
		for (int k = 0; name && k < evhooks_count; k++)
			if (!stricmp(evhooks[k].name, name))
				fn = evhooks[k].fn;
		if (!fn) {
			if (developer->value)
				Con_Printf("event %s isn't hooked by the client\n", name ? name : "?");
			continue;
		}
		CLIENT_CALL(name, fn(&ev.args));
	}
	events_count = keep;
}

void CL_ClearEvents(void)
{
	events_count = 0;
}

// ---------------------------------------------------------------- the event API

static void EV_PlaySound(int ent, float *origin, int channel, const char *sample, float volume, float attenuation,
	int flags, int pitch) {}   // sound arrives in step 6
static void EV_StopSound(int ent, int channel, const char *sample) {}
static void EV_StopAllSounds(int entnum, int entchannel) {}

static int EV_FindModelIndex(const char *name) { return sv.active ? SV_ModelIndex(name) : 0; }
static int EV_IsLocal(int playernum) { return playernum == 0; }

static int EV_LocalPlayerDucking(void)
{
	edict_t *pl = SV_LocalPlayer();
	return pl && (pl->v.flags & FL_DUCKING) ? 1 : 0;
}

static void EV_LocalPlayerViewheight(float *vh)
{
	edict_t *pl = SV_LocalPlayer();
	if (pl)
		VectorCopy(pl->v.view_ofs, vh);
	else
		VectorClear(vh);
}

static void EV_LocalPlayerBounds(int hull, float *mins, float *maxs) { SV_PM_HullBounds(hull, mins, maxs); }

static int EV_IndexFromTrace(pmtrace_t *tr)
{
	physent_t *pe = tr ? SV_PM_GetPhysent(tr->ent) : NULL;
	return pe ? pe->info : 0;
}

static physent_t *EV_GetPhysent(int idx) { return SV_PM_GetPhysent(idx); }
static void EV_SetUpPlayerPrediction(int dopred, int includelocal) {}
static void EV_PushPMStates(void) { SV_PM_PushClientState(); }
static void EV_PopPMStates(void) { SV_PM_PopClientState(); }
static void EV_SetSolidPlayers(int playernum) { SV_PM_SetSolidPlayers(playernum); }
static void EV_SetTraceHull(int hull) { SV_PM_SetTraceHull(hull); }

static void EV_PlayerTrace(float *start, float *end, int flags, int ignore_pe, pmtrace_t *tr)
{
	SV_PM_ClientTrace(start, end, flags, ignore_pe, tr);
}

static void EV_WeaponAnimation(int sequence, int body) { CL_SetViewModelAnim(sequence, body); }
static unsigned short EV_PrecacheEvent(int type, const char *name) { return SV_PrecacheEvent(type, name); }

static void EV_PlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, float *origin,
	float *angles, float f1, float f2, int i1, int i2, int b1, int b2)
{
	CL_PlaybackEvent(flags | FEV_CLIENT, invoker, index, delay, origin, angles, f1, f2, i1, i2, b1, b2);
}

static const char *EV_TraceTexture(int ground, float *vstart, float *vend)
{
	physent_t *pe = SV_PM_GetPhysent(ground);
	if (!pe || pe->info < 0 || pe->info >= sv.num_edicts)
		return NULL;
	return SV_TraceTexture(EDICT_NUM(pe->info), vstart, vend);
}

// called from inside event hooks too, so it only marks them; CL_FireEvents drops them
static void EV_KillEvents(int entnum, const char *name)
{
	for (int i = 0; i < events_count; i++) {
		const char *n = EventName(events[i].index);
		if (events[i].args.entindex == entnum && n && name && !stricmp(n, name))
			events[i].index = 0;
	}
}

void CL_InitEventAPI(event_api_t *api)
{
	memset(api, 0, sizeof(*api));
	api->version = 1;
	api->EV_PlaySound = EV_PlaySound;
	api->EV_StopSound = EV_StopSound;
	api->EV_FindModelIndex = EV_FindModelIndex;
	api->EV_IsLocal = EV_IsLocal;
	api->EV_LocalPlayerDucking = EV_LocalPlayerDucking;
	api->EV_LocalPlayerViewheight = EV_LocalPlayerViewheight;
	api->EV_LocalPlayerBounds = EV_LocalPlayerBounds;
	api->EV_IndexFromTrace = EV_IndexFromTrace;
	api->EV_GetPhysent = EV_GetPhysent;
	api->EV_SetUpPlayerPrediction = EV_SetUpPlayerPrediction;
	api->EV_PushPMStates = EV_PushPMStates;
	api->EV_PopPMStates = EV_PopPMStates;
	api->EV_SetSolidPlayers = EV_SetSolidPlayers;
	api->EV_SetTraceHull = EV_SetTraceHull;
	api->EV_PlayerTrace = EV_PlayerTrace;
	api->EV_WeaponAnimation = EV_WeaponAnimation;
	api->EV_PrecacheEvent = EV_PrecacheEvent;
	api->EV_PlaybackEvent = EV_PlaybackEvent;
	api->EV_TraceTexture = EV_TraceTexture;
	api->EV_StopAllSounds = EV_StopAllSounds;
	api->EV_KillEvents = EV_KillEvents;
}
