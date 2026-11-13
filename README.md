# newen

A Half-Life (GoldSrc-compatible) engine with Lua built into the engine. Written from scratch in C, 32-bit, for Windows and Linux.
"newen" is a placeholder name.

You need your own copy of Half-Life (Steam). newen contains no Valve code or game files: it loads the game's own
files and DLLs from your install. Building needs Visual Studio (x86 C and C++ compilers) on Windows, or gcc and g++ with 32-bit libraries on Linux; prebuilt Windows and Linux builds are on the
Releases page: unpack one anywhere and run `newen.exe` (Windows) or `./newen` (Linux, which needs the 32-bit SDL2 and
OpenGL libraries: `lib32-sdl2-compat` and `lib32-mesa` or your GPU's 32-bit driver on Arch, `libsdl2-2.0-0:i386` on
Debian/Ubuntu).

## Build and run

```
build.bat
bin\newen.exe
```

On Linux (Arch names; Debian/Ubuntu need `gcc-multilib` and `libsdl2-dev:i386`):

```
sudo pacman -S cmake ninja lib32-gcc-libs lib32-sdl2-compat lib32-mesa   # lib32-nvidia-utils on NVIDIA
./build.sh
bin/newen
```

It runs the Linux Half-Life from Steam (`hl.so`, `client.so`, `vgui.so`). On its first start it starts itself again
with the Half-Life folder added to `LD_LIBRARY_PATH`, because `client.so` looks for `vgui.so` there.

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
| `third_party/` | SDL2 2.30.9 (zlib), Lua 5.4.7 (MIT), stb_image/stb_vorbis (public domain/MIT), dr_mp3/dr_flac (public domain/MIT-0) |

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

### Modern renderer (optional)

`r_renderer modern` (then `reload` or the next map) adds per-pixel dynamic lights on top of the normal look:
muzzle flashes, explosions and other dynamic lights shade each pixel smoothly instead of brightening the
lightmap in 16-unit blocks. The map's own baked lighting is unchanged. `r_renderer classic` (the default) draws
exactly as before. If the graphics driver can't run it or a shader fails, newen says why and draws classic;
`r_rendererinfo` shows which renderer is drawing.

Materials, modern only: next to a texture's name, `textures/<name>_n.png` is a tangent-space normal map (+Y, the
OpenGL convention) and `textures/<name>_s.png` a specular map (brightness = shine strength). Textures without them
stay flat and matte. They react to dynamic lights only, since Half-Life's lightmaps have no light direction.
Models get them as `models/textures/<model>/<texture>_n.png` and `_s.png` (beside the HD model textures).
`tools/test_materials.py <Half-Life folder> <map or models/x.mdl> bin/userdata/valve` makes rough test maps from
a map's or a model's own textures for trying it out.

- `r_dlightscale` (brightness of dynamic lights), `r_gloss` (shine sharpness, default 32), `r_specscale`.
- `r_testlight <x y z radius> [r g b]`, `r_testlight here [radius]`, `r_testlight off`: an engine light for
  testing, in both renderers; `r_testlight_orbit <distance>` makes it circle its point.
- Shadows (`r_shadows 1`, default): the test light and the game's own lights (explosions, glowing monsters, the
  lights maps and mods create) cast shadows from walls, floors and other world brushes. `r_shadow_lights` (2) picks
  how many lights cast at once: the test light first, then the brightest ones near you. Muzzle flashes and the
  flashlight don't cast. `r_shadow_soft` (1 = soft edges), `r_shadow_minradius` (smaller lights never cast).
  `r_shadow_size` (256, pixels per side of each of its six views), `r_shadow_bias`, `r_shadow_debug 1` (shows the
  shadow map). Doors, lifts and other moving brushes cast shadows too, and see-through textures (grates, fences)
  cast them with holes. People, monsters and items cast shadows that follow their animation (`r_shadow_models`).
- The flashlight (F) is a beam from a lamp beside your eyes, with a soft round pattern and its own shadow
  (`r_flashlight_cone 0` gives classic's spot of light back). `r_flashlight_fov` (30 degrees), `r_flashlight_range`
  (1000), `r_flashlight_brightness` (1); `textures/flashlight.png` replaces the pattern (black at the edges).
  `r_testspot here|follow [angle range]` / `off` is a test spotlight that works without the game.
- Sun shadows: outdoors, people, monsters, items, doors and trains cast shadows in the map's sunlight (the map's own
  walls and cliffs already have theirs baked in). `r_sun_shadows`, `r_sun_shadow_darkness` (0.45),
  `r_sun_shadow_range` (2000), `r_sun_shadow_size` (2048), `r_sun_angles "pitch yaw"` to move the sun.
- `r_timerefresh [frames]` times drawing the view (turning once round) and prints milliseconds per frame.
- Decals (bullet holes, blood, scorch marks) and the gun in your hands get per-pixel light too
  (`r_modern_viewmodel 0` gives the gun classic's). See-through models and brushes (glass) keep classic's dynamic
  light.

## Main menu

newen starts in Half-Life's own main menu: the game's `cl_dlls/gameui.so` (`GameUI.dll` on Windows) running on
Half-Life's `vgui2`, as in the Steam game, so New game, Load/Save game, Options, the server browser, Create server
and the Loading dialog are Valve's own. Escape opens it during a game (which pauses) and closes it again; Resume
game, Leave game and Quit work as in Half-Life. `` ` `` still opens newen's console on top.

It needs Steam running (otherwise newen starts in the console). `gameui 0` (from the next start) or `-nogameui`
turns the menu off. The Load and Save dialogs show saves made by this version of newen; older ones load with
`load <name>`. Joining servers doesn't work yet (no network play), and options for things newen doesn't have yet do
nothing. `togglemenu` / `gameui_activate` / `gameui_hide` show and hide it from the console.

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
- `gameobj_info` shows the compatibility probe for calls into the game's entity objects. Every map checks
  the player, world and a scientist before enabling calls. A failed probe disables calls until restart.
  `gameobj_testbadslot 1` before `map` deliberately tests refusal; it is not saved. This is the first
  Lua API expansion step; the new damage functions are not implemented yet.
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

### Source models (Half-Life 2's format, in progress)
newen is learning to use Source `.mdl` files (versions 44-48, with their `.vvd`, `.vtx`, `.vmt` and `.vtf` files)
wherever a Half-Life model works. So far they **load, draw, animate and work as NPC models**: the game DLL
gets their activities, events, walking speed, attachments and hitboxes; characters have eyes, turn their
heads, open their mouths to speech and aim; view models work; in the modern renderer they take per-pixel
dynamic lights with their own bump and shine maps (`r_source_bumpmaps 0` turns those maps off). Source
characters have no death animations of their own (they ragdoll in Half-Life 2). The plan and its progress are
in `docs/SOURCE_MDL_PLAN.md`.

- **Settings file per model**: `modelcfg/<model path>.newen` in any game folder or newen's `base/`
  (`hl2:models/combine_soldier.mdl` reads `modelcfg/hl2/models/combine_soldier.newen`). One setting a line:
  `activity <ACT_NAME> <sequence>` (this sequence answers that Half-Life activity),
  `sequence <name> <sequence>` (it also answers to that name, for game code that asks by name),
  `event <number> <number>` (renumber an animation event), `blend <0-1> <pose parameter> [reverse]` (what the
  game's aim blend drives), `controller <0-3> <pose parameter> [reverse]`, `head <bone> [reverse]` (the bone
  the game's head turn moves), `mouth <bone> <x|y|z> <degrees>` (a jaw bone, for models without the jaw flex).
  `mdlinfo` shows what a model has to offer (sequences, events, pose parameters). newen ships one as an
  example: with `mdl_replace models/hgrunt.mdl hl2:models/combine_soldier.mdl`, Half-Life's grunts are Combine
  soldiers.
  For view models there's also `order <sequence> <sequence> ...` (the client plays weapon animations by
  number, so the first sequences must follow the Half-Life weapon's order) and `viewmodel_fov <degrees>`.
- **Half-Life 2 weapons in your hands**: newen ships the settings for four. Put these in a cfg and load a map:
  `mdl_replace models/v_9mmAR.mdl hl2:models/weapons/v_smg1.mdl`,
  `mdl_replace models/v_9mmhandgun.mdl hl2:models/weapons/v_pistol.mdl`,
  `mdl_replace models/v_crowbar.mdl hl2:models/weapons/v_crowbar.mdl`,
  `mdl_replace models/v_shotgun.mdl hl2:models/weapons/v_shotgun.mdl`.
  Source view models are drawn with their own field of view, `r_viewmodel_fov_source` (54, as Half-Life 2;
  0 = the same as the world).
- `src_headtest <degrees>` and `src_mouthtest <0..1>` hold every Source model's head turned or mouth open, to
  look at them.

- `mdl_replace <model> <other model>` loads another model file wherever the first is asked for, from the next
  map on: `mdl_replace models/zombie.mdl hl2:models/zombie/classic.mdl`, then `map c1a1a`.
  `mdl_replace <model> ""` undoes it, `mdl_replace` alone lists them. It isn't saved; put it in a cfg.

- `mdlinfo <model>` lists a Source model's animation files, pose parameters and sequences. Half-Life 2's humans
  have `ragdoll` (a T-pose) as sequence 0, so name a sequence: `mdlview hl2:models/alyx.mdl idle01`.
- `src_posetest "<pose parameter> <value>"` forces one pose parameter on every Source model, to look at blends
  (`src_posetest "move_yaw 90"` with `run_all` strafes); `src_posetest ""` turns it off.

- A Source model placed in a game folder's `models/` (materials under `materials/`) loads by its normal name.
- Half-Life 2's own files are reached with the `hl2:` prefix: `mdlview hl2:models/props_c17/oildrum001.mdl`.
  Half-Life 2 is found through Steam, or start with `-hl2dir <folder>`. It's only read, never written, and
  nothing without the prefix ever comes from it (both games have a `models/barney.mdl`).
- `srcfs_find <path or pattern>` shows where a file comes from (`srcfs_find hl2:models/barney*`);
  `srcmat_dump <material>` prints a material's settings and writes its texture to
  `bin/userdata/<game>/srcdump/` as a TGA.
- Limits now: model names are at most 63 characters including `hl2:` (16 of Half-Life 2's 2209 models are
  longer); refraction materials (force fields) aren't drawn; no normal maps, no per-pixel dynamic lights on
  Source models yet.

Settings: `fov` (Hor+ widescreen), `r_texturemode trilinear|bilinear|nearest` and `r_anisotropy` (then
`r_applyfilters`), `r_texgamma` and `r_lightgamma` (brightness curves, applied on map load), `r_wateralpha`,
`r_fullbright`, `r_lightmap`, `r_novis`, `r_speeds`.

## Level changes

Walking into a level change takes you to the next map with what you carry, and anything at the landmark comes along
(monsters following you, items); going back to a map finds it as you left it. `changelevel <map> [landmark]` does the
same from the console; `levelstates` lists the maps this game remembers. `map` starts a new game.

## Saved games

`save [name]` and `load [name]` (F6 and F9 quick save and load, as "quick"), or the main menu's Save game and Load
game; every level change autosaves; dying and pressing fire loads the last save. Saves go to
`bin/userdata/<game>/save/`; `saves` lists them.

## Sound

Every sound the game and client make plays through newen's own mixer: positional (distance and stereo direction),
sentences (soldiers, scientists, the HEV suit) with the speaker's mouth moving, the game's music (`cd play <track>`,
or `mp3 play <file>`) from `media/`, and room echo where the map sets it (muffled under water). A `.flac` or
`.ogg` file next to a `.wav` (same name) is played instead of it, for HD sound packs, and `media/<track>.ogg` or
`.flac` replaces a CD track's MP3.

Beyond GoldSrc: mixing in floating point at 48 kHz with a limiter instead of clipping; filtered (sinc)
resampling, so the game's 11 and 22 kHz sounds come out clearer; sounds to one side reach the far ear a fraction of
a millisecond later, sounds behind you are slightly duller, and a sound with a wall between you is quieter and
muffled; volumes glide smoothly while things move.

- Cvars: `volume`, `MP3Volume`, `snd_rate` (44100 or 48000, at start), `s_mixahead`, `s_maxchannels`, `room_type`,
  `s_show` (1 = channel count, 2 = every sound that starts). `-nosound` turns sound off.
- `s_resample` (1 sinc, 0 linear like GoldSrc), `s_spatial` (ear delay and rear dulling), `s_occlusion` (0-1, how
  much walls muffle), `s_limiter`. Each 0 gives GoldSrc's behaviour back.
- Commands: `play <sound>`, `playvol <sound> <volume>`, `speak <sentence or words>`, `stopsound`, `soundlist`,
  `soundinfo`, `cd play|loop|stop|pause|resume`, `mp3 play|loop|stop`, `s_record [file.wav]` (records what you hear
  into `bin/userdata/<game>/`; again stops), `s_playat <sound> <x y z> [attenuation]` (a sound out in the world).

## Closed captions

Speech and sounds can be shown as captions in a box near the bottom of the screen, in the speaker's colour,
for as long as the sound plays. Only sounds loud enough to hear where you stand get one.

- Sentences built from spoken words (the VOX announcer, the HEV suit, soldiers' radio) are captioned
  automatically from their words. Everything else needs a line in the caption file.
- `base/captions/english.txt` is the caption file (its comments explain the format: `Tokens` are lines by
  sentence name or sound path, `Speakers` are colours and which word folders are captioned automatically,
  `Words` respell or drop words). Add your own lines in `captions/english_<anything>.txt` in a game folder,
  `bin/userdata/<game>/` or an addon; it loads after the main file and wins. Plain ASCII text only for now.
- A sentence made of whole recordings (scientists, guards, G-Man, the tram voice) is captioned from each
  recording's own line (`"scientist/hello.wav" "Hello."`). newen doesn't ship those lines (they're Valve's
  dialogue); `python tools/make_captions.py` makes them on your machine with whisper.cpp speech recognition
  from your installed Half-Life, into `bin/userdata/valve/captions/english_whisper.txt` (see the top of the
  script for what it needs). It's machine transcription: fix any line there or in a file of your own.
- `<at:seconds>` in a line shows the text after it that far into the sound, so a long speech comes in parts.
- Cvars: `closecaption` (1 = on), `cc_subtitles` (1 = speech only, no `[Explosion]`-style effects), `cc_lang`
  (`english`), `cc_lines` (most lines on screen), `cc_linger` (seconds after the sound), `cc_minvolume`, `cc_font`.
- Commands: `cc_emit <sentence, sound path or text>` (shows a caption), `cc_reload`, `cc_flush`.

## Console

`` ` `` opens the console (Escape opens the main menu). Tab completes, Up and Down walk through history, PgUp/PgDn and the mouse wheel scroll,
Ctrl+L clears it. Useful commands: `cmdlist`, `cvarlist`, `find <text>`, `path`, `screenshot`, `condump`,
`vid_restart`, `cl_showfps 1`, `fps_max`.

## Physics (in progress)

newen is getting a physics engine, Jolt, for ragdolls and objects that tumble (`docs/PHYSICS_PLAN.md`).
Half-Life's own movement stays as it is. So far:

- `phys_test` checks the library without a map: a box dropped onto a floor must land flat, stop and sleep;
  it prints PASS or FAIL.
- The map is in the physics world: its walls and floors, and every solid brush entity (doors, lifts, trains,
  walls), which follow Half-Life's own movement, so boxes ride lifts and get pushed by doors. A map only
  gets its physics world when something first needs one.
- Test boxes: `phys_dropbox [half size 8] [mass kg 20]` drops one in front of you, `phys_boxat x y z [half
  size] [mass]` puts one at a point, `phys_boxes` lists them (`phys_boxes all` also lists the brush
  entities), `phys_clear` removes them. They're drawn as plain orange boxes, aren't solid to the player
  or monsters yet, and aren't saved.
- `phys_maxbodies` (8192): the most bodies one map's physics world holds, from the next map.

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
`surface.CreateFont(name, { font, size, weight })/SetFont`, `surface.SetMaterial(Material(...))/DrawTexturedRect/
DrawTexturedRectUV`, `draw.SimpleText/DrawText/RoundedBox` (with a font name from `CreateFont`), hook `HUDPaint`
(the same as `DrawOverlay`), `cvars.AddChangeCallback(name, fn(name, old, new), [id])/RemoveChangeCallback`, `Color`, `captions.Add(text, [seconds], [color])/Clear` (hook
`CaptionShow(key, text, isSfx)`: return a string to change a caption, false to hide it), `input.IsKeyDown/GetKeyName/GetKeyCode`, `file.Read/Write/Exists/Find/Delete/CreateDir`,
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
- Damage, through the game's own rules (armor, god mode, death, gibs): `ent:TakeDamage(amount, [attacker],
  [inflictor], [DMG_type])`; `ent:FireBullets{ Src, Dir, Spread = Vector(x, y, 0), Num, Damage, Distance,
  Tracer, IgnoreEntity, Callback = function(hit, damage) end }` (bullets with hit groups, blood and decals,
  aimed from the entity's eyes by default); `util.BlastDamage(inflictor, attacker, pos, radius, damage)`.
  Damage types are the `DMG_*` globals (`DMG_BULLET`, `DMG_BLAST`...). If newen's startup check on the game
  DLL fails (`gameobj_info` says why), `TakeDamage` does nothing and returns false, and `BlastDamage` uses
  the game's own `env_explosion` instead.
- Sound: `ent:EmitSound(name, [level 75], [pitch 100], [volume 1], [CHAN_*])` (level in decibels as in GMod:
  75 normal, 60 quiet, 90 loud, 0 heard everywhere; `"!NAME"` plays a sentence from `sentences.txt`),
  `ent:StopSound([name])`, `sound.Play(name, pos, [level], [pitch], [volume])`, `surface.PlaySound(name)` (not
  placed in the world). Hook `EntityEmitSound(data)` sees every sound the game, client.dll (gunfire, swings) or
  Lua starts: `data.SoundName`, `Entity`, `Channel`, `Volume`, `SoundLevel`, `Pitch`, `Flags`, `Pos`; return
  false to silence it, or change fields and return true.
- Effects, the same ones the game makes: `effects.Explosion(pos, [scale], [nosound], [nolight])`,
  `effects.Smoke(pos, [scale])`, `effects.Sparks(pos)`, `effects.Ricochet(pos)`, `effects.Teleport(pos)`,
  `effects.Tracer(from, to)`, `effects.Blood(pos, dir, [color 247 red / 195 yellow], [amount])`,
  `effects.Light{ pos, radius, color = Color(r, g, b), time, decay }`, `effects.Beam{ start, endpos, sprite,
  width, noise, life, color, brightness, speed }`, `effects.Sprite(pos, sprite, [scale], [brightness])`;
  `util.Decal(name, start, endpos, [filter])` (where the line hits the world or a brush entity);
  `util.ScreenShake(pos, amplitude, frequency, duration, [radius])`; `ply:ScreenFade(SCREENFADE.IN or .OUT,
  Color(r, g, b, a), fadetime, [holdtime])`. Effects show when client.dll is running.
- Traces also return `HitGroup` (0 body, 1 head, 2 chest, 3 stomach, 4/5 arms, 6/7 legs). Lines that hit a
  monster or player are checked against its hitboxes, as in GoldSrc, so head shots count for the game's
  weapons too; `sv_hitboxes 0` goes back to whole-box hits.
- `PlayerHurt(ply, amount, inflictor, attackerGuess, damageBits)` fires after server DLL calls return,
  including without a client DLL. It snapshots the player's `dmg_take` when the game completes a `Damage`
  message: an accumulated HUD damage amount, not one event per hit or guaranteed health loss (godmode can
  still accumulate it). Zero-amount HUD indicator updates are ignored. `inflictor` is the current damage
  source; `attackerGuess` is its owner when present, otherwise the inflictor, and is only a best guess.
  Removed entities become `NULL`. The amount retains the field's value without byte wrapping;
  `damageBits` contains only the HUD-visible flags sent by the game. Return values do not alter damage.
- More entity calls: `GetAttachment(n)` (`.Pos`), `LookupBone(name)`, `LookupAttachment(name)`,
  `GetBonePosition(bone or name)`, `SetBodygroup(group, value)/GetBodygroup/GetBodygroupCount/GetNumBodyGroups`,
  `SetPlaybackRate/GetPlaybackRate`, `SetNoDraw/GetNoDraw`. Players: `Freeze(bool)/IsFrozen`,
  `SetMaxSpeed/GetMaxSpeed`, `StripWeapons()`, `GetViewModel()`, `PrintMessage(HUD_PRINTCENTER / HUD_PRINTTALK /
  HUD_PRINTCONSOLE, text)`. `ents.FindInCone(origin, dir, range, cosine)`, `ents.FindAlongRay(start, endpos,
  [mins], [maxs])`, `ents.FireTargets(targetname, [activator])` (uses every entity with that name, as a map trigger).
- More hooks: `KeyPress(ply, IN_*)`/`KeyRelease(ply, IN_*)`, `OnNPCKilled(npc, attackerGuess)`,
  `CalcView(ply, origin, angles, fov)` (return `{ origin, angles, fov }` to move the camera).
- Drawing in the world, inside hooks `PostDrawOpaque` (after solid things) and `PostDrawTranslucent` (after
  effects): `render.DrawLine(a, b, color, [depth])`, `render.DrawWireframeBox(pos, angles, mins, maxs, color,
  [depth])`, `render.DrawBox(...)`, `render.DrawSphere/DrawWireframeSphere(pos, radius, [segments], color)`,
  `render.SetMaterial(Material(...))` + `render.DrawSprite(pos, width, height, color)`,
  `render.GetViewPos/GetViewAngles`. `depth` false draws through walls. `vector:ToScreen()` gives `{ x, y,
  visible }` for name tags in `HUDPaint`.
- `ClientsideModel(model)`: a model only Lua knows (Half-Life or `hl2:`), drawn until `:Remove()` or the map
  changes: `SetPos/GetPos`, `SetAngles/GetAngles`, `SetModel`, `SetSequence(number or name)`,
  `SetPlaybackRate`, `SetCycle(0..1)`, `SetBody`, `SetSkin`, `SetColor(Color with alpha)`, `SetNoDraw`,
  `DrawModel()` (inside a draw hook), `IsValid`. `Material("sprites/x.spr" or "path.png")` for pictures.
- Examples: `base/lua/examples/render_demo.lua` (boxes and name tags on monsters, a spinning model) and
  `api_demo.lua` (fonts, a HUD picture, key log, third-person `CalcView`); run them with
  `lua_openscript examples/<name>.lua`.
- `newen_spawn <classname>` (from the example script) spawns an entity where you're looking.

### Scripted entities (`ENT`)

A file `lua/entities/<class>.lua` fills in a global `ENT` table and becomes the class `<class>` (or call
`scripted_ents.Register(ENT, class)` yourself; `scripted_ents.Get(class)`, `scripted_ents.GetList()`). Create
one with `ents.Create(class)`, `ent:SetPos(...)`, `ent:Spawn()`; a map entity whose classname is a Lua class
becomes one too (a Lua class wins over the game's own class of the same name).

- Functions, all optional: `ENT:Initialize()` (after spawning), `ENT:KeyValue(key, value)` (each key from
  the map, before `Initialize`), `ENT:Think()` (every frame, unless `self:NextThink(CurTime() + delay)` sets
  a later time), `ENT:Touch(other)`, `ENT:Use(activator, caller, useType, value)`, `ENT:OnTakeDamage(dmginfo)`,
  `ENT:OnRemove()`, `ENT:Draw()` (extra drawing with `render.*`; the model is drawn anyway),
  `ENT:OnRestore()` (after a saved game or a revisited level brought it back), `ENT:OnReloaded()` (after
  `lua_reload`).
- `self` is the Entity: every entity method and field works, and any other field (`self.Charge = 10`) is
  kept in the entity's own table, then looked up in `ENT`. `ENT.Base = "other_class"` inherits from another
  class (`self.BaseClass`).
- `+use`: the player uses the Lua entity with a `Use` function they're looking at within reach.
  `self:SetUseType(SIMPLE_USE)` calls it once per press; the default `CONTINUOUS_USE` every frame the key is
  held. While it's used the game doesn't see the use key (it would play its "nothing to use" sound).
- Damage: give it `self.takedamage = DAMAGE_YES` and health. The game's own bullets and explosions take
  health as they do from any entity (at 0 the game removes it); `OnTakeDamage` hears of it the same frame,
  with the amount (hits in one frame come together) and, when the game says, who. `ent:TakeDamage` from Lua
  calls it right away with attacker, inflictor and type. `dmginfo:GetDamage/GetAttacker/GetInflictor/
  GetDamageType`.
- Saved games and level states keep the entity's own table: numbers, strings, booleans, vectors, angles,
  entities and tables of those (functions aren't kept). Times worth keeping should come from `ServerTime()`,
  the map's clock, which stops while the game is paused and comes back with a save (`CurTime()` is the
  program's).
- Lua entities don't follow the player through a level change.
- Example: `base/lua/entities/lua_healthcharger.lua`, a health charger: `lua_run SpawnHealthCharger()` puts
  one on the floor in front of you; hold the use key on it.

## Roadmap

Phase 1, Half-Life runs:
1. **Core** - window, console, cvars, commands, keys, filesystem, Lua VM *(done)*
2. **World** - BSP v30 and big maps, WAD and HD textures, lightmaps, PVS, noclip camera *(done)*
3. **Models and sprites** - MDL (no fixed limits), SPR, HD model textures *(done)*
4. **Server** - load `hl.dll`, engine functions, collision, entity physics, player movement, Lua game API *(done)*
5. **Client** - load `client.dll` and `vgui.dll`, HUD, view model, input, events, effects, decals, beams, titles *(done)*; VGUI mouse input *(next)*
6. **Single player** - sound, level changes, saved games *(done)*

Phase 2 so far: **big maps.** BSP30 maps work out to ±32768. newen also loads its own extended format, NBSP
(`docs/NBSP.md`), which has no fixed size or count limits; `tools/bsp2nbsp.py in.bsp out.bsp [x y z]` converts a
Half-Life map to it (and can move it, for testing). The map compiler is a separate project,
`newen-compiler` (ericw-tools with NBSP output): `qbsp -nbsp` or `qbsp -hlbsp`, then `vis` and `light`.

Closed captions are done too (see above).

Then: Jolt physics, lights and shadows, particles, dev tools; Half-Life 2 maps (`docs/SOURCE_PLAN.md`, planned); the rest of
VGUI2 (the main menu already runs on it); more games
(Opposing Force, Blue Shift, CS 1.6, Cry of Fear); co-op, terrain, 64-bit, Linux.

## Licence

newen is under the MIT licence (`LICENSE`). The libraries in `third_party/` keep their own licences.
Half-Life, its DLLs and its content belong to Valve and are not part of this project.
