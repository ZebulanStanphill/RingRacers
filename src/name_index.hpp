// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  name_index.hpp
/// \brief Hash indexes that make the engine's linear name lookups fast without changing their results
///
/// Startup looks up thousands of lump, texture and folder names with scans over every lump or texture of a file, which
/// takes seconds on slow CPUs. These indexes find the same entry such a scan would: the caller keeps its own name
/// comparison and walks only the positions whose stored hash matches, in the scan's order.
///
/// Header-only and free of engine types, so that the engine and the native tests share it.

#ifndef __NAME_INDEX_HPP__
#define __NAME_INDEX_HPP__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace name_index
{

/// From a stored hash to the positions that have it, in ascending order.
class HashPositions
{
public:
	/// Positions must be added in increasing order.
	void Add(uint32_t hash, uint32_t pos)
	{
		std::vector<uint32_t>& v = map_[hash];
		assert(v.empty() || v.back() < pos);
		v.push_back(pos);
	}

	void Clear() { map_.clear(); }

	/// The smallest position >= start with this hash for which match(pos) is true, or -1.
	template <class Match>
	int64_t FirstFrom(uint32_t hash, uint32_t start, Match&& match) const
	{
		auto it = map_.find(hash);
		if (it == map_.end())
			return -1;
		const std::vector<uint32_t>& v = it->second;
		for (auto p = std::lower_bound(v.begin(), v.end(), start); p != v.end(); ++p)
		{
			if (match(*p))
				return *p;
		}
		return -1;
	}

	/// The largest position < end with this hash for which match(pos) is true, or -1.
	template <class Match>
	int64_t LastBelow(uint32_t hash, uint32_t end, Match&& match) const
	{
		auto it = map_.find(hash);
		if (it == map_.end())
			return -1;
		const std::vector<uint32_t>& v = it->second;
		auto p = std::lower_bound(v.begin(), v.end(), end);
		while (p != v.begin())
		{
			--p;
			if (match(*p))
				return *p;
		}
		return -1;
	}

private:
	std::unordered_map<uint32_t, std::vector<uint32_t>> map_;
};

} // namespace name_index

#endif // __NAME_INDEX_HPP__
