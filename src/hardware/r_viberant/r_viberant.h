// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file r_viberant.h
/// \brief The VibeRant driver: the hardware renderer's driver, written against the Direct3D 12 interfaces and run on Direct3D 12, Vulkan, or Metal

#ifndef __R_VIBERANT_H__
#define __R_VIBERANT_H__

#include "../../doomtype.h"
#include "../hw_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Test hook: cumulative capture counts, unaffected by perf logging.
void VibeRant_GetCaptureCountsForTest(UINT64 *fogCopies, UINT64 *waterCopies, UINT64 *waterRoundTrips);
void VibeRant_GetReuseCountsForTest(UINT64 *placed, UINT64 *blocks, UINT64 *srvs);
// Test hook: cumulative texture creations by storage: shared heap blocks, heaps of their own, committed.
void VibeRant_GetTextureStorageCountsForTest(UINT64 *placed, UINT64 *dedicated, UINT64 *committed);
// Test hook: queries an existing placed texture's block and the device's current allocation info.
boolean VibeRant_GetTexturePlacementForTest(UINT32 id, UINT32 *heap, UINT64 *offset,
	UINT64 *blockBytes, UINT64 *reportedBytes, UINT64 *alignment);
// Test hook: atlas pages that currently have a resource.
UINT32 VibeRant_GetAtlasPagesForTest(void);
// Test hook: set before initialization; zero keeps the production ring size.
void VibeRant_SetRingBytesForTest(size_t vertex, size_t index, size_t constant);
// Runs palette rendering's last step and reads its result like ReadRect reads the screen; nothing is read without it
void VibeRant_ReadPaletteRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
// Runs the last steps before the present (palette rendering's, then the screen effect's) and reads the frame they hand to
// it like ReadRect reads the screen
void VibeRant_ReadPresentedRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
// Puts that frame where Present_Frame would, on a screenWidth by screenHeight target filled with another color first, for
// the Screen Scaling mode (0 Fit, 1 Integer, 2 1:1), and reads a rectangle of the target (y up from its bottom). Returns
// false, reading nothing, for Fit or a frame the mode doesn't fit, which the system scales instead.
boolean VibeRant_ReadPlacedRectForTest(INT32 mode, INT32 screenWidth, INT32 screenHeight, INT32 x, INT32 y, INT32 width,
	INT32 height, INT32 dst_stride, UINT16 *dst_data);
// The same for Fit as a desktop swap chain shows it: the frame in screen_scaling::FitRect's rectangle of the target, scaled
// with linear filtering. Returns false, reading nothing, for a size that is not positive.
boolean VibeRant_ReadFitRectForTest(INT32 screenWidth, INT32 screenHeight, INT32 x, INT32 y, INT32 width, INT32 height,
	INT32 dst_stride, UINT16 *dst_data);

// Which backend the driver starts on, where the build has both: Vulkan (VibeRant Vulkan) or Direct3D 12 itself. validation
// turns on the backend's debugging layer (Vulkan's validation layers, or Direct3D 12's debug layer, whose errors go to the
// log). software picks a software device, WARP or lavapipe, over the GPU, for machines without one. Call before the driver
// is first started; later calls change nothing.
void VibeRant_SelectBackend(boolean vulkan, boolean validation, boolean software);
void *VibeRant_GetDriverFunction(const char *name);
boolean VibeRant_Surface(INT32 w, INT32 h);
void VibeRant_FinishUpdate(INT32 waitvbl);
boolean VibeRant_TakeTextureFlushRequest(void);

// Surface-probe console command: a request starts a probe at the next Begin
boolean VibeRant_RequestSurfaceProbe(INT32 x, INT32 y);
boolean VibeRant_BeginSurfaceProbe(void);
void VibeRant_EndSurfaceProbe(void);
void VibeRant_SurfaceProbeSource(const char *kind, INT32 source);
void VibeRant_SurfaceProbeTexture(const char *name);

void VibeRant_SetPortalPlane(const float *worldPlane);
void VibeRant_SetPortalStencil(INT32 level);
boolean VibeRant_PortalsSupported(void);
boolean VibeRant_DrawPortalAperture(const FOutVector *vertices, INT32 level, boolean restore);
void VibeRant_ResetPortalDepth(INT32 level);
// The spring mask is the stencil buffer's top bit, which portal levels never reach. DrawSpringMask sets or clears it
// over a convex polygon at any depth, writing no color or depth; while UseSpringMask is on, drawing skips the marked
// pixels (and stays inside the portal level, as always).
boolean VibeRant_DrawSpringMask(const FOutVector *vertices, INT32 count, boolean set);
void VibeRant_UseSpringMask(boolean on, INT32 level);
void VibeRant_FillVoid(void);

boolean VibeRant_ApplyFadeMap(const UINT8 *table, size_t size, UINT32 tableId, UINT8 strength);
void VibeRant_GetPresentTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);
void VibeRant_LogPerf(double seconds);
boolean VibeRant_CanRollSprite(void);

// Gives the window back to ANGLE if the driver's swap chain has it, for the Software renderer; no context is made
// current. The driver takes the window again when VibeRant D3D starts.
void VibeRant_ReleaseWindow(void);

// Waits for the GPU, then releases the swap chain and the device
void VibeRant_Shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
