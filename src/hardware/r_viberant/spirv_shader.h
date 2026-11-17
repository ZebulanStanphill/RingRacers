// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  spirv_shader.h
/// \brief The driver's shaders as SPIR-V, for the backends that take it in place of Direct3D's bytecode
///
/// shaders.hlsl is compiled to SPIR-V by glslang's HLSL front end, at build time and (add-on shaders, screen effects)
/// at run time, with the registers of its one root signature as bindings: b0.. at 0, t0.. at 16, s0.. at 32. What is
/// here is what every such backend needs of that SPIR-V and knows nothing of the API underneath: the Vulkan backend
/// (vulkan/, vk12) hands the patched module to Vulkan, and the Metal backend (metal/, mtl12) translates it further.

#ifndef RR_SPIRV_SHADER_H
#define RR_SPIRV_SHADER_H

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcommon.h>
#else
#include "vulkan/vk12_win.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace rr_spirv
{

// Root parameters of RR_ROOT_SIGNATURE, in its order, and the binding each has in the SPIR-V
enum
{
	kCbvCount = 6, // b0..b5 at bindings 0..5
	kSrvCount = 11, // t0..t10 at bindings 16..26
	kSamplerCount = 2, // s0..s1 at bindings 32..33
	kSrvBinding = 16,
	kSamplerBinding = 32,
	kRootParameters = 19,
};
struct RootParameter
{
	enum Kind { CBV, SRV, SAMPLER } kind;
	unsigned index; // b, t or s register
};
extern const RootParameter kRoot[kRootParameters];

// A patched shader stage: the SPIR-V with every user input and output at the location its HLSL semantic names, and
// what the module says of itself
struct ShaderInfo
{
	std::vector<uint32_t> words;
	std::string entry;
	struct Input { std::string semantic; uint32_t location; };
	std::vector<Input> inputs; // a vertex shader's
	uint32_t uniformBytes[kCbvCount] = {}; // sizes of the uniform blocks at bindings 0..5; 0 where the stage has none
	bool usesBinding[40] = {}; // which bindings the stage declares
};
bool PatchShader(const void *code, size_t bytes, ShaderInfo &info, std::string &error);
// The location of a semantic ("TEXCOORD", 1), the same in every stage; UINT32_MAX if it has none
uint32_t SemanticLocation(const char *name, unsigned index);

// HLSL to SPIR-V with glslang's HLSL front end, for the shaders the driver compiles at run time (add-on shaders and
// screen effects, which reach it as HLSL), with the bindings shaders.hlsl is built with. profile's first letter gives
// the stage ("vs_5_1", "ps_5_1"). code receives the SPIR-V, or errors the compiler's messages.
HRESULT CompileHlsl(const void *source, size_t size, const char *entry, const char *profile, ID3DBlob **code, ID3DBlob **errors);

} // namespace rr_spirv

#endif
