// The map preview cache's file format and lookups (src/map_preview_cache.hpp); runs without the engine.
#include "map_preview_cache.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace map_preview_cache;

static const uint32_t kParser = 0x01020304;

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

static bool keepAll(const Key&) { return true; }

static Key key(const char* name, uint32_t crc = 1, uint32_t size = 2, uint32_t disksize = 3)
{
	Key k;
	k.name = name;
	k.crc32 = crc;
	k.size = size;
	k.disksize = disksize;
	return k;
}

static std::vector<uint8_t> bytes(std::initializer_list<int> l)
{
	std::vector<uint8_t> v;
	for (int b : l)
		v.push_back(static_cast<uint8_t>(b));
	return v;
}

static MapEntry mapEntry(bool pic, bool mini, std::vector<uint8_t> p, std::vector<uint8_t> m)
{
	MapEntry e;
	e.hasPicture = pic;
	e.hasMinimap = mini;
	e.picture = std::move(p);
	e.minimap = std::move(m);
	return e;
}

static GhostEntry ghost(bool valid, uint32_t time, uint32_t lap, const char* name)
{
	GhostEntry g;
	g.valid = valid;
	g.time = time;
	g.lap = lap;
	g.name = name;
	return g;
}

// A cache with every kind of entry
static Cache sample()
{
	Cache c;
	c.PutMap(key("maps/a.pk3/MAPA1.wad"), mapEntry(true, true, bytes({1, 2, 3}), bytes({4, 5})));
	c.PutMap(key("maps/b.wad"), mapEntry(true, false, bytes({9}), {}));
	c.PutMap(key("maps/c.wad"), mapEntry(false, false, {}, {}));
	c.PutMap(key("maps/d.wad"), mapEntry(true, true, {}, bytes({0})));
	c.PutGhost(key("staffghosts/MAPA1/x.lmp"), ghost(true, 4000, 1234, "Sonic"));
	c.PutGhost(key("staffghosts/MAPA1/y.lmp"), ghost(false, 0, 0, ""));
	return c;
}

static int testRoundTrip()
{
	std::vector<uint8_t> file = sample().Save(kParser, keepAll);
	Cache c;
	CHECK(c.Load(file.data(), file.size(), kParser));
	CHECK(!c.Dirty());

	const MapEntry* m = c.FindMap(key("maps/a.pk3/MAPA1.wad"));
	CHECK(m && m->hasPicture && m->hasMinimap && m->picture == bytes({1, 2, 3}) && m->minimap == bytes({4, 5}));
	m = c.FindMap(key("maps/b.wad"));
	CHECK(m && m->hasPicture && !m->hasMinimap && m->picture == bytes({9}) && m->minimap.empty());
	m = c.FindMap(key("maps/c.wad"));
	CHECK(m && !m->hasPicture && !m->hasMinimap);
	m = c.FindMap(key("maps/d.wad"));
	CHECK(m && m->hasPicture && m->picture.empty() && m->hasMinimap && m->minimap == bytes({0}));

	const GhostEntry* g = c.FindGhost(key("staffghosts/MAPA1/x.lmp"));
	CHECK(g && g->valid && g->time == 4000 && g->lap == 1234 && g->name == "Sonic");
	g = c.FindGhost(key("staffghosts/MAPA1/y.lmp"));
	CHECK(g && !g->valid);

	// Saving what was loaded gives the same file
	CHECK(c.Save(kParser, keepAll) == file);
	return 0;
}

static int testKeyFields()
{
	Cache c = sample();
	CHECK(c.FindMap(key("maps/b.wad")));
	CHECK(!c.FindMap(key("maps/B.wad")));
	CHECK(!c.FindMap(key("maps/b.wad", 99)));
	CHECK(!c.FindMap(key("maps/b.wad", 1, 99)));
	CHECK(!c.FindMap(key("maps/b.wad", 1, 2, 99)));
	CHECK(!c.FindGhost(key("maps/b.wad"))); // maps and ghosts are separate
	CHECK(c.FindGhost(key("staffghosts/MAPA1/x.lmp")));
	CHECK(!c.FindGhost(key("staffghosts/MAPA1/x.lmp", 7)));
	CHECK(!c.FindGhost(key("staffghosts/MAPA1/x.lmp", 1, 7)));
	CHECK(!c.FindGhost(key("staffghosts/MAPA1/x.lmp", 1, 2, 7)));
	CHECK(!c.FindGhost(key("staffghosts/MAPA1/z.lmp")));
	return 0;
}

static int testRejectedFiles()
{
	const std::vector<uint8_t> file = sample().Save(kParser, keepAll);
	Cache c;

	// Another parser version
	CHECK(!c.Load(file.data(), file.size(), kParser + 1));
	CHECK(!c.FindMap(key("maps/b.wad")));

	// A cache that held entries is emptied by a failed load
	c = sample();
	CHECK(!c.Load(file.data(), file.size(), kParser + 1));
	CHECK(!c.FindMap(key("maps/b.wad")) && !c.FindGhost(key("staffghosts/MAPA1/x.lmp")));

	// Wrong magic
	std::vector<uint8_t> bad = file;
	bad[0] ^= 1;
	CHECK(!c.Load(bad.data(), bad.size(), kParser));
	CHECK(!c.FindMap(key("maps/b.wad")));

	// Another format version
	bad = file;
	bad[8] = 2;
	CHECK(!c.Load(bad.data(), bad.size(), kParser));

	// Trailing bytes
	bad = file;
	bad.push_back(0);
	CHECK(!c.Load(bad.data(), bad.size(), kParser));

	// A count larger than the file holds
	bad = file;
	bad[16] = 0xFF;
	bad[17] = 0xFF;
	CHECK(!c.Load(bad.data(), bad.size(), kParser));
	CHECK(!c.FindMap(key("maps/b.wad")));

	// A lump length running past the end: the first entry's picture length follows its key and flags
	bad = file;
	const size_t lenAt = 24 + 2 + std::strlen("maps/a.pk3/MAPA1.wad") + 12 + 1;
	bad[lenAt + 3] = 0x7F;
	CHECK(!c.Load(bad.data(), bad.size(), kParser));

	// A name length running past the end
	bad = file;
	bad[24] = 0xFF;
	bad[25] = 0xFF;
	CHECK(!c.Load(bad.data(), bad.size(), kParser));

	// Empty and null input
	CHECK(!c.Load(nullptr, 0, kParser));
	CHECK(!c.Load(file.data(), 0, kParser));

	// And a good file still loads after all that
	CHECK(c.Load(file.data(), file.size(), kParser));
	CHECK(c.FindMap(key("maps/b.wad")));
	return 0;
}

static int testTruncations()
{
	const std::vector<uint8_t> file = sample().Save(kParser, keepAll);
	for (size_t n = 0; n < file.size(); n++)
	{
		// A copy of exactly n bytes, so the sanitizer sees any read past the end
		std::vector<uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
		Cache c;
		CHECK(!c.Load(cut.empty() ? nullptr : cut.data(), cut.size(), kParser));
		CHECK(!c.FindMap(key("maps/b.wad")));
	}
	return 0;
}

static int testKeep()
{
	Cache c = sample();
	const std::vector<uint8_t> pruned = c.Save(kParser, [](const Key& k) { return k.name == "maps/b.wad" || k.name == "staffghosts/MAPA1/y.lmp"; });
	Cache d;
	CHECK(d.Load(pruned.data(), pruned.size(), kParser));
	CHECK(d.FindMap(key("maps/b.wad")));
	CHECK(!d.FindMap(key("maps/c.wad")) && !d.FindMap(key("maps/a.pk3/MAPA1.wad")));
	CHECK(d.FindGhost(key("staffghosts/MAPA1/y.lmp")));
	CHECK(!d.FindGhost(key("staffghosts/MAPA1/x.lmp")));

	// Keeping nothing is a valid, empty file
	const std::vector<uint8_t> none = c.Save(kParser, [](const Key&) { return false; });
	CHECK(d.Load(none.data(), none.size(), kParser));
	CHECK(!d.FindMap(key("maps/b.wad")));
	return 0;
}

static int testPointersAndDirty()
{
	Cache c;
	CHECK(!c.Dirty());
	c.PutMap(key("first"), mapEntry(true, false, bytes({7, 7}), {}));
	CHECK(c.Dirty());
	const MapEntry* first = c.FindMap(key("first"));
	const uint8_t* firstData = first->picture.data();
	c.PutGhost(key("g"), ghost(true, 1, 2, "n"));
	const GhostEntry* g = c.FindGhost(key("g"));
	for (int i = 0; i < 500; i++)
	{
		c.PutMap(key("m", static_cast<uint32_t>(i)), mapEntry(true, true, bytes({i & 255}), {}));
		c.PutGhost(key("h", static_cast<uint32_t>(i)), ghost(false, 0, 0, ""));
	}
	CHECK(c.FindMap(key("first")) == first && first->picture.data() == firstData && first->picture == bytes({7, 7}));
	CHECK(c.FindGhost(key("g")) == g && g->name == "n");

	// Replacing an entry keeps one of it
	c.PutMap(key("first"), mapEntry(false, false, {}, {}));
	CHECK(!c.FindMap(key("first"))->hasPicture);

	// Loading clears the dirty flag; Save does not set or clear it
	const std::vector<uint8_t> file = c.Save(kParser, keepAll);
	CHECK(c.Dirty());
	CHECK(c.Load(file.data(), file.size(), kParser));
	CHECK(!c.Dirty());
	c.Save(kParser, keepAll);
	CHECK(!c.Dirty());
	c.PutGhost(key("again"), ghost(false, 0, 0, ""));
	CHECK(c.Dirty());
	return 0;
}

int main()
{
	if (testRoundTrip() || testKeyFields() || testRejectedFiles() || testTruncations() || testKeep() || testPointersAndDirty())
		return 1;
	std::puts("map_preview_cache: ok");
	return 0;
}
