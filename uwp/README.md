# Ring Racers UWP (Zebulan Edition)

Unofficial UWP port of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4**, intended for Xbox One and Xbox Series consoles. Requires Xbox Dev Mode. Based on [worleydl's port](https://github.com/worleydl/ringracers-uwp); this is not an official Kart Krew project, so don't report bugs to them.

Only tested on Xbox One X. Should also work (even better) on Xbox Series X|S, and it may work (with lowered graphics settings) on an original Xbox One, but I don't have those consoles to test it on. Probably also works on Windows 10/11 PCs, but I can scarcely think of a reason why you wouldn't just use the official release there.

Coding agents were used heavily in the development of this port.

## Installing

You need a USB drive and a PC to prepare it: Windows, or a Mac or Linux PC with [Xbox Media Drive Helper](https://github.com/ZebulanStanphill/xbox-media-drive-helper).

1. Format the drive as NTFS: on Windows, right-click it in File Explorer and choose **Format**; on Linux, use `mkfs.ntfs`; macOS can't format NTFS without third-party software. This erases the drive. A drive that is already NTFS can be used as it is. exFAT and FAT32 have no permissions to set in step 4, and haven't been tried.
2. Create a folder named `ringracers` at the root of the drive.
3. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [official v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) into that folder (so you have e.g. `ringracers\bios.pk3` and `ringracers\data\`). Files from other versions won't load. A Mac needs third-party software to write to NTFS; Linux uses the `ntfs3` driver or `ntfs-3g`.
4. Give apps access to the drive. On Windows, open a Command Prompt as administrator and run these, with `X:` replaced by the drive's letter on the PC (on a large drive they take a while):

   ```
   icacls X:\ /grant "*S-1-15-2-1:(OI)(CI)F" /t /c
   icacls X:\ /grant "*S-1-5-11:(OI)(CI)M" /t /c
   ```

   The game runs in a sandbox, like every app on the Xbox, and Windows lets a sandboxed app open a file only if the file's permissions allow both the app, through "ALL APPLICATION PACKAGES" (`S-1-15-2-1`), and the user it runs as, for example through "Authenticated Users" (`S-1-5-11`). A drive formatted by Windows already allows Authenticated Users but not ALL APPLICATION PACKAGES, so the first command is the one it needs. The second is for drives prepared some other way, such as our test drive, which allowed only the app side and which the game couldn't open until Authenticated Users was added. `(OI)(CI)` makes files created later, on Windows or by the game, get the same access. On a Mac or Linux PC, Xbox Media Drive Helper's **Prepare for Xbox** makes both grants (skipping the second where the drive already allows the user) and saves a journal that can undo them.
5. Plug the drive into the Xbox, where it is `E:\`, so the game finds its files in `E:\ringracers`. The Xbox doesn't format a drive it uses for media or change its permissions, so everything the game needs comes from the steps above. A drive of at least 128 GB on USB 3.0 or later makes the Xbox ask how to use it. Choose **Use for media**: **Format storage device** erases it for installed games, after which PCs see it as unformatted. Smaller or slower drives are used for media without asking.
6. Install the app on your Xbox. Unless you've modded your console somehow, this requires [Dev Mode](https://developer.microsoft.com/en-US/games/partner/signup). Builds come from [GitHub Actions](#github-actions) or a [local build](#local).
7. Changing the "App type" that Dev Home shows for the game (App or Game) isn't necessary. The package declares the `expandedResources` capability, which Microsoft documents as granting Game Mode resources on Xbox: CPU cores reserved for the app and a memory partition of at least 4 GB, where plain apps get a share of the cores, 1 GB and, on Xbox One, no Direct3D 12. On an Xbox One X with either type selected, the game got Direct3D 12, the same 5 GB memory budget and the same race speed. The setting does change some behaviour (with Game, controllers are reported twice, which the port's SDL patch handles), so if you do switch it, mention it when reporting a bug.

### If the game can't use the drive

Every error message the game shows ends with the drives it can see, and is also saved as `E:\ringracers\error.txt` when the drive is writable. Error 5 means access denied and error 2 or 3 means not found:

- `E: error 5` or `E:\ringracers\bios.pk3: error 5`: the drive's permissions don't let the game in. Run step 4 again.
- `E: ok, ringracers error 2`: the drive works, but there is no `ringracers` folder at its root.
- `E:\ringracers\bios.pk3: error 2`: the folder is there, but the game files aren't in it (step 3).

Files copied onto the drive from a Mac or Linux PC through NTFS-3G, which most of their NTFS drivers use, don't inherit the drive's permissions: they get NTFS-3G's own default, which leaves out ALL APPLICATION PACKAGES. Run step 4 again after copying files that way; in Xbox Media Drive Helper, prepare the drive again and add to the existing journal. Files copied on Windows inherit the permissions.

If Windows reports errors on the drive, copy `ringracers\ringracers` (your config and saves) off it before letting Windows repair it. A repair moves files it can't put back in place into a `found.000` folder at the root of the drive; on our test drive, that was everything the game had written.

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
- wall light slices in split-screen
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
