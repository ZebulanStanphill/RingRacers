// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_placement.inc
/// \brief [UWP]: Puts the finished frame at a rectangle of the swap chain, for Screen Scaling's Integer and 1:1 modes

// Included by r_d3d12.cpp after the screen effect. Options > Video > Screen Scaling (cv_scr_scaling) is 0 Fit, 1 Integer
// or 2 1:1 (screen_scaling.hpp). Fit copies the frame into a swap chain of the frame's size, which the system scales to
// the window. The other two have the swap chain at the screen's size, and Present_Frame hands the back buffer to
// PlaceFrame: it is cleared to black and the frame, whatever the screen effect left (the screen, the palette step's
// texture or the effect's target), is drawn point-sampled over the placement's rectangle. 1:1 is the same draw with
// the rectangle as large as the frame, which samples every texel at its center, so a copy would give the same pixels.
//
// The pipeline is compiled when first needed, from the sources below, against the driver's root signature. If it
// can't be made, PlaceFrame returns false, having recorded nothing, and Present_Frame goes back to Fit.

// A triangle over the viewport, with the frame's texture coordinates, from 0 at its top left, across the viewport
// (the draw's viewport is the placement's rectangle)
constexpr char kPlaceVertexShader[] =
	"struct Output { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
	"Output main(uint id : SV_VertexID)\n"
	"{\n"
	"	Output o;\n"
	"	float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
	"	o.pos = float4(p, 0.0, 1.0);\n"
	"	o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
	"	return o;\n"
	"}\n";
constexpr char kPlacePixelShader[] =
	"Texture2D t_frame : register(t0);\n"
	"SamplerState s_point : register(s0);\n"
	"float4 main(float2 uv : TEXCOORD0) : SV_Target\n"
	"{\n"
	"	return float4(t_frame.SampleLevel(s_point, uv, 0).rgb, 1.0);\n"
	"}\n";

struct PlacePass
{
	enum State { UNTRIED, READY, FAILED } state = UNTRIED;
	ComPtr<ID3D12PipelineState> pso;
	UINT sampler = UINT32_MAX; // point filtered, clamped
};
PlacePass g_placePass;

// Makes the pipeline and sampler; false, logging why once, if it can't
bool PreparePlacePass()
{
	PlacePass &pass = g_placePass;
	if (pass.state == PlacePass::READY)
		return true;
	if (pass.state == PlacePass::FAILED)
		return false;

	pass.state = PlacePass::FAILED;
	std::string error;
	ComPtr<ID3DBlob> vertex, pixel;
	if (!CompileEffectStage(kPlaceVertexShader, "vs_5_1", 0, vertex, error)
		|| !CompileEffectStage(kPlacePixelShader, "ps_5_1", 0, pixel, error))
	{
		Log("Screen Scaling can't place frames: %s", error.c_str());
		return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = g_root.Get();
	desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
	desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
	desc.BlendState = MakeBlendDesc(1 << 15); // opaque, all channels
	// Neither depth nor stencil, with valid fixed-function values like the palette step's
	desc.DepthStencilState = MakeDepthDesc(2ULL | (8ULL << 4) | (1ULL << 8) | (1ULL << 12) | (1ULL << 16) | (255ULL << 20) | (255ULL << 28));
	desc.RasterizerState = MakeRasterDesc(0);
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = kScreenFormat; // the swap chain's format too
	desc.SampleDesc.Count = 1;
	if (Core_Failed(Core_Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pass.pso)), "CreateGraphicsPipelineState (screen scaling)"))
		return false;
	pass.pso->SetName(L"screen scaling");
	pass.sampler = CreateEffectSampler(D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_FILTER_MIN_MAG_MIP_POINT);
	if (pass.sampler == UINT32_MAX)
	{
		Log("Screen Scaling can't place frames: no room for its sampler");
		pass.pso.Reset();
		return false;
	}
	pass.state = PlacePass::READY;
	return true;
}

// A view of a texture of the screen's format, made when first needed
UINT TextureSrv(GpuTexture &texture)
{
	if (texture.srv == UINT32_MAX)
	{
		const UINT index = AllocateSrv();
		if (index == UINT32_MAX)
			return UINT32_MAX;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = kScreenFormat;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		Core_Device()->CreateShaderResourceView(texture.res.Get(), &srv, SrvCpu(index));
		texture.srv = index;
	}
	return texture.srv;
}

// The view of what D3D12_FinishUpdate presents: the screen, the palette step's texture or the screen effect's target
UINT PresentedSrv(ID3D12Resource *source)
{
	if (source == g_rt.Get())
		return EnsureScreenSrv() ? g_screenSrv : UINT32_MAX;
	if (source == g_postTexture.res.Get())
		return TextureSrv(g_postTexture);
	if (source == g_effectTarget.res.Get())
		return TextureSrv(g_effectTarget);
	return UINT32_MAX;
}

// Present_Frame's PlaceFrameFn. The target is in RENDER_TARGET and the source in sourceState, which it's left in.
bool PlaceFrame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, ID3D12Resource *target,
	D3D12_CPU_DESCRIPTOR_HANDLE rtv, const screen_scaling::Placement &placement, UINT targetWidth, UINT targetHeight)
{
	(void)target; // the render target view is what is drawn to
	if (!CanDraw() || placement.fit || placement.x < 0 || placement.y < 0 || placement.w <= 0 || placement.h <= 0
		|| (UINT)(placement.x + placement.w) > targetWidth || (UINT)(placement.y + placement.h) > targetHeight
		|| !PreparePlacePass())
		return false;
	const UINT srv = PresentedSrv(source);
	if (srv == UINT32_MAX)
	{
		D3D12_LOG_ONCE("Screen Scaling can't place frames: no view of the presented frame");
		return false;
	}

	ID3D12GraphicsCommandList *list = Core_List();
	const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	const D3D12_VIEWPORT viewport = {(float)placement.x, (float)placement.y, (float)placement.w, (float)placement.h, 0.0f, 1.0f};
	const D3D12_RECT scissor = {placement.x, placement.y, placement.x + placement.w, placement.y + placement.h};

	Core_Transition(source, sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Core_FlushBarriers();
	list->ClearRenderTargetView(rtv, black, 0, nullptr);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->SetPipelineState(g_placePass.pso.Get());
	// The root signature and descriptor heaps are set whenever the list opens (BindRenderTarget)
	list->SetGraphicsRootConstantBufferView(0, g_zeroConstants->GetGPUVirtualAddress());
	list->SetGraphicsRootDescriptorTable(1, SrvGpu(srv)); // t0: the frame
	list->SetGraphicsRootDescriptorTable(2, SamplerGpu(g_placePass.sampler)); // s0: point, clamped
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->DrawInstanced(3, 1, 0, 0);
	g_stats.psoChanges++;
	g_stats.rootSets += 2;
	g_stats.tableSets += 2;

	// The frame goes back to what it was; everything this pass set, and the caches of it, is stale
	Core_Transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sourceState);
	Core_FlushBarriers();
	BindRenderTarget();
	return true;
}

void ShutdownPlacePass(bool idle)
{
	if (idle)
		g_placePass.pso.Reset();
	else
		Core_Retire(g_placePass.pso);
	g_placePass = PlacePass();
}
