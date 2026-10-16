// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_core.cpp
/// \brief [UWP]: VibeRant D3D12's device, queue, command list, fences, deferred releases and CPU descriptor heaps
///
/// D3D11 kept whatever the GPU still used alive; D3D12 doesn't. Everything here assumes one queue, so fence values
/// complete in order: an allocator or retired object tagged with a value is free once the fence reaches it.

#include "d3d12_core.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "../../doomdef.h"
#include "../../i_system.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

namespace rr_d3d12
{
namespace
{

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_event = nullptr;
ComPtr<IDXGIAdapter3> g_memoryAdapter;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap;
UINT g_rtvStep = 0, g_dsvStep = 0;
UINT64 g_frequency = 0;
bool g_uma = false;
bool g_forceWarp = false;
bool g_dred = false;
HRESULT g_dredResult = S_OK;

UINT64 g_lastSubmitted = 0; // the value the last submission signals
UINT64 g_lastSignaled = 0; // the last value whose Signal reached the queue
bool g_signalFailed = false;
bool g_listOpen = false;

// Allocators of submitted lists, oldest first. One is recording (g_current).
struct Allocator
{
	ComPtr<ID3D12CommandAllocator> allocator;
	UINT64 fence;
};
std::deque<Allocator> g_allocators;
ComPtr<ID3D12CommandAllocator> g_current;
size_t g_allocatorCount = 0;
constexpr size_t kAllocatorCap = 8; // Present keeps two frames in flight, plus mid-frame submissions

struct Retired
{
	UINT64 fence;
	ComPtr<IUnknown> object;
};
std::deque<Retired> g_retired;

std::vector<D3D12_RESOURCE_BARRIER> g_barriers;
void (*g_listBegin)(void) = nullptr;

int g_errors = 0;
bool g_removed = false, g_removalLogged = false, g_timeoutLogged = false;
CoreStats g_stats;

double PreciseToMs(precise_t t)
{
	return t * 1000.0 / I_GetPrecisePrecision();
}

// At most one line per second, with suppressed events counted
void WarnStall(const char *what, double ms, UINT64 value, UINT64 done)
{
	static precise_t lastWarning = 0;
	static bool warned = false;
	static UINT64 suppressed = 0;
	const precise_t now = I_GetPreciseTime();
	if (warned && now - lastWarning < I_GetPrecisePrecision())
	{
		suppressed++;
		return;
	}
	Core_Log("gave up waiting for %s after %.1f ms (fence %llu, GPU at %llu); %llu stall warnings suppressed", what, ms,
		(unsigned long long)value, (unsigned long long)done, (unsigned long long)suppressed);
	lastWarning = now;
	warned = true;
	suppressed = 0;
}

void MarkRemoved(const char *what)
{
	if (!g_removed)
		Core_Log("the device was removed while waiting for %s", what);
	g_removed = true;
}

const char *DescribeRemoval(HRESULT reason)
{
	switch (reason)
	{
		case DXGI_ERROR_DEVICE_HUNG: return "hung: a command took too long or was invalid";
		case DXGI_ERROR_DEVICE_REMOVED: return "removed";
		case DXGI_ERROR_DEVICE_RESET: return "reset: a badly formed command";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "driver internal error";
		case DXGI_ERROR_INVALID_CALL: return "invalid call";
		case S_OK: return "not removed";
		default: return "other";
	}
}

const char *DescribeBreadcrumb(D3D12_AUTO_BREADCRUMB_OP op)
{
	static const char *const names[] =
	{
		"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
		"CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
		"ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
		"ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission",
	};
	return (unsigned)op < sizeof names / sizeof *names ? names[op] : "other";
}

const char *DebugName(const char *a, const wchar_t *w, char *buffer, int size)
{
	if (a)
		return a;
	if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, buffer, size, nullptr, nullptr))
		return buffer;
	return "(unnamed)";
}

void EnableDred()
{
	// Must happen before the device is made. Optional: on Xbox it may not exist.
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
	g_dredResult = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
	g_dred = SUCCEEDED(g_dredResult) && settings;
	if (g_dred)
	{
		settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	}
}

// DRED's record of which operations of each command list finished: for a removed device, and for a wait that timed out
void LogBreadcrumbs(ID3D12DeviceRemovedExtendedData *dred)
{
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		int nodes = 0, unfinished = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE *node = breadcrumbs.pHeadAutoBreadcrumbNode; node && nodes < 32;
			node = node->pNext, nodes++)
		{
			const UINT32 count = node->BreadcrumbCount;
			const UINT32 last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			if (last >= count || !node->pCommandHistory || unfinished >= 4)
				continue;
			unfinished++;
			char listName[64], queueName[64];
			Core_Log("DRED: list '%s' on '%s' stopped after %u of %u operations",
				DebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW, listName, sizeof listName),
				DebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW, queueName, sizeof queueName),
				(unsigned)last, (unsigned)count);
			// The last five finished operations and the one that didn't
			for (UINT32 i = last >= 5 ? last - 5 : 0; i <= last && i < count; i++)
				Core_Log("DRED:   %s %u %s", i == last ? "->" : "  ", (unsigned)i, DescribeBreadcrumb(node->pCommandHistory[i]));
		}
		Core_Log("DRED: %d breadcrumb lists, %d unfinished", nodes, unfinished);
	}
	else
		Core_Log("DRED: no breadcrumbs");
}

// The first timed-out wait of a run says which operations the GPU hadn't finished, as a removed device would
void LogTimeout(const char *what, double ms)
{
	if (g_timeoutLogged || !g_device || !g_dred)
		return;
	g_timeoutLogged = true;
	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(g_device.As(&dred)))
		return;
	Core_Log("GPU wait for %s timed out after %.0f ms; breadcrumbs:", what, ms);
	LogBreadcrumbs(dred.Get());
}

void LogCaps(const DXGI_ADAPTER_DESC1 &desc)
{
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {4, levels, D3D_FEATURE_LEVEL_11_0};
	const bool flKnown = SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl));

	// E_INVALIDARG means a runtime that doesn't know the model, so step down
	D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
	bool smKnown = false;
	for (unsigned version = 0x64; version >= 0x51; version = version == 0x60 ? 0x51 : version - 1)
	{
		sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(version);
		const HRESULT hr = g_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
		if (SUCCEEDED(hr))
		{
			smKnown = true;
			break;
		}
		if (hr != E_INVALIDARG)
			break;
	}

	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	const bool optionsKnown = SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options));
	D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
	const bool archKnown = SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch));
	g_uma = archKnown && arch.UMA;
	D3D12_FEATURE_DATA_ROOT_SIGNATURE root = {D3D_ROOT_SIGNATURE_VERSION_1_1};
	HRESULT rootResult = g_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof root);
	if (FAILED(rootResult))
	{
		root.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		rootResult = g_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof root);
	}
	D3D12_FEATURE_DATA_FORMAT_SUPPORT color = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
	D3D12_FEATURE_DATA_FORMAT_SUPPORT depth = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
	g_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &color, sizeof color);
	g_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &depth, sizeof depth);

	char name[128] = {};
	if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr))
		snprintf(name, sizeof name, "(unknown)");
	Core_Log("device: adapter '%s' %04X:%04X LUID %08X:%08X, FL %s0x%X, SM %s0x%X, binding tier %s%u, heap tier %s%u, UMA %s%u, cache-coherent UMA %s%u, root signature %s0x%X; RGBA8 support 0x%X, D24S8 support 0x%X",
		name, desc.VendorId, desc.DeviceId, (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart,
		flKnown ? "" : "unknown ", flKnown ? (unsigned)fl.MaxSupportedFeatureLevel : 0,
		smKnown ? "" : "unknown ", smKnown ? (unsigned)sm.HighestShaderModel : 0,
		optionsKnown ? "" : "unknown ", (unsigned)options.ResourceBindingTier,
		optionsKnown ? "" : "unknown ", (unsigned)options.ResourceHeapTier,
		archKnown ? "" : "unknown ", (unsigned)arch.UMA,
		archKnown ? "" : "unknown ", (unsigned)arch.CacheCoherentUMA,
		SUCCEEDED(rootResult) ? "" : "unknown ", SUCCEEDED(rootResult) ? (unsigned)root.HighestVersion : 0,
		(unsigned)color.Support1, (unsigned)depth.Support1);
}

void CountSubmission(const char *reason)
{
	g_stats.submissions++;
	if (!strcmp(reason, "present"))
		g_stats.presentSubmissions++;
	else if (!strcmp(reason, "ring"))
		g_stats.ringSubmissions++;
	else if (!strcmp(reason, "readback"))
		g_stats.readbackSubmissions++;
	else if (!strcmp(reason, "upload"))
		g_stats.uploadSubmissions++;
	else
		g_stats.otherSubmissions++;
}

// The oldest submitted allocator, reset, if its list has finished
ComPtr<ID3D12CommandAllocator> ReuseOldest()
{
	ComPtr<ID3D12CommandAllocator> allocator;
	if (g_allocators.empty() || !Core_FenceDone(g_allocators.front().fence))
		return allocator;
	allocator = std::move(g_allocators.front().allocator);
	g_allocators.pop_front();
	if (Core_Failed(allocator->Reset(), "ID3D12CommandAllocator::Reset"))
	{
		allocator.Reset();
		g_allocatorCount--;
	}
	return allocator;
}

// Never resets an allocator whose list the GPU may still be executing
ComPtr<ID3D12CommandAllocator> TakeAllocator()
{
	ComPtr<ID3D12CommandAllocator> allocator = ReuseOldest();
	if (allocator)
		return allocator;

	if (g_allocatorCount >= kAllocatorCap && !g_allocators.empty())
	{
		// The GPU is behind: wait for the oldest list rather than making more
		if (Core_Wait(g_allocators.front().fence, 500, "a command allocator"))
		{
			allocator = ReuseOldest();
			if (allocator)
				return allocator;
		}
	}

	if (Core_Failed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
		"CreateCommandAllocator"))
		return nullptr;
	g_allocatorCount++;
	g_stats.allocatorsCreated++;
	if (g_allocatorCount > kAllocatorCap)
		D3D12_LOG_ONCE("made command allocator %u, over the cap of %u, since the GPU didn't finish in time",
			(unsigned)g_allocatorCount, (unsigned)kAllocatorCap);
	return allocator;
}

// Opens the list on a free allocator, then lets the driver bind its targets again
bool Reopen()
{
	if (!g_current)
		g_current = TakeAllocator();
	if (!g_current)
		return false;
	if (Core_Failed(g_list->Reset(g_current.Get(), nullptr), "ID3D12GraphicsCommandList::Reset"))
		return false;
	g_listOpen = true;
	Core_CollectGarbage();
	if (g_listBegin)
		g_listBegin();
	return true;
}

template <typename T>
void Leak(ComPtr<T> &object)
{
	(void)object.Detach();
}

} // namespace

void Core_Log(const char *format, ...)
{
	char message[2048];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	I_OutputMsg("UWP D3D12: %s\n", message);
}

bool Core_Failed(HRESULT hr, const char *what)
{
	if (SUCCEEDED(hr))
		return false;

	g_errors++;
	if (g_errors <= 50 || g_errors % 1000 == 0)
	{
		const HRESULT reason = g_device ? g_device->GetDeviceRemovedReason() : S_OK;
		Core_Log("%s failed: 0x%08X (device removed reason 0x%08X; error %d)", what, (unsigned)hr, (unsigned)reason, g_errors);
	}
	return true;
}

int Core_Errors()
{
	return g_errors;
}

void Core_ForceWarp(bool force)
{
	g_forceWarp = force;
}

bool Core_Init(ID3D11Device *angle)
{
	if (g_device)
		return true;

	Core_Log("starting VibeRant's Direct3D 12 backend");

	ComPtr<IDXGIFactory4> factory;
	if (Core_Failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
		return false;

	// Sharing needs the adapter ANGLE draws the window with
	ComPtr<IDXGIAdapter1> adapter;
	if (angle)
	{
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> angleAdapter;
		DXGI_ADAPTER_DESC angleDesc = {};
		if (Core_Failed(angle->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "QueryInterface IDXGIDevice (ANGLE)")
			|| Core_Failed(dxgiDevice->GetAdapter(&angleAdapter), "GetAdapter (ANGLE)")
			|| Core_Failed(angleAdapter->GetDesc(&angleDesc), "GetDesc (ANGLE)")
			|| Core_Failed(factory->EnumAdapterByLuid(angleDesc.AdapterLuid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid"))
			return false;
	}
	else if (g_forceWarp)
	{
		Core_Log("no ANGLE device: using WARP for testing");
		if (Core_Failed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter"))
			return false;
	}
	else
	{
		Core_Log("no ANGLE device: using the first hardware adapter, so frames can only be read back");
		for (UINT i = 0;; i++)
		{
			ComPtr<IDXGIAdapter1> candidate;
			if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_ADAPTER_DESC1 candidateDesc = {};
			if (FAILED(candidate->GetDesc1(&candidateDesc)) || (candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				continue;
			adapter = candidate;
			break;
		}
		if (!adapter)
		{
			Core_Log("no hardware adapter");
			return false;
		}
	}
	DXGI_ADAPTER_DESC1 desc = {};
	adapter->GetDesc1(&desc);

	EnableDred();
	if (Core_Failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice"))
	{
		g_device.Reset();
		return false;
	}
	if (FAILED(adapter.As(&g_memoryAdapter)))
		Core_Log("video memory budgets unavailable (IDXGIAdapter3 not supported)");
	LogCaps(desc);

	D3D12_COMMAND_QUEUE_DESC queue = {};
	queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queue.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
	rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv.NumDescriptors = kRtvCount;
	D3D12_DESCRIPTOR_HEAP_DESC dsv = {};
	dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsv.NumDescriptors = kDsvCount;
	g_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	if (Core_Failed(g_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue")
		|| Core_Failed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence")
		|| Core_Failed(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap (RTV)")
		|| Core_Failed(g_device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&g_dsvHeap)), "CreateDescriptorHeap (DSV)")
		|| !g_event)
	{
		if (!g_event)
			Core_Log("CreateEventEx failed (%lu)", (unsigned long)GetLastError());
		Core_Shutdown(true); // nothing was submitted
		return false;
	}
	g_rtvStep = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g_dsvStep = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	if (Core_Failed(g_queue->GetTimestampFrequency(&g_frequency), "GetTimestampFrequency"))
		g_frequency = 0;
	g_queue->SetName(L"VibeRant queue");
	g_fence->SetName(L"VibeRant fence");

	g_current = TakeAllocator();
	if (!g_current || Core_Failed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_current.Get(), nullptr,
		IID_PPV_ARGS(&g_list)), "CreateCommandList"))
	{
		Core_Shutdown(true);
		return false;
	}
	g_list->SetName(L"VibeRant frame");
	g_listOpen = true; // CreateCommandList opens it

	char dred[64];
	if (g_dred)
		snprintf(dred, sizeof dred, "on");
	else
		snprintf(dred, sizeof dred, "unavailable (0x%08X)", (unsigned)g_dredResult);
	Core_Log("queue, fence and command list ready; timestamp frequency %llu; DRED %s", (unsigned long long)g_frequency, dred);

	if (g_listBegin)
		g_listBegin();
	return true;
}

void Core_Shutdown(bool idle)
{
	if (!g_device)
		return;

	g_barriers.clear();
	if (!idle && !Core_DeviceRemoved())
	{
		// Releasing what a running list uses is undefined; at exit, keeping it costs nothing
		Core_Log("keeping the D3D12 objects until exit: the GPU didn't finish (fence %llu of %llu)",
			(unsigned long long)Core_CompletedFence(), (unsigned long long)g_lastSignaled);
		for (Retired &retired : g_retired)
			Leak(retired.object);
		for (Allocator &allocator : g_allocators)
			Leak(allocator.allocator);
		g_retired.clear();
		g_allocators.clear();
		Leak(g_current);
		Leak(g_list);
		Leak(g_rtvHeap);
		Leak(g_dsvHeap);
		Leak(g_fence);
		Leak(g_queue);
		Leak(g_device);
		g_event = nullptr; // a pending SetEventOnCompletion may still use it
	}
	else
	{
		g_retired.clear();
		g_list.Reset();
		g_current.Reset();
		g_allocators.clear();
		g_rtvHeap.Reset();
		g_dsvHeap.Reset();
		g_fence.Reset();
		g_queue.Reset();
		g_device.Reset(); // after everything made from it
		if (g_event)
			CloseHandle(g_event);
		g_event = nullptr;
		Core_Log("released the device");
	}
	g_memoryAdapter.Reset();
	g_allocatorCount = 0;
	g_lastSubmitted = g_lastSignaled = 0;
	g_signalFailed = false;
	g_listOpen = false;
	g_removed = g_removalLogged = g_timeoutLogged = false;
}

ID3D12Device *Core_Device()
{
	return g_device.Get();
}

ID3D12CommandQueue *Core_Queue()
{
	return g_queue.Get();
}

ID3D12GraphicsCommandList *Core_List()
{
	return g_listOpen ? g_list.Get() : nullptr;
}

IDXGIAdapter3 *Core_MemoryAdapter()
{
	return g_memoryAdapter.Get();
}

UINT64 Core_TimestampFrequency()
{
	return g_frequency;
}

bool Core_UMA()
{
	return g_uma;
}

void Core_SetListBegin(void (*callback)(void))
{
	g_listBegin = callback;
}

UINT64 Core_Submit(const char *reason)
{
	if (!g_list)
		return 0;
	if (!g_listOpen)
	{
		// A failed Reset left it closed: try again, without anything to execute
		Reopen();
		return 0;
	}

	Core_FlushBarriers();
	CountSubmission(reason);
	UINT64 value = 0;
	const HRESULT closed = g_list->Close();
	g_listOpen = false;
	if (!Core_Failed(closed, "ID3D12GraphicsCommandList::Close"))
	{
		ID3D12CommandList *lists[] = {g_list.Get()};
		g_queue->ExecuteCommandLists(1, lists);
		value = ++g_lastSubmitted;
		if (Core_Failed(g_queue->Signal(g_fence.Get(), value), "ID3D12CommandQueue::Signal"))
			g_signalFailed = true; // later signals still cover this list, as the queue runs in order
		else
			g_lastSignaled = value;
		g_allocators.push_back({std::move(g_current), value});
	}
	else if (g_current && Core_Failed(g_current->Reset(), "ID3D12CommandAllocator::Reset (unexecuted)"))
	{
		// Nothing ran, so the allocator only held this list's commands; if it can't be reset, drop it
		g_current.Reset();
		g_allocatorCount--;
	}
	Reopen();
	return value;
}

UINT64 Core_PendingFence()
{
	return g_lastSubmitted + 1;
}

UINT64 Core_CompletedFence()
{
	if (!g_fence)
		return 0;
	const UINT64 done = g_fence->GetCompletedValue();
	if (done == UINT64_MAX)
		g_removed = true;
	return done;
}

bool Core_FenceDone(UINT64 value)
{
	return Core_CompletedFence() >= value;
}

bool Core_Wait(UINT64 value, DWORD ms, const char *what, double *waitedMs)
{
	if (waitedMs)
		*waitedMs = 0.0;
	if (!g_fence)
		return false;

	UINT64 done = g_fence->GetCompletedValue();
	if (done == UINT64_MAX)
	{
		MarkRemoved(what);
		return false;
	}
	if (done >= value)
		return true;
	if (value > g_lastSignaled)
	{
		// Nothing will ever signal it, so don't block
		D3D12_LOG_ONCE("not waiting for %s: fence %llu was never signaled (last %llu)", what,
			(unsigned long long)value, (unsigned long long)g_lastSignaled);
		return false;
	}

	const precise_t start = I_GetPreciseTime();
	bool reached = false;
	for (;;)
	{
		const double elapsed = PreciseToMs(I_GetPreciseTime() - start);
		if (elapsed >= ms)
			break;
		// An earlier wait that timed out may still set the event, so check the fence after every wake
		if (!g_event || FAILED(g_fence->SetEventOnCompletion(value, g_event)))
			Sleep(1);
		else
			WaitForSingleObjectEx(g_event, (DWORD)std::max(1.0, std::ceil(ms - elapsed)), FALSE);
		done = g_fence->GetCompletedValue();
		if (done == UINT64_MAX)
		{
			MarkRemoved(what);
			break;
		}
		if (done >= value)
		{
			reached = true;
			break;
		}
	}

	const double waited = PreciseToMs(I_GetPreciseTime() - start);
	if (waitedMs)
		*waitedMs = waited;
	g_stats.waits++;
	g_stats.waitMs += waited;
	g_stats.worstWaitMs = std::max(g_stats.worstWaitMs, waited);
	if (!reached && !g_removed)
	{
		g_stats.waitTimeouts++;
		WarnStall(what, waited, value, done);
		LogTimeout(what, waited);
	}
	return reached;
}

bool Core_WaitIdle(DWORD ms, const char *what)
{
	if (!g_fence || Core_DeviceRemoved())
		return true;
	if (g_signalFailed)
	{
		// Lists after the last signal may still be running, with nothing to wait for
		Core_Log("can't confirm the GPU is idle for %s: a fence signal failed", what);
		return false;
	}
	if (g_lastSignaled == 0)
		return true;
	return Core_Wait(g_lastSignaled, ms, what) || Core_DeviceRemoved();
}

bool Core_DeviceRemoved()
{
	if (!g_device)
		return false;
	if (!g_removed && FAILED(g_device->GetDeviceRemovedReason()))
		g_removed = true;
	return g_removed;
}

bool Core_RemovalSeen()
{
	return g_removed;
}

HRESULT Core_RemovedReason()
{
	return g_device ? g_device->GetDeviceRemovedReason() : S_OK;
}

void Core_LogDeviceRemoved()
{
	if (!g_device || g_removalLogged)
		return;
	g_removalLogged = true;

	const HRESULT reason = g_device->GetDeviceRemovedReason();
	Core_Log("the device was removed: 0x%08X (%s); fence %llu of %llu", (unsigned)reason, DescribeRemoval(reason),
		(unsigned long long)Core_CompletedFence(), (unsigned long long)g_lastSignaled);
	if (!g_dred)
	{
		Core_Log("DRED: unavailable (0x%08X)", (unsigned)g_dredResult);
		return;
	}

	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (Core_Failed(g_device.As(&dred), "QueryInterface ID3D12DeviceRemovedExtendedData"))
		return;

	LogBreadcrumbs(dred.Get());

	D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
	{
		Core_Log("DRED: page fault at GPU address 0x%llX", (unsigned long long)fault.PageFaultVA);
		int count = 0;
		char name[64];
		for (const D3D12_DRED_ALLOCATION_NODE *node = fault.pHeadExistingAllocationNode; node && count < 8; node = node->pNext, count++)
			Core_Log("DRED:   existing allocation '%s' (type %d)", DebugName(node->ObjectNameA, node->ObjectNameW, name, sizeof name),
				(int)node->AllocationType);
		count = 0;
		for (const D3D12_DRED_ALLOCATION_NODE *node = fault.pHeadRecentFreedAllocationNode; node && count < 8; node = node->pNext, count++)
			Core_Log("DRED:   recently freed allocation '%s' (type %d)", DebugName(node->ObjectNameA, node->ObjectNameW, name, sizeof name),
				(int)node->AllocationType);
	}
	else
		Core_Log("DRED: no page fault");
}

void Core_Retire(ComPtr<IUnknown> object)
{
	if (object)
		g_retired.push_back({Core_PendingFence(), std::move(object)});
}

void Core_CollectGarbage()
{
	if (g_retired.empty())
		return;
	const UINT64 done = Core_CompletedFence();
	while (!g_retired.empty() && g_retired.front().fence <= done)
		g_retired.pop_front();
}

size_t Core_PendingReleases()
{
	return g_retired.size();
}

void Core_Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (!resource || before == after)
		return;
	// A resource may appear once in a ResourceBarrier call, so a transition back, queued before anything used the
	// resource in its new state, replaces the one pending: A->B then B->C is A->C, and A->B then B->A is nothing
	for (size_t i = g_barriers.size(); i-- > 0;)
	{
		D3D12_RESOURCE_BARRIER &pending = g_barriers[i];
		if (pending.Transition.pResource != resource)
			continue;
		if (pending.Transition.StateAfter == before)
		{
			if (pending.Transition.StateBefore == after)
				g_barriers.erase(g_barriers.begin() + (std::ptrdiff_t)i);
			else
				pending.Transition.StateAfter = after;
			return;
		}
		break;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	g_barriers.push_back(barrier);
}

void Core_FlushBarriers()
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (list && !g_barriers.empty())
	{
		list->ResourceBarrier((UINT)g_barriers.size(), g_barriers.data());
		g_stats.barriers += (int)g_barriers.size();
	}
	g_barriers.clear();
}

D3D12_CPU_DESCRIPTOR_HANDLE Core_Rtv(UINT index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
	if (g_rtvHeap && index < kRtvCount)
	{
		handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += (SIZE_T)index * g_rtvStep;
	}
	return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Core_Dsv(UINT index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
	if (g_dsvHeap && index < kDsvCount)
	{
		handle = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += (SIZE_T)index * g_dsvStep;
	}
	return handle;
}

CoreStats Core_TakeStats()
{
	const CoreStats stats = g_stats;
	g_stats = CoreStats();
	return stats;
}

size_t Core_Allocators()
{
	return g_allocatorCount;
}

} // namespace rr_d3d12
