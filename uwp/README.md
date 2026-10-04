# Ring Racers UWP

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4** for Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs with it to them.

- `patches/ringracers-uwp.patch` builds Ring Racers v2.4 as a static library for UWP (see the patch header for what it changes).
- `uwp/` is the launcher app that links it, along with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) built for OpenGL ES by `patches/sdl-angle.patch`, and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore), which runs OpenGL ES on Direct3D 11. Unlike Mesa's Direct3D 12 driver, this doesn't need shader model 6, which Xbox One consoles don't offer to UWP games.

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
3. At launch, the game asks whether to check its files. Checking reads all of them in full, which is slow from a USB drive, so once the files have passed a check, you can skip it on later launches.

## Notes

- Only the software renderer is available. Legacy GL needs desktop OpenGL, which ANGLE doesn't provide.
- `latest-log.txt` records how long each loading step and asset file took and, every 5 seconds, the frame rate and where frame and game logic time went (the same timings as the `perfstats` overlay, plus a breakdown of drawing the HUD and presenting each frame).
- `uwp-profile.txt` records what code the game's main thread was running, sampled about 1000 times a second while a level is being played. The `ringracers-uwp-symbols` artifact from the same build maps it to functions.
- The app requests microphone access for 2.4's voice chat. If access is denied, the game logs a warning and voice input stays off.
