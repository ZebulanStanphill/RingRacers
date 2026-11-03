// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  map_preview_cache.hpp
/// \brief The on-disk cache of what P_InitMapData takes from the game's PK3s at startup
///
/// Finding each map's thumbnail and minimap means inflating its whole map WAD, and each staff ghost's brief means
/// inflating the ghost, which is most of the startup time on slow storage. This caches the raw PICTURE and MINIMAP lump
/// bytes of each map WAD and the parsed brief of each staff ghost, keyed by the zip entry: its full path, CRC32, size
/// and compressed size. A new or changed file therefore never matches an old entry. A ghost's brief also depends on the
/// parser that read it, so the file records a parser version, and a file made by another one is discarded whole.
///
/// Header-only and free of engine types, so that the engine glue (p_mapcache.cpp) and the native tests share it. The
/// file is little-endian and written byte by byte, so it is the same on every platform.

#ifndef __MAP_PREVIEW_CACHE_HPP__
#define __MAP_PREVIEW_CACHE_HPP__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace map_preview_cache
{

constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxName = 1024;
constexpr size_t kMaxLump = 64u * 1024u * 1024u;

struct Key
{
	std::string name; // the zip entry's full path
	uint32_t crc32 = 0;
	uint32_t size = 0; // uncompressed
	uint32_t disksize = 0; // compressed

	bool operator<(const Key& o) const
	{
		return std::tie(name, crc32, size, disksize) < std::tie(o.name, o.crc32, o.size, o.disksize);
	}
	bool operator==(const Key& o) const
	{
		return std::tie(name, crc32, size, disksize) == std::tie(o.name, o.crc32, o.size, o.disksize);
	}
	bool operator!=(const Key& o) const { return !(*this == o); }
};

struct MapEntry
{
	bool hasPicture = false;
	bool hasMinimap = false;
	std::vector<uint8_t> picture;
	std::vector<uint8_t> minimap;
};

struct GhostEntry
{
	bool valid = false; // false: the brief parser rejected this ghost
	uint32_t time = 0;
	uint32_t lap = 0;
	std::string name;
};

class Cache
{
public:
	// Replaces the cache with the file's contents. Anything wrong with the file leaves the cache empty and returns false.
	bool Load(const uint8_t* data, size_t size, uint32_t parserVersion)
	{
		maps_.clear();
		ghosts_.clear();
		dirty_ = false;
		if (!Parse(data, size, parserVersion))
		{
			maps_.clear();
			ghosts_.clear();
			return false;
		}
		return true;
	}

	const MapEntry* FindMap(const Key& key) const
	{
		auto it = maps_.find(key);
		return it == maps_.end() ? nullptr : &it->second;
	}

	const GhostEntry* FindGhost(const Key& key) const
	{
		auto it = ghosts_.find(key);
		return it == ghosts_.end() ? nullptr : &it->second;
	}

	void PutMap(const Key& key, MapEntry entry)
	{
		maps_[key] = std::move(entry);
		dirty_ = true;
	}

	void PutGhost(const Key& key, GhostEntry entry)
	{
		ghosts_[key] = std::move(entry);
		dirty_ = true;
	}

	bool Dirty() const { return dirty_; }

	// The file's contents, with only the entries whose key keep() accepts
	std::vector<uint8_t> Save(uint32_t parserVersion, const std::function<bool(const Key&)>& keep) const
	{
		std::vector<uint8_t> out;
		uint32_t mapCount = 0, ghostCount = 0;
		for (const auto& e : maps_)
			mapCount += keep(e.first) ? 1 : 0;
		for (const auto& e : ghosts_)
			ghostCount += keep(e.first) ? 1 : 0;

		out.insert(out.end(), kMagic, kMagic + 8);
		Put32(out, kFormatVersion);
		Put32(out, parserVersion);
		Put32(out, mapCount);
		Put32(out, ghostCount);

		for (const auto& e : maps_)
		{
			if (!keep(e.first))
				continue;
			const MapEntry& m = e.second;
			PutKey(out, e.first);
			out.push_back(static_cast<uint8_t>((m.hasPicture ? 1 : 0) | (m.hasMinimap ? 2 : 0)));
			if (m.hasPicture)
			{
				Put32(out, static_cast<uint32_t>(m.picture.size()));
				out.insert(out.end(), m.picture.begin(), m.picture.end());
			}
			if (m.hasMinimap)
			{
				Put32(out, static_cast<uint32_t>(m.minimap.size()));
				out.insert(out.end(), m.minimap.begin(), m.minimap.end());
			}
		}

		for (const auto& e : ghosts_)
		{
			if (!keep(e.first))
				continue;
			const GhostEntry& g = e.second;
			PutKey(out, e.first);
			out.push_back(g.valid ? 1 : 0);
			if (g.valid)
			{
				Put32(out, g.time);
				Put32(out, g.lap);
				const size_t n = g.name.size() > 255 ? 255 : g.name.size();
				out.push_back(static_cast<uint8_t>(n));
				out.insert(out.end(), g.name.begin(), g.name.begin() + static_cast<std::ptrdiff_t>(n));
			}
		}
		return out;
	}

private:
	static constexpr char kMagic[9] = "RRMAPCV1";

	std::map<Key, MapEntry> maps_;
	std::map<Key, GhostEntry> ghosts_;
	bool dirty_ = false;

	static void Put32(std::vector<uint8_t>& out, uint32_t v)
	{
		for (int i = 0; i < 4; i++)
			out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

	static void PutKey(std::vector<uint8_t>& out, const Key& k)
	{
		const size_t n = k.name.size() > kMaxName ? kMaxName : k.name.size();
		out.push_back(static_cast<uint8_t>(n & 0xFF));
		out.push_back(static_cast<uint8_t>(n >> 8));
		out.insert(out.end(), k.name.begin(), k.name.begin() + static_cast<std::ptrdiff_t>(n));
		Put32(out, k.crc32);
		Put32(out, k.size);
		Put32(out, k.disksize);
	}

	// Bounds-checked reader
	struct Reader
	{
		const uint8_t* p;
		size_t left;

		bool Skip(size_t n)
		{
			if (n > left)
				return false;
			p += n;
			left -= n;
			return true;
		}
		bool U8(uint8_t& v)
		{
			if (left < 1)
				return false;
			v = *p;
			return Skip(1);
		}
		bool U16(uint16_t& v)
		{
			if (left < 2)
				return false;
			v = static_cast<uint16_t>(p[0] | (p[1] << 8));
			return Skip(2);
		}
		bool U32(uint32_t& v)
		{
			if (left < 4)
				return false;
			v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
				(static_cast<uint32_t>(p[3]) << 24);
			return Skip(4);
		}
		bool Bytes(size_t n, const uint8_t*& out)
		{
			if (n > left)
				return false;
			out = p;
			return Skip(n);
		}
	};

	static bool ReadKey(Reader& r, Key& k)
	{
		uint16_t n;
		const uint8_t* s;
		if (!r.U16(n) || n > kMaxName || !r.Bytes(n, s))
			return false;
		k.name.assign(reinterpret_cast<const char*>(s), n);
		return r.U32(k.crc32) && r.U32(k.size) && r.U32(k.disksize);
	}

	static bool ReadLump(Reader& r, std::vector<uint8_t>& out)
	{
		uint32_t n;
		const uint8_t* s;
		if (!r.U32(n) || n > kMaxLump || !r.Bytes(n, s))
			return false;
		out.assign(s, s + n);
		return true;
	}

	bool Parse(const uint8_t* data, size_t size, uint32_t parserVersion)
	{
		if (data == nullptr)
			return false;
		Reader r{data, size};
		const uint8_t* magic;
		if (!r.Bytes(8, magic))
			return false;
		for (int i = 0; i < 8; i++)
			if (magic[i] != static_cast<uint8_t>(kMagic[i]))
				return false;
		uint32_t format, parser, mapCount, ghostCount;
		if (!r.U32(format) || !r.U32(parser) || !r.U32(mapCount) || !r.U32(ghostCount))
			return false;
		if (format != kFormatVersion || parser != parserVersion)
			return false;

		// Each entry takes at least 15 bytes (an empty key and one byte), so a count the rest of the file cannot hold is corrupt
		if (mapCount > r.left / 15 || ghostCount > (r.left - mapCount * 15) / 15)
			return false;

		for (uint32_t i = 0; i < mapCount; i++)
		{
			Key k;
			MapEntry m;
			uint8_t flags;
			if (!ReadKey(r, k) || !r.U8(flags) || (flags & ~3))
				return false;
			m.hasPicture = (flags & 1) != 0;
			m.hasMinimap = (flags & 2) != 0;
			if (m.hasPicture && !ReadLump(r, m.picture))
				return false;
			if (m.hasMinimap && !ReadLump(r, m.minimap))
				return false;
			maps_[k] = std::move(m);
		}

		for (uint32_t i = 0; i < ghostCount; i++)
		{
			Key k;
			GhostEntry g;
			uint8_t valid;
			if (!ReadKey(r, k) || !r.U8(valid) || valid > 1)
				return false;
			g.valid = valid != 0;
			if (g.valid)
			{
				uint8_t n;
				const uint8_t* s;
				if (!r.U32(g.time) || !r.U32(g.lap) || !r.U8(n) || !r.Bytes(n, s))
					return false;
				g.name.assign(reinterpret_cast<const char*>(s), n);
			}
			ghosts_[k] = std::move(g);
		}

		return r.left == 0;
	}
};

} // namespace map_preview_cache

#endif // __MAP_PREVIEW_CACHE_HPP__
