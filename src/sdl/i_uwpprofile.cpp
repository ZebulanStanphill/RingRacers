// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_uwpprofile.cpp
/// \brief [UWP]: Sampling profiler for the main thread, what CPUs it runs on, and a log of crashes, to find what's
///        slow or broken on Xbox
///
/// A thread pauses the main thread about 50 times a second and records the address it was running, the calls
/// that led there (found with the unwind data of each module), and what it was doing (uwpprofphase_t). Every 20
/// seconds, it writes how many samples it took at each address and with each call stack while a level was being
/// played to E:\ringracers\uwp-profile.txt. Addresses are written as offsets into their module, which for the game
/// itself (ringracers-uwp.exe) ringracers-uwp.map from the same build maps to functions. Each sample pauses the main
/// thread for about 0.25 ms on Xbox, so sampling more often slows the game noticeably.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../i_system.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

volatile INT32 i_uwpprofphase = UWPPROF_OTHER;
volatile INT32 i_uwpprofinlevel = false;

namespace
{

constexpr const char* kProfilePath = "E:\\ringracers\\uwp-profile.txt";
constexpr auto kWriteInterval = std::chrono::seconds(20);
constexpr LONGLONG kSampleInterval = -200000; // 20 ms, in 100 ns units (negative for relative to now)
constexpr const char* kPhaseNames[NUMUWPPROFPHASES] = {"other", "game logic", "3D view", "2D", "display"};
constexpr int kMaxFrames = 12; // of each call stack, innermost first
constexpr size_t kMaxModules = 64; // to unwind through
constexpr int kMaxProcessors = 64;

// Looked up at runtime, so a missing function stops just the feature that needs it instead of the game
using SuspendResumeThreadFn = DWORD (WINAPI *)(HANDLE);
using GetThreadContextFn = BOOL (WINAPI *)(HANDLE, LPCONTEXT);
// RtlVirtualUnwind, with pointers to structures whose definitions UWP apps may not get as void*
using VirtualUnwindFn = void* (WINAPI *)(DWORD, DWORD64, DWORD64, const void*, CONTEXT*, void**, DWORD64*, void*);
using GetStackLimitsFn = void (WINAPI *)(PULONG_PTR, PULONG_PTR);
using GetProcessorNumberFn = DWORD (WINAPI *)(void);
using GetSystemCpuSetInformationFn = BOOL (WINAPI *)(void*, ULONG, PULONG, HANDLE, ULONG);
using GetCpuSetsFn = BOOL (WINAPI *)(HANDLE, PULONG, ULONG, PULONG);

void* FindKernelFunction(const char* name)
{
	for (const wchar_t* module : {L"kernelbase.dll", L"kernel32.dll", L"ntdll.dll"})
	{
		if (HMODULE handle = GetModuleHandleW(module))
		{
			if (FARPROC proc = GetProcAddress(handle, name))
			{
				return reinterpret_cast<void*>(proc);
			}
		}
	}
	return nullptr;
}

// An entry of a module's exception directory (RUNTIME_FUNCTION)
struct RuntimeFunction
{
	DWORD begin;
	DWORD end;
	DWORD unwind;
};

struct UnwindModule
{
	uintptr_t base;
	uintptr_t end;
	const RuntimeFunction* functions; // sorted by begin
	size_t count;
};

// Fixed size, so that looking up modules while the main thread is paused never allocates
struct UnwindModules
{
	UnwindModule modules[kMaxModules];
	size_t count = 0;

	const UnwindModule* find(uintptr_t address) const
	{
		for (size_t i = 0; i < count; i++)
		{
			if (address >= modules[i].base && address < modules[i].end)
			{
				return &modules[i];
			}
		}
		return nullptr;
	}

	// Adds the module mapped at base, from its headers
	void add(uintptr_t base)
	{
		if (count == kMaxModules || find(base))
		{
			return;
		}

		const auto image = reinterpret_cast<const uint8_t*>(base);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return;
		}
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
			nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
		{
			return;
		}

		const IMAGE_DATA_DIRECTORY& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		UnwindModule& module = modules[count++];
		module.base = base;
		module.end = base + nt->OptionalHeader.SizeOfImage;
		module.functions = reinterpret_cast<const RuntimeFunction*>(image + exceptions.VirtualAddress);
		module.count = exceptions.VirtualAddress ? exceptions.Size / sizeof (RuntimeFunction) : 0;
	}
};

const RuntimeFunction* FindFunction(const UnwindModule& module, DWORD rva)
{
	size_t low = 0;
	size_t high = module.count;

	while (low < high)
	{
		const size_t mid = (low + high) / 2;
		const RuntimeFunction& function = module.functions[mid];

		if (rva < function.begin)
		{
			high = mid;
		}
		else if (rva >= function.end)
		{
			low = mid + 1;
		}
		else
		{
			return &function;
		}
	}
	return nullptr;
}

struct Sampler
{
	HANDLE thread;
	SuspendResumeThreadFn suspend;
	SuspendResumeThreadFn resume;
	GetThreadContextFn getcontext;
	VirtualUnwindFn unwind; // null if call stacks can't be found
	uintptr_t stacklow, stackhigh; // the main thread's stack
};

using Stack = std::array<uintptr_t, kMaxFrames>;

struct StackHash
{
	size_t operator()(const Stack& stack) const
	{
		uint64_t hash = 14695981039346656037ull;
		for (uintptr_t frame : stack)
		{
			hash = (hash ^ frame) * 1099511628211ull;
		}
		return static_cast<size_t>(hash);
	}
};

struct Profile
{
	std::unordered_map<uintptr_t, uint32_t> counts[NUMUWPPROFPHASES]; // samples in levels by address
	std::unordered_map<Stack, uint32_t, StackHash> stacks[NUMUWPPROFPHASES]; // samples in levels by call stack
	uint64_t levelsamples = 0;
	uint64_t othersamples = 0;
	uint64_t failedsamples = 0;
};

// Profiler samples and the time they paused the main thread, since the perf log last took them
std::atomic<uint32_t> g_pausesamples{0};
std::atomic<uint64_t> g_pausenanoseconds{0};

// The profiler's thread, which catches its own faults reading the main thread's stack
std::atomic<DWORD> g_samplerthreadid{0};

GetProcessorNumberFn g_getprocessornumber;
uint32_t g_processorcounts[kMaxProcessors]; // only touched by the main thread

// Walks a stack from context, filling frames with the address being run and then return addresses, innermost first,
// for up to maxframes. Takes no locks and allocates nothing, since the main thread could be paused holding them.
// Stops at a module it doesn't know, which it sets unknown to, so it can be added once the thread is resumed, unless
// discover is set, which adds the module right away (for walking the current thread's stack).
int UnwindStack(VirtualUnwindFn unwind, UnwindModules& modules, bool discover, uintptr_t stacklow, uintptr_t stackhigh,
	CONTEXT* context, uintptr_t* frames, int maxframes, uintptr_t* unknown)
{
	int count = 0;

	__try
	{
		frames[count++] = context->Rip;

		while (count < maxframes)
		{
			const uintptr_t pc = context->Rip;
			const DWORD64 sp = context->Rsp;
			const UnwindModule* module = modules.find(pc);

			if (!module && discover)
			{
				MEMORY_BASIC_INFORMATION info = {};
				if (VirtualQuery(reinterpret_cast<LPCVOID>(pc), &info, sizeof info) && info.Type == MEM_IMAGE)
				{
					modules.add(reinterpret_cast<uintptr_t>(info.AllocationBase));
					module = modules.find(pc);
				}
			}

			if (!module)
			{
				*unknown = pc;
				break;
			}

			if (const RuntimeFunction* function = FindFunction(*module, static_cast<DWORD>(pc - module->base)))
			{
				void* handlerdata;
				DWORD64 establisher;
				unwind(0, module->base, pc, function, context, &handlerdata, &establisher, nullptr);
			}
			else
			{
				// Functions that don't touch the stack have no unwind data; the return address is on top
				if (sp < stacklow || sp + sizeof (DWORD64) > stackhigh)
				{
					break;
				}
				context->Rip = *reinterpret_cast<const DWORD64*>(sp);
				context->Rsp = sp + sizeof (DWORD64);
			}

			if (!context->Rip || context->Rsp <= sp || context->Rsp < stacklow || context->Rsp >= stackhigh)
			{
				break;
			}
			frames[count++] = context->Rip;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return count;
}

void WriteProfile(const Profile& profile, double seconds)
{
	struct Entry
	{
		size_t module;
		uintptr_t offset;
		uint32_t count;
	};

	FILE* file = fopen(kProfilePath, "w");

	if (!file)
	{
		return;
	}

	// The module each address is in, from its 64 KB region, since modules are mapped at multiples of 64 KB
	std::vector<uintptr_t> modules;
	std::unordered_map<uintptr_t, size_t> regionmodules;
	auto find_module = [&](uintptr_t address) -> size_t
	{
		auto it = regionmodules.find(address >> 16);
		if (it != regionmodules.end())
		{
			return it->second;
		}

		MEMORY_BASIC_INFORMATION info = {};
		uintptr_t base = 0;
		if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof info))
		{
			base = reinterpret_cast<uintptr_t>(info.AllocationBase);
		}

		size_t index = std::find(modules.begin(), modules.end(), base) - modules.begin();
		if (index == modules.size())
		{
			modules.push_back(base);
		}
		regionmodules[address >> 16] = index;
		return index;
	};

	std::vector<Entry> entries[NUMUWPPROFPHASES];
	for (int phase = 0; phase < NUMUWPPROFPHASES; phase++)
	{
		for (const auto& [address, count] : profile.counts[phase])
		{
			const size_t module = find_module(address);
			entries[phase].push_back({module, address - modules[module], count});
		}
		std::sort(entries[phase].begin(), entries[phase].end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });
	}

	// Each address in a call stack, numbered in the order first seen
	std::unordered_map<uintptr_t, size_t> frameids;
	std::vector<uintptr_t> frames;
	for (const auto& stacks : profile.stacks)
	{
		for (const auto& [stack, count] : stacks)
		{
			for (uintptr_t frame : stack)
			{
				if (frame && frameids.emplace(frame, frames.size()).second)
				{
					frames.push_back(frame);
					find_module(frame);
				}
			}
		}
	}

	fprintf(file, "Ring Racers UWP profile: %llu samples while playing levels and %llu at other times over %.0f s (%.0f per second), %llu failed\n",
		static_cast<unsigned long long>(profile.levelsamples), static_cast<unsigned long long>(profile.othersamples), seconds,
		(profile.levelsamples + profile.othersamples) / seconds, static_cast<unsigned long long>(profile.failedsamples));
	fprintf(file, "Lines after each phase: module, offset into it (hex), samples\n");
	fprintf(file, "Lines after each phase's stacks: samples, then the frames of a call stack, innermost first; frames after the first are return addresses\n");

	for (size_t i = 0; i < modules.size(); i++)
	{
		char path[MAX_PATH] = "?";
		if (modules[i] && !GetModuleFileNameA(reinterpret_cast<HMODULE>(modules[i]), path, sizeof path))
		{
			snprintf(path, sizeof path, "?");
		}
		fprintf(file, "module %zu: %s\n", i, path);
	}

	for (int phase = 0; phase < NUMUWPPROFPHASES; phase++)
	{
		uint64_t samples = 0;
		for (const Entry& entry : entries[phase])
		{
			samples += entry.count;
		}

		fprintf(file, "phase %s: %llu samples\n", kPhaseNames[phase], static_cast<unsigned long long>(samples));
		for (const Entry& entry : entries[phase])
		{
			fprintf(file, "%zu %llx %u\n", entry.module, static_cast<unsigned long long>(entry.offset), entry.count);
		}
	}

	fprintf(file, "frames: %zu\n", frames.size());
	for (size_t i = 0; i < frames.size(); i++)
	{
		const size_t module = find_module(frames[i]);
		fprintf(file, "frame %zu %zu %llx\n", i, module, static_cast<unsigned long long>(frames[i] - modules[module]));
	}

	for (int phase = 0; phase < NUMUWPPROFPHASES; phase++)
	{
		fprintf(file, "stacks %s: %zu\n", kPhaseNames[phase], profile.stacks[phase].size());
		for (const auto& [stack, count] : profile.stacks[phase])
		{
			fprintf(file, "%u", count);
			for (uintptr_t frame : stack)
			{
				if (!frame)
				{
					break;
				}
				fprintf(file, " %zu", frameids[frame]);
			}
			fprintf(file, "\n");
		}
	}

	fclose(file);
}

void SampleMainThread(Sampler sampler)
{
	using Clock = std::chrono::steady_clock;

	Profile profile;
	UnwindModules modules;
	const Clock::time_point start = Clock::now();
	Clock::time_point lastwrite = start;
	uint64_t written = 0;

	modules.add(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
	g_samplerthreadid = GetCurrentThreadId();

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	// Sleep() can take as long as the system timer's period, which is often 15 ms
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	for (;;)
	{
		LARGE_INTEGER due;
		due.QuadPart = kSampleInterval;

		if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
		{
			WaitForSingleObject(timer, INFINITE);
		}
		else
		{
			Sleep(20);
		}

		CONTEXT context = {};
		context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

		// Nothing that could wait on the main thread (like allocating) may happen until it's resumed
		BOOL gotcontext = FALSE;
		INT32 phase = UWPPROF_OTHER;
		INT32 inlevel = false;
		Stack stack = {};
		int frames = 0;
		uintptr_t unknown = 0;
		const Clock::time_point pausestart = Clock::now();
		if (sampler.suspend(sampler.thread) != static_cast<DWORD>(-1))
		{
			gotcontext = sampler.getcontext(sampler.thread, &context);
			phase = i_uwpprofphase;
			inlevel = i_uwpprofinlevel;
			if (gotcontext && inlevel && sampler.unwind)
			{
				frames = UnwindStack(sampler.unwind, modules, false, sampler.stacklow, sampler.stackhigh, &context,
					stack.data(), kMaxFrames, &unknown);
			}
			sampler.resume(sampler.thread);
		}
		g_pausenanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - pausestart).count();
		g_pausesamples++;

		if (!gotcontext)
		{
			profile.failedsamples++;
		}
		else if (inlevel && phase >= 0 && phase < NUMUWPPROFPHASES)
		{
			profile.counts[phase][frames ? stack[0] : context.Rip]++;
			if (frames)
			{
				profile.stacks[phase][stack]++;
			}
			profile.levelsamples++;
		}
		else
		{
			profile.othersamples++;
		}

		// A module that wasn't loaded when the last stack was unwound, to unwind through next time
		if (unknown)
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(unknown), &info, sizeof info) && info.Type == MEM_IMAGE)
			{
				modules.add(reinterpret_cast<uintptr_t>(info.AllocationBase));
			}
		}

		// Written when there are new samples from levels, or failures, so those show up too
		const Clock::time_point now = Clock::now();
		if (now - lastwrite >= kWriteInterval && profile.levelsamples + profile.failedsamples > written)
		{
			WriteProfile(profile, std::chrono::duration<double>(now - start).count());
			written = profile.levelsamples + profile.failedsamples;
			lastwrite = now;
		}
	}
}

} // namespace

void I_UWPStartProfiler(void)
{
	Sampler sampler = {};

	sampler.suspend = reinterpret_cast<SuspendResumeThreadFn>(FindKernelFunction("SuspendThread"));
	sampler.resume = reinterpret_cast<SuspendResumeThreadFn>(FindKernelFunction("ResumeThread"));
	sampler.getcontext = reinterpret_cast<GetThreadContextFn>(FindKernelFunction("GetThreadContext"));
	if (!sampler.suspend || !sampler.resume || !sampler.getcontext)
	{
		I_OutputMsg("UWP: can't profile: SuspendThread, ResumeThread, or GetThreadContext is missing\n");
		return;
	}

	// GetCurrentThread() only means the main thread when called from it
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampler.thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
	{
		I_OutputMsg("UWP: can't profile: error %lu getting the main thread\n", GetLastError());
		return;
	}

	sampler.unwind = reinterpret_cast<VirtualUnwindFn>(FindKernelFunction("RtlVirtualUnwind"));
	auto getstacklimits = reinterpret_cast<GetStackLimitsFn>(FindKernelFunction("GetCurrentThreadStackLimits"));
	if (sampler.unwind && getstacklimits)
	{
		ULONG_PTR low = 0, high = 0;
		getstacklimits(&low, &high);
		sampler.stacklow = low;
		sampler.stackhigh = high;
	}
	if (!sampler.stackhigh)
	{
		sampler.unwind = nullptr;
		I_OutputMsg("UWP: profiling without call stacks: RtlVirtualUnwind or GetCurrentThreadStackLimits is missing\n");
	}

	try
	{
		std::thread(SampleMainThread, sampler).detach();
	}
	catch (const std::system_error& error)
	{
		I_OutputMsg("UWP: can't profile: %s\n", error.what());
		CloseHandle(sampler.thread);
		return;
	}

	I_OutputMsg("UWP: profiling the main thread while playing levels about %lld times a second, written to %s every %lld s\n",
		-10000000LL / kSampleInterval, kProfilePath, static_cast<long long>(kWriteInterval.count()));
}

void I_UWPTakeProfilerStats(INT32 *samples, double *pausedms)
{
	*samples = static_cast<INT32>(g_pausesamples.exchange(0));
	*pausedms = g_pausenanoseconds.exchange(0) / 1e6;
}

namespace
{

struct CpuSet
{
	DWORD id;
	uint8_t processor, core, cache, efficiency, flags, scheduling;
};

constexpr uint8_t kCpuSetParked = 1;
constexpr uint8_t kCpuSetAllocated = 2; // for some processes only
constexpr uint8_t kCpuSetAllocatedToUs = 4;
constexpr uint8_t kCpuSetRealTime = 8;

// The CPU sets Windows lists for the game, or none if it can't. Logs why it can't if log is set.
std::vector<CpuSet> ReadCpuSets(bool log)
{
	auto getinfo = reinterpret_cast<GetSystemCpuSetInformationFn>(FindKernelFunction("GetSystemCpuSetInformation"));
	std::vector<CpuSet> sets;
	ULONG size = 0;

	if (!getinfo)
	{
		if (log)
			I_OutputMsg("UWP: can't list CPU sets: GetSystemCpuSetInformation is missing\n");
		return sets;
	}

	getinfo(nullptr, 0, &size, GetCurrentProcess(), 0);
	std::vector<uint8_t> buffer(size);
	if (!size || !getinfo(buffer.data(), size, &size, GetCurrentProcess(), 0))
	{
		if (log)
			I_OutputMsg("UWP: can't list CPU sets: error %lu\n", GetLastError());
		return sets;
	}

	// SYSTEM_CPU_SET_INFORMATION, read by offset, since UWP apps may not get its definition
	for (size_t offset = 0; offset + 8 <= size;)
	{
		const uint8_t* entry = buffer.data() + offset;
		DWORD entrysize, type;

		memcpy(&entrysize, entry, sizeof entrysize);
		memcpy(&type, entry + 4, sizeof type);
		if (!entrysize || offset + entrysize > size)
		{
			break;
		}

		if (type == 0 && entrysize >= 32) // CpuSetInformation
		{
			CpuSet set;
			memcpy(&set.id, entry + 8, sizeof set.id);
			set.processor = entry[14];
			set.core = entry[15];
			set.cache = entry[16];
			set.efficiency = entry[18];
			set.flags = entry[19];
			set.scheduling = entry[20];
			sets.push_back(set);
		}
		offset += entrysize;
	}

	return sets;
}

} // namespace

unsigned int I_UWPUsableProcessors(void)
{
	unsigned int ours = 0;

	// Only trusted when some processors are marked as the game's. As an app, Xbox marks none: it lists the
	// processors it keeps for games as allocated to someone else, and the system's as free
	for (const CpuSet& set : ReadCpuSets(false))
	{
		if (set.flags & kCpuSetAllocatedToUs)
		{
			ours++;
		}
	}
	return ours >= 2 ? ours : 0;
}

int I_UWPCpuSetMode(void)
{
	unsigned int ours = 0, others = 0;

	for (const CpuSet& set : ReadCpuSets(false))
	{
		if (set.flags & kCpuSetAllocatedToUs)
		{
			ours++;
		}
		else if (set.flags & kCpuSetAllocated)
		{
			others++;
		}
	}

	if (ours)
	{
		return 1;
	}
	return others ? 0 : -1;
}

void I_UWPLogCpuSets(void)
{
	auto getprocesssets = reinterpret_cast<GetCpuSetsFn>(FindKernelFunction("GetProcessDefaultCpuSets"));
	auto getthreadsets = reinterpret_cast<GetCpuSetsFn>(FindKernelFunction("GetThreadSelectedCpuSets"));

	g_getprocessornumber = reinterpret_cast<GetProcessorNumberFn>(FindKernelFunction("GetCurrentProcessorNumber"));

	for (const CpuSet& set : ReadCpuSets(true))
	{
		I_OutputMsg("UWP: CPU set %lu: processor %u, core %u, cache %u, efficiency class %u, scheduling class %u%s%s%s%s\n",
			set.id, set.processor, set.core, set.cache, set.efficiency, set.scheduling,
			(set.flags & kCpuSetParked) ? ", parked" : "", (set.flags & kCpuSetAllocated) ? ", allocated" : "",
			(set.flags & kCpuSetAllocatedToUs) ? ", allocated to this process" : "",
			(set.flags & kCpuSetRealTime) ? ", real time" : "");
	}

	auto log_sets = [](const char* what, GetCpuSetsFn get, HANDLE handle)
	{
		ULONG ids[64];
		ULONG count = 0;
		char list[512] = "";

		if (!get || !get(handle, ids, 64, &count))
		{
			return;
		}

		for (ULONG i = 0; i < count && i < 64; i++)
		{
			const size_t used = strlen(list);
			snprintf(list + used, sizeof list - used, "%s%lu", i ? ", " : "", ids[i]);
		}
		I_OutputMsg("UWP: %s CPU sets: %s\n", what, count ? list : "none (any)");
	};
	log_sets("the process's default", getprocesssets, GetCurrentProcess());
	log_sets("the main thread's", getthreadsets, GetCurrentThread());
}

void I_UWPNoteProcessor(void)
{
	if (g_getprocessornumber)
	{
		g_processorcounts[g_getprocessornumber() % kMaxProcessors]++;
	}
}

void I_UWPTakeProcessorStats(char *buffer, size_t size)
{
	uint32_t total = 0;

	buffer[0] = '\0';
	for (uint32_t count : g_processorcounts)
	{
		total += count;
	}

	for (int i = 0; i < kMaxProcessors && total; i++)
	{
		if (g_processorcounts[i])
		{
			const size_t used = strlen(buffer);
			snprintf(buffer + used, size - used, "%s%d (%.0f%%)", used ? ", " : "", i, 100.0 * g_processorcounts[i] / total);
		}
	}
	if (!total)
	{
		snprintf(buffer, size, "unknown");
	}

	memset(g_processorcounts, 0, sizeof g_processorcounts);
}

// [UWP]: Logs crashes that the game's own handler never sees. The app runtime starts the main thread, not the C
// runtime, so the signal handler isn't called for an access violation there, and Windows closes the game without
// a word. A vectored exception handler sees every exception first, so it logs this one and its call stack to
// latest-log.txt and leaves the rest as before. If something catches the exception, the game carries on, so a logged
// "crash" that the game survived wasn't one.
extern "C" FILE *logstream;

namespace
{

// long, not LONG, which m_swap.h defines as a macro
using VectoredHandlerFn = long (CALLBACK *)(EXCEPTION_POINTERS*);
using AddVectoredExceptionHandlerFn = void* (WINAPI *)(ULONG, VectoredHandlerFn);

DWORD g_mainthreadid;
VirtualUnwindFn g_crashunwind;
GetStackLimitsFn g_crashstacklimits;
UnwindModules g_crashmodules;
std::atomic<int> g_crashlogs{0};

bool IsCrash(DWORD code)
{
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_STACK_OVERFLOW:
	case 0xC0000374: // STATUS_HEAP_CORRUPTION
	case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
		return true;
	default:
		return false;
	}
}

// Writes where an address is as module+offset, which for the game itself ringracers-uwp.map maps to a function
void DescribeAddress(uintptr_t address, char* buffer, size_t size)
{
	MEMORY_BASIC_INFORMATION info = {};
	char path[MAX_PATH] = "?";

	if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof info) || info.Type != MEM_IMAGE)
	{
		snprintf(buffer, size, "%llx", static_cast<unsigned long long>(address));
		return;
	}

	const uintptr_t base = reinterpret_cast<uintptr_t>(info.AllocationBase);
	if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, sizeof path))
	{
		snprintf(path, sizeof path, "?");
	}
	const char* name = strrchr(path, '\\');
	snprintf(buffer, size, "%s+%llx", name ? name + 1 : path, static_cast<unsigned long long>(address - base));
}

long CALLBACK LogCrash(EXCEPTION_POINTERS* info)
{
	const DWORD code = info->ExceptionRecord->ExceptionCode;
	const DWORD thread = GetCurrentThreadId();
	const char* threadname = thread == g_mainthreadid ? "the main thread" : "another thread";
	char where[MAX_PATH + 32];

	// The profiler catches its own faults reading the main thread's stack
	if (!IsCrash(code) || thread == g_samplerthreadid)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Each one, in case something catches them, but not endlessly
	if (g_crashlogs++ >= 5)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (code == EXCEPTION_STACK_OVERFLOW)
	{
		// Too little stack is left to format anything
		if (logstream)
		{
			fputs("UWP: crash: stack overflow\n", logstream);
			fflush(logstream);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	DescribeAddress(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress), where, sizeof where);
	if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
	{
		const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
		I_OutputMsg("UWP: crash: access violation %s %llx at %s on %s\n",
			kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
			static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]), where, threadname);
	}
	else
	{
		I_OutputMsg("UWP: crash: exception %08lx at %s on %s\n", code, where, threadname);
	}

	if (g_crashunwind && g_crashstacklimits)
	{
		ULONG_PTR low = 0, high = 0;
		CONTEXT context = *info->ContextRecord;
		uintptr_t frames[32];
		uintptr_t unknown = 0;

		g_crashstacklimits(&low, &high);
		const int count = UnwindStack(g_crashunwind, g_crashmodules, true, low, high, &context, frames, 32, &unknown);

		// Return addresses point after their call, so describe the byte before, which is in the caller
		for (int i = 1; i < count; i++)
		{
			DescribeAddress(frames[i] - 1, where, sizeof where);
			I_OutputMsg("UWP:   called from %s\n", where);
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void I_UWPInstallCrashHandler(void)
{
	auto add = reinterpret_cast<AddVectoredExceptionHandlerFn>(FindKernelFunction("AddVectoredExceptionHandler"));

	g_mainthreadid = GetCurrentThreadId();
	g_crashunwind = reinterpret_cast<VirtualUnwindFn>(FindKernelFunction("RtlVirtualUnwind"));
	g_crashstacklimits = reinterpret_cast<GetStackLimitsFn>(FindKernelFunction("GetCurrentThreadStackLimits"));

	if (!add || !add(1, LogCrash))
	{
		I_OutputMsg("UWP: can't log crashes: AddVectoredExceptionHandler is missing or failed\n");
	}
}
