// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_msl.cpp
/// \brief mtl12's shaders: the driver's SPIR-V as Metal Shading Language, with SPIRV-Cross
///
/// The Vulkan backend hands Vulkan the SPIR-V that glslang makes of shaders.hlsl (and of add-on shaders and screen
/// effects at run time), patched to link by semantic. Metal takes source in its own language, so the same patched
/// SPIR-V goes through SPIRV-Cross here. The bindings the SPIR-V has (b0.. at 0, t0.. at 16, s0.. at 32) get the
/// explicit Metal indices of mtl12_msl.h, the same in both stages, so that the command list binds a register without
/// asking the shader where it went.

#include "mtl12_msl.h"

#include <exception>

#include <spirv_msl.hpp>

#include "../spirv_shader.h"

namespace rr_mtl12
{

bool TranslateToMsl(const std::vector<uint32_t> &words, const std::string &entry, bool vertex, std::string &msl,
	std::string &function, bool used[40], std::string &error)
{
	try
	{
		const spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
		spirv_cross::CompilerMSL compiler(words);
		compiler.set_entry_point(entry, model);

		spirv_cross::CompilerMSL::Options options;
		options.platform = spirv_cross::CompilerMSL::Options::macOS;
		// Metal 2.1 (macOS 10.14) has everything the shaders use
		options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 1);
		compiler.set_msl_options(options);

		for (unsigned i = 0; i < rr_spirv::kCbvCount; i++)
		{
			spirv_cross::MSLResourceBinding binding;
			binding.stage = model;
			binding.desc_set = 0;
			binding.binding = i;
			binding.count = 1;
			binding.msl_buffer = i;
			compiler.add_msl_resource_binding(binding);
		}
		for (unsigned i = 0; i < rr_spirv::kSrvCount; i++)
		{
			spirv_cross::MSLResourceBinding binding;
			binding.stage = model;
			binding.desc_set = 0;
			binding.binding = rr_spirv::kSrvBinding + i;
			binding.count = 1;
			binding.msl_texture = i;
			compiler.add_msl_resource_binding(binding);
		}
		for (unsigned i = 0; i < rr_spirv::kSamplerCount; i++)
		{
			spirv_cross::MSLResourceBinding binding;
			binding.stage = model;
			binding.desc_set = 0;
			binding.binding = rr_spirv::kSamplerBinding + i;
			binding.count = 1;
			binding.msl_sampler = i;
			compiler.add_msl_resource_binding(binding);
		}

		msl = compiler.compile();
		function = compiler.get_cleansed_entry_point_name(entry, model);
		for (unsigned binding = 0; binding < 40; binding++)
			used[binding] = compiler.is_msl_resource_binding_used(model, 0, binding);
		return true;
	}
	catch (const std::exception &e)
	{
		error = e.what();
		return false;
	}
}

} // namespace rr_mtl12
