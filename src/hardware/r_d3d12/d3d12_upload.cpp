// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#include "d3d12_upload.h"
#include <limits>
namespace rr_d3d12
{
bool UploadRing::Create(size_t capacity)
{
	if (!capacity || !Core_Device()) return false;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = capacity;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> replacement;
	if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&replacement)), "upload ring resource")) return false;
	replacement->SetName(name_.c_str());
	void *mapped = nullptr;
	const D3D12_RANGE noRead = {0, 0};
	if (Core_Failed(replacement->Map(0, &noRead, &mapped), "upload ring map")) return false;
	// Keep old storage mapped until its last GPU use retires; destroying the resource unmaps it.
	Core_Retire(resource_);
	resource_ = replacement;
	mapped_ = static_cast<unsigned char *>(mapped);
	allocator_.Reset(capacity);
	++generation_;
	return true;
}
bool UploadRing::Init(size_t size, const wchar_t *name)
{
	Shutdown();
	name_ = name ? name : L"upload ring";
	stats_ = {};
	frameBytes_ = 0;
	return Create(size);
}
void UploadRing::Shutdown()
{
	Core_Retire(resource_);
	mapped_ = nullptr;
	allocator_.Reset(0);
	++generation_;
	frameBytes_ = 0;
}
UploadAllocation UploadRing::Alloc(size_t size, size_t alignment)
{
	if (!mapped_ || !size || !alignment || Core_RemovalSeen()) return {};
	if (size > Capacity())
	{
		size_t capacity = Capacity();
		while (capacity < size)
		{
			if (capacity > std::numeric_limits<size_t>::max() / 2) { capacity = size; break; }
			capacity *= 2;
		}
		if (!Create(capacity)) return {};
		++stats_.grows;
		std::string label;
		for (const wchar_t ch : name_) label += ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '?';
		Core_Log("growing the %s to %llu bytes", label.c_str(), static_cast<unsigned long long>(capacity));
	}
	allocator_.Collect(Core_CompletedFence());
	auto plan = allocator_.Prepare(size, alignment);
	if (!plan.valid) return {};
	if (plan.blockingFence)
	{
		// A current-list range cannot be waited on before its submission signals it.
		if (plan.blockingFence >= Core_PendingFence())
		{
			if (!Core_Submit("ring")) return {};
		}
		double ms = 0;
		++stats_.stalls;
		const bool done = Core_Wait(plan.blockingFence, 500, "upload ring", &ms);
		stats_.stallMs += ms;
		if (!done) return {};
		allocator_.Collect(Core_CompletedFence());
		plan = allocator_.Prepare(size, alignment);
		if (!plan.valid || plan.blockingFence) return {};
	}
	const size_t padding = plan.wrapped ? 0 : plan.offset - allocator_.Head();
	allocator_.Commit(plan, size, Core_PendingFence());
	if (plan.wrapped) { ++stats_.wraps; ++generation_; }
	frameBytes_ += size + padding;
	if (frameBytes_ > stats_.peakBytes) stats_.peakBytes = frameBytes_;
	return {mapped_ + plan.offset, resource_->GetGPUVirtualAddress() + plan.offset, plan.offset, generation_};
}
bool UploadRing::Touch(const UploadAllocation &allocation, size_t size)
{
	if (!allocation || allocation.generation != generation_ || !size ||
		allocation.offset > Capacity() || size > Capacity() - allocation.offset || Core_RemovalSeen()) return false;
	// The most recent consumer determines retirement, including cached constant reuses.
	allocator_.Touch(allocation.offset, size, Core_PendingFence());
	return true;
}
RingStats UploadRing::TakeStats()
{
	const RingStats result = stats_;
	stats_ = {};
	return result;
}
}
