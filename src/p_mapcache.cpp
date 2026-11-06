// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  p_mapcache.cpp
/// \brief [UWP]: P_InitMapData's use of the map preview cache (map_preview_cache.hpp)

#ifdef _UWP

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

#include "p_mapcache.h"

#include "cxxutil.hpp"
#include "doomdef.h"
#include "d_main.h"
#include "g_demo.h"
#include "i_system.h"
#include "i_uwpdata.h"
#include "z_zone.h"

namespace
{

// Bump this when G_GetStaffGhostBrief changes what it returns for a ghost
constexpr uint32_t kGhostBriefVersion = 1;

// Also changes with the demo format, which the parser reads
uint32_t ParserVersion()
{
	return (kGhostBriefVersion << 24) ^ (static_cast<uint32_t>(MAXPLAYERNAME) << 16) ^ G_UWPDemoVersions();
}

// Every key looked up or stored this session, across P_InitMapData calls (add-ons run it again): saving keeps only
// these, so entries for files that are gone do not pile up
std::set<map_preview_cache::Key>& UsedKeys()
{
	static std::set<map_preview_cache::Key> used;
	return used;
}

// The key of a PK3 lump, or false for anything else
bool LumpKey(UINT16 wadindex, UINT16 lumpnum, map_preview_cache::Key& key)
{
	if (wadindex >= numwadfiles || wadfiles[wadindex] == NULL || wadfiles[wadindex]->type != RET_PK3 ||
		lumpnum >= wadfiles[wadindex]->numlumps)
	{
		return false;
	}
	const lumpinfo_t& info = wadfiles[wadindex]->lumpinfo[lumpnum];
	if (info.fullname == NULL)
	{
		return false;
	}
	key.name = info.fullname;
	key.crc32 = info.uwpcrc32;
	key.size = static_cast<uint32_t>(info.size);
	key.disksize = static_cast<uint32_t>(info.disksize);
	return true;
}

std::vector<uint8_t> CopyLump(const virtlump_t* lump)
{
	if (lump == NULL || lump->data == NULL)
	{
		return {};
	}
	return std::vector<uint8_t>(lump->data, lump->data + lump->size);
}

} // namespace

UWPMapCache::UWPMapCache()
{
	std::memset(&picture_, 0, sizeof picture_);
	std::memset(&minimap_, 0, sizeof minimap_);
}

void UWPMapCache::Load()
{
	if (loaded_)
	{
		return;
	}
	loaded_ = true;
	const std::string oldpath = std::string(srb2home) + PATHSEP + "mapcache.bin";
	// Internal storage is far faster than the USB drive, where the cache used to be (i_uwpdata.h)
	const char* local = I_UWPLocalFolder();
	path_ = local ? std::string(local) + PATHSEP + "mapcache.bin" : oldpath;
	if (local)
	{
		// Delete the old cache, so that no one has to clean it up by hand
		std::error_code ec;
		bool removed = std::filesystem::remove(oldpath, ec);
		removed = std::filesystem::remove(oldpath + ".tmp", ec) || removed;
		if (removed)
		{
			I_OutputMsg("UWP: map cache: deleted the old one from %s\n", oldpath.c_str());
		}
	}

	// One read of the whole file, unbuffered: a stream's small buffer made this many small reads from the USB drive
	FILE* in = std::fopen(path_.c_str(), "rb");
	if (!in)
	{
		I_OutputMsg("UWP: no map cache yet\n");
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

	if (readok)
	{
		I_OutputMsg("UWP: map cache: read %.1f MB in %.2f s\n", static_cast<double>(bytes.size()) / (1024.0 * 1024.0),
			static_cast<double>(I_GetPreciseTime() - start) / static_cast<double>(I_GetPrecisePrecision()));
	}

	if (!readok || !cache_.Load(bytes.data(), bytes.size(), ParserVersion()))
	{
		I_OutputMsg("UWP: map cache %s is unreadable or from another version; starting over\n", path_.c_str());
	}
}

UWPMapCache::~UWPMapCache()
{
	char saved[64] = "";

	if (!loaded_)
	{
		return; // No PK3 maps or ghosts this time
	}

	if (cache_.Dirty())
	{
		const std::vector<uint8_t> bytes = cache_.Save(ParserVersion(), [](const map_preview_cache::Key& key) {
			return UsedKeys().count(key) != 0;
		});
		const std::string tmp = path_ + ".tmp";
		bool ok = false;
		const precise_t savestart = I_GetPreciseTime();
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (out)
			{
				out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				out.flush();
				ok = static_cast<bool>(out);
			}
		}
		std::error_code ec;
		if (ok)
		{
			std::filesystem::rename(tmp, path_, ec);
			ok = !ec;
		}
		if (ok)
		{
			std::snprintf(saved, sizeof saved, "; saved %.1f MB in %.2f s", static_cast<double>(bytes.size()) / (1024.0 * 1024.0),
				static_cast<double>(I_GetPreciseTime() - savestart) / static_cast<double>(I_GetPrecisePrecision()));
		}
		else
		{
			I_OutputMsg("UWP: map cache could not be saved to %s%s%s\n", path_.c_str(), ec ? ": " : "",
				ec ? ec.message().c_str() : "");
			std::filesystem::remove(tmp, ec);
		}
	}

	I_OutputMsg("UWP: map cache: %u maps and %u staff ghosts from the cache, %u maps and %u ghosts read from the game files%s\n",
		mapHits_, ghostHits_, mapReads_, ghostReads_, saved);
}

void UWPMapCache::GetMapPictures(lumpnum_t maplump, virtres_t **virtmap, virtlump_t **thumbnail, virtlump_t **minimap)
{
	map_preview_cache::Key key;
	const bool cacheable = LumpKey(WADFILENUM(maplump), LUMPNUM(maplump), key) && W_IsLumpWad(maplump);

	if (cacheable)
	{
		Load();
		UsedKeys().insert(key);
		if (const map_preview_cache::MapEntry* hit = cache_.FindMap(key))
		{
			std::memset(&picture_, 0, sizeof picture_);
			std::memset(&minimap_, 0, sizeof minimap_);
			// The patches made from these copy the bytes
			picture_.data = const_cast<UINT8*>(hit->picture.data());
			picture_.size = hit->picture.size();
			minimap_.data = const_cast<UINT8*>(hit->minimap.data());
			minimap_.size = hit->minimap.size();
			*virtmap = NULL;
			*thumbnail = hit->hasPicture ? &picture_ : NULL;
			*minimap = hit->hasMinimap ? &minimap_ : NULL;
			mapHits_++;
			return;
		}
	}

	*virtmap = vres_GetMap(maplump);
	*thumbnail = vres_Find(*virtmap, "PICTURE");
	*minimap = vres_Find(*virtmap, "MINIMAP");

	if (cacheable)
	{
		map_preview_cache::MapEntry entry;
		entry.hasPicture = *thumbnail != NULL;
		entry.hasMinimap = *minimap != NULL;
		entry.picture = CopyLump(*thumbnail);
		entry.minimap = CopyLump(*minimap);
		cache_.PutMap(key, std::move(entry));
		mapReads_++;
	}
}

staffbrief_t *UWPMapCache::GetStaffBrief(UINT16 wadindex, UINT16 lumpnum)
{
	map_preview_cache::Key key;
	const bool cacheable = LumpKey(wadindex, lumpnum, key);

	if (cacheable)
	{
		Load();
		UsedKeys().insert(key);
		if (const map_preview_cache::GhostEntry* hit = cache_.FindGhost(key))
		{
			ghostHits_++;
			if (!hit->valid)
			{
				return NULL;
			}
			staffbrief_t* brief = static_cast<staffbrief_t*>(Z_Malloc(sizeof(staffbrief_t), PU_STATIC, NULL));
			if (brief != NULL)
			{
				std::memset(brief, 0, sizeof *brief);
				strlcpy(brief->name, hit->name.c_str(), sizeof brief->name);
				brief->time = hit->time;
				brief->lap = hit->lap;
			}
			return brief;
		}
	}

	size_t lumplength = W_LumpLengthPwad(wadindex, lumpnum);
	UINT8* ghostdata = static_cast<UINT8*>(Z_Malloc(lumplength, PU_STATIC, nullptr));
	auto ghostdata_finalizer = srb2::finally([=]() { Z_Free(ghostdata); });

	W_ReadLumpPwad(wadindex, lumpnum, ghostdata);
	staffbrief_t* brief = G_GetStaffGhostBrief(ghostdata);

	if (cacheable)
	{
		map_preview_cache::GhostEntry entry;
		if (brief != NULL)
		{
			entry.valid = true;
			entry.time = brief->time;
			entry.lap = brief->lap;
			entry.name = brief->name;
		}
		cache_.PutGhost(key, std::move(entry));
		ghostReads_++;
	}
	return brief;
}

#endif // _UWP
