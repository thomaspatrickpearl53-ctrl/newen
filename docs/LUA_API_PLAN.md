# Bigger Lua API: plan

Written 2026-11-12 from newen at commit c9a8232 (release 0.2.0). **All seven steps are built and checked** (2026-11-13):
step 1 by ChatGPT/Codex (`codex/luaapi-1`), step 2 with the scope the owner chose on 2026-11-13 (section 10)
and steps 3-7 on later branches. What wasn't built from section 2's table, and why, is in HANDOFF
(section 9, "Lua API plan: steps 5-7"). It is the first of three plans the owner asked for
together; `docs/SWEP_PLAN.md` builds on this one, and `docs/PHYSICS_PLAN.md` adds physics calls to it.

## 1. In plain terms

Lua can already find, create, move and change entities, trace lines, read the player's buttons, draw
rectangles and text, and run console commands (README, "Lua"). What it can't do is most of what makes a
script feel like part of the game: hurt things, make noise, make effects, draw in the world, or show
things that aren't entities. This plan adds those, in groups that each work on their own, with the same
GMod-style names as the rest of the API so addons move over easily.

## 2. What's there now, what's missing

| Area | Now | Added by this plan |
|---|---|---|
| Damage | Nothing (`PlayerHurt` only reports) | `ent:TakeDamage`, `ent:TraceAttack` (bullets with blood and hit groups), `util.BlastDamage`, an `EntityTakeDamage` hook |
| Sound | Nothing | `ent:EmitSound`, `sound.Play` at a position, `surface.PlaySound` (UI), `ent:StopSound`, sentence playback |
| Effects | Nothing | `effects.*`: sparks, blood, explosion, smoke, tracer, beam, dynamic light, decal, screen shake and fade, particles |
| Drawing in the world | Nothing | `render.*` inside new hooks `PostDrawOpaque` / `PostDrawTranslucent`: lines, boxes, spheres, sprites, models, `ToScreen` |
| Things that aren't entities | Nothing | `ClientsideModel(model)`: a model Lua places, animates and removes, drawn by the engine |
| Entities | Basics | `ent:SetSequence` with timing, `GetAttachment`, `GetBonePosition`, `SetBodygroup`, `SetPlaybackRate`, `SetNoDraw`, `GetKeyValue`, `Fire(input)` for map logic (`target`/`targetname` firing), `ents.FindInCone`, `ents.FindAlongRay` |
| Players | `Give`, aim, health | `StripWeapons`, `GiveAmmo`/`GetAmmo`, `SetMaxSpeed`, `ScreenShake`, `PrintMessage` (center text, chat line), `SetFOV`, `Freeze`, `GetActiveWeapon` (class name) |
| Networking | n/a | Not needed: game and client share one process (HANDOFF: no network protocol yet). A later `net.*` belongs to multiplayer |
| Timers, files, convars | Have them | `timer.Repeat`, `cvars.AddChangeCallback`, `file.Delete`, `file.CreateDir` (write folder only) |
| UI | Immediate rectangles and text | `surface.DrawTexturedRect` with `Material("path.png")`, `surface.SetFont`/`CreateFont`, `draw.DrawText` with alignment; a small `vgui`-like panel set is **not** in this plan (see 8) |
| Hooks | 20 | `EntityTakeDamage`, `OnNPCKilled`, `PlayerUse`, `KeyPress`/`KeyRelease`, `PostDrawOpaque`, `PostDrawTranslucent`, `HUDPaint` (alias of `DrawOverlay`), `CalcView` (change the camera), `PlayerFootstep`, `EntityEmitSound` (change or block a game sound) |

## 3. The hard one: damage

Half-Life's damage lives inside `hl.dll` as C++ methods (`TakeDamage`, `TraceAttack`) on the entity's
object. The engine has no entry for them. Three ways in:

| Way | How | For | Against |
|---|---|---|---|
| A. Call the game's methods | Find each entity's method table (the first word of `pvPrivateData`) and call `TakeDamage` / `TraceAttack` / `Killed` by their slot number, as server plugins for Half-Life have done for 20 years | Real damage: blood, hit groups, death animations, monsters react, the game's own rules (armor, god mode) | The required slots match on the tested Windows/Linux builds, but another game or rebuild could change them. Windows calls need the `thiscall` trick `vgui_bridge.c` already uses; the global multi-damage transaction still needs the section 10 bridge |
| B. Let map entities do it | Spawn a hidden `env_explosion` with its visual flags off for radius damage; set health directly for direct hits | No game internals | No bullets, no hit groups, no blood, setting health doesn't kill properly |
| C. Both | A when the slot numbers check out, B otherwise | Clean refusal when probe answers differ; fault guards on Linux and Visual C++ | MinGW calls are unguarded; two damage paths to test. A passing probe does not establish compatibility with every mod |

**Recommended: C.** The expected slot numbers are **checked by probing, not trusted**: at map start the engine calls
harmless methods with known answers (`IsPlayer` on the player is true and on the world false, `Classify`
on a scientist returns the "human passive" class) through the expected slots for this platform. If any
answer is wrong, the method path is switched off for the session with one console line, and damage falls
back to B. The numbers come from the public Half-Life SDK's class layout, the same rule as `game_api.h`
(decision L1). A new file keeps all of this in one place: `sv_gameobj.c` ("calls into the game DLL's
objects").

The same method table supplies `TraceAttack`, which the SWEP plan needs for real Half-Life bullets.
**Implementation finding:** `ApplyMultiDamage` is a global function, not a method-table entry; finishing
that damage transaction needs the additional bridge described in section 10.

## 4. Sound, effects, drawing

- **Sound** goes through `S_StartSound` (already used by the game) with Lua choosing the entity and channel.
  `EntityEmitSound` hooks the game's own `PF_EmitSound`, so a script can swap or silence any game sound.
- **Effects** reuse the code the client's temp-entity messages already drive (`r_part.c`, `r_beam.c`,
  `r_decal.c`, `cl_tent.c`): `effects.Sparks(pos, count)`, `effects.Blood(pos, dir, color)`,
  `effects.Explosion(pos, scale)`, `effects.Tracer(from, to)`, `effects.Beam{...}`, `effects.Light{pos,
  radius, color, time}`, `effects.Decal(name, trace)`, `util.ScreenShake`, `util.ScreenFade`.
  No new rendering code; every effect already exists for the game DLL.
- **Drawing in the world**: two new hooks run inside `R_RenderView`, after solid things and after see-through
  things, with the world's camera set up. `render.DrawLine`, `DrawWireframeBox`, `DrawBox`, `DrawSphere`,
  `DrawSprite(material, pos, size, color)`, `DrawModel(ClientsideModel)`, and `vector:ToScreen()` for
  name tags. Works in classic and modern.
- **Clientside models**: a Lua-owned `rentity_t` in `r_ents.c`'s list, with position, angles, sequence,
  body, skin, scale and colour from Lua, removed with the script or the map. Source models work too.

## 5. Safety rules (unchanged, extended)

- Everything still runs inside the sandbox: no `io`, no `os.execute`, files only under the write folder.
- Every new call checks its arguments and the entity's validity; a bad call is a Lua error, never a crash.
- Calls into `hl.dll` (damage) use the engine's crash guard on Linux and Visual C++; a caught fault turns
  the method path off and is reported. The 32-bit MinGW build has no structured exception handling, so
  calls there remain unguarded, as with `CLIENT_CALL`.
- Hooks that can change the game (`EntityTakeDamage`, `EntityEmitSound`) are timed by `lua_timeout` like
  every other hook.

## 6. Files

| File | Change |
|---|---|
| `src/engine/sv_gameobj.c` (new) | Method tables of the game's objects: slot numbers, the probe, `thiscall` calls for `TakeDamage`, `TraceAttack`, `Killed`, `Classify`, `IsPlayer`, `Use`; the fallback. Global multi-damage functions require the section 10 bridge |
| `src/engine/lua_game.c` | Damage, entity and player additions, `ents.FindInCone`, `Fire` |
| `src/engine/lua_fx.c` (new) | `sound.*`, `effects.*`, `util.ScreenShake/Fade` |
| `src/engine/lua_render.c` (new) | `render.*`, `ClientsideModel`, `Material`, the two draw hooks |
| `src/engine/lua_host.c` | `surface` textures and fonts, `timer.Repeat`, `cvars` callbacks, new hooks wired |
| `r_main.c`, `r_ents.c`, `snd_main.c`, `sv_engfuncs.c` | Hook call sites (draw hooks, `EntityEmitSound`, `PlayerUse`) |
| `README.md` | The API list, grouped |
| `base/lua/examples/` (new) | One small script per group, run by the tests (and useful to read) |

## 7. Build order (each step: build both platforms, a test script in `bin/userdata`, log + screenshot)

| Step | Deliverable | Check |
|---|---|---|
| 1 — built, 2026-11-12 | `sv_gameobj.c`: the probe only, reported by a `gameobj_info` command | Passed on Linux `hl.so` and Windows `hl.dll` (Wine); deliberately wrong slot refused on both. Model, scene and memory checks recorded in HANDOFF |
| 2 — built, 2026-11-13 | Damage, revised scope (section 10): hitbox traces in the engine, `ent:TakeDamage`, `ent:FireBullets`, `util.BlastDamage`, `DMG_*` names, the `env_explosion` fallback. No `EntityTakeDamage` hook (deferred) | Head shot from a script kills a scientist (×3 from the game's own skill cvar, blood on the wall behind, death fall); HL2 Kleiner gets a chest hit; probe forced off: `TakeDamage` refuses, `BlastDamage` kills through `env_explosion`; Windows build gives identical results under Wine; probe passes on all 125 maps |
| 3 — built, 2026-11-13 | Sound: `ent:EmitSound`, `ent:StopSound`, `sound.Play`, `surface.PlaySound`, `CHAN_*`, hook `EntityEmitSound` over the game's sounds **and client.dll's** (gunfire, swings) | Sentence `!SC_HELLO0` on a scientist, thunder at a position, a sound blocked by the hook, the weapon pickup sound swapped (name, pitch, level), the MP5's gunshots swapped for the shotgun's (Windows build under Wine, client.dll on) |
| 4 — built, 2026-11-13 | Effects: `effects.Explosion/Smoke/Sparks/Ricochet/Teleport/Tracer/Blood/Light/Beam/Sprite`, `util.Decal`, `util.ScreenShake`, `ply:ScreenFade` + `SCREENFADE`; built as the game's own temp-entity messages read by `CL_ParseTempEntity` | Screenshot of each from a script (client.dll on). Found and fixed on the way: newen's explosions had no bang or flash, also for the game's grenades |
| 5 — built, 2026-11-13 | Drawing in the world and clientside models | Screenshot: boxes round every monster, a floating Source model spinning, name tags via `ToScreen`; classic and modern |
| 6 — built, 2026-11-13 | Entity and player additions, timers, convars, files, UI textures and fonts | Each call exercised by the example scripts; `lua_reload` mid-map leaves nothing behind |
| 7 — done, 2026-11-13 | README and HANDOFF | Owner review |

## 8. Not in this plan

- A panel library (`vgui.Create`, buttons, text entries). Large on its own; worth a separate plan once the
  owner says what it's for (menus for SWEPs? a spawn menu?).
- `net.*` (client/server messages): nothing to send them over until multiplayer.
- Scripted entities (`ENT` tables): in `docs/SWEP_PLAN.md`, together with weapons, because both need the
  same "Lua thinks for this object" machinery.
- Physics calls: in `docs/PHYSICS_PLAN.md`.

## 9. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| L1 | Call `hl.dll`'s C++ methods by slot number, found from the public HL SDK layout and checked by probing | Yes, with the `env_explosion` fallback (option C in section 3). Without it there are no real bullets or damage, for Lua or for SWEPs |
| L2 | GMod names wherever GMod has the same thing | Yes, as so far |
| L3 | A panel library | Later, its own plan |

## 10. Implementation findings: damage-bridge decision needed

Step 1 passed on the installed Steam Linux `hl.so` and Windows `hl.dll` under Wine. The public
[CBaseEntity layout](https://github.com/ValveSoftware/halflife/blob/master/dlls/cbase.h) has no virtual
destructor: Classify 8 and IsPlayer 40 are the same on both platforms. These give the expected player,
world and scientist answers (1, 0, 3). Substituting Classify for IsPlayer deliberately produces 2 on the
player, and disables the method path without a crash. The remaining damage work is not built.

Three gaps prevent step 2 from delivering its stated check as written:

1. The public [weapons declarations](https://github.com/ValveSoftware/halflife/blob/master/dlls/weapons.h)
   put `ClearMultiDamage`, `AddMultiDamage`, `ApplyMultiDamage` and `gMultiDamage` outside the class.
   Neither installed library dynamically exports these names (checked with `readelf --dyn-syms --wide`
   and MinGW `objdump -p`). A virtual `TraceAttack` call alone does not provide a callable transaction
   boundary for finishing its accumulated damage. Do not guess a virtual slot or silently replace this
   with an ordinary `TakeDamage` call and claim complete bullet behavior.
2. A hook around Lua's own damage calls cannot change stock weapon or NPC damage: those calls stay
   inside the game library. The current `PlayerHurt` message hook reports damage afterward. A general
   `EntityTakeDamage` hook needs additional interception. The physics plan depends on this for stock
   bullets hitting Lua props, whose proposed edicts have no game object.
3. Current entity traces use bounding boxes and leave hit groups unset. A real aimed headshot check
   needs studio-hitbox tracing. Supplying an artificial hit group only tests the method call.

**Recommended decision:** retain the intended feature scope and add a damage-bridge investigation before
step 2. Establish and validate a cross-platform way to finish trace-damage transactions, forward stock
damage to Lua (including scripted entities), and identify studio hit groups. Write down any required
binary-address discovery or method interception and its compatibility/failure rules for owner review
before implementing it.

**Alternative:** deliver Lua-origin direct and blast damage first, explicitly defer complete bullets and
stock-damage interception, and revise the dependent SWEP and physics checks to match the reduced scope.

This is the stop required by the brief's section 8: a step cannot be built as the plan describes.

### Decision (owner, 2026-11-13) and what was built

The owner chose the recommended way through (proposed in review of the stop above):

1. **Hitbox traces in the engine** (`sv_world.c`, `HitboxTrace`), as GoldSrc does: a line that hits a
   monster's or player's box is tested against the model's posed hitboxes; the closest gives `hitgroup`,
   `endpos` and the normal. A line that passes between hitboxes still hits the box (hit group 0), so what can
   be hit is unchanged. This also gives Half-Life's own weapons their hit groups: before, newen never set
   one, so head shots did body damage. Cvar `sv_hitboxes` (archived, default 1; 0 = boxes only, the old
   behaviour). Works for Source models through their stand-in header's hitboxes.
2. **Bullets without `TraceAttack`/`ApplyMultiDamage`** (`sv_damage.c`): the engine traces, scales by hit
   group from the game's own `sk_<monster|player>_<part><skill>` cvars (Half-Life's defaults if missing),
   draws blood (colour from the game's `BloodColor`, slot 14) and decals, then calls the checked
   `TakeDamage` (slot 11). Armor, god mode, pain, death and gibs stay the game's.
3. **No general `EntityTakeDamage` hook** for the game's own damage: deferred. The SWEP and physics plans
   were revised so Lua entities always have a real game object behind them (see `docs/SWEP_PLAN.md` section 4).

Probe change: `BloodColor` is checked too (player red, world none), which anchors `TakeDamage`'s slot between
two checked ones. It is checked on the player, not the scientist: a temporary unspawned scientist (maps
without one, such as c1a1a) has no blood colour yet, which failed the probe in the first version.
