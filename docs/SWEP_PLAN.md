# Lua weapons (SWEPs) and scripted entities: plan

Design only, written 2026-11-12 from newen at commit c9a8232. Nothing here is built. Implementation starts
only when the owner says "go". It needs `docs/LUA_API_PLAN.md` first (damage, sound, effects), and is the
second of the three plans the owner asked for together.

## 1. In plain terms

A Lua file defines a weapon the way Garry's Mod does: a `SWEP` table with its name, HUD slot, models,
ammo and functions (`PrimaryAttack`, `Reload`, ...). The player can be given it, it appears in Half-Life's
own weapon menu next to the crowbar and the MP5, the HUD shows its ammo, it uses the view model in your
hands (Half-Life or Source), fires real bullets that hurt monsters, and survives saves and level changes.

The same machinery gives `ENT` tables: map-placeable or Lua-spawned objects whose thinking, touching and
using are Lua functions (a health station, a moving platform, a pickup for a Lua weapon).

## 2. Why it isn't simple

Half-Life splits every weapon in two:

| Part | Lives in | Does |
|---|---|---|
| The weapon object | `hl.dll` | Ammo, firing, damage, the active weapon, switching, picking up |
| The HUD weapon | `client.dll` | The slot menu, icons (`sprites/weapon_*.txt`), ammo counters, crosshair |

They talk through **messages** (`WeaponList` describes each weapon type, `CurWeapon` says which is active and
its clip, `AmmoX` sets an ammo count) and through the **weapon bits** (`pev->weapons`, which weapons the
player owns). Neither DLL can be changed. But newen sits between them: every message from `hl.dll` to
`client.dll` passes through `PF_MessageEnd`, every command from the client passes through the engine, and
the engine fills in the client's view of the player each frame. That's the lever: newen can add messages,
hold back messages, and claim commands.

## 3. How a Lua weapon works (recommended: "alongside", option A)

- **Registration**: `weapons.Register(SWEP, "weapon_lua_pipe")`; files in `lua/weapons/*.lua` register
  themselves at start.
- **A weapon number**: the client keeps 32 weapon bits; Half-Life uses 1-15 and 31 (the suit). A Lua weapon
  gets one of 16-30 the first time it's given, kept for the session, so **15 different Lua weapons can be
  owned at once** (any number can be registered).
- **The HUD**: newen sends the client its own `WeaponList` message for the weapon (name, slot, position,
  ammo type and maximum), sets its bit in the weapon bits it hands the client, and sends `CurWeapon`/`AmmoX`
  as Lua's clip and ammo change. Icons come from `sprites/weapon_lua_pipe.txt` if the addon ships one, else
  a generic icon newen ships in `base/`.
- **Choosing it**: the client's menu sends `weapon_lua_pipe`; newen claims that command before `hl.dll`
  sees it. The Half-Life weapon in hand **stays the active weapon inside `hl.dll`** but is put to sleep:
  - its fire, alt-fire and reload buttons are taken out of the input before `hl.dll` reads it;
  - its view-model animations and its `CurWeapon` messages are held back;
  - newen remembers its view model and HUD state, and puts them back if the player switches back to it
    (`hl.dll` doesn't redeploy a weapon that is already active, so newen has to).
  The Lua weapon's view model, world model and animations are set by the engine directly.
- **Leaving it**: choosing a Half-Life weapon passes the command on to `hl.dll` as usual. If `hl.dll` switches
  weapons on its own (picking up a better one), newen notices the view model change and calls `Holster`.
- **Firing**: Lua decides when (`self:SetNextPrimaryFire(CurTime() + 0.1)`), and `self:ShootBullet{num,
  spread, damage, force, tracer}` does what Half-Life's `FireBulletsPlayer` does: traces, `TraceAttack` on
  what it hits (blood, hit groups, through `docs/LUA_API_PLAN.md`'s damage), decals, sparks, tracers.
  `self:SendWeaponAnim(seq or activity name)`, `self:EmitSound`, `self:TakePrimaryAmmo(n)`,
  `self.Owner:ViewPunch(ang)`. Muzzle flashes come from the view model's own events, as today.
- **Ammo**: Lua weapons use ammo types of their own (`game.AddAmmoType{name, max}`), kept by newen, shown by
  the HUD through the same messages. Sharing Half-Life's ammo (a Lua pistol using 9mm) needs one more
  number from inside `hl.dll` (where the player's ammo counts are); listed as decision S2.
- **Saves and level changes**: newen writes the player's Lua weapons (class, clip, ammo, and the plain data
  in the weapon's table: numbers, strings, booleans, vectors, tables of those) into the save file and carries
  them across level changes, as `hl.dll` does for its own weapons. Dying or being stripped (`hl.dll` clears
  the weapon bits) clears them.

The other way (option B) is to go fully inside `hl.dll`: holster its weapon through its method table and
clear its "active weapon" field. Fewer tricks with messages, but two more numbers from inside the DLL that
can't be checked by a harmless probe. Recommended only if A shows a problem it can't solve.

## 4. Scripted entities (`ENT`)

- `scripted_ents.Register(ENT, "lua_healthcharger")`; files in `lua/entities/*.lua`.
- A Lua entity is a real edict **with a real, harmless game object behind it**: newen creates it as one of
  `hl.dll`'s plainest classes (`info_target`: no model, no thinking of its own) and gives it Lua's model,
  size and solidity. Traces hit it, `ents.*` find it, it can be touched, used and shot. The engine's entity
  loop (`sv_phys.c`) and touch/use dispatch send its think, touch and use to Lua instead of the game DLL.
  *Revised 2026-11-13:* the first version of this plan had Lua entities with no game object, but
  `hl.dll`'s bullet and explosion code assumes every entity it hits has one, and would most likely crash on
  the first shot. With a real object, the game's own weapons damage it through the object's ordinary
  health handling.
- **Damage to Lua entities**: there's no hook into the game's own damage (deferred, see
  `docs/LUA_API_PLAN.md` section 10). Each frame newen compares a Lua entity's `health` with the last frame
  and calls `OnTakeDamage(amount, attacker)` when it dropped (`dmg_inflictor` names the attacker); Lua's own
  damage calls `OnTakeDamage` directly.
- Functions: `Initialize`, `Think` (with `self:NextThink(t)`), `Touch(other)`, `Use(activator)`,
  `OnTakeDamage(info)`, `Draw` (optional, for custom drawing through `render.*`), `KeyValue(k, v)` so maps
  can place them: a map entity whose class is registered in Lua becomes a Lua entity at map load.
- Saved and restored like Lua weapons (plain data in the entity's table).
- A weapon lying on the floor is an `ENT` that gives its `SWEP` when touched, so SWEPs can be placed in maps.

## 5. Files

| File | Change |
|---|---|
| `src/engine/lua_weapons.c` (new) | `weapons.Register`, the inventory per player, weapon numbers, the HUD messages, claiming commands, sleeping the game's weapon, `ShootBullet`, saving |
| `src/engine/lua_ents.c` (new) | `scripted_ents.Register`, Lua edicts, think/touch/use dispatch, map placement, saving |
| `sv_engfuncs.c` | `PF_MessageEnd` asks the weapon code whether to hold a message back |
| `sv_main.c`, `sv_phys.c` | Commands claimed before `ClientCommand`; Lua edicts' think/touch/use go to Lua |
| `cl_main.c` | The weapon bits and view model the client sees; held-back weapon animations |
| `sv_save.c` | A "newen Lua data" chunk in save files and level states |
| `base/sprites/weapon_lua_generic.txt` (+ sprite) | The icon for Lua weapons without their own |
| `base/lua/weapons/`, `base/lua/entities/` | Two examples: a Lua pistol (Source view model or Half-Life's) and a health charger |
| `README.md` | SWEP and ENT reference |

## 6. Build order (each step: both platforms, a test script, log + screenshot)

| Step | Deliverable | Check |
|---|---|---|
| 1 | Registry and HUD: a Lua weapon given to the player shows in the slot menu with an icon and ammo | Screenshot of the weapon menu; `CurWeapon` and `WeaponList` logged |
| 2 | Choosing it and back: the Half-Life weapon sleeps and wakes, view models swap, the HUD follows | Switch crowbar > Lua > crowbar > MP5 > Lua; no stray fire or idle animation; screenshot each |
| 3 | Firing: `ShootBullet`, animations, sounds, ammo, reload | A Lua pistol kills a soldier on c2a5 (blood, death animation); ammo counts down on the HUD |
| 4 | Saves and level changes | Save with a Lua weapon held, load, change level: weapon, clip and its table's data all come back |
| 5 | Scripted entities: Lua edicts, think/touch/use, map placement, saving | A Lua health charger works by `+use`; a map entity of a Lua class spawns; save/load keeps its state |
| 6 | Weapon pickups as entities; the two examples polished; README | Owner review |

## 7. Limits after this plan

- 15 different Lua weapons owned at once (the client's weapon bits).
- No client-side prediction (`cl_lw` is off in newen anyway), so Lua weapons fire on the server's frame, as
  Half-Life's do in newen today.
- Lua weapons have their own ammo unless decision S2 says otherwise.
- Multiplayer: not considered (no network protocol yet).

## 8. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| S1 | How a Lua weapon coexists with `hl.dll`'s active weapon | A ("alongside": the game's weapon sleeps while newen holds back its input and messages). B only if A hits a wall |
| S2 | Lua weapons share Half-Life's ammo (9mm, buckshot...) | Not at first: their own ammo types. Sharing needs one more unprobeable number from `hl.dll` |
| S3 | Scripted entities in the same plan | Yes: weapon pickups need them, and the machinery is the same |
| S4 | GMod's `SWEP`/`ENT` field and function names | Yes, as far as Half-Life has the same idea, so GMod weapons port with small changes |
