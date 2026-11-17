// SPDX-License-Identifier: GPL-2.0-only
#ifndef HW_DRIVER_H
#define HW_DRIVER_H

#include "../doomtype.h"

// Which driver the hardware renderer (rendermode == render_opengl) draws with. The engine code around it behaves
// as Software does where the VibeRant driver (src/hardware/r_d3d12) is the driver, and as upstream's Legacy GL does
// where OpenGL is, so it asks HWR_VibeRant() instead of deciding when it is compiled. HWR_VibeRant() is true for the
// VibeRant driver on any of its backends: Direct3D 12, Vulkan, or Metal.
#if defined(HAVE_VIBERANT) && defined(_UWP)
#define HWR_VibeRant() 1 // Xbox/UWP: the only driver
#elif defined(HAVE_VIBERANT)
#ifdef __cplusplus
extern "C" {
#endif
// Set by the video code before the hardware renderer first starts, then fixed for the run of the game: the
// hardware renderer is never re-initialized with the other driver.
extern boolean hwr_viberant;
// Whether that driver runs on Vulkan (VibeRant Vulkan) instead of Direct3D 12 itself (VibeRant D3D). The engine
// doesn't draw differently for it, so only the video code and the menus ask.
extern boolean hwr_vulkan;
// A failed start also fixes the choice: resources cannot be restarted with another driver.
boolean HWR_DriverStarted(void);
void HWR_StartDriver(boolean d3d12, boolean vulkan);
#ifdef __cplusplus
}
#endif
#define HWR_VibeRant() (hwr_viberant)
#else
#define HWR_VibeRant() 0 // OpenGL only
#endif

// The VibeRant driver's backend, where there is a choice
#if defined(HAVE_VIBERANT) && !defined(_UWP) && defined(HAVE_VIBERANT_VULKAN)
#define HWR_Vulkan() (hwr_viberant && hwr_vulkan)
#else
#define HWR_Vulkan() 0
#endif
// A build with the Metal backend (macOS) has no other, so there the VibeRant driver is VibeRant Metal
#if defined(HAVE_VIBERANT) && !defined(_UWP) && defined(HAVE_VIBERANT_METAL)
#define HWR_Metal() (hwr_viberant)
#define HWR_VIBERANT_NAME(vulkan) ((void)(vulkan), "VibeRant Metal")
#else
#define HWR_Metal() 0
#define HWR_VIBERANT_NAME(vulkan) ((vulkan) ? "VibeRant Vulkan" : "VibeRant D3D")
#endif

#endif
