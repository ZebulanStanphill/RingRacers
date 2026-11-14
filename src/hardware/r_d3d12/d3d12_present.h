// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.h
/// \brief [UWP]: Shows VibeRant D3D12's frames on the window with a swap chain of its own

#ifndef RR_D3D12_PRESENT_H
#define RR_D3D12_PRESENT_H

#include "d3d12_core.h"
#include "../../screen_scaling.hpp"

namespace rr_d3d12
{

// Takes the window from ANGLE, whose window surface goes, as a CoreWindow takes one swap chain at a time. Call with
// ANGLE's context current, when Legacy GL starts. Returns true once the swap chain has the window, or if no context
// is current yet, in which case Present_Frame takes it with the first frame. Returns false if taking it failed: ANGLE
// has the window and its context back, and Legacy GL can't present, so the caller falls back to Software.
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
// how. Fit copies the frame into a swap chain of its own size, which the system scales to the window, keeping the
// aspect ratio. Integer and 1:1 (unless the frame is larger than the screen) size the swap chain to the screen and have
// place put the frame in it. Stops the game with an error if the window can't be taken or the swap chain resized.
void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync, PlaceFrameFn place);

// Gives the window back to ANGLE, by releasing the swap chain and making ANGLE's window surface again, for the
// Software renderer to use. Does nothing without the swap chain, and makes no context current: the caller does.
// Starting Legacy GL again takes the window again.
void Present_ReleaseWindow();

// Releases the swap chain, then ANGLE's device. idle: the GPU has finished, so D3D12 objects can go now rather than
// being retired.
void Present_Shutdown(bool idle);

// Last Present's copied/shown slots (the swap chain's back buffer, which is both) and source draw IDs; -1/0 if no
// copy/draw occurred.
void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);

// Perf lines for frames since the last call; frames is at least 1
void Present_LogPerf(double frames);

// The first frames trace their submission, waits and present
bool Present_Tracing();
void Present_FrameDone();

// The swap chain's buffers
UINT64 Present_SwapChainBytes();

} // namespace rr_d3d12
#endif
