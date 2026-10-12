# Direct3D 12 placed-texture guard

## The driver defect

On Xbox One X, the D3D12 driver (`umd12ddi.dll`) keeps a 4-byte state word for each placed buffer or non-render-target texture. The word sits at:

`HeapOffset + align_up(end of the resource's data, 256)`

`GetResourceAllocationInfo` doesn't reserve space for it:
- **Data smaller than the reported size:** the word lands in the resource's own padding. This happens with mip chains, odd sizes and block-compressed formats.
- **Data that fills the reported size:** the word lands on the first byte after the resource. That covers buffers, and textures such as 256×256 RGBA8 with one mip.

When the word lands outside, `CreatePlacedResource` writes `0` there. Each later transition rewrites it: `COPY_DEST` and `UNORDERED_ACCESS` write `1`, and `COPY_SOURCE` and `COMMON` write `0`.

That word was the black-whites bug under palette rendering. A 256×256 texture's word landed on the first texel of the light table placed right after it.

If the resource ends exactly at the end of a CPU-mapped heap, the write falls outside the mapping, and `CreatePlacedResource` raises an access violation. Render-target textures are not affected: their reported size includes an extra 64 KB that holds their state.

A standalone diagnostic app, run on an Xbox One X with OS 10.0.26100.9445, established this:
- **Direct CPU checks:** the word was observed changing during `CreatePlacedResource` and during each later transition.
- **Exact fault address:** the address of the crash was predicted before the call and matched exactly.
- **Byte map:** every DWORD each step changed was mapped across 29 buffer and texture layouts.

The report, logs and app source are kept outside this repository.

## The workaround

`CreateTextureResource` asks the texture arena for `SizeInBytes + 4 KB` for every placed texture. The arena then rounds that up to the texture's placement alignment, so the effective gap is one alignment unit:
- **4 KB** for small-placement textures;
- **64 KB** for others.

A smaller constant would give the same blocks: the next resource can't start before the next aligned offset anyway. The guard also keeps any texture from ending exactly at the end of its 16 MB heap.

The texture arena uses alignment-rounded quarter-octave classes (P, 1.25P, 1.5P, 1.75P, 2P), with chunks and free lists separated by alignment. Most textures then already have slack after their data, and the guard costs extra memory only when a texture exactly fills its class. Static uploads keep their power-of-two classes; the workaround doesn't apply to them.

## Known gap

Committed textures appear to hold the same word over their last texel, so it becomes `0` after any state change. Textures over 4 MB, and textures whose placement fails, still fall back to committed resources. Those can show a wrong bottom-right pixel.
