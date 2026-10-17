// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.cpp
/// \brief [UWP]: Shows VibeRant D3D12's frames on the window with a swap chain of its own
///
/// The driver takes the window from ANGLE when it starts (or with the first frame, if no ANGLE context is current
/// by then): ANGLE's window surface goes, as a CoreWindow takes one swap chain at a time. Each frame is then copied
/// into the next buffer of a swap chain of Direct3D 12's own, on the driver's queue, and presented from there. The
/// window goes back to ANGLE when the Software renderer is chosen, and the next time Legacy GL starts it's taken
/// again.
///
/// Only taking and giving back the window touch ANGLE: its EGL display and Direct3D 11 device, and a few
/// OpenGL ES calls to let go of the window's back buffer. While the swap chain has the window, ANGLE has no window
/// surface and no context current.

#include "d3d12_present.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "SDL.h"
#include "SDL_syswm.h"

#include "../../doomdef.h"
#include "../../i_system.h"
#include "../../screen.h"
#include "../../sdl/i_uwptrace.h"
#include "../../sdl/sdlmain.h"
#include "../hw_main.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

extern "C" __declspec(dllimport) void uwp_GetScreenSize(int*, int*);

// ringracers-uwp's patches/sdl-egl-surface-handoff.patch to SDL: destroys and recreates the window's EGL surface,
// ANGLE's swap chain on the CoreWindow, which the SDL headers this is built with don't declare
extern "C" __declspec(dllimport) int SDLCALL SDL_WinRTReleaseGLSurface(SDL_Window *window);
extern "C" __declspec(dllimport) int SDLCALL SDL_WinRTRestoreGLSurface(SDL_Window *window);

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

// ==========================================================================
//                                                        EGL AND OPENGL ES
// ==========================================================================

// Loaded at runtime from ANGLE
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef intptr_t EGLAttrib;

constexpr EGLint EGL_CONFIG_ID = 0x3028;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_DEVICE_EXT = 0x322C;
constexpr EGLint EGL_D3D11_DEVICE_ANGLE = 0x33A1;

#define EGLAPIENTRY WINAPI
using PFNEGLGETCURRENTDISPLAY = EGLDisplay (EGLAPIENTRY *)(void);
using PFNEGLCHOOSECONFIG = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using PFNEGLCREATEPBUFFERSURFACE = EGLSurface (EGLAPIENTRY *)(EGLDisplay, EGLConfig, const EGLint *);
using PFNEGLMAKECURRENT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFNEGLQUERYCONTEXT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLContext, EGLint, EGLint *);
using PFNEGLGETCONFIGATTRIB = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLConfig, EGLint, EGLint *);
using PFNEGLDESTROYSURFACE = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface);
using PFNEGLGETERROR = EGLint (EGLAPIENTRY *)(void);
using PFNEGLGETPROCADDRESS = void *(EGLAPIENTRY *)(const char *);
using PFNEGLQUERYDISPLAYATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLint, EGLAttrib *);
using PFNEGLQUERYDEVICEATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDeviceEXT, EGLint, EGLAttrib *);

struct Egl
{
	bool loaded = false;
	PFNEGLGETCURRENTDISPLAY GetCurrentDisplay = nullptr;
	PFNEGLCHOOSECONFIG ChooseConfig = nullptr;
	PFNEGLCREATEPBUFFERSURFACE CreatePbufferSurface = nullptr;
	PFNEGLMAKECURRENT MakeCurrent = nullptr;
	PFNEGLQUERYCONTEXT QueryContext = nullptr;
	PFNEGLGETCONFIGATTRIB GetConfigAttrib = nullptr;
	PFNEGLDESTROYSURFACE DestroySurface = nullptr;
	PFNEGLGETERROR GetError = nullptr;
	PFNEGLGETPROCADDRESS GetProcAddress = nullptr;
	PFNEGLQUERYDISPLAYATTRIBEXT QueryDisplayAttribEXT = nullptr;
	PFNEGLQUERYDEVICEATTRIBEXT QueryDeviceAttribEXT = nullptr;
};
Egl egl;

EGLint EglError()
{
	return egl.GetError ? egl.GetError() : 0;
}

void *GetEglFunction(HMODULE module, const char *name)
{
	void *function = module ? reinterpret_cast<void *>(::GetProcAddress(module, name)) : nullptr;

	if (!function && egl.GetProcAddress)
		function = egl.GetProcAddress(name);
	if (!function)
		function = SDL_GL_GetProcAddress(name);
	return function;
}

// Tried again until ANGLE's functions are all there, since libEGL.dll may not be loaded before the window exists
bool LoadEgl()
{
	if (egl.loaded)
		return true;

	HMODULE module = GetModuleHandleW(L"libEGL.dll");
	if (!module)
		LOG_ONCE("libEGL.dll isn't loaded (%lu); looking up EGL through SDL", (unsigned long)GetLastError());

#define LOADEGL(member, name) *reinterpret_cast<void **>(&egl.member) = GetEglFunction(module, name)
	LOADEGL(GetProcAddress, "eglGetProcAddress");
	LOADEGL(GetCurrentDisplay, "eglGetCurrentDisplay");
	LOADEGL(ChooseConfig, "eglChooseConfig");
	LOADEGL(CreatePbufferSurface, "eglCreatePbufferSurface");
	LOADEGL(MakeCurrent, "eglMakeCurrent");
	LOADEGL(QueryContext, "eglQueryContext");
	LOADEGL(GetConfigAttrib, "eglGetConfigAttrib");
	LOADEGL(DestroySurface, "eglDestroySurface");
	LOADEGL(GetError, "eglGetError");
	LOADEGL(QueryDisplayAttribEXT, "eglQueryDisplayAttribEXT");
	LOADEGL(QueryDeviceAttribEXT, "eglQueryDeviceAttribEXT");
#undef LOADEGL

	egl.loaded = egl.GetCurrentDisplay && egl.ChooseConfig && egl.CreatePbufferSurface && egl.MakeCurrent && egl.QueryContext
		&& egl.GetConfigAttrib && egl.DestroySurface && egl.GetError;
	if (egl.loaded)
		Log("EGL functions loaded");
	else
		LOG_ONCE("EGL functions are missing: GetCurrentDisplay %d, ChooseConfig %d, CreatePbufferSurface %d, MakeCurrent %d, QueryContext %d, GetConfigAttrib %d, DestroySurface %d, GetError %d",
			egl.GetCurrentDisplay != nullptr, egl.ChooseConfig != nullptr, egl.CreatePbufferSurface != nullptr,
			egl.MakeCurrent != nullptr, egl.QueryContext != nullptr, egl.GetConfigAttrib != nullptr,
			egl.DestroySurface != nullptr, egl.GetError != nullptr);
	return egl.loaded;
}

// OpenGL ES 2: what letting go of the window's back buffer needs
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;

constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;

#define GLAPIENTRY WINAPI
struct Gles
{
	bool loaded = false;
	void (GLAPIENTRY *BindFramebuffer)(GLenum, GLuint);
	void (GLAPIENTRY *Clear)(GLbitfield);
	void (GLAPIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
	void (GLAPIENTRY *Disable)(GLenum);
	void (GLAPIENTRY *Enable)(GLenum);
	void (GLAPIENTRY *Flush)(void);
	void (GLAPIENTRY *GetBooleanv)(GLenum, GLboolean *);
	GLenum (GLAPIENTRY *GetError)(void);
	void (GLAPIENTRY *GetIntegerv)(GLenum, GLint *);
	GLboolean (GLAPIENTRY *IsEnabled)(GLenum);
};
Gles gl;

bool LoadGles()
{
	if (gl.loaded)
		return true;

	bool ok = true;
#define LOADGL(name) \
	if (!(*reinterpret_cast<void **>(&gl.name) = SDL_GL_GetProcAddress("gl" #name))) \
	{ \
		Log("couldn't find gl" #name); \
		ok = false; \
	}
	LOADGL(BindFramebuffer)
	LOADGL(Clear)
	LOADGL(ColorMask)
	LOADGL(Disable)
	LOADGL(Enable)
	LOADGL(Flush)
	LOADGL(GetBooleanv)
	LOADGL(GetError)
	LOADGL(GetIntegerv)
	LOADGL(IsEnabled)
#undef LOADGL

	gl.loaded = ok;
	return ok;
}

// ==========================================================================
//                                                                 PRESENTING
// ==========================================================================

constexpr UINT kBuffers = 3; // the swap chain's buffers
constexpr UINT kLatency = 2; // and the frames the CPU can be ahead of it
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // the render target's too
constexpr int kRestoreRetries = 3;
constexpr UINT kFallbackWidth = 1280, kFallbackHeight = 720; // when neither the video mode nor the window has a size

// A frame's GPU time: begin and end timestamps, in the ring entry the frame number selects
constexpr int kTimingEntries = 4;
struct FrameTiming
{
	bool timing = false; // the frame's begin timestamp is recorded
	UINT64 fence = 0; // the submission that resolved its timestamps, until they're read
};

struct Presenter
{
	ComPtr<ID3D11Device> angleDevice;
	ComPtr<ID3D11DeviceContext> angleContext;
	bool deviceProbed = false;
	UINT64 frame = 0;
	FrameTiming timings[kTimingEntries];
	ComPtr<ID3D12QueryHeap> timestamps; // a begin and end per entry
	ComPtr<ID3D12Resource> timestampBuffer; // READBACK, 16 bytes per entry
	bool timestampsTried = false;
	bool stopped = false; // shut down, so nothing presents
};
Presenter g_present;
INT32 g_traceCopied = -1, g_traceShown = -1;
UINT32 g_traceCopyFrame, g_traceShowFrame;

struct PresentStats
{
	precise_t copyTime;
	double gpuMs, worstGpuMs;
	int gpuFrames;
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
	bool owned = false; // the swap chain has the window, so ANGLE has no surface on it and no context current
	SDL_GLContext context = nullptr; // ANGLE's, which is made current again when the window goes back
	ComPtr<IDXGISwapChain3> swapChain;
	ComPtr<ID3D12Resource> buffers[kBuffers];
	HANDLE latency = nullptr; // signaled when the swap chain can take another frame
	UINT width = 0, height = 0;
	char failure[96] = ""; // the step that last failed to take the window
};
Direct g_direct;

// Keep a reference: EGL owns the pointer returned by the device query. ANGLE's Direct3D 11 device is what the window
// is released through, so the device is needed to take the window, and nothing else.
void InitAngleDevice()
{
	if (g_present.deviceProbed)
		return;
	if (!LoadEgl())
		return;
	const EGLDisplay display = egl.GetCurrentDisplay();
	if (!display)
		return; // retry once ANGLE's context is current
	g_present.deviceProbed = true;
	if (!egl.QueryDisplayAttribEXT || !egl.QueryDeviceAttribEXT)
		return;

	EGLAttrib device = 0, d3dDevice = 0;
	if (!egl.QueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &device) || !device)
		return;
	const EGLDeviceEXT eglDevice = reinterpret_cast<EGLDeviceEXT>(device);
	if (!egl.QueryDeviceAttribEXT(eglDevice, EGL_D3D11_DEVICE_ANGLE, &d3dDevice) || !d3dDevice)
	{
		Log("couldn't get ANGLE's device (EGL error 0x%X)", EglError());
		return;
	}
	g_present.angleDevice = reinterpret_cast<ID3D11Device *>(d3dDevice); // ComPtr assignment AddRefs
	g_present.angleDevice->GetImmediateContext(&g_present.angleContext);
	Log("ANGLE's device has feature level 0x%X", (unsigned)g_present.angleDevice->GetFeatureLevel());
}

void CreateTimestamps()
{
	if (g_present.timestampsTried)
		return;
	g_present.timestampsTried = true;
	if (!Core_TimestampFrequency())
		return;

	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count = 2 * kTimingEntries;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = kTimingEntries * 2 * sizeof(UINT64);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (Core_Failed(Core_Device()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&g_present.timestamps)), "CreateQueryHeap (timestamps)")
		|| Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, IID_PPV_ARGS(&g_present.timestampBuffer)), "CreateCommittedResource (timestamps)"))
	{
		g_present.timestamps.Reset();
		g_present.timestampBuffer.Reset();
		return;
	}
	g_present.timestampBuffer->SetName(L"GPU timestamps");
}

// The OpenGL ES state of the GL2 RHI that letting go of the window changes, to put it back
struct SavedGlState
{
	GLint framebuffer;
	GLboolean scissorTest, colorMask[4];
};

void SaveGlState(SavedGlState &s)
{
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
	s.scissorTest = gl.IsEnabled(GL_SCISSOR_TEST);
	gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
}

void RestoreGlState(const SavedGlState &s)
{
	gl.BindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
	if (s.scissorTest)
		gl.Enable(GL_SCISSOR_TEST);
	else
		gl.Disable(GL_SCISSOR_TEST);
	gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
}

// Reads the GPU time of the frame an entry timed, once its submission has finished; otherwise drops it
void ReadGpuTime(FrameTiming &entry)
{
	const UINT64 fence = entry.fence;
	entry.fence = 0;
	if (!fence || !g_present.timestampBuffer || !Core_FenceDone(fence) || Core_DeviceRemoved())
		return;

	const size_t index = &entry - g_present.timings;
	const D3D12_RANGE range = {index * 2 * sizeof(UINT64), (index + 1) * 2 * sizeof(UINT64)};
	void *data = nullptr;
	if (Core_Failed(g_present.timestampBuffer->Map(0, &range, &data), "Map (timestamps)"))
		return;
	UINT64 times[2];
	memcpy(times, static_cast<const BYTE *>(data) + range.Begin, sizeof times);
	const D3D12_RANGE written = {0, 0};
	g_present.timestampBuffer->Unmap(0, &written);

	// No disjoint query in D3D12: drop what can't be right
	if (times[1] <= times[0])
		return;
	const double ms = (times[1] - times[0]) * 1000.0 / Core_TimestampFrequency();
	g_stats.gpuMs += ms;
	g_stats.gpuFrames++;
	if (ms > 250.0)
		LOG_ONCE("GPU frame exceeded 250 ms (%.1f ms)", ms);
	if (ms > g_stats.worstGpuMs)
		g_stats.worstGpuMs = ms;
}

// At the start of a frame's recording
void BeginGpuTime(FrameTiming &entry)
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list || !g_present.timestamps)
		return;
	const UINT index = (UINT)(&entry - g_present.timings);
	list->EndQuery(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index);
	entry.timing = true;
}

// Before the frame's copy; Present_Frame records which submission resolves it
bool EndGpuTime(FrameTiming &entry)
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!entry.timing || !list || !g_present.timestamps)
		return false;
	entry.timing = false;
	const UINT index = (UINT)(&entry - g_present.timings);
	list->EndQuery(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index + 1);
	list->ResolveQueryData(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index, 2,
		g_present.timestampBuffer.Get(), index * 2 * sizeof(UINT64));
	return true;
}

// After the frame's submission: the next frame is timed with the entry it'll use, whose last timing has had a few
// frames to finish
void NextFrame()
{
	g_present.frame++;
	FrameTiming &next = g_present.timings[g_present.frame % kTimingEntries];
	ReadGpuTime(next);
	BeginGpuTime(next);
}

// Whatever happens to presenting, the frame's commands must still run, or retired objects are never freed
void SubmitWithoutPresenting()
{
	Core_Submit("present");
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

// ANGLE's window surface again, once the CoreWindow lets go of the swap chain, which DXGI can take a moment to do
bool RestoreAngleSurface()
{
	for (int attempt = 0; attempt <= kRestoreRetries; attempt++)
	{
		if (attempt)
			Sleep(50);
		if (SDL_WinRTRestoreGLSurface(window) == 0)
			return true;
		Log("ANGLE couldn't have the window back (attempt %d of %d): %s", attempt + 1, kRestoreRetries + 1, SDL_GetError());
	}
	return false;
}

// With no window ANGLE can't show anything, so there's nothing else to do
void RestoreAngleSurfaceOrDie()
{
	if (!RestoreAngleSurface())
		I_Error("Direct3D 12 couldn't give the game window back to ANGLE (%s).\nRestart the game.", SDL_GetError());
}

enum TakeResult { TAKE_OK, TAKE_LATER, TAKE_FAILED };

// Takes the window from ANGLE, which has to let go of it first: a CoreWindow takes one swap chain at a time. ANGLE's
// D3D11 context keeps the window's back buffer bound, so before its surface goes, ANGLE draws to a pbuffer instead
// (a clear makes it bind the pbuffer) and flushes, without touching the rest of its state. Destroying the window
// surface then releases the swap chain, and once D3D11 has run its deferred destruction, the CoreWindow is free.
// TAKE_FAILED: ANGLE has the window and its context back, g_direct.failure names the step, and the log says what
// follows (consequence). TAKE_LATER: no context is current to take it from yet.
TakeResult TakeWindow(UINT width, UINT height, const char *consequence)
{
	const precise_t start = I_GetPreciseTime();
	const bool trace = Present_Tracing();
	const SDL_GLContext context = SDL_GL_GetCurrentContext();
	if (!context)
	{
		LOG_ONCE("no ANGLE context is current, so the window can't be taken from it yet");
		return TAKE_LATER;
	}

	EGLDisplay display = nullptr;
	EGLSurface pbuffer = nullptr;
	auto fail = [&](const char *step, unsigned code)
	{
		snprintf(g_direct.failure, sizeof g_direct.failure, "%s failed: 0x%X", step, code);
		Log("couldn't take the window from ANGLE (%s); %s", g_direct.failure, consequence);
		ReleaseSwapChain(true); // nothing has used it
		if (pbuffer)
		{
			SDL_GL_MakeCurrent(window, nullptr); // so SDL doesn't think the context is current, and EGL lets go of the pbuffer
			egl.DestroySurface(display, pbuffer);
		}
		RestoreAngleSurfaceOrDie(); // nothing to do if the window surface was never destroyed
		SDL_GL_MakeCurrent(window, context);
		return TAKE_FAILED;
	};

	// What can fail without changing anything comes first
	if (!LoadEgl())
		return fail("loading EGL", 0);
	display = egl.GetCurrentDisplay();
	if (!display)
	{
		LOG_ONCE("ANGLE has no current EGL display, so the window can't be taken from it yet");
		return TAKE_LATER;
	}
	if (!LoadGles())
		return fail("loading OpenGL ES functions", 0);
	InitAngleDevice();
	if (!g_present.angleContext)
		return fail("getting ANGLE's Direct3D 11 device", 0);

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	// SDL's WinRT driver puts the CoreWindow first in the union (info.winrt.window), which these headers may not declare
	IUnknown *coreWindow = nullptr;
	if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINRT)
	{
		Log("no CoreWindow: %s", SDL_GetError());
		return fail("SDL_GetWindowWMInfo", 0);
	}
	memcpy(&coreWindow, &info.info, sizeof coreWindow);
	if (!coreWindow)
		return fail("SDL_GetWindowWMInfo (no CoreWindow)", 0);

	ComPtr<IDXGIFactory2> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (Core_Failed(hr, "CreateDXGIFactory2 (swap chain)"))
		return fail("CreateDXGIFactory2", (unsigned)hr);

	// A pbuffer with the config of ANGLE's context, so that the context can be made current on it
	EGLint configId = 0, surfaceType = 0, count = 0;
	EGLConfig config = nullptr;
	if (!egl.QueryContext(display, context, EGL_CONFIG_ID, &configId))
		return fail("eglQueryContext", EglError());
	const EGLint wanted[] = {EGL_CONFIG_ID, configId, EGL_NONE};
	if (!egl.ChooseConfig(display, wanted, &config, 1, &count) || count < 1)
		return fail("eglChooseConfig", EglError());
	if (!egl.GetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surfaceType) || !(surfaceType & EGL_PBUFFER_BIT))
	{
		Log("ANGLE's context config %d has no pbuffers (surface type 0x%X)", (int)configId, (unsigned)surfaceType);
		return fail("EGL_PBUFFER_BIT", 0);
	}
	const EGLint pbufferSize[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
	pbuffer = egl.CreatePbufferSurface(display, config, pbufferSize);
	if (!pbuffer)
		return fail("eglCreatePbufferSurface", EglError());

	// From here on, the window is ANGLE's no longer. Clear the pbuffer so that ANGLE binds it, in place of the window's
	// back buffer, and flush; the GL state the clear needs is put back as it was.
	if (!egl.MakeCurrent(display, pbuffer, pbuffer, context))
		return fail("eglMakeCurrent (pbuffer)", EglError());
	{
		SavedGlState saved;
		SaveGlState(saved);
		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
		gl.Disable(GL_SCISSOR_TEST);
		gl.ColorMask(1, 1, 1, 1);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		gl.Flush();
		RestoreGlState(saved);
	}
	if (trace)
		Log("  ANGLE draws to a pbuffer (config %d, GL error 0x%X)", (int)configId, gl.GetError());

	// Makes nothing current (not the pbuffer either) and destroys the window surface and its swap chain
	if (SDL_WinRTReleaseGLSurface(window) != 0)
	{
		Log("%s", SDL_GetError());
		return fail("SDL_WinRTReleaseGLSurface", 0);
	}
	egl.DestroySurface(display, pbuffer);
	pbuffer = nullptr;
	// D3D11 destroys what's no longer bound when it runs the commands before, which Flush makes it do
	g_present.angleContext->Flush();

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = kFormat;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kBuffers;
	// Letterboxed when the screen's aspect ratio isn't the window's
	desc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = kSwapChainFlags;

	// The CoreWindow can still be letting go of ANGLE's swap chain
	ComPtr<IDXGISwapChain1> swapChain;
	for (int attempt = 0; attempt <= kRestoreRetries; attempt++)
	{
		if (attempt)
			Sleep(50);
		hr = factory->CreateSwapChainForCoreWindow(Core_Queue(), coreWindow, &desc, nullptr, &swapChain);
		if (SUCCEEDED(hr))
			break;
		Log("CreateSwapChainForCoreWindow failed (0x%08X, attempt %d of %d)", (unsigned)hr, attempt + 1, kRestoreRetries + 1);
	}
	if (Core_Failed(hr, "CreateSwapChainForCoreWindow"))
		return fail("CreateSwapChainForCoreWindow", (unsigned)hr);
	hr = swapChain.As(&g_direct.swapChain);
	if (Core_Failed(hr, "QueryInterface IDXGISwapChain3"))
		return fail("QueryInterface IDXGISwapChain3", (unsigned)hr);
	// The bars' color
	const DXGI_RGBA black = {0.0f, 0.0f, 0.0f, 1.0f};
	hr = g_direct.swapChain->SetBackgroundColor(&black);
	if (FAILED(hr))
		Log("SetBackgroundColor failed (0x%08X)", (unsigned)hr);
	hr = g_direct.swapChain->SetMaximumFrameLatency(kLatency);
	if (Core_Failed(hr, "SetMaximumFrameLatency"))
		return fail("SetMaximumFrameLatency", (unsigned)hr);
	g_direct.latency = g_direct.swapChain->GetFrameLatencyWaitableObject();
	if (!g_direct.latency)
		return fail("GetFrameLatencyWaitableObject", 0);
	if (!GetBuffers())
		return fail("GetBuffer", 0);

	g_direct.owned = true;
	g_direct.context = context;
	g_direct.width = width;
	g_direct.height = height;
	CreateTimestamps();
	Log("took the window from ANGLE: %ux%u swap chain, %u buffers, frame latency %u (%.1f ms)", width, height, kBuffers,
		kLatency, PreciseToMs(I_GetPreciseTime() - start));
	return TAKE_OK;
}

// Gives the window back to ANGLE, with no context current
void GiveBackWindow()
{
	const precise_t start = I_GetPreciseTime();
	ReleaseSwapChain(Core_WaitIdle(2000, "giving the window back to ANGLE"));
	g_direct.owned = false;
	RestoreAngleSurfaceOrDie();
	Log("gave the window back to ANGLE (%.1f ms)", PreciseToMs(I_GetPreciseTime() - start));
}

// The buffers can't be resized while the GPU or anything else holds them
bool ResizeSwapChain(UINT width, UINT height)
{
	const UINT oldWidth = g_direct.width, oldHeight = g_direct.height;
	if (!Core_WaitIdle(2000, "resizing the swap chain"))
		return false;
	for (ComPtr<ID3D12Resource> &buffer : g_direct.buffers)
		buffer.Reset();
	if (Core_Failed(g_direct.swapChain->ResizeBuffers(kBuffers, width, height, kFormat, kSwapChainFlags), "ResizeBuffers (swap chain)")
		|| !GetBuffers())
		return false;
	g_direct.width = width;
	g_direct.height = height;
	Log("resized the swap chain from %ux%u to %ux%u", oldWidth, oldHeight, width, height);
	return true;
}

} // namespace

bool Present_TakeWindow()
{
	if (g_direct.owned)
		return true;

	UINT width = vid.width > 0 ? (UINT)vid.width : 0, height = vid.height > 0 ? (UINT)vid.height : 0;
	if (!width || !height)
	{
		int windowWidth = 0, windowHeight = 0;
		uwp_GetScreenSize(&windowWidth, &windowHeight);
		width = windowWidth > 0 && windowHeight > 0 ? (UINT)windowWidth : kFallbackWidth;
		height = windowWidth > 0 && windowHeight > 0 ? (UINT)windowHeight : kFallbackHeight;
	}

	// TAKE_LATER: Present_Frame tries again once an ANGLE context is current
	return TakeWindow(width, height, "Legacy GL can't present, so the game falls back to Software") != TAKE_FAILED;
}

void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync)
{
	g_traceCopied = g_traceShown = -1;
	g_traceCopyFrame = g_traceShowFrame = 0;
	const bool trace = Present_Tracing();

	if (g_present.stopped || !source)
	{
		SubmitWithoutPresenting();
		return;
	}

	if (!g_direct.owned)
	{
		// Only if no ANGLE context was current when the driver started
		const TakeResult taken = TakeWindow(width, height, "Legacy GL can't present");
		if (taken == TAKE_LATER)
		{
			SubmitWithoutPresenting();
			return;
		}
		if (taken == TAKE_FAILED)
			I_Error("Direct3D 12 couldn't take the game window from ANGLE (%s).\nSet Renderer to Software in Video Options > Advanced.",
				g_direct.failure);
	}
	else if (g_direct.width != width || g_direct.height != height)
	{
		if (!ResizeSwapChain(width, height))
		{
			Log("couldn't resize the swap chain to %ux%u", width, height);
			I_Error("Direct3D 12 couldn't resize its swap chain to %ux%u.\nSet Renderer to Software in Video Options > Advanced.",
				width, height);
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
	FrameTiming &timing = g_present.timings[g_present.frame % kTimingEntries];
	ID3D12GraphicsCommandList *list = Core_List();
	bool timed = false;
	t = now;
	if (list)
	{
		timed = EndGpuTime(timing);
		Core_Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Core_Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		Core_FlushBarriers();
		list->CopyResource(back, source);
		Core_Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
		Core_Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}
	g_traceCopied = g_traceShown = (INT32)index;
	g_traceCopyFrame = g_traceShowFrame = UWPTraceFrameId();
	const UINT64 fence = list ? Core_Submit("present") : 0;
	if (!list)
		SubmitWithoutPresenting(); // reopens the list
	if (timed && fence)
		timing.fence = fence;
	now = I_GetPreciseTime();
	g_stats.copyTime += now - t;
	if (!fence)
	{
		NextFrame(); // nothing was submitted, so there is nothing to show
		return;
	}

	// Queued behind the copy on the same queue
	t = now;
	const HRESULT hr = g_direct.swapChain->Present(vsync ? 1 : 0, 0);
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
		Log("  copied the screen to back buffer %u, submitted as fence %llu; waited %.2f ms for the frame latency object%s; presented with sync interval %d (0x%08X, %.2f ms)",
			index, (unsigned long long)fence, waitMs, waited == WAIT_OBJECT_0 ? "" : " (gave up)", vsync ? 1 : 0, (unsigned)hr, presentMs);

	NextFrame();
}

void Present_ReleaseWindow()
{
	if (g_direct.owned)
		GiveBackWindow();
}

void Present_Shutdown(bool idle)
{
	// The window stays without ANGLE's surface: SDL_DestroyWindow doesn't need one
	ReleaseSwapChain(idle);
	g_direct.owned = false;
	if (idle)
	{
		g_present.timestamps.Reset();
		g_present.timestampBuffer.Reset();
	}
	else
	{
		Core_Retire(g_present.timestamps);
		Core_Retire(g_present.timestampBuffer);
	}
	g_present.angleContext.Reset();
	g_present.angleDevice.Reset();
	g_present.deviceProbed = false;
	g_present.stopped = true; // nothing presents after this
}

void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe)
{
	*copied = g_traceCopied;
	*shown = g_traceShown;
	*copyframe = g_traceCopyFrame;
	*showframe = g_traceShowFrame;
}

void Present_LogPerf(double frames)
{
	const PresentStats &s = g_stats;
	const double n = frames > 0 ? frames : 1;
	I_OutputMsg("UWP perf:   D3D12: present: direct swap chain (%u buffers, latency %u); per frame copy %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures\n",
		kBuffers, kLatency, PreciseToMs(s.copyTime) / n, s.latencyWaitMs / n, s.worstLatencyWaitMs,
		s.latencyTimeouts, PreciseToMs(s.presentTime) / n, s.worstPresentMs, s.presentFailures);
	I_OutputMsg("UWP perf:   D3D12: GPU %.2f ms per frame (worst %.2f, %d timed)\n",
		s.gpuFrames ? s.gpuMs / s.gpuFrames : 0.0, s.worstGpuMs, s.gpuFrames);
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
