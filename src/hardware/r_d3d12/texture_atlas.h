#ifndef RR_TEXTURE_ATLAS_H
#define RR_TEXTURE_ATLAS_H
#include <algorithm>
#include <cstdint>
#include <vector>

// One page, with 16-pixel shelf classes and four-pixel horizontal granularity.
// Pending rectangles still count as occupied until Collect observes their fence.
class TextureAtlas
{
public:
	static constexpr unsigned Size = 2048;
	struct Rect { unsigned x = 0, y = 0, w = 0, h = 0; explicit operator bool() const { return w != 0; } };
	Rect Allocate(unsigned w, unsigned h)
	{
		if (!w || !h || w > 256 || h > 256) return {};
		w = (w + 3) & ~3u; h = (h + 15) & ~15u;
		for (auto &s : shelves) if (s.h == h)
			for (auto &r : s.free) if (r.w >= w)
			{
				Rect result{r.x, s.y, w, h}; r.x += w; r.w -= w;
				++live; area += w * h; return result;
			}
		unsigned y = shelves.empty() ? 0 : shelves.back().y + shelves.back().h;
		if (y + h > Size) return {};
		shelves.push_back({y, h, {{w, y, Size - w, h}}});
		++live; area += w * h; return {0, y, w, h};
	}
	void Free(Rect r, uint64_t fence) { if (r) pending.push_back({r, fence}); }
	void Collect(uint64_t fence)
	{
		for (auto it = pending.begin(); it != pending.end();)
		{
			if (it->fence > fence) { ++it; continue; }
			const Rect r = it->rect;
			for (auto &s : shelves) if (s.y == r.y)
			{
				s.free.push_back(r);
				std::sort(s.free.begin(), s.free.end(), [](Rect a, Rect b) { return a.x < b.x; });
				for (size_t i = 1; i < s.free.size();)
					if (s.free[i-1].x + s.free[i-1].w == s.free[i].x)
					{ s.free[i-1].w += s.free[i].w; s.free.erase(s.free.begin() + i); }
					else ++i;
				break;
			}
			--live; area -= r.w * r.h; it = pending.erase(it);
		}
		if (!live) shelves.clear();
	}
	unsigned UsedArea() const { return area; }
private:
	struct Shelf { unsigned y, h; std::vector<Rect> free; };
	struct Pending { Rect rect; uint64_t fence; };
	std::vector<Shelf> shelves;
	std::vector<Pending> pending;
	unsigned live = 0, area = 0;
};
#endif
