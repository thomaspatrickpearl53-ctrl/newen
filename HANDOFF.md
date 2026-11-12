# newen - handoff notes

Everything someone new needs to pick this project up: what it is, what was decided and why, how the code is laid
out, how to build and test it, what works, what doesn't, and what comes next. Read `README.md` first for the
user-facing overview (commands, cvars, Lua API); this file is the developer side.

2026-11-01: **Half-Life's own main menu** (owner: "exactly how Half-Life has it"): newen hosts the game's
`gameui.so`/`GameUI.dll` on Half-Life's `vgui2`, as GoldSrc does, so the menu, its dialogs and the loading screen
are Valve's own. This pulls part of Phase 3 (VGUI2) forward. Built and tested on Linux; the Windows side is written
but not built yet (see "GameUI" in section 8). Code: `gameui.c`, `vgui2_surface.c`, `vgui2_fs.c`.

2026-10-30: **Linux build** (owner's request): `build.sh`, `sys_linux.c`, `font_stb.c`; runs the Linux Steam
Half-Life's `hl.so`, `client.so` and `vgui.so`. See "Linux" in section 5.

2026-10-29: **the modern renderer's steps 0-7 are built on branch `modern-renderer`** (section 9, "Renderer plan");
the plan is finished; what comes next is the owner's call.

Status as of 2026-10-22: **Phase 1 is done. Phase 2 started with big maps: the ±32768 check, the NBSP format,
the compiler and a TrenchBroom configuration are done, and so are closed captions (section 9). Designs for an
optional modern renderer (`docs/RENDERER_PLAN.md`) and for Half-Life 2 maps (`docs/SOURCE_PLAN.md`) are written;
neither is built. The state before the rest of Phase 2 is tagged `pre-phase2-v1`.**
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
| **C (C11)**, MSVC on Windows, gcc `-m32` on Linux, CMake + Ninja | Matches the GoldSrc ABI naturally; the Linux game libraries are GCC-built, so GCC gives the same struct layout |
| **32-bit only** for now | The Steam `hl.dll`/`client.dll` are 32-bit. CMake refuses a 64-bit build. 64-bit is a later goal (via a rebuilt open-source HLSDK) |
| **Windows and Linux** (Linux since 2026-10-30), SDL2 for window/input | Keep platform code in `sys_win.c`/`sys_linux.c` and `font_win.c`/`font_stb.c`; other files use `Sys_*` (libraries, files, crash guard) |
| **Clean-room**: written from documented formats; **never copy Xash3D code** | Xash3D is GPL; copying would force GPL on newen. Reading public format docs is fine |
| **Game interface headers are our own** (`game_api.h`, `client_api.h`), written from the public HL SDK layout, not copied from the SDK | Owner's choice (step 4). Keeps newen free of the SDK license; size asserts guard the layout |
| **GameUI (the main menu) is hosted on Half-Life's own vgui2**, not redrawn | Owner's choice (2026-11-01): the menu must be exactly Half-Life's. vgui2 and GameUI only need the engine's interfaces (surface, file system, panels, a few services), which are written from the layouts in Half-Life's Linux binaries (they carry debug info) |
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
- **Linux and Windows are updated together** (owner, 2026-11-01): every change is written for both in the same step.
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
| Linux machine | CachyOS (Arch), gcc with `-m32`, Steam's Linux Half-Life in `~/.local/share/Steam/steamapps/common/Half-Life`, same RTX 3050 (NVIDIA 615). Needs `cmake`, `ninja` (optional; make works) and `lib32-sdl2-compat` |

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
| stb_truetype | 1.26 | raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_truetype.h (sha256 ecd30b05...6b5090aab), downloaded 2026-10-30 for the Linux fonts | public domain / MIT |
| dr_flac | (same commit) | .../dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_flac.h (sha256 111144e7...cff945c) | public domain / MIT-0 |

The decoders are downloaded with the owner's OK (2026-10-17) into `third_party/stb` and `third_party/dr_libs`, and
compiled once in `snd_codecs.c` with warnings off.

## 5. Build, run, test

```
build.bat                       -> bin\newen.exe (+ SDL2.dll, + base\ copied next to it)
bin\newen.exe +map c1a0
```

### Linux
```
./build.sh                      -> bin/newen (+ base/ copied next to it); build folder build-linux/
bin/newen +map c1a0
```
`build.sh` passes `-m32`; extra arguments go to CMake's first configure (`-DSDL2_LIBRARY=<any 32-bit
libSDL2-2.0.so.0>` works without `lib32-sdl2-compat`, e.g. the one in Steam's `ubuntu12_32/steam-runtime`; set
`CMAKE=` to use a cmake that isn't on PATH). Testing works the same way as on Windows (paths with `/`). What differs:
- Libraries: `liblist.gam`'s `gamedll_linux` (`dlls/hl.so`), `cl_dlls/client.so`, `vgui.so` from the Half-Life
  folder, through `Sys_LoadLibrary` (dlopen, **`RTLD_LOCAL`**, like Windows DLLs). Never make them global:
  client.so has empty stubs of 53 game functions (`cl_dll/hl/hl_baseentity.cpp`, e.g.
  `CBaseMonster::MonsterInitThink`), and a global client.so made hl.so's own calls bind to them, so monsters
  animated but never thought or moved (found 2026-10-30). hl.so uses the C++ runtime without listing it as a
  dependency (Valve's engine links it), so the first load opens `libstdc++.so.6` globally. `client.so` lists `vgui.so` as a dependency by bare
  name, and vgui.so has no SONAME, so the loader won't match the copy the engine loaded. `FS_Init` calls
  `Sys_AddLibraryPath`, which restarts the process once (before the window opens) with the Half-Life folder
  appended to `LD_LIBRARY_PATH`, as Steam's `hl.sh` does. `NEWEN_RESTARTED` stops a loop.
- `GiveFnptrsToDll` is cdecl (`GIVEFNPTRS_CALL` in `game_api.h`).
- VGUI: `vgui.so`'s vtables have the same slots as `vgui.dll`'s (checked slot by slot: App 55, SurfaceBase 35; no
  virtual destructors or overloaded virtuals). `vgui_bridge.c` only swaps the mangled names (`SYM()`) and the
  calling convention (`THISCALL`/`VTHIS`: GCC passes `this` as the first stack argument). The vtable copies include
  the two words before slot 0 (offset-to-top, type info) in case vgui uses RTTI on them.
- Client crash guard: `CLIENT_CALL` uses `sigsetjmp`; `sys_linux.c`'s signal handler jumps back to the innermost
  guard (`sys_crash_guard`). Tested with a deliberate NULL into `client.so`: logged, client off, engine kept going.
- Crash reports: signal handler, `backtrace()`, names from `dladdr` (`-rdynamic` exports the engine's non-static
  functions; static ones show as `newen+0x...`, use `addr2line -e bin/newen`). stderr isn't redirected to a file.
- Files: `FS_FullPath` turns backslashes into slashes, and if no search path has the exact name, `Sys_FixCase`
  looks for one that differs only in case (Half-Life's files were made on Windows).
- Fonts: `font_stb.c` asks fontconfig (`fc-match`) for the Windows face name and rasterizes with stb_truetype;
  Consolas becomes Noto Sans Mono / DejaVu Sans Mono, Trebuchet MS becomes Noto Sans.
- Save files store the game's mangled function names, so Windows and Linux saves don't load on the other system.

Command line: `-basedir <hl folder>`, `-game <mod>`, `-hd`, `-dev`, and `+command args` (executed after startup,
like GoldSrc; negative numbers count as arguments).

### How testing has been done (automated, no human needed)

Menu tests (GameUI): `gameui_press <panel name>` clicks a control (`gameui_panels` lists the visible ones with
their names; main menu items are `#GameUI_GameMenu_NewGame` and so on, dialog buttons have their `.res` names such
as `PlayGame`, `Load`, `Save`, `OKButton`), `gameui_key <key>` presses a key, `gameui_click x y` clicks a point. The
menu lays itself out over its first frames, so wait ~150 frames after opening it. **Commands GameUI sends
(`map`, `save`, `load`...) are appended to the command buffer, after anything still queued** (`wait`, `screenshot`,
`quit`), so take screenshots after them with a real-time Lua hook instead, e.g. a script in
`bin/userdata/valve/lua/` run with `+lua_openscript`. `lua_run` code on the command line must not contain spaces
inside one argument (the argument gets quoted and becomes a string) nor a lone `-` or `+` (they start a new
option/command); zsh doesn't split unquoted variables, so drive it from bash.

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

All engine code is in `src/engine/`, about 24,800 lines. One executable, no DLL split yet.

| File | Lines | What it does |
|---|---|---|
| `common.h` | 300 | Every shared declaration: memory, strings, cvar_t (GoldSrc layout), commands, console, keys (GoldSrc key numbers), filesystem, video, draw, Lua, host, renderer entry points |
| `common.c` | 181 | `Mem_*` (zeroed alloc, never NULL), `va()`, wildcard match, growable `strbuf_t`, `COM_Parse` tokenizer, command-line `COM_CheckParm` |
| `sys_win.c` | 216 | `main` (via SDL2main), timers (QPC), `Sys_Error` (message box + log), dirs/files, Steam path from the registry, dir listing. Crash reports: an unhandled exception, `abort()` or a pure virtual call writes the call stack (names from `newen.pdb`, module+offset for the game DLLs) to the console log; `Sys_LogException` does the same for crashes caught inside the client. The C runtime's messages (a failed `assert` in a game DLL) go to `bin/userdata/stderr.log` |
| `sys_linux.c` | 321 | The Linux `sys_win.c`: `main`, `clock_gettime` timers, `Sys_Error` (SDL message box + log), dirs/files, Steam folder (native, `~/.steam`, Flatpak), `Sys_FixCase`, dlopen wrappers, `Sys_AddLibraryPath` (restart with `LD_LIBRARY_PATH`), `Sys_ExportName` (dladdr), signal handler for crash reports and the client crash guard |
| `host.c` | 213 | `Host_Init` order, the frame loop (fps cap, events, command buffer, camera, Lua, render, 2D), config writing (`newen_config.cfg`), `+commands` |
| `cmd.c` | 406 | Command buffer (`;`/newline split, `wait`), tokenizer (`//` comments), commands (sorted list, `CMD_LUA` flag), aliases, `exec`, `find`, `cmdlist`. `cmd_defer_cvars`: while the saved config runs, unknown `name value` lines become pending cvar values |
| `cvar.c` | 334 | Cvars in a hash plus the GoldSrc-style linked list; side table for default/description; pending values applied when a cvar is created later (so Lua cvars persist); `cvarlist`, `toggle`, `reset` |
| `console.c` | 473 | Scrollback ring buffer (`con_maxlines`), colors, notify lines, input line with cursor/history/Tab completion/Ctrl+V/Ctrl+L, slide animation, log file, `condump` |
| `keys.c` | 221 | Bindings (`bind`, `unbind`, `bindlist`); routing: `` ` `` -> console, console open -> console, then Lua `KeyEvent` hook, then bindings. `+cmd` bindings get the key number appended like GoldSrc |
| `filesystem.c` | 324 | Search paths (see the comment at the top), finding Half-Life, `FS_LoadFile`, `FS_ListFiles` (merged, sorted), `FS_OpenWrite` (write dir only), `path`, `dir` |
| `vid_sdl.c` | 351 | SDL window and GL 2.1 compatibility context, key/mouse mapping to GoldSrc key numbers, relative mouse for mouse look (or a hidden, confined cursor for `m_rawinput 0`), window focus, vsync, `vid_restart`, `screenshot` (TGA) |
| `draw.c` / `font_win.c` / `font_stb.c` | 106 / 155 / 150 | 2D: filled rects and console text. Fonts are rasterized into alpha atlases (ASCII 32-126): monospace for the console, proportional for titles. Windows: GDI. Linux: fontconfig picks the file, stb_truetype draws it |
| `lua_host.c` | 950 | The Lua VM (see section 7) |
| `mathlib.h` | 61 | vec3 macros, `AngleVectors` (Quake conventions) |
| `gl_local.h` | 87 | Renderer-internal: GL extension pointers (`qglActiveTexture`, buffers), texture API, WAD API, `refdef_t` |
| `image.c` | 215 | stb_image loading through the FS, GL texture list, filters (`r_texturemode`, `r_anisotropy`), texture gamma table, checkerboard for missing textures, GL extension loading |
| `wad.c` | 204 | WAD3 archives (loaded whole into memory), miptex lookup, miptex to RGBA (`{` transparency with color bleeding); `Wad_Open`/`Wad_FindIn` keep one WAD apart from the map's list (decals.wad) |
| `bsp.h` / `bsp.c` | 162 / 1023 | BSP v30 and NBSP loader into `world_t` (NBSP lumps are read wide; BSP30 lumps are widened into scratch copies, see `docs/NBSP.md`): planes, textures (HD override, then embedded, then WAD), `+` animation chains, faces and extents, lightmap atlas packing (1024² pages, as many as needed), nodes/leafs/parents, PVS decompression, submodels, entity lump, light styles, skybox, the world VBO. Detects Blue Shift's swapped lumps |
| `r_world.c` | 716 | Drawing: light style animation and lightmap re-upload of visible changed faces, PVS leaf marking, recursive node walk with frustum culling, surfaces sorted into (texture, lightmap page) batches drawn with `glDrawElements` from the VBO, two texture units (diffuse x lightmap), water (immediate mode, warped UVs, unlit), skybox (Quake 2 face orientation) plus sky faces written to depth only, brush entities from the entity lump with render modes and transparent sorting |
| `model.h` | 317 | Studio (`studiohdr_t` and friends) and sprite file structures written from the documented formats; `model_t` (cache entry; brush models keep the entity text at GoldSrc's offset 384, because the client's spectator code reads it there), `rentity_t` (what the renderer draws, with GoldSrc's render modes), and the model/studio/sprite/light/entity entry points |
| `model.c` | 615 | Model cache (`Mod_ForName`; failures stay cached as `mod_bad` so they warn once; `Mod_ClearAll` on map change frees everything and its `TF_MODEL` textures). Studio loader: keeps the file in memory, range-checks every table, bone parents and vertex bones, loads `<name>T.mdl` textures and `<name>NN.mdl` sequence groups, decodes strips/fans into one triangle list per mesh, HD texture overrides. Sprite loader: palette per `texFormat`, single frames and groups. `mdlinfo`, `modellist` |
| `src_local.h`, `src_fs.c`, `src_material.c`, `src_studio.c`, `src_anim.c` | 3330 | Source content (see "Source models" in section 9): Half-Life 2's VPK archives behind the `hl2:` prefix, VMT/VTF materials, MDL 44-48 + VVD + VTX loading with a stand-in GoldSrc header for the DLLs, Source animation decoding |
| `r_studio.c` | 641 | Bone setup (RLE anim values, quaternion slerp between frames, 2-way sequence blending, bone controllers and mouth, motion bone zeroed, pitch flipped like GoldSrc), CPU skinning, GoldSrc-style lighting (ambient + shade from `R_LightPoint`, light from above, lambert 1.5 wrap; flatshade, fullbright), chrome UVs from the view axes, masked and additive meshes, entity render modes |
| `r_sprite.c` | 165 | Sprite frames (groups by interval), the 5 orientation types, render modes, glows (constant screen size, hidden by `R_LineBlocked` with GoldSrc's 8-unit tolerance, no depth test) |
| `r_light.c` | 253 | `R_LightPoint` (trace down 2048 units through the BSP, read the lightmap sample with light styles, same gamma table as the lightmaps), `R_LineBlocked` (segment vs solid leaves), and dynamic lights (up to 32 a frame, one bit each in a surface's `dlightbits`): `R_PushDlights` marks world faces, `R_DlightSpace` moves them into a brush entity's space, `R_DlightPoint` adds them to model light |
| `r_ents.c` | 343 | The renderer's entity list: built from the entity lump (model keys plus a temporary classname-to-model preview table), frustum + PVS culling (`R_BoxVisible` in `r_world.c`), opaque first, then translucent far to near. `mdlview`, `r_entlist` |
| `game_api.h` | 716 | Clean-room GoldSrc game interface: `entvars_t`, `edict_t`, `globalvars_t`, `TraceResult`, `KeyValueData`, `enginefuncs_t` (159 entries in order, plus 32 reserved slots in case a newer DLL copies a bigger table), `DLL_FUNCTIONS`, `NEW_DLL_FUNCTIONS`, constants. Static asserts check the sizes (entvars 676, edict 804) |
| `server.h` | 185 | Server state (`sv`: edicts, precache lists, client info), `trace_t`/`hull_t`, entry points of the `sv_*.c` files, including the checked game-object probe |
| `sv_main.c` | 953 | Loads the game DLL (`liblist.gam` `gamedll`, `GiveFnptrsToDll` is stdcall, `GetEntityAPI2`/`GetEntityAPI`, optional `GetNewDLLFunctions`), string pool (`string_t` = offset from an empty `pStringBase`), edict alloc/free (0.5 s reuse delay), precache lists (model 1 = world, 2.. = `*n`), map spawn (class export, `KeyValue`, `Spawn`; "angle" becomes "angles"), `ServerActivate`, 2 settling frames, local client connect, the ghost player, `SV_SetupRenderEntities` (edicts to the renderer every frame), commands `edicts`, `status`, `god`, `notarget`, `tracetest`, unknown commands to `pfnClientCommand`; runs the game-object probe before Lua's spawn hooks |
| `sv_gameobj.c` | 164 | Checked C++ game-object calls: map-start player/world/scientist probe, session failure latch, guarded temporary scientist construction/cleanup, `gameobj_info` and the deliberately wrong-slot check. Linux and MSVC guards; MinGW calls remain unguarded |
| `sv_engfuncs.c` | 1410 | Every `enginefuncs_t` function. Real: precache, models, traces (line, hull, toss, model, texture), PVS (`FindClientInPVS`, `EntitiesInPVS`, fat PVS, `CheckVisibility`), entity search, private data, cvars (game cvars via `Cvar_RegisterExternal`), info strings, bone/attachment positions, CRC32, random, files, the game's own command buffer (`ServerCommand`/`ServerExecute`), `NameForFunction` via the DLL's export table. Stubs: sound, messages, events, deltas, voice, save/restore helpers |
| `sv_world.c` | 433 | Collision: hulls from the BSP clipnodes (hull 0 built from the render nodes in `bsp.c`), box hull, `SV_HullPointContents`, `RecursiveHullCheck`, rotated brush entities, `SV_Move` (world + every linked edict: owner, monsterclip, ignore-glass, `ShouldCollide`), linking (`pfnSetAbsBox`, touched leafs for PVS, brush-shaped trigger touches) |
| `sv_phys.c` | 781 | `SV_Physics`: `StartFrame`, then per movetype: push (move and rotate, blocking, riders), step (gravity, ground friction), toss/bounce/fly/missile, noclip, follow; think timing (`ltime` for pushers), `FL_KILLME` removal; monster movement: `SV_movestep` (step up/down 18), `SV_CheckBottom`, `SV_MoveToOrigin`, `SV_DropToFloor` |
| `sv_pmove.c` | 764 | Player movement: fills `playermove_t` (player state, movevars from the `sv_*` cvars, physinfo, physents = world + solid entities within ~256 units, moveents = water/ladder brushes), all the pm callbacks (player traces against physents with the pm hull numbering stand/duck/point/large mapped to BSP hulls 1/3/0/2, `PM_TestPlayerPosition`, point contents incl. content brushes, `PM_HullForBsp`, `PM_TraceModel` into GoldSrc's trace_t layout, `memfgets`, files), then per command `CmdStart`, `PlayerPreThink`, think, `PM_Move`, copy back, link with trigger touches, touch what the move hit, `PlayerPostThink`, `CmdEnd` |
| `sv_save.c` | 687 | Level states: the game saves its entities into a `SAVERESTOREDATA` (data buffer that grows if the game fills it, token table, one `ENTITYTABLE` entry per edict); `pfnParmsChangeLevel` lists this map's connections and marks the entities in each transition. A level change keeps every visited map's state in memory; the next map restores its own state if it has one (without the player), then the old map's marked entities and the player, moved by the landmark offset. Globals (env_global, the tram) restore with `globalEntity` into the new map's copy. Saved games: `save/<name>.sav` in the game's userdata folder holds every level state (field-name tokens as strings), the game's global state (`pfnSaveGlobalState`) and the skill; loading replaces the remembered levels, runs `ResetGlobalState` + `RestoreGlobalState` and restores the current level with the player (`fUseLandmark` with no offset, so the player isn't sent to a spawn point). `save`, `load`, `autosave`, `saves`, `levelstates` |
| `cl_input.c` | 139 | **Temporary** (client.dll's job in GoldSrc): `+forward/+back/+moveleft/+moveright/+moveup/+movedown/+jump/+duck/+attack/+attack2/+use/+reload/+speed/+alt1/+showscores` and `impulse`, the `cl_*speed` cvars, `CL_CreateCmd` builds a `usercmd_t` each frame. `cl_viewangles` is the look direction (mouse) |
| `lua_local.h` | 17 | What `lua_host.c` shares with `lua_game.c`: `Lua_GetState`, `Lua_Call`, `Lua_Hook`, `LuaG_PushEntity` |
| `lua_game.c` | 1284 | The game API for Lua: `Vector`/`Angle` userdata (3 floats, operators, methods), `Entity` userdata (edict index + serial, so stale handles become invalid; methods plus direct entvars fields through a name/offset/type table), `ents`, `player`, `util.TraceLine/TraceHull`, constants, and the hooks the server fires |
| `client_api.h` | 710 | Clean-room client interface: `cl_enginefunc_t` (the SDK order, plus the later Steam/HL25 additions and 32 reserved slots), `cl_entity_t` (3000 bytes), `entity_state_t` (340), `ref_params_t`, `client_data_t`, `SCREENINFO`, `client_sprite_t`, the client export table. The tri/efx/event/demo/net/voice API tables are padded arrays of stubs for now |
| `cl_main.c` | 1342 | Loads client.dll (after vgui.dll), every `cl_enginefunc_t` function (HUD sprites with `hud.txt` parsing and normal/holes/additive drawing, FillRGBA (additive, like GoldSrc), text, cvars/commands, user message hooks, player info, entities for the client, `V_CalcRefdef` with `ref_params_t`, the view model, crosshair, center print), the user-message queue (game DLL messages are buffered in `sv_engfuncs.c` and dispatched at the start of the client frame; `svc_weaponanim` sets the view model animation, `svc_temp_entity` goes to `cl_tent.c`, unhooked ScreenFade/ScreenShake to `cl_view.c`), the API tables from the files below, `CL_CrashFilter` (with `CLIENT_CALL` in `cl_local.h`: a crash in the client logs its stack and turns the client off instead of killing the engine), `HUD_PlayerMoveInit` at the first map (the client's pm_shared needs its texture types before any event) |
| `vgui_bridge.c` | 480 | Hosts vgui.dll (vgui.so on Linux, see section 5): constructs `App(true)`, a full-screen root `Panel` and a `SurfaceBase` with our own 35-slot vtable (thiscall via `__fastcall` + dummy EDX), OpenGL drawing for rects, textures and text (glyphs from vgui's own `Font::getCharRGBA`), paint/clip stack from `getAbsExtents/getClipRect/getInset`. App's `main` and `platTick` are stubbed in a copied vtable. VGUI1 alpha is transparency (0 = opaque). Mouse input: the client shows the cursor through `setCursor` (anything but `dc_none`) when a menu wants it; then `VGUI_WantsMouse` turns off mouse look and `vid_sdl.c` sends motion, buttons and the wheel to App's `internal*` methods instead of key bindings. vgui reads the cursor back through `GetMousePos`, so that returns the position last passed in |
| `vgui2_local.h` | 115 | What the VGUI2 files share: the C++ ABI macros (`THISCALL`/`VTHIS` as in `vgui_bridge.c`, `VT_PREFIX`, `VT_DTORS`: GCC has two destructor slots and two words before slot 0, MSVC one destructor slot), `VG_METHOD`, the IPanel method numbers and `VP_*` wrappers |
| `gameui.c` | 849 | Hosts GameUI: checks Steam is running (GameUI ends the process otherwise) and sets `SteamAppId=70`, loads `vgui2` from the Half-Life folder and `cl_dlls/gameui`, registers the KeyValues size (24) with vgui2, starts vgui2 with the factory list GoldSrc uses (engine, vgui2, file system, HTML slot, GameUI, client), makes the three engine panels (root, client, GameUI) as vgui2 panels whose `IClientPanel` side is ours, loads `trackerscheme.res` and `valve_%language%.txt`, then `IGameUI::Initialize/Start` with the client's `cl_enginefunc_t`. The engine's interfaces: `IEngineVGui` (the panels), `IGameUIFuncs` (keys, video modes), `IBaseUI` (GameUI calls back to hide/show). Menu state (`active`), pause (`GameUI_Paused`: the menu or `setpause`), input to vgui2's `IInputInternal` (engine key numbers to vgui2 `KeyCode`s, UTF-8 text, mouse), the loading dialog (`StartProgressBar` plus two painted frames, since maps load in one go). Test commands: `gameui_press <panel>`, `gameui_click`, `gameui_key`, `gameui_panels` |
| `vgui2_surface.c` | 1162 | `ISurface` ("VGUI_Surface026", 105 methods): drawing in 2D GL, the paint/clip stack from IPanel, textures (`DrawSetTextureFile` loads `<name>.tga`), fonts glyph by glyph (`Font_Open`/`Font_Glyph`; drop shadow, blur, scanlines, additive), popups back to front: the root's tree is painted first, then each popup that `ShouldPaintChildPanel` marked (that's how a popup is hidden with its parent), `RestrictPaintToSinglePanel` (the Quit and Loading dialogs paint alone on black), `CalculateMouseVisible` every frame (who gets the mouse and keyboard), SDL system cursors for vgui's cursor shapes. Also the `VP_*` IPanel wrappers |
| `vgui2_fs.c` | 593 | `IFileSystem` ("VFileSystem009"): reads through the engine's search paths plus GameUI's own `AddSearchPath` folders; writes only under userdata: relative names in the game's write folder, absolute paths inside the Half-Life folder mirrored under `userdata/` (`<hl>/platform/config/x.vdf` becomes `userdata/platform/config/x.vdf`, read back from there) |
| `cl_local.h` | 173 | What the client-side files share: client exports and time, `CLIENT_CALL`, TE_ numbers, the effect entry points |
| `cl_event.c` | 235 | Events: `PlaybackEvent` (game, pm_shared or client) builds `event_args_t` (players shoot along `v_angle`), queued with delay and `FEV_UPDATE`, fired by name to the client's hooks in `CL_Frame`. The event API: traces through `sv_pmove.c`'s client view (every solid entity), `EV_KillEvents` marks instead of removing (it's called from inside hooks). FEV_NOTHOST events are played too (no client-side prediction) |
| `cl_tent.c` | 1661 | Dynamic lights (the client's 32-slot dlight/elight pool, entity effect lights, the flashlight), handed to `r_light.c` each frame. Temp entities in fixed blocks (the client keeps pointers) on free/active lists; `HUD_TempEntUpdate` moves them and hands back visible ones. The whole efx API (models, sprites, muzzle flashes, sparks, blood, gibs, bubbles, projectiles, player attachments), `svc_temp_entity` parsing (all TE_ types), client events in model animations (5000+, fired by frame range with the last frame kept in `latched`), view model attachments every frame, `CL_GetEntityPoint` ("entity:attachment" packing) |
| `r_part.c` | 677 | Particles from the Quake palette (`gfx/palette.lmp`): dots and streaks, the Quake ramps for fire and explosions, every particle effect of the efx API |
| `r_beam.c` | 514 | Beams: points, entities, rings, disks/cylinders (radius grows with time), follow trails; noise or sine, scrolling sprite texture, additive. The game's CBeam entities (`FL_CUSTOMENTITY`) are drawn as beams each frame; widths are in tenths like the messages |
| `r_decal.c` | 390 | Decals: one name list shared by the game (`DecalIndex`), the client and messages; decals.wad textures (index = opacity, last palette colour = colour); a decal is clipped to every surface polygon it lands on (texture-aligned), kept per surface in the brush model's own space, drawn after its surface with the lightmap. `r_decals` limit (permanent map decals kept), `decalcount` |
| `cl_tri.c` | 244 | The triangle API (immediate-mode GL), WorldToScreen/ScreenToWorld from the view matrices captured each frame |
| `cl_view.c` | 304 | titles.txt (`$position`, `$color`, timing...), TE_TEXTMESSAGE through "__NETMESSAGE__" slots and the client's HudText hook, ScreenFade (GoldSrc's fade timing, modulate) and ScreenShake (`V_CalcShake`/`V_ApplyShake`) |
| `cl_caption.c` | 586 | Closed captions: `S_StartSound` (and `speak`) hands every started sound to `CC_SoundStarted` with its play time (sentence words with their cuts, pitch and time compression) and its loudness at the listener without the `volume` cvar. The caption comes from `captions/<cc_lang>.txt` then `captions/<cc_lang>_*.txt` (later entries win; exact keys before wildcards): `Tokens` by sentence name or sample path, else the sentence's recordings' own lines joined (`RecordingCaption`; `_comma`/`_period` skipped), else an automatic caption from the words when every word's folder is in `Speakers` (`Words` respell or drop words). Source-style tags `<clr>`, `<sfx>`, `<len>`, `<cr>`, plus newen's `<at:seconds>`: `Queue` splits a line there into parts with their own `show_at`, held in the pending list until due. Captions queue and are shown at the next `CC_Draw` (after the HUD) through the Lua hook `CaptionShow`; a repeat of a line on screen only extends it. Its own proportional font atlas (`cc_font`), normal blending, box at 80% of the screen height |
| `cl_studio.c` | 134 | The studio model API table given to the client at startup (`HUD_GetStudioModelInterface`). The engine still draws models; the table's queries are real (the egon asks `IsHardware`) and its drawing entries do nothing |
| `snd_local.h` | 40 | What the sound files share: `sfx_t` (16-bit samples at the file's rate, loop start), sentences |
| `snd_main.c` | 1135 | Sound: channels (one sound per entity and channel, except auto and static), placement each frame (entities are followed; distance falloff scaled by attenuation over 1000 units, constant-power panning; the local player's own sounds are flat; with `s_spatial` the pan stops at about -8 dB in the far ear, which hears the sound up to 0.63 ms later (`EAR_DELAY`), sounds behind are low-passed a little; with `s_occlusion` `R_LineBlocked` from the listener eases a channel to -45% gain and a strong low-pass behind world geometry), gains, ear delay and muffle glide over each mix, mixing ahead `s_mixahead` seconds of float audio into SDL's queue with 8-tap Lanczos resampling (`ReadAt`, `sinc_table`; linear with `s_resample 0`; that's also the pitch), stereo samples placed in the world folded to mono, a peak limiter (`s_limiter`: instant attack, 0.1 s release, ceiling -0.2 dB) instead of clipping, sentences word by word (pitch/volume/start/end, time compression skips the end of every 25 ms), mouths (voice loudness, 0..64), music (MP3, OGG or FLAC streamed from memory, `ReadMusic`; `cd` maps tracks to `media/*.ogg/.flac/.mp3`, first found), room echo presets for `room_type`, muffling under water, `s_record` (16-bit WAV of the limited mix), `s_playat`, commands |
| `snd_mem.c` | 383 | Loading: WAV (8/16/24-bit, float, any rate; the "cue" chunk's first point is the loop start), OGG/FLAC/MP3; a `.flac`/`.ogg` next to a `.wav` replaces it (the loop point still comes from the WAV); sentences.txt (directory prefixes, standalone and attached `(p v s e t)` options, `,` and `.` as `_comma`/`_period` words). The game sends sentences by number (`!806`) |
| `snd_codecs.c` | 10 | The vendored decoders' implementations |
| `r_shader.c` | 67 | GLSL programs from `base/shaders` (compile/link with the driver's log on failure) |
| `r_shadow.c` | 588 | Shadow maps: the chosen lights' (`r_shadow_lights`) six depth views each in a 3x2 atlas (FBO), world, brush entity (`{` with holes) and model casters, `r_shadow_debug` |
| `r_modern.c` | 207 | The modern renderer: `r_renderer` selection and fallback, materials (`_n`/`_s`), the per-light additive pass state |
| `r_main.c` | 371 | View: Hor+ FOV, projection, Quake modelview, frustum planes, `R_CullBox`; the noclip camera; commands `map`, `maps`, `disconnect`, `viewpos`, `setpos`, `texlist` |

`base/lua/includes/init.lua` (231 lines) is the core Lua library (hook, timer, Color, draw helpers, string/table/math
utilities). `base/lua/autorun/newen_welcome.lua` is the demo (watermark plus `newen_hello`).

### Frame order (`Host_Frame`)
fps cap, then `Vid_PumpEvents`, `Cbuf_Execute`, `R_UpdateCamera`, `GameUI_Frame` (GameUI's and vgui2's RunFrame),
`CL_Frame` and `SV_Frame` (skipped while `GameUI_Paused`), `Lua_Frame` (Think), `Vid_BeginFrame` (clear),
`R_RenderView` (3D: world, brush entities, then `R_DrawEntities` for models and sprites), `Draw_Begin2D`, the HUD,
captions, `Lua_DrawOverlay`, `GameUI_Paint`, `Con_Draw`, FPS counter, `Vid_EndFrame` (screenshot, swap).

### Startup order (`Host_Init`)
Cmd, Cvar, Con, Keys, FS (finds HL, opens the log), default binds, the saved config (with cvar deferral), then
`newen_autoexec.cfg`, Vid, Draw, R (GL extensions, images, world cvars), Lua, client, server, GameUI, then
`+commands`, and the main menu shows (the console opens instead when GameUI didn't load).

## 7. Engine Lua (lua_host.c)

- One `lua_State` for the engine's lifetime; `lua_reload` rebuilds it. Lua concommands are removed on reload, but cvars stay.
- **Script tree** (merged, first match wins): `userdata/<game>/lua`, `userdata/<game>/addons/*/lua`,
  `base/addons/*/lua`, `base/lua`. It deliberately does **not** include the game folders, because `hl_lua/lua/` from
  the hl-lua mod would shadow newen's core library. `addon.json` isn't parsed yet; addons load alphabetically.
- Load order: `includes/init.lua`, then every `autorun/*.lua` (merged and sorted), then hook `Initialize`.
- Sandboxing: `io`, `dofile`, `loadfile`, `os.execute/remove/rename/exit/tmpname` and `package` paths are removed. `file.*` goes through the FS; writes only go to `userdata/<game>/data/`.
- **Timeouts**: an instruction-count hook checks a deadline (`lua_timeout`, default 5 s) per outermost call.
- **Errors**: tracebacks print in red. The same error repeating within 5 s is shown once (so a broken Think hook doesn't spam).
- Engine-to-Lua hooks: `Initialize`, `Think`, `DrawOverlay` (surface.* only works here), `KeyEvent(name, down, keynum)` (return true to eat the key), `WorldLoaded(map)`, `WorldUnloaded(map)`, `ShutDown`, `CaptionShow(key, text, isSfx)` (return a string to change
  the caption, false to hide it; fired from `CC_Draw`, not from inside the sound call).
- Game hooks (step 4c, `lua_game.c`, fired from `sv_main.c`/`sv_pmove.c`): `InitPostEntity`, `Tick` (every server
  frame), `OnEntityCreated` (found by scanning edicts after each frame, because the game creates most entities
  without telling the engine; each slot remembers the serial it announced), `EntityRemoved` (from `ED_Free`),
  `PlayerSpawn`/`PlayerInitialSpawn`, `PlayerDeath` (health crossing 0), `PlayerCommand` (before unknown commands go
  to the game), `StartCommand` (the usercmd as a table, copied back), `EntityKeyValue` (while a map spawns),
  `PlayerHurt(ply, amount, inflictor, attackerGuess, damageBits)`: `PF_MessageEnd` in `sv_engfuncs.c` spots the
  game's `Damage` message to the local player and queues the player's `dmg_take` before the DLL clears it;
  `SV_Frame` fires the queue after the server frame (`LuaG_FlushPlayerHurt`), so it works without a client DLL. The
  amount is the game's accumulated HUD damage, not one event per hit; `attackerGuess` is the inflictor's owner if it
  has one. Tested on c1a0d with a headcrab (two bites, 5 each).
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
  - Monsters walked jerkily: they only move when they think (about 10 times a second). `SV_SetupRenderEntities`
    draws `MOVETYPE_STEP` entities `r_steplerp_delay` (0.1 s, GoldSrc's cl_interp) in the past, between the two
    recorded steps around that moment, turning them too (`StepLerp`; `r_steplerp 0` turns it off; jumps over 128
    units and `EF_NOINTERP` aren't slid). A step never slides over more than the delay, so a walk after a pause
    starts where the monster is drawn. (2026-11-01: the first version slid from the drawn place over the last gap
    between steps; a step after a pause or a turn in place then jumped halfway and the monster lagged and caught
    up, "sometimes lag and glitch". Measured on c2a5's grunts: drawn speed now stays within ~5% of the walk.)
  - A loud continuous noise while the tram moves, and c1a0's big doors sounding several times: `snd_main.c` had
    `SND_CHANGE_VOL`/`SND_CHANGE_PITCH` as 1 and 2 (the network message's bits) instead of GoldSrc's 64 and 128, so
    every pitch or volume change started the sound again (about 3800 tram loops at once in 20 s of c0a0).
  - Space didn't jump: GameUI's keyboard options run `exec userconfig.cfg`, and the owner's Steam Half-Life has a
    bunny-hop script there (`bind space +bhop` plus aliases). The bind was kept in newen's config but the aliases
    aren't, and newen never ran `userconfig.cfg` at startup. It does now, after the config, as GoldSrc does.
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
- The console font is ASCII-only (GDI on Windows, stb_truetype on Linux). It needs UTF-8 later (VGUI2 fonts will need real font rendering anyway).
- **Linux VGUI1 text from named fonts is blank**: Linux `vgui.so`'s `FontPlat::getCharRGBA` is empty (Valve's Linux engine draws that text itself). Half-Life's menus use the bitmap fonts in `gfx/vgui/fonts/*.tga`, which do work; a mod whose VGUI1 scheme names a TrueType font would need `font_stb.c` behind `GetGlyph`. Not seen yet: single player shows no VGUI1 panels.
- **The Windows build wasn't rebuilt after the Linux port** (done on Linux): `sv_main.c`, `cl_main.c`, `vgui_bridge.c`, `sys_win.c` and `CMakeLists.txt` changed for both. Run `build.bat` once.
- **GameUI (main menu)**:
  - Escape shows the menu; in a map it hides it again (dialogs go with it, as in GoldSrc); without a map it goes to
    vgui2 (closes dialogs). `` ` `` keeps opening newen's own console on top; GameUI's console window isn't used.
    `gameui 0` (from the next start) or `-nogameui` turns the menu off and brings back the console at startup.
  - **Windows is written but not built or run.** The vtables assume MSVC's layout: one destructor slot, and
    overloaded virtuals reversed (`DrawSetColor`/`DrawSetTextColor` in ISurface, `Size` in IFileSystem). That rule is
    from experience with Source's ISurface on Windows, not checked against Windows' `GameUI.dll`. If the Windows
    menu crashes or draws wrong colours, look there first. `SetDllDirectory` (in `Sys_AddLibraryPath`) is what lets
    `vgui2.dll`/`GameUI.dll` find `tier0.dll`, `vstdlib.dll` and `steam_api.dll`.
  - Needs Steam running (GameUI starts the Steam API and ends the process if it can't). Without Steam the engine
    says so and uses the console.
  - Saves: GameUI's Load/Save dialogs read GoldSrc's "JSAV" header. Saves made since 2026-11-01 start with one
    (map name, and the comment: worldspawn's message or the map, then mm:ss of `sv.time`) before newen's own data;
    older saves still load with `load` but don't show in the dialogs. GameUI passes names with `.sav`; `save`/`load`
    strip it.
  - The Options pages read and set cvars and keys through the engine; settings newen doesn't have (renderer
    switching, some video and audio options) do nothing yet. Find servers lists real servers through Steam, but
    newen can't connect (no network protocol). Change game shows nothing. No startup music (`media/gamestartup.mp3`
    isn't in `valve`).
  - Linux fonts: a scheme face the system doesn't have (Tahoma, Verdana, Trebuchet MS, Courier) uses the fonts
    Half-Life ships in `platform/resource/linux_fonts` (DejaVu Sans, Liberation Mono); Marlett (arrows, check
    marks) comes from `valve/resource/marlett.ttf`. The real Linux Half-Life's substitutions weren't checked, so
    text widths may differ a little from Steam's Linux build. On a narrow window the 800-pixel logo is cut off at
    the right, as it is in GoldSrc.
  - Fixed on the way: `cl_enginefunc_t` was missing `pfnPlaySoundVoiceByName`, `pfnPrimeMusicStream` and
    `pfnPlaySoundByNameAtPitch` (HL25 layout), so the last entries were shifted; GameUI's `pfnGetAppID` landed on
    `ClientCmd`. A static assert now checks the layout.
- `+command` arguments are only quoted when they contain spaces.
- The FPS counter reads 0 for the first half second.
- **Untested**: Blue Shift lump detection, the HD override seen on screen (the loader path *is* confirmed by the log: "1 HD texture replacement"), and `-game` mods.
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
  code and the engine callbacks (ladders were fixed, see the owner-reported bugs above).
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
- Model lighting is tuned by eye (`r_studio_ambient 0.1`, `r_studio_shade 0.9`), lit from straight above, plus
  dynamic lights at the model's origin (`R_DlightPoint`). Chrome uses the camera axes, not GoldSrc's per-bone chrome vectors.
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

### Step 4: server DLL (done)

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

### Step 5: client DLL (done)

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

### Step 6: single player (done)

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

**Extended format, done (owner said go with the recommendations, 2026-10-18): NBSP, see `docs/NBSP.md`.** BSP30's
lumps with 32-bit indices and float bounds (the same field layout as Quake's BSP2); header `NBSP`, version 1, the 15
lumps; extra data later goes in a BSPX block after the lumps. `bsp.c` reads both:
`GetNodes/GetFaces/GetLeafs/GetEdges/GetMarkSurfaces/GetClipnodes` hand out the wide structs and widen BSP30 lumps
into scratch copies freed after the load. `tools/bsp2nbsp.py in out [x y z]` converts (and optionally moves) a
BSP30 map. Tested: converted `c2a5` renders pixel-identical; `c1a3b` converted and moved to +60000 and to -120000
plays the same as the original (walking, jumping, a zombie, explosion, bullets).

**Compiler, done: `%USERPROFILE%\newen-compiler`** (owner's choice, 2026-10-18). The owner maps in
**TrenchBroom** for big maps (Hammer 3.5 stops at +-4096) and also has their own web map editor. VHLT was ruled out:
its license is GPL overridden by Valve's HL SDK EULA (Half-Life-engine mods only). The compiler is **ericw-tools**
(plain GPL v3; it has Half-Life output, `-hlbsp`) with NBSP added, kept in its own folder and git repo (local branch
`newen`, never pushed) so newen's code stays clean. `NEWEN.md` there lists the changes; `build-newen.bat` builds it
64-bit (Embree and oneTBB from their GitHub releases in `deps/`). `qbsp -nbsp` writes NBSP; `qbsp -hlbsp` writes
BSP30 and switches to NBSP by itself past BSP30's limits. Tested: a generated room map compiles (qbsp, vis, light)
and plays in newen as BSP30 and as NBSP; a map with rooms at 0, +60000 and -60000 joined by teleporters compiles
(auto-upgraded) and plays, lit, in every room.

**TrenchBroom configuration, done: `tools/trenchbroom/`.** `install.ps1 -TrenchBroom <folder of TrenchBroom.exe>`
copies the "newen" game (`GameConfig.cfg`: Valve/Standard map formats, WAD textures from `valve`, soft map bounds
±131072, tool variables for newen-compiler's qbsp/vis/light and newen's maps folder) and TrenchBroom's own
`HalfLife.fgd` into `%APPDATA%\TrenchBroom\games\newen`, plus two compile profiles ("Compile and play", "Compile
only": `qbsp -hlbsp`, vis, light, copy to `bin/userdata/valve/maps`) and an engine profile that launches
`bin\newen.exe +map <map>`. Close TrenchBroom before installing (it writes its profiles back).

Next for big maps, when wanted: coarser lightmaps outdoors (ericw-tools already writes a lightmap-scale BSPX lump;
newen would read it), VIS hints for big open areas.

**Renderer plan: `docs/RENDERER_PLAN.md`.** An optional `r_renderer modern` path (per-pixel dynamic lights,
normal/specular maps, shadow maps) beside the current renderer, which stays the default. The owner accepted every
recommendation in its section 8 (2026-10-29) and said go. **All of its steps (0-11) are done (branch
`modern-renderer`); the owner reviewed 0-5 in game ("it works") and said continue through 11. After the plan (owner's pick, 2026-10-29,
recommendations accepted): the cone flashlight and sun shadows of moving things, below. Later options, each needing the owner's
choice: HDR/bloom, a material descriptor file, a lit shader for see-through surfaces, sharing model poses between
the shadow and main passes.** What was built:
- `r_renderer classic|modern` (archived, applied in `R_LoadMap` before `BSP_Load` via `R_SelectRenderer`), the
  capability check (`LoadShaderExtensions` in `image.c`: GL 2.0 entry points, GLSL >= 1.20, 3 texture units and
  coordinate sets), `r_modern_forcefail`, `r_rendererinfo`. Any failure prints the reason and draws classic.
- `r_shader.c` loads `base/shaders/<name>.vert/.frag` through the filesystem (a copy in `userdata/<game>/shaders`
  overrides it; the shaders are recompiled at every map load).
- `r_modern.c`: the light pass. **Deviation from the plan's step 2:** the base pass stays the classic
  fixed-function drawing (a shader version would only have reproduced it); modern adds one additive pass per light
  per batch (`DrawLightPasses` in `r_world.c`) with `ftransform()` so depths match. Lighting is computed in the drawn
  model's space (lights through `R_DlightSpace`), so brush entities need no matrices. Falloff is classic's
  (radius - distance) / 255, times N.L, plus Blinn-Phong. See-through brush entities get no light pass.
- In modern, lightmaps follow light styles only (`UpdateLightmap`, `BSP_BuildSurfaceLightmap` skip dynamic light),
  so dynamic light isn't counted twice and there are no lightmap re-uploads for it.
- `BuildModernStream` in `bsp.c`: a second VBO (`modernvert_t`: face normal, tangent along +s with the handedness of
  the "up the image" bitangent), only for modern. Normals go through `glNormalPointer`, tangents through texture
  coordinate set 2.
- Materials `textures/<name>_n` / `_s` (`R_LoadMaterial`, `TF_DATA` = no texture gamma), modern only. The
  material descriptor file the plan mentions isn't built (its syntax was never agreed); `r_gloss`/`r_specscale` are
  global.
- `r_testlight` (`r_light.c`): an engine-owned light added after the client's in `R_PushDlights`, so it works
  without a game or client DLL.
- Step 6, models (`r_studio.c`): `SetupLighting` lists the dynamic lights whose radius reaches the model (radius +
  model radius); those get a pass per mesh (`DrawLightPasses`) and are left out of the vertex colour
  (`R_DlightPoint`). Only opaque (`kRenderNormal`) models; see-through ones and the view model
  (`r_studio_viewmodel`, set by `CL_DrawViewModel`) keep classic's point light. Fullbright and additive meshes get
  no pass. Per-corner normals from the skinned normals; tangents (only when there's a normal map) per triangle
  from the deformed positions and UVs, so bumps are faceted across a mesh's triangles (a baseline; smoothing would
  share tangents per vertex+UV). Chrome meshes ignore their normal map (their UVs follow the view).
  Materials: `models/textures/<model>/<texture>_n` / `_s` (`R_LoadModelMaterial`, `TF_MODEL` lifetime).
  `tools/test_materials.py` takes `models/<x>.mdl` too.
- Step 7, shadows (`r_shadow.c`, `Shadow()` in `light.frag`): `R_RenderShadows` runs in `R_DrawWorld` after
  `R_PushDlights`, for the first light flagged `shadow` (only `r_testlight` sets it; client lights have no
  identity yet, so picking among them is step 10). Six 90-degree views into one `GL_DEPTH_COMPONENT24` texture,
  3x2 tiles of `r_shadow_size` (256), through an FBO (`gl_fbo`: ARB_framebuffer_object entry points and a 4th
  texture unit, checked in `LoadShaderExtensions`; without it modern runs unshadowed and `r_rendererinfo` says
  why). Casters: opaque world faces in the light's sphere from a BSP walk that ignores the camera's PVS; both sides
  drawn, polygon offset 1.5/2. The shader picks the tile by the major axis (same face table as `r_shadow.c`),
  turns the stored depth back into distance, compares with 4 nearest samples clamped inside the tile, and looks up
  a little off the surface along the face normal (1-3 shadow pixels, more at grazing angles): without that normal
  offset whole walls shadowed themselves. `r_shadow_bias` (1 unit) on top. Brush entities pass their rotation
  (`u_rot`) so turned doors look up the right direction. A light inside a wall is drawn unshadowed with a warning.
  `r_shadow_debug 1` shows the atlas (2D, after the 3D view), 2 also prints caster counts. The previous main view's
  matrices and viewport are pushed and restored around the depth pass.
- Step 8, brush and `{` casters (`r_shadow.c`): `CollectBrushCasters` takes every solid or alpha-tested
  (`kRenderNormal`/`kRenderTransAlpha`) brush entity whose bounds reach the light, from `R_BrushEntityCount`/
  `R_GetBrushEntity` in `r_world.c` (this frame's list, before camera culling, so an off-screen door still casts).
  Faces are sorted into batches per entity and per `{` texture (`BuildBatches`); entities get `DrawBrushEntity`'s
  transform, `{` batches bind their texture and alpha-test at 0.25 like the main pass, so grates cast with holes.
  World `{` faces are included the same way (c1a0d has none; its grates are func_walls).
- Step 9, model casters: `CollectModelCasters` walks every model this frame (`R_EntityTotal`/`R_EntityAt` in
  `r_ents.c`, before camera culling) and takes the solid/alpha-tested studio models whose radius reaches the
  light; `R_StudioCasterGeometry` (`r_studio.c`) poses each once and hands world-space triangles mesh by mesh to
  the shadow pass, which draws them into all six views from CPU arrays (masked meshes alpha-tested at 0.5, additive
  ones skipped). The main pass poses the model again: the pose isn't shared between the shadow and main passes
  yet (the plan's "shared per-frame pose data" is only shared across the six views). `BodySubmodel`, `SkinVerts`
  and `MaxMeshTris` are now shared by drawing and casting. `r_shadow_models 0` turns model casters off.
- Step 10, several shadow lights: every dynamic light has an id (`R_AddDlight(..., id, shadow)`): the client's
  are slot + key (`UpdateDlights` in `cl_tent.c`), the test light `R_TESTLIGHT_ID`. Muzzle flashes and the
  flashlight are marked no-shadow in `dlight_noshadow[]` beside the client's `dlight_t` pool (its layout is the
  client's, so no field could be added). `ChooseLights` (`r_shadow.c`) scores the rest by brightest colour x radius
  / max(distance to the view, radius), x1.5 for an id that had a map last frame (hysteresis), the test light always
  first; `r_shadow_lights` (default 2, at most `MAX_SHADOW_SLOTS` 8) get one shadow slot each (own FBO and atlas,
  kept between frames), lights under `r_shadow_minradius` (64) never. Others still light, unshadowed.
  `R_ModernSetLight` binds the light's own atlas on unit 3. Filtering: `Pcf` in `light.frag` compares the 4
  nearest pixels and blends them bilinearly; `r_shadow_soft 1` (default) averages 4 of those a pixel apart (16
  reads) for a ~3-pixel soft edge. The tile clamp is 2.5 pixels now.
- Step 11, compatibility: decals get a light pass each (`DecalLightPasses` in `r_decal.c`) right after they're
  drawn: the face's normal, a made-up tangent, and `R_ModernBlendedLights` (`u_blend`: no alpha cut, the decal's
  alpha kept, `GL_SRC_ALPHA, GL_ONE`), so the light goes in through the decal's alpha (the surface under it already
  had its share). `R_DrawDecals` takes the model space's `rot` for the shadow lookup. See-through brush entities
  (blended render modes) have no light passes, so their lightmaps keep classic's dynamic light in modern too
  (`BSP_BuildSurfaceLightmap(..., dlights)`, decided in `UpdateLightmap` from `lightpass_ok`). The view model gets
  per-pixel light like world models (`r_modern_viewmodel`, default 1); it draws in its 0..0.3 depth range and
  receives shadows but casts none. See-through studio models keep classic's point light. Sky, water, sprites,
  beams, particles, client triangles and the HUD stay on the classic path (program 0) unchanged.
- Cone flashlight (after the plan): spotlights are `rdlight_t`s with `spot`, `dir` (world axes) and `tanhalf`; their
  radius is the range. `Flashlight` in `cl_tent.c` calls `R_SetFlashlight` (modern, `R_FlashlightCone`: a lamp 8
  units right of and below the eyes, aimed at the crosshair's hit) instead of classic's point light; `AddSpots` in
  `R_PushDlights` adds it for the frame it was set, plus `r_testspot here|follow`. Ids `R_FLASHLIGHT_ID`,
  `R_TESTSPOT_ID`. In `light.frag` a spot projects the point onto its beam (`u_spotdir/right/up`, `R_SpotBasis`),
  reads the pattern (`u_cookie`, unit 4: `MakeCookie` in `r_modern.c` draws a soft round beam with a brighter
  middle and a faint ring, or loads `textures/flashlight.*`, its border forced black) and fades as (1 - d/range)^1.5
  times `r_flashlight_brightness`. Its shadow is one perspective view (`SpotModelview`, a square texture twice
  `r_shadow_size`) in its own slot ahead of the point lights' (`MAX_SPOT_SLOTS` 2, outside `r_shadow_lights`);
  `SpotShadow` reads it. Spots are skipped by classic-style dynamic light (`R_SurfaceDlights`, `R_DlightPoint`),
  so see-through things don't glow in a 1000-unit sphere. Needs 5 texture units (`R_SpotlightsAvailable`), else the
  classic flashlight. Cvars `r_flashlight_cone`, `_fov` (30), `_range` (1000), `_brightness` (1). The spot marks
  every surface in its range sphere and the shader throws away what's outside the cone (not culled on the CPU).
- Sun shadows (owner's pick and recommendations, 2026-10-29): the lightmaps already hold the sun and the world's own
  shadows, so only moving things add any. `R_SunNewMap` reads `light_environment` (`pitch`, `angle`/`angles`, the
  compiler's convention) or `r_sun_angles "pitch yaw"`. `RenderSun` (r_shadow.c, after the light slots) draws two
  orthographic depth maps (`r_sun_shadow_size` 2048) of a square `r_sun_shadow_range` (2000) round the viewer,
  snapped to whole pixels, 4x that deep along the sun: the world plus fixed brush entities (`FixedClass` in
  r_world.c: func_wall, func_wall_toggle, func_illusionary, func_detail; c2a5 builds its canyon from them, and
  counting them as moving darkened everything), and the moving ones plus models. `R_AddBrushEntity` takes the
  classname for that. `sun.vert/frag` multiplies (`GL_DST_COLOR, GL_ZERO`) each lit world/brush batch after its
  base pass and before its light passes (`DrawSunPass` in r_world.c) by 1 - `r_sun_shadow_darkness` (0.45) where
  the moving map is nearer the sun than the surface and the world map isn't, filtered like the light shadows,
  only on faces turned to the sun, fading out at the square's edge. Models don't receive sun shadows; decals drawn
  over a sun shadow aren't darkened. `r_sun_shadows 0` turns it off; a broken sun shader only turns this off.
- Audio upgrade (owner's pick, 2026-10-29: "do a lot"): see `snd_main.c` in the code map. Measured from
  `s_record` WAVs of c1a0d (`ambience/burning1.wav`, 11 kHz 8-bit, from `s_playat` 200 units off): to the right,
  the left ear is 14 dB down and 30 samples (0.63 ms) late (GoldSrc's pan left it silent); behind is duller than
  in front (3.6% vs 4.1% of the energy above 3 kHz); behind the wall at -2760 1120 -200 from the door room it's 5
  dB quieter and much duller (1.0% vs 4.1%); twelve 357 shots at once clip 42,304 samples with `s_limiter 0` and
  none with it (peak 0.977); sinc resampling halves the images above the 11 kHz sample's 5.5 kHz limit (0.42% ->
  0.22% of the energy) and keeps twice the real highs (2-5 kHz: 3.6% -> 6.4%); a Catmull-Rom first try made more
  images than linear, so it was replaced. An OGG of track 2 in `media/` plays instead of its MP3. c2a5's firefight
  with the game records without clipping. Not done: 24-bit and float sources are still stored as 16-bit; no
  Doppler or HRTF; occlusion only knows the world, not doors.
- Speed pass (2026-10-29): `r_timerefresh [frames]` (r_main.c) draws the view N times while turning once round,
  `glFinish` after each, and prints the average and slowest frame. Two fixes: the sun's square now follows the
  viewer in steps of an eighth of its size and its world map is drawn again only after a step or a sun change
  (`sun_world_valid`); and `DrawBatches` draws every batch's base first, then one sun pass for all solid faces
  (plus one per `{` texture), then the light passes per batch, instead of switching shaders twice per batch.
  Measured on the RTX 3050 at 1280x720, `-nogamedll`, 256 frames (the numbers wander by ~0.1-0.3 ms run to run):
  lab classic 0.47 ms, modern 0.49, + a light 1.08, + its shadow 1.53-1.60, + flashlight with shadow 1.25;
  canyon classic 0.54, modern 0.57-0.74, + sun 1.21-1.28 (was 1.80-1.90), + sun, light and flashlight 2.09 (was
  2.59). An A/B against the previous commit's build gave pixel-identical shots of the lab with a light, the lab
  with the test spot and the door room; the canyon differed only on an animating model. Not done: the point
  lights' world casters are still drawn every frame (a static light could keep them), models are posed again for
  each shadow, the spotlight marks everything in its range sphere.

Verified (2026-10-29, RTX 3050): classic is pixel-identical to the pre-change build in c1a0d and c2a5 world shots
(`r_drawentities 0`, `-nogamedll`, notify lines, FPS and watermark off; models animate, so full scenes aren't
comparable), also with material files present. Modern with no dynamic light is pixel-identical too. A test light
lights the world; the c1a0d door (func_door 39) moves under it; flat vs `tools/test_materials.py` maps differ and
the shine moves with the light; uniform tilted normal maps (+X, +Y) brighten towards the right/up lights, so the
tangent convention is right for unmirrored faces. A broken shader falls back to classic with the compiler's
message. With the game and client DLLs, the HUD, view model and menus draw normally. Step 6: classic world shots
still pixel-identical; c1a0d's scientists are lit from the side the light is on (a strong red light reddens their
faces and shoes; white coats are already at full red); with scientist test maps only the scientists' pixels
change. Step 7: classic and modern-without-lights still pixel-identical; c1a0d's lab table (a world brush) shadows
the floor under it (light at -2330 1030 -195); in the box-shaped room by func_door 39 only the strip of door the
frame hides changes with shadows on, so the six views meet without false shadows; a light at -2280 1300 -240 is
reported inside a wall; with the game DLL the debug atlas and the scene draw normally. Step 8: a light behind c1a0d's closed func_door 39
leaves the room's floor dark and lights a beam through the doorway once the game opens the door; the {grate4a
func_wall (rendermode 4) at -2818 -400 -208 throws bar stripes onto its window frame and lets light through onto
the pipes behind; floor grates ({gratestep2) let slivers of light into the pit. A faint lit patch stays on the
floor left of door 39 with the light behind the wall, even with brush entities off: probably a real opening in
the map behind the vents, not investigated. Classic and modern-without-lights still pixel-identical; c2a5 with the
game runs (~250 fps) with the shadow light on. Step 9: c1a0d's two scientists throw person-shaped shadows on the
wall behind them (light at -2330 1150 -240, with and without the game DLL); a scientist behind the camera still
shadows the corkboard in view; `mdlview gman walk`'s shadow changes pose between frames 12 apart. Classic and
modern-without-lights still pixel-identical. Step 10: with the game, c1a0d's scientists given EF_BRIGHTLIGHT (4) by
Lua plus the test light: with `r_shadow_lights 2` the same two ids (the test light and one glow) held the maps for
all 32 frames logged; 1/2/3 give visibly more shadows; `r_shadow_debug 1` shows one atlas per light. Soft edges
replace the stair-steps (scientist's shadow at 256). Classic and modern-without-lights still pixel-identical
(re-run after a mouse movement turned the camera in one batch: setpos right before the screenshot avoids it).
Step 11: c1a0c's infodecal blood under a test light looks like classic's (dark red takes little light in both);
a debug override showed every decal getting its pass. c1a0d's glass doors (rendermode 2, models 8/11) brighten
under a light like classic's. The hivehand in your hands is lit from the light's side with `r_modern_viewmodel 1`
and evenly with 0. c2a5's firefight spot (soldiers, god mode) ran 540 frames in modern without errors (22 decals,
blood; no soldier reached the camera's view). Loading modern, then `r_renderer classic` and the map again in the
same run is pixel-identical to the classic baseline; `vid_restart` to 960x600 in modern draws correctly. Classic
and modern-without-lights still pixel-identical. Cone flashlight: `r_testspot` in c1a0d's lab draws a round soft
beam and the left scientist's head and shoulders shadow the corkboard; with the game (`impulse 101`, `impulse
100`) the beam lands on what the crosshair is on, moves when the view turns, and a scientist in it casts a
shadow beside him; classic keeps its spot of light. Classic and modern-without-lights still pixel-identical.
Sun: in c2a5 at 1856 300 350 only the barrels' ground shadows change (8,310 pixels) once func_walls count as
scenery; c1a0d with a forced sun (`r_sun_angles "-60 30"`) changes only the animating scientists (the ceiling
blocks it); c2a5's firefight spot runs with the game. Classic and modern world shots still pixel-identical.
Not checked: a client light
(muzzle flash) or an explosion's shadow on screen in modern (scripted weapon switches picked the hivehand), mirrored texture alignments,
bodygroup/skin changes and chrome under a light, performance numbers (`r_speeds` counts world passes only), shadows
on rotated brush entities, very large lights (the far plane is the radius; precision past ~1000 units untested).

**Half-Life 2 maps plan (design only, 2026-10-22): `docs/SOURCE_PLAN.md`.** How to bring the owner's NewenSource
project (`%USERPROFILE%\Documents\Codex\2026-10-18\let\outputs\NewenSource`, read only) into newen:
Source maps load beside GoldSrc ones without the HL DLLs, Jolt for their physics. Its section 11 lists the owner's
decisions. The owner chose to do the rest of Phase 2 first (2026-10-22).

**Source models (`docs/SOURCE_MDL_PLAN.md`, owner's "start building" 2026-11-12): steps 1-9 of 10 are done on Linux, plus eyes; the Windows build is still to do.**
Goal: Source MDL 44-48 usable anywhere a GoldSrc model is (NPCs, view model, map models, Lua) through a stand-in
GoldSrc header for the DLLs. Written fresh (NewenSource couldn't be found on the Linux machine).
- Built and tested on Linux only (Windows not built yet; the code is plain C11 with no platform calls):
  `src_fs.c` (finds Half-Life 2 through Steam or `-hl2dir`, caches it in `bin/basedir_hl2.txt`, reads
  `hl2/gameinfo.txt`'s search paths, VPK v1/v2 directories in a hash table, archives before loose files because
  Steam leaves 0-byte placeholder files beside them; `srcfs_find`), `src_material.c` (VMT with `include`/patch
  blocks, VTF 7.0-7.5 decoded on the CPU: DXT1/3/5 and the plain formats; material cache freed with the model
  cache; `srcmat_dump`), `src_studio.c` (MDL header, VVD with fixups, VTX strip groups to triangle lists, skins,
  body parts, materials through the model's folder list, a minimal stand-in header: one root bone, one
  one-frame sequence, the body parts), and a Source branch in `r_studio.c` (`SrcDrawModel`, `SrcCasterGeometry`:
  rest pose, classic model lighting, alpha-tested / translucent / additive materials, shadows through the usual
  caster hook). `Mod_ForName` loads through `SrcFS_LoadFile` and sends studio versions other than 10 to
  `SrcStudio_Load`; `FS_FindSteamGame` in `filesystem.c` is the generalised Steam library search.
- The `hl2:` prefix is how Half-Life 2's files are named (`hl2:models/barney.mdl`); `SafePath` rejects `:` so
  Half-Life's own lookups can never reach them. Plan decisions M4 (prefix) and M5 (view model FOV 54) were
  recommended and not objected to; M4 is built this way.
- Checked: all 2209 models in `hl2_misc_dir.vpk` through `mdlinfo`: 2155 load; 38 are animation-only files with no
  `.vvd` (they're `$includemodel` targets, step 4); 16 have names over 63 characters with the prefix (`model_t`'s
  name is 64 bytes and client.dll reads that layout). No file was reported damaged. Missing-material warnings left
  are files Half-Life 2 itself doesn't ship (`Perftest\...`, `Grass_tuft_*`). Screenshots on c1a0d and c1a0:
  `hl2:models/barney.mdl`, `props_c17/oildrum001.mdl` (skin 3), `weapons/w_smg1.mdl`, `props_combine/breenglobe.mdl`.
- Step 3 (animation, 2026-11-12): `src_anim.c` decodes bones, animation descriptions and sequences into
  `srcbone_t`/`srcanim_t`/`srcseq_t` at load (`SrcAnim_Load`) and poses a sequence at a cycle
  (`SrcAnim_CalcPose`): per-bone records with 48/64-bit quaternions, half-float positions, run-length angle and
  position lists, delta animations (shown added to the rest pose), per-sequence bone weights (0 = bone left
  alone), sections, and animation blocks from the `.ani` file. `r_studio.c`'s `SrcSetupBones` chains the bones
  like the GoldSrc path and `SrcTransformVerts` skins with up to three bones (`bone x posetobone`). The stand-in
  header now carries every bone (name, parent, rest values) and every sequence (label, fps, frames, loop flag,
  box), so `e->sequence`, `R_StudioFrame`, `R_StudioLookupSequence` and `mdlview <model> <sequence>` work
  unchanged. Blend grids play their middle animation until step 4.
- Checked: the 2155 models still load with the bone/animation checks on; `mdlview` screenshots of
  `zombie/classic.mdl` (Idle01, attackA, walk4), `headcrabclassic.mdl` Run1, `antlion.mdl`, and `barney.mdl` /
  `alyx.mdl` (facing forward now; T-pose because their only own sequence is `ragdoll`, the rest are in
  `$includemodel` files). One shipped model (`props_animated_breakable/smokestack.mdl`) names an animation it
  doesn't hold; such a sequence is the rest pose instead of refusing the model.
- Step 4 (include files and blending, 2026-11-12), all in `src_anim.c`: `SrcAnim_Link` loads a model's
  `$includemodel` files (and theirs, 4 deep) as animation-only `srcmodel_t`s kept in a shared cache
  (`LoadInclude`; freed by `SrcAnim_FreeIncludes` from `Mod_ClearAll`, after the models), maps their bones and
  pose parameters to the model's by name (`srcinclude_t`), and builds the model's full sequence list
  (`srcvseq_t`: own sequences first; a same-named later one is skipped unless the earlier is an override
  placeholder, flag 0x1000). `SrcAnim_Seq(s, i)` is how everything reads sequence `i`. An included animation is
  decoded against its own file's bones (scales and base values) and written to the model's bones.
  Pose parameters (`srcposeparam_t`, resting value = 0 units when the range holds it): `GridPosition` places a
  parameter along one side of a sequence's animation grid (listed pose keys, or evenly between the sequence's
  start and end), and `SrcAnim_CalcPose` blends up to four animations. Entities have no pose values yet (step
  6); until then every parameter rests, and `src_posetest "<name> <value>"` forces one for testing.
  A sequence with an empty grid (layer-only) or naming a missing animation is the rest pose.
  `src_material.c` now reads `>=dxNN` blocks in a VMT (the Combine soldier keeps `$basetexture` there).
- Checked: `barney.mdl` has 784 sequences from 4 include files, `alyx.mdl` 1011; screenshots of Barney
  `idle01` / `walk_all` / `run_all`, `run_all` at `move_yaw` 90, 45 and -135 (blends), `combine_soldier.mdl`
  `CombatIdle1_SMG1`, `humans/group01/male_07.mdl` `walk_all`, `vortigaunt.mdl`, and `monk.mdl`
  `Gesture_How_Came` at four times (306 frames, in `.ani` blocks, cut into sections). All 2209 again: the same
  2155 load, no include file refused.
- Not built: layered sequences (autolayers; the Combine soldier's aim is one, so `aim_pitch` does nothing on his
  idle), IK. Sequence 0 of HL2's humans is `ragdoll` (a T-pose), so `mdlview <human>` without a sequence name
  shows that.
- Step 5 (the stand-in for the game DLL, 2026-11-12), `BuildStandIn` in `src_studio.c`: every sequence now
  carries its **activity** (Source names them; `hl_activities[]` is Half-Life's numbering, same name = same
  number, unknown = 0) and weight, its numbered **events** (frame from the cycle; named `AE_` events are left
  out), and its **travel** (`linearmovement` from the last movement record of its timing animation, read in
  `SrcAnim_Load`), which is what hl.dll turns into ground speed. Also copied: `eyeposition`, attachments (bone
  and position), and the first hitbox set with hit groups.
  **Borrowed activities:** hl.dll plays sequence 0 for an activity the model lacks, and for Half-Life 2's
  characters that's a T-pose. So for each Half-Life activity without a sequence, `activity_fallbacks[]` is
  followed (run scared > run > walk, turns > idle...) and the stand-in gets an extra sequence entry past the
  model's own list (`srcmodel_t::aliasseqs`; `SrcSetupBones` maps such a number back). Deaths have no fallback:
  Source characters ragdoll and hold no death animation, so a dying Source-model NPC shows sequence 0.
- `mdl_replace <model> <other model>` (`model.c`, `Replacement`): the other file is loaded wherever the first
  is asked for, under the first one's name, from the next map on. It's how a Source model becomes an NPC's
  model without touching hl.dll: `mdl_replace models/zombie.mdl hl2:models/zombie/classic.mdl`. Not saved to
  the config; put it in a cfg or Lua.
- Eyes (owner's request, 2026-11-12): eyeball meshes (mesh material type 1) get their iris. `src_studio.c`
  reads each submodel's eyeballs (`srceyeball_t`), `src_material.c` loads an eye material's `$iris` next to
  its `$basetexture` (`srcmaterial_t::iris`, clamped), and `SrcDrawIris` in `r_studio.c` draws it over the
  white of the eye, laid flat across the ball and centred on the eye's forward line (plus its `zoffset`).
  Eyes look straight ahead; they don't track anything, and there's no glint. MDL 44 stores the eye's forward
  pointing into the head; the loader flips it when it points back at the bone.
- Checked (game unpaused, see the test note below): a Lua-spawned `monster_zombie` with
  `hl2:models/zombie/classic.mdl` dropped to the floor, walked 120 units to the player and attacked
  (sequences 53 and 67 are borrowed-activity entries: run > walk, and the attack); `monster_scientist` as
  `hl2:models/kleiner.mdl` on c1a0d and c1a1 idles, talks and accepts "follow". Eyes on Barney (v44), Alyx,
  Kleiner, male_07, G-Man, Eli, Mossman. All 2209 again: the same 2155 load.
- **Headless test note:** on Linux the main menu counts as open after `+map`, and that pauses the game
  (`GameUI_Paused`): server time stays at 1.2 and nothing thinks. Tests of anything that moves need
  `+map x +wait 300 +gameui_hide +unpause` first (`status` shows the time). Earlier screenshots in this
  section that weren't `mdlview` were taken paused.
- Known gaps for NPC use: Half-Life code that asks for sequences by name (`monster_sitting_scientist`,
  scripted sequences) finds none in a Source model; events with monster-specific numbers mean what the Source
  model's author meant, not what hl.dll expects; no death animations; aiming, head turning and the mouth are
  step 6. These are what the plan's sidecar file is for.
- Step 6 (2026-11-12):
  - **Layered sequences** (`AddLayers` in `src_anim.c`): a sequence's autolayers are other sequences of the
    same file, faded over the cycle (or a pose parameter) and laid on top: delta layers are added by their
    weight, others blended in, both per bone by the layer's weight list. `CalcAnimation` now returns a delta
    animation as its changes alone; `SrcAnim_CalcPose` adds the rest pose for a delta sequence shown on its own.
    One level only (a layer's own layers are ignored). This is how Source aims: the Combine soldier's
    `CombatIdle1_SMG1` carries an aim layer, a grid of deltas picked by `aim_pitch`/`aim_yaw`.
  - **The game's controls** (`SrcSetupBones` in `r_studio.c`): an entity's controller and blending bytes are
    read as 0..1 of a pose parameter (`srcmodel_t::ctrlpose`, `blendpose`, with reverse flags). The stand-in
    carries a bone controller / blend range per mapping, so hl.dll's own degree-to-byte maths gives the right
    byte. **Head turning** is a bone turn instead, because HL2's humans have no sequence that uses `head_yaw`:
    controller 0 turns the bone named like a head (`...Head1`, `head`) about the model's up axis by up to
    `SRC_HEAD_TURN` (60) degrees either way; positive is to the model's left, as in GoldSrc. Only without such
    a bone does controller 0 drive `head_yaw`. Aim blends are mapped only by a settings file: most monsters
    never set their blend bytes, and a 0 byte would read as "aim fully up".
  - **Mouth** (`LoadJawMorphs` in `src_studio.c`, applied in `SrcTransformVerts`): the entity's mouth value
    (saturating at 64, as GoldSrc) moves the vertices of the jaw-drop flex: named `jaw_drop`, or facial action
    26 (`AU26`/`AU26R`) with lip-part 25 at half in older models. Half-float deltas before version 48, fixed
    point from 48. No other flexes are used. A settings file can name a jaw bone instead.
  - **Settings file** (`LoadSettings` in `src_studio.c`): `modelcfg/<model path>.newen` through Half-Life's
    search paths (`hl2:models/x.mdl` > `modelcfg/hl2/models/x.newen`), one setting a line: `controller`,
    `blend`, `activity`, `sequence`, `event`, `mouth`, `head` (the file's comment lists them). `activity` and
    `sequence` add stand-in entries (`srcalias_t`, replacing the borrowed-activity list from step 5, which now
    only fills what the file leaves open). `base/modelcfg/hl2/models/combine_soldier.newen` ships as the worked
    example: the soldier as Half-Life's grunt.
  - `mdlinfo` on a Source model now also lists each sequence's events (`number@frame`, or the name of a named
    one), the stand-in's extra sequences, the head bone and the mouth source. `src_headtest <degrees>` and
    `src_mouthtest <0..1>` force those on every Source model.
  - Checked: `src_posetest "aim_pitch -50"` / `40` / `"aim_yaw 40"` on the soldier's combat idle (aims up,
    down, left); Barney's and Alyx's mouths at `src_mouthtest` 0.5 and 1; `src_headtest` 45 and -45 (eyes go
    with the head); `monster_human_grunt` with the soldier and its settings file ran at the player and
    rifle-butted him (the soldier's events are the grunt's own numbers: 2 reload, 3 kick, 4 burst, 7 grenade);
    Kleiner-as-scientist heads sit straight; all 2209 again, the same 2155 load.
  - Not checked in a running game: a scientist's head following the player, a mouth moving to a sentence,
    the grunt's aim pitch with `blend 0 aim_pitch reverse` (the direction is a guess from the pose test: Source's
    `aim_pitch` is negative upwards).
  - `base/` reaches `bin/base/` only when the build links; after adding a file under `base/`, build or copy it.
- Step 7 (view models, 2026-11-12):
  - client.dll plays a weapon's animations **by number** (the MP5's shoot is sequence 5), so the settings
    file's `order <sequence> ...` puts sequences first in the stand-in. The stand-in's list is now: ordered
    ones, the model's own list, then aliases; `SrcStudio_StandInSeq` (`src_studio.c`) is the one place that
    maps a stand-in number back, used by `BuildStandIn`, `SrcSetupBones` and `mdlinfo`.
  - **Field of view**: `R_StudioViewModelFov` gives a Source view model's own FOV (settings
    `viewmodel_fov`, else cvar `r_viewmodel_fov_source`, 54, archived; 0 = the world's).
    `CL_DrawViewModel` swaps the projection for it (`R_BeginViewModelFov`/`R_EndViewModelFov` in
    `r_main.c`), and `ComputeAttachments` (`cl_tent.c`) moves the view model's attachments sideways by
    `R_ViewModelFovScale` so the muzzle flash, drawn with the world's projection, sits on the barrel.
  - **Events**: `EventNumber` maps Source's named `AE_MUZZLEFLASH` to Half-Life's 5001 with options "11";
    the settings file's `event` line now also takes a name (`event AE_SOMETHING 5004`), and number 0 drops
    an event. Other named events are still left out.
  - Shipped in `base/modelcfg/hl2/models/weapons/`: `v_smg1` (for the MP5), `v_pistol` (9mm handgun),
    `v_crowbar`, `v_shotgun`, each with the `mdl_replace` line in its comment.
  - Checked on c1a0d, unpaused, weapons from Lua `Give`: all four draw at the right place and size with
    their hands; the SMG fires with the shoot animation, shells and the flash on the barrel; the shotgun
    fires; face culling (`GL_FRONT`, as for GoldSrc view models) is right for Source meshes. The stock MP5
    is unchanged. All 2209 again: the same 2155 load.
  - Not checked: reload and deploy timing against the client's (animation lengths differ from Half-Life's,
    and the client times its weapon logic itself), the crowbar's swings, the pistol firing. Source's shell
    event (6001) is ignored; Half-Life's client throws its own shells.
  - In the headless tests `+attack` on the command line loses its `+`; put it in a cfg and `+exec` that.
- Step 8 (modern renderer, 2026-11-12):
  - `SrcDrawLightPasses` (`r_studio.c`) gives Source models the per-pixel dynamic light passes GoldSrc models
    get (`R_ModernBeginLights` / `SetLight` / `EndLights`), on solid meshes; see-through, additive and unlit
    ones get none, as on the GoldSrc path. The classic point-light fallback from step 2 is gone.
  - `SrcMat_LoadBump` (`src_material.c`) loads a material's `$bumpmap` the first time a light pass needs it,
    with the green channel turned over (Source's normal maps have +Y down the image, newen's up), and a
    shine map: the bump map's alpha for `$phong` / `$normalmapalphaenvmapmask`, or the base texture's alpha
    for `$basealphaenvmapmask` / `$basemapalphaphongmask`. `r_source_bumpmaps 0` (archived) turns both off.
  - Tangents: the `.vvd`'s per-vertex tangents are loaded with the vertices (same fixups) and turned by each
    vertex's first bone; which way the bump map's "up" runs is worked out per triangle from the texture
    coordinates, as the GoldSrc path does, so the sign never depends on the file.
  - **Base texture alpha**: a Source texture's alpha is transparency only for `$alphatest` / `$translucent`
    (and additive) materials; otherwise it's a shine mask or unused, and the texture is now uploaded solid.
    The light shader cuts pixels with low alpha, which left such models (the oil drum) dark.
  - Checked with `r_testlight` on c1a0d: Barney lit from one side, the Combine soldier and the oil drum with
    bump maps on and off (relief and highlights with, flatter without), a Source model's shadow on the wall.
    Classic unchanged. All 2209 again: the same 2155 load.
  - Not checked: a Source view model under its own muzzle flash or the flashlight cone; whether the shine
    strength matches Half-Life 2's look (`r_gloss` and `r_specscale` are shared with every other surface).
  - Not built from Source's shaders: environment-map reflections, self-illumination masks, rim light,
    detail textures, eye glints.
- Step 9 (regression, 2026-11-12, Linux):
  - Half-Life's own content: all 226 models in `valve/models` (texture and sequence-group files left out)
    and all 170 sprites through `mdlinfo`: 225 + 170 load, the one refusal is `doctor.mdl` (studio version 9,
    as before this work).
  - Pictures: the plan's step 0 (reference shots before starting) was never taken, so "pixel-identical to
    before" could only be checked against one older reference that happened to match a test view:
    `bin/userdata/valve/renderer_ref/lab_a.png` (Windows, 2026-10-30, `-nogamedll +map c1a0d +setpos -2250
    1136 -220 8 180 0`). Today's Linux shot of the same view has no differing pixels in the world; what
    differs is the console text at the top and the two scientists' idle animation phase. c1a0, c2a5 and
    c1a0d taken twice in a row differ only in a small text and (c1a0d) the animated scientists.
  - Play: stock game on c2a5's firefight spot (god mode, firing), `save`, `load`, then `map c1a0`; the same
    with nine `mdl_replace` lines (Kleiner, Barney, zombie, headcrab, Combine soldier, four view models)
    across c2a5 with save and load, c1a1a and c1a0d. No errors, no crash.
  - Memory: a 32-bit AddressSanitizer build (below) ran all 2209 Half-Life 2 models through `mdlinfo` and a
    session with replaced NPCs fighting, the SMG view model firing, blended and layered `mdlview` sequences,
    the mouth and head tests, modern lights with bump maps, and three map changes: no reports from newen's
    code (Valve's own `tier0` trips one in its profiler at exit; `alloc_dealloc_mismatch=0` hides it).
    Recipe: `cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    "-DCMAKE_C_FLAGS=-m32 -fsanitize=address -fno-omit-frame-pointer" "-DCMAKE_EXE_LINKER_FLAGS=-m32
    -fsanitize=address" -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu -DSDL2_LIBRARY=<the 32-bit libSDL2>
    -DSDL2_INCLUDE_DIR=/usr/include/SDL2`, build it (it overwrites `bin/newen`; rebuild the normal one
    after), run with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:alloc_dealloc_mismatch=0:log_path=<file>`.
  - **Windows: not built.** The four new files compile clean under `gcc -std=c11 -pedantic -Wall -Wextra
    -Wconversion` and use nothing outside C11 and the engine's own helpers (no POSIX calls), but MSVC hasn't
    seen them. First thing on the Windows machine: `build.bat`, then the `mdlview hl2:...` and `mdl_replace`
    checks from README. `bin\basedir_hl2.txt` caches where Half-Life 2 was found.
- **Release 0.2.0 and the MinGW Windows build (2026-11-12):** `tools/make_release.sh` builds both archives
  on Linux into `dist/` (ignored by git): `newen-<version>-linux-x86.tar.gz` from `build.sh`, and
  `newen-<version>-win32.zip` cross-built with `i686-w64-mingw32-gcc` into `build-mingw/`. The version is
  `ENGINE_VERSION` in `common.h`. `THIRD_PARTY_LICENSES.txt` (from the 0.1.0 archive) is in the repository now.
  For MinGW: `CMakeLists.txt` takes `-DOUT_DIR=` (so a second build doesn't overwrite `bin/`), skips Visual
  C++'s `SDL2main.lib` and defines `SDL_MAIN_HANDLED`; `CLIENT_CALL` (`cl_local.h`) and the `VGUI_Paint` guard
  (`cl_main.c`) run unguarded there, because 32-bit MinGW C has no `__try`. The Visual C++ build is unchanged.
  This was the first time the Windows side of `gameui.c` / `vgui2_*.c` and the Source model files went through
  a Windows compiler: no errors. Run under Wine against the Linux Steam install (which carries `hl.dll` and
  `client.dll` but not `vgui.dll`): the Windows `hl.dll` loads and runs c1a0 and c1a0d, Half-Life 2 is read
  from `-hl2dir`, Source models load and draw. **Not tested: anything needing client.dll (HUD, weapons, view
  models, the menu), because `vgui.dll` isn't in the Linux install, and real Windows.** 0.1.0 was a Visual C++
  build; the MinGW one needs Windows 10+ (system UCRT) and no VCRUNTIME140.dll.
- Not built: IK, layers of layers.
- Next: the Windows build, step 10 (docs review by the owner), and a commit (nothing from this work is
  committed). Still open from the HL2 map plan: its static props should use this loader. After that the owner
  wants physics and ragdolls, Lua SWEPs and a much bigger Lua API, each planned first.

**Three plans (original design handover, 2026-11-12; current implementation status is below):**
`docs/LUA_API_PLAN.md` (damage through `hl.dll`'s method table checked by probing, sound, effects, drawing
in the world, clientside models; decision L1), `docs/SWEP_PLAN.md` (Lua weapons in Half-Life's own HUD by
sitting between `hl.dll` and `client.dll`'s messages, plus scripted entities; S1-S4), `docs/PHYSICS_PLAN.md`
(Jolt beside Half-Life's movement: ragdolls from `.phy`, props as scripted entities solid to `hl.dll` as
boxes, pushes; P1 is the Jolt decision, P5 the order). `docs/BRIEF_FOR_AGENTS.md` is a hand-over brief for another
coding agent (ChatGPT/Codex) to build them: rules, the recommended decisions taken as accepted, order of work,
build and test recipes, and the pitfalls found in the Source models work.

**Closed captions, done (owner's pick and recommendations, 2026-10-22).** See `cl_caption.c` in the code map and
README's "Closed captions". The owner chose automatic captions from word sentences plus a caption file that
overrides them. `base/captions/english.txt` ships the automatic setup (VOX, HEV, soldiers), word fixes, a few
sound-effect lines and one hand-tidied announcement. The single-recording dialogue (scientists, Barney, G-Man, the
tram, the hazard course, the Nihilanth, the soldiers' longer lines) isn't shipped, because it's Valve's text:
`tools/make_captions.py` (owner's choice, 2026-10-22) runs whisper.cpp over the installed game's 672 voice
recordings and writes `bin/userdata/valve/captions/english_whisper.txt` on the owner's machine. Setup there:
whisper.cpp b5130 (`whisper-bin-x64.zip`, CPU) and `ggml-medium.en.bin` in `bin/whisper/`; the full run took
about an hour on the test machine (cached in `whisper_cache.json`, so re-running only redoes new files). It
keeps whisper's segment times as `<at:>` parts, splits long untimed segments into sentences paced by length,
strips leading punctuation and shortens screams; G-Man's mumbles get "[Mumbling]". The medium model was accurate
on nearly every line; a handful of misheard words (and two guesses at the Nihilanth) are corrected in the owner's
`english_whisper_fixes.txt`, which loads after it. Tested in c1a0d: a one-recording sentence, a two-recording
one (SC_HEAL2) joined, a long briefing changing to its second part on time, and a map scientist's own "Hello.".
Earlier tests on c1a0d (`speak`, `cc_emit` of HEV, VOX, soldier and explosion lines, an
add-on file with a wildcard sentence key, the Lua hook changing and hiding lines, `captions.Add` in red,
`cc_subtitles 1` hiding effects, fading out) and c0a0/c1a0 (no stray captions; that opening's speech is all single
recordings). Limits: ASCII only (the GDI font atlas); one colour per caption; HL2's UTF-16 caption files can't be
read yet; `closecaption` defaults to 1, so test scripts comparing pixels should set it to 0.
Phase 3: VGUI2. Phase 4: Opposing Force, Blue Shift, CS 1.6, Cry of Fear. Later: co-op, terrain, 64-bit.

### Lua API plan: step 1 built; decision needed before step 2 (2026-11-12)

Owner authorized the brief's implementation. Step 1 is on `codex/luaapi-1`; the rest of the Lua API,
SWEP and physics work has not been built. See `docs/LUA_API_PLAN.md`, section 10, for the stop before damage.

- `sv_gameobj.c` is compiled on both platforms. `SV_SpawnServer` probes after the local client connects,
  before Lua's entity/spawn hooks. It checks IsPlayer on player/world (1/0) and Classify on a scientist (3).
  A map without a scientist gets a temporary, unspawned one, freed after the check. Allocation capacity is
  checked first. Disconnect/map unload clears readiness; a failure stays latched until process restart.
- `gameobj_info` reports readiness, slots, answers and the guard in use. `gameobj_testbadslot 1` before a
  map deliberately uses Classify for IsPlayer (same call signature, wrong answer 2); it is not archived.
- ABI correction: Valve's public CBaseEntity declaration has **no virtual destructor**. Classify 8 and
  IsPlayer 40 are identical in Linux and Windows; VGUI's destructor-slot offset does not apply. Only
  method declarations/order were consulted, no SDK implementation or other engine code was imported.
- Linux uses the existing nestable `SYS_GUARDED` signal guard; Visual C++ uses structured exceptions.
  Object calls and temporary construction/cleanup are guarded. A fault in temporary cleanup quarantines
  its private data and finishes freeing the edict, deliberately leaking that one allocation rather than
  retrying a broken DLL callback at map unload. **MinGW remains unguarded**, as with CLIENT_CALL.
- Builds/checks: Linux gcc `-m32`, MinGW i686 and a strict gcc C11 syntax/warning check pass. Normal builds
  still show the pre-existing `S_Update` array-parameter warnings; no new warnings in this file.
  Linux `hl.so` and Windows `hl.dll` under Wine pass on c0a0/c1a0d (answers 1, 0, 3). Both reject the
  deliberately wrong slot without a crash; Linux also keeps calls off after another map with the test
  reset to 0. `gameobj_info` is safe before a map, after disconnect and with `-nogamedll`.
- Regression: compared against an executable using the original `sv_main.c` and without `sv_gameobj.c`.
  All 226 stock models checked before/after: 225 load, doctor.mdl is refused as before. Paused c1a0,
  c2a5 and c1a0d screenshots, ignoring the top 120 pixels of console notifications: c1a0/c1a0d have no
  differences; c2a5 differs by one pixel at (632, 595), with no visible scene change. Screenshots were
  inspected. Logs and images are in `bin/userdata/valve/luaapi_checks/` (ignored). Saved settings restored.
- Memory: separate 32-bit AddressSanitizer build in `build-asan/out/`, c0a0 then c1a0d and the probes:
  no reports, with `detect_leaks=0:halt_on_error=1:alloc_dealloc_mismatch=0`. Normal `bin/newen` is intact.
- Not checked: real Windows or Visual C++; client/HUD/menu interaction (these runs use `-noclientdll`;
  VGUI could not load in the baseline setup); deliberately crashing a constructor or cleanup callback.
  No damage API or gameplay interception is enabled by this step.

**Stop required by the brief's section 8:** `ApplyMultiDamage` is a global function, not a virtual slot,
and neither installed game library dynamically exports it or its accumulator. A Lua-only wrapper also
cannot intercept stock game damage, which the physics plan needs for bullets hitting scripted props.
Current traces do not identify studio hit groups. The plan's full bullet/hook/headshot check therefore
needs a reviewed damage bridge; do not silently substitute reduced behavior. Recommended: retain the
intended scope and add the investigation/validation described in `docs/LUA_API_PLAN.md`, section 10.
