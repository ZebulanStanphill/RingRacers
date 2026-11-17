// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_msl.h
/// \brief mtl12's shaders: the driver's SPIR-V as Metal Shading Language
///
/// Plain C++ with nothing of Metal in it, so that it also builds, and is tested, where Metal doesn't exist.

#ifndef RR_MTL12_MSL_H
#define RR_MTL12_MSL_H

#include <cstdint>
#include <string>
#include <vector>

namespace rr_mtl12
{

// Where the root signature's registers land in a Metal function's argument tables: constant buffers b0..b5 at buffer
// indices 0..5, textures t0..t10 at texture indices 0..10, samplers s0 and s1 at sampler indices 0 and 1. The vertex
// buffer takes the last buffer index Metal has, away from the constant buffers.
enum
{
	kVertexBufferIndex = 30,
	kMaxVertexAttribute = 30, // a Metal vertex descriptor has attributes 0..30
};

// A patched stage (rr_spirv::PatchShader's words and entry point) as Metal Shading Language source. A vertex shader's
// inputs are attributes numbered as their locations, and stages link by location. function receives the name the
// entry point has in the source, which isn't always its own ("main" is taken in Metal). used says which of the
// SPIR-V's bindings the Metal function takes as arguments: glslang declares every register shaders.hlsl has in every
// stage, and the function only has the ones its code reaches, which are the ones Metal wants bound.
bool TranslateToMsl(const std::vector<uint32_t> &words, const std::string &entry, bool vertex, std::string &msl,
	std::string &function, bool used[40], std::string &error);

} // namespace rr_mtl12

#endif
