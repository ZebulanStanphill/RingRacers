// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#ifndef RR_VIBERANT_PIPELINE_H
#define RR_VIBERANT_PIPELINE_H
#include "viberant_core.h"
#include "pipeline_key.h"
namespace rr_viberant
{
enum VSKind
{
	VS_POLY,
	VS_POLYCLIP,
	VS_SKY,
	VS_MODEL,
	VS_MODELLIGHT,
	VS_WIPE,
	VS_FULLSCREEN,
	VS_POSTIMAGE,
	NUMVS
};
enum PSKind
{
	PS_FIXED,
	PS_DEFAULT,
	PS_SOFTWARE,
	PS_MODELLIGHT,
	PS_WATER,
	PS_WATERREFRACTION,
	PS_FOG,
	PS_FOGREMAP,
	PS_SKY,
	PS_WIPE,
	PS_WIPEFULL,
	PS_PALETTEPOST,
	PS_FADEMAP,
	PS_POSTIMAGE,
	NUMPS
};
enum Layout
{
	LAYOUT_POLY,
	LAYOUT_SKY,
	LAYOUT_MODEL,
	NUMLAYOUTS,
	LAYOUT_NONE = NUMLAYOUTS
};
D3D12_BLEND_DESC MakeBlendDesc(std::uint16_t key);
D3D12_DEPTH_STENCIL_DESC MakeDepthDesc(std::uint64_t key);
D3D12_RASTERIZER_DESC MakeRasterDesc(std::uint8_t key);
D3D12_GRAPHICS_PIPELINE_STATE_DESC BuildPipelineDesc(ID3D12RootSignature *root, const PipelineKey &key);
D3D12_GRAPHICS_PIPELINE_STATE_DESC BuildPipelineDesc(ID3D12RootSignature *root, const PipelineKey &key,
	const D3D12_SHADER_BYTECODE *customVs, const D3D12_SHADER_BYTECODE *customPs);
const char *ShaderName(VSKind kind);
const char *ShaderName(PSKind kind);
} // namespace rr_viberant
#endif
