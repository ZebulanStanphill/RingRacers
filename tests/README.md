# Native regression tests

These tests run on Linux, macOS, or WSL without game assets, an Xbox, SDL,
Direct3D, or vcpkg. Requirements: Python 3.9+, Git, CMake 3.24+, and `clang++`
with its AddressSanitizer/UndefinedBehaviorSanitizer runtimes. Ubuntu's Clang and
Apple's Command Line Tools work. Configuration fetches pinned glslang and SPIRV-Cross sources and needs network access on the first run. Native MSVC/clang-cl is not supported by this
harness; the Windows `warp` job below compiles the Direct3D 12 driver and shaders, and
the Build workflow (`.github/workflows/build.yml`, `uwp/build.ps1`) builds and packages the UWP app.

## Running

From the repository root:

```sh
cmake -S tests -B build/regression \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/regression --parallel 4
ctest --test-dir build/regression --output-on-failure --output-junit results.xml
```

The tests extract production code from this checkout; set `RR_SOURCE_DIR` to test
another one. `cmake --build` regenerates harnesses when tested engine files or
templates change. Variants built with `_UWP` also define `HAVE_VIBERANT`, as UWP
builds have the Direct3D 12 driver by default; engine code under `HAVE_VIBERANT`
does not assume `_UWP` (see "Direct3D 12 without `_UWP`" under Coverage). The standalone test project does not configure the
engine's normal CMake project or change its source files.

Which hardware driver draws is decided at run time by `HWR_VibeRant()`
(`src/hardware/hw_driver.h`): the constant 1 with `_UWP`, the constant 0 without
`HAVE_VIBERANT`, and the setting `hwr_viberant` with `HAVE_VIBERANT` but not `_UWP`, a build
that has both drivers. The `_uwp` and `_desktop` variants therefore run the
Direct3D 12 and the OpenGL behaviour respectively. A variant named `_both` is
built with `HAVE_VIBERANT` only, links the engine's own `hw_driver.c` (through
`tests/hw-driver.cpp`) and sets `hwr_viberant` to true and then false in one
executable, expecting the `_uwp` results and then the `_desktop` ones.

The native tests have no desktop variant of the Direct3D 12 driver: engine CMake
enables it by default with `SRB2_CONFIG_UWP`, which defines `_UWP`, and in Windows
desktop builds made with MSVC or clang-cl (the `windows` job's, which builds the desktop
presenter). All built-in shader compilation commands define `_UWP` and
`RR_D3D12`, in desktop builds too; the HLSL therefore keeps only those active branches.
The Windows WARP harness uses the same definitions, except that its second
executable, which runs the desktop presenter, is compiled without `_UWP` (below).

A desktop variant (built without `_UWP`) exists only where the extracted code
differs under `_UWP`: it is the one check that the UWP changes stay off desktop
builds. Those are `online_desktop_hw`, `menu_desktop`, `intro_desktop`, `transparency_desktop`, `front_bbox_desktop` and `semibright_light_desktop`.
The code that now picks a driver at run time also has `_both` variants: `transparency_both`, `front_bbox_both`,
`semibright_light_both`, `sky_claim_both` and `horizon_cache_both`.
Where the preprocessed code is identical, a second executable would only repeat
the first, so the test runs the cases for each runtime setting (for example both
renderers) in one executable.

Sanitizers are enabled by default and any reported undefined behavior fails the
test. Use `-DRR_SANITIZERS=OFF` only for a toolchain without sanitizer support.
Assertions remain enabled in every build configuration, including Release.
To run one group, use e.g. `ctest --test-dir build/regression -R '^online_' -V`.

## Coverage

- **Metal presenter, a window out of sight:** `metal_present_seen` runs the production
  decision of VibeRant Metal's presenter (`metal/mtl_present_seen.h`, plain C++; the
  presenter itself only builds on macOS) against a model of a `CAMetalLayer` with three
  drawables and a display that, per refresh, takes a drawable from a window it serves
  (to show it, or to let it go) and none from one it doesn't, with Metal's reports
  coming at once (a Mac's own GPU) or a second late (a runner's virtual display), all
  of them or one in four. A window in sight gets every frame. A window the system
  wrongly calls out of sight is shown a frame per refresh, with no gap longer than
  two, with any of these kinds of report.
  A window that really is out of sight gets two probes at most and never makes the
  game wait, where reports are prompt; where they are late, the game waits a second
  ten times in the first ten minutes and once per five minutes after. A window whose
  drawables the system takes without showing them (a locked screen) gets frames at
  that pace, and nothing waits. A window the system hands drawables back to slowly
  makes the game wait six times in the first minute and four in the ten after. Also:
  no probe goes out while two drawables are unaccounted for unless reports are late,
  the pause after a slow probe doubles from one second to five minutes and ends after
  thirty quick ones or when the window is in sight, a minimized or hidden window gets
  nothing, `-mtlunseen` presents regardless, and a layer in sight without a drawable
  is left alone for a quarter of a second doubling to four. The model's own negative
  controls pass (no probes: the picture freezes under a wrong report; going by the
  count with late reports, all or a quarter of them: gaps of most of a second), and
  defects put into the header one at a time (not kept) each failed an assertion. What a real layer and
  display do is not covered here: `capture.py`'s `blind` scenes are for that ("Metal
  frames" and "On a Mac", below).

- **Metal presenter, what a drawable holds:** `metal_present_check` runs the production
  comparison behind `-mtlcheck` (`metal/mtl_present_check.h`, plain C++) on drawables
  made here from a frame the way the driver's placement makes them: at the frame's own
  size, a level's rounding apart, centered in a larger drawable, a 320x200 frame as
  Fit and as Integer on a 1024x768 screen, and a 1280x720 frame made smaller and
  larger. Each must match; and each of these must not: a black drawable, red and blue
  swapped, upside down, another frame, moved by four pixels, scaled where it was meant
  at its own size, filtered half a pixel off, ten levels too bright, a pixel outside
  the rectangle that isn't black, and a rectangle that isn't in the drawable. Five
  defects put into the header one at a time (not kept) each failed an assertion.
  `metal_capture_judging` (`tests/metal-frames/test_capture.py`) checks how
  `capture.py` judges a run by its log ("Metal frames", below).

- **Texture atlas:** `viberant_texture_atlas` tests the production shelf allocator
  with rounded dimensions, full pages, fence-deferred frees, coalescing, and
  empty-page reset. GPU sampling and Xbox resource creation timing require
  renderer captures and device testing; see [atlas notes](../docs/viberant-texture-atlas.md).


- **Plane polygon loops:** `plane_polygon_loops` runs the production plane polygon
  construction and seg adjustment on a four-subsector map whose partitions cross a
  quarter of a unit apart. It checks that each polygon has exactly its GL seg loop's
  corners, that the T-join pass leaves loop polygons alone, that dented and unclosed
  loops fall back to the BSP cut while a rounding-sized dent is accepted, that a loop
  starting with GL segs is complete, and that a leaf the BSP walk skips still gets its
  loop. It also moves polyobjects as the game leaves them, to a spawn spot far from
  their anchor, never moved, and two sharing a vertex with different anchors, and
  checks that every polygon (and the polygon points of every seg) is made of the map's
  own corners, where the node builder saw the polyobjects, and that the live vertices
  end bit for bit where they were, a shared one included. Prevents seams of void
  between a floor's subsectors where BSP partitions cross within the merge tolerance,
  found on Test Run's bowl, and Balloon Park's funnel hidden by the floor of the
  sliding blocks' control sector, which reached from the map's polyobject to where the
  blocks had moved.

- **Startup intro input:** `intro_uwp` and `intro_desktop` execute the production
  intro ticker/responder at immediate and 17/18-tic boundaries, preserving the
  disclaimer visibility and progression guards, natural durations, key mapping
  and event latch behavior. `startup_gamepads` executes the initial SDL
  controller-added event drain and open handler against mocks, checking failed
  opens, instance IDs, preserved buttons and no duplicate opens. The pre-fix
  ticker fails the UWP immediate associates-card skip case. See
  [timing analysis and device limitations](../docs/uwp-intro-skip.md).

- **Floor sprites under the floor:** `splat_clear` executes the production
  `HWR_FloorOverFloorSprite` and `HWR_ClearFloorUnderSprite` (cut from
  `hw_main.c`) and the lines of `HWR_DrawSprite` that call them after
  `HWR_SpriteVertices`, with the level's `P_FloorzAtPos`, `P_CeilingzAtPos`, and
  `P_BaseStepUp` stubbed. For a level and a sloped sprite (a camera that isn't over
  its center), a floor exactly the clearance under the sprite, far under it, or a
  hair above the clearance must leave the corners bit for bit as they were. A
  rise at any one of the 25 sample points (interior, edge, or corner) must move
  all four corners toward the camera along their lines of sight by one common
  share, onto a plane parallel to the sprite's and raised by 1.25 times the
  clearance it needed, with the picture unmoved. The largest rise wins; one
  of more than a step is another floor and is ignored, even beside a smaller
  one that counts, while one of exactly a step counts. A camera under the plane,
  in it, or above it by less than the clearance leaves the quad alone, a camera
  barely above it clamps the share to a quarter, and an edge-on sprite is left
  alone. A flipped sprite on a ceiling asks `P_CeilingzAtPos` (and an upright one
  only `P_FloorzAtPos`), and moves toward a camera below it. The points asked
  must be the grid over the quad, including its four corners and its center,
  with the object's `z` and height. The call must happen after
  `HWR_SpriteVertices` and only for a floor sprite. Prevents a sneaker panel on
  Robotnik Coaster's track from being hidden by the floor under it in the
  hardware renderers.

- **Upside-down walls:** `upright_wall` executes the production cut of a
  one-sided wall to the part whose top is above its bottom, and the call that
  uses it. For VibeRant, a wall of a sector whose ceiling is below its floor
  (Ice Paradise's sky barrier) or with no height must not be drawn, and a wall
  whose top and bottom cross along a slope must become the triangle up to the
  crossing, with positions and texture coordinates taken from each edge. Legacy
  GL must draw every wall as it is, and both must leave the wall untouched for
  the sky walls that follow.

- **Quit signals:** `quit_signal` executes the production handler of SIGINT and
  SIGTERM. The first signal must only leave a note for the event loop
  (`I_QuitSignaled`), with the program still running and the handler still in
  place; a second one must end the process at once, without running exit
  handlers. The old handler ran the whole shutdown from inside the signal, which
  could hang or crash when the signal had interrupted an allocation or a
  graphics driver call.

- **Input queue overflow:** `event_queue` executes the production `D_PostEvent` and
  gamepad device bookkeeping. A pad's added event followed by 0 to 1000 axis
  events with nothing reading the queue (stick movement during the loading
  screen) must still register the pad once, and the newest 127 events must
  survive in order. The pre-fix queue wrapped at 128 events and lost the added
  event, so the pad's input was ignored. Also checks a removal and a second pad
  across an overflow, and bookkeeping that posts an event of its own.

- **Xbox Fullscreen row:** `video_fullscreen` and `video_fullscreen_uwp` execute
  the production SDL device-family query, menu definition, removal helper and
  refresh function through the opening hook in `M_InitOptions`. Only Xbox in the UWP variant removes Fullscreen; desktop,
  mobile and unknown UWP families retain it. Check all other rows stay in order,
  the spacer directly precedes Screen Scaling after removal, repeated refreshes
  do nothing, and the helper handles empty, absent, first and last items.
  SDL's device-family result is mocked; real device detection and menu drawing
  require the UWP app on a device.

- **Renderer setting at startup:** `renderer_setting`, `renderer_setting_uwp`,
  `renderer_setting_both` (a desktop build with Direct3D 12 only),
  `renderer_setting_vulkan` (Vulkan only, as on Linux),
  `renderer_setting_metal` (Metal only, as on macOS), and
  `renderer_setting_all` (both backends, as on Windows) execute production
  setting callbacks and the SDL command-line selection block with real renderer
  mappings and driver lifetime bookkeeping. Fresh child processes check every
  setting the build lists, `-software`, `-opengl`, `-d3d12`, `-vulkan`, and `-metal` where
  available, and `-renderer` with every listed name, and that the driver and
  the backend that start are the ones the setting names.
  The Metal-only case also checks the exact list of settings and the default the
  production `cvars.cpp` gives the variable, and every variant checks that the
  name of a renderer the build doesn't list selects nothing.
  Config-only cases execute the callback and model application of its queued
  setting; they do not create SDL contexts or initialize a GPU driver.
  Check stale startup queues, repeated changes before a frame, command-line
  priority and load failure. Desktop cases start each hardware renderer
  separately, switch through Software, then select each other one (the other
  driver, or VibeRant's other backend): no switch is queued, the saved setting
  survives, and the exact restart notice is printed. A failed
  start cannot unlock the driver choice. These native tests do not verify real
  Windows driver initialization, menu drawing/popups, or presentation.

- **Renderer description:** `renderer_description_both`, `renderer_description_vulkan`,
  `renderer_description_metal`, and `renderer_description_all` (the same four kinds of
  desktop build) execute the production `M_RefreshAdvancedVideoOptions` with the
  production `vaopt_e` for every renderer setting the build has, while Software, Legacy
  GL, or a VibeRant renderer runs. The lines above the Renderer row must describe the
  renderer the setting names, and with Software, which names none, the VibeRant renderer
  the build lists first: never Direct3D 12 in a build without it, which is what a Linux
  or macOS build said. The options heading must name the renderer that runs. Menu drawing
  is not covered.

- **D3D12 rolled sprites:** `sprite_brightmap` also executes the geometry-only
  helper and the extracted HLSL integer `RollSource` function. For all angles
  1–71, both horizontal flips, asymmetric patches with holes, transparent
  patches, and default/frame/default-slot pivots, compare canvas crop, dimensions,
  offsets and pixels against production CPU rotation (itself checked against the
  vanilla fixture). Check cache hits and zone owner clearing, and pixel-unit UVs
  and padded base-texture texel scales. No rotated bitmap is needed by this path.
  The geometry array costs 9 KiB per used sprite viewing-angle slot and uses
  `PU_ROLL_GEOMETRY` (a level tag) with a zone owner pointer. Both level
  teardown and rotated-patch purges on WAD reload clear the cache;
  it must not use `PU_PATCH_ROTATED`, whose destructor expects a patch object.
  A surface payload enters the existing immutable draw-constant ring, and batching
  splits when that payload changes. Light and spring clipping interpolate the same
  canvas-pixel coordinates. The existing software sprite pixel shader handles
  rotation, so there are no additional PSOs to prewarm. Its base/brightmap
  sampling uses the same mapped source texel and retains size-mismatch fallback.
  CPU rotation remains for paper/floor sprites, linkdraw sprites, shaders off,
  active custom sprite shaders, every non-nearest or mipmapped filter mode, and
  art over 1024 pixels or pivots outside ±4096 pixels. Models, precipitation,
  shadows and non-D3D12 builds retain their paths. The five-second VibeRant perf line
  reports submitted shader/CPU rolled sprites and geometry computations.
  Windows WARP's `roll-sprite` scene compares every target pixel with a CPU
  inverse-mapped patch uploaded through the ordinary masked path, including a
  brightmap, both flips, an off-centre pivot and 5/45/90/135/265/355-degree angles.
  WARP, Xbox visuals and the performance improvement must be checked on Windows
  and the device; native tests do not measure those.


- **UDP send failures:** execute production `SOCK_Send` and `SOCK_SendToAddr`
  against a `sendto` mock, with host POSIX errors and the extracted production
  Winsock aliases (including raw 10065/10051). Check successful sends, would-block,
  refusal, host/network-unreachable drops, recovery on the next send, disconnected
  peers, broadcast/socket selection, address lengths, and fatal unexpected errors.
  Against the pre-fix sender both executables abort on the first unreachable
  error. These checks run under ASan/UBSan; live netcode retries/timeouts
  and Xbox behavior remain untested. Run with `ctest -R '^udp_send_'`.

- **Encore post-image selection:** execute full production `P_CalcChasePostImg`
  and `P_CalcPostImg` with real engine headers, plus chase water/heat helpers.
  Check heat, water, no distortion and water priority with and without Encore,
  independent mirroring, first-person gravity flip, FOF bounds/eligibility
  (including views below, at and above a heat FOF), awayview selection (with the
  awayview object and the player's own eyes on opposite sides of the water, so the
  awayview height is checked in first person and chase), matching split views and
  untouched unrelated views.
  Flat FOF heights, heat-tag lookup and removed-object detection are dependency
  stubs; these checks validate selection, not rendered pixels.

- **Online catch-up and title card:** the production packet sender, tic expansion,
  title-card function, client-state enum, packet structs, and server `PingUpdate`.
  Check all eligibility/missed-packet combinations, low-byte wraparound across
  the client receive window, packet contents/length, per-clock-tic rate limiting,
  and preservation of simulation tics. Check that only online UWP Legacy GL
  clients defer the blocking title, while hosts, Software, desktop and demos keep
  it; also check clock refresh after an offline load. A delayed-loading simulation
  tests the server's delay-limit decision with production keepalives and
  another healthy remote player, across three load durations and two limits.
  Compile/run UWP hardware and desktop hardware variants; the desktop one
  checks that the online changes stay off desktop builds.
- **Options slide:** production reset, tick, quit, and color-change functions with
  actual menu structures. Exercise exact and skipped completion tics, clock
  wraparound, repeat target changes, resets during a slide, and instant wipes.
  The desktop variant compiles upstream's timing branch and checks that it is
  unchanged on desktop.
- **Frame capture tic list:** `frame_capture` runs the header behind the engine's `-shottics`
  capture mode (`src/frame_capture.hpp`; the mode itself is `src/d_framecapture.cpp`). Check that
  a list of level times is read into ascending tics without repeats, that an empty list, an
  empty item, a sign, a decimal, a hex number, a suffix, a space, a ten-digit number and a list
  over the limit of 1024 are refused, and that screenshot names are `<MAP>_<tic>.png` with the
  map's name cleaned to letters, digits, `_` and `-`, so that no map name can name another
  directory and the same replay and tic always give the same file. The capture itself (one
  frame per tic, the screenshot, the exit) needs a game with its data and a renderer, so it is
  covered by running it, not here; see "Capture mode" in
  [tests/frame-compare/README.md](frame-compare/README.md).
- **Bounded diagnostics:** production trace implementation and frame-window
  selection. Check fade expiry/event caps, title sample caps, both frame windows
  (with their line caps empty, so the edges of the title window and of the 10-20 s
  race window are decided by the clocks alone), load resets, tics across the unlogged gap, camera/presentation snapshots, missing
  player objects, unpresented frames, and that tracing
  leaves the player's object unchanged. One executable runs every case with
  Software and with Legacy GL, which differ only in the present slot the trace
  reports.
- **Add-on shader translation:** compile the production GLSL adapter, glslang
  parser/linker and SPIRV-Cross emitter from `r_viberant/`. Translate 36 shader/layout
  pairs with reordered varyings, uniforms/initializers, matrices and fragment
  coordinates; reject invalid interfaces, malformed sources and invalid
  layouts, then verify compiler recovery. The 1 MB source limit is checked with a
  valid shader padded by a comment: either stage translates at exactly 1 MB and is
  refused, for its size, one byte over. Check the expected constant-buffer
  declarations, std140 pack offsets and resource slots directly, with every
  register in space 0. Native translation emits 79 general Shader Model 5.1
  stages to `translated-shaders`; Windows CI compiles these and the smoke-test
  add-on's 19 stages there as SM5.1, for 98 total (the 79 include one stand-in screen
  effect fragment, from `TranslateScreenEffect`, which the screen-effect scenes below draw).
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
  validate, leaves valid unrefracted water. Cleaning discards everything. Loading
  refuses the default shader slot, slots below it or past the last base shader, null
  code and a source over 1 MB, logs that, and leaves the replacement already loaded
  alone; exactly 1 MB is accepted, trailing NUL padding is dropped and NULs inside the
  source are kept. The
  Direct3D 12 WARP harness checks the same loader through pixels (below). Add-on
  visuals and load cost on Xbox remain untested.
- **Installable shader smoke test:** [download and instructions](addons/shader-smoke/README.md).
  The shipped PK3 marks ordinary floors/walls with magenta/green checkers and
  ripple-enabled water with moving cyan/orange bands. Its packaged entries pass
  through the production shader loader; native checks verify five stage bindings,
  water's default vertex stage, untouched targets, missing-path rejection, nine
  translated target/layout pairs and the water refraction variant. Windows CI
  compiles the pack's 19 SM5.1 stages, bringing the combined translated corpus to 98.
- **Installable renderer test map:** [download and
  instructions](addons/renderer-test/README.md). A generated UDMF race map,
  `D_RenderTest_v1.pk3`, with a room each for fog, overlapping water and heat
  haze, polyobject ripples, wall light slices, translucent ordering and
  brightmaps, a rainy skybox hall, a parity wing for renderer comparisons,
  and a sprite-textured 3D model (the model
  files are installed beside the game, not in the PK3). CI builds ZDBSP,
  rebuilds the map from `build.py`, compares every entry and validates the
  nodes, and uploads it as `renderer-test-map`.
- **Wall light slices:** execute production UWP wall splitting and clipping with
  real slope/FOF height evaluation. Reproduce a Water Palace pixel capture
  (line 1, `WTPTU1`): an 18-unit sloped tier crosses the
  water light plane at -52. The pre-fix splitter draws outside the original
  wall and fails the vertex-bound assertion. Check 152 fixtures, including
  100 seeded sloped walls, reversed slopes, crossing light planes, zero-height
  endpoints, a tier that closes inside its span, a light boundary exactly at the
  wall's bottom with another light below (one piece, no zero-height one), the first
  capture's normal fence, NOSHADE, solid FOF holes and matching/nonmatching
  CUTEXTRA kinds. The solid FOF cases also run as one of the walls Software treats as
  solid (`hwr_splitvoid`): exactly the cut part, the FOF's bottom up to its top, must
  be filled black (`HWR_DrawCutVoid`, through the same clip to the original wall as the
  wall's pieces), and nothing for other walls or kinds; a FOF reaching below the wall
  fills only down to the wall's bottom, and with two stacked solid FOFs the second, whose
  cut lies wholly below the wall, fills nothing. Every textured piece must be drawn with
  the wall's texture bound again after a fill (`HWR_GetTexture`). Dropping the solid-wall condition or
  the rebind fails it. An independent sampled polygon oracle checks exact coverage,
  lighting, preserved UVs and no outside geometry, with the light plane evaluated
  in double precision from the slope's own origin, direction and gradient rather
  than by the engine's `P_GetLightZAt`; every submitted piece must have height; draw stubs check opaque/queued/fog
  dispatch, texture IDs, colormaps and alpha. These tests verify submitted
  geometry, not Xbox visuals or performance.
- **First-person view after the finish:** `first_person_angle` runs the extracted
  `P_FirstPersonFollowKart` and full `P_RunChaseCameras` loop. First-person
  views must follow a game-steered human kart every tic (finish or Lua), while
  driving, replay, bot and missing-object views stay unchanged. Chase views run
  their existing path without calling Lua twice. Split views use their displayed
  players independently, including on a second tic. The same test covers the
  first-person view's pitch on slopes (`P_FirstPersonFollowSlope`,
  `P_FirstPersonSlopePitch`, and `R_SetupAimingFrame`): the slope's full angle
  facing up it, the opposite facing down it, none across it, and its cosine share
  in between; a quarter of the remaining way each tic, never past the target,
  and exactly level again in the air; the kart's angle rather than the local one
  in a replay and for a dead player; the pitch added to the view's own aim in
  both first-person cases and to neither the chase camera's nor a cut-away
  view's; a fresh start, without easing, after tics spent as a chase camera; and
  one pitch per split-screen view. Each fails with the matching production line
  changed. This checks local view state,
  not a rendered race. Run with `ctest -R '^first_person_angle$'`.
- **First person on the title screen:** `first_person_title` runs the extracted
  `R_ViewNeedsCamera` and the part of `R_SetupFrame` that settles on the camera
  or the player's eyes. With 1st Person set, the title screen (arriving from a
  level or at startup) and a level view of a player with no kart must use the
  camera, since first person reads the player's kart; a player with a kart must
  stay in first person. It doesn't load the title map: that was checked by hand,
  where leaving a level for the title screen in first person crashed before the
  fix in both renderers. Run with `ctest -R '^first_person_title$'`.
- **Camera Position menu:** `camera_position` compiles the extracted
  `options-video-camera.c` and Video Options table. Camera Position must sit
  between Color Profile and the gap before Advanced, with one row per local
  player on that player's menu variable. Opening the menu and its ticker must
  copy `chasecam` into the rows without setting it, changing a row must set only
  that player's `chasecam`, and the definitions in `cvars.cpp` must keep
  `chasecam` On/Off and saved, with the rows' variables menu-only. Variable
  storage and menu drawing are mocks; it doesn't draw the menu or load a config.
  Run with `ctest -R '^camera_position$'`.
- **Translucent wall lighting:** `translucent_wall_light` compares extracted hardware
  middle-texture and 3D-floor side lighting with Software, including light lists,
  fog colormaps, fog lights, NOSHADE, opaque walls, tripwires and the alpha 242/243
  boundary. A translucent window in a dim, colormapped sector darkened with distance
  in hardware but not in Software; found comparing Xbox screenshots of the renderer
  test map's room 1. Draw and renderer dependencies are stubs; no pixels are rendered.
- **Title background light lists:** replay the title map's actual BSP with
  production `HWR_AddLine`, seg processing, wall light slicing, clipping,
  `R_Prep3DFloors`, and all 72 authored slope constructors. All 58 cold first
  wall submissions must equal an explicitly prepared reference and the second
  traversal in order, vertices/UVs, light, colormap, flags, alpha, and queued/fog
  choice. Because that replay is differential, `R_Prep3DFloors` itself is also
  checked against light lists worked out by hand from the title map for two
  sectors (heights, light levels, colormaps, casters and flags of the fog,
  solid and colormap-only FOFs). Focused cases cover NULL back sectors, absent FOFs, a valid list left
  untouched, portal-search exclusion, real-sector preparation before the fake-flat
  copy, preserved target/control moved flags, and the normal subsector refresh.
  Re-preparing every FOF sector with `numlights` zeroed, as the subsector refresh does, must keep the same
  light-list allocation and rebuild identical contents.
  The first traversal must also prepare at least one cold back sector from
  `HWR_AddLine` itself, so the replay cannot stop exercising the fix; without it the
  cold walls differ from the reference. Compact title-map JSON
  and uncompressed ZNODES are expanded at configure time; generated initialization
  headers are never committed. Texture/GPU calls and non-wall subsector work are
  stubs, so this checks wall submissions rather than rendered pixels or the wipe.
  The faithful slope setup retains existing signed-shift UB: the title targets'
  `title-shifts.ignorelist` disables **only `shift-base`** in `MakeViaMapthings`
  and `FixedDiv2`. ASan and all other UBSan checks remain enabled, including in
  traversal and lighting. No authored slope touches the affected FOF sectors;
  retaining the complete map still checks the other submitted sloped geometry.
  The same replay also runs production `HWR_ProcessSeg` on one of the map's one-sided walls with its floor given
  a slope (24 units up at the wall's start, rising along it) and a row offset, in all four pegging modes (top,
  lower unpegged, and both with `NOSKEW`): every corner's texture row must be Software's `rw_midtexturemid` minus
  its height, which for a lower-unpegged wall follows the floor's slope. Removing the fix for lower-unpegged
  walls fails it by the floor's 24 units, the Green Hills Zone pit walls' misalignment.
- **Pixel history diagnostic:** the Direct3D 12 WARP `surface-probe` scene (below)
  checks the driver side: command-ordered copies, logged pixel ownership and the
  inactive path. There is no native test of the `gr_surfaceprobe` console command's
  argument parsing; a mistake there can only give a wrong message or sampling pixel.
  This validates the diagnostic, not the underlying rendering.
- **View interpolation after a reset:** execute the production `R_InterpolateView`,
  `R_UpdateViewInterpolation`, `R_ResetViewInterpolation` and `R_SetViewContext`
  with their real state. After a level load (the title screen after EXIT GAME from a
  race), tics can run before any frame is drawn, because loading skips frames. They
  must not age the reset or copy the previous level's last view into the old view:
  the first drawn frame has to be the new view, not a lerp from the old level (which
  put the title camera outside its map). Check eight undrawn tics, resets on and
  after the last update's tic, plain interpolation, one view's reset leaving the
  others interpolating, and the sky view. The pre-fix implementation fails the
  undrawn-tics case. The clock, BSP lookup and freelook setup are stubs; this is
  render-only state, and the Xbox title screen after EXIT GAME is untested.
- **Wipe math:** translate the production `PSWipeFull` HLSL function to native
  Clang vector arithmetic, with CPU texture sampling stubs. Compare against an
  independent scalar color oracle for crossfade, inversion, Mega Drive black/white
  fades, reverse masks, Encore wiggle, all 32 mask values and 200 base rows, screen
  edges, and five resolutions including an odd size. Check mask coordinates,
  captured-image sampling bounds, and alpha.
- **Fill transparency:** execute production `V_DrawFill`, HUD alpha resolution,
  coordinate snapping and `HWR_DrawFill`. The test compares
  2,968 Software/Legacy GL cases across explicit alpha levels, all three HUD alpha
  flavors, HUD settings/fades, slide overrides and split-view layouts. The actual
  fully faded Watching underline must submit no quad. Check partial alpha and
  blend flags, opaque geometry/colors, no-scale rectangles, opaque fullscreen
  clears and translucent fullscreen coverage at 320×200, 1280×720 and 853×480;
  six direct hardware calls with fully transparent/unsupported alpha levels must
  return safely. Replacing either fill implementation with the pre-fix version
  fails the parity checks. Recording stubs stop at the Software quad and hardware
  polygon/clear boundaries; Xbox visuals and GPU blending remain untested.
- **Raw picture formats:** production conversion and cache-loading functions. Check all five source modes, palette destination widths, grayscale/RGB alpha, downscaling and upscaling, row padding, malformed headers/modes/dimensions/pixel lengths, and lumps that are exactly header plus pixels or have trailing bytes. The RGB24 case fails with the pre-feature implementation.
- **Polyobject ripples:** run the production `HWR_RenderPolyObjectPlane`,
  `HWR_RippleBlend`, shader availability check, and shared `R_IsRipplePlane`
  against real engine structures. The test checks 1,600
  combinations of independent floor/ceiling flags, control-sector precedence,
  surrounding-sector fallback, absent sectors, Reduce VFX, shader availability,
  opaque/translucent planes, and ordinary-sector/FOF ripple flags. Inspect the
  submitted shader, blend flags, alpha, and translated polygon geometry; invalid
  polygons must submit no draw. Replacing either changed renderer function with
  its pre-feature implementation fails the corresponding behavior assertion.
- **Underwater/heat distortion:** production postprocess dispatch, D3D fullscreen vertices and pixel-shader sampling. Compare more than 14 million UV samples against the unmodified vanilla 2.4 `rhi_glsl_fragment_postimg.glsl` fixture, including full-frame wave amplitude, tic timing (heat is swept every 7 tics to tic 2100 so both of its waves are active on some rows), paused frames, texture padding, viewport edges and all screen layouts at nine resolutions. Check viewport selection/reset, intermission captures, Reduce VFX suppression and unaffected views. The fixture comes from release `data/shaders.pk3` (GPLv2, as the engine); it is the independent Software reference. GPU shader compilation is covered by Windows CI.
- **Transparency ordering:** extract the production node builder, object pass,
  model/sprite dispatcher, linkdraw gating, shared sprite geometry, view transform,
  and slope helpers. Each UWP/desktop executable checks 220 scenes and cases,
  including 128 seeded scenes with an independent depth-order and pixel-composite
  oracle. Cover front/behind objects, mixed alpha flags, model load fallback and
  player skins, empty/objects-only/faces-only frames, opaque objects, always-on-top,
  linkdraw display offsets (including a yawed view where a corner and the center of the
  parent give different depths), actual precipitation-sized storage with the
  precipitation both behind and in front of a wall, disjoint bounds,
  slanted walls, sloped floors, moved polyobject planes, near-plane crossings,
  pitched billboards, viewpoints translated along each axis (the wall and sprite
  footprints overlap on screen only for the correct translation), and an attachment sorted with the faces. Three integration
  cases execute the merged dispatcher with adjacent water pieces and inserted
  sprites/models, checking capture continuation, interruption and end-of-view reset
  with and without an inserted object; desktop builds retain their existing capture behavior. Replacing
  both production dispatchers with their pre-feature implementations fails the
  front-object draw-order assertion in both variants. Dependency stubs record
  draws and blend a shared CPU pixel; they do not render complete game scenes.
  Another 42 cases check coincident red/blue fog planes at both volume boundaries,
  three eye heights (including equal floor/ceiling distances), seven surrounding
  plane counts. The first-created rover must remap last,
  matching Software's equal-height lightlist winner; non-tied planes retain their
  distance order, and coincident fog walls retain reverse drawcount order and
  separate the plane runs. Run each scene with libc qsort and with equal-key
  groups reversed, another legal qsort result, so stable host sorting cannot hide
  the regression. The pre-fix comparator fails both executables' face-order
  assertion. These checks record submissions, not fog pixels: Software gives both
  coincident planes the selected lightlist colormap, whereas hardware retains each
  rover's control colormap. Exposed coincident exit sides also retain their existing
  order (Software's FOF list order, hardware's reverse drawcount order). Complete
  overlapping fog visuals still need an Xbox test.
- **Fog boundaries and captures:** compare 24,960 production shader row
  selections with the actual Software zlight/scalelight tables and 16,384
  palette-remap samples. Extract bounds from `r_viberant/viberant_captures.inc` and
  exercise full/split viewports, eye-plane crossings and behind-camera/offscreen
  faces in `fog_capture_viberant`, including oversized-face clamping. Shader-only
  checks stay in `fog_boundaries`. `fog_capture_viberant` checks capture copies,
  failure, resizing, bindings and constants against literal expected boxes (including fractional vertices, which
  pin the floor/ceil edge rounding) and literal constant bytes for each viewport,
  poly flag, light level and view width. Wall-slice tests reject directional light
  adjustment on fog. Windows WARP executes the shipped SM5.1 path and rejects
  a reciprocal-depth fog shader with the `fog-palette` oracle. Complete fog
  volumes, overlapping visuals and Xbox performance still need device testing.
- **Surface lighting:** `surface_light` compares the built-in shader's colormap
  row with Software's actual plane and wall/sprite tables for all 256 light levels,
  table edges and midpoints, near and far distances, and full/half-width views.
  It also checks the driver's surface classification and viewport light ratio.
  Sprites and walls in dim sectors were darker than Software's, found comparing
  Xbox screenshots of the renderer test map. WARP's `surface-light` scene reads
  distinct row colors for a plane, wall and sprite at the same known depth.
  Semibright sprites get the same checks. Software keeps the sector's light for them and squeezes the
  distance into the nearer half of its table; Legacy GL had raised the light level instead, which
  is far brighter in a dim sector (light 64: rows 0 to 6 against Software's 22 to 31). With
  `u_light_plane` 2, the shader's row must equal `scalelight` read through the index lines
  extracted from `R_ProjectSprite` and `R_SplitSprite`, including the `SC_SEMIBRIGHT` statement, for all 256
  light levels, the same distances and both view widths, and a few literal rows pin the numbers
  (light 160 at 200.125: plain row 10, semibright row 4). The driver lines that strip
  `HWR_LIGHT_SEMIBRIGHT` from the light level and choose `lightPlane` 2 are extracted too: only a
  wall, sprite or model with the bit gets 2, and planes, water and fog keep 1 even with it set.
  `semibright_light_uwp` and `semibright_light_desktop` extract `HWR_SemiBrightLight`, `HWR_UseShader`,
  `R_ThingIsSemiBright` and the end of `HWR_ObjectLightLevelPost`. With the Direct3D 12 driver and shaders the
  level is left alone and only the bit is added (level 0 to 255 kept); without shaders, and in the
  desktop build, the old `192 + level / 2` applies and the bit is never set; other things get neither.
  `semibright_light_both` runs both cases in one executable, switching `hwr_viberant`.
  Where the calls are made is checked as text, because the sprite and model draw functions need a map
  and a renderer to run: `HWR_SplitSprite` (both pieces), `HWR_DrawSprite` and `HWR_DrawModel` call it right after each
  `HWR_Lighting`, and `HWR_DrawPrecipitationSprite` and `HWR_DrawDropShadow` do not. WARP's
  `surface-light-semibright` scene draws a semibright sprite at that depth and expects row 4.
- **Batch sort keys:** extract the production batching code from `hw_batching.c` (the sort-key struct, key builder, comparator, collection and the `HWR_RenderBatches` draw loop, with a recording driver) and check the Legacy GL batch order: skywalls and horizon lines first in submission order, then shader, texture, brightmap, polyflags, colors and light fields (including colors above and below `0x80000000`), identical states in submission order, and the shaderless order. It also checks `PF_BeforeSky`, the flag of a masked wall texture between two sky-ceilinged (or two sky-floored) sectors. Software shows that texture against the sky, but in hardware the invisible sky polygons, which batching draws first, wrote depth over the part of the texture above the nearer sky ceiling and cut it off flat (found on the renderer test map). Flagged polygons must sort before the untextured group and before every ordinary textured polygon, with and without shaders and in every submission order. Among themselves they keep the normal order (texture, brightmap, polyflags, colors, light, submission index; the shader no longer takes part). The order of all other polygons, the untextured group above all, is unchanged by flagged polygons. A polygon that is both untextured and flagged stays in the untextured group. The draw loop test submits a flagged wall, a sky occluder and an ordinary wall in all six orders, and a larger scene, and checks the draw calls come out flagged wall, occluder, ordinary wall, and that an unflagged wall is still drawn after the occluder. These fail if the key builder stops giving flagged polygons their own group. Run with `ctest -R '^batch_sort_keys'`.
- **Sprite brightmaps:** execute the shared Software brightmap lookup, real patch converters/pixel reader, production sprite rotation and companion cache, patch destruction, hardware sprite/precipitation admission, draw texture-selection blocks, mipmap loaders, batch replay and Direct3D texture binding. The original 13 admission cases cover frame/default names (including Ark Arrow `SYM0` / `SYMBXXB`), skin sprite2, unknown-frame fallback, missing names, absent maps and independent width/height mismatches. Rotation checks cover 852 transforms: all 71 nonzero angles, horizontal flips, two asymmetric source shapes, explicit/default/implicit noncentral pivots, differing source/map header offsets, asymmetric holes, different opaque bounds and fully transparent maps. An independent coordinate oracle checks 2,925,912 pixels in both vertical orientations; a pinned v2.4 raster snapshot checks that base/Software pixels and headers stay identical. Shared brightmaps across distinct base offsets/pivots must not alias; cached lookups must allocate nothing. Rotated-patch purge/rebuild, source-only tag purge and 144 dynamic brightmap reloads exercise real patch cleanup and check stable live allocation counts and released GPU cache entries. Ordinary and light-split paths bind rotated pairs with Batching on/off (72 cases), alongside 3,408 production horizontal/vertical UV orientations, the existing eight ordinary/precipitation path/mode transitions and resident-data upload check. For every rotated case the coordinate oracle must also reject a missing map, a blank map of the right size and the right map moved one pixel in each direction (the previous dropped-map behavior and a one-pixel transform error), so it cannot go blind unnoticed. Zone allocation/WAD lookup, color translation and GPU allocation/upload are dependency mocks; rotation/conversion/cache/destruction and extracted projection/binding blocks are production code. Vertical orientation checks execute production texture-coordinate assignment and compare shared pixel coordinates, without running full billboard geometry or GPU rasterization. These tests do not validate Xbox visuals. Run with `ctest -R '^sprite_brightmap'`.
- **Encore brightmap colors:** run production sprite colormap selection, Software
  full-bright selection, mapped-cache lookup/update, column conversion and light-table
  conversion. Check 12,288 combinations covering all 256 indices, 1/2/3/4-byte
  output, both palette settings, ordinary/Encore/exempt sprites, cache reuse and
  in-place remap changes. Omitting or repeating the production column remap
  fails the pixel comparison; for every remapped combination the test also checks
  that the full-bright color of the unremapped or double-remapped index differs
  from the right one, so neither error can hide. Direct3D 12 WARP draws four
  already translated palette indices, dark and bright, with Palette Rendering on and off. The native
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
  submitted triangles. The default run also checks, in process, that the independent
  sampling oracle rejects four injected errors: restoring the old per-pixel depth
  behavior (a full wall and an edge), and the clipping errors the preservation cases
  guard against (ordering lost, or the `MF_SPRING` gate lost), since old depth testing
  already preserves those two cases. Software draws every 3D floor
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
  32-corner silhouette that needs 48 (it must report none); telling the clip it has room
  for 12 corners in an output of 5 must change the guards, so a zero count can be trusted. The smallest
  fuzzed scene that overflowed (two sloped 3D floor planes of 29 and 32 corners, a 3D floor side, a full-size
  spring; hex-float literals copied into the test) must submit only triangles or the quad and still draw something, and
  3000 random views from a SplitMix64 generator (not `std::uniform_*_distribution`, which differs between
  standard libraries) of such planes and sides must all finish. Without the bounds both
  fail with an AddressSanitizer stack-buffer-overflow in `HWR_SpringHalfPlane`. A seam between two sloped floors
  on one plane, which Software leaves undrawn, is replayed from a Green Hills Zone `gr_springprobe` capture: the
  probed camera, a spring and the seam side passing beside the camera below eye level, with the side's two tops
  raised by every combination of 0 to 24 float ulps and the camera jittered by up to 0.01 units (1,250 cases,
  plain and 3D floor). The spring must stay the exact quad in every case; before flat silhouettes were rejected,
  325 cases cut it away. A probed Endless Mine frame (a spring on the side of a diagonal 3D floor beam, its top
  under the beam's top plane, the camera above) must leave the spring the exact quad, since the plane is behind
  it in its own screen columns, while the same spring moved inside the beam must not be drawn; judged by the
  plane's nearest corner anywhere, as before, the first is cut to three triangles. A spring's 3D model follows its sprite (`HWR_DrawSpringModel`, with the model
  drawer and the driver's stencil mask stubbed): it is drawn as any model when nothing is recorded, when the
  spring stands in front of the wall, for another object, and for an always-on-top spring; it isn't drawn,
  and counts as drawn, inside a block with either driver; half hidden, the wall that cuts the sprite is
  marked before the model is drawn, the mask is in use during the draw at the current portal level, and the
  same wall clears its mark afterwards, also when there is no model to draw; without VibeRant, or when a mark
  can't be drawn, the model is drawn whole with no mask; and a 3D floor side that misses the sprite but
  comes within twice its size is marked as well. Each of these fails with the matching line of the
  production code changed. Seg traversal, GPU depth and Xbox visuals still need separate validation;
  the stencil mask itself is a driver scene (`spring-mask`, below).
- **Shared brightmap textures:** `shared_brightmap_texture`, `shared_brightmap_flat`
  and `shared_brightmap_patch` execute production texture/texture-flat and patch
  collection after the same mipmap was used as a brightmap. Its flag must be clear
  and the next polygon must record it as its own texture with no brightmap.
  The pre-fix loaders leave the flag set and retain the preceding base texture.
  `shared_brightmap_batches` executes full production polygon collection, sorting
  and rendering for A+M, M alone, A+M and A alone, using distinct shader IDs to
  preserve that order. A fake driver selects the unit from the flag at each bind;
  every draw checks both units. Before the fix, M alone binds on unit 1.
  `shared_brightmap_gl` and `shared_brightmap_gl_update` execute OpenGL's complete
  `SetTexture`, `UpdateTexture` and `SetNoTexture` with mocked GL calls, checking
  resident and updated brightmap-to-base transitions and base-to-brightmap reuse.
  The pre-fix cache skips M's unit-0 bind. Allocation/conversion dependencies in
  the collection harness are stubs; its mipmaps are already resident.
  WARP's `shared-brightmap` scene checks the second surface's M color after A+M
  in the same frame, for both a resident bind and an in-place brightmap update.
  These native paths have no `_UWP`-dependent extracted code, so there is no
  duplicate desktop variant.

- **Palette brightmaps:** Direct3D 12 WARP executes the production SM5.1
  `PSSoftware` palette path. Brightmapped texels must use the base colormap's
  full-bright row at their palette index; other texels keep the surface colormap
  row at the rasterized depth. `brightmap-palette` varies light and depth and
  checks four palette indices, each dark and bright, as the Encore translation hands
  them over. A surface-colormap shader mutant must fail. `brightmap-smooth` covers
  the smooth-light path. This does not validate Xbox visuals.
- **Direct3D 12 bookkeeping:** `viberant_allocators` compiles the production
  `r_viberant/allocators.h` and `pipeline_key.h` headers under ASan and UBSan, with no
  GPU. It checks `RingAllocator` alignment (including non-power-of-two), wraparound,
  fence blocking with the highest overlapping fence, `Collect` and `Touch`
  retirement, same-submission wrap into retired space, zero, exact-capacity and
  SIZE_MAX limits. `PipelineKey` equality and hashing are checked for each field and
  across a deterministic 65,536-key sample. `IndexFreeList` (shader-resource view
  indices) checks exhaustion, a freed index waiting for its fence (inclusive of the
  completed value), out-of-order and shared fences, ignored out-of-range frees,
  `Reset`, and a range ending at the top of the index space. `SizeClassArena` checks fenced reuse
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
  `atlas-pages` fills three atlas pages, deletes the textures of two, and checks through a test-only page count
  that one empty page is kept and the other released, that the released page is created again once the
  others are full, and that textures in both draw.
  None of this runs during normal rendering or retains diagnostic metadata. The CPU mip generator is checked
  against a scalar oracle for odd sizes and 1×N textures. The 3D texture
repack used for the palette lookup table is checked for row pitch, slice order and
untouched padding.
- **Screen Scaling placement:** `screen_scaling` compiles the production `src/screen_scaling.hpp`, which both renderers
  use to place the frame, under ASan and UBSan with no GPU or engine. Check Fit, Integer at exact fits and k of 1, 2, 3 and 5
  (limited by the width or the height), odd remainders (the extra pixel is at the right and the bottom), wider and taller
  screens, 1:1, and the fallbacks to Fit when the frame is larger than the screen in either dimension, for an unlisted mode
  and for sizes that are not positive. A sweep of 9 frame sizes over about 12,000 screens checks the rectangle,
  centering, maximal k and the fallbacks. `FitRect`, the rectangle a renderer that draws Fit itself (the desktop
  Direct3D 12 presenter) uses, is checked the same way: wider and taller screens, exact fits, a frame larger than the
  screen, odd remainders, the side that is rounded down, a side held at 1, the sizes that are not positive (which give
  what `Compute` gives), and a sweep of 10 frame sizes over the same screens that checks it spans the screen along one
  side, has the frame's aspect ratio along the other and is centered. The Direct3D 12 WARP scenes below draw the placement.
- **Map preview cache:** `map_preview_cache` compiles the production `src/map_preview_cache.hpp`, which the UWP build uses to
  keep each map WAD's thumbnail and minimap bytes and each staff ghost's parsed brief on disk between launches, under ASan and
  UBSan with no engine. Check a round trip of maps (both lumps, the picture only, neither, an empty lump) and ghosts (valid
  with a name, and one the parser rejected), and that a change to any of the key's name, CRC32, size or compressed size
  misses. A file with the wrong magic, another format or parser version, trailing bytes, a count or a length that runs past
  the end, or any truncation (every length of a valid file) is rejected whole, without a read out of bounds. Pruning with
  `keep`, the stability of the pointers `Find` returns across later `Put`s, and when the cache is dirty are checked too.
- **Lump header cache:** `lump_header_cache` compiles the production `src/lump_header_cache.hpp`, which the UWP build uses to
  keep the bytes of the small lump reads made at startup (`W_ReadLumpHeaderPwad`) on disk between launches, under ASan and
  UBSan with no engine. Check a round trip (including an empty read and the longest entry), that a change to any of the key's
  name, CRC32, size, compressed size, offset or requested size misses, and that an over-long or null entry is not stored. A
  file with the wrong magic or format version, trailing bytes, a count, name or data length that runs past the end, or any
  truncation (every length of a valid file) is rejected whole, without a read out of bounds. Only the entries found or put
  since loading are saved, the pointers `Find` returns stay valid across later `Put`s, and a save is needed exactly when
  something was put or a loaded entry went unused. Hooking it into the engine's reads is untested here.
- **UWP data copy record:** `uwp_data_copy` compiles the production `src/uwp_data_copy.hpp`, which decides whether the
  copy of a game data file in the app's internal storage can be loaded instead of the file on the USB drive, under ASan and
  UBSan with no engine. Check the one-line sidecar's round trip (including an empty file, the largest size and times, and a
  file with no expected MD5), and that other magic or versions, missing fields, signs, overflowing numbers, uppercase or
  short MD5s, extra lines, trailing text, embedded NULs and every truncation are rejected. Check the usability decision: a
  matching copy is usable, and a missing source or copy, a missing or invalid sidecar, a source whose size or last-write
  time changed, an expected MD5 that changed (or appeared for a file recorded without one), and a copy of the wrong size
  each give their own verdict, in a fixed order. Check which files are never copied (music, alternate music, sounds and
  staff ghosts, by name without regard to case), what the data folder keeps (a copied file's copy and sidecar, a kept file
  with an expected MD5's `.verified` record, nothing else), and when a kept file's verified record spares its check (the
  file's size, last-write time and expected MD5 unchanged). Copying, renaming, deleting and loading on Xbox are untested
  here. The worker only copies files with an expected MD5 while checking them; the checksum-free music and sound
  packs stay on USB. Legacy no-MD5 sidecar parsing remains covered for existing records.
- **Name indexes:** `name_index` compiles the production `src/name_index.hpp`, the hash indexes that replace the linear
  lump-name, PK3 folder and texture-name scans at startup, under ASan and UBSan with no engine. Randomized differential tests compare
  `HashPositions::FirstFrom` and `LastBelow` with the original forward (`W_CheckNumForNamePwad`) and backward
  (`R_CheckTextureNumForName`) scans over the same hash and `strncasecmp` predicate: real and forced-colliding hashes,
  entries whose stored hash is not that of their name, case variations, names shorter than, exactly and longer than 8
  characters, high-bit bytes, nonzero start positions and misses. `FirstFrom` is also compared with
  `W_CheckNumForLongNamePwad`'s forward scan, whose predicate is a full-string `strcasecmp` of names that can be longer
  than the 8 hashed characters (shared 8-char prefixes, forced collisions, case, past-the-end starts). `FolderStarts` is compared with the original
  `W_CheckNumForFolderStartPK3` prefix scan, including its SLADE single-directory-entry rule and the
  past-the-end result on a miss: nested folders, case variations, prefixes that are not directories (`map/` against
  `maps/`) and high-bit bytes. The texture-id memo's key (the first 8 characters, uppercased) is checked to match
  exactly when the old memo scan's `strncasecmp` over 8 characters matched.
- **Fork version text:** `fork_version` compiles the production `src/fork_version.h`, which formats the version shown on
  the title screen, in `version` and in the startup log, under ASan and UBSan with no engine. Check a release (`v2.4_z1`,
  `v2.4_z1.1`, `v2.4.1_z1`, whatever the revision), a development build (`v2.4_z1-dev (1a2b3c4d)`, also before any release
  as `_z0`, with a longer hash cut to 8 characters, a shorter one kept as is and either case), a revision that is empty,
  null or not a hash (upstream's `illegal` and `Unknown`), which leaves out the parentheses, and truncation into a small
  buffer, which stays terminated and writes nothing past it.
- **Fork release detection:** `fork_version_release` runs `cmake/Modules/ForkVersion.cmake` (the function that
  `cmake/Comptime.cmake` calls at every build) in CMake script mode against temporary git repositories made with their own
  user name and email, ignoring the developer's git configuration. A build is a release only with a `FORKVERSION` other than
  `0`, a clean tracked tree and a tag `<SRB2VERSION>_z<FORKVERSION>` or `v<SRB2VERSION>_z<FORKVERSION>` on `HEAD`. Check that
  no tag, a tag on the parent commit only, a tag for another fork or Ring Racers version, a tag with a capital `Z`, a dirty tree, a `_z0` tag with
  `FORKVERSION` `"0"`, a directory that is not a repository and a missing git are development builds, and that both tag
  spellings (and an annotated tag) are releases. Also checks that the two versions are read from `src/version.h` and
  `src/fork_version.h`. Runs when git is found.
- **Point-in-subsector cache:** `point_subsector_cache` compiles the production `R_PointOnSide` and the UWP
  `R_PointInSubsector` with its 2-way set-associative cache against a randomly generated BSP (axis-aligned and
  diagonal partitions), and compares every answer with an uncached walk of the tree: 400k lookups per BSP over
  repeated and fresh points, from a hot pool smaller than the cache to one far larger than it, with immediate
  repeats. It checks that the cache is empty after `R_ClearPointInSubsectorCache` on a new BSP, that two points
  sharing a set are both kept and that a third evicts the oldest, and that the lookup and hit counters stay
  consistent. Whether the real race hit rate improves is measured on Xbox, not here.
- **Extraction safeguards:** ensure comments, strings, nested blocks and prototypes
  do not truncate extracted functions; reject missing/ambiguous definitions.
- **Direct3D 12 without `_UWP`:** `python3 tests/check-viberant-desktop.py build/ninja-release/compile_commands.json`
  takes the compile database of a normal (Linux or macOS) game build, re-runs the recorded compile
  command of every `src/hardware/*.c` file with `-DHAVE_VIBERANT -fsyntax-only` added and fails if any
  reports an error. It guards that the engine code under `HAVE_VIBERANT` (portals, wall light slices,
  post-processing, palette rendering) does not depend on `_UWP`, so the driver can be built into
  Windows desktop builds; Xbox-only code inside it has its own `_UWP` guard. That code is only
  compiled in by `#ifdef HAVE_VIBERANT`; whether it runs is decided by `HWR_VibeRant()`. CI no longer runs it: the `linux` job builds the
  driver with its Vulkan backend, which compiles this code without `_UWP`.
  The driver itself is not checked here: the `windows` job builds it into the desktop game, and the
  `warp` job runs the desktop presenter.

Implementations under test are extracted at configure time from `RR_SOURCE_DIR`,
not copied into the tests. `#line` directives point compiler/sanitizer diagnostics
back to those engine files. Only dependency stubs, inputs and expected behavior
live in `templates/`, including hand-worked literal bounds and constant bytes
that independently check Direct3D 12 captures. If a function or section moves incompatibly, generation
fails instead of silently skipping its test.

These are isolated native checks. The network simulation does not run UDP clients
or the whole game; the wipe test does not compile HLSL or exercise GPU sampling.
The ripple harness records submitted polygons without executing a GPU shader.
The transparency tests do not validate GPU depth/stencil, portal clipping, model
triangle rasterization, or the complete sprite/model drawing implementations.
These tests do not validate Xbox rendering, frame presentation, deployment, controller
input, performance, or complete replay/gameplay equivalence. The `warp` job compiles
the real driver and shader code, and the Build workflow builds the UWP app.

## Direct3D 12 WARP harness

From an x64 Visual Studio developer PowerShell on Windows:

```powershell
./tests/run-d3d12.ps1
```

`-Engine` tests another checkout.

The harness compiles production `r_viberant.cpp`, `viberant_core.cpp`,
`viberant_upload.cpp`, `viberant_pipeline.cpp` and `viberant_prewarm.cpp` against
the real engine headers. It links the production add-on shader translator, built by
`tests/viberant-translator/` with the pinned glslang and SPIRV-Cross sources. CI caches that build per runner
image, keyed on its two CMake files, and builds it without fetching when the cache is restored, so only the
translator's own source is recompiled. SDL, engine-symbol and headless-present functions are
test stubs. ANGLE is not used. Shaders compile with `fxc` at SM5.1 against the
production root signature. Scenes run on WARP with the debug layer and GPU-based
validation enabled. Any debug-layer warning, error or corruption message, or any
`Core_Errors` report, fails the run. The sole allowlist is warning #820 about
a missing or mismatched optimized render-target clear value: the runtime guarantees
the requested clear color, which the pixel oracle checks. It only affects clear
performance. Scenes cover clears, blends, depth, indexed
triangles, 2D lines and a tiny-ring stress test. A quad is two triangles and the screen's center
lies in only one of them, so the blend, depth, indexed and model scenes also sample a pixel in the
other. The ring stress draws a 32×32 grid of 2×2-pixel tiles per frame for four frames, each tile
a color of its own, through rings of 1,024, 512 and 2,048 bytes, and checks every pixel, so a ring
overwrite, stale constant or corrupt vertex in any draw shows, not only in the last.
The harness checks its own oracles rather than running a negative process per scene.
Before the scenes, a self-test hands every kind of comparison (a screen pixel, a pixel of the
palette step, of an image read earlier or of a probed pixel, a line's rows, a plain verdict, and
the capture-count deltas) a wrong expectation, off by two levels in each channel in turn where that
applies, and requires the specific rejection that kind throws; a right expectation, or one a level
off, must pass. A hit registry then requires every listed scene's oracle to run at least once in the
full run, and no oracle to run that isn't listed, so a skipped, renamed or misspelled scene fails the
run. Six negative controls remain, because each changes the code under test while keeping the
correct oracle, which must then reject it: shader mutants of `water-refraction` and `fog-palette`
(reciprocal depth), of `brightmap-palette` (the surface row instead of the full-bright row), of
`surface-light` (the planes' table for every surface) and of `surface-light-semibright` (the semibright
branch made unreachable), which
rebuild only the pipeline object from the edited HLSL, and `custom-shader-nocustom`, which turns
add-on shaders off. Each is its own process that replays the scenes up to the one whose oracle
rejects it, with the debug layer on but GPU-based validation off, since it runs the same GPU work
as the full run; it must exit having rejected the scene it names, and a control still running
after two minutes is killed and fails, so a GPU or WARP hang can't hold the job. The `warp` job
itself is limited to 20 minutes (it takes about three).
New texture, model and sky scenes cover texture creation, point sampling, reuploads,
delete-and-recreate, mip chains, sky fan-strip rebuilds, and model buffered/lerped
UV fallback (`texture`, `texture-update`, `texture-reuse`, `mips`, `sky`, `model-buffered`,
`model-lerped` and `model-fallback`). Tiny models, lit models and textured sky
aren't exercised, so they remain untested here. WARP doesn't measure Xbox performance;
texture creation time, heap behavior and frame rate need an Xbox run.
Portal scenes require the production PSO preflight to succeed (`portal-supported`).
`portal-aperture` checks color preservation and stencil-1 confinement; `portal-nested`
checks incrementing 1 to 2 and restoring 2 to 1. `portal-restore` checks that the return
pass writes entrance depth, and `portal-depth` checks that the fullscreen far-depth
reset only touches the aperture, allowing farther child geometry while preserving
outside occlusion. `portal-plane` checks world-plane clipping and disabling the plane.
The spring mask scenes cover the stencil bit that hides a spring's 3D model behind the sides
Software draws over the spring's sprite: `spring-mask` marks half the screen from behind a
nearer wall, without changing color, and a draw that uses the mask skips that half;
`spring-mask-unused` checks that the mark hides nothing while the mask is out of use and is
still there afterwards; `spring-mask-clear` checks that the same polygon clears it and that
marking wrote no depth; `spring-mask-portal` checks that a masked draw inside a portal also
stays inside the aperture, and that clearing the mark leaves the portal level and the
aperture's restore as they were.
These pixel scenes use production driver
entry points under debug/GBV; recursive map visuals, transformed camera/model clipping,
and fog/water portal interactions still require further testing.
Void scenes cover `VibeRant_FillVoid`, which turns black every pixel of the current viewport and portal stencil level
that still has the depth clear's far value, the way Software draws what the level doesn't cover. Opaque geometry
keeps its color (`void-fill`), a depth-only (Invisible) surface, standing in for a sky plane or wall, keeps what was
drawn under it (`void-claim`), translucent geometry drawn afterwards blends with black over the void and with the sky over
the claim (`void-translucent`), the fill writes no depth, so a farther surface still shows over the void and is still
hidden by the claim (`void-depth`), the fill stays inside a split-screen viewport although the depth clear covers the
whole target (`void-viewport`), and inside a portal only the portal's pixels are filled (`void-portal`). Which surfaces
claim the sky is decided by `hw_main.c` (`HWR_RenderSkyClaim`, `HWR_DrawSkyWall`) and is not exercised here.
Custom-shader scenes load an add-on fragment shader that outputs a fixed color, compile it
through the production loader, and draw a quad textured with (200, 40, 16). `custom-shader`
must show the fixed color (64, 128, 191). `custom-shader-clean` must show the built-in color
again after cleaning the shaders. `custom-shader-invalid` loads invalid GLSL, which must log a
built-in fallback and draw the built-in color. `custom-shader-nocustom` sets Ignore custom
shaders, so the custom-color oracle must fail: it's the negative control. They haven't been run
locally, and Xbox results are untested. See [the Direct3D 12 add-on shader notes](../docs/viberant-custom-shaders.md).
Dispatch scenes run last, after the model and sky scenes, and replace only the fragment stage, so
the built-in vertex stage places the geometry. `custom-none` compiles with nothing loaded and
draws the built-in color. A fragment for
`SHADER_FLOOR` must show (64, 128, 191) on a polygon (`custom-layout-poly`, with the pixel outside
the quad left alone), on the sky dome (`custom-layout-sky`) and on a model (`custom-layout-model`),
the three layouts. `custom-replace-invalid` replaces the working shader with invalid GLSL, which must draw the
built-in color, and `custom-replace-recover` loads a good one again (191, 128, 64).
`custom-sprite-clip` selects the clipping variant of a sprite replacement. `custom-model-kept`
draws a model with Model Lighting on and only a model replacement, which stays in use, and
`custom-model-lighting` adds a model-lighting replacement, which must take over. Failures that only
the GPU can cause, such as one layout failing to create, are covered by the native test, which also
counts translations, compilations and pipelines, so compiling with nothing loaded or again without a
change is shown to do none of them.
`custom-water-refraction` loads a water fragment that outputs a fixed color with the surface's
alpha and draws it over 64 colored bands at two tics. The result must be that color blended
over the scenery shifted by the production ripple (four rows down, then eight up), which the
unrefracted output differs from by several levels.
Screen-effect scenes (SalCRT and SalCRT Sharp, `screen-effect-off`, `screen-effect`, `screen-effect-fallback` and
`screen-effect-palette`) use stand-in shaders from `screen-effect-fixtures.h`, written with the game's RHI macros but not
the game's CRT shaders, which are game data. A stub `W_ReadShader` serves them through a glsllist, as the driver reads
the game's. A 64×64 frame of four quarters of different colors must come out as its red, half its green and 128 (the
dot pattern's red, read at a coordinate that only a repeating sampler wraps back to it, and the frame's height over 64),
in the right quarter (`screen-effect`); with the effect off or on Sharp Bilinear it is as drawn (`screen-effect-off`).
A second frame builds nothing again, and the palette step's texture is read when Palette Rendering is on
(`screen-effect-palette`). SalCRT Sharp's fragment doesn't translate, so it must present the plain frame and log once
(`screen-effect-fallback`). The native translator test checks the same sources' registers, semantics and constant
offsets and the rejected cases. The game's own shaders, the screen effect's look, and Xbox are untested here.
Screen Scaling scenes (`scaling-integer`, `scaling-1to1`, `scaling-fallback` and `scaling-effect`) place the same frame of four
colored quarters, through the driver's `PlaceFrame` and a test-only entry point, on screen-sized targets filled with magenta
first. At 128×128, 201×151, 200×200, 400×130, 64×64, 70×64 and 64×130 for Integer (k of 2, 2, 3, 2, 1, 1 and 1) and 101×90, 400×300
and 64×64 for 1:1, every pixel of the rectangle must be the frame's pixel k times smaller and every other pixel black, with
the rectangle's position worked out by hand (including the odd remainders). Integer and 1:1 on screens the frame doesn't fit
(and Fit) must place nothing. With SalCRT on, the placed pixels are the effect's output. The harness stubs `Present_Frame`, so the swap chain
(its size, buffer states and render target views) is not exercised, nor is the black border of a real swap chain's
buffers on Xbox.
`scaling-fit` draws Fit as the desktop presenter does, in a swap chain the size of the window: `FitRect`'s rectangle,
scaled with the second, linear-filtered sampler (`k` of 0). The frame has 64 columns shading from red to green and a top
half with less blue than its bottom half, so there is a boundary to blend along both axes. At 100×100, 200×100, 101×200,
130×70, 55×55, 40×30 and 64×64 (a factor of 1.5625, bars at the sides, bars above and below with an odd remainder, 70/64, a frame
scaled down by a little and by more than half, and the frame's own size), the oracle compares every pixel of a magenta-filled target: black outside the
rectangle, worked out by hand, and inside it a bilinear sample of the frame read earlier at each pixel's center, clamped at the
frame's edges. Direct3D filters with fixed-point weights, so a pixel between texels of different colors may be two levels
off and the rest one. Pixels that differ from the nearest texel by more than four levels must exist in each case, so that point sampling fails, except
the last two: at a scale of exactly 1 the filters agree, and at 64/30 no pixel's center falls between the texels of the frame's one
sharp edge (the top and bottom halves), leaving only the gradient, which differs from point sampling by a level or two. The self-test places the red quad's frame at the right rectangle, and requires a rectangle a column
off or a size off to be rejected, as well as a flat frame, which can't tell the filters apart. A screen with no size must place nothing.
Desktop presenter checks (`d3d12-present.cpp`, run by `run-d3d12.ps1` after the scenes) are their own
executable, `d3d12-present.exe`, because the presenter, `d3d_present_win32.cpp`, is the one driver file that must not be
compiled with `_UWP`, and the scenes' executable is. It compiles that file and `viberant_core.cpp`, the device and queue it
presents on, without `_UWP`, against the real engine headers, and runs on WARP with the debug layer (not GPU-based
validation: there is no shader work). Stubs supply the engine services, `cv_scr_scaling`, and what stands in for SDL's window
lookup (`d3d12-present-stubs/SDL.h` and `SDL_syswm.h`, with `SDL_GetWindowWMInfo` defined by the test): it hands the presenter
the `HWND` of a real popup window, created hidden and never shown, whose client area is its size. The driver's `PlaceFrame`
is replaced by a stub that records the placement, target size and source state it is called with, checks the target is a
buffer of the swap chain's size and format, and clears it, so what's checked is the presenter, not the pixels, which the
`scaling-fit` scene covers for the real `PlaceFrame`. The checks, each with the debug layer and `Core_Errors` clean:
taking before the game's window exists succeeds and makes nothing (the driver starts first; the first frame takes it); taking succeeds and makes exactly one child window (visible, disabled, `WS_EX_NOACTIVATE`, `HTTRANSPARENT` for
`WM_NCHITTEST`, 1 for `WM_ERASEBKGND`) as large as the parent's client area, with a swap chain of that size
(`Present_SwapChainBytes`); frames are presented with and without vsync at Fit, Integer and 1:1, and the rectangle and `k`
the presenter asks the driver for are those worked out by hand at 320×200, 500×300 and 160×100 (a 64×48 frame), and for a
640×480 frame larger than the window, which Integer and 1:1 fit; the child and the swap chain follow the parent's client
size, resized once each time; a window with no client area (0×0) places and presents nothing and keeps its swap chain; releasing
destroys the child and the swap chain, twice without harm; a second and a third take work, the first at the window's current
size; a frame the driver can't place (the stub returns false) is logged once and from then on frames are copied to a swap chain
of the frame's size while the child keeps the window's size, until the window is taken again, and a missing placement function
is the same failure; no `Present` failed (read from the perf line, `Present_LogPerf`, and the log); and shutting down leaves
nothing for the debug layer to report and no child. It exits 77 if the debug layer isn't installed, and `run-d3d12.ps1` fails.
Presenting is only observed through the API, not on a display: the window is hidden, so whether DWM
shows the frames, and how the child window behaves over a window with an OpenGL pixel format on real hardware,
aren't tested here. Likewise tearing (`DXGI_PRESENT_ALLOW_TEARING`) is used only if the runner's WARP allows it, and the log says whether it did.
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
removal run under GPU-based validation, and the `palette:` perf line must report no light
table alive; the fade map scenes below are what read the master palette. Fog remapping with
palette rendering is covered below; water with palette rendering and Xbox colors remain
untested here.
The `surface-probe` scene requests the Direct3D 12 driver's pixel history at 50%,50% (pixel 32,32 of
the 64×64 target), draws three flat quads of different colors, labels one with
`SurfaceProbeSource("wall", 7)` and ends the probe. It checks the logged `begin` pixel, each draw's
`raw=` value (RGBA8 as a little-endian DWORD, alpha `0xFF`, each color channel within one like the
other scenes) in order, `kind=wall source=7` on the labelled draw and `kind=polygon source=-1` on the
others, and the `end draws=3 color_changes=3` line. The screen must still show the last color
afterwards, a repeated request while one is pending must be refused, and a second frame without a
request must neither begin a probe nor log `surface probe:`. The harness stubs presentation
and has no ANGLE or window, so nothing here covers the swap chain or its hand-offs
with ANGLE.
The DRED breadcrumbs logged by a timed-out GPU wait are also unexercised here.

Fog and water scenes run the production capture path with the same tiny rings, so command-list
submissions exercise rebinding and constant lifetimes. Fog draws over known scenery, including
palette remapping through a light table. Water checks ordinary surface sampling and captured-scene
refraction against the fixed-point sine and independent blend math; grouped surfaces must keep their
original captured background while later pieces draw. The `fog-palette` and `water-refraction`
shader mutants (above) must fail their oracles, and capture counters check the grouping under GPU-based validation. Native D3D12
capture tests mock the texture, copy, binding and constant helpers, checking literal expected bounds (including fractional vertices) and
constant bytes, split views, clipping, failures, resizing and disjoint water growth. A growing water
capture uses one batched transition round trip around up to four strips; an unchanged grouped
rectangle uses none. `capture-mixed` requires fog and water in the same command list;
`capture-recreate` checks capture replacement after flushing resources still referenced by
recorded draws and after a resize; `capture-skipped` skips two-vertex fans after capture
and checks the next capture/draw, including pending opposite-transition merging. Xbox visual results, render-target decompression cost and performance
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
every row against the sine. The grid argument of `PostImgRedraw` is ignored,
so a shifted grid doesn't shift anything; the sway is what moves pixels.
Fade map scenes give the master palette known colors and a table of three rows (identity,
index 7 to 40, then 7 to 41 and 19 to 43). `fade-map` and `fade-map-row` check the strength's row,
`fade-map-clamp` a strength past the last row, `fade-map-remake` the same table id with four
rows, `fade-map-off` that nothing is drawn (and false is returned) without Palette
Rendering, `fade-map-after` that a draw after a fade map is unaffected, and `fade-map-reuse`
replaces tables for 64 frames while recorded work reads them. `resize-screen` and
`resize-fade-map` switch the screen to 640×400 and back, which remakes the screen
textures and the fade map's copy of the screen, under GPU-based validation.
The inversion oracle caught the upstream `PF_Invert`/`PF_Occlude` bit collision;
`PF_Invert` now uses its own blend bit, so it survives the blend mask. If the Graphics
Tools feature is missing, the script retries its install and then fails loudly
rather than running without the debug layer.

Reinitialization is currently unsupported: `g_shutdown` is terminal. These tests
do not change that lifecycle. They do not cover Xbox rendering or performance.

## Frame comparison between renderers

`tests/frame-compare/` plays the same staff ghosts in two builds or renderers with the game's data,
takes screenshots at the same game tics, and compares them pixel by pixel, to check that one renderer
(VibeRant D3D under Wine, a Vulkan build, Legacy GL) draws what another draws. The engine's part is the
`-shottics 70,210,350` capture mode (one frame per tic, no interpolation, a screenshot named
`<MAP>_<tic>.png`, quit after the last one; `-shotghost <staff ghost>` starts the replay), whose tic
list parsing has the native test `frame_capture`; the runner, the comparer and the report are Python run with
`uv run`. It needs real game data and a window, so the Tests workflow does not run it; the Map sweep workflow
("Automated map sweeps" below) plays the same capture mode on hosted runners. See
[frame-compare/README.md](frame-compare/README.md) for the commands, the sides it takes (a native binary or
a Windows build under Wine, each with its renderer's arguments), the default replays, and how to read the report.

## Vulkan driver harness (Linux)

The same driver test, `viberant-driver.cpp`, also runs against the Vulkan backend
(`src/hardware/r_viberant/vulkan`, [`docs/vulkan-backend.md`](../docs/vulkan-backend.md)):
the production driver, vk12, and the runtime shader translator are built into a
headless executable (`tests/vulkan-driver/CMakeLists.txt`) that needs no window,
SDL library, game assets, or game process. From the repository root, with CMake, a
C++17 compiler, Git, the Vulkan loader, a Vulkan device, and
`VK_LAYER_KHRONOS_validation` installed:

```sh
tests/run-vulkan.sh
```

The script builds (at most three jobs; `RR_VULKAN_BUILD` overrides the build
directory, `build/vulkan-tests` by default), runs the scenes, and then the negative
controls. The first configuration fetches the production pins of glslang,
SPIRV-Cross, Vulkan-Headers, volk, and DirectX-Headers, and builds glslang, which
takes several minutes.

The device is a CPU implementation, normally lavapipe (Mesa's software Vulkan
driver; `mesa-vulkan-drivers`): the harness asks vk12 for a CPU device, the
counterpart of forcing WARP, and fails if there is none. The `vulkan` CI job installs `mesa-vulkan-drivers`,
`vulkan-validationlayers`, and `libvulkan1` on `ubuntu-24.04` and runs the script.
To use a hardware device instead (for example, the machine's own GPU, with a
Vulkan SDK's validation layers):

```sh
sdk="$HOME/.cache/rr-linux/vulkan-sdk/1.4.363.1/x86_64"
RR_VULKAN_HARDWARE=1 \
VK_ADD_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d" \
LD_LIBRARY_PATH="$sdk/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
tests/run-vulkan.sh
```

What it covers is what the WARP run covers: the same scene registry, oracle
self-test, pixel comparisons (RGB tolerance 1), and capture, ring, and lifetime
counters, through the production driver's entry points, against the same expected
colors. A scene that is never reached, or reached but missing from the registry,
fails the run, as it does on WARP. Where the two runs differ:

- **Validation** replaces the Direct3D debug layer and GPU-based validation. The
  harness requires `VK_LAYER_KHRONOS_validation` (it fails if the log doesn't say
  "validation on"), every validation error fails the run at the next check, and
  `rr_vk12::ValidationErrors()` counts them for the program's life, so the errors a
  device's teardown reports are counted too, whatever the log's limit of 200
  messages. `Core_Errors` must stay zero, as on WARP. The debug-layer message
  IDs the WARP run ignores or requires have no Vulkan counterpart and aren't used.
- **Waits** are scaled by 20 on a CPU device (`Core_ScaleWaitsForTest`), so that the
  driver's 500 ms wait for a readback is ten seconds. On a GitHub runner (4 vCPUs,
  Mesa 25.2.8, LLVM 20.1.2) lavapipe's first readback of the run outlasted 500 ms in
  two runs (the driver had logged only its first prewarmed pipeline), so the oracle
  self-test read an unwritten pixel; the same Mesa, LLVM, and validation layer
  versions never took longer than 101 ms on a development machine, on one core or
  four, and the slowest of a passing CI run's 853 readbacks took 252 ms. The
  cause of the first stall (compiling or loading LLVM on a cold machine, while the
  prewarm worker does the same) wasn't isolated. A hardware device
  (`RR_VULKAN_HARDWARE`) keeps the waits production gives it, and the Direct3D run
  is unchanged.
- **Negative controls** (`vulkan-controls.py`) are the code-under-test ones of the
  WARP run: add-on shaders turned off, and the five shader defects of
  `run-d3d12.ps1` (`water-refraction`, `fog-palette`, `brightmap-palette`,
  `surface-light`, `surface-light-semibright`) injected into `shaders.hlsl` and
  compiled to SPIR-V with the production flags. Each runs as its own process and
  must fail through its named pixel oracle (`NEGATIVE_ORACLE_REJECTED`) within 120
  seconds. The production shaders are rebuilt afterwards.
- **Presentation** isn't exercised: the headless presenter stub is the one WARP's
  `viberant-driver` uses, and the Vulkan swap chain (`vk_present.cpp`) has no test
  here. The separate Windows presenter executable is specific to `run-d3d12.ps1`.
- **Water, grouped** (`water-group`) starts from a green of 200 on Vulkan, where
  the WARP run uses 120. The scene draws water with an alpha of 1/255 over a uniform
  capture of the screen twice, so each channel is scaled by 254/255 and rounded
  twice. AMD's Vulkan driver (RADV, Mesa 26.2 on a Vega 10) exports 8-bit colors
  through 16-bit floats rounded toward zero, so a result within about an eighth of a
  level above a rounding boundary lands one level lower. A green of 120 gives
  119.53, which should round to 120 but gives 119, and the second pass takes that
  119 (118.53) to 118: two levels from the 120 that the scene expects (and WARP
  gives), past the tolerance of 1. Sweeping a uniform background from 0 to 255
  through one water draw gives, for all 256 values, exactly what rounding the product
  toward zero in half precision and then to the nearest level predicts, and 15 of
  them (113 to 127) differ from exact rounding. The ripple's dependence on the
  reciprocal of `SV_Position.w` plays no part: the background is uniform, so every
  offset samples the same color. WARP and lavapipe round to nearest. Rather than
  accept two answers, the Vulkan run starts from a green whose results can't be taken
  below a boundary: 200 gives 199.22 and then 198.22, and the unchanged red of 80
  gives 79.69, 0.19 of a level above a boundary, which is more than the truncation
  takes away at that size (0.06). Every device then agrees on one expected value,
  computed by the scene's own per-pass formula, which for the Direct3D run's 120 is
  the color it always expected.

## Metal driver harness (macOS)

The same driver test, `viberant-driver.cpp`, runs a third time against the Metal backend
(`src/hardware/r_viberant/metal`, [`docs/metal-backend.md`](../docs/metal-backend.md)):
the production driver, mtl12, and the runtime shader translator are built into a
headless executable (`tests/metal-driver/CMakeLists.txt`) that needs no window, SDL
library, game assets, or game process. (The game's macOS build has the backend too,
with a presenter: the `macos` job builds it.) On a Mac with Xcode's
command-line tools, CMake, and Git, from the repository root:

```sh
tests/run-metal.sh
```

The script builds (at most three jobs; `RR_METAL_BUILD` overrides the build
directory, `build/metal-tests` by default, and `RR_METAL_CMAKE_ARGS` adds
arguments to the configuration), runs the scenes, and then the negative controls.
The first configuration fetches the production pins of glslang, SPIRV-Cross, and
DirectX-Headers, and builds glslang and SPIRV-Cross with its Metal target.

It passes on an Apple M4 Pro (macOS 27) and on an Intel Iris Plus Graphics 645
(macOS 15) as it does on the runner, whose GPU is a virtual one.

Metal code can't be built or run off macOS, so on other systems the way to run
this is the `metal` CI job, on `macos-15` (arm64). GitHub's runner has a Metal
device, "Apple Paravirtual device": the virtual machine's GPU, which passes
commands to the host's. Metal has no CPU implementation, so there is no
counterpart of WARP or lavapipe, and the test ignores `Core_ForceWarp`. The job
caches the build of the shader tools by runner image and their pins, and takes
a little over a minute with the cache; building the tools first adds about a
minute and a half.

What it covers is what the WARP and Vulkan runs cover: the same scene registry,
oracle self-test, pixel comparisons (RGB tolerance 1), and capture, ring, and
lifetime counters, through the production driver's entry points, against the same
expected colors. No scene is skipped or given another expected color for Metal;
`water-group` starts from the green the Vulkan run starts from. Where the runs
differ:

- **Validation** is Metal's own, turned on from outside the program: the script
  sets `MTL_DEBUG_LAYER=1` (API validation) and `MTL_SHADER_VALIDATION=1`, and the
  test fails unless mtl12's device line says both are on, which it reads off the
  classes Metal wrapped the device in (`MTLDebugDevice`, and `MTLGPUDebugDevice`
  or, on the runner's GPU, `MTLLegacySVDevice`). A failure of API validation stops
  the program where it happens (an abort, exit status 134), which fails the script,
  and the script's `MTL_DEBUG_LAYER_WARNING_MODE=assert` makes the debug layer's
  warnings (a state set twice, a buffer bound and never used) do the same. What
  shader validation finds comes back with the command buffer; mtl12 writes it to
  the log and counts it, and `rr_mtl12::ValidationErrors()` being nonzero fails the
  run at the next check, as does a command buffer that failed for any other reason.
  `Core_Errors` must stay zero, as on WARP. Both kinds were tried once on the
  runner with mtl12 broken on purpose (not kept in the tree): a scissor rectangle
  larger than its target stopped the program in `setScissorRect:`, and textures
  never given to the encoder made the first command buffer come back with "Null
  texture access executing fragment function", which the next check turned into
  "Metal validation error". Without the two variables the test refuses to run.
- **Waits** are scaled by 20, as on lavapipe, because the runner's GPU is shared
  and virtual. They haven't been needed: with `RR_METAL_PRODUCTION_WAITS=1`, which
  keeps the waits production gives the GPU, the run passed on the runner too, and
  the longest of its 2,356 fence waits took 23 ms; the longest wait of five runs
  in a row with the scaled waits was 88 ms, against production's 500.
- **Negative controls** (`metal-controls.py`) are the six of the Vulkan run: add-on
  shaders turned off, and the five shader defects injected into `shaders.hlsl`,
  compiled to SPIR-V with the production flags, and translated by mtl12. Each runs
  as its own process and must fail through its named pixel oracle
  (`NEGATIVE_ORACLE_REJECTED`) within 120 seconds. The production shaders are
  rebuilt afterwards.
- **The presenter's hooks** are checked after the scenes, outside the registry
  (the other backends have no counterpart): a texture like a drawable's is wrapped
  with `rr_mtl12::WrapTexture`, cleared through a render target view, copied to a
  readback buffer, and compared; then a drawable of a `CAMetalLayer` that is on no
  window is wrapped, cleared, and presented with `PresentWithNextSubmit`, and its
  submission must complete without a validation error. `metal-objects.mm` makes
  the texture and the layer, which C++ can't. If the layer gives no drawable, the
  test says so in capitals and goes on. What a window shows is not tested here: the
  presenter (`metal/mtl_present.mm`) is part of the game, not of this executable.
- **A failed command buffer** gets four runs of its own before the negative
  controls (`metal-controls.py`, `metal-driver --metal-command-buffer-error
  <code>`), since nothing goes on after one. Nothing here can make Metal fail a
  command buffer, so after one frame that works, mtl12 is told to treat the next
  submission's as failed with a `MTLCommandBufferError` code
  (`FailNextSubmissionForTest`): a timeout (2), a page fault (3), out of memory
  (8), and the device removed (11). The wait for that submission must fail, the
  fence must read as `UINT64_MAX`, the device must say it was removed with the
  reason that code stands for, the log must have the failure, and the frame that
  follows must stop the game: the driver calls `I_Error` with the reason, which
  exits with 1, and the script requires that exit, the run's
  `METAL_FAILURE_REACHED_DRIVER` line, and the error's text. With mtl12 as it was
  before (only the device removed counted; not kept in the tree) the run for a
  timeout failed with "the wait for its fence succeeded".
- **Null views** are checked after the scenes, outside the registry: mtl12 is
  told that the texture of the next null view of a new kind can't be set up
  (`FailNextNullViewForTest`), and a null view of a kind the driver makes none of
  must then leave no texture behind, the next one of that kind must make it, and a
  third must reuse it. A fifth failed-command-buffer run (`--metal-command-buffer-error
  2 null-view`) fails the command buffer that writes such a texture, which mtl12
  submits by itself, and requires the same as the other four. Both were tried
  against mtl12 as it was (not kept in the tree): keeping the texture failed the
  first, and committing its command buffer outside the submissions failed the
  second with "the null view's command buffer wasn't the device's to fail".
- **The sprite shader's pole** is checked with the add-on shader scenes, outside
  the registry: the clipping hack pulls a sprite vertex's clip z in by `80 / z`,
  which was infinite at a z of exactly zero. 49 quads are drawn at the 49 floats
  around the depth where the driver's projection gives zero (whichever way a GPU
  rounds, some should land on it), nothing may show, and validation must stay
  clean. Only the shader validation of Apple's own GPUs reports an infinity that a
  vertex function puts out: on an M4 Pro the check failed with "INF or NAN
  detected in interpolant" before the shader's fix and passes with it. On the
  runner's virtual GPU and on Intel's it can only check that nothing is drawn.
- **Autorelease pools.** The script runs the test with
  `OBJC_DEBUG_MISSING_POOLS=YES`, which has the Objective-C runtime report every
  object autoreleased with no pool in place, by thread, and fails if a report names
  the test's own thread (the test prints which that is). It found two in the
  test's own helper, which released a texture and a drawable outside a pool. It is
  a weak check of mtl12, though: under validation Metal frees the driver's
  resources on threads of its own, so the place where the driver's thread did
  autorelease without a pool (a resource's destructor) passes here with the pool
  taken out. The game without validation is where that shows:
  `capture.py hardware`'s `autorelease` scene ("On a Mac", below). Metal's own
  threads report tens of thousands under validation, which the script counts and
  leaves alone.
- **The shader cache** is checked after the add-on shader scenes, outside the
  registry as well: sixteen add-on shaders are each compiled, drawn with (the
  pixel is checked), and cleaned, and once the frames that used them are done,
  mtl12's cache must hold as many compiled functions as before
  (`rr_mtl12::CountsForTest`). With the pruning taken out of mtl12 (not kept in
  the tree) the count went from 42 to 58 and the run failed.
- **Not exercised**: the paths of mtl12 that the scenes don't reach, among them a
  view of fewer mips than its texture, buffer-to-buffer copies and timestamp
  queries (only the UWP presenter uses them), and a lost device.

## Metal frames (the game on a macOS runner)

There is no Mac among the machines this fork is developed on, so the macOS game with
VibeRant Metal is run where there always is one: on GitHub's `macos-15` runners (Apple
silicon, with the virtual GPU "Apple Paravirtual device"), by the workflow
`.github/workflows/metal-frames.yml`. It takes about three runner-hours, so it runs
only when asked: from the Actions page (**Metal frames** → Run workflow, once the
workflow is on the default branch), or by pushing a commit to a branch named
`frames/<anything>` (delete the branch afterwards).

- The `build` job builds the game as the `macos` job of `tests.yml` does (with
  ccache between runs), uploads the app, and fetches the official assets of Ring
  Racers v2.4 (750 MB, from the release's page) into a cache. The download is
  checked against a SHA-256 the workflow pins (`RR_ASSETS_SHA256`) before it is
  unpacked, and the cache's key carries that hash's first digits, so another file
  under the same name fails the job instead of being kept for every later run.
  What is typed into the Run workflow form (shards, ghosts, level times) reaches
  the scripts as environment variables, not as part of their text.
- The `sweep` jobs (five side by side by default, about 35 minutes each) play the
  staff ghosts of `tests/metal-frames/ghosts.txt`, the first one of each of the 213
  maps, in the frame capture mode (`-shotghost`, `-shottics 70,350,700,1400,2100`;
  [frame-compare/README.md](frame-compare/README.md) describes the mode), one game
  per ghost, and upload the frames and each run's log as `metal-frames-<n>`.
- The `scenes` job runs what the ghosts don't reach and uploads `metal-scenes`:
  screenshots the game takes itself and pictures of the screen (`screencapture`),
  the latter taken when the log says the game has reached the moment. The scenes:
  the game started without naming a renderer; the title screen and a menu, with keys
  sent through the system (CoreGraphics events from `osascript`, which reach whatever
  window has the keyboard); quitting through that menu (`menu-quit`: the same keys
  and then yes to Exit Program's question, with a command line that would only end
  the game after the run's two minutes are up); 3D models on and off; changing the
  renderer while a replay plays, starting in VibeRant Metal and starting in Software,
  in a window and in fullscreen (`fullscreen-switch`, `fullscreen-switch-software`,
  where each of the two windows that trade places is in a Space of its own, and
  `summary.json` has the presenter's lines about them in order); the three
  Screen Scaling modes in fullscreen with a 320x200 frame; `vid_wait`;
  `-mtldebug`; and `blind`, where `-mtlblind` has the presenter take its window for
  out of sight although it is on the runner's screen: at least ten frames must be
  presented for every second of the replay all the same (the log counts them), the
  layer must never be without a drawable, and two pictures of the middle of the
  screen taken two seconds apart must not be the same file.

`tests/metal-frames/capture.py` (Python's standard library only) does the running, on
a Mac as well ("On a Mac", below): `capture.py sweep --game <the binary in the .app>
--data <game data> --out results [--only AERIAL,MARBLE] [--shard 0/5]`, or
`capture.py scenes ...`.
Every run of the game gets a fresh home directory, `MTL_DEBUG_LAYER=1`, and
`MTL_SHADER_VALIDATION=1`. A run fails, and with it the job, if the game doesn't exit
with 0 in time (a game that stops at its error dialog is killed after five seconds),
if the log doesn't say that the backend is Metal with both kinds of validation on, if
a frame the capture mode announced is missing, if the presenter didn't present
the frames it was given (below), if the game's OpenGL window wasn't
still hidden when the game quit with VibeRant Metal (the presenter's last line says
what SDL says of it), if the log doesn't say that the Metal layer has its window's
color space, or if the log has a validation
message, a failed command buffer, a "not implemented" line, an error of the driver's,
a wait for the GPU that the driver gave up on, or a fallback to Software. `summary.json` in each artifact has every run's result.

**What a run says about the window.** The frames a sweep collects are read back from
the driver's screen, before the presenter has anything to do with them, so by
themselves they say nothing of the window. Three things are checked beside them:

- *That frames were presented.* The presenter counts, in its last line of the log,
  the frames it presented, the frames it didn't because the system called the window
  out of sight, and the frames the layer had no drawable for. A run fails if none was
  presented, and, since the window is expected to be on screen, if more than twenty
  frames and more than a twentieth of them weren't. (A new window doesn't count as
  visible for its first frames: runs on the runners skipped at most six.) This holds
  for sweeps too, which used to print the count and pass whatever it was. Behind a
  locked screen, or with several games' windows on top of each other, the game needs
  `-mtlunseen`, and the failure says so. The two exceptions are `capture.py hardware`'s
  `speed-metal` on a Mac whose screen is locked, which is the game as it runs there,
  and the `blind` scenes.
- *That a drawable holds the frame* (`-mtlcheck`). The presenter reads a window's
  first three drawables back, and every sixtieth after, with the frame it was given,
  and compares them (`metal/mtl_present_check.h`): the frame must be at the rectangle
  Screen Scaling placed it at, pixel for pixel where it is at its own size and by the
  mean colors of 8 by 8 blocks where it is scaled, and the rest of the drawable must be
  black. A drawable that differs fails the run, as do fewer than three compared. The
  scenes that have it are the three Screen Scaling ones, `default-title`, and
  `switch-renderer` on the runner, and `fullscreen`, `fullscreen-no-space`,
  `video-mode`, and `metal-software-metal` on a Mac; a sweep has it when the game is
  given `-mtlcheck` after `--`. It is a test's path: a drawable that can be read back
  is made with the layer's `framebufferOnly` off, which a player's never is, so the
  runner's sweep and the other scenes run without it.
- *Not checked: that the display showed it.* Nothing here reads the screen. On the
  runner the `scenes` job takes pictures of the screen, which a person has to look at;
  on a Mac reached over SSH there are none. So "the frame is in the drawable that was
  presented" is as far as an unattended run goes.

`tests/metal-frames/test_capture.py` (the test `metal_capture_judging`, which runs
with the native tests) holds `capture.py` to this with logs made up from the
presenter's own lines: logs that must pass, and for each reason to fail a log that
must fail for it and for nothing else.

What the jobs can't judge is whether the pictures are right. For the frames, compare
them with another renderer's frames of the same game logic, such as VibeRant Vulkan's
from a PC (the same ghosts and level times with the Linux build and `-vulkan`):

```sh
gh run download <run id> -R ZebulanStanphill/RingRacers -p 'metal-frames-*' -D metal
mkdir metal-all && mv metal/*/frames/*.png metal-all/
uv run tests/metal-frames/compare.py metal-all <the other frames> comparison
```

`compare.py` counts, per frame, the pixels that differ, those more than one level of a
channel apart, and those that differ beyond texel edges (neither frame has the pixel's
color within a pixel of it), prints the medians and the frames that stand out, and
writes side-by-side sheets of the latter. One level on a blended surface and the other
texel on a texel edge are the differences two GPUs have; `docs/metal-backend.md`
("What was tested, and what wasn't") has the figures of the first such comparison and
what each of its outliers was. The pictures of the screen have to be looked at.

The runners' display is 1024x768, so the game's 1280x720 window doesn't fit it: the
system cuts the window to the display's height, and the frame is fitted into what is
left, which the pictures of the screen show. The capture mode's screenshots are read
back from the driver and are 1280x720 regardless.

## On a Mac

The driver test and the game run on a Mac as they do on the runner, and a Mac has what the runner lacks: a real GPU
and a real display. `tests/run-metal.sh` needs CMake and Apple's command line tools. The game needs SDL2, libpng, and
Opus besides (built from source into a prefix of their own they need no Homebrew: `-DCMAKE_PREFIX_PATH=<prefix>` and
`PKG_CONFIG_PATH=<prefix>/lib/pkgconfig`), and the official assets unpacked somewhere. Then:

```sh
game=build/bin/ringracers_*.app/Contents/MacOS/ringracers_*
python3 tests/metal-frames/capture.py sweep --game $game --data <assets> --out sweep --idle 180 -- -mtlunseen -mtlcheck
python3 tests/metal-frames/capture.py hardware --game $game --data <assets> --out hardware --idle 180
```

`--idle 180` starts each game only once nobody has touched the Mac's keyboard or mouse for three minutes: the game
opens a window on the Mac's screen. `--shard K/N` splits the sweep, and several shards can run side by side, each
with its own `--out`, as far as the GPU keeps up: four at once were no load for an M4 Pro, and three at once under
validation put an Intel Iris Plus 645 more than half a second behind, which is where the driver stops waiting for
it (a screenshot it gives up on is noise, and the run fails with "the GPU fell behind"). A Mac nobody sits at usually has its screen locked. To a
window the system calls out of sight, VibeRant Metal presents only while it can expect the layer to have a drawable
free (`docs/metal-backend.md`, "The window"), which behind these two Macs' locked screens was every frame, at the
system's pace. `-mtlunseen` makes it present whatever the system says, as the sweep should, since a sweep requires
its frames to have been presented. `-mtlcheck` has the presenter read drawables back and compare them with the frames
it placed in them ("Metal frames", above, has what the two do and don't show). The frames the sweep collects are read
back from the driver either way.

`capture.py hardware` is for what only a Mac with a display can show and a log can tell. It takes no pictures of
the screen and sends no keys, so it needs no screen-recording or accessibility permission, and it works on a locked
screen. Each scene is a run of the game (or two), judged like the sweep's, and by what it is there for:

- `speed-metal`, `speed-software`, `speed-legacy-gl`: a replay in the capture mode, which draws one frame per tic
  of game logic as fast as it can, with `vid_wait` off and without Metal's validation; the time per frame is the
  time between the frames captured at level times 350 and 1400, divided by 1,050. VibeRant Metal runs here as the
  game would on that Mac, without `-mtlunseen`: behind a locked screen its frames are presented at the system's
  pace for a window nobody sees, so the time is that pace and not the renderer's, and the line says how long the
  layer took to give a drawable.
- `vsync-on`, `vsync-off`: the same with `-mtlunseen` and `vid_wait` on and off. On a screen that can be seen, on
  should take a refresh of the display per frame. To a window behind a locked screen the system hands drawables
  back at a pace of its own either way, so there the two only show that presenting works.
- `blind`: the replay with `-mtlblind`, which has the presenter act as if the system had called its window out of
  sight. At least ten frames must be presented for every second all the same (each while a drawable is surely free),
  and the layer must never be without a drawable. Behind a locked screen the system takes the frames
  too, and nobody sees them; the line says that the screen is locked.
- `autorelease`: the replay with `OBJC_DEBUG_MISSING_POOLS=YES`; the driver's thread, which mtl12 names in the log
  when that variable is set, must have no report of an object autoreleased without a pool
  (`docs/metal-backend.md`, "Autorelease pools").
- `fullscreen`: started on the whole screen as a player starts it, then `fullscreen No` and `fullscreen Yes`; the
  presenter's drawables must go from the display's size to the window's and back. The game's fullscreen is a Space
  of its own, which the system doesn't make while the screen is locked: there the game stays in its window, so the
  scenes that are about Spaces aren't run behind a locked screen, and the mode says so ("not run"). `fullscreen-no-space`
  is the same with SDL's hint `SDL_VIDEO_MAC_FULLSCREEN_SPACES=0`, where the window covers the screen as it is,
  locked or not.
- `fullscreen-switch`, `fullscreen-switch-software`: the renderer changed in fullscreen, from VibeRant Metal to
  Software and back, and from Software to VibeRant Metal and back. Without `-mtlunseen`, so that a Metal window that
  ends up on a Space that isn't showing is found out by the frames it doesn't get. Not run behind a locked screen.
- `video-mode`: `vid_mode` to 800x600 and back to 1280x720; frames and drawables must follow.
- `metal-software-metal`, `software-metal-software`, `legacy-gl-software-legacy-gl`: the renderer changed while a
  replay plays, there and back, with a screenshot at each stop.
- `metal-then-legacy-gl`: between the two hardware renderers the setting takes effect at the next start, so the
  first game must go on with VibeRant Metal and the second, started without naming a renderer, must be Legacy GL's.

What these gave on a Mac mini with an M4 Pro and on a MacBook Pro with an Intel Iris Plus 645 is in
`docs/metal-backend.md` ("What was tested, and what wasn't"). What the mode can't tell is what the screen looks like. For that someone has to look, or `capture.py scenes` has to run
on a Mac whose screen is unlocked and where the terminal may record the screen and send keys.

## CI and adding cases

`.github/workflows/tests.yml` runs this suite with ASan and UBSan (the
`regressions` job) on every pushed commit and manual workflow dispatch. Fork pull
requests run on `pull_request`; same-repository PR events skip the jobs because
push already covers those commits, including `master` and branches without a PR.
The native job caches the pinned glslang/SPIRV-Cross build and ZDBSP executable
by runner image, pins and build definitions. A hit keeps FetchContent disconnected
and skips the ZDBSP build; a miss fetches and builds them normally.
The Windows `warp` job, which compiles the translated add-on shader fixtures and runs
the Direct3D 12 WARP regressions, depends on it passing. CTest's JUnit report and
detailed run log are uploaded as `regression-results`, including when a test fails.
The `vulkan` and `metal` jobs run the driver test on those backends (see the two
harness sections above); the `metal` job caches its glslang and SPIRV-Cross build by
runner image, pins, and the harness's build file, and a hit keeps FetchContent
disconnected.
The `linux` and `macos` jobs build the game with the `ninja-release` preset and check
`compile_commands.json`: the Linux build must have the driver with only its Vulkan backend
(`HAVE_VIBERANT` and `HAVE_VIBERANT_VULKAN`, no `HAVE_VIBERANT_D3D`), and the macOS build must have the driver
with only its Metal backend (`HAVE_VIBERANT` and `HAVE_VIBERANT_METAL`, without `HAVE_VIBERANT_VULKAN` or
`HAVE_VIBERANT_D3D`). The `macos` job also checks that the Metal backend's
Objective-C++ files and the presenter are compiled with `-fobjc-arc`, which they
rely on to keep Metal's objects alive, and uploads the app as `macos-app` (an
archive of the unsigned `.app` for Apple silicon; it loads Homebrew's SDL2, libpng,
and Opus, and looks for the game's data in the directory it is started from).
Nothing in this workflow runs the macOS game. The `windows` job builds the ordinary (not UWP) Windows desktop game
in its default configuration, which has the driver beside OpenGL (`SRB2_CONFIG_VIBERANT_D3D` is on by
default for MSVC and clang-cl): it checks that `compile_commands.json` has `HAVE_VIBERANT` and compiles
`d3d_present_win32.cpp`, the desktop presenter, and not `d3d_present_uwp.cpp`, UWP's. It builds with
the `ninja-x64_windows_vcpkg-release` preset, `clang-cl`
from Visual Studio and a full vcpkg clone (`vcpkg.json` pins a `builtin-baseline`), with
vcpkg's binary cache stored by runner image, triplet and the hash of `vcpkg.json` and
the overlay ports. `fxc`, which compiles the driver's shaders, comes from the developer shell
(it is on `PATH` and in `WindowsSdkVerBinPath`, which CMake searches), whose environment
the job exports to its later steps; the job's first step fails if `fxc` isn't found.
It uploads the executable and the DLLs the build copies next to it as
`windows-desktop`, a game build without assets for testing, which offers Software, VibeRant D3D and Legacy GL. The build is only
compiled and linked: nothing runs the game, so what it draws on a display is untested.

When fixing a bug, add the smallest reproducible input and an assertion about its
observable result. Test the relevant platform variants and important boundaries.
Check that the new case fails against the pre-fix implementation, then passes
with the fix. When an oracle needs a proof that it can fail, feed it a deliberately
wrong input in the same executable and assert the specific mismatch; do not add a
second executable or a `WILL_FAIL` test, which passes on any failure (a sanitizer
abort, a stale source anchor, a crash). Avoid tests that only assert a source
spelling or duplicate the implementation. Keep randomized cases seeded and retain failing seeds as cases.
To add a group, add its stubs/cases in `templates/`, extract its production code
in `generate.py`, and register its executable in `CMakeLists.txt`. Register extracted production and reference source files as configure dependencies too, so editing Software's ripple calculation regenerates its water-refraction oracle.

- **Water refraction:** extract Software's production fixed-point ripple
  calculation and sine table, compare 262,144 offsets (distances both on and off
  multiples of 1/8, so the shader's `floor` calls matter), and exercise 8,640 shader
  composites across blend modes, alpha values, distances, times and split-view
  edges. `water_capture_viberant` extracts projected bounds from
  `r_viberant/viberant_captures.inc` and checks bounded copies, disjoint rectangle
  growth, grouping, allocation failures, resize and clipped faces. Its expected bounds
  (including fractional vertices), constant bytes, scene and sine texture bindings
  are literal values, and the growth strips must cover exactly the union of the
  pieces' bounds. Engine-side water-plane continuation and transparent-node dispatch
  checks cover slope/layer changes and wall/non-ripple interruptions. Direct3D 12
  WARP checks the shipped blend/capture path and rejects a reciprocal-depth water
  shader with the `water-refraction` oracle. Xbox GPU performance needs device testing.

- **Sky plane claims:** `sky_claim` extracts the D3D12 gate from `HWR_Subsector`.
  Check floors from above and ceilings from below, rejection from the far side
  and at equality, non-sky flats, both planes together and the validcount guard.
  Cull heights represent the plane at the camera's x,y, independently of the
  submitted local heights (including slopes). The old unconditional claims fail.
  `sky_claim_both` also checks that with OpenGL as the driver nothing is claimed,
  for every combination of flats and camera heights.
  Software creates sky visplanes from either side, but from the far side only
  fills beyond marking seg edges; hardware's sky walls reproduce that area.
  Claiming the plane itself hid the lower skybox buildings on prison map 157
  when the Xbox chase camera dipped below sky floors. This test checks claim
  submission, not rendered pixels or Xbox output.

- **Sky patch offsets:** Software's production `R_SetupSkyDraw` supplies the reference horizon and column offsets. Check the hardware offset helper and dome vertices for both hemispheres, signed offsets and odd/even texture heights; include Water Palace's actual `WPZSKY1` header (512×224, left 0, top 16), whose horizon is row 128. These tests verify alignment. They do not verify background visibility through water.

- **Untextured step flat bleeding:** `flat_bleed` runs the production D3D12 decision,
  UV and draw helpers. Cover the Green Hills Zone linedef 757 slope-rounding sliver,
  textured steps, reversed/equal heights, either sky flat, camera visibility (including
  equality), polyobjects, mirrored ceilings and steps at only one endpoint. Compare
  288 flat/texture/patch/PNG UV cases with a separate SETUP3DVERT model, including
  negative map coordinates, scrolling and rotation. Submission stubs check exact
  front-plane edges, degenerate endpoints, back-flat binding, paired UVs, ordinary,
  absolute, transferred and light-list lighting, midpoint plane height, slope-light
  inputs, shader flags and probe attribution. Visibility and step-direction mutations
  must fail. The title replay stubs these draws because it records walls and does not
  model camera height. These tests check geometry submission, not rendered pixels;
  D3D12 compilation belongs to Windows CI and Xbox visuals remain untested.

- **Slope shading:** `slope_light` runs the production `HWR_CalcSlopeLight` and
  the statement in `HWR_RenderPlane` that calls it (cut out of the source by
  `generate.py`, so the test breaks if its expression changes), and `flat_bleed`
  checks the call in `HWR_DrawFlatBleed`. It checks Software's rule: a slope's
  offset is added for a surface that faces up, subtracted for one that faces down
  under directional lighting (and added for it without), left alone where the
  sector gives no offset or there is no slope or sector, and clamped to 0 and 255
  in both directions. A sector's floor must face up, its ceiling down, a 3D floor's
  top (its control sector's ceiling) up, and its bottom down, and a flat bleeding
  across a step passes whether it is a floor or a ceiling. Prevents the far side of
  Balloon Park's funnel, made of 3D floor tops under a light contrast of 48, being
  navy instead of light blue. These tests check the light level asked for, not
  rendered pixels.

- **Software's light steps:** upstream gives the hardware renderer a fine light
  offset (`hwLightOffset`, in light levels) where Software has a coarse one
  (`lightOffset`, in light numbers, eight levels each). VibeRant's shaders cut light
  into Software's 32 light numbers, so with VibeRant and shaders the offset must go
  in Software's steps, and `HWR_SoftwareLightSteps` says when. `slope_light` (for
  `HWR_CalcSlopeLight`) and `wall_light_steps` (for `HWR_CalcWallLight`) run the
  production functions on both paths. With OpenGL, or VibeRant without shaders (off
  or not available), they keep the fine rule: `P_ApplyLightOffsetFine` is asked
  about the light level, `hwLightOffset` is added, and a disagreeing `lightOffset`
  is ignored. With VibeRant and shaders, `P_ApplyLightOffset` is asked about the
  light number (the level shifted right by three), `lightOffset` is added in eights
  of a level, a slope facing down under directional lighting has it subtracted, the
  result is clamped to 0 and 255, and neither `P_ApplyLightOffsetFine` nor
  `hwLightOffset` is ever used. A missing slope, sector, or seg leaves the light
  alone on both paths. Levels 192 with fine offset -1 and coarse 0 (must stay 192,
  not 191) and 199 with fine +4 and coarse +1 (must be 207, not 203) are fixed
  cases, and levels 0 to 7 and 248 to 255 get no offset on the new path. A loop over
  every level from 0 to 255 and every coarse offset from -6 to 6, with the real
  `P_ApplyLightOffset` range rule (only the sector's lighting model stubbed), checks
  that the result's light number is Software's, `(level >> 3) + offset` cut to 0 to
  31 where the offset applies. These tests check arithmetic, not rendered pixels.

- **Walls Software gives no light offset:** on the same steps path, Software leaves
  some walls without an offset (`R_RenderMaskedSegRange`, `R_RenderThickSideRange`,
  `R_RenderSegLoop`), and the hardware renderer follows it. `wall_light_steps` runs
  the production `HWR_SoftwareSkipsWallOffset` through its whole truth table: a 3D
  floor's side has none when the 3D floor is fog, the light has the fog flag, or the
  colormap is fog; a solid wall has none under any colormap; a middle texture has
  none under a fog colormap; and a missing colormap is never fog. It runs
  `HWR_StepWallLight` and `HWR_SliceWallLight` too: off the steps path the slice's
  light is exactly `HWR_CalcWallLight`'s fine one, whatever sector (or none) it is
  given, and on it none leaves the light as it is while a sector gives the coarse
  offset judged by that sector's lighting model, not the seg's front sector's (the two
  disagree, and both orders are checked). `HWR_FogColormapWallLight` is run with and
  without the steps path, with no colormap, with a fog or plain colormap that is the
  sector's, and with a fog colormap that is another one's. `wall_light_slices` runs
  the production `HWR_SplitWall` and `HWR_DrawWallLightSlice` over every combination
  of a solid wall, a masked or translucent middle texture, a 3D floor's side, and a
  fog 3D floor's side; two lights with no, plain, or fog colormaps, with and without
  the fog flag; and a `FOF_NOSHADE` light between two others. Each piece
  must get the sector `HWR_SplitWall` was given (never the seg's front sector) or
  none, and the light level and eight more where there is one, and a `FOF_NOSHADE`
  light keeps the piece of the light above it. `translucent_wall_light` runs the
  production calls in `HWR_ProcessSeg`: a middle texture without a light list
  (masked, translucent, and tripwire), and a 3D floor's side seen from the front
  sector, take the sector's own light where the colormap is its fog, and the side seen
  from the back sector keeps the light it is given. These check arithmetic and the
  calls made, not rendered pixels.

- **Camera-underground walls:** `void_wall` runs the production `HWR_WallFillsView` (`hw_main.c`) against an independent
  model of Software's columns: the seg sampled, its visible run's ends bisected, and clipped to the screen's columns by angle, and
  `R_StoreWallRange`'s `pixlow`/`pixhigh` at its first and last visible column from `R_SetupFreelook`'s shifted
  `centery` and `R_ExecuteSetViewSize`'s projection, with Software's floor- and ceiling-plane conditions.
  The model includes FOV, the ±80-degree Software aim clamp, the FOV-divided freelook shear, and
  independently computed, quantized roll zoom (including split-screen dimensions and the two-player FOV scale). Fixed cases
  cover an underground step, the same step from above the front floor (never empty), no lower texture, both or only one
  floor sky, a wall top still on screen, the mirrored ceiling case and the Xbox Green Hills regression
  (FOV 100, pitch -28 degrees, roll 19.33 degrees, camera below the front floor with the lower wall top
  still on screen). Sweeps vary distance, wall height, pitch (including beyond ±80 degrees), yaw,
  roll (0, 19.33, 45 and -27 degrees), FOV, split-screen, segs reaching past the screen's edges and a sloped back floor, and must decide
  like the model in every case, lower and upper walls apart. Flipping the pitch term or dropping the below-the-floor
  condition fails it. The test also runs the rule's own branch of `HWR_AddLine`: an accepted seg gets its black wall
  (none during the portal search, and nothing at all behind a portal exit or for a seg the rule rejects) and is never
  added to the clipper as solid, which would stop everything behind it from being drawn, including what this view can
  see above the wall where Software's can't; adding the clip range back fails it. The black the rule draws and the
  Xbox view are not covered; the decision and the black wall were compared with Software on macOS (Legacy GL on
  OpenGL) at the Green Hills Zone pose of the user's report and four others, and the open clipper with VibeRant D3D
  under Wine at the pose of a later Xbox report (640x480, pitch 31 degrees, roll -10 degrees), where the sky above two
  blocks had been black.
- **Horizon visibility:** use all 23 horizon boundaries from vanilla 2.4 Water Palace's TEXTMAP, with source/archive identity recorded in `fixtures/water-palace-horizons.json`. Compare the actual hardware horizon draw condition and helper to Software's production line acceptance, fake-flat and empty-line functions. Check its 22 empty sky boundaries, the remaining real boundary, lighting/texture/colormap/tag/slope changes, camera side, fake-flat control sectors (a chase camera makes `R_FakeFlat` substitute the back sector's heights and flat, which flips the decision in both directions; a first-person camera leaves it unchanged), minisegs, one-sided horizons and ordinary non-sky horizons. The old unconditional horizon condition fails. This verifies the renderer's geometry decision, not rendered pixels.

- **Internal FOF visibility:** extract Software's actual `R_DrawSinglePlane` rejection block, the hardware helper, and both face-admission conditions. Compare 945 decisions using real engine FOF structures, all CUTEXTRA/EXTRA/water/fog combinations, touching boundaries, moving control heights, and all 29 overlapping-water sectors recorded from vanilla 2.4 Water Palace in `fixtures/water-palace-water.json`. The old hardware height checks admit 15 internal water boundaries that Software suppresses. Removing the new condition from either face fails this test. This verifies the visibility decision; the reported scenery artifacts still require an Xbox retest.

- **Invisible geometry and composite blend state:** only the Direct3D 12 WARP
  scenes check this (`blend-invisible*` and `blend-remap-*`, above), through real
  pipeline states and pixels: all 100 pairs of blend modes with Invisible set, the
  surface after each, and water refraction and fog remap composites followed by an
  ordinary translucent surface.

- **UWP frame schedule:** `frame_schedule` executes the production pure pacing
  helper with fast 60 Hz loops, alternating 30/10 ms loops, a three-second load,
  Unlimited, singletics, skipped catch-up frames and a wrapping precise clock.
  Only render sleep changes; simulation remains untouched. Xbox pacing and actual
  scheduler wake-up precision require device testing.
  The alternating case stays within the four-frame debt limit: a sustained
  overload eventually resets debt as specified, after which a fast loop can sleep.

- **Horizon scan cache:** `horizon_cache` executes the production D3D12 cache
  and both plane/sky admission gates. Cover minisegs, null linedefs, ordinary and
  horizon lines, cache reuse, runtime changes, zone teardown and replacement
  levels. `horizon_cache_both` also checks that with OpenGL as the driver every
  subsector's lines are scanned, as upstream does, and the cache is never built.
  See [the invalidation notes](../docs/uwp-horizon-cache.md). These checks use
  ASan/UBSan and verify scan admission rather than GPU pixels or Xbox timings.

- **UWP PGO capture lifecycle:** not part of ctest; run `python3 tests/check-pgo.py`
  (needs `clang++` or `CXX`, no CMake or SDL). It compiles the production capture
  helper with real engine headers and small SDL/compiler-rt mocks under ASan/UBSan,
  and checks startup idempotence, USB directories, unique file names, rotation and
  counter reset after a dump, keeping counters after a failed write, synchronous
  suspend capture and unrelated-event filtering. It tests capture sequencing, not
  compiler-rt itself; Windows linking, real profiles and PGO performance need the
  Xbox. `docs/uwp-pgo.md` describes capture and toolchain use.

- **Hidden front BSP children:** `front_bbox_uwp` and `front_bbox_desktop` execute
  production `HWR_RenderBSPNode` on a two-level tree. Check hidden front-subtree
  rejection, visible back siblings, front-to-back order, portal rejection and each
  visited leaf's own `hwbbox`. Desktop retains unconditional front traversal.
  `front_bbox_both` walks the tree with each driver in turn. The
  pre-fix UWP walk fails the hidden-subtree case. These checks cover traversal,
  not rendered pixels or Xbox output.

- **Midtextures between skies:** `midtexture_sky` executes the production cut
  helper for Municipal Meadow's MIDPEG trees, one matching sky plane, ordinary
  openings, polyobjects and the sloped second endpoint. Generation checks both
  endpoint computations call it. It also executes the production
  `HWR_MidtextureBeforeSky` and the `HWR_ProcessSeg` statement that adds its result
  to the blend mode. Where both sectors have sky ceilings, or both have sky floors,
  Software has no ceiling or floor edge to clip a masked wall texture with, so the
  texture shows against the sky; in hardware the sky occluders' depth cut it off
  flat at the nearer sky ceiling (found on the renderer test map with a tall tree
  line texture). The helper must return `PF_BeforeSky` for a masked texture there,
  and nothing for: only one sky ceiling or floor, a sky ceiling on one side and a
  sky floor on the other, polyobjects, and every blend without `PF_Masked`
  (translucent, additive, subtractive, reverse subtract, multiplicative,
  environment, invert, fog). The caller keeps every other blend bit. Generation
  reads all single-bit flags from `hw_defs.h` and the test checks that
  `PF_BeforeSky` is one bit and shares it with none of them, nor with
  `PF_Blending` or `PF_EnvironmentTrans`. This tests flag decisions, not GPU
  pixels; the draw order is covered by `batch_sort_keys`.

- **Level darkness:** `level_darkness` executes Software's `R_AdjustLightLevel`
  and the hardware adapter for no darkness, bright/dark weighting, all fine
  fractions, separate views, map darkness and contrast/highlight cheats. Generation
  checks all five wall/plane lighting sinks and excludes all other lighting sinks.
  Plane, polyobject/FOF plane, opaque/split wall, flat-bleed and sorted translucent
  wall paths adjust after their existing slope/seg contrast. Sprites, models,
  shadows and sky remain unchanged. Fog capture consumes the adjusted payload
  without applying darkness again; its native mock checks the constant bytes.
  `R_SetupFrame` and `R_SkyboxFrame` select player/sky contexts before traversal;
  `R_GetViewNumber` maps both context groups to the same four player slots.
  These checks cover light arithmetic/submission, not gameplay or GPU pixels.

- **Fog-style linedefs:** `midtexture_fog` executes the production fog-midtexture
  helper and Software's alpha-table selection for 192 combinations of all blend
  modes, polyobjects, tripwires and alpha boundaries. Submission mocks require
  one untextured fog wall, the original quad, front-sector fog-alpha inputs and
  preserved wall lighting, or no draw for invisible fog. Other modes, plain
  masked walls, polyobjects and tripwires retain the textured path. Generation
  checks the helper gates the existing midtexture dispatch after geometry is
  computed. Software's upstream fog extent mixes world and screen coordinates;
  this implements the intended tint across the midtexture rectangle, rather
  than matching those inconsistent extents or the texture's opaque columns.
  These checks cover decisions/submission, not GPU pixels or Xbox visuals.

- **Two-player view width:** `split_projection` extracts the D3D12 transform
  and shared frustum helper. Check 20 projections (five FOVs, all view layouts)
  against Software's 1.7x horizontal half-tangent, 100 literal frustum angles
  (five pitches in every layout), and 25 pitched two-player corner bounds.
  Single, three- and four-player projections stay unchanged. The sky dome uses
  the same driver transform; `HWR_WallFillsView` already uses 1.7. Both normal
  rendering and portal searches use the widened frustum. The legacy sky quad's
  height/aim doubling describes the half-height viewport, not a zoom factor.
  These extracted functions compile natively; the full Windows D3D12 driver,
  WARP and Xbox visuals cannot be checked on macOS.

- **Software perspective shear:** `software_shear` compares the production hardware
  aiming setup and D3D12 projection translation against Software's rounded view
  centre, within one pixel. Checks 1920x1080, two-player 1920x540 (with the 1.7x
  FOV tangent), three/four-player 960x540 and 1280x800, three FOVs, pitches up
  and down including -17.3 degrees, flip, and Off/On/Third-person selection.
  Also checks zero pitch while shearing, preserved aim and pitch when off, and
  unchanged shear for 320x200-shaped views. Hardware's Software-perspective
  picture sat lower than Software's at 16:9 and much lower in two-player;
  this was found comparing Xbox screenshots. Matrix operations are dependency
  stubs capturing the driver's NDC translation; GPU pixels are not tested.

- **Translucent overlay:** `overlay_blend` executes production surface blending
  for every style at four translucency levels and the raw-alpha helper at seven
  alpha boundaries (88 decisions). Only overlay with partial alpha changes mode;
  opaque overlay stays masked and other styles retain their flags and alpha.
  Linedefs with partial alpha and models already use `HWR_SurfaceBlend`; both
  FOF wall sides and both FOF plane faces now use the same raw-alpha rule.
  This checks blend selection, not GPU pixels or Xbox visuals.

- **RF_SHADOWDRAW:** `shadow_draw` compares production hardware scale, flattening,
  slope adjustment and position with Software's extracted projection block and
  `R_SkewShadowSprite` in 960 cases (flat/sloped, five directions, four camera
  heights, two depths, gravity flips, stretching and object scaling). Check literal flat numbers,
  paper/floor exclusions, non-positive depth/height, unchanged unflagged inputs and zero-shear vertices,
  and that the flat shape keeps the sprite's own width, as Software does by
  stretching the whole patch over its original columns (27 width/radius/scale cases),
  and the quad's shear with and without camera billboarding. Shadow-draw sprites
  skip light-list splitting as in Software. Models keep their original object
  transforms; only a sprite fallback is flattened. A continuous hardware quad
  cannot copy Software's integer-column rounding or global freelook shear;
  hardware retains true perspective. Native tests check geometry, not pixels,
  model rasterization or Xbox output.

- **Drop shadow color:** `drop_shadow_color` checks cached constant-color maps,
  mapped-patch binding, opaque texel colors and transparent holes through the real
  patch cache and column converter, full light without sector or light-list colormaps,
  and normal/white blend modes. Software's real projection selection and shadow
  column establish the reference: use the shadow color's blend-table row, ignoring
  patch pixels and colormaps. Hardware's drop shadows were less than half as dark
  as Software's; this was found comparing Xbox screenshots. Native checks cover
  color conversion and draw inputs, not GPU blending or Xbox pixels.

- **Split sprites without height:** `sprite_height` runs the part of
  `HWR_SplitSprite` that divides by the sprite's height: a sprite without
  height isn't drawn, and the thinnest one that has a height gets finite
  texture coordinates.
- **Split fullbright colormap fading:** `sprite_fade` executes the production
  split-sprite lighting block and fade helper with real engine types and
  brightness predicates. Check fullbright/ordinary sprites, flagged/unflagged
  colormaps, above/below boundaries, RF_NOCOLORMAPS, object light offsets and
  overrides, semibright, paper sprites, missing colormaps, boundary equality and
  NOSHADE. Software only fades newly split lower fullbright pieces; projection
  keeps the original piece fullbright and drawing suppresses its colormap tint.
  Hardware uses the entry's light without object offsets or directional light,
  retaining shader distance diminishing and suppressing tint. Fullbright and
  semibright are mutually exclusive; existing semibright lighting is unchanged.
  The harness stubs boundary heights and ordinary post-lighting dependencies;
  it checks lighting decisions, not shader pixels or Xbox output.

## Automated map sweeps

`.github/workflows/map-sweep.yml` plays the real staff ghost on each of the 213 official maps with each renderer on a
GitHub-hosted runner, takes five frames per map, checks that the game stayed healthy, and reports what changed since
the last sweep of `master`. A rendering change that breaks a map nobody played shows up as a red sweep with
before, after, and difference pictures, with no one at a keyboard. It is the same frame capture mode as
`tests/frame-compare` (`-shotghost`, `-shottics`), run on hosted runners instead of a desktop.

### What runs where

| Renderer | Runner | How it is made to run |
|---|---|---|
| Software | `ubuntu-24.04` | The Linux build under Xvfb. Mesa's llvmpipe is the OpenGL the game opens for its 2D drawing in every renderer. |
| Legacy GL | `ubuntu-24.04` | The same build and display, `-opengl`, on llvmpipe. |
| VibeRant Vulkan | `ubuntu-24.04` | `-vulkan -vkdebug` on lavapipe, with Vulkan's validation layers. The job finds lavapipe's ICD file (its name depends on the Mesa package) and prints `vulkaninfo`, so it is the only device. |
| VibeRant D3D | `windows-2022` | The Windows desktop build, `-d3d12 -warp -d3ddebug`: Direct3D 12 on WARP with the debug layer (the "Graphics Tools" optional feature, installed as `tests/run-d3d12.ps1` does). |

The desktop game always opens an OpenGL 2.1 context for its 2D drawing, and a Windows runner has only Microsoft's OpenGL
1.1. The job therefore puts Mesa's `opengl32.dll` and `libgallium_wgl.dll` (llvmpipe) beside the executable, from
the [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) build, pinned to release 26.2.5 by SHA-256 in the
workflow. Only those two files are copied: the archive's `d3d10warp.dll` would be loaded in place of the system's WARP,
and the Direct3D 12 device would fail to be made (found on a runner: `D3D12CreateDevice failed: 0x887A0004`). Release 26.2.6
of the same build has no OpenGL driver in it at all. Mesa supplies only that startup context; the log must say
`VibeRant backend: D3D12`, `using WARP for testing`, and `D3D12 debug layer enabled`, or the map fails. The game data
is the official v2.4 asset archive, downloaded once, checked against the SHA-256 the release publishes, and cached.

### Running it

The workflow runs only when asked and nightly: not on pushes. Run it from Actions > Map sweep > Run workflow, or

```sh
gh workflow run map-sweep.yml --ref master -f ref=my-branch                      # everything, at 640x400
gh workflow run map-sweep.yml --ref master -f ref=my-branch -f size=1280x720     # full size, several times slower
gh workflow run map-sweep.yml --ref master -f ref=my-branch -f renderers=vulkan,d3d -f maps=RR_AERIALHIGHLANDS,RR_RAMPPARK
```

`ref` is the branch, tag, or commit to build; the workflow file is always master's. Do this for a branch that changes
rendering before merging it (`AGENTS.md`). `renderers` is a comma-separated subset of `software`, `gl`, `vulkan`, `d3d`;
`maps` are `RR_` names (a game's internal map names, as in `tests/map-sweep/ghosts.txt`); `baseline_run` compares with a
named run instead of master's. A run with a few maps takes a few minutes and compares only those maps.

The nightly run (05:37 UTC) is for a `master` that has moved: it first checks whether a complete sweep of the same
commit already exists and ends if it does. Only one sweep runs at a time in the repository, and at most six capture jobs
run at once, so the other workflows still get runners.

### Frame size

The game takes only the window sizes in its own list (`windowedModes` in `src/sdl/i_video.cpp`); `-width 640 -height 360`
is not one, and the game then quietly draws 320x200, which is how the first version of this sweep ran. The sweep
checks each PNG's size against the size it asked for. The choices are:

- `640x400` (the default): twice the game's base size, 16:10.
- `320x200`: the base size, which is also what Software renders natively, so hardware frames sit on the same pixel grid as
  Software's. The cheapest.
- `1280x720`: 16:9, as the earlier hand-made sweeps and `tests/frame-compare` use. The most detail and the widescreen
  layout, at about twice the cost of 640x400 on the hardware renderers.

At 640x400 the sweep loses what needs more pixels to show: seams of a pixel or two between walls, thin textures far away,
the widescreen HUD and field of view, filtering of distant geometry. What it keeps is anything that changes an object,
a wall, a plane, the lighting, or the HUD. Run `size=1280x720` for the review of a change to how geometry is clipped or
filtered. A baseline is only used by a sweep at its own size.

### What is played

Each map's first staff ghost in the official `staffghosts.pk3` (`tests/map-sweep/ghosts.txt`), at level times 70, 350,
700, 1400, and 2100, five frames in all, 1,065 frames per renderer. Fifty of the ghosts end before tic 2,135; those maps
(`tests/map-sweep/schedules.json`) use 70, 210, 350, 525, and 700 instead. The list was made before any sweep ran, not
after a failure, and a replay that ends before a requested tic is a health failure either way.

### Health: what makes a sweep red

The capture fails a map, and so the sweep, for any of:

- a crash, a nonzero exit, or the game's error dialog (the game prints the error and waits for a button; the run
  notices and stops it after a few seconds), `I_Error`, a signal;
- a hang: no new frame for five minutes (slow is fine; the whole map has 25 minutes);
- fewer frames than asked for, a file that is not a PNG of the asked-for size, or no `done; N of N screenshots taken` line;
- a "not implemented" line from a backend, a lost or removed device, or a driver error line such as `failed: 0x...`;
- a fallback to Software when another renderer was asked for ("couldn't start (the log says why), so the game is using
  the Software renderer"), or a log that does not show the renderer asked for starting (`tests/map-sweep/renderers.py`);
- a Vulkan validation error (`VUID-`, `Validation Error`; `-vkdebug`), or a Direct3D debug layer error (`D3D12 debug
  ERROR`; `-d3ddebug`);
- a shard that did not report, or a frame inventory that does not match what the maps asked for.

### Comparing with the last sweep

Each renderer's frames are compared with that renderer's frames from the **newest complete, healthy sweep of `master`,
at the same size**, up to 50 runs back and 90 days old (artifact `baseline-<renderer>` of that run, with the commit,
and the runner image and driver versions in `manifest.json`). The report gives, per renderer:

- how many frames are identical;
- for each changed frame, two percentages of its pixels: *plain* (any RGB difference) and *structural* (a differing pixel
  counts only if neither image has that pixel's color within 8 levels in the other's 3-by-3 neighborhood, which removes
  most texel-edge flips);
- before, after, and difference crops of the worst 12 frames (gray: any difference; red: structural);
- whether the graphics stack changed: each map records its shard's runner image version and graphics stack (Mesa and
  the LLVM it compiles with, or WARP), since a hosted image rolls out over days and the shards of one run can differ.
  Only the stack counts as the environment, and the image versions are printed beside it: two versions of the Linux
  image were in use together for the whole of the first measurements, with the same Mesa, and every frame of Software,
  VibeRant Vulkan, and VibeRant D3D came out the same on both.

The sweep is **red** if any health check failed, if a frame is missing, extra, or resized, or if a frame whose
graphics stack is the same as its baseline's has more than 0.1% structural or 2% plain difference. A changed stack
(a new Mesa, a new WARP) makes visual differences information only, so the sweep after such an update is blind to
regressions in the maps it touched: look at its report. The thresholds prompt a look; they do not prove a picture is
right or wrong.

The new frames of a complete, healthy sweep of `master` become the next baseline **even if the sweep is red**, so a
deliberate visual change turns exactly one sweep red, as a prompt to look. A sweep of a branch, of a subset of maps, or
with an unhealthy renderer never becomes a baseline, and a branch is compared with `master`'s baseline. The first sweep
of `master` finds none and records one.

#### Are the frames reproducible?

Software, VibeRant Vulkan, and VibeRant D3D: yes. Two complete sweeps, built from different commits and run on
different hosted runners, gave identical frames wherever the second commit changed nothing: all 526 frames the deliberate change of the second commit (a box in
a corner of the HUD from level time 700) could not touch, and all 539 that it could, outside the box, with each of the
three renderers. Hosted CPUs vary (a probe of six jobs landed on an AMD EPYC 7763 with AVX2, two EPYC 9V74, an EPYC 9V45,
and two Xeon 6975P-C with AVX-512) and each capture records its CPU model; the frames did not depend on it, as far as these
runs show. A reproducible renderer is what makes the comparison with a baseline worth reading.

Legacy GL is not reproducible on 23 of the 213 maps (Water Palace, Whirl Waters, Aqua Tunnel, and Hydro Plant among them). On 21 of them, 55 of the 1,065 frames differed between
those two sweeps outside the deliberate change, in 4 to 52% of their pixels, and `RR_WHIRLWATERS` differed twice on one
machine, with and without llvmpipe's `LP_NATIVE_VECTOR_WIDTH=128`. The cause is not known; those pictures look the same at
a glance. `tests/map-sweep/unstable.json` lists the maps: their differences are shown in the report, marked
`(unstable)`, and never make a sweep red, while their health checks still count. Mega Lava Reef and Wood were
added after three sweeps, of `master` and of two commits that changed only what VibeRant draws: Mega Lava Reef's frames at
tics 1400 and 2100 differed in all three (scattered pixels over the whole picture, up to 17% of it), and one of Wood's (441
pixels) in one of them. Add a map to it only with evidence that
two runs of one commit on one stack differ, or of commits that can't differ in that renderer, and take it out when the
renderer is fixed. Another Legacy GL map that
flickers turns one sweep red and belongs in the list.

**Vulkan against D3D**: the report also compares VibeRant Vulkan's frames with VibeRant D3D's of the same commit (two
renderers, two operating systems, always information only, never red).

### Reading a run

The page of the run shows a table (verdict, maps, frames, failures, identical frames, baseline run, capture minutes,
seconds per map) and each renderer's changed frames. The artifacts are `map-sweep-reports` (`<renderer>/summary.md`,
`report.json` with every frame, the crops, `cross-backend/`, and `negative-control/`, where one real frame is inverted to
show that the comparison reports a changed frame), `frames-<renderer>-<shard>` (frames, one log per map, and the manifest),
and `baseline-<renderer>`.

### What it does not cover

The Xbox, real GPUs (llvmpipe, lavapipe, and WARP are CPU renderers with their own arithmetic: a pass is not a pass on
an AMD, NVIDIA, or Intel GPU, nor on Microsoft's Direct3D 12 driver), other settings and resolutions than the one
chosen, anything the ghosts do not drive past, the title screen and menus, and times other than the five chosen.
It finds regressions and crashes on the ghosts' paths; it is not a review of how a frame looks.

### Cost

Measured on hosted runners (2026-11-15). A complete sweep at 640x400, all four renderers, 36 capture jobs and at most six
at a time, took **2 hours 52 minutes** and **902 runner minutes** (15.0 hours): Software 44.5 minutes in 2 jobs, Legacy GL
106.5 in 6, VibeRant Vulkan 361.7 in 14, VibeRant D3D 374.3 in 14, the Linux and Windows builds 4.6 and 8.3, planning and
reporting 2.5. One map (five frames, the game's start and exit included) took on average, and at most: Software 12 and 24
seconds, Legacy GL 29 and 65, VibeRant Vulkan 98 and 224 (lavapipe, validation on), VibeRant D3D 102 and 195 (WARP, debug
layer on). A check of five maps with all four renderers takes about 18 minutes and 37 runner minutes; one map of one
renderer a few minutes beyond the build.

At 320x200 a map takes 0.5 to 0.8 of the time it takes at 640x400. At 1280x720 five maps took on average: Software 22
seconds, Legacy GL 60, VibeRant Vulkan 164, VibeRant D3D about 210 or more (two of the five hit the first version's
240-second limit). The planner (`renderers.py`) assumes 25, 68, 184, and 290 and splits the sweep into jobs of about
30 minutes each, so 35 for VibeRant D3D at that size; its 290 is an estimate. A sweep leaves about 430 MB of frames
(14 days) and 365 MB of baselines (90 days) in artifacts. Hosted runners cost nothing for a public repository at the time of writing, but a sweep
fills six of the account's concurrent jobs for three hours, which is why `max-parallel` is 6 and sweeps queue behind one
another. Capture times are in every `manifest.json` (`runs`, per map), for the next measurement.

### Adding a renderer (VibeRant Metal)

The renderers are one table, `tests/map-sweep/renderers.py`. A row there (`label`, `os: macos-15`, `build: macos`,
`flags: ['-metal']`, `requires: ['VibeRant backend: Metal']`, plus its validation patterns in `errors`, and measured
`seconds`) puts it in the planner, the capture script's assertions, and the report. The workflow then needs a `build-macos`
job like the one in `metal-frames.yml` (packed as a tarball, since an artifact loses the executable bit), the capture
job's download and setup steps for macOS, `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` in its environment,
and one more `baseline-metal` upload step in the report job. Adding `('vulkan', 'metal')` to `CROSS` compares them.

### The tooling

The ghost list, shard planning, comparer, and report generator are in `tests/map-sweep` and have unit tests there
(`python -m unittest discover -s tests/map-sweep`; NumPy and Pillow from `requirements.txt`), which the workflow runs
before every sweep and CTest registers as `map_sweep` when both are installed. Regenerate the ghost list and schedules from
a copy of the official `staffghosts.pk3` with `python3 tests/map-sweep/ghosts.py staffghosts.pk3 > ghosts.txt` and
`--schedules > schedules.json`.
