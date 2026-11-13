// newen - the physics library (Jolt) behind phys.h; the only C++ file in newen
//
// Jolt works in metres; the game in inches. Everything crossing phys.h is converted here and nowhere else.
// Jolt runs single-threaded: newen's frame is short and 32-bit, and it keeps MinGW free of thread
// questions. Two object layers: things that never move, and everything else (static against static is
// never tested).
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "phys.h"

extern "C" void Con_Printf(const char *fmt, ...);
extern "C" void Con_DPrintf(const char *fmt, ...);
extern "C" void Con_Warnf(const char *fmt, ...);

using namespace JPH;

static const float METRES = 0.0254f;          // one inch
static const float INCHES = 1.0f / 0.0254f;
static const float SUBSTEP = 1.0f / 120.0f;
static const float DEG = 3.14159265358979f / 180.0f;

// ---------------------------------------------------------------- layers

namespace Layers {
	static const ObjectLayer STILL = 0;
	static const ObjectLayer MOVING = 1;
	static const uint COUNT = 2;
}

class BroadPhaseLayers final : public BroadPhaseLayerInterface {
public:
	uint GetNumBroadPhaseLayers() const override { return Layers::COUNT; }
	BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override { return BroadPhaseLayer((BroadPhaseLayer::Type)layer); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(BroadPhaseLayer layer) const override
	{
		return (BroadPhaseLayer::Type)layer == Layers::STILL ? "still" : "moving";
	}
#endif
};

class ObjectVsBroadPhase final : public ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(ObjectLayer a, BroadPhaseLayer b) const override
	{
		return a == Layers::MOVING || (BroadPhaseLayer::Type)b == Layers::MOVING;
	}
};

class ObjectPairs final : public ObjectLayerPairFilter {
public:
	bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override
	{
		return a == Layers::MOVING || b == Layers::MOVING;
	}
};

// ---------------------------------------------------------------- start and stop

static bool started;
static BroadPhaseLayers bp_layers;
static ObjectVsBroadPhase obj_vs_bp;
static ObjectPairs obj_pairs;

static void TraceToConsole(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_DPrintf("[Jolt] %s\n", buf);
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertToConsole(const char *expr, const char *msg, const char *file, uint line)
{
	Con_Warnf("[Jolt] assertion %s (%s) at %s:%u\n", expr, msg ? msg : "", file, line);
	return false;   // don't break into a debugger
}
#endif

extern "C" int Phys_Init(void)
{
	if (started)
		return 1;
	RegisterDefaultAllocator();
	Trace = TraceToConsole;
	JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertToConsole;)
	Factory::sInstance = new Factory();
	RegisterTypes();
	started = true;
	return 1;
}

extern "C" void Phys_Shutdown(void)
{
	if (!started)
		return;
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
	started = false;
}

extern "C" const char *Phys_Version(void)
{
	static char v[96];
	snprintf(v, sizeof(v), "Jolt %d.%d.%d (%s, %s precision)", JPH_VERSION_MAJOR, JPH_VERSION_MINOR,
		JPH_VERSION_PATCH,
#if defined(JPH_USE_AVX2)
		"AVX2",
#elif defined(JPH_USE_SSE4_1)
		"SSE4.1",
#elif defined(JPH_USE_SSE)
		"SSE2",
#else
		"no SIMD",
#endif
#ifdef JPH_DOUBLE_PRECISION
		"double"
#else
		"single"
#endif
	);
	return v;
}

// ---------------------------------------------------------------- conversions

static RVec3 ToJolt(const float *v) { return RVec3(v[0] * METRES, v[1] * METRES, v[2] * METRES); }

static void FromJolt(RVec3Arg v, float *out)
{
	out[0] = (float)v.GetX() * INCHES;
	out[1] = (float)v.GetY() * INCHES;
	out[2] = (float)v.GetZ() * INCHES;
}

// Half-Life's angles: yaw about Z, then pitch about Y (positive tips the nose down), then roll about X
static Quat AnglesToQuat(const float *a)
{
	if (!a)
		return Quat::sIdentity();
	return Quat::sRotation(Vec3::sAxisZ(), a[1] * DEG) * Quat::sRotation(Vec3::sAxisY(), a[0] * DEG) *
		Quat::sRotation(Vec3::sAxisX(), a[2] * DEG);
}

static void QuatToAngles(QuatArg q, float *out)
{
	Vec3 f = q.RotateAxisX(), l = q.RotateAxisY(), u = q.RotateAxisZ();
	float sp = -f.GetZ();
	sp = sp > 1 ? 1 : sp < -1 ? -1 : sp;
	out[0] = asinf(sp) / DEG;
	if (fabsf(sp) < 0.9999f) {
		out[1] = atan2f(f.GetY(), f.GetX()) / DEG;
		out[2] = atan2f(l.GetZ(), u.GetZ()) / DEG;
	} else {
		// straight up or down: yaw and roll are the same turn; put it all in yaw
		out[1] = atan2f(-l.GetX(), l.GetY()) / DEG;
		out[2] = 0;
	}
}

// ---------------------------------------------------------------- worlds

struct physworld_s {
	PhysicsSystem system;
	TempAllocatorImpl temp;
	JobSystemSingleThreaded jobs;
	float leftover = 0;
	int bodies = 0;

	physworld_s() : temp(8 * 1024 * 1024), jobs(cMaxPhysicsJobs) {}
};

extern "C" physworld_t *Phys_CreateWorld(int maxbodies, float gravity)
{
	if (!Phys_Init())
		return nullptr;
	if (maxbodies < 16)
		maxbodies = 16;
	physworld_t *w = new physworld_t();
	w->system.Init((uint)maxbodies, 0, (uint)maxbodies * 2, (uint)maxbodies * 2, bp_layers, obj_vs_bp, obj_pairs);
	Phys_SetGravity(w, gravity);
	return w;
}

extern "C" void Phys_DestroyWorld(physworld_t *w)
{
	if (!w)
		return;
	BodyInterface &bi = w->system.GetBodyInterfaceNoLock();
	BodyIDVector ids;
	w->system.GetBodies(ids);
	for (BodyID id : ids) {
		bi.RemoveBody(id);
		bi.DestroyBody(id);
	}
	delete w;
}

extern "C" void Phys_SetGravity(physworld_t *w, float gravity)
{
	if (w)
		w->system.SetGravity(Vec3(0, 0, -gravity * METRES));
}

extern "C" void Phys_Step(physworld_t *w, float dt)
{
	if (!w || dt <= 0)
		return;
	w->leftover += dt;
	// after a long hitch, give up on catching up rather than spend the next frames simulating
	if (w->leftover > 0.25f)
		w->leftover = 0.25f;
	while (w->leftover >= SUBSTEP) {
		w->system.Update(SUBSTEP, 1, &w->temp, &w->jobs);
		w->leftover -= SUBSTEP;
	}
}

extern "C" void Phys_Optimize(physworld_t *w)
{
	if (w)
		w->system.OptimizeBroadPhase();
}

extern "C" int Phys_BodyCount(physworld_t *w)
{
	return w ? w->bodies : 0;
}

// ---------------------------------------------------------------- bodies

static BodyInterface &Bodies(physworld_t *w) { return w->system.GetBodyInterface(); }

static EMotionType Motion(physmotion_t m)
{
	return m == PHYS_STATIC ? EMotionType::Static : m == PHYS_KINEMATIC ? EMotionType::Kinematic : EMotionType::Dynamic;
}

extern "C" physbody_t Phys_AddBox(physworld_t *w, const float halfsize[3], const float origin[3], const float angles[3],
	physmotion_t motion, float mass)
{
	if (!w)
		return PHYS_NOBODY;
	Vec3 half(halfsize[0] * METRES, halfsize[1] * METRES, halfsize[2] * METRES);
	// Jolt's boxes round their edges by a convex radius, which must fit inside the box
	float smallest = half.ReduceMin();
	if (smallest <= 0.001f)
		return PHYS_NOBODY;
	float radius = smallest * 0.5f < cDefaultConvexRadius ? smallest * 0.5f : cDefaultConvexRadius;
	BoxShapeSettings box(half, radius);
	ShapeSettings::ShapeResult shape = box.Create();
	if (shape.HasError()) {
		Con_Warnf("Phys_AddBox: %s\n", shape.GetError().c_str());
		return PHYS_NOBODY;
	}
	BodyCreationSettings s(shape.Get(), ToJolt(origin), AnglesToQuat(angles), Motion(motion),
		motion == PHYS_STATIC ? Layers::STILL : Layers::MOVING);
	if (motion == PHYS_DYNAMIC && mass > 0) {
		s.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
		s.mMassPropertiesOverride.mMass = mass;
	}
	BodyID id = Bodies(w).CreateAndAddBody(s, motion == PHYS_STATIC ? EActivation::DontActivate : EActivation::Activate);
	if (id.IsInvalid()) {
		Con_Warnf("Physics: out of bodies\n");
		return PHYS_NOBODY;
	}
	w->bodies++;
	return id.GetIndexAndSequenceNumber();
}

extern "C" void Phys_RemoveBody(physworld_t *w, physbody_t b)
{
	if (!w || b == PHYS_NOBODY)
		return;
	BodyID id(b);
	BodyInterface &bi = Bodies(w);
	if (!bi.IsAdded(id))
		return;
	bi.RemoveBody(id);
	bi.DestroyBody(id);
	w->bodies--;
}

extern "C" void Phys_GetPose(physworld_t *w, physbody_t b, float origin[3], float angles[3])
{
	RVec3 pos;
	Quat rot;
	Bodies(w).GetPositionAndRotation(BodyID(b), pos, rot);
	if (origin)
		FromJolt(pos, origin);
	if (angles)
		QuatToAngles(rot, angles);
}

extern "C" void Phys_SetPose(physworld_t *w, physbody_t b, const float origin[3], const float angles[3])
{
	Bodies(w).SetPositionAndRotation(BodyID(b), ToJolt(origin), AnglesToQuat(angles), EActivation::Activate);
}

extern "C" void Phys_GetVelocity(physworld_t *w, physbody_t b, float velocity[3])
{
	Vec3 v = Bodies(w).GetLinearVelocity(BodyID(b));
	velocity[0] = v.GetX() * INCHES;
	velocity[1] = v.GetY() * INCHES;
	velocity[2] = v.GetZ() * INCHES;
}

extern "C" void Phys_SetVelocity(physworld_t *w, physbody_t b, const float velocity[3])
{
	Bodies(w).SetLinearVelocity(BodyID(b), Vec3(velocity[0] * METRES, velocity[1] * METRES, velocity[2] * METRES));
}

extern "C" void Phys_AddImpulse(physworld_t *w, physbody_t b, const float impulse[3])
{
	Bodies(w).AddImpulse(BodyID(b), Vec3(impulse[0] * METRES, impulse[1] * METRES, impulse[2] * METRES));
}

extern "C" int Phys_IsAwake(physworld_t *w, physbody_t b)
{
	return Bodies(w).IsActive(BodyID(b)) ? 1 : 0;
}
