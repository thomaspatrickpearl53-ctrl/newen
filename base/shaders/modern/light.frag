#version 120
// newen modern renderer: one dynamic light, diffuse plus Blinn-Phong specular, added (GL_ONE, GL_ONE).
// The falloff matches classic dynamic lights: (radius - distance) on the lightmap's 0..255 scale.

uniform sampler2D u_diffuse;
uniform sampler2D u_normalmap;   // tangent space, +Y up the image; flat (0.5, 0.5, 1) when there's none
uniform sampler2D u_specmap;     // strength; black when there's none
uniform vec3  u_lightcolor;
uniform float u_radius;
uniform float u_gloss;
uniform float u_specscale;

// shadow map (r_shadow.c): six 90-degree depth views of the light in a 3x2 atlas, in this order
uniform float u_shadow;          // 1 = this light has the shadow map
uniform sampler2D u_shadowmap;
uniform vec4  u_shadowinfo;      // near, far, bias (units), 1 / view size
uniform mat3  u_rot;             // the drawn model's directions to the world's

varying vec2 v_st;
varying vec3 v_tolight;
varying vec3 v_toview;
varying vec3 v_normal;
varying vec4 v_tangent;

// d: from the light to the point, world axes. Returns how lit the point is (0..1), from 4 samples.
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
	vec2 f = clamp(vec2(dot(d, r), dot(d, u)) / m * 0.5 + 0.5, vec2(1.5 * texel), vec2(1.0 - 1.5 * texel));
	vec2 uv = (tile + f) / vec2(3.0, 2.0);
	vec2 ofs = vec2(texel) / vec2(3.0, 2.0) * 0.5;
	float n = u_shadowinfo.x, fa = u_shadowinfo.y;
	float ref = m - u_shadowinfo.z;
	float lit = 0.0;
	for (int i = 0; i < 4; i++) {
		vec2 o = vec2(i == 0 || i == 2 ? -ofs.x : ofs.x, i < 2 ? -ofs.y : ofs.y);
		float z = texture2D(u_shadowmap, uv + o).r * 2.0 - 1.0;
		float dist = 2.0 * fa * n / (fa + n - z * (fa - n));   // back to distance along the view
		lit += ref <= dist ? 1.0 : 0.0;
	}
	return lit * 0.25;
}

void main()
{
	vec4 albedo = texture2D(u_diffuse, v_st);
	if (albedo.a <= 0.25)
		discard;   // the same cut as the '{' textures' alpha test
	float dist = length(v_tolight);
	float atten = (u_radius - dist) / 255.0;
	if (atten <= 0.0)
		discard;

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
		float pixel = 2.0 * dist * u_shadowinfo.w;
		vec3 offset = n * pixel * (1.0 + 2.0 * (1.0 - max(dot(n, L), 0.0)));
		lit = Shadow(u_rot * (offset - v_tolight));
	}

	gl_FragColor = vec4((albedo.rgb * ndl + s * spec) * u_lightcolor * atten * lit, 1.0);
}
