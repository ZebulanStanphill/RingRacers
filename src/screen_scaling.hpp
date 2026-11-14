// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  screen_scaling.hpp
/// \brief Where the game's frame goes on the screen: Options > Video > Screen Scaling (cv_scr_scaling)
///
/// Header-only and free of engine types, so that both renderers (the RHI blit in i_video_common.cpp and VibeRant D3D's
/// swap chain in r_d3d12/) and the native tests share one calculation.

#ifndef __SCREEN_SCALING_HPP__
#define __SCREEN_SCALING_HPP__

namespace screen_scaling
{

// cv_scr_scaling's values
enum Mode
{
	kFit = 0, // scaled to fit, keeping the aspect ratio
	kInteger = 1, // scaled by the largest whole number that fits
	kOneToOne = 2, // unscaled
};

struct Placement
{
	// True: the frame is scaled to fit and letterboxed by whoever draws it (the system's aspect-ratio scaling, or the
	// blit's own correction). The rectangle is then the whole screen and k is 0.
	bool fit = true;
	// Otherwise the frame goes at exactly this rectangle, k times its size, centered in the screen with the remainder
	// split by floor division (so any odd pixel is on the right and the bottom). k is 0 with fit false for FitRect's
	// rectangle: scaled by a factor that is not a whole number, and drawn with linear filtering.
	int x = 0, y = 0, w = 0, h = 0;
	int k = 0;
};

// What is left to whoever draws the frame: the whole screen (0 where the size isn't positive), no scale
constexpr Placement WholeScreen(int screenWidth, int screenHeight)
{
	Placement whole;
	whole.w = screenWidth > 0 ? screenWidth : 0;
	whole.h = screenHeight > 0 ? screenHeight : 0;
	return whole;
}

// frame*: the game's frame (vid.width by vid.height); screen*: the screen or window (vid.realwidth by vid.realheight).
// Integer and 1:1 fall back to fit when the frame is larger than the screen in either dimension, as does any mode
// that is not listed or a size that is not positive.
constexpr Placement Compute(int frameWidth, int frameHeight, int screenWidth, int screenHeight, int mode)
{
	const Placement fit = WholeScreen(screenWidth, screenHeight);
	if (frameWidth <= 0 || frameHeight <= 0 || screenWidth <= 0 || screenHeight <= 0)
		return fit;
	if (mode != kInteger && mode != kOneToOne)
		return fit;

	int k = 1;
	if (mode == kInteger)
	{
		const int kx = screenWidth / frameWidth;
		const int ky = screenHeight / frameHeight;
		k = kx < ky ? kx : ky;
	}
	if (k < 1 || frameWidth > screenWidth || frameHeight > screenHeight)
		return fit;

	Placement placed;
	placed.fit = false;
	placed.k = k;
	placed.w = frameWidth * k;
	placed.h = frameHeight * k;
	placed.x = (screenWidth - placed.w) / 2;
	placed.y = (screenHeight - placed.h) / 2;
	return placed;
}

// For a renderer that draws the frame itself into a screen-sized target, in Fit mode: the largest rectangle with the
// frame's aspect ratio that fits the screen, centered, with the remainder split like Compute's (floor division, so any odd
// pixel is on the right and the bottom). The size along the limiting side is the screen's; the other is the frame's aspect
// ratio times it, rounded down, and at least 1. The frame is rarely scaled by a whole number there, so the placement has
// fit false and k 0 (drawn with linear filtering). A size that is not positive gives what Compute gives: the whole screen,
// left to whoever draws it, with fit true.
constexpr Placement FitRect(int frameWidth, int frameHeight, int screenWidth, int screenHeight)
{
	if (frameWidth <= 0 || frameHeight <= 0 || screenWidth <= 0 || screenHeight <= 0)
		return WholeScreen(screenWidth, screenHeight);

	// frame width / frame height against screen width / screen height, without dividing
	const long long frameWide = (long long)frameWidth * screenHeight;
	const long long screenWide = (long long)screenWidth * frameHeight;

	Placement placed;
	placed.fit = false;
	placed.k = 0;
	if (frameWide <= screenWide)
	{
		// Not wider than the screen: it fills the height
		placed.h = screenHeight;
		placed.w = (int)((long long)screenHeight * frameWidth / frameHeight);
		if (placed.w < 1)
			placed.w = 1;
	}
	else
	{
		// Wider: it fills the width
		placed.w = screenWidth;
		placed.h = (int)((long long)screenWidth * frameHeight / frameWidth);
		if (placed.h < 1)
			placed.h = 1;
	}
	placed.x = (screenWidth - placed.w) / 2;
	placed.y = (screenHeight - placed.h) / 2;
	return placed;
}

} // namespace screen_scaling

#endif
