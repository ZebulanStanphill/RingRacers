// Whether a drawable holds the frame that VibeRant Metal's presenter placed in it: the production comparison
// (src/hardware/r_d3d12/metal/mtl_present_check.h, what -mtlcheck judges with), given drawables made here from a
// frame the way the driver's placement makes them, and drawables that are wrong in the ways a presenter could be.
#include "hardware/r_d3d12/metal/mtl_present_check.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace rr_mtl_present;

namespace
{

struct Image
{
	int width, height, pitch;
	bool bgra;
	std::vector<uint8_t> bytes;
	Image(int w, int h, bool b) : width(w), height(h), pitch((w * 4 + 255) & ~255), bgra(b), bytes((size_t)pitch * h, 0) {}
	uint8_t *At(int x, int y) { return &bytes[(size_t)y * pitch + (size_t)x * 4]; }
	void Set(int x, int y, int r, int g, int b)
	{
		uint8_t *p = At(x, y);
		p[bgra ? 2 : 0] = (uint8_t)r, p[1] = (uint8_t)g, p[bgra ? 0 : 2] = (uint8_t)b, p[3] = 255;
	}
	void Get(int x, int y, double rgb[3]) const
	{
		const uint8_t *p = &bytes[(size_t)y * pitch + (size_t)x * 4];
		rgb[0] = p[bgra ? 2 : 0], rgb[1] = p[1], rgb[2] = p[bgra ? 0 : 2];
	}
	Pixels View() const { return {bytes.data(), width, height, pitch, bgra}; }
};

unsigned g_seed = 12345;
int Random(int below)
{
	g_seed = g_seed * 1664525u + 1013904223u;
	return (int)((g_seed >> 8) % (unsigned)below);
}

// Something like a frame of the game: a sky that fades, a floor of small checks, flat patches of strong color, and a
// band of noise, with nothing the same left and right or top and bottom
Image GameFrame(int w, int h, int variant = 0)
{
	Image frame(w, h, false);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			int r, g, b;
			if (y < h * 2 / 5)
				r = 40 + 80 * y / h + variant * 60, g = 90 + 200 * y / h, b = 255 - 100 * x / w;
			else if (y < h * 4 / 5)
			{
				const bool check = ((x / 4) + (y / 4)) & 1;
				r = check ? 200 : 60, g = check ? 160 - variant * 90 : 80 + 100 * x / w, b = check ? 40 : 30;
			}
			else
				r = Random(256), g = Random(256), b = Random(128);
			frame.Set(x, y, r, g, b);
		}
	for (int patch = 0; patch < 12; patch++)
	{
		const int px = Random(w - w / 8), py = Random(h - h / 8), r = Random(256), g = Random(256), b = Random(256);
		for (int y = py; y < py + h / 8; y++)
			for (int x = px; x < px + w / 8; x++)
				frame.Set(x, y, r, g, b);
	}
	return frame;
}

// The frame drawn into a black drawable at a rectangle, as the driver's placement draws it: with the nearest texel
// where the rectangle is the frame's size or a whole multiple of it, and with linear filtering otherwise
Image Place(const Image &frame, int dw, int dh, Rect at, bool bgra, bool linear)
{
	Image drawable(dw, dh, bgra);
	for (int y = 0; y < at.h; y++)
		for (int x = 0; x < at.w; x++)
		{
			const double u = (x + 0.5) * frame.width / at.w - 0.5, v = (y + 0.5) * frame.height / at.h - 0.5;
			double rgb[3];
			if (!linear)
				frame.Get(std::min(frame.width - 1, (int)((x + 0.5) * frame.width / at.w)), std::min(frame.height - 1, (int)((y + 0.5) * frame.height / at.h)), rgb);
			else
			{
				const int x0 = std::max(0, (int)std::floor(u)), y0 = std::max(0, (int)std::floor(v));
				const int x1 = std::min(frame.width - 1, x0 + 1), y1 = std::min(frame.height - 1, y0 + 1);
				const double fx = std::min(1.0, std::max(0.0, u - x0)), fy = std::min(1.0, std::max(0.0, v - y0));
				double a[3], b[3], c[3], d[3];
				frame.Get(x0, y0, a), frame.Get(x1, y0, b), frame.Get(x0, y1, c), frame.Get(x1, y1, d);
				for (int i = 0; i < 3; i++)
					rgb[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
			}
			drawable.Set(at.x + x, at.y + y, (int)(rgb[0] + 0.5), (int)(rgb[1] + 0.5), (int)(rgb[2] + 0.5));
		}
	return drawable;
}

Shown Check(const Image &frame, const Image &drawable, Rect at)
{
	return CompareShown(frame.View(), drawable.View(), at);
}

} // namespace

int main()
{
	const Image frame = GameFrame(1280, 720);

	// The frame at its own size in a drawable of its size, as in the game's window: every pixel is the frame's
	const Rect whole = {0, 0, 1280, 720};
	{
		const Shown shown = Check(frame, Place(frame, 1280, 720, whole, true, false), whole);
		assert(shown.matches && shown.unscaled && shown.worstBlock == 0 && shown.pixelsOff == 0 && shown.outside == 0 && shown.pixels == 1280 * 720);
	}
	// A level's rounding apart everywhere is still the frame
	{
		Image drawable = Place(frame, 1280, 720, whole, true, false);
		for (int y = 0; y < 720; y++)
			for (int x = 0; x < 1280; x++)
				for (int c = 0; c < 3; c++)
					if (drawable.At(x, y)[c] < 255 && ((x + y + c) & 1))
						drawable.At(x, y)[c]++;
		const Shown shown = Check(frame, drawable, whole);
		assert(shown.matches && shown.pixelsOff == 0 && shown.worstBlock > 0.3 && shown.worstBlock <= 1);
	}
	// 1:1 in a larger drawable (a 1440x900 screen), with black around it
	const Rect centered = {80, 90, 1280, 720};
	assert(Check(frame, Place(frame, 1440, 900, centered, true, false), centered).matches);

	// Scaled: a 320x200 frame on a 1024x768 screen, as Fit (1024x640, linear), as Integer (960x600), and a 1280x720
	// frame made smaller (a window cut to the screen) and larger (a 1920x1080 screen)
	{
		const Image small = GameFrame(320, 200);
		const Rect fit = {0, 64, 1024, 640}, integer = {32, 84, 960, 600};
		const Shown fitted = Check(small, Place(small, 1024, 768, fit, true, true), fit);
		assert(fitted.matches && !fitted.unscaled && fitted.worstBlock <= 2);
		assert(Check(small, Place(small, 1024, 768, integer, true, false), integer).matches);
		const Rect smaller = {0, 96, 1024, 576}, larger = {0, 0, 1920, 1080};
		assert(Check(frame, Place(frame, 1024, 768, smaller, true, true), smaller).matches);
		assert(Check(frame, Place(frame, 1920, 1080, larger, true, true), larger).matches);
		// A scaled frame that is ten levels too bright: only the blocks' mean colors can tell
		Image bright = Place(small, 1024, 768, fit, true, true);
		for (int y = fit.y; y < fit.y + fit.h; y++)
			for (int x = fit.x; x < fit.x + fit.w; x++)
				for (int c = 0; c < 3; c++)
					bright.At(x, y)[c] = (uint8_t)std::min(255, bright.At(x, y)[c] + 10);
		const Shown brighter = Check(small, bright, fit);
		assert(!brighter.matches && brighter.outside == 0 && brighter.worstBlock > 7 && brighter.worstBlock <= 13);
		// The scaled frame in the wrong place, or at another size than was meant
		assert(!Check(small, Place(small, 1024, 768, integer, true, false), fit).matches);
		assert(!Check(small, Place(small, 1024, 768, fit, true, true), integer).matches);
	}

	// What a presenter could get wrong
	{
		// Nothing drawn: black
		assert(!Check(frame, Image(1280, 720, true), whole).matches);
		// Red and blue swapped: the drawable's bytes written in the screen's order
		Image swapped = Place(frame, 1280, 720, whole, false, false);
		swapped.bgra = true;
		const Shown colors = Check(frame, swapped, whole);
		assert(!colors.matches && colors.worstBlock > 20);
		// Upside down, as OpenGL's rows would be
		Image flipped(1280, 720, true);
		for (int y = 0; y < 720; y++)
			for (int x = 0; x < 1280; x++)
			{
				double rgb[3];
				frame.Get(x, 719 - y, rgb);
				flipped.Set(x, y, (int)rgb[0], (int)rgb[1], (int)rgb[2]);
			}
		assert(!Check(frame, flipped, whole).matches);
		// Another frame of the same scene (other colors in the sky and on the floor)
		const Image other = GameFrame(1280, 720, 1);
		assert(!Check(frame, Place(other, 1280, 720, whole, true, false), whole).matches);
		// Moved by a few pixels
		const Rect moved = {84, 90, 1280, 720};
		const Shown shifted = Check(frame, Place(frame, 1440, 900, moved, true, false), centered);
		assert(!shifted.matches && shifted.outside > 0 && shifted.pixelsOff > 1000);
		// Scaled although it was meant at its own size: the pixels differ even where the mean colors nearly agree
		const Rect stretched = {0, 0, 1440, 900};
		assert(!Check(frame, Place(frame, 1440, 900, stretched, true, true), centered).matches);
		// At its own size and in its place, but filtered half a pixel off: the mean colors agree, the pixels don't
		Image soft(1280, 720, true);
		for (int y = 0; y < 720; y++)
			for (int x = 0; x < 1280; x++)
			{
				double a[3], b[3];
				frame.Get(x, y, a), frame.Get(std::min(1279, x + 1), y, b);
				soft.Set(x, y, (int)((a[0] + b[0]) / 2), (int)((a[1] + b[1]) / 2), (int)((a[2] + b[2]) / 2));
			}
		const Shown softened = Check(frame, soft, whole);
		assert(!softened.matches && softened.outside == 0 && softened.worstBlock <= 1 && softened.pixelsOff > softened.pixels / 20);
		// The frame where it belongs, and the drawable not cleared around it
		Image dirty = Place(frame, 1440, 900, centered, true, false);
		dirty.Set(3, 3, 0, 9, 0);
		const Shown border = Check(frame, dirty, centered);
		assert(!border.matches && border.outside == 1 && border.pixelsOff == 0);
		// A rectangle that isn't in the drawable
		assert(!Check(frame, Place(frame, 1280, 720, whole, true, false), centered).matches);
	}
	puts("metal_present_check: passed");
	return 0;
}
