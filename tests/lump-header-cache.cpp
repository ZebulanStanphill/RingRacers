// The lump header cache's file format and lookups (src/lump_header_cache.hpp); runs without the engine.
#include "lump_header_cache.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace lump_header_cache;

#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			assert(false); \
			return 1; \
		} \
	} while (0)

static Key key(const char* name, uint32_t crc = 1, uint32_t size = 2, uint32_t disksize = 3, uint32_t offset = 0,
	uint32_t request = 8)
{
	Key k;
	k.name = name;
	k.crc32 = crc;
	k.size = size;
	k.disksize = disksize;
	k.offset = offset;
	k.request = request;
	return k;
}

static std::vector<uint8_t> bytes(std::initializer_list<int> l)
{
	std::vector<uint8_t> v;
	for (int b : l)
		v.push_back(static_cast<uint8_t>(b));
	return v;
}

static bool put(Cache& c, const Key& k, const std::vector<uint8_t>& v)
{
	return c.Put(k, v.data(), v.size());
}

static bool same(const Entry* e, const std::vector<uint8_t>& v)
{
	return e && e->data == v;
}

// A cache with a few reads, all used
static Cache sample()
{
	Cache c;
	put(c, key("sprites/a.png"), bytes({0x89, 'P', 'N', 'G', 13, 10, 26, 10}));
	put(c, key("sprites/b.lmp"), bytes({1, 2, 3, 4}));
	put(c, key("flats/empty"), {});
	put(c, key("sprites/a.png", 1, 2, 3, 16, 4), bytes({0, 0, 1, 0}));
	return c;
}

static Cache load(const std::vector<uint8_t>& file, bool& ok)
{
	Cache c;
	ok = c.Load(file.data(), file.size());
	return c;
}

static int testRoundTrip()
{
	std::vector<uint8_t> file = sample().Save();
	bool ok;
	Cache c = load(file, ok);
	CHECK(ok && c.Size() == 4);
	CHECK(!c.NeedsSave() || c.UsedCount() == 0); // nothing used yet
	CHECK(same(c.Find(key("sprites/a.png")), bytes({0x89, 'P', 'N', 'G', 13, 10, 26, 10})));
	CHECK(same(c.Find(key("sprites/b.lmp")), bytes({1, 2, 3, 4})));
	CHECK(same(c.Find(key("flats/empty")), {})); // an empty read is a hit, not a miss
	CHECK(same(c.Find(key("sprites/a.png", 1, 2, 3, 16, 4)), bytes({0, 0, 1, 0})));

	// Everything was used, so nothing changed and saving gives the same file
	CHECK(!c.NeedsSave());
	CHECK(c.Save() == file);
	return 0;
}

static int testKeyFields()
{
	Cache c = sample();
	CHECK(c.Find(key("sprites/b.lmp")));
	CHECK(!c.Find(key("sprites/B.lmp")));
	CHECK(!c.Find(key("sprites/b.lmp", 99)));
	CHECK(!c.Find(key("sprites/b.lmp", 1, 99)));
	CHECK(!c.Find(key("sprites/b.lmp", 1, 2, 99)));
	CHECK(!c.Find(key("sprites/b.lmp", 1, 2, 3, 1)));
	CHECK(!c.Find(key("sprites/b.lmp", 1, 2, 3, 0, 9)));
	CHECK(!c.Find(key("sprites/b.lmp", 1, 2, 3, 0, 0))); // whole-lump request differs from an 8 byte one
	CHECK(!c.Find(key("sprites/c.lmp")));

	// The same read with other bytes replaces the entry
	put(c, key("sprites/b.lmp"), bytes({9}));
	CHECK(c.Size() == 4 && same(c.Find(key("sprites/b.lmp")), bytes({9})));
	return 0;
}

static int testPutLimits()
{
	Cache c;
	std::vector<uint8_t> big(kMaxData + 1, 7);
	CHECK(!c.Put(key("big"), big.data(), big.size()));
	CHECK(c.Size() == 0 && !c.NeedsSave());
	big.pop_back();
	CHECK(c.Put(key("big"), big.data(), big.size()));
	CHECK(!c.Put(key("null"), nullptr, 1));
	CHECK(c.Size() == 1);

	// The largest entry survives a round trip
	const std::vector<uint8_t> file = c.Save();
	bool ok;
	Cache d = load(file, ok);
	CHECK(ok && same(d.Find(key("big")), big));
	return 0;
}

static int testRejectedFiles()
{
	const std::vector<uint8_t> file = sample().Save();
	Cache c;

	// A cache that held entries is emptied by a failed load
	c = sample();
	std::vector<uint8_t> bad = file;
	bad[0] ^= 1; // wrong magic
	CHECK(!c.Load(bad.data(), bad.size()));
	CHECK(c.Size() == 0 && !c.Find(key("sprites/b.lmp")) && !c.NeedsSave());

	// Another format version
	bad = file;
	bad[8] = 2;
	CHECK(!c.Load(bad.data(), bad.size()));
	CHECK(c.Size() == 0);

	// Trailing bytes
	bad = file;
	bad.push_back(0);
	CHECK(!c.Load(bad.data(), bad.size()));

	// A count larger than the file holds
	bad = file;
	bad[12] = 0xFF;
	bad[13] = 0xFF;
	CHECK(!c.Load(bad.data(), bad.size()));
	CHECK(c.Size() == 0);

	// A name length running past the end
	bad = file;
	bad[16] = 0xFF;
	bad[17] = 0xFF;
	CHECK(!c.Load(bad.data(), bad.size()));

	// A data length over the limit: the first entry's follows its name and five numbers
	const size_t first = 16 + 2 + std::strlen("flats/empty") + 20; // entries are sorted by name
	bad = file;
	bad[first + 1] = 0x7F;
	CHECK(!c.Load(bad.data(), bad.size()));

	// Empty and null input
	CHECK(!c.Load(nullptr, 0));
	CHECK(!c.Load(file.data(), 0));

	// And a good file still loads after all that
	CHECK(c.Load(file.data(), file.size()));
	CHECK(c.Size() == 4);
	return 0;
}

static int testTruncations()
{
	const std::vector<uint8_t> file = sample().Save();
	for (size_t n = 0; n < file.size(); n++)
	{
		// A copy of exactly n bytes, so the sanitizer sees any read past the end
		std::vector<uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
		Cache c;
		CHECK(!c.Load(cut.empty() ? nullptr : cut.data(), cut.size()));
		CHECK(c.Size() == 0);
	}
	return 0;
}

static int testUsedOnly()
{
	const std::vector<uint8_t> file = sample().Save();
	bool ok;
	Cache c = load(file, ok);
	CHECK(ok);

	// Nothing used yet: all four would be dropped
	CHECK(c.UsedCount() == 0 && c.NeedsSave());

	CHECK(c.Find(key("sprites/b.lmp")));
	CHECK(c.Find(key("flats/empty")));
	CHECK(c.UsedCount() == 2 && c.NeedsSave());

	std::vector<uint8_t> pruned = c.Save();
	Cache d = load(pruned, ok);
	CHECK(ok && d.Size() == 2);
	CHECK(d.Find(key("sprites/b.lmp")) && d.Find(key("flats/empty")));
	CHECK(!d.Find(key("sprites/a.png")) && !d.Find(key("sprites/a.png", 1, 2, 3, 16, 4)));
	CHECK(!d.NeedsSave());

	// A new entry needs a save even when every loaded one was used
	CHECK(put(d, key("new"), bytes({5})));
	CHECK(d.NeedsSave());

	// Using nothing saves a valid, empty file
	Cache e = load(file, ok);
	const std::vector<uint8_t> none = e.Save();
	CHECK(none.size() == 16);
	CHECK(d.Load(none.data(), none.size()) && d.Size() == 0);
	return 0;
}

static int testPointersStable()
{
	Cache c;
	put(c, key("first"), bytes({7, 7}));
	const Entry* first = c.Find(key("first"));
	const uint8_t* firstData = first->data.data();
	for (int i = 0; i < 500; i++)
		put(c, key("m", static_cast<uint32_t>(i)), bytes({i & 255}));
	CHECK(c.Find(key("first")) == first && first->data.data() == firstData && first->data == bytes({7, 7}));
	return 0;
}

static int testLongName()
{
	// A name of the longest length allowed (the engine never caches a longer one)
	Cache c;
	put(c, key(std::string(kMaxName, 'x').c_str()), bytes({1}));
	const std::vector<uint8_t> file = c.Save();
	bool ok;
	Cache d = load(file, ok);
	CHECK(ok && d.Size() == 1 && same(d.Find(key(std::string(kMaxName, 'x').c_str())), bytes({1})));
	return 0;
}

int main()
{
	if (testRoundTrip() || testKeyFields() || testPutLimits() || testRejectedFiles() || testTruncations() ||
		testUsedOnly() || testPointersStable() || testLongName())
		return 1;
	std::puts("lump_header_cache: ok");
	return 0;
}
