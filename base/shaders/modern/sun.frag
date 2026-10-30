#version 120
// newen modern renderer: darkens where something moving stands between the surface and the sun but the world
// itself doesn't (the lightmap already has the world's own sun shadows). Drawn with GL_DST_COLOR, GL_ZERO.

uniform sampler2D u_diffuse;     // only its alpha: '{' holes take no shadow
uniform sampler2D u_sunworld;    // the world's depth along the sun
uniform sampler2D u_sunmoving;   // models', doors' and trains' depth along the sun
uniform vec3 u_suneye, u_sundir, u_sunright, u_sunup;
uniform vec4 u_suninfo;          // half the covered square's side, depth range, darkness, 1 / map size

varying vec2 v_st;
varying vec3 v_world;
varying vec3 v_normal;

// 1 if the moving things hide the sun at uv and the world doesn't
float Moved(vec2 uv, float ref)
{
	float range = u_suninfo.y;
	float w = texture2D(u_sunworld, uv).r * range;
	float m = texture2D(u_sunmoving, uv).r * range;
	return (m < ref ? 1.0 : 0.0) * (w < ref ? 0.0 : 1.0);
}

// the four pixels round uv, blended by where uv falls between them
float Pcf(vec2 uv, float ts, float ref)
{
	vec2 p = uv / ts - 0.5;
	vec2 w = fract(p);
	vec2 base = (floor(p) + 0.5) * ts;
	float a = Moved(base, ref), b = Moved(base + vec2(ts, 0.0), ref);
	float c = Moved(base + vec2(0.0, ts), ref), e = Moved(base + vec2(ts), ref);
	return mix(mix(a, b, w.x), mix(c, e, w.x), w.y);
}

void main()
{
	if (texture2D(u_diffuse, v_st).a <= 0.25)
		discard;
	vec3 n = normalize(v_normal);
	// facing away from the sun it's already in the map's shade
	float facing = clamp(dot(n, -u_sundir) * 4.0, 0.0, 1.0);
	if (facing <= 0.0)
		discard;
	float ts = u_suninfo.w;
	float pixel = u_suninfo.x * 2.0 * ts;   // one shadow map pixel in world units
	// a little off the surface towards the sun, so a face doesn't shadow itself between pixels
	vec3 p = v_world + n * pixel * 1.5 - u_sundir * pixel;
	vec3 rel = p - u_suneye;
	vec2 uv = vec2(dot(rel, u_sunright), dot(rel, u_sunup)) / u_suninfo.x * 0.5 + 0.5;
	// fade out towards the edge of the covered square instead of stopping in a line
	vec2 edge = abs(uv * 2.0 - 1.0);
	float fade = clamp((1.0 - max(edge.x, edge.y)) * 10.0, 0.0, 1.0);
	if (fade <= 0.0)
		discard;
	float ref = dot(rel, u_sundir);
	float s = 0.25 * (Pcf(uv + vec2(-0.75, -0.75) * ts, ts, ref) + Pcf(uv + vec2(0.75, -0.75) * ts, ts, ref) +
	                  Pcf(uv + vec2(-0.75, 0.75) * ts, ts, ref) + Pcf(uv + vec2(0.75, 0.75) * ts, ts, ref));
	float k = 1.0 - u_suninfo.z * s * facing * fade;
	gl_FragColor = vec4(k, k, k, 1.0);
}
