# Ring Racers UWP (Zebulan Edition)

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4**, intended for Xbox One and Xbox Series consoles. Requires Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs to them.

Only tested on Xbox One X. Should also work (even better) on Xbox Series X|S, and it may work (with lowered graphics settings) on an original Xbox One, but I don't have those consoles to test it on. Probably also works on Windows 10/11 PCs, but I can scarcely think of a reason why you wouldn't just use the official release there.

Coding agents were used heavily in the development of this port.

## Installing

1. Plug an external USB storage drive into your Xbox and format it for *media* (not apps/games). This wipes its contents. It will be your `E:\` drive. The Xbox formats it as an ordinary NTFS volume and sets permissions on it that let apps read and write it; there is nothing special about its partition layout. A Windows PC reads and writes it like any other NTFS drive (after it has been in the Xbox, Windows may warn that the drive has errors, though the files are intact); macOS reads NTFS but writes it only with third-party software, and Linux needs the `ntfs3` driver or `ntfs-3g`, which most distributions include. An NTFS drive formatted elsewhere and given the same permission (see the warning below) may also work, but we haven't tried it.
2. Create a folder named `ringracers` at the root of the drive.
3. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [official v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) into `E:\ringracers` (so you have e.g. `E:\ringracers\bios.pk3` and `E:\ringracers\data\`). Files from other versions won't load.
4. Install the app on your Xbox. Unless you've modded your console somehow, this requires [Dev Mode](https://developer.microsoft.com/en-US/games/partner/signup). Builds come from [GitHub Actions](#github-actions) or a [local build](#local).
5. Changing the "App type" that Dev Home shows for the game (App or Game) isn't necessary. The package declares the `expandedResources` capability, which Microsoft documents as granting Game Mode resources on Xbox: CPU cores reserved for the app and a memory partition of at least 4 GB, where plain apps get a share of the cores, 1 GB and, on Xbox One, no Direct3D 12. On an Xbox One X with either type selected, the game got Direct3D 12, the same 5 GB memory budget and the same race speed. The setting does change some behaviour (with Game, controllers are reported twice, which the port's SDL patch handles), so if you do switch it, mention it when reporting a bug.

> [!WARNING]
> Unplug the USB drive before switching the Xbox to Retail mode. If the drive is plugged in when the console boots into Retail mode, the files on it can end up with attributes or permissions that stop the game from writing to them: `latest-log.txt` stops updating, and settings and saves may not be kept. To fix this, connect the drive to a Windows PC and reset the folder's attributes and permissions, for example (with the drive at `E:`):
>
> ```
> attrib -s -h -r E:\ringracers\* /s /d
> icacls E:\ringracers /reset /t /c
> icacls E:\ringracers /grant "*S-1-15-2-1:(OI)(CI)F" /t /c
> ```
>
> `S-1-15-2-1` is "ALL APPLICATION PACKAGES": the Xbox grants it when it formats the drive, and the game, like any app, needs it in order to write there.

## Differences from vanilla Ring Racers

[CHANGES.md](CHANGES.md) lists every change in detail. The ones you'll notice:

- **VibeRant D3D renderer (the default).** Vanilla's hardware renderer, Legacy GL, needs desktop OpenGL, which the Xbox doesn't have, so this port redid it on Direct3D 12. It runs faster than Software on Xbox and fills in much of what vanilla's Legacy GL lacks:
  - Software-style colors and palette flashes (the Palette Rendering option)
  - portals, as in Marble Garden Zone
  - fog volumes
  - Software's screen fades, wipes and Encore inversion
  - glowing sprite brightmaps
  - Software-strength underwater and heat distortion
  - add-on shaders
  - Water surfaces go beyond Software: viewed from above, their ripples also distort the track beneath the water, not just the background.

  Levels preload their textures and models to reduce hitches during races. Options → Video → Advanced also has **Better transparency ordering** (off by default; sorts translucent sprites and models among water and other see-through surfaces, at some CPU cost) and **Batching** (resets to On at each launch). To go back to Software, change Options → Video → Advanced → Renderer, or set `renderer` to `Software` in `E:\ringracers\ringracers\ringconfig.cfg`. A config saved by an earlier build keeps its renderer.
- **Faster performance.** The Software renderer, the game logic and the hardware renderer are optimized for the Xbox's CPU. Game logic gives the same results as vanilla, so replays, ghosts and netplay with PC players work as usual.
- **Startup file check in the background.** The game files are checked while the game loads instead of before; press B on the loading screen to skip it.
- **Files on the USB drive.** Config and saves are in `E:\ringracers\ringracers\`.
- **Voice chat** asks for microphone access. If you deny it, voice input stays off.
- **Tip:** in Software, turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves some time each frame when the camera tilts.

### Not yet tested on Xbox

- fog volumes
- sprite brightmaps
- split-screen with VibeRant D3D, including heat distortion
- Better transparency ordering

CHANGES.md marks every untested change.

## Reporting problems

Report bugs on [this repository's issues](https://github.com/ZebulanStanphill/ringracers-uwp/issues), not to Kart Krew. Include these files from `E:\ringracers`:

- `latest-log.txt`: the log of the last session, with loading times, performance every 5 seconds, and the call stack if the game crashed.
- `error.txt`: the last error message, if the game showed one.
- `uwp-profile.txt`: which code the game spent its time in while racing. Each launch replaces it, so copy it off before launching again.

## Building

### GitHub Actions

Every push builds the app on Windows and uploads a `ringracers-uwp` artifact containing a sideloadable `.msix` and its `.cer`. In Xbox Device Portal, install the `.cer` along with the package. The certificate is new for each run, so uninstall the previous build before installing one from a different run. The engine's native regression tests run in the fork's CI, described in [tests/README.md](https://github.com/ZebulanStanphill/RingRacers/blob/master/tests/README.md).

### Local

Requirements (Windows):

- Visual Studio 2022 with:
  - **Desktop development with C++**, including *C++ Clang tools for Windows* and *C++ CMake tools for Windows*
  - **Universal Windows Platform development**, including *C++ (v143) Universal Windows Platform tools*
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) cloned and bootstrapped, with `VCPKG_ROOT` pointing to it

The repository has to be cloned with its submodules: `git clone --recurse-submodules https://github.com/ZebulanStanphill/ringracers-uwp.git`, or run `git submodule update --init` in an existing clone.

From an **x64 Native Tools Command Prompt for VS 2022**:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

This builds the `RingRacers` submodule (initializing it if it's missing); builds SDL2 and downloads ANGLE into `deps/`; then generates and builds the launcher solution at `build/ringracers-uwp.sln`. Open that in Visual Studio to deploy to your Xbox (Remote Machine) or to create an app package (Project → Publish → Create App Packages).

### Repository layout

- `RingRacers/`: the [ZebulanStanphill/RingRacers](https://github.com/ZebulanStanphill/RingRacers) submodule, Ring Racers v2.4 with this port's engine changes as one commit each, along with their tests and Direct3D 12 docs.
- `patches/sdl-angle.patch`, `patches/sdl-controller-duplicates.patch` and `patches/sdl-egl-surface-handoff.patch`: build [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) for OpenGL ES, stop it from seeing each controller twice, and let Direct3D 12 take the window from ANGLE and give it back.
- `uwp/`: the launcher app, which links the game with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), SDL2, and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore).
- `tools/`: scripts that summarize the performance lines of `latest-log.txt`, turn `uwp-profile.txt` into function timings (with the `ringracers-uwp-symbols` artifact from the same build), and name the functions in a logged crash.

## Credits

The port builds on [worleydl's UWP port](https://github.com/worleydl/ringracers-uwp). VibeRant D3D's portals are inspired by [SRB2Kart Saturn](https://github.com/Indev450/SRB2Kart-Saturn)'s.
