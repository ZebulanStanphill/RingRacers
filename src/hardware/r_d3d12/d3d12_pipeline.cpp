// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#include "d3d12_pipeline.h"
#include <climits>
// [UWP]: Keep these separate from the SM4 headers with the same array names.
#include "r_d3d12_shaders/VSPoly.h"
#include "r_d3d12_shaders/VSPolyClip.h"
#include "r_d3d12_shaders/VSSky.h"
#include "r_d3d12_shaders/VSModel.h"
#include "r_d3d12_shaders/VSModelLight.h"
#include "r_d3d12_shaders/VSWipe.h"
#include "r_d3d12_shaders/VSFullscreen.h"
#include "r_d3d12_shaders/VSPostImage.h"
#include "r_d3d12_shaders/PSFixed.h"
#include "r_d3d12_shaders/PSDefault.h"
#include "r_d3d12_shaders/PSSoftware.h"
#include "r_d3d12_shaders/PSSoftwareModelLight.h"
#include "r_d3d12_shaders/PSWater.h"
#include "r_d3d12_shaders/PSWaterRefraction.h"
#include "r_d3d12_shaders/PSFog.h"
#include "r_d3d12_shaders/PSFogRemap.h"
#include "r_d3d12_shaders/PSSky.h"
#include "r_d3d12_shaders/PSWipe.h"
#include "r_d3d12_shaders/PSWipeFull.h"
#include "r_d3d12_shaders/PSPalettePostprocess.h"
#include "r_d3d12_shaders/PSFadeMap.h"
#include "r_d3d12_shaders/PSPostImage.h"

namespace rr_d3d12
{
namespace
{
// [UWP]: Match the former Direct3D 11 driver and hw_defs.h's packed float vertices.
const D3D12_INPUT_ELEMENT_DESC kPoly[] = {
	{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
const D3D12_INPUT_ELEMENT_DESC kSky[] = {
	{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
const D3D12_INPUT_ELEMENT_DESC kModel[] = {
	{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
const D3D12_INPUT_LAYOUT_DESC kLayouts[] = {
	{kPoly, ARRAYSIZE(kPoly)},
	{kSky, ARRAYSIZE(kSky)},
	{kModel, ARRAYSIZE(kModel)},
	{nullptr, 0},
};
const D3D12_SHADER_BYTECODE kVs[] = {
	{g_VSPoly, sizeof g_VSPoly},
	{g_VSPolyClip, sizeof g_VSPolyClip},
	{g_VSSky, sizeof g_VSSky},
	{g_VSModel, sizeof g_VSModel},
	{g_VSModelLight, sizeof g_VSModelLight},
	{g_VSWipe, sizeof g_VSWipe},
	{g_VSFullscreen, sizeof g_VSFullscreen},
	{g_VSPostImage, sizeof g_VSPostImage},
};
const char *kVsNames[] = {"VSPoly",		  "VSPolyClip", "VSSky",		"VSModel",
						  "VSModelLight", "VSWipe",		"VSFullscreen", "VSPostImage"};
const D3D12_SHADER_BYTECODE kPs[] = {
	{g_PSFixed, sizeof g_PSFixed},		 {g_PSDefault, sizeof g_PSDefault},
	{g_PSSoftware, sizeof g_PSSoftware}, {g_PSSoftwareModelLight, sizeof g_PSSoftwareModelLight},
	{g_PSWater, sizeof g_PSWater},		 {g_PSWaterRefraction, sizeof g_PSWaterRefraction},
	{g_PSFog, sizeof g_PSFog},			 {g_PSFogRemap, sizeof g_PSFogRemap},
	{g_PSSky, sizeof g_PSSky},			 {g_PSWipe, sizeof g_PSWipe},
	{g_PSWipeFull, sizeof g_PSWipeFull}, {g_PSPalettePostprocess, sizeof g_PSPalettePostprocess},
	{g_PSFadeMap, sizeof g_PSFadeMap},	 {g_PSPostImage, sizeof g_PSPostImage},
};
const char *kPsNames[] = {"PSFixed",	"PSDefault",
						  "PSSoftware", "PSSoftwareModelLight",
						  "PSWater",	"PSWaterRefraction",
						  "PSFog",		"PSFogRemap",
						  "PSSky",		"PSWipe",
						  "PSWipeFull", "PSPalettePostprocess",
						  "PSFadeMap",	"PSPostImage"};
static_assert(ARRAYSIZE(kVs) == NUMVS, "Every built-in vertex shader needs bytecode");
static_assert(ARRAYSIZE(kPs) == NUMPS, "Every built-in pixel shader needs bytecode");
static_assert(ARRAYSIZE(kVsNames) == NUMVS, "Every vertex shader needs a name");
static_assert(ARRAYSIZE(kPsNames) == NUMPS, "Every pixel shader needs a name");
static_assert(ARRAYSIZE(kLayouts) == LAYOUT_NONE + 1, "Every layout needs a descriptor");

D3D12_BLEND Blend(unsigned f)
{
	const D3D12_BLEND values[] = {D3D12_BLEND_ZERO,			 D3D12_BLEND_ONE,		D3D12_BLEND_SRC_ALPHA,
								  D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_DEST_COLOR,
								  D3D12_BLEND_INV_DEST_COLOR};
	return f < 7 ? values[f] : D3D12_BLEND_ONE;
}
} // namespace
const char *ShaderName(VSKind k)
{
	return unsigned(k) < NUMVS ? kVsNames[k] : "custom VS";
}
const char *ShaderName(PSKind k)
{
	return unsigned(k) < NUMPS ? kPsNames[k] : "custom PS";
}
D3D12_BLEND_DESC MakeBlendDesc(std::uint16_t key)
{
	D3D12_BLEND_DESC d = {};
	auto &t = d.RenderTarget[0];
	const bool opaque = (key & 0x8000) != 0;
	t.BlendEnable = !opaque && (key & 1023) != 0;
	t.SrcBlend = (opaque || !(key & 1023)) ? D3D12_BLEND_ONE : Blend((key >> 1) & 7);
	t.DestBlend = opaque ? D3D12_BLEND_ZERO : Blend((key >> 4) & 7);
	unsigned eq = (key >> 7) & 7;
	t.BlendOp = eq == 1 ? D3D12_BLEND_OP_SUBTRACT : eq == 2 ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
	t.SrcBlendAlpha = D3D12_BLEND_ONE;
	t.DestBlendAlpha = D3D12_BLEND_ZERO;
	t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	t.LogicOp = D3D12_LOGIC_OP_NOOP;
	t.RenderTargetWriteMask = opaque ? D3D12_COLOR_WRITE_ENABLE_ALL : (key >> 10) & 15;
	return d;
}
D3D12_DEPTH_STENCIL_DESC MakeDepthDesc(std::uint64_t key)
{
	D3D12_DEPTH_STENCIL_DESC d = {};
	d.DepthEnable = (key & 1) != 0;
	d.DepthWriteMask = (key & 4) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	d.DepthFunc = (key & 2) ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_LESS_EQUAL;
	d.StencilEnable = (key & 8) != 0;
	d.StencilReadMask = (key >> 20) & 255;
	d.StencilWriteMask = (key >> 28) & 255;
	d.FrontFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>((key >> 4) & 15);
	d.FrontFace.StencilFailOp = static_cast<D3D12_STENCIL_OP>((key >> 8) & 15);
	d.FrontFace.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>((key >> 12) & 15);
	d.FrontFace.StencilPassOp = static_cast<D3D12_STENCIL_OP>((key >> 16) & 15);
	d.BackFace = d.FrontFace;
	return d;
}
D3D12_RASTERIZER_DESC MakeRasterDesc(std::uint8_t key)
{
	D3D12_RASTERIZER_DESC d = {};
	d.FillMode = D3D12_FILL_MODE_SOLID;
	d.CullMode = (key & 3) == 2 ? D3D12_CULL_MODE_FRONT : (key & 3) == 1 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	d.FrontCounterClockwise = TRUE;
	d.DepthBias = (key & 4) ? -1 : 0;
	d.SlopeScaledDepthBias = (key & 4) ? -1.0f : 0.0f;
	d.DepthClipEnable = TRUE;
	return d;
}
D3D12_GRAPHICS_PIPELINE_STATE_DESC BuildPipelineDesc(ID3D12RootSignature *root, const PipelineKey &k)
{
	return BuildPipelineDesc(root, k, nullptr, nullptr);
}
D3D12_GRAPHICS_PIPELINE_STATE_DESC BuildPipelineDesc(ID3D12RootSignature *root, const PipelineKey &k,
	const D3D12_SHADER_BYTECODE *customVs, const D3D12_SHADER_BYTECODE *customPs)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root;
	if (k.vs < NUMVS)
		d.VS = kVs[k.vs];
	else if (customVs) d.VS = *customVs;
	if (k.ps < NUMPS)
		d.PS = kPs[k.ps];
	else if (customPs) d.PS = *customPs;
	if (k.layout <= LAYOUT_NONE)
		d.InputLayout = kLayouts[k.layout];
	d.BlendState = MakeBlendDesc(k.blend);
	d.DepthStencilState = MakeDepthDesc(k.depth);
	d.RasterizerState = MakeRasterDesc(k.raster);
	d.SampleMask = UINT_MAX;
	d.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(k.topologyType);
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = static_cast<DXGI_FORMAT>(k.rtFormat);
	d.DSVFormat = static_cast<DXGI_FORMAT>(k.dsvFormat);
	d.SampleDesc.Count = 1;
	return d;
}
} // namespace rr_d3d12
