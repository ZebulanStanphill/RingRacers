// SPDX-License-Identifier: GPL-2.0-only
/// \file hw_driver.c
/// \brief Which driver the hardware renderer uses

#include "hw_driver.h"

#if defined(HAVE_VIBERANT) && !defined(_UWP)
boolean hwr_viberant = false;
boolean hwr_vulkan = false;
static boolean driver_started = false;

boolean HWR_DriverStarted(void) { return driver_started; }
void HWR_StartDriver(boolean d3d12, boolean vulkan)
{
	if (driver_started)
		return;
	hwr_viberant = d3d12;
	hwr_vulkan = d3d12 && vulkan;
	driver_started = true;
}
#endif
