// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file r_d3d12.h
/// \brief [UWP]: Legacy GL's driver for Direct3D 12 (VibeRant D3D12), presented with its own swap chain

#ifndef __R_D3D12_H__
#define __R_D3D12_H__

#include "../../doomtype.h"
#include "../hw_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Test hook: cumulative capture counts, unaffected by perf logging.
void D3D12_GetCaptureCountsForTest(UINT64 *fogCopies, UINT64 *waterCopies, UINT64 *waterRoundTrips);
void D3D12_GetReuseCountsForTest(UINT64 *placed, UINT64 *blocks, UINT64 *srvs);
// Test hook: cumulative texture creations by storage: shared heap blocks, heaps of their own, committed.
void D3D12_GetTextureStorageCountsForTest(UINT64 *placed, UINT64 *dedicated, UINT64 *committed);
// Test hook: queries an existing placed texture's block and the device's current allocation info.
boolean D3D12_GetTexturePlacementForTest(UINT32 id, UINT32 *heap, UINT64 *offset,
	UINT64 *blockBytes, UINT64 *reportedBytes, UINT64 *alignment);
// Test hook: set before initialization; zero keeps the production ring size.
void D3D12_SetRingBytesForTest(size_t vertex, size_t index, size_t constant);
// Runs palette rendering's last step and reads its result like ReadRect reads the screen; nothing is read without it
void D3D12_ReadPaletteRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);

void *D3D12_GetDriverFunction(const char *name);
boolean D3D12_Surface(INT32 w, INT32 h);
void D3D12_FinishUpdate(INT32 waitvbl);
boolean D3D12_TakeTextureFlushRequest(void);

// Surface-probe console command: a request starts a probe at the next Begin
boolean D3D12_RequestSurfaceProbe(INT32 x, INT32 y);
boolean D3D12_BeginSurfaceProbe(void);
void D3D12_EndSurfaceProbe(void);
void D3D12_SurfaceProbeSource(const char *kind, INT32 source);
void D3D12_SurfaceProbeTexture(const char *name);

void D3D12_SetPortalPlane(const float *worldPlane);
void D3D12_SetPortalStencil(INT32 level);
boolean D3D12_PortalsSupported(void);
boolean D3D12_DrawPortalAperture(const FOutVector *vertices, INT32 level, boolean restore);
void D3D12_ResetPortalDepth(INT32 level);

boolean D3D12_ApplyFadeMap(const UINT8 *table, size_t size, UINT32 tableId, UINT8 strength);
void D3D12_GetPresentTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);
void D3D12_LogPerf(double seconds);

// Gives the window back to ANGLE if the driver's swap chain has it, for the Software renderer; no context is made
// current. The driver takes the window again when Legacy GL starts.
void D3D12_ReleaseWindow(void);

// Waits for the GPU, then releases the swap chain and the device
void D3D12_Shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
