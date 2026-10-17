// newen - console variables
// cvar_t keeps GoldSrc's layout; newen's extra data lives in a side table.
#include "common.h"

typedef struct cvarinfo_s {
	cvar_t *var;
	char   *defstring;
	char   *desc;
	qboolean owned;           // allocated by the engine (not a game DLL's static)
	struct cvarinfo_s *hashnext;
} cvarinfo_t;

#define CVAR_HASH 512
static cvarinfo_t *cvar_hash[CVAR_HASH];
static cvar_t *cvar_vars;     // GoldSrc-style linked list, sorted by insertion

typedef struct pending_s {
	char *name, *value;
	struct pending_s *next;
} pending_t;
static pending_t *pending;    // saved values for cvars not created yet

static unsigned HashName(const char *s)
{
	unsigned h = 0;
	for (; *s; s++)
		h = h * 31 + (unsigned)(*s | 0x20);
	return h % CVAR_HASH;
}

static cvarinfo_t *FindInfo(const char *name)
{
	for (cvarinfo_t *ci = cvar_hash[HashName(name)]; ci; ci = ci->hashnext)
		if (!stricmp(ci->var->name, name))
			return ci;
	return NULL;
}

cvar_t *Cvar_Find(const char *name)
{
	cvarinfo_t *ci = FindInfo(name);
	return ci ? ci->var : NULL;
}

cvar_t *Cvar_First(void)
{
	return cvar_vars;
}

const char *Cvar_Description(const cvar_t *var)
{
	cvarinfo_t *ci = FindInfo(var->name);
	return ci && ci->desc ? ci->desc : "";
}

const char *Cvar_Default(const cvar_t *var)
{
	cvarinfo_t *ci = FindInfo(var->name);
	return ci && ci->defstring ? ci->defstring : "";
}

static void SetString(cvar_t *var, const char *value)
{
	cvarinfo_t *ci = FindInfo(var->name);
	if (ci && ci->owned)
		Mem_Free(var->string);
	var->string = Mem_Strdup(value);
	var->value = (float)atof(value);
	if (ci)
		ci->owned = true;   // string is ours from now on
}

cvar_t *Cvar_Get(const char *name, const char *value, int flags, const char *desc)
{
	cvarinfo_t *ci = FindInfo(name);
	if (ci) {
		ci->var->flags |= flags;
		if (desc && !ci->desc)
			ci->desc = Mem_Strdup(desc);
		if (!ci->defstring)
			ci->defstring = Mem_Strdup(value);
		return ci->var;
	}
	if (Cmd_Exists(name)) {
		Con_Warnf("Cvar_Get: \"%s\" is already a command\n", name);
		return NULL;
	}

	cvar_t *var = Mem_Alloc(sizeof(*var));
	var->name = Mem_Strdup(name);
	var->string = Mem_Strdup(value);
	var->value = (float)atof(value);
	var->flags = flags;
	var->next = cvar_vars;
	cvar_vars = var;

	ci = Mem_Alloc(sizeof(*ci));
	ci->var = var;
	ci->defstring = Mem_Strdup(value);
	ci->desc = desc ? Mem_Strdup(desc) : NULL;
	ci->owned = true;
	unsigned h = HashName(name);
	ci->hashnext = cvar_hash[h];
	cvar_hash[h] = ci;

	// a saved value that was read before this cvar existed
	for (pending_t **pp = &pending; *pp; pp = &(*pp)->next) {
		if (!stricmp((*pp)->name, name)) {
			pending_t *p = *pp;
			*pp = p->next;
			Cvar_DirectSet(var, p->value);
			Mem_Free(p->name);
			Mem_Free(p->value);
			Mem_Free(p);
			break;
		}
	}
	return var;
}

void Cvar_SetPending(const char *name, const char *value)
{
	pending_t *p;
	for (p = pending; p; p = p->next)
		if (!stricmp(p->name, name))
			break;
	if (!p) {
		p = Mem_Alloc(sizeof(*p));
		p->name = Mem_Strdup(name);
		p->next = pending;
		pending = p;
	} else {
		Mem_Free(p->value);
	}
	p->value = Mem_Strdup(value);
}

void Cvar_DirectSet(cvar_t *var, const char *value)
{
	if (!var || !value)
		return;
	if (var->flags & FCVAR_PRINTABLEONLY) {
		for (const char *p = value; *p; p++)
			if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
				return;
	}
	if (!strcmp(var->string, value))
		return;
	SetString(var, value);
}

void Cvar_Set(const char *name, const char *value)
{
	cvar_t *var = Cvar_Find(name);
	if (!var) {
		Con_DPrintf("Cvar_Set: no variable \"%s\"\n", name);
		return;
	}
	Cvar_DirectSet(var, value);
}

void Cvar_SetValue(const char *name, float value)
{
	if (value == (float)(int)value)
		Cvar_Set(name, va("%d", (int)value));
	else
		Cvar_Set(name, va("%g", value));
}

float Cvar_VariableValue(const char *name)
{
	cvar_t *var = Cvar_Find(name);
	return var ? var->value : 0.0f;
}

const char *Cvar_VariableString(const char *name)
{
	cvar_t *var = Cvar_Find(name);
	return var ? var->string : "";
}

qboolean Cvar_Command(void)
{
	cvar_t *var = Cvar_Find(Cmd_Argv(0));
	if (!var)
		return false;
	if (Cmd_Argc() == 1) {
		const char *desc = Cvar_Description(var);
		Con_Printf("\"%s\" is \"%s\" (default \"%s\")%s%s\n", var->name, var->string,
			Cvar_Default(var), *desc ? " - " : "", desc);
		return true;
	}
	if (var->flags & FCVAR_READONLY) {
		Con_Warnf("%s is read-only\n", var->name);
		return true;
	}
	Cvar_DirectSet(var, Cmd_Argv(1));
	return true;
}

void Cvar_WriteArchived(FILE *f)
{
	for (cvar_t *var = cvar_vars; var; var = var->next)
		if (var->flags & FCVAR_ARCHIVE)
			fprintf(f, "%s \"%s\"\n", var->name, var->string);
	// keep settings of addons that weren't loaded this time
	for (pending_t *p = pending; p; p = p->next)
		fprintf(f, "%s \"%s\"\n", p->name, p->value);
}

void Cvar_RemoveFlagged(int flag)
{
	cvar_t **prev = &cvar_vars;
	while (*prev) {
		cvar_t *var = *prev;
		if (!(var->flags & flag)) {
			prev = &var->next;
			continue;
		}
		*prev = var->next;
		unsigned h = HashName(var->name);
		for (cvarinfo_t **cp = &cvar_hash[h]; *cp; cp = &(*cp)->hashnext) {
			if ((*cp)->var == var) {
				cvarinfo_t *ci = *cp;
				*cp = ci->hashnext;
				Mem_Free(ci->defstring);
				Mem_Free(ci->desc);
				Mem_Free(ci);
				break;
			}
		}
		Mem_Free(var->name);
		Mem_Free(var->string);
		Mem_Free(var);
	}
}

static int CompareCvars(const void *a, const void *b)
{
	return stricmp((*(cvar_t *const *)a)->name, (*(cvar_t *const *)b)->name);
}

static void Cvarlist_f(void)
{
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL;
	int n = 0;
	for (cvar_t *v = cvar_vars; v; v = v->next)
		n++;
	cvar_t **list = Mem_Alloc(sizeof(*list) * (n + 1));
	n = 0;
	for (cvar_t *v = cvar_vars; v; v = v->next)
		if (!filter || Q_WildcardMatch(filter, v->name) || !strnicmp(v->name, filter, strlen(filter)))
			list[n++] = v;
	qsort(list, n, sizeof(*list), CompareCvars);
	for (int i = 0; i < n; i++) {
		const char *desc = Cvar_Description(list[i]);
		Con_Printf("%-24s \"%s\"%s%s\n", list[i]->name, list[i]->string, *desc ? "  " : "", desc);
	}
	Con_Printf("%d cvars\n", n);
	Mem_Free(list);
}

static void Toggle_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: toggle <cvar> [value1 value2 ...]\n");
		return;
	}
	cvar_t *var = Cvar_Find(Cmd_Argv(1));
	if (!var) {
		Con_Printf("toggle: no cvar \"%s\"\n", Cmd_Argv(1));
		return;
	}
	if (Cmd_Argc() == 2) {
		Cvar_DirectSet(var, var->value ? "0" : "1");
		return;
	}
	// cycle through the given values
	int i;
	for (i = 2; i < Cmd_Argc(); i++)
		if (!stricmp(var->string, Cmd_Argv(i)))
			break;
	i = (i + 1 < Cmd_Argc()) ? i + 1 : 2;
	Cvar_DirectSet(var, Cmd_Argv(i));
}

static void Reset_f(void)
{
	cvar_t *var = Cmd_Argc() > 1 ? Cvar_Find(Cmd_Argv(1)) : NULL;
	if (!var) {
		Con_Printf("usage: reset <cvar>\n");
		return;
	}
	Cvar_DirectSet(var, Cvar_Default(var));
}

void Cvar_Init(void)
{
	Cmd_AddCommand("cvarlist", Cvarlist_f, "lists cvars, optionally filtered: cvarlist r_*");
	Cmd_AddCommand("toggle", Toggle_f, "flips a cvar between 0 and 1, or cycles given values");
	Cmd_AddCommand("reset", Reset_f, "sets a cvar back to its default");
}
