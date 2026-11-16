// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl_present_seen.h
/// \brief Whether the Metal presenter presents a frame to a window that may be out of sight
///
/// A CAMetalLayer's drawables only come back from the display. A layer whose window isn't being shown runs out of
/// them, and asking it for the next one then waits a second and gives nothing, which would hold the game up at every
/// frame. So the presenter asks the system whether any of the window is on screen (NSWindow's occlusion state) and
/// doesn't present every frame while none is.
///
/// But the system's answer is not the last word. If it were ever wrong (a window that is on screen and reported as
/// covered), presenting nothing would freeze the picture while the game runs on. So a window the system calls out of
/// sight still gets frames, one at a time: a frame is presented (a probe) only when none presented before it is
/// still out. Metal reports for every presented drawable whether and when it reached the display. A probe that was
/// shown is followed by the next frame at once, so a window that is in fact on screen keeps its picture moving, at
/// up to one frame per refresh of the display. A probe that is never heard of again, as for a window that really is
/// out of sight, is followed by nothing; and since no drawable but the one on screen is out when a probe asks for
/// one, the layer has one to give and asking it doesn't wait. A probe the system dropped is followed by another try
/// a second later.
///
/// Plain C++, with no header of Apple's or the game's, so that tests/metal-present-seen.cpp can run it anywhere.

#ifndef RR_MTL_PRESENT_SEEN_H
#define RR_MTL_PRESENT_SEEN_H

#include <algorithm>

namespace rr_mtl_present
{

// What to do with a frame
enum class Frame
{
	Present, // the window is in sight
	Probe,   // the system calls the window out of sight, and nothing presented before is still out: present this frame
	Unseen,  // not presented: the window is out of sight
	Wait,    // not presented: the layer gave no drawable a moment ago
};

struct Seen
{
	// The layer had no drawable for a probe, which is the wait this is all here to avoid: no more until the window is in sight
	bool stopped = false;
	// The earliest time of the next probe, after one that was dropped
	double probeAtMs = 0;
	// After the layer gave no drawable to a window in sight: when to ask again, and how long to wait the next time
	double retryAtMs = 0, retryMs = 0;
};

struct Now
{
	double ms;      // a clock that only goes forward
	bool usable;    // the window has a size and the game hasn't minimized or hidden it
	bool visible;   // the system says some of the window is on screen
	bool force;     // -mtlunseen: present whatever the system says
	int out;        // presented drawables that Metal hasn't yet reported as shown or dropped
	bool dropped;   // since the last frame, Metal reported a probe as dropped
};

constexpr double kDroppedProbeMs = 1000;
constexpr double kFirstRetryMs = 250, kLastRetryMs = 4000;

inline Frame Decide(Seen &seen, const Now &now)
{
	if (now.dropped)
		seen.probeAtMs = now.ms + kDroppedProbeMs;
	// A window without a size, or one the game itself minimized or hid: there is nothing to be wrong about
	if (!now.usable)
		return Frame::Unseen;
	if (now.visible || now.force)
	{
		// The next time out of sight starts afresh
		seen.stopped = false;
		seen.probeAtMs = 0;
		return now.ms < seen.retryAtMs ? Frame::Wait : Frame::Present;
	}
	if (seen.stopped || now.out > 0 || now.ms < seen.probeAtMs)
		return Frame::Unseen;
	return Frame::Probe;
}

// The layer gave a drawable for the frame Decide said to present
inline void GotDrawable(Seen &seen)
{
	seen.retryMs = 0;
}

// The layer gave none, after its wait of a second
inline void NoDrawable(Seen &seen, double nowMs, Frame frame)
{
	if (frame == Frame::Probe)
	{
		seen.stopped = true;
		return;
	}
	// Not shown, and the layer isn't asked again for a pause that doubles up to four seconds
	seen.retryMs = std::min(kLastRetryMs, std::max(kFirstRetryMs, seen.retryMs * 2));
	seen.retryAtMs = nowMs + seen.retryMs;
}

} // namespace rr_mtl_present

#endif
