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
void         Phys_Step(physworld_t *w, float dt);   // fixed 1/120 s substeps; leftover time carries over
void         Phys_Optimize(physworld_t *w);         // after adding many static bodies at once
int          Phys_BodyCount(physworld_t *w);

physbody_t   Phys_AddBox(physworld_t *w, const float halfsize[3], const float origin[3], const float angles[3],
	physmotion_t motion, float mass);
void         Phys_RemoveBody(physworld_t *w, physbody_t b);
void         Phys_GetPose(physworld_t *w, physbody_t b, float origin[3], float angles[3]);
void         Phys_SetPose(physworld_t *w, physbody_t b, const float origin[3], const float angles[3]);
void         Phys_GetVelocity(physworld_t *w, physbody_t b, float velocity[3]);
void         Phys_SetVelocity(physworld_t *w, physbody_t b, const float velocity[3]);
void         Phys_AddImpulse(physworld_t *w, physbody_t b, const float impulse[3]);   // kg * inches/s
int          Phys_IsAwake(physworld_t *w, physbody_t b);

#ifdef __cplusplus
}
#endif
