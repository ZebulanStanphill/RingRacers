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
/// sight still gets frames, as long as asking for a drawable can't wait: a frame is presented (a probe) while at most
/// one drawable presented before it is unaccounted for. Metal reports for every presented drawable when the system is
/// done with it. The layer has three drawables. With one on screen and at most one more out, a third is free, and
/// asking for it doesn't wait. While the system takes what it is given, reports keep coming and a probe follows each,
/// so a window that is in fact on screen keeps its picture moving, at up to one frame per refresh of the display.
/// When the system takes nothing, as from a window it has stopped serving, two probes go out and no more.
///
/// "At most one" and not "none", so that a report that is a frame late doesn't halve the pace. But reports can be
/// much later than that. On a Mac's own GPU, behind a locked screen, each came at once. On a virtual machine's display
/// (GitHub's runners) each came about a second after its drawable was presented, for a window in plain sight too,
/// while the layer went on giving drawables without a wait: there the count of drawables out says nothing, and
/// waiting for it left a picture that stood still for seconds. So the presenter tells this the time each report took,
/// and where most reports have come late, a probe goes out without asking the count.
///
/// A report also doesn't say reliably whether the drawable was shown: its time is zero for a frame the system
/// dropped, and was zero for every frame of a window in plain sight on that virtual machine. So the time is only
/// counted for the log. Behind a locked screen this means frames presented to nobody at the system's pace, which
/// was a few milliseconds per drawable on the Macs this ran on.
///
/// The system might also hand drawables back slowly to a window nobody sees, as it slows other work of such
/// windows. Then every probe would wait, and the game with it. So after a probe whose drawable took longer than a
/// quarter of a second to come, or didn't come, no probe goes out for a second, then two, doubling to five minutes,
/// until thirty in a row have come quickly or the window is in sight: the game waits less and less often, and a
/// hiccup doesn't stop the picture for good. Where reports are prompt this hardly comes up, since the count keeps
/// probes from asking a layer whose drawables are all out. Where they are late it is the only guard, and a window
/// that really is out of sight there costs the game a second's wait, at most, at ever longer intervals.
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
	Probe,   // the system calls the window out of sight, and a drawable is surely free: present this frame
	Unseen,  // not presented: the window is out of sight
	Wait,    // not presented: the layer gave no drawable a moment ago
};

struct Seen
{
	// After a probe whose drawable was slow to come or didn't: when the next may go, how long the pause was, and how
	// many probes in a row have been quick since
	double probeAtMs = 0, pauseMs = 0;
	int quick = 0;
	// After the layer gave no drawable to a window in sight: when to ask again, and how long to wait the next time
	double retryAtMs = 0, retryMs = 0;
};

struct Now
{
	double ms;      // a clock that only goes forward
	bool usable;    // the window has a size and the game hasn't minimized or hidden it
	bool visible;   // the system says some of the window is on screen
	bool force;     // -mtlunseen: present whatever the system says
	int out;        // presented drawables that Metal hasn't yet reported on
	bool late;      // most of Metal's reports have taken longer than kLateReportMs to come, so out says nothing
};

// The layer's drawables (CAMetalLayer's maximumDrawableCount, which the presenter sets): one on screen, and with at
// most kProbeOut more out when a probe asks, one free
constexpr int kDrawables = 3, kProbeOut = 1;
constexpr double kFirstRetryMs = 250, kLastRetryMs = 4000;
// A drawable that takes longer than this to come is one the system is in no hurry to give
constexpr double kSlowProbeMs = 250;
constexpr double kFirstPauseMs = 1000, kLastPauseMs = 300000;
// A report that takes longer than this is no use for telling what the layer has free
constexpr double kLateReportMs = 250;
// After this many quick probes in a row the next slow one pauses for a second again. One quick probe says little: a
// layer that gives a drawable every so often gives the first one after a pause at once.
constexpr int kQuickProbes = 30;

inline Frame Decide(Seen &seen, const Now &now)
{
	// A window without a size, or one the game itself minimized or hid: there is nothing to be wrong about
	if (!now.usable)
		return Frame::Unseen;
	if (now.visible || now.force)
	{
		seen.probeAtMs = seen.pauseMs = 0; // the next time out of sight starts afresh
		seen.quick = 0;
		return now.ms < seen.retryAtMs ? Frame::Wait : Frame::Present;
	}
	if (now.ms < seen.probeAtMs || (!now.late && now.out > kProbeOut))
		return Frame::Unseen;
	return Frame::Probe;
}

namespace seen_detail
{
inline void Pause(Seen &seen, double nowMs)
{
	seen.quick = 0;
	seen.pauseMs = std::min(kLastPauseMs, std::max(kFirstPauseMs, seen.pauseMs * 2));
	seen.probeAtMs = nowMs + seen.pauseMs;
}
} // namespace seen_detail

// The layer gave a drawable for the frame Decide said to present, after waitedMs. Returns false if that was a probe
// and the wait was long enough for probes to pause.
inline bool GotDrawable(Seen &seen, Frame frame, double nowMs, double waitedMs)
{
	seen.retryMs = 0;
	if (frame != Frame::Probe)
		return true;
	if (waitedMs > kSlowProbeMs)
	{
		seen_detail::Pause(seen, nowMs);
		return false;
	}
	if (++seen.quick >= kQuickProbes)
		seen.pauseMs = 0;
	return true;
}

// The layer gave none, after its wait of a second
inline void NoDrawable(Seen &seen, double nowMs, Frame frame)
{
	if (frame == Frame::Probe)
	{
		seen_detail::Pause(seen, nowMs);
		return;
	}
	// Not shown, and the layer isn't asked again for a pause that doubles up to four seconds
	seen.retryMs = std::min(kLastRetryMs, std::max(kFirstRetryMs, seen.retryMs * 2));
	seen.retryAtMs = nowMs + seen.retryMs;
}

} // namespace rr_mtl_present

#endif
