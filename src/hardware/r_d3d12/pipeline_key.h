// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_PIPELINE_KEY_H
#define RR_PIPELINE_KEY_H
#include <cstdint>
#include <cstddef>
#include <initializer_list>
namespace rr_d3d12
{
struct PipelineKey
{
	std::uint16_t vs = 0, ps = 0;
	std::uint16_t custom = 0;
	std::uint8_t layout = 0, topologyType = 3, rtFormat = 28, dsvFormat = 45;
	std::uint16_t blend = 0;
	std::uint8_t raster = 0;
	std::uint64_t depth = 0;
	bool operator==(const PipelineKey &b) const
	{
		return vs == b.vs && ps == b.ps && custom == b.custom && layout == b.layout && topologyType == b.topologyType &&
			   rtFormat == b.rtFormat && dsvFormat == b.dsvFormat && blend == b.blend && raster == b.raster &&
			   depth == b.depth;
	}
	bool operator!=(const PipelineKey &b) const
	{
		return !(*this == b);
	}
};
struct PipelineKeyHash
{
	std::size_t operator()(const PipelineKey &k) const
	{
		std::uint64_t h = 14695981039346656037ULL;
		for (auto v : {std::uint64_t(k.vs), std::uint64_t(k.ps), std::uint64_t(k.custom), std::uint64_t(k.layout), std::uint64_t(k.topologyType),
					   std::uint64_t(k.rtFormat), std::uint64_t(k.dsvFormat), std::uint64_t(k.blend),
					   std::uint64_t(k.raster), k.depth})
		{
			for (int i = 0; i < 8; i++)
			{
				h ^= (v >> (i * 8)) & 255;
				h *= 1099511628211ULL;
			}
		}
		return static_cast<std::size_t>(h);
	}
};
} // namespace rr_d3d12
#endif
