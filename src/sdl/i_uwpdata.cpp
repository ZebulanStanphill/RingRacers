// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Zebulan Stanphill.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_uwpdata.cpp
/// \brief [UWP]: Copies of the game data files in the app's internal storage
///
/// The game's data files are on a USB drive that reads at ~18 MB/s. A launch that has to check a file on the drive
/// (W_UWPAssetCheckWorker, in w_wad.cpp) writes it to <LocalState>\data\X.pk3 as it reads it, and then a sidecar
/// X.pk3.src recording what it was copied from; later launches load the copy while the source is unchanged. The
/// rule for that is in uwp_data_copy.hpp. Every file is written under a .tmp name and renamed once whole, so a copy
/// that exists is complete, and the sidecar is written last, so a copy without a current sidecar is never used.

#ifdef _UWP

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

#include "SDL.h"
#include "SDL_system.h"

// SDL's WinRT path lookup, which SDL_system.h only declares when __WINRT__ is defined, as it isn't in this build.
// pathType is SDL's SDL_WinRT_Path, whose SDL_WINRT_PATH_LOCAL_FOLDER (the app's LocalState) is 1.
#ifndef __WINRT__
extern "C" __declspec(dllimport) const char* SDLCALL SDL_WinRTGetFSPathUTF8(int pathType);
#define SDL_WINRT_PATH_LOCAL_FOLDER 1
#endif

#include "../doomdef.h"
#include "../i_system.h"
#include "../i_uwpdata.h"
#include "../uwp_data_copy.hpp"
#include "../w_wad.h" // MAX_WADPATH

namespace
{

constexpr size_t kMaxEntries = 32;
constexpr unsigned long long kFreeSpaceMargin = 256ull * 1024 * 1024;
constexpr size_t kWidePath = 1100;

// ---- Main thread ----

struct Entry
{
	char source[MAX_WADPATH];
	char dest[MAX_WADPATH]; // empty if there is nowhere to copy to
	bool usable;
};

Entry g_entries[kMaxEntries];
size_t g_numEntries;

bool g_localLooked;
char g_localRoot[MAX_WADPATH]; // empty if unavailable
char g_dataDir[MAX_WADPATH]; // empty if unavailable
double g_decisionSeconds;

double Elapsed(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::wstring Widen(const char* utf8)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	if (n <= 0)
	{
		return std::wstring();
	}
	std::wstring wide(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], n);
	wide.resize(static_cast<size_t>(n) - 1);
	return wide;
}

const char* BaseName(const char* path)
{
	const char* name = path;
	for (const char* p = path; *p; p++)
	{
		if (*p == '/' || *p == '\\')
		{
			name = p + 1;
		}
	}
	return name;
}

bool StatPath(const wchar_t* path, uint64_t& size, int64_t& modified)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		return false;
	}
	size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	modified = static_cast<int64_t>((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
		data.ftLastWriteTime.dwLowDateTime);
	return true;
}

// Leftovers of a copy that was interrupted
void DeleteTemporaryFiles(const std::wstring& dir)
{
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileExW((dir + L"\\*.tmp").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE)
	{
		return;
	}
	std::vector<std::wstring> names;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			names.push_back(found.cFileName);
		}
	} while (FindNextFileW(find, &found));
	FindClose(find);
	for (const std::wstring& name : names)
	{
		DeleteFileW((dir + L"\\" + name).c_str());
	}
}

void LookUpLocalFolder()
{
	if (g_localLooked)
	{
		return;
	}
	g_localLooked = true;

	const char* local = SDL_WinRTGetFSPathUTF8(SDL_WINRT_PATH_LOCAL_FOLDER);
	if (local == nullptr || *local == '\0' || strlen(local) + 16 >= sizeof g_localRoot)
	{
		I_OutputMsg("UWP: data: no local folder, loading from the USB drive\n");
		return;
	}
	strlcpy(g_localRoot, local, sizeof g_localRoot);
	size_t length = strlen(g_localRoot);
	while (length > 0 && (g_localRoot[length - 1] == '\\' || g_localRoot[length - 1] == '/'))
	{
		g_localRoot[--length] = '\0';
	}
}

void PrepareDataFolder()
{
	static bool prepared;
	if (prepared)
	{
		return;
	}
	prepared = true;
	LookUpLocalFolder();
	if (!g_localRoot[0])
	{
		return;
	}

	snprintf(g_dataDir, sizeof g_dataDir, "%s\\data", g_localRoot);
	const std::wstring dir = Widen(g_dataDir);
	if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		I_OutputMsg("UWP: data: cannot create %s (error %lu), loading from the USB drive\n", g_dataDir, GetLastError());
		g_dataDir[0] = '\0';
		return;
	}
	DeleteTemporaryFiles(dir);
}

// The sidecar of a copy, if it is there and parses
bool ReadRecord(const std::wstring& path, uwp_data_copy::Record& record)
{
	FILE* in = _wfopen(path.c_str(), L"rb");
	if (!in)
	{
		return false;
	}
	char text[uwp_data_copy::kMaxRecord + 1];
	const size_t length = fread(text, 1, sizeof text, in);
	const bool readok = !ferror(in);
	fclose(in);
	return readok && uwp_data_copy::ParseRecord(text, length, record);
}

bool SameFile(const char* a, const char* b)
{
	return _stricmp(a, b) == 0;
}

// ---- The checking thread ----

// Only the checking thread opens, writes and commits copies, one at a time
struct Copy
{
	FILE* file;
	unsigned long long size; // the source's, when the copy was opened
	unsigned long long written;
	long long modified; // likewise
	char md5[33];
	wchar_t source[kWidePath];
	wchar_t dest[kWidePath];
	wchar_t tmp[kWidePath];
};

Copy g_copy;

void SetError(char* error, size_t errorsize, const char* what, unsigned long code)
{
	snprintf(error, errorsize, "%s (error %lu)", what, code);
}

void SetErrno(char* error, size_t errorsize, const char* what)
{
	snprintf(error, errorsize, "%s (errno %d)", what, errno);
}

// Writes `text` to `path` through a .tmp name, and renames it over any file there
bool WriteSmallFile(const wchar_t* path, const wchar_t* tmp, const std::string& text, char* error, size_t errorsize)
{
	FILE* out = _wfopen(tmp, L"wb");
	if (!out)
	{
		SetErrno(error, errorsize, "Cannot create record");
		return false;
	}
	bool ok = fwrite(text.data(), 1, text.size(), out) == text.size() && fflush(out) == 0 && _commit(_fileno(out)) == 0;
	ok = (fclose(out) == 0) && ok;
	if (!ok)
	{
		SetErrno(error, errorsize, "Cannot write record");
		DeleteFileW(tmp);
		return false;
	}
	if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
	{
		SetError(error, errorsize, "Cannot rename record", GetLastError());
		DeleteFileW(tmp);
		return false;
	}
	return true;
}

} // namespace

extern "C" {

const char* I_UWPLocalFolder(void)
{
	LookUpLocalFolder();
	return g_localRoot[0] ? g_localRoot : nullptr;
}

const char* I_UWPDataResolve(const char* source, const char* expectedmd5)
{
	if (g_numEntries == kMaxEntries || strlen(source) >= sizeof g_entries[0].source)
	{
		return source;
	}
	const auto start = std::chrono::steady_clock::now();

	// The caller's string may be in a buffer that logging reuses, so work from a copy of it
	Entry& entry = g_entries[g_numEntries++];
	strlcpy(entry.source, source, sizeof entry.source);
	entry.dest[0] = '\0';
	entry.usable = false;

	PrepareDataFolder();

	const std::wstring wideSource = Widen(entry.source);
	uwp_data_copy::SourceStat sourcestat;
	sourcestat.exists = StatPath(wideSource.c_str(), sourcestat.size, sourcestat.modified);
	std::string expected;
	const bool expectedok = uwp_data_copy::NormalizeMd5(expectedmd5, expected);
	const char* name = BaseName(entry.source);
	const char* why = nullptr;

	if (!sourcestat.exists)
	{
		// Left to the caller to report if it needs the file
	}
	else if (!g_dataDir[0])
	{
		why = "no internal storage";
	}
	else if (!expectedok || strlen(g_dataDir) + 1 + strlen(name) >= sizeof entry.dest)
	{
		why = "cannot copy";
	}
	else
	{
		snprintf(entry.dest, sizeof entry.dest, "%s\\%s", g_dataDir, name);
		const std::wstring wideDest = Widen(entry.dest);
		uwp_data_copy::CopyStat copystat;
		int64_t copymodified = 0;
		copystat.exists = StatPath(wideDest.c_str(), copystat.size, copymodified);

		uwp_data_copy::Record record;
		const bool haverecord = copystat.exists && ReadRecord(wideDest + L".src", record);
		const uwp_data_copy::Verdict verdict =
			uwp_data_copy::Decide(sourcestat, copystat, haverecord ? &record : nullptr, expected);
		entry.usable = verdict == uwp_data_copy::Verdict::Usable;
		if (!entry.usable)
		{
			why = uwp_data_copy::VerdictText(verdict);
		}
	}

	if (sourcestat.exists)
	{
		if (entry.usable)
		{
			I_OutputMsg("UWP: data: %s: internal storage\n", name);
		}
		else
		{
			I_OutputMsg("UWP: data: %s: USB drive (%s)\n", name, why ? why : "unknown");
		}
	}

	g_decisionSeconds += Elapsed(start);
	return entry.usable ? entry.dest : entry.source;
}

void I_UWPDataLogStartup(void)
{
	I_OutputMsg("UWP: data: chose where to load from in %.3f s\n", g_decisionSeconds);
}

boolean I_UWPDataIsCopy(const char* path)
{
	for (size_t i = 0; i < g_numEntries; i++)
	{
		if (g_entries[i].usable && SameFile(g_entries[i].dest, path))
		{
			return true;
		}
	}
	return false;
}

const char* I_UWPDataCopyDest(const char* source)
{
	for (size_t i = 0; i < g_numEntries; i++)
	{
		const Entry& entry = g_entries[i];
		if (!entry.usable && entry.dest[0] && SameFile(entry.source, source))
		{
			return entry.dest;
		}
	}
	return nullptr;
}

boolean I_UWPDataCopyOpen(const char* source, const char* dest, const char* expectedmd5, char* error, size_t errorsize)
{
	I_UWPDataCopyAbort();
	std::string md5;
	if (!uwp_data_copy::NormalizeMd5(expectedmd5, md5))
	{
		snprintf(error, errorsize, "Bad expected MD5");
		return false;
	}
	const std::wstring wideSource = Widen(source);
	const std::wstring wideDest = Widen(dest);
	if (wideSource.empty() || wideDest.empty() || wideSource.size() >= kWidePath || wideDest.size() + 8 >= kWidePath)
	{
		snprintf(error, errorsize, "Path too long");
		return false;
	}

	uint64_t size = 0;
	int64_t modified = 0;
	if (!StatPath(wideSource.c_str(), size, modified))
	{
		SetError(error, errorsize, "Cannot read the source's size", GetLastError());
		return false;
	}

	// Room for the file, and for the system to keep working: the margin is for the latter
	const size_t slash = wideDest.find_last_of(L"\\/");
	const std::wstring dir = slash == std::wstring::npos ? L"." : wideDest.substr(0, slash);
	ULARGE_INTEGER available;
	if (!GetDiskFreeSpaceExW(dir.c_str(), &available, nullptr, nullptr))
	{
		SetError(error, errorsize, "Cannot check free space", GetLastError());
		return false;
	}
	if (available.QuadPart < size || available.QuadPart - size < kFreeSpaceMargin)
	{
		snprintf(error, errorsize, "Not enough free space (%llu MB free, %llu MB needed)",
			static_cast<unsigned long long>(available.QuadPart / (1024 * 1024)),
			static_cast<unsigned long long>((size + kFreeSpaceMargin) / (1024 * 1024)));
		return false;
	}

	wcscpy(g_copy.source, wideSource.c_str());
	wcscpy(g_copy.dest, wideDest.c_str());
	swprintf(g_copy.tmp, kWidePath, L"%ls.tmp", wideDest.c_str());
	strlcpy(g_copy.md5, md5.c_str(), sizeof g_copy.md5);
	g_copy.size = size;
	g_copy.modified = modified;
	g_copy.written = 0;

	DeleteFileW(g_copy.tmp);
	g_copy.file = _wfopen(g_copy.tmp, L"wb");
	if (!g_copy.file)
	{
		SetErrno(error, errorsize, "Cannot create the copy");
		return false;
	}
	setvbuf(g_copy.file, nullptr, _IONBF, 0);
	return true;
}

boolean I_UWPDataCopyWrite(const void* data, size_t size, char* error, size_t errorsize)
{
	if (!g_copy.file)
	{
		snprintf(error, errorsize, "No copy open");
		return false;
	}
	if (fwrite(data, 1, size, g_copy.file) != size)
	{
		SetErrno(error, errorsize, "Cannot write the copy");
		I_UWPDataCopyAbort();
		return false;
	}
	g_copy.written += size;
	return true;
}

boolean I_UWPDataCopyCommit(char* error, size_t errorsize)
{
	if (!g_copy.file)
	{
		snprintf(error, errorsize, "No copy open");
		return false;
	}

	uint64_t size = 0;
	int64_t modified = 0;
	if (g_copy.written != g_copy.size)
	{
		snprintf(error, errorsize, "Copied %llu bytes of %llu", g_copy.written, g_copy.size);
		I_UWPDataCopyAbort();
		return false;
	}
	if (!StatPath(g_copy.source, size, modified) || size != g_copy.size || modified != g_copy.modified)
	{
		snprintf(error, errorsize, "The source changed while it was copied");
		I_UWPDataCopyAbort();
		return false;
	}

	bool ok = fflush(g_copy.file) == 0 && _commit(_fileno(g_copy.file)) == 0;
	ok = (fclose(g_copy.file) == 0) && ok;
	g_copy.file = nullptr;
	if (!ok)
	{
		SetErrno(error, errorsize, "Cannot finish the copy");
		DeleteFileW(g_copy.tmp);
		return false;
	}

	// The old record goes first, so that an old record never describes the new copy
	const std::wstring recordPath = std::wstring(g_copy.dest) + L".src";
	const std::wstring recordTmp = recordPath + L".tmp";
	DeleteFileW(recordPath.c_str());
	if (!MoveFileExW(g_copy.tmp, g_copy.dest, MOVEFILE_REPLACE_EXISTING))
	{
		SetError(error, errorsize, "Cannot rename the copy", GetLastError());
		DeleteFileW(g_copy.tmp);
		return false;
	}

	uwp_data_copy::Record record;
	record.size = g_copy.size;
	record.modified = g_copy.modified;
	record.md5 = g_copy.md5;
	if (!WriteSmallFile(recordPath.c_str(), recordTmp.c_str(), uwp_data_copy::FormatRecord(record), error, errorsize))
	{
		DeleteFileW(g_copy.dest); // not usable without its record
		return false;
	}
	return true;
}

void I_UWPDataCopyAbort(void)
{
	if (g_copy.file)
	{
		fclose(g_copy.file);
		g_copy.file = nullptr;
		DeleteFileW(g_copy.tmp);
	}
}

} // extern "C"

#endif // _UWP
