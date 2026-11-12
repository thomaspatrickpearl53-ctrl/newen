// newen - Linux platform layer and entry point (the sys_win.c of Linux)
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <SDL.h>
#include "common.h"

static struct timespec time_start;
static char exe_path[MAX_OSPATH], exe_dir[MAX_OSPATH];

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
	fprintf(stderr, "%s\n", msg);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, ENGINE_NAME " error", msg, NULL);
	exit(1);
}

double Sys_FloatTime(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - time_start.tv_sec) + (now.tv_nsec - time_start.tv_nsec) * 1e-9;
}

void Sys_Sleep(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) && errno == EINTR)
		;
}

const char *Sys_ExeDir(void)
{
	return exe_dir;
}

qboolean Sys_DirExists(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

qboolean Sys_FileExists(const char *path)
{
	struct stat st;
	return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

void Sys_MakeDirs(const char *path)
{
	char tmp[MAX_OSPATH];
	Q_strncpy(tmp, path, sizeof(tmp));
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = 0;
			mkdir(tmp, 0755);
			*p = c;
		}
	}
}

// the usual Steam installs: native, the old ~/.steam link, Flatpak
const char *Sys_SteamPath(void)
{
	static char path[MAX_OSPATH];
	if (path[0])
		return path;
	const char *home = getenv("HOME");
	if (!home)
		return NULL;
	static const char *const tries[] = {
		".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam",
	};
	for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", home, tries[i]);
		if (Sys_DirExists(va("%s/steamapps", path)))
			return path;
	}
	path[0] = 0;
	return NULL;
}

void Sys_ListDir(const char *dir, sys_listfn_t fn, void *ctx)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		qboolean isdir = e->d_type == DT_DIR;
		if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
			isdir = Sys_DirExists(va("%s/%s", dir, e->d_name));
		fn(e->d_name, isdir, ctx);
	}
	closedir(d);
}

// Half-Life's files were made on Windows and their names don't always match the case the game
// asks for. Walks the path after the first `keep` characters, fixing each part's case in place.
qboolean Sys_FixCase(char *path, size_t keep)
{
	char *part = path + keep;
	while (*part == '/')
		part++;
	while (*part) {
		char *end = strchr(part, '/');
		if (end)
			*end = 0;
		char dir[MAX_OSPATH];
		size_t dlen = (size_t)(part - path);
		if (dlen >= sizeof(dir))
			return false;
		memcpy(dir, path, dlen);
		dir[dlen ? dlen - 1 : 0] = 0;   // without the slash
		if (!dlen)
			Q_strncpy(dir, "/", sizeof(dir));
		qboolean found = false;
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != NULL)
				if (!stricmp(e->d_name, part)) {
					memcpy(part, e->d_name, strlen(part));   // same length: only the case differs
					found = true;
					break;
				}
			closedir(d);
		}
		if (end)
			*end = '/';
		if (!found)
			return false;
		if (!end)
			break;
		part = end + 1;
	}
	return Sys_FileExists(path);
}

// ---------------------------------------------------------------- loadable libraries

void *Sys_LoadLibrary(const char *path)
{
	// Local, like a Windows DLL: client.so has empty copies of the monster code for weapon prediction
	// (CBaseMonster::MonsterInitThink and 50 more), and with RTLD_GLOBAL hl.so's calls bind to those,
	// so monsters animate but never think. client.so still gets vgui.so as its own dependency.
	// hl.so uses the C++ runtime (__dynamic_cast) without listing it: Valve's engine links it
	static qboolean cxx_loaded;
	if (!cxx_loaded) {
		cxx_loaded = true;
		if (!dlopen("libstdc++.so.6", RTLD_NOW | RTLD_GLOBAL))
			Con_Warnf("Couldn't load the 32-bit C++ runtime: %s\n", dlerror());
	}
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *Sys_GetProcAddress(void *lib, const char *name)
{
	return lib ? dlsym(lib, name) : NULL;
}

void Sys_FreeLibrary(void *lib)
{
	if (lib)
		dlclose(lib);
}

const char *Sys_LibraryError(void)
{
	const char *e = dlerror();
	return e ? e : "unknown error";
}

// client.so asks for "vgui.so" by bare name, and the loader only finds such names through
// LD_LIBRARY_PATH: vgui.so has no SONAME that would match the copy the engine loads itself. The
// variable is read once at process start, so the engine starts itself again with the folder added.
// Runs before the window opens; Steam's hl.sh does the same for its engine.
void Sys_AddLibraryPath(const char *dir)
{
	const char *cur = getenv("LD_LIBRARY_PATH");
	size_t dlen = strlen(dir);
	for (const char *p = cur; p && *p; ) {
		const char *end = strchr(p, ':');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == dlen && !strncmp(p, dir, n))
			return;
		p = end ? end + 1 : p + n;
	}
	if (getenv("NEWEN_RESTARTED") || strchr(dir, ':')) {
		Con_Warnf("Couldn't add %s to LD_LIBRARY_PATH; client.so may not find vgui.so\n", dir);
		return;
	}
	// appended: the system's own libraries still come first
	char value[16384];
	if (cur && *cur)
		snprintf(value, sizeof(value), "%s:%s", cur, dir);
	else
		Q_strncpy(value, dir, sizeof(value));
	setenv("LD_LIBRARY_PATH", value, 1);
	setenv("NEWEN_RESTARTED", "1", 1);
	fflush(NULL);
	execv(exe_path, com_argv);   // not /proc/self/exe: the process would be named "exe"
	Con_Warnf("Couldn't restart with LD_LIBRARY_PATH (%s)\n", strerror(errno));
}

// the exported (mangled) name at an address; lib only says which library (dladdr finds it)
const char *Sys_ExportName(void *lib, void *addr)
{
	Dl_info di;
	if (!lib || !dladdr(addr, &di) || !di.dli_sname || di.dli_saddr != addr)
		return NULL;
	return di.dli_sname;
}

// ---------------------------------------------------------------- crash reports

sigjmp_buf *sys_crash_guard;
char sys_crash_where[256];

// "hl.so+0x1234": the module an address is in and the offset, or the engine function's name
static void DescribeAddress(void *addr, char *out, size_t size)
{
	Dl_info di;
	if (!dladdr(addr, &di) || !di.dli_fname) {
		snprintf(out, size, "%p", addr);
		return;
	}
	const char *base = strrchr(di.dli_fname, '/');
	base = base ? base + 1 : di.dli_fname;
	if (di.dli_sname)
		snprintf(out, size, "%s!%s+0x%x", base, di.dli_sname, (unsigned)((char *)addr - (char *)di.dli_saddr));
	else
		snprintf(out, size, "%s+0x%x", base, (unsigned)((char *)addr - (char *)di.dli_fbase));
}

static void LogStack(int skip)
{
	void *frames[48];
	int n = backtrace(frames, 48);
	for (int i = skip; i < n; i++) {
		char where[256];
		// return addresses point after the call
		DescribeAddress((char *)frames[i] - (i > skip ? 1 : 0), where, sizeof(where));
		Con_Errorf("  %s\n", where);
	}
}

// the current call stack into the log (Windows passes the exception's context; here it's the handler's stack)
void Sys_LogException(void *unused)
{
	LogStack(1);
}

static void CrashHandler(int sig, siginfo_t *si, void *uc)
{
	void *pc = (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_EIP];
	if (sys_crash_guard && sig != SIGABRT) {
		// inside a guarded call: report and jump back out instead of ending the process
		sigjmp_buf *guard = sys_crash_guard;
		sys_crash_guard = NULL;
		char where[200];
		DescribeAddress(pc, where, sizeof(where));
		snprintf(sys_crash_where, sizeof(sys_crash_where), "%s (reading %p)", where, si->si_addr);
		Con_Errorf("Crash inside the client (%s), call stack:\n", strsignal(sig));
		LogStack(1);
		siglongjmp(*guard, 1);
	}
	char where[200];
	DescribeAddress(pc, where, sizeof(where));
	Con_Errorf("CRASH: %s at %s (address %p)\n", strsignal(sig), where, si->si_addr);
	LogStack(1);
	Con_Shutdown();
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv)
{
	// its own stack, so a stack overflow can still be reported
	static char altstack[65536];
	stack_t ss = { altstack, 0, sizeof(altstack) };
	sigaltstack(&ss, NULL);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = CrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);

	clock_gettime(CLOCK_MONOTONIC, &time_start);

	ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
	if (n <= 0)
		Q_strncpy(exe_path, argv[0], sizeof(exe_path));
	else
		exe_path[n] = 0;
	Q_strncpy(exe_dir, exe_path, sizeof(exe_dir));
	char *slash = strrchr(exe_dir, '/');
	if (slash)
		*slash = 0;

	Host_Init(argc, argv);
	for (;;)
		Host_Frame();
}
