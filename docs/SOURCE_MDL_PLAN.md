# Source models (MDL 44-48): integration plan

Written 2026-11-12 from newen at commit 38e6a2d. The owner said "start building" the same day.
**Status: steps 1 to 9 and eyes are done on Linux** (see HANDOFF section 9, "Source models"). Left: the
Windows build and checks, and step 10 (the owner's docs review).

What the built steps found, against this plan:
- Step 9: there was no "step 0" in this plan's build order, so no reference pictures from before the work;
  the regression leaned on one older reference shot, repeat runs, loading all content, play sessions and a
  memory-checked build instead. A future plan should take its reference shots first.
- Step 8: a Source base texture's alpha is usually a shine mask, not transparency; uploading it as-is made
  the light shader cut the model out. Shine comes from the bump map's alpha or the base texture's, depending
  on the material.
- Step 7: the missing piece for view models wasn't in the plan: client.dll plays weapon animations by
  sequence number, so a Source view model needs its sequences put in the Half-Life weapon's order (the
  settings file's `order`). Decision M5 (FOV 54) is built as the cvar `r_viewmodel_fov_source`. The muzzle
  flash needed its position corrected for the different field of view.
- Step 6, against section 4.4: head turning is a bone turn (as GoldSrc does it), not `head_yaw`, because
  HL2's humans have no sequence that uses that parameter. Aim blends are mapped only by the settings file
  (an unset blend byte can't be told from "fully up"). The mouth uses the jaw-drop flex after all (4.4 said
  flexes weren't planned): HL2's characters have no jaw bone, so it was that or no mouths. Layered sequences
  were needed for aiming and are built. The sidecar file (4.5) is `modelcfg/<model path>.newen`, not next to
  the model, so files inside Half-Life 2's archives can have one; the view model FOV setting comes with step 7.
- Step 5: attachments and hitboxes went into the stand-in here (planned for step 6); they were a few lines.
  New: activities the model lacks borrow a related sequence, because hl.dll's fallback is sequence 0 (a
  T-pose on HL2's characters). New: `mdl_replace`, the way to give an NPC a Source model without the game
  DLL's help. Source characters have no death animations (they ragdoll), so dying NPCs need the sidecar file
  or a future ragdoll.
- Eyes were listed under "limits" (section 10); the owner asked for them. Built: the iris laid over the
  eyeball, looking straight ahead. Still out: eyes that follow a target, glints, eyelids (flexes).
- Step 4: include files are animation-only models in a shared cache, not ordinary cached models (they have no
  mesh, so `Mod_ForName` can't hold them). Layered sequences turned out to matter for aiming (the Combine
  soldier's aim is a layer), so they belong with step 6. VMT `>=dxNN` blocks had to be read.
- Search order is archives first, then loose files (gameinfo.txt's order): Steam leaves 0-byte placeholder
  `.mdl` files beside the VPKs.
- A minimal stand-in header (one bone, one sequence, the body parts) had to exist from step 2 on, because the
  engine's GoldSrc-shaped code reads `studio.hdr` for every studio model. Step 5 fills it in.
- 16 of HL2's 2209 model names are over 63 characters with the `hl2:` prefix and can't load (`model_t`'s name is
  64 bytes, and client.dll reads that layout).
- Rest-pose vertices of characters are 90 degrees off; the skeleton turns them (done in step 3).
- Step 3: the stand-in header got its bones and sequences here rather than in step 5, so sequence numbers and
  frame timing work through the existing GoldSrc-shaped code. HL2's humans (Barney, Alyx) hold only a `ragdoll`
  sequence of their own; everything else is in include files (step 4). A sequence naming an animation the file
  doesn't hold (one shipped model does) is the rest pose.

## 1. In plain terms

A Source `.mdl` (Half-Life 2's model format) should work **anywhere a GoldSrc `.mdl` works**: as an NPC's model,
the view model in your hands, a player or `p_` weapon model, a map model (`env_model`, `cycler`, a monster's
`model` key) and through Lua (`ent:SetModel`). hl.dll and client.dll stay unchanged and never find out the model is
a different format.

How: newen loads the Source files (`.mdl` + `.vvd` + `.vtx` + materials) for drawing, and also builds a small
**stand-in GoldSrc header** in memory (studio version 10) that describes the same model the way hl.dll expects:
bones, sequences, activities, events, movement, attachments, hitboxes and bodyparts. The DLLs only ever see the
stand-in; the renderer draws from the real Source data.

This also gives the HL2 map plan (`docs/SOURCE_PLAN.md`, step 5) its prop loader, so there's one Source model
loader, not two.

## 2. What exists and what it's worth

| Piece | State | Verdict |
|---|---|---|
| NewenSource `source_models.c` / `source_assets.c` | Static MDL/VVD/VTX, VPK, VMT, VTF loaders | The owner OK'd reuse (2026-11-12), but the project can't be found: it isn't on the Linux machine (all mounted drives searched by name, 2026-11-12). **Written fresh** instead, under the same rule as SOURCE_PLAN's D3: Valve's public SDK headers and community format docs only for offsets and sizes, newen's own names and structs. If NewenSource turns up later, it's a cross-check, not a source. SOURCE_PLAN's "reuse" verdicts for these two files change the same way |
| newen `model.c` / `r_studio.c` | GoldSrc loader, bone setup, CPU skinning, lighting, chrome, render modes; modern renderer hooks (`SetupLighting`, `R_StudioCasterGeometry`) | Unchanged for GoldSrc models. A Source branch at the top of the entry points |
| Installed HL2 (Linux Steam, checked 2026-11-12) | `hl2/hl2_misc_dir.vpk` (VPK v2) holds 2209 models: 2114 v44, 6 v45, 9 v46, 2 v47, 78 v48. No v49 | v44-48 covers all of HL2. NPC animations live in separate files: `barney.mdl` (v44) pulls `barney_animations.mdl`, `barney_gestures.mdl`, `barney_postures.mdl` (v48) through `$includemodel`; `combine_soldier.mdl` (v46) uses `combine_soldier_anims.mdl` (v48) |

## 3. Every place a model is used, and what a Source model needs there

| Use | What reads the model today | For a Source model |
|---|---|---|
| NPCs (hl.dll monster code) | `pfnGetModelPtr` (`sv_engfuncs.c` `PF_GetModelPtr`) hands hl.dll the raw `studiohdr_t`; it reads sequences, activities, events, movement, bodygroups, controllers, blending | Returns the stand-in header. See section 4 |
| Engine bone queries | `PF_GetBonePosition`, `PF_GetAttachment`, `PF_AnimationAutomove` | Go through the Source bone setup |
| Model events | `CL_StudioEvents` (`cl_tent.c`) reads the header's events and calls client.dll's `HUD_StudioEvent` (muzzle flash, shells); the server side runs hl.dll's `HandleAnimEvent` | Events copied into the stand-in (section 4.3) |
| View model | `pev->viewmodel` → `CL_DrawViewModel` | Same path. Source view models are made for a narrower FOV (HL2 uses 54); a per-model setting in the sidecar plus a cvar `r_viewmodel_fov_source` (default 54) |
| Map models | `r_ents.c`, `SV_SetModel`, `MakeStatic` | Free once `Mod_ForName` loads Source models |
| Lua | `ent:SetModel`, `LookupSequence`, `util.PrecacheModel` | Free through `Mod_ForName` / `R_StudioLookupSequence` |
| Modern renderer | `SetupLighting`, `R_StudioCasterGeometry`, normal/spec maps | Source branch inside each; VMT `$bumpmap` as the normal map |
| Hitboxes | The loader checks them, but traces don't use them today (`sv_world.c` traces hulls only), for GoldSrc models either | The stand-in gets the default hitbox set, so the day hitbox traces arrive, Source models get them too |

## 4. The stand-in GoldSrc header

Built once per model at load time, kept next to the Source data, freed with the model. `model_t` stays
`mod_studio` (client.dll reads `model_t::type`), `studio.hdr` points at the stand-in, and a new `studio.source`
points at the Source data. `r_studio.c` checks `studio.source` first.

### 4.1 What goes into it
- **Bones**: names, parents, and default positions/rotations. Source models may have up to 128 bones (as GoldSrc).
- **Sequences**: name, fps, frame count, loop flag, activity number and weight, events, bounding box, linear
  movement, one blend. Animation data isn't copied; the stand-in's animation offsets point at a zeroed block, and
  newen's own bone setup reads the Source animation.
- **Attachments**: bone and position (Source stores a full matrix; the renderer keeps it).
- **Hitboxes**: Source's first hitbox set, with hitgroups.
- **Bodyparts and models**: names and counts so `SetBodygroup` works; zero vertices and meshes.
- **Textures and skins**: one placeholder texture per skin family, so skin numbers range-check.
- **Bone controllers**: made up from pose parameters (4.4).

### 4.2 Activities
GoldSrc stores activity numbers (the HL SDK's `activity.h` list, which newen would carry as a name table); Source
stores names (`"ACT_IDLE"`). Mapping by name: same name → same number. Names HL doesn't have get 0 (no activity),
which hl.dll treats like a model without that activity. The sidecar file can rename them (`ACT_RUN_RIFLE` →
`ACT_RUN`).

### 4.3 Events
v44-48 events carry a number and an options string; numbered ones (5001 muzzle flash etc. are the same numbers in
both games) are copied. Named events (`AE_...`) get a small table or are dropped, listed once by `mdlinfo`.

### 4.4 Controllers, blending and pose parameters
GoldSrc has 4 bone controllers, a mouth controller and 2 blend values. Source replaced all of them with named pose
parameters. Default mapping by name:
- blending 0/1 → `aim_pitch`/`aim_yaw`, else `body_pitch`/`body_yaw`
- controller 0 → `head_yaw`, controller 1 → `head_pitch`
- mouth → a `jaw`/`Jaw` bone if present, opened by the mouth value; else nothing (Source mouths are facial flex
  animation, which isn't planned)

The stand-in's controllers get the pose parameter's range, so hl.dll's `SetBoneController` converts degrees the same
way it does for GoldSrc.

### 4.5 Sidecar file
Optional `models/<name>.newen` next to the model (or in `bin/userdata`/`base`), plain key/value text: controller
and blend mappings, activity renames, view model FOV, scale. Original files are never changed, matching newen's
"extensions, not edits" rule.

## 5. Where the files come from

HL has its own `models/barney.mdl`, so HL2's files can't simply join Half-Life's search paths (SOURCE_PLAN keeps
them apart for the same reason). Proposal:
- A Source model is detected by its contents (version 44-48), not its path, so a Source `.mdl` dropped loose into a
  mod's `models/` folder just works. Its `.vvd`, `.vtx`, `$includemodel` files and `materials/` come from the same
  Half-Life search paths.
- HL2's own content is reached with a prefix: `hl2:models/barney.mdl`. That looks in HL2's loose files and VPKs
  (`src_fs.c`, found through Steam or `-hl2dir`). Inside an `hl2:` model, its companions and materials also come
  from HL2. Nothing unprefixed ever falls through to HL2.
- **To confirm in step 4**: hl.dll and client.dll treat the model name as a plain string up to 64 characters, so the
  colon is safe. If it isn't, use a folder prefix (`hl2/models/...`) instead.

## 6. Animation (the biggest new part)

All new code. Needed, in order:
1. Bone default pose and the per-bone pose-to-bone matrices (Source vertices are in model space with up to 3
   bone weights each).
2. Animation descriptions: per-bone data with rotation as compressed quaternions (48- and 64-bit) or Euler RLE
   values, positions as 48-bit half floats or RLE values; delta animations; v48 animation sections and `.ani`
   animation blocks.
3. Sequences: frame → cycle, looping, blend grids over 1-2 pose parameters (bilinear between the grid's animations).
4. `$includemodel`: load the referenced MDLs (cached as ordinary models), match bones by name, and add their
   sequences to the main model's list.
5. Layers and autolayers (gestures, postures) only where HL2 NPC idles need them; otherwise left out.

Each step is checked with `mdlview` screenshots at two frames.

## 7. Files

| File | Responsibility |
|---|---|
| `src/engine/src_fs.c` | (shared with SOURCE_PLAN) finding HL2, VPK v1/v2, `SrcFS_LoadFile`, the `hl2:` prefix |
| `src/engine/src_material.c` | (shared) VMT and VTF, material cache, missing → checkerboard |
| `src/engine/src_studio.c` | MDL/VVD/VTX loading, skins, bodygroups, `$includemodel`, the stand-in header, sidecar file |
| `src/engine/src_anim.c` | Source animation decoding, pose parameters, blend grids, bone setup |
| `model.c` / `model.h` | `studio.source` pointer; `Mod_ForName` sends v44-48 files to `src_studio.c`; `mdlinfo` |
| `r_studio.c` | Source branch in setup/draw/caster/lighting; CPU skinning with 3 weights |
| `sv_engfuncs.c`, `cl_tent.c`, `cl_studio.c` | Bone position/attachment/automove and events through the Source path where needed |

All pure C11, no platform code, so Windows and Linux stay in step.

## 8. Build order (each step: build on both platforms, run headless, read the log, look at the screenshot)

| Step | Deliverable | Check |
|---|---|---|
| 1 | `src_fs.c` (VPK v2 as HL2 ships, v1 too; `hl2:` prefix) and `src_material.c` (VMT with `include`, VTF 7.x: DXT1/3/5, RGBA/BGRA/RGB/I8) | `srcfs_find hl2:models/barney.mdl` finds it; entry count logged per VPK; a VTF shows in a debug view |
| 2 | `src_studio.c`: static mesh, skins, bodygroups; `mdlview hl2:models/props_c17/oildrum001.mdl` | Screenshot; `mdlinfo` on all 2209 HL2 models: every one loads or gives a clear reason |
| 3 | `src_anim.c`: bones, own animations, 3-weight skinning | `mdlview hl2:models/zombie/classic.mdl` at two frames differs and looks right |
| 4 | `$includemodel`, blend grids | `mdlview hl2:models/barney.mdl` walking; `mdlview hl2:models/combine_soldier.mdl` |
| 5 | The stand-in header, activities, events, movement | `cycler` with a Source model; `monster_barney` with `hl2:models/barney.mdl` (via `+setmodel`-style Lua) idles, follows and walks without a crash; log shows activities found |
| 6 | Controllers, blending, mouth fallback, sidecar file | Barney aims at you and turns his head in screenshots |
| 7 | View model: events to client.dll, view model FOV | `hl2:models/weapons/v_smg1.mdl` swapped in by Lua, firing: muzzle flash and shells in a screenshot |
| 8 | Modern renderer: `$bumpmap` normal maps, `$phong` → specular, shadows | Shadow screenshot of a Source NPC under the flashlight |
| 9 | Regression | c1a0, c2a5, c1a0d pixel-identical to before; every GoldSrc model in `valve/` still loads (`mdlinfo` all) |
| 10 | README/HANDOFF update with the limits | Owner review |

## 9. Failing gracefully

| Situation | Behaviour |
|---|---|
| `.vvd`/`.vtx` missing or checksum mismatch | Model is `mod_bad`, one warning (as GoldSrc models today) |
| Version outside 44-48 | "Source model version N isn't supported", `mod_bad` |
| `$includemodel` file missing | Model loads with its own sequences only, one warning |
| Material missing | Checkerboard, counted in `mdlinfo` |
| HL2 not found and an `hl2:` path is used | "Half-Life 2 not found; start with -hl2dir <folder>", `mod_bad` |
| Unknown animation encoding | That sequence holds the default pose, one warning naming it |

## 10. Limits after this plan

No facial flex animation (mouths only through a jaw bone), no eyeballs that look at you (eyes draw as plain
meshes), no IK (feet don't adapt to slopes), no jigglebones or other procedural bones, LOD 0 only, no material
proxies (animated textures), no `.phy` collision, no bone-merged hands (`c_arms`). No hitbox traces, the same as
GoldSrc models today.

## 11. Decisions for the owner

| # | Decision | State |
|---|---|---|
| M1 | Scope: Source models anywhere a GoldSrc model works | Owner, 2026-11-12 |
| M2 | Reuse NewenSource's model and asset code | OK'd by the owner, but the code can't be found; written fresh (2026-11-12) |
| M3 | Test content | HL2 installed (Linux Steam), owner 2026-11-12 |
| M4 | Where files come from: loose files by contents, HL2 only through `hl2:` | Recommended, not confirmed |
| M5 | Default view model FOV for Source view models (54) | Recommended, not confirmed |
| M6 | Order with SOURCE_PLAN: this plan's steps 1-2 also cover SOURCE_PLAN steps 3 and part of 5 | Recommended: do this first, SOURCE_PLAN reuses it |
