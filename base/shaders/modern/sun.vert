#version 120
// newen modern renderer: sun shadows of moving things, multiplied over a lightmapped surface (r_shadow.c's sun maps)

uniform mat3 u_rot;      // the drawn model's directions to the world's
uniform vec3 u_origin;   // and where its origin is

varying vec2 v_st;
varying vec3 v_world;
varying vec3 v_normal;

void main()
{
	gl_Position = ftransform();   // the same depth as the pass underneath
	v_st = gl_MultiTexCoord0.xy;
	v_world = u_rot * gl_Vertex.xyz + u_origin;
	v_normal = u_rot * gl_Normal;
}
