// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_VIBERANT_PREWARM_H
#define RR_VIBERANT_PREWARM_H
#include "viberant_core.h"
namespace rr_viberant
{
void Prewarm_Start(ID3D12Device *device, ID3D12RootSignature *root);
void Prewarm_Stop();
bool Prewarm_Done();
void Prewarm_LogDone(); // main thread only; logs once after worker completes
} // namespace rr_viberant
#endif
