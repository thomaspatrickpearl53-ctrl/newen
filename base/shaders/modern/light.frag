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

varying vec2 v_st;
varying vec3 v_tolight;
varying vec3 v_toview;
varying vec3 v_normal;
varying vec4 v_tangent;

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

	gl_FragColor = vec4((albedo.rgb * ndl + s * spec) * u_lightcolor * atten, 1.0);
}
