// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  lump_header_cache.hpp
/// \brief The on-disk cache of the small lump reads made at startup (W_ReadLumpHeaderPwad)
///
/// Startup reads thousands of lump headers (a PNG's first 8 bytes, a patch's header, a flat's first bytes), each at a
/// scattered offset in a PK3 and, for a deflated entry, by inflating from the entry's start. On slow storage that is
/// most of the startup time. This caches the bytes each such read returned, keyed by the zip entry (its full path,
/// CRC32, size and compressed size, so a new or changed file never matches an old entry) and by the read (offset and
/// requested size).
///
/// The cache keeps track of which entries were found or put since it was loaded, and Save writes only those, so entries
/// for files that are gone do not pile up.
///
/// Header-only and free of engine types, so that the engine glue (w_uwpheadercache.cpp) and the native tests share it.
/// The file is little-endian and written byte by byte, so it is the same on every platform.

#ifndef __LUMP_HEADER_CACHE_HPP__
#define __LUMP_HEADER_CACHE_HPP__

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace lump_header_cache
{

constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxName = 1024;
constexpr size_t kMaxData = 1024; // The most bytes one entry holds; the engine caps reads lower than this

struct Key
{
	std::string name; // the zip entry's full path
	uint32_t crc32 = 0;
	uint32_t size = 0; // uncompressed
	uint32_t disksize = 0; // compressed
	uint32_t offset = 0; // where in the lump the read starts
	uint32_t request = 0; // the size asked for (0: all of the lump)

	bool operator<(const Key& o) const
	{
		return std::tie(name, crc32, size, disksize, offset, request) <
			std::tie(o.name, o.crc32, o.size, o.disksize, o.offset, o.request);
	}
	bool operator==(const Key& o) const
	{
		return std::tie(name, crc32, size, disksize, offset, request) ==
			std::tie(o.name, o.crc32, o.size, o.disksize, o.offset, o.request);
	}
	bool operator!=(const Key& o) const { return !(*this == o); }
};

struct Entry
{
	std::vector<uint8_t> data; // what the read returned; its size is the length the read returned
	bool used = false; // found or put since the cache was loaded
};

class Cache
{
public:
	// Replaces the cache with the file's contents. Anything wrong with the file leaves the cache empty and returns false.
	bool Load(const uint8_t* data, size_t size)
	{
		entries_.clear();
		added_ = false;
		if (!Parse(data, size))
		{
			entries_.clear();
			return false;
		}
		return true;
	}

	// The entry for a read, or null. Marks it as used. The pointer lasts until the next Load, and Puts do not move it.
	const Entry* Find(const Key& key)
	{
		auto it = entries_.find(key);
		if (it == entries_.end())
			return nullptr;
		it->second.used = true;
		return &it->second;
	}

	// Stores what a read returned (at most kMaxData bytes; a longer read is not stored, and false is returned)
	bool Put(const Key& key, const uint8_t* data, size_t len)
	{
		if (len > kMaxData || (len != 0 && data == nullptr))
			return false;
		Entry& e = entries_[key];
		e.data.assign(data, data + len);
		e.used = true;
		added_ = true;
		return true;
	}

	size_t Size() const { return entries_.size(); }

	// Entries since Load that Save would keep
	size_t UsedCount() const
	{
		size_t n = 0;
		for (const auto& e : entries_)
			n += e.second.used ? 1 : 0;
		return n;
	}

	// Whether the file on disk differs from what Save would write: something was put, or a loaded entry went unused
	bool NeedsSave() const { return added_ || UsedCount() != entries_.size(); }

	// The file's contents, with only the entries used since Load
	std::vector<uint8_t> Save() const
	{
		std::vector<uint8_t> out;
		out.insert(out.end(), kMagic, kMagic + 8);
		Put32(out, kFormatVersion);
		Put32(out, static_cast<uint32_t>(UsedCount()));

		for (const auto& e : entries_)
		{
			if (!e.second.used)
				continue;
			const Key& k = e.first;
			const size_t n = k.name.size() > kMaxName ? kMaxName : k.name.size();
			out.push_back(static_cast<uint8_t>(n & 0xFF));
			out.push_back(static_cast<uint8_t>(n >> 8));
			out.insert(out.end(), k.name.begin(), k.name.begin() + static_cast<std::ptrdiff_t>(n));
			Put32(out, k.crc32);
			Put32(out, k.size);
			Put32(out, k.disksize);
			Put32(out, k.offset);
			Put32(out, k.request);
			const size_t len = e.second.data.size();
			out.push_back(static_cast<uint8_t>(len & 0xFF));
			out.push_back(static_cast<uint8_t>(len >> 8));
			out.insert(out.end(), e.second.data.begin(), e.second.data.end());
		}
		return out;
	}

private:
	static constexpr char kMagic[9] = "RRLHDRV1";
	static constexpr size_t kMinEntry = 2 + 5 * 4 + 2; // an empty name and no data

	std::map<Key, Entry> entries_;
	bool added_ = false;

	static void Put32(std::vector<uint8_t>& out, uint32_t v)
	{
		for (int i = 0; i < 4; i++)
			out.push_back(static_cast<uint8_t>(v >> (8 * i)));
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

	bool Parse(const uint8_t* data, size_t size)
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
		uint32_t format, count;
		if (!r.U32(format) || !r.U32(count) || format != kFormatVersion)
			return false;

		// A count that the rest of the file cannot hold is corrupt
		if (count > r.left / kMinEntry)
			return false;

		for (uint32_t i = 0; i < count; i++)
		{
			Key k;
			uint16_t n, len;
			const uint8_t* s;
			const uint8_t* d;
			if (!r.U16(n) || n > kMaxName || !r.Bytes(n, s))
				return false;
			k.name.assign(reinterpret_cast<const char*>(s), n);
			if (!r.U32(k.crc32) || !r.U32(k.size) || !r.U32(k.disksize) || !r.U32(k.offset) || !r.U32(k.request))
				return false;
			if (!r.U16(len) || len > kMaxData || !r.Bytes(len, d))
				return false;
			Entry e;
			e.data.assign(d, d + len);
			entries_[k] = std::move(e); // a repeated key keeps the last, as Save never writes one
		}

		return r.left == 0;
	}
};

} // namespace lump_header_cache

#endif // __LUMP_HEADER_CACHE_HPP__
