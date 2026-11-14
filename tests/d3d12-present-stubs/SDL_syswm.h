// The part of SDL's window manager information d3d12_present_win32.cpp reads (SDL_GetWindowWMInfo on Windows), for
// tests/d3d12-present.cpp, which supplies SDL_GetWindowWMInfo
#pragma once

#include <windows.h>

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_version
{
	unsigned char major, minor, patch;
} SDL_version;

#define SDL_VERSION(x) \
	{ \
		(x)->major = 2; \
		(x)->minor = 0; \
		(x)->patch = 0; \
	}

typedef enum
{
	SDL_SYSWM_UNKNOWN,
	SDL_SYSWM_WINDOWS
} SDL_SYSWM_TYPE;

typedef struct SDL_SysWMinfo
{
	SDL_version version;
	SDL_SYSWM_TYPE subsystem;
	union
	{
		struct
		{
			HWND window;
			HDC hdc;
			HINSTANCE hinstance;
		} win;
		unsigned char dummy[64];
	} info;
} SDL_SysWMinfo;

SDL_bool SDLCALL SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info);

#ifdef __cplusplus
}
#endif
