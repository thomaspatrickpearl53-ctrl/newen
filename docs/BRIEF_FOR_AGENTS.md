# Brief: building the Lua API, SWEP and physics plans

For any coding agent (ChatGPT/Codex or another) asked to build the three plans in `docs/`. Written
2026-11-12 at commit c9a8232 (release 0.2.0). Read this whole file before writing code.

## Paste this to start

> You're working on newen, a from-scratch GoldSrc-compatible Half-Life engine in C with Lua built in.
> Read `CLAUDE.md`, then `HANDOFF.md`, then `docs/BRIEF_FOR_AGENTS.md`, and follow the brief. Build the
> plans in `docs/LUA_API_PLAN.md`, `docs/SWEP_PLAN.md` and `docs/PHYSICS_PLAN.md`, one step at a time in the
> order the brief gives, each step on its own branch with its own check passing before the next starts.
> Stop and report after each plan's last step. Don't push, tag or publish anything.

## 1. The project in five lines

- C11, 32-bit only (Half-Life's `hl.dll`/`hl.so` and `client.dll`/`client.so` are 32-bit and newen loads them).
- Windows (Visual C++ x86 via `build.bat`, or MinGW i686 cross-built on Linux) and Linux (gcc `-m32` via
  `build.sh`). Every change works on both; platform code only in `sys_win.c`/`sys_linux.c`, `font_win.c`/`font_stb.c`.
- newen runs the installed Steam Half-Life's own game code. It never changes those DLLs and never writes into
  the Half-Life folder; everything it writes goes to `bin/userdata/<game>/`.
- `HANDOFF.md` is the map of the code (section "Code map") and the record of every decision. `README.md` is
  the user's reference. Both are updated by every step.
- The owner reads plans and reviews; they don't read code. Reports to them are in plain language.

## 2. Rules that can't bend

1. **Clean room.** Never copy, paraphrase or port Xash3D (GPL) or any other engine's code. Valve's public
   SDK headers (HL SDK, Source SDK) may be read **only** for structure layouts, field offsets and method
   order, then written with newen's own names, as `game_api.h` and `src_studio.c` do. No SDK file is pasted.
   Community format documentation (for `.phy`) is fine to read; write the reader yourself.
2. **Never write into the Half-Life or Half-Life 2 folders.** Read only.
3. **Style.** Tabs. `Mem_Alloc`/`Mem_Realloc`/`Mem_Free` (never raw `malloc`). Growable arrays, not fixed
   limits; a limit that must exist becomes a cvar. Short comments that say *why*. Match the file you're in.
4. **Both platforms in step.** No POSIX or Win32 calls outside the `sys_*` files. Check the new code builds
   with gcc `-m32`, MinGW i686, and (by reading) Visual C++: no GCC-only syntax, no VLAs, no `typeof`.
5. **Nothing from outside** except Jolt (decision P1 below): no new libraries, no package managers.
6. **Don't** push, tag, release, or change `ENGINE_VERSION`. Commit on your branch only.

## 3. Decisions (the plans' recommendations, accepted by the owner when they handed this over)

| Plan | Decision | Taken |
|---|---|---|
| Lua API | L1 | Call `hl.dll`'s C++ methods through its method table, slot numbers from the public HL SDK class layout, **checked by probing at every map start**; fall back to `env_explosion`/health damage if the probe fails |
| | L2, L3 | GMod names; no panel library |
| SWEP | S1 | Option A ("alongside"): the game's active weapon sleeps while newen holds back its input, view-model animations and `CurWeapon` messages |
| | S2, S3, S4 | Lua weapons have their own ammo types; scripted entities are in; GMod `SWEP`/`ENT` names |
| Physics | P1 | Jolt (MIT) from its GitHub releases into `third_party/JoltPhysics/` (only `Jolt/`, `Build/`, `LICENSE`, `README.md`). Record the exact version in HANDOFF's third-party table and `THIRD_PARTY_LICENSES.txt` |
| | P2, P3, P4 | First-version scope as the plan says; props solid to `hl.dll` as upright boxes; Half-Life-model ragdolls from hitboxes behind a cvar, off by default |

If the owner later changes one of these, they'll say so; until then don't reopen them.

## 4. Order of work

1. `docs/LUA_API_PLAN.md`, steps 1-7.
2. `docs/SWEP_PLAN.md`, step 5 only (scripted entities: props need them).
3. `docs/PHYSICS_PLAN.md`, steps 1-7.
4. `docs/SWEP_PLAN.md`, steps 1-4 and 6.

One branch per step, named `<plan>-<step>` (`luaapi-1`, `phys-3`...), started from the previous step's
branch. A step is done when the plan's **Check** column for it passes and you've written down how you
checked it (section 7).

## 5. Building and testing

**Build (Linux):** `./build.sh` (output `bin/newen`). If `build-linux/CMakeCache.txt` points at a cmake that
no longer exists, rerun `cmake -S . -B build-linux` once.

**Build (Windows, cross from Linux):**
```
cmake -S . -B build-mingw -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc -DCMAKE_RC_COMPILER=i686-w64-mingw32-windres \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DOUT_DIR=$PWD/build-mingw/out"
cmake --build build-mingw --parallel
```
Once C++ arrives (physics step 1), add `-DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++`. Run the Windows build
under Wine with `-basedir` and `-hl2dir` in quotes (the Half-Life 2 path has a space).

**Run headless:** `bin/newen +map c1a0 +wait 60 +screenshot +wait 1 +quit`, then read
`bin/userdata/valve/console.log` and convert `bin/userdata/valve/screenshots/*.tga` with ffmpeg to look at them.

Things that cost earlier work hours:

- **The game is paused after `+map` in headless runs** (the main menu counts as open). Anything that must
  move or think needs `+map x +wait 300 +gameui_hide +unpause` first. `status` prints the server time; if it
  doesn't advance, you're paused.
- `+attack` on the command line loses its `+`. Put `+attack` in a cfg in `bin/userdata/valve/` and `+exec` it.
- `base/` is copied to `bin/base/` only when the exe links. After adding a file under `base/`, build or copy it.
- Lua test scripts go in `bin/userdata/valve/lua/` and run with `+lua_openscript name.lua`. Delete them after.
- Test views used throughout HANDOFF: `c1a0d` at `setpos -2250 1136 -220 8 180 0` (two scientists),
  `c2a5` at `setpos 1856 300 350 20 200 0` (soldiers), `c1a1a` for zombies.
- Half-Life 2 content: `hl2:` prefix (`mdlview hl2:models/barney.mdl idle01`); NPC models swap with
  `mdl_replace models/zombie.mdl hl2:models/zombie/classic.mdl` before `map`.
- Memory checking: HANDOFF section 9 "Source models", step 9, has the AddressSanitizer build recipe. Run it on
  anything that parses files (`.phy`) or calls into `hl.dll`. It overwrites `bin/newen`; rebuild after.
- Before and after each step: `mdlinfo` over all of `valve/models` (225 of 226 load; `doctor.mdl` is refused
  by design) and the c1a0 / c2a5 / c1a0d screenshots must look the same as before your change, apart from
  what the step is meant to change.

## 6. Technical notes for these plans

- **Calling `hl.dll`'s C++ methods from C** (L1): the object is `edict->pvPrivateData`, its first word is the
  method table. Visual C++ passes `this` in ECX (`thiscall`); `vgui_bridge.c` and `vgui2_local.h` show the
  `__fastcall` + dummy-EDX trick that matches it from C. GCC on Linux passes `this` as the first stack
  argument (plain cdecl). **Correction checked during Lua API step 1:** `CBaseEntity` has no virtual
  destructor, so the required slots are identical on both platforms (Classify 8, TraceAttack 10,
  TakeDamage 11, Killed 13, IsPlayer 40, Use 46). VGUI's destructor-slot rule does not apply to it.
  Keep slot definitions together in `sv_gameobj.c`. MinGW-built newen still talks to
  Visual-C++-built `hl.dll` on Windows, so choose by the target (`_WIN32`), not the compiler.
- **Every call into `hl.dll`** goes through the crash guard: `CLIENT_CALL`-style on Visual C++ (`__try`),
  `SYS_GUARDED` on Linux; MinGW has no `__try` in 32-bit C (see `cl_local.h`), so there the call runs
  unguarded. Say so in HANDOFF for anything new.
- **Messages between the DLLs** all pass `PF_MessageEnd` (`sv_engfuncs.c`); `SV_UserMsgName` turns a message
  number into its name. The client's view of the player (weapon bits, view model) is built in `cl_main.c`.
- **Lua edicts** (scripted entities) have no `pvPrivateData`. Find every place newen calls `gEntityInterface`
  with an entity (think, touch, use, blocked, save, restore, `pfnKeyValue`...) and make it skip or reroute
  Lua edicts. `sv_save.c` already skips edicts without private data; Lua data needs its own chunk.
- **Source models** (for ragdolls): `src_studio.c` (loader, stand-in header, settings files),
  `src_anim.c` (bones, `SrcAnim_CalcPose`), `r_studio.c` (`SrcSetupBones` builds the bone matrices;
  ragdolls replace the pose there). Bone order: parents before children.
- **Jolt**: one C++ file (`phys_jolt.cpp`) behind `phys.h` with no C++ types in it. Inches outside,
  metres inside (×0.0254). `docs/SOURCE_PLAN.md` section 5 is an earlier design of the same bridge; keep the
  API general enough that HL2 maps can use it later.

## 7. When a step is done

- Commit on its branch, message in the project's style (short title, a body saying what and why), ending with
  the attribution line your tool requires.
- `HANDOFF.md`: update the code map row for every file you added or changed a lot, and add what you did,
  how you checked it, and what you did **not** check, under the plan's heading in section 9 (follow the
  "Source models" entries as the model).
- `README.md`: anything a user can now type or set.
- The plan file: mark the step built and note where reality differed from the plan.
- Report to the owner in plain words: what works now, what you saw in the screenshots, what isn't tested.
  Send the screenshot that shows it best.

## 8. When to stop and ask

- A plan step can't be done the way the plan says (for example: the `hl.dll` probe fails on the owner's
  installs, Jolt doesn't build for MinGW i686, a message trick breaks the HUD).
- Anything that would change how existing Half-Life play looks or behaves.
- Anything not covered here that needs a decision the owner hasn't made.

**Current stop (2026-11-12):** Lua API step 1 is built and checked. Step 2 needs a damage-bridge design
decision; see `LUA_API_PLAN.md`, section 10. `ApplyMultiDamage` is a global function, not a virtual method,
and neither installed game library exports it. Stock game damage also bypasses a Lua-only wrapper. Do not
silently reduce the damage hook's scope: the physics plan relies on stock bullets reaching Lua props.
