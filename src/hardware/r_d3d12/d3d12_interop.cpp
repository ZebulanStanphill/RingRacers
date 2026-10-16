// DR. ROBOTNIK'S RING RACERS
// This program is free software distributed under the terms of the GNU GPL v2.
// See the 'LICENSE' file for more details.
/// \brief [UWP]: Diagnostic D3D12/ANGLE sharing probe; no renderer selection or presentation changes.

#include "d3d12_interop.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace rr_d3d12
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kSize = 64;
constexpr unsigned kTexture2D = 0x0DE1, kTextureBinding2D = 0x8069;
constexpr unsigned kD3DTexture = 0x33A3, kShareHandle = 0x3200;
constexpr int kBackBuffer = 0x3084;
constexpr int kWidth = 0x3057, kHeight = 0x3056, kNone = 0x3038;
constexpr unsigned kBadAttribute = 0x3004;
constexpr int kTextureFormat = 0x3080, kTextureRgba = 0x305E;
constexpr int kTextureTarget = 0x3081, kEglTexture2D = 0x305F;

void Log(const EglApi &egl, const char *format, ...)
{
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	if (egl.LogMessage)
		egl.LogMessage(message);
}

bool Check(const EglApi &egl, const char *scope, const char *operation, HRESULT hr)
{
	Log(egl, "%s: %s 0x%08X", scope, operation, (unsigned)hr);
	return SUCCEEDED(hr);
}

template <typename T>
bool CheckObject(const EglApi &egl, const char *scope, const char *operation, HRESULT hr, const ComPtr<T> &object)
{
	if (!Check(egl, scope, operation, hr))
		return false;
	if (object.Get())
		return true;
	Log(egl, "%s: %s returned a null interface (0x%08X)", scope, operation, (unsigned)E_POINTER);
	return false;
}

bool ValidHandle(const EglApi &egl, const char *scope, HANDLE handle)
{
	if (handle && handle != INVALID_HANDLE_VALUE)
		return true;
	Log(egl, "%s: null shared handle (0x%08X)", scope, (unsigned)E_HANDLE);
	return false;
}

struct Handle
{
	HANDLE value = nullptr;
	~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// Poll rather than registering an event that might remain outstanding after a timeout.
bool WaitFence(const EglApi &egl, ID3D12Fence *fence, UINT64 value, DWORD limit, const char *scope)
{
	if (!fence)
	{
		Log(egl, "%s: null fence (0x%08X)", scope, (unsigned)E_POINTER);
		return false;
	}
	const ULONGLONG start = GetTickCount64();
	for (;;)
	{
		const UINT64 done = fence->GetCompletedValue();
		if (done == UINT64_MAX)
		{
			Log(egl, "%s: fence device removed", scope);
			return false;
		}
		if (done >= value)
			return true;
		if (GetTickCount64() - start >= limit)
		{
			Log(egl, "%s: fence timeout after %lu ms", scope, (unsigned long)limit);
			return false;
		}
		Sleep(1);
	}
}

struct TexturePair
{
	ComPtr<ID3D12Resource> resource;
	ComPtr<ID3D11Texture2D> texture, legacy, staging;
	ComPtr<ID3D11Query> copied;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12DescriptorHeap> rtv;
};

struct ProbeState
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> fence, sharedFence, reverseFence;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Fence> angleFence, angleReverseFence;
	TexturePair paths[3];
	UINT64 nextFence = 0;
	bool outstanding = false;
};

D3D11_TEXTURE2D_DESC TextureDesc(UINT misc)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = desc.Height = kSize;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = misc;
	return desc;
}

bool OpenTexture(ProbeState &state, ID3D11Device *angle, const EglApi &egl,
	TexturePair &pair, bool angleOwned, const char *scope)
{
	Handle handle;
	if (angleOwned)
	{
		const auto desc = TextureDesc(D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED);
		ComPtr<IDXGIResource1> dxgi;
		if (!CheckObject(egl, scope, "CreateTexture2D (NT)", angle->CreateTexture2D(&desc, nullptr, &pair.texture), pair.texture)
			|| !CheckObject(egl, scope, "QI IDXGIResource1", pair.texture.As(&dxgi), dxgi)
			|| !Check(egl, scope, "CreateSharedHandle", dxgi->CreateSharedHandle(nullptr,
				DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle.value)))
			return false;
		if (!ValidHandle(egl, scope, handle.value))
			return false;
		return CheckObject(egl, scope, "OpenSharedHandle", state.device->OpenSharedHandle(handle.value, IID_PPV_ARGS(&pair.resource)), pair.resource);
	}

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = desc.Height = kSize;
	desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ComPtr<ID3D11Device1> device1;
	if (!CheckObject(egl, scope, "CreateCommittedResource (shared)", state.device->CreateCommittedResource(&heap,
		D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pair.resource)), pair.resource)
		|| !Check(egl, scope, "CreateSharedHandle", state.device->CreateSharedHandle(pair.resource.Get(), nullptr,
			GENERIC_ALL, nullptr, &handle.value))
		|| !CheckObject(egl, scope, "QI Device1", angle->QueryInterface(IID_PPV_ARGS(&device1)), device1))
		return false;
	if (!ValidHandle(egl, scope, handle.value))
		return false;
	return CheckObject(egl, scope, "OpenSharedResource1", device1->OpenSharedResource1(handle.value, IID_PPV_ARGS(&pair.texture)), pair.texture);
}

bool Clear(ProbeState &state, TexturePair &pair, const EglApi &egl, const char *scope,
	const float *color, ID3D12Fence *signalFence = nullptr, UINT64 signalValue = 0)
{
	// New recording objects each time; never reset an allocator that might still be executing.
	pair.list.Reset();
	pair.allocator.Reset();
	pair.rtv.Reset();
	D3D12_DESCRIPTOR_HEAP_DESC heap = {};
	heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heap.NumDescriptors = 1;
	if (!CheckObject(egl, scope, "CreateDescriptorHeap", state.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pair.rtv)), pair.rtv)
		|| !CheckObject(egl, scope, "CreateCommandAllocator", state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&pair.allocator)), pair.allocator)
		|| !CheckObject(egl, scope, "CreateCommandList", state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			pair.allocator.Get(), nullptr, IID_PPV_ARGS(&pair.list)), pair.list))
		return false;
	const auto rtv = pair.rtv->GetCPUDescriptorHandleForHeapStart();
	state.device->CreateRenderTargetView(pair.resource.Get(), nullptr, rtv);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = pair.resource.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	pair.list->ResourceBarrier(1, &barrier);
	pair.list->ClearRenderTargetView(rtv, color, 0, nullptr);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	pair.list->ResourceBarrier(1, &barrier);
	if (!Check(egl, scope, "Close", pair.list->Close()))
		return false;
	ID3D12CommandList *lists[] = {pair.list.Get()};
	state.queue->ExecuteCommandLists(1, lists);
	const UINT64 value = ++state.nextFence;
	if (!Check(egl, scope, "Signal (CPU)", state.queue->Signal(state.fence.Get(), value))
		|| !WaitFence(egl, state.fence.Get(), value, 500, scope))
	{
		state.outstanding = true;
		return false;
	}
	if (signalFence)
	{
		if (!Check(egl, scope, "Signal (shared)", state.queue->Signal(signalFence, signalValue))
			|| !WaitFence(egl, signalFence, signalValue, 100, scope))
		{
			state.outstanding = true;
			return false;
		}
	}
	return true;
}

bool ReadPixel(ProbeState &state, ID3D11Device *angle, TexturePair &pair,
	const EglApi &egl, const char *scope, UINT expected)
{
	if (!pair.staging)
	{
		auto desc = TextureDesc(0);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		const D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
		if (!CheckObject(egl, scope, "CreateTexture2D (staging)", angle->CreateTexture2D(&desc, nullptr, &pair.staging), pair.staging)
			|| !CheckObject(egl, scope, "CreateQuery", angle->CreateQuery(&query, &pair.copied), pair.copied))
			return false;
	}
	if (pair.legacy)
		state.context->CopyResource(pair.legacy.Get(), pair.texture.Get());
	state.context->CopyResource(pair.staging.Get(), pair.legacy ? pair.legacy.Get() : pair.texture.Get());
	state.context->End(pair.copied.Get());
	state.context->Flush();
	const ULONGLONG start = GetTickCount64();
	for (;;)
	{
		BOOL done = FALSE;
		HRESULT hr = state.context->GetData(pair.copied.Get(), &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hr == S_OK && done)
			break;
		if (FAILED(hr) || GetTickCount64() - start >= 100)
		{
			Log(egl, "%s: readback query 0x%08X, %s", scope, (unsigned)hr, FAILED(hr) ? "failed" : "timeout after 100 ms");
			state.outstanding = true;
			return false;
		}
		Sleep(1);
	}
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	const HRESULT hr = state.context->Map(pair.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
	if (!Check(egl, scope, "Map (DO_NOT_WAIT)", hr))
		return false;
	UINT pixel = 0;
	const bool valid = mapped.pData && mapped.RowPitch >= kSize * 4;
	if (valid)
		memcpy(&pixel, mapped.pData, sizeof pixel);
	state.context->Unmap(pair.staging.Get(), 0);
	if (valid && pixel == expected)
	{
		Log(egl, "%s: content ok", scope);
		return true;
	}
	Log(egl, "%s: mismatch got 0x%08X expected 0x%08X%s", scope, pixel, expected, valid ? "" : " (invalid mapping)");
	return false;
}

// RAII restores the caller's GL binding even when creation or eglBindTexImage fails.
struct Pbuffer
{
	const EglApi &egl;
	void *surface = nullptr;
	unsigned texture = 0;
	int previous = 0;
	bool bound = false;
	Pbuffer(const EglApi &api) : egl(api) { egl.GetIntegerv(kTextureBinding2D, &previous); }
	~Pbuffer()
	{
		if (bound)
		{
			egl.BindTexture(kTexture2D, texture);
			if (!egl.ReleaseTexImage(egl.display, surface, kBackBuffer))
				Log(egl, "cleanup: eglReleaseTexImage error 0x%04X", (unsigned)egl.GetError());
		}
		if (surface && !egl.DestroySurface(egl.display, surface))
			Log(egl, "cleanup: eglDestroySurface error 0x%04X", (unsigned)egl.GetError());
		if (texture)
			egl.DeleteTextures(1, &texture);
		egl.BindTexture(kTexture2D, (unsigned)previous);
	}
	bool Create(unsigned type, void *buffer, void *config, const char *scope)
	{
		const int attributes[] = {kWidth, (int)kSize, kHeight, (int)kSize,
			kTextureFormat, kTextureRgba, kTextureTarget, kEglTexture2D, kNone};
		surface = egl.CreatePbufferFromClientBuffer(egl.display, type, buffer, config, attributes);
		unsigned error = (unsigned)egl.GetError();
		if (!surface && type == kD3DTexture && error == kBadAttribute)
		{
			// Older ANGLE builds take the size from the texture and reject EGL_WIDTH/EGL_HEIGHT here
			Log(egl, "%s: eglCreatePbufferFromClientBuffer rejected the size (EGL 0x%04X); retrying without it", scope, error);
			surface = egl.CreatePbufferFromClientBuffer(egl.display, type, buffer, config, attributes + 4);
			error = (unsigned)egl.GetError();
		}
		Log(egl, "%s: eglCreatePbufferFromClientBuffer %s (EGL 0x%04X)", scope, surface ? "ok" : "failed", error);
		if (!surface)
			return false;
		egl.GenTextures(1, &texture);
		if (!texture)
		{
			Log(egl, "%s: glGenTextures failed (GL 0x%04X)", scope, egl.GetGlError());
			return false;
		}
		egl.BindTexture(kTexture2D, texture);
		int current = 0;
		egl.GetIntegerv(kTextureBinding2D, &current);
		if ((unsigned)current != texture)
		{
			Log(egl, "%s: glBindTexture failed (GL 0x%04X)", scope, egl.GetGlError());
			return false;
		}
		bound = egl.BindTexImage(egl.display, surface, kBackBuffer) != 0;
		Log(egl, "%s: eglBindTexImage %s (EGL 0x%04X)", scope, bound ? "ok" : "failed", (unsigned)egl.GetError());
		return bound;
	}
};

void Caps(ProbeState &state, const EglApi &egl, const DXGI_ADAPTER_DESC &desc)
{
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {4, levels, D3D_FEATURE_LEVEL_11_0};
	const bool flKnown = Check(egl, "caps", "FEATURE_LEVELS",
		state.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl));
	D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
	bool smKnown = false;
	for (unsigned version = 0x64; version >= 0x51; version = version == 0x60 ? 0x51 : version - 1)
	{
		sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(version);
		const HRESULT hr = state.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
		if (SUCCEEDED(hr))
		{
			smKnown = true;
			break;
		}
		Log(egl, "caps: SHADER_MODEL 0x%X 0x%08X", version, (unsigned)hr);
		if (hr != E_INVALIDARG)
			break;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	const bool optionsKnown = Check(egl, "caps", "OPTIONS",
		state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options));
	D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
	const bool archKnown = Check(egl, "caps", "ARCHITECTURE",
		state.device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch));
	D3D12_FEATURE_DATA_ROOT_SIGNATURE root = {D3D_ROOT_SIGNATURE_VERSION_1_1};
	HRESULT hr = state.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof root);
	if (FAILED(hr))
	{
		Check(egl, "caps", "ROOT_SIGNATURE 1.1", hr);
		root.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		hr = state.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof root);
	}
	Check(egl, "caps", "ROOT_SIGNATURE", hr);
	char name[128] = {};
	if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr))
		snprintf(name, sizeof name, "(unknown)");
	Log(egl, "device: adapter '%s' %04X:%04X LUID %08X:%08X, FL %s0x%X, SM %s0x%X, binding tier %s%u, heap tier %s%u, UMA %s%u, cache-coherent UMA %s%u, root signature %s0x%X",
		name, desc.VendorId, desc.DeviceId, (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart,
		flKnown ? "" : "unknown ", flKnown ? (unsigned)fl.MaxSupportedFeatureLevel : 0,
		smKnown ? "" : "unknown ", smKnown ? (unsigned)sm.HighestShaderModel : 0,
		optionsKnown ? "" : "unknown ", (unsigned)options.ResourceBindingTier,
		optionsKnown ? "" : "unknown ", (unsigned)options.ResourceHeapTier,
		archKnown ? "" : "unknown ", (unsigned)arch.UMA,
		archKnown ? "" : "unknown ", (unsigned)arch.CacheCoherentUMA,
		SUCCEEDED(hr) ? "" : "unknown ", SUCCEEDED(hr) ? (unsigned)root.HighestVersion : 0);
}

void FenceProbe(ProbeState &state, ID3D11Device *angle, const EglApi &egl, InteropProbe &result, TexturePair *pair)
{
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext4> context4;
	HRESULT hr = angle->QueryInterface(IID_PPV_ARGS(&device5));
	Log(egl, "fence: Device5 %s (0x%08X)", device5 ? "yes" : "no", (unsigned)hr);
	if (!CheckObject(egl, "fence", "QI Context4", state.context.As(&context4), context4) || !device5 || !context4)
		return;
	if (!CheckObject(egl, "fence", "CreateFence (shared)", state.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
		IID_PPV_ARGS(&state.sharedFence)), state.sharedFence))
		return;
	{
		Handle handle;
		if (!Check(egl, "fence", "CreateSharedHandle", state.device->CreateSharedHandle(state.sharedFence.Get(), nullptr,
			GENERIC_ALL, nullptr, &handle.value)))
			return;
		if (!ValidHandle(egl, "fence", handle.value))
			return;
		if (!CheckObject(egl, "fence", "OpenSharedFence", device5->OpenSharedFence(handle.value, IID_PPV_ARGS(&state.angleFence)), state.angleFence))
			return;
	}
	const float second[] = {192.0f / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f, 1.0f};
	// A GPU wait cannot be cancelled. Only enqueue it after the target has completed on the CPU.
	const bool ready = pair ? Clear(state, *pair, egl, "fence", second, state.sharedFence.Get(), 2)
		: Check(egl, "fence", "Signal (shared)", state.queue->Signal(state.sharedFence.Get(), 2))
			&& WaitFence(egl, state.sharedFence.Get(), 2, 100, "fence");
	if (!ready && !pair)
		state.outstanding = true;
	if (ready && state.angleFence->GetCompletedValue() != UINT64_MAX && state.angleFence->GetCompletedValue() >= 2)
	{
		hr = context4->Wait(state.angleFence.Get(), 2);
		if (Check(egl, "fence", "Context4 Wait", hr))
			result.gpuWait = pair && ReadPixel(state, angle, *pair, egl, "fence", 0xFF8040C0);
		Log(egl, "fence: gpu wait %s", result.gpuWait ? "ok" : (pair ? "failed" : "unverified (no shared texture)"));
	}
	else
		Log(egl, "fence: gpu wait skipped (target not completed)");

	// A separate fence for the reverse direction avoids rewinding the D3D12 signal timeline.
	if (!CheckObject(egl, "fence", "CreateFence (reverse)", state.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
		IID_PPV_ARGS(&state.reverseFence)), state.reverseFence))
		return;
	{
		Handle handle;
		if (!Check(egl, "fence", "CreateSharedHandle (reverse)", state.device->CreateSharedHandle(state.reverseFence.Get(),
			nullptr, GENERIC_ALL, nullptr, &handle.value))
			|| !ValidHandle(egl, "fence", handle.value)
			|| !CheckObject(egl, "fence", "OpenSharedFence (reverse)", device5->OpenSharedFence(handle.value,
				IID_PPV_ARGS(&state.angleReverseFence)), state.angleReverseFence))
			return;
	}
	hr = context4->Signal(state.angleReverseFence.Get(), 1);
	if (Check(egl, "fence", "Context4 Signal", hr))
	{
		context4->Flush();
		result.gpuSignal = WaitFence(egl, state.reverseFence.Get(), 1, 100, "fence");
		if (!result.gpuSignal)
			state.outstanding = true;
	}
	Log(egl, "fence: gpu signal %s", result.gpuSignal ? "ok" : "failed");
}

} // namespace

InteropProbe D3D12Interop_Probe(ID3D11Device *angle, const EglApi &egl, EGLConfig config)
{
	const ULONGLONG start = GetTickCount64();
	InteropProbe result;
	// One exit path guarantees the elapsed line even if device creation fails.
	auto run = [&]()
	{
		if (!angle || !config || !egl.display || !egl.CreatePbufferFromClientBuffer || !egl.DestroySurface
			|| !egl.BindTexImage || !egl.ReleaseTexImage || !egl.GetError || !egl.GetIntegerv || !egl.GenTextures
			|| !egl.DeleteTextures || !egl.BindTexture || !egl.GetGlError)
		{
			Log(egl, "device: skipped (missing ANGLE device, config or EGL/GLES functions)");
			return;
		}
		std::unique_ptr<ProbeState> state(new (std::nothrow) ProbeState);
		if (!state)
		{
			Log(egl, "device: probe allocation failed (0x%08X)", (unsigned)E_OUTOFMEMORY);
			return;
		}
		ComPtr<IDXGIDevice> dxgi;
		ComPtr<IDXGIAdapter> angleAdapter;
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<IDXGIFactory4> factory;
		DXGI_ADAPTER_DESC desc = {};
		if (!CheckObject(egl, "device", "QI IDXGIDevice", angle->QueryInterface(IID_PPV_ARGS(&dxgi)), dxgi)
			|| !CheckObject(egl, "device", "GetAdapter", dxgi->GetAdapter(&angleAdapter), angleAdapter)
			|| !Check(egl, "device", "GetDesc", angleAdapter->GetDesc(&desc))
			|| !CheckObject(egl, "device", "CreateDXGIFactory2", CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), factory)
			|| !CheckObject(egl, "device", "EnumAdapterByLuid", factory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&adapter)), adapter)
			|| !CheckObject(egl, "device", "D3D12CreateDevice", D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
				IID_PPV_ARGS(&state->device)), state->device))
			return;
		result.device = true;
		Caps(*state, egl, desc);
		D3D12_COMMAND_QUEUE_DESC queue = {};
		queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!CheckObject(egl, "device", "CreateCommandQueue", state->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&state->queue)), state->queue)
			|| !CheckObject(egl, "device", "CreateFence", state->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence)), state->fence))
			return;
		UINT64 frequency = 0;
		if (Check(egl, "device", "GetTimestampFrequency", state->queue->GetTimestampFrequency(&frequency)))
			Log(egl, "timestamp frequency %llu", (unsigned long long)frequency);
		angle->GetImmediateContext(&state->context);
		if (!state->context)
		{
			Log(egl, "device: missing ANGLE immediate context");
			return;
		}
		TexturePair *fencePair = nullptr;
		const float color[] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
		for (unsigned path = 0; path < 3; path++)
		{
			const char *scope = path == 0 ? "share A" : path == 1 ? "share B" : "share C";
			auto &pair = state->paths[path];
			bool opened = OpenTexture(*state, angle, egl, pair, path == 1, scope);
			if (!opened && path == 2)
			{
				pair = TexturePair();
				Log(egl, "%s: retry ANGLE-owned transport", scope);
				opened = OpenTexture(*state, angle, egl, pair, true, scope);
			}
			if (!opened)
				continue;
			void *buffer = pair.texture.Get();
			if (path == 2)
			{
				const auto desc11 = TextureDesc(D3D11_RESOURCE_MISC_SHARED);
				ComPtr<IDXGIResource> legacy;
				HANDLE handle = nullptr; // Legacy DXGI handles are not NT handles: never CloseHandle.
				if (!CheckObject(egl, scope, "CreateTexture2D (legacy)", angle->CreateTexture2D(&desc11, nullptr, &pair.legacy), pair.legacy)
					|| !CheckObject(egl, scope, "QI IDXGIResource", pair.legacy.As(&legacy), legacy)
					|| !Check(egl, scope, "GetSharedHandle", legacy->GetSharedHandle(&handle)))
					continue;
				if (!ValidHandle(egl, scope, handle))
					continue;
				buffer = handle;
			}
			Pbuffer surface(egl);
			if (!surface.Create(path == 2 ? kShareHandle : kD3DTexture, buffer, config, scope))
				continue;
			result.share[path] = Clear(*state, pair, egl, scope, color)
				&& ReadPixel(*state, angle, pair, egl, scope, 0xFFC08040);
			if (result.share[path] && !fencePair)
				fencePair = &pair;
		}
		// An earlier timeout must not cause us to reset or reuse its recording objects.
		if (!state->outstanding)
			FenceProbe(*state, angle, egl, result, fencePair);
		else
			Log(egl, "fence: skipped (prior GPU timeout)");
		if (state->outstanding)
		{
			// D3D12 does not retain resources used by an in-flight command list. A hung GPU cannot be safely
			// drained within a bounded wait: retain this small one-shot probe allocation until process exit.
			Log(egl, "cleanup: retaining probe resources after GPU timeout to avoid releasing in-flight objects");
			(void)state.release();
		}
	};
	run();
	Log(egl, "result: device %s, share A %s, share B %s, share C %s, gpu wait %s, gpu signal %s",
		result.device ? "yes" : "no", result.share[0] ? "ok" : "failed", result.share[1] ? "ok" : "failed",
		result.share[2] ? "ok" : "failed", result.gpuWait ? "ok" : "unavailable", result.gpuSignal ? "ok" : "unavailable");
	Log(egl, "took %llu ms", (unsigned long long)(GetTickCount64() - start));
	return result;
}

} // namespace rr_d3d12
