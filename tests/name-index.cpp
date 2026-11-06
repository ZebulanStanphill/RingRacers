// The hash indexes behind the startup name lookups (src/name_index.hpp); runs without the engine.
// Each is checked against a straightforward linear reference of the engine scan it replaces.
#include "name_index.hpp"
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	return 0;
}

int main()
{
	if (testHashPositions())
		return 1;
	std::puts("name_index: ok");
	return 0;
}
