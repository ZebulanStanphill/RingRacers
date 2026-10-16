// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  texture_repack.h
/// \brief [UWP]: Copies tightly packed 3D texture data into the rows and slices GetCopyableFootprints describes

#ifndef RR_D3D12_TEXTURE_REPACK_H
#define RR_D3D12_TEXTURE_REPACK_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rr_d3d12
{

// source holds depth slices of rows tightly packed; destination has rowPitch bytes between rows and rows rows in
// each slice, the footprint's RowPitch and Height. The padding in each row is left alone, since it's never sampled.
inline void RepackTexture3D(void *destination, const void *source, size_t rowBytes, size_t rows, size_t depth,
	size_t rowPitch)
{
	auto *dst = static_cast<uint8_t *>(destination);
	const auto *src = static_cast<const uint8_t *>(source);

	for (size_t slice = 0; slice < depth; ++slice)
	{
		for (size_t row = 0; row < rows; ++row)
			memcpy(dst + (slice * rows + row) * rowPitch, src + (slice * rows + row) * rowBytes, rowBytes);
	}
}

} // namespace rr_d3d12
#endif
