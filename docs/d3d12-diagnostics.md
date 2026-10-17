# Direct3D 12 diagnostics

The Direct3D 12 driver has the former Direct3D 11 driver's pixel history,
plus two tools of its own for presenting and GPU hangs.

## Pixel history: `gr_surfaceprobe`

`gr_surfaceprobe [x_percent y_percent]` (default `15 35`) works as it did under the former Direct3D 11 driver and
logs the same lines, so existing tooling and readers still work:

```
surface probe: begin pixel=(x,y) screen=WxH format=N
surface probe: sample=i raw=0x%08X kind=... source=... texture=... name=... flags=0x... shader=N mask=N depth=(a,b,c) alpha=0.000 vertices=N
surface probe: sample=i v0=(x,z,y;s,t)
surface probe: end draws=N color_changes=N rows_omitted=N overflow=0
```

`raw` is the pixel as a little-endian RGBA8 DWORD. A `sample=` row appears for each draw that changed the
pixel's color (the first 256), with up to eight `v` rows for polygon and batch draws.

How the driver takes its samples differs from the former Direct3D 11 driver's atlas:

- After each polygon, indexed batch, sky loop, model mesh or color clear, it transitions the screen to
  `COPY_SOURCE`, copies the one pixel into slot *i* of a single readback buffer with `CopyTextureRegion`, and
  transitions back. A slot is 512 bytes, the placed-footprint alignment, with a 256-byte row pitch, so
  8,192 slots take a 4 MB buffer.
- At the end of the world pass it submits (counted as a `readback` submission), waits up to two
  seconds, maps the used slots once, logs, and releases the buffer through `Core_Retire`.
- While no probe is requested, nothing is copied, allocated or mapped.
- The requested frame stalls on the GPU. Don't read its timings as a benchmark.

## Presenting: the swap chain

The driver presents with a swap chain of its own on the window, with no ANGLE in between. A CoreWindow takes
one swap chain at a time, so the driver takes the window from ANGLE: ANGLE draws to a 1x1 pbuffer, so that it
lets go of the window's back buffer, and its window surface is destroyed (SDL's `SDL_WinRTReleaseGLSurface`,
from `patches/sdl-egl-surface-handoff.patch` in ringracers-uwp). Then each frame is copied into the next of three
back buffers and presented, with `vid_wait` as the sync interval, and the swap chain's frame latency (2)
enforced by waiting on its waitable object before the copy. Frames whose aspect ratio isn't the window's are
letterboxed with black bars. There is nothing to configure; the driver is built only for UWP.

### When the window is taken

- **When Legacy GL starts.** `D3D_Init` takes the window every time the driver starts or the player switches
  to Legacy GL, with ANGLE's context current. The swap chain starts at the video mode's size, and is resized
  when a frame's size differs.
- **With the first frame, if no ANGLE context was current at start** (an `-opengl` launch starts the
  driver before the window exists). The log says `no ANGLE context is current, so the window can't be taken from
  it yet` until it can.
- **Software gets it back.** When the player switches to the Software renderer, the window goes back to ANGLE
  (`SDL_WinRTRestoreGLSurface`, retried up to three more times 50 ms apart, since DXGI can take a moment to
  release the CoreWindow), and ANGLE's context is made current again.

While the swap chain has the window, ANGLE has no window surface and no context current, so no GL call is
made; ANGLE's swap interval doesn't matter, as `Present`'s sync interval is the vsync. The calls ANGLE sees are
the pbuffer, a clear, a flush and the surface hand-off when taking or giving back the window, and a flush of its
Direct3D 11 context after the take.

### Log lines

```
UWP D3D12: took the window from ANGLE: WxH swap chain, 3 buffers, frame latency 2 (X ms)
UWP D3D12: resized the swap chain from AxB to CxD
UWP D3D12: gave the window back to ANGLE (X ms)
```

and the perf line:

```
UWP perf:   D3D12: present: direct swap chain (3 buffers, latency 2); per frame copy %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures
```

`copy` is recording and submitting the frame's copy; `latency wait` is the time blocked on the waitable
object (a timeout is 500 ms); a `present failure` is an `IDXGISwapChain::Present` error other than a removed
or reset device, which ends the run in `D3D12_FinishUpdate` as before. A second line, `GPU %.2f ms per frame
(worst %.2f, %d timed)`, is the GPU time of the frames' commands, from timestamps around each frame's work.

The first three frames also trace their steps (`  copied the screen to back buffer N, submitted as fence F;
waited X ms for the frame latency object; presented with sync interval S (0x..., X ms)`).

### When taking the window fails

If a step of taking the window fails, the log names it:

```
UWP D3D12: couldn't take the window from ANGLE (<step> failed: 0x...); Legacy GL can't present, so the game falls back to Software
```

ANGLE gets the window and its context back, and the driver fails to start (`D3D_Init` returns false), so
the engine's existing handling of a Legacy GL library error switches to the Software renderer. Switching to
Legacy GL again tries to take the window again. If the take only happens with the first frame (the `-opengl`
launch above), there is nothing to fall back from, and the game stops with `Direct3D 12 couldn't take the game
window from ANGLE (<step>)`, naming the same step, and a suggestion to set Renderer to Software.

Two failures later on also stop the game with an error, since there is no other way to show frames: the swap
chain can't be resized (`Direct3D 12 couldn't resize its swap chain to WxH`), or ANGLE can't have the window back
even after the retries (`Direct3D 12 couldn't give the game window back to ANGLE`, which asks for a restart).

The WARP harness stubs presentation and has no ANGLE or window, so no scene covers this.

## GPU wait timeouts

When the GPU doesn't reach a fence in time (and the device wasn't removed), the driver already logs
`gave up waiting for <what> ...`. The first such timeout in a run now follows it with

```
UWP D3D12: GPU wait for <what> timed out after N ms; breadcrumbs:
UWP D3D12: DRED: list '...' on '...' stopped after N of M operations
...
```

using the same DRED auto-breadcrumb dump as a device removal, so a hang that never removes the device
still shows which command list and operation the GPU stopped on. Later timeouts log only the stall
warning. The dump needs DRED, which Xbox may not provide; then nothing extra is logged.

## Tests

The `surface-probe` WARP scene covers the probe (see [tests/README.md](../tests/README.md)). The other
features, including the swap chain and its hand-offs with ANGLE, are untested off Xbox.
