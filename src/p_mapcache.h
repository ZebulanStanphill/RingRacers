// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  p_mapcache.h
/// \brief [UWP]: P_InitMapData's use of the map preview cache (map_preview_cache.hpp)

#ifndef __P_MAPCACHE__
#define __P_MAPCACHE__

#ifdef _UWP

#include "doomtype.h"
#include "doomstat.h"
#include "w_wad.h"
#include "map_preview_cache.hpp"

// One P_InitMapData call's use of mapcache.bin in the app's local folder (srb2home if no local folder is available).
// Reads it at the first PK3 map or ghost; a call without either reads nothing. Saves changes on destruction.
// A cache that cannot be read or written is only logged, and the game reads its files as usual.
class UWPMapCache
{
public:
	UWPMapCache();
	~UWPMapCache();
	UWPMapCache(const UWPMapCache&) = delete;
	UWPMapCache& operator=(const UWPMapCache&) = delete;

	// Sets *virtmap to the map's virtual resources and *thumbnail and *minimap to its PICTURE and MINIMAP lumps (NULL
	// when it has none), as vres_GetMap and vres_Find do. A PK3's map WAD that the cache has leaves *virtmap NULL, and
	// the lumps are valid until the next call.
	void GetMapPictures(lumpnum_t maplump, virtres_t **virtmap, virtlump_t **thumbnail, virtlump_t **minimap);

	// G_GetStaffGhostBrief of the PK3 ghost lump (allocated the same way), or NULL if it is not a ghost
	staffbrief_t *GetStaffBrief(UINT16 wadindex, UINT16 lumpnum);

private:
	void Load();

	bool loaded_ = false;
	map_preview_cache::Cache cache_;
	std::string path_;
	virtlump_t picture_, minimap_;
	unsigned mapHits_ = 0, mapReads_ = 0, ghostHits_ = 0, ghostReads_ = 0;
};

#endif // _UWP

#endif // __P_MAPCACHE__
