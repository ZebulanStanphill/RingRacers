# Direct3D 12 add-on shaders

Add-on shaders from `SHADERS` lumps reach the Direct3D 12 renderer through the same
load-time translation as the Direct3D 11 renderer, with two differences: the compiler
targets Shader Model 5.1, and each translated program must build a pipeline state object
(PSO) before it is used.

## Translation and compilation

- Legacy GL's GLSL goes through the pinned glslang and SPIRV-Cross translators to HLSL.
  The HLSL is then compiled with `D3DCompile` for the 5_1 profiles (`vs_5_1` and `ps_5_1`).
- The Direct3D 11 renderer still uses Shader Model 4 (`vs_4_0` and `ps_4_0`), which is a
  separate compiler target. The Shader Model 4 limits described in [CHANGES.md](../CHANGES.md)
  don't apply to the Direct3D 12 path.
- Every polygon, sky and model layout is translated and checked, not only the polygon one.

## Load-time PSO validation

Before a custom program is used, the renderer creates its PSO. A program that fails to compile,
translate or create a PSO is rejected at load time, and that base shader type falls back to the
built-in shader. Translation and PSO validation failures log the affected type and built-in fallback;
HLSL compilation failures retain the compiler diagnostic. A bad add-on doesn't disable
the rest of the renderer.

Custom PSOs encode the base shader type above the built-in index ranges:
`vs = NUMVS + type`, `ps = NUMPS + 2*type + refraction`. Their keys also include a
nonzero generation. Recompilation and cleanup retire all custom PSOs through the
GPU fence and reset binding caches; built-in keys retain generation zero and the
prewarm worker uses only built-in bytecode. The render-target height at b4 is cached
per frame/height and ring generation, reserved with draw constants to survive wraps,
and rebound after root resets.

## Fallbacks

- **Missing stage:** Legacy GL's shared built-in GLSL source is used.
- **Invalid add-on:** the built-in shader is used for that base shader type.
- **Ignore custom shaders** (`HWD_SHADEROPTION_NOCUSTOM`): the built-in shaders are used,
  and custom programs are never drawn.
- **Sprites:** custom sprite stages also replace `SHADER_SPRITECLIPHACK`, so clipped sprites
  use the replacement too.

## Water refraction

Custom water keeps the built-in refraction only when the program and its composite variant
both succeed:

- The water surface's custom fragment shader replaces its color. A second polygon pixel
  variant alpha-tests that result and composites it over the captured, distorted scene
  with the built-in blend equations.
- Only the water surface gets this composite. Other surfaces with custom shaders draw
  normally.
- A custom water vertex stage can move the surface, so the capture covers the whole current
  view instead of the CPU-projected bounds.
- If the variant fails, the scene capture fails, or Reduce VFX is on, custom water draws
  without refraction, using normal framebuffer blending. The output is unrefracted in that
  case.

Underwater and heat postprocessing always keeps its built-in shader.

## Tests

- `tests/custom-shaders.cpp` (native) emits 78 general stages for each shader model and
  checks identical register slots and std140 member offsets, with explicit space 0 for SM5.1.
  The PK3 test adds 19 SM4 stages, retaining the existing total of 97 SM4 stages. Windows CI compiles the
  SM5.1 and SM4 stages with `fxc` under both profiles.
- `tests/d3d12-driver.cpp` (WARP) covers the driver path:
  - `custom-shader` draws a fixed color from a custom fragment shader.
  - `custom-shader-clean` checks that cleaning the shaders returns to the built-in color.
  - `custom-shader-invalid` checks that invalid GLSL falls back to the built-in shader.
  - `custom-shader-nocustom` is a negative control: with Ignore custom shaders set, the
    custom color must not appear.

Xbox and Windows runs of these tests and their visual results are untested. See
[the test README](../tests/README.md).
