// Screen Scaling's placement (src/screen_scaling.hpp), which both renderers use; runs without a graphics API or GPU.
#include "screen_scaling.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace screen_scaling;

// The rectangle and k a placement must have, from expectations worked out by hand
static void expectPlaced(int frameW, int frameH, int screenW, int screenH, int mode, int x, int y, int w, int h, int k)
{
	const Placement p = Compute(frameW, frameH, screenW, screenH, mode);
	if (p.fit || p.x != x || p.y != y || p.w != w || p.h != h || p.k != k)
	{
		std::fprintf(stderr, "%dx%d in %dx%d, mode %d: got fit %d (%d, %d) %dx%d k=%d, expected (%d, %d) %dx%d k=%d\n",
			frameW, frameH, screenW, screenH, mode, p.fit, p.x, p.y, p.w, p.h, p.k, x, y, w, h, k);
		assert(false);
	}
}

// A placement left to the system's fit: the whole screen and no scale
static void expectFit(int frameW, int frameH, int screenW, int screenH, int mode)
{
	const Placement p = Compute(frameW, frameH, screenW, screenH, mode);
	if (!p.fit || p.x != 0 || p.y != 0 || p.k != 0 || p.w != (screenW > 0 ? screenW : 0) || p.h != (screenH > 0 ? screenH : 0))
	{
		std::fprintf(stderr, "%dx%d in %dx%d, mode %d: expected fit, got fit %d (%d, %d) %dx%d k=%d\n",
			frameW, frameH, screenW, screenH, mode, p.fit, p.x, p.y, p.w, p.h, p.k);
		assert(false);
	}
}

static void fitTests()
{
	// cv_scr_scaling's values (src/cvars.cpp); Fit is the default
	static_assert(kFit == 0 && kInteger == 1 && kOneToOne == 2, "the cvar's values changed");
	// Fit is the whole screen, scaled by whoever draws it, whatever the sizes
	expectFit(640, 400, 1920, 1080, kFit);
	expectFit(640, 400, 640, 400, kFit);
	expectFit(1920, 1080, 640, 400, kFit);
	expectFit(320, 200, 1280, 720, kFit);
	// A mode that is not listed is Fit as well
	expectFit(640, 400, 1920, 1080, -1);
	expectFit(640, 400, 1920, 1080, 3);
	// So is any size that makes no sense
	expectFit(0, 400, 1920, 1080, kInteger);
	expectFit(640, 0, 1920, 1080, kOneToOne);
	expectFit(640, 400, 0, 1080, kInteger);
	expectFit(640, 400, 1920, -5, kOneToOne);
	expectFit(-640, 400, 1920, 1080, kInteger);
	expectFit(640, 400, 0, 0, kFit);
}

static void integerTests()
{
	// An exact fit, at 1 and at 2
	expectPlaced(640, 400, 1280, 800, kInteger, 0, 0, 1280, 800, 2);
	expectPlaced(640, 400, 640, 400, kInteger, 0, 0, 640, 400, 1);
	// k is 1: the frame fits once, with a margin all round
	expectPlaced(640, 400, 1000, 700, kInteger, 180, 150, 640, 400, 1);
	expectPlaced(1280, 720, 1920, 1080, kInteger, 320, 180, 1280, 720, 1);
	// k is 2, limited by the height (1920 / 640 is 3, 1080 / 400 is 2)
	expectPlaced(640, 400, 1920, 1080, kInteger, 320, 140, 1280, 800, 2);
	expectPlaced(854, 480, 1920, 1080, kInteger, 106, 60, 1708, 960, 2);
	// k is 3, filling the screen, and filling only the height
	expectPlaced(640, 400, 1920, 1200, kInteger, 0, 0, 1920, 1200, 3);
	expectPlaced(1280, 720, 3840, 2160, kInteger, 0, 0, 3840, 2160, 3);
	// k is 5, limited by the height (3840 / 640 is 6, 2160 / 400 is 5)
	expectPlaced(640, 400, 3840, 2160, kInteger, 320, 80, 3200, 2000, 5);
	// Odd remainders: the extra pixel is on the right and the bottom (the margins are floor and ceiling of half)
	expectPlaced(640, 400, 1001, 701, kInteger, 180, 150, 640, 400, 1);
	expectPlaced(320, 200, 1001, 601, kInteger, 20, 0, 960, 600, 3);
	expectPlaced(320, 200, 1000, 601, kInteger, 20, 0, 960, 600, 3);
	// A screen much wider or taller than the frame: the other dimension decides
	expectPlaced(320, 200, 2000, 300, kInteger, 840, 50, 320, 200, 1);
	expectPlaced(320, 200, 400, 1000, kInteger, 40, 400, 320, 200, 1);
	expectPlaced(640, 400, 640, 1000, kInteger, 0, 300, 640, 400, 1);
	// A frame larger than the screen in one dimension, or both, is fit by the system
	expectFit(640, 400, 639, 1000, kInteger);
	expectFit(640, 400, 1000, 399, kInteger);
	expectFit(1920, 1080, 1280, 720, kInteger);
	expectFit(640, 400, 320, 200, kInteger);
	expectFit(640, 400, 1, 1, kInteger);
}

static void oneToOneTests()
{
	// Unscaled and centered, however large the screen is
	expectPlaced(640, 400, 1920, 1080, kOneToOne, 640, 340, 640, 400, 1);
	expectPlaced(640, 400, 640, 400, kOneToOne, 0, 0, 640, 400, 1);
	expectPlaced(1280, 720, 3840, 2160, kOneToOne, 1280, 720, 1280, 720, 1);
	// Odd remainders: the extra pixel is on the right and the bottom
	expectPlaced(640, 400, 641, 401, kOneToOne, 0, 0, 640, 400, 1);
	expectPlaced(640, 400, 642, 403, kOneToOne, 1, 1, 640, 400, 1);
	expectPlaced(640, 400, 643, 640, kOneToOne, 1, 120, 640, 400, 1);
	// A frame larger than the screen in one dimension, or both, is fit by the system
	expectFit(641, 400, 640, 1000, kOneToOne);
	expectFit(640, 401, 1000, 400, kOneToOne);
	expectFit(1920, 1080, 1280, 720, kOneToOne);
	expectFit(640, 400, 100, 100, kOneToOne);
}

// The results hold for any sizes: the rectangle is the frame times k, inside the screen and centered with the extra pixel
// of an odd remainder at the right and bottom; Integer's k can't be larger; 1:1's is 1; a frame larger than the screen is fit.
static void sweepTests()
{
	const int frames[][2] = {{1, 1}, {2, 3}, {64, 64}, {320, 200}, {320, 224}, {640, 400}, {854, 480}, {1280, 720}, {1920, 1080}};
	int integerPlaced = 0, oneToOnePlaced = 0, fits = 0;
	for (const auto &frame : frames)
	{
		for (int screenW = 1; screenW <= 2200; screenW += (screenW < 70 ? 1 : 37))
		{
			for (int screenH = 1; screenH <= 1300; screenH += (screenH < 70 ? 1 : 41))
			{
				for (const int mode : {kInteger, kOneToOne})
				{
					const Placement p = Compute(frame[0], frame[1], screenW, screenH, mode);
					const bool larger = frame[0] > screenW || frame[1] > screenH;
					assert(p.fit == larger);
					if (p.fit)
					{
						assert(p.x == 0 && p.y == 0 && p.w == screenW && p.h == screenH && p.k == 0);
						fits++;
						continue;
					}
					assert(p.k >= 1 && p.w == frame[0] * p.k && p.h == frame[1] * p.k);
					assert(p.x >= 0 && p.y >= 0 && p.x + p.w <= screenW && p.y + p.h <= screenH);
					assert(p.x == (screenW - p.w) / 2 && p.y == (screenH - p.h) / 2);
					const int right = screenW - p.w - p.x, bottom = screenH - p.h - p.y;
					assert(right == p.x || right == p.x + 1);
					assert(bottom == p.y || bottom == p.y + 1);
					if (mode == kOneToOne)
					{
						assert(p.k == 1);
						oneToOnePlaced++;
					}
					else
					{
						// One more would not fit
						assert((p.k + 1) * frame[0] > screenW || (p.k + 1) * frame[1] > screenH);
						integerPlaced++;
					}
				}
				// Fit is never placed
				assert(Compute(frame[0], frame[1], screenW, screenH, kFit).fit);
			}
		}
	}
	assert(integerPlaced > 1000 && oneToOnePlaced > 1000 && fits > 1000);
}

// FitRect's rectangle, worked out by hand: not fit, k 0 (scaled by a factor that isn't a whole number)
static void expectFitRect(int frameW, int frameH, int screenW, int screenH, int x, int y, int w, int h)
{
	const Placement p = FitRect(frameW, frameH, screenW, screenH);
	if (p.fit || p.k != 0 || p.x != x || p.y != y || p.w != w || p.h != h)
	{
		std::fprintf(stderr, "%dx%d in %dx%d, FitRect: got fit %d (%d, %d) %dx%d k=%d, expected (%d, %d) %dx%d k=0\n",
			frameW, frameH, screenW, screenH, p.fit, p.x, p.y, p.w, p.h, p.k, x, y, w, h);
		assert(false);
	}
}

// A size that makes no sense gives what Compute gives: the whole screen (0 where it isn't positive), for the renderer to scale
static void expectFitRectWhole(int frameW, int frameH, int screenW, int screenH)
{
	const Placement p = FitRect(frameW, frameH, screenW, screenH);
	const Placement c = Compute(frameW, frameH, screenW, screenH, kFit);
	if (!p.fit || p.x != 0 || p.y != 0 || p.k != 0 || p.w != (screenW > 0 ? screenW : 0) || p.h != (screenH > 0 ? screenH : 0)
		|| p.fit != c.fit || p.x != c.x || p.y != c.y || p.w != c.w || p.h != c.h || p.k != c.k)
	{
		std::fprintf(stderr, "%dx%d in %dx%d, FitRect: expected the whole screen, got fit %d (%d, %d) %dx%d k=%d\n",
			frameW, frameH, screenW, screenH, p.fit, p.x, p.y, p.w, p.h, p.k);
		assert(false);
	}
}

static void fitRectTests()
{
	// A screen wider than the frame's aspect ratio: the frame fills the height, with bars at the sides
	expectFitRect(640, 400, 1920, 1080, 96, 0, 1728, 1080);
	expectFitRect(640, 400, 2000, 400, 680, 0, 640, 400);
	// Taller: it fills the width, with bars above and below
	expectFitRect(640, 400, 1000, 1000, 0, 187, 1000, 625);
	expectFitRect(1280, 720, 1000, 1500, 0, 469, 1000, 562);
	// Exactly the frame's aspect ratio, at the frame's size and at other sizes: no bars
	expectFitRect(640, 400, 640, 400, 0, 0, 640, 400);
	expectFitRect(640, 400, 1280, 800, 0, 0, 1280, 800);
	expectFitRect(1280, 720, 1920, 1080, 0, 0, 1920, 1080);
	expectFitRect(320, 200, 160, 100, 0, 0, 160, 100);
	// The frame isn't the one 16:9 case: 4:3 in 16:9
	expectFitRect(640, 480, 1920, 1080, 240, 0, 1440, 1080);
	// A frame larger than the screen is scaled down
	expectFitRect(1920, 1080, 640, 400, 0, 20, 640, 360);
	// Odd remainders: the extra pixel is on the right and the bottom
	expectFitRect(320, 200, 1000, 601, 19, 0, 961, 601);
	expectFitRect(640, 400, 1001, 700, 0, 37, 1001, 625);
	expectFitRect(100, 100, 201, 101, 50, 0, 101, 101);
	// The size along the other side is rounded down: 854x480 in 1366x768 is 767.77 high
	expectFitRect(854, 480, 1366, 768, 0, 0, 1366, 767);
	// A side is never less than 1
	expectFitRect(1000, 1, 10, 10, 0, 4, 10, 1);
	expectFitRect(1, 1000, 10, 10, 4, 0, 1, 10);
	expectFitRect(1, 1, 1, 1, 0, 0, 1, 1);
	// A size that makes no sense is the whole screen, like Compute's Fit
	expectFitRectWhole(0, 400, 1920, 1080);
	expectFitRectWhole(640, 0, 1920, 1080);
	expectFitRectWhole(-640, 400, 1920, 1080);
	expectFitRectWhole(640, 400, 0, 1080);
	expectFitRectWhole(640, 400, 1920, -5);
	expectFitRectWhole(640, 400, 0, 0);
	expectFitRectWhole(0, 0, 0, 0);
	expectFitRectWhole(-1, -1, 640, 400);
}

// The results hold for any sizes: the rectangle is inside the screen, spans it along one side and has the frame's aspect
// ratio along the other, rounded down; the remainder is split with the extra pixel at the right and bottom.
static void fitRectSweepTests()
{
	const int frames[][2] = {{1, 1}, {2, 3}, {3, 2}, {64, 64}, {320, 200}, {320, 224}, {640, 400}, {854, 480}, {1280, 720}, {1920, 1080}};
	int wider = 0, taller = 0, exact = 0;
	for (const auto &frame : frames)
	{
		for (int screenW = 1; screenW <= 2200; screenW += (screenW < 70 ? 1 : 37))
		{
			for (int screenH = 1; screenH <= 1300; screenH += (screenH < 70 ? 1 : 41))
			{
				const Placement p = FitRect(frame[0], frame[1], screenW, screenH);
				assert(!p.fit && p.k == 0);
				assert(p.w >= 1 && p.h >= 1 && p.w <= screenW && p.h <= screenH);
				assert(p.w == screenW || p.h == screenH);
				assert(p.x == (screenW - p.w) / 2 && p.y == (screenH - p.h) / 2);
				const int right = screenW - p.w - p.x, bottom = screenH - p.h - p.y;
				assert(right == p.x || right == p.x + 1);
				assert(bottom == p.y || bottom == p.y + 1);
				// The aspect ratio, to within what whole pixels allow: the side that isn't the screen's is the frame's
				// proportion of the one that is, rounded down, unless that is below 1
				const long long fw = frame[0], fh = frame[1];
				if (p.h == screenH && p.w != screenW)
				{
					assert(p.w == 1 || ((long long)p.w * fh <= (long long)p.h * fw && (long long)(p.w + 1) * fh > (long long)p.h * fw));
					wider++;
				}
				else if (p.w == screenW && p.h != screenH)
				{
					assert(p.h == 1 || ((long long)p.h * fw <= (long long)p.w * fh && (long long)(p.h + 1) * fw > (long long)p.w * fh));
					taller++;
				}
				else
				{
					// Fills both sides: the screen has the frame's aspect ratio, or one side is so thin it was held at 1
					assert(p.x == 0 && p.y == 0);
					exact++;
				}
			}
		}
	}
	assert(wider > 1000 && taller > 1000 && exact > 10);
}

int main()
{
	// The calculation is usable at compile time
	constexpr Placement compileTime = Compute(640, 400, 1920, 1080, kInteger);
	static_assert(!compileTime.fit && compileTime.k == 2 && compileTime.x == 320 && compileTime.y == 140, "Compute");
	constexpr Placement compileTimeFit = FitRect(640, 400, 1920, 1080);
	static_assert(!compileTimeFit.fit && compileTimeFit.k == 0 && compileTimeFit.x == 96 && compileTimeFit.w == 1728, "FitRect");
	fitTests();
	fitRectTests();
	fitRectSweepTests();
	integerTests();
	oneToOneTests();
	sweepTests();
	std::puts("screen scaling placement tests passed");
	return 0;
}
