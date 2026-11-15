// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_commands.mm
/// \brief mtl12's command list, which records Direct3D 12's calls into a Metal command buffer
///
/// Direct3D 12 draws to whatever targets are bound, clears them whenever it is told to, and copies between draws.
/// Metal draws inside render passes, clears a target as a pass begins (its load action), and copies with another
/// encoder, of which a command buffer has one open at a time. So the list begins a pass with the first draw after the
/// targets are set, loading and storing everything, and ends it before anything that can't be in one (a copy, a clear,
/// other targets). A clear waits for the next pass on its target and becomes that pass's load action; if the target is
/// used otherwise first (copied from, handed to a shader, the list closed), it gets a pass of its own with no draws.
/// Bindings are gathered as the driver sets them and reach the encoder with the draw.

#include "mtl12_internal.h"

#include <algorithm>

namespace rr_mtl12
{

// ---------------------------------------------------------------------------------------------------------------
// The list

List::~List()
{
	// A list released while open (the driver shutting down) never commits its command buffer, but Metal still wants
	// every encoder ended
	EndPass();
	EndBlit();
}

HRESULT List::Reset(ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial)
{
	if (open_ || !allocator || !g.queue)
		return E_FAIL;
	@autoreleasepool
	{
		cmd_ = NewCommandBuffer();
	}
	if (!cmd_)
		return E_OUTOFMEMORY;
	open_ = true;
	ResetState();
	pipeline_ = static_cast<Pipeline *>(initial);
	return S_OK;
}

HRESULT List::Close()
{
	if (!open_)
		return E_FAIL;
	@autoreleasepool
	{
		EndPass();
		EndBlit();
		// A clear that no pass took still has to happen
		FlushClears(nil, nil);
	}
	open_ = false;
	return S_OK;
}

id<MTLCommandBuffer> List::TakeBuffer()
{
	id<MTLCommandBuffer> buffer = cmd_;
	cmd_ = nil;
	return buffer;
}

void List::ResetState()
{
	color_ = depth_ = Slot();
	hasColor_ = hasDepth_ = false;
	passKey_ = {MTLPixelFormatInvalid, MTLPixelFormatInvalid};
	passWidth_ = passHeight_ = 0;
	clears_.clear();
	pipeline_ = nullptr;
	topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	viewport_ = {};
	scissor_ = {};
	stencilRef_ = 0;
	// Direct3D 12's blend factor is white until it is set
	for (float &factor : blendFactor_)
		factor = 1.0f;
	for (BufferBinding &binding : cbv_)
		binding = BufferBinding();
	for (Slot &slot : srv_)
		slot = Slot();
	for (Slot &slot : sampler_)
		slot = Slot();
	vertex_ = index_ = BufferBinding();
	vertexStride_ = 0;
	indexType_ = MTLIndexTypeUInt16;
	ResetEncoderState();
}

void List::ResetEncoderState()
{
	encoder_ = EncoderState();
}

void List::EndPass()
{
	if (pass_)
	{
		[pass_ endEncoding];
		pass_ = nil;
	}
}

void List::EndBlit()
{
	if (blit_)
	{
		[blit_ endEncoding];
		blit_ = nil;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Clears

List::PendingClear *List::FindClear(id<MTLTexture> texture)
{
	for (PendingClear &clear : clears_)
		if (clear.texture == texture)
			return &clear;
	return nullptr;
}

List::PendingClear &List::AddClear(const Slot &target)
{
	// A pass that draws to the texture would load what the clear is about to replace
	if (pass_ && ((hasColor_ && color_.texture == target.texture) || (hasDepth_ && depth_.texture == target.texture)))
		EndPass();
	PendingClear *found = FindClear(target.texture);
	if (found)
		return *found;
	PendingClear clear;
	clear.texture = target.texture;
	clears_.push_back(clear);
	return clears_.back();
}

// Makes the texture's waiting clear the load action of its attachment in pass
void List::TakeClear(id<MTLTexture> texture, MTLRenderPassDescriptor *pass)
{
	PendingClear *clear = FindClear(texture);
	if (!clear)
		return;
	if (clear->color)
	{
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].clearColor = clear->colorValue;
	}
	if (clear->depth)
	{
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.clearDepth = clear->depthValue;
	}
	if (clear->stencil)
	{
		pass.stencilAttachment.loadAction = MTLLoadActionClear;
		pass.stencilAttachment.clearStencil = clear->stencilValue;
	}
	clears_.erase(clears_.begin() + (clear - clears_.data()));
}

static MTLRenderPassDescriptor *PassFor(id<MTLTexture> color, id<MTLTexture> depth)
{
	// Direct3D has no passes: everything already in the targets is kept, and everything drawn stays
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	if (color)
	{
		pass.colorAttachments[0].texture = color;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	}
	if (depth)
	{
		// The one depth format has depth and stencil in one texture
		pass.depthAttachment.texture = depth;
		pass.depthAttachment.loadAction = MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.texture = depth;
		pass.stencilAttachment.loadAction = MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
	}
	return pass;
}

// The texture's waiting clear, in a pass of its own: something other than a draw to it is about to use the texture
void List::FlushClear(id<MTLTexture> texture)
{
	PendingClear *clear = texture ? FindClear(texture) : nullptr;
	if (!clear)
		return;
	EndPass();
	EndBlit();
	const bool depthStencil = clear->depth || clear->stencil;
	MTLRenderPassDescriptor *pass = PassFor(depthStencil ? nil : texture, depthStencil ? texture : nil);
	TakeClear(texture, pass);
	id<MTLRenderCommandEncoder> empty = [cmd_ renderCommandEncoderWithDescriptor:pass];
	empty.label = @"clear";
	[empty endEncoding];
}

void List::FlushClears(id<MTLTexture> exceptFirst, id<MTLTexture> exceptSecond)
{
	for (size_t i = 0; i < clears_.size();)
	{
		id<MTLTexture> texture = clears_[i].texture;
		if (texture == exceptFirst || texture == exceptSecond)
			i++;
		else
			FlushClear(texture); // removes it
	}
}

void List::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT rects, const D3D12_RECT *)
{
	const Slot target = *SlotOf(view);
	if (!open_ || !target.texture)
		return;
	if (rects)
	{
		MTL12_LOG_ONCE("ClearRenderTargetView: clearing part of a target is not implemented by mtl12");
		return;
	}
	PendingClear &clear = AddClear(target);
	clear.color = true;
	clear.colorValue = MTLClearColorMake(color[0], color[1], color[2], color[3]);
}

void List::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
	UINT rects, const D3D12_RECT *)
{
	const Slot target = *SlotOf(view);
	if (!open_ || !target.texture || !(flags & (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)))
		return;
	if (rects)
	{
		MTL12_LOG_ONCE("ClearDepthStencilView: clearing part of a target is not implemented by mtl12");
		return;
	}
	PendingClear &clear = AddClear(target);
	if (flags & D3D12_CLEAR_FLAG_DEPTH)
	{
		clear.depth = true;
		clear.depthValue = depth;
	}
	if (flags & D3D12_CLEAR_FLAG_STENCIL)
	{
		clear.stencil = true;
		clear.stencilValue = stencil;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Passes and state

bool List::BeginPass()
{
	if (pass_)
		return true;
	if (!open_ || (!hasColor_ && !hasDepth_))
		return false;
	EndBlit();
	@autoreleasepool
	{
		id<MTLTexture> color = hasColor_ ? color_.texture : nil;
		id<MTLTexture> depth = hasDepth_ ? depth_.texture : nil;
		// Clears of other textures can't wait inside this pass: it may sample them
		FlushClears(color, depth);
		MTLRenderPassDescriptor *pass = PassFor(color, depth);
		if (color)
			TakeClear(color, pass);
		if (depth)
			TakeClear(depth, pass);
		pass_ = [cmd_ renderCommandEncoderWithDescriptor:pass];
	}
	if (!pass_)
		return false;
	passDraws_ = 0;
	passKey_ = {hasColor_ ? color_.format : MTLPixelFormatInvalid, hasDepth_ ? depth_.format : MTLPixelFormatInvalid};
	passWidth_ = hasColor_ ? color_.width : depth_.width;
	passHeight_ = hasColor_ ? color_.height : depth_.height;
	if (hasColor_ && hasDepth_)
	{
		passWidth_ = std::min(passWidth_, depth_.width);
		passHeight_ = std::min(passHeight_, depth_.height);
	}
	ResetEncoderState();
	return true;
}

void List::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *targets, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *depth)
{
	// The views are read now: the driver may rewrite the slots right after
	Slot color = count && targets ? *SlotOf(targets[0]) : Slot();
	Slot depthSlot = depth ? *SlotOf(*depth) : Slot();
	if (color.texture == color_.texture && depthSlot.texture == depth_.texture && hasColor_ == (color.texture != nil)
		&& hasDepth_ == (depthSlot.texture != nil))
		return;
	EndPass();
	color_ = color;
	depth_ = depthSlot;
	hasColor_ = color_.texture != nil;
	hasDepth_ = depth_.texture != nil;
	if (count > 1)
		MTL12_LOG_ONCE("OMSetRenderTargets: only one render target is supported");
}

void List::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers)
{
	// Metal tracks what its textures are used for by itself. What a transition means here is that a target stops being
	// one: its pass has to end before a shader or a copy reads it, and so does a clear that was waiting for a pass.
	for (UINT i = 0; i < count; i++)
	{
		if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
			continue; // aliasing needs nothing: each placed texture is its own
		Resource *resource = static_cast<Resource *>(barriers[i].Transition.pResource);
		if (!resource || !resource->texture)
			continue;
		const D3D12_RESOURCE_STATES after = barriers[i].Transition.StateAfter;
		if (after & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE))
			continue;
		id<MTLTexture> texture = resource->texture;
		if (pass_ && ((hasColor_ && color_.texture == texture) || (hasDepth_ && depth_.texture == texture)))
			EndPass();
		@autoreleasepool
		{
			FlushClear(texture);
		}
	}
}

void List::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	topology_ = topology;
}

MTLPrimitiveType List::Primitive() const
{
	switch (topology_)
	{
		case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
		case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return MTLPrimitiveTypeLine;
		case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return MTLPrimitiveTypeLineStrip;
		case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return MTLPrimitiveTypePoint;
		default: return MTLPrimitiveTypeTriangle;
	}
}

void List::RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports)
{
	if (!count || !viewports)
		return;
	// As it is: Metal's viewport has Direct3D's origin and depth range
	viewport_.originX = viewports->TopLeftX;
	viewport_.originY = viewports->TopLeftY;
	viewport_.width = viewports->Width;
	viewport_.height = viewports->Height;
	viewport_.znear = viewports->MinDepth;
	viewport_.zfar = viewports->MaxDepth;
}

void List::RSSetScissorRects(UINT count, const D3D12_RECT *rects)
{
	if (!count || !rects)
		return;
	scissor_ = *rects;
}

void List::OMSetBlendFactor(const FLOAT factor[4])
{
	for (int i = 0; i < 4; i++)
		blendFactor_[i] = factor ? factor[i] : 1.0f;
}

void List::OMSetStencilRef(UINT ref)
{
	stencilRef_ = ref;
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
	(root.kind == RootParameter::SRV ? srv_[root.index] : sampler_[root.index]) = *SlotOf(base);
}

void List::SetGraphicsRootConstantBufferView(UINT parameter, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (parameter >= kRootParameters || kRoot[parameter].kind != RootParameter::CBV)
		return;
	BufferBinding found;
	if (!FindBuffer(address, &found.buffer, &found.offset))
	{
		MTL12_LOG_ONCE("SetGraphicsRootConstantBufferView: address 0x%llx is in no buffer", (unsigned long long)address);
		return;
	}
	cbv_[kRoot[parameter].index] = found;
}

void List::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view)
{
	index_ = BufferBinding();
	if (view && FindBuffer(view->BufferLocation, &index_.buffer, &index_.offset))
		indexType_ = view->Format == DXGI_FORMAT_R32_UINT ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

void List::IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (start || count > 1)
		MTL12_LOG_ONCE("IASetVertexBuffers: only slot 0 is supported");
	vertex_ = BufferBinding();
	vertexStride_ = 0;
	if (count && views && FindBuffer(views->BufferLocation, &vertex_.buffer, &vertex_.offset))
		vertexStride_ = views->StrideInBytes;
}

void List::BindBuffer(bool vertexStage, unsigned index, const BufferBinding &binding)
{
	BufferBinding &bound = index == kVertexBufferIndex ? encoder_.vertexData
		: vertexStage ? encoder_.vertexBuffers[index] : encoder_.pixelBuffers[index];
	if (bound.buffer == binding.buffer)
	{
		if (bound.offset == binding.offset)
			return;
		if (vertexStage)
			[pass_ setVertexBufferOffset:(NSUInteger)binding.offset atIndex:index];
		else
			[pass_ setFragmentBufferOffset:(NSUInteger)binding.offset atIndex:index];
	}
	else if (vertexStage)
		[pass_ setVertexBuffer:binding.buffer offset:(NSUInteger)binding.offset atIndex:index];
	else
		[pass_ setFragmentBuffer:binding.buffer offset:(NSUInteger)binding.offset atIndex:index];
	bound = binding;
}

bool List::PrepareDraw()
{
	if (!open_ || !pipeline_)
		return false;

	// Direct3D draws nothing outside the scissor rectangle, and nothing at all before one is set. Metal's has to lie
	// within the targets and to have an area.
	const int64_t left = std::max<int64_t>(0, scissor_.left), top = std::max<int64_t>(0, scissor_.top);
	const uint32_t width = hasColor_ ? color_.width : depth_.width, height = hasColor_ ? color_.height : depth_.height;
	const int64_t right = std::min<int64_t>(scissor_.right, width), bottom = std::min<int64_t>(scissor_.bottom, height);
	if (right <= left || bottom <= top)
		return false;

	// Everything the shaders use has to be there, or Metal's validation stops the program where Direct3D's would
	// only say so
	for (unsigned i = 0; i < kCbvCount; i++)
	{
		if ((pipeline_->vertexUses[i] || pipeline_->pixelUses[i]) && !cbv_[i].buffer)
		{
			MTL12_LOG_ONCE("draw: the shaders use constant buffer b%u, which isn't bound", i);
			return false;
		}
	}
	for (unsigned i = 0; i < kSrvCount; i++)
	{
		if (pipeline_->pixelUses[kSrvBinding + i] && !srv_[i].texture)
		{
			MTL12_LOG_ONCE("draw: the shaders use texture t%u, which isn't bound", i);
			return false;
		}
	}
	for (unsigned i = 0; i < kSamplerCount; i++)
	{
		if (pipeline_->pixelUses[kSamplerBinding + i] && !sampler_[i].sampler)
		{
			MTL12_LOG_ONCE("draw: the shaders use sampler s%u, which isn't bound", i);
			return false;
		}
	}
	if (pipeline_->readsVertices && !vertex_.buffer)
	{
		MTL12_LOG_ONCE("draw: the shaders read vertices, and no vertex buffer is bound");
		return false;
	}

	// A pass that holds as much as the device's driver can take (the paravirtual GPU's) ends here, and the draw begins
	// the next one, which loads what this one stored
	if (g.encoderDraws && pass_ && passDraws_ >= g.encoderDraws)
		EndPass();
	if (!BeginPass())
		return false;
	passDraws_++;
	id<MTLRenderPipelineState> state = pipeline_->Get(passKey_);
	if (!state)
		return false;
	if (state != encoder_.pipeline)
	{
		[pass_ setRenderPipelineState:state];
		encoder_.pipeline = state;
	}
	// A depth-stencil state that tests or writes needs the target it tests
	id<MTLDepthStencilState> depthStencil = hasDepth_ ? pipeline_->depthStencil : g.noDepthStencil;
	if (depthStencil != encoder_.depthStencil)
	{
		[pass_ setDepthStencilState:depthStencil];
		encoder_.depthStencil = depthStencil;
	}
	if (!encoder_.rasterSet || encoder_.cull != pipeline_->cull)
	{
		[pass_ setCullMode:pipeline_->cull];
		encoder_.cull = pipeline_->cull;
	}
	if (!encoder_.rasterSet || encoder_.winding != pipeline_->winding)
	{
		[pass_ setFrontFacingWinding:pipeline_->winding];
		encoder_.winding = pipeline_->winding;
	}
	if (!encoder_.rasterSet || encoder_.depthClip != pipeline_->depthClip)
	{
		[pass_ setDepthClipMode:pipeline_->depthClip];
		encoder_.depthClip = pipeline_->depthClip;
	}
	if (!encoder_.rasterSet || encoder_.depthBias != pipeline_->depthBias || encoder_.depthBiasSlope != pipeline_->depthBiasSlope
		|| encoder_.depthBiasClamp != pipeline_->depthBiasClamp)
	{
		[pass_ setDepthBias:pipeline_->depthBias slopeScale:pipeline_->depthBiasSlope clamp:pipeline_->depthBiasClamp];
		encoder_.depthBias = pipeline_->depthBias;
		encoder_.depthBiasSlope = pipeline_->depthBiasSlope;
		encoder_.depthBiasClamp = pipeline_->depthBiasClamp;
	}
	encoder_.rasterSet = true;
	// The driver sets these again whenever it reopens its list or changes its mind, mostly to what they were
	if (!encoder_.viewportSet || memcmp(&encoder_.viewport, &viewport_, sizeof viewport_))
	{
		[pass_ setViewport:viewport_];
		encoder_.viewport = viewport_;
		encoder_.viewportSet = true;
	}
	const MTLScissorRect rect = {(NSUInteger)left, (NSUInteger)top, (NSUInteger)(right - left), (NSUInteger)(bottom - top)};
	if (!encoder_.scissorSet || memcmp(&encoder_.scissor, &rect, sizeof rect))
	{
		[pass_ setScissorRect:rect];
		encoder_.scissor = rect;
		encoder_.scissorSet = true;
	}
	if (!encoder_.stencilSet || encoder_.stencilRef != stencilRef_)
	{
		[pass_ setStencilReferenceValue:stencilRef_];
		encoder_.stencilRef = stencilRef_;
		encoder_.stencilSet = true;
	}
	if (!encoder_.blendSet || memcmp(encoder_.blendFactor, blendFactor_, sizeof blendFactor_))
	{
		[pass_ setBlendColorRed:blendFactor_[0] green:blendFactor_[1] blue:blendFactor_[2] alpha:blendFactor_[3]];
		memcpy(encoder_.blendFactor, blendFactor_, sizeof blendFactor_);
		encoder_.blendSet = true;
	}

	for (unsigned i = 0; i < kCbvCount; i++)
	{
		if (pipeline_->vertexUses[i])
			BindBuffer(true, i, cbv_[i]);
		if (pipeline_->pixelUses[i])
			BindBuffer(false, i, cbv_[i]);
	}
	for (unsigned i = 0; i < kSrvCount; i++)
	{
		if (pipeline_->pixelUses[kSrvBinding + i] && encoder_.textures[i] != srv_[i].texture)
		{
			[pass_ setFragmentTexture:srv_[i].texture atIndex:i];
			encoder_.textures[i] = srv_[i].texture;
		}
	}
	for (unsigned i = 0; i < kSamplerCount; i++)
	{
		if (pipeline_->pixelUses[kSamplerBinding + i] && encoder_.samplers[i] != sampler_[i].sampler)
		{
			[pass_ setFragmentSamplerState:sampler_[i].sampler atIndex:i];
			encoder_.samplers[i] = sampler_[i].sampler;
		}
	}
	if (pipeline_->readsVertices)
	{
		BindBuffer(true, kVertexBufferIndex, vertex_);
		if (pipeline_->stride && vertexStride_ && pipeline_->stride != vertexStride_)
			MTL12_LOG_ONCE("draw: the vertex buffer's stride is %u, the input layout's %u", (unsigned)vertexStride_, (unsigned)pipeline_->stride);
	}
	return true;
}

void List::DrawInstanced(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance)
{
	@autoreleasepool
	{
		if (vertices && instances && PrepareDraw())
			[pass_ drawPrimitives:Primitive() vertexStart:firstVertex vertexCount:vertices instanceCount:instances
				baseInstance:firstInstance];
	}
}

void List::DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance)
{
	@autoreleasepool
	{
		if (!indices || !instances || !index_.buffer || !PrepareDraw())
			return;
		const NSUInteger indexBytes = indexType_ == MTLIndexTypeUInt32 ? 4 : 2;
		[pass_ drawIndexedPrimitives:Primitive() indexCount:indices indexType:indexType_ indexBuffer:index_.buffer
			indexBufferOffset:(NSUInteger)index_.offset + firstIndex * indexBytes instanceCount:instances
			baseVertex:baseVertex baseInstance:firstInstance];
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Copies

// The encoder copies go through, with no pass open and no clear waiting on the textures the copy reads or writes
id<MTLBlitCommandEncoder> List::Blit(id<MTLTexture> first, id<MTLTexture> second)
{
	EndPass();
	if (first)
		FlushClear(first);
	if (second)
		FlushClear(second);
	if (g.encoderCopies && blit_ && blitCopies_ >= g.encoderCopies)
		EndBlit();
	if (!blit_)
	{
		blit_ = [cmd_ blitCommandEncoder];
		blitCopies_ = 0;
	}
	blitCopies_++;
	return blit_;
}

static MTLSize MipSize(const Resource *resource, UINT mip)
{
	const bool volume = resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	return MTLSizeMake(std::max<NSUInteger>(1, (NSUInteger)resource->desc.Width >> mip),
		std::max<NSUInteger>(1, resource->desc.Height >> mip),
		volume ? std::max<NSUInteger>(1, (NSUInteger)resource->desc.DepthOrArraySize >> mip) : 1);
}

void List::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
	const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
	if (!open_ || !dst || !src)
		return;
	Resource *to = static_cast<Resource *>(dst->pResource);
	Resource *from = static_cast<Resource *>(src->pResource);
	if (!to || !from)
		return;

	// The driver copies color only, one mip at a time, of textures with one layer
	const bool toBuffer = dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	const bool fromBuffer = src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	@autoreleasepool
	{
		if (fromBuffer && !toBuffer)
		{
			if (!from->buffer || !to->texture)
				return;
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &foot = src->PlacedFootprint;
			const unsigned texel = std::max(1u, Format(foot.Footprint.Format).bytes);
			const NSUInteger imageBytes = (NSUInteger)foot.Footprint.RowPitch * foot.Footprint.Height;
			NSUInteger offset = (NSUInteger)foot.Offset;
			MTLSize size = MTLSizeMake(foot.Footprint.Width, foot.Footprint.Height, foot.Footprint.Depth);
			if (box)
			{
				offset += (NSUInteger)box->front * imageBytes + (NSUInteger)box->top * foot.Footprint.RowPitch + (NSUInteger)box->left * texel;
				size = MTLSizeMake(box->right - box->left, box->bottom - box->top, box->back - box->front);
			}
			[Blit(to->texture, nil) copyFromBuffer:from->buffer sourceOffset:offset sourceBytesPerRow:foot.Footprint.RowPitch
				sourceBytesPerImage:imageBytes sourceSize:size toTexture:to->texture destinationSlice:0
				destinationLevel:dst->SubresourceIndex destinationOrigin:MTLOriginMake(x, y, z)];
		}
		else if (toBuffer && !fromBuffer)
		{
			if (!from->texture || !to->buffer)
				return;
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &foot = dst->PlacedFootprint;
			const unsigned texel = std::max(1u, Format(foot.Footprint.Format).bytes);
			const NSUInteger imageBytes = (NSUInteger)foot.Footprint.RowPitch * foot.Footprint.Height;
			const NSUInteger offset = (NSUInteger)foot.Offset + (NSUInteger)z * imageBytes + (NSUInteger)y * foot.Footprint.RowPitch
				+ (NSUInteger)x * texel;
			MTLOrigin origin = MTLOriginMake(0, 0, 0);
			MTLSize size = MipSize(from, src->SubresourceIndex);
			if (box)
			{
				origin = MTLOriginMake(box->left, box->top, box->front);
				size = MTLSizeMake(box->right - box->left, box->bottom - box->top, box->back - box->front);
			}
			[Blit(from->texture, nil) copyFromTexture:from->texture sourceSlice:0 sourceLevel:src->SubresourceIndex sourceOrigin:origin
				sourceSize:size toBuffer:to->buffer destinationOffset:offset destinationBytesPerRow:foot.Footprint.RowPitch
				destinationBytesPerImage:imageBytes];
		}
		else if (!toBuffer && !fromBuffer)
		{
			if (!from->texture || !to->texture)
				return;
			MTLOrigin origin = MTLOriginMake(0, 0, 0);
			MTLSize size = MipSize(from, src->SubresourceIndex);
			if (box)
			{
				origin = MTLOriginMake(box->left, box->top, box->front);
				size = MTLSizeMake(box->right - box->left, box->bottom - box->top, box->back - box->front);
			}
			[Blit(from->texture, to->texture) copyFromTexture:from->texture sourceSlice:0 sourceLevel:src->SubresourceIndex
				sourceOrigin:origin sourceSize:size toTexture:to->texture destinationSlice:0
				destinationLevel:dst->SubresourceIndex destinationOrigin:MTLOriginMake(x, y, z)];
		}
		else
			MTL12_LOG_ONCE("CopyTextureRegion: buffer to buffer isn't a texture copy");
	}
}

void List::CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
{
	Resource *to = static_cast<Resource *>(dst);
	Resource *from = static_cast<Resource *>(src);
	if (!open_ || !to || !from)
		return;
	@autoreleasepool
	{
		if (to->buffer && from->buffer)
		{
			[Blit(nil, nil) copyFromBuffer:from->buffer sourceOffset:0 toBuffer:to->buffer destinationOffset:0
				size:(NSUInteger)std::min(to->bytes, from->bytes)];
			return;
		}
		if (!to->texture || !from->texture)
			return;
		const UINT mips = std::min<UINT>(std::max<UINT>(1, to->desc.MipLevels), std::max<UINT>(1, from->desc.MipLevels));
		id<MTLBlitCommandEncoder> blit = Blit(from->texture, to->texture);
		for (UINT mip = 0; mip < mips; mip++)
			[blit copyFromTexture:from->texture sourceSlice:0 sourceLevel:mip sourceOrigin:MTLOriginMake(0, 0, 0)
				sourceSize:MipSize(from, mip) toTexture:to->texture destinationSlice:0 destinationLevel:mip
				destinationOrigin:MTLOriginMake(0, 0, 0)];
	}
}

void List::CopyBufferRegion(ID3D12Resource *dst, UINT64 dstOffset, ID3D12Resource *src, UINT64 srcOffset, UINT64 bytes)
{
	Resource *to = static_cast<Resource *>(dst);
	Resource *from = static_cast<Resource *>(src);
	if (!open_ || !to || !from || !to->buffer || !from->buffer || !bytes)
		return;
	@autoreleasepool
	{
		[Blit(nil, nil) copyFromBuffer:from->buffer sourceOffset:(NSUInteger)srcOffset toBuffer:to->buffer
			destinationOffset:(NSUInteger)dstOffset size:(NSUInteger)bytes];
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Timestamps

void List::ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst, UINT64 offset)
{
	// Metal has no timestamp a command list can write between two of its commands (its counter sample buffers aren't
	// on every device and resolve another way), so every query reads as zero: the driver's GPU times say 0 ms
	QueryHeap *queries = static_cast<QueryHeap *>(heap);
	Resource *to = static_cast<Resource *>(dst);
	if (!open_ || !queries || !to || !to->buffer || type != D3D12_QUERY_TYPE_TIMESTAMP || start + count > queries->count || !count)
		return;
	@autoreleasepool
	{
		[Blit(nil, nil) fillBuffer:to->buffer range:NSMakeRange((NSUInteger)offset, (NSUInteger)count * sizeof(uint64_t)) value:0];
	}
}

} // namespace rr_mtl12
