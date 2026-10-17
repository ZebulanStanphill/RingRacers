// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_core.h
/// \brief [UWP]: VibeRant D3D12's device, queue, command list, fences, deferred releases and CPU descriptor heaps
///
/// The driver records into one command list, which Core_Submit executes and reopens. Nothing a submitted list uses
/// may be released before its fence completes: release through Core_Retire, which keeps the object until then.

#ifndef RR_D3D12_CORE_H
#define RR_D3D12_CORE_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace rr_d3d12
{

using Microsoft::WRL::ComPtr;

// "UWP D3D12: " and the message, on a line of its own
void Core_Log(const char *format, ...);

// Logs a failed HRESULT with the device's removal reason (the first 50, then every 1000th) and counts it
bool Core_Failed(HRESULT hr, const char *what);
int Core_Errors();

// Logs something the first time it happens
#define D3D12_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; rr_d3d12::Core_Log(__VA_ARGS__); } } while (0)

// Makes the device on the first hardware adapter, its queue, fence and an open command list. Called again after a
// failure, it tries again.
bool Core_Init();

// Test hook: makes Core_Init select WARP rather than a hardware adapter. Set before initialization.
void Core_ForceWarp(bool force);

// Releases everything if the GPU is idle (or the device was removed); otherwise keeps it all until exit, since
// releasing objects a list may still use is undefined. Call after the driver has retired or released its own objects.
void Core_Shutdown(bool idle);

ID3D12Device *Core_Device();
ID3D12CommandQueue *Core_Queue();
ID3D12GraphicsCommandList *Core_List(); // open for recording between submissions
IDXGIAdapter3 *Core_MemoryAdapter(); // for QueryVideoMemoryInfo; may be null
UINT64 Core_TimestampFrequency();
bool Core_UMA();

// Called whenever the list is (re)opened: set the render targets, viewport and scissor again there
void Core_SetListBegin(void (*callback)(void));

// Closes and executes the list, signals the fence and reopens the list. Returns the value the GPU will signal when
// the work is done, or 0 if nothing could be submitted. reason is counted: "present" is the frame's normal
// submission; anything else ("ring", "readback", "upload", "resize") is a mid-frame one.
UINT64 Core_Submit(const char *reason);

// The value the next Core_Submit signals, which covers everything recorded so far
UINT64 Core_PendingFence();
UINT64 Core_CompletedFence(); // UINT64_MAX once the device is removed
bool Core_FenceDone(UINT64 value);

// Waits up to ms for the GPU to reach value. Never waits for a value that hasn't been signaled; treats the device's
// removal as failure. waitedMs, if given, receives the time blocked.
bool Core_Wait(UINT64 value, DWORD ms, const char *what, double *waitedMs = nullptr);

// Waits for everything submitted so far; true if the GPU is idle or the device is gone
bool Core_WaitIdle(DWORD ms, const char *what);

bool Core_DeviceRemoved();
// What Core_DeviceRemoved last found, or a completed fence of UINT64_MAX showed, without asking the device: that
// goes through the kernel, too slow for every draw
bool Core_RemovalSeen();
HRESULT Core_RemovedReason();
void Core_LogDeviceRemoved(); // the reason and DRED's breadcrumbs and page fault, once

// Keeps object alive until the work recorded so far is done
void Core_Retire(ComPtr<IUnknown> object);
template <typename T>
void Core_Retire(ComPtr<T> &object)
{
	if (object)
	{
		ComPtr<IUnknown> unknown;
		object.As(&unknown);
		Core_Retire(unknown);
		object.Reset();
	}
}
void Core_CollectGarbage(); // frees retired objects whose fence has completed
size_t Core_PendingReleases();

// Batched resource transitions, flushed before clears, copies, draws and Close
void Core_Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
void Core_FlushBarriers();

// CPU-only RTV/DSV heaps. Views are read when a clear or OMSetRenderTargets is recorded, so they can be rewritten
// right after.
enum
{
	kRtvScreen = 0,
	kRtvPost,
	kRtvCount = 16,
};
enum
{
	kDsvScreen = 0,
	kDsvCount = 4,
};
D3D12_CPU_DESCRIPTOR_HANDLE Core_Rtv(UINT index);
D3D12_CPU_DESCRIPTOR_HANDLE Core_Dsv(UINT index);

// Since the last Core_TakeStats
struct CoreStats
{
	int submissions; // all of them
	int presentSubmissions;
	int ringSubmissions, readbackSubmissions, uploadSubmissions, otherSubmissions;
	int barriers;
	int waits, waitTimeouts;
	double waitMs, worstWaitMs;
	int allocatorsCreated;
};
CoreStats Core_TakeStats();
size_t Core_Allocators();

} // namespace rr_d3d12
#endif
