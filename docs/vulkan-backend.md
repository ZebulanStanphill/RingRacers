# VibeRant Vulkan

VibeRant Vulkan runs the shared VibeRant driver (`src/hardware/r_viberant`) on Vulkan. It provides hardware
rendering on platforms without Direct3D 12 (Linux) and a second choice on Windows. Code above the device is
API-independent, so both renderers draw the same picture by design and share driver fixes.

## How

The driver uses about 70 methods across a dozen Direct3D 12 interfaces. `src/hardware/r_viberant/vulkan/`
("vk12") implements those interfaces (`ID3D12Device`, `ID3D12GraphicsCommandList`, `ID3D12Resource`, ...) on Vulkan.
`Core_Init` makes either a Direct3D 12 or vk12 device. The rest of the driver calls the same interfaces. `vk12_bases.h`, generated from Microsoft's header by `gen_bases.py`, defines every method of
those interfaces as "not implemented". vk12 overrides the methods the driver calls and logs calls to
unimplemented methods.

The driver uses `Core_Vulkan()` for the backend differences below.

- **Device creation** (`viberant_core.cpp`): `rr_vk12::CreateDevice` instead of `D3D12CreateDevice`. Vulkan is loaded at
  run time (volk). Without Vulkan, the renderer fails to start and the game falls back to Software.
- **Waiting for the GPU** (`Core_Wait`): vk12 waits on its Vulkan fence and has no Win32 event to set.
- **Shaders** (`viberant_pipeline.cpp`): the same `shaders.hlsl`, compiled to SPIR-V by glslang's HLSL front end at build
  time (`r_vulkan_shaders/*.h`, arrays named `g_<entry>_spv`) instead of to DXBC by fxc. Add-on shaders and the CRT
  screen effects take the existing GLSL to SPIR-V to HLSL path and then HLSL to SPIR-V with glslang at run time
  (`rr_vk12::CompileHlsl`) instead of `D3DCompile`.
- **The root signature** (`r_viberant.cpp`): vk12 has `shaders.hlsl`'s `RR_ROOT_SIGNATURE` built in (`kRoot`) and takes
  a token in place of Direct3D's serialized one.
- **Presentation**: `vulkan/vk_present.cpp` is a third implementation of `viberant_present.h`, with a `VkSwapchainKHR`
  whose images are wrapped as `ID3D12Resource`s, so the driver's frame placement draws into them unchanged.
- **Headers**: Windows builds use the Windows SDK's `d3d12.h`. Other platforms use Microsoft's MIT-licensed
  DirectX-Headers (the same declarations) and `vulkan/vk12_win.h`, which stands in for the few Win32 calls the driver
  makes and for `D3DCompile`.

Engine code uses `HWR_VibeRant()` to check whether VibeRant is drawing, on either backend. `HAVE_VIBERANT`
is defined whenever the driver is built; `HAVE_VIBERANT_D3D` and `HAVE_VIBERANT_VULKAN` say which backends are. A Windows
desktop build has both in one program, and the Renderer setting (or `-d3d12` and `-vulkan`) picks one per run.

## Mapping

| Direct3D 12 | vk12 |
| --- | --- |
| clip space, viewport | a viewport of negative height (`VK_KHR_maintenance1`, core in Vulkan 1.1), which makes clip space, winding, and `SV_Position` Direct3D's; depth is 0 to 1 in both |
| `SV_Position.w` in pixel shaders | glslang's `--hlsl-dx-position-w` |
| linking stages and vertex buffers by semantic | SPIR-V links by location number, so the shaders are compiled with their semantics kept and `PatchShader` gives each input and output the location its semantic stands for, the same in every stage (`spirv_shader.cpp`, which knows nothing of Vulkan: the root signature's layout, `PatchShader`, and the run-time HLSL compiler are shared with any other backend that takes SPIR-V) |
| the root signature: 6 root constant buffers, 11 tables of one texture, 2 tables of one sampler | one descriptor set: dynamic uniform buffers b0 to b5 at bindings 0 to 5, sampled images t0 to t10 at 16 to 26, samplers s0 and s1 at 32 and 33 |
| a root constant buffer named by GPU virtual address | each buffer gets a made-up address range; the address resolves to a buffer and an offset, which is bound as a dynamic offset. The range bound is the size of the shader's own uniform block, read from the SPIR-V |
| descriptor heap slots | records of a view or a sampler; descriptor sets are written when a draw needs one that differs, from pools that belong to the command allocator and are reset with it |
| views | kept by their resource and destroyed with it, since the driver rewrites view slots while recorded commands still use the old views |
| a null view | a 1x1 (or 1x1x1) image of the view's format, cleared to zero |
| resource states and `ResourceBarrier` | image layouts and `vkCmdPipelineBarrier`; buffers need none. A new image reaches the layout of its initial state before the next command that could use it |
| `OMSetRenderTargets`, `Clear*View`, draws | render passes that load and store everything, begun at the first draw or clear after the targets are set and ended by whatever can't be in one (a copy, a layout change, other targets). Clears are `vkCmdClearAttachments` |
| one pipeline state for lists and strips and any targets | a Vulkan pipeline per topology and target formats, the usual ones made with the pipeline state so that the driver's prewarming still compiles ahead of time |
| committed and placed resources, heaps | a `VkDeviceMemory` per committed resource or heap; a placed resource binds at its offset, with the size and alignment Vulkan reports through `GetResourceAllocationInfo` |
| upload and readback heaps, `Map` | host-visible memory, mapped for the resource's life |
| fences, `Signal`, waits | a value that completes with the `VkFence` of the submission it was signaled after |
| `D24_UNORM_S8_UINT` | the first the device can render to of `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT` |
| `IDXGIAdapter3::QueryVideoMemoryInfo` | `VK_EXT_memory_budget`, off Windows |
| releasing objects | vk12 destroys on the last `Release`: the driver already keeps what the GPU may still use until its fence completes |

## The window

The Vulkan presenter creates a matching second window because the OpenGL window that presents Software frames
cannot also present Vulkan frames. It hides the first window and sets `sdlmain.h`'s `window` to the second. The
video code sizes that window, makes it fullscreen, and takes input from it. When Software takes over, the first window comes back as the second one was. Where VibeRant
Vulkan may be the renderer, the OpenGL window is made hidden and only shown once a renderer that presents on it is
the one the game runs with (`I_ShowDeferredWindow`), so starting the game shows one window.

The swap chain is always the window's size, and the driver's placement draws each frame into it: at Screen
Scaling's Integer or 1:1 rectangle, or scaled with linear filtering to the largest rectangle of the frame's aspect
ratio, as the Direct3D 12 desktop presenter does.

## What can differ from VibeRant D3D

The driver's logic, shaders, textures, and states are the same, and so is everything the engine does around them.
The shader compilers can round the last bit differently, and Vulkan uses 32-bit float depth on devices without
24-bit depth. A pixel whose texture coordinate falls on the boundary
between two texels can therefore take the other texel. Two Direct3D 12 devices differ from each other the same way.

Where two surfaces lie in exactly the same plane, the same rounding decides which of them shows, so such a place can
look different on the two backends, as it can on two Direct3D 12 devices. Neither result is definitive. Software draws those surfaces in order, while the hardware renderers rely on the
depth buffer.

Measured on one PC (Radeon Vega 10, with the Windows build's two backends under Wine, where Direct3D 12 is itself
translated to Vulkan): of 1,002 frames from the 213 maps, the median frame differs in 0.08% of its pixels and the
worst in 1.4%. Four frames differ by more than texel edges in over 0.1% of their pixels: two of a fine texture far
away (Daytona Speedway), and two of surfaces in the same plane (a wall in Ice Paradise, a one-tic effect in Haunted
Ship). The wall in Ice Paradise is one Software doesn't draw, and VibeRant has stopped drawing it since the
measurement, so the two backends now agree there.

## Checking it

- **Validation layers**: `-vkdebug` turns on `VK_LAYER_KHRONOS_validation` (if installed) and logs up to 200
  messages as `Vulkan: validation error: ...` or `Vulkan: validation warning: ...`. Any message is a bug.
- **The driver test** (`tests/README.md`) runs the driver's pixel checks on Vulkan.
- **Frame comparison** (`tests/frame-compare`) plays the same replay with two renderers and compares screenshots
  taken at the same tics.
