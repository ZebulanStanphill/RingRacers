# VibeRant diagnostics

Use VibeRant diagnostics for pixel history, presentation, and GPU hangs. Presentation diagnostics
and GPU-hang reports here apply to VibeRant D3D, the Direct3D 12 backend.

## Performance logs (UWP)

If a five-second `UWP perf:` block is written during a level, its `level:` line records the map lump name and title, gametype, gear, and active modes. The `racers:` line gives the active player, bot, spectator, and local player counts, along with the 3D model and drop shadow settings and the first local player's FOV. Run `uwp/tools/perf-summary.py` with `-t` to see this context alongside each block.

## Camera position in screenshots (UWP)

On UWP, an in-level screenshot logs the camera position with any renderer.

```
UWP screenshot camera: ringracers0028.png map=N lt=N renderer=hardware camera=(x,y,z) yaw=Y pitch=P roll=R fov=F sector=N
```

Use the last drawn view's values (the last player's in split-screen) to reproduce a rendering bug from
the same spot on a PC. A screenshot needs no console, so it works even when opening the console makes
it impractical to keep the kart still.

## Pixel history: `gr_surfaceprobe`

`gr_surfaceprobe [x_percent y_percent]` (default `15 35`) logs pixel history:

```
surface probe: begin pixel=(x,y) screen=WxH format=N
surface probe: sample=i raw=0x%08X kind=... source=... texture=... name=... flags=0x... shader=N mask=N depth=(a,b,c) alpha=0.000 vertices=N
surface probe: sample=i v0=(x,z,y;s,t)
surface probe: end draws=N color_changes=N rows_omitted=N overflow=0
```

`raw` is the pixel as a little-endian RGBA8 DWORD. A `sample=` row appears for each draw that changed the
pixel's color (the first 256), with up to eight `v` rows for polygon and batch draws.

The driver samples the pixel during the world pass.

- After each polygon, indexed batch, sky loop, model mesh, or color clear, it transitions the screen to
  `COPY_SOURCE`, copies the one pixel into slot *i* of a single readback buffer with `CopyTextureRegion`, and
  transitions back. A slot is 512 bytes, the placed-footprint alignment, with a 256-byte row pitch, so
  8,192 slots take a 4 MB buffer.
- At the end of the world pass it submits (counted as a `readback` submission), waits up to two
  seconds, maps the used slots once, logs, and releases the buffer through `Core_Retire`.
- While no probe is requested, nothing is copied, allocated, or mapped.
- The requested frame stalls on the GPU. Don't read its timings as a benchmark.

## Presenting: the swap chain

On UWP, VibeRant D3D presents directly on the window with its own swap chain (`d3d_present_uwp.cpp`).
Windows desktop builds use `d3d_present_win32.cpp`, and VibeRant Vulkan uses `vulkan/vk_present.cpp`.
A CoreWindow takes
one swap chain at a time, so the driver takes the window from ANGLE: ANGLE draws to a 1x1 pbuffer, so that it
lets go of the window's back buffer, and its window surface is destroyed (SDL's `SDL_WinRTReleaseGLSurface`,
from `uwp/patches/sdl-egl-surface-handoff.patch`). The driver waits on the frame-latency object (latency 2) before filling the next of three back buffers. Fit
copies the frame to a frame-sized swap chain. Integer and 1:1 draw it into the placement rectangle of a
screen-sized swap chain. The driver presents with the sync interval set by `vid_wait`. Frames whose aspect
ratio differs from the window's get black letterbox bars. This needs no configuration.

### When the window is taken

- **When VibeRant D3D starts.** `VibeRant_Init` takes the window every time the driver starts or the player switches
  to VibeRant D3D, with ANGLE's context current. The swap chain starts at the video mode's size and resizes when the selected layout needs different buffer
  dimensions.
- **With the first frame, if no ANGLE context was current at start** (an `-opengl` launch starts the
  driver before the window exists). The log says `no ANGLE context is current, so the window can't be taken from
  it yet` until it can.
- **Software gets it back.** When the player switches to the Software renderer, the window goes back to ANGLE
  (`SDL_WinRTRestoreGLSurface`, retried up to three more times 50 ms apart, since DXGI can take a moment to
  release the CoreWindow), and ANGLE's context is made current again.

While the swap chain has the window, ANGLE has no window surface and no current context and makes no GL calls. `Present`'s sync interval controls vsync. The calls ANGLE sees are
the pbuffer, a clear, a flush, and the surface hand-off when taking or giving back the window, and a flush of its
Direct3D 11 context after the take.

### Log lines

```
VibeRant D3D: took the window from ANGLE: WxH swap chain, 3 buffers, frame latency 2 (X ms)
VibeRant D3D: resized the swap chain from AxB to CxD
VibeRant D3D: gave the window back to ANGLE (X ms)
```

and the perf line:

```
VibeRant perf: present: direct swap chain (3 buffers, latency 2); per frame copy %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures
```

`copy` measures recording and submitting the frame copy or placement draw. `latency wait` measures time
blocked on the waitable object (a timeout is 500 ms). A `present failure` is an `IDXGISwapChain::Present` error other than a removed
or reset device, which ends the run in `VibeRant_FinishUpdate` as before. A second line, `GPU %.2f ms per frame
(worst %.2f, %d timed)`, is the GPU time of the frames' commands, from timestamps around each frame's work.

The first three frames also trace their steps (`  copied the screen to back buffer N, submitted as fence F;
waited X ms for the frame latency object; presented with sync interval S (0x..., X ms)`).

### When taking the window fails

The log names the step that failed during window takeover.

```
VibeRant D3D: couldn't take the window from ANGLE (<step> failed: 0x...); VibeRant D3D can't present, so the game falls back to Software
```

ANGLE gets the window and its context back, and `VibeRant_Init` returns false.
The engine handles the Legacy GL library error by switching to Software. Switching to
VibeRant D3D again tries to take the window again. If the take only happens with the first frame (the `-opengl`
launch above), there is nothing to fall back from, and the game stops with `Direct3D 12 couldn't take the game
window from ANGLE (<step>)`, naming the same step, and a suggestion to set Renderer to Software.

The game also stops if it can't show frames after a swap-chain resize fails (`Direct3D 12 couldn't resize its swap chain to WxH`), or if ANGLE can't get the window back
after the retries (`Direct3D 12 couldn't give the game window back to ANGLE`, which asks for a restart).

The WARP harness stubs presentation and has no ANGLE or window, so no scene covers this.

## GPU wait timeouts

When the GPU doesn't reach a fence in time (and the device wasn't removed), the driver logs
`gave up waiting for <what> ...`. The first such timeout in a run also logs DRED breadcrumbs, when available.

```
VibeRant D3D: GPU wait for <what> timed out after N ms; breadcrumbs:
VibeRant D3D: DRED: list '...' on '...' stopped after N of M operations
...
```

The driver uses the same DRED auto-breadcrumb dump as for device removal to show where the GPU stopped,
even if the device survives the hang. Later timeouts log only the stall warning. Without DRED, which
Xbox may not provide, the first timeout also logs only the warning.

## Running without a GPU: `-warp` and `-d3ddebug` (desktop builds)

Pass these startup options on the command line when running desktop builds without a GPU, such as on CI.
Neither option appears in a menu. Each takes effect only when passed. UWP builds ignore both.

- `-warp` makes the driver use a software device in place of the first hardware one: Windows' WARP adapter for
  VibeRant D3D (the log says `using WARP for testing`), a CPU Vulkan device (lavapipe) for VibeRant Vulkan. If there is
  none, the renderer fails to start, and the game falls back to Software and says so, as it does for any other renderer
  that can't start. Without the option a software device is never chosen.
- `-d3ddebug` turns on the Direct3D 12 debug layer before the device is made (`D3D12 debug layer enabled`), and every
  submission then copies the debug layer's messages of error severity or worse to the log as
  `D3D12 debug ERROR: <message>`, counting each as a driver error. The layer is the "Graphics Tools" optional feature
  of Windows; if it isn't installed the renderer fails to start with `D3D12 debug layer unavailable`. With
  VibeRant Vulkan the option means what `-vkdebug` does, Vulkan's validation layers, so one command line can ask for
  validation of either backend. The layer makes the game several times slower.

The map sweep (`tests/README.md`, "Automated map sweeps") plays VibeRant D3D with `-d3d12 -warp -d3ddebug` on a hosted
Windows runner, and fails a map whose log has a `D3D12 debug ERROR` line.

## Tests

The `surface-probe` WARP scene covers the probe (see [tests/README.md](../tests/README.md)). The other
features, including the swap chain and its hand-offs with ANGLE, are untested off Xbox. The two startup options are
exercised by the map sweep on a hosted runner; the WARP harness makes its device without them.
