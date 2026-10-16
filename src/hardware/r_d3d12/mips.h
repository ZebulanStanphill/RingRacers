// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_D3D12_MIPS_H
#define RR_D3D12_MIPS_H
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <utility>
namespace rr_d3d12
{
struct MipLevelRGBA8 { uint32_t width = 0, height = 0; std::vector<uint8_t> pixels; };
// Linear UNORM box filtering, independently per channel (including alpha).
// Area weighting includes every odd-edge texel rather than discarding it.
// Invalid dimensions/stride return an empty chain. Input includes row padding.
inline std::vector<MipLevelRGBA8> GenerateMipChainRGBA8(const uint8_t *data,
 uint32_t width, uint32_t height, size_t rowPitch = 0)
{
 std::vector<MipLevelRGBA8> chain;
 if (!data || !width || !height) return chain;
 if (uint64_t(width) * height > UINT64_MAX / 255) return chain;
 if (uint64_t(width) * 4 > std::numeric_limits<size_t>::max()) return chain;
 const size_t rowBytes = size_t(width) * 4;
 if (!rowPitch) rowPitch = rowBytes;
 if (rowPitch < rowBytes || height > SIZE_MAX / rowPitch || height > SIZE_MAX / rowBytes) return chain;
 MipLevelRGBA8 base; base.width = width; base.height = height; base.pixels.resize(rowBytes * height);
 for (uint32_t y = 0; y < height; ++y) std::memcpy(base.pixels.data() + size_t(y) * rowBytes, data + size_t(y) * rowPitch, rowBytes);
 chain.push_back(std::move(base));
 while (width > 1 || height > 1) {
  const auto &src = chain.back();
  MipLevelRGBA8 dst; dst.width = std::max(1u, width / 2); dst.height = std::max(1u, height / 2);
  dst.pixels.resize(size_t(dst.width) * dst.height * 4);
  // Integer coordinates in units of 1/dst dimension make overlap exact.
  for (uint32_t y = 0; y < dst.height; ++y) for (uint32_t x = 0; x < dst.width; ++x) {
   const uint64_t left = uint64_t(x) * width, right = uint64_t(x + 1) * width;
   const uint64_t top = uint64_t(y) * height, bottom = uint64_t(y + 1) * height;
   for (unsigned channel = 0; channel < 4; ++channel) {
    uint64_t sum = 0;
    for (uint32_t sy = uint32_t(top / dst.height); sy < (bottom + dst.height - 1) / dst.height; ++sy) {
     const uint64_t wy = std::min(bottom, uint64_t(sy + 1) * dst.height) - std::max(top, uint64_t(sy) * dst.height);
     for (uint32_t sx = uint32_t(left / dst.width); sx < (right + dst.width - 1) / dst.width; ++sx) {
      const uint64_t wx = std::min(right, uint64_t(sx + 1) * dst.width) - std::max(left, uint64_t(sx) * dst.width);
      sum += src.pixels[(size_t(sy) * width + sx) * 4 + channel] * wx * wy;
     }
    }
    const uint64_t weight = uint64_t(width) * height;
    dst.pixels[(size_t(y) * dst.width + x) * 4 + channel] = uint8_t((sum + weight / 2) / weight);
   }
  }
  width = dst.width; height = dst.height; chain.push_back(std::move(dst));
 }
 return chain;
}
}
#endif
