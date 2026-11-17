// The frame capture mode's tic list, screenshot names, and time between frames (src/frame_capture.hpp); runs without the
// engine.
#include "frame_capture.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			assert(false); \
			std::abort(); \
		} \
	} while (0)

using frame_capture::parse_tics;
using frame_capture::shot_name;

static bool parses(const char* text, std::vector<std::uint32_t> expected)
{
	std::vector<std::uint32_t> out{99, 98}; // must be cleared
	std::string error;
	if (!parse_tics(text, out, error))
	{
		return false;
	}
	return out == expected && error.empty();
}

static bool fails(const char* text)
{
	std::vector<std::uint32_t> out{99};
	std::string error;
	const bool ok = parse_tics(text, out, error);
	return !ok && out.empty() && !error.empty();
}

int main()
{
	// Lists: one tic, several, in any order, with repeats and leading zeros
	CHECK(parses("70", {70}));
	CHECK(parses("70,210,350", {70, 210, 350}));
	CHECK(parses("350,70,210", {70, 210, 350}));
	CHECK(parses("70,70,5,70", {5, 70}));
	CHECK(parses("0,00035,35", {0, 35}));
	CHECK(parses("999999999", {999999999u}));

	// Not a list of level times
	CHECK(fails(nullptr));
	CHECK(fails(""));
	CHECK(fails(","));
	CHECK(fails("70,"));
	CHECK(fails(",70"));
	CHECK(fails("70,,210"));
	CHECK(fails("70 210"));
	CHECK(fails("70, 210"));
	CHECK(fails("-70"));
	CHECK(fails("7.5"));
	CHECK(fails("seventy"));
	CHECK(fails("70s"));
	CHECK(fails("0x46"));
	CHECK(fails("1000000000")); // ten digits: could overflow 32 bits

	// The limit on how many
	{
		std::string many;
		for (std::size_t i = 0; i < frame_capture::kMaxShots; i++)
		{
			many += (i ? "," : "") + std::to_string(i);
		}
		std::vector<std::uint32_t> out;
		std::string error;
		CHECK(parse_tics(many.c_str(), out, error));
		CHECK(out.size() == frame_capture::kMaxShots);
		many += ",5000";
		CHECK(!parse_tics(many.c_str(), out, error));
		CHECK(out.empty());
	}

	// Names: map and level time, no counter
	CHECK(shot_name("RR_AERIALHIGHLANDS", 70) == "RR_AERIALHIGHLANDS_00070.png");
	CHECK(shot_name("RR_AERIALHIGHLANDS", 70) == shot_name("RR_AERIALHIGHLANDS", 70));
	CHECK(shot_name("MAP01", 0) == "MAP01_00000.png");
	CHECK(shot_name("MAP01", 123456) == "MAP01_123456.png");
	CHECK(shot_name("MAP01", 3, "tga") == "MAP01_00003.tga");
	CHECK(shot_name("Marble Garden/Zone", 1) == "Marble_Garden_Zone_00001.png");
	CHECK(shot_name("..\\x", 1) == "___x_00001.png"); // a map name cannot name another directory
	CHECK(shot_name("", 1) == "MAP_00001.png");
	CHECK(shot_name(nullptr, 1) == "MAP_00001.png");
	CHECK(shot_name("A", 9) < shot_name("A", 10)); // sorts in time order

	// The time between frames: one tic while capturing, however long the clock says the frame took (none, or several
	// tics); what was measured otherwise
	const std::int32_t tic = 65536;
	CHECK(frame_capture::frame_delta(true, 0, tic) == tic);
	CHECK(frame_capture::frame_delta(true, 3 * tic, tic) == tic);
	CHECK(frame_capture::frame_delta(true, tic / 7, tic) == tic);
	CHECK(frame_capture::frame_delta(false, 0, tic) == 0);
	CHECK(frame_capture::frame_delta(false, 3 * tic, tic) == 3 * tic);
	CHECK(frame_capture::frame_delta(false, tic / 7, tic) == tic / 7);

	std::puts("frame_capture: ok");
	return 0;
}
