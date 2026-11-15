# Ring Racers (Zebulan Edition)

> [!IMPORTANT]
> This is an **unofficial fork** of [Dr. Robotnik's Ring Racers](https://github.com/KartKrewDev/RingRacers) v2.4. Kart Krew Dev neither made nor supports this fork. Report bugs [here](https://github.com/ZebulanStanphill/RingRacers/issues), not to them.

This fork does two things the official game doesn't:

- It runs on Xbox One and Xbox Series consoles in Dev Mode.
- It has a new hardware renderer, VibeRant, which looks much closer to the Software renderer than the official hardware renderer, Legacy GL, does. It runs on Direct3D 12 on Xbox and Windows (VibeRant D3D) and on Vulkan on Linux and Windows (VibeRant Vulkan).

The game itself plays the same. Replays, ghosts, and online play with people on the official game work as usual.

I've only tested the Xbox port on an Xbox One X. It should run better on Xbox Series X|S, and it may run on an original Xbox One with lowered graphics settings, but I don't have those consoles to try. If you have one of these consoles and can test the game, let me know how it runs!

The Xbox port builds on the [older one by worleydl](https://github.com/worleydl/ringracers-uwp) and goes further beyond, with an aim of providing as seamless an experience as possible.

Coding agents were used heavily in the development of this fork. I have thoroughly playtested it to iron out various bugs and corner cases. (Shout out to Green Hills Zone, which seemed to be a nexus of such issues, for whatever reason.) I intend to actively maintain this fork and keep it up-to-date with future releases of the vanilla game.

## What's different from the official game

[CHANGES.md](CHANGES.md) lists every change and says which have been tested. All Xbox testing was on an Xbox One X. This is what you'll notice.

### VibeRant D3D and VibeRant Vulkan

Legacy GL needs desktop OpenGL, which the Xbox doesn't have. VibeRant D3D started as Legacy GL moved to Direct3D 12 and then gained what Legacy GL draws wrongly or not at all:

- Software's colors and palette flashes (the Palette Rendering option)
- portals, as in Marble Garden Zone
- fog volumes
- Software's screen fades, wipes, and Encore inversion
- glowing sprite brightmaps
- underwater and heat distortion as strong as Software's
- translucent sprites, models, water, and other see-through surfaces drawn in the right order
- add-on shaders

It goes past Software in one place: seen from above, rippling water also distorts the track under it, not just the background. The other places where it differs from Software on purpose, such as true perspective when you look up or down, are listed under [Intended differences from Software](CHANGES.md#intended-differences-from-software).

On Xbox, VibeRant D3D is the default renderer, and on the Xbox One X it runs faster than Software.

**VibeRant Vulkan** is the same renderer on Vulkan, for PCs. It isn't a second implementation: it shares VibeRant D3D's drawing code and shaders, and draws the same picture. Compared on 1,002 frames from all 213 tracks, the two differ in about one pixel in a thousand, at the edges between texels. It has been tested on one Linux PC, with an AMD Radeon. If Vulkan isn't working on your PC, the game says so and uses Software.

Windows desktop builds have both. Those have only been tested under Wine, not on a real Windows PC.

### On Xbox

- The game's files, your config, and your saves are on a USB drive, in `E:\ringracers`.
- Both renderers and the game logic are optimized for the Xbox One's CPU, which the Xbox Series consoles' CPUs are compatible with, startup is shorter, and each level loads its textures and models up front so that races hitch less.
- To switch to the Software renderer, use Options → Video → Advanced → Renderer, or set `renderer` to `Software` in `E:\ringracers\ringracers\ringconfig.cfg`.
- Voice chat asks for microphone access. If you deny it, voice input stays off.
- In Software, turning off Screen Tilting (Options → Profile Setup → your profile → Accessibility; the Guest profile can't be edited) saves some time each frame when the camera tilts.

### New video options

Both are in Options → Video.

**Screen Scaling** decides how the picture is enlarged when the game's resolution is lower than your screen's:

- **Fit** (the default, and what the game did before) enlarges it as far as it will go without changing its shape.
- **Integer** enlarges it by a whole number (2×, 3×, and so on), so every game pixel comes out the same size.
- **1:1** doesn't enlarge it at all.

Integer and 1:1 can provide a sharper picture for certain resolutions, with the trade-off of a black border/margin surrounding the picture.

**Camera Position** lets each local player pick the usual view from behind the kart (**3rd Person**) or a view from the driver's seat (**1st Person**). The official game hides this feature under a console command (`chasecam`), and forgets your setting when you quit.

## Installing on Xbox

You need:

- an Xbox One or Xbox Series console in [Developer Mode](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/devkit-activation)
- a USB drive
- a PC to prepare the drive with: Windows, or a Mac or Linux PC with [Xbox Media Drive Helper](https://github.com/ZebulanStanphill/xbox-media-drive-helper)

### Prepare the USB drive

1. Format the drive as NTFS, unless it already is. This erases the drive. On Windows, right-click it in File Explorer and choose **Format**. On Linux, use `mkfs.ntfs`. macOS needs third-party software to format or write to NTFS. exFAT and FAT32 drives haven't been tried; they have no file permissions, so step 4 doesn't apply to them.
2. Create a folder named `ringracers` at the top of the drive.
3. Extract `Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip` from the [official v2.4 release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4) into that folder, so that you have `ringracers\bios.pk3` and `ringracers\data\`. Files from other versions of the game won't load.
4. Give Xbox apps access to the drive.
   - On Windows, open a Command Prompt as administrator and run these two commands, with `X:` replaced by the drive's letter. On a large drive they take a while.

     ```
     icacls X:\ /grant "*S-1-15-2-1:(OI)(CI)F" /t /c
     icacls X:\ /grant "*S-1-5-11:(OI)(CI)M" /t /c
     ```
   - On a Mac or Linux PC, use **Prepare for Xbox** in Xbox Media Drive Helper.
5. Plug the drive into the Xbox. If the Xbox asks how to use it, choose **Use for media**. **Format storage device** would erase it and turn it into storage for installed games, after which PCs see it as unformatted. The Xbox only asks about drives of at least 128 GB on USB 3.0 or later.

The Xbox calls the drive `E:`, so the game looks for its files in `E:\ringracers`. It keeps your config and saves in `E:\ringracers\ringracers`.

### Install the game

1. Download the `ringracers-uwp` artifact of the latest successful **Build** run from this repository's [Actions](https://github.com/ZebulanStanphill/RingRacers/actions/workflows/build.yml) tab (GitHub asks you to sign in), or [build it yourself](#xbox-app-on-windows). It contains an `.msix` package and a `.cer` certificate.
2. In Xbox Device Portal, install the `.cer` along with the `.msix`. Each build has a new certificate, so uninstall the game before installing a different build. Your config and saves are on the USB drive and stay.
3. Start the game. You don't need to change the **App type** that Dev Home shows for it: on an Xbox One X, the game ran the same as an App and as a Game. If the game says at launch that it is running as an app, follow the steps it shows to change the type to Game. If you have changed the type, say so when you report a bug.

### Why the drive needs permissions

Preparing the drive includes giving Xbox apps access to it. That is needed because every app on the Xbox runs in a sandbox. Windows lets a sandboxed app open a file only if the file's permissions allow both the app and the user it runs as:

- The first command allows the app, through the group "ALL APPLICATION PACKAGES" (`S-1-15-2-1`). A drive formatted by Windows doesn't have this.
- The second allows the user, through "Authenticated Users" (`S-1-5-11`). A drive formatted by Windows already has this, but a drive formatted another way may not. The game couldn't open a drive that had only the first grant until the second was added.

`(OI)(CI)` makes files added later get the same access. Xbox Media Drive Helper makes both grants, skips the second where the drive already has it, and saves a journal that can undo them. The Xbox can't do this part for you. The only formatting it does is **Format storage device**, which turns a drive into storage for installed games. A drive used for media is left exactly as it is, permissions included.

Files copied onto the drive on Windows inherit these permissions. Files copied from a Mac or Linux PC through NTFS-3G don't: they get NTFS-3G's own default, which leaves out ALL APPLICATION PACKAGES. After copying files that way, give Xbox apps access to the drive again (in Xbox Media Drive Helper, prepare the drive again and add to the existing journal).

### If the game can't use the drive

Every error message the game shows ends with a list of the drives it can see. The message is also saved as `E:\ringracers\error.txt` if the drive can be written to. In the list, error 5 means access denied, and error 2 or 3 means not found:

- `E: error 5` or `E:\ringracers\bios.pk3: error 5`: the drive's permissions don't let the game in. Give Xbox apps access to the drive again.
- `E: ok, ringracers error 2`: the drive works, but there is no `ringracers` folder at the top of it.
- `E:\ringracers\bios.pk3: error 2`: the folder is there, but the game's files aren't in it. Extract the assets archive into it.

If Windows says the drive has errors, copy `ringracers\ringracers` (your config and saves) off it before letting Windows repair it. A repair moves files it can't put back in place into a `found.000` folder at the top of the drive. That happened to a drive of mine, and it took everything the game had written.

## Reporting problems

Report bugs on [this repository's issues](https://github.com/ZebulanStanphill/RingRacers/issues), not to Kart Krew. On Xbox, include these files from `E:\ringracers`:

- `latest-log.txt`: the log of the last session, with loading times, performance every 5 seconds, and the call stack if the game crashed.
- `error.txt`: the last error message, if the game showed one.
- `uwp-profile.txt`: which code the game spent its time in while racing. Each launch replaces it, so copy it off before launching again.

On a PC, include `latest-log.txt` from the game's own folder (`~/.ringracers` on Linux). For a problem with VibeRant Vulkan, start the game once with `-vkdebug` first if you have Vulkan's validation layers installed: their messages go into that log.

## Building

### GitHub Actions

Every push runs two workflows:

- **Build** builds the Xbox app on Windows and uploads it as the `ringracers-uwp` artifact, which [Install the game](#install-the-game) explains how to use.
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

This builds the engine, builds SDL2, downloads ANGLE into `uwp/deps/`, and then generates and builds the launcher solution at `uwp/build/ringracers-uwp.sln`. Open that in Visual Studio to deploy to your Xbox (Remote Machine) or to create an app package (Project → Publish → Create App Packages).

To help optimize future builds with a recording of your own play session, see [docs/uwp-pgo.md](docs/uwp-pgo.md).

### Desktop game

The game for Windows, Linux, and macOS builds the way upstream's does. You need a C++ toolchain that supports C++17 and C11 (GCC, MinGW, Clang, or Apple Clang), CMake, and ninja.

On Linux you need these libraries on the system: libcurl, zlib, libpng, libogg, libvorbis, libvpx, libyuv, SDL2, and libopus. On Windows and macOS, install [vcpkg](https://vcpkg.io/en/) instead to build them alongside the game.

`CMakePresets.json` has [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html), among them:

- `ninja-debug`: non-optimized, assertions enabled
- `ninja-develop`: optimized, assertions enabled
- `ninja-release`: optimized
- `ninja-x86_mingw_static_vcpkg-debug`, `-develop`, and `-release`
- `ninja-x64_osx_vcpkg-debug`, `-develop`, and `-release`
- `ninja-arm64_osx_vcpkg-debug`, `-develop`, and `-release`

The vcpkg presets need the `VCPKG_ROOT` environment variable set before the first `cmake` run. Their build directories are subdirectories of `build/`. Configure and build with, for example:

    cmake --preset ninja-x86_mingw_static_vcpkg-develop
    cmake --build --preset ninja-x86_mingw_static_vcpkg-develop

Linux builds include VibeRant Vulkan (`SRB2_CONFIG_VULKAN`), and Windows builds made with MSVC or clang-cl include it and VibeRant D3D (`SRB2_CONFIG_D3D12`). Configuring then downloads the pinned sources they are built with (glslang, SPIRV-Cross, Vulkan-Headers, volk, and, off Windows, DirectX-Headers); `-DSRB2_CONFIG_VULKAN=OFF` builds without them. To run, VibeRant Vulkan needs only your system's Vulkan loader and driver. `.github/workflows/tests.yml` has the commands for each platform.

### Repository layout

`master` is upstream's `maintenance-2.4` branch with this fork's changes on top, one commit per change. The fork's own folders:

- `uwp/`: the Xbox app's launcher and `build.ps1`. The launcher links the game with libuwp from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep), SDL2, and [ANGLE for UWP](https://www.nuget.org/packages/ANGLE.WindowsStore).
- `uwp/patches/`: changes to [worleydl's SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) that build it for OpenGL ES, stop it from seeing each controller twice, and let Direct3D 12 take the window from ANGLE and give it back.
- `uwp/pgo/`: recordings of play on an Xbox One X, which the build uses to optimize the engine.
- `uwp/tools/`: scripts that summarize the performance lines of `latest-log.txt`, turn `uwp-profile.txt` into function timings (with the `ringracers-uwp-symbols` artifact from the same build), and name the functions in a logged crash.
- [`uwp/tools/storage-probe/`](uwp/tools/storage-probe/): a separate Xbox Dev Mode app that measures storage volume identities and toggles a 512 MiB ballast file to test whether apps share internal space.
- `src/hardware/r_d3d12/`: the VibeRant driver, with its Vulkan backend in `vulkan/`. Its docs are in `docs/`.
- `tests/`: the regression tests and test add-ons.

## Credits

- [Kart Krew Dev](https://www.kartkrew.org/) made Dr. Robotnik's Ring Racers, which is based on [Sonic Robo Blast 2](https://srb2.org/), itself based on [Doom Legacy](http://doomlegacy.sourceforge.net/). Their source is at [gitlab.com/kart-krew-dev/ring-racers](https://gitlab.com/kart-krew-dev/ring-racers), mirrored at [KartKrewDev/RingRacers](https://github.com/KartKrewDev/RingRacers).
- [worleydl](https://github.com/worleydl) made the [UWP port](https://github.com/worleydl/ringracers-uwp) this one builds on, along with [SDL2 for UWP](https://github.com/worleydl/SDL-uwp-gl) and [uwp-dep](https://github.com/worleydl/uwp-dep).
- VibeRant D3D's portals implementation is inspired by [SRB2Kart Saturn](https://github.com/Indev450/SRB2Kart-Saturn)'s, and its palette rendering is ported from Sonic Robo Blast 2.

## License and disclaimer

The source code is available under the GNU General Public License version 2.0 or higher, as vanilla Ring Racers' is.

Dr. Robotnik's Ring Racers is a work of fan art made available for free without intent to profit or harm the intellectual property rights of the original works it is based on. Neither Kart Krew Dev nor this fork is affiliated with SEGA Corporation, and neither claims ownership of any of SEGA's intellectual property used in the game.
