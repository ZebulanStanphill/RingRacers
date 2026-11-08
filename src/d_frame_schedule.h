// SPDX-License-Identifier: GPL-2.0-only
#ifndef D_FRAME_SCHEDULE_H
#define D_FRAME_SCHEDULE_H
#include "doomtype.h"

// [UWP]: Render pacing only; no simulation clock or tic state is changed.
// Callers supply intervals below INT64_MAX/4; signed differences allow clock wrap.
static inline precise_t D_FrameSchedule(precise_t enter, precise_t finish,
	precise_t budget, bool enabled, bool skipped, precise_t &deadline, bool &scheduled)
{
	if (!enabled || !budget)
	{
		scheduled = false;
		return 0;
	}
	deadline = scheduled ? deadline + budget : enter + budget;
	scheduled = true;
	if ((INT64)(finish - deadline) > (INT64)(4 * budget))
		deadline = finish; // A long load drops debt rather than bursting forever.
	return !skipped && (INT64)(deadline - finish) > 0 ? deadline - finish : 0;
}
#endif
