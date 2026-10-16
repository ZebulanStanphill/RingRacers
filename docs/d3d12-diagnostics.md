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

## Present path and synchronization: `gr_d3d12_present`

`gr_d3d12_present` is a saved console variable, `Auto` (the default), `Readback` or `CpuSync`, kept out
of the menu. The value is read once, when the path is selected at launch: set it in the console or
`ringconfig.cfg`, then restart.

### Direct swap chain (`Auto`)

By default, the driver presents with a swap chain of its own on the window, with no ANGLE in between. A
CoreWindow takes one swap chain at a time, so the first frame drawn takes the window from ANGLE: ANGLE
draws to a 1x1 pbuffer, so that it lets go of the window's back buffer, and its window surface is
destroyed (SDL's `SDL_WinRTReleaseGLSurface`, from `patches/sdl-egl-surface-handoff.patch` in
ringracers-uwp). Then each frame is copied into the next of three back buffers and presented, with
`vid_wait` as the sync interval, and the swap chain's frame latency (2) enforced by waiting on its
waitable object before the copy. The player sees the current frame, not the previous one.

When the player switches to the Software renderer, the window goes back to ANGLE (`SDL_WinRTRestoreGLSurface`,
retried up to three more times 50 ms apart, since DXGI can take a moment to release the CoreWindow); the
next frame Legacy GL draws takes it again.

The log shows:

```
UWP D3D12: present: direct swap chain (3 buffers, frame latency 2), taking the window from ANGLE with the first frame
UWP D3D12: took the window from ANGLE: WxH swap chain, 3 buffers, frame latency 2 (X ms)
UWP D3D12: resized the swap chain from AxB to CxD
UWP D3D12: gave the window back to ANGLE (X ms)
```

and the perf line replaces the interop one:

```
UWP perf:   D3D12: present: direct swap chain (3 buffers, latency 2); per frame copy %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures
```

`copy` is recording and submitting the frame's copy; `latency wait` is the time blocked on the waitable
object (a timeout is 500 ms); a `present failure` is an `IDXGISwapChain::Present` error other than a removed
or reset device, which ends the run in `D3D12_FinishUpdate` as before. The `GPU ... ms per frame` line
works as it does with interop.

If a step of taking the window fails, or the swap chain can't be resized, the log names it:

```
UWP D3D12: direct: couldn't take the window from ANGLE (<step> failed: 0x...); using the interop paths instead
```

and ANGLE gets the window and its context back. The driver then probes interop paths A, B and C (D if they
all fail) and presents that way for the rest of the run, never trying the swap chain again. If ANGLE can't
have the window back even after the retries, the game stops with an error that suggests `CpuSync`.

### Interop with ANGLE (`Readback`, `CpuSync` and the fallback)

`Readback` and `CpuSync` skip the swap chain (`direct swap chain not used (gr_d3d12_present ...)`) and
keep ANGLE in charge of the window, as before the swap chain existed. ANGLE draws each frame a frame
later, from one of four textures it shares with the driver. The driver probes ANGLE interop paths A, B
and C and falls back to the readback path D only if they all fail. `Readback` skips the probes and logs
`interop path D: forced by gr_d3d12_present`. This is how to test path D on hardware where another
path would work.

On paths A, B and C, `Auto` and `CpuSync` differ in how the Direct3D 12 queue and ANGLE's device are
synchronized. `Auto` uses two shared fences (`present sync: shared fences`): ANGLE waits for each frame's
copy, and the queue waits for ANGLE's last read of a presentation texture before copying into it again.
`CpuSync` keeps the older CPU waits instead, the copy's fence and an ANGLE event query per texture
(`present sync: cpu fence + ANGLE queries`), to compare the two; it's read when the presentation textures
are made. The `present:` perf line names the sync in use and ends with `GPU waits: copy N, read N`. If the
fences can't be made, or a call on them fails, the CPU waits take over and the log says why. Path D
always waits on the CPU.

The WARP harness stubs presentation and has no ANGLE or window, so no scene covers this variable.

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
features, including the direct swap chain, its hand-offs with ANGLE and the shared-fence present sync, are
untested off Xbox and Windows CI.
