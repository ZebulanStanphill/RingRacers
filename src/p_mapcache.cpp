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
#include <iterator>
#include <set>
#include <string>
#include <system_error>

#include "p_mapcache.h"

#include "cxxutil.hpp"
#include "doomdef.h"
#include "d_main.h"
#include "g_demo.h"
#include "i_system.h"
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
	path_ = std::string(srb2home) + PATHSEP + "mapcache.bin";

	std::ifstream in(path_, std::ios::binary);
	if (!in)
	{
		I_OutputMsg("UWP: no map cache yet\n");
		return;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (in.bad() || !cache_.Load(bytes.data(), bytes.size(), ParserVersion()))
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
			std::snprintf(saved, sizeof saved, "; saved %.1f MB", static_cast<double>(bytes.size()) / (1024.0 * 1024.0));
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
