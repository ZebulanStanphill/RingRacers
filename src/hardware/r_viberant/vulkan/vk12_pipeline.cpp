// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_pipeline.cpp
/// \brief vk12's pipeline states: a Direct3D 12 pipeline description as Vulkan pipelines
///
/// One Direct3D 12 pipeline state draws lists and strips, to whatever targets are bound. A Vulkan pipeline is for one
/// topology and one render pass, so a pipeline state here holds one Vulkan pipeline per topology and pass it is used
/// with. Those the description itself implies (its topology type's list and strip, its own target formats) are made
/// with the object, so that making pipeline states ahead of time, as the driver does, compiles them ahead of time.

#include "vk12_internal.h"

#include <algorithm>

namespace rr_vk12
{

Pipeline::~Pipeline()
{
	if (!g.device)
		return;
	for (Variant &variant : variants_)
		vkDestroyPipeline(g.device, variant.pipeline, nullptr);
	if (vs_)
		vkDestroyShaderModule(g.device, vs_, nullptr);
	if (ps_)
		vkDestroyShaderModule(g.device, ps_, nullptr);
}

static VkShaderModule MakeModule(const D3D12_SHADER_BYTECODE &code, ShaderInfo &info, const char *stage)
{
	std::string error;
	if (!PatchShader(code.pShaderBytecode, code.BytecodeLength, info, error))
	{
		Log("%s shader: %s", stage, error.c_str());
		return VK_NULL_HANDLE;
	}
	VkShaderModuleCreateInfo create = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	create.codeSize = info.words.size() * sizeof(uint32_t);
	create.pCode = info.words.data();
	VkShaderModule module = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateShaderModule(g.device, &create, nullptr, &module), "vkCreateShaderModule")))
		return VK_NULL_HANDLE;
	return module;
}

HRESULT Pipeline::Init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
{
	ShaderInfo vs, ps;
	vs_ = MakeModule(desc.VS, vs, "vertex");
	ps_ = MakeModule(desc.PS, ps, "pixel");
	if (!vs_ || !ps_)
		return E_INVALIDARG;
	vsEntry_ = vs.entry;
	psEntry_ = ps.entry;
	for (unsigned i = 0; i < kCbvCount; i++)
		uniformBytes[i] = std::max(vs.uniformBytes[i], ps.uniformBytes[i]);
	for (unsigned i = 0; i < ARRAYSIZE(usesBinding); i++)
		usesBinding[i] = vs.usesBinding[i] || ps.usesBinding[i];

	stride = 0;
	for (UINT i = 0; i < desc.InputLayout.NumElements; i++)
	{
		const D3D12_INPUT_ELEMENT_DESC &element = desc.InputLayout.pInputElementDescs[i];
		const FormatInfo format = Format(element.Format);
		const uint32_t location = SemanticLocation(element.SemanticName, element.SemanticIndex);
		if (format.format == VK_FORMAT_UNDEFINED || location == UINT32_MAX || element.InputSlot)
		{
			Log("pipeline: input element %s%u isn't supported", element.SemanticName, (unsigned)element.SemanticIndex);
			return E_INVALIDARG;
		}
		stride = std::max<uint32_t>(stride, element.AlignedByteOffset + format.bytes);
		// A buffer may carry more than the shader reads
		bool read = false;
		for (const ShaderInfo::Input &input : vs.inputs)
			read |= input.location == location;
		if (read)
			attributes_.push_back({location, 0, format.format, element.AlignedByteOffset});
	}
	for (const ShaderInfo::Input &input : vs.inputs)
	{
		bool fed = false;
		for (const VkVertexInputAttributeDescription &attribute : attributes_)
			fed |= attribute.location == input.location;
		if (!fed)
		{
			Log("pipeline: the vertex shader reads %s, which the input layout doesn't have", input.semantic.c_str());
			return E_INVALIDARG;
		}
	}

	blend_ = desc.BlendState;
	raster_ = desc.RasterizerState;
	depth_ = desc.DepthStencilState;
	topologyType_ = desc.PrimitiveTopologyType;
	declared_.color = desc.NumRenderTargets ? Format(desc.RTVFormats[0]).format : VK_FORMAT_UNDEFINED;
	declared_.depth = desc.DSVFormat != DXGI_FORMAT_UNKNOWN ? Format(desc.DSVFormat).format : VK_FORMAT_UNDEFINED;

	if (topologyType_ == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
		return Make(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, declared_) ? S_OK : E_FAIL;
	if (!Make(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, declared_) || !Make(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, declared_))
		return E_FAIL;
	return S_OK;
}

VkPipeline Pipeline::Get(VkPrimitiveTopology topology, const PassKey &pass)
{
	for (const Variant &variant : variants_)
		if (variant.topology == topology && variant.pass.color == pass.color && variant.pass.depth == pass.depth)
			return variant.pipeline;
	return Make(topology, pass);
}

static VkBlendFactor Factor(D3D12_BLEND blend)
{
	switch (blend)
	{
		case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
		case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
		case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
		case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
		case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
		case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
		case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		default: return VK_BLEND_FACTOR_ONE;
	}
}

static VkBlendOp BlendOp(D3D12_BLEND_OP op)
{
	// The same names mean the same: SUBTRACT is source minus destination in both
	switch (op)
	{
		case D3D12_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
		case D3D12_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
		case D3D12_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
		case D3D12_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
		default: return VK_BLEND_OP_ADD;
	}
}

static VkStencilOpState StencilFace(const D3D12_DEPTH_STENCILOP_DESC &face, const D3D12_DEPTH_STENCIL_DESC &desc)
{
	// Both enumerations run in the same order, Direct3D's from 1 and Vulkan's from 0
	VkStencilOpState state = {};
	state.failOp = (VkStencilOp)(face.StencilFailOp - 1);
	state.depthFailOp = (VkStencilOp)(face.StencilDepthFailOp - 1);
	state.passOp = (VkStencilOp)(face.StencilPassOp - 1);
	state.compareOp = (VkCompareOp)(face.StencilFunc - 1);
	state.compareMask = desc.StencilReadMask;
	state.writeMask = desc.StencilWriteMask;
	return state;
}

VkPipeline Pipeline::Make(VkPrimitiveTopology topology, const PassKey &pass)
{
	const VkRenderPass renderPass = RenderPass(pass);
	if (!renderPass)
		return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs_;
	stages[0].pName = vsEntry_.c_str();
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = ps_;
	stages[1].pName = psEntry_.c_str();

	const VkVertexInputBindingDescription binding = {0, stride, VK_VERTEX_INPUT_RATE_VERTEX};
	VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	if (!attributes_.empty())
	{
		input.vertexBindingDescriptionCount = 1;
		input.pVertexBindingDescriptions = &binding;
		input.vertexAttributeDescriptionCount = (uint32_t)attributes_.size();
		input.pVertexAttributeDescriptions = attributes_.data();
	}

	VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	assembly.topology = topology;

	VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport.viewportCount = viewport.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = raster_.CullMode == D3D12_CULL_MODE_FRONT ? VK_CULL_MODE_FRONT_BIT
		: raster_.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	// The viewport is upside down (List::RSSetViewports), so windings read as Direct3D's
	raster.frontFace = raster_.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
	raster.depthBiasEnable = raster_.DepthBias != 0 || raster_.SlopeScaledDepthBias != 0.0f;
	raster.depthBiasConstantFactor = (float)raster_.DepthBias;
	raster.depthBiasClamp = raster_.DepthBiasClamp;
	raster.depthBiasSlopeFactor = raster_.SlopeScaledDepthBias;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	const bool hasDepth = pass.depth != VK_FORMAT_UNDEFINED;
	depth.depthTestEnable = hasDepth && depth_.DepthEnable;
	depth.depthWriteEnable = hasDepth && depth_.DepthEnable && depth_.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
	depth.depthCompareOp = (VkCompareOp)(std::max<int>(1, depth_.DepthFunc) - 1);
	depth.stencilTestEnable = hasDepth && depth_.StencilEnable;
	if (depth_.StencilEnable)
	{
		depth.front = StencilFace(depth_.FrontFace, depth_);
		depth.back = StencilFace(depth_.BackFace, depth_);
	}
	depth.maxDepthBounds = 1.0f;

	const D3D12_RENDER_TARGET_BLEND_DESC &target = blend_.RenderTarget[0];
	VkPipelineColorBlendAttachmentState attachment = {};
	attachment.blendEnable = target.BlendEnable;
	attachment.srcColorBlendFactor = Factor(target.SrcBlend);
	attachment.dstColorBlendFactor = Factor(target.DestBlend);
	attachment.colorBlendOp = BlendOp(target.BlendOp);
	attachment.srcAlphaBlendFactor = Factor(target.SrcBlendAlpha);
	attachment.dstAlphaBlendFactor = Factor(target.DestBlendAlpha);
	attachment.alphaBlendOp = BlendOp(target.BlendOpAlpha);
	attachment.colorWriteMask = target.RenderTargetWriteMask & 0xF;
	VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend.attachmentCount = pass.color != VK_FORMAT_UNDEFINED ? 1 : 0;
	blend.pAttachments = &attachment;

	const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
	VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic.dynamicStateCount = 3;
	dynamic.pDynamicStates = dynamics;

	VkGraphicsPipelineCreateInfo create = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	create.stageCount = 2;
	create.pStages = stages;
	create.pVertexInputState = &input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &viewport;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &multisample;
	create.pDepthStencilState = &depth;
	create.pColorBlendState = &blend;
	create.pDynamicState = &dynamic;
	create.layout = g.pipelineLayout;
	create.renderPass = renderPass;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (FAILED(Result(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &create, nullptr, &pipeline), "vkCreateGraphicsPipelines")))
		return VK_NULL_HANDLE;
	SetDebugName(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, name_);
	variants_.push_back({topology, pass, pipeline});
	return pipeline;
}

} // namespace rr_vk12
