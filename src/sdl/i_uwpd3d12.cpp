// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_uwpd3d12.cpp
/// \brief [UWP]: Checks that Direct3D 12 hardware is available, and logs what might tell Xbox's App and Game app
///        types apart

#include <cstdio>
#include <cstdlib>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "SDL.h"

#include "../doomdef.h"
#include "../i_system.h"

using Microsoft::WRL::ComPtr;

namespace
{

// The Basic Render Driver, a software adapter
constexpr UINT kMicrosoftVendor = 0x1414;
constexpr UINT kBasicRenderDevice = 0x8C;

// Logs each adapter; returns whether a hardware adapter can create a feature level 11_0 device and a queue
bool HasD3D12Hardware()
{
	ComPtr<IDXGIFactory1> factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(hr))
	{
		I_OutputMsg("UWP: Direct3D 12 unavailable: CreateDXGIFactory1 failed (HRESULT 0x%08x)\n", (unsigned)hr);
		return false;
	}

	ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; index++, adapter.Reset())
	{
		DXGI_ADAPTER_DESC1 desc;
		if (FAILED(adapter->GetDesc1(&desc)))
			continue;

		char name[128];
		WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
		I_OutputMsg("UWP: Direct3D 12 check: adapter %u %s (vendor 0x%04X, device 0x%04X, flags 0x%X)\n", index, name,
			desc.VendorId, desc.DeviceId, desc.Flags);

		// The Xbox GPU also reports Microsoft's vendor ID, so only the software flag and Basic Render's device ID count
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			|| (desc.VendorId == kMicrosoftVendor && desc.DeviceId == kBasicRenderDevice))
			continue;

		ComPtr<ID3D12Device> device;
		hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		if (FAILED(hr))
		{
			I_OutputMsg("UWP: Direct3D 12 check: D3D12CreateDevice failed (HRESULT 0x%08x)\n", (unsigned)hr);
			continue;
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		ComPtr<ID3D12CommandQueue> queue;
		hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
		if (FAILED(hr))
		{
			I_OutputMsg("UWP: Direct3D 12 check: CreateCommandQueue failed (HRESULT 0x%08x)\n", (unsigned)hr);
			continue;
		}

		// Not yet known to differ between the app types; logged to find out
		ComPtr<IDXGIAdapter3> adapter3;
		DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonlocal = {};
		if (SUCCEEDED(adapter.As(&adapter3))
			&& SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))
			&& SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
		{
			I_OutputMsg("UWP: Direct3D 12 check: video memory budget %llu MB local, %llu MB non-local\n",
				local.Budget >> 20, nonlocal.Budget >> 20);
		}

		I_OutputMsg("UWP: Direct3D 12 check: supported\n");
		return true;
	}

	I_OutputMsg("UWP: Direct3D 12 unavailable: no hardware adapter supports it\n");
	return false;
}

} // namespace

void I_UWPRequireGameMode(void)
{
	// On an Xbox One X, the App type offered hardware Direct3D 12 at feature level 12_0 and the same 5120 MB budget
	// as Game, and Game launches have been seen with no processors marked as the game's, like App launches. None of
	// these tells the app types apart, so they're only logged, and only missing Direct3D 12 shows the pop-up.
	const int cpusetmode = I_UWPCpuSetMode();
	I_OutputMsg("UWP: app type signals: processors %s\n",
		cpusetmode == 1 ? "some are the game's" : cpusetmode == 0 ? "none are the game's" : "not reported");

	if (HasD3D12Hardware())
		return;

	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
		"Dr. Robotnik's Ring Racers " VERSIONSTRING,
		"The game is running as an app, which gets fewer CPU cores and less of the GPU.\n"
		"\n"
		"In Dev Home, set the app type to \"Game\":\n"
		"1. Under \"Games and Apps\", highlight \"Ring Racers\".\n"
		"2. Press the \"View\" button.\n"
		"3. Choose \"View details\".\n"
		"4. Change \"App type\" from \"App\" to \"Game\".\n"
		"\n"
		"Then start the game again.", NULL);

	// Nothing is set up this early. exit() would run static destructors and atexit handlers that abort, so flush
	// the log and leave without them
	fflush(NULL);
	_Exit(1);
}
