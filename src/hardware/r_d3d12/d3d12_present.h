// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.h
/// \brief [UWP]: Shows VibeRant D3D12's frames on the window: with a swap chain of its own, or through ANGLE

#ifndef RR_D3D12_PRESENT_H
#define RR_D3D12_PRESENT_H

#include "d3d12_core.h"

namespace rr_d3d12
{

// ANGLE's Direct3D 11 device, once its EGL display is current; null without one
ID3D11Device *Present_AngleDevice();

// Picks how frames reach the window. By default, Direct3D 12 presents them with a swap chain of its own, which it takes
// from ANGLE with the first frame: ANGLE's window surface goes, as a CoreWindow takes one swap chain at a time. The
// fallback, and what gr_d3d12_present Readback and CpuSync select, is interop with ANGLE, which draws the frames to
// the window: A (a D3D12 texture ANGLE opens), B (an ANGLE texture D3D12 opens), C (either, plus a copy on ANGLE's
// device into a legacy shared texture) or D (reading frames back), the first whose test texture shows the right
// color. Call with ANGLE's context current; Present_Frame calls it if it couldn't run before.
void Present_SelectPath();

// Copies source (in sourceState, which it's left in) to the swap chain's next buffer, submits the frame and presents
// it with vsync as the sync interval. Over interop, to a presentation texture, and draws the previous one to the
// window, a frame late so its copy has finished (vsync is then ANGLE's swap interval).
void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync);

// Whether Direct3D 12's swap chain has the window, so that ANGLE has no window surface or current context
bool Present_OwnsWindow();

// Gives the window back to ANGLE, by releasing the swap chain and making ANGLE's window surface again, for the
// Software renderer to use. Does nothing without the swap chain, and makes no context current: the caller does. The
// next frame presented takes the window again.
void Present_ReleaseWindow();

// Wipes must reach the window before presenting stops: over interop, show the frame being copied rather than the
// previous one. The swap chain already shows the current frame.
void Present_Immediately();

// Releases the swap chain, or the presentation textures and their EGL surfaces and the shared fences, then ANGLE's
// device. idle: the GPU has finished, so D3D12 objects can go now rather than being retired.
void Present_Shutdown(bool idle);

// Last Present's copied/shown slots (the swap chain's back buffer, which is both) and source draw IDs; -1/0 if no
// copy/draw occurred.
void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);

// Perf lines for frames since the last call; frames is at least 1
void Present_LogPerf(double frames);

// The first frames trace their submission, waits and present
bool Present_Tracing();
void Present_FrameDone();

// The swap chain's buffers, or the presentation textures
UINT64 Present_SlotBytes();

} // namespace rr_d3d12
#endif
