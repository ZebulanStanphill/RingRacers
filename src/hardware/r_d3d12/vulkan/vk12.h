// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12.h
/// \brief The part of Direct3D 12 the VibeRant driver uses, on Vulkan
///
/// vk12 implements ID3D12Device and the interfaces made from it with Vulkan, so that the driver (r_d3d12.cpp and its
/// parts) runs unchanged on platforms without Direct3D 12: VibeRant Vulkan. It covers what the driver calls and no
/// more; a method the driver doesn't use logs "not implemented" once and fails. docs/vulkan-backend.md describes
/// the mapping.
///
/// One device at a time, used from one thread, like the driver uses Direct3D 12.

#ifndef RR_VK12_H
#define RR_VK12_H

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#else
#include "vk12_win.h"
#endif

#include <volk.h>

#include "../spirv_shader.h"

namespace rr_vk12
{

struct DeviceOptions
{
	bool validation = false; // VK_LAYER_KHRONOS_validation and debug messages in the log, if the layer is installed
	bool software = false; // a CPU implementation (lavapipe), for tests; otherwise the first discrete, then integrated, GPU
};

// Loads Vulkan and makes the instance and the device. memory, if given, receives an adapter that answers
// QueryVideoMemoryInfo, or null where the device can't say (no VK_EXT_memory_budget). Logs why it failed.
HRESULT CreateDevice(const DeviceOptions &options, ID3D12Device **device, IDXGIAdapter3 **memory);

// What the driver passes to CreateRootSignature in place of Direct3D's serialized root signature: vk12 has the one
// layout of shaders.hlsl's RR_ROOT_SIGNATURE built in (see vk12_pipeline.cpp)
extern const unsigned char kRootSignature[16];

// Blocks until the GPU has finished everything up to the fence's value, or ms pass. False on a timeout or a lost device.
bool WaitFence(ID3D12Fence *fence, UINT64 value, DWORD ms);

// HLSL to SPIR-V for the shaders the driver compiles at run time, shared with the other backend that takes SPIR-V
// (../spirv_shader.h)
using rr_spirv::CompileHlsl;

// "name (driver version), Vulkan x.y.z" of the device in use
const char *DeviceDescription();

// For the presenter, which owns the surface and the swap chain
VkInstance Instance();
VkPhysicalDevice PhysicalDevice();
VkDevice Device();
uint32_t QueueFamily();

// A swap chain image as a resource the driver can draw into and copy to. Its state is D3D12_RESOURCE_STATE_PRESENT,
// which for these resources is VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; releasing it leaves the image to the swap chain.
HRESULT WrapSwapchainImage(VkImage image, VkFormat format, UINT width, UINT height, ID3D12Resource **resource);

// The next ExecuteCommandLists waits for wait (an image acquisition) before writing color and signals signal (for the
// present) when done; either may be null. Applies to that one submission.
void SetNextSubmitSemaphores(VkSemaphore wait, VkSemaphore signal);

// vkQueuePresentKHR on the device's queue
VkResult Present(VkSwapchainKHR swapchain, uint32_t image, VkSemaphore wait);

// How many validation-layer errors have been reported since the program began, teardown's included, whatever the log's limit
uint64_t ValidationErrors();

// True once a call has returned VK_ERROR_DEVICE_LOST; the device then reports itself removed
bool DeviceLost();
void NoteResult(VkResult result, const char *what);

} // namespace rr_vk12

#endif
