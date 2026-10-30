#version 120
// newen modern renderer: one dynamic light, diffuse plus Blinn-Phong specular, added (GL_ONE, GL_ONE).
// Point lights fall off like classic dynamic lights: (radius - distance) on the lightmap's 0..255 scale.
// Spotlights fade from full brightness at the lamp to nothing at their range, through their pattern.

uniform sampler2D u_diffuse;
uniform sampler2D u_normalmap;   // tangent space, +Y up the image; flat (0.5, 0.5, 1) when there's none
uniform sampler2D u_specmap;     // strength; black when there's none
uniform vec3  u_lightcolor;
uniform float u_radius;
uniform float u_gloss;
uniform float u_specscale;
uniform float u_blend;           // 1 = drawn over something with its alpha (decals): no cut, alpha kept

// shadow map (r_shadow.c): six 90-degree depth views of the light in a 3x2 atlas, in this order
uniform float u_shadow;          // 1 = this light has the shadow map
uniform sampler2D u_shadowmap;
uniform vec4  u_shadowinfo;      // near, far, bias (units), 1 / view size
uniform float u_shadowsoft;      // 1 = r_shadow_soft's wider filter
uniform mat3  u_rot;             // the drawn model's directions to the world's

// spotlights (the flashlight): a beam along u_spotdir; a point d from the light is at
// (d.right, d.up) / (d.dir * tan(half angle)) across it, -1..1, where the pattern and the shadow map are read
uniform float u_spot;            // 1 = this light is a spotlight
uniform vec3  u_spotdir, u_spotright, u_spotup;   // world axes
uniform vec4  u_spotinfo;        // tan(half angle), 1 / shadow map size
uniform sampler2D u_cookie;      // the beam's pattern (black outside the circle)

varying vec2 v_st;
varying vec3 v_tolight;
varying vec3 v_toview;
varying vec3 v_normal;
varying vec4 v_tangent;

// 1 if a point ref units along the view is in front of what the shadow map saw at uv
float Lit(vec2 uv, float ref)
{
	float n = u_shadowinfo.x, fa = u_shadowinfo.y;
	float z = texture2D(u_shadowmap, uv).r * 2.0 - 1.0;
	float dist = 2.0 * fa * n / (fa + n - z * (fa - n));   // back to distance along the view
	return ref <= dist ? 1.0 : 0.0;
}

// the four pixels round uv, each compared and then blended by where uv falls between them, so the edge is
// smooth instead of stepping a whole pixel at a time
float Pcf(vec2 uv, vec2 ts, float ref)
{
	vec2 p = uv / ts - 0.5;
	vec2 w = fract(p);
	vec2 base = (floor(p) + 0.5) * ts;
	float a = Lit(base, ref), b = Lit(base + vec2(ts.x, 0.0), ref);
	float c = Lit(base + vec2(0.0, ts.y), ref), e = Lit(base + ts, ref);
	return mix(mix(a, b, w.x), mix(c, e, w.x), w.y);
}

// d: from the light to the point, world axes. Returns how lit the point is (0..1).
float Shadow(vec3 d)
{
	vec3 a = abs(d);
	vec3 r, u;
	vec2 tile;
	float m;
	if (a.x >= a.y && a.x >= a.z) {
		m = a.x;
		if (d.x > 0.0) { r = vec3(0.0, -1.0, 0.0); u = vec3(0.0, 0.0, 1.0); tile = vec2(0.0, 0.0); }
		else           { r = vec3(0.0, 1.0, 0.0);  u = vec3(0.0, 0.0, 1.0); tile = vec2(1.0, 0.0); }
	} else if (a.y >= a.z) {
		m = a.y;
		if (d.y > 0.0) { r = vec3(1.0, 0.0, 0.0);  u = vec3(0.0, 0.0, 1.0); tile = vec2(2.0, 0.0); }
		else           { r = vec3(-1.0, 0.0, 0.0); u = vec3(0.0, 0.0, 1.0); tile = vec2(0.0, 1.0); }
	} else {
		m = a.z;
		if (d.z > 0.0) { r = vec3(1.0, 0.0, 0.0);  u = vec3(0.0, -1.0, 0.0); tile = vec2(1.0, 1.0); }
		else           { r = vec3(1.0, 0.0, 0.0);  u = vec3(0.0, 1.0, 0.0);  tile = vec2(2.0, 1.0); }
	}
	float texel = u_shadowinfo.w;   // one shadow map pixel, as a fraction of a view
	// keep every sample inside this view, so nothing reads the neighbouring tile
	vec2 f = clamp(vec2(dot(d, r), dot(d, u)) / m * 0.5 + 0.5, vec2(2.5 * texel), vec2(1.0 - 2.5 * texel));
	vec2 uv = (tile + f) / vec2(3.0, 2.0);
	vec2 ts = vec2(texel) / vec2(3.0, 2.0);   // one pixel in the atlas
	float ref = m - u_shadowinfo.z;
	if (u_shadowsoft < 0.5)
		return Pcf(uv, ts, ref);
	// four filtered lookups a pixel apart: a soft edge about three pixels wide
	return 0.25 * (Pcf(uv + vec2(-0.75, -0.75) * ts, ts, ref) + Pcf(uv + vec2(0.75, -0.75) * ts, ts, ref) +
	               Pcf(uv + vec2(-0.75, 0.75) * ts, ts, ref) + Pcf(uv + vec2(0.75, 0.75) * ts, ts, ref));
}

// A spotlight's single view: like Shadow, but one perspective view the size of the whole texture
float SpotShadow(vec3 d)
{
	float z = dot(d, u_spotdir);
	float ts = u_spotinfo.y;
	vec2 uv = vec2(dot(d, u_spotright), dot(d, u_spotup)) / (z * u_spotinfo.x) * 0.5 + 0.5;
	uv = clamp(uv, vec2(2.5 * ts), vec2(1.0 - 2.5 * ts));
	float ref = z - u_shadowinfo.z;
	vec2 t2 = vec2(ts);
	if (u_shadowsoft < 0.5)
		return Pcf(uv, t2, ref);
	return 0.25 * (Pcf(uv + vec2(-0.75, -0.75) * t2, t2, ref) + Pcf(uv + vec2(0.75, -0.75) * t2, t2, ref) +
	               Pcf(uv + vec2(-0.75, 0.75) * t2, t2, ref) + Pcf(uv + vec2(0.75, 0.75) * t2, t2, ref));
}

void main()
{
	vec4 albedo = texture2D(u_diffuse, v_st);
	if (u_blend < 0.5 && albedo.a <= 0.25)
		discard;   // the same cut as the '{' textures' alpha test
	float dist = length(v_tolight);
	float atten;
	vec3 cookie = vec3(1.0);
	if (u_spot > 0.5) {
		// outside the beam: nothing. Inside: the pattern, and a smooth fade to the end of the range
		vec3 dw = u_rot * -v_tolight;
		float z = dot(dw, u_spotdir);
		if (z <= 0.0)
			discard;
		vec2 p = vec2(dot(dw, u_spotright), dot(dw, u_spotup)) / (z * u_spotinfo.x);
		if (abs(p.x) >= 1.0 || abs(p.y) >= 1.0)
			discard;
		cookie = texture2D(u_cookie, p * 0.5 + 0.5).rgb;
		float k = 1.0 - dist / u_radius;
		if (k <= 0.0)
			discard;
		atten = k * sqrt(k);
	} else {
		atten = (u_radius - dist) / 255.0;
		if (atten <= 0.0)
			discard;
	}

	vec3 n = normalize(v_normal);
	vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
	vec3 b = cross(n, t) * v_tangent.w;
	vec3 tn = texture2D(u_normalmap, v_st).xyz * 2.0 - 1.0;
	vec3 N = normalize(t * tn.x + b * tn.y + n * max(tn.z, 0.0));

	vec3 L = v_tolight / dist;
	// a bump can lean towards a light behind the face; the face itself must still face it
	float facing = clamp(dot(n, L) * 8.0, 0.0, 1.0);
	float ndl = max(dot(N, L), 0.0) * facing;

	vec3 V = normalize(v_toview);
	vec3 H = normalize(L + V);
	float spec = pow(max(dot(N, H), 0.0), u_gloss) * ndl * u_specscale;
	vec3 s = texture2D(u_specmap, v_st).rgb;

	float lit = 1.0;
	if (u_shadow > 0.5 && ndl > 0.0) {
		// look the shadow up a little off the surface, more where the light grazes it, so a face doesn't
		// shadow itself between shadow map pixels ("acne"); one pixel is 2 * distance / size wide there
		float pixel = 2.0 * dist * (u_spot > 0.5 ? u_spotinfo.x * u_spotinfo.y : u_shadowinfo.w);
		vec3 offset = n * pixel * (1.0 + 2.0 * (1.0 - max(dot(n, L), 0.0)));
		vec3 d = u_rot * (offset - v_tolight);
		lit = u_spot > 0.5 ? SpotShadow(d) : Shadow(d);
	}

	gl_FragColor = vec4((albedo.rgb * ndl + s * spec) * u_lightcolor * cookie * atten * lit, u_blend > 0.5 ? albedo.a : 1.0);
}
