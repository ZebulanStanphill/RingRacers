// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_uwpheadercache.h
/// \brief [UWP]: W_ReadLumpHeaderPwad's use of the lump header cache (lump_header_cache.hpp)

#ifndef __W_UWPHEADERCACHE__
#define __W_UWPHEADERCACHE__

#ifdef _UWP

#include <cstddef>

#include "doomtype.h"

// The cache is used from the start of the game until W_UWPHeaderCacheFinish, on the main thread only. It holds the bytes
// that small reads of PK3 lumps returned, in <LocalState>\lumpheaders.bin, which the first lookup reads.

// If W_ReadLumpHeaderPwad(wad, lump, dest, size, offset) is a read the cache has, copies the bytes to dest, sets *result to
// the length to return, and returns true, having touched no file. Otherwise returns false.
bool W_UWPHeaderCacheLookup(UINT16 wad, UINT16 lump, void *dest, size_t size, size_t offset, size_t *result);

// After a read that W_UWPHeaderCacheLookup missed: records what it returned (result bytes at dest), if it can be cached
void W_UWPHeaderCacheStore(UINT16 wad, UINT16 lump, const void *dest, size_t size, size_t offset, size_t result);

// Ends the cache's use: saves it if it changed, frees it, and sends later reads straight to the files
void W_UWPHeaderCacheFinish(void);

#endif // _UWP

#endif // __W_UWPHEADERCACHE__
