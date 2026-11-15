// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_resources.cpp
/// \brief vk12's buffers and images, their memory and views, and the render passes and framebuffers drawn through

#include "vk12_internal.h"

#include <algorithm>
#include <mutex>

namespace rr_vk12
{

bool ImageInfo(const D3D12_RESOURCE_DESC &desc, VkImageCreateInfo &info, FormatInfo &format)
{
	format = Format(desc.Format);
	if (format.format == VK_FORMAT_UNDEFINED)
		return false;
	const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	info.format = format.format;
	info.extent.width = (uint32_t)desc.Width;
	info.extent.height = desc.Height;
	info.extent.depth = volume ? desc.DepthOrArraySize : 1;
	info.mipLevels = std::max<uint32_t>(1, desc.MipLevels);
	info.arrayLayers = volume ? 1 : std::max<uint32_t>(1, desc.DepthOrArraySize);
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	else if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

VkImageLayout LayoutOf(const Resource *resource, D3D12_RESOURCE_STATES state)
{
	// The states the driver uses. COMMON is also PRESENT, which is what it means for a swap chain's image.
	if (state == D3D12_RESOURCE_STATE_COMMON)
		return resource->swapchain ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
	if (state & D3D12_RESOURCE_STATE_RENDER_TARGET)
		return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (state & D3D12_RESOURCE_STATE_DEPTH_WRITE)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (state & D3D12_RESOURCE_STATE_COPY_DEST)
		return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	if (state == D3D12_RESOURCE_STATE_COPY_SOURCE)
		return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	if (state & D3D12_RESOURCE_STATE_DEPTH_READ)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	if (state & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	return VK_IMAGE_LAYOUT_GENERAL;
}

HRESULT CreateResource(const D3D12_HEAP_PROPERTIES *heapProperties, Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state, Resource **out)
{
	*out = nullptr;
	Resource *resource = new Resource;
	resource->desc = desc;
	resource->initialState = state;
	const D3D12_HEAP_TYPE type = heap ? heap->GetDesc().Properties.Type : heapProperties->Type;
	VkMemoryPropertyFlags fallback;
	const VkMemoryPropertyFlags wanted = HeapMemoryFlags(type, &fallback);
	VkMemoryRequirements requirements;
	HRESULT hr;

	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		info.size = desc.Width;
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
			| VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		hr = Result(vkCreateBuffer(g.device, &info, nullptr, &resource->buffer), "vkCreateBuffer");
		if (FAILED(hr))
		{
			resource->Release();
			return hr;
		}
		resource->bytes = desc.Width;
		vkGetBufferMemoryRequirements(g.device, resource->buffer, &requirements);
	}
	else
	{
		VkImageCreateInfo info;
		if (!ImageInfo(desc, info, resource->format))
		{
			Log("CreateResource: format %d isn't known to vk12", (int)desc.Format);
			resource->Release();
			return E_INVALIDARG;
		}
		hr = Result(vkCreateImage(g.device, &info, nullptr, &resource->image), "vkCreateImage");
		if (FAILED(hr))
		{
			resource->Release();
			return hr;
		}
		vkGetImageMemoryRequirements(g.device, resource->image, &requirements);
	}

	VkDeviceMemory memory;
	VkDeviceSize offset;
	if (heap)
	{
		// Placed: the heap's memory, where the driver's allocator put it
		if (!(requirements.memoryTypeBits & (1u << heap->MemoryType())) || heapOffset % requirements.alignment
			|| heapOffset + requirements.size > heap->GetDesc().SizeInBytes)
		{
			VK12_LOG_ONCE("CreatePlacedResource: a %llux%u resource doesn't fit its heap at %llu (needs %llu bytes aligned to %llu, memory types 0x%X, heap type %u)",
				(unsigned long long)desc.Width, (unsigned)desc.Height, (unsigned long long)heapOffset,
				(unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
				(unsigned)requirements.memoryTypeBits, (unsigned)heap->MemoryType());
			resource->Release();
			return E_INVALIDARG;
		}
		memory = heap->Memory();
		offset = heapOffset;
		resource->heap = heap;
		heap->AddRef();
	}
	else
	{
		const uint32_t memoryType = MemoryType(requirements.memoryTypeBits, wanted, fallback);
		VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = memoryType;
		hr = memoryType == UINT32_MAX ? E_OUTOFMEMORY
			: Result(vkAllocateMemory(g.device, &allocate, nullptr, &resource->memory), "vkAllocateMemory");
		if (FAILED(hr))
		{
			resource->Release();
			return hr;
		}
		memory = resource->memory;
		offset = 0;
	}

	if (resource->buffer)
	{
		hr = Result(vkBindBufferMemory(g.device, resource->buffer, memory, offset), "vkBindBufferMemory");
		if (SUCCEEDED(hr) && type != D3D12_HEAP_TYPE_DEFAULT && resource->memory)
			hr = Result(vkMapMemory(g.device, resource->memory, 0, VK_WHOLE_SIZE, 0, &resource->mapped), "vkMapMemory");
		if (SUCCEEDED(hr))
		{
			// An address range of its own, with room before the next so that a range's end never reads as another buffer
			resource->address = g.nextAddress;
			g.nextAddress += (desc.Width + 0x1FFFF) & ~0xFFFFull;
			g.buffers[resource->address] = resource;
		}
	}
	else
	{
		hr = Result(vkBindImageMemory(g.device, resource->image, memory, offset), "vkBindImageMemory");
		if (SUCCEEDED(hr))
		{
			resource->pendingInitialLayout = true;
			g.initialLayouts.push_back(resource);
		}
	}
	if (FAILED(hr))
	{
		resource->Release();
		return hr;
	}
	*out = resource;
	return S_OK;
}

HRESULT WrapSwapchainImage(VkImage image, VkFormat format, UINT width, UINT height, ID3D12Resource **out)
{
	Resource *resource = new Resource;
	resource->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resource->desc.Width = width;
	resource->desc.Height = height;
	resource->desc.DepthOrArraySize = 1;
	resource->desc.MipLevels = 1;
	resource->desc.Format = format == VK_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	resource->desc.SampleDesc.Count = 1;
	resource->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	resource->image = image;
	resource->format = {format, 4, VK_IMAGE_ASPECT_COLOR_BIT};
	resource->swapchain = true;
	resource->layout = VK_IMAGE_LAYOUT_UNDEFINED; // the first transition from PRESENT discards, as a new image must
	*out = resource;
	return S_OK;
}

Resource::~Resource()
{
	if (!g.device)
		return;
	if (pendingInitialLayout)
		g.initialLayouts.erase(std::remove(g.initialLayouts.begin(), g.initialLayouts.end(), this), g.initialLayouts.end());
	for (KeptView &kept : views_)
	{
		ForgetFramebuffers(kept.view);
		vkDestroyImageView(g.device, kept.view, nullptr);
	}
	if (buffer)
	{
		g.buffers.erase(address);
		vkDestroyBuffer(g.device, buffer, nullptr);
	}
	if (image && !swapchain)
		vkDestroyImage(g.device, image, nullptr);
	if (memory)
		vkFreeMemory(g.device, memory, nullptr); // unmaps it
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
	if (buffer)
		SetDebugName(VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, name_);
	else if (image)
		SetDebugName(VK_OBJECT_TYPE_IMAGE, (uint64_t)image, name_);
}

VkImageView Resource::View(VkFormat viewFormat, VkImageViewType type, VkImageAspectFlags aspect, uint32_t mips)
{
	for (const KeptView &kept : views_)
		if (kept.format == viewFormat && kept.type == type && kept.aspect == aspect && kept.mips == mips)
			return kept.view;
	VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	info.image = image;
	info.viewType = type;
	info.format = viewFormat;
	info.subresourceRange.aspectMask = aspect;
	info.subresourceRange.levelCount = mips;
	info.subresourceRange.layerCount = 1;
	VkImageView view = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateImageView(g.device, &info, nullptr, &view), "vkCreateImageView")))
		return VK_NULL_HANDLE;
	views_.push_back({viewFormat, type, aspect, mips, view});
	return view;
}

Resource *NullImage(DXGI_FORMAT format, bool volume)
{
	for (Resource *image : g.nullImages)
		if (image->desc.Format == format && (image->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) == volume)
			return image;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	Resource *image = nullptr;
	// A null view reads as zero in Direct3D 12, so the image is cleared to zero with its first layout
	if (FAILED(CreateResource(&heap, nullptr, 0, desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &image)))
		return nullptr;
	image->zeroFill = true;
	image->SetName(L"null view");
	g.nullImages.push_back(image);
	return image;
}

bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, VkBuffer *buffer, VkDeviceSize *offset)
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

VkRenderPass RenderPass(const PassKey &key)
{
	// Prewarming creates pipelines on a worker while the driver begins its first pass.
	// Serialize lookup and creation together: replacing the same key leaks the losing pass.
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);
	auto found = g.passes.find(key);
	if (found != g.passes.end())
		return found->second;

	// Direct3D has no passes: everything already in the targets is kept, and they stay in the states (layouts) the
	// driver put them in
	VkAttachmentDescription attachments[2] = {};
	VkAttachmentReference color = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference depth = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	uint32_t count = 0;
	if (key.color != VK_FORMAT_UNDEFINED)
	{
		attachments[count].format = key.color;
		attachments[count].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[count].initialLayout = attachments[count].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.attachment = count++;
	}
	if (key.depth != VK_FORMAT_UNDEFINED)
	{
		attachments[count].format = key.depth;
		attachments[count].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[count].loadOp = attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[count].storeOp = attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[count].initialLayout = attachments[count].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.attachment = count++;
	}
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = key.color != VK_FORMAT_UNDEFINED ? 1 : 0;
	subpass.pColorAttachments = &color;
	subpass.pDepthStencilAttachment = key.depth != VK_FORMAT_UNDEFINED ? &depth : nullptr;
	VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	info.attachmentCount = count;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	VkRenderPass pass = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateRenderPass(g.device, &info, nullptr, &pass), "vkCreateRenderPass")))
		return VK_NULL_HANDLE;
	g.passes[key] = pass;
	return pass;
}

VkFramebuffer Framebuffer(VkRenderPass pass, const Slot *color, const Slot *depth, VkExtent2D extent)
{
	const Context::FramebufferKey key = {pass, color ? color->view : VK_NULL_HANDLE, depth ? depth->view : VK_NULL_HANDLE};
	auto found = g.framebuffers.find(key);
	if (found != g.framebuffers.end())
		return found->second;
	VkImageView views[2];
	uint32_t count = 0;
	if (color)
		views[count++] = color->view;
	if (depth)
		views[count++] = depth->view;
	VkFramebufferCreateInfo info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	info.renderPass = pass;
	info.attachmentCount = count;
	info.pAttachments = views;
	info.width = extent.width;
	info.height = extent.height;
	info.layers = 1;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateFramebuffer(g.device, &info, nullptr, &framebuffer), "vkCreateFramebuffer")))
		return VK_NULL_HANDLE;
	g.framebuffers[key] = framebuffer;
	return framebuffer;
}

void ForgetFramebuffers(VkImageView view)
{
	for (auto it = g.framebuffers.begin(); it != g.framebuffers.end();)
	{
		if (it->first.color == view || it->first.depth == view)
		{
			vkDestroyFramebuffer(g.device, it->second, nullptr);
			it = g.framebuffers.erase(it);
		}
		else
			++it;
	}
}

} // namespace rr_vk12
