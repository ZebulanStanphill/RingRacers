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

namespace rr_d3d12
{

// Takes the window from ANGLE, whose window surface goes, as a CoreWindow takes one swap chain at a time. Call with
// ANGLE's context current, when Legacy GL starts. Returns true once the swap chain has the window, or if no context
// is current yet, in which case Present_Frame takes it with the first frame. Returns false if taking it failed: ANGLE
// has the window and its context back, and Legacy GL can't present, so the caller falls back to Software.
bool Present_TakeWindow();

// Copies source (in sourceState, which it's left in) to the swap chain's next buffer, submits the frame and presents
// it with vsync as the sync interval. Stops the game with an error if the window can't be taken or the swap chain
// resized.
void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync);

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
