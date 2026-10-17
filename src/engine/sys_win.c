// newen - Windows platform layer and entry point
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <SDL.h>
#include "common.h"

static double perf_freq;
static LARGE_INTEGER perf_start;
static char exe_dir[MAX_OSPATH];

void Sys_Error(const char *fmt, ...)
{
	static int recursive;
	char msg[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (!recursive) {
		recursive = 1;
		Con_Errorf("FATAL: %s\n", msg);
		Con_Shutdown();  // flush the log
	}
	MessageBoxA(NULL, msg, ENGINE_NAME " error", MB_OK | MB_ICONERROR);
	exit(1);
}

double Sys_FloatTime(void)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - perf_start.QuadPart) / perf_freq;
}

void Sys_Sleep(int ms)
{
	Sleep(ms);
}

const char *Sys_ExeDir(void)
{
	return exe_dir;
}

qboolean Sys_DirExists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

qboolean Sys_FileExists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void Sys_MakeDirs(const char *path)
{
	char tmp[MAX_OSPATH];
	Q_strncpy(tmp, path, sizeof(tmp));
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = 0;
			if (!(p[-1] == ':'))
				CreateDirectoryA(tmp, NULL);
			*p = c;
		}
	}
}

const char *Sys_SteamPath(void)
{
	static char path[MAX_OSPATH];
	HKEY key;
	DWORD size = sizeof(path), type;
	if (path[0])
		return path;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
		return NULL;
	LONG r = RegQueryValueExA(key, "SteamPath", NULL, &type, (BYTE *)path, &size);
	RegCloseKey(key);
	if (r != ERROR_SUCCESS || type != REG_SZ) {
		path[0] = 0;
		return NULL;
	}
	path[sizeof(path) - 1] = 0;
	return path;
}

void Sys_ListDir(const char *dir, sys_listfn_t fn, void *ctx)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(va("%s/*", dir), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

int main(int argc, char **argv)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	perf_freq = (double)f.QuadPart;
	QueryPerformanceCounter(&perf_start);
	timeBeginPeriod(1);

	GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
	for (char *p = exe_dir; *p; p++)
		if (*p == '\\')
			*p = '/';
	char *slash = strrchr(exe_dir, '/');
	if (slash)
		*slash = 0;

	Host_Init(argc, argv);
	for (;;)
		Host_Frame();
}
