# Physics and ragdolls on Half-Life maps: plan

Design written 2026-11-12 from newen at commit c9a8232. **Step 1 is built (2026-11-13, branch `phys-1`); the
owner's "start physics" confirmed P1 (Jolt).** See "Built" at the end. Third of the three plans the
owner asked for together; its Lua calls build on `docs/LUA_API_PLAN.md`, and its props become scripted
entities from `docs/SWEP_PLAN.md`.

## 1. In plain terms

Half-Life has no physics engine: things move in straight lines, slide and fall, and nothing tumbles. This
plan adds Jolt (a free physics library, MIT licence) beside Half-Life's own movement, for three things:

1. **Ragdolls.** A Half-Life 2 character used as an NPC model (`mdl_replace`) falls down limp when it dies,
   with the joints its model file describes, instead of snapping to a T-pose (today it has no death animation).
2. **Physics props.** Lua (and maps, through scripted entities) can place objects that fall, tumble, stack
   and get knocked over: Half-Life 2's barrels and crates with their own collision shapes, or any model with
   a box.
3. **Pushes.** Explosions throw props and ragdolls; Lua bullets and Lua code push them; the player shoves
   them by walking into them.

Half-Life's own movement (the player, monsters, doors, trains, grenades) is untouched: `hl.dll` keeps doing
all of it. Jolt runs alongside and only moves what it owns.

## 2. Where it fits

```
Host frame ──> SV_Frame (hl.dll thinks, moves the player and monsters)
           ──> Phys_Step (Jolt, fixed 120 Hz substeps)
                 ├─ static world: the map's world-model faces (built at map load)
                 ├─ kinematic: doors, trains, platforms follow their entities each frame
                 ├─ kinematic: the player (a capsule following the player's position, so walking shoves things)
                 ├─ dynamic: props, ragdoll parts
                 └─ results ─> props' edicts (position, angles, box), ragdoll bone poses for the renderer
```

- One C++ file, `phys_jolt.cpp`, behind a plain-C header `phys.h` (world mesh, convex hulls, boxes, kinematic
  and dynamic bodies, constraints, ray casts, impulses). Inches outside, metres inside (×0.0254), as the HL2
  map plan (`docs/SOURCE_PLAN.md`, section 5) already designed, so both plans share it.
- The rest of the engine stays C. `CMakeLists.txt` gets `project(newen C CXX)` and builds Jolt as a static
  library, for all three compilers newen uses: Visual C++ x86, gcc `-m32`, MinGW i686.

## 3. Ragdolls

- **Shapes and joints from the model's `.phy` file** (Half-Life 2 ships one beside each character): one
  convex shape per bone (the hull's points, read from the file's collision data), and the text section's
  `solid` blocks (bone, mass, damping) and `ragdollconstraint` blocks (each joint's angle limits per axis).
  The format is read from public community documentation, under the same rule as the rest of the Source
  work (`docs/SOURCE_MDL_PLAN.md`, decision M2; SOURCE_PLAN D3). New file `src_phys.c`.
- **When**: an entity whose model has a `.phy` gets `deadflag` set by `hl.dll` (it is dying). newen takes its
  current pose and the speed of each bone from the last two frames, builds the ragdoll, and from then on
  draws that entity from the ragdoll's bones instead of its animation. `hl.dll` still owns the corpse (it
  can gib it, remove it, fade it); when it goes, the ragdoll goes.
- **Visual only, as Half-Life 2's client ragdolls are**: the player and monsters walk through them, they don't
  block bullets for `hl.dll`. They collide with the world, props and each other. They settle, then sleep.
- **Limits**: `phys_ragdoll_max` (default 16): the oldest is removed beyond it; `phys_ragdoll_time` (0 =
  keep until the map changes).
- **Half-Life's own models** have no `.phy`. A ragdoll can be made from their hitboxes (a box per bone, joint
  limits guessed from bone names). Decision P4.

## 4. Physics props

- A prop is a **scripted entity** (`docs/SWEP_PLAN.md`, section 4) of class `prop_physics`, so maps can place
  them, Lua can spawn them, saves keep them, and traces hit them.
- Shape: the model's `.phy` (one or more convex pieces) for Source models; for Half-Life models, the box of
  the model's first sequence. Mass from the `.phy` or volume × density.
- **What Half-Life sees**: Half-Life's collision only knows upright boxes. Each frame the prop's edict gets
  the box round its current shape, so the player and monsters bump into it, stand on it and shoot it. A long
  object lying diagonally gets a box bigger than itself; that's the price of `hl.dll`'s collision (decision P3).
- **Taking damage**: a prop is a scripted entity with a real game object behind it, so `hl.dll`'s weapons hurt
  it like any entity; newen sees its health drop (`docs/SWEP_PLAN.md` section 4) and pushes it away from the
  attacker (the exact line of the shot isn't known for the game's own bullets), and it can break (`health`
  key). Lua bullets know the line and push along it. *Revised 2026-11-13*: the general damage hook this
  relied on is deferred (`docs/LUA_API_PLAN.md` section 10).
- Lua: `physics.SpawnProp(model, pos, angles)`, `ent:GetPhysicsObject()` with `ApplyForceCenter`,
  `ApplyForceOffset`, `SetVelocity`, `GetVelocity`, `EnableMotion`, `Wake`, `Sleep`, `GetMass`, `SetMass`;
  `ent:BecomeRagdoll()`; hooks `PhysicsCollide(ent, data)` (for impact sounds and damage).

## 5. Pushes

| Source | How newen knows | Push |
|---|---|---|
| Explosions from `hl.dll` (grenades, rockets, satchels, `env_explosion`) | The explosion temp-entity message passes through `PF_MessageEnd` | Radial impulse falling off with distance, line of sight checked with a Jolt ray |
| `hl.dll`'s bullets | The prop's health drops (it has a real game object); ragdolls aren't hit by the game's bullets | Impulse away from the attacker |
| Lua bullets and Lua code | Directly | As asked |
| The player | Kinematic capsule following the player | Walking shoves light things; heavy things don't move (Jolt's character strength) |

## 6. Saves and level changes

- Props are saved as scripted entities: position, angles, velocity, asleep or not. On load they continue.
- Ragdolls are not saved: after loading, a corpse that had a ragdoll lies in its saved pose (the last
  ragdoll pose is copied into the entity before saving).
- Level changes: props don't travel (as in Half-Life 2, unless they're carried).

## 7. Files

| File | Responsibility |
|---|---|
| `third_party/JoltPhysics/` | The library: `Jolt/`, `Build/`, `LICENSE`, `README.md` only (no samples or tests) |
| `src/engine/phys.h` | The C API, shared with the future HL2 map plan |
| `src/engine/phys_jolt.cpp` | The only C++ file: Jolt setup, unit conversion, stepping, bodies, constraints, rays, self-tests (`phys_test`) |
| `src/engine/phys_world.c` | World mesh from the map, brush entities as kinematic bodies, the player capsule, per-frame sync with edicts |
| `src/engine/src_phys.c` | `.phy` reading: convex pieces per bone, solids, ragdoll constraints |
| `src/engine/phys_ragdoll.c` | Dying entities become ragdolls; poses for the renderer; limits |
| `src/engine/lua_phys.c` | `physics.*`, physics objects, `prop_physics` as a built-in scripted entity, `PhysicsCollide` |
| `r_studio.c` | Draw an entity from its ragdoll's bones when it has one |
| `CMakeLists.txt`, `build.bat`, `build.sh`, `tools/make_release.sh` | C++ and Jolt for all three compilers |
| `THIRD_PARTY_LICENSES.txt`, HANDOFF's third-party table | Jolt's MIT licence |

## 8. Build order (each step: both platforms, log + screenshot)

| Step | Deliverable | Check |
|---|---|---|
| 1 | Jolt in `third_party`, C++ in the build for Visual C++ x86, gcc `-m32` and MinGW i686, `phys_test` (a box falls on a plane, no map needed) | `phys_test` prints PASS from Linux and the Windows exe under Wine; c1a0 unchanged |
| 2 | World mesh and brush entities | `phys_dropbox` drops a box in c1a0d; it lands on the floor, rides a lift, is pushed by a door |
| 3 | `.phy` reading | `phyinfo hl2:models/combine_soldier.mdl` lists bones, masses, joint limits; all of HL2's `.phy` files read without errors |
| 4 | Ragdolls | A Source-model soldier shot on c2a5 falls limp; screenshots over two seconds; 20 dead at once stay under the limit and the frame time is logged |
| 5 | Props: scripted entity, box for `hl.dll`, Lua API | A stack of HL2 crates in c1a0d: the player stands on one, shoves another, shoots a third; save/load keeps them |
| 6 | Pushes: explosions, `hl.dll` bullets, the player | A grenade scatters crates and ragdolls; shots push a barrel |
| 7 | Half-Life model ragdolls from hitboxes (if P4 says yes), README, HANDOFF | Owner review |

## 9. Limits after this plan

- Ragdolls are visual: you can't stand on a corpse or hide behind one.
- Props are boxes to `hl.dll`: monsters and the player collide with an upright box round them.
- No physics gun or grabbing (a Lua addon could do it with this API).
- No physics for Half-Life's own movement (player, monsters, grenades, gibs).
- Memory: 32-bit; Jolt's limits become cvars (`phys_maxbodies` etc.), applied at map load.

## 10. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| P1 | Jolt (MIT) as newen's physics library, downloaded from its GitHub releases into `third_party/` | Yes. It's what HANDOFF and the HL2 map plan already recommend, and it's what NewenSource used on 32-bit Windows. The 32-bit MinGW build is the unknown, checked first (step 1) |
| P2 | First version scope: ragdolls for Source models, props, explosion and Lua pushes, player shoving | Yes, as section 1 |
| P3 | Props solid to `hl.dll` as upright boxes | Yes: otherwise the player walks through crates |
| P4 | Ragdolls for Half-Life's own models, built from hitboxes | Yes, but behind a cvar and off by default: guessed joints may look wrong; Half-Life's death animations stay the default |
| P5 | Order of the three plans | Lua API first (damage is needed by both others); then scripted entities (`docs/SWEP_PLAN.md` section 4, which props are built on); then physics (ragdolls fix the Source NPCs' T-pose deaths) and Lua weapons, in whichever order matters more to you. Physics steps 1-4 (Jolt, world, `.phy`, ragdolls) don't need scripted entities, so ragdolls can come early if you want them first |

## Built

- **Step 1 (2026-11-13, `phys-1`)**: Jolt 5.6.0, `phys.h`, `phys_jolt.cpp`, `phys_world.c` (`phys_test`).
  Different from the plan: Jolt is single-threaded for now; only SSE2 is assumed; the C++ runtime is linked
  into the exe on both platforms; Jolt allocates with its own allocator. Details in HANDOFF section 9.
