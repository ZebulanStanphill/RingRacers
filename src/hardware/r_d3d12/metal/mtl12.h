// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12.h
/// \brief The part of Direct3D 12 the VibeRant driver uses, on Metal
///
/// mtl12 implements ID3D12Device and the interfaces made from it with Apple's Metal, so that the driver (r_d3d12.cpp
/// and its parts) runs unchanged on macOS: VibeRant Metal. It covers what the driver calls and no more; a method the
/// driver doesn't use logs "not implemented" and fails. docs/metal-backend.md describes the mapping.
///
/// One device at a time, used from one thread, like the driver uses Direct3D 12 (pipeline states may be made on
/// another thread meanwhile, as the driver's prewarming does).
///
/// This header is C++: Metal's objects cross it as void pointers, so that the driver's C++ files can include it.

#ifndef RR_MTL12_H
#define RR_MTL12_H

// No Windows SDK: Direct3D 12's declarations from DirectX-Headers, and the stand-ins for the few Win32 calls the
// driver makes, which the Vulkan backend's header has without anything of Vulkan in it
#include "../vulkan/vk12_win.h"

namespace rr_mtl12
{

struct DeviceOptions
{
	// Has each command buffer say which of its passes failed, and writes what Metal's shader validation reports of
	// a command buffer to the log. Metal's validation itself is turned on from outside the program, with the
	// environment variables MTL_DEBUG_LAYER=1 (API validation) and MTL_SHADER_VALIDATION=1; the log's device line says
	// which of them the device was made with.
	bool validation = false;
	// Vulkan's backend can ask for a CPU device; Metal has none. Ignored, and here so that the two read alike.
	bool software = false;
};

// Makes the device and its queue. memory, if given, receives an adapter that answers QueryVideoMemoryInfo with the
// device's recommended working set and what it has allocated. Logs why it failed.
HRESULT CreateDevice(const DeviceOptions &options, ID3D12Device **device, IDXGIAdapter3 **memory);

// What the driver passes to CreateRootSignature in place of Direct3D's serialized root signature: mtl12 has the one
// layout of shaders.hlsl's RR_ROOT_SIGNATURE built in (../spirv_shader.h's kRoot)
extern const unsigned char kRootSignature[16];

// Blocks until the GPU has finished everything up to the fence's value, or ms pass. False on a timeout or a lost device.
bool WaitFence(ID3D12Fence *fence, UINT64 value, DWORD ms);

// "name, Metal" of the device in use
const char *DeviceDescription();

// How many errors Metal has reported through command buffers since the program began: a command buffer that failed,
// and each fault its shader validation logged. A failure of Metal's API validation isn't among them: that one
// stops the program where it happens.
uint64_t ValidationErrors();

// True once a command buffer has failed because the device was removed; the device then reports itself removed
bool DeviceLost();

// For the driver test: what mtl12 holds that should go when the driver lets go of it
struct TestCounts
{
	size_t shaders; // compiled Metal functions in the shader cache
};
TestCounts CountsForTest();

// For the presenter, which owns the layer and its drawables

// The id<MTLDevice>, not retained; null without a device
void *Device();

// An id<MTLTexture> (a drawable's) as a resource the driver can draw into and copy to. Its state is
// D3D12_RESOURCE_STATE_PRESENT. The resource retains the texture until its last Release.
HRESULT WrapTexture(void *texture, ID3D12Resource **resource);

// The next ExecuteCommandLists presents this id<CAMetalDrawable> with its last command buffer (one of its own, if it
// is given no lists). Applies to that one submission; null takes it back.
void PresentWithNextSubmit(void *drawable);

} // namespace rr_mtl12

#endif
