// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present_win32.cpp
/// \brief [Windows desktop]: Shows VibeRant D3D12's frames in a child window of the game window with a swap chain of its own
///
/// A flip-model swap chain can't be made on a window that has an OpenGL pixel format, and the Software renderer's RHI keeps
/// its OpenGL context on the SDL window, so the driver makes a child window of it, as large as the parent's client area, and
/// the swap chain goes on that. The child takes no input and no focus: hit testing passes through it to the SDL window.
/// Presenting is taken when Legacy GL starts and given back (the child destroyed) when the Software renderer is chosen; the
/// parent's pixel format and contexts are never touched.
///
/// The swap chain is always the size of the parent's client area, in pixels, and each frame is drawn into its back buffer
/// through the driver's PlaceFrame, which clears the buffer to black and puts the frame at the rectangle Screen Scaling
/// asks for: Integer's or 1:1's (screen_scaling::Compute), or otherwise the largest one of the frame's aspect ratio that fits
/// (screen_scaling::FitRect), scaled with linear filtering. If the driver can't place a frame, frames are copied into a swap
/// chain of their size instead, which DXGI stretches to the window, until the window is taken again.

#include "d3d12_present.h"

#include <dxgi1_5.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "SDL.h"
#include "SDL_syswm.h"

#include "../../doomdef.h"
#include "../../i_system.h"
#include "../../screen.h"
#include "../../sdl/sdlmain.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

namespace rr_d3d12
{
namespace
{

#define Log Core_Log
#define LOG_ONCE D3D12_LOG_ONCE

double PreciseToMs(precise_t t)
{
	return t * 1000.0 / I_GetPrecisePrecision();
}

constexpr UINT kBuffers = 3; // the swap chain's buffers
static_assert(kRtvBack2 - kRtvBack0 + 1 >= (int)kBuffers, "each swap chain buffer needs a view slot (kRtvBack0...)");
constexpr UINT kLatency = 2; // and the frames the CPU can be ahead of it
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // the render target's too
constexpr UINT kFallbackWidth = 1280, kFallbackHeight = 720; // the swap chain's size when the window has no client area yet
constexpr wchar_t kChildClass[] = L"RingRacersDirect3D12Present";

struct Presenter
{
	bool classRegistered = false;
	bool stopped = false; // shut down, so nothing presents
	bool placementFailed = false; // the driver couldn't place a frame, so frames are copied into a swap chain of their size, until the window is taken again
};
Presenter g_present;
INT32 g_traceSlot = -1;

struct PresentStats
{
	precise_t drawTime;
	double latencyWaitMs, worstLatencyWaitMs; // waiting for the swap chain to take another frame
	int latencyTimeouts;
	precise_t presentTime;
	double worstPresentMs;
	int presentFailures;
};
PresentStats g_stats;

int g_tracedFrames = 0;
constexpr int kTraceFrames = 3;

struct Direct
{
	bool owned = false; // the child window and swap chain exist
	HWND parent = nullptr; // the SDL window
	HWND child = nullptr; // what the swap chain is on
	UINT childWidth = 0, childHeight = 0; // the size the child was last given
	ComPtr<IDXGISwapChain3> swapChain;
	ComPtr<ID3D12Resource> buffers[kBuffers];
	HANDLE latency = nullptr; // signaled when the swap chain can take another frame
	UINT flags = 0; // the swap chain's, which ResizeBuffers repeats
	bool tearing = false; // the swap chain can present without waiting for the display
	UINT width = 0, height = 0; // the swap chain's size
	char failure[96] = ""; // the step that last failed to make the window and swap chain
};
Direct g_direct;

// Whatever happens to presenting, the frame's commands must still run, or retired objects are never freed
void SubmitWithoutPresenting()
{
	Core_Submit("present");
}

// ==========================================================================
//                                                                CHILD WINDOW
// ==========================================================================

// All input stays with the SDL window: the child is transparent to the mouse, disabled, and never activated
LRESULT CALLBACK ChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_ERASEBKGND:
		return 1; // the swap chain has the whole window
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterChildClass()
{
	if (g_present.classRegistered)
		return true;
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = ChildProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = kChildClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;
	g_present.classRegistered = true;
	return true;
}

void DestroyChild()
{
	if (g_direct.child && IsWindow(g_direct.child))
		DestroyWindow(g_direct.child);
	g_direct.child = nullptr;
	g_direct.parent = nullptr;
	g_direct.childWidth = g_direct.childHeight = 0;
}

// The parent's client area in pixels, 0 by 0 when it's minimized or has none
void ParentClientSize(HWND parent, UINT &width, UINT &height)
{
	RECT rect = {};
	width = height = 0;
	if (!parent || !IsWindow(parent) || IsIconic(parent) || !GetClientRect(parent, &rect))
		return;
	width = rect.right > rect.left ? (UINT)(rect.right - rect.left) : 0;
	height = rect.bottom > rect.top ? (UINT)(rect.bottom - rect.top) : 0;
}

// ==========================================================================
//                                                               SWAP CHAIN
// ==========================================================================

bool GetBuffers()
{
	for (UINT i = 0; i < kBuffers; i++)
	{
		wchar_t name[32];
		swprintf(name, sizeof name / sizeof *name, L"swap chain buffer %u", i);
		if (Core_Failed(g_direct.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_direct.buffers[i])), "GetBuffer (swap chain)"))
			return false;
		g_direct.buffers[i]->SetName(name);
		// To clear the buffer and draw the placed frame in it; made again whenever the buffers are got again
		Core_Device()->CreateRenderTargetView(g_direct.buffers[i].Get(), nullptr, Core_Rtv(kRtvBack0 + i));
	}
	return true;
}

// idle: the GPU has finished with the buffers, so they can go now rather than being retired
void ReleaseSwapChain(bool idle)
{
	for (ComPtr<ID3D12Resource> &buffer : g_direct.buffers)
	{
		if (idle)
			buffer.Reset();
		else
			Core_Retire(buffer);
	}
	if (idle)
		g_direct.swapChain.Reset();
	else
		Core_Retire(g_direct.swapChain);
	if (g_direct.latency)
	{
		CloseHandle(g_direct.latency);
		g_direct.latency = nullptr;
	}
	g_direct.width = g_direct.height = 0;
}

// Makes the child window and swap chain on the SDL window; false, with g_direct.failure naming the step, and the log saying
// what follows (consequence), if it can't, in which case nothing is left behind.
bool CreateWindowAndSwapChain(const char *consequence)
{
	const precise_t start = I_GetPreciseTime();
	auto fail = [&](const char *step, unsigned code)
	{
		snprintf(g_direct.failure, sizeof g_direct.failure, "%s failed: 0x%X", step, code);
		Log("couldn't create the window to present on (%s); %s", g_direct.failure, consequence);
		ReleaseSwapChain(true); // nothing has used it
		DestroyChild();
		return false;
	};

	if (!Core_Device() || !Core_Queue())
		return fail("the device", 0);

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS || !info.info.win.window)
	{
		Log("no window handle: %s", SDL_GetError());
		return fail("SDL_GetWindowWMInfo", 0);
	}
	const HWND parent = info.info.win.window;

	// The child covers the parent's client area, and the swap chain has its size; with none yet (minimized), the swap chain
	// is made at a size of its own and the first frame with an area resizes it
	UINT clientWidth, clientHeight;
	ParentClientSize(parent, clientWidth, clientHeight);
	const bool hasArea = clientWidth && clientHeight;
	const UINT width = hasArea ? clientWidth : kFallbackWidth, height = hasArea ? clientHeight : kFallbackHeight;

	if (!RegisterChildClass())
		return fail("RegisterClassEx", (unsigned)GetLastError());
	g_direct.child = CreateWindowExW(WS_EX_NOACTIVATE, kChildClass, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0,
		(int)clientWidth, (int)clientHeight, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!g_direct.child)
		return fail("CreateWindowEx", (unsigned)GetLastError());
	g_direct.parent = parent;
	g_direct.childWidth = clientWidth;
	g_direct.childHeight = clientHeight;

	ComPtr<IDXGIFactory2> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (Core_Failed(hr, "CreateDXGIFactory2 (swap chain)"))
		return fail("CreateDXGIFactory2", (unsigned)hr);

	// Presenting without vsync can tear where the system allows it
	g_direct.tearing = false;
	ComPtr<IDXGIFactory5> factory5;
	BOOL tearing = FALSE;
	if (SUCCEEDED(factory.As(&factory5))
		&& SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) && tearing)
		g_direct.tearing = true;
	g_direct.flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_direct.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = kFormat;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kBuffers;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = g_direct.flags;

	ComPtr<IDXGISwapChain1> swapChain;
	hr = factory->CreateSwapChainForHwnd(Core_Queue(), g_direct.child, &desc, nullptr, nullptr, &swapChain);
	if (Core_Failed(hr, "CreateSwapChainForHwnd"))
		return fail("CreateSwapChainForHwnd", (unsigned)hr);
	hr = swapChain.As(&g_direct.swapChain);
	if (Core_Failed(hr, "QueryInterface IDXGISwapChain3"))
		return fail("QueryInterface IDXGISwapChain3", (unsigned)hr);
	// The game handles Alt+Enter and the window's size itself
	hr = factory->MakeWindowAssociation(g_direct.child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	if (FAILED(hr))
		Log("MakeWindowAssociation failed (0x%08X)", (unsigned)hr);
	hr = g_direct.swapChain->SetMaximumFrameLatency(kLatency);
	if (Core_Failed(hr, "SetMaximumFrameLatency"))
		return fail("SetMaximumFrameLatency", (unsigned)hr);
	g_direct.latency = g_direct.swapChain->GetFrameLatencyWaitableObject();
	if (!g_direct.latency)
		return fail("GetFrameLatencyWaitableObject", 0);
	if (!GetBuffers())
		return fail("GetBuffer", 0);

	g_direct.owned = true;
	g_present.placementFailed = false; // placing is tried again whenever the window is taken
	g_direct.width = width;
	g_direct.height = height;
	Log("created the window to present on: %ux%u swap chain, %u buffers, frame latency %u, tearing %s (%.1f ms)", width, height,
		kBuffers, kLatency, g_direct.tearing ? "allowed" : "not allowed", PreciseToMs(I_GetPreciseTime() - start));
	return true;
}

// Releases the swap chain and destroys the child window, which gives the SDL window back as it was
void GiveBackWindow()
{
	const precise_t start = I_GetPreciseTime();
	ReleaseSwapChain(Core_WaitIdle(2000, "giving the window back"));
	DestroyChild();
	g_direct.owned = false;
	Log("gave the window back (%.1f ms)", PreciseToMs(I_GetPreciseTime() - start));
}

// The buffers can't be resized while the GPU or anything else holds them
bool ResizeSwapChain(UINT width, UINT height)
{
	const UINT oldWidth = g_direct.width, oldHeight = g_direct.height;
	if (!Core_WaitIdle(2000, "resizing the swap chain"))
		return false;
	for (ComPtr<ID3D12Resource> &buffer : g_direct.buffers)
		buffer.Reset();
	if (Core_Failed(g_direct.swapChain->ResizeBuffers(kBuffers, width, height, kFormat, g_direct.flags), "ResizeBuffers (swap chain)")
		|| !GetBuffers())
		return false;
	g_direct.width = width;
	g_direct.height = height;
	Log("resized the swap chain from %ux%u to %ux%u", oldWidth, oldHeight, width, height);
	return true;
}

// How a frame of the given size is shown: the swap chain's size, and where in it the frame goes
struct Layout
{
	screen_scaling::Placement placement; // fit: the frame is copied to a swap chain of its size, which DXGI stretches
	UINT width, height; // the swap chain's size
};

Layout ChooseLayout(UINT frameWidth, UINT frameHeight, UINT clientWidth, UINT clientHeight)
{
	Layout layout;
	if (g_present.placementFailed)
	{
		layout.width = frameWidth;
		layout.height = frameHeight;
		return layout;
	}

	layout.width = clientWidth;
	layout.height = clientHeight;
	// Integer and 1:1, where the frame fits the window; otherwise (and for Fit) the largest rectangle of its aspect ratio
	layout.placement = screen_scaling::Compute((int)frameWidth, (int)frameHeight, (int)clientWidth, (int)clientHeight,
		cv_scr_scaling.value);
	if (layout.placement.fit)
		layout.placement = screen_scaling::FitRect((int)frameWidth, (int)frameHeight, (int)clientWidth, (int)clientHeight);
	return layout;
}

// Says how frames are shown whenever that changes: Screen Scaling, a resolution or the window's size
void LogLayout(UINT frameWidth, UINT frameHeight, const Layout &layout)
{
	static UINT lastFrameWidth, lastFrameHeight;
	static Layout last;
	static bool logged = false;
	const screen_scaling::Placement &p = layout.placement, &l = last.placement;
	if (logged && lastFrameWidth == frameWidth && lastFrameHeight == frameHeight && layout.width == last.width
		&& layout.height == last.height && p.fit == l.fit && p.x == l.x && p.y == l.y && p.w == l.w && p.h == l.h && p.k == l.k)
		return;
	logged = true;
	lastFrameWidth = frameWidth;
	lastFrameHeight = frameHeight;
	last = layout;
	if (p.fit)
		Log("presenting %ux%u frames copied to a swap chain of their size, which the system stretches to the %ux%u window",
			frameWidth, frameHeight, layout.width, layout.height);
	else if (!p.k)
		Log("presenting %ux%u frames fit, scaled with linear filtering, in a %ux%u swap chain at (%d, %d) %dx%d", frameWidth,
			frameHeight, layout.width, layout.height, p.x, p.y, p.w, p.h);
	else
		Log("presenting %ux%u frames at %dx in a %ux%u swap chain at (%d, %d)", frameWidth, frameHeight, p.k, layout.width,
			layout.height, p.x, p.y);
}

} // namespace

bool Present_TakeWindow()
{
	if (g_direct.owned)
		return true;
	return CreateWindowAndSwapChain("Legacy GL can't present, so the game falls back to Software");
}

void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync, PlaceFrameFn place)
{
	g_traceSlot = -1;
	const bool trace = Present_Tracing();

	if (g_present.stopped || !source || !width || !height)
	{
		SubmitWithoutPresenting();
		return;
	}

	// Only if the window was given back and the driver is still drawing
	if (!g_direct.owned && !CreateWindowAndSwapChain("Legacy GL can't present"))
		I_Error("Direct3D 12 couldn't create its window to present on (%s).\nSet Renderer to Software in Video Options > Advanced.",
			g_direct.failure);

	// A window with no client area (minimized) shows nothing
	UINT clientWidth, clientHeight;
	ParentClientSize(g_direct.parent, clientWidth, clientHeight);
	if (!clientWidth || !clientHeight)
	{
		LOG_ONCE("the game window has no client area, so nothing is presented until it has one");
		SubmitWithoutPresenting();
		return;
	}
	// The child covers the whole client area
	if (g_direct.childWidth != clientWidth || g_direct.childHeight != clientHeight)
	{
		if (SetWindowPos(g_direct.child, nullptr, 0, 0, (int)clientWidth, (int)clientHeight,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOCOPYBITS))
		{
			g_direct.childWidth = clientWidth;
			g_direct.childHeight = clientHeight;
		}
		else
			LOG_ONCE("couldn't resize the child window to %ux%u (error %lu)", clientWidth, clientHeight, (unsigned long)GetLastError());
	}

	const Layout layout = ChooseLayout(width, height, clientWidth, clientHeight);
	LogLayout(width, height, layout);
	if (g_direct.width != layout.width || g_direct.height != layout.height)
	{
		if (!ResizeSwapChain(layout.width, layout.height))
		{
			Log("couldn't resize the swap chain to %ux%u", layout.width, layout.height);
			I_Error("Direct3D 12 couldn't resize its swap chain to %ux%u.\nSet Renderer to Software in Video Options > Advanced.",
				layout.width, layout.height);
		}
	}

	// The swap chain takes another frame once it has presented one of the last kLatency
	precise_t t = I_GetPreciseTime();
	const DWORD waited = WaitForSingleObjectEx(g_direct.latency, 500, FALSE);
	precise_t now = I_GetPreciseTime();
	const double waitMs = PreciseToMs(now - t);
	g_stats.latencyWaitMs += waitMs;
	g_stats.worstLatencyWaitMs = std::max(g_stats.worstLatencyWaitMs, waitMs);
	if (waited != WAIT_OBJECT_0)
	{
		g_stats.latencyTimeouts++;
		LOG_ONCE("gave up waiting for the swap chain's frame latency object after %.0f ms (%s)", waitMs,
			waited == WAIT_TIMEOUT ? "timed out" : "wait failed");
	}

	const UINT index = g_direct.swapChain->GetCurrentBackBufferIndex();
	ID3D12Resource *back = g_direct.buffers[index].Get();
	ID3D12GraphicsCommandList *list = Core_List();
	t = now;
	if (list)
	{
		if (layout.placement.fit)
		{
			Core_Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Core_Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
			Core_FlushBarriers();
			list->CopyResource(back, source);
			Core_Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
			Core_Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		}
		else
		{
			// The swap chain is the window's size: the buffer is cleared to black and the frame drawn in its place,
			// which leaves source as it was
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kRtvBack0 + index);
			Core_Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Core_FlushBarriers();
			if (!place || !place(source, sourceState, back, rtv, layout.placement, layout.width, layout.height))
			{
				// Having recorded nothing: this frame is black, and from the next, frames are copied to a swap chain of
				// their size, which DXGI stretches to the window
				const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
				list->ClearRenderTargetView(rtv, black, 0, nullptr);
				g_present.placementFailed = true;
				Log("couldn't place the frame in the swap chain; frames are copied to a swap chain of their size from now on");
			}
			Core_Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		}
	}
	g_traceSlot = (INT32)index;
	const UINT64 fence = list ? Core_Submit("present") : 0;
	if (!list)
		SubmitWithoutPresenting(); // reopens the list
	now = I_GetPreciseTime();
	g_stats.drawTime += now - t;
	if (!fence)
		return; // nothing was submitted, so there is nothing to show

	// Queued behind the drawing on the same queue
	t = now;
	const UINT flags = !vsync && g_direct.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
	const HRESULT hr = g_direct.swapChain->Present(vsync ? 1 : 0, flags);
	now = I_GetPreciseTime();
	const double presentMs = PreciseToMs(now - t);
	g_stats.presentTime += now - t;
	g_stats.worstPresentMs = std::max(g_stats.worstPresentMs, presentMs);
	// A removed or reset device ends the run in D3D12_FinishUpdate
	if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)
	{
		g_stats.presentFailures++;
		LOG_ONCE("IDXGISwapChain::Present failed (0x%08X)", (unsigned)hr);
	}
	if (trace)
		Log("  %s the window's back buffer %u, submitted as fence %llu; waited %.2f ms for the frame latency object%s; presented with sync interval %d, flags 0x%X (0x%08X, %.2f ms)",
			layout.placement.fit ? "copied to" : "placed in", index, (unsigned long long)fence, waitMs,
			waited == WAIT_OBJECT_0 ? "" : " (gave up)", vsync ? 1 : 0, flags, (unsigned)hr, presentMs);
}

void Present_ReleaseWindow()
{
	if (g_direct.owned)
		GiveBackWindow();
}

void Present_Shutdown(bool idle)
{
	// The SDL window stays as it was: SDL_DestroyWindow doesn't need anything of ours
	ReleaseSwapChain(idle);
	DestroyChild();
	g_direct.owned = false;
	if (g_present.classRegistered && UnregisterClassW(kChildClass, GetModuleHandleW(nullptr)))
		g_present.classRegistered = false;
	g_present.stopped = true; // nothing presents after this
}

void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe)
{
	*copied = g_traceSlot;
	*shown = g_traceSlot;
	*copyframe = 0; // there is no frame id to report off the Xbox
	*showframe = 0;
}

void Present_LogPerf(double frames)
{
	const PresentStats &s = g_stats;
	const double n = frames > 0 ? frames : 1;
	I_OutputMsg("UWP perf:   D3D12: present: window swap chain (%u buffers, latency %u, %ux%u, tearing %s); per frame draw %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures\n",
		kBuffers, kLatency, g_direct.width, g_direct.height, g_direct.tearing ? "allowed" : "not allowed",
		PreciseToMs(s.drawTime) / n, s.latencyWaitMs / n, s.worstLatencyWaitMs, s.latencyTimeouts,
		PreciseToMs(s.presentTime) / n, s.worstPresentMs, s.presentFailures);
	g_stats = PresentStats();
}

bool Present_Tracing()
{
	return g_tracedFrames < kTraceFrames;
}

void Present_FrameDone()
{
	if (g_tracedFrames < kTraceFrames)
		g_tracedFrames++;
}

UINT64 Present_SwapChainBytes()
{
	if (g_direct.swapChain)
		return (UINT64)kBuffers * g_direct.width * g_direct.height * 4;
	return 0;
}

} // namespace rr_d3d12
