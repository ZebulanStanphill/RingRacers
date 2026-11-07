// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Zebulan Stanphill.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  uwp_data_copy.hpp
/// \brief [UWP]: The record kept beside a copy of a game data file, and when the copy can be used
///
/// On Xbox the game's data files are on a USB drive that reads at ~18 MB/s, so the first launch copies them to the
/// app's internal storage (sdl/i_uwpdata.cpp), as it verifies them, and later launches load the copies. A copy
/// X.pk3 has a sidecar X.pk3.src: one line recording the source it was copied from. The copy can be used only while
/// the source is still the file that was copied (same size and last-write time), the copy is whole, and the MD5 the
/// game expects of the file is the one it was verified against, so a game update that changes a file's expected MD5
/// makes a fresh copy.
///
/// The internal storage is a ~3 GB volume shared by the data of every sideloaded app, so the big files that are only
/// read in small parts or once are not copied (kKeptOnDrive) and load from the USB drive. One of those with an
/// expected MD5 would be checked from the drive on every launch, so after a check passes a "verified" record,
/// X.pk3.verified, in the same format as the sidecar, notes the file's size, last-write time and MD5; while the file and
/// the expected MD5 are as recorded, it loads from the drive without being checked again.
///
/// Header-only and free of engine and Windows types, so that the engine glue and the native tests share it.

#ifndef __UWP_DATA_COPY_HPP__
#define __UWP_DATA_COPY_HPP__

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace uwp_data_copy
{

constexpr char kMagic[] = "ringracers-data-copy";
constexpr unsigned kFormatVersion = 1;
constexpr size_t kMaxRecord = 256;

// What a copy was made from. The MD5 is 32 lowercase hex digits, or "-" for a file with no expected MD5.
struct Record
{
	uint64_t size = 0; // the source's size in bytes
	int64_t modified = 0; // the source's last-write time, in whatever unit the caller reads it in (it is only compared)
	std::string md5 = "-";
};

// Files that are never copied to internal storage, and why
struct KeptFile
{
	const char* name;
	const char* why;
};

constexpr KeptFile kKeptOnDrive[] = {
	// ~300 MB and ~90 MB, and a track is only read when it starts
	{"music.pk3", "large, read only when a track starts"},
	{"altmusic.pk3", "large, read only when a track starts"},
	// ~40 MB, kept in RAM by the engine instead (sdl/new_sound.cpp), so it is read from the drive once, at startup
	{"sounds.pk3", "held in memory"},
	// ~50 MB, only read for Time Attack staff ghosts; the menus use the map cache
	{"staffghosts.pk3", "read only for staff ghosts"},
};

inline char LowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// File names on Windows compare without regard to case
inline bool SameName(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
	{
		return false;
	}
	for (size_t i = 0; i < a.size(); i++)
	{
		if (LowerAscii(a[i]) != LowerAscii(b[i]))
		{
			return false;
		}
	}
	return true;
}

// Why the file called `name` (no folder) is never copied, or null if it is one that is
inline const char* KeptReason(const std::string& name)
{
	for (const KeptFile& kept : kKeptOnDrive)
	{
		if (SameName(name, kept.name))
		{
			return kept.why;
		}
	}
	return nullptr;
}

inline bool IsKeptOnDrive(const std::string& name)
{
	return KeptReason(name) != nullptr;
}

// A file of the game's list this launch, for deciding what the data folder should hold
struct ListedFile
{
	std::string name;
	bool hasMd5 = false; // the game expects an MD5 of it
};

constexpr const char* kSidecarSuffix = ".src";
constexpr const char* kVerifiedSuffix = ".verified";

// Whether `fileName` in the data folder is the copy or sidecar of a listed file that is copied, or the verified record
// of a listed file that is kept on the drive and has an expected MD5. Anything else is left over from an earlier
// version, or from a file the game no longer keeps, and is deleted.
inline bool WantedInDataFolder(const std::string& fileName, const std::vector<ListedFile>& listed)
{
	for (const ListedFile& file : listed)
	{
		if (IsKeptOnDrive(file.name))
		{
			if (file.hasMd5 && SameName(fileName, file.name + kVerifiedSuffix))
			{
				return true;
			}
		}
		else if (SameName(fileName, file.name) || SameName(fileName, file.name + kSidecarSuffix))
		{
			return true;
		}
	}
	return false;
}

// The source file as it is now
struct SourceStat
{
	bool exists = false;
	uint64_t size = 0;
	int64_t modified = 0;
};

// The copy file as it is now
struct CopyStat
{
	bool exists = false;
	uint64_t size = 0;
};

enum class Verdict
{
	Usable,
	SourceMissing,
	CopyMissing,
	NoRecord, // no sidecar, or one that does not parse
	SourceChanged, // the source's size or last-write time is not the one recorded
	Md5Changed, // the MD5 expected now is not the one the copy was verified against
	CopyIncomplete, // the copy's size is not the source's
};

inline const char* VerdictText(Verdict v)
{
	switch (v)
	{
		case Verdict::Usable: return "usable";
		case Verdict::SourceMissing: return "source missing";
		case Verdict::CopyMissing: return "no copy";
		case Verdict::NoRecord: return "no valid record";
		case Verdict::SourceChanged: return "source changed";
		case Verdict::Md5Changed: return "expected MD5 changed";
		case Verdict::CopyIncomplete: return "copy incomplete";
	}
	return "unknown";
}

inline bool IsHexDigit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// The text form of an expected MD5: lowercase hex, or "-" for none. Returns false for anything else.
inline bool NormalizeMd5(const char* text, std::string& out)
{
	out.clear();
	if (text == nullptr)
	{
		out = "-";
		return true;
	}
	for (const char* p = text; *p; p++)
	{
		char c = *p;
		if (c >= 'A' && c <= 'F')
		{
			c = static_cast<char>(c - 'A' + 'a');
		}
		if (!IsHexDigit(c))
		{
			out.clear();
			return false;
		}
		out.push_back(c);
	}
	if (out.size() != 32)
	{
		out.clear();
		return false;
	}
	return true;
}

// "ringracers-data-copy 1 <size> <modified> <md5>\n"
inline std::string FormatRecord(const Record& r)
{
	char line[kMaxRecord];
	std::snprintf(line, sizeof line, "%s %u %llu %lld %s\n", kMagic, kFormatVersion,
		static_cast<unsigned long long>(r.size), static_cast<long long>(r.modified), r.md5.c_str());
	return line;
}

inline bool ParseUnsigned(const char*& p, uint64_t& out)
{
	if (*p < '0' || *p > '9')
	{
		return false;
	}
	uint64_t v = 0;
	while (*p >= '0' && *p <= '9')
	{
		const unsigned d = static_cast<unsigned>(*p - '0');
		if (v > (UINT64_MAX - d) / 10)
		{
			return false;
		}
		v = v * 10 + d;
		p++;
	}
	out = v;
	return true;
}

// Parses the text of a sidecar. Returns false unless it is exactly one well-formed line of this version.
inline bool ParseRecord(const char* text, size_t length, Record& out)
{
	if (text == nullptr || length == 0 || length >= kMaxRecord)
	{
		return false;
	}
	const std::string line(text, length);
	if (line.find('\0') != std::string::npos)
	{
		return false;
	}
	const char* p = line.c_str();

	const size_t magiclen = sizeof kMagic - 1;
	if (line.compare(0, magiclen, kMagic) != 0 || p[magiclen] != ' ')
	{
		return false;
	}
	p += magiclen + 1;

	uint64_t version = 0;
	if (!ParseUnsigned(p, version) || version != kFormatVersion || *p != ' ')
	{
		return false;
	}
	p++;

	uint64_t size = 0;
	if (!ParseUnsigned(p, size) || *p != ' ')
	{
		return false;
	}
	p++;

	bool negative = false;
	if (*p == '-')
	{
		negative = true;
		p++;
	}
	uint64_t modified = 0;
	if (!ParseUnsigned(p, modified) || modified > static_cast<uint64_t>(INT64_MAX) || *p != ' ')
	{
		return false;
	}
	p++;

	std::string md5;
	if (*p == '-' && (p[1] == '\n' || p[1] == '\0'))
	{
		md5 = "-";
		p++;
	}
	else
	{
		for (; IsHexDigit(*p); p++)
		{
			md5.push_back(*p);
		}
		if (md5.size() != 32)
		{
			return false;
		}
	}
	if (*p == '\n')
	{
		p++;
	}
	if (*p != '\0')
	{
		return false; // trailing text, or a second line
	}

	out.size = size;
	out.modified = negative ? -static_cast<int64_t>(modified) : static_cast<int64_t>(modified);
	out.md5 = md5;
	return true;
}

// Whether the copy can be loaded instead of the source. `record` is null when there is no sidecar or it did not
// parse; `expectedMd5` is NormalizeMd5's form of what the game expects of the file.
inline Verdict Decide(const SourceStat& source, const CopyStat& copy, const Record* record, const std::string& expectedMd5)
{
	if (!source.exists)
	{
		return Verdict::SourceMissing;
	}
	if (!copy.exists)
	{
		return Verdict::CopyMissing;
	}
	if (record == nullptr)
	{
		return Verdict::NoRecord;
	}
	if (record->size != source.size || record->modified != source.modified)
	{
		return Verdict::SourceChanged;
	}
	if (record->md5 != expectedMd5)
	{
		return Verdict::Md5Changed;
	}
	if (copy.size != source.size)
	{
		return Verdict::CopyIncomplete;
	}
	return Verdict::Usable;
}

enum class VerifiedVerdict
{
	Verified, // checked on an earlier launch, and the file and the expected MD5 are as they were
	SourceMissing,
	NoRecord,
	SourceChanged,
	Md5Changed,
};

inline const char* VerifiedVerdictText(VerifiedVerdict v)
{
	switch (v)
	{
		case VerifiedVerdict::Verified: return "verified earlier";
		case VerifiedVerdict::SourceMissing: return "source missing";
		case VerifiedVerdict::NoRecord: return "no valid verified record";
		case VerifiedVerdict::SourceChanged: return "source changed";
		case VerifiedVerdict::Md5Changed: return "expected MD5 changed";
	}
	return "unknown";
}

// Whether a file kept on the drive can skip its check: `record` is its verified record (null if there is none or it did
// not parse), and `expectedMd5` is NormalizeMd5's form of what the game expects of it. A file with no expected MD5
// ("-") is never checked, so it has no record.
inline VerifiedVerdict DecideVerified(const SourceStat& source, const Record* record, const std::string& expectedMd5)
{
	if (!source.exists)
	{
		return VerifiedVerdict::SourceMissing;
	}
	if (record == nullptr || expectedMd5 == "-")
	{
		return VerifiedVerdict::NoRecord;
	}
	if (record->size != source.size || record->modified != source.modified)
	{
		return VerifiedVerdict::SourceChanged;
	}
	if (record->md5 != expectedMd5)
	{
		return VerifiedVerdict::Md5Changed;
	}
	return VerifiedVerdict::Verified;
}

} // namespace uwp_data_copy

#endif // __UWP_DATA_COPY_HPP__
