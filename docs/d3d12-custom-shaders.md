# Direct3D 12 add-on shaders

Add-on shaders from `SHADERS` lumps are translated at load time to Shader Model 5.1, and each
translated program must build a pipeline state object (PSO) before it is used.

## Translation and compilation

- Legacy GL's GLSL goes through the pinned glslang and SPIRV-Cross translators to HLSL.
  The HLSL is then compiled with `D3DCompile` for the 5_1 profiles (`vs_5_1` and `ps_5_1`).
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

## Screen effect

Options > Video > Screen Effect (`cv_scr_effect`) is 0 Nearest, 1 Sharp Bilinear, 2 SalCRT or 3 SalCRT Sharp.

- **Nearest and Sharp Bilinear** only choose how the RHI path scales its smaller backbuffer up to the window.
  VibeRant D3D draws the frame at the frame's own size, so there is nothing for them to scale and both draw the plain
  frame. The frame is then placed on the screen by Screen Scaling (below), which samples it point by point.
- **SalCRT and SalCRT Sharp** run the game's CRT fragment shader over the finished frame as a last pass, after
  palette rendering's last step and before the present. The pass draws the frame (the palette step's texture
  with palette rendering, otherwise the screen) into a render target of the same size, which is then the frame that
  is presented. Screenshots and other captures read the screen or the palette step, so they don't have the effect.

The CRT shaders are game data in `shaders.pk3` and aren't in this repository. The driver reads them with
`W_ReadShader` when the mode is first used, as the RHI GL backend does: `rhi_glsllist_crt_fragment.txt`
(or `rhi_glsllist_crtsharp_fragment.txt`) lists the sources, `rhi_glsl_lib_versioncompat` and then
`rhi_glsl_fragment_crt` (or `rhi_glsl_fragment_crtsharp`), which are concatenated in order. `TranslateScreenEffect`
puts `#version 450` and `#define ENABLE_VA_TEXCOORD0` (the define the RHI gives the program) in front, so that the
compatibility macros choose their Vulkan-style declarations; the GL backend itself uses version 120. The RHI declares
every uniform with `UNIFORM(set, binding)`, which Vulkan GLSL allows only on resources, so after the compatibility
library `UNIFORM` is redefined to keep `layout(set = 0, binding = n)` for samplers (set 0) and drop it for the loose
uniforms (sets 1 and 2), which glslang's relaxed Vulkan rules gather into one constant buffer. The result goes
through SPIRV-Cross to Shader Model 5.1 HLSL and `D3DCompile`, as add-on shaders do, with
`D3DCOMPILE_AVOID_FLOW_CONTROL` because the shader samples inside loops, plus
`D3DCOMPILE_ALL_RESOURCES_BOUND`, which Shader Model 5.1 requires alongside it.

The pass uses the shared root signature, with a vertex shader of its own that covers the screen with a triangle:
`v_texcoord0` runs from 0 at the top left of the frame, like the texture, and `v_color` is white.

| Shader                         | Bound as                                                        |
|--------------------------------|------------------------------------------------------------------|
| `s_sampler0`                   | `t0`/`s0`: the frame, linear filter, clamped                    |
| `s_sampler1`                   | `t1`/`s1`: the 12x4 dot pattern, linear filter, repeating       |
| `u_sampler0_size`              | b0, at the offset the translator reports: the frame's size in pixels, which is also the target's |
| `u_projection`, `u_modelview`, `u_texcoord0_transform` | b0, left zero (the CRT shader doesn't read them) |

The dot pattern's bytes are the RHI's, from `src/hwr2/blit_rect.cpp`. Each mode builds its pipeline state the first
time it is used and keeps it; nothing is rebuilt per frame, and the render target follows the frame's size.

If a lump is missing, translation or compilation fails, or a resource or the pipeline can't be created, the driver
logs the reason once and shows the plain frame for that mode for the rest of the session. It never raises an
error for this, and the other mode is tried on its own. The per-frame performance lines count the passes and their CPU time.

## Screen scaling

Options > Video > Screen Scaling (`cv_scr_scaling`, placement in `src/screen_scaling.hpp`) is 0 Fit, 1 Integer or
2 1:1. It places the frame (`vid.width` by `vid.height`) on the screen (`vid.realwidth` by `vid.realheight`, or the
size `uwp_GetScreenSize` reports). It is the last step, after palette rendering's step and the screen effect, which
always run at the frame's size: the effect's output is what gets placed.

- **Fit** is the behavior described above: the swap chain is the frame's size, `Present_Frame` copies the frame into
  it, and the system's `DXGI_SCALING_ASPECT_RATIO_STRETCH` fits it to the window, keeping the aspect ratio.
- **Integer** scales the frame by the largest whole number k with k times its width and height inside the screen,
  centered with floor division (the extra pixel of an odd remainder is at the right and the bottom), with black around it.
- **1:1** is the same with k fixed at 1.
- A frame larger than the screen in either dimension (so k would be 0) is Fit for both.

For Integer and 1:1 the swap chain is the size of the screen, so the system scales nothing. `Present_Frame` takes
the next back buffer from PRESENT to RENDER_TARGET and calls the driver's `PlaceFrame` with a view of it. That
clears the buffer to black, moves the presented frame (the screen, the palette step's texture or the effect's
target, in RENDER_TARGET) to PIXEL_SHADER_RESOURCE, draws a triangle with a viewport and scissor of the placement's
rectangle, and restores the frame's state. The pixel shader point-samples the frame, and the vertex shader's texture
coordinates run from 0 to 1 across the viewport, so each frame texel covers k by k pixels exactly. At k = 1 that is
a copy, drawn the same way so there is one path to check. Both shaders are compiled at run time with `D3DCompile` (as
the screen effect's vertex shader is) and use the shared root signature, with a point sampler of their own. The back
buffer then returns to PRESENT. Changing the mode or the screen's size resizes the swap chain
(`ResizeSwapChain`), which makes the buffers and their render target views again; the log says how frames are presented
whenever that changes. If the pipeline can't be made, the driver logs why once and presents with Fit for the rest of the
session.

The software and Legacy GL (RHI) path draws the same placement through its blit rectangle, with the render pass's black
clear around it (`i_video_common.cpp`), and Scale and Screen X/Y then act within that rectangle.

## Tests

- `tests/custom-shaders.cpp` (native) emits 79 general Shader Model 5.1 stages and checks their register
  slots and std140 member offsets, with every register in space 0. Windows CI compiles these and the
  smoke-test add-on's 19 stages with `fxc` (`vs_5_1` and `ps_5_1`), for 98 total.
- `tests/d3d12-driver.cpp` (WARP) covers the driver path:
  - `custom-shader` draws a fixed color from a custom fragment shader.
  - `custom-shader-clean` checks that cleaning the shaders returns to the built-in color.
  - `custom-shader-invalid` checks that invalid GLSL falls back to the built-in shader.
  - `custom-shader-nocustom` is a negative control: with Ignore custom shaders set, the
    custom color must not appear.

- The screen effect has stand-in shaders (`tests/screen-effect-fixtures.h`, with the RHI's macros but not the game's
  code). `custom-shaders.cpp` translates them and checks the registers, the semantics and the constant buffer's
  offsets, and rejects broken, incomplete and oversized sources. The WARP scenes `screen-effect-off`, `screen-effect`,
  `screen-effect-fallback` and `screen-effect-palette` serve them through a stub `W_ReadShader`, check the presented
  pixels (the effect's output on a frame of four colors, so a flipped frame shows; the plain frame with the effect off;
  the palette step's texture as the source), that a built mode is built once, and that a mode whose shader doesn't
  translate presents the plain frame and logs once.
- Screen Scaling's calculation is the native `screen_scaling` test (Fit, Integer, 1:1, odd remainders, wider and taller
  screens, and the fallbacks when the frame is larger than the screen in either dimension, plus a sweep of invariants).
  The WARP scenes `scaling-integer`, `scaling-1to1`, `scaling-fallback` and `scaling-effect` run the driver's `PlaceFrame`
  on a screen-sized target through `D3D12_ReadPlacedRectForTest` (the target is filled with magenta first): every pixel
  of the placed rectangle is checked against the frame's pixel k times smaller, every pixel outside it against black, at
  exact fits, odd remainders, k of 1, 2 and 3, and wider and taller screens; Fit and frames larger than the screen place
  nothing; and with SalCRT on, the placed pixels are the effect's output.

The WARP scenes run in Windows CI, and add-on shaders work on Xbox. The screen effect and Screen Scaling are untested on Xbox; so is the swap chain's own behavior with them
(`Present_Frame` is a stub in the WARP harness). See
[the test README](../tests/README.md).
