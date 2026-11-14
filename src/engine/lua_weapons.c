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
// Taking a Lua weapon in hand (step 2): the game's own weapon stays the active one inside the game DLL
// but is put to sleep. Its fire, alt-fire and reload buttons are taken out of the input, and its
// view-model animations and CurWeapon messages are held back; the client is shown the Lua weapon's view
// model instead of the game's. Going back, the game's weapon is woken: its CurWeapon is sent again and its
// view model draws (the game doesn't redeploy a weapon that is already its active one). If the game
// changes weapon by itself, its view model changes, and that ends the Lua weapon's turn.
//
// Firing (step 3): each frame the weapon in hand gets Think, and PrimaryAttack / SecondaryAttack / Reload
// while their buttons are held and its own next-fire times allow, as Garry's Mod calls them. ShootBullet
// fires through the engine's bullets (sv_damage.c), which hurt through the game's own damage rules.
//
// Saved games and level changes (step 4): the player's Lua weapons, their clips, their own tables' plain
// data, the ammo and which weapon was in hand are packed into one block (LuaW_SaveData). A saved game
// carries it; a level change packs it before the new map starts and unpacks it once the player is in;
// lua_reload does the same around the new Lua state.
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
	char    *viewmodel;  // SWEP.ViewModel, read when it's taken in hand; NULL for none
	double   next_primary, next_secondary;   // on the map's clock (sv.time)
	double   reload_end;                     // 0 when not reloading
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

// the Lua weapon in hand (an index into weps), and the game's weapon asleep behind it
#define WEAPON_BUTTONS (IN_ATTACK | IN_ATTACK2 | IN_RELOAD)
static int      active = -1;
static int      previous = -1;      // what "lastinv" goes to: a Lua weapon, or -1 for the game's own
static char     game_vm[128];       // the game's view model when it went to sleep, or last seen awake (by name:
                                    // a loaded game brings the same model back under another string number)
static int      resume = -1;        // a weapon to take in hand again once the player's first input comes
static int      game_cur[3];        // the game's last CurWeapon for a weapon in hand: state, number, clip
static qboolean game_cur_known;
static int      game_anim_body;     // the body of the game's last view-model animation
static int      held_buttons;       // the weapon buttons taken out of the input this frame
static qboolean holster_asked;      // Holster already ran for the command the game is working on
static int      last_buttons;       // last frame's, to tell a press from a hold

static qboolean GameVmChanged(const edict_t *pl)
{
	return strcmp(game_vm, pl->v.viewmodel ? SV_String(pl->v.viewmodel) : "") != 0;
}

static void RememberGameVm(const edict_t *pl)
{
	Q_strncpy(game_vm, pl->v.viewmodel ? SV_String(pl->v.viewmodel) : "", sizeof(game_vm));
}

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

// state 0: the weapon isn't the one in hand, and the HUD only takes its clip; 1: it is
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

static void GameSwitched(edict_t *pl);

// a message from the game DLL on its way to the local player's client; true holds it back
qboolean LuaW_GameMessage(int type, const char *name, const unsigned char *data, int len)
{
	edict_t *pl = SV_LocalPlayer();
	if (type == SVC_WEAPONANIM && len >= 2) {
		game_anim_body = data[1];
		if (active >= 0 && pl)
			GameSwitched(pl);   // a new weapon's deploy isn't the sleeping weapon's
		return active >= 0;
	}
	if (!name)
		return false;
	if (!strcmp(name, "CurWeapon") && len >= 3) {
		// state 0 with a weapon number only brings a clip; anything else says what is in hand
		if (data[0] == 0 && (signed char)data[1] >= 1)
			return false;
		game_cur[0] = data[0];
		game_cur[1] = (signed char)data[1];
		game_cur[2] = (signed char)data[2];
		game_cur_known = true;
		if (active >= 0 && pl)
			GameSwitched(pl);
		return active >= 0;
	}
	if (!strcmp(name, "InitHUD") || !strcmp(name, "ResetHUD")) {
		ResendAll();
		return false;
	}
	if (strcmp(name, "WeaponList"))
		return false;
	// name, then: ammo 1, its maximum, ammo 2, its maximum, slot, place, number, flags
	int n = 0;
	while (n < len && data[n])
		n++;
	if (n + 9 > len)
		return false;
	const signed char *f = (const signed char *)data + n + 1;
	int slot = f[4], pos = f[5], id = f[6];
	if (id < 1 || id >= HUD_WEAPONS)
		return false;
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
	return false;
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

// ---------------------------------------------------------------- taking a weapon in hand

static void QueueWeaponAnim(int seq, int body)
{
	unsigned char buf[2] = { (unsigned char)seq, (unsigned char)body };
	CL_QueueMessage(SVC_WEAPONANIM, buf, 2);
}

// the sequence a view model draws itself with; Half-Life's models name it one of these. -1 if none.
static int DrawSequence(const char *model)
{
	static const char *names[] = { "draw", "deploy", "draw1", "up" };
	const model_t *m = model && *model ? Mod_ForName(model, false) : NULL;
	for (size_t i = 0; m && i < sizeof(names) / sizeof(names[0]); i++) {
		int seq = R_StudioLookupSequence(m, names[i]);
		if (seq >= 0)
			return seq;
	}
	return -1;
}

// the view model the client should show instead of the game's; NULL when the game's weapon is in hand,
// "" for a Lua weapon without one
const char *LuaW_ViewModel(void)
{
	if (active < 0)
		return NULL;
	return weps[active].viewmodel ? weps[active].viewmodel : "";
}

// weapon:Holster(next); false only if the function says so (GMod's weapons return true to allow it)
static qboolean CallHolster(lua_State *L, lweapon_t *w, lweapon_t *next)
{
	int top = lua_gettop(L);
	qboolean ok = true;
	PushWeapon(L, w);
	lua_getfield(L, -1, "Holster");
	if (lua_isfunction(L, -1)) {
		lua_insert(L, -2);
		if (next)
			PushWeapon(L, next);
		else
			lua_pushnil(L);
		gGlobals.time = (float)sv.time;
		if (Lua_Call(2, 1, va("%s:Holster", w->cls)) && lua_isboolean(L, -1) && !lua_toboolean(L, -1))
			ok = false;
	}
	lua_settop(L, top);
	return ok;
}

// the game's weapon is in hand again. replay: it's the same weapon as before, which the game won't
// deploy again, so its HUD state and a drawing animation are put back here.
static void Wake(edict_t *pl, qboolean replay)
{
	if (active < 0)
		return;
	weps[active].reload_end = 0;   // a reload that wasn't finished is lost
	active = -1;
	held_buttons = 0;
	RememberGameVm(pl);
	if (!replay)
		return;
	if (game_cur_known) {
		int msg = SV_UserMsgIndex("CurWeapon");
		unsigned char buf[3] = { (unsigned char)game_cur[0], (unsigned char)game_cur[1], (unsigned char)game_cur[2] };
		if (msg)
			CL_QueueMessage(msg, buf, 3);
	}
	int seq = pl->v.viewmodel ? DrawSequence(SV_String(pl->v.viewmodel)) : -1;
	QueueWeaponAnim(seq >= 0 ? seq : 0, game_anim_body);
}

// the game changed weapon by itself while a Lua weapon was in hand (it picked a new one up, or the
// player's command made it switch): its view model isn't the sleeping one's any more
static void GameSwitched(edict_t *pl)
{
	if (active < 0 || !GameVmChanged(pl))
		return;
	lua_State *L = Lua_GetState();
	lweapon_t *w = &weps[active];
	previous = active;
	Wake(pl, false);
	if (L && !holster_asked)
		CallHolster(L, w, NULL);   // it can't refuse
}

// takes the Lua weapon in hand; false if the one in hand refuses to be put away
static qboolean Select(lua_State *L, edict_t *pl, int i)
{
	if (i == active)
		return true;
	lweapon_t *w = &weps[i];
	if (active >= 0) {
		if (!CallHolster(L, &weps[active], w))
			return false;
		weps[active].sent_clip = -2;   // the HUD hears it's no longer the one in hand
		weps[active].reload_end = 0;
		previous = active;
	} else {
		RememberGameVm(pl);   // the game's weapon goes to sleep
		previous = -1;
	}
	active = i;
	held_buttons = 0;
	w->sent_clip = -2;
	// SWEP.ViewModel, read now so a file run again takes effect
	Mem_Free(w->viewmodel);
	w->viewmodel = NULL;
	int top = lua_gettop(L);
	if (PushClass(L, w->cls) && lua_getfield(L, -1, "ViewModel") == LUA_TSTRING && *lua_tostring(L, -1))
		w->viewmodel = Mem_Strdup(lua_tostring(L, -1));
	lua_settop(L, top);
	int seq = DrawSequence(w->viewmodel);
	QueueWeaponAnim(seq >= 0 ? seq : 0, 0);
	// not while it's being drawn, as Half-Life's weapons wait half a second
	w->reload_end = 0;
	w->next_primary = w->next_secondary = sv.time + 0.5;
	last_buttons = WEAPON_BUTTONS;   // a button still down from before isn't a new press
	CallWeapon(L, w, "Deploy");
	return true;
}

// the input on its way to the game: a sleeping weapon doesn't get its buttons
void LuaW_PlayerCmd(edict_t *pl, usercmd_t *cmd)
{
	if (pl != SV_LocalPlayer())
		return;
	if (resume >= 0) {
		// after a loaded game or a level change: the game has its own weapon out again by now, and the
		// Lua weapon that was in hand takes its place as when it was first chosen
		lua_State *L = Lua_GetState();
		int i = resume;
		resume = -1;
		if (L && active < 0 && i < weps_count && weps[i].owned && pl->v.health > 0)
			Select(L, pl, i);
	}
	if (active < 0) {
		if (GameVmChanged(pl)) {
			RememberGameVm(pl);
			previous = -1;   // the game moved on to another weapon of its own
		}
		return;
	}
	GameSwitched(pl);
	if (active < 0)
		return;
	held_buttons = cmd->buttons & WEAPON_BUTTONS;
	cmd->buttons &= (unsigned short)~WEAPON_BUTTONS;
}

// the game weapon the player has by that name (as the game listed it), or 0
static int GameWeaponId(edict_t *pl, const char *cls)
{
	for (int i = 1; i < HUD_WEAPONS - 1; i++)
		if (game_names[i] && !stricmp(game_names[i], cls) && (pl->v.weapons & (1 << i)))
			return i;
	return 0;
}

// ---------------------------------------------------------------- firing

// a sequence of the weapon's view model by number or name (argument idx); -1 if there's no such one
static int ViewSequence(lua_State *L, lweapon_t *w, int idx)
{
	const model_t *m = w->viewmodel ? Mod_ForName(w->viewmodel, false) : NULL;
	if (lua_type(L, idx) == LUA_TSTRING)
		return m ? R_StudioLookupSequence(m, lua_tostring(L, idx)) : -1;
	return lua_isnumber(L, idx) ? (int)lua_tointeger(L, idx) : -1;
}

static void FinishReload(lua_State *L, lweapon_t *w)
{
	w->reload_end = 0;
	ammo_t *a = ClassAmmo(L, w->cls, "Primary");
	int size = ClassInt(L, w->cls, "Primary", "ClipSize", -1);
	if (size <= 0 || !a)
		return;
	int take = size - (w->clip1 > 0 ? w->clip1 : 0);
	if (take > a->count)
		take = a->count;
	if (take > 0) {
		w->clip1 = (w->clip1 > 0 ? w->clip1 : 0) + take;
		a->count -= take;
	}
}

// weapon:name() for a held button, if the class has that function
static void CallAttack(lua_State *L, int index, const char *name)
{
	CallWeapon(L, &weps[index], name);
}

// once a frame for the Lua weapon in hand, with this frame's buttons and view angles
void LuaW_PlayerThink(edict_t *pl)
{
	lua_State *L = Lua_GetState();
	if (active < 0 || !L || pl != SV_LocalPlayer())
		return;
	int buttons = held_buttons, pressed = buttons & ~last_buttons;
	last_buttons = buttons;
	if (pl->v.health <= 0)
		return;
	// each call may strip the weapon or change the one in hand; "mine" says it is still this one
	int mine = active;
#define STILL_MINE (active == mine && weps[mine].owned)
	CallWeapon(L, &weps[mine], "Think");
	if (STILL_MINE && weps[mine].reload_end && sv.time >= weps[mine].reload_end)
		FinishReload(L, &weps[mine]);
	if (STILL_MINE && (buttons & IN_RELOAD) && !weps[mine].reload_end)
		CallAttack(L, mine, "Reload");
	// a weapon that isn't automatic fires once per press
	if (STILL_MINE && (buttons & IN_ATTACK) && !weps[mine].reload_end && sv.time >= weps[mine].next_primary) {
		int top = lua_gettop(L);
		qboolean automatic = PushClass(L, weps[mine].cls) && lua_getfield(L, -1, "Primary") == LUA_TTABLE
			&& lua_getfield(L, -1, "Automatic") != LUA_TNIL && lua_toboolean(L, -1);
		lua_settop(L, top);
		if (automatic || (pressed & IN_ATTACK))
			CallAttack(L, mine, "PrimaryAttack");
	}
	if (STILL_MINE && (buttons & IN_ATTACK2) && !weps[mine].reload_end && sv.time >= weps[mine].next_secondary) {
		int top = lua_gettop(L);
		qboolean automatic = PushClass(L, weps[mine].cls) && lua_getfield(L, -1, "Secondary") == LUA_TTABLE
			&& lua_getfield(L, -1, "Automatic") != LUA_TNIL && lua_toboolean(L, -1);
		lua_settop(L, top);
		if (automatic || (pressed & IN_ATTACK2))
			CallAttack(L, mine, "SecondaryAttack");
	}
#undef STILL_MINE
}

// ---------------------------------------------------------------- the player's weapons

static void Strip(lua_State *L, lweapon_t *w)
{
	if (!w->owned)
		return;
	int i = (int)(w - weps);
	if (previous == i)
		previous = -1;
	if (active == i) {
		edict_t *pl = SV_LocalPlayer();
		if (L)
			CallHolster(L, w, NULL);
		if (pl)
			Wake(pl, pl->v.health > 0);
		active = -1;
	}
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
		int got = a1 && give > 0 ? AddAmmo(a1, give) : 0;
		if (got > 0 && a1->hud)
			SendByteMsg("AmmoPickup", a1->hud, got);
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

// A command on its way to the game DLL. 1: it was for a Lua weapon and is done. 2: it chooses one of the
// game's weapons while a Lua weapon is in hand; the game gets it, then LuaW_GameCommandDone. 0: not ours.
int LuaW_ClientCommand(edict_t *pl)
{
	lua_State *L = Lua_GetState();
	if (!L || pl != SV_LocalPlayer())
		return 0;
	const char *cmd = Cmd_Argv(0);
	// "give weapon_lua_x" in the console, under the same rule as the game's give (sv_cheats)
	if (Cmd_Argc() >= 2 && !stricmp(cmd, "give") && IsRegistered(L, Cmd_Argv(1))) {
		if (Cvar_VariableValue("sv_cheats") == 0) {
			Con_Printf("give needs sv_cheats 1\n");
			return 1;
		}
		int top = lua_gettop(L);
		LuaW_Give(L, pl, Cmd_Argv(1));
		lua_settop(L, top);
		return 1;
	}
	// the weapon menu sends the weapon's name; "use <name>" does the same from the console
	const char *cls = !stricmp(cmd, "use") && Cmd_Argc() >= 2 ? Cmd_Argv(1) : cmd;
	lweapon_t *w = FindWeapon(cls);
	if (w && w->owned) {
		if (pl->v.health > 0)
			Select(L, pl, (int)(w - weps));
		return 1;
	}
	if (!stricmp(cmd, "lastinv")) {
		if (previous >= 0 && weps[previous].owned) {
			Select(L, pl, previous);
			return 1;
		}
		if (active < 0)
			return 0;   // between two of the game's weapons: the game's business
		if (CallHolster(L, &weps[active], NULL)) {
			previous = active;
			Wake(pl, true);
		}
		return 1;
	}
	if (active >= 0 && GameWeaponId(pl, cls)) {
		if (!CallHolster(L, &weps[active], NULL))
			return 1;
		holster_asked = true;
		return 2;
	}
	return 0;
}

// after the game had a command that chose one of its weapons: if it switched to another, its messages
// already ended the Lua weapon's turn; if it was the sleeping weapon, the game did nothing and it is woken
void LuaW_GameCommandDone(edict_t *pl)
{
	holster_asked = false;
	if (active < 0)
		return;
	if (GameVmChanged(pl)) {
		GameSwitched(pl);
		return;
	}
	previous = active;
	weps[active].sent_clip = -2;
	Wake(pl, true);
}

// ply:StripWeapons() takes the Lua weapons too (the game's strip is only noticed when it had weapons to take)
void LuaW_StripPlayer(edict_t *pl)
{
	if (IsLocalPlayer(pl))
		StripAll(false);
}

void LuaW_PlayerDied(edict_t *pl)
{
	if (!IsLocalPlayer(pl))
		return;
	active = previous = -1;   // nothing to wake: the game put its weapon away itself
	held_buttons = 0;
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
			SendCurWeapon(w, i == active);
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
	active = previous = -1;
	held_buttons = 0;
	game_vm[0] = 0;
	resume = -1;
	game_cur_known = false;
	ResendAll();
}

// ---------------------------------------------------------------- saved games, level changes, lua_reload

#define WSAVE_MAGIC 0x3157454e   // "NEW1"

typedef struct { unsigned char *data; int size, cap; } wbuf_t;

static void Put(wbuf_t *b, const void *p, int n)
{
	if (b->size + n > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 256;
		while (b->cap < b->size + n)
			b->cap *= 2;
		b->data = Mem_Realloc(b->data, (size_t)b->cap);
	}
	if (n > 0)
		memcpy(b->data + b->size, p, (size_t)n);
	b->size += n;
}

static void PutInt(wbuf_t *b, int v) { Put(b, &v, 4); }
static void PutStr(wbuf_t *b, const char *s) { int n = (int)strlen(s); PutInt(b, n); Put(b, s, n); }

typedef struct { const unsigned char *p, *end; qboolean bad; } rbuf_t;

static int GetInt(rbuf_t *r)
{
	int v = 0;
	if (r->bad || r->end - r->p < 4) {
		r->bad = true;
		return 0;
	}
	memcpy(&v, r->p, 4);
	r->p += 4;
	return v;
}

// the next n bytes, or NULL
static const unsigned char *GetBytes(rbuf_t *r, int n)
{
	if (r->bad || n < 0 || r->end - r->p < n) {
		r->bad = true;
		return NULL;
	}
	const unsigned char *p = r->p;
	r->p += n;
	return p;
}

static void GetStr(rbuf_t *r, char *out, size_t size)
{
	int n = GetInt(r);
	const unsigned char *p = GetBytes(r, n);
	out[0] = 0;
	if (!p)
		return;
	size_t len = (size_t)n < size - 1 ? (size_t)n : size - 1;
	memcpy(out, p, len);
	out[len] = 0;
}

static unsigned char *pending;   // what the player gets back when the map that is starting is up
static int            pending_size;

// The player's Lua weapons and ammo as one block: for a saved game, a level change, lua_reload. NULL if
// there's nothing to keep. The caller frees it.
unsigned char *LuaW_SaveData(int *size)
{
	*size = 0;
	if (pending) {
		// asked while a map is starting (the autosave of a level change): what is on its way is the state
		unsigned char *copy = Mem_Alloc((size_t)pending_size);
		memcpy(copy, pending, (size_t)pending_size);
		*size = pending_size;
		return copy;
	}
	lua_State *L = Lua_GetState();
	int held = 0, counts = 0;
	for (int i = 0; i < weps_count; i++)
		held += weps[i].owned;
	for (int i = 0; i < ammo_count; i++)
		counts += ammo[i].count > 0;
	if (!L || (!held && !counts))
		return NULL;
	wbuf_t b = { NULL, 0, 0 };
	PutInt(&b, WSAVE_MAGIC);
	PutInt(&b, counts);
	for (int i = 0; i < ammo_count; i++)
		if (ammo[i].count > 0) {
			PutStr(&b, ammo[i].name);
			PutInt(&b, ammo[i].count);
		}
	PutInt(&b, held);
	for (int i = 0; i < weps_count; i++) {
		lweapon_t *w = &weps[i];
		if (!w->owned)
			continue;
		PutStr(&b, w->cls);
		PutInt(&b, w->clip1);
		PutInt(&b, w->clip2);
		int top = lua_gettop(L), tsize = 0;
		PushWeapon(L, w);
		unsigned char *t = LuaE_PackTable(L, -1, &tsize);
		lua_settop(L, top);
		PutInt(&b, tsize);
		Put(&b, t, tsize);
		Mem_Free(t);
	}
	int hand = active >= 0 ? active : resume;
	PutStr(&b, hand >= 0 && weps[hand].owned ? weps[hand].cls : "");
	*size = b.size;
	return b.data;
}

// copies the saved table's fields into the weapon's own table (Owner is set anew: the saved one is an
// edict number)
static void FillTable(lua_State *L, lweapon_t *w, const unsigned char *t, int tsize)
{
	int top = lua_gettop(L);
	PushWeapon(L, w);
	if (t && tsize > 0 && LuaE_UnpackTable(L, t, tsize)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			lua_rawset(L, top + 1);
		}
	}
	LuaG_PushEntity(L, SV_LocalPlayer());
	lua_setfield(L, top + 1, "Owner");
	lua_settop(L, top);
}

// Gives the block back. everything: the weapons, clips and ammo too (a saved game, a level change), then
// OnRestore. Otherwise only the tables of the weapons the player still holds (lua_reload), then OnReloaded.
static void Restore(const unsigned char *data, int size, qboolean everything)
{
	lua_State *L = Lua_GetState();
	if (!L || !data || size < 12)
		return;
	rbuf_t r = { data, data + size, false };
	if (GetInt(&r) != WSAVE_MAGIC)
		return;
	char name[128];
	int n = GetInt(&r);
	for (int i = 0; i < n && !r.bad; i++) {
		GetStr(&r, name, sizeof(name));
		int count = GetInt(&r);
		ammo_t *a = FindAmmo(name);
		if (everything && a && !r.bad) {
			a->count = count < 0 ? 0 : count > a->max ? a->max : count;
			a->sent = -1;
		} else if (everything && !r.bad) {
			Con_Warnf("[Lua] the saved ammo type %s isn't there any more; %d lost\n", name, count);
		}
	}
	n = GetInt(&r);
	for (int i = 0; i < n && !r.bad; i++) {
		GetStr(&r, name, sizeof(name));
		int clip1 = GetInt(&r), clip2 = GetInt(&r), tsize = GetInt(&r);
		const unsigned char *t = GetBytes(&r, tsize);
		if (r.bad)
			break;
		lweapon_t *w = FindWeapon(name);
		if (everything) {
			if (!IsRegistered(L, name)) {
				Con_Warnf("[Lua] the saved weapon %s isn't there any more\n", name);
				continue;
			}
			if (!w) {
				weps = Mem_Realloc(weps, sizeof(*weps) * (size_t)(weps_count + 1));
				w = &weps[weps_count++];
				memset(w, 0, sizeof(*w));
				w->cls = Mem_Strdup(name);
			}
			w->owned = true;
			w->listed = false;
			w->sent_clip = -2;
			w->popup = false;
			w->clip1 = clip1;
			w->clip2 = clip2;
			w->reload_end = 0;
		} else if (!w || !w->owned) {
			continue;
		}
		FillTable(L, w, t, tsize);
		CallWeapon(L, w, everything ? "OnRestore" : "OnReloaded");
	}
	GetStr(&r, name, sizeof(name));
	if (everything && !r.bad && name[0]) {
		lweapon_t *w = FindWeapon(name);
		if (w && w->owned)
			resume = (int)(w - weps);
	}
	if (r.bad)
		Con_Warnf("Lua weapons: saved data is damaged; some of it is lost\n");
}

// a saved game's block, for the map it is about to start
void LuaW_SetPending(const unsigned char *data, int size)
{
	Mem_Free(pending);
	pending = NULL;
	pending_size = 0;
	if (data && size > 0) {
		pending = Mem_Alloc((size_t)size);
		memcpy(pending, data, (size_t)size);
		pending_size = size;
	}
}

// A map is about to start. A level change takes the player's Lua weapons along as they are now; a loaded
// game brings its own (LuaW_SetPending); a new game starts without any.
void LuaW_MapChange(qboolean levelchange, qboolean loadgame)
{
	if (loadgame)
		return;
	LuaW_SetPending(NULL, 0);
	if (levelchange)
		pending = LuaW_SaveData(&pending_size);
}

// the player is in the new map
void LuaW_MapStarted(void)
{
	if (!pending)
		return;
	unsigned char *data = pending;
	int size = pending_size;
	pending = NULL;
	pending_size = 0;
	Restore(data, size, true);
	Mem_Free(data);
}

static unsigned char *reload_data;
static int            reload_size;

void LuaW_StateStopping(void)
{
	Mem_Free(reload_data);
	reload_data = LuaW_SaveData(&reload_size);
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
	// The weapon lying in the world is a scripted entity of the same class, so ents.Create(class) and a map
	// entity of that class make a pickup. A file in lua/entities of that name replaces it (they load later).
	lua_getglobal(L, "scripted_ents");
	if (lua_istable(L, -1) && lua_getfield(L, -1, "Register") == LUA_TFUNCTION) {
		lua_createtable(L, 0, 2);
		lua_pushstring(L, "lua_weapon_pickup");
		lua_setfield(L, -2, "Base");
		lua_pushstring(L, cls);
		lua_setfield(L, -2, "WeaponClass");
		lua_pushstring(L, cls);
		lua_call(L, 2, 0);
	}
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

// self:SendWeaponAnim(sequence number or name): the view model's animation, while the weapon is in hand
static int w_SendWeaponAnim(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	if ((int)(w - weps) != active)
		return 0;
	int seq;
	if (lua_type(L, 2) == LUA_TSTRING) {
		const model_t *m = w->viewmodel ? Mod_ForName(w->viewmodel, false) : NULL;
		seq = m ? R_StudioLookupSequence(m, lua_tostring(L, 2)) : -1;
		if (seq < 0)
			return luaL_error(L, "%s has no sequence named %s", w->viewmodel ? w->viewmodel : "the weapon (no view model)", lua_tostring(L, 2));
	} else {
		seq = (int)luaL_checkinteger(L, 2);
	}
	QueueWeaponAnim(seq, (int)luaL_optinteger(L, 3, 0));
	return 0;
}

// self:SetNextPrimaryFire(CurTime() + delay), as in GMod: CurTime is newen's clock, the weapon waits on the
// map's (which stops while paused), so only the delay is kept
static int w_SetNextPrimaryFire(lua_State *L)
{
	SelfWeapon(L, true)->next_primary = sv.time + (luaL_checknumber(L, 2) - host_realtime);
	return 0;
}

static int w_SetNextSecondaryFire(lua_State *L)
{
	SelfWeapon(L, true)->next_secondary = sv.time + (luaL_checknumber(L, 2) - host_realtime);
	return 0;
}

static int w_GetNextPrimaryFire(lua_State *L)
{
	lua_pushnumber(L, host_realtime + (SelfWeapon(L, true)->next_primary - sv.time));
	return 1;
}

static int w_GetNextSecondaryFire(lua_State *L)
{
	lua_pushnumber(L, host_realtime + (SelfWeapon(L, true)->next_secondary - sv.time));
	return 1;
}

// takes n from the clip, or from the player's ammo for a weapon without a clip
static void TakeAmmo(lua_State *L, lweapon_t *w, const char *sub, int *clip, int n)
{
	if (*clip >= 0) {
		*clip = *clip > n ? *clip - n : 0;
		return;
	}
	ammo_t *a = ClassAmmo(L, w->cls, sub);
	if (a)
		AddAmmo(a, -n);
}

static int w_TakePrimaryAmmo(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	TakeAmmo(L, w, "Primary", &w->clip1, (int)luaL_optinteger(L, 2, 1));
	return 0;
}

static int w_TakeSecondaryAmmo(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	TakeAmmo(L, w, "Secondary", &w->clip2, (int)luaL_optinteger(L, 2, 1));
	return 0;
}

static void OwnerSound(const char *name)
{
	edict_t *pl = SV_LocalPlayer();
	if (pl)
		S_StartSound(NUM_FOR_EDICT(pl), 1 /* CHAN_WEAPON */, name, NULL, 0.8f, 0.8f, 0, 100);
}

static int w_DefaultReload(lua_State *L);

// true if there's something to fire. If not: the empty click, a wait, and a reload when there's ammo for one.
static int CanAttack(lua_State *L, const char *sub, int clip, double *next)
{
	lweapon_t *w = SelfWeapon(L, true);
	ammo_t *a = ClassAmmo(L, w->cls, sub);
	qboolean ok = clip >= 0 ? clip > 0 : (!a || a->count > 0);
	if (!ok) {
		OwnerSound("weapons/357_cock1.wav");   // Half-Life's empty click
		*next = sv.time + 0.2;
		if (clip == 0 && a && a->count > 0 && !strcmp(sub, "Primary")) {
			lua_settop(L, 1);
			w_DefaultReload(L);
		}
	}
	lua_pushboolean(L, ok);
	return 1;
}

static int w_CanPrimaryAttack(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	return CanAttack(L, "Primary", w->clip1, &w->next_primary);
}

static int w_CanSecondaryAttack(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	return CanAttack(L, "Secondary", w->clip2, &w->next_secondary);
}

// self:DefaultReload(sequence number or name, seconds): starts a reload if the clip isn't full and there is
// ammo. The clip is filled when the animation (or the given time) is over. True if it started.
static int w_DefaultReload(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	ammo_t *a = ClassAmmo(L, w->cls, "Primary");
	int size = ClassInt(L, w->cls, "Primary", "ClipSize", -1);
	if ((int)(w - weps) != active || w->reload_end || size <= 0 || w->clip1 >= size || !a || a->count <= 0) {
		lua_pushboolean(L, false);
		return 1;
	}
	const model_t *m = w->viewmodel ? Mod_ForName(w->viewmodel, false) : NULL;
	int seq = ViewSequence(L, w, 2);
	if (seq < 0 && m)
		seq = R_StudioLookupSequence(m, "reload");
	float t = (float)luaL_optnumber(L, 3, 0);
	if (t <= 0)
		t = m && seq >= 0 ? R_StudioSequenceDuration(m, seq) : 0;
	if (t <= 0)
		t = 1.5f;
	if (seq >= 0)
		QueueWeaponAnim(seq, 0);
	w->reload_end = sv.time + t;
	w->next_primary = w->next_secondary = w->reload_end;
	lua_pushboolean(L, true);
	return 1;
}

static void NumField(lua_State *L, int t, const char *a, const char *b, float *out)
{
	if (lua_getfield(L, t, a) == LUA_TNUMBER || (lua_pop(L, 1), lua_getfield(L, t, b)) == LUA_TNUMBER)
		*out = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);
}

// self:ShootBullet{ damage = 8, num = 1, spread = 0.01, tracer = 0, distance = 8192 } (or GMod's order:
// damage, num, spread): bullets from the owner's eyes along their aim, as Half-Life's weapons fire them
static int w_ShootBullet(lua_State *L)
{
	lweapon_t *w = SelfWeapon(L, true);
	edict_t *pl = SV_LocalPlayer();
	(void)w;
	if (!pl)
		return 0;
	float damage = 8, num = 1, spread = 0, tracer = 0, distance = 8192;
	if (lua_istable(L, 2)) {
		NumField(L, 2, "damage", "Damage", &damage);
		NumField(L, 2, "num", "Num", &num);
		NumField(L, 2, "spread", "Spread", &spread);
		NumField(L, 2, "tracer", "Tracer", &tracer);
		NumField(L, 2, "distance", "Distance", &distance);
	} else {
		damage = (float)luaL_optnumber(L, 2, 8);
		num = (float)luaL_optnumber(L, 3, 1);
		spread = (float)luaL_optnumber(L, 4, 0);
	}
	svbullets_t b;
	memset(&b, 0, sizeof(b));
	b.attacker = pl;
	VectorAdd(pl->v.origin, pl->v.view_ofs, b.src);
	vec3_t ang;
	VectorAdd(pl->v.v_angle, pl->v.punchangle, ang);   // the kick of the last shots throws the aim off
	AngleVectors(ang, b.dir, NULL, NULL);
	b.spread_x = b.spread_y = spread;
	b.num = num < 1 ? 1 : num > 256 ? 256 : (int)num;
	b.damage = damage;
	b.distance = distance;
	b.tracer_every = (int)tracer;
	pl->v.effects |= EF_MUZZLEFLASH;   // the light of the shot
	gGlobals.time = (float)sv.time;
	SV_FireBullets(&b, NULL, NULL);
	return 0;
}

// self:EmitSound(name, [level], [pitch], [volume], [channel]): from the owner, on the weapon channel
static int w_EmitSound(lua_State *L)
{
	SelfWeapon(L, true);
	int n = lua_gettop(L);
	LuaG_PushEntity(L, SV_LocalPlayer());
	lua_getfield(L, -1, "EmitSound");
	lua_insert(L, -2);
	for (int i = 2; i <= 5; i++) {
		if (i <= n)
			lua_pushvalue(L, i);
		else
			lua_pushnil(L);
	}
	if (n >= 6)
		lua_pushvalue(L, 6);
	else
		lua_pushinteger(L, 1);   // CHAN_WEAPON
	lua_call(L, 6, 0);
	return 0;
}

// the Reload every weapon has unless it brings its own
static int w_Reload(lua_State *L)
{
	lua_settop(L, 1);
	return w_DefaultReload(L);
}

// ply:ViewPunch(Angle): kicks the view; it settles back by itself
static int p_ViewPunch(lua_State *L)
{
	edict_t *pl = LuaG_CheckEdict(L, 1);
	const float *a = LuaG_CheckVec(L, 2);
	VectorAdd(pl->v.punchangle, a, pl->v.punchangle);
	return 0;
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
	{ "SendWeaponAnim", w_SendWeaponAnim }, { "SetNextPrimaryFire", w_SetNextPrimaryFire },
	{ "SetNextSecondaryFire", w_SetNextSecondaryFire }, { "GetNextPrimaryFire", w_GetNextPrimaryFire },
	{ "GetNextSecondaryFire", w_GetNextSecondaryFire }, { "TakePrimaryAmmo", w_TakePrimaryAmmo },
	{ "TakeSecondaryAmmo", w_TakeSecondaryAmmo }, { "CanPrimaryAttack", w_CanPrimaryAttack },
	{ "CanSecondaryAttack", w_CanSecondaryAttack }, { "DefaultReload", w_DefaultReload },
	{ "ShootBullet", w_ShootBullet }, { "EmitSound", w_EmitSound }, { "Reload", w_Reload },
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

// ply:GetActiveWeapon(): the Lua weapon in hand, or nil while it's one of the game's
static int p_GetActiveWeapon(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	if (IsLocalPlayer(pl) && active >= 0)
		PushWeapon(L, &weps[active]);
	else
		lua_pushnil(L);
	return 1;
}

// ply:SelectWeapon(class): a Lua weapon, or one of the game's
static int p_SelectWeapon(lua_State *L)
{
	edict_t *pl = CheckPlayer(L);
	const char *cls = luaL_checkstring(L, 2);
	if (IsLocalPlayer(pl) && sv.active)
		Cbuf_AddText(va("%s\n", cls));   // the same road as the weapon menu's choice
	return 0;
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
			Con_Printf("  %-24s %s, number %d, slot %d place %d, clip %d\n", w->cls,
				i == active ? "in hand" : w->owned ? "held" : "not held",
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
	// lua_reload: the player keeps their weapons (clip and ammo are here), unless the class is gone
	for (int i = 0; i < weps_count; i++) {
		if (weps[i].owned && !IsRegistered(L, weps[i].cls)) {
			edict_t *pl = SV_LocalPlayer();
			if (active == i && pl)
				Wake(pl, true);
			if (previous == i)
				previous = -1;
			weps[i].owned = false;
		}
		weps[i].listed = false;
	}
	// what was in the weapons' own tables comes back
	Restore(reload_data, reload_size, false);
	Mem_Free(reload_data);
	reload_data = NULL;
	reload_size = 0;
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
		{ "StripWeapon", p_StripWeapon }, { "GetActiveWeapon", p_GetActiveWeapon }, { "SelectWeapon", p_SelectWeapon },
		{ "ViewPunch", p_ViewPunch }, { "GiveAmmo", p_GiveAmmo }, { "GetAmmoCount", p_GetAmmoCount },
		{ "SetAmmo", p_SetAmmo }, { "RemoveAmmo", p_RemoveAmmo }, { NULL, NULL }
	};
	LuaG_AddEntityMethods(L, methods);
}
