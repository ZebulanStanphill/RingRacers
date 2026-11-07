// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Zebulan Stanphill.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_uwpdata.h
/// \brief [UWP]: Copies of the game data files in the app's internal storage (sdl/i_uwpdata.cpp)
///
/// The data files are on a USB drive that reads at ~18 MB/s, so a launch that has to check a file on the drive copies it
/// to <LocalState>\data as it does, and later launches load the copy (see uwp_data_copy.hpp for when a copy is used).
/// The music, sound and staff ghost files are not copied (the internal storage is small and shared): they load from the
/// drive, and a staff ghost file that passed its check leaves a record that spares the next launch's.

#ifndef __I_UWPDATA_H__
#define __I_UWPDATA_H__

#include "doomtype.h"

#ifdef _UWP

#ifdef __cplusplus
extern "C" {
#endif

// ---- Main thread ----

// The app's local folder (LocalState, on internal storage), without a trailing separator, or NULL if it can't be had.
// Only the first call looks it up and logs.
const char *I_UWPLocalFolder(void);

// The path to load a main data file from: the copy in internal storage if there is a usable one, otherwise `source`
// (any path), whose existence, size and last-write time are what the copy is judged against. `expectedmd5` is the
// 32-digit hex MD5 the game expects of the file, or NULL for a file with none. Logs where the file will load from. The
// result lasts for the process. The first call also deletes leftover temporary files from the copy folder. A file that
// is never copied (see uwp_data_copy::kKeptOnDrive) resolves to `source`.
const char *I_UWPDataResolve(const char *source, const char *expectedmd5);

// Once every file has been resolved: deletes what the copy folder holds that is not the copy or record of one of them
// (logging each with its size), and logs how long the decisions made by I_UWPDataResolve took
void I_UWPDataLogStartup(void);

// Whether `path` needs no check: it is a usable copy chosen by I_UWPDataResolve, which was verified when it was made, or
// the source of a file kept on the drive that was verified on an earlier launch and is unchanged
boolean I_UWPDataIsVerified(const char *path);

// Where the file loaded from `source` is to be copied to this launch, or NULL if it isn't to be (including a file that
// is never copied)
const char *I_UWPDataCopyDest(const char *source);

// ---- The checking thread ----
// One copy at a time. Their errors are short messages in the buffer given. They log nothing and allocate nothing from
// the zone.

// Starts copying `source` to `dest`, which is written to dest.tmp until I_UWPDataCopyCommit. Fails if the folder
// doesn't have the file's size plus 256 MB free. `expectedmd5` is as for I_UWPDataResolve: it is what the file has
// been (or will be) verified against.
boolean I_UWPDataCopyOpen(const char *source, const char *dest, const char *expectedmd5, char *error, size_t errorsize);

// Appends to the copy. On failure the copy is abandoned and its temporary file deleted.
boolean I_UWPDataCopyWrite(const void *data, size_t size, char *error, size_t errorsize);

// Finishes a copy of the whole file that has been verified: replaces any old copy and its record with the new ones. On
// failure the copy is abandoned.
boolean I_UWPDataCopyCommit(char *error, size_t errorsize);

// Abandons the copy in progress, if any, deleting its temporary file
void I_UWPDataCopyAbort(void);

// For a file that is checked but never copied (and so not by a file with a copy to make): call before reading `source`.
// Returns whether the file gets a verified record if the check passes; if so, deletes any old record and notes the
// source's size and last-write time. `expectedmd5` is as for I_UWPDataCopyOpen.
boolean I_UWPDataVerifiedOpen(const char *source, const char *expectedmd5);

// Once the check of the file given to I_UWPDataVerifiedOpen has passed: writes its record, unless the source has changed
// since. Failure only means the next launch checks it again.
boolean I_UWPDataVerifiedCommit(char *error, size_t errorsize);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _UWP

#endif // __I_UWPDATA_H__
