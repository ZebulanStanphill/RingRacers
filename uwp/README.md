# Ring Racers UWP

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4** for Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs with it to them.

- `patches/ringracers-uwp.patch` builds Ring Racers v2.4 as a static library for UWP. [CHANGES.md](CHANGES.md) lists everything the port adds, removes, or optimizes compared with vanilla v2.4.
- `uwp/` is the launcher app that links it, along with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) built for OpenGL ES by `patches/sdl-angle.patch` (`patches/sdl-controller-duplicates.patch` also keeps it from seeing each controller twice), and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore), which runs OpenGL ES on Direct3D 11. Unlike Mesa's Direct3D 12 driver, this doesn't need shader model 6, which Xbox One consoles don't offer to UWP games.

## Building

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

Every push builds the app on a Windows runner and uploads a `ringracers-uwp` artifact containing a sideloadable `.msix` and its `.cer`. The certificate is generated fresh for each run, so uninstall the previous build before installing one from a different run. In Xbox Device Portal, install the `.cer` along with the package.

## Installing

1. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) to `E:\ringracers` so that `E:\ringracers\bios.pk3` and `E:\ringracers\data\` exist. The game verifies these files, so assets from other versions won't load. Config and saves go in `E:\ringracers\ringracers\`, the log is written to `E:\ringracers\latest-log.txt`, and if the game crashes, the error is saved to `E:\ringracers\error.txt`.
2. Install the app on your Xbox. The first load takes a while.
3. In Dev Home, set the app type to Game: under Games and Apps, highlight Ring Racers, press the View button, choose View details, and change App type to Game. As an app, it shares 2 to 4 CPU cores with the system and gets 1 GB of memory and part of the GPU; as a game, it gets 4 dedicated CPU cores plus 2 shared, 5 GB of memory, and the whole GPU. Reinstalling resets this, and the game can't tell which type it's running as, so check it after each install.
4. At launch, the game asks whether to check its files, with Skip check selected. Checking reads all of them in full, which is slow from a USB drive, so check once after installing or changing files, then skip on later launches. Quit (or B) closes the game instead.

## Untested changes

These are in the latest build but haven't been tried on an Xbox yet:

- **Legacy GL: skybox views leave out precipitation and things marked to hide from skyboxes** (`RF_HIDEINSKYBOX`, such as Battle overtime's barrier markers), as the Software renderer does. Rain and snow used to fall inside the distant scenery too.
- **If Legacy GL runs out of video memory, it clears its texture cache.** This may cause a brief hitch instead of slowing to a crawl.

## Notes

- Credits: the port builds on [worleydl's UWP port](https://github.com/worleydl/ringracers-uwp), and VibeRant D3D11's portals are inspired by [SRB2Kart Saturn](https://github.com/Indev450/SRB2Kart-Saturn)'s.
- Legacy GL is available as **VibeRant D3D11** (Options → Video → Advanced → Renderer). Legacy GL needs desktop OpenGL, which ANGLE doesn't provide, so the port draws its hardware renderer with Direct3D 11 instead, following the OpenGL driver's state and shaders, and shows each frame through ANGLE. Custom shaders from add-ons aren't supported. It loads the level's textures and supported kart models during level loading to reduce mid-race hitches. With this and reduced drawing-settings upload overhead, the latest Xbox One X test reported roughly 27–54 FPS at 1920×1200 in races, compared with about 22 FPS at 640×400 before. Level loads were not noticeably longer; some momentary pauses remain. Its Palette Rendering option (Options → Video → Advanced; on by default, needs Shaders on) limits colors to the palette and shows palette flashes like the Software renderer; turning it off goes back to Legacy GL's smooth lighting, and its cost shows in the perf summary's `D3D11: palette rendering` line. If it misbehaves, switch back to Software, or set `renderer` to `Software` in `E:\ringracers\ringracers\ringconfig.cfg`. Its startup and any errors are logged as `UWP D3D11:` lines in `latest-log.txt`, and while it's on, the perf summary adds `Legacy GL:` and `D3D11:` lines (Legacy GL's timings, the portals it drew per frame and the CPU time their views took, and the driver's draws, texture uploads, CPU and GPU time, how long presenting took, ANGLE read waits and fallback finishes, and a `D3D11: memory` line with local and non-local video memory usage/budgets and the driver's tracked texture, buffer, and screen allocations).
- The latest Xbox One X test confirmed that **VibeRant D3D11 shows frames in order**: the loading percentage counts up normally, and the player model no longer jumps back and forth. Both symptoms came from reusing presentation textures before ANGLE finished reading them.
- `latest-log.txt` records how long each loading step and asset file took and, every 5 seconds, the frame rate and where frame and game logic time went (the same timings as the `perfstats` overlay, plus a breakdown of drawing the HUD and presenting each frame), the number of objects in the level, which processors the main thread ran on, and how long the profiler paused it. If the game crashes, it ends with the error and the calls that led to it (the `UWP: crash:` lines), which the game's own crash handler doesn't catch on Xbox.
- `uwp-profile.txt` records what code the game's main thread was running, and the calls that led there, sampled about 50 times a second while a level is being played (each sample pauses the game for about a quarter of a millisecond). The `ringracers-uwp-symbols` artifact from the same build maps it to functions. Each launch replaces it, so copy it off before launching again.
- `UWP frame:` traces drawn level frames while the race title card is up and from 10 to 20 seconds of level time (up to 600 per load), including interpolated/raw player position, camera, title timers, and Legacy GL presentation slots/source frame IDs. `UWP fade:` traces fades, wipes, palettes, and title-map lighting for the first 10 seconds after a game-state change or level load (up to 200 events per window). Both are written to `latest-log.txt`.
- `UWP: track title:` in `latest-log.txt` reports how many frames the pre-level title animation drew, its elapsed time, and its average and worst drawing and presentation times. The timings distinguish time spent drawing from time spent presenting.
- `tools/` has scripts for reading these: `perf-summary.py` averages the perf lines of `latest-log.txt` for each stretch of racing at the same resolution, `profile-report.py` turns `uwp-profile.txt` into the functions the time went to, `annotate-function.py` shows which instructions of one function were slow, and `crash-report.py` names the game's functions in a logged crash.
- Turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves rotating every frame of the 3D view when the camera tilts. Its cost grows with resolution. With Parallel Software Rendering enabled, the port also shares this work across the rendering workers.
- The app requests microphone access for 2.4's voice chat. If access is denied, the game logs a warning and voice input stays off.
