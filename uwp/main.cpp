#include <Windows.h>
#include "SDL2/SDL.h"

extern "C" __declspec(dllimport) void* uwp_GetWindowReference();

static int bootstrap(int argc, char** argv)
{
	uwp_GetWindowReference(); // Call once on the UI thread to cache the CoreWindow for other threads

	return SDL_main(argc, argv);
}

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return SDL_WinRTRunApp(bootstrap, NULL);
}
