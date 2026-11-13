// newen - physics in the engine: commands and checks (later: the map's world mesh, brush entities,
// the player's capsule, syncing with edicts)
//
// Jolt starts the first time something needs it, so a game that never uses physics never pays for it.
#include "common.h"
#include "mathlib.h"
#include "phys.h"

// ---------------------------------------------------------------- phys_test

static int failures;

static void Check(int ok, const char *what, const char *detail)
{
	Con_Printf("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
	if (!ok)
		failures++;
}

static float AngleDiff(float a, float b)
{
	float d = fmodf(a - b, 360);
	if (d > 180)
		d -= 360;
	if (d < -180)
		d += 360;
	return fabsf(d);
}

// a box dropped tumbling onto a floor must come to rest flat on one face, the right height above it,
// still, and asleep; angles and impulses must come back in the game's units
static void PhysTest_f(void)
{
	if (!Phys_Init()) {
		Con_Printf("phys_test: FAIL (Jolt didn't start)\n");
		return;
	}
	Con_Printf("phys_test: %s\n", Phys_Version());
	failures = 0;
	double t0 = Sys_FloatTime();
	physworld_t *w = Phys_CreateWorld(64, 800);
	char d[256];

	// the floor's top is at z = 0
	static const float floorhalf[3] = { 512, 512, 16 }, floororg[3] = { 0, 0, -16 };
	physbody_t floorb = Phys_AddBox(w, floorhalf, floororg, NULL, PHYS_STATIC, 0);
	Phys_Optimize(w);
	Check(floorb != PHYS_NOBODY, "static floor", NULL);

	// angles survive a round trip
	static const float rt_org[3] = { 0, 0, 500 }, rt_ang[3] = { 30, 45, -20 }, half8[3] = { 8, 8, 8 };
	physbody_t k = Phys_AddBox(w, half8, rt_org, rt_ang, PHYS_KINEMATIC, 0);
	float org[3], ang[3];
	Phys_GetPose(w, k, org, ang);
	float worst = 0;
	for (int i = 0; i < 3; i++)
		worst = fmaxf(worst, AngleDiff(ang[i], rt_ang[i]));
	snprintf(d, sizeof(d), "(%.3f %.3f %.3f), off by %.4f degrees", ang[0], ang[1], ang[2], worst);
	Check(worst < 0.01f && fabsf(org[2] - 500) < 0.01f, "angles round trip", d);
	Phys_RemoveBody(w, k);

	// a 20 kg box: an impulse of 2000 kg*in/s makes 100 in/s
	static const float side_org[3] = { 200, 0, 8 }, push[3] = { 2000, 0, 0 };
	physbody_t s = Phys_AddBox(w, half8, side_org, NULL, PHYS_DYNAMIC, 20);
	Phys_AddImpulse(w, s, push);
	float vel[3];
	Phys_GetVelocity(w, s, vel);
	snprintf(d, sizeof(d), "%.2f in/s", vel[0]);
	Check(fabsf(vel[0] - 100) < 0.5f, "impulse in game units", d);

	// the tumbling drop
	static const float drop_org[3] = { 0, 0, 200 }, drop_ang[3] = { 20, 30, 10 };
	physbody_t b = Phys_AddBox(w, half8, drop_org, drop_ang, PHYS_DYNAMIC, 20);
	Check(b != PHYS_NOBODY, "dynamic box", NULL);
	float lowest = 1e9f;
	int steps = 0;
	for (float t = 0; t < 4; t += 1.0f / 60) {   // 4 s of 60 fps frames
		Phys_Step(w, 1.0f / 60);
		Phys_GetPose(w, b, org, NULL);
		lowest = fminf(lowest, org[2]);
		steps++;
	}
	Phys_GetPose(w, b, org, ang);
	Phys_GetVelocity(w, b, vel);
	snprintf(d, sizeof(d), "z %.2f (lowest %.2f)", org[2], lowest);
	Check(fabsf(org[2] - 8) < 0.5f && lowest > 6, "rests on the floor", d);
	vec3_t f, r, u;
	AngleVectors(ang, f, r, u);
	float flat = fmaxf(fabsf(f[2]), fmaxf(fabsf(r[2]), fabsf(u[2])));
	snprintf(d, sizeof(d), "angles %.1f %.1f %.1f", ang[0], ang[1], ang[2]);
	Check(flat > 0.999f, "lies flat on a face", d);
	snprintf(d, sizeof(d), "%.3f in/s", VectorLength(vel));
	Check(VectorLength(vel) < 0.5f, "came to rest", d);
	Check(!Phys_IsAwake(w, b), "went to sleep", NULL);
	Phys_GetPose(w, s, org, NULL);
	snprintf(d, sizeof(d), "slid to x %.1f", org[0]);
	Check(org[0] > 210 && org[0] < 400 && !Phys_IsAwake(w, s), "pushed box slid and stopped", d);
	Check(Phys_BodyCount(w) == 3, "body count", NULL);

	Phys_DestroyWorld(w);
	Con_Printf("phys_test: %s (%d frames simulated in %.1f ms)\n", failures ? "FAIL" : "PASS", steps,
		(Sys_FloatTime() - t0) * 1000);
}

void Phys_InitCommands(void)
{
	Cmd_AddCommand("phys_test", PhysTest_f, "checks the physics library: a box dropped on a floor (no map needed)");
}
