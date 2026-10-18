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

`map c1a0` loads a map and starts it; `maps` lists them, `disconnect` unloads, `reload` restarts.
You play as the player: mouse to look, WASD to walk, Space to jump, Ctrl to crouch, Shift to walk slowly, E to use,
mouse buttons to attack, V for noclip. `viewpos` and `setpos x y z [pitch yaw roll]` read and set your position.
Without a game DLL (`-nogamedll`) the camera flies freely instead (`cam_speed`, `sensitivity`).

Rendering: BSP v30 (and Blue Shift's lump order), WAD3 and embedded textures, lightmaps with animated light
styles (`lightstyle`), PVS and frustum culling, skyboxes, warped water, animated `+` textures, `{` see-through
textures, and brush entities with their render modes. No fixed limits: any number of faces, textures and lightmap
pages, coordinates beyond +-4096.

HD textures: put `textures/<name>.png` (or .tga/.jpg/.bmp) in a game folder or in `bin\userdata\<game>\` and it
replaces that texture at any resolution. `texlist` shows the names.

## Game DLL (server)

newen loads the mod's game DLL (`gamedll` in `liblist.gam`, so `valve/dlls/hl.dll` for Half-Life) through the
GoldSrc interface, and `map` starts a local server: the DLL spawns the map's entities, monsters think and move,
doors and trains run, and the renderer draws what the game sets up. `-nogamedll` turns it off (maps then show the
entities from the map file only).

The player moves with the game's own movement code (walking, jumping, crouching, swimming, ladders, noclip), so
doors, buttons, triggers and pickups work. Level changes only print a message for now.

newen also loads the mod's client DLL (`cl_dlls/client.dll`) together with Half-Life's `vgui.dll`: the game's own
HUD (health, armor, ammo, weapon menus, pickup icons), crosshair, mouse look, view bob and the weapon in your hands
come from it. Keys 1-9 pick weapons, the mouse wheel cycles them, F is the flashlight. `m_rawinput 0` switches mouse look to
the cursor-based mode; alt-tabbing away releases the mouse. VGUI menus that show a cursor take the mouse while open.
Titles and credits use `cl_titlefont` (default Trebuchet MS, as Steam Half-Life). `-noclientdll` runs without
it (no HUD, engine-side movement keys).

The client's effects run too: muzzle flashes, ejected shells, tracers, sparks, bullet holes and blood (decals),
explosions and smoke, gibs, the egon and gauss beams, lasers and lightning, on-screen titles (titles.txt), screen fades
and shakes. Cvars: `r_decals` (most decals at once, default 4096), `r_particles` / `r_particles_max`,
`cl_tempents_max`, `r_drawbeams`; `decalcount` shows how many decals there are. Still missing: sound, dynamic lights
(muzzle flash light), VGUI menus beyond what the HUD needs.

If newen crashes, the end of `bin/userdata/<game>/console.log` has the call stack, and `bin/userdata/stderr.log` has
any message from a game DLL's failed assert.

- `status` shows the game DLL, map and player health; `edicts [classname]` lists the game's entities.
- `god`, `notarget` and `noclip` toggle invulnerability, being ignored by monsters, and flying through walls;
  `kill` kills you (the map restarts on the next attack press, like Half-Life).
- `impulse <n>` sends an impulse (`sv_cheats 1` then `impulse 101` gives every weapon).
- Unknown console commands go to the game, as in GoldSrc (`say`, `give` with `sv_cheats 1`...).
- `sv_maxedicts` (default 2048, GoldSrc 900) sets how many entities a map may have.
  `tracetest [hull]` checks collision under the camera.

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

## Level changes

Walking into a level change takes you to the next map with what you carry, and anything at the landmark comes along
(monsters following you, items); going back to a map finds it as you left it. `changelevel <map> [landmark]` does the
same from the console; `levelstates` lists the maps this game remembers. `map` starts a new game.

## Saved games

`save [name]` and `load [name]` (F6 and F9 quick save and load, as "quick"); every level change autosaves; dying and
pressing fire loads the last save. Saves go to `bin/userdata/<game>/save/`; `saves` lists them.

## Sound

Every sound the game and client make plays through newen's own mixer: positional (distance and stereo direction),
sentences (soldiers, scientists, the HEV suit) with the speaker's mouth moving, the game's music (`cd play <track>`,
or `mp3 play <file>`) from `media/*.mp3`, and room echo where the map sets it (muffled under water). A `.flac` or
`.ogg` file next to a `.wav` (same name) is played instead of it, for HD sound packs.

- Cvars: `volume`, `MP3Volume`, `snd_rate` (44100 or 48000, at start), `s_mixahead`, `s_maxchannels`, `room_type`,
  `s_show` (1 = channel count, 2 = every sound that starts). `-nosound` turns sound off.
- Commands: `play <sound>`, `playvol <sound> <volume>`, `speak <sentence or words>`, `stopsound`, `soundlist`,
  `soundinfo`, `cd play|loop|stop|pause|resume`, `mp3 play|loop|stop`, `s_record [file.wav]` (records what you hear
  into `bin/userdata/<game>/`; again stops).

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

Game API (works while a map runs):

- `Vector(x, y, z)` and `Angle(p, y, r)` with math operators and `Length`, `Distance`, `Dot`, `Cross`,
  `GetNormalized`, `Angle()` / `Forward`, `Right`, `Up`.
- `Entity(i)`, `LocalPlayer()`, `NULL`, `IsValid(ent)`; `ents.Create`, `GetAll`, `FindByClass`, `FindByName`,
  `FindByModel`, `FindInSphere`, `FindInBox`, `GetCount`; `player.GetAll`.
- Entity methods: `GetPos/SetPos`, `GetAngles/SetAngles`, `GetVelocity/SetVelocity`, `GetModel/SetModel`,
  `Health/SetHealth`, `Armor/SetArmor`, `GetClass`, `GetName`, `GetOwner`, `SetKeyValue`, `Spawn`, `Use`, `Touch`,
  `Remove`, `SetSequence`, `LookupSequence`, `SetColor`, `SetSize`, `IsPlayer`, `IsNPC`, `Alive`, `EyePos`,
  `EyeAngles`, `GetAimVector`, `Give` (players), `Kill`, and more. Any entity field works directly too:
  `ent.health = 50`, `ent.target`, `ent.rendermode`...
- `util.TraceLine{ start, endpos, filter }` and `util.TraceHull{ ..., mins, maxs }` return `Hit`, `HitPos`,
  `HitNormal`, `Fraction`, `Entity`, `HitWorld`, `HitTexture`; `util.PointContents`, `util.PrecacheModel`.
- Hooks: `InitPostEntity`, `Tick`, `OnEntityCreated(ent)`, `EntityRemoved(ent)`, `PlayerSpawn(ply)`,
  `PlayerInitialSpawn(ply)`, `PlayerDeath(ply)`, `PlayerCommand(ply, cmd, args)` (return true to swallow),
  `StartCommand(ply, cmd)` (change the movement input), `EntityKeyValue(class, key, value)` (return a string to
  change a map's entity keys as it loads).
- `newen_spawn <classname>` (from the example script) spawns an entity where you're looking.

## Roadmap

Phase 1, Half-Life runs:
1. **Core** - window, console, cvars, commands, keys, filesystem, Lua VM *(done)*
2. **World** - BSP v30 and big maps, WAD and HD textures, lightmaps, PVS, noclip camera *(done)*
3. **Models and sprites** - MDL (no fixed limits), SPR, HD model textures *(done)*
4. **Server** - load `hl.dll`, engine functions, collision, entity physics, player movement, Lua game API *(done)*
5. **Client** - load `client.dll` and `vgui.dll`, HUD, view model, input, events, effects, decals, beams, titles *(done)*; VGUI mouse input *(next)*
6. **Single player** - sound, level changes, saved games *(done)*

Then: Jolt physics, extended BSP, lights and shadows, particles, captions, dev tools; VGUI2; more games
(Opposing Force, Blue Shift, CS 1.6, Cry of Fear); co-op, terrain, 64-bit, Linux.
