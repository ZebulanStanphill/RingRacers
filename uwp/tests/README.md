# Native regression tests

These tests run on Linux, macOS, or WSL without game assets, an Xbox, SDL,
Direct3D, or vcpkg. Requirements: Python 3.9+, Git, CMake 3.24+, and `clang++`
with its AddressSanitizer/UndefinedBehaviorSanitizer runtimes. Ubuntu's Clang and
Apple's Command Line Tools work. Configuration fetches pinned glslang and SPIRV-Cross sources and needs network access on the first run. Native MSVC/clang-cl is not supported by this
harness; the existing Windows CI job still builds and packages the UWP app.

## Running

From the repository root:

```sh
python3 tests/prepare-source.py build/regression-source
cmake -S tests -B build/regression \
  -DRR_SOURCE_DIR="$PWD/build/regression-source" \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/regression --parallel 4
ctest --test-dir build/regression --output-on-failure --output-junit results.xml
```

`prepare-source.py` fetches vanilla v2.4 at commit
`7f895c9a7f5ed77a1ccb3e94b326660e744b523e` and applies the current
`patches/ringracers-uwp.patch`. It refuses an existing destination, rather than
resetting someone's checkout or silently reusing an older patch. After changing
the patch, prepare a new destination and set `RR_SOURCE_DIR` to it. Alternatively,
point `RR_SOURCE_DIR` at an existing, up-to-date patched engine checkout to test
engine edits directly. `cmake --build` regenerates harnesses when tested engine
files or templates change. The standalone test project does not configure the
engine's normal CMake project or change its source files.

Sanitizers are enabled by default and any reported undefined behavior fails the
test. Use `-DRR_SANITIZERS=OFF` only for a toolchain without sanitizer support.
Assertions remain enabled in every build configuration, including Release.
To run one group, use e.g. `ctest --test-dir build/regression -R '^online_' -V`.

## Coverage

- **UDP send failures:** execute production `SOCK_Send` and `SOCK_SendToAddr`
  against a `sendto` mock, with host POSIX errors and the extracted production
  Winsock aliases (including raw 10065/10051). Check successful sends, would-block,
  refusal, host/network-unreachable drops, recovery on the next send, disconnected
  peers, broadcast/socket selection, address lengths, and fatal unexpected errors.
  Pre-fix sender controls fail specifically on unreachable errors with a clean
  nonzero exit. These checks run under ASan/UBSan; live netcode retries/timeouts
  and Xbox behavior remain untested. Run with `ctest -R '^udp_send_'`.

- **Encore post-image selection:** execute full production `P_CalcChasePostImg`
  and `P_CalcPostImg` with real engine headers, plus chase water/heat helpers.
  Check heat, water, no distortion and water priority with and without Encore,
  independent mirroring, first-person gravity flip, FOF bounds/eligibility,
  awayview selection, matching split views and untouched unrelated views.
  Flat FOF heights, heat-tag lookup and removed-object detection are dependency
  stubs; these checks validate selection, not rendered pixels.

- **Online catch-up and title card:** the production packet sender, tic expansion,
  title-card function, client-state enum, packet structs, and server `PingUpdate`.
  Check all eligibility/missed-packet combinations, low-byte wraparound across
  the client receive window, packet contents/length, per-clock-tic rate limiting,
  and preservation of simulation tics. Check that only online UWP Legacy GL
  clients defer the blocking title, while hosts, Software, desktop and demos keep
  it; also check clock refresh after an offline load. A delayed-loading simulation
  tests the server's delay-limit decision with
  another healthy remote player, across three load durations and two limits.
  Compile/run UWP hardware, UWP software, and desktop hardware variants.
- **Options slide:** production reset, tick, quit, and color-change functions with
  actual menu structures. Exercise exact and skipped completion tics, clock
  wraparound, repeat target changes, resets during a slide, and instant wipes.
  The desktop variant preserves the upstream timing behavior.
- **Bounded diagnostics:** production trace implementation and frame-window
  selection. Check fade expiry/event caps, title sample caps, both frame windows,
  load resets, tics across the unlogged gap, camera/presentation snapshots, missing
  player objects, unpresented frames, and that tracing
  leaves the player's object unchanged. Compile/run hardware and software variants.
- **Add-on shader translation:** compile the production GLSL adapter, glslang
  parser/linker and SPIRV-Cross emitter from `r_d3d12/`. Translate 36 shader/layout
  pairs with reordered varyings, uniforms/initializers, matrices and fragment
  coordinates; reject invalid interfaces, malformed/oversized sources and invalid
  layouts, then verify compiler recovery. Check the expected constant-buffer
  declarations, std140 pack offsets and resource slots directly, with every
  register in space 0. Native translation emits 78 general Shader Model 5.1
  stages to `translated-shaders`; Windows CI compiles these and the smoke-test
  add-on's 19 stages there as SM5.1, for 97 total. The Direct3D 11
  dispatch and GPU factory harnesses were removed with that driver.
  `custom_shader_dispatch` runs the production Direct3D 12 loader, compiler,
  cleaner and shader selection with translation, shader compilation and pipeline
  creation stubbed. With no add-on loaded nothing is translated or created and
  the built-ins are selected. A fragment-only replacement gets the real built-in
  vertex stage for the polygon, sky and model layouts and is selected per layout;
  recompiling without a change translates nothing. A replacement that fails to
  translate, to compile either stage on any one layout, or to create a pipeline
  on any one layout is discarded whole, with its pipelines, leaving the built-ins
  (and other types' replacements) in use; a later good load works again. Sprite
  clipping shares the sprite replacement, a custom model shader is kept for lit
  models unless a model-lighting replacement is supplied, Ignore custom shaders,
  Shaders Off and postprocessing bypass add-ons, and custom water gets a
  polygon-only refraction variant that, if it fails to translate, compile or
  validate, leaves valid unrefracted water. Cleaning discards everything. The
  Direct3D 12 WARP harness checks the same loader through pixels (below). Add-on
  visuals and load cost on Xbox remain untested.
- **Installable shader smoke test:** [download and instructions](addons/shader-smoke/README.md).
  The shipped PK3 marks ordinary floors/walls with magenta/green checkers and
  ripple-enabled water with moving cyan/orange bands. Its packaged entries pass
  through the production shader loader; native checks verify five stage bindings,
  water's default vertex stage, untouched targets, missing-path rejection, nine
  translated target/layout pairs and the water refraction variant. Windows CI
  compiles the pack's 19 SM5.1 stages, bringing the combined translated corpus to 97.
  The user confirmed the pack's markers on Xbox; its refracted water is untested there.
- **Wall light slices:** execute production UWP wall splitting and clipping with
  real slope/FOF height evaluation. Reproduce the second build 93 Xbox pixel
  capture (Water Palace line 1, `WTPTU1`): an 18-unit sloped tier crosses the
  water light plane at -52. The pre-fix splitter draws outside the original
  wall and fails the vertex-bound assertion. Check 144 fixtures, including
  100 seeded sloped walls, reversed slopes, crossing light planes, zero-height
  endpoints, a tier that closes inside its span, the first capture's normal
  fence, NOSHADE, solid FOF holes and matching/nonmatching CUTEXTRA kinds.
  An independent sampled polygon oracle checks exact coverage, lighting,
  preserved UVs and no outside geometry; draw stubs check opaque/queued/fog
  dispatch, texture IDs, colormaps and alpha. These tests verify submitted
  geometry, not Xbox visuals or performance. The user confirmed the Water
  Palace wall fix and water effect on Xbox One X in build 94; all split-screen
  visual checks and a controlled performance comparison remain untested.
- **Title background light lists:** replay the title map's actual BSP with
  production `HWR_AddLine`, seg processing, wall light slicing, clipping,
  `R_Prep3DFloors`, and all 72 authored slope constructors. All 58 cold first
  wall submissions must equal an explicitly prepared reference and the second
  traversal in order, vertices/UVs, light, colormap, flags, alpha, and queued/fog
  choice. Focused cases cover NULL back sectors, absent FOFs, a valid list left
  untouched, portal-search exclusion, real-sector preparation before the fake-flat
  copy, preserved target/control moved flags, and the normal subsector refresh.
  A generated negative control removes only the added preparation and must fail
  equality with exactly 22 lighting/colormap differences. Compact title-map JSON
  and uncompressed ZNODES are expanded at configure time; generated initialization
  headers are never committed. Texture/GPU calls and non-wall subsector work are
  stubs, so this checks wall submissions rather than rendered pixels or the wipe.
  The faithful slope setup retains existing signed-shift UB: the title targets'
  `title-shifts.ignorelist` disables **only `shift-base`** in `MakeViaMapthings`
  and `FixedDiv2`. ASan and all other UBSan checks remain enabled, including in
  traversal and lighting. No authored slope touches the affected FOF sectors;
  retaining the complete map still checks the other submitted sloped geometry.
- **Pixel history diagnostic:** execute the production engine command handler.
  Reject malformed coordinates and unsupported rendering modes, require batching
  to be off, and check default and explicit coordinates passed to the Direct3D
  12 request function. The former Direct3D 11 copy/readback mocks and extracted
  SM4 device harness were removed. The Direct3D 12 WARP `surface-probe` scene
  checks command-ordered copies, logged pixel ownership and the inactive path.
  This validates the diagnostic, not the underlying brown-wall rendering.
- **Suspect sky-surface diagnostics:** execute the production trace and both floor/wall dispatch conditions against real engine structures. Check per-type caps, one sampled frame per five-second interval and view, 64-bit frame IDs, map/time resets, portal/skybox exclusions, regular-floor selection, original sector IDs alongside fake-flat heights, settings, unchanged vertices, eight-vertex limits and bounded rows with one log write per surface. These checks validate diagnostics; they do not reproduce or fix the reported brown walls.
- **Wipe math:** translate the production `PSWipeFull` HLSL function to native
  Clang vector arithmetic, with CPU texture sampling stubs. Compare against an
  independent scalar color oracle for crossfade, inversion, Mega Drive black/white
  fades, reverse masks, Encore wiggle, all 32 mask values and 200 base rows, screen
  edges, and five resolutions including an odd size. Check mask coordinates,
  captured-image sampling bounds, and alpha.
- **Fill transparency:** execute production `V_DrawFill`, HUD alpha resolution,
  coordinate snapping and `HWR_DrawFill` in UWP and desktop variants. Each compares
  2,968 Software/Legacy GL cases across explicit alpha levels, all three HUD alpha
  flavors, HUD settings/fades, slide overrides and split-view layouts. The actual
  fully faded Watching underline must submit no quad. Check partial alpha and
  blend flags, opaque geometry/colors, no-scale rectangles, opaque fullscreen
  clears and translucent fullscreen coverage at 320×200, 1280×720 and 853×480;
  six direct hardware calls with fully transparent/unsupported alpha levels must
  return safely. Replacing either fill implementation with the pre-fix version
  fails the parity checks. Recording stubs stop at the Software quad and hardware
  polygon/clear boundaries; Xbox visuals and GPU blending remain untested.
- **Raw picture formats:** production conversion and cache-loading functions. Check all five source modes, palette destination widths, grayscale/RGB alpha, downscaling and upscaling, row padding, malformed headers/modes/dimensions/pixel lengths, and trailing bytes. The RGB24 case fails with the pre-feature implementation.
- **Polyobject ripples:** run the production `HWR_RenderPolyObjectPlane`,
  `HWR_RippleBlend`, shader availability check, and shared `R_IsRipplePlane`
  against real engine structures. Each UWP/desktop executable checks 1,600
  combinations of independent floor/ceiling flags, control-sector precedence,
  surrounding-sector fallback, absent sectors, Reduce VFX, shader availability,
  opaque/translucent planes, and ordinary-sector/FOF ripple flags. Inspect the
  submitted shader, blend flags, alpha, and translated polygon geometry; invalid
  polygons must submit no draw. Replacing either changed renderer function with
  its pre-feature implementation fails the corresponding behavior assertion in
  both platform variants.
- **Underwater/heat distortion:** production postprocess dispatch, D3D fullscreen vertices and pixel-shader sampling. Compare more than 600,000 UV samples against the unmodified vanilla 2.4 `rhi_glsl_fragment_postimg.glsl` fixture, including full-frame wave amplitude, tic timing, paused frames, texture padding, viewport edges and all screen layouts at nine resolutions. Check viewport selection/reset, intermission captures, Reduce VFX suppression and unaffected views. The fixture comes from release `data/shaders.pk3` (GPLv2, as the engine); it is the independent Software reference. GPU shader compilation is covered by Windows CI. The user reported improved single-view underwater intensity on Xbox; heat and all two-/three-/four-player visual checks remain untested.
- **Transparency ordering:** extract the production node builder, object pass,
  model/sprite dispatcher, linkdraw gating, shared sprite geometry, view transform,
  and slope helpers. Each UWP/desktop executable checks 180 scenes and cases,
  including 128 seeded scenes with an independent depth-order and pixel-composite
  oracle. Cover front/behind objects, mixed alpha flags, model load fallback and
  player skins, empty/objects-only/faces-only frames, opaque objects, always-on-top,
  linkdraw display offsets, actual precipitation-sized storage, disjoint bounds,
  slanted walls, sloped floors, moved polyobject planes, near-plane crossings,
  pitched billboards, translated viewpoints, and the toggle's Off path. Six integration
  cases execute the merged dispatcher with adjacent water pieces and inserted
  sprites/models, checking capture continuation, interruption and end-of-view reset
  with both toggle settings; desktop builds retain their existing capture behavior. Replacing
  both production dispatchers with their pre-feature implementations fails the
  front-object draw-order assertion in both variants. Dependency stubs record
  draws and blend a shared CPU pixel; they do not render complete game scenes.
- **Fog boundaries and captures:** compare 24,960 production shader row
  selections with the actual Software zlight/scalelight tables and 16,384
  palette-remap samples. Extract bounds from `r_d3d12/d3d12_captures.inc` and
  exercise full/split viewports, eye-plane crossings and behind-camera/offscreen
  faces. `fog_capture_d3d12` checks capture reuse, failure, resizing, resource
  accounting, bindings and constants against fixed independent bounds and byte
  initializer oracles from the former Direct3D 11 driver. Its obsolete native
  capture harness was removed. Wall-slice tests reject directional light
  adjustment on fog. Windows WARP executes the shipped SM5.1 path and rejects
  a reciprocal-depth fog shader with the `fog-palette` oracle. Complete fog
  volumes, overlapping visuals and Xbox performance still need device testing.
- **Sprite brightmaps:** execute the shared Software brightmap lookup, real patch converters/pixel reader, production sprite rotation and companion cache, patch destruction, hardware sprite/precipitation admission, draw texture-selection blocks, mipmap loaders, batch replay and Direct3D texture binding. The original 13 admission cases cover frame/default names (including Ark Arrow `SYM0` / `SYMBXXB`), skin sprite2, unknown-frame fallback, missing names, absent maps and independent width/height mismatches. Rotation checks cover 852 transforms: all 71 nonzero angles, horizontal flips, two asymmetric source shapes, explicit/default/implicit noncentral pivots, differing source/map header offsets, asymmetric holes, different opaque bounds and fully transparent maps. An independent coordinate oracle checks 2,925,912 pixels in both vertical orientations; a pinned v2.4 raster snapshot checks that base/Software pixels and headers stay identical. Shared brightmaps across distinct base offsets/pivots must not alias; cached lookups must allocate nothing. Rotated-patch purge/rebuild, source-only tag purge and 144 dynamic brightmap reloads exercise real patch cleanup and check stable live allocation counts and released GPU cache entries. Ordinary, light-split and linkdraw paths bind rotated pairs with Batching on/off (72 cases), alongside 3,408 production horizontal/vertical UV orientations, the existing eight ordinary/precipitation path/mode transitions and resident-data upload check. Separate negative executables must reject the previous dropped-map behavior and a one-pixel transform error. Zone allocation/WAD lookup, color translation and GPU allocation/upload are dependency mocks; rotation/conversion/cache/destruction and extracted projection/binding blocks are production code. Vertical orientation checks execute production texture-coordinate assignment and compare shared pixel coordinates, without running full billboard geometry or GPU rasterization. These tests do not validate Xbox visuals. Run with `ctest -R '^sprite_brightmap'`.
- **Encore brightmap colors:** run production sprite colormap selection, Software
  full-bright selection, mapped-cache lookup/update, column conversion and light-table
  conversion. Check 12,288 combinations covering all 256 indices, 1/2/3/4-byte
  output, both palette settings, ordinary/Encore/exempt sprites, cache reuse and
  in-place remap changes. Generated production mutants that omit or repeat the
  column remap must fail with a color mismatch. Direct3D 12 WARP checks normal,
  Encore and exempt texels with Palette Rendering on and off. Separate controls
  omit or repeat the remap on bright pixels for each palette setting. The native
  test verifies production pretranslation; WARP supplies pretranslated textures
  to verify shader behavior. These complement each other without running a
  complete game or validating Xbox visuals.
- **Spring wall silhouettes:** `spring_clip` executes the production silhouette
  helpers, the complete ordinary sprite draw function, both light-split sprite
  submissions, the solid-wall recording and both opaque FOF side recordings
  (extracted from `HWR_ProcessSeg` before light slicing, including the texture
  gate that leaves masked holes depth-tested). Independent ray/triangle samples
  check a spring hidden inside an angled box (with a plain and a 3D floor side,
  and an oblique camera), a spring in front of a wall left as the exact quad,
  a spring partly behind a wall's edge with only the covered part removed, sloped
  walls in either direction, a wall crossing the camera plane, a wall wholly nearer than
  the origin, and an ordinary sprite or always-on-top spring left untouched. FOF
  sides are admitted when opaque, whether solid or intangible; fog and translucent
  sides are not. Zone allocation and GPU submission are stubs that collect the
  submitted triangles. CTest requires five negative controls to fail:
  `--negative-full` and `--negative-edge` restore the old per-pixel depth behavior,
  while `--negative-front` and `--negative-nonspring` inject the clipping errors the
  preservation cases guard against (ordering lost, or the `MF_SPRING` gate lost),
  since old depth testing already preserves those two cases, and `--negative-bound`
  (below) tells the clip it has more room than its buffer. Software draws every 3D floor
  side and plane nearer than the farthest one a sprite sorts behind over it, so those
  entries are subtracted together: the test records two sides whose nearer one fails its own
  test and checks that it is still subtracted (and not when recorded after the farther one),
  and a plane swept up by a side. The production plane recorder (`HWR_AddSpringFOFPlane`,
  extracted from `HWR_Subsector`'s call sites) is run on a 70-corner subsector polygon
  and must split it into three pieces. Plane cases put a slab over a camera-above spring and
  under a camera-below one (hidden), hide it when only the quad's corners reach past the plane
  (the sprite's own heights decide, as in Software), and leave the spring whole when it pokes through,
  and a replay of a bumper from an Xbox capture must leave no sliver along the edge its face and top share;
  the plane is edge-on, behind the origin, beside it, off its rows, fog or see-through,
  and for a sloped plane find the height per point. `--metropolis` replays the nine wedge-shaped
  3D floors of the yellow bumpers in Metropolis (map geometry copied into the test, the
  spring at the map's quarter object scale) from eight cameras in the pit and prints, per
  view, the visible area, the piece peak and what remains under each kind of occluder,
  and writes the first view's `gr_springprobe` log through a stub `I_OutputMsg`; the test fails when
  anything Software would paint over the spring remains, or when any of the spring is left
  visible at all (before the draw order rule about half of a full-size spring stayed visible
  along the wall). The buffers are bounded: a cut can add more than one corner when
  a plane polygon's long edge is split into collinear corners and rounding leaves them
  out of line, which once overran the stack on Metropolis. The half-plane clip is called on a
  zigzag whose every edge crosses the plane, with too little room (it must return -1 and leave
  guard slots holding sentinels untouched) and with exactly enough (12 corners), and the near-plane clip on a
  32-corner silhouette that needs 48 (it must report none); `--negative-bound` passes a capacity
  larger than the real output, so the guards change and the program fails. The smallest
  fuzzed scene that overflowed (two sloped 3D floor planes of 29 and 32 corners, a 3D floor side, a full-size
  spring; hex-float literals copied into the test) must submit only triangles or the quad and still draw something, and
  3000 random views from a SplitMix64 generator (not `std::uniform_*_distribution`, which differs between
  standard libraries) of such planes and sides must all finish. Without the bounds both
  fail with an AddressSanitizer stack-buffer-overflow in `HWR_SpringHalfPlane`. Seg traversal,
  GPU depth/stencil and Xbox visuals still need separate validation.
- **Palette brightmaps:** Direct3D 12 WARP executes the production SM5.1
  `PSSoftware` palette path. Brightmapped texels must use the base colormap's
  full-bright row at their palette index; other texels keep the surface colormap
  row at the rasterized depth. `brightmap-palette` varies light and depth and
  checks normal, Encore and exempt texels. A surface-colormap shader mutant must
  fail. `brightmap-smooth` covers the smooth-light path. Both scenes also have
  separate expected-pixel negative controls. This does not validate Xbox visuals.
- **Direct3D 12 bookkeeping:** `d3d12_allocators` compiles the production
  `r_d3d12/allocators.h` and `pipeline_key.h` headers under ASan and UBSan, with no
  GPU. It checks `RingAllocator` alignment (including non-power-of-two), wraparound,
  fence blocking with the highest overlapping fence, `Collect` and `Touch`
  retirement, same-submission wrap into retired space, zero, exact-capacity and
  SIZE_MAX limits. `PipelineKey` equality and hashing are checked for each field and
  across a deterministic 65,536-key sample. `SizeClassArena` checks fenced reuse
  through `Free` and `Collect`, chunk growth with `AddChunk`, dedicated allocations
  above `MaxClass`, and exhaustion once no chunk can be added. The texture arena's
  opt-in quarter-octave classes are checked on both sides of every threshold,
  rounded to 4 KB or 64 KB alignment, with dense packing, separate alignment
  chunks/free lists, fenced reuse, and a seeded mixed-alignment nonoverlap oracle.
  The 4 KB post-allocation texture guard is included in placement capacity and
  cannot overlap another live allocation; default static-upload behavior remains
  power-of-two and class-aligned. The explicit WARP `texture-guard` fixture uses
  a test-only getter that queries the live resource's allocation size and checks
  the guard and nonoverlap for 256×256 and 64×64 textures, then creates a 1024×1024
  texture, which exceeds the arena's largest class with its guard, and checks that it
  got a heap of its own rather than committed storage and that its last texel draws.
  None of this runs during normal rendering or retains diagnostic metadata. The CPU mip generator is checked
  against a scalar oracle for odd sizes and 1×N textures. The 3D texture
repack used for the palette lookup table is checked for row pitch, slice order and
untouched padding.
- **Extraction safeguards:** ensure comments, strings, nested blocks and prototypes
  do not truncate extracted functions; reject missing/ambiguous definitions.

Implementations under test are extracted at configure time from `RR_SOURCE_DIR`,
not copied into the tests. `#line` directives point compiler/sanitizer diagnostics
back to those engine files. Only dependency stubs, inputs and expected behavior
live in `templates/`, including the frozen former-driver bounds and constant
initializer references that independently check Direct3D 12 captures. If a function or section moves incompatibly, generation
fails instead of silently skipping its test.

These are isolated native checks. The network simulation does not run UDP clients
or the whole game; the wipe test does not compile HLSL or exercise GPU sampling.
The ripple harness records submitted polygons without executing a GPU shader.
The transparency tests do not validate GPU depth/stencil, portal clipping, model
triangle rasterization, or the complete sprite/model drawing implementations.
These tests do not validate Xbox rendering, frame presentation, deployment, controller
input, performance, or complete replay/gameplay equivalence. Existing Windows CI
continues to compile the actual UWP and shader code.

## Direct3D 12 WARP harness

From an x64 Visual Studio developer PowerShell on Windows:

```powershell
python tests/prepare-source.py build/shader-regression-source
./tests/run-d3d12.ps1 -Engine build/shader-regression-source
```

The harness compiles production `r_d3d12.cpp`, `d3d12_core.cpp`,
`d3d12_upload.cpp`, `d3d12_pipeline.cpp` and `d3d12_prewarm.cpp` against
the real engine headers. SDL, engine-symbol and headless-present functions are
test stubs. ANGLE is not used. Shaders compile with `fxc` at SM5.1 against the
production root signature. Scenes run on WARP with the debug layer and GPU-based
validation enabled. Any debug-layer warning, error or corruption message, or any
`Core_Errors` report, fails the run. The sole allowlist is warning #820 about
a missing or mismatched optimized render-target clear value: the runtime guarantees
the requested clear color, which the pixel oracle checks. It only affects clear
performance. Scenes cover clears, blends, depth, indexed
triangles, 2D lines and a tiny-ring stress test, and negative controls must fail. Each
negative control is its own process that replays the scenes before its own with the debug
layer on but GPU-based validation off, since it runs the same GPU work as the full run and
only changes what its oracle expects; the runner keeps one per core going at once and
prints each one's output as it finishes.
New texture, model and sky scenes cover texture creation, point sampling, reuploads,
delete-and-recreate, mip chains, sky fan-strip rebuilds, and model buffered/lerped
UV fallback. Their negative controls are `texture`, `texture-update`,
`texture-reuse`, `mips`, `sky`, `model-buffered`, `model-lerped` and `model-fallback`;
each must be rejected by its own oracle. Tiny models, lit models and textured sky
aren't exercised, so they remain untested here. WARP doesn't measure Xbox performance;
texture creation time, heap behavior and frame rate need an Xbox run.
Portal scenes require the production PSO preflight to succeed (`portal-supported`).
`portal-aperture` checks color preservation and stencil-1 confinement; `portal-nested`
checks incrementing 1 to 2 and restoring 2 to 1. `portal-restore` checks that the return
pass writes entrance depth, and `portal-depth` checks that the fullscreen far-depth
reset only touches the aperture, allowing farther child geometry while preserving
outside occlusion. `portal-plane` checks world-plane clipping and disabling the plane.
Each has its own required negative control. These pixel scenes use production driver
entry points under debug/GBV; recursive map visuals, transformed camera/model clipping,
and fog/water portal interactions still require further testing.
Custom-shader scenes load an add-on fragment shader that outputs a fixed color, compile it
through the production loader, and draw a quad textured with (200, 40, 16). `custom-shader`
must show the fixed color (64, 128, 191). `custom-shader-clean` must show the built-in color
again after cleaning the shaders. `custom-shader-invalid` loads invalid GLSL, which must log a
built-in fallback and draw the built-in color. `custom-shader-nocustom` sets Ignore custom
shaders, so the custom-color oracle must fail: it's the negative control. They haven't been run
locally, and Xbox results are untested. See [the Direct3D 12 add-on shader notes](../docs/d3d12-custom-shaders.md).
Dispatch scenes run last, after the model and sky scenes, and replace only the fragment stage, so
the built-in vertex stage places the geometry. `custom-none-quiet` compiles with nothing loaded,
which must log no compilation, and `custom-none` draws the built-in color. A fragment for
`SHADER_FLOOR` must show (64, 128, 191) on a polygon (`custom-layout-poly`, with the pixel outside
the quad left alone), on the sky dome (`custom-layout-sky`) and on a model (`custom-layout-model`),
the three layouts. `custom-recompile` compiles again without a change, which must log nothing.
`custom-replace-invalid` replaces the working shader with invalid GLSL, which must draw the
built-in color, and `custom-replace-recover` loads a good one again (191, 128, 64).
`custom-sprite-clip` selects the clipping variant of a sprite replacement. `custom-model-kept`
draws a model with Model Lighting on and only a model replacement, which stays in use, and
`custom-model-lighting` adds a model-lighting replacement, which must take over. Failures that only
the GPU can cause, such as one layout failing to create, are covered by the native test.
`custom-water-refraction` loads a water fragment that outputs a fixed color with the surface's
alpha and draws it over 64 colored bands at two tics. The result must be that color blended
over the scenery shifted by the production ripple (four rows down, then eight up), which the
unrefracted output differs from by several levels.
Invisible-geometry scenes run all 100 pairs of blend modes through a depth-only draw: after a
translucent sprite silhouette and an untextured sky wall (both Invisible, depth writing), the
target keeps its color (`blend-invisible`), a farther visible surface is hidden by the depth they
wrote (`blend-invisible-depth`), and a nearer visible surface blends in its own mode again
(`blend-invisible-restore`). `blend-remap-mask` draws water and fog composites with Invisible set,
which must leave the target alone, because the composites disable blending but keep the write mask, and
`blend-remap-restore` draws a fog composite and then an ordinary translucent surface three times,
which must blend over the remapped color each time.
Palette scenes turn Palette Rendering on with a known 64³ lookup table (index 7 only
where red is low and green and blue are high, otherwise 19), a light table and a screen
palette. A colormapped textured polygon must read the light table's color from the screen
(`palette`), and the palette step, read back through a test-only entry point, must
turn that into the screen palette's color for its lookup index (`palette-post`).
`light-table-update` and `light-table-update-post` update a light table between two draws
of one frame, with no readback between: the first must keep the old color and the second
get the new one. `light-table-clear` clears and recreates the table for 96 frames while
recorded draws still use it, and requires descriptors to be reused after their fences.
`screen-palette` changes the screen palette between frames, and `palette-off` checks that
the last step doesn't run with Palette Rendering off. The master palette's creation and
removal run under GPU-based validation, and the `palette:` perf line's counters are
checked; the fade map scenes below are what read the master palette. Fog remapping with
palette rendering is covered below; water with palette rendering and Xbox colors remain
untested here.
The `surface-probe` scene requests the Direct3D 12 driver's pixel history at 50%,50% (pixel 32,32 of
the 64×64 target), draws three flat quads of different colors, labels one with
`SurfaceProbeSource("wall", 7)` and ends the probe. It checks the logged `begin` pixel, each draw's
`raw=` value (RGBA8 as a little-endian DWORD, alpha `0xFF`, each color channel within one like the
other scenes) in order, `kind=wall source=7` on the labelled draw and `kind=polygon source=-1` on the
others, and the `end draws=3 color_changes=3` line. The screen must still show the last color
afterwards, a repeated request while one is pending must be refused, and a second frame without a
request must neither begin a probe nor log `surface probe:`. Its negative control offsets the
expected red channel and must fail the log oracle. The forced readback present path
(`gr_d3d12_present Readback`) has no scene, since the harness stubs presentation and has no ANGLE;
for the same reason, nothing here covers the shared-fence present sync or `gr_d3d12_present CpuSync`.
The DRED breadcrumbs logged by a timed-out GPU wait are also unexercised here.

Fog and water scenes run the production capture path with the same tiny rings, so command-list
submissions exercise rebinding and constant lifetimes. Fog draws over known scenery, including
palette remapping through a light table. Water checks ordinary surface sampling and captured-scene
refraction against the fixed-point sine and independent blend math; grouped surfaces must keep their
original captured background while later pieces draw. The `fog-remap`, `fog-palette`, `water-
surface`, `water-refraction`, `water-group` and `water-group-counts` negative controls must each
fail their oracle, and capture counters check the grouping under GPU-based validation. Native D3D12
capture tests mock the texture, copy, binding and constant helpers, checking the former Direct3D 11 driver's fixed bounds and
constant initializer oracles, split views, clipping, failures, resizing and disjoint water growth. A growing water
capture uses one batched transition round trip around up to four strips; an unchanged grouped
rectangle uses none. `capture-mixed` requires fog and water in the same command list;
`capture-recreate` checks capture replacement after flushing resources still referenced by
recorded draws and after a resize; `capture-skipped` skips two-vertex fans after capture
and checks the next capture/draw, including pending opposite-transition merging. Each has
a negative control. Xbox visual results, render-target decompression cost and performance
remain untested. Water sine creation also checks small-resource alignment under GBV:
the 8192×1 R32_SINT table exceeds sixteen 4 KB tiles despite its 32 KB raw size,
so it uses normal 64 KB placement alignment.
Screen scenes draw a screen of four colored quadrants, copy it, clear the screen and
draw the copy back. `screen-copy` does so with the intermission background, which must
reproduce every quadrant; `final-screen` draws the final screen into a viewport twice
as wide as it is tall, which must show the image with black bars; `screen-flush` flushes the
screen textures and then, for 64 frames, copies, draws and flushes them again while recorded work
still reads them, requiring descriptors to come back after their fences.
Wipe scenes set a known 80×50 mask lump (four byte values, one per quadrant) and wipe a
solid start screen to a solid end screen. `wipe-blend` checks a crossfade and `wipe-mode`
the to-black, to-white and invert modes, each normal and reversed, at four sampled pixels
against the shader's formulas computed in double precision, which also pins the mask's
orientation. `wipe-wiggle` sets Encore's wiggle and compares every pixel of a start screen with
a vertical edge to the edge's per-row shift. `wipe-fallback` makes the mask lump missing,
which leaves the two-pass wipe: the end screen drawn over the start screen by a 2×2 alpha
texture on the second texture unit. `wipe-after` checks that a draw after a wipe, and a
mask of another size, still work, since the wipe passes end by resetting every binding.
`postimg-identity` redraws the saved screen with no distortion, which must reproduce it,
and `postimg-shift` with the water's sway, checking the pixels either side of the edge in
every row against the sine. The grid argument of `PostImgRedraw` is ignored, as in the former Direct3D 11
driver, so a shifted grid doesn't shift anything; the sway is what moves pixels.
Fade map scenes give the master palette known colors and a table of three rows (identity,
index 7 to 40, then 7 to 41 and 19 to 43). `fade-map` and `fade-map-row` check the strength's row,
`fade-map-clamp` a strength past the last row, `fade-map-remake` the same table id with four
rows, `fade-map-off` that nothing is drawn (and false is returned) without Palette
Rendering, `fade-map-after` that a draw after a fade map is unaffected, and `fade-map-reuse`
replaces tables for 64 frames while recorded work reads them. `resize-screen` and
`resize-fade-map` switch the screen to 640×400 and back, which remakes the screen
textures and the fade map's copy of the screen, under GPU-based validation.
The `screens:` perf line's counts of wipes, post images and fade maps are checked.
The inversion oracle caught the upstream `PF_Invert`/`PF_Occlude` bit collision;
`PF_Invert` now uses its own blend bit, so it survives the blend mask. If the Graphics
Tools feature is missing, the script retries its install and then fails loudly
rather than running without the debug layer.

Reinitialization is currently unsupported: `g_shutdown` is terminal. These tests
do not change that lifecycle. They do not cover Xbox rendering or performance.

## CI and adding cases

The `regressions` job runs this suite with ASan and UBSan on every push, pull
request, and manual workflow dispatch. The Windows `warp` job, which compiles the
translated add-on shader fixtures and runs the Direct3D 12 WARP
regressions, depends on it passing; the Windows `build` job runs alongside both,
so a failed test fails the run but the package is still uploaded. CTest's JUnit
report and detailed run log are uploaded as `regression-results`, including when
a test fails.

When fixing a bug, add the smallest reproducible input and an assertion about its
observable result. Test the relevant platform variants and important boundaries.
Check that the new case fails against the pre-fix implementation, then passes
with the fix. Avoid tests that only assert a source spelling or duplicate the
implementation. Keep randomized cases seeded and retain failing seeds as cases.
To add a group, add its stubs/cases in `templates/`, extract its production code
in `generate.py`, and register its executable in `CMakeLists.txt`. Register extracted production and reference source files as configure dependencies too, so editing Software's ripple calculation regenerates its water-refraction oracle.

- **Water refraction:** extract Software's production fixed-point ripple
  calculation and sine table, compare 131,072 offsets, and exercise 5,184 shader
  composites across blend modes, alpha values, distances, times and split-view
  edges. Extract projected bounds from `r_d3d12/d3d12_captures.inc`.
  `water_capture_d3d12` checks bounded copies, disjoint rectangle growth, grouping,
  allocation failures, resize and clipped faces. Its independent bounds and
  constant byte initializer oracles are frozen versions from the former Direct3D
  11 driver. Engine-side water-plane continuation and transparent-node dispatch
  checks moved from the removed `water_capture` harness to this Direct3D 12
  harness, retaining slope/layer changes and wall/non-ripple interruptions.
  Direct3D 11 blend-state and capture harnesses were removed; Direct3D 12 WARP
  checks the shipped blend/capture path and rejects a reciprocal-depth water
  shader with the `water-refraction` oracle. The build 94 Xbox retest confirmed
  working water effects and the separate wall light-slice fix. A controlled
  Xbox GPU comparison of bounded captures remains pending.

- **Sky patch offsets:** Software's production `R_SetupSkyDraw` supplies the reference horizon and column offsets. Check the hardware offset helper and dome vertices for both hemispheres, signed offsets and odd/even texture heights; include Water Palace's actual `WPZSKY1` header (512×224, left 0, top 16), whose horizon is row 128. These tests verify alignment; the user confirmed the revised position on Xbox. They do not verify background visibility through water.

- **Horizon visibility:** use all 23 horizon boundaries from vanilla 2.4 Water Palace's TEXTMAP, with source/archive identity recorded in `fixtures/water-palace-horizons.json`. Compare the actual hardware horizon draw condition and helper to Software's production line acceptance, fake-flat and empty-line functions. Check its 22 empty sky boundaries, the remaining real boundary, lighting/texture/colormap/tag/slope changes, camera side, fake-flat control sectors, minisegs, one-sided horizons and ordinary non-sky horizons. The old unconditional horizon condition fails. This verifies the renderer's geometry decision; the user confirmed that the background cutoff is fixed on Xbox. Surface ripple artifacts are tracked separately.

- **Internal FOF visibility:** extract Software's actual `R_DrawSinglePlane` rejection block, the hardware helper, and both face-admission conditions. Compare 945 decisions using real engine FOF structures, all CUTEXTRA/EXTRA/water/fog combinations, touching boundaries, moving control heights, and all 29 overlapping-water sectors recorded from vanilla 2.4 Water Palace in `fixtures/water-palace-water.json`. The old hardware height checks admit 15 internal water boundaries that Software suppresses. Removing the new condition from either face fails this test. This verifies the visibility decision; the reported scenery artifacts still require an Xbox retest.

- **Invisible geometry and composite blend state:** `water_blend_state` executes the
  production `SetBlendMode`, `D3D_SetBlend`, `StatePipelineKey`, `MakeBlendDesc` and
  `MakeDepthDesc`, and decodes the state a draw would give its pipeline. All 100 pairs of
  blend modes, including the translucent sprite-silhouette to untextured sky-wall
  transition while Invisible stays set, must have a zero color write mask with depth
  writing on and depth always off, and then restore RGB writes and the visible surface's
  own blend. A water refraction or fog remap composite must disable blending and keep the
  write mask for both mask settings, and the surface after it must get the translucent
  blend again, after repeated use. The Direct3D 12 driver builds this override per draw
  from the key rather than caching it, so there is no stale cached override to restore.
  Windows WARP checks the same behavior through pixels (above). The state fix did not
  resolve the reported brown walls; pixel history then identified inverted wall light
  slices, and the user confirmed that separate fix in build 94.

- **Direct3D water shader runtime:** run `tests/run-d3d12.ps1` as described
  above. The production SM5.1 capture/refraction path runs on Direct3D 12 WARP;
  an injected reciprocal-depth shader must fail the same `water-refraction`
  pixel oracle. The former SM4 WARP runner and its water, fog and brightmap
  sources were deleted because that shader profile is no longer shipped.
  Xbox gameplay visuals remain a separate device check.
