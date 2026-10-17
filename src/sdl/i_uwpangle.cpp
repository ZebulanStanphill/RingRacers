// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Zebulan Stanphill.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_uwpangle.cpp
/// \brief [UWP]: Times the GPU work ANGLE (OpenGL ES on Direct3D 11) does on the Software renderer's frames, to see what
///        replacing it with a Direct3D 12 RHI could save
///
/// The CPU timings in the perf log show how long the main thread spends in ANGLE, not how long the GPU spends on
/// what it's given. This puts Direct3D 11 timestamp queries on ANGLE's own device, at five marks in a frame:
///
///   T0, T1: before and after VID_DisplaySoftwareScreen (uploading the software screen)
///   T2:     the start of I_FinishUpdate's RHI part (flushing the 2D layer, upscaling, ImGui)
///   T3, T4: before and after the present
///
/// A timestamp is taken when the GPU reaches it, so a stage whose commands the CPU issues more slowly than the GPU runs
/// them reads as an upper bound: it includes the GPU waiting for commands. ANGLE may also hold some work back until
/// it's needed (a texture upload is often applied by the draw that uses it), so the screen upload's time can end up
/// in the 2D/upscale/ImGui one.
///
/// Each frame has a disjoint query around its timestamps. Frames are kept in a small ring and read back when their
/// entry comes round again, without flushing or waiting, so a result that isn't ready yet is dropped, not waited for.
/// Marks are only taken on the main thread, while the Software renderer is drawing and ANGLE's context is current.

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "SDL.h"

#include "../doomdef.h"
#include "../i_system.h"
#include "../i_video.h"

using Microsoft::WRL::ComPtr;

namespace
{

constexpr int kFrames = 4; // in the ring
constexpr int kMarks = UWPANGLE_NUMMARKS;

// ANGLE's EGL, from the same library the Direct3D 12 driver (hardware/r_d3d12/d3d12_present.cpp) finds it in
typedef void *EGLDisplay;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef intptr_t EGLAttrib;

constexpr EGLint EGL_DEVICE_EXT = 0x322C;
constexpr EGLint EGL_D3D11_DEVICE_ANGLE = 0x33A1;

#define EGLAPIENTRY WINAPI
using PFNEGLGETCURRENTDISPLAY = EGLDisplay (EGLAPIENTRY *)(void);
using PFNEGLQUERYDISPLAYATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLint, EGLAttrib *);
using PFNEGLQUERYDEVICEATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDeviceEXT, EGLint, EGLAttrib *);
using PFNEGLGETERROR = EGLint (EGLAPIENTRY *)(void);

void *GetEglFunction(HMODULE module, const char *name)
{
	void *function = module ? reinterpret_cast<void *>(::GetProcAddress(module, name)) : nullptr;

	if (!function)
		function = SDL_GL_GetProcAddress(name);
	return function;
}

struct Frame
{
	enum State
	{
		Idle,
		Open, // marks are being taken
		Pending, // all marks taken (or given up on), waiting to be read
	};

	State state = Idle;
	unsigned marks = 0; // bit mask of the marks that were taken
	int last = -1; // the last mark taken
	ComPtr<ID3D11Query> disjoint;
	ComPtr<ID3D11Query> stamps[kMarks];
};

struct Timing
{
	bool tried = false;
	bool enabled = false;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Frame frames[kFrames];
	int current = 0; // the frame being marked, or the next one to start
	// Since the last I_UWPTakeAngleStats
	uwpanglestats_t stats = {};
} g_timing;

void Unavailable(const char *reason, ...)
{
	char message[160];
	va_list args;

	va_start(args, reason);
	vsnprintf(message, sizeof message, reason, args);
	va_end(args);
	I_OutputMsg("UWP: ANGLE GPU timing unavailable: %s\n", message);
}

bool CreateQuery(ID3D11Query **query, D3D11_QUERY type)
{
	D3D11_QUERY_DESC desc = {};
	desc.Query = type;
	return SUCCEEDED(g_timing.device->CreateQuery(&desc, query));
}

// Finds ANGLE's Direct3D 11 device through EGL, which needs its context to be current
void Initialize()
{
	g_timing.tried = true;

	HMODULE module = GetModuleHandleW(L"libEGL.dll");
	const auto getcurrentdisplay = reinterpret_cast<PFNEGLGETCURRENTDISPLAY>(GetEglFunction(module, "eglGetCurrentDisplay"));
	const auto querydisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXT>(GetEglFunction(module, "eglQueryDisplayAttribEXT"));
	const auto querydevice = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXT>(GetEglFunction(module, "eglQueryDeviceAttribEXT"));
	const auto geterror = reinterpret_cast<PFNEGLGETERROR>(GetEglFunction(module, "eglGetError"));

	if (!getcurrentdisplay || !querydisplay || !querydevice)
	{
		Unavailable("EGL functions are missing (eglGetCurrentDisplay %d, eglQueryDisplayAttribEXT %d, eglQueryDeviceAttribEXT %d)",
			getcurrentdisplay != nullptr, querydisplay != nullptr, querydevice != nullptr);
		return;
	}

	const EGLDisplay display = getcurrentdisplay();
	if (!display)
	{
		Unavailable("no current EGL display");
		return;
	}

	EGLAttrib eglDevice = 0, d3dDevice = 0;
	if (!querydisplay(display, EGL_DEVICE_EXT, &eglDevice) || !eglDevice)
	{
		Unavailable("couldn't get ANGLE's EGL device (EGL error 0x%X)", geterror ? (unsigned)geterror() : 0u);
		return;
	}
	if (!querydevice(reinterpret_cast<EGLDeviceEXT>(eglDevice), EGL_D3D11_DEVICE_ANGLE, &d3dDevice) || !d3dDevice)
	{
		Unavailable("couldn't get ANGLE's Direct3D 11 device (EGL error 0x%X)", geterror ? (unsigned)geterror() : 0u);
		return;
	}

	// EGL owns the pointer; assigning it to a ComPtr takes a reference of our own
	g_timing.device = reinterpret_cast<ID3D11Device *>(d3dDevice);
	g_timing.device->GetImmediateContext(&g_timing.context);
	if (!g_timing.context)
	{
		Unavailable("ANGLE's device has no immediate context");
		I_UWPShutdownAngleTiming();
		g_timing.tried = true;
		return;
	}

	for (Frame &frame : g_timing.frames)
	{
		bool ok = CreateQuery(frame.disjoint.GetAddressOf(), D3D11_QUERY_TIMESTAMP_DISJOINT);
		for (int mark = 0; ok && mark < kMarks; mark++)
			ok = CreateQuery(frame.stamps[mark].GetAddressOf(), D3D11_QUERY_TIMESTAMP);
		if (!ok)
		{
			Unavailable("couldn't create timestamp queries");
			I_UWPShutdownAngleTiming();
			g_timing.tried = true;
			return;
		}
	}

	g_timing.enabled = true;
	I_OutputMsg("UWP: ANGLE GPU timing on (feature level 0x%X)\n", (unsigned)g_timing.device->GetFeatureLevel());
}

// Reads back the frame in this ring entry, if it has one waiting
void Collect(Frame &frame)
{
	if (frame.state != Frame::Pending)
		return;
	frame.state = Frame::Idle;

	const unsigned needed = (1u << UWPANGLE_FINISH_START) | (1u << UWPANGLE_PRESENT_START) | (1u << UWPANGLE_PRESENT_END);
	if ((frame.marks & needed) != needed)
		return; // given up on: the frame didn't finish (an early return, or marks out of order)

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
	HRESULT hr = g_timing.context->GetData(frame.disjoint.Get(), &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr != S_OK)
	{
		g_timing.stats.notready++;
		return;
	}

	UINT64 stamps[kMarks] = {};
	for (int mark = 0; mark < kMarks; mark++)
	{
		if (!(frame.marks & (1u << mark)))
			continue;
		hr = g_timing.context->GetData(frame.stamps[mark].Get(), &stamps[mark], sizeof stamps[mark], D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hr != S_OK)
		{
			g_timing.stats.notready++;
			return;
		}
	}

	if (disjoint.Disjoint || !disjoint.Frequency)
	{
		g_timing.stats.disjoint++;
		return;
	}

	const double ms = 1000.0 / static_cast<double>(disjoint.Frequency);
	const auto span = [&](int from, int to) { return stamps[to] >= stamps[from] ? (stamps[to] - stamps[from]) * ms : 0.0; };
	const bool screen = (frame.marks & (1u << UWPANGLE_SCREEN_START)) && (frame.marks & (1u << UWPANGLE_SCREEN_END));
	const double screentime = screen ? span(UWPANGLE_SCREEN_START, UWPANGLE_SCREEN_END) : 0.0;
	const double finishtime = span(UWPANGLE_FINISH_START, UWPANGLE_PRESENT_START);
	const double presenttime = span(UWPANGLE_PRESENT_START, UWPANGLE_PRESENT_END);
	const double total = screentime + finishtime + presenttime;

	uwpanglestats_t &stats = g_timing.stats;
	stats.frames++;
	stats.screen += screentime;
	stats.finish += finishtime;
	stats.present += presenttime;
	stats.total += total;
	if (total > stats.worst)
		stats.worst = total;
}

// Ends the frame's disjoint query and leaves it for Collect
void Close(Frame &frame)
{
	g_timing.context->End(frame.disjoint.Get());
	frame.state = Frame::Pending;
}

} // namespace

void I_UWPAngleMark(uwpanglemark_t mark)
{
	if (rendermode != render_soft || !SDL_GL_GetCurrentContext())
		return;
	if (!g_timing.tried)
		Initialize();
	if (!g_timing.enabled || mark < 0 || mark >= kMarks)
		return;

	Frame *frame = &g_timing.frames[g_timing.current];

	// A frame that didn't get to its end (I_FinishUpdate returned early, or the renderer changed), or marks out of order:
	// give up on it and start another
	if (frame->state == Frame::Open && mark <= frame->last)
	{
		Close(*frame);
		g_timing.current = (g_timing.current + 1) % kFrames;
		frame = &g_timing.frames[g_timing.current];
	}

	if (frame->state != Frame::Open)
	{
		// The entry comes round again, with the frame that was in it kFrames ago
		Collect(*frame);
		frame->state = Frame::Open;
		frame->marks = 0;
		frame->last = -1;
		g_timing.context->Begin(frame->disjoint.Get());
	}

	g_timing.context->End(frame->stamps[mark].Get());
	frame->marks |= 1u << mark;
	frame->last = mark;

	if (mark == UWPANGLE_PRESENT_END)
	{
		Close(*frame);
		g_timing.current = (g_timing.current + 1) % kFrames;
	}
}

boolean I_UWPTakeAngleStats(uwpanglestats_t *stats)
{
	*stats = g_timing.stats;
	g_timing.stats = {};
	return stats->frames > 0;
}

void I_UWPShutdownAngleTiming(void)
{
	// Release the queries before the device they belong to
	for (Frame &frame : g_timing.frames)
	{
		frame.disjoint.Reset();
		for (ComPtr<ID3D11Query> &stamp : frame.stamps)
			stamp.Reset();
		frame.state = Frame::Idle;
	}
	g_timing.context.Reset();
	g_timing.device.Reset();
	g_timing.enabled = false;
	g_timing.tried = false;
	g_timing.current = 0;
}
