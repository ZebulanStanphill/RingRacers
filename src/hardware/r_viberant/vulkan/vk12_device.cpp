// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_device.cpp
/// \brief vk12's device: the Vulkan instance and device, and ID3D12Device, the queue and fences on them

#include "vk12_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "../../../doomdef.h"
#include "../../../i_system.h"

#undef min
#undef max
#undef clamp

namespace rr_vk12
{

Context g;
const unsigned char kRootSignature[16] = {'R', 'R', 'V', 'K', '1', '2', 'R', 'O', 'O', 'T', 0, 0, 0, 0, 0, 1};

void Log(const char *format, ...)
{
	char message[2048];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	I_OutputMsg("Vulkan: %s\n", message);
}

HRESULT Unimplemented(const char *method)
{
	static int logged = 0;
	if (logged < 32)
	{
		logged++;
		Log("%s is not implemented by vk12", method);
	}
	return E_NOTIMPL;
}

void NoteResult(VkResult result, const char *what)
{
	if (result == VK_ERROR_DEVICE_LOST && !g.lost)
	{
		g.lost = true;
		Log("the device was lost in %s", what);
	}
}

bool DeviceLost()
{
	return g.lost;
}

HRESULT Result(VkResult result, const char *what)
{
	if (result >= 0)
		return S_OK;
	NoteResult(result, what);
	static int logged = 0;
	if (logged < 50)
	{
		logged++;
		Log("%s failed: VkResult %d", what, (int)result);
	}
	switch (result)
	{
		case VK_ERROR_OUT_OF_HOST_MEMORY:
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return E_OUTOFMEMORY;
		case VK_ERROR_DEVICE_LOST:
			return DXGI_ERROR_DEVICE_REMOVED;
		default:
			return E_FAIL;
	}
}

void SetDebugName(VkObjectType type, uint64_t handle, const std::string &name)
{
	if (!g.debugUtils || !handle || name.empty())
		return;
	VkDebugUtilsObjectNameInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
	info.objectType = type;
	info.objectHandle = handle;
	info.pObjectName = name.c_str();
	vkSetDebugUtilsObjectNameEXT(g.device, &info);
}

FormatInfo Format(DXGI_FORMAT format)
{
	switch (format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_B8G8R8A8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R8_UNORM: return {VK_FORMAT_R8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R32_SINT: return {VK_FORMAT_R32_SINT, 4, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R32_UINT: return {VK_FORMAT_R32_UINT, 4, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R16_UINT: return {VK_FORMAT_R16_UINT, 2, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R32G32B32_FLOAT: return {VK_FORMAT_R32G32B32_SFLOAT, 12, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_R32G32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, 8, VK_IMAGE_ASPECT_COLOR_BIT};
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return {g.depthStencil, 4, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
		default: return {VK_FORMAT_UNDEFINED, 0, 0};
	}
}

uint32_t MemoryType(uint32_t allowed, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags fallback)
{
	for (int pass = 0; pass < 2; pass++)
	{
		const VkMemoryPropertyFlags flags = pass ? fallback : wanted;
		for (uint32_t i = 0; i < g.memory.memoryTypeCount; i++)
			if ((allowed & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & flags) == flags)
				return i;
	}
	return UINT32_MAX;
}

VkMemoryPropertyFlags HeapMemoryFlags(D3D12_HEAP_TYPE type, VkMemoryPropertyFlags *fallback)
{
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	switch (type)
	{
		case D3D12_HEAP_TYPE_UPLOAD:
			*fallback = host;
			return host;
		case D3D12_HEAP_TYPE_READBACK:
			*fallback = host;
			return host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		default:
			*fallback = 0;
			return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Submissions and fences

void UpdateSubmissions(uint64_t waitFor, DWORD ms)
{
	while (!g.submissions.empty())
	{
		Context::Submission &oldest = g.submissions.front();
		VkResult result;
		if (waitFor && oldest.id <= waitFor && ms)
			result = vkWaitForFences(g.device, 1, &oldest.fence, VK_TRUE, (uint64_t)ms * 1000000ull);
		else
			result = vkGetFenceStatus(g.device, oldest.fence);
		if (result == VK_ERROR_DEVICE_LOST)
		{
			NoteResult(result, "a fence");
			return;
		}
		if (result != VK_SUCCESS)
			return;
		g.completedSubmission = oldest.id;
		vkResetFences(g.device, 1, &oldest.fence);
		g.freeFences.push_back(oldest.fence);
		g.submissions.pop_front();
	}
}

Fence::~Fence() = default;

void Fence::Pend(UINT64 value, uint64_t submission)
{
	pending_.push_back({value, submission});
}

UINT64 Fence::GetCompletedValue()
{
	if (g.lost)
		return UINT64_MAX;
	if (!pending_.empty())
		UpdateSubmissions();
	while (!pending_.empty() && pending_.front().submission <= g.completedSubmission)
	{
		completed_ = pending_.front().value;
		pending_.pop_front();
	}
	return g.lost ? UINT64_MAX : completed_;
}

HRESULT Fence::Signal(UINT64 value)
{
	completed_ = value;
	return S_OK;
}

bool Fence::Wait(UINT64 value, DWORD ms)
{
	if (GetCompletedValue() >= value)
		return !g.lost;
	uint64_t submission = 0;
	for (const Pending &pending : pending_)
	{
		if (pending.value >= value)
		{
			submission = pending.submission;
			break;
		}
	}
	if (!submission)
		return false; // nothing submitted will signal it
	const precise_t start = I_GetPreciseTime();
	for (;;)
	{
		const double elapsed = (I_GetPreciseTime() - start) * 1000.0 / I_GetPrecisePrecision();
		if (elapsed >= ms)
			break;
		UpdateSubmissions(submission, (DWORD)std::max(1.0, ms - elapsed));
		if (g.lost)
			return false;
		if (g.completedSubmission >= submission)
			break;
	}
	return GetCompletedValue() >= value && !g.lost;
}

bool WaitFence(ID3D12Fence *fence, UINT64 value, DWORD ms)
{
	return fence && static_cast<Fence *>(fence)->Wait(value, ms);
}

void SetNextSubmitSemaphores(VkSemaphore wait, VkSemaphore signal)
{
	g.nextWait = wait;
	g.nextSignal = signal;
}

void Queue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists)
{
	std::vector<VkCommandBuffer> buffers;
	for (UINT i = 0; i < count; i++)
	{
		List *list = static_cast<List *>(static_cast<ID3D12GraphicsCommandList *>(lists[i]));
		if (list->Open())
		{
			VK12_LOG_ONCE("ExecuteCommandLists: a list that wasn't closed");
			continue;
		}
		if (list->Buffer())
			buffers.push_back(list->Buffer());
	}

	VkFence fence = VK_NULL_HANDLE;
	if (!g.freeFences.empty())
	{
		fence = g.freeFences.back();
		g.freeFences.pop_back();
	}
	else
	{
		VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (FAILED(Result(vkCreateFence(g.device, &info, nullptr, &fence), "vkCreateFence")))
			return;
	}

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = (uint32_t)buffers.size();
	submit.pCommandBuffers = buffers.data();
	if (g.nextWait)
	{
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &g.nextWait;
		submit.pWaitDstStageMask = &waitStage;
	}
	if (g.nextSignal)
	{
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &g.nextSignal;
	}
	const VkResult result = vkQueueSubmit(g.queue, 1, &submit, fence);
	g.nextWait = g.nextSignal = VK_NULL_HANDLE;
	if (FAILED(Result(result, "vkQueueSubmit")))
	{
		g.freeFences.push_back(fence);
		return;
	}
	g.submissions.push_back({++g.lastSubmission, fence});
}

HRESULT Queue::Signal(ID3D12Fence *fence, UINT64 value)
{
	if (!fence)
		return E_POINTER;
	// The value is reached when everything submitted so far is done
	static_cast<Fence *>(fence)->Pend(value, g.lastSubmission);
	return g.lost ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

HRESULT Queue::GetTimestampFrequency(UINT64 *frequency)
{
	if (!frequency)
		return E_POINTER;
	VkQueueFamilyProperties families[32];
	uint32_t count = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &count, families);
	if (g.queueFamily >= count || !families[g.queueFamily].timestampValidBits || g.properties.limits.timestampPeriod <= 0)
		return E_FAIL;
	*frequency = (UINT64)(1000000000.0 / g.properties.limits.timestampPeriod + 0.5);
	return S_OK;
}

VkResult Present(VkSwapchainKHR swapchain, uint32_t image, VkSemaphore wait)
{
	VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	info.waitSemaphoreCount = wait ? 1 : 0;
	info.pWaitSemaphores = &wait;
	info.swapchainCount = 1;
	info.pSwapchains = &swapchain;
	info.pImageIndices = &image;
	const VkResult result = vkQueuePresentKHR(g.queue, &info);
	NoteResult(result, "vkQueuePresentKHR");
	return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Descriptor heaps

DescriptorHeap::DescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC &desc) : desc_(desc), slots_(desc.NumDescriptors)
{
}

DescriptorHeap::~DescriptorHeap() = default;

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::GetCPUDescriptorHandleForHeapStart()
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle;
	handle.ptr = (SIZE_T)(uintptr_t)slots_.data();
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GetGPUDescriptorHandleForHeapStart()
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle;
	handle.ptr = (UINT64)(uintptr_t)slots_.data();
	return handle;
}

QueryHeap::~QueryHeap()
{
	if (pool)
		vkDestroyQueryPool(g.device, pool, nullptr);
}

Heap::~Heap()
{
	if (memory_)
		vkFreeMemory(g.device, memory_, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// The memory adapter

#ifndef _WIN32 // Windows has DXGI's own adapter interface, which this isn't; budgets are left unknown there for now
class MemoryAdapter : public IDXGIAdapter3
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (!out)
			return E_POINTER;
		if (SameGuid(riid, __uuidof(IUnknown)))
		{
			*out = static_cast<IUnknown *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG left = --refs_;
		if (!left)
			delete this;
		return left;
	}

	// Local: the device's own heaps. Budget and usage are VK_EXT_memory_budget's, summed over those heaps.
	HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT, DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO *info) override
	{
		if (!info || !g.device || !g.memoryBudget)
			return E_FAIL;
		VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
		VkPhysicalDeviceMemoryProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
		properties.pNext = &budget;
		vkGetPhysicalDeviceMemoryProperties2(g.physical, &properties);
		*info = {};
		bool anyLocal = false;
		for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; i++)
			anyLocal |= (properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
		for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; i++)
		{
			const bool local = !anyLocal || (properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
			if (local != (group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL))
				continue;
			info->Budget += budget.heapBudget[i];
			info->CurrentUsage += budget.heapUsage[i];
		}
		return S_OK;
	}

private:
	std::atomic<ULONG> refs_{1};
};
#endif

// ---------------------------------------------------------------------------------------------------------------
// The device

class DeviceImpl : public Object<ID3D12DeviceBase, ID3D12Device>
{
public:
	~DeviceImpl() override;

	HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *, REFIID riid, void **out) override
	{
		return Finish(new Queue, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID riid, void **out) override
	{
		Allocator *allocator = new Allocator;
		const HRESULT hr = allocator->Init();
		if (FAILED(hr))
		{
			allocator->Release();
			return hr;
		}
		return Finish(allocator, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *allocator,
		ID3D12PipelineState *, REFIID riid, void **out) override
	{
		List *list = new List;
		const HRESULT hr = list->Init(static_cast<Allocator *>(allocator));
		if (FAILED(hr))
		{
			list->Release();
			return hr;
		}
		return Finish(list, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
		void **out) override
	{
		if (!desc)
			return E_INVALIDARG;
		Pipeline *pipeline = new Pipeline;
		const HRESULT hr = pipeline->Init(*desc);
		if (FAILED(hr))
		{
			pipeline->Release();
			return hr;
		}
		return Finish(pipeline, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void *blob, SIZE_T bytes, REFIID riid, void **out) override
	{
		if (bytes != sizeof kRootSignature || memcmp(blob, kRootSignature, sizeof kRootSignature))
		{
			Log("CreateRootSignature: only vk12's own root signature is supported");
			return E_INVALIDARG;
		}
		return Finish(new RootSignature, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out) override
	{
		if (!desc)
			return E_INVALIDARG;
		return Finish(new DescriptorHeap(*desc), riid, out);
	}

	UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override
	{
		return (UINT)sizeof(Slot);
	}

	HRESULT STDMETHODCALLTYPE CreateFence(UINT64 value, D3D12_FENCE_FLAGS, REFIID riid, void **out) override
	{
		return Finish(new Fence(value), riid, out);
	}

	HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override
	{
		return g.lost ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
	}

	HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out) override
	{
		if (!desc || desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP)
			return E_INVALIDARG;
		VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = desc->Count;
		VkQueryPool pool = VK_NULL_HANDLE;
		const HRESULT hr = Result(vkCreateQueryPool(g.device, &info, nullptr, &pool), "vkCreateQueryPool");
		if (FAILED(hr))
			return hr;
		return Finish(new QueryHeap(pool, desc->Count), riid, out);
	}

	HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size) override;

	void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) override;
	void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) override;
	void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) override;
	void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) override;

	D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT count,
		const D3D12_RESOURCE_DESC *descs) override;
	HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS,
		const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *, REFIID riid, void **out) override
	{
		if (!heap || !desc)
			return E_INVALIDARG;
		Resource *resource = nullptr;
		const HRESULT hr = CreateResource(heap, nullptr, 0, *desc, state, &resource);
		return FAILED(hr) ? hr : Finish(resource, riid, out);
	}
	HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **out) override;
	HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc,
		D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *, REFIID riid, void **out) override
	{
		if (!heap || !desc)
			return E_INVALIDARG;
		Resource *resource = nullptr;
		const HRESULT hr = CreateResource(nullptr, static_cast<Heap *>(heap), offset, *desc, state, &resource);
		return FAILED(hr) ? hr : Finish(resource, riid, out);
	}
	void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base,
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *rowBytes, UINT64 *total) override;

	std::map<std::string, VkSampler> samplers;
};

HRESULT DeviceImpl::CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size)
{
	if (!data)
		return E_INVALIDARG;
	switch (feature)
	{
		case D3D12_FEATURE_FEATURE_LEVELS:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
				return E_INVALIDARG;
			auto *levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(data);
			levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
			for (UINT i = 0; i < levels->NumFeatureLevels; i++)
				if (levels->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_0)
					levels->MaxSupportedFeatureLevel = std::max(levels->MaxSupportedFeatureLevel, levels->pFeatureLevelsRequested[i]);
			return S_OK;
		}
		case D3D12_FEATURE_SHADER_MODEL:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
				return E_INVALIDARG;
			auto *model = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(data);
			model->HighestShaderModel = D3D_SHADER_MODEL_5_1;
			return S_OK;
		}
		case D3D12_FEATURE_D3D12_OPTIONS:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
				return E_INVALIDARG;
			auto *options = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(data);
			*options = {};
			options->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
			options->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
			return S_OK;
		}
		case D3D12_FEATURE_ARCHITECTURE:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
				return E_INVALIDARG;
			auto *architecture = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(data);
			const bool unified = g.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
			architecture->TileBasedRenderer = FALSE;
			architecture->UMA = unified;
			architecture->CacheCoherentUMA = unified;
			return S_OK;
		}
		case D3D12_FEATURE_ROOT_SIGNATURE:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
				return E_INVALIDARG;
			auto *root = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(data);
			root->HighestVersion = std::min(root->HighestVersion, D3D_ROOT_SIGNATURE_VERSION_1_1);
			return S_OK;
		}
		case D3D12_FEATURE_FORMAT_SUPPORT:
		{
			if (size != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
				return E_INVALIDARG;
			auto *support = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(data);
			const FormatInfo info = Format(support->Format);
			support->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
			support->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
			if (info.format == VK_FORMAT_UNDEFINED)
				return S_OK;
			VkFormatProperties properties;
			vkGetPhysicalDeviceFormatProperties(g.physical, info.format, &properties);
			const VkFormatFeatureFlags features = properties.optimalTilingFeatures;
			UINT flags = 0;
			if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
				flags |= D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD
					| D3D12_FORMAT_SUPPORT1_MIP;
			if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
				flags |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
			if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
				flags |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
			if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
				flags |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
			if (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
				flags |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_TEXTURE2D;
			support->Support1 = (D3D12_FORMAT_SUPPORT1)flags;
			return S_OK;
		}
		default:
			VK12_LOG_ONCE("CheckFeatureSupport: feature %d isn't known to vk12", (int)feature);
			return E_INVALIDARG;
	}
}

void DeviceImpl::CreateShaderResourceView(ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	Resource *target = static_cast<Resource *>(resource);
	const bool volume = desc ? desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D
		: target && target->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	if (!target)
		target = NullImage(desc ? desc->Format : DXGI_FORMAT_R8G8B8A8_UNORM, volume);
	*slot = {};
	slot->serial = g.nextSerial++;
	if (!target || !target->image)
		return;
	const FormatInfo format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? Format(desc->Format) : target->format;
	uint32_t mips = target->desc.MipLevels;
	if (desc)
	{
		const UINT wanted = volume ? desc->Texture3D.MipLevels : desc->Texture2D.MipLevels;
		if (wanted != UINT32_MAX && wanted)
			mips = std::min<uint32_t>(mips, wanted);
	}
	slot->resource = target;
	slot->view = target->View(format.format, volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, mips);
	slot->format = format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

void DeviceImpl::CreateRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	Resource *target = static_cast<Resource *>(resource);
	*slot = {};
	slot->serial = g.nextSerial++;
	if (!target || !target->image)
		return;
	const FormatInfo format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? Format(desc->Format) : target->format;
	slot->resource = target;
	slot->view = target->View(format.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1);
	slot->format = format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

void DeviceImpl::CreateDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *,
	D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	Resource *target = static_cast<Resource *>(resource);
	*slot = {};
	slot->serial = g.nextSerial++;
	if (!target || !target->image)
		return;
	slot->resource = target;
	slot->view = target->View(target->format.format, VK_IMAGE_VIEW_TYPE_2D, target->format.aspect, 1);
	slot->format = target->format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

static VkSamplerAddressMode AddressMode(D3D12_TEXTURE_ADDRESS_MODE mode)
{
	switch (mode)
	{
		case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	}
}

void DeviceImpl::CreateSampler(const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	*slot = {};
	slot->serial = g.nextSerial++;
	if (!desc)
		return;
	const std::string key(reinterpret_cast<const char *>(desc), sizeof *desc);
	auto found = samplers.find(key);
	if (found != samplers.end())
	{
		slot->sampler = found->second;
		return;
	}

	// D3D12_FILTER's bits: mip linear (1), mag linear (4), min linear (16), anisotropic (0x55 with 0x40)
	const bool anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc->Filter);
	VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	info.magFilter = anisotropic || (desc->Filter & 0x4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter = anisotropic || (desc->Filter & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = anisotropic || (desc->Filter & 0x1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = AddressMode(desc->AddressU);
	info.addressModeV = AddressMode(desc->AddressV);
	info.addressModeW = AddressMode(desc->AddressW);
	info.mipLodBias = desc->MipLODBias;
	if (anisotropic && g.anisotropy && desc->MaxAnisotropy > 1)
	{
		info.anisotropyEnable = VK_TRUE;
		info.maxAnisotropy = std::min((float)desc->MaxAnisotropy, g.properties.limits.maxSamplerAnisotropy);
	}
	else
		info.maxAnisotropy = 1.0f;
	info.compareOp = VK_COMPARE_OP_NEVER;
	info.minLod = desc->MinLOD;
	info.maxLod = desc->MaxLOD >= 1000.0f ? VK_LOD_CLAMP_NONE : desc->MaxLOD;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSampler sampler = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateSampler(g.device, &info, nullptr, &sampler), "vkCreateSampler")))
		return;
	samplers[key] = sampler;
	slot->sampler = sampler;
}

D3D12_RESOURCE_ALLOCATION_INFO DeviceImpl::GetResourceAllocationInfo(UINT, UINT count, const D3D12_RESOURCE_DESC *descs)
{
	// Only as the driver asks: one texture at a time
	D3D12_RESOURCE_ALLOCATION_INFO result = {UINT64_MAX, 0};
	if (count != 1 || !descs)
		return result;
	if (descs->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		result.SizeInBytes = (descs->Width + 65535) & ~65535ull;
		result.Alignment = 65536;
		return result;
	}
	VkImageCreateInfo info;
	FormatInfo format;
	if (!ImageInfo(*descs, info, format))
		return result;
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(g.device, &info, nullptr, &image) != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(g.device, image, &requirements);
	vkDestroyImage(g.device, image, nullptr);
	// Direct3D 12 places textures at 4 KB (what a request for small resource alignment gets, when the texture is
	// small enough) or at 64 KB, and the driver sorts its placements into those two classes and rounds sizes to them.
	// Vulkan's own alignment is the least the image needs: lavapipe says 64 bytes and RADV 256 for a 1x1 texture, which
	// the driver wouldn't place at all, so the class is the greater of the two.
	UINT64 alignment = descs->Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
		&& requirements.alignment <= D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
		? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	alignment = std::max<UINT64>(alignment, requirements.alignment);
	result.SizeInBytes = (requirements.size + alignment - 1) / alignment * alignment;
	result.Alignment = alignment;
	return result;
}

HRESULT DeviceImpl::CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **out)
{
	if (!desc || !desc->SizeInBytes)
		return E_INVALIDARG;
	// A memory type every sampled texture can use: what a small RGBA8 texture reports
	D3D12_RESOURCE_DESC probe = {};
	probe.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	probe.Width = probe.Height = 64;
	probe.DepthOrArraySize = 1;
	probe.MipLevels = 1;
	probe.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	probe.SampleDesc.Count = 1;
	if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES)
		probe.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	VkImageCreateInfo info;
	FormatInfo format;
	if (!ImageInfo(probe, info, format))
		return E_FAIL;
	VkImage image = VK_NULL_HANDLE;
	HRESULT hr = Result(vkCreateImage(g.device, &info, nullptr, &image), "vkCreateImage (heap probe)");
	if (FAILED(hr))
		return hr;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(g.device, image, &requirements);
	vkDestroyImage(g.device, image, nullptr);

	VkMemoryPropertyFlags fallback;
	const VkMemoryPropertyFlags wanted = HeapMemoryFlags(desc->Properties.Type, &fallback);
	const uint32_t type = MemoryType(requirements.memoryTypeBits, wanted, fallback);
	if (type == UINT32_MAX)
		return E_OUTOFMEMORY;
	VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate.allocationSize = desc->SizeInBytes;
	allocate.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	hr = Result(vkAllocateMemory(g.device, &allocate, nullptr, &memory), "vkAllocateMemory (heap)");
	if (FAILED(hr))
		return hr;
	return Finish(new Heap(*desc, memory, type), riid, out);
}

void DeviceImpl::GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base,
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *rowBytes, UINT64 *total)
{
	// Direct3D 12's layout: each subresource starts on a 512-byte boundary and its rows on 256-byte ones
	UINT64 offset = base;
	UINT64 end = base;
	const FormatInfo format = desc ? Format(desc->Format) : FormatInfo{};
	for (UINT i = 0; desc && i < count; i++)
	{
		const UINT mip = first + i;
		const UINT width = std::max<UINT64>(1, desc->Width >> mip);
		const UINT height = std::max<UINT>(1, desc->Height >> mip);
		const UINT depth = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<UINT>(1, desc->DepthOrArraySize >> mip) : 1;
		const UINT64 rowSize = (UINT64)width * format.bytes;
		const UINT pitch = (UINT)((rowSize + 255) & ~255ull);
		offset = (offset + 511) & ~511ull;
		if (layouts)
		{
			layouts[i].Offset = offset;
			layouts[i].Footprint.Format = desc->Format;
			layouts[i].Footprint.Width = width;
			layouts[i].Footprint.Height = height;
			layouts[i].Footprint.Depth = depth;
			layouts[i].Footprint.RowPitch = pitch;
		}
		if (rows)
			rows[i] = height;
		if (rowBytes)
			rowBytes[i] = rowSize;
		// The last row isn't padded
		end = offset + (UINT64)pitch * height * (depth - 1) + (UINT64)pitch * (height - 1) + rowSize;
		offset = end;
	}
	if (total)
		*total = end - base;
}

// ---------------------------------------------------------------------------------------------------------------
// Making and destroying the device

// Counted by the callback, which any thread may run, and kept past the device's destruction, so that a test can also
// check the messages of teardown. The log stops at 200 messages; the count doesn't.
static std::atomic<uint64_t> validationErrors{0};
uint64_t ValidationErrors() { return validationErrors; }

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		validationErrors++;
	if (g.validationMessages < 200)
	{
		g.validationMessages++;
		Log("%s: %s", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "validation error" : "validation warning",
			data && data->pMessage ? data->pMessage : "(no message)");
	}
	return VK_FALSE;
}

static bool HasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
{
	for (const VkExtensionProperties &extension : list)
		if (!strcmp(extension.extensionName, name))
			return true;
	return false;
}

static void DestroyGlobals()
{
	if (g.device)
	{
		vkDeviceWaitIdle(g.device);
		for (Resource *image : g.nullImages)
			image->Release();
		g.nullImages.clear();
		for (auto &framebuffer : g.framebuffers)
			vkDestroyFramebuffer(g.device, framebuffer.second, nullptr);
		for (auto &pass : g.passes)
			vkDestroyRenderPass(g.device, pass.second, nullptr);
		for (Context::Submission &submission : g.submissions)
			vkDestroyFence(g.device, submission.fence, nullptr);
		for (VkFence fence : g.freeFences)
			vkDestroyFence(g.device, fence, nullptr);
		if (g.pipelineCache)
			vkDestroyPipelineCache(g.device, g.pipelineCache, nullptr);
		if (g.pipelineLayout)
			vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
		if (g.setLayout)
			vkDestroyDescriptorSetLayout(g.device, g.setLayout, nullptr);
		vkDestroyDevice(g.device, nullptr);
	}
	if (g.instance)
	{
		if (g.messenger)
			vkDestroyDebugUtilsMessengerEXT(g.instance, g.messenger, nullptr);
		vkDestroyInstance(g.instance, nullptr);
	}
	g = Context();
}

DeviceImpl::~DeviceImpl()
{
	if (g.device)
	{
		vkDeviceWaitIdle(g.device);
		for (auto &sampler : samplers)
			vkDestroySampler(g.device, sampler.second, nullptr);
	}
	DestroyGlobals();
	Log("released the device");
}

static bool MakeLayouts()
{
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	for (unsigned i = 0; i < kCbvCount; i++)
		bindings.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	for (unsigned i = 0; i < kSrvCount; i++)
		bindings.push_back({kSrvBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	for (unsigned i = 0; i < kSamplerCount; i++)
		bindings.push_back({kSamplerBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set.bindingCount = (uint32_t)bindings.size();
	set.pBindings = bindings.data();
	if (FAILED(Result(vkCreateDescriptorSetLayout(g.device, &set, nullptr, &g.setLayout), "vkCreateDescriptorSetLayout")))
		return false;
	VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &g.setLayout;
	if (FAILED(Result(vkCreatePipelineLayout(g.device, &layout, nullptr, &g.pipelineLayout), "vkCreatePipelineLayout")))
		return false;
	VkPipelineCacheCreateInfo cache = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	return SUCCEEDED(Result(vkCreatePipelineCache(g.device, &cache, nullptr, &g.pipelineCache), "vkCreatePipelineCache"));
}

HRESULT CreateDevice(const DeviceOptions &options, ID3D12Device **device, IDXGIAdapter3 **memory)
{
	if (!device)
		return E_POINTER;
	*device = nullptr;
	if (memory)
		*memory = nullptr;
	if (g.device)
	{
		Log("a device already exists");
		return E_FAIL;
	}

	if (volkInitialize() != VK_SUCCESS)
	{
		Log("no Vulkan loader was found (libvulkan or vulkan-1.dll)");
		return E_FAIL;
	}
	if (volkGetInstanceVersion() < VK_API_VERSION_1_1)
	{
		Log("the Vulkan loader is older than 1.1");
		return E_FAIL;
	}

	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> instanceExtensions(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExtensions.data());
	std::vector<const char *> enabled;
	// The presenter makes the surface through SDL, which needs these enabled; which ones exist depends on the system
	for (const char *name : {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
		"VK_KHR_win32_surface", "VK_EXT_metal_surface", "VK_KHR_portability_enumeration"})
		if (HasExtension(instanceExtensions, name))
			enabled.push_back(name);
	const bool debugUtils = HasExtension(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (debugUtils)
		enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	std::vector<const char *> layers;
	if (options.validation)
	{
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> available(count);
		vkEnumerateInstanceLayerProperties(&count, available.data());
		bool found = false;
		for (const VkLayerProperties &layer : available)
			found |= !strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation");
		if (found)
			layers.push_back("VK_LAYER_KHRONOS_validation");
		else
			Log("validation was asked for, but VK_LAYER_KHRONOS_validation isn't installed");
	}

	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.pApplicationName = "Dr. Robotnik's Ring Racers";
	application.pEngineName = "VibeRant";
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instance = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = (uint32_t)enabled.size();
	instance.ppEnabledExtensionNames = enabled.data();
	instance.enabledLayerCount = (uint32_t)layers.size();
	instance.ppEnabledLayerNames = layers.data();
	if (HasExtension(instanceExtensions, "VK_KHR_portability_enumeration"))
		instance.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
	if (FAILED(Result(vkCreateInstance(&instance, nullptr, &g.instance), "vkCreateInstance")))
	{
		g = Context();
		return E_FAIL;
	}
	volkLoadInstance(g.instance);
	g.debugUtils = debugUtils;
	if (debugUtils && !layers.empty())
	{
		VkDebugUtilsMessengerCreateInfoEXT messenger = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = DebugMessage;
		vkCreateDebugUtilsMessengerEXT(g.instance, &messenger, nullptr, &g.messenger);
	}

	// The first device of the best kind that has a graphics queue and a swap chain
	vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(g.instance, &count, devices.data());
	int bestRank = -1;
	std::vector<VkExtensionProperties> deviceExtensions;
	for (VkPhysicalDevice candidate : devices)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(candidate, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_1)
			continue;
		const bool cpu = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
		int rank;
		if (options.software)
			rank = cpu ? 4 : 0;
		else
			switch (properties.deviceType)
			{
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: rank = 4; break;
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 3; break;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: rank = 2; break;
				case VK_PHYSICAL_DEVICE_TYPE_CPU: rank = 0; break;
				default: rank = 1; break;
			}
		if (rank <= bestRank)
			continue;
		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data());
		if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			continue;
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		uint32_t family = UINT32_MAX;
		for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				family = i;
		if (family == UINT32_MAX)
			continue;
		bestRank = rank;
		g.physical = candidate;
		g.queueFamily = family;
		g.properties = properties;
		deviceExtensions = extensions;
	}
	if (!g.physical)
	{
		Log(options.software ? "no CPU Vulkan device (lavapipe) was found" : "no Vulkan 1.1 device with a graphics queue and a swap chain was found");
		DestroyGlobals();
		return E_FAIL;
	}
	if (!options.software && g.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
		Log("only a CPU Vulkan device was found; it will be slow");

	vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
	VkPhysicalDeviceFeatures available;
	vkGetPhysicalDeviceFeatures(g.physical, &available);
	VkPhysicalDeviceFeatures features = {};
	features.samplerAnisotropy = available.samplerAnisotropy;
	features.shaderClipDistance = available.shaderClipDistance;
	features.depthClamp = available.depthClamp;
	g.anisotropy = available.samplerAnisotropy;
	if (!available.shaderClipDistance)
		Log("the device has no clip distances; portals will not clip");

	std::vector<const char *> deviceEnabled = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	g.memoryBudget = HasExtension(deviceExtensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	if (g.memoryBudget)
		deviceEnabled.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	if (HasExtension(deviceExtensions, "VK_KHR_portability_subset"))
		deviceEnabled.push_back("VK_KHR_portability_subset");

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue.queueFamilyIndex = g.queueFamily;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	VkDeviceCreateInfo create = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	create.queueCreateInfoCount = 1;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = (uint32_t)deviceEnabled.size();
	create.ppEnabledExtensionNames = deviceEnabled.data();
	create.pEnabledFeatures = &features;
	if (FAILED(Result(vkCreateDevice(g.physical, &create, nullptr, &g.device), "vkCreateDevice")))
	{
		g.device = VK_NULL_HANDLE;
		DestroyGlobals();
		return E_FAIL;
	}
	volkLoadDevice(g.device);
	vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);

	// Direct3D's D24S8: the first of these the device can render to and sample
	for (VkFormat format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT})
	{
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(g.physical, format, &properties);
		if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			g.depthStencil = format;
			break;
		}
	}
	if (g.depthStencil == VK_FORMAT_UNDEFINED || g.properties.limits.minUniformBufferOffsetAlignment > 256 || !MakeLayouts())
	{
		Log("the device lacks a depth-stencil format, or needs uniform buffers aligned to more than 256 bytes");
		DestroyGlobals();
		return E_FAIL;
	}

	snprintf(g.description, sizeof g.description, "%s (driver %u.%u.%u), Vulkan %u.%u.%u", g.properties.deviceName,
		VK_VERSION_MAJOR(g.properties.driverVersion), VK_VERSION_MINOR(g.properties.driverVersion),
		VK_VERSION_PATCH(g.properties.driverVersion), VK_VERSION_MAJOR(g.properties.apiVersion),
		VK_VERSION_MINOR(g.properties.apiVersion), VK_VERSION_PATCH(g.properties.apiVersion));
	Log("device: %s, %04X:%04X, depth-stencil %s, anisotropy %s, clip distances %s, memory budgets %s%s", g.description,
		g.properties.vendorID, g.properties.deviceID, g.depthStencil == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32FS8",
		g.anisotropy ? "yes" : "no", available.shaderClipDistance ? "yes" : "no", g.memoryBudget ? "yes" : "no",
		layers.empty() ? "" : ", validation on");

	*device = new DeviceImpl;
#ifndef _WIN32
	if (memory && g.memoryBudget)
		*memory = new MemoryAdapter;
#endif
	return S_OK;
}

const char *DeviceDescription() { return g.description; }
VkInstance Instance() { return g.instance; }
VkPhysicalDevice PhysicalDevice() { return g.physical; }
VkDevice Device() { return g.device; }
uint32_t QueueFamily() { return g.queueFamily; }

} // namespace rr_vk12
