// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_commands.cpp
/// \brief vk12's command allocators and the command list, which records Direct3D 12's calls as Vulkan commands
///
/// Direct3D 12 draws to whatever targets are bound; Vulkan draws inside render passes. The list begins a pass with
/// the first draw or clear after the targets are set and ends it before anything that can't be in one (a copy, a
/// layout change, other targets). Bindings are gathered as the driver sets them and reach Vulkan with the draw.

#include "vk12_internal.h"

#include <algorithm>

namespace rr_vk12
{

// ---------------------------------------------------------------------------------------------------------------
// Allocators

HRESULT Allocator::Init()
{
	VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	info.queueFamilyIndex = g.queueFamily;
	return Result(vkCreateCommandPool(g.device, &info, nullptr, &pool_), "vkCreateCommandPool");
}

Allocator::~Allocator()
{
	if (!g.device)
		return;
	for (VkDescriptorPool pool : setPools_)
		vkDestroyDescriptorPool(g.device, pool, nullptr);
	if (pool_)
		vkDestroyCommandPool(g.device, pool_, nullptr);
}

HRESULT Allocator::Reset()
{
	const HRESULT hr = Result(vkResetCommandPool(g.device, pool_, 0), "vkResetCommandPool");
	usedBuffers_ = 0;
	for (VkDescriptorPool pool : setPools_)
		vkResetDescriptorPool(g.device, pool, 0);
	currentSetPool_ = 0;
	return hr;
}

VkCommandBuffer Allocator::TakeBuffer()
{
	if (usedBuffers_ < buffers_.size())
		return buffers_[usedBuffers_++];
	VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	info.commandPool = pool_;
	info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	info.commandBufferCount = 1;
	VkCommandBuffer buffer = VK_NULL_HANDLE;
	if (FAILED(Result(vkAllocateCommandBuffers(g.device, &info, &buffer), "vkAllocateCommandBuffers")))
		return VK_NULL_HANDLE;
	buffers_.push_back(buffer);
	usedBuffers_++;
	return buffer;
}

VkDescriptorSet Allocator::TakeSet()
{
	const uint32_t kSets = 512;
	for (;;)
	{
		if (currentSetPool_ >= setPools_.size())
		{
			const VkDescriptorPoolSize sizes[] = {
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets * kCbvCount},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * kSrvCount},
				{VK_DESCRIPTOR_TYPE_SAMPLER, kSets * kSamplerCount},
			};
			VkDescriptorPoolCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			info.maxSets = kSets;
			info.poolSizeCount = 3;
			info.pPoolSizes = sizes;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			if (FAILED(Result(vkCreateDescriptorPool(g.device, &info, nullptr, &pool), "vkCreateDescriptorPool")))
				return VK_NULL_HANDLE;
			setPools_.push_back(pool);
		}
		VkDescriptorSetAllocateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		info.descriptorPool = setPools_[currentSetPool_];
		info.descriptorSetCount = 1;
		info.pSetLayouts = &g.setLayout;
		VkDescriptorSet set = VK_NULL_HANDLE;
		const VkResult result = vkAllocateDescriptorSets(g.device, &info, &set);
		if (result == VK_SUCCESS)
			return set;
		if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
		{
			Result(result, "vkAllocateDescriptorSets");
			return VK_NULL_HANDLE;
		}
		currentSetPool_++; // this one is full until the allocator is reset
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Layouts

static void LayoutAccess(VkImageLayout layout, bool source, VkAccessFlags *access, VkPipelineStageFlags *stage)
{
	switch (layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED:
			*access = 0;
			*stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			*access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			*stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			*access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			*stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			*access = VK_ACCESS_SHADER_READ_BIT;
			*stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			*access = VK_ACCESS_TRANSFER_READ_BIT;
			*stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			*access = VK_ACCESS_TRANSFER_WRITE_BIT;
			*stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			// Leaving it: after the stages the submission's wait for the acquired image covers
			*access = 0;
			*stage = source ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
				: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;
		default:
			*access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			*stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}
}

static void Transition(VkCommandBuffer cmd, Resource *resource, VkImageLayout to)
{
	VkImageLayout from = resource->layout;
	if (from == to)
		return;
	VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	VkPipelineStageFlags sourceStage, targetStage;
	LayoutAccess(from, true, &barrier.srcAccessMask, &sourceStage);
	LayoutAccess(to, false, &barrier.dstAccessMask, &targetStage);
	if (resource->swapchain && from == VK_IMAGE_LAYOUT_UNDEFINED)
		sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = resource->image;
	barrier.subresourceRange.aspectMask = resource->format.aspect;
	barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	vkCmdPipelineBarrier(cmd, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	resource->layout = to;
}

// ---------------------------------------------------------------------------------------------------------------
// The list

HRESULT List::Init(Allocator *allocator)
{
	return Reset(allocator, nullptr);
}

HRESULT List::Reset(ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial)
{
	if (open_ || !allocator)
		return E_FAIL;
	allocator_ = static_cast<Allocator *>(allocator);
	cmd_ = allocator_->TakeBuffer();
	if (!cmd_)
		return E_OUTOFMEMORY;
	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	const HRESULT hr = Result(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");
	if (FAILED(hr))
		return hr;
	open_ = true;
	ResetState();
	pipeline_ = static_cast<Pipeline *>(initial);
	return S_OK;
}

HRESULT List::Close()
{
	if (!open_)
		return E_FAIL;
	EndPass();
	// An image made but never used still has to reach the layout its state names before another list assumes it
	FlushInitialLayouts();
	open_ = false;
	return Result(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
}

void List::ResetState()
{
	color_ = depth_ = Slot();
	hasColor_ = hasDepth_ = inPass_ = false;
	pipeline_ = nullptr;
	boundPipeline_ = VK_NULL_HANDLE;
	topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	viewport_ = {};
	scissor_ = {};
	stencilRef_ = 0;
	viewportDirty_ = scissorDirty_ = stencilDirty_ = true;
	for (BufferBinding &binding : cbv_)
		binding = BufferBinding();
	for (Slot &slot : srv_)
		slot = Slot();
	for (Slot &slot : sampler_)
		slot = Slot();
	setDirty_ = true;
	set_ = VK_NULL_HANDLE;
	vertex_ = index_ = BufferBinding();
	vertexStride_ = 0;
	vertexDirty_ = indexDirty_ = true;
}

void List::EndPass()
{
	if (inPass_)
	{
		vkCmdEndRenderPass(cmd_);
		inPass_ = false;
	}
}

void List::FlushInitialLayouts()
{
	if (g.initialLayouts.empty())
		return;
	EndPass();
	for (Resource *resource : g.initialLayouts)
	{
		resource->pendingInitialLayout = false;
		if (resource->layout != VK_IMAGE_LAYOUT_UNDEFINED)
			continue; // a barrier already moved it
		if (resource->zeroFill)
		{
			Transition(cmd_, resource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			const VkClearColorValue zero = {};
			VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
			vkCmdClearColorImage(cmd_, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
		}
		Transition(cmd_, resource, LayoutOf(resource, resource->initialState));
	}
	g.initialLayouts.clear();
}

bool List::BeginPass()
{
	FlushInitialLayouts();
	if (inPass_)
		return true;
	if (!hasColor_ && !hasDepth_)
		return false;
	passKey_ = {hasColor_ ? color_.format : VK_FORMAT_UNDEFINED, hasDepth_ ? depth_.format : VK_FORMAT_UNDEFINED};
	passExtent_ = hasColor_ ? VkExtent2D{color_.width, color_.height} : VkExtent2D{depth_.width, depth_.height};
	if (hasColor_ && hasDepth_)
	{
		passExtent_.width = std::min(passExtent_.width, depth_.width);
		passExtent_.height = std::min(passExtent_.height, depth_.height);
	}
	const VkRenderPass pass = RenderPass(passKey_);
	const VkFramebuffer framebuffer = pass ? Framebuffer(pass, hasColor_ ? &color_ : nullptr, hasDepth_ ? &depth_ : nullptr, passExtent_)
		: VK_NULL_HANDLE;
	if (!framebuffer)
		return false;
	VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	begin.renderPass = pass;
	begin.framebuffer = framebuffer;
	begin.renderArea.extent = passExtent_;
	vkCmdBeginRenderPass(cmd_, &begin, VK_SUBPASS_CONTENTS_INLINE);
	inPass_ = true;
	return true;
}

void List::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *targets, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *depth)
{
	// The views are read now: the driver may rewrite the slots right after
	Slot color = count && targets ? *SlotOf(targets[0]) : Slot();
	Slot depthSlot = depth ? *SlotOf(*depth) : Slot();
	if (color.view == color_.view && depthSlot.view == depth_.view && hasColor_ == (color.view != VK_NULL_HANDLE)
		&& hasDepth_ == (depthSlot.view != VK_NULL_HANDLE))
		return;
	EndPass();
	color_ = color;
	depth_ = depthSlot;
	hasColor_ = color_.view != VK_NULL_HANDLE;
	hasDepth_ = depth_.view != VK_NULL_HANDLE;
	if (count > 1)
		VK12_LOG_ONCE("OMSetRenderTargets: only one render target is supported");
}

void List::ClearAttachment(const Slot &target, bool color, VkImageAspectFlags aspect, const VkClearValue &value,
	UINT rects, const D3D12_RECT *rect)
{
	if (!target.view)
		return;
	const bool bound = color ? hasColor_ && color_.view == target.view : hasDepth_ && depth_.view == target.view;
	bool temporary = false;
	uint32_t attachment = 0;
	if (bound)
	{
		if (!BeginPass())
			return;
	}
	else
	{
		// Not one of the bound targets: a pass of its own around the clear
		EndPass();
		FlushInitialLayouts();
		const PassKey key = {color ? target.format : VK_FORMAT_UNDEFINED, color ? VK_FORMAT_UNDEFINED : target.format};
		const VkExtent2D extent = {target.width, target.height};
		const VkRenderPass pass = RenderPass(key);
		const VkFramebuffer framebuffer = pass ? Framebuffer(pass, color ? &target : nullptr, color ? nullptr : &target, extent)
			: VK_NULL_HANDLE;
		if (!framebuffer)
			return;
		VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = pass;
		begin.framebuffer = framebuffer;
		begin.renderArea.extent = extent;
		vkCmdBeginRenderPass(cmd_, &begin, VK_SUBPASS_CONTENTS_INLINE);
		temporary = true;
	}

	VkClearAttachment clear = {};
	clear.aspectMask = aspect;
	clear.colorAttachment = attachment;
	clear.clearValue = value;
	VkClearRect area = {};
	area.layerCount = 1;
	if (rects && rect)
	{
		area.rect.offset = {(int32_t)rect->left, (int32_t)rect->top};
		area.rect.extent = {(uint32_t)(rect->right - rect->left), (uint32_t)(rect->bottom - rect->top)};
	}
	else
		area.rect.extent = temporary ? VkExtent2D{target.width, target.height} : passExtent_;
	vkCmdClearAttachments(cmd_, 1, &clear, 1, &area);
	if (temporary)
		vkCmdEndRenderPass(cmd_);
}

void List::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT rects, const D3D12_RECT *rect)
{
	VkClearValue value = {};
	memcpy(value.color.float32, color, sizeof value.color.float32);
	ClearAttachment(*SlotOf(view), true, VK_IMAGE_ASPECT_COLOR_BIT, value, rects, rect);
}

void List::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
	UINT rects, const D3D12_RECT *rect)
{
	VkClearValue value = {};
	value.depthStencil = {depth, stencil};
	VkImageAspectFlags aspect = 0;
	if (flags & D3D12_CLEAR_FLAG_DEPTH)
		aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (flags & D3D12_CLEAR_FLAG_STENCIL)
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if (aspect)
		ClearAttachment(*SlotOf(view), false, aspect, value, rects, rect);
}

void List::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers)
{
	bool any = false;
	for (UINT i = 0; i < count; i++)
	{
		if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
			continue; // aliasing needs nothing: each placed image is its own VkImage
		Resource *resource = static_cast<Resource *>(barriers[i].Transition.pResource);
		if (!resource || !resource->image)
			continue;
		if (!any)
		{
			EndPass();
			FlushInitialLayouts();
			any = true;
		}
		Transition(cmd_, resource, LayoutOf(resource, barriers[i].Transition.StateAfter));
	}
}

void List::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	switch (topology)
	{
		case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		case D3D_PRIMITIVE_TOPOLOGY_LINELIST: topology_ = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: topology_ = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
		case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: topology_ = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
		default: topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
	}
}

void List::RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports)
{
	if (!count || !viewports)
		return;
	// Upside down, which makes Vulkan's clip space Direct3D's
	viewport_.x = viewports->TopLeftX;
	viewport_.y = viewports->TopLeftY + viewports->Height;
	viewport_.width = viewports->Width;
	viewport_.height = -viewports->Height;
	viewport_.minDepth = viewports->MinDepth;
	viewport_.maxDepth = viewports->MaxDepth;
	viewportDirty_ = true;
}

void List::RSSetScissorRects(UINT count, const D3D12_RECT *rects)
{
	if (!count || !rects)
		return;
	scissor_.offset = {std::max<int32_t>(0, rects->left), std::max<int32_t>(0, rects->top)};
	scissor_.extent = {(uint32_t)std::max<int32_t>(0, rects->right - scissor_.offset.x),
		(uint32_t)std::max<int32_t>(0, rects->bottom - scissor_.offset.y)};
	scissorDirty_ = true;
}

void List::OMSetBlendFactor(const FLOAT[4])
{
}

void List::OMSetStencilRef(UINT ref)
{
	stencilRef_ = ref;
	stencilDirty_ = true;
}

void List::SetPipelineState(ID3D12PipelineState *pipeline)
{
	pipeline_ = static_cast<Pipeline *>(pipeline);
}

void List::SetGraphicsRootDescriptorTable(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
	if (parameter >= kRootParameters || !base.ptr)
		return;
	const RootParameter &root = kRoot[parameter];
	if (root.kind == RootParameter::CBV)
		return;
	Slot &bound = root.kind == RootParameter::SRV ? srv_[root.index] : sampler_[root.index];
	const Slot &slot = *SlotOf(base);
	if (bound.view != slot.view || bound.sampler != slot.sampler)
		setDirty_ = true;
	bound = slot;
}

void List::SetGraphicsRootConstantBufferView(UINT parameter, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (parameter >= kRootParameters || kRoot[parameter].kind != RootParameter::CBV)
		return;
	BufferBinding &bound = cbv_[kRoot[parameter].index];
	BufferBinding found;
	if (!FindBuffer(address, &found.buffer, &found.offset))
	{
		VK12_LOG_ONCE("SetGraphicsRootConstantBufferView: address 0x%llx is in no buffer", (unsigned long long)address);
		return;
	}
	if (bound.buffer != found.buffer)
		setDirty_ = true;
	bound = found;
}

void List::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view)
{
	index_ = BufferBinding();
	if (view && FindBuffer(view->BufferLocation, &index_.buffer, &index_.offset))
		indexType_ = view->Format == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
	indexDirty_ = true;
}

void List::IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (start || count > 1)
		VK12_LOG_ONCE("IASetVertexBuffers: only slot 0 is supported");
	vertex_ = BufferBinding();
	vertexStride_ = 0;
	if (count && views && FindBuffer(views->BufferLocation, &vertex_.buffer, &vertex_.offset))
		vertexStride_ = views->StrideInBytes;
	vertexDirty_ = true;
}

bool List::PrepareDraw()
{
	if (!open_ || !pipeline_ || !BeginPass())
		return false;

	const VkPipeline pipeline = pipeline_->Get(topology_, passKey_);
	if (!pipeline)
		return false;
	if (pipeline != boundPipeline_)
	{
		vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		boundPipeline_ = pipeline;
	}
	if (viewportDirty_)
	{
		vkCmdSetViewport(cmd_, 0, 1, &viewport_);
		viewportDirty_ = false;
	}
	if (scissorDirty_)
	{
		vkCmdSetScissor(cmd_, 0, 1, &scissor_);
		scissorDirty_ = false;
	}
	if (stencilDirty_)
	{
		vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef_);
		stencilDirty_ = false;
	}

	// The set holds what the last pipeline that needed a new one used. Another pipeline can draw with it if it finds
	// everything it declares there, with uniform blocks of the same sizes.
	bool write = setDirty_ || !set_;
	for (unsigned i = 0; i < kCbvCount && !write; i++)
		write = pipeline_->uniformBytes[i] && (!setWritten_[i] || setRanges_[i] != pipeline_->uniformBytes[i]);
	for (unsigned i = 0; i < kSrvCount && !write; i++)
		write = pipeline_->usesBinding[kSrvBinding + i] && !setWritten_[kSrvBinding + i];
	for (unsigned i = 0; i < kSamplerCount && !write; i++)
		write = pipeline_->usesBinding[kSamplerBinding + i] && !setWritten_[kSamplerBinding + i];
	if (write)
	{
		set_ = allocator_->TakeSet();
		if (!set_)
			return false;
		VkWriteDescriptorSet writes[kCbvCount + kSrvCount + kSamplerCount];
		VkDescriptorBufferInfo buffers[kCbvCount];
		VkDescriptorImageInfo images[kSrvCount + kSamplerCount];
		uint32_t count = 0, imageCount = 0;
		memset(setWritten_, 0, sizeof setWritten_);
		for (unsigned i = 0; i < kCbvCount; i++)
		{
			setRanges_[i] = pipeline_->uniformBytes[i];
			if (!pipeline_->uniformBytes[i])
				continue;
			if (!cbv_[i].buffer)
			{
				VK12_LOG_ONCE("draw: the shaders use constant buffer b%u, which isn't bound", i);
				continue;
			}
			buffers[i] = {cbv_[i].buffer, 0, pipeline_->uniformBytes[i]};
			writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[count].dstSet = set_;
			writes[count].dstBinding = i;
			writes[count].descriptorCount = 1;
			writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			writes[count++].pBufferInfo = &buffers[i];
			setWritten_[i] = true;
		}
		for (unsigned i = 0; i < kSrvCount; i++)
		{
			if (!pipeline_->usesBinding[kSrvBinding + i])
				continue;
			if (!srv_[i].view)
			{
				VK12_LOG_ONCE("draw: the shaders use texture t%u, which isn't bound", i);
				continue;
			}
			images[imageCount] = {VK_NULL_HANDLE, srv_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[count].dstSet = set_;
			writes[count].dstBinding = kSrvBinding + i;
			writes[count].descriptorCount = 1;
			writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			writes[count++].pImageInfo = &images[imageCount++];
			setWritten_[kSrvBinding + i] = true;
		}
		for (unsigned i = 0; i < kSamplerCount; i++)
		{
			if (!pipeline_->usesBinding[kSamplerBinding + i])
				continue;
			if (!sampler_[i].sampler)
			{
				VK12_LOG_ONCE("draw: the shaders use sampler s%u, which isn't bound", i);
				continue;
			}
			images[imageCount] = {sampler_[i].sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
			writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[count].dstSet = set_;
			writes[count].dstBinding = kSamplerBinding + i;
			writes[count].descriptorCount = 1;
			writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			writes[count++].pImageInfo = &images[imageCount++];
			setWritten_[kSamplerBinding + i] = true;
		}
		vkUpdateDescriptorSets(g.device, count, writes, 0, nullptr);
		setDirty_ = false;
	}
	uint32_t offsets[kCbvCount];
	for (unsigned i = 0; i < kCbvCount; i++)
		offsets[i] = setWritten_[i] ? (uint32_t)cbv_[i].offset : 0;
	vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipelineLayout, 0, 1, &set_, kCbvCount, offsets);

	if (vertexDirty_ && vertex_.buffer)
	{
		vkCmdBindVertexBuffers(cmd_, 0, 1, &vertex_.buffer, &vertex_.offset);
		vertexDirty_ = false;
	}
	if (pipeline_->stride && vertexStride_ && pipeline_->stride != vertexStride_)
		VK12_LOG_ONCE("draw: the vertex buffer's stride is %u, the input layout's %u", (unsigned)vertexStride_, (unsigned)pipeline_->stride);
	return true;
}

void List::DrawInstanced(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance)
{
	if (PrepareDraw())
		vkCmdDraw(cmd_, vertices, instances, firstVertex, firstInstance);
}

void List::DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance)
{
	if (!PrepareDraw() || !index_.buffer)
		return;
	if (indexDirty_)
	{
		vkCmdBindIndexBuffer(cmd_, index_.buffer, index_.offset, indexType_);
		indexDirty_ = false;
	}
	vkCmdDrawIndexed(cmd_, indices, instances, firstIndex, baseVertex, firstInstance);
}

// ---------------------------------------------------------------------------------------------------------------
// Copies

static VkImageSubresourceLayers Layers(const Resource *resource, UINT subresource)
{
	// The driver copies color only, one mip at a time, of textures with one layer
	return {resource->format.aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
		subresource, 0, 1};
}

static VkExtent3D MipExtent(const Resource *resource, UINT mip)
{
	const bool volume = resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	return {std::max<uint32_t>(1, (uint32_t)resource->desc.Width >> mip), std::max<uint32_t>(1, resource->desc.Height >> mip),
		volume ? std::max<uint32_t>(1, (uint32_t)resource->desc.DepthOrArraySize >> mip) : 1};
}

void List::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
	const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
	if (!dst || !src)
		return;
	Resource *to = static_cast<Resource *>(dst->pResource);
	Resource *from = static_cast<Resource *>(src->pResource);
	if (!to || !from)
		return;
	EndPass();
	FlushInitialLayouts();

	const bool toBuffer = dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	const bool fromBuffer = src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	if (fromBuffer && !toBuffer)
	{
		const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &foot = src->PlacedFootprint;
		const unsigned texel = std::max(1u, Format(foot.Footprint.Format).bytes);
		VkBufferImageCopy region = {};
		region.bufferOffset = foot.Offset;
		region.bufferRowLength = foot.Footprint.RowPitch / texel;
		region.bufferImageHeight = foot.Footprint.Height;
		region.imageSubresource = Layers(to, dst->SubresourceIndex);
		region.imageOffset = {(int32_t)x, (int32_t)y, (int32_t)z};
		region.imageExtent = {foot.Footprint.Width, foot.Footprint.Height, foot.Footprint.Depth};
		if (box)
		{
			region.bufferOffset += (UINT64)box->front * foot.Footprint.RowPitch * foot.Footprint.Height
				+ (UINT64)box->top * foot.Footprint.RowPitch + (UINT64)box->left * texel;
			region.imageExtent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
		}
		vkCmdCopyBufferToImage(cmd_, from->buffer, to->image, to->layout, 1, &region);
	}
	else if (toBuffer && !fromBuffer)
	{
		const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &foot = dst->PlacedFootprint;
		const unsigned texel = std::max(1u, Format(foot.Footprint.Format).bytes);
		VkBufferImageCopy region = {};
		region.bufferOffset = foot.Offset + (UINT64)z * foot.Footprint.RowPitch * foot.Footprint.Height
			+ (UINT64)y * foot.Footprint.RowPitch + (UINT64)x * texel;
		region.bufferRowLength = foot.Footprint.RowPitch / texel;
		region.bufferImageHeight = foot.Footprint.Height;
		region.imageSubresource = Layers(from, src->SubresourceIndex);
		region.imageExtent = MipExtent(from, src->SubresourceIndex);
		if (box)
		{
			region.imageOffset = {(int32_t)box->left, (int32_t)box->top, (int32_t)box->front};
			region.imageExtent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
		}
		vkCmdCopyImageToBuffer(cmd_, from->image, from->layout, to->buffer, 1, &region);
	}
	else if (!toBuffer && !fromBuffer)
	{
		VkImageCopy region = {};
		region.srcSubresource = Layers(from, src->SubresourceIndex);
		region.dstSubresource = Layers(to, dst->SubresourceIndex);
		region.dstOffset = {(int32_t)x, (int32_t)y, (int32_t)z};
		region.extent = MipExtent(from, src->SubresourceIndex);
		if (box)
		{
			region.srcOffset = {(int32_t)box->left, (int32_t)box->top, (int32_t)box->front};
			region.extent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
		}
		vkCmdCopyImage(cmd_, from->image, from->layout, to->image, to->layout, 1, &region);
	}
	else
		VK12_LOG_ONCE("CopyTextureRegion: buffer to buffer isn't a texture copy");
}

void List::CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
{
	Resource *to = static_cast<Resource *>(dst);
	Resource *from = static_cast<Resource *>(src);
	if (!to || !from)
		return;
	EndPass();
	FlushInitialLayouts();
	if (to->buffer && from->buffer)
	{
		const VkBufferCopy region = {0, 0, std::min(to->bytes, from->bytes)};
		vkCmdCopyBuffer(cmd_, from->buffer, to->buffer, 1, &region);
		return;
	}
	if (!to->image || !from->image)
		return;
	const UINT mips = std::min<UINT>(std::max<UINT>(1, to->desc.MipLevels), std::max<UINT>(1, from->desc.MipLevels));
	for (UINT mip = 0; mip < mips; mip++)
	{
		VkImageCopy region = {};
		region.srcSubresource = Layers(from, mip);
		region.dstSubresource = Layers(to, mip);
		region.extent = MipExtent(from, mip);
		vkCmdCopyImage(cmd_, from->image, from->layout, to->image, to->layout, 1, &region);
	}
}

void List::CopyBufferRegion(ID3D12Resource *dst, UINT64 dstOffset, ID3D12Resource *src, UINT64 srcOffset, UINT64 bytes)
{
	Resource *to = static_cast<Resource *>(dst);
	Resource *from = static_cast<Resource *>(src);
	if (!to || !from || !to->buffer || !from->buffer || !bytes)
		return;
	EndPass();
	const VkBufferCopy region = {srcOffset, dstOffset, bytes};
	vkCmdCopyBuffer(cmd_, from->buffer, to->buffer, 1, &region);
}

// ---------------------------------------------------------------------------------------------------------------
// Timestamps

void List::EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
	QueryHeap *queries = static_cast<QueryHeap *>(heap);
	if (!queries || type != D3D12_QUERY_TYPE_TIMESTAMP || index >= queries->count)
		return;
	EndPass();
	vkCmdResetQueryPool(cmd_, queries->pool, index, 1);
	vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries->pool, index);
}

void List::ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst, UINT64 offset)
{
	QueryHeap *queries = static_cast<QueryHeap *>(heap);
	Resource *to = static_cast<Resource *>(dst);
	if (!queries || !to || !to->buffer || type != D3D12_QUERY_TYPE_TIMESTAMP || start + count > queries->count)
		return;
	EndPass();
	vkCmdCopyQueryPoolResults(cmd_, queries->pool, start, count, to->buffer, offset, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
}

} // namespace rr_vk12
