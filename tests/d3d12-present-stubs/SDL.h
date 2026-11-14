// The few SDL declarations d3d12_present_win32.cpp and the engine headers it includes use (sdlmain.h, screen.h), for
// tests/d3d12-present.cpp, which supplies the definitions. The real SDL isn't linked.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;
typedef struct SDL_Window SDL_Window;
typedef struct _SDL_GameController SDL_GameController;
#ifndef SDLCALL
#ifdef _WIN32
#define SDLCALL __cdecl
#else
#define SDLCALL
#endif
#endif

const char *SDLCALL SDL_GetError(void);

#ifdef __cplusplus
}
#endif
