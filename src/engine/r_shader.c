// newen - GLSL programs (modern renderer), read from base/shaders through the filesystem
#include "gl_local.h"

static GLuint CompileStage(GLenum type, const char *path)
{
	size_t len;
	char *src = FS_LoadFile(path, &len);
	if (!src) {
		Con_Warnf("Shader %s not found\n", path);
		return 0;
	}
	GLuint sh = qglCreateShader(type);
	const GLchar *text = src;
	GLint size = (GLint)len;
	qglShaderSource(sh, 1, &text, &size);
	qglCompileShader(sh);
	Mem_Free(src);
	GLint ok = 0;
	qglGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096] = "";
		qglGetShaderInfoLog(sh, sizeof(log), NULL, log);
		Con_Warnf("Shader %s didn't compile:\n%s\n", path, log);
		qglDeleteShader(sh);
		return 0;
	}
	return sh;
}

GLuint R_LoadProgram(const char *name)
{
	char path[MAX_OSPATH];
	snprintf(path, sizeof(path), "shaders/%s.vert", name);
	GLuint vs = CompileStage(GL_VERTEX_SHADER, path);
	if (!vs)
		return 0;
	snprintf(path, sizeof(path), "shaders/%s.frag", name);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, path);
	if (!fs) {
		qglDeleteShader(vs);
		return 0;
	}
	GLuint prog = qglCreateProgram();
	qglAttachShader(prog, vs);
	qglAttachShader(prog, fs);
	qglLinkProgram(prog);
	// attached shaders are only flagged here; they go when the program does
	qglDeleteShader(vs);
	qglDeleteShader(fs);
	GLint ok = 0;
	qglGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096] = "";
		qglGetProgramInfoLog(prog, sizeof(log), NULL, log);
		Con_Warnf("Shader %s didn't link:\n%s\n", name, log);
		qglDeleteProgram(prog);
		return 0;
	}
	return prog;
}

void R_FreeProgram(GLuint *prog)
{
	if (*prog)
		qglDeleteProgram(*prog);
	*prog = 0;
}
