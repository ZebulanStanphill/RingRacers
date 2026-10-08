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
- **Split-screen distortion:** production postprocess dispatch and D3D grid vertices. Check viewport selection/reset, all screen layouts, water/heat sampling bounds at nine resolutions, paused animation, per-view phase speed, intermission capture and unaffected views. GPU shader compilation is covered by Windows CI; Xbox visual validation remains.
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
in `generate.py`, and register its executable in `CMakeLists.txt`.
