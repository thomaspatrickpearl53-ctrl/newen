# newen

A Half-Life (GoldSrc-compatible) engine with Lua built into the engine. Written from scratch in C, 32-bit, for Windows.
"newen" is a placeholder name.

## Build and run

```
build.bat
bin\newen.exe
```

It finds Half-Life through Steam on its own, or you can start it with `-basedir "C:\path\to\Half-Life"`. Other
options: `-game <folder>` (default `valve`), `-hd` (use the `_hd` content folders), `-dev` (developer messages), and
any `+command` like GoldSrc (`+lua_run 2+2`).

newen never writes into the Half-Life folder. Configs, logs and screenshots go to `bin\userdata\<game>\`.

## Layout

| Path | What |
|---|---|
| `src/engine/` | engine source |
| `base/lua/includes/init.lua` | core Lua library (hook, timer, Color, draw...) |
| `base/lua/autorun/` | engine scripts that run on start |
| `base/addons/<id>/lua/` | addons shipped with the engine |
| `bin/userdata/<game>/addons/<id>/lua/` | your addons |
| `third_party/` | SDL2 2.30.9, Lua 5.4.7, stb_image 2.30 |

## Maps

`map c1a0` loads a map and puts the camera at the player start; `maps` lists them, `disconnect` unloads.
Until the game DLLs are loaded the camera flies freely: mouse to look, WASD to move, Space/Ctrl up and down,
Shift for speed (`cam_speed`, `sensitivity`). `viewpos` and `setpos x y z [pitch yaw roll]` read and set it.

Rendering: BSP v30 (and Blue Shift's lump order), WAD3 and embedded textures, lightmaps with animated light
styles (`lightstyle`), PVS and frustum culling, skyboxes, warped water, animated `+` textures, `{` see-through
textures, and brush entities with their render modes. No fixed limits: any number of faces, textures and lightmap
pages, coordinates beyond +-4096.

HD textures: put `textures/<name>.png` (or .tga/.jpg/.bmp) in a game folder or in `bin\userdata\<game>\` and it
replaces that texture at any resolution. `texlist` shows the names.

## Models and sprites

Studio models (`.mdl` v10, with separate `T` texture files and `01`... sequence files) and sprites (`.spr` v2) load
and animate. Until the game DLL runs, maps show what they can on their own: entities with a model key
(`env_sprite`, `env_glow`, `monster_furniture`, `cycler`...) plus a preview table of common monsters and items.

- `mdlview <model> [sequence] [body] [skin]` puts a model or sprite in front of you (`mdlview scientist walk`,
  `mdlview sprites/fexplo.spr`); `mdlview` alone removes it.
- `mdlinfo <model>` lists sequences, body parts and textures; `modellist` lists what is loaded; `r_entlist` lists
  what is drawn.
- HD model textures: `models/textures/<model>/<texture>.png`, e.g. `models/textures/scientist/Sci2_Face.png`
  (`mdlinfo` shows the texture names).
- Lighting: `r_studio_ambient`, `r_studio_shade`, `r_studio_lambert`. `r_drawentities 0` hides models, sprites and
  brush entities.

Settings: `fov` (Hor+ widescreen), `r_texturemode trilinear|bilinear|nearest` and `r_anisotropy` (then
`r_applyfilters`), `r_texgamma` and `r_lightgamma` (brightness curves, applied on map load), `r_wateralpha`,
`r_fullbright`, `r_lightmap`, `r_novis`, `r_speeds`.

## Console

`` ` `` opens the console. Tab completes, Up and Down walk through history, PgUp/PgDn and the mouse wheel scroll,
Ctrl+L clears it. Useful commands: `cmdlist`, `cvarlist`, `find <text>`, `path`, `screenshot`, `condump`,
`vid_restart`, `cl_showfps 1`, `fps_max`.

## Lua (engine level)

| Command | |
|---|---|
| `lua_run <code>` | runs code; an expression prints its value |
| `lua_openscript <file>` | runs `lua/<file>` |
| `lua_reload` | restarts Lua and reruns every script |
| `lua_paths`, `lua_mem` | script folders, memory use |
| `lua_timeout` | seconds one call may run before it is stopped (default 5) |

API so far: `hook.Add/Remove/Run` (events: `Initialize`, `Think`, `DrawOverlay`, `KeyEvent`, `ShutDown`),
`timer.Simple/Create/Remove/...`, `concommand.Add/Remove`, `CreateConVar/GetConVar`, `RunConsoleCommand`,
`surface.SetDrawColor/DrawRect/DrawOutlinedRect/SetTextColor/SetTextPos/DrawText/GetTextSize`,
`draw.SimpleText/RoundedBox`, `Color`, `input.IsKeyDown/GetKeyName/GetKeyCode`, `file.Read/Write/Exists/Find`,
`engine.GetGameDir/Version/GetMap` (hooks `WorldLoaded`, `WorldUnloaded` get the map name), `CurTime/RealTime/FrameTime/SysTime/ScrW/ScrH`, `print/Msg/MsgN/include/PrintTable`.
The names follow Garry's Mod and Half-Life: Lua so addons can move over.

## Roadmap

Phase 1, Half-Life runs:
1. **Core** - window, console, cvars, commands, keys, filesystem, Lua VM *(done)*
2. **World** - BSP v30 and big maps, WAD and HD textures, lightmaps, PVS, noclip camera *(done)*
3. **Models and sprites** - MDL (no fixed limits), SPR, HD model textures *(done)*
4. Server - load `hl.dll`, engine functions, collision and movement, Lua hooks
5. Client - load `client.dll`, HUD, VGUI1
6. Single player - loopback server, save/load, level changes, stereo and HQ audio, Half-Life: Lua API

Then: Jolt physics, extended BSP, lights and shadows, particles, captions, dev tools; VGUI2; more games
(Opposing Force, Blue Shift, CS 1.6, Cry of Fear); co-op, terrain, 64-bit, Linux.
