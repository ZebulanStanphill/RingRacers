// UWP-only LLVM instrumentation capture. No simulation state is touched.
#include "../i_system.h"
#include "../doomdef.h"

#if defined(_UWP) && defined(SRB2_PGO_GENERATE)
#include <SDL.h>
#include <chrono>
#include <cstdio>
#include <mutex>

extern "C" {
// Supply the runtime anchor ourselves: disable its automatic initializer/atexit
// writer. All writes are explicit, including suspension, with a writable path.
int __llvm_profile_runtime = 0;
void __llvm_profile_set_filename(const char *);
int __llvm_profile_write_file(void);
void __llvm_profile_reset_counters(void);
}

static std::mutex pgoMutex;
static char pgoFilename[256];
static unsigned long long pgoSequence;
static bool pgoReady;

static void PgoNextFile(void)
{
	const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::snprintf(pgoFilename, sizeof pgoFilename,
		"E:\\ringracers\\pgo\\ringracers-%lld-%llu.profraw",
		static_cast<long long>(stamp), pgoSequence++);
	__llvm_profile_set_filename(pgoFilename);
}

void I_UWPPgoDump(void)
{
	std::lock_guard<std::mutex> lock(pgoMutex);
	if (!pgoReady)
		return;
	const int result = __llvm_profile_write_file();
	I_OutputMsg("PGO: %s: %s (status %d)\n", pgoFilename,
		result == 0 ? "saved" : "FAILED", result);
	// Keep counters on failure so a later dump can retry without losing data.
	if (result == 0)
	{
		__llvm_profile_reset_counters();
		PgoNextFile();
	}
}

static int SDLCALL PgoEventWatch(void *, SDL_Event *event)
{
	// SDL delivers this synchronously from WinRT's suspension callback, before
	// suspension completes; polling later in the game loop would be too late.
	if (event->type == SDL_APP_WILLENTERBACKGROUND)
		I_UWPPgoDump();
	return 1;
}

void I_UWPPgoInit(void)
{
	std::lock_guard<std::mutex> lock(pgoMutex);
	if (pgoReady)
		return;
	I_mkdir("E:\\ringracers", 0755);
	I_mkdir("E:\\ringracers\\pgo", 0755);
	PgoNextFile();
	pgoReady = true;
	SDL_AddEventWatch(PgoEventWatch, nullptr);
	I_OutputMsg("PGO: recording to %s\n", pgoFilename);
}
#endif
