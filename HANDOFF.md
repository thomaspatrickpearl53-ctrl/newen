# newen - handoff notes

Everything someone new needs to pick this project up: what it is, what was decided and why, how the code is laid
out, how to build and test it, what works, what doesn't, and what comes next. Read `README.md` first for the
user-facing overview (commands, cvars, Lua API); this file is the developer side.

Status as of 2026-10-17: **Phase 1, steps 1 to 3 are done; step 4 is done except the Lua hooks.** The engine
boots, has a full console and engine-level Lua, renders Half-Life maps, models and sprites, and runs the stock Steam
`hl.dll`: it spawns every map's entities, monsters think, path-find and fight, doors and trains move, and the player
walks with the DLL's own movement code (doors open, triggers fire, weapons can be picked up). Next up is Lua at the
DLL boundary (4c), then the client DLL (step 5: HUD, view model, input, VGUI1).

---

## 1. What this project is

A from-scratch engine that runs **Half-Life (GoldSrc) content and game DLLs**, like Xash3D, with **Lua built into
the engine itself** so any GoldSrc game can be scripted. "newen" is a placeholder name (taken from the folder
`hl_newen`), and the owner can rename it at any time.

The owner already has a separate project, **Half-Life: Lua** (`%USERPROFILE%\hl-lua`, v0.10.0). It's a
Garry's Mod-style Lua 5.4 mod built inside the HL SDK's `hl.dll`/`client.dll`. newen is meant to eventually run
those DLLs unchanged and to offer the same Lua API at engine level, so hl-lua addons port over. Don't modify the
hl-lua folder from this project.

## 2. Decisions already made (don't relitigate)

| Decision | Why |
|---|---|
| **Run real Half-Life**: load the stock `hl.dll` and `client.dll` through the exact GoldSrc ABI | Gameplay lives in the DLLs; matching the ABI means HL and mods run without rewriting game code |
| **C (C11)**, MSVC, CMake + Ninja | Matches the GoldSrc ABI naturally |
| **32-bit only** for now | The Steam `hl.dll`/`client.dll` are 32-bit. CMake refuses a 64-bit build. 64-bit is a later goal (via a rebuilt open-source HLSDK) |
| **Windows first**, SDL2 for window/input | Linux/Steam Deck later; keep platform code in `sys_win.c`, `font_win.c` |
| **Clean-room**: written from documented formats; **never copy Xash3D code** | Xash3D is GPL; copying would force GPL on newen. Reading public format docs is fine |
| **Game interface headers are our own** (`game_api.h`), written from the public HL SDK layout, not copied from the SDK | Owner's choice (step 4). Keeps newen free of the SDK license; size asserts guard the layout |
| **Never write into the Half-Life folder** | All writes go to `bin/userdata/<game>/` |
| **Lua in the engine**, GMod-style API names (`hook.Add`, `concommand.Add`, `CreateConVar`, `surface.*`...) | Same names as hl-lua so addons move over |
| **No hardcoded limits** where avoidable: growable arrays; remaining limits become cvars | The owner wants the engine to "be able to do a lot" |
| **Original files load unchanged**; new capability comes through extensions (HD overrides, extended formats) | Compatibility first |
| Order: **Half-Life first**, then VGUI2, then other games (Cry of Fear last) | Owner's explicit choice |
| Physics: **Jolt** (MIT) recommended over Havok | Havok is proprietary and unavailable. The owner asked about Havok; Jolt was recommended but not formally confirmed |

### Features the owner asked for (not built yet)
- High-quality **WADs/textures** (bigger WAD textures plus true-color overrides). The override path is **done**; normal/specular maps aren't.
- High-quality **audio**: 44.1/48 kHz, 16/24-bit, OGG/FLAC/MP3, more channels, and **full stereo** (stereo music/UI; positional sounds downmixed or given width; proper panning).
- High-quality **models**: raised MDL limits, true-color model textures, later glTF/IQM.
- **VGUI1** (needed for stock HL client.dll) and later **VGUI2** (real GameUI main menu, CS 1.6, Cry of Fear), with Lua-controllable panels.
- **Big maps**: ±32768 first (BSP30 already allows it; our loader has no ±4096 limit), then an extended BSP format, maybe terrain and origin rebasing later.
- Other games: Opposing Force, Blue Shift, CS 1.6, Cry of Fear (needs VGUI2 + reverse engineering).

### Ideas proposed but not confirmed by the owner
Dynamic lights/shadows, bloom/HDR, Lua particle system, co-op campaign, closed captions, dev overlay, VS Code Lua
debugger, in-game entity editor, content mounting, voice chat, controller support, demo free-cam, Discord presence.
Ask before building these.

## 3. Working with the owner

- They like to **discuss first**. When they said "don't make anything until I say exactly 'go'", they meant it. Wait for the word.
- They told the assistant **not to look in their other folders** unless asked. Only `hl_newen` and the Steam Half-Life install are in scope. (They did ask once for a quick look at `hl-lua`.)
- They're not necessarily a programmer. Explain in plain terms and show screenshots of results.

## 4. Environment

| Thing | Where / version |
|---|---|
| Project | `%USERPROFILE%\hl_newen` (git repo since step 3; `.gitignore` excludes `build/` and `bin/`) |
| Half-Life | `C:\Program Files (x86)\Steam\steamapps\common\Half-Life` (found automatically, cached in `bin/basedir.txt`) |
| Compiler | Visual Studio Community 2026 (v18), MSVC 19.51, x86 toolset via `vcvars32.bat` |
| CMake / Ninja | Bundled with VS: `Common7\IDE\CommonExtensions\Microsoft\CMake\...` (CMake isn't on PATH) |
| GPU used for testing | NVIDIA RTX 3050, OpenGL 4.6 |
| Other tools on the machine | git, Python 3.14, ffmpeg (on PATH; used to convert TGA screenshots to PNG) |

The CMake "Visual Studio 18 2026" generator fails to find VS in this environment, so `build.bat` uses Ninja inside
a `vcvars32` shell. Use `build.bat`.

### Third-party code (in `third_party/`, vendored, downloaded 2026-10-17)
| Library | Version | Source | License |
|---|---|---|---|
| SDL2 | 2.30.9 (VC dev package, x86 libs used) | github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip | zlib |
| Lua | 5.4.7 | lua.org/ftp/lua-5.4.7.tar.gz (sha256 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30) | MIT |
| stb_image | 2.30 | raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h | public domain |

## 5. Build, run, test

```
build.bat                       -> bin\newen.exe (+ SDL2.dll, + base\ copied next to it)
bin\newen.exe +map c1a0
```

Command line: `-basedir <hl folder>`, `-game <mod>`, `-hd`, `-dev`, and `+command args` (executed after startup,
like GoldSrc; negative numbers count as arguments).

### How testing has been done (automated, no human needed)
Run the exe with a command script, let it quit itself, then read the log and look at screenshots:

```
bin\newen.exe +map c2a5 +setpos 1856 300 350 20 200 0 +wait 30 +screenshot +wait 2 +quit
```
- `wait N` waits N frames. `screenshot` saves the **end of the current frame**, so a `map` command later in the same
  frame shows up in it. Put `+wait 1` after `+screenshot` if more commands follow.
- Log: `bin/userdata/valve/console.log` (rewritten each run). Screenshots: `bin/userdata/valve/screenshots/shotNNNN.tga`.
  Convert with `ffmpeg -i shot0012.tga shot.png` to view.
- Useful test spots: `c1a0` spawn (484 318 -176, tram tunnel); `c2a5` at `1856 300 350 20 200 0` (canyon, barrels,
  grass) and `1000 -1200 300 15 120 0` (road); `c1a0d` spawn (lab).
- Movement: put commands in `bin/userdata/valve/test.cfg` and run `+map c1a0d +exec test.cfg` (`-forward` can't be
  given on the command line, it looks like an option). `setpos -2540 1168 -224 0 180 0`, `+forward`, `wait 40`,
  `-forward` walks into func_door 39, which slides open (check with `edicts func_door`, origin y becomes 58).
- Game DLL: `+map c2a5 +setpos 1679 150 100 0 90 0 +wait 400 +status` (soldiers find and kill the player in
  about 7 seconds; add `+god` to survive); `+map c0a0 +edicts func_tracktrain` twice, a few hundred frames apart
  (one tram moves). Running every campaign map for 400 frames gave no crashes (c1a1 through c5a1, t0a0).
- Models: `c1a0d` at `-2250 1136 -220 8 180 0` (two scientists by the soda machines); `+map c1a0d +mdlview gman walk`
  (a model at the spawn); `c2a5` at `1700 850 296 0 90 0` (an `env_glow`). `viewpos` prints `leaf 0` when the camera
  is inside a wall, which is easy to do with `setpos`.
- `mdlview` and `setpos` work in the same frame as `map` (`mdlview` reads `r_refdef.angles`, not the per-frame
  direction vectors).

Nothing has been played by hand yet (mouse look, WASD flying, console typing). Ask the owner to try it.

## 6. Code map

All engine code is in `src/engine/`, about 12,400 lines. One executable, no DLL split yet.

| File | Lines | What it does |
|---|---|---|
| `common.h` | 252 | Every shared declaration: memory, strings, cvar_t (GoldSrc layout), commands, console, keys (GoldSrc key numbers), filesystem, video, draw, Lua, host, renderer entry points |
| `common.c` | 181 | `Mem_*` (zeroed alloc, never NULL), `va()`, wildcard match, growable `strbuf_t`, `COM_Parse` tokenizer, command-line `COM_CheckParm` |
| `sys_win.c` | 126 | `main` (via SDL2main), timers (QPC), `Sys_Error` (message box + log), dirs/files, Steam path from the registry, dir listing |
| `host.c` | 192 | `Host_Init` order, the frame loop (fps cap, events, command buffer, camera, Lua, render, 2D), config writing (`newen_config.cfg`), `+commands` |
| `cmd.c` | 406 | Command buffer (`;`/newline split, `wait`), tokenizer (`//` comments), commands (sorted list, `CMD_LUA` flag), aliases, `exec`, `find`, `cmdlist`. `cmd_defer_cvars`: while the saved config runs, unknown `name value` lines become pending cvar values |
| `cvar.c` | 302 | Cvars in a hash plus the GoldSrc-style linked list; side table for default/description; pending values applied when a cvar is created later (so Lua cvars persist); `cvarlist`, `toggle`, `reset` |
| `console.c` | 473 | Scrollback ring buffer (`con_maxlines`), colors, notify lines, input line with cursor/history/Tab completion/Ctrl+V/Ctrl+L, slide animation, log file, `condump` |
| `keys.c` | 189 | Bindings (`bind`, `unbind`, `bindlist`); routing: `` ` `` -> console, console open -> console, then Lua `KeyEvent` hook, then bindings. `+cmd` bindings get the key number appended like GoldSrc |
| `filesystem.c` | 324 | Search paths (see the comment at the top), finding Half-Life, `FS_LoadFile`, `FS_ListFiles` (merged, sorted), `FS_OpenWrite` (write dir only), `path`, `dir` |
| `vid_sdl.c` | 304 | SDL window and GL 2.1 compatibility context, key/mouse mapping to GoldSrc key numbers, relative mouse for mouse look, vsync, `vid_restart`, `screenshot` (TGA) |
| `draw.c` / `font_win.c` | 106 / 77 | 2D: filled rects and console text. The font is rasterized from a Windows font with GDI into an alpha atlas (ASCII 32-126, monospace) |
| `lua_host.c` | 945 | The Lua VM (see section 7) |
| `mathlib.h` | 61 | vec3 macros, `AngleVectors` (Quake conventions) |
| `gl_local.h` | 77 | Renderer-internal: GL extension pointers (`qglActiveTexture`, buffers), texture API, WAD API, `refdef_t` |
| `image.c` | 215 | stb_image loading through the FS, GL texture list, filters (`r_texturemode`, `r_anisotropy`), texture gamma table, checkerboard for missing textures, GL extension loading |
| `wad.c` | 162 | WAD3 archives (loaded whole into memory), miptex lookup, miptex to RGBA (`{` transparency with color bleeding) |
| `bsp.h` / `bsp.c` | 129 / 827 | BSP v30 loader into `world_t`: planes, textures (HD override, then embedded, then WAD), `+` animation chains, faces and extents, lightmap atlas packing (1024² pages, as many as needed), nodes/leafs/parents, PVS decompression, submodels, entity lump, light styles, skybox, the world VBO. Detects Blue Shift's swapped lumps |
| `r_world.c` | 637 | Drawing: light style animation and lightmap re-upload of visible changed faces, PVS leaf marking, recursive node walk with frustum culling, surfaces sorted into (texture, lightmap page) batches drawn with `glDrawElements` from the VBO, two texture units (diffuse x lightmap), water (immediate mode, warped UVs, unlit), skybox (Quake 2 face orientation) plus sky faces written to depth only, brush entities from the entity lump with render modes and transparent sorting |
| `model.h` | 301 | Studio (`studiohdr_t` and friends) and sprite file structures written from the documented formats; `model_t` (cache entry), `rentity_t` (what the renderer draws, with GoldSrc's render modes), and the model/studio/sprite/light/entity entry points |
| `model.c` | 605 | Model cache (`Mod_ForName`; failures stay cached as `mod_bad` so they warn once; `Mod_ClearAll` on map change frees everything and its `TF_MODEL` textures). Studio loader: keeps the file in memory, range-checks every table, bone parents and vertex bones, loads `<name>T.mdl` textures and `<name>NN.mdl` sequence groups, decodes strips/fans into one triangle list per mesh, HD texture overrides. Sprite loader: palette per `texFormat`, single frames and groups. `mdlinfo`, `modellist` |
| `r_studio.c` | 635 | Bone setup (RLE anim values, quaternion slerp between frames, 2-way sequence blending, bone controllers and mouth, motion bone zeroed, pitch flipped like GoldSrc), CPU skinning, GoldSrc-style lighting (ambient + shade from `R_LightPoint`, light from above, lambert 1.5 wrap; flatshade, fullbright), chrome UVs from the view axes, masked and additive meshes, entity render modes |
| `r_sprite.c` | 165 | Sprite frames (groups by interval), the 5 orientation types, render modes, glows (constant screen size, hidden by `R_LineBlocked` with GoldSrc's 8-unit tolerance, no depth test) |
| `r_light.c` | 104 | `R_LightPoint` (trace down 2048 units through the BSP, read the lightmap sample with light styles, same gamma table as the lightmaps) and `R_LineBlocked` (segment vs solid leaves) |
| `r_ents.c` | 321 | The renderer's entity list: built from the entity lump (model keys plus a temporary classname-to-model preview table), frustum + PVS culling (`R_BoxVisible` in `r_world.c`), opaque first, then translucent far to near. `mdlview`, `r_entlist` |
| `game_api.h` | 481 | Clean-room GoldSrc game interface: `entvars_t`, `edict_t`, `globalvars_t`, `TraceResult`, `KeyValueData`, `enginefuncs_t` (159 entries in order, plus 32 reserved slots in case a newer DLL copies a bigger table), `DLL_FUNCTIONS`, `NEW_DLL_FUNCTIONS`, constants. Static asserts check the sizes (entvars 676, edict 804) |
| `server.h` | 118 | Server state (`sv`: edicts, precache lists, client info), `trace_t`/`hull_t`, entry points of the `sv_*.c` files |
| `sv_main.c` | 740 | Loads the game DLL (`liblist.gam` `gamedll`, `GiveFnptrsToDll` is stdcall, `GetEntityAPI2`/`GetEntityAPI`, optional `GetNewDLLFunctions`), string pool (`string_t` = offset from an empty `pStringBase`), edict alloc/free (0.5 s reuse delay), precache lists (model 1 = world, 2.. = `*n`), map spawn (class export, `KeyValue`, `Spawn`; "angle" becomes "angles"), `ServerActivate`, 2 settling frames, local client connect, the ghost player, `SV_SetupRenderEntities` (edicts to the renderer every frame), commands `edicts`, `status`, `god`, `notarget`, `tracetest`, unknown commands to `pfnClientCommand` |
| `sv_engfuncs.c` | 1307 | Every `enginefuncs_t` function. Real: precache, models, traces (line, hull, toss, model, texture), PVS (`FindClientInPVS`, `EntitiesInPVS`, fat PVS, `CheckVisibility`), entity search, private data, cvars (game cvars via `Cvar_RegisterExternal`), info strings, bone/attachment positions, CRC32, random, files, the game's own command buffer (`ServerCommand`/`ServerExecute`), `NameForFunction` via the DLL's export table. Stubs: sound, messages, events, deltas, voice, save/restore helpers |
| `sv_world.c` | 433 | Collision: hulls from the BSP clipnodes (hull 0 built from the render nodes in `bsp.c`), box hull, `SV_HullPointContents`, `RecursiveHullCheck`, rotated brush entities, `SV_Move` (world + every linked edict: owner, monsterclip, ignore-glass, `ShouldCollide`), linking (`pfnSetAbsBox`, touched leafs for PVS, brush-shaped trigger touches) |
| `sv_phys.c` | 707 | `SV_Physics`: `StartFrame`, then per movetype: push (move and rotate, blocking, riders), step (gravity, ground friction), toss/bounce/fly/missile, noclip, follow; think timing (`ltime` for pushers), `FL_KILLME` removal; monster movement: `SV_movestep` (step up/down 18), `SV_CheckBottom`, `SV_MoveToOrigin`, `SV_DropToFloor` |
| `sv_pmove.c` | 530 | Player movement: fills `playermove_t` (player state, movevars from the `sv_*` cvars, physinfo, physents = world + solid entities within ~256 units, moveents = water/ladder brushes), all the pm callbacks (player traces against physents with the pm hull numbering stand/duck/point/large mapped to BSP hulls 1/3/0/2, `PM_TestPlayerPosition`, point contents incl. content brushes, `PM_HullForBsp`, `PM_TraceModel` into GoldSrc's trace_t layout, `memfgets`, files), then per command `CmdStart`, `PlayerPreThink`, think, `PM_Move`, copy back, link with trigger touches, touch what the move hit, `PlayerPostThink`, `CmdEnd` |
| `cl_input.c` | 150 | **Temporary** (client.dll's job in GoldSrc): `+forward/+back/+moveleft/+moveright/+moveup/+movedown/+jump/+duck/+attack/+attack2/+use/+reload/+speed/+alt1/+showscores` and `impulse`, the `cl_*speed` cvars, `CL_CreateCmd` builds a `usercmd_t` each frame. `cl_viewangles` is the look direction (mouse) |
| `r_main.c` | 288 | View: Hor+ FOV, projection, Quake modelview, frustum planes, `R_CullBox`; the noclip camera; commands `map`, `maps`, `disconnect`, `viewpos`, `setpos`, `texlist` |

`base/lua/includes/init.lua` (231 lines) is the core Lua library (hook, timer, Color, draw helpers, string/table/math
utilities). `base/lua/autorun/newen_welcome.lua` is the demo (watermark plus `newen_hello`).

### Frame order (`Host_Frame`)
fps cap, then `Vid_PumpEvents`, `Cbuf_Execute`, `R_UpdateCamera`, `Lua_Frame` (Think), `Vid_BeginFrame` (clear),
`R_RenderView` (3D: world, brush entities, then `R_DrawEntities` for models and sprites), `Draw_Begin2D`, `Lua_DrawOverlay`, `Con_Draw`, FPS counter, `Vid_EndFrame` (screenshot,
swap).

### Startup order (`Host_Init`)
Cmd, Cvar, Con, Keys, FS (finds HL, opens the log), default binds, the saved config (with cvar deferral), then
`newen_autoexec.cfg`, Vid, Draw, R (GL extensions, images, world cvars), Lua, then `+commands`, and the console
opens.

## 7. Engine Lua (lua_host.c)

- One `lua_State` for the engine's lifetime; `lua_reload` rebuilds it. Lua concommands are removed on reload, but cvars stay.
- **Script tree** (merged, first match wins): `userdata/<game>/lua`, `userdata/<game>/addons/*/lua`,
  `base/addons/*/lua`, `base/lua`. It deliberately does **not** include the game folders, because `hl_lua/lua/` from
  the hl-lua mod would shadow newen's core library. `addon.json` isn't parsed yet; addons load alphabetically.
- Load order: `includes/init.lua`, then every `autorun/*.lua` (merged and sorted), then hook `Initialize`.
- Sandboxing: `io`, `dofile`, `loadfile`, `os.execute/remove/rename/exit/tmpname` and `package` paths are removed. `file.*` goes through the FS; writes only go to `userdata/<game>/data/`.
- **Timeouts**: an instruction-count hook checks a deadline (`lua_timeout`, default 5 s) per outermost call.
- **Errors**: tracebacks print in red. The same error repeating within 5 s is shown once (so a broken Think hook doesn't spam).
- Engine-to-Lua hooks: `Initialize`, `Think`, `DrawOverlay` (surface.* only works here), `KeyEvent(name, down, keynum)` (return true to eat the key), `WorldLoaded(map)`, `WorldUnloaded(map)`, `ShutDown`.
- Helpers for C code: `SafeCall`, `RunHook(name, nargs, nres)`, `Lua_RunHookString`.

## 8. Known issues and shortcuts (fix when convenient)

- **Brush entities are placed from the entity lump** (`R_BuildBrushEntities`) with a hardcoded list of hidden classes (`HiddenClass`). This is temporary until `hl.dll` drives entities in step 4.
- **Noclip camera reads WASD directly** (`Key_IsDown`), not through bindings, so it won't clash with `client.dll`'s `+forward` etc. Remove it, or keep it as a `noclip`-style free-cam, once the client DLL handles movement.
- **The `map` command only loads the world** for viewing. In step 4 it becomes "start a local server".
- **Brightness curves were tuned by eye** (`r_texgamma 0.8` exponent, `r_lightgamma 1.6`), not matched to GoldSrc's `texgamma`/`lightgamma`/`brightness`. Compare side by side with the real game later.
- Water is unlit, not subdivided (the warp is coarse on big polygons) and drawn in immediate mode. `r_wateralpha` defaults to 1.
- `kRenderTransColor` brush entities still draw textured (should be flat color).
- The sky depth pass loops over all surfaces every frame. Fine for now, but it could use the visible list.
- The console font is ASCII-only and Windows-only (GDI). It needs a cross-platform path and UTF-8 later (VGUI2 fonts will need real font rendering anyway).
- **Escape toggles the console** because there's no menu yet.
- `+command` arguments are only quoted when they contain spaces.
- The FPS counter reads 0 for the first half second.
- **Untested**: Blue Shift lump detection, maps beyond ±4096, the HD override seen on screen (the loader path *is* confirmed by the log: "1 HD texture replacement"), `-game` mods, and hand-driven mouse/keyboard play.
- `bin/` holds build output plus `userdata/`. Add `build/`, `bin/` to `.gitignore` when git is set up (keep `base/` and `third_party/` in git).
- The PDB/ILK in `bin/` come from the RelWithDebInfo build.
- **Server shortcuts (step 4)**: no sound, so
  `EmitSound` and friends do nothing; messages to the client are dropped (no HUD, no TextMsg); `ChangeLevel` only
  prints; no save/restore; no area tree (traces check every edict); rotating pushers don't carry riders; studio
  hitboxes aren't traced (bounding boxes only); `func_water`-style entity contents aren't in `SV_PointContents`;
  `MakeStatic` keeps studio models and sprites only. A few monsters report "stuck in wall" (headcrabs and a roach in
  c1a2, Big Momma in c4a2); compare with the real game before treating that as a bug.
- `GetHullBounds` from the game DLL is for player movement (its numbering is stand/duck/point/large) and in the HL
  SDK it doesn't even fill the arrays (it assigns to its own pointer parameters). The engine pre-fills the defaults
  before calling it, and it must never change the BSP clip hull sizes.
- **Player shortcuts (step 4b)**: no view model, HUD, crosshair or sound; weapons fire on the server but nothing shows
  it. The view has no bob, roll or smoothing (those are client.dll's `V_CalcRefdef`). Death restarts the map through
  `reload` (single player rules send it). `SetView` (camera entities) is ignored. Ducking under low ceilings, ladders
  and water use the DLL's code and the engine callbacks, but haven't been tested by hand yet.
- The movement keys are bound in `DefaultBindings` (host.c) every start: w a s d, space, ctrl, shift, e, r, mouse1/2,
  tab, v = noclip. They run before the saved config, so the owner's rebinds win.
- `ServerCommand` text goes to its own buffer, run by `ServerExecute`, after `GameInit`, before each spawn and every
  frame. Putting it in the console buffer made `ServerExecute` run the user's queued commands (even `quit`) in the
  middle of a map load.
- Without a game DLL (`-nogamedll` or none found), the old entity-lump path is used: see the next item.
- **Models and sprites from the entity lump are the fallback** (`R_BuildMapEntities`), like brush entities. The
  classname-to-model table in `r_ents.c` only exists so maps look populated; it goes away when `hl.dll` spawns things.
  Monsters stand in their idle sequence; `scripted_sequence` poses, dead bodies (`monster_*_dead`) and anything the
  DLL spawns later aren't shown. Non-looping sequences freeze on their last frame. `env_sprite` with a targetname and
  without "start on" is hidden, as hl.dll does.
- Studio models ignore the entity's `scale` (HL1 mostly does too). Only 2-way blending (`blending[0]`); no
  sequence-to-sequence transitions, gait sequences, attachments, hitboxes or animation events yet.
- Skinning and lighting run on the CPU: about 0.07 ms per model on the test machine. Move to GPU skinning if crowds
  get big.
- Model lighting is tuned by eye (`r_studio_ambient 0.6`, `r_studio_shade 0.5`), lit from straight above, no
  dynamic lights. Chrome uses the camera axes, not GoldSrc's per-bone chrome vectors.
- Sprites are drawn in immediate mode and aren't sorted against translucent brush entities (brush entities draw
  first).
- `models/doctor.mdl` in Steam HL is studio version 9 and is rejected (GoldSrc rejects it too). Every other model and
  sprite in `valve/` loads without warnings (checked by running `mdlinfo` on all of them).
- The HD model texture path is confirmed by the log ("scientist.mdl: 1 HD texture") but hasn't been seen on screen.

## 9. Next steps (Phase 1)

### Step 3: MDL models and sprites (done)
See the code map (`model.c`, `r_studio.c`, `r_sprite.c`, `r_light.c`, `r_ents.c`) and the known issues above. For
step 5, `R_StudioSetupBones` and `R_StudioDrawModel` are the pieces the client DLL's `StudioModelRenderer` will call
through `engine_studio_api_t`. `rentity_t` mirrors the drawing fields of GoldSrc's `entity_state_t` (studio `frame` is
a 0..255 cycle like `pev->frame`). Entities from `hl.dll` will replace `R_BuildMapEntities`, and `Mod_ForName` is
what `pfnPrecacheModel`/`pfnSetModel` will use.

### Step 4: server DLL (in progress)

**4a, done:** DLL loading, the full engine functions table, edicts and strings, map spawning, collision hulls and
traces, entity physics and thinking, PVS queries, rendering from edicts, a ghost player. See the code map.

**4b, done: player movement.** See `sv_pmove.c` and `cl_input.c` in the code map. The camera follows the player's
eyes (`view_ofs` + `punchangle`); `noclip` is the game's own noclip movetype; `fixangle` from the game sets the view.

**4c, next: Lua at the DLL boundary**, as planned below. `pfnUpdateClientData` and `pfnGetWeaponData` wait for
step 5, along with removing `cl_input.c` once client.dll registers its own `+forward` and friends.

The original plan for step 4:
- Load `valve/dlls/hl.dll` (path from `liblist.gam` `gamedll`), call `GiveFnptrsToDll(&enginefuncs, &globalvars)`, `GetEntityAPI2`/`GetEntityAPI` (`DLL_FUNCTIONS`), `GetNewDLLFunctions`.
- Implement `enginefuncs_t` (about 150 functions, in the exact order and with the exact signatures), `edict_t`/`entvars_t` with the exact layout, string pool (`pfnAllocString`, `globals->pStringBase` offsets), precache, `pfnSetModel`, traces (`pfnTraceLine`, `pfnTraceHull` against BSP clipnodes with the 4 hulls, plus entities), `pfnCVarRegister` for **external cvar_t structs** (cvar.c already keeps the GoldSrc layout and an `owned` flag), messages (`pfnMessageBegin`...), lightstyles (`pfnLightStyle` goes to `R_SetLightStyle`), `pfnServerCommand`, save/restore hooks.
- Physics: `SV_Physics` for movetypes (none, walk, step, fly, toss, push, noclip, bounce, follow, pushstep), touch/think, `pm_shared` player movement via `pfnPM_*`.
- **Headers**: decide with the owner how to get the ABI structs. Clean-room headers written from the public HL SDK's layout are the safest. The HL SDK's Valve license allows free non-commercial use for HL mods, and the hl-lua repo has a copy. Don't copy Xash3D headers.
- Lua: hook the engine-to-DLL boundary (spawn, think, touch, use, client connect/command, every frame), `ents.Create(classname)` (calls the DLL's exported classname function), entvars access, traces. This is where the Vector/Angle types arrive in Lua (match hl-lua's `lua_vector.cpp` API).

### Step 5: client DLL
`client.dll`: `F` export/`Initialize(cl_enginefunc_t*)`, `HUD_*` callbacks, `cl_enginefunc_t`, triangle API, event API, efx API, studio interface (model rendering moves to the client), demo API, net API stubs, **VGUI1** (`vgui.dll` from the HL folder, `VGui_*` interfaces), view model, HUD sprites (`hud.txt`), `+forward` and other movement commands registered by the client.

### Step 6: single player
Loopback client/server with GoldSrc's protocol structures (delta.lst), `changelevel` with landmarks and transitions,
save/load, sound (stereo and HQ formats, sentences, room DSP; probably SDL audio or miniaudio, decided at that step),
then port the hl-lua Lua API onto the engine.

### After Phase 1
Phase 2: Jolt physics, extended BSP format plus compiler changes, lights/shadows, particles, captions, dev tools.
Phase 3: VGUI2. Phase 4: Opposing Force, Blue Shift, CS 1.6, Cry of Fear. Later: co-op, terrain, 64-bit, Linux.
