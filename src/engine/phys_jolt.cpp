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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

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
	static const ObjectLayer RAGDOLL = 2;   // as MOVING, and the players' bodies push it
	static const ObjectLayer PLAYER = 3;    // meets ragdoll parts only
	static const uint COUNT = 2;            // broad phase layers: still, everything else
}

class BroadPhaseLayers final : public BroadPhaseLayerInterface {
public:
	uint GetNumBroadPhaseLayers() const override { return Layers::COUNT; }
	BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override
	{
		return BroadPhaseLayer((BroadPhaseLayer::Type)(layer == Layers::STILL ? 0 : 1));
	}
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
		return a != Layers::STILL || (BroadPhaseLayer::Type)b != 0;
	}
};

class ObjectPairs final : public ObjectLayerPairFilter {
public:
	bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override
	{
		if (a == Layers::PLAYER || b == Layers::PLAYER)
			return a == Layers::RAGDOLL || b == Layers::RAGDOLL;
		return a != Layers::STILL || b != Layers::STILL;
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
	int bodies = 0;
	Array<Ref<GroupFilterTable>> groups;   // physgroup_t - 1; freed slots are null and reused
	Array<Ref<Constraint>> joints;         // physjoint_t - 1, the same way
	Array<physcontact_t> contacts;         // new touches of tagged bodies since Phys_TakeContacts
	ContactListener *listener = nullptr;

	physworld_s() : temp(8 * 1024 * 1024), jobs(cMaxPhysicsJobs) {}
};

// Tagged bodies' new touches, for impact sounds and Lua's PhysicsCollide. Jolt runs on one thread here,
// so the list needs no lock.
class TouchList final : public ContactListener {
public:
	explicit TouchList(physworld_t *w) : world(w) {}
	void OnContactAdded(const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &) override
	{
		int t1 = (int)b1.GetUserData(), t2 = (int)b2.GetUserData();
		if ((t1 <= 0 && t2 <= 0) || world->contacts.size() >= 256 || m.mRelativeContactPointsOn1.empty())
			return;
		RVec3 p = m.GetWorldSpaceContactPointOn1(0);
		Vec3 v1 = b1.IsStatic() ? Vec3::sZero() : b1.GetPointVelocity(p);
		Vec3 v2 = b2.IsStatic() ? Vec3::sZero() : b2.GetPointVelocity(p);
		physcontact_t c;
		c.tag1 = t1;
		c.tag2 = t2;
		c.speed = fabsf((v2 - v1).Dot(m.mWorldSpaceNormal)) * INCHES;
		for (int i = 0; i < 3; i++) {
			c.pos[i] = (float)p[i] * INCHES;
			c.normal[i] = m.mWorldSpaceNormal[i];
		}
		world->contacts.push_back(c);
	}
private:
	physworld_t *world;
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
	w->listener = new TouchList(w);
	w->system.SetContactListener(w->listener);
	return w;
}

extern "C" void Phys_DestroyWorld(physworld_t *w)
{
	if (!w)
		return;
	for (Ref<Constraint> &c : w->joints)
		if (c != nullptr)
			w->system.RemoveConstraint(c);
	BodyInterface &bi = w->system.GetBodyInterfaceNoLock();
	BodyIDVector ids;
	w->system.GetBodies(ids);
	for (BodyID id : ids) {
		bi.RemoveBody(id);
		bi.DestroyBody(id);
	}
	w->system.SetContactListener(nullptr);
	delete w->listener;
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
	// the frame's whole time, in equal substeps no longer than 1/120 s, so kinematic bodies told to reach
	// a pose in dt (Phys_MoveKinematic) get there exactly
	if (dt > 0.25f)
		dt = 0.25f;
	int steps = (int)ceilf(dt / SUBSTEP - 0.001f);
	w->system.Update(dt, steps < 1 ? 1 : steps, &w->temp, &w->jobs);
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

extern "C" physbody_t Phys_AddMesh(physworld_t *w, const float *tris, int numtris, const float origin[3],
	const float angles[3], physmotion_t motion)
{
	if (!w || numtris <= 0 || motion == PHYS_DYNAMIC)
		return PHYS_NOBODY;
	TriangleList list;
	list.reserve((size_t)numtris);
	for (int i = 0; i < numtris; i++) {
		const float *t = tris + i * 9;
		list.push_back(Triangle(Float3(t[0] * METRES, t[1] * METRES, t[2] * METRES),
			Float3(t[3] * METRES, t[4] * METRES, t[5] * METRES), Float3(t[6] * METRES, t[7] * METRES, t[8] * METRES)));
	}
	MeshShapeSettings mesh(list);   // drops degenerate triangles itself
	ShapeSettings::ShapeResult shape = mesh.Create();
	if (shape.HasError()) {
		Con_Warnf("Phys_AddMesh: %s\n", shape.GetError().c_str());
		return PHYS_NOBODY;
	}
	BodyCreationSettings s(shape.Get(), ToJolt(origin), AnglesToQuat(angles), Motion(motion),
		motion == PHYS_STATIC ? Layers::STILL : Layers::MOVING);
	BodyID id = Bodies(w).CreateAndAddBody(s, EActivation::DontActivate);
	if (id.IsInvalid()) {
		Con_Warnf("Physics: out of bodies\n");
		return PHYS_NOBODY;
	}
	w->bodies++;
	return id.GetIndexAndSequenceNumber();
}

// Meshes can't move in Jolt; moving map pieces (doors, lifts) are made of convex pieces instead
extern "C" physbody_t Phys_AddConvexSet(physworld_t *w, const float *points, const int *counts, int numpieces,
	const float origin[3], const float angles[3], physmotion_t motion, float mass)
{
	if (!w || numpieces <= 0)
		return PHYS_NOBODY;
	StaticCompoundShapeSettings compound;
	int used = 0;
	for (int i = 0; i < numpieces; i++) {
		Array<Vec3> pts;
		pts.reserve((size_t)counts[i]);
		for (int k = 0; k < counts[i]; k++, points += 3)
			pts.push_back(Vec3(points[0] * METRES, points[1] * METRES, points[2] * METRES));
		// a small rounding radius: map brushes can be thin (a 4-unit door is 10 cm)
		Ref<ConvexHullShapeSettings> hull = new ConvexHullShapeSettings(pts, 0.005f);
		ShapeSettings::ShapeResult r = hull->Create();
		if (r.HasError())
			continue;   // flat or degenerate: nothing to collide with
		compound.AddShape(Vec3::sZero(), Quat::sIdentity(), r.Get());
		used++;
	}
	if (!used)
		return PHYS_NOBODY;
	ShapeSettings::ShapeResult shape = compound.Create();
	if (shape.HasError()) {
		Con_Warnf("Phys_AddConvexSet: %s\n", shape.GetError().c_str());
		return PHYS_NOBODY;
	}
	BodyCreationSettings s(shape.Get(), ToJolt(origin), AnglesToQuat(angles), Motion(motion),
		motion == PHYS_STATIC ? Layers::STILL : Layers::MOVING);
	if (motion == PHYS_DYNAMIC && mass > 0) {
		s.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
		s.mMassPropertiesOverride.mMass = mass;
	}
	BodyID id = Bodies(w).CreateAndAddBody(s, motion == PHYS_DYNAMIC ? EActivation::Activate : EActivation::DontActivate);
	if (id.IsInvalid()) {
		Con_Warnf("Physics: out of bodies\n");
		return PHYS_NOBODY;
	}
	w->bodies++;
	return id.GetIndexAndSequenceNumber();
}

extern "C" void Phys_MoveKinematic(physworld_t *w, physbody_t b, const float origin[3], const float angles[3], float dt)
{
	if (w && dt > 0)
		Bodies(w).MoveKinematic(BodyID(b), ToJolt(origin), AnglesToQuat(angles), dt);
}

extern "C" void Phys_RemoveBody(physworld_t *w, physbody_t b)
{
	if (!w || b == PHYS_NOBODY)
		return;
	BodyID id(b);
	BodyInterface &bi = Bodies(w);
	if (!bi.IsAdded(id))
		return;
	// what rested on it is asleep and wouldn't notice it's gone
	AABox box = bi.GetTransformedShape(id).GetWorldSpaceBounds();
	box.ExpandBy(Vec3::sReplicate(0.1f));
	bi.RemoveBody(id);
	bi.ActivateBodiesInAABox(box, BroadPhaseLayerFilter(), ObjectLayerFilter());
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

// ---------------------------------------------------------------- jointed things

static Mat44 ToJoltMatrix(const float m[3][4])
{
	return Mat44(Vec4(m[0][0], m[1][0], m[2][0], 0), Vec4(m[0][1], m[1][1], m[2][1], 0), Vec4(m[0][2], m[1][2], m[2][2], 0),
		Vec4(m[0][3] * METRES, m[1][3] * METRES, m[2][3] * METRES, 1));
}

// one hull per piece; flat or degenerate pieces are left out. Null if nothing is left.
static RefConst<Shape> ConvexSetShape(const float *points, const int *counts, int numpieces, const char *who)
{
	StaticCompoundShapeSettings compound;
	int used = 0;
	for (int i = 0; i < numpieces; i++) {
		Array<Vec3> pts;
		pts.reserve((size_t)counts[i]);
		for (int k = 0; k < counts[i]; k++, points += 3)
			pts.push_back(Vec3(points[0] * METRES, points[1] * METRES, points[2] * METRES));
		Ref<ConvexHullShapeSettings> hull = new ConvexHullShapeSettings(pts, 0.005f);
		ShapeSettings::ShapeResult r = hull->Create();
		if (r.HasError())
			continue;
		compound.AddShape(Vec3::sZero(), Quat::sIdentity(), r.Get());
		used++;
	}
	if (!used)
		return nullptr;
	ShapeSettings::ShapeResult shape = compound.Create();
	if (shape.HasError()) {
		Con_Warnf("%s: %s\n", who, shape.GetError().c_str());
		return nullptr;
	}
	return shape.Get();
}

extern "C" physbody_t Phys_AddConvexSetAt(physworld_t *w, const float *points, const int *counts, int numpieces,
	const float pose[3][4], physmotion_t motion, float mass, float inertiascale)
{
	if (!w || numpieces <= 0)
		return PHYS_NOBODY;
	RefConst<Shape> shape = ConvexSetShape(points, counts, numpieces, "Phys_AddConvexSetAt");
	if (!shape)
		return PHYS_NOBODY;
	Mat44 m = ToJoltMatrix(pose);
	BodyCreationSettings s(shape, RVec3(m.GetTranslation()), m.GetQuaternion().Normalized(), Motion(motion),
		motion == PHYS_STATIC ? Layers::STILL : Layers::MOVING);
	if (motion == PHYS_DYNAMIC && mass > 0) {
		s.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
		s.mMassPropertiesOverride.mMass = mass;
	}
	if (inertiascale > 0)
		s.mInertiaMultiplier = inertiascale;
	BodyID id = Bodies(w).CreateAndAddBody(s, motion == PHYS_DYNAMIC ? EActivation::Activate : EActivation::DontActivate);
	if (id.IsInvalid()) {
		Con_Warnf("Physics: out of bodies\n");
		return PHYS_NOBODY;
	}
	w->bodies++;
	return id.GetIndexAndSequenceNumber();
}

extern "C" void Phys_GetMatrix(physworld_t *w, physbody_t b, float pose[3][4])
{
	RVec3 pos;
	Quat rot;
	Bodies(w).GetPositionAndRotation(BodyID(b), pos, rot);
	Mat44 m = Mat44::sRotation(rot);
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			pose[r][c] = m(r, c);
		pose[r][3] = (float)pos[r] * INCHES;
	}
}

extern "C" void Phys_SetMotion(physworld_t *w, physbody_t b, const float velocity[3], const float spin[3])
{
	BodyInterface &bi = Bodies(w);
	BodyID id(b);
	Vec3 v(velocity[0] * METRES, velocity[1] * METRES, velocity[2] * METRES);
	Vec3 a(spin[0] * DEG, spin[1] * DEG, spin[2] * DEG);
	// Jolt's speed is the centre of mass's, which a turning body moves differently from its origin
	Vec3 arm = Vec3(bi.GetCenterOfMassPosition(id) - bi.GetPosition(id));
	bi.SetLinearAndAngularVelocity(id, v + a.Cross(arm), a);
}

extern "C" void Phys_SetDamping(physworld_t *w, physbody_t b, float linear, float angular)
{
	BodyLockWrite lock(w->system.GetBodyLockInterface(), BodyID(b));
	if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
		return;
	MotionProperties *mp = lock.GetBody().GetMotionProperties();
	mp->SetLinearDamping(linear < 0 ? 0 : linear);
	mp->SetAngularDamping(angular < 0 ? 0 : angular);
}

extern "C" void Phys_Sleep(physworld_t *w, physbody_t b)
{
	Bodies(w).DeactivateBody(BodyID(b));
}

extern "C" physgroup_t Phys_NewGroup(physworld_t *w, int numparts)
{
	if (!w || numparts < 1)
		return 0;
	Ref<GroupFilterTable> table = new GroupFilterTable((uint)numparts);
	for (int i = 0; i < numparts; i++)
		for (int j = i + 1; j < numparts; j++)
			table->DisableCollision((CollisionGroup::SubGroupID)i, (CollisionGroup::SubGroupID)j);
	size_t slot = 0;
	while (slot < w->groups.size() && w->groups[slot] != nullptr)
		slot++;
	if (slot == w->groups.size())
		w->groups.push_back(table);
	else
		w->groups[slot] = table;
	return (physgroup_t)slot + 1;
}

static GroupFilterTable *Group(physworld_t *w, physgroup_t g)
{
	return w && g > 0 && (size_t)g <= w->groups.size() ? w->groups[(size_t)g - 1].GetPtr() : nullptr;
}

extern "C" void Phys_GroupCollide(physworld_t *w, physgroup_t g, int part1, int part2)
{
	GroupFilterTable *t = Group(w, g);
	if (t && part1 != part2 && part1 >= 0 && part2 >= 0)
		t->EnableCollision((CollisionGroup::SubGroupID)part1, (CollisionGroup::SubGroupID)part2);
}

extern "C" void Phys_SetGroup(physworld_t *w, physbody_t b, physgroup_t g, int part)
{
	GroupFilterTable *t = Group(w, g);
	if (!t)
		return;
	BodyLockWrite lock(w->system.GetBodyLockInterface(), BodyID(b));
	if (lock.Succeeded())
		lock.GetBody().SetCollisionGroup(CollisionGroup(t, (CollisionGroup::GroupID)g, (CollisionGroup::SubGroupID)part));
}

extern "C" void Phys_FreeGroup(physworld_t *w, physgroup_t g)
{
	if (Group(w, g))
		w->groups[(size_t)g - 1] = nullptr;
}

extern "C" physjoint_t Phys_AddJoint(physworld_t *w, physbody_t parent, physbody_t child, const float frame1[3][4],
	const float frame2[3][4], const float mindeg[3], const float maxdeg[3], float friction)
{
	if (!w || parent == PHYS_NOBODY || child == PHYS_NOBODY || parent == child)
		return 0;
	BodyID ids[2] = { BodyID(parent), BodyID(child) };
	Ref<Constraint> joint;
	{
		BodyLockMultiWrite lock(w->system.GetBodyLockInterface(), ids, 2);
		Body *b1 = lock.GetBody(0), *b2 = lock.GetBody(1);
		if (!b1 || !b2)
			return 0;
		Mat44 f1 = ToJoltMatrix(frame1), f2 = ToJoltMatrix(frame2);
		SixDOFConstraintSettings s;
		// the frames are in the bodies' own spaces; Jolt wants them from each centre of mass
		s.mSpace = EConstraintSpace::LocalToBodyCOM;
		s.mPosition1 = RVec3(f1.GetTranslation() - b1->GetShape()->GetCenterOfMass());
		s.mAxisX1 = f1.GetAxisX().Normalized();
		s.mAxisY1 = f1.GetAxisY().Normalized();
		s.mPosition2 = RVec3(f2.GetTranslation() - b2->GetShape()->GetCenterOfMass());
		s.mAxisX2 = f2.GetAxisX().Normalized();
		s.mAxisY2 = f2.GetAxisY().Normalized();
		s.mSwingType = ESwingType::Pyramid;   // the limits aren't the same both ways (a knee)
		s.MakeFixedAxis(SixDOFConstraintSettings::EAxis::TranslationX);
		s.MakeFixedAxis(SixDOFConstraintSettings::EAxis::TranslationY);
		s.MakeFixedAxis(SixDOFConstraintSettings::EAxis::TranslationZ);
		for (int i = 0; i < 3; i++) {
			SixDOFConstraintSettings::EAxis axis = (SixDOFConstraintSettings::EAxis)(SixDOFConstraintSettings::EAxis::RotationX + i);
			float lo = mindeg[i], hi = maxdeg[i];
			lo = lo < -180 ? -180 : lo > 180 ? 180 : lo;
			hi = hi < -180 ? -180 : hi > 180 ? 180 : hi;
			if (hi - lo < 0.5f) {
				// no turning: a range that doesn't hold zero can't be fixed there, so it stays a (tiny) range
				if (lo <= 0 && hi >= 0)
					s.MakeFixedAxis(axis);
				else
					s.SetLimitedAxis(axis, lo * DEG, (lo + 0.5f) * DEG);
			} else
				s.SetLimitedAxis(axis, lo * DEG, hi * DEG);
			s.mMaxFriction[axis] = friction > 0 ? friction : 0;
		}
		joint = s.Create(*b1, *b2);
	}
	if (joint == nullptr)
		return 0;
	w->system.AddConstraint(joint);
	size_t slot = 0;
	while (slot < w->joints.size() && w->joints[slot] != nullptr)
		slot++;
	if (slot == w->joints.size())
		w->joints.push_back(joint);
	else
		w->joints[slot] = joint;
	return (physjoint_t)slot + 1;
}

extern "C" void Phys_RemoveJoint(physworld_t *w, physjoint_t j)
{
	if (!w || j <= 0 || (size_t)j > w->joints.size() || w->joints[(size_t)j - 1] == nullptr)
		return;
	w->system.RemoveConstraint(w->joints[(size_t)j - 1]);
	w->joints[(size_t)j - 1] = nullptr;
}

// ---------------------------------------------------------------- more for props

extern "C" void Phys_GetBounds(physworld_t *w, physbody_t b, float mins[3], float maxs[3])
{
	AABox box = Bodies(w).GetTransformedShape(BodyID(b)).GetWorldSpaceBounds();
	for (int i = 0; i < 3; i++) {
		mins[i] = box.mMin[i] * INCHES;
		maxs[i] = box.mMax[i] * INCHES;
	}
}

extern "C" void Phys_GetMassCenter(physworld_t *w, physbody_t b, float pos[3])
{
	FromJolt(Bodies(w).GetCenterOfMassPosition(BodyID(b)), pos);
}

extern "C" void Phys_AddImpulseAt(physworld_t *w, physbody_t b, const float impulse[3], const float pos[3])
{
	Bodies(w).AddImpulse(BodyID(b), Vec3(impulse[0] * METRES, impulse[1] * METRES, impulse[2] * METRES), ToJolt(pos));
}

extern "C" void Phys_GetSpin(physworld_t *w, physbody_t b, float spin[3])
{
	Vec3 a = Bodies(w).GetAngularVelocity(BodyID(b));
	for (int i = 0; i < 3; i++)
		spin[i] = a[i] / DEG;
}

extern "C" void Phys_Wake(physworld_t *w, physbody_t b)
{
	Bodies(w).ActivateBody(BodyID(b));
}

extern "C" void Phys_EnableMotion(physworld_t *w, physbody_t b, int on)
{
	BodyInterface &bi = Bodies(w);
	BodyID id(b);
	// the layer stays "moving": things still land on it, and it can be let go again
	bi.SetMotionType(id, on ? EMotionType::Dynamic : EMotionType::Static, on ? EActivation::Activate : EActivation::DontActivate);
}

extern "C" float Phys_GetMass(physworld_t *w, physbody_t b)
{
	BodyLockRead lock(w->system.GetBodyLockInterface(), BodyID(b));
	if (!lock.Succeeded() || !lock.GetBody().GetMotionPropertiesUnchecked())
		return 0;
	float inv = lock.GetBody().GetMotionPropertiesUnchecked()->GetInverseMassUnchecked();
	return inv > 0 ? 1.0f / inv : 0;
}

extern "C" void Phys_SetMass(physworld_t *w, physbody_t b, float mass)
{
	if (mass <= 0)
		return;
	BodyLockWrite lock(w->system.GetBodyLockInterface(), BodyID(b));
	if (!lock.Succeeded() || !lock.GetBody().GetMotionPropertiesUnchecked())
		return;
	MotionProperties *mp = lock.GetBody().GetMotionPropertiesUnchecked();
	float inv = mp->GetInverseMassUnchecked();
	if (inv > 0)
		mp->ScaleToMass(mass);   // the resistance to turning grows with it
}

extern "C" void Phys_SetFriction(physworld_t *w, physbody_t b, float friction)
{
	Bodies(w).SetFriction(BodyID(b), friction < 0 ? 0 : friction);
}

extern "C" void Phys_SetTag(physworld_t *w, physbody_t b, int tag)
{
	Bodies(w).SetUserData(BodyID(b), (uint64)(uint32)tag);
}

extern "C" int Phys_TakeContacts(physworld_t *w, physcontact_t *out, int max)
{
	if (!w)
		return 0;
	int n = (int)w->contacts.size() < max ? (int)w->contacts.size() : max;
	for (int i = 0; i < n; i++)
		out[i] = w->contacts[(size_t)i];
	w->contacts.clear();
	return n;
}

// ---------------------------------------------------------------- pushes

extern "C" void Phys_SetLayer(physworld_t *w, physbody_t b, physlayer_t layer)
{
	Bodies(w).SetObjectLayer(BodyID(b), layer == PHYS_LAYER_RAGDOLL ? Layers::RAGDOLL :
		layer == PHYS_LAYER_PLAYER ? Layers::PLAYER : Layers::MOVING);
}

extern "C" int Phys_Explode(physworld_t *w, const float pos[3], float radius, float speed, float refmass)
{
	if (!w || radius <= 0 || speed <= 0)
		return 0;
	RVec3 centre = ToJolt(pos);
	float r = radius * METRES;
	BodyIDVector ids;
	w->system.GetBodies(ids);
	const NarrowPhaseQuery &query = w->system.GetNarrowPhaseQuery();
	SpecifiedBroadPhaseLayerFilter still_bp(BroadPhaseLayer(0));
	SpecifiedObjectLayerFilter still(Layers::STILL);
	BodyInterface &bi = Bodies(w);
	int pushed = 0;
	for (BodyID id : ids) {
		if (bi.GetMotionType(id) != EMotionType::Dynamic)
			continue;
		RVec3 com = bi.GetCenterOfMassPosition(id);
		Vec3 d = Vec3(com - centre);
		float dist = d.Length();
		if (dist >= r)
			continue;
		// the map in between shelters it
		if (dist > 0.05f) {
			RRayCast ray(centre, d);
			RayCastResult hit;
			if (query.CastRay(ray, hit, still_bp, still) && hit.mFraction < 0.97f)
				continue;
		}
		float mass;
		{
			BodyLockRead lock(w->system.GetBodyLockInterface(), id);
			if (!lock.Succeeded())
				continue;
			float inv = lock.GetBody().GetMotionProperties()->GetInverseMass();
			if (inv <= 0)
				continue;
			mass = 1.0f / inv;
		}
		// a body right at the middle goes up
		Vec3 dir = dist > 0.05f ? d / dist : Vec3(0, 0, 1);
		float dv = speed * METRES * (1.0f - dist / r);
		if (mass > refmass && refmass > 0)
			dv *= refmass / mass;
		bi.AddImpulse(id, dir * (dv * mass));
		pushed++;
	}
	return pushed;
}

extern "C" physbody_t Phys_CastRay(physworld_t *w, const float from[3], const float to[3], float hitpos[3], int *tag)
{
	if (!w)
		return PHYS_NOBODY;
	RVec3 a = ToJolt(from), b = ToJolt(to);
	RRayCast ray(a, Vec3(b - a));
	RayCastResult hit;
	SpecifiedBroadPhaseLayerFilter moving_bp(BroadPhaseLayer(1));
	// not the players' own bodies: a shot starts inside the shooter's
	class NotPlayers final : public ObjectLayerFilter {
	public:
		bool ShouldCollide(ObjectLayer layer) const override { return layer != Layers::PLAYER; }
	} not_players;
	if (!w->system.GetNarrowPhaseQuery().CastRay(ray, hit, moving_bp, not_players))
		return PHYS_NOBODY;
	if (hitpos)
		FromJolt(ray.GetPointOnRay(hit.mFraction), hitpos);
	if (tag)
		*tag = (int)(uint32)Bodies(w).GetUserData(hit.mBodyID);
	return hit.mBodyID.GetIndexAndSequenceNumber();
}
