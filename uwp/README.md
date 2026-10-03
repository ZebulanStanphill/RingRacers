# Ring Racers UWP

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4** for Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs with it to them.

- `patches/ringracers-uwp.patch` builds Ring Racers v2.4 as a static library for UWP (see the patch header for what it changes).
- `uwp/` is the launcher app that links it, along with SDL2, Mesa, and libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep).

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

This clones Ring Racers v2.4 into `RingRacers/`, applies the patch, builds it, then generates and builds the launcher solution at `build/ringracers-uwp.sln`. Open that in Visual Studio to deploy to your Xbox (Remote Machine) or to create an app package (Project → Publish → Create App Packages).

## Installing

1. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) to `E:\ringracers` so that `E:\ringracers\bios.pk3` and `E:\ringracers\data\` exist. The game verifies these files, so assets from other versions won't load. Config, saves, and add-ons go in `E:\ringracers\ringracers\`.
2. Install the app on your Xbox. The first load takes a while.

## Notes

From the 2.3 port; these may differ with 2.4 and the newer Mesa build:

- Vsync is forced on by the OpenGL driver; leave the in-game vsync off.
- Performance varies by track; 1280x800 with Legacy GL is a good starting point. 3D models hurt framerate.
- Use the software renderer for online play; Legacy GL is unstable on this platform.

The app requests microphone access for 2.4's voice chat. If access is denied, the game logs a warning and voice input stays off.
