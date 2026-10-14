# Direct3D 12 diagnostics

The Direct3D 12 driver (**Direct3D 12 (test)**, `gr_d3d12`) has the Direct3D 11 driver's pixel history,
plus two tools of its own for presenting and GPU hangs.

## Pixel history: `gr_surfaceprobe`

`gr_surfaceprobe [x_percent y_percent]` (default `15 35`) works as it does under Direct3D 11, and
logs the same lines, so existing tooling and readers work with either renderer:

```
surface probe: begin pixel=(x,y) screen=WxH format=N
surface probe: sample=i raw=0x%08X kind=... source=... texture=... name=... flags=0x... shader=N mask=N depth=(a,b,c) alpha=0.000 vertices=N
surface probe: sample=i v0=(x,z,y;s,t)
surface probe: end draws=N color_changes=N rows_omitted=N overflow=0
```

`raw` is the pixel as a little-endian RGBA8 DWORD. A `sample=` row appears for each draw that changed the
pixel's color (the first 256), with up to eight `v` rows for polygon and batch draws.

How the driver takes its samples differs from Direct3D 11's atlas:

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
of the menu. By default, the driver probes ANGLE interop paths A, B and C and falls back to the readback path D
only if they all fail. `Readback` skips the probes and logs `interop path D: forced by gr_d3d12_present`.
This is how to test path D on hardware where another path would work. The value is read once, when
the path is selected at launch: set it in the console or `ringconfig.cfg`, then restart.

On paths A, B and C, `Auto` synchronizes the Direct3D 12 queue and ANGLE's device on the GPU through
two shared fences (`present sync: shared fences`): ANGLE waits for each frame's copy, and the queue
waits for ANGLE's last read of a presentation texture before copying into it again. `CpuSync` keeps the
older CPU waits instead, the copy's fence and an ANGLE event query per texture (`present sync: cpu
fence + ANGLE queries`), to compare the two; it's read when the presentation textures are made, so
restart after changing it. The `present:` perf line names the sync in use and ends with `GPU waits:
copy N, read N`. If the fences can't be made, or a call on them fails, the CPU waits take over and the
log says why. Path D always waits on the CPU.

The WARP harness stubs presentation and has no ANGLE, so no scene covers this variable.

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
features, including the shared-fence present sync, are untested off Xbox and Windows CI.
