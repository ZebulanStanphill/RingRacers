# Ring Racers UWP (Zebulan Edition)

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4**, intended for Xbox One and Xbox Series consoles. Requires Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs to them.

Only tested on Xbox One X. Should also work (even better) on Xbox Series X|S, and it may work (with lowered graphics settings) on an original Xbox One, but I don't have those consoles to test it on. Probably also works on Windows 10/11 PCs, but I can scarcely think of a reason why you wouldn't just use the official release there.

- `patches/ringracers-uwp.patch` builds Ring Racers v2.4 as a static library for UWP. [CHANGES.md](CHANGES.md) lists everything the port adds, removes, or optimizes compared with vanilla v2.4.
- `uwp/` is the launcher app that links it, along with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) built for OpenGL ES by `patches/sdl-angle.patch` (`patches/sdl-controller-duplicates.patch` also keeps it from seeing each controller twice), and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore), which runs OpenGL ES on Direct3D 11. This path does not require Shader Model 6. Xbox One UWP's Direct3D 11 feature level is 10.1; its Direct3D 12 path can use higher shader models in Game mode. See the [renderer roadmap and compatibility limits](CHANGES.md#graphics).

Coding agents were used heavily in the development of this port.

[Native regression tests](tests/README.md) run isolated engine checks with memory
and undefined-behavior sanitizers locally and in CI, without game assets or an Xbox.

## Building

### Local

Requirements (Windows):

- Visual Studio 2022 with:
  - **Desktop development with C++**, including *C++ Clang tools for Windows* and *C++ CMake tools for Windows*
  - **Universal Windows Platform development**, including *C++ (v143) Universal Windows Platform tools*
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) cloned and bootstrapped, with `VCPKG_ROOT` pointing to it

From an **x64 Native Tools Command Prompt for VS 2022**:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

This clones Ring Racers v2.4 into `RingRacers/`, applies the patch, and builds it; builds SDL2 and downloads ANGLE into `deps/`; then generates and builds the launcher solution at `build/ringracers-uwp.sln`. Open that in Visual Studio to deploy to your Xbox (Remote Machine) or to create an app package (Project → Publish → Create App Packages).

### GitHub Actions

Every push first runs the native regression tests on Linux, then builds the app on a Windows runner and uploads a `ringracers-uwp` artifact containing a sideloadable `.msix` and its `.cer`. Test reports are uploaded as `regression-results`. The certificate is generated fresh for each run, so uninstall the previous build before installing one from a different run. In Xbox Device Portal, install the `.cer` along with the package.

## Installing

1. Plug an external USB storage drive into your Xbox and format it for *media* (not apps/games). Note that this will wipe its contents. This will be your `E:\` drive. (Note that drives formatted this way can be read and written to on Windows like any other NTFS drive, though it will incorrectly warn that the drive has errors; the drive is not readable by default on macOS and most Linux configurations due to some peculiarities in its MBR signature.)
2. Create a folder at the root of the USB drive titled `ringracers`.
3. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [official Ring Racers v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) and place the contents in `E:\ringracers` (e.g. `E:\ringracers\bios.pk3` and `E:\ringracers\data\*`). The game (optionally) verifies these files, so assets from other versions won't load. Config and saves go in `E:\ringracers\ringracers\`, the log is written to `E:\ringracers\latest-log.txt`, and if the game crashes, the error is saved to `E:\ringracers\error.txt`.
4. Install the app on your Xbox. Unless you've modded your console somehow, this requires [Dev Mode](https://developer.microsoft.com/en-US/games/partner/signup).
5. In Dev Home, set the app type to "Game": under Games and Apps, highlight Ring Racers, press the View button (the small one to the left of the Xbox logo button on a standard Xbox controller), choose "View details", and change "App type" from "App" to "Game". As an app, it shares 2 to 4 CPU cores with the system and gets 1 GB of memory and part of the GPU; as a game, it gets 4 dedicated CPU cores plus 2 shared, 5 GB of memory, and the whole GPU. Reinstalling resets this, and the game can't tell which type it's running as, so check it after each install.
6. At launch, the game asks whether to check its files, defaulting to "Skip check". Checking reads all of them in full, which is a bit slow, so check once after installing or changing files, then skip on later launches. Quit (or B) closes the game instead.

## Untested changes

These are in the latest build but haven't yet been confirmed by a human to actually work:

- **VibeRant D3D11: add-on shaders** translate supported Legacy GL GLSL shaders at load time. Invalid shaders fall back to the built-in program; choose Shaders → Ignore custom shaders to retain the built-in effects. A custom water shader replaces the built-in surface effect. Add-on visuals and loading cost still need Xbox testing. [Compatibility limits](CHANGES.md#graphics).
- **VibeRant D3D11: fog volumes** remap scenery at fog walls and planes with Shaders on, including colored and overlapping volumes. Native and SM4 GPU regressions cover remapping and integration with water and custom shaders; Xbox visuals and performance still need testing.
- **VibeRant D3D11: remaining water coverage and GPU performance checks.** The user confirmed working water effects and the brown-wall fix on Water Palace in build 94. Other overlapping-water-volume cases and a controlled GPU performance comparison remain untested. Native tests cover hidden water boundaries, bounded shared captures and actual Direct3D depth attenuation; Windows CI also executes the real SM4 shaders through WARP.
- **VibeRant D3D11: heat distortion and every split-screen visual check** remain untested on Xbox. The user reported improved single-view underwater intensity. Native regression tests cover all layouts; a second controller is needed to confirm view isolation, mixed underwater/above-water cameras, heat, and the HUD on the device.
- **Legacy GL: skybox views leave out precipitation and things marked to hide from skyboxes** (`RF_HIDEINSKYBOX`, such as Battle overtime's barrier markers), as the Software renderer does. Rain and snow used to fall inside the distant scenery too.
- **If Legacy GL runs out of video memory, it clears its texture cache.** This may cause a brief hitch instead of slowing to a crawl.

## Hardware water rendering

**VibeRant D3D11's water-surface ripples enhance the Software renderer's effect:** viewed from above, they refract visible submerged track geometry as well as the background. In the user's Water Palace comparison, Software's ripple distortion affected only the background. This enhancement and the removal of the large brown walls were confirmed on Xbox One X in build 94. The walls came from inverted lighting slices of a sloped wall; each slice is now clipped to the original wall. [Implementation and regression coverage](CHANGES.md#graphics).

The **Batching** switch is under **Options → Video → Advanced**, in the **VibeRant D3D11 Options** section directly below **Better transparency ordering**. Change it with Left/Right on the controller. It starts On and resets to On on the next launch.

For future visual investigations, bounded surface logging and the opt-in `gr_surfaceprobe` command remain available. Pixel history requires Batching off and a single hardware-rendered level view; its capture can stall that frame. [Diagnostic instructions and limits](CHANGES.md#diagnostics).

## Notes

- Credits: the port builds on [worleydl's UWP port](https://github.com/worleydl/ringracers-uwp), and VibeRant D3D11's portals are inspired by [SRB2Kart Saturn](https://github.com/Indev450/SRB2Kart-Saturn)'s.
- The "Legacy GL" renderer has been ported and revised, and this build's custom version of it is called **VibeRant D3D11** . Enable it via Options → Video → Advanced → Renderer. Legacy GL needs desktop OpenGL, which ANGLE doesn't provide, so the port draws its hardware renderer with Direct3D 11 instead, following the OpenGL driver's state and shaders, and shows each frame through ANGLE. It translates add-on vertex and fragment GLSL shaders to HLSL at load time; unsupported or invalid shaders log an error and use the built-in shader. This support still needs Xbox testing. It loads the level's textures and supported kart models during level loading to reduce mid-race hitches. With this and reduced drawing-settings upload overhead, the latest Xbox One X test reported roughly 27–54 FPS at 1920×1200 in races, compared with about 22 FPS at 640×400 before. Level loads were not noticeably longer; some momentary pauses remain. Its Palette Rendering option (Options → Video → Advanced; on by default, needs Shaders on) limits colors to the palette and shows palette flashes like the Software renderer; turning it off goes back to Legacy GL's smooth lighting, and its cost shows in the perf summary's `D3D11: palette rendering` line. If it misbehaves, switch back to Software, or set `renderer` to `Software` in `E:\ringracers\ringracers\ringconfig.cfg`. Its startup and any errors are logged as `UWP D3D11:` lines in `latest-log.txt`, and while it's on, the perf summary adds `Legacy GL:` and `D3D11:` lines (Legacy GL's timings, the portals it drew per frame and the CPU time their views took, and the driver's draws, texture uploads, CPU and GPU time, how long presenting took, ANGLE read waits and fallback finishes, and a `D3D11: memory` line with local and non-local video memory usage/budgets and the driver's tracked texture, buffer, and screen allocations).
- **Future Direct3D 12 renderer (deferred).** Consider a native D3D12 backend after the current renderer and the rest of the port have been thoroughly optimized and tested. It could support shader features beyond the current Shader Model 4 target, subject to Xbox/PC capabilities and compiler integration. Uninitialized custom uniform arrays and extra vertex attributes need separate adapter/engine work and could also be added to D3D11; an API switch alone would not enable them. [Platform support and roadmap details](CHANGES.md#graphics).
- **Fog quality depends on Shaders.** Fog-volume remapping requires Shaders on. With Shaders Off, fog retains the older approximation and can differ from Software in color, brightness and where the effect appears. This is an accepted quality tradeoff; [implementation details](CHANGES.md#graphics) describe both paths. A custom fog shader replaces the built-in effect.
- **Better transparency ordering** (Options → Video → Advanced) sorts blended sprites and models among transparent walls, floors, water, and fog. It works with Shaders on or off and defaults to Off because the extra overlap checks can cost CPU time in busy scenes. Enable it for improved overlaps; turn it off to retain the previous rendering order. Intersecting geometry and translucent models can still have ordering artifacts. Xbox visual correctness and performance are untested; [implementation and limitations](CHANGES.md#graphics). Integration tests cover the newer bounded water captures with the toggle on and off; Xbox visual and performance checks, including split-screen, remain untested.
- The latest Xbox One X test confirmed that **VibeRant D3D11 shows frames in order**: the loading percentage counts up normally, and the player model no longer jumps back and forth. Both symptoms came from reusing presentation textures before ANGLE finished reading them.
- The latest Xbox online retest confirmed that **VibeRant D3D11 gets past race loading without delay-limit kicks**: eight subsequent online level transitions, including Angel Island, continued successfully with the title animating in the HUD. The client acknowledges actual buffered tics during loading and keeps simulation running while the title animates; the server's delay rules remain unchanged.
- `latest-log.txt` records how long each loading step and asset file took and, every 5 seconds, the frame rate and where frame and game logic time went (the same timings as the `perfstats` overlay, plus a breakdown of drawing the HUD and presenting each frame), the number of objects in the level, which processors the main thread ran on, and how long the profiler paused it. If the game crashes, it ends with the error and the calls that led to it (the `UWP: crash:` lines), which the game's own crash handler doesn't catch on Xbox.
- `uwp-profile.txt` records what code the game's main thread was running, and the calls that led there, sampled about 50 times a second while a level is being played (each sample pauses the game for about a quarter of a millisecond). The `ringracers-uwp-symbols` artifact from the same build maps it to functions. Each launch replaces it, so copy it off before launching again.
- `UWP frame:` traces drawn level frames while the race title card is up and from 10 to 20 seconds of level time (up to 600 per load), including interpolated/raw player position, camera, title timers, and Legacy GL presentation slots/source frame IDs. `UWP fade:` traces fades, wipes, palettes, and title-map lighting for the first 10 seconds after a game-state change or level load (up to 200 events per window). Both are written to `latest-log.txt`.
- `UWP: track title:` in `latest-log.txt` reports how many frames the pre-level title animation drew, its elapsed time, and its average and worst drawing and presentation times. The timings distinguish time spent drawing from time spent presenting. Online VibeRant clients log that the title runs in the HUD instead of timing a blocking pre-level animation. A `UWP: server delay-limit kick:` line records the last server-reported delay and limit plus the local simulated, received and level tics.
- `tools/` has scripts for reading these: `perf-summary.py` averages the perf lines of `latest-log.txt` for each stretch of racing at the same resolution, `profile-report.py` turns `uwp-profile.txt` into the functions the time went to, `annotate-function.py` shows which instructions of one function were slow, and `crash-report.py` names the game's functions in a logged crash.
- Turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves rotating every frame of the 3D view when the camera tilts. Its cost grows with resolution. With Parallel Software Rendering enabled, the port also shares this work across the rendering workers.
- The app requests microphone access for 2.4's voice chat. If access is denied, the game logs a warning and voice input stays off.
