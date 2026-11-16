// What VibeRant Metal's presenter does with a frame when its window may be out of sight: the production decision
// (src/hardware/r_d3d12/metal/mtl_present_seen.h), driven here by a model of the layer and the display.
#include "hardware/r_d3d12/metal/mtl_present_seen.h"
#include <cassert>

using namespace rr_mtl_present;

namespace
{

// A CAMetalLayer of three drawables and a display that, at each refresh, shows the oldest drawable presented to a
// window it is showing, and takes none from a window it isn't. The game draws a frame every frameMs.
struct Model
{
	bool composited = true;  // the display takes the window's drawables
	bool reported = true;    // the system says the window is on screen
	bool usable = true, force = false;
	double refreshMs = 16, frameMs = 4;

	Seen seen;
	double ms = 0, nextRefreshMs = 16;
	int queued = 0;          // presented and not yet shown
	int presented = 0, probes = 0, unseen = 0, waits = 0, noDrawable = 0, shown = 0;
	double stalledMs = 0;    // time spent waiting for a drawable that didn't come
	double longestGapMs = 0, lastShownMs = 0;

	// The display's refreshes up to now
	void Refresh()
	{
		for (; nextRefreshMs <= ms; nextRefreshMs += refreshMs)
		{
			if (!composited || !queued)
				continue;
			--queued;
			++shown;
			longestGapMs = std::max(longestGapMs, nextRefreshMs - lastShownMs);
			lastShownMs = nextRefreshMs;
		}
	}

	void Frame()
	{
		Refresh();
		const Now now = {ms, usable, reported, force, queued, false};
		const rr_mtl_present::Frame frame = Decide(seen, now);
		if (frame == Frame::Present || frame == Frame::Probe)
		{
			// One drawable is on screen, so two can be out. Asking for a third waits until the display has shown one,
			// or, if it shows none for a second, gets nothing.
			if (queued >= 2 && composited)
			{
				ms = nextRefreshMs;
				Refresh();
			}
			if (queued >= 2)
			{
				ms += 1000;
				stalledMs += 1000;
				++noDrawable;
				NoDrawable(seen, ms, frame);
			}
			else
			{
				GotDrawable(seen);
				++queued;
				++presented;
				probes += frame == Frame::Probe;
			}
		}
		else if (frame == Frame::Unseen)
			++unseen;
		else
			++waits;
		ms += frameMs;
	}

	void Run(double forMs)
	{
		for (const double end = ms + forMs; ms < end;)
			Frame();
	}
};

} // namespace

int main()
{
	// A window in sight: every frame is presented, at the display's pace, and none waits for nothing
	{
		Model m;
		m.Run(10000);
		assert(m.presented >= 620 && m.presented <= 628 && m.unseen == 0 && m.probes == 0 && m.stalledMs == 0);
		assert(m.shown >= 620 && m.longestGapMs <= 16);
	}

	// A window that really is out of sight (the system says so, and the display doesn't take its drawables): the
	// game is never held up, which is the point of asking the system at all, and nothing is presented but, at most,
	// one probe
	for (double frameMs : {1.0, 4.0, 16.0, 40.0})
	{
		Model m;
		m.frameMs = frameMs;
		m.Run(1000);
		m.composited = m.reported = false;
		const int before = m.presented, queued = m.queued;
		m.Run(60000);
		assert(m.presented - before == (queued ? 0 : 1) && m.probes == m.presented - before);
		assert(m.stalledMs == 0 && m.noDrawable == 0);
		assert(m.unseen >= 60000 / frameMs - 2);
		// Back in sight: frames are presented again at once
		m.composited = m.reported = true;
		const int hidden = m.presented;
		m.Run(1000);
		assert(m.presented - hidden >= std::min(1000 / frameMs, 60.0) - 2 && m.stalledMs == 0);
	}

	// The case this is for: the system says the window is out of sight, and it is wrong. The picture must keep moving:
	// a frame at every refresh of the display (or every frame, if the game draws fewer), with no wait.
	for (double frameMs : {1.0, 4.0, 16.0, 40.0})
	{
		Model m;
		m.frameMs = frameMs;
		m.Run(1000);
		m.reported = false;
		const int before = m.shown;
		m.lastShownMs = m.ms;
		m.longestGapMs = 0;
		m.Run(10000);
		const double period = std::max(frameMs, m.refreshMs);
		assert(m.shown - before >= 10000 / period - 3);
		assert(m.longestGapMs <= period + m.refreshMs + frameMs);
		assert(m.stalledMs == 0 && m.noDrawable == 0);
		assert(m.probes >= m.shown - before - 2);
	}

	// The same from the window's first frame on: a new window doesn't count as visible until the system has put it on
	// screen, and its first frame is presented regardless
	{
		Model m;
		m.reported = false;
		m.Run(100);
		assert(m.presented >= 5 && m.probes == m.presented && m.stalledMs == 0);
	}
	{
		Model m;
		m.reported = m.composited = false;
		m.Run(100);
		assert(m.presented == 1 && m.probes == 1 && m.stalledMs == 0);
	}

	// Without the probes (as the presenter was: only the first frame of a window went out regardless), a wrong report
	// freezes the picture. The model must be able to tell: this is the test's own negative control.
	{
		Model m;
		m.Run(1000);
		m.reported = false;
		m.seen.stopped = true; // no probes
		const int before = m.shown;
		m.Run(10000);
		assert(m.shown - before <= 2);
	}

	// A probe the layer has no drawable for (which the count of drawables out should prevent, but a layer may keep its
	// own count): one wait, and then no more probes until the window is in sight
	{
		Seen seen;
		Now now = {0, true, false, false, 0, false};
		assert(Decide(seen, now) == Frame::Probe);
		NoDrawable(seen, 1000, Frame::Probe);
		now.ms = 1000;
		for (int i = 0; i < 100; ++i, now.ms += 100)
			assert(Decide(seen, now) == Frame::Unseen);
		now.visible = true;
		assert(Decide(seen, now) == Frame::Present);
		now.visible = false;
		assert(Decide(seen, now) == Frame::Probe);
	}

	// A dropped probe is tried again a second later, not at once
	{
		Seen seen;
		Now now = {0, true, false, false, 0, false};
		assert(Decide(seen, now) == Frame::Probe);
		now = {50, true, false, false, 0, true};
		assert(Decide(seen, now) == Frame::Unseen);
		now = {1049, true, false, false, 0, false};
		assert(Decide(seen, now) == Frame::Unseen);
		now.ms = 1050;
		assert(Decide(seen, now) == Frame::Probe);
	}

	// A window the game minimized or hid, or one without a size, gets nothing, whatever the system says; -mtlunseen
	// presents to any other
	{
		Seen seen;
		Now now = {0, false, true, true, 0, false};
		assert(Decide(seen, now) == Frame::Unseen);
		now = {0, true, false, true, 5, false};
		assert(Decide(seen, now) == Frame::Present);
	}

	// A window in sight whose layer gives no drawable: the frame isn't shown, and the layer is left alone for a quarter
	// of a second, then half, up to four seconds, and from the start again once it gives one
	{
		Seen seen;
		Now now = {0, true, true, false, 0, false};
		double expected = 250;
		for (int i = 0; i < 7; ++i)
		{
			assert(Decide(seen, now) == Frame::Present);
			NoDrawable(seen, now.ms, Frame::Present);
			now.ms += expected - 1;
			assert(Decide(seen, now) == Frame::Wait);
			now.ms += 1;
			expected = std::min(4000.0, expected * 2);
		}
		assert(expected == 4000);
		assert(Decide(seen, now) == Frame::Present);
		GotDrawable(seen);
		NoDrawable(seen, now.ms, Frame::Present);
		now.ms += 250;
		assert(Decide(seen, now) == Frame::Present);
	}
	return 0;
}
