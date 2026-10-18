# newen - handoff notes

Everything someone new needs to pick this project up: what it is, what was decided and why, how the code is laid
out, how to build and test it, what works, what doesn't, and what comes next. Read `README.md` first for the
user-facing overview (commands, cvars, Lua API); this file is the developer side.

Status as of 2026-10-17: **Phase 1, steps 1 to 4 are done; step 5 (client DLL) has 5a and 5b done; step 6 has
sound (6a), level changes (6b) and saved games (6c) done.**
The engine boots, has a full console and engine-level Lua, renders Half-Life maps, models and sprites, and runs the
stock Steam `hl.dll`: it spawns every map's entities, monsters think, path-find and fight, doors and trains move, and
the player walks with the DLL's own movement code. Engine Lua can see and change the game. The stock `client.dll` runs
too (with `vgui.dll` hosted by the engine): HUD, crosshair, input, view bob, view model, and since 5b the client's
effects: weapon events (muzzle flashes, shells, tracers, bullet holes), temp entities, particles, beams (egon, gauss,
lasers), explosions, decals, titles.txt messages, screen fades and shakes. Sound (6a): every game, client, event and
movement sound through our own mixer on SDL audio, sentences with lip movement, the CD music as MP3s, room echo. Level
changes (6b): the player and what's at the landmark (weapons, followers) go on to the next map, and a map visited before
comes back as it was left; the opening tram ride runs. Saved games (6c): save/load, F6/F9, an autosave at every level
change, and dying reloads the last save. The owner's reported bugs are fixed. 5c is done: dynamic lights, VGUI
mouse input, titles in Steam GoldSrc's font. That finishes Phase 1 as the owner picked it: 6d (copying hl-lua's
API) was dropped on 2026-10-18 (see section 1).

---

## 1. What this project is

A from-scratch engine that runs **Half-Life (GoldSrc) content and game DLLs**, like Xash3D, with **Lua built into
the engine itself** so any GoldSrc game can be scripted. "newen" is a placeholder name (taken from the folder
`hl_newen`), and the owner can rename it at any time.

The owner already has a separate project, **Half-Life: Lua** (`%USERPROFILE%\hl-lua`, v0.10.0). It's a
Garry's Mod-style Lua 5.4 mod built inside the HL SDK's `hl.dll`/`client.dll`. newen is meant to eventually run
those DLLs unchanged. The owner **doesn't need hl-lua addons to port over** (said 2026-10-18), so newen's own Lua
doesn't have to copy hl-lua's API; it grows its own way (GMod-style names are still the default). Don't modify the
hl-lua folder from this project.

## 2. Decisions already made (don't relitigate)

| Decision | Why |
|---|---|
| **Run real Half-Life**: load the stock `hl.dll` and `client.dll` through the exact GoldSrc ABI | Gameplay lives in the DLLs; matching the ABI means HL and mods run without rewriting game code |
| **C (C11)**, MSVC, CMake + Ninja | Matches the GoldSrc ABI naturally |
| **32-bit only** for now | The Steam `hl.dll`/`client.dll` are 32-bit. CMake refuses a 64-bit build. 64-bit is a later goal (via a rebuilt open-source HLSDK) |
| **Windows first**, SDL2 for window/input | Linux/Steam Deck later; keep platform code in `sys_win.c`, `font_win.c` |
| **Clean-room**: written from documented formats; **never copy Xash3D code** | Xash3D is GPL; copying would force GPL on newen. Reading public format docs is fine |
| **Game interface headers are our own** (`game_api.h`, `client_api.h`), written from the public HL SDK layout, not copied from the SDK | Owner's choice (step 4). Keeps newen free of the SDK license; size asserts guard the layout |
| **VGUI1 is hosted, not reimplemented**: the engine loads Half-Life's own `vgui.dll` and provides the drawing surface | The stock client.dll imports 548 vgui.dll functions and builds its viewport on an engine panel at `HUD_VidInit`. vgui.dll exports its vtables, so the layout is read from the binary (no SDK headers) |
| **Never write into the Half-Life folder** | All writes go to `bin/userdata/<game>/` |
| **Lua in the engine**, GMod-style API names (`hook.Add`, `concommand.Add`, `CreateConVar`, `surface.*`...) | Familiar GMod-style names. Matching hl-lua exactly isn't required any more (owner, 2026-10-18) |
| **No hardcoded limits** where avoidable: growable arrays; remaining limits become cvars | The owner wants the engine to "be able to do a lot" |
| **Original files load unchanged**; new capability comes through extensions (HD overrides, extended formats) | Compatibility first |
| Order: **Half-Life first**, then VGUI2, then other games (Cry of Fear last) | Owner's explicit choice |
| Sound: **SDL2 audio and our own mixer**, with dr_mp3/dr_flac/stb_vorbis decoders | Owner's choice (step 6). SDL2 is already there; our own mixer keeps full control of stereo, 3D placement and GoldSrc's room echo |
| **No client/server network protocol yet** (GoldSrc's delta.lst loopback) | Owner's choice (step 6): game and client share one process and call each other directly; the protocol only matters for multiplayer/co-op |
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
| stb_vorbis | 1.22 | raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c (sha256 4c7cb2ff...5e65636) | public domain / MIT |
| dr_mp3 | 0.7.4 | raw.githubusercontent.com/mackron/dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_mp3.h (sha256 997b7ee1...e0a2f) | public domain / MIT-0 |
| dr_flac | (same commit) | .../dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_flac.h (sha256 111144e7...cff945c) | public domain / MIT-0 |

The decoders are downloaded with the owner's OK (2026-10-17) into `third_party/stb` and `third_party/dr_libs`, and
compiled once in `snd_codecs.c` with warnings off.

## 5. Build, run, test

```
build.bat                       -> bin\newen.exe (+ SDL2.dll, + base\ copied next to it)
bin\newen.exe +map c1a0
```

Command line: `-basedir <hl folder>`, `-game <mod>`, `-hd`, `-dev`, and `+command args` (executed after startup,
like GoldSrc; negative numbers count as arguments).

### How testing has been done (automated, no human needed)
Run the exe with a command script, let it quit itself, then read the log and look at screenshots. If it crashes,
the log ends with the call stack, and a failed assert's text is in `bin/userdata/stderr.log`:

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

The owner has played it by hand since step 5a (walking, mouse look, the console, `impulse 101`) and reported:
monsters walk jerkily, monsters look too bright, and you can't get off ladders (see section 8). Player walking feels
right to them; don't "fix" it.

## 6. Code map

All engine code is in `src/engine/`, about 24,100 lines. One executable, no DLL split yet.

| File | Lines | What it does |
|---|---|---|
| `common.h` | 289 | Every shared declaration: memory, strings, cvar_t (GoldSrc layout), commands, console, keys (GoldSrc key numbers), filesystem, video, draw, Lua, host, renderer entry points |
| `common.c` | 181 | `Mem_*` (zeroed alloc, never NULL), `va()`, wildcard match, growable `strbuf_t`, `COM_Parse` tokenizer, command-line `COM_CheckParm` |
| `sys_win.c` | 216 | `main` (via SDL2main), timers (QPC), `Sys_Error` (message box + log), dirs/files, Steam path from the registry, dir listing. Crash reports: an unhandled exception, `abort()` or a pure virtual call writes the call stack (names from `newen.pdb`, module+offset for the game DLLs) to the console log; `Sys_LogException` does the same for crashes caught inside the client. The C runtime's messages (a failed `assert` in a game DLL) go to `bin/userdata/stderr.log` |
| `host.c` | 213 | `Host_Init` order, the frame loop (fps cap, events, command buffer, camera, Lua, render, 2D), config writing (`newen_config.cfg`), `+commands` |
| `cmd.c` | 406 | Command buffer (`;`/newline split, `wait`), tokenizer (`//` comments), commands (sorted list, `CMD_LUA` flag), aliases, `exec`, `find`, `cmdlist`. `cmd_defer_cvars`: while the saved config runs, unknown `name value` lines become pending cvar values |
| `cvar.c` | 334 | Cvars in a hash plus the GoldSrc-style linked list; side table for default/description; pending values applied when a cvar is created later (so Lua cvars persist); `cvarlist`, `toggle`, `reset` |
| `console.c` | 473 | Scrollback ring buffer (`con_maxlines`), colors, notify lines, input line with cursor/history/Tab completion/Ctrl+V/Ctrl+L, slide animation, log file, `condump` |
| `keys.c` | 221 | Bindings (`bind`, `unbind`, `bindlist`); routing: `` ` `` -> console, console open -> console, then Lua `KeyEvent` hook, then bindings. `+cmd` bindings get the key number appended like GoldSrc |
| `filesystem.c` | 324 | Search paths (see the comment at the top), finding Half-Life, `FS_LoadFile`, `FS_ListFiles` (merged, sorted), `FS_OpenWrite` (write dir only), `path`, `dir` |
| `vid_sdl.c` | 341 | SDL window and GL 2.1 compatibility context, key/mouse mapping to GoldSrc key numbers, relative mouse for mouse look (or a hidden, confined cursor for `m_rawinput 0`), window focus, vsync, `vid_restart`, `screenshot` (TGA) |
| `draw.c` / `font_win.c` | 106 / 155 | 2D: filled rects and console text. Fonts are rasterized from Windows fonts with GDI into alpha atlases (ASCII 32-126): monospace for the console, proportional for titles |
| `lua_host.c` | 950 | The Lua VM (see section 7) |
| `mathlib.h` | 61 | vec3 macros, `AngleVectors` (Quake conventions) |
| `gl_local.h` | 87 | Renderer-internal: GL extension pointers (`qglActiveTexture`, buffers), texture API, WAD API, `refdef_t` |
| `image.c` | 215 | stb_image loading through the FS, GL texture list, filters (`r_texturemode`, `r_anisotropy`), texture gamma table, checkerboard for missing textures, GL extension loading |
| `wad.c` | 204 | WAD3 archives (loaded whole into memory), miptex lookup, miptex to RGBA (`{` transparency with color bleeding); `Wad_Open`/`Wad_FindIn` keep one WAD apart from the map's list (decals.wad) |
| `bsp.h` / `bsp.c` | 151 / 864 | BSP v30 loader into `world_t`: planes, textures (HD override, then embedded, then WAD), `+` animation chains, faces and extents, lightmap atlas packing (1024² pages, as many as needed), nodes/leafs/parents, PVS decompression, submodels, entity lump, light styles, skybox, the world VBO. Detects Blue Shift's swapped lumps |
| `r_world.c` | 710 | Drawing: light style animation and lightmap re-upload of visible changed faces, PVS leaf marking, recursive node walk with frustum culling, surfaces sorted into (texture, lightmap page) batches drawn with `glDrawElements` from the VBO, two texture units (diffuse x lightmap), water (immediate mode, warped UVs, unlit), skybox (Quake 2 face orientation) plus sky faces written to depth only, brush entities from the entity lump with render modes and transparent sorting |
| `model.h` | 316 | Studio (`studiohdr_t` and friends) and sprite file structures written from the documented formats; `model_t` (cache entry; brush models keep the entity text at GoldSrc's offset 384, because the client's spectator code reads it there), `rentity_t` (what the renderer draws, with GoldSrc's render modes), and the model/studio/sprite/light/entity entry points |
| `model.c` | 615 | Model cache (`Mod_ForName`; failures stay cached as `mod_bad` so they warn once; `Mod_ClearAll` on map change frees everything and its `TF_MODEL` textures). Studio loader: keeps the file in memory, range-checks every table, bone parents and vertex bones, loads `<name>T.mdl` textures and `<name>NN.mdl` sequence groups, decodes strips/fans into one triangle list per mesh, HD texture overrides. Sprite loader: palette per `texFormat`, single frames and groups. `mdlinfo`, `modellist` |
| `r_studio.c` | 640 | Bone setup (RLE anim values, quaternion slerp between frames, 2-way sequence blending, bone controllers and mouth, motion bone zeroed, pitch flipped like GoldSrc), CPU skinning, GoldSrc-style lighting (ambient + shade from `R_LightPoint`, light from above, lambert 1.5 wrap; flatshade, fullbright), chrome UVs from the view axes, masked and additive meshes, entity render modes |
| `r_sprite.c` | 165 | Sprite frames (groups by interval), the 5 orientation types, render modes, glows (constant screen size, hidden by `R_LineBlocked` with GoldSrc's 8-unit tolerance, no depth test) |
| `r_light.c` | 104 | `R_LightPoint` (trace down 2048 units through the BSP, read the lightmap sample with light styles, same gamma table as the lightmaps) and `R_LineBlocked` (segment vs solid leaves) |
| `r_ents.c` | 343 | The renderer's entity list: built from the entity lump (model keys plus a temporary classname-to-model preview table), frustum + PVS culling (`R_BoxVisible` in `r_world.c`), opaque first, then translucent far to near. `mdlview`, `r_entlist` |
| `game_api.h` | 711 | Clean-room GoldSrc game interface: `entvars_t`, `edict_t`, `globalvars_t`, `TraceResult`, `KeyValueData`, `enginefuncs_t` (159 entries in order, plus 32 reserved slots in case a newer DLL copies a bigger table), `DLL_FUNCTIONS`, `NEW_DLL_FUNCTIONS`, constants. Static asserts check the sizes (entvars 676, edict 804) |
| `server.h` | 177 | Server state (`sv`: edicts, precache lists, client info), `trace_t`/`hull_t`, entry points of the `sv_*.c` files |
| `sv_main.c` | 940 | Loads the game DLL (`liblist.gam` `gamedll`, `GiveFnptrsToDll` is stdcall, `GetEntityAPI2`/`GetEntityAPI`, optional `GetNewDLLFunctions`), string pool (`string_t` = offset from an empty `pStringBase`), edict alloc/free (0.5 s reuse delay), precache lists (model 1 = world, 2.. = `*n`), map spawn (class export, `KeyValue`, `Spawn`; "angle" becomes "angles"), `ServerActivate`, 2 settling frames, local client connect, the ghost player, `SV_SetupRenderEntities` (edicts to the renderer every frame), commands `edicts`, `status`, `god`, `notarget`, `tracetest`, unknown commands to `pfnClientCommand` |
| `sv_engfuncs.c` | 1392 | Every `enginefuncs_t` function. Real: precache, models, traces (line, hull, toss, model, texture), PVS (`FindClientInPVS`, `EntitiesInPVS`, fat PVS, `CheckVisibility`), entity search, private data, cvars (game cvars via `Cvar_RegisterExternal`), info strings, bone/attachment positions, CRC32, random, files, the game's own command buffer (`ServerCommand`/`ServerExecute`), `NameForFunction` via the DLL's export table. Stubs: sound, messages, events, deltas, voice, save/restore helpers |
| `sv_world.c` | 433 | Collision: hulls from the BSP clipnodes (hull 0 built from the render nodes in `bsp.c`), box hull, `SV_HullPointContents`, `RecursiveHullCheck`, rotated brush entities, `SV_Move` (world + every linked edict: owner, monsterclip, ignore-glass, `ShouldCollide`), linking (`pfnSetAbsBox`, touched leafs for PVS, brush-shaped trigger touches) |
| `sv_phys.c` | 781 | `SV_Physics`: `StartFrame`, then per movetype: push (move and rotate, blocking, riders), step (gravity, ground friction), toss/bounce/fly/missile, noclip, follow; think timing (`ltime` for pushers), `FL_KILLME` removal; monster movement: `SV_movestep` (step up/down 18), `SV_CheckBottom`, `SV_MoveToOrigin`, `SV_DropToFloor` |
| `sv_pmove.c` | 764 | Player movement: fills `playermove_t` (player state, movevars from the `sv_*` cvars, physinfo, physents = world + solid entities within ~256 units, moveents = water/ladder brushes), all the pm callbacks (player traces against physents with the pm hull numbering stand/duck/point/large mapped to BSP hulls 1/3/0/2, `PM_TestPlayerPosition`, point contents incl. content brushes, `PM_HullForBsp`, `PM_TraceModel` into GoldSrc's trace_t layout, `memfgets`, files), then per command `CmdStart`, `PlayerPreThink`, think, `PM_Move`, copy back, link with trigger touches, touch what the move hit, `PlayerPostThink`, `CmdEnd` |
| `sv_save.c` | 687 | Level states: the game saves its entities into a `SAVERESTOREDATA` (data buffer that grows if the game fills it, token table, one `ENTITYTABLE` entry per edict); `pfnParmsChangeLevel` lists this map's connections and marks the entities in each transition. A level change keeps every visited map's state in memory; the next map restores its own state if it has one (without the player), then the old map's marked entities and the player, moved by the landmark offset. Globals (env_global, the tram) restore with `globalEntity` into the new map's copy. Saved games: `save/<name>.sav` in the game's userdata folder holds every level state (field-name tokens as strings), the game's global state (`pfnSaveGlobalState`) and the skill; loading replaces the remembered levels, runs `ResetGlobalState` + `RestoreGlobalState` and restores the current level with the player (`fUseLandmark` with no offset, so the player isn't sent to a spawn point). `save`, `load`, `autosave`, `saves`, `levelstates` |
| `cl_input.c` | 139 | **Temporary** (client.dll's job in GoldSrc): `+forward/+back/+moveleft/+moveright/+moveup/+movedown/+jump/+duck/+attack/+attack2/+use/+reload/+speed/+alt1/+showscores` and `impulse`, the `cl_*speed` cvars, `CL_CreateCmd` builds a `usercmd_t` each frame. `cl_viewangles` is the look direction (mouse) |
| `lua_local.h` | 17 | What `lua_host.c` shares with `lua_game.c`: `Lua_GetState`, `Lua_Call`, `Lua_Hook`, `LuaG_PushEntity` |
| `lua_game.c` | 1202 | The game API for Lua: `Vector`/`Angle` userdata (3 floats, operators, methods), `Entity` userdata (edict index + serial, so stale handles become invalid; methods plus direct entvars fields through a name/offset/type table), `ents`, `player`, `util.TraceLine/TraceHull`, constants, and the hooks the server fires |
| `client_api.h` | 710 | Clean-room client interface: `cl_enginefunc_t` (the SDK order, plus the later Steam/HL25 additions and 32 reserved slots), `cl_entity_t` (3000 bytes), `entity_state_t` (340), `ref_params_t`, `client_data_t`, `SCREENINFO`, `client_sprite_t`, the client export table. The tri/efx/event/demo/net/voice API tables are padded arrays of stubs for now |
| `cl_main.c` | 1265 | Loads client.dll (after vgui.dll), every `cl_enginefunc_t` function (HUD sprites with `hud.txt` parsing and normal/holes/additive drawing, FillRGBA (additive, like GoldSrc), text, cvars/commands, user message hooks, player info, entities for the client, `V_CalcRefdef` with `ref_params_t`, the view model, crosshair, center print), the user-message queue (game DLL messages are buffered in `sv_engfuncs.c` and dispatched at the start of the client frame; `svc_weaponanim` sets the view model animation, `svc_temp_entity` goes to `cl_tent.c`, unhooked ScreenFade/ScreenShake to `cl_view.c`), the API tables from the files below, `CL_CrashFilter` (with `CLIENT_CALL` in `cl_local.h`: a crash in the client logs its stack and turns the client off instead of killing the engine), `HUD_PlayerMoveInit` at the first map (the client's pm_shared needs its texture types before any event) |
| `vgui_bridge.c` | 441 | Hosts vgui.dll: constructs `App(true)`, a full-screen root `Panel` and a `SurfaceBase` with our own 35-slot vtable (thiscall via `__fastcall` + dummy EDX), OpenGL drawing for rects, textures and text (glyphs from vgui's own `Font::getCharRGBA`), paint/clip stack from `getAbsExtents/getClipRect/getInset`. App's `main` and `platTick` are stubbed in a copied vtable. VGUI1 alpha is transparency (0 = opaque). Mouse input: the client shows the cursor through `setCursor` (anything but `dc_none`) when a menu wants it; then `VGUI_WantsMouse` turns off mouse look and `vid_sdl.c` sends motion, buttons and the wheel to App's `internal*` methods instead of key bindings. vgui reads the cursor back through `GetMousePos`, so that returns the position last passed in |
| `cl_local.h` | 173 | What the client-side files share: client exports and time, `CLIENT_CALL`, TE_ numbers, the effect entry points |
| `cl_event.c` | 235 | Events: `PlaybackEvent` (game, pm_shared or client) builds `event_args_t` (players shoot along `v_angle`), queued with delay and `FEV_UPDATE`, fired by name to the client's hooks in `CL_Frame`. The event API: traces through `sv_pmove.c`'s client view (every solid entity), `EV_KillEvents` marks instead of removing (it's called from inside hooks). FEV_NOTHOST events are played too (no client-side prediction) |
| `cl_tent.c` | 1547 | Temp entities in fixed blocks (the client keeps pointers) on free/active lists; `HUD_TempEntUpdate` moves them and hands back visible ones. The whole efx API (models, sprites, muzzle flashes, sparks, blood, gibs, bubbles, projectiles, player attachments), `svc_temp_entity` parsing (all TE_ types), client events in model animations (5000+, fired by frame range with the last frame kept in `latched`), view model attachments every frame, `CL_GetEntityPoint` ("entity:attachment" packing) |
| `r_part.c` | 677 | Particles from the Quake palette (`gfx/palette.lmp`): dots and streaks, the Quake ramps for fire and explosions, every particle effect of the efx API |
| `r_beam.c` | 514 | Beams: points, entities, rings, disks/cylinders (radius grows with time), follow trails; noise or sine, scrolling sprite texture, additive. The game's CBeam entities (`FL_CUSTOMENTITY`) are drawn as beams each frame; widths are in tenths like the messages |
| `r_decal.c` | 390 | Decals: one name list shared by the game (`DecalIndex`), the client and messages; decals.wad textures (index = opacity, last palette colour = colour); a decal is clipped to every surface polygon it lands on (texture-aligned), kept per surface in the brush model's own space, drawn after its surface with the lightmap. `r_decals` limit (permanent map decals kept), `decalcount` |
| `cl_tri.c` | 244 | The triangle API (immediate-mode GL), WorldToScreen/ScreenToWorld from the view matrices captured each frame |
| `cl_view.c` | 304 | titles.txt (`$position`, `$color`, timing...), TE_TEXTMESSAGE through "__NETMESSAGE__" slots and the client's HudText hook, ScreenFade (GoldSrc's fade timing, modulate) and ScreenShake (`V_CalcShake`/`V_ApplyShake`) |
| `cl_studio.c` | 134 | The studio model API table given to the client at startup (`HUD_GetStudioModelInterface`). The engine still draws models; the table's queries are real (the egon asks `IsHardware`) and its drawing entries do nothing |
| `snd_local.h` | 40 | What the sound files share: `sfx_t` (16-bit samples at the file's rate, loop start), sentences |
| `snd_main.c` | 820 | Sound: channels (one sound per entity and channel, except auto and static), placement each frame (entities are followed; distance falloff scaled by attenuation over 1000 units, constant-power panning; the local player's own sounds are flat), mixing ahead `s_mixahead` seconds into SDL's queue with linear resampling (that's also the pitch), sentences word by word (pitch/volume/start/end, time compression skips the end of every 25 ms), mouths (voice loudness, 0..64), music (an MP3 streamed from memory; `cd` maps tracks to `media/*.mp3`), room echo presets for `room_type`, muffling under water, `s_record`, commands |
| `snd_mem.c` | 383 | Loading: WAV (8/16/24-bit, float, any rate; the "cue" chunk's first point is the loop start), OGG/FLAC/MP3; a `.flac`/`.ogg` next to a `.wav` replaces it (the loop point still comes from the WAV); sentences.txt (directory prefixes, standalone and attached `(p v s e t)` options, `,` and `.` as `_comma`/`_period` words). The game sends sentences by number (`!806`) |
| `snd_codecs.c` | 10 | The vendored decoders' implementations |
| `r_main.c` | 371 | View: Hor+ FOV, projection, Quake modelview, frustum planes, `R_CullBox`; the noclip camera; commands `map`, `maps`, `disconnect`, `viewpos`, `setpos`, `texlist` |

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
- Game hooks (step 4c, `lua_game.c`, fired from `sv_main.c`/`sv_pmove.c`): `InitPostEntity`, `Tick` (every server
  frame), `OnEntityCreated` (found by scanning edicts after each frame, because the game creates most entities
  without telling the engine; each slot remembers the serial it announced), `EntityRemoved` (from `ED_Free`),
  `PlayerSpawn`/`PlayerInitialSpawn`, `PlayerDeath` (health crossing 0), `PlayerCommand` (before unknown commands go
  to the game), `StartCommand` (the usercmd as a table, copied back), `EntityKeyValue` (while a map spawns).
- These hooks can fire while Lua is already running (Lua spawns an entity, the game frees another inside that
  call), so they save and restore the Lua stack top instead of clearing it. Keep it that way.
- `SERVER` and `CLIENT` are both true: one Lua state runs both sides. Concommand callbacks get the local player.
- The game's entity think/touch/use calls aren't hooked (they happen inside the DLL); only the engine-side
  boundary is. GMod-style damage hooks need either DLL cooperation or client-side work later.
- Helpers for C code: `SafeCall`, `RunHook(name, nargs, nres)`, `Lua_RunHookString`.

## 8. Known issues and shortcuts (fix when convenient)

- **Dynamic lights (5c).** `cl_tent.c` keeps the lights (the client's `CL_AllocDlight`, `TE_DLIGHT`, `TE_ELIGHT`,
  `EF_MUZZLEFLASH`/`EF_BRIGHTLIGHT`/`EF_DIMLIGHT`/`EF_LIGHT`, the view model's flash) and hands the live ones to
  `r_light.c` each frame. They are added into the lightmaps Quake-style (`R_PushDlights` marks world faces through the
  BSP tree; brush entities mark their own faces with the lights moved into their space, `R_DlightSpace`) and into model
  light (`R_DlightPoint`). The flashlight is `EF_DIMLIGHT` on the local player: a light where the view hits something,
  dimmer with distance. On surfaces that are already bright it is subtle, as in GoldSrc (lightmaps clamp).
- **Owner-reported bugs.** All fixed, the flashlight by the dynamic lights above. Earlier fixes:
  - Monsters walked jerkily: they only move when they think (about 10 times a second). `SV_SetupRenderEntities` now
    slides `MOVETYPE_STEP` entities from where they were drawn to the new place over the time between steps, turning
    them too (`StepLerp`; `r_steplerp 0` turns it off; jumps over 128 units and `EF_NOINTERP` aren't slid).
  - Monsters looked too bright: model light is now GoldSrc's split, a tenth from every side and the rest from above
    (`r_studio_ambient 0.1`, `r_studio_shade 0.9`), and the share goes through the light curve (`r_lightgamma`) before
    it multiplies the floor's brightness, so sides and undersides are darker.
  - You couldn't get off ladders: `PM_TraceModel` traced with the player's box, which starts inside the ladder and
    finds no surface; it uses the point hull now, as GoldSrc does.
  - The view model tilted the wrong way when looking up or down (`CL_BuildViewModel` flipped the pitch the client's
    V_CalcRefdef had already set).
  - Buttons, chargers and signs never switched to their "+a" textures (brush entities were drawn with frame 0;
    `R_AddBrushEntity` takes the entity's frame now).
- Without a game DLL, brush entities are placed from the entity lump (`R_BuildBrushEntities`) with a hardcoded list
  of hidden classes (`HiddenClass`).
- Without a game DLL, the free camera reads WASD directly (`Key_IsDown`), not through bindings. With one, `map`
  starts a local server and the player moves through the DLLs.
- **Brightness curves were tuned by eye** (`r_texgamma 0.8` exponent, `r_lightgamma 1.6`), not matched to GoldSrc's `texgamma`/`lightgamma`/`brightness`. Compare side by side with the real game later.
- Water is unlit, not subdivided (the warp is coarse on big polygons) and drawn in immediate mode. `r_wateralpha` defaults to 1.
- `kRenderTransColor` brush entities still draw textured (should be flat color).
- The sky depth pass loops over all surfaces every frame. Fine for now, but it could use the visible list.
- The console font is ASCII-only and Windows-only (GDI). It needs a cross-platform path and UTF-8 later (VGUI2 fonts will need real font rendering anyway).
- **Escape toggles the console** because there's no menu yet.
- `+command` arguments are only quoted when they contain spaces.
- The FPS counter reads 0 for the first half second.
- **Untested**: Blue Shift lump detection, maps beyond ±4096, the HD override seen on screen (the loader path *is* confirmed by the log: "1 HD texture replacement"), and `-game` mods.
- Files in `base/` are copied to `bin/base/` only when the exe links. After changing only Lua files, copy them by
  hand (or touch a C file) or the old scripts run.
- `bin/` holds build output plus `userdata/`; `.gitignore` excludes `build/` and `bin/` (`base/` and `third_party/` are in git).
- The PDB/ILK in `bin/` come from the RelWithDebInfo build.
- **Server shortcuts (step 4)**: messages to the client are queued for client.dll (step 5a); decals and the map's sounds aren't part of a level state; no area tree (traces check
  every edict); studio
  hitboxes aren't traced (bounding boxes only); `func_water`-style entity contents aren't in `SV_PointContents`;
  `MakeStatic` keeps studio models and sprites only. A few monsters report "stuck in wall" (headcrabs and a roach in
  c1a2, Big Momma in c4a2); compare with the real game before treating that as a bug.
- **Sound shortcuts (6a)**: no occlusion (walls don't muffle) and no Doppler; the room echo is a simple delay with
  presets per `room_type`, not GoldSrc's DSP; linear resampling (8-bit 11 kHz sounds get some high-frequency
  images); the CD track table (`cd_tracks` in `snd_main.c`) is from memory of the Steam release, check it against the
  game; sounds keep playing during the console. Test sound with `SDL_AUDIODRIVER=dummy` (nothing comes out of the
  speakers) and `s_record`, then look at the WAV (loudness over time, `ffmpeg ... showspectrumpic`).
- **Client shortcuts (after 5b)**: the client's studio renderer is never
  used (the engine draws models; HUD_GetStudioModelInterface only gets the API table); `cl_lw` is 0 (no client-side
  weapon prediction, so the server's weapon entities are shown, e.g. the egon's CBeam); spray logos aren't loaded; `PlayerInfo_SetValueForKey` and `SetMousePos`
  are no-ops; VGUI gets the mouse but not the keyboard (the client's
  menus take number keys through `HUD_Key_Event` anyway). The view model is drawn with depth range 0-0.3.
- **Macros evaluate their arguments more than once** (`VectorCopy` and friends in `mathlib.h`): never put `++` or a
  call with side effects inside them. `VectorCopy(a, out[count++])` in the decal clipper advanced by 3 and corrupted
  the heap on every bullet hole.
- **Tests and saved settings**: most cvars are archived, so a test script that sets one (`fps_max`, `r_decals`...)
  changes the owner's config. Test scripts must set everything back before `quit`.
- In developer mode, a cvar the client asks for that doesn't exist is printed: a missing engine cvar crashed the egon
  (`cl_lw`, read through a NULL pointer).
- The client's `CL_CreateMove` clears the usercmd, so the engine fills `msec` after calling it. `HUD_VidInit` runs
  before the server spawns the map, so InitHUD/ResetHUD from the player's spawn reach a ready HUD (the other order
  dropped them).
- `SV_DropToFloor` looks past entities when the trace only starts inside one: `impulse 101`/`Give` spawn items inside
  the player, and Quake's trace combining then hid the floor, so the suit "fell out of level" and the HUD stayed off.
- Mouse look goes through the client. With `m_rawinput 1` (default) HL25 reads `SDL_GetRelativeMouseState`, so the
  engine uses SDL relative mode. With `m_rawinput 0` the client reads the cursor and recentres it with
  `SetCursorPos` at `GetWindowCenterX/Y` (screen coordinates), so the engine only hides and confines the cursor.
  Both let go of the mouse when the window loses focus. `+mlook` and `+jlook` are run at startup when a client DLL is
  loaded, as Half-Life's default config does; without them the mouse's up/down walks you.
- `GetHullBounds` from the game DLL is for player movement (its numbering is stand/duck/point/large) and in the HL
  SDK it doesn't even fill the arrays (it assigns to its own pointer parameters). The engine pre-fills the defaults
  before calling it, and it must never change the BSP clip hull sizes.
- **Player shortcuts (step 4b)**: death restarts the map through
  `reload` (single player rules send it), which loads the last save (or autosave) of this game, or restarts the map
  if there is none. `SetView` (camera entities) is ignored. Ducking, ladders and water use the DLL's
  code and the engine callbacks; ladders are broken (see the owner-reported bugs above).
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

**4c, done: Lua at the DLL boundary.** See `lua_game.c` and section 7. Tested with a throwaway autorun script on
c1a0d: vector math, 206 map entities announced, traces (hit a func_door, `HitTexture` c1a0b_dr4b), a Lua-spawned
headcrab that dropped to the floor and attacked, `Give`, `PlayerCommand`, `EntityRemoved`, `ent.health = 50`.

`pfnUpdateClientData` and `pfnGetWeaponData` wait for step 5, along with removing `cl_input.c` once client.dll
registers its own `+forward` and friends.

The original plan for step 4:
- Load `valve/dlls/hl.dll` (path from `liblist.gam` `gamedll`), call `GiveFnptrsToDll(&enginefuncs, &globalvars)`, `GetEntityAPI2`/`GetEntityAPI` (`DLL_FUNCTIONS`), `GetNewDLLFunctions`.
- Implement `enginefuncs_t` (about 150 functions, in the exact order and with the exact signatures), `edict_t`/`entvars_t` with the exact layout, string pool (`pfnAllocString`, `globals->pStringBase` offsets), precache, `pfnSetModel`, traces (`pfnTraceLine`, `pfnTraceHull` against BSP clipnodes with the 4 hulls, plus entities), `pfnCVarRegister` for **external cvar_t structs** (cvar.c already keeps the GoldSrc layout and an `owned` flag), messages (`pfnMessageBegin`...), lightstyles (`pfnLightStyle` goes to `R_SetLightStyle`), `pfnServerCommand`, save/restore hooks.
- Physics: `SV_Physics` for movetypes (none, walk, step, fly, toss, push, noclip, bounce, follow, pushstep), touch/think, `pm_shared` player movement via `pfnPM_*`.
- **Headers**: decide with the owner how to get the ABI structs. Clean-room headers written from the public HL SDK's layout are the safest. The HL SDK's Valve license allows free non-commercial use for HL mods, and the hl-lua repo has a copy. Don't copy Xash3D headers.
- Lua: hook the engine-to-DLL boundary (spawn, think, touch, use, client connect/command, every frame), `ents.Create(classname)` (calls the DLL's exported classname function), entvars access, traces. This is where the Vector/Angle types arrive in Lua (match hl-lua's `lua_vector.cpp` API).

### Step 5: client DLL (in progress)

**5a, done:** client.dll + vgui.dll load, the HUD draws (health, armor, ammo, weapon menu and history, flashlight,
crosshair), `CL_CreateMove` drives movement and mouse look, `V_CalcRefdef` places the view and the view model,
user messages flow from the game DLL. See `cl_main.c`, `vgui_bridge.c`, `client_api.h`.

**5b, done:** events, the event/efx/triangle APIs, temp entities, particles, beams, decals, `svc_temp_entity`,
titles.txt, screen fade and shake, the studio API table (the engine keeps drawing models). See the code map. Tested on
c1a0d: the 9mm (muzzle flash, sparks, shells, bullet holes on a door), shotgun, egon and gauss beams from the gun
barrel, a grenade (explosion sprite, smoke, scorch decal), env_fade/env_shake spawned from Lua, and the c1a0 chapter
title. Fixed on the way: the client's pm_shared was never initialised (its assert fired on impact sounds), the
world model's entity text wasn't where the client reads it, the HUD's additive sprites ignored alpha (the crosshair
was a white box).

**5c, done:** dynamic lights (`cl_tent.c` keeps them, `r_light.c` adds them to lightmaps and model light), VGUI mouse
input (`vgui_bridge.c`), and the title font: the client's `DrawCharacter`/`GetScreenInfo` use a proportional
Trebuchet MS, 18 tall at 480 lines scaled with the screen, weight 600, drawn additively, like Steam GoldSrc's
"CreditsFont" (`cl_titlefont`; `Font_RasterizeProportional` in `font_win.c`).

The original plan for step 5:
`client.dll`: `F` export/`Initialize(cl_enginefunc_t*)`, `HUD_*` callbacks, `cl_enginefunc_t`, triangle API, event API, efx API, studio interface (model rendering moves to the client), demo API, net API stubs, **VGUI1** (`vgui.dll` from the HL folder, `VGui_*` interfaces), view model, HUD sprites (`hud.txt`), `+forward` and other movement commands registered by the client.

### Step 6: single player (in progress)

**6a, done: sound.** See `snd_*.c` in the code map. Tested with `s_record` on c1a0d and c1a0: item pickups, pistol
shots, `speak "vox/hello doctor freeman"`, footsteps, `cd play 2` (stereo MP3), a scientist's sentence (`!806`,
SC_POK0) louder in the ear he's on, and 8 channels of c1a0's opening.

**6b, done: level changes.** See `sv_save.c`. `PF_ChangeLevel` queues `changelevel2 <map> <landmark>` at the front of
the command buffer; `changelevel` saves the current level and loads the next with `SV_SpawnServer(map, oldmap,
landmark)`: no `ResetGlobalState`, the map's own saved state if visited, the transition, then `ServerActivate`, and the
restored player isn't put in the server again (`ClientPutInServer` would reset him). Tested: c1a0 to c1a0d and back by
walking into the triggers (weapons, health, armor and the 18 entities at the landmark carried; c1a0 restored as left),
and c0a0's tram ride into c0a0a. Found on the way:
- Pushers with `FL_ALWAYSTHINK` must think every frame: func_tracktrain steers 0.1 s ahead, and thinking every 0.5 s
  made the tram overshoot, reverse and spin.
- Rotating pushers carry riders now (rotated round the pusher's origin, turned with it, the player's view too), and a
  rider stuck after a push may stay where it was if that's free (Quake's rule), instead of blocking.
- `EntitiesInPVS` includes entities without a model and puts a follower (a held weapon) where its leader is; the game
  uses it to find what's at the landmark, and the player's weapons were left behind.
- Seen once: the first frame after the c0a0 → c0a0a change is a flat olive colour (a ScreenFade sent across the
  change?). One frame; check against the real game.

**6c, done: saved games.** See `sv_save.c`. The file format is newen's own (a GoldSrc .sav can't be loaded; `saves`
also lists any in the Half-Life folder's `valve/save`). Tested: save in c1a0d, move, load (same place, weapons,
armor), `kill` then fire reloads the last save, `+load test1` from the command line, and walking back to c1a0 after a
load finds it as it was left.

**Phase 1 is done** in the owner's order (the reported bugs, 5c). 6d, the hl-lua Lua API on the engine, was
dropped: the owner doesn't need hl-lua addons to run on newen. The GoldSrc network protocol is left for multiplayer.
Ask the owner what comes next.

### After Phase 1
Phase 2: Jolt physics, extended BSP format plus compiler changes, lights/shadows, particles, captions, dev tools.

**Phase 2 started with big maps (owner's choice, 2026-10-18): check +-32768, then plan the extended BSP format and
compiler changes with the owner before building.** The check, done: `tools/bspshift.py <in.bsp> <out.bsp> x y z`
moves a whole map (geometry, bounds, texture alignment so lightmaps stay valid, entity origins; maps with origin
brushes are refused). `c1a3b` moved by +-28000 (to +-31968, in `bin/userdata/valve/maps/far_pos|far_neg.bsp`)
plays the same as the original: walking, jumping, collision, a zombie's AI, bullets, decals, sparks, sound,
save/load. Found and fixed: temp entity coordinates were GoldSrc's 1/8-unit shorts (+-4096), so explosions and
other effects wrapped; they carry full floats now (engine-read only, `PF_WriteCoord`/`ReadCoord`). Limits that
are in the stock game code, not ours: `hl.dll`'s `IsInWorld` deletes grenades, satchels, snarks and gibs outside
+-4096; `client.dll` reads the damage direction (and spectator positions) as +-4096 shorts. Stock Half-Life
maps already pass 4096 (`lambda_bunker` reaches x 9000, `c2a2c` z -9000). BSP30 itself stops at +-32767 (node and
leaf bounds are shorts).
Phase 3: VGUI2. Phase 4: Opposing Force, Blue Shift, CS 1.6, Cry of Fear. Later: co-op, terrain, 64-bit, Linux.
