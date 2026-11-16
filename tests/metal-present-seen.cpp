// What VibeRant Metal's presenter does with a frame when its window may be out of sight: the production decision
// (src/hardware/r_d3d12/metal/mtl_present_seen.h), driven here by a model of the layer and the display.
#include "hardware/r_d3d12/metal/mtl_present_seen.h"
#include <cassert>
#include <cstdio>
#include <deque>

using namespace rr_mtl_present;

namespace
{

// A CAMetalLayer of three drawables and a display that, at each refresh, takes the oldest drawable presented to a
// window (to show it, or to let it go, as behind a locked screen), or takes none from a window it has stopped serving.
// The game draws a frame every frameMs.
struct Model
{
	bool composited = true;  // the display takes the window's drawables
	bool shows = true;       // and puts them on screen
	bool reported = true;    // the system says the window is on screen
	bool usable = true, force = false;
	// How long after the display took a drawable Metal reports on it: at once (a Mac's own GPU), or a second later (a
	// virtual machine's display)
	double reportLagMs = 0;
	double refreshMs = 16, frameMs = 4;
	double handBackMs = 0;   // the layer gives a drawable no sooner than this after the last one

	Seen seen;
	double ms = 0, nextRefreshMs = 16;
	int queued = 0;          // presented and not yet taken by the display
	bool held = false;       // the display holds one (the one on screen)
	std::deque<double> presentedAt; // of the queued ones
	std::deque<std::pair<double, double>> reports; // due at, for a drawable presented at
	int unreported = 0, lateReports = 0, promptReports = 0;
	int presented = 0, probes = 0, unseen = 0, waits = 0, noDrawable = 0, shown = 0;
	double stalledMs = 0;    // time spent waiting for a drawable that didn't come
	double lastGivenMs = -1e9, waitedForProbesMs = 0, longestProbeWaitMs = 0;
	double longestGapMs = 0, lastShownMs = 0;

	// The display's refreshes up to now
	void Refresh()
	{
		for (; nextRefreshMs <= ms; nextRefreshMs += refreshMs)
		{
			if (!composited || !queued)
				continue;
			--queued;
			held = true;
			reports.push_back({nextRefreshMs + reportLagMs, presentedAt.front()});
			presentedAt.pop_front();
			if (!shows)
				continue;
			++shown;
			longestGapMs = std::max(longestGapMs, nextRefreshMs - lastShownMs);
			lastShownMs = nextRefreshMs;
		}
	}

	// What the presenter knows: the drawables not yet reported on, and whether most reports so far took long
	void Reports()
	{
		for (; !reports.empty() && reports.front().first <= ms; reports.pop_front())
		{
			--unreported;
			(reports.front().first - reports.front().second > kLateReportMs ? lateReports : promptReports)++;
		}
	}

	void Frame()
	{
		Refresh();
		Reports();
		const Now now = {ms, usable, reported, force, unreported, lateReports > promptReports};
		const rr_mtl_present::Frame frame = Decide(seen, now);
		if (frame == Frame::Present || frame == Frame::Probe)
		{
			// The display holds one drawable, so two can be out. Asking for a third waits until the display has taken
			// one, or, if it takes none for a second, gets nothing. A display that is slow with this window
			// (handBackMs) makes every request wait that long after the one before.
			const double asked = ms;
			while (kDrawables - (held ? 1 : 0) - queued <= 0 && composited && nextRefreshMs - asked < 1000)
			{
				ms = nextRefreshMs;
				Refresh();
			}
			if (handBackMs && lastGivenMs + handBackMs > ms)
				ms = std::min(lastGivenMs + handBackMs, asked + 1000);
			if (frame == Frame::Probe)
			{
				waitedForProbesMs += ms - asked;
				longestProbeWaitMs = std::max(longestProbeWaitMs, ms - asked);
			}
			if (kDrawables - (held ? 1 : 0) - queued <= 0 || ms - asked >= 1000)
			{
				ms = asked + 1000;
				stalledMs += 1000;
				++noDrawable;
				NoDrawable(seen, ms, frame);
			}
			else
			{
				GotDrawable(seen, frame, ms, ms - asked);
				lastGivenMs = ms;
				++queued;
				++unreported;
				presentedAt.push_back(ms);
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
	for (bool late : {false, true})
	{
		Model m;
		m.reportLagMs = late ? 1000 : 0;
		m.Run(10000);
		assert(m.presented >= 620 && m.presented <= 628 && m.unseen == 0 && m.probes == 0 && m.stalledMs == 0);
		assert(m.shown >= 620 && m.longestGapMs <= 16);
	}

	// A window that really is out of sight (the system says so, and the display doesn't take its drawables), where
	// reports are prompt: the game is never held up, which is the point of asking the system at all, and nothing is
	// presented but two probes at most
	for (double frameMs : {1.0, 4.0, 16.0, 40.0})
	{
		Model m;
		m.frameMs = frameMs;
		m.Run(1000);
		m.composited = m.reported = false;
		const int before = m.presented;
		m.Run(60000);
		assert(m.presented - before <= 2 && m.probes == m.presented - before);
		assert(m.stalledMs == 0 && m.noDrawable == 0 && m.waitedForProbesMs == 0);
		assert(m.unseen >= 60000 / frameMs - 3);
		// Back in sight: frames are presented again at once
		m.composited = m.reported = true;
		const int hidden = m.presented;
		m.Run(1000);
		assert(m.presented - hidden >= std::min(1000 / frameMs, 60.0) - 3 && m.stalledMs == 0);
	}
	// The same where reports have been late (a virtual machine's display): the count can't be used, so the layer's
	// last drawables go out and the game waits, but less and less often: ten times in the first ten minutes, a
	// second each, and once in five minutes after that
	{
		Model m;
		m.reportLagMs = 1000;
		m.Run(5000);
		assert(m.lateReports > m.promptReports);
		m.composited = m.reported = false;
		m.Run(600000);
		assert(m.stalledMs <= 10000 && m.noDrawable <= 10);
		const double first = m.stalledMs;
		m.Run(3600000);
		assert(m.stalledMs - first <= 13000);
		m.composited = m.reported = true;
		const int hidden = m.presented;
		m.Run(1000);
		assert(m.presented - hidden >= 55);
	}

	// The case this is for: the system says the window is out of sight, and it is wrong. The picture must keep moving:
	// a frame at every refresh of the display (or every frame, if the game draws fewer), with no wait. Whichever way
	// the reports come.
	for (bool late : {false, true})
		for (double frameMs : {1.0, 4.0, 16.0, 40.0})
		{
			Model m;
			m.reportLagMs = late ? 1000 : 0;
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
			// Where the count isn't used, a probe waits as a frame for a window in sight does: for the display, a
			// refresh at most
			assert(m.stalledMs == 0 && m.noDrawable == 0 && m.longestProbeWaitMs <= (late ? m.refreshMs : 0));
			assert(m.probes >= m.shown - before - 3);
		}

	// A window the display takes drawables from without showing them (behind a locked screen, where the system lets them
	// go at a pace of its own): frames go out at that pace, and the game is never held up
	{
		Model m;
		m.frameMs = 2;
		m.Run(1000);
		m.reported = m.shows = false;
		const int before = m.presented;
		m.Run(10000);
		assert(m.presented - before >= 600 && m.presented - before <= 630);
		assert(m.stalledMs == 0 && m.noDrawable == 0 && m.unseen >= 4000);
	}

	// A window the system serves slowly while it calls it out of sight: it takes the drawables, reports on them, and
	// gives the next one only after a while. Probes pause after a slow one, for longer each time, so the game waits
	// for a small and shrinking share of the time: at most six waits in the first minute, and four in the ten after.
	for (double handBackMs : {300.0, 500.0, 2000.0})
	{
		Model m;
		m.Run(1000);
		m.reported = m.shows = false;
		m.handBackMs = handBackMs;
		m.Run(60000);
		const double first = m.waitedForProbesMs; // the waits that got no drawable are in it
		assert(first <= 6000);
		m.Run(600000);
		assert(m.waitedForProbesMs - first <= 4000);
		// Back in sight: every frame again
		m.reported = m.shows = true;
		m.handBackMs = 0;
		const int before = m.presented;
		m.Run(1000);
		assert(m.presented - before >= 55);
	}
	// A hand-back that is only a little slow doesn't pause anything: a virtual machine's display took up to a tenth
	// of a second now and then
	{
		Model m;
		m.Run(1000);
		m.reported = false;
		m.handBackMs = 120;
		const int before = m.shown;
		m.Run(10000);
		assert(m.shown - before >= 80);
	}

	// The pause: a second after a slow probe or one without a drawable, then two, doubling to five minutes; over once a probe
	// is quick, or the window is in sight
	{
		Seen seen;
		double ms = 0, pause = 1000;
		for (int i = 0; i < 12; i++)
		{
			assert(Decide(seen, {ms, true, false, false, 0, false}) == Frame::Probe);
			if (i & 1)
				NoDrawable(seen, ms, Frame::Probe);
			else
				assert(!GotDrawable(seen, Frame::Probe, ms, 251));
			assert(Decide(seen, {ms + pause - 1, true, false, false, 0, false}) == Frame::Unseen);
			ms += pause;
			pause = std::min(300000.0, pause * 2);
		}
		assert(pause == 300000);
		// One quick probe doesn't end it: the next slow one pauses for five minutes again
		assert(Decide(seen, {ms, true, false, false, 0, false}) == Frame::Probe);
		assert(GotDrawable(seen, Frame::Probe, ms, 250));
		assert(Decide(seen, {ms + 1, true, false, false, 0, false}) == Frame::Probe);
		assert(!GotDrawable(seen, Frame::Probe, ms + 1, 900));
		assert(Decide(seen, {ms + 300000, true, false, false, 0, false}) == Frame::Unseen);
		ms += 300001;
		// Thirty in a row do
		for (int i = 0; i < 30; i++, ms++)
		{
			assert(Decide(seen, {ms, true, false, false, 0, false}) == Frame::Probe);
			assert(GotDrawable(seen, Frame::Probe, ms, 5));
		}
		assert(!GotDrawable(seen, Frame::Probe, ms, 900));
		assert(Decide(seen, {ms + 999, true, false, false, 0, false}) == Frame::Unseen);
		assert(Decide(seen, {ms + 1000, true, false, false, 0, false}) == Frame::Probe);
		assert(!GotDrawable(seen, Frame::Probe, ms + 1000, 900));
		ms += 1000;
		// In sight: no pause, and the next time out of sight starts afresh
		assert(Decide(seen, {ms + 2, true, true, false, 0, false}) == Frame::Present);
		assert(Decide(seen, {ms + 3, true, false, false, 0, false}) == Frame::Probe);
		// A window in sight may wait for its drawables as long as the display takes
		assert(GotDrawable(seen, Frame::Present, ms, 900));
	}

	// No probe while two drawables are unaccounted for: with the one on screen, the layer's three would be out
	{
		Seen seen;
		assert(Decide(seen, {0, true, false, false, 3, false}) == Frame::Unseen);
		assert(Decide(seen, {0, true, false, false, 2, false}) == Frame::Unseen);
		assert(Decide(seen, {0, true, false, false, 1, false}) == Frame::Probe);
		assert(Decide(seen, {0, true, false, false, 0, false}) == Frame::Probe);
		assert(kDrawables - 1 - kProbeOut >= 1);
		// Where reports are late the count isn't asked, only the pause
		assert(Decide(seen, {0, true, false, false, 40, true}) == Frame::Probe);
		NoDrawable(seen, 1000, Frame::Probe);
		assert(Decide(seen, {1999, true, false, false, 40, true}) == Frame::Unseen);
		assert(Decide(seen, {2000, true, false, false, 40, true}) == Frame::Probe);
	}

	// Without the probes (as the presenter was: only the first frame of a window went out regardless), a wrong report
	// freezes the picture. The model must be able to tell: this is the test's own negative control.
	{
		Model m;
		m.Run(1000);
		m.reported = false;
		m.seen.probeAtMs = 1e18; // no probes
		const int before = m.shown;
		m.Run(10000);
		assert(m.shown - before <= 2);
	}
	// And going by the count where reports are a second late, as an earlier version did, leaves the picture standing
	// for most of every second: the model can tell that too
	{
		Model m;
		m.reportLagMs = 1000;
		m.Run(3000);
		m.reported = false;
		m.lateReports = -1000000; // as if the reports weren't known to be late
		const int before = m.shown;
		m.longestGapMs = 0;
		m.lastShownMs = m.ms;
		m.Run(10000);
		assert(m.shown - before <= 30 && m.longestGapMs >= 900);
	}

	// A new window: its first frames go out although the system doesn't call it visible yet
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
		assert(m.presented == 2 && m.probes == 2 && m.stalledMs == 0);
	}

	// A window the game minimized or hid, or one without a size, gets nothing, whatever the system says; -mtlunseen
	// presents to any other
	{
		Seen seen;
		assert(Decide(seen, {0, false, true, true, 0, false}) == Frame::Unseen);
		assert(Decide(seen, {0, true, false, true, 5, false}) == Frame::Present);
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
		GotDrawable(seen, Frame::Present, now.ms, 1);
		NoDrawable(seen, now.ms, Frame::Present);
		now.ms += 250;
		assert(Decide(seen, now) == Frame::Present);
	}
	puts("metal_present_seen: passed");
	return 0;
}
