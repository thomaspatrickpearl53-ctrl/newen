# Source map support: integration plan

Design only, written 2026-10-22 from newen at commit 67ed61d and the owner's NewenSource project
(`%USERPROFILE%\Documents\Codex\2026-10-18\let\outputs\NewenSource`, read only, never modified).
Nothing here is built. Implementation starts only when the owner says "go".

## 1. In plain terms

NewenSource is a copy of newen (taken at commit `d6c811d`, ten commits ago) with about 2,700 lines
added that load and walk around Half-Life 2 maps: it reads HL2's VPK archives, VBSP maps, VMT/VTF
materials, displacement terrain, static props (MDL/VVD/VTX) and a basic map-logic system (doors,
buttons, triggers, relays, timers), and uses Jolt for collision, the player capsule and physics boxes.
It has been tested on `d1_trainstation_02` and `d1_canals_01`.

The plan moves that code into newen as an add-on module, next to the GoldSrc code:

- `map c1a0` works exactly as today (hl.dll, client.dll, GoldSrc movement, nothing changed).
- `map d1_trainstation_02` notices a Source map, loads it through the new module, and never hands
  it to Half-Life's game or client DLL. The player walks with Jolt instead.
- You can go back and forth between the two in one session.

About 80% of what's needed already exists in NewenSource and works. The rest is: tidying it into
newen's style and file layout, keeping HL2's files separate from Half-Life's, crouching, pushable
physics props (NewenSource's props are all frozen in place), visibility culling, and a Lua API.

## 2. What NewenSource has, and what happens to each piece

Files compared with newen's history: only six files are new; NewenSource's other changes to
newen's files are small hooks, about 70 lines in total.

| NewenSource piece | Lines | What it does | Verdict |
|---|---|---|---|
| `source_assets.c` | 213 | VPK v1/v2 archives (directory, preload bytes, `_NNN.vpk` data files), the map's embedded ZIP (pakfile lump, stored and deflate via stb's zlib), VMT `$basetexture` with `include` patches, `$alphatest/$translucent/$additive/$nocull`, `UnlitGeneric`, VTF 7.x mip selection and DXT1/3/5, RGBA/BGRA/RGB/I8 decoding | **Reuse logic as is**, reformatted and split into an archive file and a material file. Mounting changes (section 4.2) |
| `source_world.c` | 431 | VBSP 19/20 header and bounds-checked lumps, faces to triangles, displacements (power 2-4), RGBE lightmaps into 1024² atlas pages, brush model reconstruction from brush planes (for doors and moving brushes), player-clip brushes, skybox, drawing (two texture units like newen's classic world), level changes through `info_landmark` | **Reuse**, reformatted (it's packed several statements per line). Split loading and drawing. Its fake one-leaf `world` setup is replaced (section 4.3). PVS added later |
| `source_models.c` | 568 | MDL 44-48 / VVD 4 / VTX 7 with checksum and range checks, VVD fixups, strips and lists, skins, static props from the `sprp` game lump (v4/5/6), shared materials, per-model VBOs, frustum culling, prop light sampled from the lightmaps, render-mesh collision shapes | **Reuse nearly as is** (already close to newen's style). Hardcoded caps become growable. Physics props are added on top |
| `source_entities.c` | 894 | Source entity I/O: outputs, connections (comma and ESC separated), delayed event queue, `!self/!activator/!caller`, wildcards, parents; func_door(_rotating), prop_door_rotating, func_button, func_movelinear, func_rotating, func_brush, func_wall_toggle, trigger_once/multiple/teleport/changelevel, logic_relay/auto/timer/branch/case, math_counter, filters, +use | **Reuse** as is. Add `prop_physics` and named `light` entities later. Expose to Lua |
| `source_physics.cpp` | 422 | The C++ Jolt bridge: PhysicsSystem with 120 Hz substeps, inches outside and metres (×0.0254) inside, Z up; static world mesh, player-clip convex hulls on their own layer, kinematic moving brushes, static prop bodies with tags, CharacterVirtual capsule (gravity 600, walk 190/run 320, 18-unit stairs, slopes up to 46°), dynamic boxes, ray casts, self-tests | **Reuse**, with Source-neutral names (`Phys_*`) so GoldSrc maps can use Jolt later. Capacities become cvars. Crouch and dynamic props added |
| `source_runtime.h` | 61 | C declarations for all of the above | Becomes newen-style headers (`src_local.h`, `phys.h`) |
| Hooks in `bsp.c`, `r_main.c`, `filesystem.c`, `r_world.c`, `cl_main.c`, `sv_main.c`, `sys_win.c`, `common.h` | ~70 | VBSP detection, draw/update/camera hand-off, Steam lookup for "Half-Life 2", `-source` switch that blocks both DLL loaders, an exe that always starts in HL2 mode | **Re-made by hand** in today's files (they've changed since `d6c811d`, e.g. NBSP). The `-source` startup switch and forced HL2 mode in `sys_win.c` are **not** copied: newen picks the path per map instead |
| `third_party/JoltPhysics` | 36 MB | Jolt (MIT), commit `bbdece36` (after v5.2.0) | **Copy** the library (`Jolt/`, `Build/`, `LICENSE`, `README.md`) into newen's `third_party/`, leaving out samples, viewer, tests and docs (most of the 36 MB) |
| `tools/verify.py` | 123 | Runs the exe: bad BSP version, bad lump, four broken-model fixtures (made up in the script, never real game files), both maps, physics and prop tests | **Port** to `tools/verify_source.py`, writing fixtures only to `bin/userdata/` |

### Licence and origin check
- NewenSource is the owner's own project, made from newen with the owner's permission (its
  HANDOFF says so). It has no licence file of its own; it's the owner's code.
- Its notes say Valve's public Source SDK 2013 headers (`studio.h`, `optimize.h`,
  `gamebspfile.h`, `bspfile.h`) were read **only for field offsets and sizes**. The code uses its own
  names and short structs (`sface_t`, `slum_t`, byte offsets like `mdl.data + 204`), not SDK
  definitions. A search for SDK and GPL markers (`Valve Corporation`, `copyright`, `dface_t`,
  `mstudio*`, `studiohdr`, `LUMP_*`, `CUtl*`, `xash`, `gpl`) in the six files found nothing. That's the
  same policy newen used for `game_api.h` (written from the HL SDK's public layout). **The owner
  should confirm this is acceptable** (decision D3).
- NewenSource's notes also name a sibling folder `SourceJolt` (the rejected Volt approach,
  containing a "mini-source-sdk"). It wasn't opened; nothing from it is to be used.
- Jolt is MIT: keep its `LICENSE` and add it to HANDOFF's third-party table.
- No HL2 content is copied. Everything is read from the installed game at run time.

## 3. Conflicts and how the plan resolves them

| Conflict | What the code does today | Resolution |
|---|---|---|
| **C vs C++** | newen is pure C11; Jolt is C++17 | One C++ file (`phys_jolt.cpp`) behind an `extern "C"` API in `phys.h`. No C++ types in any header. `project(newen C CXX)`, Jolt built as a static library with its warnings off, newen's `/W4` unchanged for C. Adds `MSVCP140.dll` at run time (part of the same Visual C++ runtime as the `VCRUNTIME140.dll` already used) |
| **32-bit build** | CMake refuses 64-bit; the HL DLLs are 32-bit | Jolt builds and runs as x86 (proven by NewenSource). Memory is the risk: 2 GB address space. HL2 maps plus Jolt's 32 MB temp allocator fit, but keep caches freed on map change, and consider `/LARGEADDRESSAWARE` (4 GB on 64-bit Windows) as an option (D7) |
| **Jolt dependency** | HANDOFF: Jolt "recommended, not formally confirmed" | The owner confirms Jolt (D1). Jolt runs **only for Source maps** in this plan; GoldSrc maps keep `sv_world.c` hulls and hl.dll's pm code. The bridge is written generically so Jolt physics on GoldSrc maps can come later as its own task |
| **Game DLL lifecycle** | `hl.dll` loads at startup (`SV_Init` → `LoadGameDLL` → `pfnGameInit`), `client.dll` and `vgui.dll` too (`CL_Init`). Every map goes `R_LoadMap` → `BSP_Load` → `CL_MapStarted` → `SV_SpawnServer` | `R_LoadMap` checks the file's first 4 bytes before anything else. For `VBSP` it ends the old map normally (the old HL map gets its usual `ServerDeactivate`), then loads the Source world and **skips `CL_MapStarted` and `SV_SpawnServer`**. `sv.active` stays false, so HUD drawing, the client's player code, events and messages all stay off, as they are at the startup console today. On a Source map the DLLs get no map calls. Note: with the DLLs present, `GameInit`/`HUD_Init` still ran once at startup; to never load them at all, start with `-nogamedll` (exists today) (D2) |
| **Commands that assume a game DLL** | `save`, `load`, `kill`, `god`, `noclip`, `reload`, `status`, `edicts`, `changelevel`, `impulse` and unknown commands go to the game | On a Source map: `noclip` toggles Source free flight, `reload` reloads the Source map, `status` reports the Source map, `save`/`load` print "not supported on Source maps yet", and the rest print that there's no game. `changelevel` between a GoldSrc and a Source map is refused (a plain `map` works) |
| **Filesystem coupling** | One search-path list over the Half-Life folder; `FS_LoadFile` is used by everything | HL2 files never enter GoldSrc lookups (otherwise a missing Half-Life sound or model could silently come from HL2). A separate Source content root: found through Steam like Half-Life is (the existing `FindInSteamLibraries`, given a folder name), override `-hl2dir <path>`, cached in `bin/basedir_hl2.txt`. Source code loads through `SrcFS_LoadFile`: map pakfile → loose `hl2/` files → VPKs from `gameinfo.txt`'s search paths. Read only, always. Map lookup: `maps/<name>.bsp` in Half-Life's paths first, then HL2's. `maps` lists both, HL2 ones marked |
| **Renderer coupling** | `R_RenderView` does view setup, then world, entities, effects, view model | Same view setup (FOV, frustum, matrices), then a branch to the Source draw. Console, Lua `DrawOverlay`, screenshots and FPS counter are outside `R_RenderView` and stay shared. The Source world keeps its own surfaces/VBO/lightmap pages (`world_t` is BSP30-shaped and used by ~14 files). Only the fields shared code reads (name, entities, entity text, sky, `loaded`) are filled |
| **Code that reads `world` directly** | `R_LightPoint`, decals, dynamic lights, `BSP_PointInLeaf`, `R_BoxVisible`, sound placement, `sv_world.c` traces | Don't run any of it on a Source map (no server, the branch skips them). Where something could still be called (Lua `util.TraceLine`, console `tracetest`), check for a Source map and use the Jolt ray cast or return "no hit". NewenSource's trick of a fake one-leaf world is replaced with explicit checks, so nothing walks a tree that isn't there |
| **Modern renderer plan** (`docs/RENDERER_PLAN.md`) | Unbuilt; plans `r_material.c`, `_n`/`_s` maps, a Y-flip option | Source materials carry `$bumpmap`/`$normalmap`, `$envmap` and specular masks. Phase-one Source drawing stays fixed-function, like classic. When the modern path exists, Source surfaces feed its material lookup (VMT bump map → normal map with the DirectX Y-flip) instead of a second shader system. No Source shader code in this plan |
| **Coordinates and units** | Both engines use inches, Z up and Quake angle order (pitch, yaw, roll) | No conversion for positions and angles. Differences: the Source player origin is at the **feet** (eye +64, crouched +28) where GoldSrc's is the box centre (eye +28 above it), so `setpos`/`viewpos` work on the eye position for Source maps, as NewenSource does. Jolt works in metres (×0.0254), converted only inside `phys_jolt.cpp`. Gravity 600 on Source maps (HL uses 800). Prop rotation goes through both `AngleVectors` (drawing) and a Jolt quaternion (collision); a test with rolled props checks they agree |
| **Brightness** | newen's `lightgammatable` (`r_lightgamma 1.6`) for BSP30 | Source lightmaps are RGB with a shared exponent; NewenSource turns them into display values with `pow(1.6·x, 1/2.2)`, checked on screenshots. Keep that as `r_source_lightscale 1.6` and `r_source_lightgamma 2.2`, archived, applied at map load |
| **Input** | With client.dll loaded, WASD are bound to the client's `+forward`... | The Source player reads the keys bound to `+forward/+back/+moveleft/+moveright/+jump/+duck/+speed/+use` from newen's binding table, so the owner's rebinds work without calling client.dll. Mouse look uses the engine path that already runs when there's no game player |
| **Hardcoded limits** in NewenSource | Jolt 16384 bodies / 32768 pairs / 16384 contacts, 8192 brush bodies, 16384 entity models, 1000 boxes, 65536 queued events, 1024-pixel texture cap, 128 brush sides on the stack | Growable arrays, or cvars where Jolt needs a number up front (`phys_maxbodies` etc., applied at map load). The event-queue cap stays as a safety guard against loops, as a cvar. Texture cap: cvar `r_source_maxtexsize` |

## 4. Architecture and module layout

newen keeps one flat `src/engine/`; the new files use a `src_` prefix (Source) and `phys` for
the engine-neutral physics bridge. Each format version is a table entry, so later VBSP versions
(21+, LZMA-compressed lumps, other Source games) are additions, not edits to every function.

```
R_LoadMap ── peek 4 bytes ──┬── 30 (BSP30), "NBSP" ─> bsp.c (unchanged path) ──> CL_MapStarted, SV_SpawnServer
                            └── "VBSP" ─────────────> src_world.c ──> src_props.c, src_ents.c ──> phys_jolt.cpp
R_RenderView ── view setup ─┬── GoldSrc: R_DrawWorld, entities, effects, view model
                            └── Source:  r_src.c (sky, world, props, physics bodies)
Host frame ─────────────────┬── GoldSrc: SV_Frame (hl.dll), CL_Frame (client.dll)
                            └── Source:  src_player.c (input → Jolt), src_ents.c (I/O, movers), Lua Think
```

### 4.1 New files
| File | From | Responsibility |
|---|---|---|
| `src/engine/src_local.h` | `source_runtime.h` | Declarations shared by the `src_*` files, the format table, the Source world struct |
| `src/engine/src_fs.c` | `source_assets.c` (VPK, ZIP) | Finding HL2, `gameinfo.txt` search paths, VPK directories (mounted on the first Source map, kept until quit), map pakfile, `SrcFS_LoadFile`, `SrcFS_FileExists` |
| `src/engine/src_material.c` | `source_assets.c` (VMT, VTF) | VMT parse (with `include`/patch), flags, VTF decode, material cache shared by world and props; missing → newen's checkerboard |
| `src/engine/src_world.c` | `source_world.c` (loading) | VBSP header and format table (19, 20), lumps, faces, displacements, lightmap pages, brush models, player clips, sky name, visibility data, pakfile mount, entity lump via `bsp.c`'s parser |
| `src/engine/r_src.c` | `source_world.c` (drawing) | Source world drawing, brush entity transforms, skybox (reusing `r_world.c`'s sky drawing), debug bodies; later PVS culling |
| `src/engine/src_props.c` | `source_models.c` | MDL/VVD/VTX, static props, entity models, prop drawing and lighting |
| `src/engine/src_ents.c` | `source_entities.c` | Entity I/O, movers, triggers, logic, level changes, physics props (new), named lights (new) |
| `src/engine/src_player.c` | `Source_Update` in `source_world.c` | Binding-aware input, walk/run/crouch/jump/use, noclip, Source `setpos`/`viewpos` |
| `src/engine/phys.h` | `source_runtime.h` (physics part) | Engine-neutral C API: world mesh, convex hulls, kinematic and dynamic bodies, character, ray casts, layers |
| `src/engine/phys_jolt.cpp` | `source_physics.cpp` | The only C++ file; Jolt setup, unit conversion, stepping, self-tests |
| `src/engine/lua_source.c` | new | Lua API for Source maps (section 7) |
| `third_party/JoltPhysics/` | NewenSource copy | Library only (see section 2) |
| `tools/verify_source.py` | `tools/verify.py` | Automated broken-input and real-map checks |

### 4.2 Files changed
| File | Change |
|---|---|
| `CMakeLists.txt` | `project(newen C CXX)`, C++17 for the one file, Jolt options and `add_subdirectory`, the new sources, link `Jolt` |
| `r_main.c` | `R_LoadMap` format check and the Source branch; `R_RenderView` Source branch after view setup; `R_UpdateCamera` and `PlaceCamera` hand-off (eye height); `setpos`/`viewpos`; `maps` listing; disconnect frees Source |
| `bsp.c` / `bsp.h` | Export the entity parser (`BSP_ParseEntities`) and a format-peek helper; `BSP_Free` also frees a Source world. BSP30/NBSP loading untouched |
| `filesystem.c` / `common.h` | Generalise the Steam library search to take a folder name; `-hl2dir`; nothing added to the GoldSrc search paths |
| `r_world.c` | Make the skybox drawer callable with given textures (`R_DrawSkyBox`) |
| `host.c` | Call the Source frame (player, entities, physics) when a Source map is loaded |
| `sv_main.c`, `sv_save.c` | Source-map behaviour of `noclip`, `reload`, `status`, `save`, `load`, `kill`, `changelevel` (section 3) |
| `cl_main.c` | Nothing, if the "no server = client idle" check holds for every per-frame client call (to be confirmed in step 2, with a log line per call kind) |
| `lua_host.c` / `lua_game.c` | Register `source.*`; `util.TraceLine` on Source maps uses Jolt |
| `README.md`, `HANDOFF.md` | Document what works and the limits, once built and tested |

## 5. Physics and movement (Source maps only)

- **World**: NewenSource collides against the visible triangles plus player clips. That misses
  invisible solid brushes (`nodraw`, `clip`, `block` textures), and it collides with non-solid
  visible faces. Better: build world collision from the **solid brushes** (the same plane
  intersection `BrushTriangles` already does for brush entities) as convex hulls. Keep displacements
  as triangle meshes. Keep `playerclip` on its own layer (player only) and `npcclip` ignored.
- **Static props**: shared mesh shapes per model, as NewenSource. Later: the model's `.phy`
  collision (convex pieces) for accurate shapes (D5).
- **Brush entities**: kinematic bodies moved by `src_ents.c` (doors, buttons, movelinear,
  rotating), as NewenSource, including pushing the player and boxes.
- **Physics props (new)**: `prop_physics`, `prop_physics_multiplayer` and `func_physbox` become
  dynamic Jolt bodies. First version: a convex hull of the render mesh, mass from the model's
  keyvalues if present, else volume × density. Drawn from the body's pose each frame.
  `prop_physics` that the map marks as motion-disabled starts asleep or static.
- **Player**: Jolt `CharacterVirtual` capsule. 72 tall standing, 36 crouched (swap shape only if
  the taller one fits, so you can't stand up into a ceiling), radius 16, eye 64/28, steps 18, slope
  limit about 45°. Pushing: the character pushes dynamic bodies with a limited force (Jolt's
  character strength setting), so crates move and heavy things don't. Movement: NewenSource sets
  the velocity directly (instant start and stop). Plan: add Source-style acceleration and friction
  (`sv_accelerate`, `sv_friction`, `sv_airaccelerate`) on top, so it feels like HL2; jump height
  tuned against the real game by the owner.
- **Stepping**: fixed 120 Hz substeps, capped at 0.1 s a frame (as NewenSource).
- **GoldSrc maps**: nothing changes. hl.dll keeps its movement; Jolt isn't even created.

## 6. Source entities

Already in NewenSource (kept): entity I/O, doors, rotating doors, buttons, movers, triggers
(once/multiple/teleport/changelevel), logic entities, filters, parents, `+use`, `info_landmark`
level changes inside HL2 maps, `info_player_start` spawn.

Added in this plan:
- `prop_physics` family (section 5).
- `light`, `light_spot`, named light styles: HL2 bakes switchable lights into extra lightmap
  styles (`styles[]` per face). Read them and support `TurnOn/TurnOff/Toggle` by rebuilding
  the touched lightmap pages. `light_environment` gives the sky light and ambient for props.
- `env_sprite`, `ambient_generic`, NPCs, weapons, scripted sequences, `point_template`, savegames:
  **not in scope**. Unknown inputs are counted and listed by `source_entity_debug 1`, as now.

## 7. Lua on Source maps

`ents`, `player` and `Entity` stay what they are: the game DLL's edicts. On a Source map they're
empty, and `player` is `nil`. A separate table avoids mixing two entity systems (D6):

- `game.GetMapFormat()` → `"bsp30"`, `"nbsp"` or `"vbsp"`; `WorldLoaded`/`WorldUnloaded` hooks fire
  for both kinds.
- `source.GetEntities([classname or targetname pattern])` → list of `SourceEntity` handles
  (index + map serial, invalid after a map change, like `Entity`).
- `SourceEntity`: `:GetClass()`, `:GetName()`, `:GetPos()`, `:GetAngles()`, `:GetKeyValue(k)`,
  `:Fire(input, [param], [delay])`, `:IsValid()`.
- `source.GetPlayerPos()/SetPlayerPos(v)`, `source.GetEyeAngles()`, `source.IsGrounded()`.
- `source.TraceLine(start, end)` → hit position, normal, entity (Jolt ray cast).
- `source.SpawnBox(pos, velocity)`; later `source.SpawnProp(model, pos, angles)`.
- Hooks: `SourceOutput(ent, outputName, activator)` when an output fires, and
  `SourceInput(ent, inputName, param)` (return true to block it).

## 8. Failing gracefully

| Situation | Behaviour |
|---|---|
| HL2 not installed / not found | `map d1_...` prints "Half-Life 2 not found; start with -hl2dir <folder>" and stays at the console. GoldSrc unaffected |
| VPK missing or damaged | Warning with the archive name; affected files fall back (checkerboard texture, prop skipped) |
| Unsupported VBSP version or compressed lumps | "VBSP version N isn't supported yet" and back to the console, no partial world |
| Out-of-range lump / face / model data | Rejected or skipped with a count, as NewenSource's bounds checks do (kept; tested by `verify_source.py`) |
| Missing material / VTF format not handled | Checkerboard, counted in the load summary ("852/854 materials") |
| Model missing or bad checksum | Prop skipped with one warning per model |
| Jolt mesh creation fails | That body is skipped with a warning; if the world mesh fails, the map loads in noclip with a clear message |
| Lua errors in `source.*` | Same as today: red traceback, repeated errors shown once |

## 9. Build order (each step: build, run headless, read the log, look at the screenshot)

Command pattern (as HANDOFF section 5). The HL2 view is NewenSource's tested spot:
```
bin\newen.exe +map d1_trainstation_02 +setpos -3924.5 -2015.34 137 0 239 0 +wait 60 +screenshot +wait 1 +quit
```
Regression views: `c1a0` spawn, `c2a5` at `setpos 1856 300 350 20 200 0`, `c1a0d` at
`setpos -2250 1136 -220 8 180 0`. Test scripts restore every cvar they change before `quit`.

| Step | Deliverable | Check |
|---|---|---|
| 0 | Reference screenshots and logs of `c1a0`, `c2a5`, `c1a0d` from today's build | Saved PNGs (pixel references for every later step) |
| 1 | Jolt in `third_party`, CMake C++ support, empty `phys_jolt.cpp` with `phys_test` (a box falls on a plane, no map needed) | Builds x86; `phys_test` prints PASS; c1a0/c2a5 pixel-identical to step 0 |
| 2 | Format check in `R_LoadMap`, Source branch that only says "Source map detected, not loaded yet", plus logging every DLL call while it's loaded | `map d1_trainstation_02` gives the message, no "Server:" line, no DLL calls logged; `map c1a0` after it works normally |
| 3 | `src_fs.c`: find HL2, `-hl2dir`, `gameinfo.txt`, VPKs, `srcfs_find <path>` debug command | Log shows the mounted entry count; finds `materials/.../*.vmt`; a Half-Life lookup for a file only in HL2 still fails (isolation) |
| 4 | `src_material.c` + `src_world.c` + `r_src.c`: world faces, lightmaps, displacements, sky | Station-hall screenshot looks like NewenSource's `independent-hl2-hall.png` (same view); log "852/854 materials, 3 displacements" |
| 5 | `src_props.c`: static props | Matches NewenSource's `station-with-props.png`; "76/76 models, 305/305 props" |
| 6 | Physics world (solid brushes, displacements, clips, props) and the player: walk, run, jump, crouch, noclip; binding-aware keys | `physics_test`-style PASS (gravity, ground, jump, crouch under a low ceiling, stand-up blocked); screenshot after walking a scripted path |
| 7 | `src_ents.c`: doors, buttons, triggers, logic, level change inside HL2 | `entity_test` PASS; a door opens in a screenshot; `d1_trainstation_01` → `02` via its trigger |
| 8 | Physics props and pushing; `spawn_box` | A crate moves when walked into (positions logged before/after); screenshot of a stack knocked over |
| 9 | Named lights / light styles, prop ambient from `light_environment` | `source_ent_fire <light> TurnOff` screenshot shows the area dark, `TurnOn` restores it |
| 10 | PVS culling for the Source world | Same screenshot as step 5; `r_speeds` shows fewer surfaces drawn |
| 11 | `lua_source.c` | A throwaway autorun script lists doors, fires one open, traces a line; log shows results |
| 12 | `verify_source.py`, then the full regression | Broken inputs are rejected and the next map loads; c1a0, c2a5, c1a0d pixel-identical to step 0; c1a0d door walk, a c2a5 firefight, save/load and a level change still work; going c1a0 → HL2 → c1a0 works |
| 13 | README/HANDOFF update with the limits | Owner review |

Steps 1-5 give "look at HL2 maps in newen". Stop there for the owner to look before physics,
if wanted (D8).

## 10. Limits after this plan (for the docs)

No HL2 gameplay: no NPCs, weapons, HUD, sounds, music, scripted scenes, choreography, savegames
or Source game DLLs. No water surfaces, 3D skybox (`sky_camera`), envmaps, bump/specular, detail
props (grass sprites), decals or overlays (`info_overlay`), material proxies (animated or
scrolling textures), HDR or Source-accurate translucent sorting. Displacement seams aren't stitched.
Only VBSP 19/20 as used by HL2 on Steam. Other Source games and episodes need their own VPK lists
(via `gameinfo.txt`) and are untested.

## 11. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| D1 | Confirm Jolt (MIT) as newen's physics library | Yes. NewenSource proves it on 32-bit Windows, and it's the only free choice of that quality |
| D2 | How strictly to keep Half-Life's DLLs away from Source maps | Load them at startup as now, but never call them for a Source map; `-nogamedll` for a session with no DLLs at all. Alternative: load them only when the first GoldSrc map starts (bigger change to startup, more risk) |
| D3 | Accept NewenSource's code, written using Valve's public SDK headers only as a description of the file layout | Yes, same rule as `game_api.h`. Otherwise those loaders would have to be written again from community format docs |
| D4 | Where HL2 maps come from | Found automatically through Steam, `-hl2dir` to override; `map name` searches Half-Life first, then HL2 |
| D5 | Collision for props | Render-mesh shapes now (as NewenSource), `.phy` files later as its own step |
| D6 | Lua shape | A separate `source.*` table and `SourceEntity` type, not mixed into `ents`/`Entity` |
| D7 | `/LARGEADDRESSAWARE` for more memory headroom | Turn on only if a map runs out; test hl.dll/client.dll with it first |
| D8 | Review point | Stop after step 5 for the owner to look at the rendering before physics and entities |
| D9 | Movement feel | Add HL2-like acceleration/friction to NewenSource's instant movement; owner tunes jump height by hand |
