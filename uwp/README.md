# Ring Racers UWP

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4** for Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs with it to them.

- `patches/ringracers-uwp.patch` builds Ring Racers v2.4 as a static library for UWP (see the patch header for what it changes).
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
3. In Dev Home, set the app type to Game: under Games and Apps, highlight Ring Racers, press the View button, choose View details, and change App type to Game. As an app, it shares 2 to 4 CPU cores with the system and gets 1 GB of memory and part of the GPU; as a game, it gets 4 dedicated CPU cores plus 2 shared, 5 GB of memory, and the whole GPU. Reinstalling may reset this.
4. At launch, the game asks whether to check its files. Checking reads all of them in full, which is slow from a USB drive, so once the files have passed a check, you can skip it on later launches.

## Notes

- Only the software renderer is available. Legacy GL needs desktop OpenGL, which ANGLE doesn't provide.
- `latest-log.txt` records how long each loading step and asset file took and, every 5 seconds, the frame rate and where frame and game logic time went (the same timings as the `perfstats` overlay, plus a breakdown of drawing the HUD and presenting each frame), the number of objects in the level, which processors the main thread ran on, and how long the profiler paused it. If the game crashes, it ends with the error and the calls that led to it (the `UWP: crash:` lines), which the game's own crash handler doesn't catch on Xbox.
- `uwp-profile.txt` records what code the game's main thread was running, and the calls that led there, sampled about 50 times a second while a level is being played (each sample pauses the game for about a quarter of a millisecond). The `ringracers-uwp-symbols` artifact from the same build maps it to functions. Each launch replaces it, so copy it off before launching again.
- `tools/` has scripts for reading these: `perf-summary.py` averages the perf lines of `latest-log.txt` for each stretch of racing at the same resolution, `profile-report.py` turns `uwp-profile.txt` into the functions the time went to, `annotate-function.py` shows which instructions of one function were slow, and `crash-report.py` names the game's functions in a logged crash.
- Turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves rotating every frame of the 3D view when the camera tilts. Its cost grows with resolution. With Parallel Software Rendering enabled, the port also shares this work across the rendering workers.
- The app requests microphone access for 2.4's voice chat. If access is denied, the game logs a warning and voice input stays off.
