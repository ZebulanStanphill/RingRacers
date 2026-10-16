// DR. ROBOTNIK'S RING RACERS
// Copyright (C) 2026 by Zebulan Stanphill.
// Licensed under the GNU General Public License, version 2.

// UWP-only, bounded render diagnostics. No engine state is changed.
#ifndef __I_UWPTRACE_H__
#define __I_UWPTRACE_H__
#ifdef _UWP
#include "../doomtype.h"
#ifdef __cplusplus
extern "C" {
#endif
void UWPTraceFade(const char *format, ...) FUNCPRINTF;
void UWPTraceGamestate(INT32 oldstate, INT32 newstate);
void UWPTraceLevelLoad(boolean loaded);
void UWPTraceBeginFrame(void);
UINT32 UWPTraceFrameId(void);
void UWPTraceView(void);
void UWPTraceTitleLightFade(INT32 sector, INT32 light, INT32 source, INT32 dest, INT32 timer, INT32 step);
void UWPTraceTitleBackground(boolean started);
#ifdef __cplusplus
}
#endif
#endif
#endif
