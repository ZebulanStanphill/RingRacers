// DR. ROBOTNIK'S RING RACERS
// This program is free software distributed under the terms of the GNU GPL v2.
// See the 'LICENSE' file for more details.
#ifndef RR_D3D12_INTEROP_H
#define RR_D3D12_INTEROP_H

// [UWP]: Isolated from engine headers: Windows defines the rpcndr.h 'small' macro.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

namespace rr_d3d12
{

using EGLConfig = void *;

// The driver loads EGL/GLES; the probe neither links ANGLE nor changes its current context.
struct EglApi
{
	void *display = nullptr;
	void *(WINAPI *CreatePbufferFromClientBuffer)(void *, unsigned, void *, void *, const int *) = nullptr;
	unsigned (WINAPI *DestroySurface)(void *, void *) = nullptr;
	unsigned (WINAPI *BindTexImage)(void *, void *, int) = nullptr;
	unsigned (WINAPI *ReleaseTexImage)(void *, void *, int) = nullptr;
	int (WINAPI *GetError)() = nullptr;
	void (WINAPI *GetIntegerv)(unsigned, int *) = nullptr;
	void (WINAPI *GenTextures)(int, unsigned *) = nullptr;
	void (WINAPI *DeleteTextures)(int, const unsigned *) = nullptr;
	void (WINAPI *BindTexture)(unsigned, unsigned) = nullptr;
	unsigned (WINAPI *GetGlError)() = nullptr;
	void (*LogMessage)(const char *) = nullptr;
};

struct InteropProbe
{
	bool device = false;
	bool share[3] = {false, false, false};
	bool gpuWait = false, gpuSignal = false;
};

InteropProbe D3D12Interop_Probe(ID3D11Device *angle, const EglApi &egl, EGLConfig config);

} // namespace rr_d3d12
#endif
