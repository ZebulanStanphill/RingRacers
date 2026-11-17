// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  frame_capture.hpp
/// \brief The parts of the frame capture mode (-shottics, d_framecapture.cpp) that need no engine types:
///        reading the tic list, naming the screenshot files, and the time between two frames.
///
/// Header-only, so that the native tests run the same code the engine does.

#ifndef __FRAME_CAPTURE_HPP__
#define __FRAME_CAPTURE_HPP__

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace frame_capture
{

// More than a replay is ever going to need; also keeps a typo from queueing millions of screenshots.
constexpr std::size_t kMaxShots = 1024;
// A tic is read as at most this many digits, so that it always fits 32 bits.
constexpr std::size_t kMaxTicDigits = 9;

// Reads "70,210,350" into ascending, duplicate-free level times. Fails (false, with the reason in error) on an empty
// list, an empty or non-numeric item, a number of more than nine digits, or more than kMaxShots tics.
inline bool parse_tics(const char* text, std::vector<std::uint32_t>& out, std::string& error)
{
	out.clear();
	error.clear();

	if (text == nullptr || *text == '\0')
	{
		error = "no tics given";
		return false;
	}

	const char* p = text;
	for (;;)
	{
		const char* start = p;
		std::uint64_t value = 0;
		while (*p >= '0' && *p <= '9')
		{
			if (static_cast<std::size_t>(p - start) >= kMaxTicDigits)
			{
				error = "tic has too many digits";
				out.clear();
				return false;
			}
			value = value * 10 + static_cast<std::uint64_t>(*p - '0');
			p++;
		}

		if (p == start)
		{
			error = (*p == '\0' || *p == ',') ? "empty item in the tic list" : "tic is not a number";
			out.clear();
			return false;
		}

		if (*p != ',' && *p != '\0')
		{
			error = "tic is not a number";
			out.clear();
			return false;
		}

		out.push_back(static_cast<std::uint32_t>(value));
		if (out.size() > kMaxShots)
		{
			error = "too many tics";
			out.clear();
			return false;
		}

		if (*p == '\0')
		{
			break;
		}
		p++; // the comma
	}

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return true;
}

// The time between two frames, for the animations that go by it (the game's renderdeltatics): what the clock measured,
// or in capture mode one tic, whatever the clock says. A captured frame is one tic after the last, and how long the
// machine took to draw it must not show in the next one.
constexpr std::int32_t frame_delta(bool capturing, std::int32_t measured, std::int32_t one_tic)
{
	return capturing ? one_tic : measured;
}

// "<MAP>_<tic>.png": the map's name (anything but letters, digits, '_' and '-' becomes '_') and the level time with at
// least five digits, so that a directory of them sorts in time order. No running counter: the same replay and tic
// always give the same name.
inline std::string shot_name(const char* mapname, std::uint32_t tic, const char* extension = "png")
{
	std::string name;
	for (const char* c = mapname ? mapname : ""; *c != '\0'; c++)
	{
		const bool ok = (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_' ||
		                *c == '-';
		name.push_back(ok ? *c : '_');
	}
	if (name.empty())
	{
		name = "MAP";
	}

	char digits[16];
	std::snprintf(digits, sizeof digits, "_%05u.", static_cast<unsigned>(tic));
	name += digits;
	name += extension;
	return name;
}

} // namespace frame_capture

#endif // __FRAME_CAPTURE_HPP__
