# Changes from Ring Racers v2.4

Everything this port adds, removes, or changes compared with vanilla [Ring Racers v2.4](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4). Unless noted otherwise, the engine changes are in `patches/ringracers-uwp.patch`.

Where each change applies:

- **UWP builds only** (`SRB2_CONFIG_UWP`, which defines `_UWP`): Xbox-specific behavior, diagnostics, Legacy GL's Direct3D 11 port, and some of the optimizations.
- **Every platform built from the patched source:**
  - the GL2 RHI's call reductions and OpenGL ES support
  - the colormap texture caching
  - the software renderer optimizations, except inlining `R_GetTextureNum`
  - the Lua userdata set
  - the state-change and interpolation-list changes
  - Legacy GL's skybox and neutral camera-fog fixes

None of the changes alter game logic results. The optimizations of game logic do the same work in less time, so replays, netplay, and ghosts should behave as in vanilla.

Changes marked *untested* are in the latest build but haven't been tried on an Xbox yet.

## Platform and build

- **Static library for a UWP launcher.** The `SRB2_CONFIG_UWP` option and the `ninja-x64_windows_uwp_vcpkg-release` preset build the game as a static library instead of an executable. The `uwp/` launcher links it, and the executable name gets no Git revision suffix. Based on worleydl's uwp-compat branch, rebased onto v2.4.
- **Dependencies.** SDL2 and libuwp come from [worleydl/uwp-dep](https://github.com/worleydl/uwp-dep) instead of vcpkg.
- **CPU target.** The game is compiled for the Xbox One's Jaguar CPU (`-march=btver2`). Xbox Series X|S CPUs support all of its instructions. Jaguar has no FMA, so floating-point results match other x64 builds.
- **Launcher (`uwp/`).**
  - Starts SDL through `SDL_WinRTRunApp`.
  - Caches the CoreWindow on the UI thread so other threads can use it.
  - Reports the process memory budget to the game.
  - The manifest is version 2.4.0.0. It requests file system, removable storage, network, expanded resources, and microphone access (for 2.4's voice chat).
- **Build script and CI.**
  - `build.ps1` clones v2.4, applies the patch, builds SDL2 and the game, downloads ANGLE (checking its SHA-256), and generates the launcher solution.
  - GitHub Actions builds every push.
  - Each run uploads a sideloadable `.msix` signed with a throwaway certificate, plus the linker map (`ringracers-uwp-symbols`).
- **Engine build changes.**
  - Legacy GL's Direct3D 11 shaders are compiled with `fxc` at build time.
  - The DbgHelp and cpptrace libraries aren't linked.
  - Runtime DLLs aren't copied next to the library.

## Graphics

- **OpenGL ES through ANGLE.** The game renders with OpenGL ES 2 on ANGLE, which runs on Direct3D 11, instead of desktop OpenGL. Mesa's Direct3D 12 driver, which worleydl's port used, needs shader model 6, which Xbox One consoles don't offer to UWP games. To support this, the GL2 RHI backend:
  - creates OpenGL ES 2 contexts
  - compiles shaders as GLSL ES 1.00
  - loads the framebuffer functions that are core in ES 2
  - uses `glClearDepthf`
  - creates textures with unsized formats
  - reads screenshots back as RGBA
  - skips `glReadBuffer`
- **SDL for ANGLE** (`patches/sdl-angle.patch`). worleydl's SDL2 for UWP is built with EGL and OpenGL ES 2 instead of WGL and desktop OpenGL.
- **Display size.**
  - The display size and refresh rate come from libuwp instead of SDL.
  - The game renders at the size of ANGLE's window surface, which can be smaller than the HDMI display mode.
  - The window is always fullscreen, which fixes a tiny loading screen.
- **VibeRant D3D11.**
  - Legacy GL needs desktop OpenGL, which ANGLE doesn't provide. So its hardware renderer draws through a new Direct3D 11 driver (`src/hardware/r_d3d11/`) instead of `r_opengl.c`.
  - The driver follows `r_opengl.c` call for call, and its GLSL shaders are ported to HLSL.
  - It renders with its own Direct3D 11 device. Each frame is copied to a texture shared with ANGLE, which draws it to the window a frame later. If ANGLE can't open the shared texture, the frame is read back instead.
  - Custom shaders from add-ons aren't supported.
  - In the menus, the renderer is named **VibeRant D3D11**, as are its options header and the warning shown when switching to it, which says in red that it's missing some features instead of calling it incomplete and broken. The description above the Renderer option calls it an enhanced port of Legacy GL with 3D models, true perspective, and more parity with Software, with some features still missing or degraded, and the red text asks for visual bugs to be reported to GitHub.com/ZebulanStanphill/ringracers-uwp instead of not at all. The loading screen while a level's geometry is prepared says some visual features are missing instead of many being missing or broken.
- **Legacy GL: skybox views** *(untested)*. Precipitation and things with `RF_HIDEINSKYBOX` are left out of the skybox view, as in the Software renderer.
- **Legacy GL: fog around the camera**. The title-screen background starts black and fades in once, like Software, instead of fading twice and getting too bright. For a camera inside one inward-facing, shaded fog block with a black tint and fade, cache its control sector once per view, cap neutral surfaces' light at its light level, and use its colormap for walls, floors/ceilings, sprites and models. In smooth lighting, apply the neutral tint through surface modulation so brightmaps cannot bypass the initial black. Apply one multiplicative pass to the sky or skybox background, and leave out that block's boundary polygons to prevent double application and brightening. This is a renderer-only approximation: it applies across the view rather than clipping at each fog exit, uses a simple sky attenuation, and retains Legacy GL's brightmap lighting behavior during the fade. Colored surface colormaps, colored/outward fog, water, unshaded/colormap-only fog and ambiguous overlapping camera fog retain their existing paths; exact Software boundary-distance remapping and full fog-volume parity are not implemented. The volume search runs once per viewpoint, with no per-polygon FOF searches or framebuffer copies.
- **Legacy GL: palette rendering**, ported from SRB2's hardware renderer.
  - Toggled by `gr_paletterendering` (Options → Video → Advanced → Palette Rendering). It's on by default and needs Shaders on.
  - As in the Software renderer, colors are limited to the palette, and surfaces are lit through the colormaps.
  - Palette flashes and the color profile apply to the whole frame. Without palette rendering, Legacy GL shows flashes as a white or pink overlay and has no other flash palettes.
- **Legacy GL: palette fade maps.** On UWP, with Palette Rendering on, the level fade-in at the start of each race and other palette fades such as demo exits use Software's fade-map rows instead of a black/white translucent overlay. Cache each lump as a 256-column texture, remap the current screen before the HUD, and return master-palette colors so the final screen-palette pass applies flashes and color correction once. Clamp strength to the available rows. Keep the overlay with Palette Rendering off or if the lump or driver resources are unavailable; the fade trace records `path=exact` or `path=overlay`.
- **Legacy GL: portals.** On UWP, linedef special 40 portals (Marble Garden Zone and Media Studio each have one pair) show the area they connect to, instead of the entrance line's wall.
  - Each frame first finds the visible portal lines without drawing, then draws each portal's view before the view it's seen from. The portal's opening, the entrance line's front-sector floor-to-ceiling span, is marked in the stencil buffer, depth is reset to far inside it, and the destination view is drawn only there. Then the opening's own depth is written over it, so the surrounding scene is hidden behind the portal and drawn in front of it as usual.
  - The destination view uses Software's pairing (the first other special-40 line with the same first tag) and transform (rotated about the lines' centers, raised by the difference in their front floors).
  - Geometry behind the exit line is cut off on the GPU by a clip plane (`SV_ClipDistance`). Lines, BSP nodes and things entirely behind it are skipped on the CPU, along with the exit line itself.
  - Confirmed on Xbox One X. The player model can disappear at some positions and angles near a portal, as it also does in Software.
  - `maxportals` (default 2) limits how deep portals inside portals go, and 0 turns them off. Portal views use the regular sky, as in Software, which only shows skyboxes in the main view. Maps without portals skip the search.
- **Legacy GL: Encore screen inversion.** Encore level starts invert the screen instead of turning it white.
- **Legacy GL: screen wipes.** Level and menu transitions use the Software renderer's Mega Drive-style fades to black and white, inversion, Encore wiggle, and reversed fades.
- **Legacy GL: final transition frame**. Screen wipes show the current frame instead of the previous one, so Encore's inversion circle reaches the screen edges before presenting stops.
- **Legacy GL: video memory safeguard** *(untested)*. If local video memory usage exceeds its budget, the next frame clears the texture cache, at most once every five seconds. This may cause a brief hitch instead of sustained slowdowns. Model buffers are retained.
- **Audio.** The audio driver is WASAPI instead of DirectSound.

## Files and startup

- **Data folder.** Game data, config, and saves are read from and written to `E:\ringracers`. The `-home` parameter and the `RINGRACERSWADDIR` variable are ignored. The save folder is created before the game checks that the config is writable.
- **Log location.** The log is written to `E:\ringracers\latest-log.txt`, since the working directory is the read-only package folder.
- **File-check prompt.** At launch, the game asks whether to check its files, skip the check (the default, so A skips), or quit (also B). When the check is skipped:
  - files with an expected MD5 use it instead of being hashed
  - the music and sound files (which have no expected MD5) hash their names instead of their contents
  - zip entries' local headers aren't read until each entry is first loaded

## Performance

These came from profiling on an Xbox One X.

### Rendering (GPU calls)

- **Legacy GL: drawing settings uploads** *(UWP only)*. On drivers that support constant-buffer offsets and writes without overwriting pending draws, drawing settings use a 2 MB buffer with 512-byte slots. The buffer is discarded only when full; repeated settings skip uploading. Other UWP drivers use a default buffer updated with `UpdateSubresource`.
- **Legacy GL: level resource precaching** *(UWP only)*. During level loading, warm wall textures and flats (including brightmaps and FOF control sectors), existing object models, and current player models and colored textures. A second pass after bot setup, player spawning and level-start hooks warms final player skins/colors before gameplay. This moves USB reads and resource creation into loading to reduce hitches during racing. It runs only with Legacy GL, and skips dedicated servers, state reloads, net snapshots and demo rewind. Models need Models enabled and a valid model entry. Sprite-textured models remain lazy because their UVs need the view-selected sprite patch; future animation frames, view-dependent FOF remaps and later-spawned objects remain lazy. Wall/flat warming uses the renderer's existing cache functions and the first sidedef/sector's remap as the common case where a cache has only one variant.

Both changes are confirmed working in the latest Xbox One X test: roughly 27–54 FPS at 1920×1200 in races, compared with about 22 FPS at 640×400 before. Level loads were not noticeably longer, and some momentary pauses remain.

The CPU cost of GL calls through ANGLE is high on Xbox, so the GL2 RHI backend makes fewer of them:

- **Skipped redundant calls.**
  - State changes, texture binds, and buffer binds that wouldn't change anything are skipped.
  - Only the vertex attribute arrays that were enabled get disabled.
  - Sampler uniforms are only set when their slot changes.
- **Things kept between frames.**
  - Uniform locations are cached per program.
  - Framebuffers are kept between frames instead of being recreated every frame.
  - Colormap and lighttable textures are kept between frames and reuploaded only when their contents change. They're freed after 300 frames unused.
- **Fewer uploads.**
  - The palette is only uploaded when it changes, and the default colormap only once.
  - Single-channel textures use `GL_RED` (`EXT_texture_rg`) instead of `GL_LUMINANCE`, which ANGLE converted to RGBA on every upload.

### Software renderer

- **Visplanes.** A new visplane clears only the columns that can be on screen, instead of all `MAXVIDWIDTH` of them.
- **Screen tilting.** Tilted views are rotated by stepping along source rows, instead of rebuilding and reading a full-screen, 4-byte-per-pixel map every time the angle changes. The output is unchanged: the same float stepping, clipping, zoom, and mirrored sampling. Large views share the work across the parallel software rendering workers.
- **Sprite sorting.** Draw nodes that share no column with the sprite being sorted are skipped without reading what they point to.
- **3D floors.** Each 3D floor plane's bounds are calculated once during node sorting.
- **Wall columns.** Unused thick-side slots are left alone, and a wall column's data is only copied when it needs remapping.
- **Masked columns.** Each masked column's clipping limits are calculated once, and read-only column data is passed straight to the drawers.
- **Texture lookups.** `R_GetTextureNum` (texture translation lookup) is inline.

### Game logic

- **`FixedMul`** is inline. Calling it cost more than the multiply.
- **Subsector lookups.** `R_PointInSubsector` remembers the subsectors of 512 recent points. Walking the BSP from its root on every lookup was about 10% of game logic. The cache is cleared whenever a level's BSP is loaded or freed.
- **Lua userdata.** A set of the pointers that have Lua userdata lets freeing a block skip asking Lua, which was about 4% of game logic.
- **State changes.** `P_SetMobjState` and `P_SetPlayerMobjState` no longer clear a 40 KB table on every state change.
  - Upstream returns without counting its call as finished when a state removes its mobj. After that, every call counted as nested and cleared the table, about 5% of game logic.
  - Each nesting depth now has its own table, and each call erases only the states it recorded.
- **Interpolated mobjs.** Mobjs record their slot in the interpolation list, so removing one doesn't search the whole list.
- **Mobj references.** `P_SetTarget` (in release builds) and `P_MobjWasRemoved` are inline. The order of reference updates around collision callbacks is unchanged, as are the `PARANOIA` checks.

### Frame pacing and threads

- **Skipped frames.** A frame skipped to catch up on game logic no longer sleeps out its frame time, which had left the game idle about 16% of the time in Time Attack.
- **No-sleep exception off.** The game sleeps to the frame cap even with V-Sync on and the FPS cap set to match the refresh rate, since not sleeping disrupted frame timing on Xbox (from worleydl's port).
- **Thread pool size.** The thread pool is sized from the processors Xbox allocates to the game, falling back to upstream's hardware count when Xbox doesn't mark any.

## Diagnostics

- **Startup log.** `latest-log.txt` records:
  - the OpenGL strings, surface size, and shader compiles
  - each step of the first frames
  - how long each loading step and asset file took, and how much of that was logging
  - the thread pool size, the process memory budget, and the CPU sets Windows lists for the game
  - Legacy GL's constant-upload capabilities and selected path, plus per-pass precache counts for wall textures, flats, newly loaded models and model textures, and elapsed milliseconds
  - Legacy GL's driver startup, presentation path and sync method (ANGLE device queries or `glFinish` on fast frames), and errors (the `UWP D3D11:` lines)
  - the track title animation's frame count, elapsed time, and average and worst drawing/presentation times (`UWP: track title:`), for diagnosing uneven animation
- **Perf summary.** Every 5 seconds, the log gets a summary with:
  - the frame rate
  - the perfstats frame and tic timings
  - a breakdown of drawing the HUD and presenting
  - counts of draws, texture uploads, and resources created
  - the level's mobjs, `P_CheckPosition` calls, and Lua mobj hooks per tic
  - `R_PointInSubsector` cache hits
  - which processors the main thread ran on, and how long the profiler paused it
  - with Legacy GL, its renderer timings, and the driver's draws, state changes, texture uploads, CPU time, presentation timings, and GPU time
  - with Legacy GL, the constant-upload path, CPU milliseconds uploading constants and constant-ring wraps per frame, plus models loaded lazily during drawing; the count drawn without buffers remains
  - with Legacy GL, the portals drawn per frame and the CPU time their views took
- **Presentation synchronization counters.** The Legacy GL perf summary includes `ANGLE read waits` (waits that actually blocked and their total milliseconds), read-query timeouts, and fallback `glFinish` counts.
- **Drawn-frame trace** (UWP only). `UWP frame:` records the display player's interpolated and raw position, player 1's main camera before portal/skybox changes, elapsed time, tics since the previous world draw, interpolation fraction, and title-card timers. It runs while the in-race title card is up and from 10 to 20 seconds of level time, capped at 300 frames per window per load. Legacy GL adds the copied/shown presentation slots and source draw IDs after presenting, identifying previous-frame presentation and immediate wipe frames.
- **Fade/palette trace** (UWP only). `UWP fade:` records game-state changes, level loads, wipe starts/ends and driver paths, screen fades/inversion, palette changes/uploads, flash overlays, and title background choices. Each change/load opens a 10-second window capped at 200 events. The first 20 title-map camera light/tint/fog samples and first 20 steps of the first animated light-fade sector are included. These diagnostics read engine state without changing game logic or rendering behavior.
- **Legacy GL video memory diagnostics.** Startup logs local and non-local budgets, usage, and reservation figures when DXGI supports them. Each perf summary adds these figures and tracked texture bytes, model/static buffer bytes and counts, ring buffer bytes, and approximate render target/present slot bytes. GPU frames over 250 ms and copy waits that give up after 500 ms log immediate warnings with local usage/budget, limited to one line per second with suppressed warnings counted.
- **Sampling profiler** (`src/sdl/i_uwpprofile.cpp`). While a level is being played, the profiler samples the main thread about 50 times a second. It writes what code was running, with call stacks and what the game was doing, to `E:\ringracers\uwp-profile.txt`.
- **Crash logging.** Crashes and their call stacks are logged from a vectored exception handler. The app runtime, not the C runtime, starts the main thread, so the game's signal handler never sees an access violation on Xbox.
- **Error dialog.**
  - It shows only the error, since the Xbox cuts off long text.
  - The error is followed by which drives the app can see and whether each has a `ringracers` folder.
  - The dialog's text is also saved to `E:\ringracers\error.txt`.
- **Tools** (`tools/`, not part of the game). Scripts that summarize the perf log, turn the profile into functions, annotate a function's instructions with samples, and name the functions in a logged crash.

## Fixes

- **Legacy GL: rapid-frame presentation**. Before reusing a presentation slot, wait for ANGLE's GPU to finish reading it, preventing out-of-order frames such as the loading percentage flipping between 0% and 100%. Use per-slot event queries on ANGLE's D3D11 device when EGL device queries are available; otherwise call `glFinish` after drawing frames presented less than 8 ms apart. Read waits give up after 100 ms and count timeouts. Normal shared presentation remains one frame late, and wipe frames still present immediately. The readback path is covered too; its texture upload and draw already share ANGLE's ordering. Confirmed on Xbox One X with ANGLE device queries: frames stay in order, the loading percentage counts up normally, and the player model no longer jumps back and forth. The percentage and model symptoms were the same presentation-slot reuse issue.
- **Track title timing**. On UWP, the animation before a race starts with a fresh clock after loading, so it plays in full. It retains upstream's one animation step per frame and HUD timer clamp. This clock fix has no effect on game logic.
- **Legacy GL: crash drawing sprite-textured models** *(untested)*. A model that uses its sprite's graphics as its texture (such as the item box) read the sprite's hardware data before it existed, which crashed if the sprite hadn't been prepared since video settings last changed (seen on Xbox with 3D models on and shaders off, on Storm Rig). The sprite is now prepared first, and the check for re-fitting the model's texture coordinates compares the sprite's dimensions, as its comment intends, instead of the model texture's, which such models may not have. Applies to every platform built from the patched source.
- **Legacy GL: missing floors and ceilings** *(untested)*. Some floors and ceilings weren't drawn, showing the sky through the track on courses such as Green Hills Zone and Storm Rig. When a level loads, Legacy GL builds one floor/ceiling polygon per subsector by cutting the map along its BSP partition lines. Upstream sent a polygon the cutting line doesn't really split to the front side without checking, and small rounding errors made that happen to polygons lying almost entirely behind the line, so whole groups of subsectors got no polygon and their planes were never drawn. Unsplit polygons now go to the side they actually lie on, and any subsector still without a polygon gets one from its closed GL-seg loop. Across the 149 stock maps with XGL3 nodes, this cuts the missing floor area from about 3.7 million to 11 thousand square units (some, like Sealed Star Gallery, were missing far more than the two reported). Inherited from upstream Legacy GL; applies to every platform built from the patched source. Some subsectors can still get a polygon larger than the subsector, also inherited.
- **Options button slide** *(untested)*. On UWP, the button with the submenu's name at the top of the Options submenus (Video, Sound, Voice, HUD, Gameplay, Server, Data, and the profile card in Profile Setup) slides into place once instead of bouncing up and down, sometimes for a long time, when the menus run below 35 FPS. The menu ticks at most once a frame, so a slow frame skips tics. Upstream only ends the slide on a tick at exactly its 5th tic and starts it over from the beginning on any later tick, so a skipped 5th tic made the button jump back and slide again. The slide now starts once and ends on any tick at or after its last tic.
- **Duplicate controllers** (`patches/sdl-controller-duplicates.patch`). Each controller no longer shows up twice when running as a game instead of an app, which made each press count twice.
- **State-change bookkeeping.** The `P_SetMobjState`/`P_SetPlayerMobjState` change under [Game logic](#game-logic) also fixes upstream's unbalanced call count.

## Removed or disabled

- **Crash handlers that can't work in the app container.** The DbgHelp minidump handler, cpptrace stack traces, and the `exchndl` crash handler are gone. The crash logging under [Diagnostics](#diagnostics) replaces them.
- **Legacy GL's OpenGL driver** (`r_opengl.c`) isn't used. The Direct3D 11 port replaces it, without support for custom shaders.
- **Master server debugging.** `masterserver_debug`'s verbose curl output and the Windows log-copy helper are only compiled with `LOGMESSAGES`, which is on by default, so this only matters to builds that turn logging off.
