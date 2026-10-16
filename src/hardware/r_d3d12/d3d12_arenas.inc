// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Included by r_d3d12.cpp: GPU backing for the pure size-class allocator.
constexpr size_t kStaticChunkBytes = 4 * 1024 * 1024;
constexpr size_t kTextureHeapBytes = 16 * 1024 * 1024;
SizeClassArena g_staticArena, g_textureArena;
struct StaticChunk { ComPtr<ID3D12Resource> resource; BYTE *cpu = nullptr; bool dedicated = false; };
std::vector<StaticChunk> g_staticChunks;
std::vector<ComPtr<ID3D12Heap>> g_textureHeaps;
UINT64 g_placedCreated = 0, g_dedicatedCreated = 0, g_committedCreated = 0, g_smallPlaced = 0, g_normalPlaced = 0;
UINT64 g_textureHeapUsed = 0, g_textureHeapPeak = 0;
size_t g_staticDedicated = 0;
std::multimap<UINT64, UINT64> g_pendingTextureBytes;
void CollectTextureBlocks()
{
	const auto fence = Core_CompletedFence();
	g_textureArena.Collect(fence);
	const auto end = g_pendingTextureBytes.upper_bound(fence);
	for (auto it = g_pendingTextureBytes.begin(); it != end; ++it) g_textureHeapUsed -= it->second;
	g_pendingTextureBytes.erase(g_pendingTextureBytes.begin(), end);
}
size_t StaticChunkCount()
{
	return (size_t)std::count_if(g_staticChunks.begin(), g_staticChunks.end(), [](const StaticChunk &chunk) { return chunk.resource && !chunk.dedicated; });
}
struct StaticAllocation
{
	ID3D12Resource *resource = nullptr;
	BYTE *cpu = nullptr;
	UINT64 offset = 0;
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	UINT bytes = 0;
	SizeClassArena::Block block;
	explicit operator bool() const { return resource != nullptr; }
};
void InitArenas()
{
	g_staticArena.Reset(256, kStaticChunkBytes);
	g_textureArena.Reset(4096, kTextureHeapBytes, SIZE_MAX, true);
}
StaticAllocation AllocateStatic(const void *data, UINT bytes)
{
	if (!bytes || Core_RemovalSeen()) return {};
	g_staticArena.Collect(Core_CompletedFence());
	auto block = g_staticArena.Allocate(bytes);
	if (block.needsChunk)
	{
		StaticChunk chunk; chunk.dedicated = block.dedicated;
		D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = block.dedicated ? bytes : kStaticChunkBytes;
		desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&chunk.resource)), "Create static arena buffer")) return {};
		chunk.resource->SetName(block.dedicated ? L"dedicated static upload buffer" : L"static upload arena chunk");
		D3D12_RANGE read = {0, 0};
		if (Core_Failed(chunk.resource->Map(0, &read, reinterpret_cast<void **>(&chunk.cpu)), "Map static arena")) return {};
		if (block.dedicated) { block = g_staticArena.AddDedicated(bytes); ++g_staticDedicated; }
		else { g_staticArena.AddChunk(); block = g_staticArena.Allocate(bytes); }
		if (g_staticChunks.size() <= block.chunk) g_staticChunks.resize(block.chunk + 1);
		g_staticChunks[block.chunk] = chunk;
	}
	if (!block.valid) return {};
	auto &chunk = g_staticChunks[block.chunk];
	StaticAllocation result = {chunk.resource.Get(), chunk.cpu + block.offset, block.offset,
		chunk.resource->GetGPUVirtualAddress() + block.offset, bytes, block};
	memcpy(result.cpu, data, bytes);
	return result;
}
void RetireStatic(StaticAllocation &allocation)
{
	if (!allocation) return;
	g_staticArena.Free(allocation.block, Core_PendingFence());
	if (allocation.block.dedicated)
	{
		Core_Retire(g_staticChunks[allocation.block.chunk].resource);
		g_staticChunks[allocation.block.chunk].cpu = nullptr;
		--g_staticDedicated;
	}
	allocation = {};
}
void ShutdownArenas()
{
	for (auto &chunk : g_staticChunks) Core_Retire(chunk.resource);
	g_staticChunks.clear();
	// Heap retirement follows retirement of all placed resources and shares their covering fence.
	for (auto &heap : g_textureHeaps) Core_Retire(heap);
	g_textureHeaps.clear();
}
