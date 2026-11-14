// SPDX-License-Identifier: GPL-2.0-only
#ifndef HW_DRIVER_H
#define HW_DRIVER_H

#include "../doomtype.h"

// Which driver the hardware renderer (rendermode == render_opengl) draws with. The engine code around it behaves
// as Software does where Direct3D 12 is the driver, and as upstream's Legacy GL does where OpenGL is, so it asks
// HWR_D3D12() instead of deciding when it is compiled.
#if defined(HAVE_D3D12) && defined(_UWP)
#define HWR_D3D12() 1 // Xbox/UWP: the only driver
#elif defined(HAVE_D3D12)
#ifdef __cplusplus
extern "C" {
#endif
// Set by the video code before the hardware renderer first starts, then fixed for the run of the game: the
// hardware renderer is never re-initialized with the other driver.
extern boolean hwr_d3d12;
#ifdef __cplusplus
}
#endif
#define HWR_D3D12() (hwr_d3d12)
#else
#define HWR_D3D12() 0 // OpenGL only
#endif

#endif
