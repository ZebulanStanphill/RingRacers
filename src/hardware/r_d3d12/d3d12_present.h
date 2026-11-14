// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.h
/// \brief Shows VibeRant D3D12's frames on the window with a swap chain of its own
///
/// Two implementations, one per platform. UWP (d3d12_present.cpp): the swap chain takes the CoreWindow from ANGLE. Windows
/// desktop (d3d12_present_win32.cpp): it goes on a child window of the SDL window, which is as large as the SDL window's
/// client area, and ANGLE isn't involved.

#ifndef RR_D3D12_PRESENT_H
#define RR_D3D12_PRESENT_H

#include "d3d12_core.h"
#include "../../screen_scaling.hpp"

namespace rr_d3d12
{

// Gets the window to present on, when Legacy GL starts.
// UWP: takes the window from ANGLE, whose window surface goes, as a CoreWindow takes one swap chain at a time. Call with
// ANGLE's context current. Returns true once the swap chain has the window, or if no context is current yet, in which
// case Present_Frame takes it with the first frame. Returns false if taking it failed: ANGLE has the window and its
// context back, and Legacy GL can't present, so the caller falls back to Software.
// Desktop: makes the child window of the SDL window and the swap chain on it, which needs no OpenGL context and never
// waits for one; the SDL window's pixel format and contexts are left alone. Returns false, with nothing left behind, if it
// can't, and the caller falls back to Software.
bool Present_TakeWindow();

// Records the clearing of target (a swap chain buffer, in RENDER_TARGET, whose view is rtv) to black, and the drawing of
// source (in sourceState, which it's left in) over the placement's rectangle of the targetWidth by targetHeight
// target: point-sampled for the screen scaling's Integer and 1:1 modes, linear-filtered when the placement's k is 0
// (screen_scaling::FitRect's). The driver provides it. Returns false,
// having recorded nothing, if it can't.
typedef bool (*PlaceFrameFn)(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, ID3D12Resource *target,
	D3D12_CPU_DESCRIPTOR_HANDLE rtv, const screen_scaling::Placement &placement, UINT targetWidth, UINT targetHeight);

// Shows source (in sourceState, which it's left in), a width by height frame, on the swap chain's next buffer, submits
// the frame and presents it with vsync as the sync interval. Options > Video > Screen Scaling (cv_scr_scaling) decides
// how. Stops the game with an error if the window can't be taken or the swap chain resized.
// UWP: Fit copies the frame into a swap chain of its own size, which the system scales to the window, keeping the
// aspect ratio. Integer and 1:1 (unless the frame is larger than the screen) size the swap chain to the screen and have
// place put the frame in it.
// Desktop: the swap chain is always the size of the SDL window's client area, which the child window is kept at, and
// place puts the frame in it for every mode: at Integer's or 1:1's rectangle, or, for Fit and for a frame larger than the
// window, at the largest rectangle of the frame's aspect ratio (screen_scaling::FitRect), scaled with linear
// filtering. Without place, or if it returns false, the frame is copied into a swap chain of its own size, which the
// system stretches to the window, from then on, until the window is taken again. A window with no client area (minimized) is not presented to.
void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync, PlaceFrameFn place);

// Gives the window back, for the Software renderer to use. Does nothing without the swap chain. Starting Legacy GL again
// takes the window again.
// UWP: releases the swap chain and makes ANGLE's window surface again, and makes no context current: the caller does.
// Desktop: releases the swap chain and destroys the child window.
void Present_ReleaseWindow();

// Releases the swap chain (and on the desktop, the child window), then on UWP ANGLE's device. idle: the GPU has finished,
// so D3D12 objects can go now rather than being retired.
void Present_Shutdown(bool idle);

// Last Present's copied/shown slots (the swap chain's back buffer, which is both) and source draw IDs; -1/0 if no
// copy/draw occurred. The draw IDs are UWP's frame trace ids, and 0 on the desktop.
void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);

// Perf lines for frames since the last call; frames is at least 1. UWP also times the GPU's work, which the desktop doesn't.
void Present_LogPerf(double frames);

// The first frames trace their submission, waits and present
bool Present_Tracing();
void Present_FrameDone();

// The swap chain's buffers
UINT64 Present_SwapChainBytes();

} // namespace rr_d3d12
#endif
