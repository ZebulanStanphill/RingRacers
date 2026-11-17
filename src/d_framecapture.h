// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d_framecapture.h
/// \brief Frame capture mode: `-shottics 70,210,350 [-shotdir <dir>]` with a replay: a staff ghost (`-shotghost RR_AERIALHIGHLANDS-blaze-MK`)
///        or a replay file (`-playdemo`, `+playdemo`).
///
/// Plays the replay with exactly one frame drawn per tic and no interpolation, takes a screenshot of the finished frame
/// (HUD included) when the level time reaches each listed tic, names it "<MAP>_<tic>.png" in the directory, and quits
/// after the last one or when the replay ends. For comparing what two renderers (or builds) draw; see
/// tests/frame-compare/README.md. Without -shottics nothing in here does anything.

#ifndef __D_FRAMECAPTURE_H__
#define __D_FRAMECAPTURE_H__

#include "doomtype.h"
#include "m_fixed.h"

#ifdef __cplusplus
extern "C" {
#endif

/// True when the game was started with -shottics.
extern boolean g_framecapture;

/// Reads -shottics and -shotdir. Fatal error on a bad tic list.
void FrameCapture_Init(void);

/// Start of each pass of the main loop: keeps the game at one tic per frame.
void FrameCapture_BeginLoop(void);

/// After a frame was drawn and before screenshots are taken: asks for one if this level time was listed.
/// `drawn` is false when the loop did not draw a frame.
void FrameCapture_Request(boolean drawn);

/// After screenshots were taken: checks the requested one was written, and quits when finished.
void FrameCapture_EndFrame(void);

/// The unsynced random numbers (M_Random and the HUD's) in capture mode: not rand(), which the game seeds with the time
/// and which differs between C libraries, but a generator that starts from the same state at every tic of the level.
UINT32 FrameCapture_Random(void);

/// I_GetTime() for the cosmetic animations the clock of the machine drives (a dancing string, a blinking button
/// prompt): in capture mode the level time, so that they are the same on every run; otherwise I_GetTime().
tic_t FrameCapture_AnimTime(void);

/// The time between two frames (renderdeltatics) for the animations that go by it, such as Legacy GL's water and heat
/// waves and the HUD's scrolling: in capture mode one tic, however long the frame before took; otherwise `measured`.
fixed_t FrameCapture_DeltaTics(fixed_t measured);

/// For M_DoScreenShot: the name of the requested screenshot, in capture mode (and `path` becomes its directory, with a
/// trailing separator); NULL otherwise, for the ordinary numbered name.
const char *FrameCapture_ShotName(char *path, size_t pathsize);

#ifdef __cplusplus
}
#endif

#endif // __D_FRAMECAPTURE_H__
