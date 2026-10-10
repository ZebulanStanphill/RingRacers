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
cmake --build build/regression --parallel 2
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
- **Add-on shader translation and dispatch:** compile the production GLSL adapter,
  glslang parser/linker and SPIRV-Cross emitter. Translate 36 shader/layout pairs
  with reordered varyings, uniforms/initializers, matrices and fragment coordinates;
  reject invalid interfaces, malformed/oversized sources and invalid layouts,
  then verify compiler recovery. Execute production driver loading/compilation and
  shader selection with GPU creation stubs: missing-stage defaults, dirty caching,
  atomic translation/GPU failures, Ignore custom shaders, sprite clipping variants,
  custom model lighting, postprocessing priority and water. Custom water must
  capture and select its refraction variant for every refracting blend mode,
  capture the whole view with a custom vertex stage, and fall back to its ordinary
  blended output after capture/variant failures, for non-ripple/opaque surfaces
  and with Reduce VFX; Ignore custom shaders restores the built-in refraction.
  Reverting the old custom-water guard fails these checks.
  Windows CI compiles these 78 stages with SM4 `fxc`, then runs the production shader
  and input-layout factory on WARP: 36,864 pixels verify polygon/sky/model data,
  driver constants, UVs, GL fragment coordinates, alpha tests and portal clipping.
  Translated custom water then draws through the production refraction variant in
  four split-view quadrants at varying depths and tics: every composite must match
  the built-in Software ripple oracle for all six blend modes, using depth and GL
  coordinates measured by a translated probe; alpha-test holes leave the scene
  untouched. The old unrefracted, framebuffer-blended output must fail.
  Add-on visuals and load cost on Xbox remain untested; existing water and wall
  confirmations do not validate custom shaders.
- **Installable shader smoke test:** [download and instructions](addons/shader-smoke/README.md).
  The shipped PK3 marks ordinary floors/walls with magenta/green checkers and
  ripple-enabled water with moving cyan/orange bands. Its packaged entries pass
  through the production shader loader; native checks verify five stage bindings,
  water's default vertex stage, untouched targets, missing-path rejection, nine
  translated target/layout pairs and the water refraction variant. Windows CI
  compiles the pack's 19 SM4 stages, bringing the combined translated corpus to 97.
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
- **Pixel history diagnostic:** execute the production copy/readback and command handlers against padded-row resource mocks. Verify inactive paths make no GPU calls, the selected color's actual writer is reported, unchanged depth-only draws are excluded from color-writer rows, metadata stays immutable, 100% clamps to the final pixel, shader-off alpha is normalized, atlas rows/capacity and report limits are respected, partial allocation/readback failures clean up, and malformed coordinates or unsupported rendering modes are rejected. The Windows WARP harness also executes the same extracted driver functions on a real SM4 device: 130 command-ordered copies across atlas rows must preserve their earlier colors while the source remains bound, without changing the source target or binding. This validates the diagnostic, not the underlying brown-wall rendering.
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
- **Fog boundaries and captures:** compare 24,960 production shader row selections with the actual Software zlight/scalelight tables and 16,384 palette-remap samples. Exercise full/split viewport bounds, eye-plane crossings, entirely behind-camera/offscreen faces, retained captures, resize/texture/view/buffer failures, resource accounting, t8/b3 bindings and light/darkness constants. Shared blend tests verify complete fog/water composites retain color masks and restore ordinary blending. Custom dispatch tests check fog overrides, Ignore custom shaders and capture failure; wall-slice tests reject directional light adjustment on fog. Windows WARP executes the production SM4 fog shader for all light levels, wall/plane rows, varying depth, full/half-width viewports, palette lookup and smooth tint/fade. An injected reciprocal-depth shader must fail the same pixel oracle. Complete fog volumes, portal/stencil interactions, overlapping volume visuals, Xbox performance and every split-screen visual check still need device testing.
- **Sprite brightmaps:** execute the shared Software brightmap cache, hardware sprite/precipitation admission and draw texture-selection blocks, hardware mipmap loaders, batch replay blocks and Direct3D texture binding. Check 13 projection cases: frame/default names (including Ark Arrow `SYM0` / `SYMBXXB`), skin sprite2, unknown-frame fallback, missing names, absent maps, rolled sprites and independent width/height mismatches. All four draw paths (ordinary, light-split, precipitation and linkdraw) must bind the base before the brightmap, mark the first upload and cached binds with `TF_BRIGHTMAP` (including retained CPU data after GPU eviction), avoid translating the brightmap, and clear texture unit 1 for a following nonbright sprite using the same base, with batching on and off (eight path/mode cases). Dependency mocks stop at WAD lookup, color translation, patch allocation and GPU upload; these checks do not render sprite pixels or validate Xbox visuals.
- **Palette brightmaps:** Windows WARP executes the production SM4 `PSSoftware` palette path. Brightmapped texels must use the base colormap's full-bright row at their palette index, while other texels keep the surface colormap row from `R_DoomColormap` at the rasterized depth. The negative control restores the old surface-colormap row for brightmapped texels and must fail. This validates the shader on WARP, not Xbox palette-rendering visuals.
- **Extraction safeguards:** ensure comments, strings, nested blocks and prototypes
  do not truncate extracted functions; reject missing/ambiguous definitions.

Implementations under test are extracted at configure time from `RR_SOURCE_DIR`,
not copied into the tests. `#line` directives point compiler/sanitizer diagnostics
back to those engine files. Only dependency stubs, inputs and expected behavior
live in `templates/`. If a function or section moves incompatibly, generation
fails instead of silently skipping its test.

These are isolated native checks. The network simulation does not run UDP clients
or the whole game; the wipe test does not compile HLSL or exercise GPU sampling.
The ripple harness records submitted polygons without executing a GPU shader.
The transparency tests do not validate GPU depth/stencil, portal clipping, model
triangle rasterization, or the complete sprite/model drawing implementations.
These tests do not validate Xbox rendering, frame presentation, deployment, controller
input, performance, or complete replay/gameplay equivalence. Existing Windows CI
continues to compile the actual UWP and shader code.

## CI and adding cases

The `regressions` job runs this suite with ASan and UBSan on every push, pull
request, and manual workflow dispatch. The Windows build depends on it passing.
CTest's JUnit report and detailed run log are uploaded as `regression-results`,
including when a test fails.

When fixing a bug, add the smallest reproducible input and an assertion about its
observable result. Test the relevant platform variants and important boundaries.
Check that the new case fails against the pre-fix implementation, then passes
with the fix. Avoid tests that only assert a source spelling or duplicate the
implementation. Keep randomized cases seeded and retain failing seeds as cases.
To add a group, add its stubs/cases in `templates/`, extract its production code
in `generate.py`, and register its executable in `CMakeLists.txt`. Register extracted production and reference source files as configure dependencies too, so editing Software's ripple calculation regenerates its water-refraction oracle.

- **Water refraction:** extract Software's production fixed-point ripple calculation and sine table, compare 131,072 offsets, and exercise 5,184 shader composites across blend modes, alpha values, distances, times and split-view edges. Native harnesses now supply depth for Direct3D's `SV_Position.w`; the earlier reciprocal-depth assumption was incorrect. Check projected capture bounds and native D3D blend-state construction/restoration. `water_capture` executes the actual capture function and transparent-node dispatch with mocked D3D resources: 21,888 composites across 2/16/96 overlapping water pieces must preserve the skyline contribution and a sharp edge. It checks one bounded copy for repeated pieces, disjoint rectangle expansion in all directions, changes of layer/slope, intervening walls/non-ripple planes, standalone draws, all four view rectangles, allocation failure and target resize. Each captured pixel is copied at most once per run; 200 short runs must copy over 98% fewer pixels than the old whole-view path, and fully clipped faces issue no copies. Reverting to per-face capture must fail. Actual GPU copies/allocation and depth/stencil execution require device validation. An earlier Xbox run confirmed the sky cutoff was fixed but still showed jagged scenery beyond the waterfall, with substantially fewer screen operations. The build 94 retest confirms working water effects and the separate wall light-slice fix for the brown walls. An earlier Grand Prix log exposed excessive whole-viewport copy cost. Bounded capture tests pass, but a controlled Xbox GPU comparison remains pending.

- **Sky patch offsets:** Software's production `R_SetupSkyDraw` supplies the reference horizon and column offsets. Check the hardware offset helper and dome vertices for both hemispheres, signed offsets and odd/even texture heights; include Water Palace's actual `WPZSKY1` header (512×224, left 0, top 16), whose horizon is row 128. These tests verify alignment; the user confirmed the revised position on Xbox. They do not verify background visibility through water.

- **Horizon visibility:** use all 23 horizon boundaries from vanilla 2.4 Water Palace's TEXTMAP, with source/archive identity recorded in `fixtures/water-palace-horizons.json`. Compare the actual hardware horizon draw condition and helper to Software's production line acceptance, fake-flat and empty-line functions. Check its 22 empty sky boundaries, the remaining real boundary, lighting/texture/colormap/tag/slope changes, camera side, fake-flat control sectors, minisegs, one-sided horizons and ordinary non-sky horizons. The old unconditional horizon condition fails. This verifies the renderer's geometry decision; the user confirmed that the background cutoff is fixed on Xbox. Surface ripple artifacts are tracked separately.

- **Internal FOF visibility:** extract Software's actual `R_DrawSinglePlane` rejection block, the hardware helper, and both face-admission conditions. Compare 945 decisions using real engine FOF structures, all CUTEXTRA/EXTRA/water/fog combinations, touching boundaries, moving control heights, and all 29 overlapping-water sectors recorded from vanilla 2.4 Water Palace in `fixtures/water-palace-water.json`. The old hardware height checks admit 15 internal water boundaries that Software suppresses. Removing the new condition from either face fails this test. This verifies the visibility decision; the reported scenery artifacts still require an Xbox retest.

- **Direct3D water shader runtime:** Windows CI compiles the actual production
  `VSPoly` and `PSWaterRefraction` shaders at SM4 and executes them on D3D11 WARP
  at feature level 10.1. A probe measures pixel-position depth and checks it
  against perspective interpolation, including a tilted triangle. Check 73,728
  scenery samples across near/far depths, wave phases and four view rectangles;
  samples outside the selected view must stay untouched. Use transparent water
  to isolate the background row chosen by the production shader. Injecting the
  old reciprocal-depth expression must fail the same pixel oracle. This covers
  rasterizer semantics and real shader execution, while Xbox gameplay visuals
  remain a separate device check.

From an x64 Visual Studio developer PowerShell on Windows, run:

```powershell
python tests/prepare-source.py build/shader-regression-source
./tests/run-d3d11.ps1 -Engine build/shader-regression-source
```

- **Invisible geometry state:** execute production `SetBlendMode`, `D3D_SetBlend` and `GetBlendState`, including the translucent sprite-silhouette to untextured sky-wall transition. All 100 pairs of blend modes must preserve the target color and depth writes, then restore RGB writes and the proper visible blend. The old implementation fails. This tests the actual D3D descriptor construction. The state fix did not resolve the reported brown walls. Pixel history subsequently identified inverted wall light slices, and the user confirmed that separate fix in build 94.
