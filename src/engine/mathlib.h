// newen - small vector math (Quake conventions: x forward, y left, z up; angles pitch yaw roll in degrees)
#pragma once
#include <math.h>

typedef float vec3_t[3];

#define M_PI_F 3.14159265358979323846f
#define DEG2RAD(a) ((a) * (M_PI_F / 180.0f))

#define DotProduct(a, b)      ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
#define VectorCopy(a, b)      ((b)[0] = (a)[0], (b)[1] = (a)[1], (b)[2] = (a)[2])
#define VectorSet(v, x, y, z) ((v)[0] = (x), (v)[1] = (y), (v)[2] = (z))
#define VectorClear(v)        ((v)[0] = (v)[1] = (v)[2] = 0)
#define VectorAdd(a, b, c)    ((c)[0] = (a)[0] + (b)[0], (c)[1] = (a)[1] + (b)[1], (c)[2] = (a)[2] + (b)[2])
#define VectorSubtract(a, b, c) ((c)[0] = (a)[0] - (b)[0], (c)[1] = (a)[1] - (b)[1], (c)[2] = (a)[2] - (b)[2])
#define VectorScale(a, s, c)  ((c)[0] = (a)[0] * (s), (c)[1] = (a)[1] * (s), (c)[2] = (a)[2] * (s))
#define VectorMA(a, s, b, c)  ((c)[0] = (a)[0] + (s) * (b)[0], (c)[1] = (a)[1] + (s) * (b)[1], (c)[2] = (a)[2] + (s) * (b)[2])

static inline float VectorLength(const vec3_t v)
{
	return sqrtf(DotProduct(v, v));
}

static inline float VectorNormalize(vec3_t v)
{
	float len = VectorLength(v);
	if (len > 0.0f) {
		float inv = 1.0f / len;
		VectorScale(v, inv, v);
	}
	return len;
}

static inline void CrossProduct(const vec3_t a, const vec3_t b, vec3_t c)
{
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
}

static inline void AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	float sy = sinf(DEG2RAD(angles[1])), cy = cosf(DEG2RAD(angles[1]));
	float sp = sinf(DEG2RAD(angles[0])), cp = cosf(DEG2RAD(angles[0]));
	float sr = sinf(DEG2RAD(angles[2])), cr = cosf(DEG2RAD(angles[2]));
	if (forward) {
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}
	if (right) {
		right[0] = -sr * sp * cy + cr * sy;
		right[1] = -sr * sp * sy - cr * cy;
		right[2] = -sr * cp;
	}
	if (up) {
		up[0] = cr * sp * cy + sr * sy;
		up[1] = cr * sp * sy - sr * cy;
		up[2] = cr * cp;
	}
}
