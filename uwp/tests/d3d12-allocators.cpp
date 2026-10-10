// Pure D3D12 bookkeeping regressions; runs without a graphics API or GPU.
#include "hardware/r_d3d12/allocators.h"
#include "hardware/r_d3d12/pipeline_key.h"
#include "hardware/r_d3d12/mips.h"
#include "hardware/r_d3d12/texture_repack.h"
#include <cmath>
#include <random>
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

static void arenaTests()
{
 using Arena = rr_d3d12::SizeClassArena;
 Arena arena;
 assert(!arena.Allocate(1).valid);
 arena.Reset(256, Arena::MaxClass, 2);
 assert(!arena.Allocate(0).needsChunk);
 assert(arena.Allocate(257).needsChunk && arena.Allocate(257).size == 512);
 assert(arena.AddChunk());
 auto a = arena.Allocate(1), b = arena.Allocate(257);
 assert(a.valid && a.size == 256 && a.offset == 0);
 assert(b.valid && b.size == 512 && b.offset == 512 && b.chunk == a.chunk);
 arena.Free(a, 8); arena.Free(b, 3); arena.Collect(2);
 auto c = arena.Allocate(257); assert(c.offset != b.offset);
 arena.Collect(3); c = arena.Allocate(257); assert(c.offset == b.offset);
 c = arena.Allocate(1); assert(c.offset != a.offset);
 arena.Collect(8); c = arena.Allocate(1); assert(c.offset == a.offset);
 assert(arena.Allocate(Arena::MaxClass).needsChunk);
 assert(arena.AddChunk());
 c = arena.Allocate(Arena::MaxClass); assert(c.valid && c.chunk == 1 && c.offset == 0);
 assert(!arena.Allocate(Arena::MaxClass).needsChunk && !arena.AddChunk());
 arena.Free(c, 99); arena.Collect(98); assert(!arena.Allocate(Arena::MaxClass).valid);
 arena.Collect(99); assert(arena.Allocate(Arena::MaxClass).valid);
 arena.Reset(4096, 16 * 1024 * 1024);
 assert(arena.AddChunk());
 for (size_t size = 4096; size <= Arena::MaxClass; size *= 2) {
  auto block = arena.Allocate(size - 1);
  assert(block.valid && block.size == size && block.offset % size == 0 && block.chunk == 0);
 }
 auto request = arena.Allocate(Arena::MaxClass + 1);
 assert(!request.valid && request.dedicated && request.needsChunk);
 auto dedicated = arena.AddDedicated(request.size);
 assert(dedicated.valid && dedicated.dedicated && dedicated.chunk == 1);
 assert(!arena.AddDedicated(Arena::MaxClass).valid);
 // Seeded allocation oracle tracks byte ranges independently of arena metadata.
 arena.Reset(256, Arena::MaxClass, 8);
 std::vector<Arena::Block> live;
 std::vector<std::pair<uint64_t, Arena::Block>> pending;
 std::mt19937 random(0xabc123);
 for (uint64_t frame = 1; frame <= 500; ++frame) {
  const uint64_t completed = frame > 3 ? frame - 3 : 0;
  arena.Collect(completed);
  pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const auto &entry) { return entry.first <= completed; }), pending.end());
  for (unsigned n = 0; n < 10; ++n) {
   const size_t bytes = 1 + random() % 65536;
   auto block = arena.Allocate(bytes);
   if (block.needsChunk) { assert(arena.AddChunk()); block = arena.Allocate(bytes); }
   if (!block.valid) continue;
   assert(block.size >= bytes && !(block.size & (block.size - 1)));
   assert(block.offset % block.size == 0 && block.offset + block.size <= arena.ChunkBytes());
   auto disjoint = [&](const Arena::Block &other) {
    assert(block.chunk != other.chunk || block.offset + block.size <= other.offset || other.offset + other.size <= block.offset);
   };
   for (const auto &other : live) disjoint(other);
   for (const auto &other : pending) disjoint(other.second);
   live.push_back(block);
  }
  for (unsigned n = 0; n < 9 && !live.empty(); ++n) {
   size_t index = random() % live.size();
   const uint64_t fence = frame + random() % 4;
   arena.Free(live[index], fence); pending.emplace_back(fence, live[index]);
   live.erase(live.begin() + index);
  }
 }
 arena.Reset(3, Arena::MaxClass); assert(!arena.AddChunk() && !arena.Allocate(1).needsChunk);
 arena.Reset(256, 123); assert(!arena.AddChunk());
}

static void mipTests()
{
 using rr_d3d12::GenerateMipChainRGBA8;
 assert(GenerateMipChainRGBA8(nullptr, 1, 1).empty());
 uint8_t pixel[4] = {1, 2, 3, 4};
 assert(GenerateMipChainRGBA8(pixel, 0, 1).empty());
 assert(GenerateMipChainRGBA8(pixel, 1, 1, 3).empty());
 std::mt19937 rng(0x521abc);
 for (uint32_t h = 1; h <= 33; ++h) for (uint32_t w = 1; w <= 33; ++w) {
  const size_t pitch = size_t(w) * 4 + 7;
  std::vector<uint8_t> data(pitch * h, 0xee);
  for (uint32_t y = 0; y < h; ++y) for (size_t x = 0; x < size_t(w) * 4; ++x) data[y * pitch + x] = uint8_t(rng());
  auto chain = GenerateMipChainRGBA8(data.data(), w, h, pitch);
  assert(!chain.empty());
  for (uint32_t y = 0; y < h; ++y) for (size_t x = 0; x < size_t(w) * 4; ++x) assert(chain[0].pixels[size_t(y) * w * 4 + x] == data[y * pitch + x]);
  for (size_t level = 1; level < chain.size(); ++level) {
   const auto &src = chain[level - 1]; const auto &dst = chain[level];
   assert(dst.width == std::max(1u, src.width / 2) && dst.height == std::max(1u, src.height / 2));
   // Independent scalar area oracle in floating source coordinates.
   for (uint32_t y = 0; y < dst.height; ++y) for (uint32_t x = 0; x < dst.width; ++x) for (unsigned c = 0; c < 4; ++c) {
    const double l = double(x) * src.width / dst.width, r = double(x + 1) * src.width / dst.width;
    const double t = double(y) * src.height / dst.height, b = double(y + 1) * src.height / dst.height;
    double sum = 0;
    for (uint32_t sy = uint32_t(t); sy < uint32_t(std::ceil(b)); ++sy) for (uint32_t sx = uint32_t(l); sx < uint32_t(std::ceil(r)); ++sx) {
     double weight = std::max(0.0, std::min(r, double(sx + 1)) - std::max(l, double(sx)))
       * std::max(0.0, std::min(b, double(sy + 1)) - std::max(t, double(sy)));
     sum += weight * src.pixels[(size_t(sy) * src.width + sx) * 4 + c];
    }
    auto expected = uint8_t(std::floor(sum / ((r - l) * (b - t)) + .500000001));
    assert(dst.pixels[(size_t(y) * dst.width + x) * 4 + c] == expected);
   }
  }
  assert(chain.back().width == 1 && chain.back().height == 1);
 }
 // Odd edge participates: a bright last column cannot disappear.
 std::vector<uint8_t> odd(3 * 4, 0); odd[8] = 255;
 assert(GenerateMipChainRGBA8(odd.data(), 3, 1)[1].pixels[0] == 85);
}

static void repackTests()
{
    // Rows are padded to a pitch, slices have a fixed row count, and the padding is left as it was.
    constexpr size_t rowBytes = 5, rows = 4, depth = 3, pitch = 8;
    std::vector<uint8_t> source(rowBytes * rows * depth), destination(pitch * rows * depth, 0xEE);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i + 1);
    rr_d3d12::RepackTexture3D(destination.data(), source.data(), rowBytes, rows, depth, pitch);
    for (size_t z = 0; z < depth; ++z) for (size_t y = 0; y < rows; ++y) for (size_t x = 0; x < pitch; ++x)
        assert(destination[(z * rows + y) * pitch + x] == (x < rowBytes ? source[(z * rows + y) * rowBytes + x] : 0xEE));
    // The palette lookup's shape: 64 bytes a row in 256-byte pitch, red fastest
    std::vector<uint8_t> lut(64 * 64 * 64), staged(256 * 64 * 64);
    for (size_t i = 0; i < lut.size(); ++i) lut[i] = uint8_t(i * 7 + (i >> 8));
    rr_d3d12::RepackTexture3D(staged.data(), lut.data(), 64, 64, 64, 256);
    for (size_t b = 0; b < 64; b += 21) for (size_t g = 0; g < 64; g += 9) for (size_t r = 0; r < 64; r += 5)
        assert(staged[(b * 64 + g) * 256 + r] == lut[(b * 64 + g) * 64 + r]);
}

int main()
{
    ringTests();
    keyTests();
    arenaTests();
    mipTests();
    repackTests();
    std::cout << "D3D12 allocator and pipeline key regressions passed\n";
}
