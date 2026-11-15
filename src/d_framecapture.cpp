// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d_framecapture.cpp
/// \brief Frame capture mode (-shottics): screenshots at chosen level times of a replay. See d_framecapture.h.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d_framecapture.h"
#include "frame_capture.hpp"

#include "doomdef.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_demo.h"
#include "g_game.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_fixed.h"
#include "m_misc.h"
#include "p_tick.h"
#include "command.h"
#include "console.h"
#include "m_random.h"
#include "i_time.h"
#include "w_wad.h"

extern "C" {

boolean g_framecapture = false;

}

namespace
{

std::vector<std::uint32_t> g_shots; // level times still to capture and already captured, ascending
std::size_t g_next = 0; // g_shots[g_next] is the one to capture next
std::string g_dir; // with a trailing path separator
std::string g_name; // file name of the screenshot asked for
std::string g_path; // g_dir + g_name
bool g_pending = false; // a screenshot was asked for in this pass of the loop
bool g_seenlevel = false; // a level has been on screen since the start
std::uint32_t g_lasttic = UINT32_MAX; // the last level time asked for, so a paused game doesn't ask twice
std::size_t g_taken = 0;
UINT32 g_random = 1; // state of FrameCapture_Random

void Finish(const char *why)
{
	I_OutputMsg("framecapture: %s; %u of %u screenshots taken\n", why, (unsigned)g_taken, (unsigned)g_shots.size());
	I_Quit();
}

// Console command "framecapture_ghost <MAP>-<skin>-<player>": plays that staff ghost out of staffghosts.pk3, which neither
// the command line nor the playdemo command can (they only read replay files, and refuse the staff ghosts' version 0).
void Command_Ghost_f(void)
{
	if (COM_Argc() < 2)
	{
		I_OutputMsg("framecapture_ghost <staff ghost>: play a staff ghost, like RR_AERIALHIGHLANDS-blaze-MK\n");
		return;
	}

	std::string spec = COM_Argv(1);
	if (spec.size() > 4 && strcasecmp(spec.c_str() + spec.size() - 4, ".lmp") == 0)
	{
		spec.resize(spec.size() - 4);
	}

	const std::string map = spec.substr(0, spec.find('-'));
	const std::string fullname = "staffghosts/" + map + "/" + spec + ".lmp";

	const INT32 mapnum = G_MapNumber(map.c_str());
	if (mapnum >= nummapheaders || mapheaderinfo[mapnum] == NULL)
	{
		I_Error("framecapture_ghost: no map %s", map.c_str());
	}

	const mapheader_t *header = mapheaderinfo[mapnum];
	for (UINT32 i = 0; i < header->ghostCount; i++)
	{
		const staffbrief_t *brief = header->ghostBrief[i];
		if (strcasecmp(wadfiles[brief->wad]->lumpinfo[brief->lump].fullname, fullname.c_str()) != 0)
		{
			continue;
		}

		if (demo.playback)
		{
			G_StopDemo();
		}
		demo.loadfiles = false;
		demo.ignorefiles = true; // as the Time Attack menu does: staff ghosts need no files
		I_OutputMsg("framecapture: playing staff ghost %s\n", fullname.c_str());
		G_DoPlayDemoEx("", (brief->wad << 16) | brief->lump);
		return;
	}

	I_Error("framecapture_ghost: no staff ghost %s (the map has %u)", fullname.c_str(), (unsigned)header->ghostCount);
}

} // namespace

extern "C" {

void FrameCapture_Init(void)
{
	const int p = M_CheckParm("-shottics");
	if (p == 0)
	{
		return;
	}

	std::string error;
	if (!M_IsNextParm() || !frame_capture::parse_tics(M_GetNextParm(), g_shots, error))
	{
		I_Error("-shottics: %s (expected a comma-separated list of level times, like -shottics 70,210,350)",
			M_IsNextParm() ? error.c_str() : "no tics given");
	}

	const int d = M_CheckParm("-shotdir");
	if (d != 0 && M_IsNextParm())
	{
		g_dir = M_GetNextParm();
	}
	else
	{
		g_dir = std::string(srb2home) + PATHSEP "framecapture";
	}
	if (g_dir.empty() || g_dir.back() != PATHSEP[0])
	{
		g_dir += PATHSEP;
	}

	g_framecapture = true;

	const int g = M_CheckParm("-shotghost");
	if (g != 0 && M_IsNextParm())
	{
		COM_AddCommand("framecapture_ghost", Command_Ghost_f);
		COM_BufAddText(va("framecapture_ghost \"%s\"\n", M_GetNextParm()));
	}

	I_OutputMsg("framecapture: %u tics to capture into %s\n", (unsigned)g_shots.size(), g_dir.c_str());
}

void FrameCapture_BeginLoop(void)
{
	if (!g_framecapture)
	{
		return;
	}

	// One tic per frame, always: demos reset this when they stop, and a loading screen must not skip tics either.
	g_singletics = true;

	// Unsynced random numbers (sparkles, rain, HUD shaking) start every frame from a state that depends on the level
	// time alone, so that a frame doesn't depend on how many random numbers the frames before it drew (a loading
	// screen draws a time-dependent number of them) and is the same on every C library.
	const UINT32 seed = (gamestate == GS_LEVEL ? (UINT32)leveltime : 0u) * 2654435761u + 0x9E3779B9u;
	g_random = seed ? seed : 1u;
	P_SetRandSeed(PR_INTERPHUDRANDOM, seed ^ 0x5bd1e995u);
	P_SetRandSeed(PR_NUISANCE, seed ^ 0x1b873593u);

	// The replay is over (or never started to play): nothing more will be drawn.
	if (g_seenlevel && (!demo.playback || gamestate != GS_LEVEL))
	{
		Finish(demo.playback ? "the level ended" : "the replay ended");
	}
}

void FrameCapture_Request(boolean drawn)
{
	if (!g_framecapture)
	{
		return;
	}

	if (!drawn || gamestate != GS_LEVEL || !demo.playback)
	{
		return;
	}

	g_seenlevel = true;

	// A tic can only be missed by a game that skipped tics; say so instead of waiting for the end of the replay.
	while (g_next < g_shots.size() && leveltime > g_shots[g_next])
	{
		I_OutputMsg("framecapture: level time %u was never drawn (now %u)\n", (unsigned)g_shots[g_next],
			(unsigned)leveltime);
		g_next++;
	}

	if (g_next >= g_shots.size())
	{
		Finish("all tics passed");
	}

	if (leveltime != g_shots[g_next] || leveltime == g_lasttic)
	{
		return;
	}

	g_lasttic = leveltime;
	g_name = frame_capture::shot_name(G_BuildMapName(gamemap), leveltime);
	g_path = g_dir + g_name;
	remove(g_path.c_str()); // a file from an earlier run must not pass for this screenshot
	g_pending = true;
	takescreenshot = true;
}

void FrameCapture_EndFrame(void)
{
	if (!g_framecapture || !g_pending)
	{
		return;
	}

	g_pending = false;
	takescreenshot = false;

	if (!FIL_FileExists(g_path.c_str()))
	{
		I_OutputMsg("framecapture: %s was not written\n", g_path.c_str());
	}
	else
	{
		g_taken++;
		I_OutputMsg("framecapture: tic %u -> %s\n", (unsigned)g_shots[g_next], g_path.c_str());
	}

	g_next++;
	if (g_next >= g_shots.size())
	{
		Finish("done");
	}
}

UINT32 FrameCapture_Random(void)
{
	// xorshift32, then a multiply to spread the bits as the game's own generators do
	g_random ^= g_random << 13;
	g_random ^= g_random >> 17;
	g_random ^= g_random << 5;
	return g_random * 36548569u;
}

tic_t FrameCapture_AnimTime(void)
{
	return g_framecapture ? (tic_t)leveltime : I_GetTime();
}

const char *FrameCapture_ShotName(char *path, size_t pathsize)
{
	if (!g_framecapture || !g_pending)
	{
		return NULL;
	}

	M_MkdirEach(g_dir.c_str(), 0, 0755);
	strlcpy(path, g_dir.c_str(), pathsize);
	return g_name.c_str();
}

} // extern "C"
