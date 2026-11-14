// newen - physics (Jolt) behind a plain-C interface
//
// The only C++ in newen is phys_jolt.cpp; everything else uses this header. Units are the game's:
// positions in inches (Half-Life units), angles in degrees as Half-Life's entities have them (pitch, yaw,
// roll; positive pitch looks down, as AngleVectors), masses in kilograms. Inside, Jolt works in metres.
// Shared with the Half-Life 2 map plan (docs/SOURCE_PLAN.md, section 5), so it stays general.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct physworld_s physworld_t;
typedef unsigned int physbody_t;           // Jolt's body ID (index and sequence number)
#define PHYS_NOBODY 0xffffffffu

typedef enum {
	PHYS_STATIC,      // never moves (the map)
	PHYS_KINEMATIC,   // moved by the engine (doors, the player), pushes dynamic bodies
	PHYS_DYNAMIC      // moved by the simulation
} physmotion_t;

int          Phys_Init(void);                 // 0 if Jolt couldn't start; calling again does nothing
void         Phys_Shutdown(void);
const char  *Phys_Version(void);              // "Jolt 5.6.0 (SSE2, single precision)"

physworld_t *Phys_CreateWorld(int maxbodies, float gravity);   // gravity in inches/s^2, downwards
void         Phys_DestroyWorld(physworld_t *w);
void         Phys_SetGravity(physworld_t *w, float gravity);
void         Phys_Step(physworld_t *w, float dt);   // in substeps of at most 1/120 s
void         Phys_Optimize(physworld_t *w);         // after adding many static bodies at once
int          Phys_BodyCount(physworld_t *w);

physbody_t   Phys_AddBox(physworld_t *w, const float halfsize[3], const float origin[3], const float angles[3],
	physmotion_t motion, float mass);
// triangles (9 floats each, front faces counter-clockwise) in the body's own space; static or kinematic only
physbody_t   Phys_AddMesh(physworld_t *w, const float *tris, int numtris, const float origin[3], const float angles[3],
	physmotion_t motion);
// convex pieces in the body's own space: counts[i] points (xyz) for piece i, one after another
physbody_t   Phys_AddConvexSet(physworld_t *w, const float *points, const int *counts, int numpieces,
	const float origin[3], const float angles[3], physmotion_t motion, float mass);
// a kinematic body reaches this pose at the end of the next dt seconds, carrying what it touches
void         Phys_MoveKinematic(physworld_t *w, physbody_t b, const float origin[3], const float angles[3], float dt);
void         Phys_RemoveBody(physworld_t *w, physbody_t b);
void         Phys_GetPose(physworld_t *w, physbody_t b, float origin[3], float angles[3]);
void         Phys_SetPose(physworld_t *w, physbody_t b, const float origin[3], const float angles[3]);
void         Phys_GetVelocity(physworld_t *w, physbody_t b, float velocity[3]);
void         Phys_SetVelocity(physworld_t *w, physbody_t b, const float velocity[3]);
void         Phys_AddImpulse(physworld_t *w, physbody_t b, const float impulse[3]);   // kg * inches/s
int          Phys_IsAwake(physworld_t *w, physbody_t b);

// ---- jointed things (ragdolls). Poses here are 3x4 matrices (rotation, then the position in column 3),
// as the model code has its bones.
physbody_t   Phys_AddConvexSetAt(physworld_t *w, const float *points, const int *counts, int numpieces,
	const float pose[3][4], physmotion_t motion, float mass, float inertiascale);
void         Phys_GetMatrix(physworld_t *w, physbody_t b, float pose[3][4]);
// how fast the body's own origin moves (inches/s) and the body turns (degrees/s about the world's axes)
void         Phys_SetMotion(physworld_t *w, physbody_t b, const float velocity[3], const float spin[3]);
void         Phys_SetDamping(physworld_t *w, physbody_t b, float linear, float angular);   // lost per second
void         Phys_Sleep(physworld_t *w, physbody_t b);

// A group's bodies only collide with each other where a pair is switched on; with everything else they
// collide as usual. 0 if there's no room.
typedef int physgroup_t;
physgroup_t  Phys_NewGroup(physworld_t *w, int numparts);
void         Phys_GroupCollide(physworld_t *w, physgroup_t g, int part1, int part2);
void         Phys_SetGroup(physworld_t *w, physbody_t b, physgroup_t g, int part);
void         Phys_FreeGroup(physworld_t *w, physgroup_t g);   // after its bodies are removed

// The child hangs on the parent at a point and may turn about the joint's three axes between mindeg and
// maxdeg (equal = no turning about that one). frame1 is the joint's place and axes in the parent body's
// space, frame2 in the child's; the angles are zero when the two line up. Axis 0 is the twist.
// friction: torque in newton metres that resists turning. 0 if it can't be made.
typedef int physjoint_t;
physjoint_t  Phys_AddJoint(physworld_t *w, physbody_t parent, physbody_t child, const float frame1[3][4],
	const float frame2[3][4], const float mindeg[3], const float maxdeg[3], float friction);
void         Phys_RemoveJoint(physworld_t *w, physjoint_t j);

// ---- more for props
void         Phys_GetBounds(physworld_t *w, physbody_t b, float mins[3], float maxs[3]);   // its box in the world
void         Phys_GetMassCenter(physworld_t *w, physbody_t b, float pos[3]);
void         Phys_AddImpulseAt(physworld_t *w, physbody_t b, const float impulse[3], const float pos[3]);   // spins it too
void         Phys_GetSpin(physworld_t *w, physbody_t b, float spin[3]);   // degrees/s about the world's axes
void         Phys_Wake(physworld_t *w, physbody_t b);
void         Phys_EnableMotion(physworld_t *w, physbody_t b, int on);     // off: it stays where it is, solid
float        Phys_GetMass(physworld_t *w, physbody_t b);
void         Phys_SetMass(physworld_t *w, physbody_t b, float mass);
void         Phys_SetFriction(physworld_t *w, physbody_t b, float friction);
// A body with a tag reports what it hits: each new touch during Phys_Step is kept until taken.
// speed: how fast the two closed on each other (inches/s); normal points from the first body to the second.
typedef struct {
	int   tag1, tag2;           // 0 = a body without one (the map)
	float pos[3], normal[3], speed;
} physcontact_t;
void         Phys_SetTag(physworld_t *w, physbody_t b, int tag);
int          Phys_TakeContacts(physworld_t *w, physcontact_t *out, int max);

// ---- pushes
// What a body collides with: ragdoll parts meet everything; a player's body meets only ragdoll parts
// (Half-Life's own collision already keeps the player out of the map and the props).
typedef enum { PHYS_LAYER_NORMAL, PHYS_LAYER_RAGDOLL, PHYS_LAYER_PLAYER } physlayer_t;
void         Phys_SetLayer(physworld_t *w, physbody_t b, physlayer_t layer);
// An explosion: every moving body within radius that the map doesn't hide from pos is thrown away from
// it, hardest at the middle. speed: how fast a light body at the middle leaves (inches/s); bodies heavier
// than refmass (kg) leave slower in proportion. Returns how many were pushed.
int          Phys_Explode(physworld_t *w, const float pos[3], float radius, float speed, float refmass);
// The first moving body a line meets, or PHYS_NOBODY: where, and the body's tag
physbody_t   Phys_CastRay(physworld_t *w, const float from[3], const float to[3], float hitpos[3], int *tag);

// phys_world.c: the running map's world; create = make it if the map has none yet. NULL without a map.
physworld_t *Phys_ServerWorld(int create);
// An explosion in the running map (the game's own, or Lua's): pushes props and ragdolls
void         Phys_ServerExplosion(const float pos[3], float radius, float damage);
// What recent explosions add to the speed of something of this mass at point (for a body made just after one)
void         Phys_BlastVelocity(const float point[3], float mass, float addvel[3]);
// A bullet's line from Lua's FireBullets: pushes the ragdoll part it meets
void         Phys_ServerBullet(const float from[3], const float to[3], float damage);

#ifdef __cplusplus
}
#endif
