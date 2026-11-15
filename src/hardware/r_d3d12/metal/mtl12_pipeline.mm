// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_pipeline.mm
/// \brief mtl12's shaders and pipeline states: a Direct3D 12 pipeline description as Metal's objects
///
/// One Direct3D 12 pipeline state draws to whatever targets are bound and holds the rasterizer and depth-stencil
/// states. A Metal render pipeline state is for the formats of one set of targets, so a pipeline state here holds one
/// per set it is used with (the description's own is made with the object, so that making pipeline states ahead of
/// time, as the driver does, compiles them ahead of time), a depth-stencil state, and the rasterizer settings, which
/// Metal sets on the encoder.

#include "mtl12_internal.h"

#include <algorithm>

namespace rr_mtl12
{

// ---------------------------------------------------------------------------------------------------------------
// Shaders

namespace
{

std::mutex shaderMutex;
// By the stage's letter and the bytes of its SPIR-V. A shader that failed stays, as null, so that it fails once.
std::map<std::string, std::shared_ptr<Shader>> shaders;

std::shared_ptr<Shader> Compile(const D3D12_SHADER_BYTECODE &code, bool vertex)
{
	const char *stage = vertex ? "vertex" : "pixel";
	rr_spirv::ShaderInfo info;
	std::string error, source, function;
	if (!rr_spirv::PatchShader(code.pShaderBytecode, code.BytecodeLength, info, error))
	{
		Log("%s shader: %s", stage, error.c_str());
		return nullptr;
	}
	bool used[40] = {};
	if (!TranslateToMsl(info.words, info.entry, vertex, source, function, used, error))
	{
		Log("%s shader %s: no Metal source: %s", stage, info.entry.c_str(), error.c_str());
		return nullptr;
	}

	MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
	options.languageVersion = MTLLanguageVersion2_1;
	// Metal's compiler otherwise assumes it may reorder arithmetic and that nothing is infinite or not a number. The
	// shaders pick palette entries and colormap rows by arithmetic that must come out as it does on the other backends.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	options.fastMathEnabled = NO;
#pragma clang diagnostic pop
	NSError *failure = nil;
	id<MTLLibrary> library = [g.device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options
		error:&failure];
	if (!library)
	{
		Log("%s shader %s: Metal didn't compile it: %s", stage, info.entry.c_str(),
			failure.localizedDescription.UTF8String ?: "(no description)");
		return nullptr;
	}
	id<MTLFunction> made = [library newFunctionWithName:[NSString stringWithUTF8String:function.c_str()]];
	if (!made)
	{
		Log("%s shader %s: its library has no function %s", stage, info.entry.c_str(), function.c_str());
		return nullptr;
	}
	made.label = [NSString stringWithUTF8String:info.entry.c_str()];

	auto shader = std::make_shared<Shader>();
	shader->function = made;
	shader->inputs = info.inputs;
	memcpy(shader->usesBinding, used, sizeof shader->usesBinding);
	return shader;
}

} // namespace

std::shared_ptr<Shader> GetShader(const D3D12_SHADER_BYTECODE &code, bool vertex)
{
	if (!code.pShaderBytecode || !code.BytecodeLength)
	{
		Log("pipeline: no %s shader", vertex ? "vertex" : "pixel");
		return nullptr;
	}
	std::string key(1, vertex ? 'v' : 'p');
	key.append(static_cast<const char *>(code.pShaderBytecode), code.BytecodeLength);
	// Held while compiling: the driver's worker makes pipeline states while its own thread does, mostly of the same
	// few shaders, and the second to ask for one should get the first's
	std::lock_guard<std::mutex> lock(shaderMutex);
	auto found = shaders.find(key);
	if (found != shaders.end())
		return found->second;
	std::shared_ptr<Shader> shader = Compile(code, vertex);
	shaders[key] = shader;
	return shader;
}

void ForgetShaders()
{
	std::lock_guard<std::mutex> lock(shaderMutex);
	shaders.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline states

static MTLBlendFactor Factor(D3D12_BLEND blend)
{
	switch (blend)
	{
		case D3D12_BLEND_ZERO: return MTLBlendFactorZero;
		case D3D12_BLEND_ONE: return MTLBlendFactorOne;
		case D3D12_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
		case D3D12_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
		case D3D12_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
		case D3D12_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
		case D3D12_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
		case D3D12_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
		case D3D12_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
		case D3D12_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
		case D3D12_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
		case D3D12_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
		case D3D12_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
		default: return MTLBlendFactorOne;
	}
}

static MTLBlendOperation BlendOp(D3D12_BLEND_OP op)
{
	// The same names mean the same: SUBTRACT is source minus destination in both
	switch (op)
	{
		case D3D12_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
		case D3D12_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
		case D3D12_BLEND_OP_MIN: return MTLBlendOperationMin;
		case D3D12_BLEND_OP_MAX: return MTLBlendOperationMax;
		default: return MTLBlendOperationAdd;
	}
}

// Both enumerations run in the same order, Direct3D's from 1 and Metal's from 0: never, less, equal, less or equal,
// greater, not equal, greater or equal, always
static MTLCompareFunction Compare(D3D12_COMPARISON_FUNC function)
{
	return function >= D3D12_COMPARISON_FUNC_NEVER && function <= D3D12_COMPARISON_FUNC_ALWAYS
		? (MTLCompareFunction)(function - 1) : MTLCompareFunctionAlways;
}

// As are these: keep, zero, replace, increment and decrement clamped, invert, increment and decrement wrapped
static MTLStencilOperation StencilOp(D3D12_STENCIL_OP op)
{
	return op >= D3D12_STENCIL_OP_KEEP && op <= D3D12_STENCIL_OP_DECR ? (MTLStencilOperation)(op - 1) : MTLStencilOperationKeep;
}

static MTLStencilDescriptor *StencilFace(const D3D12_DEPTH_STENCILOP_DESC &face, const D3D12_DEPTH_STENCIL_DESC &desc)
{
	MTLStencilDescriptor *stencil = [[MTLStencilDescriptor alloc] init];
	stencil.stencilCompareFunction = Compare(face.StencilFunc);
	stencil.stencilFailureOperation = StencilOp(face.StencilFailOp);
	stencil.depthFailureOperation = StencilOp(face.StencilDepthFailOp);
	stencil.depthStencilPassOperation = StencilOp(face.StencilPassOp);
	stencil.readMask = desc.StencilReadMask;
	stencil.writeMask = desc.StencilWriteMask;
	return stencil;
}

static bool VertexFormat(DXGI_FORMAT format, MTLVertexFormat *metal, unsigned *bytes)
{
	switch (format)
	{
		case DXGI_FORMAT_R32G32B32_FLOAT: *metal = MTLVertexFormatFloat3; *bytes = 12; return true;
		case DXGI_FORMAT_R32G32_FLOAT: *metal = MTLVertexFormatFloat2; *bytes = 8; return true;
		case DXGI_FORMAT_R8G8B8A8_UNORM: *metal = MTLVertexFormatUChar4Normalized; *bytes = 4; return true;
		default: return false;
	}
}

// Notes a register that a compiled function reads, by its place in the function's argument tables (mtl12_msl.h)
API_AVAILABLE(macos(13.0))
static void MarkUsed(id<MTLBinding> binding, bool uses[40])
{
	if (!binding.used)
		return;
	const NSUInteger index = binding.index;
	switch (binding.type)
	{
		case MTLBindingTypeBuffer:
			if (index < kCbvCount)
				uses[index] = true;
			break;
		case MTLBindingTypeTexture:
			if (index < kSrvCount)
				uses[kSrvBinding + index] = true;
			break;
		case MTLBindingTypeSampler:
			if (index < kSamplerCount)
				uses[kSamplerBinding + index] = true;
			break;
		default:
			break;
	}
}

HRESULT Pipeline::Init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
{
	@autoreleasepool
	{
		vs_ = GetShader(desc.VS, true);
		ps_ = GetShader(desc.PS, false);
		if (!vs_ || !ps_)
			return E_INVALIDARG;
		memcpy(vertexUses, vs_->usesBinding, sizeof vertexUses);
		memcpy(pixelUses, ps_->usesBinding, sizeof pixelUses);
		// The root signature shows textures and samplers to pixel shaders only
		for (unsigned i = kCbvCount; i < ARRAYSIZE(vertexUses); i++)
		{
			if (vertexUses[i])
			{
				Log("pipeline: the vertex shader uses a texture or a sampler, which the root signature gives pixel shaders only");
				return E_INVALIDARG;
			}
		}

		stride = 0;
		std::vector<uint32_t> fed;
		for (UINT i = 0; i < desc.InputLayout.NumElements; i++)
		{
			const D3D12_INPUT_ELEMENT_DESC &element = desc.InputLayout.pInputElementDescs[i];
			MTLVertexFormat format;
			unsigned bytes;
			const uint32_t location = rr_spirv::SemanticLocation(element.SemanticName, element.SemanticIndex);
			if (!VertexFormat(element.Format, &format, &bytes) || location > kMaxVertexAttribute || element.InputSlot)
			{
				Log("pipeline: input element %s%u isn't supported", element.SemanticName, (unsigned)element.SemanticIndex);
				return E_INVALIDARG;
			}
			stride = std::max<uint32_t>(stride, element.AlignedByteOffset + bytes);
			// A buffer may carry more than the shader reads
			bool read = false;
			for (const rr_spirv::ShaderInfo::Input &input : vs_->inputs)
				read |= input.location == location;
			if (!read)
				continue;
			if (!vertexDescriptor_)
				vertexDescriptor_ = [[MTLVertexDescriptor alloc] init];
			vertexDescriptor_.attributes[location].format = format;
			vertexDescriptor_.attributes[location].offset = element.AlignedByteOffset;
			vertexDescriptor_.attributes[location].bufferIndex = kVertexBufferIndex;
			fed.push_back(location);
		}
		for (const rr_spirv::ShaderInfo::Input &input : vs_->inputs)
		{
			if (std::find(fed.begin(), fed.end(), input.location) == fed.end())
			{
				Log("pipeline: the vertex shader reads %s, which the input layout doesn't have", input.semantic.c_str());
				return E_INVALIDARG;
			}
		}
		if (vertexDescriptor_)
		{
			vertexDescriptor_.layouts[kVertexBufferIndex].stride = stride;
			vertexDescriptor_.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
			vertexDescriptor_.layouts[kVertexBufferIndex].stepRate = 1;
			readsVertices = true;
		}

		blend_ = desc.BlendState;
		const D3D12_RASTERIZER_DESC &raster = desc.RasterizerState;
		cull = raster.CullMode == D3D12_CULL_MODE_FRONT ? MTLCullModeFront
			: raster.CullMode == D3D12_CULL_MODE_BACK ? MTLCullModeBack : MTLCullModeNone;
		// Metal's clip space, viewport and winding are Direct3D's, so windings read the same
		winding = raster.FrontCounterClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
		depthBias = (float)raster.DepthBias;
		depthBiasSlope = raster.SlopeScaledDepthBias;
		depthBiasClamp = raster.DepthBiasClamp;
		depthClip = raster.DepthClipEnable ? MTLDepthClipModeClip : MTLDepthClipModeClamp;

		const D3D12_DEPTH_STENCIL_DESC &depth = desc.DepthStencilState;
		MTLDepthStencilDescriptor *depthInfo = [[MTLDepthStencilDescriptor alloc] init];
		depthInfo.depthCompareFunction = depth.DepthEnable ? Compare(depth.DepthFunc) : MTLCompareFunctionAlways;
		depthInfo.depthWriteEnabled = depth.DepthEnable && depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
		if (depth.StencilEnable)
		{
			depthInfo.frontFaceStencil = StencilFace(depth.FrontFace, depth);
			depthInfo.backFaceStencil = StencilFace(depth.BackFace, depth);
		}
		depthStencil = [g.device newDepthStencilStateWithDescriptor:depthInfo];
		if (!depthStencil)
		{
			Log("pipeline: Metal made no depth-stencil state");
			return E_FAIL;
		}

		declared_.color = desc.NumRenderTargets ? Format(desc.RTVFormats[0]).format : MTLPixelFormatInvalid;
		declared_.depth = desc.DSVFormat != DXGI_FORMAT_UNKNOWN ? Format(desc.DSVFormat).format : MTLPixelFormatInvalid;
		return Make(declared_) ? S_OK : E_FAIL;
	}
}

id<MTLRenderPipelineState> Pipeline::Get(const PassKey &pass)
{
	for (const Variant &variant : variants_)
		if (variant.pass.color == pass.color && variant.pass.depth == pass.depth)
			return variant.state;
	return Make(pass);
}

id<MTLRenderPipelineState> Pipeline::Make(const PassKey &pass)
{
	@autoreleasepool
	{
		MTLRenderPipelineDescriptor *info = [[MTLRenderPipelineDescriptor alloc] init];
		if (!name_.empty())
			info.label = [NSString stringWithUTF8String:name_.c_str()];
		info.vertexFunction = vs_->function;
		info.fragmentFunction = ps_->function;
		info.vertexDescriptor = vertexDescriptor_;
		info.rasterSampleCount = 1;
		if (pass.color != MTLPixelFormatInvalid)
		{
			const D3D12_RENDER_TARGET_BLEND_DESC &target = blend_.RenderTarget[0];
			MTLRenderPipelineColorAttachmentDescriptor *color = info.colorAttachments[0];
			color.pixelFormat = pass.color;
			color.blendingEnabled = target.BlendEnable != 0;
			color.sourceRGBBlendFactor = Factor(target.SrcBlend);
			color.destinationRGBBlendFactor = Factor(target.DestBlend);
			color.rgbBlendOperation = BlendOp(target.BlendOp);
			color.sourceAlphaBlendFactor = Factor(target.SrcBlendAlpha);
			color.destinationAlphaBlendFactor = Factor(target.DestBlendAlpha);
			color.alphaBlendOperation = BlendOp(target.BlendOpAlpha);
			// Direct3D's mask has red in its lowest bit, Metal's in its highest
			MTLColorWriteMask mask = MTLColorWriteMaskNone;
			if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_RED) mask |= MTLColorWriteMaskRed;
			if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_GREEN) mask |= MTLColorWriteMaskGreen;
			if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_BLUE) mask |= MTLColorWriteMaskBlue;
			if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_ALPHA) mask |= MTLColorWriteMaskAlpha;
			color.writeMask = mask;
		}
		if (pass.depth != MTLPixelFormatInvalid)
		{
			// The one depth format has both
			info.depthAttachmentPixelFormat = pass.depth;
			info.stencilAttachmentPixelFormat = pass.depth;
		}
		NSError *failure = nil;
		id<MTLRenderPipelineState> state = nil;
		if (!reflected_)
		{
			// The first time, Metal is also asked what the compiled functions take. A function's source can name a
			// register that its code, once compiled, never reads, and Metal's validation warns about a buffer or a
			// texture bound for nothing.
			reflected_ = true;
			if (@available(macOS 13.0, *))
			{
				MTLRenderPipelineReflection *reflection = nil;
				state = [g.device newRenderPipelineStateWithDescriptor:info options:MTLPipelineOptionBindingInfo
					reflection:&reflection error:&failure];
				if (state && reflection)
				{
					memset(vertexUses, 0, sizeof vertexUses);
					memset(pixelUses, 0, sizeof pixelUses);
					for (id<MTLBinding> binding in reflection.vertexBindings)
						MarkUsed(binding, vertexUses);
					for (id<MTLBinding> binding in reflection.fragmentBindings)
						MarkUsed(binding, pixelUses);
				}
			}
		}
		if (!state)
			state = [g.device newRenderPipelineStateWithDescriptor:info error:&failure];
		if (!state)
		{
			Log("pipeline %s: Metal made no pipeline state: %s", name_.c_str(),
				failure.localizedDescription.UTF8String ?: "(no description)");
			return nil;
		}
		variants_.push_back({pass, state});
		return state;
	}
}

} // namespace rr_mtl12
