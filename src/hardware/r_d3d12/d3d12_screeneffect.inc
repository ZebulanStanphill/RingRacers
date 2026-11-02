// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_screeneffect.inc
/// \brief [UWP]: The SalCRT and SalCRT Sharp screen effects, as a last pass before the frame is presented

// Included by r_d3d12.cpp after palette rendering's last step. Options > Video > Screen Effect (cv_scr_effect) is
// 0 Nearest, 1 Sharp Bilinear, 2 SalCRT, 3 SalCRT Sharp. The first two only choose how the RHI path scales a smaller
// backbuffer up to the window; this driver draws the frame at its own size, so there is nothing for them to do.
// The other two run the game's CRT fragment shader over the finished frame, into a target of the same size. Screen Scaling
// (d3d12_placement.inc) then decides where on the screen the result goes.
//
// The shader is game data (shaders.pk3), not part of this repository: it is read with W_ReadShader, from the
// glsllist the RHI GL backend reads (rhi_glsllist_crt_fragment.txt, whose lines name the sources to concatenate), and
// translated to HLSL by custom_shader.cpp. Each mode builds its pipeline when first used. If anything fails, the mode
// logs why once and the frame is presented as it is, for the rest of the session.

// What shaders.pk3 calls the programs of modes 2 and 3
constexpr const char *kEffectPrograms[2] = {"crt", "crtsharp"};
constexpr const char *kEffectNames[2] = {"SalCRT", "SalCRT Sharp"};

// The RHI's blit_rect.cpp dot pattern, 12x4 RGBA8. The same table: it is only read there, from a function's static.
constexpr UINT8 kCrtDotPattern[] = {
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,

	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,

	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,

	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
};

// The game's vertex shader draws a quad from a vertex buffer; this one covers the screen with a triangle. v_texcoord0
// is the position in the frame, from 0 at its top left, like the texture's rows (the RHI's flip puts its origin there).
// v_color is white, as the RHI's vertex shader gives it without a color attribute.
// The position comes last: the translated pixel shader doesn't read it, and D3D links the stages by register, so
// the texcoords must sit in the registers the pixel shader's input signature puts them in.
constexpr char kEffectVertexShader[] =
	"struct Output { float2 uv : TEXCOORD0; float4 color : TEXCOORD1; float4 pos : SV_Position; };\n"
	"Output main(uint id : SV_VertexID)\n"
	"{\n"
	"	Output o;\n"
	"	float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
	"	o.pos = float4(p, 0.0, 1.0);\n"
	"	o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
	"	o.color = float4(1.0, 1.0, 1.0, 1.0);\n"
	"	return o;\n"
	"}\n";

// A mode's state: untried, ready, or given up
struct ScreenEffect
{
	enum State { UNTRIED, READY, FAILED } state = UNTRIED;
	ComPtr<ID3D12PipelineState> pso;
	int sizeOffset = -1; // where u_sampler0_size is in the constant buffer
	unsigned constantsBytes = 0;
};
ScreenEffect g_screenEffects[2];
ComPtr<ID3DBlob> g_effectVertexShader;

// Reads a lump of shaders.pk3 (or the flat shader directory) as text
bool ReadEffectLump(const std::string &name, std::string &text)
{
	size_t size = 0;
	if (!W_ReadShader(name.c_str(), &size, nullptr))
		return false;
	text.assign(size, '\0');
	if (size && !W_ReadShader(name.c_str(), &size, &text[0]))
		return false;
	text.resize(size);
	return true;
}

// The program's fragment sources in the order its glsllist names them, as the RHI GL backend lists them
bool ReadEffectSources(const char *program, std::vector<std::string> &sources, std::string &error)
{
	const std::string listName = std::string("rhi_glsllist_") + program + "_fragment.txt";
	std::string list;
	if (!ReadEffectLump(listName, list))
	{
		error = "no " + listName + " in the shaders";
		return false;
	}
	size_t position = 0;
	while (position <= list.size())
	{
		size_t end = list.find('\n', position);
		if (end == std::string::npos)
			end = list.size();
		std::string line = list.substr(position, end - position);
		position = end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		// Entries not ending in .glsl have it appended, as in the RHI
		if (line.size() < 5 || line.compare(line.size() - 5, 5, ".glsl") != 0)
			line += ".glsl";
		std::string source;
		if (!ReadEffectLump(line, source))
		{
			error = "no " + line + " in the shaders";
			return false;
		}
		sources.push_back(std::move(source));
	}
	if (sources.empty())
	{
		error = listName + " lists no sources";
		return false;
	}
	return true;
}

// D3DCompile without Core_Failed: a shader that doesn't compile is a fallback, not a driver error
bool CompileEffectStage(const std::string &hlsl, const char *profile, UINT flags, ComPtr<ID3DBlob> &code, std::string &error)
{
	ComPtr<ID3DBlob> messages;
	const HRESULT result = D3DCompile(hlsl.data(), hlsl.size(), "screen effect", nullptr, nullptr, "main", profile,
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | flags, 0, &code, &messages);
	if (SUCCEEDED(result))
		return true;
	char prefix[64];
	snprintf(prefix, sizeof prefix, "%s compilation failed (0x%08X)", profile, (unsigned)result);
	error = prefix;
	if (messages)
		error += std::string(": ") + std::string((const char *)messages->GetBufferPointer(), std::min<size_t>(messages->GetBufferSize(), 4096));
	code.Reset();
	return false;
}

UINT CreateEffectSampler(D3D12_TEXTURE_ADDRESS_MODE address, D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR)
{
	if (g_nextSampler >= 2048)
		return UINT32_MAX;
	D3D12_SAMPLER_DESC desc = {};
	desc.Filter = filter;
	desc.AddressU = desc.AddressV = desc.AddressW = address;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.MaxLOD = 0.0f; // neither texture has mips
	const UINT index = g_nextSampler++;
	auto handle = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * g_samplerStep;
	Core_Device()->CreateSampler(&desc, handle);
	return index;
}

// Builds a mode's pipeline: the sources, their HLSL, its bytecode and the pipeline state
bool BuildScreenEffect(ScreenEffect &effect, int mode, std::string &error)
{
	std::vector<std::string> sources;
	if (!ReadEffectSources(kEffectPrograms[mode], sources, error))
		return false;
	ScreenEffectHLSL translated;
	if (!TranslateScreenEffect(sources, translated, error))
	{
		error = "translation failed: " + error;
		return false;
	}
	// The constants are one 256-byte ring slot; a size the shader doesn't have is only a shader without one
	if (translated.constantsBytes > 256 || (translated.sampler0SizeOffset >= 0 && translated.sampler0SizeOffset + 8 > 256))
	{
		error = "its constants don't fit in 256 bytes";
		return false;
	}
	if (!g_effectVertexShader)
	{
		const std::string vertex = kEffectVertexShader;
		if (!CompileEffectStage(vertex, "vs_5_1", 0, g_effectVertexShader, error))
			return false;
	}
	// The shader's loops sample the frame; unrolled, they need no uniform flow control. Shader Model 5.1 only allows
	// that with every resource bound, which this pass always has (b0, t0, t1, s0, s1).
	ComPtr<ID3DBlob> pixel;
	if (!CompileEffectStage(translated.fragment, "ps_5_1", D3DCOMPILE_AVOID_FLOW_CONTROL | D3DCOMPILE_ALL_RESOURCES_BOUND, pixel, error))
		return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = g_root.Get();
	desc.VS = {g_effectVertexShader->GetBufferPointer(), g_effectVertexShader->GetBufferSize()};
	desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
	desc.BlendState = MakeBlendDesc(1 << 15); // opaque, all channels
	// Neither depth nor stencil, with valid fixed-function values like the palette step's
	desc.DepthStencilState = MakeDepthDesc(2ULL | (8ULL << 4) | (1ULL << 8) | (1ULL << 12) | (1ULL << 16) | (255ULL << 20) | (255ULL << 28));
	desc.RasterizerState = MakeRasterDesc(0);
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = kScreenFormat;
	desc.SampleDesc.Count = 1;
	const HRESULT result = Core_Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&effect.pso));
	if (FAILED(result))
	{
		char message[64];
		snprintf(message, sizeof message, "pipeline creation failed (0x%08X)", (unsigned)result);
		error = message;
		effect.pso.Reset();
		return false;
	}
	effect.pso->SetName(L"screen effect");
	effect.sizeOffset = translated.sampler0SizeOffset;
	effect.constantsBytes = translated.constantsBytes;
	return true;
}

// The shared resources: the dot pattern, the two samplers, and the target and views of this frame
bool PrepareEffectResources(UINT &sourceSrv, bool palette, std::string &error)
{
	if (g_effectSamplers[0] == UINT32_MAX || g_effectSamplers[1] == UINT32_MAX)
	{
		g_effectSamplers[0] = CreateEffectSampler(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		g_effectSamplers[1] = CreateEffectSampler(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
		if (g_effectSamplers[0] == UINT32_MAX || g_effectSamplers[1] == UINT32_MAX)
		{
			error = "no room for its samplers";
			return false;
		}
	}
	if (!g_dotPattern.res)
	{
		if (!CreateTextureStorage(&g_dotPattern, 0, 12, 4, DXGI_FORMAT_R8G8B8A8_UNORM, false, kCrtDotPattern))
		{
			error = "couldn't make its dot pattern texture";
			return false;
		}
		g_dotPattern.res->SetName(L"screen effect dot pattern");
	}
	if (!g_effectTarget.res || g_effectTarget.width != g_rtWidth || g_effectTarget.height != g_rtHeight)
	{
		RetireTexture(g_effectTarget);
		const D3D12_RESOURCE_DESC desc = g_rt->GetDesc();
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g_effectTarget.res)), "CreateCommittedResource (screen effect)"))
		{
			g_effectTarget.res.Reset();
			error = "couldn't make its render target";
			return false;
		}
		g_effectTarget.res->SetName(L"screen effect");
		g_effectTarget.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_effectTarget.width = g_rtWidth;
		g_effectTarget.height = g_rtHeight;
		g_effectTarget.format = desc.Format;
		Core_Device()->CreateRenderTargetView(g_effectTarget.res.Get(), nullptr, Core_Rtv(kRtvEffect));
		Log("made a %ux%u texture for the screen effect", g_rtWidth, g_rtHeight);
	}
	// The view of what is shown: the palette step's texture, or the screen
	if (palette)
	{
		if (g_postTexture.srv == UINT32_MAX)
		{
			const UINT index = AllocateSrv();
			if (index == UINT32_MAX)
			{
				error = "no room for a view of the palette step's texture";
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Format = kScreenFormat;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			Core_Device()->CreateShaderResourceView(g_postTexture.res.Get(), &srv, SrvCpu(index));
			g_postTexture.srv = index;
		}
		sourceSrv = g_postTexture.srv;
	}
	else
	{
		if (!EnsureScreenSrv())
		{
			error = "no room for a view of the screen";
			return false;
		}
		sourceSrv = g_screenSrv;
	}
	return true;
}

enum RunResult { RUN_DONE, RUN_SKIPPED, RUN_FAILED };

// Runs the effect over the palette step's texture or the screen (in RENDER_TARGET), leaving the result in
// g_effectTarget. Having recorded nothing, it returns RUN_FAILED if the effect can't be made, or RUN_SKIPPED if the
// frame can't take it, which says nothing about the next one.
RunResult RunScreenEffect(ScreenEffect &effect, bool palette, std::string &error)
{
	UINT sourceSrv = UINT32_MAX;
	if (!PrepareEffectResources(sourceSrv, palette, error))
		return RUN_FAILED;
	// Alloc can submit and reopen the list: do it before recording anything
	UploadAllocation constants = g_constantRing.Alloc(256, 256);
	if (!constants || !CanDraw())
		return RUN_SKIPPED;
	memset(constants.cpu, 0, 256);
	if (effect.sizeOffset >= 0)
	{
		// The frame's size in pixels, which is also the target's: the effect is drawn at the frame's own size
		const float size[2] = {(float)g_rtWidth, (float)g_rtHeight};
		memcpy(static_cast<UINT8 *>(constants.cpu) + effect.sizeOffset, size, sizeof size);
	}
	if (!g_constantRing.Touch(constants, 256))
		return RUN_SKIPPED;

	ID3D12GraphicsCommandList *list = Core_List();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kRtvEffect);
	const D3D12_VIEWPORT viewport = {0.0f, 0.0f, (float)g_rtWidth, (float)g_rtHeight, 0.0f, 1.0f};
	const D3D12_RECT scissor = {0, 0, (LONG)g_rtWidth, (LONG)g_rtHeight};

	Transition(g_effectTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(g_dotPattern, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (palette)
		Transition(g_postTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	else
		Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Core_FlushBarriers();
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->SetPipelineState(effect.pso.Get());
	list->SetGraphicsRootConstantBufferView(0, constants.gpu);
	list->SetGraphicsRootDescriptorTable(1, SrvGpu(sourceSrv)); // t0: s_sampler0, the frame
	list->SetGraphicsRootDescriptorTable(2, SamplerGpu(g_effectSamplers[0])); // s0: linear, clamped
	list->SetGraphicsRootDescriptorTable(3, SrvGpu(g_dotPattern.srv)); // t1: s_sampler1, the dot pattern
	list->SetGraphicsRootDescriptorTable(4, SamplerGpu(g_effectSamplers[1])); // s1: linear, repeating
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->DrawInstanced(3, 1, 0, 0);
	g_stats.psoChanges++;
	g_stats.rootSets += 2;
	g_stats.tableSets += 4;

	// The frame goes back to what it was; everything this pass set, and the caches of it, is stale
	if (palette)
		Transition(g_postTexture, D3D12_RESOURCE_STATE_RENDER_TARGET);
	else
		Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Core_FlushBarriers();
	BindRenderTarget();
	return RUN_DONE;
}

// What to present: source, the finished frame (the palette step's texture if palette, else the screen), or that frame
// through the screen effect. Either is in RENDER_TARGET.
ID3D12Resource *ApplyScreenEffect(bool palette)
{
	ID3D12Resource *plain = palette ? g_postTexture.res.Get() : g_rt.Get();
	const INT32 mode = cv_scr_effect.value;
	if ((mode != 2 && mode != 3) || !CanDraw() || Core_RemovalSeen())
		return plain;
	ScreenEffect &effect = g_screenEffects[mode - 2];
	if (effect.state == ScreenEffect::FAILED)
		return plain;

	const precise_t start = I_GetPreciseTime();
	std::string error;
	bool done = false;
	if (effect.state == ScreenEffect::UNTRIED)
	{
		if (BuildScreenEffect(effect, mode - 2, error))
		{
			effect.state = ScreenEffect::READY;
			Log("screen effect %s is ready (%.1f ms); constants %u bytes, frame size at %d", kEffectNames[mode - 2],
				PreciseToMs(I_GetPreciseTime() - start), effect.constantsBytes, effect.sizeOffset);
		}
		else
			effect.state = ScreenEffect::FAILED;
	}
	if (effect.state == ScreenEffect::READY)
	{
		const RunResult result = RunScreenEffect(effect, palette, error);
		done = result == RUN_DONE;
		if (result == RUN_FAILED)
			effect.state = ScreenEffect::FAILED;
	}
	if (effect.state == ScreenEffect::FAILED)
		Log("screen effect %s failed: %s; showing the frame without it", kEffectNames[mode - 2], error.c_str());
	g_stats.effectTime += I_GetPreciseTime() - start;
	if (!done)
		return plain;
	g_stats.effectPasses++;
	return g_effectTarget.res.Get();
}

// The pipelines are the only thing worth keeping between sessions: the rest follows the device
void ShutdownScreenEffect(bool idle)
{
	for (ScreenEffect &effect : g_screenEffects)
	{
		if (idle)
			effect.pso.Reset();
		else
			Core_Retire(effect.pso);
		effect = ScreenEffect();
	}
	g_effectVertexShader.Reset();
	RetireTexture(g_effectTarget);
	RetireTexture(g_dotPattern);
	g_effectSamplers[0] = g_effectSamplers[1] = UINT32_MAX;
}
