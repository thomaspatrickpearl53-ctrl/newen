#version 120
// newen modern renderer: one dynamic light added over a lightmapped surface.
// Everything is in the drawn model's own space (the world's, or a brush entity's), so no matrices are needed.

uniform vec3 u_lightorg;
uniform vec3 u_vieworg;

varying vec2 v_st;
varying vec3 v_tolight;
varying vec3 v_toview;
varying vec3 v_normal;
varying vec4 v_tangent;

void main()
{
	gl_Position = ftransform();   // the same depth as the fixed-function pass underneath
	v_st = gl_MultiTexCoord0.xy;
	v_tolight = u_lightorg - gl_Vertex.xyz;
	v_toview = u_vieworg - gl_Vertex.xyz;
	v_normal = gl_Normal;
	v_tangent = gl_MultiTexCoord2;
}
