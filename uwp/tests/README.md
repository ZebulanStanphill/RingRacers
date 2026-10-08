# Native regression tests

These tests run on Linux, macOS, or WSL without game assets, an Xbox, SDL,
Direct3D, or vcpkg. Requirements: Python 3.9+, Git, CMake 3.24+, and `clang++`
with its AddressSanitizer/UndefinedBehaviorSanitizer runtimes. Ubuntu's Clang and
Apple's Command Line Tools work. Native MSVC/clang-cl is not supported by this
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
- **Wipe math:** translate the production `PSWipeFull` HLSL function to native
  Clang vector arithmetic, with CPU texture sampling stubs. Compare against an
  independent scalar color oracle for crossfade, inversion, Mega Drive black/white
  fades, reverse masks, Encore wiggle, all 32 mask values and 200 base rows, screen
  edges, and five resolutions including an odd size. Check mask coordinates,
  captured-image sampling bounds, and alpha.
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

- **Water refraction:** extract Software's production fixed-point ripple calculation and sine table, compare 131,072 offsets, and exercise 5,184 shader composites across blend modes, alpha values, distances, times and split-view edges. Native harnesses now supply depth for Direct3D's `SV_Position.w`; the earlier reciprocal-depth assumption was incorrect. Check projected capture bounds and native D3D blend-state construction/restoration. `water_capture` executes the actual capture function and transparent-node dispatch with mocked D3D resources: 21,888 composites across 2/16/96 overlapping water pieces must preserve the skyline contribution and a sharp edge. It checks one bounded copy for repeated pieces, disjoint rectangle expansion in all directions, changes of layer/slope, intervening walls/non-ripple planes, standalone draws, all four view rectangles, allocation failure and target resize. Each captured pixel is copied at most once per run; 200 short runs must copy over 98% fewer pixels than the old whole-view path, and fully clipped faces issue no copies. Reverting to per-face capture must fail. Actual GPU copies/allocation and depth/stencil execution require device validation. An earlier Xbox run confirmed the sky cutoff was fixed but still showed jagged scenery beyond the waterfall, with substantially fewer screen operations. The subsequent report says the ripple effect is fixed; its brown walls are tracked by the invisible-geometry regression. The latest Grand Prix log exposed excessive whole-viewport copy cost. Bounded capture tests pass, but a controlled Xbox GPU comparison remains pending.

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

- **Invisible geometry state:** execute production `SetBlendMode`, `D3D_SetBlend` and `GetBlendState`, including the translucent sprite-silhouette to untextured sky-wall transition. All 100 pairs of blend modes must preserve the target color and depth writes, then restore RGB writes and the proper visible blend. The old implementation fails. This tests the actual D3D descriptor construction; Xbox wall appearance still needs confirmation.
