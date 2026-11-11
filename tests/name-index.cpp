// The hash indexes behind the startup name lookups (src/name_index.hpp); runs without the engine.
// Each is checked against a straightforward linear reference of the engine scan it replaces.
#include "name_index.hpp"
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <strings.h>
#include <vector>

using namespace name_index;

#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			assert(false); \
			std::abort(); \
		} \
	} while (0)

static std::mt19937 rng(12345);

static uint32_t roll(uint32_t n) { return rng() % n; }

// The engine's quickncasehash (doomdef.h).
static uint32_t quickncasehash(const char* p, size_t n)
{
	size_t i = 0;
	uint32_t x = 5381;
	while (i < n && p[i])
	{
		x = (x * 33) ^ (uint32_t)tolower((unsigned char)p[i]);
		i++;
	}
	return x;
}

// A name from a tiny alphabet, so that equal and case-different names are common.
static std::string randomName(size_t maxlen)
{
	static const char alphabet[] = "abAB_1\x80\xe9";
	std::string s;
	size_t len = roll(maxlen + 1);
	for (size_t i = 0; i < len; i++)
		s += alphabet[roll(sizeof alphabet - 1)];
	return s;
}

static std::string randomCase(std::string s)
{
	for (char& c : s)
		if (roll(2))
			c = (char)((c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c);
	return s;
}

struct Entry
{
	char name[9]; // like lumpinfo_t::name: at most 8 chars plus NUL
	uint32_t hash; // may differ from the hash of name
};

// mode 0: the real hash; 1: a tiny hash (forced collisions)
static uint32_t hashFor(const std::string& name, int mode)
{
	if (mode == 1)
		return quickncasehash(name.c_str(), 8) % 3;
	return quickncasehash(name.c_str(), 8);
}

static int testHashPositions()
{
	// Direct behavior
	{
		HashPositions h;
		CHECK(h.FirstFrom(1, 0, [](uint32_t) { return true; }) == -1);
		CHECK(h.LastBelow(1, 10, [](uint32_t) { return true; }) == -1);
		h.Add(7, 2);
		h.Add(7, 5);
		h.Add(9, 3);
		h.Add(7, 8);
		auto any = [](uint32_t) { return true; };
		CHECK(h.FirstFrom(7, 0, any) == 2);
		CHECK(h.FirstFrom(7, 2, any) == 2);
		CHECK(h.FirstFrom(7, 3, any) == 5);
		CHECK(h.FirstFrom(7, 8, any) == 8);
		CHECK(h.FirstFrom(7, 9, any) == -1);
		CHECK(h.FirstFrom(9, 4, any) == -1);
		CHECK(h.LastBelow(7, 9, any) == 8);
		CHECK(h.LastBelow(7, 8, any) == 5);
		CHECK(h.LastBelow(7, 3, any) == 2);
		CHECK(h.LastBelow(7, 2, any) == -1);
		CHECK(h.FirstFrom(7, 0, [](uint32_t p) { return p == 5; }) == 5);
		CHECK(h.LastBelow(7, 9, [](uint32_t p) { return p == 5; }) == 5);
		CHECK(h.FirstFrom(7, 6, [](uint32_t p) { return p == 5; }) == -1);
		h.Clear();
		CHECK(h.FirstFrom(7, 0, any) == -1);
	}

	// Differential: W_CheckNumForNamePwad (forward from startlump) and R_CheckTextureNumForName (backward)
	for (int round = 0; round < 3000; round++)
	{
		int mode = round % 3; // 0 real hash, 1 tiny hash, 2 real hash but some entries hashed from another name (WADNAME lumps)
		size_t count = roll(40);
		std::vector<Entry> entries(count);
		HashPositions index;
		for (size_t i = 0; i < count; i++)
		{
			std::string n = randomName(8);
			std::memset(entries[i].name, 0, sizeof entries[i].name);
			std::memcpy(entries[i].name, n.data(), n.size());
			if (mode == 2 && roll(3) == 0)
				entries[i].hash = quickncasehash(randomName(8).c_str(), 8); // hash not of this name
			else
				entries[i].hash = hashFor(n, mode == 1 ? 1 : 0);
			index.Add(entries[i].hash, (uint32_t)i);
		}

		for (int q = 0; q < 40; q++)
		{
			std::string query;
			if (count && roll(2))
			{
				query = entries[roll((uint32_t)count)].name;
				for (char& c : query)
					if (roll(2))
						c = (char)((c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c);
				if (roll(3) == 0)
					query += randomName(4); // longer than 8: only the first 8 chars count
			}
			else
				query = randomName(12);
			const char* name = query.c_str();
			uint32_t qhash = hashFor(query, mode == 1 ? 1 : 0);
			uint32_t start = roll((uint32_t)count + 2);
			uint32_t end = roll((uint32_t)count + 1);

			// Reference: the original forward scan (W_CheckNumForNamePwad)
			int64_t wantF = -1;
			for (uint32_t i = start; i < count; i++)
			{
				if (entries[i].hash != qhash)
					continue;
				if (strncasecmp(entries[i].name, name, 8))
					continue;
				wantF = i;
				break;
			}
			int64_t gotF = start < count
				? index.FirstFrom(qhash, start, [&](uint32_t p) { return !strncasecmp(entries[p].name, name, 8); })
				: -1;
			CHECK(gotF == wantF);

			// Reference: the original backward scan (R_CheckTextureNumForName)
			int64_t wantL = -1;
			for (int64_t i = (int64_t)end - 1; i >= 0; i--)
			{
				if (entries[i].hash != qhash)
					continue;
				if (strncasecmp(entries[i].name, name, 8))
					continue;
				wantL = i;
				break;
			}
			int64_t gotL = index.LastBelow(qhash, end, [&](uint32_t p) { return !strncasecmp(entries[p].name, name, 8); });
			CHECK(gotL == wantL);
		}
	}

	// Differential: W_CheckNumForLongNamePwad. The stored hash covers only the first 8 chars, but the match is a
	// full-string strcasecmp, so names longer than 8 that share a prefix are told apart by the predicate alone.
	for (int round = 0; round < 3000; round++)
	{
		int mode = round % 2; // 0 real hash, 1 tiny hash (forced collisions)
		size_t count = roll(40);
		std::vector<std::string> longnames(count);
		std::vector<uint32_t> hashes(count);
		HashPositions index;
		for (size_t i = 0; i < count; i++)
		{
			std::string n = randomName(8);
			if (roll(2))
				n += randomName(6); // longer than 8, often with an equal 8-char prefix
			longnames[i] = n;
			hashes[i] = hashFor(n, mode);
			index.Add(hashes[i], (uint32_t)i);
		}
		for (int q = 0; q < 40; q++)
		{
			std::string query;
			if (count && roll(2))
			{
				query = randomCase(longnames[roll((uint32_t)count)]);
				if (roll(3) == 0)
					query += randomName(2);
			}
			else
				query = randomName(14);
			uint32_t qhash = hashFor(query, mode);
			uint32_t start = roll((uint32_t)count + 2);

			int64_t want = -1;
			for (uint32_t i = start; i < count; i++)
			{
				if (hashes[i] != qhash)
					continue;
				if (strcasecmp(longnames[i].c_str(), query.c_str()))
					continue;
				want = i;
				break;
			}
			int64_t got = start < count
				? index.FirstFrom(qhash, start, [&](uint32_t p) { return !strcasecmp(longnames[p].c_str(), query.c_str()); })
				: -1;
			CHECK(got == want);
		}
	}
	return 0;
}

// The original W_CheckNumForFolderStartPK3 with startlump 0
static uint32_t linearFolderStart(const std::vector<std::string>& names, const char* name)
{
	size_t name_length = std::strlen(name);
	uint32_t i;
	for (i = 0; i < names.size(); i++)
	{
		if (strncasecmp(name, names[i].c_str(), name_length) == 0)
		{
			if (names[i].size() == name_length)
				i++;
			break;
		}
	}
	return i;
}

static int testFolderStarts()
{
	static const char* const segments[] = {
		"maps", "Maps", "MAPS", "map", "mapsx", "staffghosts", "StaffGhosts", "a", "A", "\xe9", "\xc9", "lua", "x.y", "",
	};
	const size_t nsegments = sizeof segments / sizeof segments[0];

	// Direct behavior: SLADE directory entry, nested folders, a prefix that is not a directory
	{
		FolderStarts f;
		f.Add("Maps/", 0);
		f.Add("Maps/MAP01.wad", 1);
		f.Add("mapsx/y", 2);
		f.Add("top", 3);
		CHECK(f.First("maps/") == std::optional<uint32_t>(0));
		CHECK(f.First("MAPS/") == std::optional<uint32_t>(0));
		CHECK(f.First("mapsx/") == std::optional<uint32_t>(2));
		CHECK(!f.First("map/"));
		CHECK(!f.First("top/"));
	}

	for (int round = 0; round < 3000; round++)
	{
		size_t count = roll(30);
		std::vector<std::string> names;
		FolderStarts index;
		for (size_t i = 0; i < count; i++)
		{
			std::string n;
			uint32_t depth = roll(4);
			for (uint32_t d = 0; d < depth; d++)
				n += std::string(segments[roll((uint32_t)nsegments)]) + "/";
			if (roll(3))
				n += std::string(segments[roll((uint32_t)nsegments)]) + (roll(2) ? ".lmp" : "");
			names.push_back(n);
			index.Add(n.c_str(), (uint32_t)i);
		}

		for (int q = 0; q < 40; q++)
		{
			std::string query;
			uint32_t kind = roll(3);
			if (kind == 0 && count)
			{
				// A directory prefix of an existing name, in other case
				const std::string& base = names[roll((uint32_t)count)];
				std::vector<size_t> slashes;
				for (size_t k = 0; k < base.size(); k++)
					if (base[k] == '/')
						slashes.push_back(k);
				if (slashes.empty())
					continue;
				query = randomCase(base.substr(0, slashes[roll((uint32_t)slashes.size())] + 1));
			}
			else
			{
				uint32_t depth = 1 + roll(3);
				for (uint32_t d = 0; d < depth; d++)
					query += randomCase(std::string(segments[roll((uint32_t)nsegments)])) + "/";
			}
			uint32_t want = linearFolderStart(names, query.c_str());
			std::optional<uint32_t> got = index.First(query.c_str());
			// What the engine hook returns
			uint32_t hook = (uint32_t)names.size();
			if (got)
			{
				hook = *got;
				if (names[hook].size() == query.size())
					hook++;
			}
			CHECK(hook == want);
		}
	}
	return 0;
}

// A copy of R_TextureCacheKey (r_textures.cpp): the texture-id memo's key
static std::string textureCacheKey(const char* name)
{
	std::string key;
	for (int i = 0; i < 8 && name[i]; i++)
	{
		char c = name[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		key += c;
	}
	return key;
}

// The memo keyed by textureCacheKey must hit exactly when the old array scan's strncasecmp over 8 chars did
static int testTextureMemoKey()
{
	for (int round = 0; round < 200000; round++)
	{
		std::string a = randomName(11);
		std::string b = roll(2) ? randomCase(a) : randomName(11);
		if (roll(4) == 0 && a.size() > 8)
			b = randomCase(a.substr(0, 8)) + randomName(3);
		bool scan = strncasecmp(a.c_str(), b.c_str(), 8) == 0;
		bool key = textureCacheKey(a.c_str()) == textureCacheKey(b.c_str());
		CHECK(scan == key);
	}
	return 0;
}

int main()
{
	if (testHashPositions() || testFolderStarts() || testTextureMemoKey())
		return 1;
	std::puts("name_index: ok");
	return 0;
}
