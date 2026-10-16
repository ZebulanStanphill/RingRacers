// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_D3D12_ALLOCATORS_H
#define RR_D3D12_ALLOCATORS_H
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>
#include <utility>

namespace rr_d3d12
{
// GPU-independent bookkeeping. Failed plans leave the head unchanged. Ranges are tagged
// at allocation time with the fence of the list that will consume them.
class RingAllocator
{
public:
	struct Plan { size_t offset = 0; uint64_t blockingFence = 0; bool wrapped = false, valid = false; };
	void Reset(size_t capacity) { capacity_ = capacity; head_ = 0; spans_.clear(); }
	void Collect(uint64_t completed)
	{
		while (!spans_.empty() && spans_.front().fence <= completed) spans_.pop_front();
	}
	Plan Prepare(size_t bytes, size_t alignment) const
	{
		Plan result;
		if (!bytes || !alignment || bytes > capacity_) return result;
		const size_t padding = (alignment - head_ % alignment) % alignment;
		result.wrapped = padding > capacity_ - head_ || bytes > capacity_ - head_ - padding;
		result.offset = result.wrapped ? 0 : head_ + padding;
		result.valid = true;
		const size_t end = result.offset + bytes;
		for (const auto &span : spans_)
			if (result.offset < span.end && span.begin < end && span.fence > result.blockingFence)
				result.blockingFence = span.fence;
		return result;
	}
	void Commit(const Plan &plan, size_t bytes, uint64_t fence)
	{
		// After a wrap, a merged span would cover the whole ring and block the rest of this submission on itself
		if (plan.wrapped)
			spans_.push_back({plan.offset, plan.offset + bytes, fence});
		else
			Touch(plan.offset, bytes, fence);
		head_ = plan.offset + bytes;
	}
	void Touch(size_t offset, size_t bytes, uint64_t fence)
	{
		// One conservative range per submission keeps overlap searches proportional to
		// frames in flight, rather than draw count. Fence values arrive in queue order.
		if (!spans_.empty() && spans_.back().fence == fence)
		{
			auto &span = spans_.back();
			if (offset < span.begin) span.begin = offset;
			if (offset + bytes > span.end) span.end = offset + bytes;
		}
		else spans_.push_back({offset, offset + bytes, fence});
	}
	size_t Capacity() const { return capacity_; }
	size_t Head() const { return head_; }
private:
	struct Span { size_t begin, end; uint64_t fence; };
	size_t capacity_ = 0, head_ = 0;
	std::deque<Span> spans_;
};

// Hands out indices from [first, first + count). A freed index is held until its fence completes,
// so the GPU never sees it reused. Frees may come in any fence order.
class IndexFreeList
{
public:
	static constexpr uint32_t Exhausted = UINT32_MAX;
	void Reset(uint32_t first, uint32_t count)
	{
		first_ = first;
		count_ = count;
		next_ = first;
		free_.clear();
		pending_.clear();
	}
	uint32_t Allocate()
	{
		if (!free_.empty())
		{
			const uint32_t index = free_.back();
			free_.pop_back();
			return index;
		}
		if (next_ - first_ >= count_) return Exhausted;
		return next_++;
	}
	// The index must come from Allocate() and be freed once. Indices outside the range are ignored.
	void Free(uint32_t index, uint64_t fence)
	{
		if (index - first_ >= count_) return;
		pending_.emplace(fence, index);
	}
	void Collect(uint64_t completed)
	{
		const auto end = pending_.upper_bound(completed);
		for (auto it = pending_.begin(); it != end; ++it) free_.push_back(it->second);
		pending_.erase(pending_.begin(), end);
	}
	// Live indices plus those waiting on a fence
	uint32_t Used() const { return (next_ - first_) - static_cast<uint32_t>(free_.size()); }
private:
	uint32_t first_ = 0, count_ = 0, next_ = 0;
	std::vector<uint32_t> free_;
	std::multimap<uint64_t, uint32_t> pending_;
};
// Chunks are bump-carved into power-of-two blocks, or opt-in quarter-octave classes. The GPU owner creates
// backing storage before AddChunk/AddDedicated, and keeps chunk IDs stable.
class SizeClassArena
{
public:
	static constexpr size_t MaxClass = 4 * 1024 * 1024;
	static constexpr uint32_t InvalidChunk = UINT32_MAX;
	struct Block {
		uint32_t chunk = InvalidChunk;
		size_t offset = 0, size = 0, alignment = 0;
		bool dedicated = false, needsChunk = false, valid = false;
	};
	void Reset(size_t minClass, size_t chunkBytes, size_t maxChunks = SIZE_MAX, bool quarterClasses = false)
	{
		minClass_ = minClass; chunkBytes_ = chunkBytes; maxChunks_ = maxChunks; quarterClasses_ = quarterClasses;
		chunks_.clear(); pending_.clear(); free_.clear(); reuses_ = 0;
		configured_ = minClass && !(minClass & (minClass - 1)) && minClass <= MaxClass
				&& chunkBytes >= MaxClass && chunkBytes % MaxClass == 0;
	}
	Block Allocate(size_t bytes, size_t alignment = 0)
	{
		Block b;
		if (!configured_ || !bytes) return b;
		b.size = bytes; b.dedicated = bytes > MaxClass;
		if (!b.dedicated) {
			b.size = minClass_;
			if (quarterClasses_) {
				if (!alignment) alignment = minClass_;
				if (alignment < minClass_ || alignment > MaxClass || (alignment & (alignment - 1))) return Block{};
				// First round to a quarter of the containing power-of-two interval,
				// then to placement alignment. All arithmetic is bounded by MaxClass.
				while (b.size <= bytes / 2) b.size *= 2;
				if (b.size < bytes) {
					const size_t step = b.size / 4 ? b.size / 4 : 1;
					b.size += ((bytes - b.size + step - 1) / step) * step;
				}
				b.size = ((b.size + alignment - 1) / alignment) * alignment;
				b.alignment = alignment;
			}
			else while (b.size < bytes) b.size *= 2;
			auto &free = free_[{b.size, b.alignment}];
			if (!free.empty()) { b = free.back(); free.pop_back(); ++reuses_; return b; }
			for (size_t i = 0; i < chunks_.size(); ++i) {
				auto &c = chunks_[i];
				if (c.dedicated) continue;
				// A fresh chunk adopts one alignment. Its classes are multiples of
				// that alignment, so fine-class bump allocation needs no padding.
				if (quarterClasses_ && c.alignment && c.alignment != b.alignment) continue;
				const size_t padding = quarterClasses_ ? 0 : (b.size - c.head % b.size) % b.size;
				if (padding > chunkBytes_ - c.head || b.size > chunkBytes_ - c.head - padding) continue;
				b.chunk = static_cast<uint32_t>(i); b.offset = c.head + padding;
				c.head = b.offset + b.size; c.alignment = b.alignment; b.valid = true; return b;
			}
		}
		b.needsChunk = CanGrow(); return b;
	}
	bool AddChunk()
	{
		if (!configured_ || !CanGrow()) return false;
		chunks_.push_back(Chunk{}); return true;
	}
	Block AddDedicated(size_t bytes)
	{
		Block b;
		if (!configured_ || bytes <= MaxClass || !CanGrow()) return b;
		Chunk c; c.blockSize = bytes; c.dedicated = true;
		b.chunk = static_cast<uint32_t>(chunks_.size()); b.size = bytes;
		b.dedicated = b.valid = true; chunks_.push_back(std::move(c)); return b;
	}
	// As with IndexFreeList, only live blocks returned by this arena may be freed,
	// once. Backing resources for dedicated blocks can retire at this same fence.
	void Free(const Block &b, uint64_t fence) { if (b.valid) pending_.emplace(fence, b); }
	void Collect(uint64_t completed)
	{
		const auto end = pending_.upper_bound(completed);
		for (auto it = pending_.begin(); it != end; ++it) {
			const auto &b = it->second;
			if (!b.dedicated) free_[{b.size, b.alignment}].push_back(b);
		}
		pending_.erase(pending_.begin(), end);
	}
	size_t Reuses() const { return reuses_; }
	size_t ChunkCount() const { return chunks_.size(); }
	size_t ChunkBytes() const { return chunkBytes_; }
private:
	struct Chunk { size_t blockSize = 0, head = 0, alignment = 0; bool dedicated = false; };
	bool CanGrow() const { return chunks_.size() < maxChunks_ && chunks_.size() < InvalidChunk; }
	size_t reuses_ = 0;
	size_t minClass_ = 0, chunkBytes_ = 0, maxChunks_ = 0;
	bool configured_ = false, quarterClasses_ = false;
	std::map<std::pair<size_t, size_t>, std::vector<Block>> free_;
	std::vector<Chunk> chunks_;
	std::multimap<uint64_t, Block> pending_;
};

}
#endif
