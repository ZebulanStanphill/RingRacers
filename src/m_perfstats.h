// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file m_perfstats.h
/// \brief Performance measurement tools.

#ifndef __M_PERFSTATS_H__
#define __M_PERFSTATS_H__

#include "doomdef.h"
#include "lua_script.h"
#include "p_local.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	PS_OFF = 0,
	PS_RENDER,
	PS_LOGIC,
	PS_BOT,
	PS_THINKFRAME,
} ps_types_t;

extern precise_t ps_tictime;
extern precise_t ps_prevtictime;

extern precise_t ps_playerthink_time;
extern precise_t ps_botticcmd_time;
extern precise_t ps_thinkertime;

extern precise_t ps_thlist_times[];
extern precise_t ps_acs_time;

extern int       ps_checkposition_calls;

extern precise_t ps_lua_thinkframe_time;
extern int       ps_lua_mobjhooks;

extern precise_t ps_voiceupdatetime;

struct ps_hookinfo_t
{
	precise_t time_taken;
	char short_src[LUA_IDSIZE];
};

void PS_SetThinkFrameHookInfo(int index, precise_t time_taken, char* short_src);

struct ps_botinfo_t
{
	boolean isBot;
	precise_t total;
	precise_t prediction; // K_CreateBotPrediction
	precise_t nudge; // K_NudgePredictionTowardsObjects
	precise_t item; // K_BotItemUsage
};

extern ps_botinfo_t ps_bots[MAXPLAYERS];

void PS_ResetBotInfo(void);

void M_DrawPerfStats(void);

#ifdef _UWP
// [UWP]: The perfstats overlay is hard to read on Xbox, so these summarize the same timings in the log every few seconds
void PS_UWPAddTic(void); // after each game tic
void PS_UWPAddLoop(precise_t worktime, boolean displayed); // after each main loop iteration, with its time before sleeping

// Timed by D_Display and I_FinishUpdate, and counted by the hardware renderer, for the summaries
extern precise_t ps_uwp_screentime, ps_uwp_twodeetime, ps_uwp_upscaletime, ps_uwp_imguitime, ps_uwp_cleanuptime,
	ps_uwp_presenttime, ps_uwp_finishtime, ps_uwp_nextframetime;
extern int ps_uwp_draws, ps_uwp_texturescreated, ps_uwp_textureuploads, ps_uwp_framebufferscreated, ps_uwp_atlasresets;
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif
