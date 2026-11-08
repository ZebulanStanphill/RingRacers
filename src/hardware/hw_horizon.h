// SPDX-License-Identifier: GPL-2.0-only
#ifndef HW_HORIZON_H
#define HW_HORIZON_H

// [UWP]: Runtime special changes invalidate the render-only subsector cache.
// Keep callers valid in software-only and non-D3D12 builds.
#if defined(HWRENDER) && defined(HAVE_D3D12)
#ifdef __cplusplus
extern "C" {
#endif
void HWR_InvalidateHorizonCache(void);
#ifdef __cplusplus
}
#endif
#else
static inline void HWR_InvalidateHorizonCache(void) {}
#endif

// A line's special changed from before to after: only a horizon special (HORIZONSPECIAL, r_defs.h) affects the
// cache, and rebuilding it scans every subsector, so one-shot triggers clearing their specials mid-race skip it.
#define HWR_LineSpecialChanged(before, after) do { \
	if ((before) == HORIZONSPECIAL || (after) == HORIZONSPECIAL) \
		HWR_InvalidateHorizonCache(); \
} while (0)
#endif
