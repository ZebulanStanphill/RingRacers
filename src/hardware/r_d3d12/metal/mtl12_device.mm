// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_device.mm
/// \brief mtl12's device: the Metal device and its queue, and ID3D12Device, the queue and fences on them

#include "mtl12_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

// i_system.h's, which this file can't include: the game's headers and Objective-C's don't agree on what a boolean is
extern "C" void I_OutputMsg(const char *format, ...);

namespace rr_mtl12
{

Context g;
Completions completions;
const unsigned char kRootSignature[16] = {'R', 'R', 'M', 'T', 'L', '1', '2', 'R', 'O', 'O', 'T', 0, 0, 0, 0, 1};

void Log(const char *format, ...)
{
	char message[2048];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	I_OutputMsg("Metal: %s\n", message);
}

HRESULT Unimplemented(const char *method)
{
	static int logged = 0;
	if (logged < 32)
	{
		logged++;
		Log("%s is not implemented by mtl12", method);
	}
	return E_NOTIMPL;
}

bool DeviceLost()
{
	return completions.lost;
}

uint64_t ValidationErrors()
{
	return completions.errors;
}

FormatInfo Format(DXGI_FORMAT format)
{
	switch (format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return {MTLPixelFormatRGBA8Unorm, 4, false};
		case DXGI_FORMAT_B8G8R8A8_UNORM: return {MTLPixelFormatBGRA8Unorm, 4, false};
		case DXGI_FORMAT_R8_UNORM: return {MTLPixelFormatR8Unorm, 1, false};
		case DXGI_FORMAT_R32_SINT: return {MTLPixelFormatR32Sint, 4, false};
		case DXGI_FORMAT_R32_UINT: return {MTLPixelFormatR32Uint, 4, false};
		case DXGI_FORMAT_R16_UINT: return {MTLPixelFormatR16Uint, 2, false};
		// Apple's GPUs have no 24-bit depth, and every Metal device has this one
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return {MTLPixelFormatDepth32Float_Stencil8, 4, true};
		default: return {MTLPixelFormatInvalid, 0, false};
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Submissions and fences

void FlushMessages()
{
	std::vector<std::string> messages;
	{
		std::lock_guard<std::mutex> lock(completions.mutex);
		messages.swap(completions.messages);
	}
	for (const std::string &message : messages)
		Log("%s", message.c_str());
}

static void Note(const std::string &message)
{
	std::lock_guard<std::mutex> lock(completions.mutex);
	if (completions.messages.size() < 200)
		completions.messages.push_back(message);
}

// Runs on a thread of Metal's when a command buffer has finished, in success or not
static void Completed(id<MTLCommandBuffer> buffer, uint64_t submission)
{
	NSError *error = buffer.error;
	if (error)
	{
		completions.errors++;
		if ([error.domain isEqualToString:MTLCommandBufferErrorDomain] && error.code == MTLCommandBufferErrorDeviceRemoved)
			completions.lost = true;
		std::string message = "a command buffer failed: ";
		message += error.localizedDescription.UTF8String ?: "(no description)";
		if (@available(macOS 11.0, *))
		{
			NSArray<id<MTLCommandBufferEncoderInfo>> *encoders = error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
			for (id<MTLCommandBufferEncoderInfo> encoder in encoders)
			{
				if (encoder.errorState != MTLCommandEncoderErrorStateFaulted && encoder.errorState != MTLCommandEncoderErrorStateAffected)
					continue;
				message += encoder.errorState == MTLCommandEncoderErrorStateFaulted ? "; faulted in " : "; affected ";
				message += encoder.label.UTF8String ?: "an unnamed encoder";
			}
		}
		Note(message);
	}
	if (@available(macOS 11.0, *))
	{
		// What shader validation found in the buffer's draws
		for (id<MTLFunctionLog> entry in buffer.logs)
		{
			completions.errors++;
			std::string message = "shader validation: ";
			message += entry.description.UTF8String ?: "(no description)";
			Note(message);
		}
	}
	if (submission)
	{
		std::lock_guard<std::mutex> lock(completions.mutex);
		completions.running.erase(submission);
		completions.changed.notify_all();
	}
}

id<MTLCommandBuffer> NewCommandBuffer()
{
	if (g.validation)
	{
		if (@available(macOS 11.0, *))
		{
			MTLCommandBufferDescriptor *descriptor = [[MTLCommandBufferDescriptor alloc] init];
			descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
			return [g.queue commandBufferWithDescriptor:descriptor];
		}
	}
	return [g.queue commandBuffer];
}

bool WaitIdle(DWORD ms)
{
	std::unique_lock<std::mutex> lock(completions.mutex);
	return completions.changed.wait_for(lock, std::chrono::milliseconds(ms), [] { return completions.running.empty(); });
}

void Fence::Pend(UINT64 value, uint64_t submission)
{
	pending_.push_back({value, submission});
}

UINT64 Fence::GetCompletedValue()
{
	FlushMessages();
	if (completions.lost)
		return UINT64_MAX;
	if (!pending_.empty())
	{
		uint64_t done;
		{
			std::lock_guard<std::mutex> lock(completions.mutex);
			done = completions.Completed();
		}
		while (!pending_.empty() && pending_.front().submission <= done)
		{
			completed_ = pending_.front().value;
			pending_.pop_front();
		}
	}
	return completed_;
}

HRESULT Fence::Signal(UINT64 value)
{
	completed_ = value;
	return S_OK;
}

bool Fence::Wait(UINT64 value, DWORD ms)
{
	if (GetCompletedValue() >= value)
		return !completions.lost;
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
	{
		std::unique_lock<std::mutex> lock(completions.mutex);
		completions.changed.wait_for(lock, std::chrono::milliseconds(ms),
			[submission] { return completions.Completed() >= submission || completions.lost; });
	}
	return GetCompletedValue() >= value && !completions.lost;
}

bool WaitFence(ID3D12Fence *fence, UINT64 value, DWORD ms)
{
	return fence && static_cast<Fence *>(fence)->Wait(value, ms);
}

void PresentWithNextSubmit(void *drawable)
{
	g.nextDrawable = (__bridge id<MTLDrawable>)drawable;
}

void Queue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists)
{
	@autoreleasepool
	{
		std::vector<id<MTLCommandBuffer>> buffers;
		for (UINT i = 0; i < count; i++)
		{
			List *list = static_cast<List *>(static_cast<ID3D12GraphicsCommandList *>(lists[i]));
			if (list->Open())
			{
				MTL12_LOG_ONCE("ExecuteCommandLists: a list that wasn't closed");
				continue;
			}
			id<MTLCommandBuffer> buffer = list->TakeBuffer();
			if (buffer)
				buffers.push_back(buffer);
		}
		id<MTLDrawable> drawable = g.nextDrawable;
		g.nextDrawable = nil;
		if (drawable && buffers.empty())
			buffers.push_back(NewCommandBuffer());

		// The submission is done when its last command buffer is: the queue runs them in the order they are committed
		uint64_t submission;
		{
			std::lock_guard<std::mutex> lock(completions.mutex);
			submission = ++completions.last;
			if (!buffers.empty())
				completions.running.insert(submission);
		}
		for (size_t i = 0; i < buffers.size(); i++)
		{
			const bool last = i + 1 == buffers.size();
			const uint64_t completes = last ? submission : 0;
			if (last && drawable)
				[buffers[i] presentDrawable:drawable];
			[buffers[i] addCompletedHandler:^(id<MTLCommandBuffer> done) {
				Completed(done, completes);
			}];
			[buffers[i] commit];
		}
	}
	FlushMessages();
}

HRESULT Queue::Signal(ID3D12Fence *fence, UINT64 value)
{
	if (!fence)
		return E_POINTER;
	// The value is reached when everything submitted so far is done
	uint64_t last;
	{
		std::lock_guard<std::mutex> lock(completions.mutex);
		last = completions.last;
	}
	static_cast<Fence *>(fence)->Pend(value, last);
	return completions.lost ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

HRESULT Queue::GetTimestampFrequency(UINT64 *frequency)
{
	if (!frequency)
		return E_POINTER;
	// Timestamp queries read as zero (see QueryHeap); the driver only needs a frequency to divide by
	*frequency = 1000000000ull;
	return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Descriptor heaps

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

// ---------------------------------------------------------------------------------------------------------------
// The memory adapter

class MemoryAdapter : public IDXGIAdapter3
{
public:
	virtual ~MemoryAdapter() = default;

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

	// Local: what Metal recommends the device's allocations stay under, and what they come to now
	HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT, DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO *info) override
	{
		if (!info || !g.device)
			return E_FAIL;
		*info = {};
		if (group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
		{
			info->Budget = g.device.recommendedMaxWorkingSetSize;
			info->CurrentUsage = g.device.currentAllocatedSize;
		}
		return S_OK;
	}

private:
	std::atomic<ULONG> refs_{1};
};

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
		return Finish(new Allocator, riid, out);
	}

	HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *allocator,
		ID3D12PipelineState *, REFIID riid, void **out) override
	{
		List *list = new List;
		const HRESULT hr = list->Reset(allocator, nullptr);
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
			Log("CreateRootSignature: only mtl12's own root signature is supported");
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
		return completions.lost ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
	}

	HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out) override
	{
		if (!desc || desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP)
			return E_INVALIDARG;
		return Finish(new QueryHeap(desc->Count), riid, out);
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
		const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out) override
	{
		if (!heap || !desc)
			return E_INVALIDARG;
		Resource *resource = nullptr;
		const HRESULT hr = CreateResource(heap->Type, nullptr, *desc, &resource);
		return FAILED(hr) ? hr : Finish(resource, riid, out);
	}
	HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **out) override
	{
		if (!desc || !desc->SizeInBytes)
			return E_INVALIDARG;
		return Finish(new Heap(*desc), riid, out);
	}
	HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc,
		D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out) override
	{
		if (!heap || !desc)
			return E_INVALIDARG;
		Heap *placed = static_cast<Heap *>(heap);
		// The heap is bookkeeping, but a resource outside it is still the caller's mistake
		const D3D12_RESOURCE_ALLOCATION_INFO info = GetResourceAllocationInfo(0, 1, desc);
		if (info.SizeInBytes == UINT64_MAX || offset % info.Alignment || offset + info.SizeInBytes > placed->GetDesc().SizeInBytes)
		{
			MTL12_LOG_ONCE("CreatePlacedResource: a %llux%u resource doesn't fit its heap at %llu (needs %llu bytes aligned to %llu)",
				(unsigned long long)desc->Width, (unsigned)desc->Height, (unsigned long long)offset,
				(unsigned long long)info.SizeInBytes, (unsigned long long)info.Alignment);
			return E_INVALIDARG;
		}
		Resource *resource = nullptr;
		const HRESULT hr = CreateResource(placed->GetDesc().Properties.Type, placed, *desc, &resource);
		return FAILED(hr) ? hr : Finish(resource, riid, out);
	}
	void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base,
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *rowBytes, UINT64 *total) override;

	std::map<std::string, id<MTLSamplerState>> samplers;
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
			architecture->TileBasedRenderer = g.tiled;
			architecture->UMA = g.unified;
			architecture->CacheCoherentUMA = g.unified;
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
			// What every Metal device on a Mac does with the few formats the driver has
			auto *support = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(data);
			const FormatInfo info = Format(support->Format);
			support->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
			support->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
			if (info.format == MTLPixelFormatInvalid)
				return S_OK;
			UINT flags = D3D12_FORMAT_SUPPORT1_TEXTURE2D;
			if (info.depthStencil)
				flags |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
			else
			{
				flags |= D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_MIP
					| D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
				if (info.format == MTLPixelFormatRGBA8Unorm || info.format == MTLPixelFormatBGRA8Unorm || info.format == MTLPixelFormatR8Unorm)
					flags |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_BLENDABLE;
			}
			support->Support1 = (D3D12_FORMAT_SUPPORT1)flags;
			return S_OK;
		}
		default:
			MTL12_LOG_ONCE("CheckFeatureSupport: feature %d isn't known to mtl12", (int)feature);
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
	*slot = Slot();
	if (!target || !target->texture)
		return;
	const FormatInfo format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? Format(desc->Format) : target->format;
	uint32_t mips = std::max<uint32_t>(1, target->desc.MipLevels);
	if (desc)
	{
		const UINT wanted = volume ? desc->Texture3D.MipLevels : desc->Texture2D.MipLevels;
		if (wanted != UINT32_MAX && wanted)
			mips = std::min<uint32_t>(mips, wanted);
	}
	@autoreleasepool
	{
		slot->texture = target->View(format.format, volume ? MTLTextureType3D : MTLTextureType2D, mips);
	}
	slot->resource = target;
	slot->format = format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

void DeviceImpl::CreateRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	Resource *target = static_cast<Resource *>(resource);
	*slot = Slot();
	if (!target || !target->texture)
		return;
	if (desc && desc->Format != DXGI_FORMAT_UNKNOWN && Format(desc->Format).format != target->format.format)
		MTL12_LOG_ONCE("CreateRenderTargetView: a view of another format than its texture's isn't supported");
	slot->resource = target;
	slot->texture = target->texture;
	slot->format = target->format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

void DeviceImpl::CreateDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *,
	D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	Resource *target = static_cast<Resource *>(resource);
	*slot = Slot();
	if (!target || !target->texture)
		return;
	slot->resource = target;
	slot->texture = target->texture;
	slot->format = target->format.format;
	slot->width = (uint32_t)target->desc.Width;
	slot->height = target->desc.Height;
}

static MTLSamplerAddressMode AddressMode(D3D12_TEXTURE_ADDRESS_MODE mode)
{
	switch (mode)
	{
		case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return MTLSamplerAddressModeRepeat;
		case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
		case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return MTLSamplerAddressModeClampToBorderColor;
		default: return MTLSamplerAddressModeClampToEdge;
	}
}

void DeviceImpl::CreateSampler(const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	Slot *slot = SlotOf(handle);
	*slot = Slot();
	if (!desc)
		return;
	const std::string key(reinterpret_cast<const char *>(desc), sizeof *desc);
	auto found = samplers.find(key);
	if (found != samplers.end())
	{
		slot->sampler = found->second;
		return;
	}

	@autoreleasepool
	{
		// D3D12_FILTER's bits: mip linear (1), mag linear (4), min linear (16), anisotropic (0x55 with 0x40)
		const bool anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc->Filter);
		MTLSamplerDescriptor *info = [[MTLSamplerDescriptor alloc] init];
		info.magFilter = anisotropic || (desc->Filter & 0x4) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		info.minFilter = anisotropic || (desc->Filter & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		info.mipFilter = anisotropic || (desc->Filter & 0x1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
		info.sAddressMode = AddressMode(desc->AddressU);
		info.tAddressMode = AddressMode(desc->AddressV);
		info.rAddressMode = AddressMode(desc->AddressW);
		info.borderColor = MTLSamplerBorderColorTransparentBlack;
		info.maxAnisotropy = anisotropic ? std::min<NSUInteger>(16, std::max<NSUInteger>(1, desc->MaxAnisotropy)) : 1;
		info.lodMinClamp = std::max(0.0f, desc->MinLOD);
		info.lodMaxClamp = std::max(info.lodMinClamp, desc->MaxLOD);
		// Metal's samplers have no level-of-detail bias. The driver never sets one.
		if (desc->MipLODBias != 0.0f)
			MTL12_LOG_ONCE("CreateSampler: a sampler's level-of-detail bias (%g) is not implemented by mtl12", desc->MipLODBias);
		id<MTLSamplerState> sampler = [g.device newSamplerStateWithDescriptor:info];
		if (!sampler)
		{
			Log("CreateSampler: Metal made no sampler");
			return;
		}
		samplers[key] = sampler;
		slot->sampler = sampler;
	}
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
	const FormatInfo format = Format(descs->Format);
	if (format.format == MTLPixelFormatInvalid)
		return result;
	// Direct3D 12 places textures at 4 KB (what a request for small resource alignment gets, when the texture is small
	// enough, which the driver works out before it asks) or at 64 KB, and the driver sorts its placements into those
	// two classes and rounds sizes to them. Nothing is placed in memory here, so the size is the texels', which is what
	// Direct3D reports for the textures the driver makes.
	const UINT64 alignment = descs->Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
		? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	const UINT64 bytes = std::max<UINT64>(1, TextureBytes(*descs, format));
	result.SizeInBytes = (bytes + alignment - 1) / alignment * alignment;
	result.Alignment = alignment;
	return result;
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
		const UINT width = (UINT)std::max<UINT64>(1, desc->Width >> mip);
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

DeviceImpl::~DeviceImpl()
{
	// As Direct3D 12 would: the driver has waited for its own work, and nothing may outlive the device
	if (!WaitIdle(5000) && !completions.lost)
		Log("command buffers were still running when the device was released");
	FlushMessages();
	@autoreleasepool
	{
		for (Resource *image : g.nullImages)
			image->Release();
		g.nullImages.clear();
		samplers.clear();
		ForgetShaders();
		g = Context();
	}
	Log("released the device");
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

	@autoreleasepool
	{
		// The system's default device is the one the display is on. A program without a window may be given none, though
		// the system has devices, so ask for them all then and take the first that isn't a low-power one.
		id<MTLDevice> chosen = MTLCreateSystemDefaultDevice();
		if (!chosen)
		{
			NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
			for (id<MTLDevice> candidate in all)
			{
				if (!chosen || (chosen.lowPower && !candidate.lowPower))
					chosen = candidate;
			}
		}
		if (!chosen)
		{
			Log("no Metal device was found");
			return E_FAIL;
		}
		id<MTLCommandQueue> queue = [chosen newCommandQueue];
		MTLDepthStencilDescriptor *plain = [[MTLDepthStencilDescriptor alloc] init];
		id<MTLDepthStencilState> noDepthStencil = [chosen newDepthStencilStateWithDescriptor:plain];
		if (!queue || !noDepthStencil)
		{
			Log("the Metal device made no command queue");
			return E_FAIL;
		}
		queue.label = @"VibeRant queue";

		g = Context();
		g.device = chosen;
		g.queue = queue;
		g.noDepthStencil = noDepthStencil;
		g.validation = options.validation;
		g.unified = chosen.hasUnifiedMemory;
		g.tiled = [chosen supportsFamily:MTLGPUFamilyApple1];
		{
			std::lock_guard<std::mutex> lock(completions.mutex);
			completions.running.clear();
			completions.lost = false;
		}

		// Metal's validation wraps the device: MTL_DEBUG_LAYER's object checks every call, and MTL_SHADER_VALIDATION's
		// instruments the shaders. Each says which object it wraps, down to the driver's own.
		std::string classes;
		bool apiValidation = false, shaderValidation = false, paravirtual = false;
		id layer = chosen;
		for (int depth = 0; layer && depth < 8; depth++)
		{
			const char *name = NSStringFromClass([layer class]).UTF8String ?: "?";
			if (!classes.empty())
				classes += " > ";
			classes += name;
			// Shader validation's object is MTLGPUDebugDevice, or MTLLegacySVDevice where the GPU's driver has it done the
			// older way (GitHub's virtual GPU, for one)
			if (!strcmp(name, "MTLDebugDevice"))
				apiValidation = true;
			else if (!strcmp(name, "MTLGPUDebugDevice") || !strcmp(name, "MTLLegacySVDevice"))
				shaderValidation = true;
			else if (!strncmp(name, "AppleParavirt", 13))
				paravirtual = true;
			if (![layer respondsToSelector:@selector(baseObject)])
				break;
			layer = [(id<RRMtl12ToolsObject>)layer baseObject];
		}

		snprintf(g.description, sizeof g.description, "%s, Metal", chosen.name.UTF8String ?: "unnamed device");
		Log("device: %s (%s), %s memory, depth-stencil D32FS8, working set %.0f MB, API validation %s, shader validation %s%s",
			g.description, classes.c_str(), g.unified ? "unified" : "separate",
			chosen.recommendedMaxWorkingSetSize / 1048576.0, apiValidation ? "on" : "off", shaderValidation ? "on" : "off",
			options.validation ? ", command buffer errors in detail" : "");
		if (options.software)
			MTL12_LOG_ONCE("a CPU device was asked for, and Metal has none; using the GPU");
		if (paravirtual)
		{
			// The GPU of a macOS virtual machine (GitHub's runners, too). Its driver writes each encoder's commands into
			// chunks of 131,072 bytes, and when an encoder ends with its last chunk within four bytes of full, the next
			// encoder made on the command buffer stops the program at an assertion of the driver's own
			// (-[AppleParavirtCommandBuffer endCurrentChunk]: it tests the first command of the new encoder against the
			// old chunk's fill). In a race that was one frame in some thousands. An encoder that never fills a chunk
			// can't end that way, so here no pass gets more draws, and no blit encoder more copies, than fit one with
			// room to spare: measured on macOS 15.8 with shader validation, a draw with the state set for it takes
			// at most about 1,200 bytes and a copy 200.
			g.encoderDraws = 48;
			g.encoderCopies = 96;
			Log("the paravirtual GPU's driver can't end an encoder on a full chunk of commands, so passes end after %u draws and blit encoders after %u copies",
				g.encoderDraws, g.encoderCopies);
		}

		*device = new DeviceImpl;
		// Without a recommendation there is nothing to keep under
		if (memory && chosen.recommendedMaxWorkingSetSize)
			*memory = new MemoryAdapter;
	}
	return S_OK;
}

const char *DeviceDescription() { return g.description; }
void *Device() { return (__bridge void *)g.device; }

} // namespace rr_mtl12
