# Custom shader smoke test

[D_ShaderSmoke_v1.pk3](D_ShaderSmoke_v1.pk3) is a 2 KB shader-only add-on for
Ring Racers 2.4, including this port's VibeRant D3D11 renderer. Its purpose is
to make custom shader selection unmistakable. Xbox visuals are **untested**.

## Install and compare on Xbox

1. Copy **the PK3 itself**, without extracting it, to
   `E:\ringracers\addons\D_ShaderSmoke_v1.pk3` on the game-data USB drive.
2. Launch the game, select **VibeRant D3D11**, and set
   **Options → Video → Advanced → Shaders** to **On**.
3. Load the file through the game's **Addons** menu. With a keyboard, the
   alternative console command is:
   `addfile "E:\ringracers\addons\D_ShaderSmoke_v1.pk3"`.
4. Start a single-player race on **Water Palace**, or any track for the first
   floors/walls check. Ordinary floors, ceilings and walls should have obvious
   **magenta/green checkers**; original texture details remain faintly visible.
   Ripple-enabled water surfaces should show smoothly moving **cyan/orange bands**.
5. At the same spot, change **Shaders** to **Ignore custom shaders**. The checkers
   and colored water bands should disappear; normal built-in rendering, including
   the port's water refraction, should return. Set **On** to restore the markers.
6. Quit and relaunch without loading the add-on to remove the test entirely.

Use a local single-player session for this first test. The file contains only
shader definitions and GLSL, with no Lua, SOC, textures, characters or maps.
Its entries use the engine's allowed cosmetic shader prefixes. An online server
can disallow custom shaders; other shader add-ons may override the same targets.

The water check requires a surface with ripple effects enabled and **Reduce VFX
Off**. Not every water-looking texture uses that shader. A valid custom water
shader deliberately replaces the built-in surface refraction effect. Underwater
screen distortion still uses its built-in postprocessing shader. Characters,
models, sky and HUD are not replaced by this pack.

The bright marker colors intentionally ignore normal sector lighting/tints and
brightmaps. They are diagnostic colors, not a lighting-parity test. Palette
Rendering may quantize them. Texture alpha and surface alpha are preserved;
masked fences should retain their holes and translucent surfaces should still
blend. Ignore custom shaders is the comparison mode; Shaders Off also disables
other effects and does not isolate custom shader selection.

## Report the result

Capture the same view with **On** and **Ignore custom shaders**, and record the
build/commit, track, Palette Rendering and Reduce VFX settings. The log should
include `custom shader 1 compiled`, `custom shader 2 compiled` and
`custom shader 7 compiled` (floor, wall and water), each for polygon, sky and
model layouts. Those messages confirm compilation; the images confirm dispatch.
Missing-source or translation/compilation errors are useful even if the markers
never appear. Every split-screen visual check remains untested.

## Sources and validation

`source/definitions.txt` becomes the archive's root `SHADERS` entry. The different
source filename avoids a collision with the `Shaders` directory on filesystems
that ignore filename case. GLSL sources are original test code under GPL-2.0-only;
their SPDX headers travel inside the package.

Rebuild deterministically using Python's standard library:

```sh
python3 tests/addons/shader-smoke/build.py
python3 tests/addons/shader-smoke/build.py --check
```

The native `custom_shader_pk3` regression reads the **packaged** entries, executes
the production `HWR_LoadCustomShadersFromFile` parser with archive I/O stubs, and
verifies all five stage assignments. It checks that water uses a built-in default
vertex stage, that other shader targets stay untouched, and that a missing archive
path is rejected. The actual GLSL adapter translates all nine target/layout pairs
to HLSL. Windows CI's `fxc` step compiles those 18 stages along with the existing
72 stages. The existing WARP rendering checks exercise the driver, but do not yet
render this pack's checker/band patterns; Xbox visuals and load cost remain untested.
