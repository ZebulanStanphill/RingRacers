// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_D3D12_UPLOAD_H
#define RR_D3D12_UPLOAD_H
#include "d3d12_core.h"
#include "allocators.h"
#include <string>
namespace rr_d3d12
{
struct UploadAllocation
{
	void *cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	size_t offset = 0;
	uint64_t generation = 0;
	explicit operator bool() const { return cpu != nullptr; }
};
struct RingStats
{
	size_t peakBytes = 0;
	unsigned wraps = 0, stalls = 0, grows = 0;
	double stallMs = 0;
};
class UploadRing
{
public:
	bool Init(size_t size, const wchar_t *name);
	UploadAllocation Alloc(size_t size, size_t alignment);
	// Call after all allocations for a draw: another ring may have submitted the list.
	// False means the allocation was invalidated by a wrap/grow and must be allocated again.
	bool Touch(const UploadAllocation &allocation, size_t size);
	void Shutdown();
	ID3D12Resource *Resource() const { return resource_.Get(); }
	size_t Capacity() const { return allocator_.Capacity(); }
	// Cached allocations must be discarded on generation changes; Touch retags each reuse.
	uint64_t Generation() const { return generation_; }
	void EndFrame() { frameBytes_ = 0; }
	RingStats TakeStats();
private:
	bool Create(size_t capacity);
	ComPtr<ID3D12Resource> resource_;
	unsigned char *mapped_ = nullptr;
	RingAllocator allocator_;
	std::wstring name_;
	uint64_t generation_ = 0;
	size_t frameBytes_ = 0;
	RingStats stats_;
};
}
#endif
