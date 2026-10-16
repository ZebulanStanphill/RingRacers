// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
#include "d3d12_prewarm.h"
#include "d3d12_pipeline.h"
#include <thread>
#include <atomic>
#include <vector>
#include <cstdio>
namespace rr_d3d12
{
namespace
{
std::thread worker;
std::atomic<bool> stop{false}, done{true};
bool logged = true;
struct Entry
{
	HRESULT hr = S_OK;
	double ms = 0;
	bool attempted = false;
};
struct Result
{
	unsigned ok = 0, failures = 0;
	double total = 0, worst = 0, wall = 0;
	unsigned worstIndex = 0;
	Entry entries[36];
	bool threadStartFailed = false, workerException = false, invalidArguments = false;
} result;
std::vector<ComPtr<ID3D12PipelineState>> retained;
struct Combo
{
	VSKind vs;
	PSKind ps;
	Layout layout;
	DXGI_FORMAT depth;
};
const Combo combos[] = {
	{VS_POLY, PS_FIXED, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_SOFTWARE, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLYCLIP, PS_SOFTWARE, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_SKY, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_SOFTWARE, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODELLIGHT, PS_MODELLIGHT, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_FULLSCREEN, PS_PALETTEPOST, LAYOUT_NONE, DXGI_FORMAT_UNKNOWN},
	{VS_WIPE, PS_WIPE, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_FULLSCREEN, PS_WIPEFULL, LAYOUT_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_FULLSCREEN, PS_FADEMAP, LAYOUT_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POSTIMAGE, PS_POSTIMAGE, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_DEFAULT, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_DEFAULT, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_DEFAULT, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_SOFTWARE, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_MODELLIGHT, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_MODELLIGHT, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_WATER, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_WATER, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_WATER, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_WATERREFRACTION, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_WATERREFRACTION, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_WATERREFRACTION, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_FOG, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_FOG, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_FOG, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_FOGREMAP, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_FOGREMAP, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_FOGREMAP, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_POLY, PS_SKY, LAYOUT_POLY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_SKY, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_FIXED, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_FIXED, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_SKY, PS_WIPE, LAYOUT_SKY, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_MODEL, PS_WIPE, LAYOUT_MODEL, DXGI_FORMAT_D24_UNORM_S8_UINT},
	{VS_FULLSCREEN, PS_FIXED, LAYOUT_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT},
};
static_assert(ARRAYSIZE(combos) == 36, "Prewarm must cover all original probe combinations");

double Ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f)
{
	return f.QuadPart > 0 ? (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart : 0;
}
struct WorkerLifetime
{
	~WorkerLifetime()
	{
		Prewarm_Stop();
	}
} lifetime;
} // namespace
void Prewarm_Stop()
{
	stop.store(true, std::memory_order_relaxed);
	if (worker.joinable())
		worker.join();
	retained.clear();
}
bool Prewarm_Done()
{
	return done.load(std::memory_order_acquire);
}
void Prewarm_Start(ID3D12Device *device, ID3D12RootSignature *root)
{
	Prewarm_Stop();
	result = {};
	logged = false;
	if (!device || !root)
	{
		result.invalidArguments = true;
		done.store(true, std::memory_order_release);
		return;
	}
	stop.store(false, std::memory_order_relaxed);
	done.store(false, std::memory_order_release);
	ComPtr<ID3D12Device> heldDevice = device;
	ComPtr<ID3D12RootSignature> heldRoot = root;
	LARGE_INTEGER frequency = {}, wallBegin = {};
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&wallBegin);
	try
	{
		worker = std::thread(
			[heldDevice, heldRoot, frequency, wallBegin]()
			{
				LARGE_INTEGER wallEnd = {};
				try
				{
					retained.reserve(ARRAYSIZE(combos));
					for (unsigned i = 0; i < ARRAYSIZE(combos) && !stop.load(std::memory_order_relaxed); ++i)
					{
						const auto &c = combos[i];
						PipelineKey key;
						key.vs = c.vs;
						key.ps = c.ps;
						key.layout = c.layout;
						key.dsvFormat = c.depth;
						key.blend = 0x8000;
						key.depth = 2ULL | (8ULL << 4) | (1ULL << 8) | (1ULL << 12) | (1ULL << 16) | (255ULL << 20) |
									(255ULL << 28);
						auto desc = BuildPipelineDesc(heldRoot.Get(), key);
						// Original probe default differs from GL emulation only in winding, which is immaterial without
						// culling.
						desc.RasterizerState.FrontCounterClockwise = FALSE;
						LARGE_INTEGER begin = {}, end = {};
						ComPtr<ID3D12PipelineState> pso;
						QueryPerformanceCounter(&begin);
						HRESULT hr = heldDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
						QueryPerformanceCounter(&end);
						if (SUCCEEDED(hr) && !pso)
							hr = E_POINTER;
						const double ms = Ms(begin, end, frequency);
						result.total += ms;
						result.entries[i] = {hr, ms, true};
						if (ms > result.worst)
						{
							result.worst = ms;
							result.worstIndex = i;
						}
						if (SUCCEEDED(hr) && pso)
						{
							wchar_t name[128];
							swprintf(name, ARRAYSIZE(name), L"D3D12 prewarm PSO %u", i + 1);
							pso->SetName(name);
							retained.push_back(pso);
							++result.ok;
						}
						else
							++result.failures;
					}
				}
				catch (...)
				{
					result.workerException = true;
				}
				QueryPerformanceCounter(&wallEnd);
				result.wall = Ms(wallBegin, wallEnd, frequency);
				done.store(true, std::memory_order_release);
			});
	}
	catch (...)
	{
		result.threadStartFailed = true;
		done.store(true, std::memory_order_release);
		Core_Log("prewarm: worker thread could not be started");
		logged = true;
	}
}
void Prewarm_LogDone()
{
	if (logged || !Prewarm_Done())
		return;
	logged = true;
	const auto &worst = combos[result.worstIndex];
	Core_Log("prewarmed %u/36 pipelines on a worker thread in %.1f ms (worst %.1f ms, %s+%s)", result.ok, result.total,
			 result.worst, ShaderName(worst.vs), ShaderName(worst.ps));
	Core_Log("prewarm: wall time %.1f ms, failures %u", result.wall, result.failures);
	for (unsigned i = 0; i < ARRAYSIZE(combos); ++i)
	{
		const auto &entry = result.entries[i];
		if (entry.attempted && FAILED(entry.hr))
			Core_Log("prewarm: %s+%s failed 0x%08X %.1f ms", ShaderName(combos[i].vs), ShaderName(combos[i].ps),
					 unsigned(entry.hr), entry.ms);
	}
	if (result.workerException)
		Core_Log("prewarm: worker stopped after an exception");
	if (result.invalidArguments)
		Core_Log("prewarm: device or root signature unavailable");
}
} // namespace rr_d3d12
