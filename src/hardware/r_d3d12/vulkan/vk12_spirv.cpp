// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_spirv.cpp
/// \brief vk12's shaders: HLSL to SPIR-V with glslang, and SPIR-V made to link the way HLSL does
///
/// Direct3D links a vertex shader's outputs to a pixel shader's inputs, and a vertex buffer's elements to a vertex
/// shader's inputs, by semantic ("TEXCOORD1"). SPIR-V links by location number, which glslang hands out in the order
/// each stage declares things. The shaders are compiled with their semantics kept (SPV_GOOGLE_hlsl_functionality1),
/// and PatchShader gives every input and output the location its semantic stands for, the same in every stage, then
/// removes the semantics, which Vulkan itself has no use for.

#include "vk12_internal.h"

#include <algorithm>
#include <cctype>
#include <map>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

namespace rr_vk12
{

uint32_t SemanticLocation(const char *name, unsigned index)
{
	char upper[32];
	size_t length = 0;
	for (; name[length] && length < sizeof upper - 1; length++)
		upper[length] = (char)toupper((unsigned char)name[length]);
	upper[length] = '\0';
	if (!strcmp(upper, "POSITION"))
		return index == 0 ? 0 : UINT32_MAX;
	if (!strcmp(upper, "NORMAL"))
		return index == 0 ? 1 : UINT32_MAX;
	if (!strcmp(upper, "COLOR"))
		return index < 2 ? 2 + index : UINT32_MAX;
	if (!strcmp(upper, "TEXCOORD"))
		return index < 60 ? 4 + index : UINT32_MAX;
	return UINT32_MAX;
}

// "TEXCOORD12" is ("TEXCOORD", 12); "POSITION" is ("POSITION", 0)
static uint32_t SemanticLocation(const std::string &semantic)
{
	size_t digits = semantic.size();
	while (digits && isdigit((unsigned char)semantic[digits - 1]))
		digits--;
	const unsigned index = digits < semantic.size() ? (unsigned)atoi(semantic.c_str() + digits) : 0;
	return SemanticLocation(semantic.substr(0, digits).c_str(), index);
}

namespace
{

enum
{
	OpExtension = 10, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
	OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71,
	OpMemberDecorate = 72, OpDecorateString = 5632, OpMemberDecorateString = 5633,
	DecorationBuiltIn = 11, DecorationArrayStride = 6, DecorationMatrixStride = 7, DecorationLocation = 30,
	DecorationBinding = 33, DecorationOffset = 35, DecorationHlslSemantic = 5635,
	StorageInput = 1, StorageUniform = 2, StorageOutput = 3,
};

std::string Literal(const uint32_t *words, uint32_t count)
{
	std::string text;
	for (uint32_t i = 0; i < count; i++)
		for (int byte = 0; byte < 4; byte++)
		{
			const char c = (char)(words[i] >> (byte * 8));
			if (!c)
				return text;
			text += c;
		}
	return text;
}

struct Type
{
	uint32_t op = 0;
	uint32_t a = 0, b = 0; // OpTypeInt/Float: width; Vector/Matrix: component type, count; Array: element, length id; Pointer: storage, type
	std::vector<uint32_t> members;
	uint32_t arrayStride = 0;
};

struct Module
{
	std::map<uint32_t, Type> types;
	std::map<uint32_t, uint32_t> constants;
	std::map<uint32_t, std::map<uint32_t, uint32_t>> memberOffsets, memberMatrixStrides;

	uint32_t Size(uint32_t id, uint32_t matrixStride = 0) const
	{
		auto found = types.find(id);
		if (found == types.end())
			return 0;
		const Type &type = found->second;
		switch (type.op)
		{
			case OpTypeInt:
			case OpTypeFloat:
				return type.a / 8;
			case OpTypeVector:
				return Size(type.a) * type.b;
			case OpTypeMatrix:
				// Columns (or rows, for a row-major one) a stride apart; the last one isn't padded
				return matrixStride ? matrixStride * (type.b - 1) + Size(type.a) : Size(type.a) * type.b;
			case OpTypeArray:
			{
				auto length = constants.find(type.b);
				const uint32_t count = length != constants.end() ? length->second : 1;
				return type.arrayStride ? type.arrayStride * count : Size(type.a) * count;
			}
			case OpTypeStruct:
			{
				uint32_t size = 0;
				auto offsets = memberOffsets.find(id);
				auto strides = memberMatrixStrides.find(id);
				for (uint32_t member = 0; member < type.members.size(); member++)
				{
					uint32_t offset = 0, stride = 0;
					if (offsets != memberOffsets.end() && offsets->second.count(member))
						offset = offsets->second.at(member);
					if (strides != memberMatrixStrides.end() && strides->second.count(member))
						stride = strides->second.at(member);
					size = std::max(size, offset + Size(type.members[member], stride));
				}
				return size;
			}
			default:
				return 0;
		}
	}
};

} // namespace

bool PatchShader(const void *code, size_t bytes, ShaderInfo &info, std::string &error)
{
	if (bytes < 20 || bytes % 4)
	{
		error = "not SPIR-V";
		return false;
	}
	const uint32_t *words = static_cast<const uint32_t *>(code);
	const size_t count = bytes / 4;
	if (words[0] != 0x07230203)
	{
		error = "not SPIR-V (compiled for Direct3D?)";
		return false;
	}

	Module module;
	std::map<uint32_t, std::string> semantics;
	std::map<uint32_t, uint32_t> locations; // glslang's, by variable
	std::map<uint32_t, uint32_t> storage, pointerOf, bindings, variableType;
	std::map<uint32_t, bool> builtin;
	bool vertex = false;

	for (size_t at = 5; at < count;)
	{
		const uint32_t length = words[at] >> 16, op = words[at] & 0xFFFF;
		if (!length || at + length > count)
		{
			error = "truncated SPIR-V";
			return false;
		}
		const uint32_t *w = words + at;
		switch (op)
		{
			case OpEntryPoint:
				vertex = w[1] == 0;
				info.entry = Literal(w + 3, length - 3);
				break;
			case OpTypeInt:
			case OpTypeFloat:
				module.types[w[1]].op = op;
				module.types[w[1]].a = w[2];
				break;
			case OpTypeVector:
			case OpTypeMatrix:
			case OpTypeArray:
				module.types[w[1]].op = op;
				module.types[w[1]].a = w[2];
				module.types[w[1]].b = w[3];
				break;
			case OpTypeStruct:
				module.types[w[1]].op = op;
				module.types[w[1]].members.assign(w + 2, w + length);
				break;
			case OpTypePointer:
				pointerOf[w[1]] = w[3];
				break;
			case OpConstant:
				module.constants[w[2]] = w[3];
				break;
			case OpVariable:
				storage[w[2]] = w[3];
				variableType[w[2]] = w[1]; // a pointer type
				break;
			case OpDecorate:
				if (w[2] == DecorationBinding)
					bindings[w[1]] = w[3];
				else if (w[2] == DecorationLocation)
					locations[w[1]] = w[3];
				else if (w[2] == DecorationBuiltIn)
					builtin[w[1]] = true;
				else if (w[2] == DecorationArrayStride)
					module.types[w[1]].arrayStride = w[3];
				break;
			case OpMemberDecorate:
				if (w[3] == DecorationOffset)
					module.memberOffsets[w[1]][w[2]] = w[4];
				else if (w[3] == DecorationMatrixStride)
					module.memberMatrixStrides[w[1]][w[2]] = w[4];
				break;
			case OpDecorateString:
				if (w[2] == DecorationHlslSemantic)
					semantics[w[1]] = Literal(w + 3, length - 3);
				break;
			default:
				break;
		}
		at += length;
	}

	// An array or a matrix with one semantic ("float4 uv[2] : TEXCOORD0") becomes several variables that all carry it,
	// at consecutive locations, and stands for that many semantic indices. Each keeps its distance from the first.
	std::map<std::string, uint32_t> firstLocation;
	for (const auto &semantic : semantics)
	{
		auto located = locations.find(semantic.first);
		if (located == locations.end() || !storage.count(semantic.first))
			continue;
		const std::string key = (storage[semantic.first] == StorageInput ? "i" : "o") + semantic.second;
		auto first = firstLocation.find(key);
		if (first == firstLocation.end() || located->second < first->second)
			firstLocation[key] = located->second;
	}

	// The second pass writes the module out with the new locations and without the semantics
	info.words.assign(words, words + 5);
	info.inputs.clear();
	for (size_t at = 5; at < count;)
	{
		const uint32_t length = words[at] >> 16, op = words[at] & 0xFFFF;
		const uint32_t *w = words + at;
		bool keep = true;
		uint32_t location = UINT32_MAX;
		if (op == OpDecorateString || op == OpMemberDecorateString)
			keep = false;
		else if (op == OpExtension)
		{
			const std::string name = Literal(w + 1, length - 1);
			keep = name != "SPV_GOOGLE_hlsl_functionality1" && name != "SPV_GOOGLE_decorate_string";
		}
		else if (op == OpDecorate && w[2] == DecorationLocation && semantics.count(w[1]) && !builtin.count(w[1]))
		{
			const std::string &semantic = semantics[w[1]];
			const uint32_t kind = storage.count(w[1]) ? storage[w[1]] : UINT32_MAX;
			// A pixel shader's outputs (SV_Target) keep glslang's locations, which are the target's index
			if (semantic.compare(0, 3, "SV_") != 0 && (kind == StorageInput || kind == StorageOutput))
			{
				location = SemanticLocation(semantic);
				if (location == UINT32_MAX)
				{
					error = "the semantic " + semantic + " has no location in vk12";
					return false;
				}
				location += w[3] - firstLocation[(kind == StorageInput ? "i" : "o") + semantic];
				if (vertex && kind == StorageInput)
					info.inputs.push_back({semantic, location});
			}
		}
		if (keep)
		{
			info.words.insert(info.words.end(), w, w + length);
			if (location != UINT32_MAX)
				info.words[info.words.size() - length + 3] = location;
		}
		at += length;
	}

	memset(info.uniformBytes, 0, sizeof info.uniformBytes);
	memset(info.usesBinding, 0, sizeof info.usesBinding);
	for (const auto &binding : bindings)
	{
		if (binding.second >= ARRAYSIZE(info.usesBinding))
		{
			error = "a binding is outside vk12's descriptor set";
			return false;
		}
		info.usesBinding[binding.second] = true;
		if (binding.second < kCbvCount && storage.count(binding.first) && storage[binding.first] == StorageUniform)
		{
			const uint32_t pointer = variableType[binding.first];
			const uint32_t size = pointerOf.count(pointer) ? module.Size(pointerOf[pointer]) : 0;
			// A uniform buffer's range is a multiple of 16 bytes, as Direct3D's constant buffers are
			info.uniformBytes[binding.second] = (size + 15) & ~15u;
		}
	}
	return true;
}

HRESULT CompileHlsl(const void *source, size_t size, const char *entry, const char *profile, ID3DBlob **code, ID3DBlob **errors)
{
	if (code)
		*code = nullptr;
	if (errors)
		*errors = nullptr;
	if (!source || !entry || !profile || !code)
		return E_INVALIDARG;
	const bool vertexStage = profile[0] == 'v';
	const EShLanguage stage = vertexStage ? EShLangVertex : EShLangFragment;

	static const bool initialized = glslang::InitializeProcess();
	std::string messages;
	std::vector<unsigned> spirv;
	if (!initialized)
		messages = "glslang initialization failed";
	else
	{
		const std::string text(static_cast<const char *>(source), size);
		const char *strings = text.c_str();
		const EShMessages rules = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
		glslang::TShader shader(stage);
		shader.setStrings(&strings, 1);
		shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 110);
		shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
		shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
		shader.setEnvTargetHlslFunctionality1();
		shader.setEntryPoint(entry);
		// As the build compiles shaders.hlsl (src/hardware/CMakeLists.txt)
		shader.setShiftBinding(glslang::EResUbo, 0);
		shader.setShiftBinding(glslang::EResTexture, kSrvBinding);
		shader.setShiftBinding(glslang::EResSampler, kSamplerBinding);
		shader.setDxPositionW(true);
		shader.setAutoMapLocations(true);
		glslang::TProgram program;
		if (!shader.parse(GetDefaultResources(), 110, false, rules))
			messages = shader.getInfoLog();
		else
		{
			program.addShader(&shader);
			if (!program.link(rules) || !program.mapIO())
				messages = program.getInfoLog();
			else
			{
				glslang::SpvOptions options;
				options.disableOptimizer = true;
				glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
			}
		}
	}

	if (spirv.empty())
	{
		if (errors)
		{
			Blob *blob = new Blob;
			blob->data.assign(messages.begin(), messages.end());
			*errors = blob;
		}
		return E_FAIL;
	}
	Blob *blob = new Blob;
	blob->data.resize(spirv.size() * sizeof(unsigned));
	memcpy(blob->data.data(), spirv.data(), blob->data.size());
	*code = blob;
	return S_OK;
}

} // namespace rr_vk12

#ifndef _WIN32
// Declared in vk12_win.h: where there is no Direct3D compiler, "compiling HLSL" means to SPIR-V, for vk12
HRESULT D3DCompile(const void *source, SIZE_T size, const char *, const void *, void *, const char *entry,
	const char *profile, UINT, UINT, ID3DBlob **code, ID3DBlob **errors)
{
	return rr_vk12::CompileHlsl(source, size, entry, profile, code, errors);
}
#endif
