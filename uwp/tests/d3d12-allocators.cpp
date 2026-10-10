// Pure D3D12 bookkeeping regressions; runs without a graphics API or GPU.
#include "hardware/r_d3d12/allocators.h"
#include "hardware/r_d3d12/pipeline_key.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <unordered_set>

using rr_d3d12::RingAllocator;
using rr_d3d12::PipelineKey;
using rr_d3d12::PipelineKeyHash;

static void allocate(RingAllocator &ring, size_t bytes, size_t alignment,
                     uint64_t fence, size_t offset, bool wrapped = false)
{
    const auto plan = ring.Prepare(bytes, alignment);
    assert(plan.valid && plan.blockingFence == 0);
    assert(plan.offset == offset && plan.wrapped == wrapped);
    ring.Commit(plan, bytes, fence);
    assert(ring.Head() == offset + bytes);
}

static void ringTests()
{
    RingAllocator ring;
    assert(ring.Capacity() == 0 && ring.Head() == 0);
    assert(!ring.Prepare(1, 1).valid);
    ring.Reset(64);
    assert(ring.Capacity() == 64 && ring.Head() == 0);
    assert(!ring.Prepare(0, 1).valid);
    assert(!ring.Prepare(1, 0).valid);
    assert(!ring.Prepare(65, 1).valid);
    allocate(ring, 5, 1, 1, 0);
    allocate(ring, 7, 6, 1, 6); // Non-power-of-two alignment and padding.
    assert(ring.Head() == 13);
    auto plan = ring.Prepare(52, 1);
    assert(plan.valid && plan.wrapped && plan.offset == 0 && plan.blockingFence == 1);
    assert(ring.Head() == 13); // Prepare, including blocked plans, never advances.
    ring.Collect(0);
    assert(ring.Prepare(52, 1).blockingFence == 1);
    ring.Collect(1);
    allocate(ring, 52, 1, 2, 0, true);
    assert(!ring.Prepare(65, 1).valid && ring.Head() == 52);
    ring.Collect(2);
    allocate(ring, 12, 1, 3, 52);
    ring.Collect(3);
    allocate(ring, 64, 1, 4, 0, true); // Exact-capacity allocation.
    assert(ring.Prepare(1, 1).blockingFence == 4);

    ring.Reset(64); // Reset retires old spans as well as the head.
    allocate(ring, 16, 1, 10, 0);
    allocate(ring, 16, 1, 11, 16);
    allocate(ring, 16, 1, 12, 32);
    plan = ring.Prepare(40, 1);
    assert(plan.wrapped && plan.blockingFence == 12); // Highest overlapping fence.
    ring.Collect(11);
    assert(ring.Prepare(40, 1).blockingFence == 12);
    assert(ring.Prepare(16, 1).blockingFence == 0); // Adjacent ranges do not overlap.
    ring.Collect(12);
    allocate(ring, 40, 1, 13, 0, true);

    ring.Reset(64);
    allocate(ring, 48, 1, 20, 0);
    ring.Collect(20);
    allocate(ring, 8, 1, 21, 48);
    allocate(ring, 8, 16, 21, 0, true); // Same submission wraps into retired space.
    allocate(ring, 8, 1, 21, 8); // Must not be blocked by a merged [0,56) span.
    plan = ring.Prepare(40, 1);
    assert(plan.valid && plan.blockingFence == 21); // Real overlap still blocks.
    ring.Collect(21);
    assert(ring.Prepare(40, 1).blockingFence == 0);

    ring.Reset(64);
    ring.Touch(16, 8, 30);
    ring.Touch(32, 8, 30);
    assert(ring.Head() == 0); // Touch records use, without allocating.
    assert(ring.Prepare(16, 1).blockingFence == 0);
    assert(ring.Prepare(17, 1).blockingFence == 30);
    ring.Touch(8, 4, 31);
    assert(ring.Prepare(12, 1).blockingFence == 31);
    ring.Collect(30);
    assert(ring.Prepare(17, 1).blockingFence == 31);
    ring.Collect(31);
    allocate(ring, 17, 1, 32, 0);

    ring.Reset(0);
    assert(ring.Capacity() == 0 && ring.Head() == 0);
    assert(!ring.Prepare(1, 1).valid);

    ring.Reset(std::numeric_limits<size_t>::max());
    allocate(ring, std::numeric_limits<size_t>::max() - 1, 1, 40, 0);
    plan = ring.Prepare(1, std::numeric_limits<size_t>::max() - 1);
    assert(plan.valid && !plan.wrapped && plan.offset == std::numeric_limits<size_t>::max() - 1);
    // Alignment arithmetic must not overflow near SIZE_MAX.
    plan = ring.Prepare(2, std::numeric_limits<size_t>::max());
    assert(plan.valid && plan.wrapped && plan.offset == 0 && plan.blockingFence == 40);
}

static void keyTests()
{
    const PipelineKey base;
    const PipelineKeyHash hash;
    assert(base == PipelineKey{} && !(base != PipelineKey{}));
    assert(hash(base) == hash(PipelineKey{}));
    auto different = [&](const PipelineKey &key) {
        assert(key != base && base != key && !(key == base));
        assert(hash(key) != hash(base));
    };
    PipelineKey key = base;
#define CHECK_FIELD(field, value) key = base; key.field = value; different(key)
    CHECK_FIELD(vs, 0x8000);
    CHECK_FIELD(ps, 0x8000);
    CHECK_FIELD(layout, 0x80);
    CHECK_FIELD(topologyType, 0x80);
    CHECK_FIELD(rtFormat, 0x80);
    CHECK_FIELD(dsvFormat, 0x80);
    CHECK_FIELD(blend, 0x8000);
    CHECK_FIELD(raster, 0x80);
    CHECK_FIELD(depth, UINT64_C(1) << 63);
#undef CHECK_FIELD
    std::unordered_set<PipelineKey, PipelineKeyHash> keys;
    std::unordered_set<size_t> hashes;
    // Deterministic mixed sample includes high bits of every field. Hash collisions
    // are legal generally; this fixed regression sample guards accidental omission.
    for (uint32_t i = 0; i < 65536; ++i)
    {
        key.vs = static_cast<uint16_t>(i);
        key.ps = static_cast<uint16_t>(i * 40503u);
        key.layout = static_cast<uint8_t>(i >> 8);
        key.topologyType = static_cast<uint8_t>(i * 3u);
        key.rtFormat = static_cast<uint8_t>(i * 5u);
        key.dsvFormat = static_cast<uint8_t>(i * 7u);
        key.blend = static_cast<uint16_t>(i ^ 0xa5a5u);
        key.raster = static_cast<uint8_t>(i * 11u);
        key.depth = UINT64_C(0x9e3779b97f4a7c15) * i;
        const PipelineKey copy = key;
        assert(copy == key && hash(copy) == hash(key));
        assert(keys.insert(key).second);
        assert(!keys.insert(copy).second);
        assert(hashes.insert(hash(key)).second);
    }
    assert(keys.size() == 65536 && hashes.size() == 65536);
}

int main()
{
    ringTests();
    keyTests();
    std::cout << "D3D12 allocator and pipeline key regressions passed\n";
}
