# Ring Racers (Zebulan Edition)

> [!IMPORTANT]
> This is an **unofficial fork** of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) **v2.4**. It is not a Kart Krew Dev project and Kart Krew doesn't support it, so report its bugs [here](https://github.com/ZebulanStanphill/RingRacers/issues), not to them.

Dr. Robotnik's Ring Racers is a kart racing game by [Kart Krew Dev](https://www.kartkrew.org/). This fork adds an Xbox port and a Direct3D 12 hardware renderer to it, and makes the game faster on the Xbox's CPU. It stays compatible with the unmodified game: game logic gives the same results, so replays, ghosts and netplay with PC players work as usual.

The Xbox port is a UWP app for Xbox One and Xbox Series consoles in Dev Mode, based on [worleydl's port](https://github.com/worleydl/ringracers-uwp). It has only been tested on an Xbox One X. It should also work (even better) on Xbox Series X|S, and it may work (with lowered graphics settings) on an original Xbox One, but I don't have those consoles to test it on.

Coding agents were used heavily in the development of this fork.

## What this fork adds

[CHANGES.md](CHANGES.md) lists every change in detail and says which have been tested on Xbox. The ones you'll notice:

- **An Xbox port.** The game runs on Xbox consoles in Dev Mode, with its files, config and saves on a USB drive. See [Installing on Xbox](#installing-on-xbox).
- **VibeRant D3D, a Direct3D 12 renderer.** Vanilla's hardware renderer, Legacy GL, needs desktop OpenGL, which the Xbox doesn't have, so this fork redid it on Direct3D 12. It is the default on Xbox, where it runs faster than Software, and it fills in much of what vanilla's Legacy GL lacks:
  - Software-style colors and palette flashes (the Palette Rendering option)
  - portals, as in Marble Garden Zone
  - fog volumes
  - Software's screen fades, wipes and Encore inversion
  - glowing sprite brightmaps
  - Software-strength underwater and heat distortion
  - translucent sprites and models sorted, among water and other see-through surfaces
  - add-on shaders
  - Water surfaces go beyond Software: viewed from above, their ripples also distort the track beneath the water, not just the background.

  It aims to look like Software. Where it differs on purpose, such as true perspective when looking up or down, is listed under [Intended differences from Software](CHANGES.md#intended-differences-from-software).
- **VibeRant D3D on Windows PCs.** Windows desktop builds have it as a third renderer beside Software and Legacy GL. This has only been tested under Wine, not on a real Windows PC.
- **Faster performance on Xbox.** The Software renderer, the game logic and the VibeRant D3D renderer are optimized for the Xbox's CPU, startup is shorter, and levels preload their textures and models to reduce hitches during races.
- **New video options.** **Screen Scaling** (Fit, Integer or 1:1) places the picture on the screen, and **Camera Position** gives each local player a 3rd Person or 1st Person view.
- **Logs for bug reports.** On Xbox the game writes a log, the last error message and a performance profile to the USB drive. See [Reporting problems](#reporting-problems).

Things to know on Xbox:

- **Renderer.** To go back to Software, change Options → Video → Advanced → Renderer, or set `renderer` to `Software` in `E:\ringracers\ringracers\ringconfig.cfg`. A config saved by an earlier build keeps its renderer.
- **Startup file check in the background.** The game files are checked while the game loads instead of before.
- **Files on the USB drive.** Config and saves are in `E:\ringracers\ringracers\`.
- **Voice chat** asks for microphone access. If you deny it, voice input stays off.
- **Tip:** in Software, turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves some time each frame when the camera tilts.

## Installing on Xbox

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
6. Install the app on your Xbox. Unless you've modded your console somehow, this requires [Dev Mode](https://developer.microsoft.com/en-US/games/partner/signup). Builds come from [GitHub Actions](#github-actions) or a [local build](#xbox-app-on-windows).
7. Changing the "App type" that Dev Home shows for the game (App or Game) isn't necessary. The package declares the `expandedResources` capability, which Microsoft documents as granting Game Mode resources on Xbox: CPU cores reserved for the app and a memory partition of at least 4 GB, where plain apps get a share of the cores, 1 GB and, on Xbox One, no Direct3D 12. On an Xbox One X with either type selected, the game got Direct3D 12, the same 5 GB memory budget and the same race speed. The setting does change some behaviour (with Game, controllers are reported twice, which the port's SDL patch handles), so if you do switch it, mention it when reporting a bug.

### If the game can't use the drive

Every error message the game shows ends with the drives it can see, and is also saved as `E:\ringracers\error.txt` when the drive is writable. Error 5 means access denied and error 2 or 3 means not found:

- `E: error 5` or `E:\ringracers\bios.pk3: error 5`: the drive's permissions don't let the game in. Run step 4 again.
- `E: ok, ringracers error 2`: the drive works, but there is no `ringracers` folder at its root.
- `E:\ringracers\bios.pk3: error 2`: the folder is there, but the game files aren't in it (step 3).

Files copied onto the drive from a Mac or Linux PC through NTFS-3G, which most of their NTFS drivers use, don't inherit the drive's permissions: they get NTFS-3G's own default, which leaves out ALL APPLICATION PACKAGES. Run step 4 again after copying files that way; in Xbox Media Drive Helper, prepare the drive again and add to the existing journal. Files copied on Windows inherit the permissions.

If Windows reports errors on the drive, copy `ringracers\ringracers` (your config and saves) off it before letting Windows repair it. A repair moves files it can't put back in place into a `found.000` folder at the root of the drive; on our test drive, that was everything the game had written.

## Reporting problems

Report bugs on [this repository's issues](https://github.com/ZebulanStanphill/RingRacers/issues), not to Kart Krew. On Xbox, include these files from `E:\ringracers`:

- `latest-log.txt`: the log of the last session, with loading times, performance every 5 seconds, and the call stack if the game crashed.
- `error.txt`: the last error message, if the game showed one.
- `uwp-profile.txt`: which code the game spent its time in while racing. Each launch replaces it, so copy it off before launching again.

## Building

### GitHub Actions

Every push runs two workflows:

- **Build** builds the Xbox app on Windows and uploads a `ringracers-uwp` artifact containing a sideloadable `.msix` and its `.cer`. In Xbox Device Portal, install the `.cer` along with the package. The certificate is new for each run, so uninstall the previous build before installing one from a different run.
- **Tests** runs the native regression tests and the Direct3D 12 tests described in [tests/README.md](tests/README.md), checks that Linux and macOS still build, and uploads the Windows desktop build as `windows-desktop`.

### Xbox app on Windows

Requirements:

- Visual Studio 2022 with:
  - **Desktop development with C++**, including *C++ Clang tools for Windows* and *C++ CMake tools for Windows*
  - **Universal Windows Platform development**, including *C++ (v143) Universal Windows Platform tools*
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) cloned and bootstrapped, with `VCPKG_ROOT` pointing to it

From an **x64 Native Tools Command Prompt for VS 2022**, in a clone of this repository:

```
powershell -ExecutionPolicy Bypass -File uwp\build.ps1
```

This builds the engine as a static library; builds SDL2 and downloads ANGLE into `uwp/deps/`; then generates and builds the launcher solution at `uwp/build/ringracers-uwp.sln`. Open that in Visual Studio to deploy to your Xbox (Remote Machine) or to create an app package (Project → Publish → Create App Packages).

[docs/uwp-pgo.md](docs/uwp-pgo.md) describes collecting profile-guided optimisation data on Xbox: installing an instrumented build, playing a varied session, and sending the captures for future optimised builds.

### Desktop game

The game for Windows, Linux and macOS builds as upstream's does, with a compatible C++ toolchain (GCC, MinGW, Clang or Apple Clang), CMake and Microsoft vcpkg. The compiler and runtime libraries must support ISO C++17 and ISO C11.

On Linux you need these libraries on the system: libcurl, zlib, libpng, libogg, libvorbis, libvpx, libyuv, SDL2 and libopus. On Windows and macOS, install [vcpkg](https://vcpkg.io/en/) instead to build them alongside the game.

There are [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) in `CMakePresets.json`, which need ninja as well. Some of them:

- `ninja-debug`: non-optimized, assertions enabled
- `ninja-develop`: optimized, assertions enabled
- `ninja-release`: optimized
- `ninja-x86_mingw_static_vcpkg-debug`, `-develop` and `-release`
- `ninja-x64_osx_vcpkg-debug`, `-develop` and `-release`
- `ninja-arm64_osx_vcpkg-debug`, `-develop` and `-release`

The vcpkg presets need the `VCPKG_ROOT` environment variable set before the first `cmake` run. Their build directories are subdirectories of `build/`. Configure and build with, for example:

    cmake --preset ninja-x86_mingw_static_vcpkg-develop
    cmake --build --preset ninja-x86_mingw_static_vcpkg-develop

VibeRant D3D is built into Windows builds made with MSVC or clang-cl (`SRB2_CONFIG_D3D12`); the Tests workflow shows how.

### Repository layout

`master` is upstream's `maintenance-2.4` branch with this fork's changes on top, one commit per change. What the fork adds beside the engine's own folders:

- `uwp/`: the Xbox app. The launcher links the game with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), SDL2, and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore); `uwp/build.ps1` builds it.
- `uwp/patches/`: build [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) for OpenGL ES, stop it from seeing each controller twice, and let Direct3D 12 take the window from ANGLE and give it back.
- `uwp/pgo/`: recordings from the Xbox that the build uses to optimise the engine.
- `uwp/tools/`: scripts that summarize the performance lines of `latest-log.txt`, turn `uwp-profile.txt` into function timings (with the `ringracers-uwp-symbols` artifact from the same build), and name the functions in a logged crash.
- [`uwp/tools/storage-probe/`](uwp/tools/storage-probe/): a separate Xbox Dev Mode app that measures storage volume identities and toggles a 512 MiB ballast file to test whether apps share internal space.
- `src/hardware/r_d3d12/`: the VibeRant D3D driver, with its docs in `docs/`.
- `tests/`: the regression tests and test add-ons.
- [CHANGES.md](CHANGES.md): the changelog.

The Xbox app used to be a separate repository, [ringracers-uwp](https://github.com/ZebulanStanphill/ringracers-uwp), which is now archived; its history is part of this one.

## Credits

- [Kart Krew Dev](https://www.kartkrew.org/) made Dr. Robotnik's Ring Racers, which is based on [Sonic Robo Blast 2](https://srb2.org/), itself based on [Doom Legacy](http://doomlegacy.sourceforge.net/). Their source is at [gitlab.com/kart-krew-dev/ring-racers](https://gitlab.com/kart-krew-dev/ring-racers), mirrored at [KartKrewDev/RingRacers](https://github.com/KartKrewDev/RingRacers).
- [worleydl](https://github.com/worleydl) made the [UWP port](https://github.com/worleydl/ringracers-uwp) this one builds on, along with [SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) and [uwp-dep](https://github.com/worleydl/uwp-dep).
- VibeRant D3D's portals are inspired by [SRB2Kart Saturn](https://github.com/Indev450/SRB2Kart-Saturn)'s, and its palette rendering is ported from Sonic Robo Blast 2.

## License and disclaimer

The source code is available under the GNU General Public License version 2.0 or higher, as Ring Racers' is.

Dr. Robotnik's Ring Racers is a work of fan art made available for free without intent to profit or harm the intellectual property rights of the original works it is based on. Neither Kart Krew Dev nor this fork is affiliated with SEGA Corporation, and neither claims ownership of any of SEGA's intellectual property used in the game.
