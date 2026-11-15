# VibeRant Vulkan

VibeRant Vulkan is the VibeRant driver (`src/hardware/r_d3d12`) running on Vulkan instead of Direct3D 12: the
hardware renderer for platforms without Direct3D 12 (Linux), and a second choice on Windows. It is the same driver
code. Nothing above the device knows which API is underneath, so the two renderers draw the same picture by
construction, and a fix to the driver is a fix to both.

## How

The driver uses a small part of Direct3D 12: about 70 methods of a dozen interfaces. `src/hardware/r_d3d12/vulkan/`
("vk12") implements those interfaces (`ID3D12Device`, `ID3D12GraphicsCommandList`, `ID3D12Resource`, ...) on Vulkan.
`Core_Init` makes either a real Direct3D 12 device or a vk12 one; everything else in the driver calls the same
interfaces either way. `vk12_bases.h`, generated from Microsoft's header by `gen_bases.py`, defines every method of
those interfaces as "not implemented", and vk12's classes override the ones the driver calls. A method the driver
starts to use and vk12 lacks says so in the log.

What differs per backend, and where the driver asks (`Core_Vulkan()`):

- **Device creation** (`d3d12_core.cpp`): `rr_vk12::CreateDevice` instead of `D3D12CreateDevice`. Vulkan is loaded at
  run time (volk), so a system without it only makes the renderer fail to start, and the game falls back to Software.
- **Waiting for the GPU** (`Core_Wait`): vk12's fence waits on Vulkan's own; it has no Win32 event to set.
- **Shaders** (`d3d12_pipeline.cpp`): the same `shaders.hlsl`, compiled to SPIR-V by glslang's HLSL front end at build
  time (`r_vulkan_shaders/*.h`, arrays named `g_<entry>_spv`) instead of to DXBC by fxc. Add-on shaders and the CRT
  screen effects take the existing GLSL to SPIR-V to HLSL path and then HLSL to SPIR-V with glslang at run time
  (`rr_vk12::CompileHlsl`) instead of `D3DCompile`.
- **The root signature** (`r_d3d12.cpp`): vk12 has `shaders.hlsl`'s `RR_ROOT_SIGNATURE` built in (`kRoot`) and takes
  a token in place of Direct3D's serialized one.
- **Presentation**: `vulkan/vk_present.cpp` is a third implementation of `d3d12_present.h`, with a `VkSwapchainKHR`
  whose images are wrapped as `ID3D12Resource`s, so the driver's frame placement draws into them unchanged.
- **Headers**: Windows builds use the Windows SDK's `d3d12.h`. Other platforms use Microsoft's MIT-licensed
  DirectX-Headers (the same declarations) and `vulkan/vk12_win.h`, which stands in for the few Win32 calls the driver
  makes and for `D3DCompile`.

Engine code keeps asking `HWR_D3D12()`: it means "the VibeRant driver is drawing", on either backend. `HAVE_D3D12`
is defined whenever the driver is built; `HAVE_D3D12_NATIVE` and `HAVE_VULKAN` say which backends are. A Windows
desktop build has both in one program, and the Renderer setting (or `-d3d12` and `-vulkan`) picks one per run.

## Mapping

| Direct3D 12 | vk12 |
| --- | --- |
| clip space, viewport | a viewport of negative height (`VK_KHR_maintenance1`, core in Vulkan 1.1), which makes clip space, winding, and `SV_Position` Direct3D's; depth is 0 to 1 in both |
| `SV_Position.w` in pixel shaders | glslang's `--hlsl-dx-position-w` |
| linking stages and vertex buffers by semantic | SPIR-V links by location number, so the shaders are compiled with their semantics kept and `PatchShader` gives each input and output the location its semantic stands for, the same in every stage (`vk12_spirv.cpp`) |
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

The game's window is made for OpenGL, which shows the Software renderer's frames, and a window can't be both
OpenGL's and Vulkan's. So the presenter makes a second window like the first for Vulkan, hides the first, and makes
the second the game's window (`sdlmain.h`'s `window`), which is what the video code sizes, makes fullscreen, and
takes input from. When Software takes over, the first window comes back as the second one was. Where VibeRant
Vulkan may be the renderer, the OpenGL window is made hidden and only shown once a renderer that presents on it is
the one the game runs with (`I_ShowDeferredWindow`), so starting the game shows one window.

The swap chain is always the window's size, and the driver's placement draws each frame into it: at Screen
Scaling's Integer or 1:1 rectangle, or scaled with linear filtering to the largest rectangle of the frame's aspect
ratio, as the Direct3D 12 desktop presenter does.

## What can differ from VibeRant D3D

The driver's logic, shaders, textures, and states are the same, and so is everything the engine does around them.
What is left is arithmetic: the two backends' shader compilers don't round the same way in the last bit, and a
device without 24-bit depth uses 32-bit float depth here. A pixel whose texture coordinate falls on the boundary
between two texels can therefore take the other texel. Two Direct3D 12 devices differ from each other the same way.

Where two surfaces lie in exactly the same plane, the same rounding decides which of them shows, so such a place can
look different on the two backends, as it can on two Direct3D 12 devices. Neither is right: Software draws those
surfaces in an order, and the hardware renderers leave it to the depth buffer.

Measured on one PC (Radeon Vega 10, with the Windows build's two backends under Wine, where Direct3D 12 is itself
translated to Vulkan): of 1,002 frames from the 213 maps, the median frame differs in 0.08% of its pixels and the
worst in 1.4%. Four frames differ by more than texel edges in over 0.1% of their pixels: two of a fine texture far
away (Daytona Speedway), and two of surfaces in the same plane (a wall in Ice Paradise, a one-tic effect in Haunted
Ship).

## Checking it

- **Validation layers**: `-vkdebug` turns on `VK_LAYER_KHRONOS_validation` (if installed) and writes its messages
  to the log as `Vulkan: validation error: ...`. Any message is a bug.
- **The driver test** (`tests/README.md`) runs the driver's pixel checks on Vulkan.
- **Frame comparison** (`tests/frame-compare`) plays the same replay with two renderers and compares screenshots
  taken at the same tics.
