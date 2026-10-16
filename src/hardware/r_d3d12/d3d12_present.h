// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.h
/// \brief [UWP]: Shows VibeRant D3D12's frames through ANGLE, which owns the window

#ifndef RR_D3D12_PRESENT_H
#define RR_D3D12_PRESENT_H

#include "d3d12_core.h"

namespace rr_d3d12
{

// ANGLE's Direct3D 11 device, once its EGL display is current; null without one
ID3D11Device *Present_AngleDevice();

// Picks how frames reach ANGLE: A (a D3D12 texture ANGLE opens), B (an ANGLE texture D3D12 opens), C (either, plus a
// copy on ANGLE's device into a legacy shared texture) or D (reading frames back), the first whose test texture
// shows the right color. Call with ANGLE's context current; Present_Frame calls it if it couldn't run before.
void Present_SelectPath();

// Copies source (in sourceState, which it's left in) to a presentation texture, submits the frame and draws the
// previous one to the window, a frame late so its copy has finished.
void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height);

// Wipes must reach the window before presenting stops: show the frame being copied rather than the previous one
void Present_Immediately();

// Releases the presentation textures and their EGL surfaces, the shared fences, then ANGLE's device. idle: the GPU
// has finished, so D3D12 objects can go now rather than being retired.
void Present_Shutdown(bool idle);

// Last Present's copied/shown slots and source draw IDs; -1/0 if no copy/draw occurred.
void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);

// Perf lines for frames since the last call; frames is at least 1
void Present_LogPerf(double frames);

// The first frames trace their submission, waits and present
bool Present_Tracing();
void Present_FrameDone();

UINT64 Present_SlotBytes();

} // namespace rr_d3d12
#endif
