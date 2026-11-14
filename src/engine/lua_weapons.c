// newen - Lua weapons: SWEP tables (weapons.Register, lua/weapons/*.lua)
//
// Half-Life splits a weapon between the game DLL (ammo, firing, which one is in hand) and the client
// DLL (the weapon menu, icons, ammo counters). They talk through messages (WeaponList describes a weapon
// type, CurWeapon gives a clip, AmmoX an ammo count) and through the player's weapon bits. Neither DLL
// can be changed, but every message and the weapon bits pass through the engine. A Lua weapon lives
// here, outside the game DLL, and the engine tells the client's HUD about it the way the game would:
// its own WeaponList with a weapon number the game doesn't use, its bit added to the bits the client
// sees, and CurWeapon / AmmoX as its clip and ammo change.
//
// So far (SWEP plan step 1): the registry, the player's Lua weapons and ammo, and the HUD. Choosing a
// Lua weapon, firing it and saving it are the next steps.
//
// The weapon a player holds is a Lua table of its own (one per class, as the game has one weapon of a
// kind per player); its metatable leads to the class's SWEP table, then SWEP.Base's, then the methods
// here. The clip and the ammo counts are kept in C so the HUD can follow them.
#include "lua_local.h"
#include "server.h"

#define SWEPS_KEY "newen.sweps"     // class name -> SWEP
#define WMETA_KEY "newen.wmeta"     // class name -> the metatable the weapon of that class gets
#define WDATA_KEY "newen.weapons"   // class name -> the player's weapon (a table)
#define WBASE_KEY "newen.wbase"     // the methods every weapon has

// what the client DLL's HUD has room for (the public SDK's layout; its arrays are this big)
#define HUD_WEAPONS    32   // weapon numbers 1..31; 31 is the suit's bit
#define HUD_AMMO       32   // ammo numbers 1..31
#define HUD_SLOTS      5
#define HUD_POSITIONS  5
#define FIRST_LUA_ID   16   // Half-Life uses 1..15

typedef struct {
	char    *name;
	int      max;
	int      count;      // the player's
	int      hud;        // the HUD's number for it; 0 = none yet (or no room)
	int      sent;       // the count the HUD has; -1 = tell it
	qboolean warned;
} ammo_t;

typedef struct {
	char    *cls;
	int      id;         // weapon number, given the first time the class is handed out; kept for the session
	int      slot, pos;
	qboolean owned;
	int      clip1, clip2;
	qboolean listed;     // the HUD has its WeaponList
	int      sent_clip;  // the clip the HUD has; -2 = tell it
	qboolean popup;      // show the pickup icon once it's listed
	qboolean warned;
} lweapon_t;

static ammo_t    *ammo;
static int        ammo_count;
static lweapon_t *weps;
static int        weps_count;

// what the game's own WeaponList messages said it uses, so Lua weapons keep out of its way (a mod may
// use other numbers and menu places than Half-Life)
static unsigned int game_ids, game_ammo;
static char        *game_names[HUD_WEAPONS];
static qboolean     game_taken[HUD_SLOTS][HUD_POSITIONS];
static qboolean     game_listed;   // seen this session
static int          last_game_bits;

static ammo_t *FindAmmo(const char *name)
{
	for (int i = 0; name && i < ammo_count; i++)
		if (!stricmp(ammo[i].name, name))
			return &ammo[i];
	return NULL;
}

static lweapon_t *FindWeapon(const char *cls)
{
	for (int i = 0; cls && i < weps_count; i++)
		if (!strcmp(weps[i].cls, cls))
			return &weps[i];
	return NULL;
}

static qboolean IsLocalPlayer(const edict_t *e)
{
	return e && e == SV_LocalPlayer();
}

// ---------------------------------------------------------------- Lua tables

static void PushRegTable(lua_State *L, const char *key)
{
	if (lua_getfield(L, LUA_REGISTRYINDEX, key) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
	}
}

// pushes the class's SWEP table; false (nothing pushed) if it isn't registered
static qboolean PushClass(lua_State *L, const char *cls)
{
	PushRegTable(L, SWEPS_KEY);
	lua_getfield(L, -1, cls);
	lua_remove(L, -2);
	if (lua_istable(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

static qboolean IsRegistered(lua_State *L, const char *cls)
{
	if (!L || !PushClass(L, cls))
		return false;
	lua_pop(L, 1);
	return true;
}

// SWEP.field, or SWEP.sub.field (Primary.ClipSize); read through the class so a base's value counts
static int ClassInt(lua_State *L, const char *cls, const char *sub, const char *field, int def)
{
	int top = lua_gettop(L);
	int v = def;
	if (PushClass(L, cls)) {
		if (sub)
			lua_getfield(L, -1, sub);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, field);
			if (lua_isnumber(L, -1))
				v = (int)lua_tonumber(L, -1);
		}
	}
	lua_settop(L, top);
	return v;
}

// the ammo type SWEP.Primary.Ammo / SWEP.Secondary.Ammo names; NULL for none
static ammo_t *ClassAmmo(lua_State *L, const char *cls, const char *sub)
{
	int top = lua_gettop(L);
	ammo_t *a = NULL;
	if (PushClass(L, cls) && lua_getfield(L, -1, sub) == LUA_TTABLE && lua_getfield(L, -1, "Ammo") == LUA_TSTRING)
		a = FindAmmo(lua_tostring(L, -1));
	lua_settop(L, top);
	return a;
}

// pushes the player's weapon of that class, making its table if it has none (after lua_reload)
static void PushWeapon(lua_State *L, lweapon_t *w)
{
	PushRegTable(L, WDATA_KEY);
	if (lua_getfield(L, -1, w->cls) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		PushRegTable(L, WMETA_KEY);
		lua_getfield(L, -1, w->cls);
		lua_remove(L, -2);
		if (lua_istable(L, -1))
			lua_setmetatable(L, -2);
		else
			lua_pop(L, 1);
		LuaG_PushEntity(L, SV_LocalPlayer());
		lua_setfield(L, -2, "Owner");
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, w->cls);
	}
	lua_remove(L, -2);
}

// weapon:name() if the class has that function
static void CallWeapon(lua_State *L, lweapon_t *w, const char *name)
{
	int top = lua_gettop(L);
	PushWeapon(L, w);
	lua_getfield(L, -1, name);
	if (lua_isfunction(L, -1)) {
		lua_insert(L, -2);
		gGlobals.time = (float)sv.time;
		Lua_Call(1, 0, va("%s:%s", w->cls, name));
	}
	lua_settop(L, top);
}

// ---------------------------------------------------------------- the HUD

static void SendWeaponList(lua_State *L, lweapon_t *w)
{
	int msg = SV_UserMsgIndex("WeaponList");
	if (!msg)
		return;
	ammo_t *a1 = ClassAmmo(L, w->cls, "Primary"), *a2 = ClassAmmo(L, w->cls, "Secondary");
	unsigned char buf[128];
	int n = 0;
	size_t len = strlen(w->cls);
	if (len > sizeof(buf) - 16)
		len = sizeof(buf) - 16;
	memcpy(buf, w->cls, len);
	n = (int)len;
	buf[n++] = 0;
	// no ammo type is -1, and a maximum of -1 with it: the menu then never shows the weapon as empty
	buf[n++] = (unsigned char)(a1 && a1->hud ? a1->hud : 255);
	buf[n++] = (unsigned char)(a1 && a1->hud ? (a1->max > 254 ? 254 : a1->max) : 255);
	buf[n++] = (unsigned char)(a2 && a2->hud ? a2->hud : 255);
	buf[n++] = (unsigned char)(a2 && a2->hud ? (a2->max > 254 ? 254 : a2->max) : 255);
	buf[n++] = (unsigned char)w->slot;
	buf[n++] = (unsigned char)w->pos;
	buf[n++] = (unsigned char)w->id;
	buf[n++] = 0;   // item flags
	CL_QueueMessage(msg, buf, n);
	Con_DPrintf("Lua weapons: WeaponList %s: number %d, slot %d, place %d, ammo %d/%d\n", w->cls, w->id,
		w->slot + 1, w->pos + 1, (signed char)buf[len + 1], (signed char)buf[len + 3]);
}

// state 0: the weapon isn't the one in hand; the HUD only takes its clip
static void SendCurWeapon(lweapon_t *w, int state)
{
	int msg = SV_UserMsgIndex("CurWeapon");
	if (!msg)
		return;
	int clip = w->clip1 > 127 ? 127 : w->clip1;   // a signed byte; -1 = the weapon has no clip
	unsigned char buf[3] = { (unsigned char)state, (unsigned char)w->id, (unsigned char)clip };
	CL_QueueMessage(msg, buf, 3);
	Con_DPrintf("Lua weapons: CurWeapon %s: state %d, number %d, clip %d\n", w->cls, state, w->id, clip);
}

static void SendByteMsg(const char *name, int a, int b)
{
	int msg = SV_UserMsgIndex(name);
	if (!msg)
		return;
	unsigned char buf[2] = { (unsigned char)a, (unsigned char)(b > 255 ? 255 : b < 0 ? 0 : b) };
	CL_QueueMessage(msg, buf, 2);
}

static unsigned int UsedIds(void)
{
	unsigned int used = game_ids | 1u | (1u << 31);
	for (int i = 0; i < weps_count; i++)
		if (weps[i].id)
			used |= 1u << weps[i].id;
	return used;
}

static qboolean PlaceFree(int slot, int pos)
{
	if (slot < 0 || slot >= HUD_SLOTS || pos < 0 || pos >= HUD_POSITIONS || game_taken[slot][pos])
		return false;
	for (int i = 0; i < weps_count; i++)
		if (weps[i].id && weps[i].slot == slot && weps[i].pos == pos)
			return false;
	return true;
}

// a weapon number and a place in the menu: the one SWEP.Slot / SWEP.SlotPos ask for if it's free, else
// the next free place in that slot, else in any slot. False if the HUD is full.
static qboolean AssignHud(lua_State *L, lweapon_t *w)
{
	unsigned int used = UsedIds();
	int id = 0;
	for (int i = FIRST_LUA_ID; i < HUD_WEAPONS - 1 && !id; i++)
		if (!(used & (1u << i)))
			id = i;
	// a mod that uses 16..30 itself leaves the low numbers
	for (int i = FIRST_LUA_ID - 1; i >= 1 && !id; i--)
		if (!(used & (1u << i)))
			id = i;
	int slot = ClassInt(L, w->cls, NULL, "Slot", 0), pos = ClassInt(L, w->cls, NULL, "SlotPos", 0);
	if (slot < 0 || slot >= HUD_SLOTS)
		slot = 0;
	qboolean found = PlaceFree(slot, pos);
	for (int p = 0; p < HUD_POSITIONS && !found; p++)
		if (PlaceFree(slot, p)) {
			pos = p;
			found = true;
		}
	for (int s = 0; s < HUD_SLOTS && !found; s++)
		for (int p = 0; p < HUD_POSITIONS && !found; p++)
			if (PlaceFree(s, p)) {
				slot = s;
				pos = p;
				found = true;
			}
	if (!id || !found) {
		if (!w->warned)
			Con_Warnf("[Lua] %s: the weapon menu is full, so it can't be shown (the HUD has %d places and %d weapon numbers)\n",
				w->cls, HUD_SLOTS * HUD_POSITIONS, HUD_WEAPONS - 2);
		w->warned = true;
		return false;
	}
	w->id = id;
	w->slot = slot;
	w->pos = pos;
	return true;
}

static void AssignAmmoHud(ammo_t *a)
{
	unsigned int used = game_ammo | 1u;
	for (int i = 0; i < ammo_count; i++)
		if (ammo[i].hud)
			used |= 1u << ammo[i].hud;
	// from the top down: the game counts its own up from 1
	for (int i = HUD_AMMO - 1; i >= 1; i--)
		if (!(used & (1u << i))) {
			a->hud = i;
			return;
		}
	if (!a->warned)
		Con_Warnf("[Lua] ammo type %s: the HUD has no room for another ammo type (it has %d)\n", a->name, HUD_AMMO - 1);
	a->warned = true;
}

// the HUD starts over (a new map, a respawn): everything is told again
static void ResendAll(void)
{
	for (int i = 0; i < weps_count; i++) {
		weps[i].listed = false;
		weps[i].sent_clip = -2;
	}
	for (int i = 0; i < ammo_count; i++)
		ammo[i].sent = -1;
}

// a message from the game DLL on its way to the client
void LuaW_GameMessage(const char *name, const unsigned char *data, int len)
{
	if (!name)
		return;
	if (!strcmp(name, "InitHUD") || !strcmp(name, "ResetHUD")) {
		ResendAll();
		return;
	}
	if (strcmp(name, "WeaponList"))
		return;
	// name, then: ammo 1, its maximum, ammo 2, its maximum, slot, place, number, flags
	int n = 0;
	while (n < len && data[n])
		n++;
	if (n + 9 > len)
		return;
	const signed char *f = (const signed char *)data + n + 1;
	int slot = f[4], pos = f[5], id = f[6];
	if (id < 1 || id >= HUD_WEAPONS)
		return;
	game_listed = true;
	game_ids |= 1u << id;
	if (f[0] > 0 && f[0] < HUD_AMMO)
		game_ammo |= 1u << f[0];
	if (f[2] > 0 && f[2] < HUD_AMMO)
		game_ammo |= 1u << f[2];
	if (slot >= 0 && slot < HUD_SLOTS && pos >= 0 && pos < HUD_POSITIONS)
		game_taken[slot][pos] = true;
	if (!game_names[id] || strcmp(game_names[id], (const char *)data)) {
		Mem_Free(game_names[id]);
		game_names[id] = Mem_Strdup((const char *)data);
	}
}

// the weapon bits the client's HUD should see on top of the game's own
int LuaW_WeaponBits(void)
{
	int bits = 0;
	for (int i = 0; i < weps_count; i++)
		if (weps[i].owned && weps[i].listed)
			bits |= 1 << weps[i].id;
	return bits;
}

// the client asks for sprites/<weapon name>.txt; a Lua weapon that ships none gets newen's icon
const char *LuaW_SpriteList(const char *path)
{
	if (!path || strnicmp(path, "sprites/", 8))
		return NULL;
	char cls[64];
	Q_strncpy(cls, path + 8, sizeof(cls));
	char *dot = strrchr(cls, '.');
	if (dot)
		*dot = 0;
	return FindWeapon(cls) ? "sprites/weapon_lua_generic.txt" : NULL;
}

// ---------------------------------------------------------------- the player's weapons

static void Strip(lua_State *L, lweapon_t *w)
{
	if (!w->owned)
		return;
	if (L) {
		CallWeapon(L, w, "OnRemove");
		PushRegTable(L, WDATA_KEY);
		lua_pushnil(L);
		lua_setfield(L, -2, w->cls);
		lua_pop(L, 1);
	}
	w->owned = false;   // its bit goes; the HUD drops it from the menu
	w->popup = false;
}

static void StripAll(qboolean ammo_too)
{
	lua_State *L = Lua_GetState();
	for (int i = 0; i < weps_count; i++)
		Strip(L, &weps[i]);
	for (int i = 0; ammo_too && i < ammo_count; i++) {
		ammo[i].count = 0;
		ammo[i].sent = -1;
	}
}

// adds to the player's ammo, up to the type's maximum; returns what was added
static int AddAmmo(ammo_t *a, int amount)
{
	int before = a->count;
	a->count += amount;
	if (a->count > a->max)
		a->count = a->max;
	if (a->count < 0)
		a->count = 0;
	return a->count - before;
}

// gives the player a Lua weapon and leaves it on the stack; false (nothing pushed) if cls isn't one
qboolean LuaW_Give(lua_State *L, edict_t *pl, const char *cls)
{
	if (!IsRegistered(L, cls))
		return false;
	if (!IsLocalPlayer(pl)) {
		lua_pushnil(L);
		return true;
	}
	lweapon_t *w = FindWeapon(cls);
	if (!w) {
		weps = Mem_Realloc(weps, sizeof(*weps) * (size_t)(weps_count + 1));
		w = &weps[weps_count++];
		memset(w, 0, sizeof(*w));
		w->cls = Mem_Strdup(cls);
	}
	int clipsize = ClassInt(L, cls, "Primary", "ClipSize", -1);
	int give = ClassInt(L, cls, "Primary", "DefaultClip", clipsize > 0 ? clipsize : 0);
	int give2 = ClassInt(L, cls, "Secondary", "DefaultClip", 0);
	ammo_t *a1 = ClassAmmo(L, cls, "Primary"), *a2 = ClassAmmo(L, cls, "Secondary");
	if (w->owned) {
		// a second one is only its ammo, as when the game's player walks over a weapon they have
		if (a1 && give > 0)
			AddAmmo(a1, give);
		if (a2 && give2 > 0)
			AddAmmo(a2, give2);
		PushWeapon(L, w);
		return true;
	}
	w->owned = true;
	w->listed = false;
	w->sent_clip = -2;
	w->popup = true;
	// the first clip comes loaded; the rest of DefaultClip goes to the player's ammo
	w->clip1 = clipsize > 0 ? (give < clipsize ? (give > 0 ? give : 0) : clipsize) : -1;
	w->clip2 = ClassInt(L, cls, "Secondary", "ClipSize", -1) > 0 ? 0 : -1;
	if (a1 && give - (w->clip1 > 0 ? w->clip1 : 0) > 0)
		AddAmmo(a1, give - (w->clip1 > 0 ? w->clip1 : 0));
	if (a2 && give2 > 0)
		AddAmmo(a2, give2);

	// a table left from before (it shouldn't be) would carry another life's data
	PushRegTable(L, WDATA_KEY);
	lua_pushnil(L);
	lua_setfield(L, -2, cls);
	lua_pop(L, 1);
	CallWeapon(L, w, "Initialize");
	int top = lua_gettop(L);
	PushWeapon(L, w);
	LuaG_PushEntity(L, pl);
	Lua_Hook("WeaponEquip", 2, 0);
	lua_settop(L, top);
	PushWeapon(L, w);
	return true;
}

// "give weapon_lua_x" in the console, under the same rule as the game's give (sv_cheats)
qboolean LuaW_ClientCommand(edict_t *pl)
{
	lua_State *L = Lua_GetState();
	if (!L || Cmd_Argc() < 2 || stricmp(Cmd_Argv(0), "give") || !IsRegistered(L, Cmd_Argv(1)))
		return false;
	if (Cvar_VariableValue("sv_cheats") == 0) {
		Con_Printf("give needs sv_cheats 1\n");
		return true;
	}
	int top = lua_gettop(L);
	LuaW_Give(L, pl, Cmd_Argv(1));
	lua_settop(L, top);
	return true;
}

void LuaW_PlayerDied(edict_t *pl)
{
	if (IsLocalPlayer(pl))
		StripAll(true);   // the game takes its own weapons and ammo from a dead player too
}

// once a server frame: follow the game taking the player's weapons, and bring the HUD up to date
void LuaW_Frame(void)
{
	edict_t *pl = SV_LocalPlayer();
	lua_State *L = Lua_GetState();
	if (!pl || !L)
		return;
	// player_weaponstrip and the like clear the game's weapon bits (the suit's stays)
	int bits = pl->v.weapons & 0x7fffffff;
	if (last_game_bits && !bits && pl->v.health > 0)
		StripAll(false);
	last_game_bits = bits;

	// numbers and menu places are picked once the game has listed its own, a moment after the map starts.
	// A game that lists none (no weapons at all) isn't waited for.
	if (!game_listed && sv.time < 1.0)
		return;
	for (int i = 0; i < ammo_count; i++) {
		ammo_t *a = &ammo[i];
		if (!a->hud && !a->warned)
			AssignAmmoHud(a);
		if (a->hud && a->sent != a->count) {
			SendByteMsg("AmmoX", a->hud, a->count);
			Con_DPrintf("Lua weapons: AmmoX %s: number %d, count %d\n", a->name, a->hud, a->count);
			a->sent = a->count;
		}
	}
	for (int i = 0; i < weps_count; i++) {
		lweapon_t *w = &weps[i];
		if (!w->owned || (!w->id && !AssignHud(L, w)))
			continue;
		if (!w->listed) {
			SendWeaponList(L, w);
			w->listed = true;
			w->sent_clip = -2;
		}
		if (w->sent_clip != w->clip1) {
			SendCurWeapon(w, 0);
			w->sent_clip = w->clip1;
		}
		if (w->popup) {
			// the icon that slides in at the right when a weapon is picked up
			int msg = SV_UserMsgIndex("WeapPickup");
			unsigned char id = (unsigned char)w->id;
			if (msg)
				CL_QueueMessage(msg, &id, 1);
			w->popup = false;
		}
	}
}

// a new map: the player starts it without Lua weapons (carrying them over is the plan's step 4)
void LuaW_ServerStarted(void)
{
	lua_State *L = Lua_GetState();
	for (int i = 0; i < weps_count; i++)
		weps[i].owned = false;
	for (int i = 0; i < ammo_count; i++)
		ammo[i].count = 0;
	if (L) {
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, WDATA_KEY);
	}
	last_game_bits = 0;
	ResendAll();
}

// ---------------------------------------------------------------- Lua: weapons

// SWEP.Base names another class: SWEP reads what it lacks from it. Without one (or until it's
// registered) it reads from the methods every weapon has. False if the base isn't registered.
static qboolean ResolveBase(lua_State *L, int swep)
{
	swep = lua_absindex(L, swep);
	int top = lua_gettop(L);
	qboolean ok = true;
	lua_getfield(L, LUA_REGISTRYINDEX, WBASE_KEY);
	int parent = lua_gettop(L);
	if (lua_getfield(L, swep, "Base") == LUA_TSTRING) {
		if (PushClass(L, lua_tostring(L, -1)) && !lua_rawequal(L, -1, swep)) {
			parent = lua_gettop(L);
			lua_pushvalue(L, parent);
			lua_setfield(L, swep, "BaseClass");
		} else {
			ok = false;
		}
	}
	lua_createtable(L, 0, 1);
	lua_pushvalue(L, parent);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, swep);
	lua_settop(L, top);
	return ok;
}

static int w_tostring(lua_State *L)
{
	lua_getfield(L, 1, "ClassName");
	lua_pushfstring(L, "Weapon [%s]", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
	return 1;
}

// weapons.Register(SWEP, classname)
static int l_Register(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	const char *cls = luaL_checkstring(L, 2);
	lua_settop(L, 2);
	lua_pushvalue(L, 2);
	lua_setfield(L, 1, "ClassName");
	PushRegTable(L, SWEPS_KEY);
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, cls);
	ResolveBase(L, 1);
	// a weapon of this class the player already holds picks up the new table (a file run again)
	PushRegTable(L, WMETA_KEY);
	if (lua_getfield(L, -1, cls) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_createtable(L, 0, 2);
		lua_pushcfunction(L, w_tostring);
		lua_setfield(L, -2, "__tostring");
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, cls);
	}
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "__index");
	lweapon_t *w = FindWeapon(cls);
	if (w)
		w->listed = false;   // its slot or ammo type may have changed
	return 0;
}

static int l_Get(lua_State *L)
{
	if (!PushClass(L, luaL_checkstring(L, 1)))
		lua_pushnil(L);
	return 1;
}

static int l_GetList(lua_State *L)
{
	lua_newtable(L);
	PushRegTable(L, SWEPS_KEY);
	int n = 0;
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
		lua_rawseti(L, -4, ++n);
	}
	lua_pop(L, 1);
	return 1;
}

// the weapon a method was called on; NULL if the player no longer has it
static lweapon_t *SelfWeapon(lua_State *L, qboolean must)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "ClassName");
	lweapon_t *w = FindWeapon(lua_tostring(L, -1));
	lua_pop(L, 1);
	if (w && w->owned) {
		// an old table of a weapon that was taken away and given again isn't the weapon
		PushRegTable(L, WDATA_KEY);
		lua_getfield(L, -1, w->cls);
		qboolean same = lua_rawequal(L, -1, 1);
		lua_pop(L, 2);
		if (same)
			return w;
	}
	if (must)
		luaL_error(L, "tried to use a weapon the player no longer has");
	return NULL;
}

static int w_IsValid(lua_State *L)  { lua_pushboolean(L, SelfWeapon(L, false) != NULL); return 1; }
static int w_GetClass(lua_State *L) { luaL_checktype(L, 1, LUA_TTABLE); lua_getfield(L, 1, "ClassName"); return 1; }
static int w_GetOwner(lua_State *L) { LuaG_PushEntity(L, SelfWeapon(L, false) ? SV_LocalPlayer() : NULL); return 1; }
static int w_Clip1(lua_State *L)    { lua_pushinteger(L, SelfWeapon(L, true)->clip1); return 1; }
static int w_Clip2(lua_State *L)    { lua_pushinteger(L, SelfWeapon(L, true)->clip2); return 1; }
static int w_SetClip1(lua_State *L) { SelfWeapon(L, true)->clip1 = (int)luaL_checkinteger(L, 2); return 0; }
static int w_SetClip2(lua_State *L) { SelfWeapon(L, true)->clip2 = (int)luaL_checkinteger(L, 2); return 0; }
static int w_GetSlot(lua_State *L)    { lua_pushinteger(L, SelfWeapon(L, true)->slot); return 1; }
static int w_GetSlotPos(lua_State *L) { lua_pushinteger(L, SelfWeapon(L, true)->pos); return 1; }

static int w_GetMaxClip1(lua_State *L)
{
	lua_pushinteger(L, ClassInt(L, SelfWeapon(L, true)->cls, "Primary", "ClipSize", -1));
	return 1;
}

static int w_GetMaxClip2(lua_State *L)
{
	lua_pushinteger(L, ClassInt(L, SelfWeapon(L, true)->cls, "Secondary", "ClipSize", -1));
	return 1;
}

static int PushAmmoName(lua_State *L, const char *sub)
{
	ammo_t *a = ClassAmmo(L, SelfWeapon(L, true)->cls, sub);
	if (a)
		lua_pushstring(L, a->name);
	else
		lua_pushnil(L);
	return 1;
}

static int PushAmmoCount(lua_State *L, const char *sub)
{
	ammo_t *a = ClassAmmo(L, SelfWeapon(L, true)->cls, sub);
	lua_pushinteger(L, a ? a->count : 0);
	return 1;
}

// the type's name (GMod gives a number; here ammo is asked for by name everywhere)
static int w_GetPrimaryAmmoType(lua_State *L)   { return PushAmmoName(L, "Primary"); }
static int w_GetSecondaryAmmoType(lua_State *L) { return PushAmmoName(L, "Secondary"); }
static int w_Ammo1(lua_State *L)                { return PushAmmoCount(L, "Primary"); }
static int w_Ammo2(lua_State *L)                { return PushAmmoCount(L, "Secondary"); }

static int w_GetPrintName(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_getfield(L, 1, "PrintName") != LUA_TSTRING)
		lua_getfield(L, 1, "ClassName");
	return 1;
}

static int w_Remove(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, false);
	if (w)
		Strip(L, w);
	return 0;
}

static const luaL_Reg weapon_methods[] = {
	{ "IsValid", w_IsValid }, { "GetClass", w_GetClass }, { "GetOwner", w_GetOwner }, { "Clip1", w_Clip1 },
	{ "Clip2", w_Clip2 }, { "SetClip1", w_SetClip1 }, { "SetClip2", w_SetClip2 }, { "GetMaxClip1", w_GetMaxClip1 },
	{ "GetMaxClip2", w_GetMaxClip2 }, { "GetSlot", w_GetSlot }, { "GetSlotPos", w_GetSlotPos },
	{ "GetPrimaryAmmoType", w_GetPrimaryAmmoType }, { "GetSecondaryAmmoType", w_GetSecondaryAmmoType },
	{ "Ammo1", w_Ammo1 }, { "Ammo2", w_Ammo2 }, { "GetPrintName", w_GetPrintName }, { "Remove", w_Remove },
	{ NULL, NULL }
};

// ---------------------------------------------------------------- Lua: the player

static edict_t *CheckPlayer(lua_State *L)
{
	edict_t *pl = LuaG_CheckEdict(L, 1);
	int n = NUM_FOR_EDICT(pl);
	if (n < 1 || n > sv.maxclients)
		luaL_error(L, "this only works on players");
	return pl;
}

// ply:HasWeapon(class): Lua weapons, and the game's own (by the names it listed and the weapon bits)
static int p_HasWeapon(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	const char *cls = luaL_checkstring(L, 2);
	lweapon_t *w = FindWeapon(cls);
	qboolean has = w && w->owned && IsLocalPlayer(pl);
	for (int i = 1; i < HUD_WEAPONS - 1 && !has; i++)
		has = game_names[i] && !strcmp(game_names[i], cls) && (pl->v.weapons & (1 << i));
	lua_pushboolean(L, has);
	return 1;
}

// ply:GetWeapon(class): the Lua weapon, or nil (the game's own weapons can't be reached)
static int p_GetWeapon(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	lweapon_t *w = FindWeapon(luaL_checkstring(L, 2));
	if (w && w->owned && IsLocalPlayer(pl))
		PushWeapon(L, w);
	else
		lua_pushnil(L);
	return 1;
}

// ply:GetWeapons(): the player's Lua weapons
static int p_GetWeapons(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	lua_newtable(L);
	int n = 0;
	for (int i = 0; IsLocalPlayer(pl) && i < weps_count; i++)
		if (weps[i].owned) {
			PushWeapon(L, &weps[i]);
			lua_rawseti(L, -2, ++n);
		}
	return 1;
}

static int p_StripWeapon(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	lweapon_t *w = FindWeapon(luaL_checkstring(L, 2));
	if (w && IsLocalPlayer(pl))
		Strip(L, w);
	return 0;
}

static ammo_t *CheckAmmo(lua_State *L, int idx)
{
	const char *name = luaL_checkstring(L, idx);
	ammo_t *a = FindAmmo(name);
	if (!a)
		luaL_error(L, "there's no Lua ammo type named %s (game.AddAmmoType makes one)", name);
	return a;
}

// ply:GiveAmmo(amount, type, hidePopup): returns what fitted
static int p_GiveAmmo(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	int amount = (int)luaL_checkinteger(L, 2);
	ammo_t *a = CheckAmmo(L, 3);
	int given = IsLocalPlayer(pl) && amount > 0 ? AddAmmo(a, amount) : 0;
	if (given > 0 && a->hud && !lua_toboolean(L, 4))
		SendByteMsg("AmmoPickup", a->hud, given);
	lua_pushinteger(L, given);
	return 1;
}

static int p_GetAmmoCount(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	ammo_t *a = CheckAmmo(L, 2);
	lua_pushinteger(L, IsLocalPlayer(pl) ? a->count : 0);
	return 1;
}

// ply:SetAmmo(amount, type)
static int p_SetAmmo(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	int amount = (int)luaL_checkinteger(L, 2);
	ammo_t *a = CheckAmmo(L, 3);
	if (IsLocalPlayer(pl))
		a->count = amount < 0 ? 0 : amount > a->max ? a->max : amount;
	return 0;
}

// ply:RemoveAmmo(amount, type)
static int p_RemoveAmmo(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	int amount = (int)luaL_checkinteger(L, 2);
	ammo_t *a = CheckAmmo(L, 3);
	if (IsLocalPlayer(pl))
		AddAmmo(a, -amount);
	return 0;
}

// ---------------------------------------------------------------- Lua: game

// game.AddAmmoType{ name = "pipe", max = 100 }; adding a name again only changes its maximum
static int l_AddAmmoType(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_getfield(L, 1, "name") != LUA_TSTRING)
		return luaL_error(L, "game.AddAmmoType needs a name");
	const char *name = lua_tostring(L, -1);
	int max = 9999;
	if (lua_getfield(L, 1, "max") == LUA_TNUMBER || lua_getfield(L, 1, "maxcarry") == LUA_TNUMBER)
		max = (int)lua_tonumber(L, -1);
	ammo_t *a = FindAmmo(name);
	if (!a) {
		ammo = Mem_Realloc(ammo, sizeof(*ammo) * (size_t)(ammo_count + 1));
		a = &ammo[ammo_count++];
		memset(a, 0, sizeof(*a));
		a->name = Mem_Strdup(name);
		a->sent = -1;
	}
	a->max = max < 1 ? 1 : max;
	if (a->count > a->max)
		a->count = a->max;
	return 0;
}

static int l_GetAmmoMax(lua_State *L)
{
	lua_pushinteger(L, CheckAmmo(L, 1)->max);
	return 1;
}

static int l_GetAmmoTypes(lua_State *L)
{
	lua_createtable(L, ammo_count, 0);
	for (int i = 0; i < ammo_count; i++) {
		lua_pushstring(L, ammo[i].name);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

// ---------------------------------------------------------------- start

// lua_weapons: what newen knows about Lua weapons, for checking a script
static void LuaWeapons_f(void)
{
	Con_Printf("%d Lua weapon class%s handed out this session:\n", weps_count, weps_count == 1 ? "" : "es");
	for (int i = 0; i < weps_count; i++) {
		lweapon_t *w = &weps[i];
		if (!w->id)
			Con_Printf("  %-24s %s, not in the menu yet\n", w->cls, w->owned ? "held" : "not held");
		else
			Con_Printf("  %-24s %s, number %d, slot %d place %d, clip %d\n", w->cls, w->owned ? "held" : "not held",
				w->id, w->slot + 1, w->pos + 1, w->clip1);
	}
	Con_Printf("%d Lua ammo type%s:\n", ammo_count, ammo_count == 1 ? "" : "s");
	for (int i = 0; i < ammo_count; i++)
		Con_Printf("  %-24s %d of %d, HUD number %d\n", ammo[i].name, ammo[i].count, ammo[i].max, ammo[i].hud);
}

// after lua/weapons/*.lua ran (at start and after lua_reload)
void LuaW_StateStarted(void)
{
	lua_State *L = Lua_GetState();
	if (!L)
		return;
	int top = lua_gettop(L);
	// bases registered after the classes that use them (the files load alphabetically)
	PushRegTable(L, SWEPS_KEY);
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		if (lua_istable(L, -1) && !ResolveBase(L, -1)) {
			lua_getfield(L, -1, "Base");
			Con_Warnf("[Lua] %s: base class %s isn't registered\n", lua_tostring(L, -3), lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_settop(L, top);
	// lua_reload: the player keeps their weapons (clip and ammo are here), unless the class is gone.
	// What was in the weapons' own tables is lost.
	for (int i = 0; i < weps_count; i++) {
		if (weps[i].owned && !IsRegistered(L, weps[i].cls))
			weps[i].owned = false;
		weps[i].listed = false;
	}
}

void LuaW_Open(lua_State *L)
{
	static qboolean cmd_added;
	if (!cmd_added) {
		Cmd_AddCommand("lua_weapons", LuaWeapons_f, "lists the Lua weapons and ammo types in use");
		cmd_added = true;
	}

	luaL_newlib(L, weapon_methods);
	lua_setfield(L, LUA_REGISTRYINDEX, WBASE_KEY);

	static const luaL_Reg lib[] = { { "Register", l_Register }, { "Get", l_Get }, { "GetList", l_GetList }, { NULL, NULL } };
	luaL_newlib(L, lib);
	lua_setglobal(L, "weapons");

	static const luaL_Reg game[] = {
		{ "AddAmmoType", l_AddAmmoType }, { "GetAmmoMax", l_GetAmmoMax }, { "GetAmmoTypes", l_GetAmmoTypes }, { NULL, NULL }
	};
	if (lua_getglobal(L, "game") != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "game");
	}
	luaL_setfuncs(L, game, 0);
	lua_pop(L, 1);

	static const luaL_Reg methods[] = {
		{ "HasWeapon", p_HasWeapon }, { "GetWeapon", p_GetWeapon }, { "GetWeapons", p_GetWeapons },
		{ "StripWeapon", p_StripWeapon }, { "GiveAmmo", p_GiveAmmo }, { "GetAmmoCount", p_GetAmmoCount },
		{ "SetAmmo", p_SetAmmo }, { "RemoveAmmo", p_RemoveAmmo }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);
}
