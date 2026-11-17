// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl_present_check.h
/// \brief Whether a drawable holds the frame that was placed in it (-mtlcheck)
///
/// The game's screenshots are read back from the driver's screen, before the presenter has anything to do with the
/// frame, so they say nothing of what the presenter put into the window's drawable. With -mtlcheck the presenter reads
/// some drawables back, with the frame they were given, and this compares the two: the frame must be in the
/// rectangle Screen Scaling placed it at, and the rest of the drawable must be black.
///
/// The frame may have been scaled with linear filtering, so the comparison is of mean colors: the rectangle is cut
/// into 8 by 8 blocks, and each block's mean color must be the mean color of the same part of the frame, to within a
/// tolerance that is small where the frame was placed at its own size and larger where it was scaled. That tells a
/// frame from black, from another frame, from one with red and blue swapped (a drawable is BGRA and the screen RGBA),
/// and from one that is flipped, shifted, or at another size. Where the frame was placed at its own size, the pixels
/// are compared one by one as well.
///
/// What this can't tell is what the display then showed: it reads the texture, not the screen.
///
/// Plain C++, with no header of Apple's or the game's, so that tests/metal-present-check.cpp can run it anywhere.

#ifndef RR_MTL_PRESENT_CHECK_H
#define RR_MTL_PRESENT_CHECK_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace rr_mtl_present
{

// Four bytes per pixel, rows pitch bytes apart; red or blue first
struct Pixels
{
	const uint8_t *data;
	int width, height, pitch;
	bool bgra;
};

struct Rect
{
	int x, y, w, h;
};

struct Shown
{
	bool unscaled;      // the frame was placed at its own size
	double worstBlock;  // the largest difference between a block's mean color and the frame's there, in levels of a channel
	long pixelsOff;     // unscaled: pixels more than one level from the frame's, in any channel
	long pixels;        // of the rectangle
	long outside;       // pixels outside the rectangle that aren't black
	bool matches;
};

constexpr int kCheckBlocks = 8;
constexpr double kUnscaledBlockLevels = 1.0, kScaledBlockLevels = 6.0;
constexpr double kUnscaledPixelsOff = 0.001; // of the rectangle's pixels

namespace check_detail
{

// The mean of one channel (0 red, 1 green, 2 blue) over the part of an image from x0,y0 to x1,y1 (fractions of a pixel count)
inline double Mean(const Pixels &image, int ox, int oy, double x0, double y0, double x1, double y1, int channel)
{
	const int left = (int)std::floor(x0), top = (int)std::floor(y0);
	const int right = std::max(left + 1, (int)std::ceil(x1)), bottom = std::max(top + 1, (int)std::ceil(y1));
	const int byte = image.bgra ? 2 - channel : channel;
	double sum = 0;
	long count = 0;
	for (int y = top; y < bottom; y++)
	{
		const uint8_t *row = image.data + (size_t)(oy + y) * image.pitch;
		for (int x = left; x < right; x++)
			sum += row[(size_t)(ox + x) * 4 + byte];
		count += right - left;
	}
	return count ? sum / count : 0;
}

} // namespace check_detail

// frame: what the presenter was given. drawable: what it handed to the display. placed: where in the drawable the frame
// was put. The rectangle must lie within the drawable.
inline Shown CompareShown(const Pixels &frame, const Pixels &drawable, const Rect &placed)
{
	Shown shown = {};
	shown.unscaled = placed.w == frame.width && placed.h == frame.height;
	shown.pixels = (long)placed.w * placed.h;
	if (placed.w <= 0 || placed.h <= 0 || placed.x < 0 || placed.y < 0 || placed.x + placed.w > drawable.width
		|| placed.y + placed.h > drawable.height || frame.width <= 0 || frame.height <= 0)
	{
		shown.worstBlock = 255;
		return shown;
	}
	const int blocksX = std::min(kCheckBlocks, std::min(placed.w, frame.width)), blocksY = std::min(kCheckBlocks, std::min(placed.h, frame.height));
	for (int by = 0; by < blocksY; by++)
		for (int bx = 0; bx < blocksX; bx++)
			for (int channel = 0; channel < 3; channel++)
			{
				const double there = check_detail::Mean(drawable, placed.x, placed.y, (double)placed.w * bx / blocksX, (double)placed.h * by / blocksY,
					(double)placed.w * (bx + 1) / blocksX, (double)placed.h * (by + 1) / blocksY, channel);
				const double meant = check_detail::Mean(frame, 0, 0, (double)frame.width * bx / blocksX, (double)frame.height * by / blocksY,
					(double)frame.width * (bx + 1) / blocksX, (double)frame.height * (by + 1) / blocksY, channel);
				shown.worstBlock = std::max(shown.worstBlock, std::fabs(there - meant));
			}
	const int dr = drawable.bgra ? 2 : 0, db = 2 - dr, fr = frame.bgra ? 2 : 0, fb = 2 - fr;
	for (int y = 0; y < drawable.height; y++)
	{
		const uint8_t *row = drawable.data + (size_t)y * drawable.pitch;
		const bool inRows = y >= placed.y && y < placed.y + placed.h;
		for (int x = 0; x < drawable.width; x++)
		{
			const uint8_t *pixel = row + (size_t)x * 4;
			if (!inRows || x < placed.x || x >= placed.x + placed.w)
				shown.outside += pixel[0] || pixel[1] || pixel[2];
			else if (shown.unscaled)
			{
				const uint8_t *meant = frame.data + (size_t)(y - placed.y) * frame.pitch + (size_t)(x - placed.x) * 4;
				shown.pixelsOff += std::abs(pixel[dr] - meant[fr]) > 1 || std::abs(pixel[1] - meant[1]) > 1 || std::abs(pixel[db] - meant[fb]) > 1;
			}
		}
	}
	shown.matches = shown.outside == 0
		&& shown.worstBlock <= (shown.unscaled ? kUnscaledBlockLevels : kScaledBlockLevels)
		&& (!shown.unscaled || shown.pixelsOff <= kUnscaledPixelsOff * shown.pixels);
	return shown;
}

} // namespace rr_mtl_present

#endif
