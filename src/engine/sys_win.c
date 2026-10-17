// newen - Windows platform layer and entry point
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdlib.h>
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

// ---------------------------------------------------------------- crash reports

// A crash or abort writes the call stack to the console log before the process ends: function
// names from newen.pdb, module+offset for the game's DLLs (which have no symbols).
static void LogStack(const DWORD64 *frames, int n)
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(proc, NULL, TRUE);
	for (int i = 0; i < n; i++) {
		char modname[MAX_PATH] = "?";
		HMODULE mod = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)(uintptr_t)frames[i], &mod))
			GetModuleFileNameA(mod, modname, sizeof(modname));
		const char *base = strrchr(modname, '\\');
		base = base ? base + 1 : modname;
		char buf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
		memset(buf, 0, sizeof(buf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line = { sizeof(line) };
		DWORD ldisp = 0;
		if (SymFromAddr(proc, frames[i], &disp, sym) && SymGetLineFromAddr64(proc, frames[i], &ldisp, &line))
			Con_Errorf("  %s!%s (%s:%lu)\n", base, sym->Name, line.FileName, line.LineNumber);
		else if (SymFromAddr(proc, frames[i], &disp, sym))
			Con_Errorf("  %s!%s+0x%llx\n", base, sym->Name, disp);
		else
			Con_Errorf("  %s+0x%x\n", base, mod ? (unsigned)(frames[i] - (DWORD64)(uintptr_t)mod) : 0);
	}
}

// the stack where an exception happened (also for crashes caught inside the client DLL)
void Sys_LogException(void *exception_pointers)
{
	EXCEPTION_POINTERS *ep = exception_pointers;
	DWORD64 frames[48];
	int n = 0;
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 sf;
	memset(&sf, 0, sizeof(sf));
	sf.AddrPC.Offset = ctx.Eip;
	sf.AddrFrame.Offset = ctx.Ebp;
	sf.AddrStack.Offset = ctx.Esp;
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
	HANDLE proc = GetCurrentProcess();
	SymInitialize(proc, NULL, TRUE);
	while (n < 48 && StackWalk64(IMAGE_FILE_MACHINE_I386, proc, GetCurrentThread(), &sf, &ctx, NULL,
		SymFunctionTableAccess64, SymGetModuleBase64, NULL) && sf.AddrPC.Offset)
		frames[n++] = sf.AddrPC.Offset;
	LogStack(frames, n);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
	Con_Errorf("CRASH: exception 0x%08lx\n", ep->ExceptionRecord->ExceptionCode);
	Sys_LogException(ep);
	Con_Shutdown();
	return EXCEPTION_CONTINUE_SEARCH;
}

static void LogHere(const char *what)
{
	void *raw[48];
	DWORD64 frames[48];
	int n = CaptureStackBackTrace(1, 48, raw, NULL);
	for (int i = 0; i < n; i++)
		frames[i] = (DWORD64)(uintptr_t)raw[i];
	Con_Errorf("CRASH: %s (the C runtime's message, if any, is in userdata/stderr.log)\n", what);
	LogStack(frames, n);
	Con_Shutdown();
}

static void AbortHandler(int sig) { LogHere("abort()"); }
static void PurecallHandler(void) { LogHere("pure virtual function call"); abort(); }

int main(int argc, char **argv)
{
	SetUnhandledExceptionFilter(CrashHandler);
	signal(SIGABRT, AbortHandler);
	_set_purecall_handler(PurecallHandler);

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

	// the C runtime's own messages (a failed assert in a game DLL) go to a file instead of nowhere
	Sys_MakeDirs(va("%s/userdata/", exe_dir));
	freopen(va("%s/userdata/stderr.log", exe_dir), "w", stderr);
	_set_error_mode(_OUT_TO_STDERR);

	Host_Init(argc, argv);
	for (;;)
		Host_Frame();
}
