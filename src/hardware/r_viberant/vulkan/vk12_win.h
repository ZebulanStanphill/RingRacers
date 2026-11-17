// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_win.h
/// \brief What the VibeRant driver uses of Windows, for platforms without it
///
/// The driver is written against Direct3D 12 and a little of Win32. Where there is no Windows SDK, the Direct3D 12
/// types and interfaces come from Microsoft's DirectX-Headers (the same declarations), and this header stands in for
/// the rest: a handful of Win32 calls, the one DXGI interface the driver asks for video memory, and D3DCompile, which
/// here compiles HLSL to SPIR-V (vk12_spirv.cpp).

#ifndef RR_VK12_WIN_H
#define RR_VK12_WIN_H

#ifdef _WIN32
#error "Windows builds use the Windows SDK's headers"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <ctime>

#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/d3dcommon.h>
#include <wsl/wrladapter.h>
#include <dxguids/dxguids.h>

// The one interface ID the driver's blobs need that DirectX-Headers leaves out
#ifndef RR_VK12_BLOB_IID
#define RR_VK12_BLOB_IID
WINADAPTER_IID(ID3D10Blob, 0x8ba5fb08, 0x5195, 0x40e2, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02);
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#ifndef EVENT_ALL_ACCESS
#define EVENT_ALL_ACCESS 0x1F0003
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif
#ifndef DXGI_ERROR_DEVICE_HUNG
#define DXGI_ERROR_DEVICE_HUNG ((HRESULT)0x887A0006)
#endif
#ifndef DXGI_ERROR_DEVICE_REMOVED
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005)
#endif
#ifndef DXGI_ERROR_DEVICE_RESET
#define DXGI_ERROR_DEVICE_RESET ((HRESULT)0x887A0007)
#endif
#ifndef DXGI_ERROR_DRIVER_INTERNAL_ERROR
#define DXGI_ERROR_DRIVER_INTERNAL_ERROR ((HRESULT)0x887A0020)
#endif
#ifndef DXGI_ERROR_INVALID_CALL
#define DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001)
#endif
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#endif

// The high-resolution counter, in nanoseconds
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	count->QuadPart = (LONGLONG)now.tv_sec * 1000000000ll + now.tv_nsec;
	return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000000ll;
	return TRUE;
}

inline void Sleep(DWORD ms)
{
	timespec time = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000l};
	nanosleep(&time, nullptr);
}

inline DWORD GetLastError()
{
	return 0;
}

// The driver waits for the GPU with rr_vk12::WaitFence on this backend; its event is never signaled or waited on
inline HANDLE CreateEventExW(void *, const wchar_t *, DWORD, DWORD)
{
	static int event;
	return &event;
}

inline BOOL CloseHandle(HANDLE)
{
	return TRUE;
}

inline DWORD WaitForSingleObjectEx(HANDLE, DWORD ms, BOOL)
{
	Sleep(ms < 1 ? 1 : ms);
	return 0x102; // WAIT_TIMEOUT
}

// Only as the driver calls it: a null-terminated wide string to UTF-8. Returns the bytes written, with the terminator.
inline int WideCharToMultiByte(unsigned, DWORD, const wchar_t *wide, int, char *out, int size, const char *, BOOL *)
{
	int used = 0;
	if (!wide || !out || size <= 0)
		return 0;
	for (; *wide; wide++)
	{
		const uint32_t c = (uint32_t)*wide;
		char bytes[4];
		int count;
		if (c < 0x80) { bytes[0] = (char)c; count = 1; }
		else if (c < 0x800) { bytes[0] = (char)(0xC0 | (c >> 6)); bytes[1] = (char)(0x80 | (c & 0x3F)); count = 2; }
		else if (c < 0x10000)
		{
			bytes[0] = (char)(0xE0 | (c >> 12)); bytes[1] = (char)(0x80 | ((c >> 6) & 0x3F));
			bytes[2] = (char)(0x80 | (c & 0x3F)); count = 3;
		}
		else
		{
			bytes[0] = (char)(0xF0 | (c >> 18)); bytes[1] = (char)(0x80 | ((c >> 12) & 0x3F));
			bytes[2] = (char)(0x80 | ((c >> 6) & 0x3F)); bytes[3] = (char)(0x80 | (c & 0x3F)); count = 4;
		}
		if (used + count >= size)
			return 0;
		memcpy(out + used, bytes, (size_t)count);
		used += count;
	}
	out[used++] = '\0';
	return used;
}

// DXGI's video memory query, which vk12 answers from VK_EXT_memory_budget
typedef enum DXGI_MEMORY_SEGMENT_GROUP
{
	DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0,
	DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} DXGI_MEMORY_SEGMENT_GROUP;

typedef struct DXGI_QUERY_VIDEO_MEMORY_INFO
{
	UINT64 Budget;
	UINT64 CurrentUsage;
	UINT64 AvailableForReservation;
	UINT64 CurrentReservation;
} DXGI_QUERY_VIDEO_MEMORY_INFO;

struct IDXGIAdapter3 : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP group,
		DXGI_QUERY_VIDEO_MEMORY_INFO *info) = 0;
};

// HLSL to SPIR-V with glslang's HLSL front end, for the shaders the driver compiles at run time (add-on shaders and
// screen effects, which reach it as HLSL). Only the arguments the driver passes are honored: the source, the entry
// point and the profile's stage ("vs_..." or "ps_..."). code receives the SPIR-V, errors the compiler's messages.
HRESULT D3DCompile(const void *source, SIZE_T size, const char *name, const void *defines, void *include,
	const char *entry, const char *profile, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);

#ifndef D3DCOMPILE_OPTIMIZATION_LEVEL3
#define D3DCOMPILE_ENABLE_STRICTNESS (1 << 11)
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DCOMPILE_WARNINGS_ARE_ERRORS (1 << 18)
#define D3DCOMPILE_AVOID_FLOW_CONTROL (1 << 9)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1 << 4)
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (1 << 3)
#define D3DCOMPILE_SKIP_OPTIMIZATION (1 << 2)
#define D3DCOMPILE_DEBUG (1 << 0)
#define D3DCOMPILE_ALL_RESOURCES_BOUND (1 << 21)
#endif

#endif
