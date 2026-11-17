// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_resources.mm
/// \brief mtl12's buffers and textures
///
/// A Direct3D 12 resource is a Metal buffer or texture of its own, whether it was committed or placed in a heap.
/// Upload and readback buffers are in shared storage, which is mapped for as long as the buffer exists; everything
/// else is private to the GPU, and reached with blits.

#include "mtl12_internal.h"

#include <algorithm>

namespace rr_mtl12
{

uint64_t TextureBytes(const D3D12_RESOURCE_DESC &desc, const FormatInfo &format)
{
	const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const UINT mips = std::max<UINT>(1, desc.MipLevels);
	uint64_t bytes = 0;
	for (UINT mip = 0; mip < mips; mip++)
	{
		const uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
		const uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
		const uint64_t depth = volume ? std::max<uint64_t>(1, desc.DepthOrArraySize >> mip) : std::max<uint64_t>(1, desc.DepthOrArraySize);
		bytes += width * height * depth * format.bytes;
	}
	return bytes;
}

HRESULT CreateResource(D3D12_HEAP_TYPE type, Heap *heap, const D3D12_RESOURCE_DESC &desc, Resource **out)
{
	*out = nullptr;
	if (!g.device)
		return E_FAIL;
	Resource *resource = new Resource;
	resource->desc = desc;

	@autoreleasepool
	{
		if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		{
			// The CPU reads and writes upload and readback buffers; the rest are the GPU's alone
			const MTLResourceOptions options = type == D3D12_HEAP_TYPE_DEFAULT ? MTLResourceStorageModePrivate
				: MTLResourceStorageModeShared;
			resource->buffer = desc.Width ? [g.device newBufferWithLength:(NSUInteger)desc.Width options:options] : nil;
			if (!resource->buffer)
			{
				Log("CreateResource: Metal made no buffer of %llu bytes", (unsigned long long)desc.Width);
				resource->Release();
				return E_OUTOFMEMORY;
			}
			resource->bytes = desc.Width;
			if (type != D3D12_HEAP_TYPE_DEFAULT)
				resource->mapped = resource->buffer.contents;
			// An address range of its own, with room before the next so that a range's end never reads as another buffer
			resource->address = g.nextAddress;
			g.nextAddress += (desc.Width + 0x1FFFF) & ~0xFFFFull;
			g.buffers[resource->address] = resource;
		}
		else
		{
			resource->format = Format(desc.Format);
			if (resource->format.format == MTLPixelFormatInvalid)
			{
				Log("CreateResource: format %d isn't known to mtl12", (int)desc.Format);
				resource->Release();
				return E_INVALIDARG;
			}
			const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
			if (!volume && desc.DepthOrArraySize > 1)
				MTL12_LOG_ONCE("CreateResource: texture arrays are not implemented by mtl12");
			MTLTextureDescriptor *info = [[MTLTextureDescriptor alloc] init];
			info.textureType = volume ? MTLTextureType3D : MTLTextureType2D;
			info.pixelFormat = resource->format.format;
			info.width = (NSUInteger)desc.Width;
			info.height = desc.Height;
			info.depth = volume ? desc.DepthOrArraySize : 1;
			info.mipmapLevelCount = std::max<NSUInteger>(1, desc.MipLevels);
			info.sampleCount = 1;
			info.storageMode = MTLStorageModePrivate;
			MTLTextureUsage usage = MTLTextureUsageUnknown;
			if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
				usage |= MTLTextureUsageRenderTarget;
			if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
				usage |= MTLTextureUsageShaderRead;
			info.usage = usage;
			resource->texture = [g.device newTextureWithDescriptor:info];
			if (!resource->texture)
			{
				Log("CreateResource: Metal made no %llux%u texture", (unsigned long long)desc.Width, (unsigned)desc.Height);
				resource->Release();
				return E_OUTOFMEMORY;
			}
		}
	}

	if (heap)
	{
		resource->heap = heap;
		heap->AddRef();
	}
	*out = resource;
	return S_OK;
}

HRESULT WrapTexture(void *texture, ID3D12Resource **out)
{
	if (!out)
		return E_POINTER;
	*out = nullptr;
	id<MTLTexture> wrapped = (__bridge id<MTLTexture>)texture;
	if (!wrapped)
		return E_INVALIDARG;
	DXGI_FORMAT format;
	switch (wrapped.pixelFormat)
	{
		case MTLPixelFormatBGRA8Unorm: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
		case MTLPixelFormatRGBA8Unorm: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
		default:
			Log("WrapTexture: pixel format %lu isn't one the driver draws to", (unsigned long)wrapped.pixelFormat);
			return E_INVALIDARG;
	}
	Resource *resource = new Resource;
	resource->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resource->desc.Width = wrapped.width;
	resource->desc.Height = (UINT)wrapped.height;
	resource->desc.DepthOrArraySize = 1;
	resource->desc.MipLevels = 1;
	resource->desc.Format = format;
	resource->desc.SampleDesc.Count = 1;
	resource->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	resource->texture = wrapped;
	resource->format = Format(format);
	*out = resource;
	return S_OK;
}

Resource::~Resource()
{
	if (buffer)
		g.buffers.erase(address);
	// A Metal texture or buffer hands its device to the autorelease pool as it is deallocated, and the driver's
	// thread has no pool of its own: without one here, each resource the driver frees would leave that reference
	// behind for good
	@autoreleasepool
	{
		views_.clear();
		texture = nil;
		buffer = nil;
	}
	if (heap)
		heap->Release();
}

HRESULT Resource::Map(UINT, const D3D12_RANGE *, void **data)
{
	if (!mapped)
		return E_FAIL;
	if (data)
		*data = mapped;
	return S_OK;
}

void Resource::Named()
{
	@autoreleasepool
	{
		NSString *label = [NSString stringWithUTF8String:name_.c_str()];
		if (buffer)
			buffer.label = label;
		else if (texture)
			texture.label = label;
	}
}

id<MTLTexture> Resource::View(MTLPixelFormat viewFormat, MTLTextureType type, uint32_t mips)
{
	if (!texture)
		return nil;
	if (viewFormat != texture.pixelFormat)
	{
		// Would need every texture made for it (MTLTextureUsagePixelFormatView), and the driver's views have their
		// textures' formats
		MTL12_LOG_ONCE("a view of another format than its texture's is not implemented by mtl12");
		return nil;
	}
	if (type != texture.textureType)
	{
		MTL12_LOG_ONCE("a view of another dimension than its texture's is not implemented by mtl12");
		return nil;
	}
	if (mips >= texture.mipmapLevelCount)
		return texture;
	for (const KeptView &kept : views_)
		if (kept.format == viewFormat && kept.type == type && kept.mips == mips)
			return kept.view;
	id<MTLTexture> view = [texture newTextureViewWithPixelFormat:viewFormat textureType:type levels:NSMakeRange(0, mips)
		slices:NSMakeRange(0, 1)];
	if (!view)
	{
		Log("Metal made no view of %u mips", (unsigned)mips);
		return nil;
	}
	views_.push_back({viewFormat, type, mips, view});
	return view;
}

Resource *NullImage(DXGI_FORMAT format, bool volume)
{
	for (Resource *image : g.nullImages)
		if (image->desc.Format == format && (image->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) == volume)
			return image;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	Resource *image = nullptr;
	if (FAILED(CreateResource(D3D12_HEAP_TYPE_DEFAULT, nullptr, desc, &image)))
		return nullptr;
	image->SetName(L"null view");

	// A null view reads as zero in Direct3D 12, so the one texel is written as zero before anything can sample it: by a
	// command buffer of its own, which the queue runs before whatever the driver commits next. It is a submission like
	// the driver's, so if it fails on the GPU the device says so, and the device isn't released while it runs.
	@autoreleasepool
	{
		const NSUInteger texel = std::max(1u, image->format.bytes);
		const bool refused = g.failNextNullImage;
		g.failNextNullImage = false;
		id<MTLBuffer> zero = refused ? nil : [g.device newBufferWithLength:16 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> commands = zero ? NewCommandBuffer() : nil;
		id<MTLBlitCommandEncoder> blit = commands ? [commands blitCommandEncoder] : nil;
		if (!blit)
		{
			// Not kept: its texel would be whatever the memory held. The view stays empty, a draw that needs it is
			// skipped with a line in the log, and the next null view of this kind tries again.
			Log("a null view's texture couldn't be written as zero, so the view is left empty");
			image->Release();
			return nullptr;
		}
		memset(zero.contents, 0, 16);
		blit.label = @"null view";
		[blit copyFromBuffer:zero sourceOffset:0 sourceBytesPerRow:texel sourceBytesPerImage:texel
			sourceSize:MTLSizeMake(1, 1, 1) toTexture:image->texture destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		Commit({commands}, nil);
	}
	g.nullImages.push_back(image);
	return image;
}

bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, id<MTLBuffer> __unsafe_unretained *buffer, uint64_t *offset)
{
	auto after = g.buffers.upper_bound(address);
	if (after == g.buffers.begin())
		return false;
	--after;
	Resource *resource = after->second;
	if (address - after->first > resource->bytes)
		return false;
	*buffer = resource->buffer;
	*offset = address - after->first;
	return true;
}

} // namespace rr_mtl12
