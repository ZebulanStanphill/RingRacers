// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_uwpheadercache.cpp
/// \brief [UWP]: W_ReadLumpHeaderPwad's use of the lump header cache (lump_header_cache.hpp)

#ifdef _UWP

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "w_uwpheadercache.h"

#include "doomdef.h"
#include "d_main.h"
#include "i_system.h"
#include "i_uwpdata.h"
#include "lump_header_cache.hpp"
#include "w_wad.h"

namespace
{

// The most bytes of one read that are cached. The hot reads at startup are 8 bytes (a PNG signature, a patch or flat
// header); reads of whole lumps are not cached, except that a lump this small is.
constexpr size_t kMaxCachedRead = 256;

bool g_active = true; // until W_UWPHeaderCacheFinish
bool g_loaded = false;
lump_header_cache::Cache g_cache;
std::string g_path;
unsigned g_hits = 0, g_misses = 0;

double Seconds(precise_t start)
{
	return static_cast<double>(I_GetPreciseTime() - start) / static_cast<double>(I_GetPrecisePrecision());
}

// The key of a read of a PK3 lump that can be cached, or false. length is how many bytes W_ReadLumpHeaderPwad reads.
bool ReadKey(UINT16 wad, UINT16 lump, size_t size, size_t offset, lump_header_cache::Key &key, size_t &length)
{
	if (wad >= numwadfiles || wadfiles[wad] == NULL || wadfiles[wad]->type != RET_PK3 || lump >= wadfiles[wad]->numlumps)
	{
		return false;
	}

	const lumpinfo_t &info = wadfiles[wad]->lumpinfo[lump];
	const size_t lumpsize = info.size;

	// As W_ReadLumpHeaderPwad: nothing is read from an empty lump or past its end, and a size of 0 or past the end is
	// all that is left
	if (info.fullname == NULL || lumpsize == 0 || lumpsize < offset)
	{
		return false;
	}
	length = (size == 0 || size + offset > lumpsize) ? lumpsize - offset : size;
	if (length == 0 || length > kMaxCachedRead)
	{
		return false;
	}

	const size_t namelen = std::strlen(info.fullname);
	if (namelen > lump_header_cache::kMaxName || size > UINT32_MAX || offset > UINT32_MAX)
	{
		return false;
	}

	key.name.assign(info.fullname, namelen);
	key.crc32 = info.uwpcrc32;
	key.size = static_cast<uint32_t>(info.size);
	key.disksize = static_cast<uint32_t>(info.disksize);
	key.offset = static_cast<uint32_t>(offset);
	key.request = static_cast<uint32_t>(size);
	return true;
}

void Load()
{
	g_loaded = true;
	const char *local = I_UWPLocalFolder();
	g_path = std::string(local ? local : srb2home) + PATHSEP + "lumpheaders.bin";

	// One read of the whole file, unbuffered, as for the map cache
	FILE *in = std::fopen(g_path.c_str(), "rb");
	if (!in)
	{
		I_OutputMsg("UWP: header cache: no file yet\n");
		return;
	}
	std::setvbuf(in, nullptr, _IONBF, 0);

	const precise_t start = I_GetPreciseTime();
	std::vector<uint8_t> bytes;
	bool readok = false;
	if (std::fseek(in, 0, SEEK_END) == 0)
	{
		const long size = std::ftell(in);
		if (size >= 0 && std::fseek(in, 0, SEEK_SET) == 0)
		{
			bytes.resize(static_cast<size_t>(size));
			size_t got = 0;
			while (got < bytes.size())
			{
				const size_t n = std::fread(bytes.data() + got, 1, bytes.size() - got, in);
				if (n == 0)
				{
					break; // End of file or an error
				}
				got += n;
			}
			readok = got == bytes.size();
		}
	}
	std::fclose(in);

	if (readok && g_cache.Load(bytes.data(), bytes.size()))
	{
		I_OutputMsg("UWP: header cache: read %.1f MB (%u entries) in %.2f s\n",
			static_cast<double>(bytes.size()) / (1024.0 * 1024.0), static_cast<unsigned>(g_cache.Size()), Seconds(start));
	}
	else
	{
		g_cache.Load(nullptr, 0); // empty
		I_OutputMsg("UWP: header cache: %s is unreadable or from another version; starting over\n", g_path.c_str());
	}
}

} // namespace

bool W_UWPHeaderCacheLookup(UINT16 wad, UINT16 lump, void *dest, size_t size, size_t offset, size_t *result)
{
	lump_header_cache::Key key;
	size_t length;

	if (!g_active || !ReadKey(wad, lump, size, offset, key, length))
	{
		return false;
	}

	if (!g_loaded)
	{
		Load();
	}

	const lump_header_cache::Entry *hit = g_cache.Find(key);
	if (hit == NULL || hit->data.size() != length) // always equal, as a stored read is complete
	{
		g_misses++;
		return false;
	}

	if (length)
	{
		std::memcpy(dest, hit->data.data(), length);
	}
	*result = length;
	g_hits++;
	return true;
}

void W_UWPHeaderCacheStore(UINT16 wad, UINT16 lump, const void *dest, size_t size, size_t offset, size_t result)
{
	lump_header_cache::Key key;
	size_t length;

	// Only a complete read is stored (a failed inflate returns 0, a short fread fewer bytes)
	if (!g_active || !g_loaded || !ReadKey(wad, lump, size, offset, key, length) || result != length)
	{
		return;
	}

	g_cache.Put(key, static_cast<const uint8_t *>(dest), length);
}

void W_UWPHeaderCacheFinish(void)
{
	if (!g_active)
	{
		return;
	}
	g_active = false;

	if (g_loaded)
	{
		char saved[96] = "";

		if (g_cache.NeedsSave())
		{
			const precise_t start = I_GetPreciseTime();
			const std::vector<uint8_t> bytes = g_cache.Save();
			const std::string tmp = g_path + ".tmp";
			bool ok = false;
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (out)
				{
					out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
					out.flush();
					ok = static_cast<bool>(out);
				}
			}
			std::error_code ec;
			if (ok)
			{
				std::filesystem::rename(tmp, g_path, ec);
				ok = !ec;
			}
			if (ok)
			{
				std::snprintf(saved, sizeof saved, "; saved %.1f MB in %.2f s",
					static_cast<double>(bytes.size()) / (1024.0 * 1024.0), Seconds(start));
			}
			else
			{
				I_OutputMsg("UWP: header cache: could not be saved to %s%s%s\n", g_path.c_str(), ec ? ": " : "",
					ec ? ec.message().c_str() : "");
				std::filesystem::remove(tmp, ec);
			}
		}

		I_OutputMsg("UWP: header cache: %u reads from the cache, %u from the game files; %u entries used%s\n", g_hits,
			g_misses, static_cast<unsigned>(g_cache.UsedCount()), saved);
	}

	g_cache = lump_header_cache::Cache();
	std::string().swap(g_path);
}

#endif // _UWP
