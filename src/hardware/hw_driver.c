// SPDX-License-Identifier: GPL-2.0-only
/// \file hw_driver.c
/// \brief Which driver the hardware renderer uses

#include "hw_driver.h"

#if defined(HAVE_D3D12) && !defined(_UWP)
// Direct3D 12 is the only driver there is on a build with it for now
boolean hwr_d3d12 = true;
#endif
