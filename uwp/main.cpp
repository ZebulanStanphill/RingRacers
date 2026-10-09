#include <Windows.h>
#include <cstdint>
#include <roapi.h>
#include <windows.system.h>
#include <windows.gaming.input.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include "SDL2/SDL.h"

extern "C" __declspec(dllimport) void* uwp_GetWindowReference();

// Query the process budget, rather than the physical memory of Xbox's VM.
// The engine uses this for its startup log and App-mode warning.
extern "C" int I_UWPGetMemoryBudget(std::uint64_t* limit, std::uint64_t* usage, int* isXbox)
{
	*limit = *usage = 0;
	*isXbox = 0;
	Microsoft::WRL::Wrappers::RoInitializeWrapper initialize(RO_INIT_MULTITHREADED);
	HRESULT result = initialize;
	if (FAILED(result) && result != RPC_E_CHANGED_MODE)
		return static_cast<int>(result);

	*isXbox = SDL_WinRTGetDeviceFamily() == SDL_WINRT_DEVICEFAMILY_XBOX;
	Microsoft::WRL::ComPtr<ABI::Windows::System::IMemoryManagerStatics> memory;
	result = RoGetActivationFactory(
		Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_System_MemoryManager).Get(),
		IID_PPV_ARGS(&memory));
	if (FAILED(result))
		return static_cast<int>(result);

	UINT64 budget = 0, used = 0;
	result = memory->get_AppMemoryUsageLimit(&budget);
	if (SUCCEEDED(result))
		result = memory->get_AppMemoryUsage(&used);
	if (SUCCEEDED(result))
	{
		*limit = budget;
		*usage = used;
	}
	return FAILED(result) ? static_cast<int>(result) : 0;
}

// Polled by the engine's file-check worker, so B works while the main thread is loading. Windows.Gaming.Input
// lists gamepads only some time after its factory is first created, so the worker joins the multithreaded
// apartment once and keeps the factory until the process exits; only that thread calls this.
extern "C" int I_UWPIsSkipButtonDown(void)
{
	static ABI::Windows::Gaming::Input::IGamepadStatics* statics = nullptr;
	if (!statics)
	{
		const HRESULT initialize = RoInitialize(RO_INIT_MULTITHREADED);
		if (FAILED(initialize) && initialize != RPC_E_CHANGED_MODE)
			return 0;
		if (FAILED(RoGetActivationFactory(
				Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Gaming_Input_Gamepad).Get(),
				IID_PPV_ARGS(&statics))))
		{
			statics = nullptr;
			return 0;
		}
	}

	Microsoft::WRL::ComPtr<ABI::Windows::Foundation::Collections::IVectorView<ABI::Windows::Gaming::Input::Gamepad*>> gamepads;
	unsigned int count = 0;
	if (FAILED(statics->get_Gamepads(&gamepads)) || FAILED(gamepads->get_Size(&count)))
		return 0;
	for (unsigned int i = 0; i < count; i++)
	{
		Microsoft::WRL::ComPtr<ABI::Windows::Gaming::Input::IGamepad> gamepad;
		ABI::Windows::Gaming::Input::GamepadReading reading = {};
		if (SUCCEEDED(gamepads->GetAt(i, &gamepad)) && SUCCEEDED(gamepad->GetCurrentReading(&reading))
			&& (reading.Buttons & ABI::Windows::Gaming::Input::GamepadButtons_B))
			return 1;
	}
	return 0;
}

static int bootstrap(int argc, char** argv)
{
	uwp_GetWindowReference(); // Call once on the UI thread to cache the CoreWindow for other threads

	return SDL_main(argc, argv);
}

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return SDL_WinRTRunApp(bootstrap, NULL);
}
