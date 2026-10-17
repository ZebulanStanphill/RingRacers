// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d3d12_present.cpp
/// \brief [UWP]: Shows VibeRant D3D12's frames on the window: with a swap chain of its own, or through ANGLE
///
/// By default, the first frame takes the window from ANGLE, whose window surface goes since a CoreWindow takes one
/// swap chain at a time, and each frame is copied into the next buffer of a swap chain of Direct3D 12's own and
/// presented from there. The window goes back to ANGLE when the Software renderer is chosen, and the next frame
/// drawn takes it again.
///
/// The interop with ANGLE is the fallback if the window can't be taken, and what gr_d3d12_present Readback and
/// CpuSync use. Presentation ported from the former Direct3D 11 driver, with D3D12 resources: each frame is copied
/// to one of four textures shared with ANGLE's device, which draws it to the window through OpenGL ES a frame later.
/// Two shared fences order the devices on the GPU: ANGLE waits for the copy's before reading the texture, and
/// D3D12's queue for ANGLE's last read of a texture before the submission that copies the next frame into it.
/// Without them (or with gr_d3d12_present CpuSync) the copy's fence is waited for on the CPU, and ANGLE's event
/// query for its read. How the texture is shared was probed on Xbox by d3d12_interop.cpp; the same paths are tried
/// here in order, with the same content check.

#include "d3d12_present.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

#include "SDL.h"
#ifdef _UWP
#include "SDL_syswm.h"
#endif

#include "../../doomdef.h"
#include "../../i_system.h"
#include "../../sdl/i_uwptrace.h"
#include "../../sdl/sdlmain.h"
#include "../hw_main.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

extern "C" __declspec(dllimport) void uwp_GetScreenSize(int*, int*);

#ifdef _UWP
// ringracers-uwp's patches/sdl-egl-surface-handoff.patch to SDL: destroys and recreates the window's EGL surface,
// ANGLE's swap chain on the CoreWindow, which the SDL headers this is built with don't declare
extern "C" __declspec(dllimport) int SDLCALL SDL_WinRTReleaseGLSurface(SDL_Window *window);
extern "C" __declspec(dllimport) int SDLCALL SDL_WinRTRestoreGLSurface(SDL_Window *window);
#endif

namespace rr_d3d12
{
namespace
{

#define Log Core_Log
#define LOG_ONCE D3D12_LOG_ONCE

double PreciseToMs(precise_t t)
{
	return t * 1000.0 / I_GetPrecisePrecision();
}

// ==========================================================================
//                                                        EGL AND OPENGL ES
// ==========================================================================

// Loaded at runtime from ANGLE
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLClientBuffer;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;

constexpr EGLint EGL_SUCCESS = 0x3000;
constexpr EGLint EGL_BAD_ATTRIBUTE = 0x3004;
constexpr EGLint EGL_ALPHA_SIZE = 0x3021;
constexpr EGLint EGL_BLUE_SIZE = 0x3022;
constexpr EGLint EGL_GREEN_SIZE = 0x3023;
constexpr EGLint EGL_RED_SIZE = 0x3024;
constexpr EGLint EGL_DEPTH_SIZE = 0x3025;
constexpr EGLint EGL_CONFIG_ID = 0x3028;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_BIND_TO_TEXTURE_RGBA = 0x303A;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040;
constexpr EGLint EGL_VENDOR = 0x3053;
constexpr EGLint EGL_VERSION = 0x3054;
constexpr EGLint EGL_EXTENSIONS = 0x3055;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_TEXTURE_RGBA = 0x305E;
constexpr EGLint EGL_TEXTURE_2D = 0x305F;
constexpr EGLint EGL_TEXTURE_FORMAT = 0x3080;
constexpr EGLint EGL_TEXTURE_TARGET = 0x3081;
constexpr EGLint EGL_BACK_BUFFER = 0x3084;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_OPENGL_ES2_BIT = 0x0004;
constexpr EGLint EGL_TRUE = 1;
constexpr EGLenum EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE = 0x3200;
constexpr EGLint EGL_DEVICE_EXT = 0x322C;
constexpr EGLint EGL_D3D11_DEVICE_ANGLE = 0x33A1;
constexpr EGLenum EGL_D3D_TEXTURE_ANGLE = 0x33A3;

#define EGLAPIENTRY WINAPI
using PFNEGLGETCURRENTDISPLAY = EGLDisplay (EGLAPIENTRY *)(void);
using PFNEGLQUERYSTRING = const char *(EGLAPIENTRY *)(EGLDisplay, EGLint);
using PFNEGLCHOOSECONFIG = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using PFNEGLCREATEPBUFFERSURFACE = EGLSurface (EGLAPIENTRY *)(EGLDisplay, EGLConfig, const EGLint *);
using PFNEGLMAKECURRENT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFNEGLQUERYCONTEXT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLContext, EGLint, EGLint *);
using PFNEGLGETCONFIGATTRIB = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLConfig, EGLint, EGLint *);
using PFNEGLCREATEPBUFFERFROMCLIENTBUFFER = EGLSurface (EGLAPIENTRY *)(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint *);
using PFNEGLDESTROYSURFACE = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface);
using PFNEGLBINDTEXIMAGE = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLSurface, EGLint);
using PFNEGLGETERROR = EGLint (EGLAPIENTRY *)(void);
using PFNEGLGETPROCADDRESS = void *(EGLAPIENTRY *)(const char *);
using PFNEGLQUERYDISPLAYATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLint, EGLAttrib *);
using PFNEGLQUERYDEVICEATTRIBEXT = EGLBoolean (EGLAPIENTRY *)(EGLDeviceEXT, EGLint, EGLAttrib *);

struct Egl
{
	bool loaded = false;
	PFNEGLGETCURRENTDISPLAY GetCurrentDisplay = nullptr;
	PFNEGLQUERYSTRING QueryString = nullptr;
	PFNEGLCHOOSECONFIG ChooseConfig = nullptr;
	PFNEGLCREATEPBUFFERSURFACE CreatePbufferSurface = nullptr;
	PFNEGLMAKECURRENT MakeCurrent = nullptr;
	PFNEGLQUERYCONTEXT QueryContext = nullptr;
	PFNEGLGETCONFIGATTRIB GetConfigAttrib = nullptr;
	PFNEGLCREATEPBUFFERFROMCLIENTBUFFER CreatePbufferFromClientBuffer = nullptr;
	PFNEGLDESTROYSURFACE DestroySurface = nullptr;
	PFNEGLBINDTEXIMAGE BindTexImage = nullptr;
	PFNEGLBINDTEXIMAGE ReleaseTexImage = nullptr;
	PFNEGLGETERROR GetError = nullptr;
	PFNEGLGETPROCADDRESS GetProcAddress = nullptr;
	PFNEGLQUERYDISPLAYATTRIBEXT QueryDisplayAttribEXT = nullptr;
	PFNEGLQUERYDEVICEATTRIBEXT QueryDeviceAttribEXT = nullptr;
};
Egl egl;

EGLint EglError()
{
	return egl.GetError ? egl.GetError() : 0;
}

void *GetEglFunction(HMODULE module, const char *name)
{
	void *function = module ? reinterpret_cast<void *>(::GetProcAddress(module, name)) : nullptr;

	if (!function && egl.GetProcAddress)
		function = egl.GetProcAddress(name);
	if (!function)
		function = SDL_GL_GetProcAddress(name);
	return function;
}

bool LoadEgl()
{
	if (egl.loaded)
		return egl.GetCurrentDisplay != nullptr;
	egl.loaded = true;

	HMODULE module = GetModuleHandleW(L"libEGL.dll");
	if (!module)
		Log("libEGL.dll isn't loaded (%lu); looking up EGL through SDL", (unsigned long)GetLastError());

#define LOADEGL(member, name) *reinterpret_cast<void **>(&egl.member) = GetEglFunction(module, name)
	LOADEGL(GetProcAddress, "eglGetProcAddress");
	LOADEGL(GetCurrentDisplay, "eglGetCurrentDisplay");
	LOADEGL(QueryString, "eglQueryString");
	LOADEGL(ChooseConfig, "eglChooseConfig");
	LOADEGL(CreatePbufferSurface, "eglCreatePbufferSurface");
	LOADEGL(MakeCurrent, "eglMakeCurrent");
	LOADEGL(QueryContext, "eglQueryContext");
	LOADEGL(GetConfigAttrib, "eglGetConfigAttrib");
	LOADEGL(CreatePbufferFromClientBuffer, "eglCreatePbufferFromClientBuffer");
	LOADEGL(DestroySurface, "eglDestroySurface");
	LOADEGL(BindTexImage, "eglBindTexImage");
	LOADEGL(ReleaseTexImage, "eglReleaseTexImage");
	LOADEGL(GetError, "eglGetError");
	LOADEGL(QueryDisplayAttribEXT, "eglQueryDisplayAttribEXT");
	LOADEGL(QueryDeviceAttribEXT, "eglQueryDeviceAttribEXT");
#undef LOADEGL

	Log("EGL functions: GetCurrentDisplay %d, ChooseConfig %d, CreatePbufferFromClientBuffer %d, BindTexImage %d",
		egl.GetCurrentDisplay != nullptr, egl.ChooseConfig != nullptr, egl.CreatePbufferFromClientBuffer != nullptr,
		egl.BindTexImage != nullptr);
	return egl.GetCurrentDisplay != nullptr;
}

// OpenGL ES 2
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned int GLbitfield;

constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_COLOR_CLEAR_VALUE = 0x0C22;
constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_STRIDE = 0x8624;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_TYPE = 0x8625;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x886A;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x889F;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;

#define GLAPIENTRY WINAPI
struct Gles
{
	bool loaded = false;
	void (GLAPIENTRY *ActiveTexture)(GLenum);
	void (GLAPIENTRY *AttachShader)(GLuint, GLuint);
	void (GLAPIENTRY *BindAttribLocation)(GLuint, GLuint, const GLchar *);
	void (GLAPIENTRY *BindBuffer)(GLenum, GLuint);
	void (GLAPIENTRY *BindFramebuffer)(GLenum, GLuint);
	void (GLAPIENTRY *BindTexture)(GLenum, GLuint);
	void (GLAPIENTRY *BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
	void (GLAPIENTRY *Clear)(GLbitfield);
	void (GLAPIENTRY *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (GLAPIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
	void (GLAPIENTRY *CompileShader)(GLuint);
	GLuint (GLAPIENTRY *CreateProgram)(void);
	GLuint (GLAPIENTRY *CreateShader)(GLenum);
	void (GLAPIENTRY *DeleteShader)(GLuint);
	void (GLAPIENTRY *DeleteTextures)(GLsizei, const GLuint *);
	void (GLAPIENTRY *Disable)(GLenum);
	void (GLAPIENTRY *DisableVertexAttribArray)(GLuint);
	void (GLAPIENTRY *DrawArrays)(GLenum, GLint, GLsizei);
	void (GLAPIENTRY *Enable)(GLenum);
	void (GLAPIENTRY *EnableVertexAttribArray)(GLuint);
	void (GLAPIENTRY *Finish)(void);
	void (GLAPIENTRY *Flush)(void);
	void (GLAPIENTRY *GenBuffers)(GLsizei, GLuint *);
	void (GLAPIENTRY *GenTextures)(GLsizei, GLuint *);
	void (GLAPIENTRY *GetBooleanv)(GLenum, GLboolean *);
	GLenum (GLAPIENTRY *GetError)(void);
	void (GLAPIENTRY *GetFloatv)(GLenum, GLfloat *);
	void (GLAPIENTRY *GetIntegerv)(GLenum, GLint *);
	void (GLAPIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
	void (GLAPIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	void (GLAPIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
	GLint (GLAPIENTRY *GetUniformLocation)(GLuint, const GLchar *);
	void (GLAPIENTRY *GetVertexAttribiv)(GLuint, GLenum, GLint *);
	void (GLAPIENTRY *GetVertexAttribPointerv)(GLuint, GLenum, void **);
	GLboolean (GLAPIENTRY *IsEnabled)(GLenum);
	void (GLAPIENTRY *LinkProgram)(GLuint);
	void (GLAPIENTRY *PixelStorei)(GLenum, GLint);
	void (GLAPIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
	void (GLAPIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (GLAPIENTRY *TexParameteri)(GLenum, GLenum, GLint);
	void (GLAPIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
	void (GLAPIENTRY *Uniform1i)(GLint, GLint);
	void (GLAPIENTRY *UseProgram)(GLuint);
	void (GLAPIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
	void (GLAPIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
};
Gles gl;

bool LoadGles()
{
	if (gl.loaded)
		return true;

	bool ok = true;
#define LOADGL(name) \
	if (!(*reinterpret_cast<void **>(&gl.name) = SDL_GL_GetProcAddress("gl" #name))) \
	{ \
		Log("couldn't find gl" #name); \
		ok = false; \
	}
	LOADGL(ActiveTexture)
	LOADGL(AttachShader)
	LOADGL(BindAttribLocation)
	LOADGL(BindBuffer)
	LOADGL(BindFramebuffer)
	LOADGL(BindTexture)
	LOADGL(BufferData)
	LOADGL(Clear)
	LOADGL(ClearColor)
	LOADGL(ColorMask)
	LOADGL(CompileShader)
	LOADGL(CreateProgram)
	LOADGL(CreateShader)
	LOADGL(DeleteShader)
	LOADGL(DeleteTextures)
	LOADGL(Disable)
	LOADGL(DisableVertexAttribArray)
	LOADGL(DrawArrays)
	LOADGL(Enable)
	LOADGL(EnableVertexAttribArray)
	LOADGL(Finish)
	LOADGL(Flush)
	LOADGL(GenBuffers)
	LOADGL(GenTextures)
	LOADGL(GetBooleanv)
	LOADGL(GetError)
	LOADGL(GetFloatv)
	LOADGL(GetIntegerv)
	LOADGL(GetProgramiv)
	LOADGL(GetShaderInfoLog)
	LOADGL(GetShaderiv)
	LOADGL(GetUniformLocation)
	LOADGL(GetVertexAttribiv)
	LOADGL(GetVertexAttribPointerv)
	LOADGL(IsEnabled)
	LOADGL(LinkProgram)
	LOADGL(PixelStorei)
	LOADGL(ShaderSource)
	LOADGL(TexImage2D)
	LOADGL(TexParameteri)
	LOADGL(TexSubImage2D)
	LOADGL(Uniform1i)
	LOADGL(UseProgram)
	LOADGL(VertexAttribPointer)
	LOADGL(Viewport)
#undef LOADGL

	gl.loaded = ok;
	return ok;
}

// ==========================================================================
//                                                                 PRESENTING
// ==========================================================================

constexpr int kPresentSlots = 4;
constexpr UINT kDirectBuffers = 3; // direct: the swap chain's buffers
constexpr UINT kDirectLatency = 2; // and the frames the CPU can be ahead of it
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // what the Xbox probe shared; the render target's too
constexpr UINT kProbeSize = 64;

// PATH_DIRECT: Direct3D 12's own swap chain on the window. Every other path is interop with ANGLE.
enum Path { PATH_NONE, PATH_A, PATH_B, PATH_C, PATH_D, PATH_DIRECT };

char PathName(Path path)
{
	return path == PATH_A ? 'A' : path == PATH_B ? 'B' : path == PATH_C ? 'C' : path == PATH_D ? 'D' : '-';
}

enum SyncMode { SYNC_CPU_FENCE, SYNC_SHARED_FENCES };

struct PresentSlot
{
	ComPtr<ID3D12Resource> resource; // shared with ANGLE (A-C), or a READBACK buffer (D)
	ComPtr<ID3D11Texture2D> texture; // ANGLE's view of the shared texture (A-C)
	ComPtr<ID3D11Texture2D> legacy; // C: what the EGL surface uses, copied from texture on ANGLE's device
	EGLSurface surface = nullptr;
	GLuint glTexture = 0;
	UINT64 copyFence = 0; // the submission that copied a frame into it
	bool copyShared = false; // the shared copy fence is signaled with copyFence too
	ComPtr<ID3D11Query> read; // completion of ANGLE's draw that samples this slot
	bool readPending = false;
	UINT64 readValue = 0; // shared fences: what ANGLE signals on the read fence after that draw; 0 for the query
	bool timing = false; // the frame's begin timestamp is recorded
	UINT64 timingFence = 0; // the submission that resolved its timestamps, until they're read
};

struct Presenter
{
	bool probed = false;
	Path path = PATH_NONE;
	bool cAngleOwned = false; // C's transport: B's rather than A's
	EGLDisplay display = nullptr;
	EGLConfig config = nullptr;
	PresentSlot slots[kPresentSlots];
	ComPtr<ID3D11Device> angleDevice;
	ComPtr<ID3D11Device1> angleDevice1;
	ComPtr<ID3D11DeviceContext> angleContext;
	bool deviceProbed = false, angleQueries = false;
	SyncMode syncMode = SYNC_CPU_FENCE;
	ComPtr<ID3D11DeviceContext4> angleContext4;
	ComPtr<ID3D12Fence> copyFence, readFence; // D3D12 signals copyFence for ANGLE; ANGLE signals readFence for D3D12
	ComPtr<ID3D11Fence> angleCopyFence, angleReadFence; // ANGLE's views of them
	UINT64 readValue = 0; // the last value ANGLE signaled on the read fence
	bool fencesTried = false, sharedFailed = false;
	precise_t lastPresent = 0;
	bool hasLastPresent = false;
	UINT width = 0, height = 0;
	UINT64 frame = 0;
	GLuint program = 0;
	GLuint quad = 0;
	float quadSize[2] = {0.0f, 0.0f};
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; // D: the frame's layout in the READBACK buffer
	UINT64 readbackBytes = 0;
	std::vector<BYTE> readback;
	ComPtr<ID3D12QueryHeap> timestamps; // a begin and end per slot
	ComPtr<ID3D12Resource> timestampBuffer; // READBACK, 16 bytes per slot
	bool timestampsTried = false;
	bool failed = false; // couldn't set up presenting, so stop trying
};
Presenter g_present;
bool g_presentImmediately = false;
INT32 g_traceCopied = -1, g_traceShown = -1;
UINT32 g_traceCopyFrame, g_traceShowFrame, g_traceSlotFrame[kPresentSlots];

struct PresentStats
{
	precise_t copyTime, glTime, swapTime;
	double copyWaitMs, worstCopyWaitMs;
	int copyWaits, copyWaitFailures;
	int angleReadWaits, angleReadTimeouts, fallbackFinishes;
	int gpuCopyWaits, gpuReadWaits; // shared fences: waits enqueued on ANGLE's context and D3D12's queue
	precise_t angleReadWaitTime;
	double gpuMs, worstGpuMs;
	int gpuFrames;
	double latencyWaitMs, worstLatencyWaitMs; // the swap chain: waiting for it to take another frame
	int latencyTimeouts;
	precise_t presentTime;
	double worstPresentMs;
	int presentFailures;
};
PresentStats g_stats;

int g_tracedFrames = 0;
constexpr int kTraceFrames = 3;

// Keep a reference: EGL owns the pointer returned by the device query.
void InitAngleDevice()
{
	if (g_present.deviceProbed)
		return;
	if (!LoadEgl())
	{
		g_present.deviceProbed = true;
		return;
	}
	const EGLDisplay display = egl.GetCurrentDisplay();
	if (!display)
		return; // retry once ANGLE's context is current
	g_present.deviceProbed = true;
	if (!egl.QueryDisplayAttribEXT || !egl.QueryDeviceAttribEXT)
		return;

	EGLAttrib device = 0, d3dDevice = 0;
	if (!egl.QueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &device) || !device)
		return;
	const EGLDeviceEXT eglDevice = reinterpret_cast<EGLDeviceEXT>(device);
	if (!egl.QueryDeviceAttribEXT(eglDevice, EGL_D3D11_DEVICE_ANGLE, &d3dDevice) || !d3dDevice)
	{
		Log("couldn't get ANGLE's device (EGL error 0x%X)", EglError());
		return;
	}
	g_present.angleDevice = reinterpret_cast<ID3D11Device *>(d3dDevice); // ComPtr assignment AddRefs
	g_present.angleDevice->GetImmediateContext(&g_present.angleContext);
	if (FAILED(g_present.angleDevice.As(&g_present.angleDevice1)))
		Log("ANGLE's device has no ID3D11Device1, so it can't open D3D12's textures");
	Log("ANGLE's device has feature level 0x%X", (unsigned)g_present.angleDevice->GetFeatureLevel());
}

// A copy on D3D12 must not overtake a pending read on ANGLE's device: waits on the CPU for its query, or for its
// value on the shared read fence
void WaitForAngleRead(PresentSlot &slot)
{
	if (!slot.readPending)
		return;
	const precise_t start = I_GetPreciseTime();
	bool blocked = false;
	UINT flags = 0; // allow one submission, then poll without repeatedly flushing
	for (;;)
	{
		if (slot.readValue)
		{
			// UINT64_MAX once D3D12's device is removed, which ends the wait too
			if (g_present.readFence->GetCompletedValue() >= slot.readValue)
				break;
			if (!blocked)
				g_present.angleContext->Flush(); // its Signal, if a swap hasn't flushed it
		}
		else
		{
			BOOL done = FALSE;
			const HRESULT hr = g_present.angleContext->GetData(slot.read.Get(), &done, sizeof done, flags);
			if (hr == S_OK && done)
				break;
			if (Core_Failed(hr, "GetData (ANGLE read finished)"))
				break;
		}
		blocked = true;
		if (PreciseToMs(I_GetPreciseTime() - start) >= 100.0)
		{
			g_stats.angleReadTimeouts++;
			LOG_ONCE("gave up waiting for ANGLE's slot read after 100 ms");
			break; // bounded wait, as with the copy fence
		}
		flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
		std::this_thread::yield();
	}
	if (blocked)
	{
		g_stats.angleReadWaits++;
		g_stats.angleReadWaitTime += I_GetPreciseTime() - start;
	}
	slot.readPending = false;
	slot.readValue = 0;
}

// A D3D12 fence ANGLE's device opens, as d3d12_interop.cpp's FenceProbe proved on Xbox. Nothing has used it on the
// GPU if this fails.
bool ShareFence(ID3D11Device5 *device5, const wchar_t *name, ComPtr<ID3D12Fence> &fence, ComPtr<ID3D11Fence> &angleFence,
	const char **step, HRESULT *hr)
{
	HANDLE handle = nullptr;
	*step = "CreateFence";
	*hr = Core_Device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
	if (FAILED(*hr))
		return false;
	*step = "CreateSharedHandle";
	*hr = Core_Device()->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
	if (SUCCEEDED(*hr) && (!handle || handle == INVALID_HANDLE_VALUE))
		*hr = E_HANDLE;
	if (FAILED(*hr))
		return false;
	*step = "OpenSharedFence";
	*hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(&angleFence));
	CloseHandle(handle);
	if (FAILED(*hr))
		return false;
	fence->SetName(name);
	return true;
}

// Made once and kept across resizes. ANGLE's event queries stay made too, for falling back.
bool InitSharedFences()
{
	if (g_present.sharedFailed)
		return false;
	if (g_present.copyFence)
		return true;
	if (g_present.fencesTried)
		return false;
	g_present.fencesTried = true;

	ComPtr<ID3D11Device5> device5;
	const char *step = "QueryInterface ID3D11Device5";
	HRESULT hr = g_present.angleDevice.As(&device5);
	if (SUCCEEDED(hr))
	{
		step = "QueryInterface ID3D11DeviceContext4";
		hr = g_present.angleContext.As(&g_present.angleContext4);
	}
	const bool ok = SUCCEEDED(hr)
		&& ShareFence(device5.Get(), L"present copy fence", g_present.copyFence, g_present.angleCopyFence, &step, &hr)
		&& ShareFence(device5.Get(), L"present read fence", g_present.readFence, g_present.angleReadFence, &step, &hr);
	if (!ok)
	{
		Log("present sync: shared fences unavailable (%s 0x%08X); cpu fence + %s", step, (unsigned)hr,
			g_present.angleQueries ? "ANGLE queries" : "glFinish");
		g_present.angleCopyFence.Reset();
		g_present.angleReadFence.Reset();
		g_present.copyFence.Reset();
		g_present.readFence.Reset();
		g_present.angleContext4.Reset();
		return false;
	}
	g_present.readValue = 0;
	return true;
}

// Called after allocating all slots, including on resize or readback fallback.
void InitPresentSync()
{
	InitAngleDevice();
	g_present.angleQueries = g_present.angleContext.Get() != nullptr;
	if (g_present.angleQueries)
	{
		const D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
		for (PresentSlot &slot : g_present.slots)
		{
			if (Core_Failed(g_present.angleDevice->CreateQuery(&desc, &slot.read), "CreateQuery (ANGLE slot read)"))
			{
				g_present.angleQueries = false;
				break;
			}
		}
	}
	if (!g_present.angleQueries)
	{
		for (PresentSlot &slot : g_present.slots)
			slot.read.Reset();
	}

	// Not for D, whose frames the CPU reads anyway; gr_d3d12_present CpuSync keeps the CPU waits to compare with
	g_present.syncMode = SYNC_CPU_FENCE;
	if (g_present.path != PATH_D && g_present.angleContext)
	{
		if (cv_gld3d12present.string && cv_gld3d12present.value == 2)
			LOG_ONCE("present sync: shared fences not used (gr_d3d12_present CpuSync)");
		else if (InitSharedFences())
			g_present.syncMode = SYNC_SHARED_FENCES;
	}

	if (g_present.syncMode == SYNC_SHARED_FENCES)
		LOG_ONCE("present sync: shared fences");
	else if (g_present.angleQueries)
		LOG_ONCE("present sync: cpu fence + ANGLE queries");
	else
		LOG_ONCE("present sync: cpu fence + glFinish on fast frames (under 8 ms)");
}

// A shared fence call failed: the CPU waits for the rest of the run, resizes included. Slots ANGLE read under the
// shared fences keep their read fence values, which WaitForAngleRead waits for on the CPU.
void FallBackToCpuSync(const char *what, HRESULT hr)
{
	g_present.sharedFailed = true;
	Log("present sync: %s failed (0x%08X); cpu fence + %s from now on", what, (unsigned)hr,
		g_present.angleQueries ? "ANGLE queries" : "glFinish");
	g_present.syncMode = SYNC_CPU_FENCE;
}

void FinishAngleRead(PresentSlot &slot, bool fastFrame)
{
	if (g_present.syncMode == SYNC_SHARED_FENCES)
	{
		// After DrawToWindow on ANGLE's immediate context, like the query's End. Flushed at once, so that the queue
		// wait for it before the slot's next copy can't stall on ANGLE's buffered commands.
		const UINT64 value = g_present.readValue + 1;
		const HRESULT hr = g_present.angleContext4->Signal(g_present.angleReadFence.Get(), value);
		if (SUCCEEDED(hr))
		{
			g_present.angleContext4->Flush();
			g_present.readValue = value;
			slot.readValue = value;
			slot.readPending = true;
			return;
		}
		FallBackToCpuSync("ID3D11DeviceContext4::Signal (present read)", hr);
	}
	if (g_present.angleQueries)
	{
		// ANGLE issues GL draws directly on its immediate context, so End is ordered after DrawToWindow.
		// It covers any earlier read under the shared fences too.
		g_present.angleContext->End(slot.read.Get());
		slot.readPending = true;
		slot.readValue = 0;
	}
	else if (fastFrame)
	{
		gl.Finish();
		g_stats.fallbackFinishes++;
	}
}

// Without the error lost to a second eglGetError
EGLSurface CreatePbuffer(EGLenum type, EGLClientBuffer buffer, EGLConfig config, UINT width, UINT height, EGLint *error)
{
	const EGLint attributes[] =
	{
		EGL_WIDTH, (EGLint)width,
		EGL_HEIGHT, (EGLint)height,
		EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
		EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
		EGL_NONE,
	};
	EGLSurface surface = egl.CreatePbufferFromClientBuffer(g_present.display, type, buffer, config, attributes);
	*error = surface ? EGL_SUCCESS : EglError();
	if (!surface && type == EGL_D3D_TEXTURE_ANGLE && *error == EGL_BAD_ATTRIBUTE)
	{
		// Older ANGLE builds take the size from the texture and reject EGL_WIDTH/EGL_HEIGHT here
		surface = egl.CreatePbufferFromClientBuffer(g_present.display, type, buffer, config, attributes + 4);
		*error = surface ? EGL_SUCCESS : EglError();
	}
	return surface;
}

// A (angleOwned false): a D3D12 texture ANGLE's device opens. B: a texture of ANGLE's device that D3D12 opens. The
// NT handle is closed once both sides have it.
bool CreateSharedTexture(bool angleOwned, UINT width, UINT height, const wchar_t *name, ComPtr<ID3D12Resource> &resource,
	ComPtr<ID3D11Texture2D> &texture)
{
	ID3D12Device *device = Core_Device();
	HANDLE handle = nullptr;
	bool ok;
	resource.Reset();
	texture.Reset();
	if (angleOwned)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = kFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		ComPtr<IDXGIResource1> dxgi;
		ok = !Core_Failed(g_present.angleDevice->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D (ANGLE, NT shared)")
			&& !Core_Failed(texture.As(&dxgi), "QueryInterface IDXGIResource1")
			&& !Core_Failed(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
				&handle), "CreateSharedHandle (ANGLE)")
			&& handle
			&& !Core_Failed(device->OpenSharedHandle(handle, IID_PPV_ARGS(&resource)), "OpenSharedHandle");
	}
	else
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = kFormat;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ok = g_present.angleDevice1
			&& !Core_Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
				nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource (shared)")
			&& !Core_Failed(device->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "CreateSharedHandle")
			&& handle
			&& !Core_Failed(g_present.angleDevice1->OpenSharedResource1(handle, IID_PPV_ARGS(&texture)), "OpenSharedResource1");
	}
	if (handle && handle != INVALID_HANDLE_VALUE)
		CloseHandle(handle);
	if (!ok)
	{
		// Nothing has used them on the GPU yet
		resource.Reset();
		texture.Reset();
		return false;
	}
	resource->SetName(name);
	return true;
}

// C: a texture of ANGLE's device with a legacy share handle, which ANGLE's pbuffers have always opened. Legacy
// handles aren't NT handles: never close them.
bool CreateLegacyTexture(UINT width, UINT height, ComPtr<ID3D11Texture2D> &texture, HANDLE *handle)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = kFormat;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	*handle = nullptr;
	ComPtr<IDXGIResource> resource;
	if (Core_Failed(g_present.angleDevice->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D (ANGLE, legacy shared)")
		|| Core_Failed(texture.As(&resource), "QueryInterface IDXGIResource")
		|| Core_Failed(resource->GetSharedHandle(handle), "GetSharedHandle")
		|| !*handle)
	{
		texture.Reset();
		return false;
	}
	return true;
}

// Clears the D3D12 side and reads it back on ANGLE's, like the S0a probe. Call with nothing else of the frame's
// recorded that matters: the clear is submitted at once.
bool CheckContent(ID3D12Resource *resource, ID3D11Texture2D *texture, ID3D11Texture2D *legacy, const char *scope)
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list)
		return false;

	const float color[] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
	const UINT expected = 0xFFC08040;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kRtvScratch);
	Core_Device()->CreateRenderTargetView(resource, nullptr, rtv);
	Core_Transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Core_FlushBarriers();
	list->ClearRenderTargetView(rtv, color, 0, nullptr);
	Core_Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	const UINT64 fence = Core_Submit("probe");
	if (!fence || !Core_Wait(fence, 500, "the interop test clear"))
	{
		Log("%s: the test clear didn't finish", scope);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	const D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
	ComPtr<ID3D11Texture2D> staging;
	ComPtr<ID3D11Query> query;
	if (Core_Failed(g_present.angleDevice->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D (interop staging)")
		|| Core_Failed(g_present.angleDevice->CreateQuery(&queryDesc, &query), "CreateQuery (interop)"))
		return false;

	ID3D11DeviceContext *context = g_present.angleContext.Get();
	if (legacy)
		context->CopyResource(legacy, texture);
	context->CopyResource(staging.Get(), legacy ? legacy : texture);
	context->End(query.Get());
	context->Flush();
	const precise_t start = I_GetPreciseTime();
	for (;;)
	{
		BOOL done = FALSE;
		const HRESULT hr = context->GetData(query.Get(), &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hr == S_OK && done)
			break;
		if (FAILED(hr) || PreciseToMs(I_GetPreciseTime() - start) >= 100.0)
		{
			Log("%s: ANGLE's copy query 0x%08X, %s", scope, (unsigned)hr, FAILED(hr) ? "failed" : "timed out after 100 ms");
			return false;
		}
		std::this_thread::yield();
	}

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (Core_Failed(context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped), "Map (interop staging)"))
		return false;
	UINT pixel = 0;
	const bool valid = mapped.pData && mapped.RowPitch >= kProbeSize * 4;
	if (valid)
		memcpy(&pixel, mapped.pData, sizeof pixel);
	context->Unmap(staging.Get(), 0);
	if (valid && pixel == expected)
	{
		Log("%s: content ok", scope);
		return true;
	}
	Log("%s: mismatch got 0x%08X expected 0x%08X%s", scope, pixel, expected, valid ? "" : " (invalid mapping)");
	return false;
}

// Makes a test texture the path's way, finds the first config ANGLE opens it with
// (config 0 fails with 0x300C on Xbox), then checks its content.
bool TryPath(Path path, const EGLConfig *configs, EGLint count)
{
	const char *scope = path == PATH_A ? "share A" : path == PATH_B ? "share B" : "share C";
	ComPtr<ID3D12Resource> resource;
	ComPtr<ID3D11Texture2D> texture, legacy;
	bool angleOwned = path == PATH_B;
	if (!CreateSharedTexture(angleOwned, kProbeSize, kProbeSize, L"interop test", resource, texture))
	{
		if (path != PATH_C)
		{
			Log("%s: couldn't share a texture", scope);
			return false;
		}
		Log("%s: retrying with an ANGLE-owned texture", scope);
		angleOwned = true;
		if (!CreateSharedTexture(true, kProbeSize, kProbeSize, L"interop test", resource, texture))
		{
			Log("%s: couldn't share a texture", scope);
			return false;
		}
	}

	HANDLE legacyHandle = nullptr;
	if (path == PATH_C && !CreateLegacyTexture(kProbeSize, kProbeSize, legacy, &legacyHandle))
	{
		Log("%s: couldn't make the legacy shared texture", scope);
		return false; // unused on the GPU, so released at once
	}

	const EGLenum type = path == PATH_C ? EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE : EGL_D3D_TEXTURE_ANGLE;
	const EGLClientBuffer buffer = path == PATH_C ? static_cast<EGLClientBuffer>(legacyHandle)
		: static_cast<EGLClientBuffer>(texture.Get());
	GLint previousTexture = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	EGLSurface surface = nullptr;
	GLuint glTexture = 0;
	int chosen = -1;
	for (EGLint i = 0; i < count && chosen < 0; i++)
	{
		EGLint error = 0;
		surface = CreatePbuffer(type, buffer, configs[i], kProbeSize, kProbeSize, &error);
		if (!surface)
		{
			Log("%s: ANGLE couldn't open it with config %d (EGL error 0x%X)", scope, (int)i, error);
			continue;
		}
		gl.GenTextures(1, &glTexture);
		gl.BindTexture(GL_TEXTURE_2D, glTexture);
		if (egl.BindTexImage(g_present.display, surface, EGL_BACK_BUFFER))
		{
			chosen = (int)i;
			break;
		}
		Log("%s: eglBindTexImage failed with config %d (EGL error 0x%X)", scope, (int)i, EglError());
		egl.DestroySurface(g_present.display, surface);
		gl.DeleteTextures(1, &glTexture);
		surface = nullptr;
		glTexture = 0;
	}

	const bool ok = chosen >= 0 && CheckContent(resource.Get(), texture.Get(), legacy.Get(), scope);

	// The EGL surface before the textures it uses
	if (surface)
	{
		gl.BindTexture(GL_TEXTURE_2D, glTexture);
		egl.ReleaseTexImage(g_present.display, surface, EGL_BACK_BUFFER);
		egl.DestroySurface(g_present.display, surface);
	}
	if (glTexture)
		gl.DeleteTextures(1, &glTexture);
	gl.BindTexture(GL_TEXTURE_2D, previousTexture);
	legacy.Reset();
	texture.Reset();
	Core_Retire(resource); // still in use if the clear timed out

	if (ok)
	{
		g_present.config = configs[chosen];
		g_present.cAngleOwned = angleOwned;
		Log("interop path %c (pbuffer config %d%s)", PathName(path), chosen,
			path == PATH_C ? (angleOwned ? ", ANGLE-owned texture" : ", D3D12-owned texture") : "");
	}
	return ok;
}

void DestroySlots(bool idle)
{
	GLint previousTexture = 0;
	if (gl.loaded)
		gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	for (PresentSlot &slot : g_present.slots)
	{
		WaitForAngleRead(slot);
		// The EGL surface first, then ANGLE's textures it uses, then D3D12's
		if (slot.surface)
		{
			if (slot.glTexture)
			{
				gl.BindTexture(GL_TEXTURE_2D, slot.glTexture);
				egl.ReleaseTexImage(g_present.display, slot.surface, EGL_BACK_BUFFER);
			}
			egl.DestroySurface(g_present.display, slot.surface);
		}
		if (slot.glTexture)
			gl.DeleteTextures(1, &slot.glTexture);
		slot.legacy.Reset();
		slot.texture.Reset();
		if (idle)
			slot.resource.Reset();
		else
			Core_Retire(slot.resource);
		slot = PresentSlot();
	}
	if (gl.loaded)
		gl.BindTexture(GL_TEXTURE_2D, previousTexture);
	g_present.width = g_present.height = 0;
}

void CreateTimestamps()
{
	if (g_present.timestampsTried)
		return;
	g_present.timestampsTried = true;
	if (!Core_TimestampFrequency())
		return;

	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count = 2 * kPresentSlots;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = kPresentSlots * 2 * sizeof(UINT64);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (Core_Failed(Core_Device()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&g_present.timestamps)), "CreateQueryHeap (timestamps)")
		|| Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, IID_PPV_ARGS(&g_present.timestampBuffer)), "CreateCommittedResource (timestamps)"))
	{
		g_present.timestamps.Reset();
		g_present.timestampBuffer.Reset();
		return;
	}
	g_present.timestampBuffer->SetName(L"GPU timestamps");
}

// Makes the textures frames are presented from, at the size of the screen. Call with ANGLE's context current.
bool CreateSlots(UINT width, UINT height)
{
	DestroySlots(false);
	CreateTimestamps();

	GLint previousTexture = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

	if (g_present.path == PATH_D)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = kFormat;
		desc.SampleDesc.Count = 1;
		Core_Device()->GetCopyableFootprints(&desc, 0, 1, 0, &g_present.footprint, nullptr, nullptr, &g_present.readbackBytes);
	}

	bool ok = true;
	for (int i = 0; i < kPresentSlots && ok; i++)
	{
		PresentSlot &slot = g_present.slots[i];
		wchar_t name[32];
		swprintf(name, sizeof name / sizeof *name, L"present slot %d", i);

		gl.GenTextures(1, &slot.glTexture);
		gl.BindTexture(GL_TEXTURE_2D, slot.glTexture);
		// Like DrawScreenFinalTexture's
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		if (g_present.path == PATH_D)
		{
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = g_present.readbackBytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ok = !Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.resource)), "CreateCommittedResource (readback)");
			if (ok)
			{
				slot.resource->SetName(name);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			}
			continue;
		}

		const bool angleOwned = g_present.path == PATH_B || (g_present.path == PATH_C && g_present.cAngleOwned);
		HANDLE legacyHandle = nullptr;
		if (!CreateSharedTexture(angleOwned, width, height, name, slot.resource, slot.texture)
			|| (g_present.path == PATH_C && !CreateLegacyTexture(width, height, slot.legacy, &legacyHandle)))
		{
			ok = false;
			break;
		}
		EGLint error = 0;
		if (g_present.path == PATH_C)
			slot.surface = CreatePbuffer(EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, legacyHandle, g_present.config, width, height, &error);
		else
			slot.surface = CreatePbuffer(EGL_D3D_TEXTURE_ANGLE, slot.texture.Get(), g_present.config, width, height, &error);
		if (!slot.surface)
		{
			Log("ANGLE couldn't open the %ux%u shared texture (EGL error 0x%X)", width, height, error);
			ok = false;
			break;
		}
		// The GL texture's storage stays the shared texture's, so it shows whatever was last copied to it
		if (!egl.BindTexImage(g_present.display, slot.surface, EGL_BACK_BUFFER))
		{
			Log("eglBindTexImage failed (EGL error 0x%X)", EglError());
			ok = false;
			break;
		}
	}

	gl.BindTexture(GL_TEXTURE_2D, previousTexture);
	if (!ok)
		return false;
	g_present.width = width;
	g_present.height = height;
	g_present.frame = 0;
	g_present.hasLastPresent = false;
	InitPresentSync();

	const GLenum error = gl.GetError();
	Log("made %d %ux%u presentation textures (interop path %c; GL error 0x%X)", kPresentSlots, width, height,
		PathName(g_present.path), error);
	return true;
}

GLuint CompileGlShader(GLenum type, const char *source)
{
	GLuint shader = gl.CreateShader(type);
	GLint status = 0;

	gl.ShaderSource(shader, 1, &source, nullptr);
	gl.CompileShader(shader);
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char info[1024] = "";
		gl.GetShaderInfoLog(shader, sizeof info, nullptr, info);
		Log("presentation shader didn't compile: %s", info);
		gl.DeleteShader(shader);
		return 0;
	}
	return shader;
}

bool CreatePresentProgram()
{
	static const char *vertexSource =
		"attribute vec2 a_position;\n"
		"attribute vec2 a_texcoord;\n"
		"varying vec2 v_texcoord;\n"
		"void main()\n"
		"{\n"
		"	v_texcoord = a_texcoord;\n"
		"	gl_Position = vec4(a_position, 0.0, 1.0);\n"
		"}\n";
	static const char *fragmentSource =
		"precision mediump float;\n"
		"uniform sampler2D u_texture;\n"
		"varying vec2 v_texcoord;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = vec4(texture2D(u_texture, v_texcoord).rgb, 1.0);\n"
		"}\n";

	if (g_present.program)
		return true;

	GLuint vertex = CompileGlShader(GL_VERTEX_SHADER, vertexSource);
	GLuint fragment = CompileGlShader(GL_FRAGMENT_SHADER, fragmentSource);
	if (!vertex || !fragment)
		return false;

	GLuint program = gl.CreateProgram();
	GLint status = 0, previousProgram = 0;

	gl.AttachShader(program, vertex);
	gl.AttachShader(program, fragment);
	gl.BindAttribLocation(program, 0, "a_position");
	gl.BindAttribLocation(program, 1, "a_texcoord");
	gl.LinkProgram(program);
	gl.DeleteShader(vertex);
	gl.DeleteShader(fragment);
	gl.GetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		Log("presentation shader didn't link");
		return false;
	}

	gl.GetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
	gl.UseProgram(program);
	gl.Uniform1i(gl.GetUniformLocation(program, "u_texture"), 0);
	gl.UseProgram(previousProgram);

	g_present.program = program;
	gl.GenBuffers(1, &g_present.quad);
	return true;
}

// The OpenGL ES state of the GL2 RHI that presenting changes, to put it back
struct SavedGlState
{
	GLint program, activeTexture, texture, arrayBuffer, framebuffer, viewport[4], unpackAlignment;
	GLboolean blend, depthTest, scissorTest, cullFace, stencilTest, colorMask[4];
	GLfloat clearColor[4];
	struct
	{
		GLint enabled, size, type, normalized, stride, buffer;
		void *pointer;
	} attributes[2];
};

void SaveGlState(SavedGlState &s)
{
	gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
	gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
	gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
	gl.GetIntegerv(GL_VIEWPORT, s.viewport);
	gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlignment);
	s.blend = gl.IsEnabled(GL_BLEND);
	s.depthTest = gl.IsEnabled(GL_DEPTH_TEST);
	s.scissorTest = gl.IsEnabled(GL_SCISSOR_TEST);
	s.cullFace = gl.IsEnabled(GL_CULL_FACE);
	s.stencilTest = gl.IsEnabled(GL_STENCIL_TEST);
	gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
	gl.GetFloatv(GL_COLOR_CLEAR_VALUE, s.clearColor);

	for (GLuint i = 0; i < 2; i++)
	{
		auto &a = s.attributes[i];
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
		gl.GetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
	}
}

void SetEnabled(GLenum capability, GLboolean enabled)
{
	if (enabled)
		gl.Enable(capability);
	else
		gl.Disable(capability);
}

void RestoreGlState(const SavedGlState &s)
{
	for (GLuint i = 0; i < 2; i++)
	{
		const auto &a = s.attributes[i];
		gl.BindBuffer(GL_ARRAY_BUFFER, a.buffer);
		gl.VertexAttribPointer(i, a.size, a.type, (GLboolean)a.normalized, a.stride, a.pointer);
		if (a.enabled)
			gl.EnableVertexAttribArray(i);
		else
			gl.DisableVertexAttribArray(i);
	}

	gl.BindBuffer(GL_ARRAY_BUFFER, s.arrayBuffer);
	gl.UseProgram(s.program);
	gl.BindTexture(GL_TEXTURE_2D, s.texture);
	gl.ActiveTexture(s.activeTexture);
	gl.BindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
	gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlignment);
	SetEnabled(GL_BLEND, s.blend);
	SetEnabled(GL_DEPTH_TEST, s.depthTest);
	SetEnabled(GL_SCISSOR_TEST, s.scissorTest);
	SetEnabled(GL_CULL_FACE, s.cullFace);
	SetEnabled(GL_STENCIL_TEST, s.stencilTest);
	gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
	gl.ClearColor(s.clearColor[0], s.clearColor[1], s.clearColor[2], s.clearColor[3]);
}

// Reads the GPU time of the frame a slot timed, once its submission has finished; otherwise drops it
void ReadGpuTime(PresentSlot &slot)
{
	const UINT64 fence = slot.timingFence;
	slot.timingFence = 0;
	if (!fence || !g_present.timestampBuffer || !Core_FenceDone(fence) || Core_DeviceRemoved())
		return;

	const size_t index = &slot - g_present.slots;
	const D3D12_RANGE range = {index * 2 * sizeof(UINT64), (index + 1) * 2 * sizeof(UINT64)};
	void *data = nullptr;
	if (Core_Failed(g_present.timestampBuffer->Map(0, &range, &data), "Map (timestamps)"))
		return;
	UINT64 times[2];
	memcpy(times, static_cast<const BYTE *>(data) + range.Begin, sizeof times);
	const D3D12_RANGE written = {0, 0};
	g_present.timestampBuffer->Unmap(0, &written);

	// No disjoint query in D3D12: drop what can't be right
	if (times[1] <= times[0])
		return;
	const double ms = (times[1] - times[0]) * 1000.0 / Core_TimestampFrequency();
	g_stats.gpuMs += ms;
	g_stats.gpuFrames++;
	if (ms > 250.0)
		LOG_ONCE("GPU frame exceeded 250 ms (%.1f ms)", ms);
	if (ms > g_stats.worstGpuMs)
		g_stats.worstGpuMs = ms;
}

// At the start of a frame's recording
void BeginGpuTime(PresentSlot &slot)
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list || !g_present.timestamps)
		return;
	const UINT index = (UINT)(&slot - g_present.slots);
	list->EndQuery(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index);
	slot.timing = true;
}

// Before the frame's copy; Present_Frame records which submission resolves it
bool EndGpuTime(PresentSlot &slot)
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!slot.timing || !list || !g_present.timestamps)
		return false;
	slot.timing = false;
	const UINT index = (UINT)(&slot - g_present.slots);
	list->EndQuery(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index + 1);
	list->ResolveQueryData(g_present.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index, 2,
		g_present.timestampBuffer.Get(), index * 2 * sizeof(UINT64));
	return true;
}

// Draws a slot's frame to ANGLE's window surface
void DrawToWindow(PresentSlot &slot, bool trace)
{
	int windowWidth = 0, windowHeight = 0;
	uwp_GetScreenSize(&windowWidth, &windowHeight);
	if (windowWidth <= 0 || windowHeight <= 0)
	{
		windowWidth = (int)g_present.width;
		windowHeight = (int)g_present.height;
	}

	// Letterbox it like DrawScreenFinalTexture
	const float origaspect = (float)g_present.width / g_present.height;
	const float newaspect = (float)windowWidth / windowHeight;
	float xoff = 1.0f, yoff = 1.0f;
	if (origaspect < newaspect)
		xoff = origaspect / newaspect;
	else if (origaspect > newaspect)
		yoff = newaspect / origaspect;

	SavedGlState saved;
	SaveGlState(saved);

	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Viewport(0, 0, windowWidth, windowHeight);
	gl.Disable(GL_BLEND);
	gl.Disable(GL_DEPTH_TEST);
	gl.Disable(GL_SCISSOR_TEST);
	gl.Disable(GL_CULL_FACE);
	gl.Disable(GL_STENCIL_TEST);
	gl.ColorMask(1, 1, 1, 1);
	gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	gl.Clear(GL_COLOR_BUFFER_BIT);

	gl.UseProgram(g_present.program);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, slot.glTexture);
	gl.BindBuffer(GL_ARRAY_BUFFER, g_present.quad);

	if (g_present.quadSize[0] != xoff || g_present.quadSize[1] != yoff)
	{
		// The top of the frame is its first row, which is where OpenGL ES's t is 0
		const GLfloat quad[] =
		{
			-xoff, yoff, 0.0f, 0.0f,
			-xoff, -yoff, 0.0f, 1.0f,
			xoff, yoff, 1.0f, 0.0f,
			xoff, -yoff, 1.0f, 1.0f,
		};
		gl.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
		g_present.quadSize[0] = xoff;
		g_present.quadSize[1] = yoff;
	}

	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), reinterpret_cast<void *>(0));
	gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), reinterpret_cast<void *>(2 * sizeof(GLfloat)));
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	RestoreGlState(saved);

	if (trace)
		Log("  drew slot %d to the %dx%d window (GL error 0x%X)", (int)(&slot - g_present.slots), windowWidth, windowHeight,
			gl.GetError());
}

// D: the shown slot's frame, from its READBACK buffer to its GL texture
void UploadReadback(PresentSlot &slot)
{
	const D3D12_RANGE range = {0, (SIZE_T)g_present.readbackBytes};
	void *data = nullptr;
	if (Core_Failed(slot.resource->Map(0, &range, &data), "Map (readback)"))
		return;

	const UINT rowBytes = g_present.width * 4;
	const UINT pitch = g_present.footprint.Footprint.RowPitch;
	const BYTE *source = static_cast<const BYTE *>(data) + g_present.footprint.Offset;
	if (pitch != rowBytes)
	{
		g_present.readback.resize((size_t)rowBytes * g_present.height);
		for (UINT y = 0; y < g_present.height; y++)
			memcpy(&g_present.readback[(size_t)y * rowBytes], source + (size_t)y * pitch, rowBytes);
		source = g_present.readback.data();
	}

	GLint previousTexture = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	gl.BindTexture(GL_TEXTURE_2D, slot.glTexture);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_present.width, g_present.height, GL_RGBA, GL_UNSIGNED_BYTE, source);
	gl.BindTexture(GL_TEXTURE_2D, previousTexture);
	const D3D12_RANGE written = {0, 0};
	slot.resource->Unmap(0, &written);
}

// After the frame's submission: the next frame is timed with the slot it'll be copied to, whose last timing has
// had a few frames to finish
void NextFrame()
{
	g_present.frame++;
	PresentSlot &next = g_present.slots[g_present.frame % kPresentSlots];
	ReadGpuTime(next);
	BeginGpuTime(next);
}

// Whatever happens to presenting, the frame's commands must still run, or retired objects are never freed
void SubmitWithoutPresenting()
{
	Core_Submit("present");
}

// Probes interop paths A, B and C, and settles for D if they all fail; or goes straight to D when gr_d3d12_present
// Readback asks. Call with ANGLE's context current, once the display, ANGLE's device and OpenGL ES are set up.
void SelectInteropPath()
{
	// gr_d3d12_present Readback: skip the A, B and C probes, to test path D where the others would be chosen
	if (cv_gld3d12present.string && cv_gld3d12present.value == 1)
	{
		g_present.path = PATH_D;
		Log("interop path D: forced by gr_d3d12_present");
		return;
	}

	const EGLint attributes[] =
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_BIND_TO_TEXTURE_RGBA, EGL_TRUE,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};
	EGLConfig configs[64];
	EGLint count = 0;
	if (!egl.ChooseConfig(g_present.display, attributes, configs, 64, &count) || count <= 0)
	{
		g_present.path = PATH_D;
		Log("interop path D: no RGBA8 pbuffer config (EGL error 0x%X)", EglError());
		return;
	}
	for (EGLint i = 0; i < count && egl.GetConfigAttrib; i++)
	{
		EGLint id = 0, depth = 0;
		egl.GetConfigAttrib(g_present.display, configs[i], EGL_CONFIG_ID, &id);
		egl.GetConfigAttrib(g_present.display, configs[i], EGL_DEPTH_SIZE, &depth);
		Log("pbuffer config %d: id %d, depth %d", (int)i, (int)id, (int)depth);
	}

	const precise_t start = I_GetPreciseTime();
	for (Path path : {PATH_A, PATH_B, PATH_C})
	{
		if (TryPath(path, configs, count))
		{
			g_present.path = path;
			Log("interop selection took %.1f ms", PreciseToMs(I_GetPreciseTime() - start));
			return;
		}
	}
	g_present.path = PATH_D;
	Log("interop path D: reading frames back, since A, B and C failed (%.1f ms)", PreciseToMs(I_GetPreciseTime() - start));
}

#ifdef _UWP

// ==========================================================================
//                                                         DIRECT SWAP CHAIN
// ==========================================================================

constexpr UINT kDirectFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr int kRestoreRetries = 3;

struct Direct
{
	bool failed = false; // couldn't take the window or present with it: interop for the rest of the run
	bool owned = false; // the swap chain has the window, so ANGLE has no surface on it and no context current
	SDL_GLContext context = nullptr; // ANGLE's, which is made current again when the window goes back
	ComPtr<IDXGISwapChain3> swapChain;
	ComPtr<ID3D12Resource> buffers[kDirectBuffers];
	HANDLE latency = nullptr; // signaled when the swap chain can take another frame
	UINT width = 0, height = 0;
};
Direct g_direct;

bool GetDirectBuffers()
{
	for (UINT i = 0; i < kDirectBuffers; i++)
	{
		wchar_t name[32];
		swprintf(name, sizeof name / sizeof *name, L"swap chain buffer %u", i);
		if (Core_Failed(g_direct.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_direct.buffers[i])), "GetBuffer (swap chain)"))
			return false;
		g_direct.buffers[i]->SetName(name);
	}
	return true;
}

// idle: the GPU has finished with the buffers, so they can go now rather than being retired
void ReleaseSwapChain(bool idle)
{
	for (ComPtr<ID3D12Resource> &buffer : g_direct.buffers)
	{
		if (idle)
			buffer.Reset();
		else
			Core_Retire(buffer);
	}
	if (idle)
		g_direct.swapChain.Reset();
	else
		Core_Retire(g_direct.swapChain);
	if (g_direct.latency)
	{
		CloseHandle(g_direct.latency);
		g_direct.latency = nullptr;
	}
	g_direct.width = g_direct.height = 0;
}

// ANGLE's window surface again, once the CoreWindow lets go of the swap chain, which DXGI can take a moment to do
bool RestoreAngleSurface()
{
	for (int attempt = 0; attempt <= kRestoreRetries; attempt++)
	{
		if (attempt)
			Sleep(50);
		if (SDL_WinRTRestoreGLSurface(window) == 0)
			return true;
		Log("ANGLE couldn't have the window back (attempt %d of %d): %s", attempt + 1, kRestoreRetries + 1, SDL_GetError());
	}
	return false;
}

// With no window ANGLE can't show anything, so there's nothing else to do
void RestoreAngleSurfaceOrDie()
{
	if (!RestoreAngleSurface())
		I_Error("Direct3D 12 couldn't give the game window back to ANGLE (%s).\nRestart the game. If it keeps happening, set gr_d3d12_present to CpuSync.",
			SDL_GetError());
}

enum TakeResult { TAKE_OK, TAKE_LATER, TAKE_FAILED };

// Takes the window from ANGLE, which has to let go of it first: a CoreWindow takes one swap chain at a time. ANGLE's
// D3D11 context keeps the window's back buffer bound, so before its surface goes, ANGLE draws to a pbuffer instead
// (a clear makes it bind the pbuffer) and flushes, without touching the rest of its state. Destroying the window
// surface then releases the swap chain, and once D3D11 has run its deferred destruction, the CoreWindow is free.
// TAKE_FAILED: ANGLE has the window and its context back, for the interop paths. TAKE_LATER: no context is current
// to take it from yet.
TakeResult TakeWindow(UINT width, UINT height)
{
	const precise_t start = I_GetPreciseTime();
	const bool trace = Present_Tracing();
	const SDL_GLContext context = SDL_GL_GetCurrentContext();
	const EGLDisplay display = egl.GetCurrentDisplay();
	if (!context || !display)
	{
		LOG_ONCE("direct: no ANGLE context is current, so the window can't be taken from it yet");
		return TAKE_LATER;
	}

	EGLSurface pbuffer = nullptr;
	auto fail = [&](const char *step, unsigned code)
	{
		Log("direct: couldn't take the window from ANGLE (%s failed: 0x%X); using the interop paths instead", step, code);
		ReleaseSwapChain(true); // nothing has used it
		if (pbuffer)
		{
			SDL_GL_MakeCurrent(window, nullptr); // so SDL doesn't think the context is current, and EGL lets go of the pbuffer
			egl.DestroySurface(display, pbuffer);
		}
		RestoreAngleSurfaceOrDie(); // nothing to do if the window surface was never destroyed
		SDL_GL_MakeCurrent(window, context);
		return TAKE_FAILED;
	};

	// What can fail without changing anything comes first
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	// SDL's WinRT driver puts the CoreWindow first in the union (info.winrt.window), which these headers may not declare
	IUnknown *coreWindow = nullptr;
	if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINRT)
	{
		Log("direct: no CoreWindow: %s", SDL_GetError());
		return fail("SDL_GetWindowWMInfo", 0);
	}
	memcpy(&coreWindow, &info.info, sizeof coreWindow);
	if (!coreWindow)
		return fail("SDL_GetWindowWMInfo (no CoreWindow)", 0);

	ComPtr<IDXGIFactory2> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (Core_Failed(hr, "CreateDXGIFactory2 (swap chain)"))
		return fail("CreateDXGIFactory2", (unsigned)hr);

	// A pbuffer with the config of ANGLE's context, so that the context can be made current on it
	EGLint configId = 0, surfaceType = 0, count = 0;
	EGLConfig config = nullptr;
	if (!egl.QueryContext(display, context, EGL_CONFIG_ID, &configId))
		return fail("eglQueryContext", EglError());
	const EGLint wanted[] = {EGL_CONFIG_ID, configId, EGL_NONE};
	if (!egl.ChooseConfig(display, wanted, &config, 1, &count) || count < 1)
		return fail("eglChooseConfig", EglError());
	if (!egl.GetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surfaceType) || !(surfaceType & EGL_PBUFFER_BIT))
	{
		Log("direct: ANGLE's context config %d has no pbuffers (surface type 0x%X)", (int)configId, (unsigned)surfaceType);
		return fail("EGL_PBUFFER_BIT", 0);
	}
	const EGLint pbufferSize[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
	pbuffer = egl.CreatePbufferSurface(display, config, pbufferSize);
	if (!pbuffer)
		return fail("eglCreatePbufferSurface", EglError());

	// From here on, the window is ANGLE's no longer. Clear the pbuffer so that ANGLE binds it, in place of the window's
	// back buffer, and flush; the GL state the clear needs is put back as it was.
	if (!egl.MakeCurrent(display, pbuffer, pbuffer, context))
		return fail("eglMakeCurrent (pbuffer)", EglError());
	{
		SavedGlState saved;
		SaveGlState(saved);
		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
		gl.Disable(GL_SCISSOR_TEST);
		gl.ColorMask(1, 1, 1, 1);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		gl.Flush();
		RestoreGlState(saved);
	}
	if (trace)
		Log("  direct: ANGLE draws to a pbuffer (config %d, GL error 0x%X)", (int)configId, gl.GetError());

	// Makes nothing current (not the pbuffer either) and destroys the window surface and its swap chain
	if (SDL_WinRTReleaseGLSurface(window) != 0)
	{
		Log("direct: %s", SDL_GetError());
		return fail("SDL_WinRTReleaseGLSurface", 0);
	}
	egl.DestroySurface(display, pbuffer);
	pbuffer = nullptr;
	// D3D11 destroys what's no longer bound when it runs the commands before, which Flush makes it do
	g_present.angleContext->Flush();

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = kFormat;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kDirectBuffers;
	// Letterboxed like DrawToWindow when the screen's aspect ratio isn't the window's
	desc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = kDirectFlags;

	// The CoreWindow can still be letting go of ANGLE's swap chain
	ComPtr<IDXGISwapChain1> swapChain;
	for (int attempt = 0; attempt <= kRestoreRetries; attempt++)
	{
		if (attempt)
			Sleep(50);
		hr = factory->CreateSwapChainForCoreWindow(Core_Queue(), coreWindow, &desc, nullptr, &swapChain);
		if (SUCCEEDED(hr))
			break;
		Log("direct: CreateSwapChainForCoreWindow failed (0x%08X, attempt %d of %d)", (unsigned)hr, attempt + 1, kRestoreRetries + 1);
	}
	if (Core_Failed(hr, "CreateSwapChainForCoreWindow"))
		return fail("CreateSwapChainForCoreWindow", (unsigned)hr);
	hr = swapChain.As(&g_direct.swapChain);
	if (Core_Failed(hr, "QueryInterface IDXGISwapChain3"))
		return fail("QueryInterface IDXGISwapChain3", (unsigned)hr);
	// The bars' color
	const DXGI_RGBA black = {0.0f, 0.0f, 0.0f, 1.0f};
	hr = g_direct.swapChain->SetBackgroundColor(&black);
	if (FAILED(hr))
		Log("direct: SetBackgroundColor failed (0x%08X)", (unsigned)hr);
	hr = g_direct.swapChain->SetMaximumFrameLatency(kDirectLatency);
	if (Core_Failed(hr, "SetMaximumFrameLatency"))
		return fail("SetMaximumFrameLatency", (unsigned)hr);
	g_direct.latency = g_direct.swapChain->GetFrameLatencyWaitableObject();
	if (!g_direct.latency)
		return fail("GetFrameLatencyWaitableObject", 0);
	if (!GetDirectBuffers())
		return fail("GetBuffer", 0);

	g_direct.owned = true;
	g_direct.context = context;
	g_direct.width = width;
	g_direct.height = height;
	CreateTimestamps();
	Log("took the window from ANGLE: %ux%u swap chain, %u buffers, frame latency %u (%.1f ms)", width, height, kDirectBuffers,
		kDirectLatency, PreciseToMs(I_GetPreciseTime() - start));
	return TAKE_OK;
}

// Gives the window back to ANGLE, with no context current
void GiveBackWindow()
{
	const precise_t start = I_GetPreciseTime();
	ReleaseSwapChain(Core_WaitIdle(2000, "giving the window back to ANGLE"));
	g_direct.owned = false;
	RestoreAngleSurfaceOrDie();
	Log("gave the window back to ANGLE (%.1f ms)", PreciseToMs(I_GetPreciseTime() - start));
}

// The window is ANGLE's again, with its context current: probe the interop paths and use one for the rest of the run
void FallBackToInterop()
{
	g_direct.failed = true;
	g_present.path = PATH_NONE;
	SelectInteropPath();
}

// The buffers can't be resized while the GPU or anything else holds them
bool ResizeSwapChain(UINT width, UINT height)
{
	const UINT oldWidth = g_direct.width, oldHeight = g_direct.height;
	if (!Core_WaitIdle(2000, "resizing the swap chain"))
		return false;
	for (ComPtr<ID3D12Resource> &buffer : g_direct.buffers)
		buffer.Reset();
	if (Core_Failed(g_direct.swapChain->ResizeBuffers(kDirectBuffers, width, height, kFormat, kDirectFlags), "ResizeBuffers (swap chain)")
		|| !GetDirectBuffers())
		return false;
	g_direct.width = width;
	g_direct.height = height;
	Log("resized the swap chain from %ux%u to %ux%u", oldWidth, oldHeight, width, height);
	return true;
}

// Copies the frame to the swap chain's next buffer and presents it. false: direct presenting has failed, ANGLE has the
// window back and an interop path is selected, which has to present this frame.
bool PresentDirect(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync)
{
	const bool trace = Present_Tracing();

	if (!g_direct.owned)
	{
		const TakeResult taken = TakeWindow(width, height);
		if (taken == TAKE_LATER)
		{
			SubmitWithoutPresenting();
			return true;
		}
		if (taken == TAKE_FAILED)
		{
			FallBackToInterop();
			return false;
		}
	}
	else if (g_direct.width != width || g_direct.height != height)
	{
		if (!ResizeSwapChain(width, height))
		{
			Log("direct: couldn't resize the swap chain to %ux%u; using the interop paths instead", width, height);
			GiveBackWindow();
			SDL_GL_MakeCurrent(window, g_direct.context);
			FallBackToInterop();
			return false;
		}
	}

	// The swap chain takes another frame once it has presented one of the last kDirectLatency
	precise_t t = I_GetPreciseTime();
	const DWORD waited = WaitForSingleObjectEx(g_direct.latency, 500, FALSE);
	precise_t now = I_GetPreciseTime();
	const double waitMs = PreciseToMs(now - t);
	g_stats.latencyWaitMs += waitMs;
	g_stats.worstLatencyWaitMs = std::max(g_stats.worstLatencyWaitMs, waitMs);
	if (waited != WAIT_OBJECT_0)
	{
		g_stats.latencyTimeouts++;
		LOG_ONCE("gave up waiting for the swap chain's frame latency object after %.0f ms (%s)", waitMs,
			waited == WAIT_TIMEOUT ? "timed out" : "wait failed");
	}

	const UINT index = g_direct.swapChain->GetCurrentBackBufferIndex();
	ID3D12Resource *back = g_direct.buffers[index].Get();
	PresentSlot &slot = g_present.slots[g_present.frame % kPresentSlots]; // only times the frame
	ID3D12GraphicsCommandList *list = Core_List();
	bool timed = false;
	t = now;
	if (list)
	{
		timed = EndGpuTime(slot);
		Core_Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Core_Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		Core_FlushBarriers();
		list->CopyResource(back, source);
		Core_Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
		Core_Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}
	g_traceCopied = g_traceShown = (INT32)index;
	g_traceCopyFrame = g_traceShowFrame = UWPTraceFrameId();
	const UINT64 fence = list ? Core_Submit("present") : 0;
	if (!list)
		SubmitWithoutPresenting(); // reopens the list
	if (timed && fence)
		slot.timingFence = fence;
	now = I_GetPreciseTime();
	g_stats.copyTime += now - t;
	if (!fence)
	{
		NextFrame(); // nothing was submitted, so there is nothing to show
		return true;
	}

	// Queued behind the copy on the same queue
	t = now;
	const HRESULT hr = g_direct.swapChain->Present(vsync ? 1 : 0, 0);
	now = I_GetPreciseTime();
	const double presentMs = PreciseToMs(now - t);
	g_stats.presentTime += now - t;
	g_stats.worstPresentMs = std::max(g_stats.worstPresentMs, presentMs);
	// A removed or reset device ends the run in D3D12_FinishUpdate
	if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)
	{
		g_stats.presentFailures++;
		LOG_ONCE("IDXGISwapChain::Present failed (0x%08X)", (unsigned)hr);
	}
	if (trace)
		Log("  copied the screen to back buffer %u, submitted as fence %llu; waited %.2f ms for the frame latency object%s; presented with sync interval %d (0x%08X, %.2f ms)",
			index, (unsigned long long)fence, waitMs, waited == WAIT_OBJECT_0 ? "" : " (gave up)", vsync ? 1 : 0, (unsigned)hr, presentMs);

	NextFrame();
	return true;
}

#endif // _UWP

} // namespace

ID3D11Device *Present_AngleDevice()
{
	InitAngleDevice();
	return g_present.angleDevice.Get();
}

void Present_SelectPath()
{
	if (g_present.probed)
		return;

	if (!LoadEgl() || !egl.ChooseConfig || !egl.CreatePbufferFromClientBuffer || !egl.BindTexImage || !egl.ReleaseTexImage
		|| !egl.DestroySurface)
	{
		g_present.probed = true;
		g_present.path = PATH_D;
		Log("interop path D: EGL functions are missing");
		return;
	}

	g_present.display = egl.GetCurrentDisplay();
	if (!g_present.display)
	{
		// Try again when presenting
		LOG_ONCE("no current EGL display yet");
		return;
	}
	g_present.probed = true;

	const char *extensions = egl.QueryString ? egl.QueryString(g_present.display, EGL_EXTENSIONS) : nullptr;
	Log("EGL vendor %s, version %s", egl.QueryString ? egl.QueryString(g_present.display, EGL_VENDOR) : "?",
		egl.QueryString ? egl.QueryString(g_present.display, EGL_VERSION) : "?");
	Log("EGL extensions: %s", extensions ? extensions : "(none)");

	InitAngleDevice();
	if (!LoadGles() || !g_present.angleDevice || !g_present.angleContext || !Core_Device())
	{
		g_present.path = PATH_D;
		Log("interop path D: %s", !gl.loaded ? "OpenGL ES functions are missing" : "no ANGLE device to share with");
		return;
	}

#ifdef _UWP
	// By default, frames are presented with Direct3D 12's own swap chain, taking the window from ANGLE with the first
	// one. The interop paths are for when that fails, so they're only probed then.
	if (cv_gld3d12present.value == 0)
	{
		if (egl.MakeCurrent && egl.QueryContext && egl.CreatePbufferSurface)
		{
			g_present.path = PATH_DIRECT;
			Log("present: direct swap chain (%u buffers, frame latency %u), taking the window from ANGLE with the first frame",
				kDirectBuffers, kDirectLatency);
			return;
		}
		Log("direct swap chain unavailable: EGL functions are missing");
	}
	else if (cv_gld3d12present.value != 0)
		Log("direct swap chain not used (gr_d3d12_present %s)", cv_gld3d12present.value == 1 ? "Readback" : "CpuSync");
#endif

	SelectInteropPath();
}

void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync)
{
	g_traceCopied = g_traceShown = -1;
	g_traceCopyFrame = g_traceShowFrame = 0;
	const bool trace = Present_Tracing();
	precise_t t = I_GetPreciseTime();
	const precise_t presentStart = t;

	if (g_present.failed || !source)
	{
		SubmitWithoutPresenting();
		return;
	}

	if (!g_present.probed)
	{
		Present_SelectPath();
		if (!g_present.probed)
		{
			SubmitWithoutPresenting(); // no EGL display yet
			return;
		}
	}

#ifdef _UWP
	// No OpenGL ES here: ANGLE has no context current while the swap chain has the window. If direct presenting just
	// failed, ANGLE has the window and the interop path selected presents this frame below.
	if (g_present.path == PATH_DIRECT && PresentDirect(source, sourceState, width, height, vsync))
		return;
#endif

	if (!LoadGles() || (!g_present.program && !CreatePresentProgram()))
	{
		Log("can't present frames, so the screen will stay black");
		g_present.failed = true;
		SubmitWithoutPresenting();
		return;
	}

	if (g_present.width != width || g_present.height != height)
	{
		if (!CreateSlots(width, height))
		{
			if (g_present.path == PATH_D)
			{
				DestroySlots(false);
				g_present.failed = true;
				Log("can't present frames, so the screen will stay black");
				SubmitWithoutPresenting();
				return;
			}
			Log("falling back to reading frames back (interop path D)");
			DestroySlots(false);
			g_present.path = PATH_D;
			if (!CreateSlots(width, height))
			{
				DestroySlots(false);
				g_present.failed = true;
				Log("can't present frames, so the screen will stay black");
				SubmitWithoutPresenting();
				return;
			}
		}
	}

	const int index = (int)(g_present.frame % kPresentSlots);
	PresentSlot &slot = g_present.slots[index];
	const bool readback = g_present.path == PATH_D;
	ID3D12GraphicsCommandList *list = Core_List();
	bool timed = false;
	if (list)
	{
		timed = EndGpuTime(slot);
		// The shared texture is COMMON whenever ANGLE might use it
		Core_Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (!readback)
			Core_Transition(slot.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		Core_FlushBarriers();
		if (readback)
		{
			D3D12_TEXTURE_COPY_LOCATION destination = {};
			destination.pResource = slot.resource.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			destination.PlacedFootprint = g_present.footprint;
			D3D12_TEXTURE_COPY_LOCATION from = {};
			from.pResource = source;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = 0;
			list->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);
		}
		else
			list->CopyResource(slot.resource.Get(), source);
		Core_Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
		if (!readback)
			Core_Transition(slot.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	}
	g_traceCopied = index;
	g_traceCopyFrame = g_traceSlotFrame[index] = UWPTraceFrameId();

	// Before the submission with the copy, not before recording it: mid-frame submissions don't have it. Under the
	// shared fences the queue waits for ANGLE's read; ANGLE's Signal for it was flushed when the slot was drawn.
	const precise_t readStart = I_GetPreciseTime();
	const bool readPending = slot.readPending;
	const UINT64 readValue = slot.readValue;
	bool queueWait = false;
	if (slot.readPending && slot.readValue && list && g_present.syncMode == SYNC_SHARED_FENCES && !Core_RemovalSeen())
	{
		if (g_present.readFence->GetCompletedValue() >= slot.readValue)
		{
			slot.readPending = false; // already read
			slot.readValue = 0;
		}
		else
		{
			const HRESULT hr = Core_Queue()->Wait(g_present.readFence.Get(), slot.readValue);
			if (SUCCEEDED(hr))
			{
				g_stats.gpuReadWaits++;
				queueWait = true;
				slot.readPending = false;
				slot.readValue = 0;
			}
			else
				FallBackToCpuSync("ID3D12CommandQueue::Wait (present read)", hr); // and wait below
		}
	}
	WaitForAngleRead(slot);
	const double readWait = PreciseToMs(I_GetPreciseTime() - readStart);
	slot.copyFence = list ? Core_Submit("present") : 0;
	slot.copyShared = false;
	if (!list)
		SubmitWithoutPresenting(); // reopens the list
	if (slot.copyFence && g_present.syncMode == SYNC_SHARED_FENCES)
	{
		// Same queue, so after the copy. The main fence's values are monotonic, so they serve the shared one too.
		const HRESULT hr = Core_Queue()->Signal(g_present.copyFence.Get(), slot.copyFence);
		if (SUCCEEDED(hr))
			slot.copyShared = true;
		else
			FallBackToCpuSync("ID3D12CommandQueue::Signal (present copy)", hr);
	}
	if (timed && slot.copyFence)
		slot.timingFence = slot.copyFence;
	if (trace)
	{
		char read[96];
		if (queueWait)
			snprintf(read, sizeof read, "enqueued queue wait for ANGLE's read of slot %d (value %llu)", index,
				(unsigned long long)readValue);
		else
			snprintf(read, sizeof read, "%s", readPending ? (readWait >= 0.01 ? "waited for ANGLE's last read of it"
				: "ANGLE's last read of it was done") : "no ANGLE read pending");
		Log("  copied the screen to slot %d; %s; submitted as fence %llu%s", index, read,
			(unsigned long long)slot.copyFence, slot.copyShared ? " (shared fence signaled)" : "");
	}

	precise_t now = I_GetPreciseTime();
	g_stats.copyTime += now - t;
	t = now;

	// Wipes show the current copy: presenting can stop after their last frame.
	// The next normal frame will show that wipe frame again once, which is acceptable.
	PresentSlot &shown = (!readback && g_present.frame > 0 && !g_presentImmediately)
		? g_present.slots[(g_present.frame - 1) % kPresentSlots] : slot;
	const int shownIndex = (int)(&shown - g_present.slots);
	if (trace && g_presentImmediately)
		Log("  showing the current wipe frame immediately");
	g_presentImmediately = false;

	// Under the shared fences ANGLE waits for the copy on the GPU: its Signal was enqueued when the slot was
	// submitted, this frame or an earlier one. Never on a removed device, whose fence may never get there.
	g_stats.copyWaits++;
	bool cpuWait = true;
	if (g_present.syncMode == SYNC_SHARED_FENCES && shown.copyShared)
	{
		const UINT64 done = Core_CompletedFence(); // UINT64_MAX once removed, which Core_RemovalSeen then reports
		if (!shown.copyFence || Core_RemovalSeen() || (done < shown.copyFence && Core_DeviceRemoved()))
		{
			g_stats.copyWaitFailures++;
			if (trace)
				Log("  slot %d's copy (fence %llu) can't be waited for; not showing it", shownIndex,
					(unsigned long long)shown.copyFence);
			NextFrame();
			return;
		}
		if (done >= shown.copyFence)
		{
			cpuWait = false;
			if (trace)
				Log("  slot %d's copy (fence %llu) was done", shownIndex, (unsigned long long)shown.copyFence);
		}
		else
		{
			const HRESULT hr = g_present.angleContext4->Wait(g_present.angleCopyFence.Get(), shown.copyFence);
			if (SUCCEEDED(hr))
			{
				cpuWait = false;
				g_stats.gpuCopyWaits++;
				if (trace)
					Log("  enqueued ANGLE wait for slot %d's copy (fence %llu; GPU at %llu)", shownIndex,
						(unsigned long long)shown.copyFence, (unsigned long long)Core_CompletedFence());
			}
			else
				FallBackToCpuSync("ID3D11DeviceContext4::Wait (present copy)", hr); // and wait below
		}
	}
	if (cpuWait)
	{
		double waited = 0.0;
		if (!shown.copyFence || !Core_Wait(shown.copyFence, 500, "a frame's copy", &waited))
		{
			g_stats.copyWaitFailures++;
			if (trace)
				Log("  slot %d's copy (fence %llu) didn't finish; not showing it", shownIndex,
					(unsigned long long)shown.copyFence);
			NextFrame();
			return;
		}
		g_stats.copyWaitMs += waited;
		g_stats.worstCopyWaitMs = std::max(g_stats.worstCopyWaitMs, waited);
		if (trace)
			Log("  waited %.2f ms for slot %d's copy (fence %llu; GPU at %llu)", waited, shownIndex,
				(unsigned long long)shown.copyFence, (unsigned long long)Core_CompletedFence());
	}

	if (readback)
		UploadReadback(shown);
	else if (g_present.path == PATH_C)
		g_present.angleContext->CopyResource(shown.legacy.Get(), shown.texture.Get()); // ordered before ANGLE's draw

	DrawToWindow(shown, trace);
	const bool fastFrame = g_present.hasLastPresent
		&& PreciseToMs(presentStart - g_present.lastPresent) < 8.0;
	FinishAngleRead(shown, fastFrame);
	g_present.lastPresent = presentStart;
	g_present.hasLastPresent = true;
	g_traceShown = shownIndex;
	g_traceShowFrame = g_traceSlotFrame[g_traceShown];

	now = I_GetPreciseTime();
	g_stats.glTime += now - t;
	t = now;

	SDL_GL_SwapWindow(window);

	now = I_GetPreciseTime();
	g_stats.swapTime += now - t;
	if (trace)
		Log("  swapped (%.2f ms)", PreciseToMs(now - t));

	NextFrame();
}

bool Present_OwnsWindow()
{
#ifdef _UWP
	return g_direct.owned;
#else
	return false;
#endif
}

void Present_ReleaseWindow()
{
#ifdef _UWP
	if (g_direct.owned)
		GiveBackWindow();
#endif
}

void Present_Immediately()
{
	if (g_present.path != PATH_DIRECT) // the swap chain shows the current frame
		g_presentImmediately = true;
}

void Present_Shutdown(bool idle)
{
	DestroySlots(idle); // waits for ANGLE's reads on the CPU
#ifdef _UWP
	// The window stays without ANGLE's surface: SDL_DestroyWindow doesn't need one
	ReleaseSwapChain(idle);
	g_direct.owned = false;
#endif
	if (idle)
	{
		// The queue has reached every copy Signal, so ANGLE's waits for them can finish: none may be left on its
		// context when the fences go. On a busy or removed device, no more GPU work.
		if (g_present.copyFence && gl.loaded)
			gl.Finish();
		g_present.timestamps.Reset();
		g_present.timestampBuffer.Reset();
		g_present.copyFence.Reset();
		g_present.readFence.Reset();
	}
	else
	{
		Core_Retire(g_present.timestamps);
		Core_Retire(g_present.timestampBuffer);
		Core_Retire(g_present.copyFence);
		Core_Retire(g_present.readFence);
	}
	g_present.angleCopyFence.Reset();
	g_present.angleReadFence.Reset();
	g_present.angleContext4.Reset();
	g_present.syncMode = SYNC_CPU_FENCE;
	g_present.fencesTried = g_present.sharedFailed = false;
	// GL objects go with ANGLE's context, as with the Direct3D 11 driver
	g_present.angleContext.Reset();
	g_present.angleDevice1.Reset();
	g_present.angleDevice.Reset();
	g_present.deviceProbed = false;
	g_present.failed = true; // nothing presents after this
}

void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe)
{
	*copied = g_traceCopied;
	*shown = g_traceShown;
	*copyframe = g_traceCopyFrame;
	*showframe = g_traceShowFrame;
}

void Present_LogPerf(double frames)
{
	const PresentStats &s = g_stats;
	const double n = frames > 0 ? frames : 1;
	if (g_present.path == PATH_DIRECT)
	{
		I_OutputMsg("UWP perf:   D3D12: present: direct swap chain (%u buffers, latency %u); per frame copy %.2f ms, latency wait %.2f ms (worst %.2f, %d timeouts), present %.2f ms (worst %.2f); %d present failures\n",
			kDirectBuffers, kDirectLatency, PreciseToMs(s.copyTime) / n, s.latencyWaitMs / n, s.worstLatencyWaitMs,
			s.latencyTimeouts, PreciseToMs(s.presentTime) / n, s.worstPresentMs, s.presentFailures);
	}
	else
	{
		I_OutputMsg("UWP perf:   D3D12: present: interop path %c, sync %s; per frame copy %.2f ms, copy-fence wait %.2f ms (worst %.2f, %d of %d not shown), GL %.2f, swap %.2f ms; ANGLE read waits %d, %.2f ms total; %d timeouts; fallback finishes %d; GPU waits: copy %d, read %d\n",
			PathName(g_present.path), g_present.syncMode == SYNC_SHARED_FENCES ? "shared fences"
				: g_present.angleQueries ? "cpu fence + ANGLE queries" : "cpu fence + glFinish",
			PreciseToMs(s.copyTime) / n, s.copyWaitMs / n, s.worstCopyWaitMs, s.copyWaitFailures, s.copyWaits,
			PreciseToMs(s.glTime) / n, PreciseToMs(s.swapTime) / n,
			s.angleReadWaits, PreciseToMs(s.angleReadWaitTime), s.angleReadTimeouts, s.fallbackFinishes, s.gpuCopyWaits, s.gpuReadWaits);
	}
	I_OutputMsg("UWP perf:   D3D12: GPU %.2f ms per frame (worst %.2f, %d timed)\n",
		s.gpuFrames ? s.gpuMs / s.gpuFrames : 0.0, s.worstGpuMs, s.gpuFrames);
	g_stats = PresentStats();
}

bool Present_Tracing()
{
	return g_tracedFrames < kTraceFrames;
}

void Present_FrameDone()
{
	if (g_tracedFrames < kTraceFrames)
		g_tracedFrames++;
}

UINT64 Present_SlotBytes()
{
#ifdef _UWP
	if (g_direct.swapChain)
		return (UINT64)kDirectBuffers * g_direct.width * g_direct.height * 4;
#endif
	UINT64 bytes = 0;
	for (const PresentSlot &slot : g_present.slots)
		if (slot.resource)
			bytes += g_present.path == PATH_D ? g_present.readbackBytes : (UINT64)g_present.width * g_present.height * 4;
	return bytes;
}

} // namespace rr_d3d12
