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
	// split by floor division (so any odd pixel is on the right and the bottom)
	int x = 0, y = 0, w = 0, h = 0;
	int k = 0;
};

// frame*: the game's frame (vid.width by vid.height); screen*: the screen or window (vid.realwidth by vid.realheight).
// Integer and 1:1 fall back to fit when the frame is larger than the screen in either dimension, as does any mode
// that is not listed or a size that is not positive.
constexpr Placement Compute(int frameWidth, int frameHeight, int screenWidth, int screenHeight, int mode)
{
	Placement fit;
	fit.w = screenWidth > 0 ? screenWidth : 0;
	fit.h = screenHeight > 0 ? screenHeight : 0;
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

} // namespace screen_scaling

#endif
