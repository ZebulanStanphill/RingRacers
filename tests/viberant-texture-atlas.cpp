#include "hardware/r_d3d12/texture_atlas.h"
#include <cassert>
#include <vector>
int main()
{
	TextureAtlas a;
	assert(!a.Allocate(257, 1)); assert(!a.Allocate(0, 4));
	auto tiny = a.Allocate(1, 1);
	assert(tiny.w == 4 && tiny.h == 16);
	a.Free(tiny, 4); a.Collect(3);
	assert(a.UsedArea() == 64 && !a.Empty()); // still waiting for its fence
	a.Collect(4); assert(!a.UsedArea() && a.Empty());
	std::vector<TextureAtlas::Rect> rects;
	for (int i = 0; i < 64; ++i) { auto r = a.Allocate(256, 256); assert(r); rects.push_back(r); }
	assert(!a.Allocate(1, 1));
	a.Free(rects[17], 10); a.Collect(9); assert(!a.Allocate(256, 256));
	a.Collect(10); auto reused = a.Allocate(256, 256);
	assert(reused.x == rects[17].x && reused.y == rects[17].y);
	for (size_t i = 0; i < rects.size(); ++i) a.Free(i == 17 ? reused : rects[i], 11);
	a.Collect(11); assert(!a.UsedArea());
	// Empty pages discard old shelf classes, and adjacent frees coalesce.
	auto first = a.Allocate(3, 17), second = a.Allocate(7, 17);
	a.Free(first, 12); a.Free(second, 12); a.Collect(12);
	assert(a.Allocate(256, 256));
	TextureAtlas b;
	auto left = b.Allocate(4, 16), right = b.Allocate(8, 16);
	assert(b.Allocate(256, 16));
	b.Free(left, 1); b.Free(right, 1); b.Collect(1);
	auto merged = b.Allocate(12, 16); assert(merged.x == 0 && merged.y == 0);
}
