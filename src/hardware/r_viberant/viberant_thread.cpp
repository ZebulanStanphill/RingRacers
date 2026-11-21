// DR. ROBOTNIK'S RING RACERS
// This program is free software distributed under the GNU General Public License, version 2.

// Windows SDK headers must precede the game's boolean macro.
#include "viberant_present.h"
#include "viberant_thread.h"

#include "../../command.h"
#include "../../doomstat.h"
#include "../../i_system.h"
#include "../../p_tick.h"
#include "../../r_fps.h"
#include "../../r_patchrotation.h"
#include "../../screen.h"
#include "../hw_main.h"
#include "../hw_model.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>

#undef min
#undef max
#undef clamp

extern "C" consvar_t cv_grdriverthread;

namespace
{

using namespace rr_viberant;
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

constexpr size_t kAlignment = 16;
constexpr size_t kQueueBytes = 16 * 1024 * 1024;

enum class SyncReason
{
	Other, TextureUpload, TextureUpdate, TextureDelete, TextureFlush, Palette, Readback, State, Shader,
	Model, Sky, Wipe, Portal, Window, Probe, Test, Frame, LargeCommand, Count
};

constexpr const char *kSyncNames[] = {
	"other", "texture uploads", "texture updates", "texture deletes", "texture flushes", "palette", "readbacks",
	"state", "shaders", "models", "sky rebuilds", "wipes", "portals", "window", "surface probes", "tests",
	"main-thread frames", "large commands"
};
static_assert(std::size(kSyncNames) == static_cast<size_t>(SyncReason::Count));

struct ProducerStats
{
	std::array<UINT64, std::size(kSyncNames)> syncs{};
	Duration syncWait{}, frameWait{}, worstFrameWait{}, spaceWait{};
	UINT64 frames = 0, bytes = 0, peakBytes = 0, spaceWaits = 0, frameWaits = 0;
};

struct WorkerStats
{
	Duration busy{}, frameBusy{}, worstFrameBusy{};
};

enum class RecordKind : UINT32 { Draw, Finish, Perf };

struct alignas(kAlignment) Record
{
	// This dispatch function is the opcode. A null opcode skips padding at the end of the ring.
	void (*execute)(std::byte *payload);
	UINT32 size;
	RecordKind kind;
};
static_assert(sizeof(Record) == kAlignment);

class Wakeup
{
public:
	template<class Ready>
	bool Wait(Ready ready, Clock::time_point deadline, bool producer)
	{
		std::unique_lock<std::mutex> lock(mutex);
		armed.store(true, std::memory_order_relaxed);
		// Either the waiter sees the new position or the publisher sees the armed waiter.
		std::atomic_thread_fence(std::memory_order_seq_cst);
		while (!ready())
		{
#if defined(HAVE_VIBERANT_D3D) && !defined(_UWP)
			if (producer)
			{
				// DXGI Present can wait for sent window messages. Leave queued input for SDL.
				lock.unlock();
				MSG message;
				PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
				lock.lock();
				const auto nextCheck = std::min(deadline, Clock::now() + std::chrono::milliseconds(2));
				if (!ready())
					condition.wait_until(lock, nextCheck);
				if (deadline != Clock::time_point::max() && Clock::now() >= deadline)
					break;
				continue;
			}
#else
			(void)producer;
#endif
			if (deadline == Clock::time_point::max())
				condition.wait(lock);
			else if (condition.wait_until(lock, deadline) == std::cv_status::timeout)
				break;
		}
		armed.store(false, std::memory_order_relaxed);
		return ready();
	}

	void Signal()
	{
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (armed.load(std::memory_order_relaxed))
		{
			std::lock_guard<std::mutex> lock(mutex);
			condition.notify_one();
		}
	}

private:
	std::atomic<bool> armed{false};
	std::mutex mutex;
	std::condition_variable condition;
};

struct Queue;
thread_local Queue *workerQueue = nullptr;

struct Queue
{
	explicit Queue(size_t bytes) : capacity(bytes), storage(new std::byte[bytes + kAlignment - 1])
	{
		void *start = storage.get();
		size_t space = bytes + kAlignment - 1;
		data = static_cast<std::byte *>(std::align(kAlignment, bytes, start, space));
	}

	const size_t capacity;
	std::unique_ptr<std::byte[]> storage;
	std::byte *data;
	std::thread worker;
	alignas(64) std::atomic<UINT64> published{0};
	alignas(64) std::atomic<UINT64> consumed{0};
	Wakeup workReady, progress;
	std::atomic<bool> stop{false}, discard{false}, failed{false};
	char fatal[1024]{};

	// The producer owns these positions. The consumer only uses published and consumed.
	UINT64 tail = 0, previousFrame = 0, frameBytes = 0;

	// The consumer owns these timings.
	WorkerStats timings;
	Clock::time_point busySince{};

	std::byte *At(UINT64 position)
	{
		return data + (position % capacity);
	}

	bool Done(UINT64 position) const
	{
		return consumed.load(std::memory_order_acquire) >= position;
	}

	bool WaitFor(UINT64 position, Clock::time_point deadline = Clock::time_point::max())
	{
		const auto ready = [&] { return Done(position); };
		return ready() || progress.Wait(ready, deadline, true);
	}

	void Publish(size_t bytes)
	{
		tail += bytes;
		published.store(tail, std::memory_order_release);
		workReady.Signal();
	}

	void AccountBusy()
	{
		const auto now = Clock::now();
		timings.busy += now - busySince;
		timings.frameBusy += now - busySince;
		busySince = now;
	}

	void Run()
	{
		workerQueue = this;
#ifdef _UWP
		I_UWPProfileDriverThread(true);
#endif
		UINT64 head = 0;
		for (;;)
		{
			auto ready = [&] { return published.load(std::memory_order_acquire) != head || stop.load(); };
			for (unsigned i = 0; i < 128 && !ready(); ++i)
			{
#if defined(HAVE_VIBERANT_D3D)
				YieldProcessor();
#endif
			}
			if (!ready())
				workReady.Wait(ready, Clock::time_point::max(), false);
			const UINT64 end = published.load(std::memory_order_acquire);
			if (head == end && stop.load())
				break;

			busySince = Clock::now();
			do
			{
				auto *record = std::launder(reinterpret_cast<Record *>(At(head)));
				const UINT32 size = record->size;
				if (record->execute && !failed.load(std::memory_order_relaxed) && !discard.load(std::memory_order_relaxed))
				{
					const RecordKind kind = record->kind;
					if (kind == RecordKind::Perf)
						AccountBusy();
					try
					{
						record->execute(reinterpret_cast<std::byte *>(record + 1));
					}
					catch (const std::exception &error)
					{
						DriverFatal("VibeRant driver thread: %s", error.what());
					}
					catch (...)
					{
						DriverFatal("VibeRant driver thread failed.");
					}
					if (kind == RecordKind::Finish)
					{
#ifdef _UWP
						// [UWP]: Count where the driver thread ran, once per frame
						I_UWPNoteThreadProcessor(UWPTHREAD_DRIVER);
#endif
						AccountBusy();
						timings.worstFrameBusy = std::max(timings.worstFrameBusy, timings.frameBusy);
						timings.frameBusy = {};
					}
				}
				head += size;
				consumed.store(head, std::memory_order_release);
				progress.Signal();
			} while (head != published.load(std::memory_order_acquire));
			AccountBusy();
		}
#ifdef _UWP
		I_UWPProfileDriverThread(false);
#endif
	}
};

std::unique_ptr<Queue> queue;
ProducerStats stats;
UINT64 totalReadbacks = 0, totalSyncs = 0, totalWraps = 0, peakPresents = 0;
bool probeActive = false, reportingFatal = false, abandoned = false;
bool surfaceStartup = false;
INT32 testMode = -1;
size_t testCapacity = 0;
int portalSupport = -1;
bool sentShaders = false;
INT32 shaderOption = 0;

void CheckFatal()
{
	if (queue && queue->failed.load(std::memory_order_acquire) && !reportingFatal)
	{
		reportingFatal = true;
		I_Error("%s", queue->fatal);
	}
}

bool Queued()
{
	CheckFatal();
	return queue && !probeActive && !surfaceStartup;
}

void Sync(SyncReason reason)
{
	if (!queue)
		return;
	++stats.syncs[static_cast<size_t>(reason)];
	++totalSyncs;
	if (reason == SyncReason::Readback)
		++totalReadbacks;
	if (!queue->Done(queue->tail))
	{
		const auto start = Clock::now();
		queue->WaitFor(queue->tail);
		stats.syncWait += Clock::now() - start;
	}
	CheckFatal();
}

void WaitForSpace(UINT64 end)
{
	if (end <= queue->capacity || queue->Done(end - queue->capacity))
		return;
	const auto start = Clock::now();
	queue->WaitFor(end - queue->capacity);
	stats.spaceWait += Clock::now() - start;
	++stats.spaceWaits;
	CheckFatal();
}

#ifdef HAVE_VIBERANT_D3D
void StartQueue()
{
	try
	{
		const size_t bytes = testCapacity ? testCapacity : kQueueBytes;
		auto state = std::make_unique<Queue>(bytes);
		state->worker = std::thread([state = state.get()] { state->Run(); });
		queue = std::move(state);
	}
	catch (const std::exception &error)
	{
		Core_Log("couldn't start the driver thread (%s); drawing on the main thread", error.what());
	}
}
#endif

bool StopQueue(bool shutdown)
{
	if (!queue)
		return true;
	const auto deadline = shutdown ? Clock::now() + std::chrono::seconds(2) : Clock::time_point::max();
	const bool drained = queue->WaitFor(queue->tail, deadline);
	if (!drained)
	{
		Core_Log("driver thread did not drain within 2 seconds; leaving graphics objects for process exit");
		queue->discard.store(true);
	}
	queue->stop.store(true);
	queue->workReady.Signal();
	if (!drained)
	{
		queue->worker.detach();
		queue.release(); // A stuck command still owns this queue and the driver's resources.
		abandoned = true;
		return false;
	}
	if (queue->worker.joinable())
		queue->worker.join();
	CheckFatal();
	queue.reset();
	stats = {};
	return true;
}

template<class Command, class Copy>
void Enqueue(Command command, size_t extra, RecordKind kind, Copy copy)
{
	static_assert(std::is_trivially_copyable_v<Command> && std::is_trivially_destructible_v<Command>);
	static_assert(alignof(Command) <= kAlignment);
	constexpr size_t overhead = sizeof(Record) + sizeof(Command);
	const size_t size = (overhead + extra + kAlignment - 1) & ~(kAlignment - 1);
	const size_t remaining = queue->capacity - (queue->tail % queue->capacity);

	if (size > remaining)
	{
		WaitForSpace(queue->tail + remaining);
		new (queue->At(queue->tail)) Record{nullptr, static_cast<UINT32>(remaining), RecordKind::Draw};
		queue->Publish(remaining);
		++totalWraps;
	}
	WaitForSpace(queue->tail + size);
	auto *record = new (queue->At(queue->tail)) Record{
		[](std::byte *payload) {
			auto &call = *std::launder(reinterpret_cast<Command *>(payload));
			call(payload + sizeof(Command));
		},
		static_cast<UINT32>(size), kind
	};
	new (record + 1) Command(command);
	copy(reinterpret_cast<std::byte *>(record + 1) + sizeof(Command));
	queue->frameBytes += size;
	queue->Publish(size);
}

template<class Command>
void Enqueue(Command command, RecordKind kind = RecordKind::Draw)
{
	Enqueue([command](std::byte *) mutable { command(); }, 0, kind, [](std::byte *) {});
}

// Variable-sized data starts on a 16-byte boundary after these command headers.
template<class T>
std::byte *CopyArray(std::byte *to, const T *from, size_t count)
{
	static_assert(std::is_trivially_copyable_v<T>);
	if (count)
		std::uninitialized_copy_n(from, count, reinterpret_cast<T *>(to));
	return to + count * sizeof(T);
}

template<class T>
struct Optional
{
	explicit Optional(const T *p) : value(p ? *p : T{}), present(p != nullptr) {}
	T *Get() { return present ? &value : nullptr; }
	T value;
	bool present;
};

template<class Command>
bool Fits(UINT64 extra)
{
	static_assert(alignof(Command) == kAlignment);
	return extra <= queue->capacity - sizeof(Record) - sizeof(Command);
}

double Milliseconds(Duration duration)
{
	return std::chrono::duration<double, std::milli>(duration).count();
}

void LogQueuePerf(double seconds, const ProducerStats &sample)
{
	if (!workerQueue)
	{
		I_OutputMsg("VibeRant perf: driver thread: %s\n",
			Core_Vulkan() ? "unavailable on Vulkan" : Core_Metal() ? "unavailable on Metal" : "off");
		return;
	}

	const auto work = workerQueue->timings;
	workerQueue->timings.busy = {};
	workerQueue->timings.worstFrameBusy = {};
	const double frames = static_cast<double>(std::max<UINT64>(sample.frames, 1));
	UINT64 syncs = 0;
	char reasons[512] = {};
	size_t used = 0;
	for (size_t i = 0; i < sample.syncs.size(); ++i)
	{
		const auto count = sample.syncs[i];
		if (!count)
			continue;
		syncs += count;
		const int wrote = snprintf(reasons + used, sizeof reasons - used, "%s%s %llu",
			used ? ", " : ": ", kSyncNames[i], static_cast<unsigned long long>(count));
		if (wrote > 0)
			used += std::min(static_cast<size_t>(wrote), sizeof reasons - used - 1);
	}
	I_OutputMsg("VibeRant perf: driver thread: on; per frame: worker busy %.2f ms (worst %.2f), main waited %.2f ms for the previous frame (worst %.2f) and %.2f ms for queue space; queue %.2f MB per frame (peak %.2f); frame waits %llu, space waits %llu; syncs %.1f per second (%.2f ms waited)%s\n",
		Milliseconds(work.busy) / frames, Milliseconds(work.worstFrameBusy), Milliseconds(sample.frameWait) / frames,
		Milliseconds(sample.worstFrameWait), Milliseconds(sample.spaceWait) / frames, sample.bytes / (1048576.0 * frames),
		sample.peakBytes / 1048576.0, static_cast<unsigned long long>(sample.frameWaits),
		static_cast<unsigned long long>(sample.spaceWaits), syncs / seconds, Milliseconds(sample.syncWait), reasons);
}

} // namespace

void rr_viberant::DriverFatal(const char *format, ...)
{
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);

	if (!workerQueue)
		I_Error("%s", message);
	if (!workerQueue->failed.load(std::memory_order_relaxed))
	{
		snprintf(workerQueue->fatal, sizeof workerQueue->fatal, "%s", message);
		workerQueue->failed.store(true, std::memory_order_release);
	}
}

boolean VibeRant_OnDriverThread(void) { return workerQueue != nullptr; }
boolean VibeRant_DriverAbandoned(void) { return abandoned; }

// Synchronous calls keep their normal argument and return types.
#define SYNC_DRIVER(result, name, reason, parameters, arguments) \
	result VibeRant_##name parameters \
	{ \
		Sync(SyncReason::reason); \
		return impl::VibeRant_##name arguments; \
	}

SYNC_DRIVER(boolean, Init, Window, (), ())
SYNC_DRIVER(void, SetPalette, Palette, (RGBA_t *palette), (palette))
SYNC_DRIVER(void, UpdateTexture, TextureUpdate, (GLMipmap_t *texture), (texture))
SYNC_DRIVER(void, DeleteTexture, TextureDelete, (GLMipmap_t *texture), (texture))
SYNC_DRIVER(void, ReadRect, Readback, (INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16 *dest), (x, y, w, h, stride, dest))
SYNC_DRIVER(void, ClearMipMapCache, TextureFlush, (), ())
SYNC_DRIVER(void, CreateModelVBOs, Model, (model_t *model), (model))
SYNC_DRIVER(INT32, GetTextureUsed, Other, (), ())
SYNC_DRIVER(void, FlushScreenTextures, TextureFlush, (), ())
SYNC_DRIVER(void, StartScreenWipe, Wipe, (), ())
SYNC_DRIVER(void, EndScreenWipe, Wipe, (), ())
SYNC_DRIVER(void, DoScreenWipe, Wipe, (), ())
SYNC_DRIVER(void, DrawIntermissionBG, Other, (), ())
SYNC_DRIVER(boolean, CompileShaders, Shader, (), ())
SYNC_DRIVER(void, CleanShaders, Shader, (), ())
SYNC_DRIVER(void, LoadCustomShader, Shader, (int n, char *code, size_t bytes, boolean fragment), (n, code, bytes, fragment))
SYNC_DRIVER(void, SetPaletteLookup, Palette, (UINT8 *lut), (lut))
SYNC_DRIVER(UINT32, CreateLightTable, Palette, (RGBA_t *table), (table))
SYNC_DRIVER(void, UpdateLightTable, Palette, (UINT32 id, RGBA_t *table), (id, table))
SYNC_DRIVER(void, ClearLightTables, Palette, (), ())
SYNC_DRIVER(void, SelectBackend, Window, (boolean vulkan, boolean validation, boolean software), (vulkan, validation, software))
SYNC_DRIVER(boolean, RequestSurfaceProbe, Probe, (INT32 x, INT32 y), (x, y))
SYNC_DRIVER(boolean, ApplyFadeMap, Palette, (const UINT8 *table, size_t size, UINT32 id, UINT8 strength), (table, size, id, strength))
SYNC_DRIVER(void, GetPresentTrace, Other, (INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe),
	(copied, shown, copyframe, showframe))
SYNC_DRIVER(void, GetCaptureCountsForTest, Test, (UINT64 *fog, UINT64 *water, UINT64 *roundTrips), (fog, water, roundTrips))
SYNC_DRIVER(void, GetReuseCountsForTest, Test, (UINT64 *placed, UINT64 *blocks, UINT64 *srvs), (placed, blocks, srvs))
SYNC_DRIVER(void, GetTextureStorageCountsForTest, Test, (UINT64 *placed, UINT64 *dedicated, UINT64 *committed),
	(placed, dedicated, committed))
SYNC_DRIVER(boolean, GetTexturePlacementForTest, Test, (UINT32 id, UINT32 *heap, UINT64 *offset, UINT64 *bytes, UINT64 *reported, UINT64 *alignment),
	(id, heap, offset, bytes, reported, alignment))
SYNC_DRIVER(UINT32, GetAtlasPagesForTest, Test, (), ())
SYNC_DRIVER(void, SetRingBytesForTest, Test, (size_t vertex, size_t index, size_t constant), (vertex, index, constant))
SYNC_DRIVER(void, ReadPaletteRectForTest, Test, (INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16 *dest), (x, y, w, h, stride, dest))
SYNC_DRIVER(void, ReadPresentedRectForTest, Test, (INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16 *dest), (x, y, w, h, stride, dest))
SYNC_DRIVER(boolean, ReadPlacedRectForTest, Test, (INT32 mode, INT32 screenW, INT32 screenH, INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16 *dest),
	(mode, screenW, screenH, x, y, w, h, stride, dest))
SYNC_DRIVER(boolean, ReadFitRectForTest, Test, (INT32 screenW, INT32 screenH, INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16 *dest),
	(screenW, screenH, x, y, w, h, stride, dest))
#undef SYNC_DRIVER

// Value-only commands share their direct and queued implementations.
#define QUEUE_DRIVER(name, parameters, arguments) \
	void VibeRant_##name parameters \
	{ \
		if (!Queued()) \
			return impl::VibeRant_##name arguments; \
		Enqueue([=] { impl::VibeRant_##name arguments; }); \
	}

QUEUE_DRIVER(SetBlend, (FBITFIELD flags), (flags))
QUEUE_DRIVER(GClipRect, (INT32 x0, INT32 y0, INT32 x1, INT32 y1, float nearclip), (x0, y0, x1, y1, nearclip))
QUEUE_DRIVER(UnSetShader, (), ())
QUEUE_DRIVER(SetShaderInfo, (hwdshaderinfo_t info, INT32 value), (info, value))
QUEUE_DRIVER(MakeScreenTexture, (), ())
QUEUE_DRIVER(MakeScreenFinalTexture, (), ())
QUEUE_DRIVER(DrawScreenFinalTexture, (int w, int h), (w, h))
QUEUE_DRIVER(SetPortalStencil, (INT32 level), (level))
QUEUE_DRIVER(ResetPortalDepth, (INT32 level), (level))
QUEUE_DRIVER(FillVoid, (), ())
QUEUE_DRIVER(UseSpringMask, (boolean on, INT32 level), (on, level))
#undef QUEUE_DRIVER

namespace
{

struct alignas(kAlignment) PolygonCommand
{
	Optional<FSurfaceInfo> surface;
	FUINT count;
	FBITFIELD flags;
	bool reduceEffects;

	void operator()(std::byte *data)
	{
		impl::VibeRant_DrawPolygon(surface.Get(), reinterpret_cast<FOutVector *>(data), count, flags, reduceEffects);
	}
};

struct alignas(kAlignment) Indexed
{
	Optional<FSurfaceInfo> surface;
	FUINT count;
	FBITFIELD flags;
	UINT32 vertexCount;
	bool reduceEffects;

	void operator()(std::byte *data)
	{
		auto *vertices = reinterpret_cast<FOutVector *>(data);
		auto *indices = reinterpret_cast<UINT32 *>(data + vertexCount * sizeof(FOutVector));
		impl::VibeRant_DrawIndexedTriangles(surface.Get(), vertices, count, flags, indices, vertexCount, reduceEffects);
	}
};

struct alignas(kAlignment) Sky
{
	gl_sky_t sky;

	void operator()(std::byte *data)
	{
		sky.loops = reinterpret_cast<gl_skyloopdef_t *>(data);
		impl::VibeRant_RenderSkyDome(&sky);
	}
};

} // namespace

void VibeRant_DrawPolygon(FSurfaceInfo *surface, FOutVector *vertices, FUINT count, FBITFIELD flags)
{
	const bool reduce = cv_reducevfx.value != 0;
	if (!Queued())
		return impl::VibeRant_DrawPolygon(surface, vertices, count, flags, reduce);

	const UINT64 bytes = UINT64(count) * sizeof(*vertices);
	if (!Fits<PolygonCommand>(bytes))
	{
		Sync(SyncReason::LargeCommand);
		return impl::VibeRant_DrawPolygon(surface, vertices, count, flags, reduce);
	}
	Enqueue(PolygonCommand{Optional(surface), count, flags, reduce}, static_cast<size_t>(bytes), RecordKind::Draw,
		[=](std::byte *data) { CopyArray(data, vertices, count); });
}

void VibeRant_DrawIndexedTriangles(FSurfaceInfo *surface, FOutVector *vertices, FUINT count, FBITFIELD flags, UINT32 *indices)
{
	const bool reduce = cv_reducevfx.value != 0;
	if (!Queued())
		return impl::VibeRant_DrawIndexedTriangles(surface, vertices, count, flags, indices, 0, reduce);
	if (!indices || !count)
	{
		// Even an empty batch updates the driver's statistics.
		Enqueue([=] { impl::VibeRant_DrawIndexedTriangles(nullptr, nullptr, 0, flags, nullptr, 0, reduce); });
		return;
	}

	UINT32 vertexCount = 0;
	for (FUINT i = 0; i < count; ++i)
		vertexCount = std::max<UINT32>(vertexCount, indices[i] + 1);
	const UINT64 bytes = UINT64(vertexCount) * sizeof(*vertices) + UINT64(count) * sizeof(*indices);
	if (!Fits<Indexed>(bytes))
	{
		Sync(SyncReason::LargeCommand);
		return impl::VibeRant_DrawIndexedTriangles(surface, vertices, count, flags, indices, vertexCount, reduce);
	}
	Enqueue(Indexed{Optional(surface), count, flags, vertexCount, reduce}, static_cast<size_t>(bytes), RecordKind::Draw,
		[=](std::byte *data) { CopyArray(CopyArray(data, vertices, vertexCount), indices, count); });
}

void VibeRant_Draw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t color)
{
	if (!Queued())
		return impl::VibeRant_Draw2DLine(v1, v2, color);
	Enqueue([a = *v1, b = *v2, color]() mutable { impl::VibeRant_Draw2DLine(&a, &b, color); });
}

void VibeRant_ClearBuffer(FBOOLEAN color, FBOOLEAN depth, FRGBAFloat *value)
{
	if (!Queued())
		return impl::VibeRant_ClearBuffer(color, depth, value);
	Enqueue([=, copy = Optional(value)]() mutable { impl::VibeRant_ClearBuffer(color, depth, copy.Get()); });
}

void VibeRant_SetTransform(FTransform *transform)
{
	if (!Queued())
		return impl::VibeRant_SetTransform(transform);
	Enqueue([copy = Optional(transform)]() mutable { impl::VibeRant_SetTransform(copy.Get()); });
}

void VibeRant_DrawModel(model_t *model, INT32 frame, float duration, float tics, INT32 nextFrame, FTransform *pos,
	float hscale, float vscale, UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surface)
{
	if (!Queued())
		return impl::VibeRant_DrawModel(model, frame, duration, tics, nextFrame, pos, hscale, vscale, flipped, hflipped, surface);
	Enqueue([=, transform = Optional(pos), info = Optional(surface)]() mutable {
		impl::VibeRant_DrawModel(model, frame, duration, tics, nextFrame, transform.Get(), hscale, vscale, flipped, hflipped, info.Get());
	});
}

void VibeRant_SetShader(int type)
{
	const bool reduce = cv_reducevfx.value != 0;
	if (!Queued())
		return impl::VibeRant_SetShader(type, reduce);
	Enqueue([=] { impl::VibeRant_SetShader(type, reduce); });
}

void VibeRant_SetScreenPalette(RGBA_t *palette)
{
	if (!Queued())
		return impl::VibeRant_SetScreenPalette(palette);
	std::array<RGBA_t, 256> copy{};
	if (palette)
		std::copy_n(palette, copy.size(), copy.begin());
	Enqueue([copy, present = palette != nullptr]() mutable {
		impl::VibeRant_SetScreenPalette(present ? copy.data() : nullptr);
	});
}

void VibeRant_PostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2])
{
	(void)points; // Sampling uses the pixel shader, so the grid is unused.
	const float time = static_cast<float>(leveltime);
	const float type = static_cast<float>(postimgtype[R_GetViewNumber()]);
	if (!Queued())
		return impl::VibeRant_PostImgRedraw(time, type);
	Enqueue([=] { impl::VibeRant_PostImgRedraw(time, type); });
}

void VibeRant_SetTexture(GLMipmap_t *texture)
{
	if (!Queued())
		return impl::VibeRant_SetTexture(texture);
	if (texture && !texture->downloaded)
	{
		Sync(SyncReason::TextureUpload);
		return impl::VibeRant_SetTexture(texture);
	}
	const UINT32 name = texture ? texture->downloaded : 0;
	const bool brightmap = texture && (texture->flags & TF_BRIGHTMAP);
	Enqueue([=] { impl::BindTextureName(name, brightmap); });
}

void VibeRant_SetSpecialState(hwdspecialstate_t state, INT32 value)
{
	const bool threaded = Queued();
	switch (state)
	{
	case HWD_SET_WATER_SURFACE:
	case HWD_SET_MODEL_LIGHTING:
	case HWD_SET_PALETTE_RENDERING:
		if (threaded)
		{
			Enqueue([=] { impl::VibeRant_SetSpecialState(state, value); });
			return;
		}
		break;
	case HWD_SET_SHADERS:
		if (threaded && sentShaders && shaderOption == value)
			return;
		Sync(SyncReason::Shader);
		sentShaders = true;
		shaderOption = value;
		break;
	default:
		Sync(SyncReason::State);
		break;
	}
	impl::VibeRant_SetSpecialState(state, value);
}

void VibeRant_RenderSkyDome(gl_sky_t *sky)
{
	if (!Queued() || sky->rebuild)
	{
		Sync(SyncReason::Sky);
		return impl::VibeRant_RenderSkyDome(sky);
	}
	const size_t count = static_cast<size_t>(std::max(0, sky->loopcount));
	const UINT64 bytes = UINT64(count) * sizeof(*sky->loops);
	if (!Fits<Sky>(bytes))
	{
		Sync(SyncReason::LargeCommand);
		return impl::VibeRant_RenderSkyDome(sky);
	}
	Sky cmd{*sky};
	cmd.sky.loops = nullptr;
	cmd.sky.data = nullptr;
	Enqueue(cmd, static_cast<size_t>(bytes), RecordKind::Draw, [=](std::byte *data) { CopyArray(data, sky->loops, count); });
}

void VibeRant_SetPortalPlane(const float *plane)
{
	if (!Queued())
		return impl::VibeRant_SetPortalPlane(plane);
	std::array<float, 4> copy{};
	if (plane)
		std::copy_n(plane, copy.size(), copy.begin());
	Enqueue([copy, present = plane != nullptr] { impl::VibeRant_SetPortalPlane(present ? copy.data() : nullptr); });
}

boolean VibeRant_PortalsSupported()
{
	if (Queued() && portalSupport >= 0)
		return portalSupport != 0;
	Sync(SyncReason::Portal);
	const boolean supported = impl::VibeRant_PortalsSupported();
	portalSupport = impl::CachedPortalSupport();
	return supported;
}

boolean VibeRant_DrawPortalAperture(const FOutVector *vertices, INT32 level, boolean restore)
{
	if (!Queued())
		return impl::VibeRant_DrawPortalAperture(vertices, level, restore);
	const std::array<FOutVector, 4> copy{vertices[0], vertices[1], vertices[2], vertices[3]};
	Enqueue([=] { impl::VibeRant_DrawPortalAperture(copy.data(), level, restore); });
	return true;
}

boolean VibeRant_DrawSpringMask(const FOutVector *vertices, INT32 count, boolean set)
{
	if (!Queued())
		return impl::VibeRant_DrawSpringMask(vertices, count, set);
	std::array<FOutVector, 32> copy{};
	if (count < 3 || static_cast<size_t>(count) > copy.size())
		return false;
	std::copy_n(vertices, count, copy.begin());
	Enqueue([=] { impl::VibeRant_DrawSpringMask(copy.data(), count, set); });
	return true;
}

boolean VibeRant_Surface(INT32 w, INT32 h)
{
	Sync(SyncReason::Window);
	// HWR_Startup can reenter the public driver table.
	surfaceStartup = true;
	const boolean ok = impl::VibeRant_Surface(w, h);
	surfaceStartup = false;
	return ok;
}

boolean VibeRant_BeginSurfaceProbe()
{
	CheckFatal();
	if (!impl::SurfaceProbePending())
		return false;
	Sync(SyncReason::Probe);
	probeActive = impl::VibeRant_BeginSurfaceProbe();
	return probeActive;
}

void VibeRant_EndSurfaceProbe()
{
	CheckFatal();
	if (!probeActive)
		return;
	Sync(SyncReason::Probe);
	impl::VibeRant_EndSurfaceProbe();
	probeActive = false;
}

void VibeRant_SurfaceProbeSource(const char *kind, INT32 source)
{
	CheckFatal();
	if (probeActive)
		impl::VibeRant_SurfaceProbeSource(kind, source);
}

void VibeRant_SurfaceProbeTexture(const char *name)
{
	CheckFatal();
	if (probeActive)
		impl::VibeRant_SurfaceProbeTexture(name);
}

boolean VibeRant_CanRollSprite()
{
	CheckFatal();
	// All writes to shader options, shader validity, and filter settings are synchronous.
	return impl::VibeRant_CanRollSprite();
}

boolean VibeRant_TakeTextureFlushRequest()
{
	CheckFatal();
	if (!impl::TextureFlushPending())
		return false;
	Sync(SyncReason::TextureFlush);
	return impl::VibeRant_TakeTextureFlushRequest();
}

void VibeRant_SyncModelUpdates()
{
	Sync(SyncReason::Model);
}

void VibeRant_FinishUpdate(INT32 waitvbl)
{
	CheckFatal();
	const INT32 effect = cv_scr_effect.value;
#ifdef HAVE_VIBERANT_D3D
	const bool enabled = testMode >= 0 ? testMode != 0 : cv_grdriverthread.value != 0;
	if (queue && !enabled)
		StopQueue(false);
	if (!queue && enabled && !Core_Vulkan() && !Core_Metal() && Core_Device() && !abandoned)
		StartQueue();
	if (Queued())
	{
		if (!queue->Done(queue->previousFrame))
		{
			const auto start = Clock::now();
			queue->WaitFor(queue->previousFrame);
			const auto waited = Clock::now() - start;
			stats.frameWait += waited;
			stats.worstFrameWait = std::max(stats.worstFrameWait, waited);
			++stats.frameWaits;
			CheckFatal();
		}
		PresentFrame frame;
		frame.mainThread = impl::PrepareFrame(frame, effect);
		if (!frame.mainThread)
		{
			const UINT64 inFlight = queue->Done(queue->previousFrame) ? 1 : 2;
			peakPresents = std::max(peakPresents, inFlight);
			Enqueue([=] { impl::VibeRant_FinishUpdate(waitvbl, effect, &frame); }, RecordKind::Finish);
			queue->previousFrame = queue->tail;
		}
		else
		{
			Sync(SyncReason::Frame);
			impl::VibeRant_FinishUpdate(waitvbl, effect, &frame);
		}
		++stats.frames;
		stats.bytes += queue->frameBytes;
		stats.peakBytes = std::max(stats.peakBytes, queue->frameBytes);
		queue->frameBytes = 0;
		return;
	}
#endif
	impl::VibeRant_FinishUpdate(waitvbl, effect, nullptr);
}

void VibeRant_LogPerf(double seconds)
{
	const bool threaded = Queued();
	if (!queue && !Core_Device())
		return;

	const UINT32 shader = hwr_roll_shader, cpu = hwr_roll_cpu, geometry = hwr_roll_geometry;
	hwr_roll_shader = hwr_roll_cpu = hwr_roll_geometry = 0;
#ifdef _UWP
	const UINT32 lazy = HWR_UWPTakeLazyModelLoads();
#else
	const UINT32 lazy = 0;
#endif
	const INT32 effect = cv_scr_effect.value;
	const auto sample = stats;
	stats = {};
	auto log = [=] {
		impl::VibeRant_LogPerf(seconds, effect, shader, cpu, geometry, lazy);
		LogQueuePerf(seconds, sample);
	};
	if (threaded)
		Enqueue(log, RecordKind::Perf);
	else
		log();
}

void VibeRant_ReleaseWindow()
{
	if (StopQueue(false))
		impl::VibeRant_ReleaseWindow();
}

void VibeRant_Shutdown()
{
	if (!abandoned && StopQueue(true))
		impl::VibeRant_Shutdown();
	portalSupport = -1;
	sentShaders = false;
}

void VibeRant_SetDriverThreadForTest(INT32 enabled, size_t bytes)
{
	Sync(SyncReason::Test);
	testMode = enabled;
	testCapacity = bytes ? std::clamp(bytes & ~(kAlignment - 1), size_t(4096), kQueueBytes) : 0;
}

void VibeRant_GetDriverThreadCountsForTest(UINT64 *readbacks, UINT64 *syncs, UINT64 *wraps, UINT64 *peak)
{
	Sync(SyncReason::Test);
	if (readbacks) *readbacks = totalReadbacks;
	if (syncs) *syncs = totalSyncs;
	if (wraps) *wraps = totalWraps;
	if (peak) *peak = peakPresents;
}

namespace
{

#define DRIVERFUNCTION(name) {#name, reinterpret_cast<void *>(&VibeRant_##name)}
const struct
{
	const char *name;
	void *function;
} kDriverFunctions[] =
{
	DRIVERFUNCTION(Init),
	DRIVERFUNCTION(SetPalette),
	DRIVERFUNCTION(Draw2DLine),
	DRIVERFUNCTION(DrawPolygon),
	DRIVERFUNCTION(DrawIndexedTriangles),
	DRIVERFUNCTION(RenderSkyDome),
	DRIVERFUNCTION(SetBlend),
	DRIVERFUNCTION(ClearBuffer),
	DRIVERFUNCTION(SetTexture),
	DRIVERFUNCTION(UpdateTexture),
	DRIVERFUNCTION(DeleteTexture),
	DRIVERFUNCTION(ReadRect),
	DRIVERFUNCTION(GClipRect),
	DRIVERFUNCTION(ClearMipMapCache),
	DRIVERFUNCTION(SetSpecialState),
	DRIVERFUNCTION(DrawModel),
	DRIVERFUNCTION(CreateModelVBOs),
	DRIVERFUNCTION(SetTransform),
	DRIVERFUNCTION(GetTextureUsed),
	DRIVERFUNCTION(PostImgRedraw),
	DRIVERFUNCTION(FlushScreenTextures),
	DRIVERFUNCTION(StartScreenWipe),
	DRIVERFUNCTION(EndScreenWipe),
	DRIVERFUNCTION(DoScreenWipe),
	DRIVERFUNCTION(DrawIntermissionBG),
	DRIVERFUNCTION(MakeScreenTexture),
	DRIVERFUNCTION(MakeScreenFinalTexture),
	DRIVERFUNCTION(DrawScreenFinalTexture),
	DRIVERFUNCTION(CompileShaders),
	DRIVERFUNCTION(CleanShaders),
	DRIVERFUNCTION(SetShader),
	DRIVERFUNCTION(UnSetShader),
	DRIVERFUNCTION(SetShaderInfo),
	DRIVERFUNCTION(LoadCustomShader),
	DRIVERFUNCTION(SetScreenPalette),
	DRIVERFUNCTION(SetPaletteLookup),
	DRIVERFUNCTION(CreateLightTable),
	DRIVERFUNCTION(UpdateLightTable),
	DRIVERFUNCTION(ClearLightTables),
};
#undef DRIVERFUNCTION

} // namespace

void *VibeRant_GetDriverFunction(const char *name)
{
	CheckFatal();
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		I_OutputMsg("VibeRant backend: %s\n", Core_Metal() ? "Metal" : Core_Vulkan() ? "Vulkan" : "D3D12");
	}

	for (const auto &function : kDriverFunctions)
	{
		if (strcmp(function.name, name) == 0)
			return function.function;
	}
	rr_viberant::Core_Log("no driver function %s", name);
	return nullptr;
}
