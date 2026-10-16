// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_palette.inc
/// \brief [UWP]: Palette rendering's lookup table, light tables, palettes and last step

// Included by r_d3d12.cpp. These are ordinary persistent SRVs and staging-ring uploads, so replacing, updating or
// clearing one follows the texture rules: an update is recorded in order with the draws around it, and a resource,
// heap block or SRV index retires with the fence of the work recorded so far.

void RetireScreenSrv()
{
	if (g_screenSrv != UINT32_MAX)
		g_srvIndices.Free(g_screenSrv, Core_PendingFence());
	g_screenSrv = UINT32_MAX;
}

// The master palette's view, or the null one, for fade maps
[[maybe_unused]] UINT MasterPaletteSrv()
{
	return g_masterPalette.res ? g_masterPalette.srv : 0;
}

// Replaces a palette's colors, creating it if need be
bool StorePalette(GpuTexture &palette, const wchar_t *name, const RGBA_t *colors)
{
	bool stored;

	if (palette.res)
		stored = UploadTexture(palette, colors);
	else
	{
		stored = CreateTextureStorage(&palette, 0, 256, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, colors);
		if (stored)
			palette.res->SetName(name);
	}
	if (stored)
		g_stats.paletteUploads++;
	return stored;
}

// The 64x64x64 lookup table's one subresource, through the staging ring. Its rows are 256 bytes apart.
bool UploadPaletteLookup(const UINT8 *lut)
{
	const D3D12_RESOURCE_DESC desc = g_paletteLookup.res->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT rows;
	UINT64 rowBytes, bytes;

	Core_Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
	// Alloc can submit and reopen the list: do it before recording the transition
	UploadAllocation allocation = g_textureRing.Alloc((size_t)bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	if (!allocation)
		return false;
	RepackTexture3D(static_cast<UINT8 *>(allocation.cpu) + footprint.Offset, lut, (size_t)rowBytes, rows,
		footprint.Footprint.Depth, footprint.Footprint.RowPitch);
	footprint.Offset += allocation.offset;

	D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
	from.pResource = g_textureRing.Resource();
	from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	from.PlacedFootprint = footprint;
	to.pResource = g_paletteLookup.res.Get();
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	Transition(g_paletteLookup, D3D12_RESOURCE_STATE_COPY_DEST);
	Core_FlushBarriers();
	Core_List()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
	Transition(g_paletteLookup, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

// Created once; placed like any texture so the driver's state word can't zero its white entry (see CreateTextureResource)
bool CreatePaletteLookup()
{
	constexpr UINT size = HWR_PALETTE_LUT_SIZE;
	const UINT index = AllocateSrv();

	if (index == UINT32_MAX)
		return false;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	desc.Width = desc.Height = desc.DepthOrArraySize = size;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	GpuTexture lookup;
	if (!CreateTextureResource(lookup, desc))
	{
		g_srvIndices.Free(index, Core_PendingFence());
		return false;
	}
	lookup.res->SetName(L"palette lookup table");
	lookup.srv = index;
	lookup.width = lookup.height = size;
	lookup.format = desc.Format;
	lookup.bytes = (UINT64)size * size * size;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
	srv.Texture3D.MipLevels = 1;
	Core_Device()->CreateShaderResourceView(lookup.res.Get(), &srv, SrvCpu(index));
	g_paletteLookup = lookup;
	g_textureBytes += lookup.bytes;
	g_textureAllocatedBytes += lookup.allocatedBytes;
	g_stats.texturesCreated++;
	return true;
}

void D3D_SetScreenPalette(RGBA_t *palette)
{
	if (!Core_Device() || !palette
		|| (g_screenPalette.res && !memcmp(g_screenPaletteColors, palette, sizeof g_screenPaletteColors)))
		return;

	if (StorePalette(g_screenPalette, L"screen palette", palette))
		memcpy(g_screenPaletteColors, palette, sizeof g_screenPaletteColors);
}

void D3D_SetPaletteLookup(UINT8 *lut)
{
	if (!Core_Device() || !lut)
		return;

	if (!g_paletteLookup.res && !CreatePaletteLookup())
		return;

	// Indexed [b][g][r], so r is x
	if (!UploadPaletteLookup(lut))
		return;
	g_stats.paletteUploads++;
	Log("palette lookup table updated");

	// HWR_SetMapPalette calls this with a lookup built from pMasterPalette whenever
	// the palette lump changes, including V_SetPaletteLump. R_ReInitColormaps only
	// changes light tables. Never use D3D_SetScreenPalette's flash/corrected colors.
	if (!pMasterPalette)
	{
		RetireTexture(g_masterPalette);
		return;
	}
	if (StorePalette(g_masterPalette, L"master palette", pMasterPalette))
	{
		g_stats.textureUploads++;
		g_stats.textureUploadBytes += 256 * sizeof(RGBA_t);
	}
}

UINT32 D3D_CreateLightTable(RGBA_t *hw_lighttable)
{
	if (!Core_Device() || !hw_lighttable)
		return 0;

	const UINT32 id = g_nextLightTableId;
	GpuTexture table;
	// 32 light levels, then the base colormap's full-bright level for brightmaps
	if (!CreateTextureStorage(&table, id, 256, 33, DXGI_FORMAT_R8G8B8A8_UNORM, false, hw_lighttable))
		return 0;

	wchar_t name[64];
	swprintf(name, sizeof name / sizeof *name, L"light table %u", id);
	table.res->SetName(name);
	g_nextLightTableId++;
	g_lightTables[id] = table;
	g_stats.lightTablesCreated++;
	return id;
}

void D3D_UpdateLightTable(UINT32 id, RGBA_t *hw_lighttable)
{
	auto it = g_lightTables.find(id);
	if (it == g_lightTables.end() || !hw_lighttable)
		return;
	// In order with the draws around it: those recorded before it keep the old colors
	if (UploadTexture(it->second, hw_lighttable))
		g_stats.lightTablesUpdated++;
}

void D3D_ClearLightTables(void)
{
	// The lists in flight keep using them; their resources and views go when the fence says so
	memset(g_boundPaletteTables, 0, sizeof g_boundPaletteTables);
	g_currentLightTable = nullptr;
	if (!g_lightTables.empty())
		Log("cleared %d light tables", (int)g_lightTables.size());
	for (auto &table : g_lightTables)
		RetireTexture(table.second);
	g_lightTables.clear();
}

// g_postTexture at the screen's size, and a view of the screen to read it with
bool PreparePaletteTargets()
{
	if (!g_postTexture.res || g_postTexture.width != g_rtWidth || g_postTexture.height != g_rtHeight)
	{
		RetireTexture(g_postTexture);
		const D3D12_RESOURCE_DESC desc = g_rt->GetDesc();
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g_postTexture.res)), "CreateCommittedResource (palette)"))
		{
			g_postTexture.res.Reset();
			return false;
		}
		g_postTexture.res->SetName(L"palette post");
		g_postTexture.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_postTexture.width = g_rtWidth;
		g_postTexture.height = g_rtHeight;
		g_postTexture.format = desc.Format;
		Core_Device()->CreateRenderTargetView(g_postTexture.res.Get(), nullptr, Core_Rtv(kRtvPost));
		Log("made a %ux%u texture for palette rendering's last step", g_rtWidth, g_rtHeight);
	}

	if (g_screenSrv == UINT32_MAX)
	{
		const UINT index = AllocateSrv();
		if (index == UINT32_MAX)
			return false;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = kScreenFormat;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		Core_Device()->CreateShaderResourceView(g_rt.Get(), &srv, SrvCpu(index));
		g_screenSrv = index;
	}
	return true;
}

// Turns the finished frame into the screen's palette, in g_postTexture. Returns whether it did.
// g_postTexture stays in RENDER_TARGET for the present copy, and g_rt returns to it for drawing.
bool PalettePostprocess()
{
	if (!CanDraw() || !g_paletteRendering || !g_paletteLookup.res || !g_screenPalette.res)
		return false;

	const precise_t start = I_GetPreciseTime();
	bool done = false;

	PipelineKey key = {};
	key.vs = VS_FULLSCREEN;
	key.ps = PS_PALETTEPOST;
	key.layout = LAYOUT_NONE;
	key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	key.rtFormat = kScreenFormat;
	key.dsvFormat = DXGI_FORMAT_UNKNOWN; // no depth buffer is bound
	key.blend = 1 << 15; // opaque, all channels
	// Neither depth nor stencil, with valid fixed-function values like the prewarmed pipelines
	key.depth = 2ULL | (8ULL << 4) | (1ULL << 8) | (1ULL << 12) | (1ULL << 16) | (255ULL << 20) | (255ULL << 28);
	ID3D12PipelineState *pso = PreparePaletteTargets() ? FindOrCreatePipeline(key) : nullptr;
	if (pso)
	{
		ID3D12GraphicsCommandList *list = Core_List();
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kRtvPost);
		const D3D12_VIEWPORT viewport = {0.0f, 0.0f, (float)g_rtWidth, (float)g_rtHeight, 0.0f, 1.0f};
		const D3D12_RECT scissor = {0, 0, (LONG)g_rtWidth, (LONG)g_rtHeight};

		Transition(g_postTexture, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Core_FlushBarriers();
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		list->SetPipelineState(pso);
		// VSFullscreen reads b0's depth; a draw's constants may not be the ones bound
		list->SetGraphicsRootConstantBufferView(0, g_zeroConstants->GetGPUVirtualAddress());
		list->SetGraphicsRootDescriptorTable(1, SrvGpu(g_screenSrv)); // t0: the finished frame
		list->SetGraphicsRootDescriptorTable(6, SrvGpu(g_paletteLookup.srv)); // t2
		list->SetGraphicsRootDescriptorTable(7, SrvGpu(g_screenPalette.srv)); // t4
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->DrawInstanced(3, 1, 0, 0);
		g_stats.psoChanges++;
		g_stats.rootSets++;
		g_stats.tableSets += 3;

		// Back to drawing the screen: everything this pass set, and the caches of it, is stale
		Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Core_FlushBarriers();
		BindRenderTarget();
		g_stats.paletteSteps++;
		done = true;
	}
	g_stats.paletteTime += I_GetPreciseTime() - start;
	return done;
}
