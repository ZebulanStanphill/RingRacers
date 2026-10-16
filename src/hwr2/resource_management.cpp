// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include "resource_management.hpp"

#include <cstring>

#include "../r_state.h"
#include "../v_video.h"
#include "../z_zone.h"

using namespace srb2;
using namespace rhi;
using namespace hwr2;

PaletteManager::PaletteManager() = default;
PaletteManager::PaletteManager(PaletteManager&&) = default;
PaletteManager::~PaletteManager() = default;
PaletteManager& PaletteManager::operator=(PaletteManager&&) = default;

constexpr std::size_t kPaletteSize = 256;
constexpr std::size_t kLighttableRows = LIGHTLEVELS;
// Frames a colormap or lighttable texture is kept after it was last drawn
constexpr uint32_t kTableLifetime = 300;

void PaletteManager::update(Rhi& rhi)
{
	if (!palette_)
	{
		palette_ = rhi.create_texture({TextureFormat::kRGBA, kPaletteSize, 1, TextureWrapMode::kClamp, TextureWrapMode::kClamp});
	}

#if 0
	if (!lighttable_)
	{
		lighttable_ = rhi.create_texture({TextureFormat::kLuminance, kPaletteSize, kLighttableRows, TextureWrapMode::kClamp, TextureWrapMode::kClamp});
	}

	if (!encore_lighttable_)
	{
		encore_lighttable_ = rhi.create_texture({TextureFormat::kLuminance, kPaletteSize, kLighttableRows, TextureWrapMode::kClamp, TextureWrapMode::kClamp});
	}
#endif

	if (!default_colormap_)
	{
		default_colormap_ = rhi.create_texture({TextureFormat::kLuminance, kPaletteSize, 1, TextureWrapMode::kClamp, TextureWrapMode::kClamp});
	}

	// Palette, uploaded only when it changes
	{
		std::array<byteColor_t, kPaletteSize> palette_32;
		for (std::size_t i = 0; i < kPaletteSize; i++)
		{
			palette_32[i] = V_GetColor(i).s;
		}
		static_assert(sizeof(palette_32) == sizeof(palette_data_));
		if (!palette_uploaded_ || std::memcmp(palette_data_.data(), palette_32.data(), sizeof(palette_32)) != 0)
		{
			std::memcpy(palette_data_.data(), palette_32.data(), sizeof(palette_32));
			palette_uploaded_ = true;
			rhi.update_texture(palette_, {0, 0, kPaletteSize, 1}, PixelFormat::kRGBA8, tcb::as_bytes(tcb::span(palette_32)));
		}
	}

#if 0
	// Lighttables
	{
		if (colormaps != nullptr)
		{
			tcb::span<const std::byte> colormap_bytes = tcb::as_bytes(tcb::span(colormaps, kPaletteSize * kLighttableRows));
			rhi.update_texture(lighttable_, {0, 0, kPaletteSize, kLighttableRows}, PixelFormat::kR8, colormap_bytes);
		}

		// FIXME: This is broken, encoremap should not be used directly.
		// Instead, use colormaps + COLORMAP_REMAPOFFSET. See R_ReInitColormaps.
		if (encoremap != nullptr)
		{
			tcb::span<const std::byte> encoremap_bytes = tcb::as_bytes(tcb::span(encoremap, kPaletteSize * kLighttableRows));
			rhi.update_texture(encore_lighttable_, {0, 0, kPaletteSize, kLighttableRows}, PixelFormat::kR8, encoremap_bytes);
		}
	}
#endif

	// Default colormap, which never changes
	if (!default_colormap_uploaded_)
	{
		std::array<uint8_t, kPaletteSize> data;
		for (std::size_t i = 0; i < kPaletteSize; i++)
		{
			data[i] = i;
		}
		rhi.update_texture(default_colormap_, {0, 0, kPaletteSize, 1}, PixelFormat::kR8, tcb::as_bytes(tcb::span(data)));
		default_colormap_uploaded_ = true;
	}
}

static void destroy_unused_tables(Rhi& rhi, srb2::HashMap<const uint8_t*, PaletteTable>& tables, uint32_t frame)
{
	for (auto itr = tables.begin(); itr != tables.end();)
	{
		if (frame - itr->second.last_used_frame > kTableLifetime)
		{
			rhi.destroy_texture(itr->second.texture);
			itr = tables.erase(itr);
		}
		else
		{
			++itr;
		}
	}
}

void PaletteManager::end_frame(Rhi& rhi)
{
	destroy_unused_tables(rhi, colormaps_, frame_);
	destroy_unused_tables(rhi, lighttables_, frame_);
	frame_++;
}

Handle<Texture> PaletteManager::find_or_create_table(
	Rhi& rhi,
	srb2::HashMap<const uint8_t*, PaletteTable>& tables,
	const uint8_t* table,
	uint32_t rows
)
{
	const std::size_t size = kPaletteSize * rows;
	tcb::span<const std::byte> table_bytes = tcb::as_bytes(tcb::span(table, size));

	auto itr = tables.find(table);
	if (itr == tables.end())
	{
		PaletteTable new_table;
		new_table.texture = rhi.create_texture({TextureFormat::kLuminance, kPaletteSize, rows, TextureWrapMode::kClamp, TextureWrapMode::kClamp});
		new_table.data.resize(size);
		std::memcpy(new_table.data.data(), table, size);
		new_table.last_used_frame = frame_;
		rhi.update_texture(new_table.texture, {0, 0, kPaletteSize, rows}, PixelFormat::kR8, table_bytes);

		Handle<Texture> texture = new_table.texture;
		tables.insert_or_assign(table, std::move(new_table));
		return texture;
	}

	// The first time a table is drawn in a frame, check that it hasn't changed (or been freed and its memory reused)
	// since it was uploaded
	PaletteTable& cached = itr->second;
	if (cached.last_used_frame != frame_)
	{
		cached.last_used_frame = frame_;
		if (std::memcmp(cached.data.data(), table, size) != 0)
		{
			std::memcpy(cached.data.data(), table, size);
			rhi.update_texture(cached.texture, {0, 0, kPaletteSize, rows}, PixelFormat::kR8, table_bytes);
		}
	}

	return cached.texture;
}

Handle<Texture> PaletteManager::find_or_create_colormap(Rhi& rhi, srb2::NotNull<const uint8_t*> colormap)
{
	return find_or_create_table(rhi, colormaps_, colormap.get(), 1);
}

Handle<Texture> PaletteManager::find_or_create_extra_lighttable(Rhi& rhi, srb2::NotNull<const uint8_t*> lighttable)
{
	return find_or_create_table(rhi, lighttables_, lighttable.get(), kLighttableRows);
}

FlatTextureManager::FlatTextureManager() = default;
FlatTextureManager::~FlatTextureManager() = default;

static uint32_t get_flat_size(lumpnum_t lump)
{
	SRB2_ASSERT(lump != LUMPERROR);

	std::size_t lumplength = W_LumpLength(lump);
	if (lumplength == 0)
	{
		return 0;
	}

	if ((lumplength & (lumplength - 1)) != 0)
	{
		// Lump length is not a power of two and therefore not a flat.
		return 0;
	}
	uint32_t lumpsize = std::pow(2, std::log2(lumplength) / 2);
	return lumpsize;
}

Handle<Texture> FlatTextureManager::find_or_create_indexed(Rhi& rhi, lumpnum_t lump)
{
	SRB2_ASSERT(lump != LUMPERROR);

	auto flat_itr = flats_.find(lump);
	if (flat_itr != flats_.end())
	{
		return flat_itr->second;
	}

	uint32_t flat_size = get_flat_size(lump);
	Handle<Texture> new_tex = rhi.create_texture({
		TextureFormat::kLuminanceAlpha,
		flat_size,
		flat_size,
		TextureWrapMode::kRepeat,
		TextureWrapMode::kRepeat
	});
	flats_.insert({lump, new_tex});

	srb2::Vector<std::array<uint8_t, 2>> flat_data;
	std::size_t lump_length = W_LumpLength(lump);
	flat_data.reserve(flat_size * flat_size);

	const uint8_t* flat_memory = static_cast<const uint8_t*>(W_CacheLumpNum(lump, PU_PATCH));
	SRB2_ASSERT(flat_memory != nullptr);

	tcb::span<const uint8_t> flat_bytes = tcb::span(flat_memory, lump_length);
	for (const uint8_t index : flat_bytes)
	{
		// The alpha/green channel is set to 0 if it's index 247; this is not usually used but fake floors can be
		// masked sometimes, so we need to treat it as transparent when rendering them.
		// See https://zdoom.org/wiki/Palette for remarks on fake 247 transparency
		flat_data.push_back({index, index == 247 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(255)});
	}

	// A flat size of 1 would end up being 2 bytes, so we need 2 more bytes to be unpack-aligned on texture upload
	// Any other size would implicitly be aligned.
	// Sure hope nobody tries to load any flats that are too big for the gpu!
	if (flat_size == 1)
	{
		flat_data.push_back({0, 0});
	}

	tcb::span<const std::byte> data_bytes = tcb::as_bytes(tcb::span(flat_data));
	rhi.update_texture(new_tex, {0, 0, flat_size, flat_size}, rhi::PixelFormat::kRG8, data_bytes);

	return new_tex;
}
