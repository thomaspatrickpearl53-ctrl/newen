# Renderer plan: optional modern lighting

**Status 2026-10-29:** the owner accepted every recommendation in section 8. Steps 0-7 are implemented (the owner
reviewed 0-5); see HANDOFF.md section 9 for what was built, how it was verified and where it departs from this plan
(the base pass stayed fixed-function; no material descriptor yet). Next: step 8 (masked and moving brush casters).

Design only, based on the current source inspected on 2026-10-20. None of the proposed renderer
features, commands or cvars below are implemented by this document. The only change authorized for
this task is this file; implementation needs a separate go-ahead.

## 1. Goal and recommended approach

Keep the existing renderer as the default, selected by `r_renderer classic`. Add an optional
`r_renderer modern` path with per-pixel dynamic lighting, normal/specular materials and shadow maps.
Use forward shading alongside the existing compatibility OpenGL drawing. Preserve baked lightmaps,
palette-derived textures, animation, render modes and the HUD instead of making stock maps depend on
new assets or a new compiler.

Recommended first target: the existing OpenGL 2.1 context, GLSL 1.20, and explicitly checked framebuffer
object support for shadows. Do not request a core-profile context: immediate mode, matrix stacks,
client arrays and fixed-function texturing are used throughout the engine and client interfaces.
This is a shader-based rendering path, not a complete migration to a newer graphics API.

Start with LDR output, no bloom, tone mapping, automatic exposure, ambient occlusion or replacement
sunlight. Use a flat normal and zero specular by default. Stock content retains its authored texture
and baked lighting character; additional materials and dynamic-light shadows are opt-in through the
modern path. Modern is not promised to be pixel-identical to classic.

## 2. What the code does today

The source is authoritative where README/HANDOFF summaries are stale.

| Current file/function | Observed behavior and consequence |
| --- | --- |
| `src/engine/vid_sdl.c`, `Vid_Init` | Requests GL 2.1, double buffering, 24-bit depth and 8-bit stencil. `vid_restart` resizes/reconfigures the window; it does not recreate the context. |
| `src/engine/image.c`, `GL_LoadExtensions` | Loads multitexture and VBO entry points through SDL. No shader or framebuffer loader exists. Windows `opengl32` exports only the old entry points, so modern calls also need loading. |
| `src/engine/r_main.c`, `R_RenderView` | Sets the compatibility matrices, captures them for the client, collects server/client entities, then draws world, models/sprites, effects/client triangles, and finally the view model. |
| `src/engine/r_world.c` | PVS/frustum traversal and batches by diffuse texture/lightmap page. Uses fixed-function texture modulation and world VBO/client arrays. Sky and water have separate immediate-mode paths. Brush entities have local transforms and render modes. |
| `src/engine/bsp.h`, `bsp.c` | `worldvert_t` contains position, diffuse UV and lightmap UV, but no normal/tangent. Lightmaps are RGB samples at 16-unit spacing, up to four styles per face, packed into 1024-square atlases. No directional lighting data is loaded. |
| `src/engine/r_light.c` | Up to 32 renderer lights, indexed by a surface bitmask. CPU dynamic lighting is added to lightmap samples; model lighting adds dynamic light at a sampled point. No shadow renderer exists. |
| `src/engine/cl_tent.c` | Owns a stable 32-slot DLL-facing dynamic-light pool. `CL_AllocElight` currently shares the dlight allocator. The flashlight is a trace-hit point light, not a projected cone. `CL_AddEntities` currently skips its light update without a client DLL. |
| `src/engine/r_studio.c` | CPU bones/skinning and per-normal lighting feed position/UV/color arrays. Static light comes from `R_LightPoint`, with the existing studio ambient/shade/lambert controls. The engine draws MDLs; the client studio renderer does not. |
| `src/engine/image.c`, `GL_UploadTexture` | Applies `texgammatable` to RGB whenever `TF_WORLD` or `TF_MODEL` is set. Those flags also control lifetime. Normal/specular data cannot use this unchanged. |
| `src/engine/cl_tri.c` | Triangle API directly calls `glBegin/glEnd`, color/UV/vertex functions and fixed fog. Projection helpers depend on captured compatibility matrices. |
| `src/engine/draw.c`, `cl_main.c`, `vgui_bridge.c` | HUD, fonts, console and VGUI use fixed-function drawing and alpha textures. `Draw_Begin2D` does not currently reset shader/FBO state because none exists. |
| `src/engine/r_world.c`, `r_speeds` | Currently prints surfaces, batches and lightmap update counts, not CPU/GPU timings. |

The classic dynamic-light contribution is summed before the lightmap brightness lookup. Simply
turning on a shader while retaining those uploads would apply dynamic lighting twice. The modern
path must use a baked-and-lightstyle-only base, with a separate dynamic-light calculation.

## 3. Classic preservation and selection

1. Add archived string cvar `r_renderer`, default `classic`; accept only `classic` and `modern`.
   Report both requested and effective renderer. Unknown values keep the active renderer and print
   usage. Capability or shader failure prints a specific reason and selects effective classic.
2. Initially apply selection at the next map load. Say so in the cvar help and log. This avoids
   changing lightmap cache ownership in the middle of a frame or resetting the game just to switch.
   Switching modes without a map reload is deferred.
3. Leave the classic context request, gamma tables, texture upload bytes, dynamic-light arithmetic,
   lightmap atlas contents, model-light defaults and draw order intact. Classic does not require
   shader compilation, new material files or shadow allocations. Optional feature probing must not
   make initialization fatal on a classic-capable driver.
4. Keep modern geometry/material caches separate from classic structures where practical. Modern
   lightmaps must not reuse a classic atlas containing transient dynamic light. Allocate the selected
   path's resources on map load and release them on unload; keep clear ownership and cleanup on errors.
5. Establish exact before/after classic screenshot comparisons on the same GPU, driver, resolution,
   configuration and frozen scene. Any changed pixel in a deterministic scene is a regression to
   investigate, including changes introduced by shared loaders. Cross-driver byte identity cannot be
   guaranteed. Do not substitute approximate shader emulation for retaining the old path.

## 4. Proposed modern frame

### Geometry, materials and lighting

Retain existing world traversal and visibility for camera rendering. Build a modern-only stream
with position, diffuse UV, lightmap UV, outward normal and tangent plus handedness. Derive BSP normals
from the plane and `SURF_PLANEBACK`. Derive tangents from texture axes projected onto the face;
orthogonalize, preserve mirrored UV handedness, and handle degenerate axes with a stable fallback.
Use the original miptex dimensions for UVs even when the replacement image is larger.

Keep CPU model animation initially. Supply skinned normals and per-corner UV tangents in a modern
stream; seams, mirrored UVs, degenerate triangles and the MDL's separate vertex/normal bone indices
need explicit handling. Form tangents from the deformed triangle positions and UVs as a correctness
baseline; optimize only after profiling. Do not introduce GPU skinning as a prerequisite.

Use a base shader for albedo multiplied by the existing baked/style illumination. Modern excludes
`R_SurfaceDlights` and `R_DlightPoint` from this base. Add dynamic diffuse and modest Blinn-Phong
specular per fragment. The specular map controls strength; a material scalar controls gloss exponent.
Missing normal maps use (0, 0, 1) in tangent space; missing specular maps have strength zero. Respect
fullbright, chrome, masked and additive model flags; chrome retains its existing UV behavior and
does not silently become an environment-reflection material.

For the first dynamic shader, draw one additive pass per affecting light with depth writes disabled,
`GL_ONE, GL_ONE` blending and material alpha tests identical to the base. Cull light/surface pairs by
radius and bounds; add screen scissor bounds when correct. This avoids GLSL uniform-array limits
and gives an easy one-light reference image. Then group a small, hardware-validated number of lights
per pass if `r_speeds` proves draw-call cost is material. CPU light lists grow with `Mem_*`; a shader's
finite capacity produces extra passes rather than silently losing lights. Explicit cvars budget
shadow-casting lights and memory, not arbitrary fixed scene arrays.

The initial color calculation deliberately operates in the existing displayed-value convention,
using the current gamma-adjusted albedo and baked base. It is an artistic LDR model, not physically
linear lighting. Do not enable framebuffer sRGB or apply another gamma pass. Proper linear/HDR
rendering requires separate raw texture/lightmap resources and explicit compositing decisions;
consider it only as a later modern feature. Normal/specular data always bypass color gamma.

BSP30/NBSP lightmaps have no incident direction. Normal/specular maps initially affect dynamic
lights only. Baked illumination stays baked: no convincing static-light normal response or dynamic
shadowing of baked sunlight can be recovered from RGB lightmaps alone. Future directional BSPX data
or explicitly authored real-time lights would be a separate format/compiler design.

### Shadow maps

Start with one controllable point light and opaque world casters. Render its six perspective views
into six tiles of a depth-texture atlas using an FBO; select the appropriate tile in the lighting
shader. Six 2D tiles with manual depth comparison work without requiring cube-shadow sampler
features beyond the proposed shader baseline. Start at 256 pixels per face, then expose resolution
and memory budgets. Check actual atlas dimensions against GPU limits before allocating.

Use a depth-only shader and matching alpha discard for cutout casters. Compare against the projected
depth convention actually stored, add slope/normal-aware bias, and then small PCF filtering. Guard
tile borders and sampling footprints to prevent light leaking between tiles; test cube-face seams.
Fit near/far planes to each light radius, not the entire map. A light inside solid geometry should
be rejected or visibly diagnosed rather than assigned an unexplained position offset.

Apply shadow visibility only to that dynamic light's diffuse/specular contribution. Never darken
the baked lightmap wholesale: its occlusion is already baked and it cannot identify which light
contributed. No soft area shadows, cascaded sunlight or translucent colored shadows in this scope.

Caster lists must be built for the light, independent of the camera frustum/PVS: an off-screen
monster or door can cast a visible shadow. Start conservatively with light-radius BSP bounds tests;
optimize with light visibility only when it is proven conservative. Moving/rotating brush transforms
and skinned model positions must match the main pass. Exclude water, sky, sprites, beams, particles,
additive/translucent entities, client triangles and the first-person view model from initial casters.
Opaque/masked world geometry, doors and MDLs become supported in separate steps.

Only a budgeted subset of lights casts shadows. Stable light identities, priorities and hysteresis
avoid flickering selection. Start with one light; propose a default budget of two point lights after
measurement, with short muzzle flashes unshadowed by default. Dynamic lighting still works for
unshadowed lights. Reuse static caster results only with an explicit invalidation policy; doors,
animation and changing light origins invalidate affected results.

### Compatibility drawing and composition

The main view still renders directly to the window back buffer; FBOs are initially only for shadow
depth. Complete each opaque object's base and dynamic passes before later decal/transparency stages.
Keep existing sky, water, sprite, particle and beam paths initially, with program zero and the same
view/depth buffer. Preserve current translucent order; transparent modern materials can retain
classic lighting until a dedicated, single-composition lit shader exists. Do not additive-redraw
alpha-blended surfaces as if they were opaque.

At each legacy boundary, explicitly restore program zero, default framebuffer, draw/read buffers,
viewport, color mask, depth function/write mask/range, culling, blend/alpha/fog state, texture bindings
and active/client-active texture units, array/VBO bindings and matrices. Shader attributes must be
disabled. Do not assume `glPushAttrib` restores programs, FBOs or vertex attributes. Avoid repeated
`glGet*` round trips per surface; use explicit pass setup and a debug-only state audit.

Call client triangle callbacks once in their existing relative order, never during shadow or
per-light passes. Maintain compatibility matrices and `CL_CaptureViewMatrices` before callbacks;
shadow cameras must not overwrite the saved main view. Retain `CLIENT_CALL` crash guards and all
triangle API signatures. Their fog/color/brightness/render modes continue to work in program-zero
drawing. Arbitrary client triangles initially neither cast nor receive modern lighting. Moving to a
core profile would require a separate buffered triangle adapter and complete UI/effects migration.

Render the view model with its existing 0..0.3 depth range and restore 0..1 afterward. Introduce modern
view-model lighting only after world MDLs work; initially it can use the classic path and casts no
world shadow. Enter `Draw_Begin2D` in a known legacy state. HUD sprites, VGUI, fonts, Lua overlays,
screen fades and console retain existing brightness/blending and are never shadowed or tone mapped.
Screenshots continue to read the completed back buffer after UI drawing.

## 5. File-by-file implementation map

These are future edits, not files changed by this plan. Keep new code C11, tabs, `Mem_*` allocations
and growable lists. No game-DLL ABI changes, SDK code copying or Xash3D code are required.

| File | Planned responsibility/change |
| --- | --- |
| `src/engine/vid_sdl.c` | Retain GL 2.1 context; report actual version/GLSL support. Ensure default-target viewport/read buffer for screenshots after shadow rendering. Do not turn `vid_restart` into a context reset incidentally. |
| `src/engine/gl_local.h` | Add loaded shader, uniform/attribute, FBO and optional timer-query declarations, capability records and renderer-private pass interfaces. No change to DLL tables. |
| `src/engine/image.c` | Load optional modern entry points without weakening classic checks. Separate data-texture gamma policy from world/model lifetime flags; preserve classic uploads exactly. Add modern material texture cleanup and explicit render-target filtering. |
| New `src/engine/r_shader.c` | Compile/link GLSL, validate required attributes/uniforms, log complete diagnostics and filenames, own programs and safe fallback. Restore usable classic state on partial failure. |
| New `src/engine/r_modern.c` | Modern pass orchestration, geometry streams, per-light draw submission and explicit legacy-state boundaries. Keep classic implementation recognizable. |
| New `src/engine/r_shadow.c` | Atlas/FBO allocation, caster collection, six-face rendering, bias/filter parameters and resource budgets. No client callbacks here. |
| New `src/engine/r_material.c` | Optional material lookup, suffix policy, fallback normal/specular textures and per-material gloss/normal settings. Cache missing files and warnings. |
| `base/shaders/modern/*` (new) | GLSL 1.20 base, dynamic-light and depth/masked-depth sources, loaded through the existing filesystem. Include names in shader errors; existing base-copy build step distributes them. |
| `src/engine/r_main.c` | Register/select `r_renderer`; initialize/free backend resources; choose path at map load; preserve camera, entity collection and main-view matrix capture. Ensure cleanup before map-owned textures/models disappear. |
| `src/engine/r_world.c` | Retain classic draw functions; expose modern-visible surface/material batches and brush transforms. Modern updates only baked/style lightmaps. Add counters and keep sky/water/legacy render modes isolated. |
| `src/engine/bsp.h`, `bsp.c` | Expose modern-only normal/tangent geometry and baked-only lightmap construction without altering disk structs or the classic builder's arithmetic. Reset backend-specific caches on reload. Existing BSP30/NBSP loading stays compatible. |
| `src/engine/r_light.c` | Keep classic bitmask path intact; expose immutable per-frame modern lights with stable identities and bounds. Share light inputs, not classic pre-lit atlas pixels. |
| `src/engine/cl_tent.c` | Feed modern light metadata including source/key for shadow selection. Preserve DLL-facing stable light pointers; no reallocating the exported pool. Keep classic flashlight behavior; projected flashlight is a later explicit choice. Separate engine-origin light updates from client-only events if modern lighting is required without a client DLL. |
| `src/engine/r_ents.c` | Expose opaque/masked render entities for modern draws and light-specific caster traversal. Preserve translucent sorting. Camera-culled models must remain available to shadow traversal. |
| `src/engine/model.h`, `model.c` | Store modern MDL per-corner geometry/material associations outside DLL-visible layouts; use existing texture replacement roots; free with model cache. Retain CPU animation and original model formats. |
| `src/engine/r_studio.c` | Reuse animated positions/normals for modern base/light/depth passes. Avoid classic dynamic point-light addition in modern base; retain existing static studio lighting and material flags. Cache deformation once per entity/frame for shadow reuse. |
| `src/engine/r_decal.c` | Preserve clipping and polygon offset; first use explicit fixed-function state and baked/style illumination on modern surfaces. Later add dynamic-light modulation without double blending. Decals never become separate shadow casters. |
| `src/engine/r_sprite.c`, `r_part.c`, `r_beam.c` | Initially preserve drawing and make entry/exit state explicit; no material/shadow migration required. |
| `src/engine/cl_tri.c`, `cl_tent.c` | Keep direct triangle API and fog valid in legacy sections; guard callback state, preserve main-view projection helpers and invoke each callback once. No ABI change in `client_api.h`. |
| `src/engine/cl_main.c`, `cl_view.c` | Audit HUD, fades and view-model state boundaries, including depth range. No HUD brightness/material changes. |
| `src/engine/draw.c`, `vgui_bridge.c`, `host.c` | Establish clean 2D state before HUD/VGUI/Lua/console. Preserve host ordering and font behavior; most VGUI drawing should need no internal rewrite. |
| `CMakeLists.txt` | Register new C files. Existing `base` copy distributes shaders; shader-only edits need an explicit copy/reload because the post-build copy runs on linking. |
| `README.md`, `HANDOFF.md` | During implementation, document only completed options, limitations and verified hardware. No changes to them in this design task. |

Materials proposal: `textures/<name>_n.png` and `_s.png` next to the existing world replacement;
`models/textures/<model>/<texture>_n.png` and `_s.png` for MDLs. Reuse normalized diffuse UVs regardless
of image resolution. Define tangent-space normals as +Y/OpenGL convention and allow a material Y-flip;
never guess based on a filename. Specular RGB is strength (initially grayscale recommended); gloss
is an explicit scalar. A small optional material descriptor can override paths/strength/exponent
and disambiguate real diffuse names ending in `_n`/`_s`. Final descriptor syntax is a decision, not
part of the existing format. Animated `+` textures resolve auxiliary maps for the selected frame.

## 6. Buildable, screenshot-testable increments

Every step ends with `build.bat`, a run that quits itself, log inspection, a classic regression image,
and the indicated modern image. A failed step is fixed before expanding scope. Any debug lights,
material overrides or shader debug views below must be explicitly introduced in that step; there
is no existing Lua dynamic-light authoring API to assume.

| Step | Small deliverable | Independent acceptance image/check |
| --- | --- | --- |
| 0 | Capture pre-change reference images/configuration and `r_speeds` counts; define repeatable fixed camera/time fixtures. | Classic c1a0d lab/scientists and c2a5 canyon; save actual pixels and logs, not just a claim. |
| 1 | Renderer selection/capability report and safe fallback; modern temporarily draws classic. Add a debug forced-failure option. | Both selections match baseline; forced unsupported path still renders HUD/world and explains fallback. |
| 2 | Shader loader and modern opaque-world base shader using existing VBO attributes. Other surfaces remain classic. | Modern static world closely matches base; classic remains pixel-identical. Deliberately invalid shader falls back cleanly. |
| 3 | Modern baked-only lightmap updates, normals and one controllable unshadowed point light. | A moving light shades each pixel on world geometry; turning it off leaves no stale lightmap patch. Lightstyle animation still works. |
| 4 | Modern moving/rotating opaque and masked brush entities. | The c1a0d door moves through a fixed light with correct local transforms and alpha holes. |
| 5 | Normal/specular world materials with no-color-gamma data upload and neutral fallbacks. | Same wall flat vs authored normal under one moving light; specular changes with view. Missing maps preserve matte lighting; mirrored UV fixture has no inverted bump. |
| 6 | Modern opaque/masked MDL dynamic lighting using CPU skinning; then MDL normal/specular support as a separate substep/build. | Scientist/headcrab under a fixed light, frozen and animated; seams/bodygroups/skins/chrome/fullbright remain correct. |
| 7 | One-light six-tile depth atlas, opaque static world casters and a depth-preview debug mode. | Corner occludes a debug light; all six directions/seams tested. HUD and view projection remain correct after the depth pass. |
| 8 | Masked world and moving brush casters. | Fence casts holes; moving door opens/closes its shadow, including while the door itself is off-screen. |
| 9 | Animated opaque/masked MDL casters, with shared per-frame pose data. | Scientist/headcrab shadow follows animation without lag; off-screen model still casts into the visible room. |
| 10 | PCF/bias tuning and budgeted multiple shadow lights; optional light-pass grouping based on measured cost. | Overlapping colored lights retain independent shadows; budget overflow degrades to unshadowed light without disappearing lights or rapid selection flicker. |
| 11 | Compatibility completion: decals, render modes, water/sky, view model, client triangles and UI across reloads. Add modern lit view model only as its own substep. | Bullet decals, egon/gauss beams, explosion particles, masked fence, HUD/console/VGUI menu, fades and resize all remain correct; classic switch-back has no leaked state. |

Steps 1-6 already deliver usable modern per-pixel lighting; shadows do not block that milestone.
Do not bundle a projected flashlight, HDR, static-light replacement or GPU skinning into these steps.

## 7. Verification, performance and risks

### Repeatable images and settings

Use c1a0d at `setpos -2250 1136 -220 8 180 0` for scientists, textured walls and model shading;
c2a5 at `setpos 1856 300 350 20 200 0` for outdoor geometry, sky and larger light bounds. Use the
c1a0d moving door for transformed brushes. Confirm the camera leaf is not solid with `viewpos`.
Add a small engine-owned fixture for mirrored UVs, cutouts and six shadow directions when existing
maps cannot isolate a case. No edits to the Half-Life install or external compiler repository.

Example existing screenshot command, with renderer selection added only after step 1 exists:

```text
bin\newen.exe +r_renderer classic +map c1a0d +setpos -2250 1136 -220 8 180 0 +wait 30 +screenshot +wait 1 +quit
```

`wait` counts frames, not seconds. Animation, RNG, lightstyles, view bob and game logic prevent exact
comparison unless frozen/controlled; step 0 must create a reproducible fixture rather than comparing
two independently advancing games. Never freeze only the drawing while letting logic move casters.
Convert TGA outputs with ffmpeg and retain side-by-side/difference PNGs. Use separate static material,
dynamic-light, shadow and compatibility captures so a brightness change cannot hide a shadow bug.

Back up original config bytes before runs, restore every changed cvar before quit where possible,
and restore the original file afterward. Runs rewrite config/logs and basedir cache; transitions can
autosave. Avoid transitions when not needed, uniquely name fixtures and report remaining artifacts.
Keep primary classic/modern performance comparisons with the same client DLL and scene. Also test
`-noclientdll` using a debug engine light; current client light collection cannot be assumed to work
there. Test disconnect, map reload, missing material, compile failure, low capability, resize and
switch-back without editing owner settings permanently.

### Speed and GPU support

- Measure current `r_speeds` first: surfaces, batches and lightmap updates. Extend it with modern
  light-pass draws, light/surface pairs, shadow faces/draws/casters, shadow updates, material switches,
  atlas bytes, CPU collection/draw times and active shadow budget. Distinguish static-style lightmap
  uploads from dynamic uploads; modern should remove the latter.
- `r_speeds` is currently counters, not a GPU benchmark. Use `host_speeds` for broad frame/logic/render
  CPU measurements and optional asynchronous GPU timer queries when supported. Read results in later
  frames; never add per-frame `glFinish` to production drawing. Report timings as unavailable when the
  extension is absent. Query support is optional, not a renderer requirement.
- Propose 60 fps at 1280x720 as the initial target on the documented RTX 3050, subject to owner choice.
  Compare warmed-up median and 95th-percentile frame times over the same recorded/fixed workload,
  capped vs uncapped with vsync/FPS settings explicitly recorded. Baseline, one light, multiple lights,
  and one/two shadow lights need separate numbers. No performance claims are established by this plan.
- Six faces per point light multiply caster cost. Start at low resolution and a small shadow budget;
  cull by radius, reuse pose data, and measure before caching shadows or building a tiled/deferred
  renderer. Many overlapping lights stress fill rate and draw calls; large maps stress caster scans.
  Bound shadow memory and extra CPU streams because the process is 32-bit.
- Shader capability depends on the actual driver, GLSL compiler, available attributes/texture units,
  depth renderability and FBO completeness, not the version string alone. Check the full used function
  set. Require at least the bindings the chosen shader variant needs (albedo, lightmap, normal,
  specular and shadow atlas); reduce variants or fall back with an explicit reason.
- GL 2.1 alone does not guarantee the required FBO path. Prefer the ARB/core FBO entry points when
  exposed; support EXT FBO through a deliberate adapter only if needed for target hardware. Never mix
  names or assume identical API coverage. Failed shadow allocation can select unshadowed modern with
  a clear warning; failed essential shaders select classic. Test both paths. The RTX 3050 is the only
  documented tested GPU, not evidence for Intel/AMD or software drivers.

### Other visual risks

- Alpha thresholds, front-face winding and transforms must agree between base, light and shadow
  passes. Wrong thresholds produce solid fence shadows; wrong bias produces acne or detached shadows.
- Near/far precision and large NBSP coordinates need explicit tests. Modern can calculate light/view
  vectors relative to a nearby origin for precision; this must not rebase gameplay or change classic
  matrices. Avoid giant shadow frusta covering an entire extended map.
- Legacy passes can inherit a shadow viewport, color mask, program or texture unit. Make UI/client
  boundary images mandatory at every stage, not a final cleanup exercise. `glGet`-based projection
  helpers must continue to see the main camera.
- Auxiliary maps need deliberate mip generation/filtering (normal renormalization and specular
  aliasing checks). Content authored for the opposite normal Y convention needs an explicit switch.
- Baked lightmaps already contain shadows; adding new copies of all map lights would double-light
  the scene. This plan shadows existing/new dynamic sources only. Static-light conversion needs a
  separate authoring and bake policy.

## 8. Decisions for the owner before implementation

| Decision | Recommendation | Tradeoff/alternative |
| --- | --- | --- |
| GL baseline | Keep GL 2.1 compatibility + GLSL 1.20; require checked FBO/depth support for shadows. | A GL 3.3 compatibility minimum simplifies later features but drops older drivers. A core profile is a much larger migration. |
| What classic means | Preserve this engine's current pixels/defaults, including tuned gamma and current quirks. | Matching original GoldSrc more closely would be a separate task and would change the baseline. |
| Modern appearance | Baked Half-Life base, matte defaults, optional normal/specular maps and restrained dynamic lights. | Linear/HDR/PBR materials need different color handling and broader visual approval. |
| Shadow scope/default | Dynamic point lights; start with one, then budget two after profiling. Opaque/masked world, brushes and MDLs. | Shadow every muzzle flash or add sunlight/transparent shadows at substantially higher cost. |
| Flashlight | Keep the current point-light behavior for the first milestone. | A projected cone with a single shadow map is useful but changes appearance and needs a distinct spotlight source. |
| Materials | `_n`/`_s` auxiliary files, +Y normals, explicit gloss and optional descriptor overrides. | Agree descriptor syntax and specular channel meaning before authoring packs; no inferred bump generation by default. |
| Switching | Archived `r_renderer classic|modern`, applied on map load; log effective fallback. | Live switching needs dual caches/rebuild rules and more state/lifetime testing. |
| Performance floor | Initially 1280x720/60 fps on the existing RTX 3050, with measured shadow budgets. | Specify older Intel/AMD hardware or higher resolutions before setting final defaults. |
| Dynamic-light sources without client | Add engine debug lights for verification and decouple server-effect light updates for modern. | Leave current no-client behavior explicitly limited; don't silently claim equivalence. |
| First review point | Stop for visual review after world per-pixel lighting/materials (steps 1-5), then approve shadows. | Implementing every step together delays feedback and makes classic regressions harder to isolate. |

None of these recommendations authorize implementation. Once choices are agreed, start at step 0,
then complete and review one increment at a time.

## 9. Technical references

Repository references: `src/engine/r_main.c`, `r_world.c`, `r_light.c`, `r_studio.c`, `bsp.c`,
`image.c`, `cl_tent.c`, `cl_tri.c`, `vid_sdl.c`, `draw.c`, `HANDOFF.md` and `docs/NBSP.md`.
No external engine implementation was used.

- [Khronos GLSL 1.20 specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.20.pdf): shader baseline.
- [Khronos ARB_framebuffer_object specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_framebuffer_object.txt): off-screen depth targets, capability and completeness requirements.
- [Khronos ARB_timer_query specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_timer_query.txt): optional asynchronous GPU timing.
