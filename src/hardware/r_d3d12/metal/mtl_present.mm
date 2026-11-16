// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl_present.mm
/// \brief Shows the VibeRant driver's frames with a CAMetalLayer on an SDL window of its own
///
/// The implementation of d3d12_present.h for the Metal backend, built like the Vulkan one (vulkan/vk_present.cpp). The
/// game's window is made for OpenGL, which the Software renderer's frames are shown with. So when the driver starts,
/// the presenter makes a second window like the first for Metal, hides the first, and makes the second the game's
/// window (sdlmain.h's `window`), which is what the video code sizes, makes fullscreen, and takes input from. When
/// Software takes over, the first window comes back as the second one was and the second goes.
///
/// The layer's drawables are always the size of the window: in points, as the game's window isn't a high-resolution
/// one, so on a Retina display the system scales the frame up as it does the other renderers' frames. The driver's
/// placement (the one the other desktop presenters use) draws each frame into the drawable: at Screen Scaling's Integer
/// or 1:1 rectangle, or scaled with linear filtering to the largest rectangle of the frame's aspect ratio. A drawable
/// is BGRA8 and only a render target, which a draw suits and a copy wouldn't.
///
/// A window that can't be seen (minimized, hidden, on another Space, or covered) isn't presented to as one in sight
/// is: a layer's drawables only come back from the display, so asking a layer nobody sees for the next one waits a
/// second and gives nothing. In case the system calls a window out of sight that isn't, such a window still gets a
/// frame whenever the display has taken the one before (mtl_present_seen.h).
///
/// Objective-C++, compiled with ARC. The game's headers can't be included here (mtl_present_game.h).

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "SDL.h"
#include "SDL_metal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

// Nothing of Apple's after this: it brings Direct3D's declarations, with Windows' BOOL under another name
#include "mtl12_internal.h"
#include "../d3d12_present.h"
#include "mtl_present_check.h"
#include "mtl_present_game.h"
#include "mtl_present_seen.h"

namespace rr_d3d12
{
namespace
{

#define Log Core_Log
#define LOG_ONCE D3D12_LOG_ONCE

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The drawable's view slot: after the ones the other presenters and the tests use
constexpr UINT kSlot = kRtvPlaceTest + 1;

struct PresentStats
{
	double acquireMs = 0, worstAcquireMs = 0;
	double drawMs = 0;
	int presented = 0;
	int unseen = 0; // frames not presented because the window was out of sight
	int noDrawable = 0; // frames the layer gave no drawable for
	int probes = 0; // of those presented: to a window the system called out of sight
};
PresentStats g_stats;
// Since the window was taken, for the log when it is given back
PresentStats g_totals;

int g_tracedFrames = 0;
constexpr int kTraceFrames = 3;
INT32 g_traceSlot = -1;

struct Presenter
{
	bool stopped = false; // shut down, so nothing presents
	bool owned = false; // the Metal window and its layer exist, and the Metal window is the game's window
	SDL_Window *metalWindow = nullptr;
	SDL_Window *gameWindow = nullptr; // the OpenGL window, hidden while this presents
	SDL_MetalView view = nullptr;
	CAMetalLayer *layer = nil;
	bool vsync = true;
	UINT width = 0, height = 0; // of the last drawable
	bool hideGameWindow = false; // once the first frame is on the Metal window, so that there is always a window to see
	bool seen = true; // whether the last frame found the window in sight
	rr_mtl_present::Seen decide; // what to do with a frame for a window that may be out of sight (mtl_present_seen.h)
	char failure[160] = "";
};
Presenter g_mtl;

// What Metal says became of the drawables presented, which it says on a thread of its own. Every presented drawable
// has a number, and a drawable Metal has reported on (shown, or let go) settles the ones before it too.
std::atomic<uint64_t> g_presentedSerial{0}, g_reportedSerial{0};
// What the reports said, since the window was taken: the drawable has a time at which it was shown, or it has none
// (dropped, or a system that gives no times). Of all presented drawables, and of the probes among them.
std::atomic<int> g_timed{0}, g_untimed{0}, g_probesTimed{0}, g_probesUntimed{0};

void Reported(uint64_t serial)
{
	uint64_t known = g_reportedSerial.load();
	while (known < serial && !g_reportedSerial.compare_exchange_weak(known, serial))
	{
	}
}

const Clock::time_point g_start = Clock::now();

// -mtlcheck: some drawables are read back with the frame they were given, and compared (mtl_present_check.h). For
// tests: a drawable that can be read isn't quite the one a player gets (the layer's framebufferOnly is off).
struct DrawableCheck
{
	bool on = false;
	ComPtr<ID3D12Resource> frame, shown; // readback buffers
	UINT64 frameBytes = 0, shownBytes = 0;
	int made = 0, differed = 0, missed = 0; // since the window was taken
};
DrawableCheck g_check;
// A window's first three frames, and every sixtieth after
constexpr int kCheckFirst = 3, kCheckEvery = 60;

UINT CheckPitch(UINT64 width)
{
	return (UINT)((width * 4 + 255) & ~UINT64(255));
}

bool CheckBuffer(ComPtr<ID3D12Resource> &buffer, UINT64 &has, UINT64 bytes)
{
	if (buffer && has >= bytes)
		return true;
	Core_Retire(buffer);
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	has = 0;
	if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&buffer)), "CreateCommittedResource (drawable check)"))
		return false;
	has = bytes;
	return true;
}

// Copies all of a texture, which is in state and stays in it, into a readback buffer
void CopyForCheck(ID3D12Resource *texture, D3D12_RESOURCE_STATES state, ID3D12Resource *buffer)
{
	const D3D12_RESOURCE_DESC desc = texture->GetDesc();
	D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
	source.pResource = texture;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.pResource = buffer;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint = {desc.Format, (UINT)desc.Width, desc.Height, 1, CheckPitch(desc.Width)};
	Core_Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Core_FlushBarriers();
	Core_List()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	Core_Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
	Core_FlushBarriers();
}

// Once the copies are done: is the frame in the drawable where it was placed?
void CompareForCheck(UINT64 fence, const D3D12_RESOURCE_DESC &frameDesc, UINT width, UINT height, const D3D12_RESOURCE_DESC &shownDesc,
	const screen_scaling::Placement &placement)
{
	const int number = g_check.made + g_check.missed + 1;
	void *frameData = nullptr, *shownData = nullptr;
	if (!Core_Wait(fence, 2000, "the drawable check") || FAILED(g_check.frame->Map(0, nullptr, &frameData)))
	{
		g_check.missed++;
		Log("presenter: drawable check %d wasn't made: the copies didn't come back", number);
		return;
	}
	if (FAILED(g_check.shown->Map(0, nullptr, &shownData)))
	{
		g_check.frame->Unmap(0, nullptr);
		g_check.missed++;
		Log("presenter: drawable check %d wasn't made: the copies didn't come back", number);
		return;
	}
	const rr_mtl_present::Pixels frame = {static_cast<const uint8_t *>(frameData), (int)std::min<UINT64>(width, frameDesc.Width),
		(int)std::min(height, frameDesc.Height), (int)CheckPitch(frameDesc.Width), frameDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM};
	const rr_mtl_present::Pixels drawable = {static_cast<const uint8_t *>(shownData), (int)shownDesc.Width, (int)shownDesc.Height,
		(int)CheckPitch(shownDesc.Width), shownDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM};
	const rr_mtl_present::Rect placed = {placement.x, placement.y, placement.w, placement.h};
	const rr_mtl_present::Shown shown = rr_mtl_present::CompareShown(frame, drawable, placed);
	g_check.shown->Unmap(0, nullptr);
	g_check.frame->Unmap(0, nullptr);
	g_check.made++;
	g_check.differed += !shown.matches;
	char pixels[96] = "scaled";
	if (shown.unscaled)
		snprintf(pixels, sizeof pixels, "at its own size, %ld of %ld pixels differ", shown.pixelsOff, shown.pixels);
	Log("presenter: drawable check %d%s: the %dx%d frame %s the %dx%d drawable at %d,%d as %dx%d (%s; a block's mean color at most %.2f levels off; %ld pixels outside aren't black)",
		number, shown.matches ? "" : " FAILED", frame.width, frame.height, shown.matches ? "is in" : "is NOT what is in", drawable.width, drawable.height,
		placed.x, placed.y, placed.w, placed.h, pixels, shown.worstBlock, shown.outside);
}

void SubmitWithoutPresenting()
{
	Core_Submit("present");
}

void Fail(const char *what)
{
	snprintf(g_mtl.failure, sizeof g_mtl.failure, "%s", what);
	Log("presenter: %s", what);
}

// Whether anything of the window is on screen. AppKit says so for the window the layer's view is in.
bool WindowSeen()
{
	NSView *view = (__bridge NSView *)g_mtl.view;
	NSWindow *cocoa = view.window;
	if (!cocoa)
		return true; // nothing to ask: let the layer decide
	return (cocoa.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

// Sees to it that the layer has its window's color space, so that the system treats the frame's colors as it treats
// what OpenGL draws in the game's other window, which shows the other renderers' frames. SDL gives every window it
// makes the sRGB color space (since SDL 2.26: "By default, OpenGL was getting a much more saturated colorspace", its
// commit says), and the system matches an OpenGL surface in such a window from sRGB to the display. A CAMetalLayer
// without a color space "isn't color-matched" (Apple's documentation of the property): its values go to the display
// as they are, which on a display of a wider gamut than sRGB (a MacBook's P3 panel) would show every color more
// saturated than the same frame from Software. On the Macs this ran on, the system had already given the layer of
// SDL's view the window's color space by the time this looks, and the log says so; this sets it where it hasn't.
// The Metal window is made by the same SDL as the OpenGL one, so its color space is that window's, whatever SDL's
// version: with an SDL that sets none, neither window's content is matched.
// Nobody has compared the two renderers on a wide-gamut panel by eye or with a meter: this is the documentation's
// word and the log's.
void MatchWindowColors(bool say)
{
	CAMetalLayer *layer = g_mtl.layer;
	NSWindow *cocoa = ((__bridge NSView *)g_mtl.view).window;
	if (!layer || !cocoa)
		return;
	NSColorSpace *space = cocoa.colorSpace;
	CGColorSpaceRef wanted = space ? space.CGColorSpace : NULL;
	CGColorSpaceRef has = layer.colorspace;
	const bool had = has == wanted || (has && wanted && CFEqual(has, wanted));
	if (!had)
		layer.colorspace = wanted;
	if (had && !say)
		return;
	NSColorSpace *display = cocoa.screen ? cocoa.screen.colorSpace : nil;
	const char *displayName = display.localizedName ? display.localizedName.UTF8String : "unknown";
	if (layer.colorspace)
		Log("presenter: colors are matched to the display as the OpenGL window's are: the layer has its window's color space, \"%s\" (%s), and the display's is \"%s\"",
			space.localizedName ? space.localizedName.UTF8String : "unnamed", had ? "the system gave it" : "set here", displayName);
	else
		Log("presenter: the window has no color space to give the layer, so the frame's colors reach the display (\"%s\") as they are",
			displayName);
}

void DestroyLayer()
{
	g_mtl.layer = nil;
	if (g_mtl.view)
		SDL_Metal_DestroyView(g_mtl.view);
	g_mtl.view = nullptr;
	g_mtl.width = g_mtl.height = 0;
}

// Makes the Metal window as the game's window is, and the layer on it; then the game's window is hidden and the Metal
// one stands in for it
bool CreateWindowAndLayer()
{
	if (g_mtl.owned)
		return true;
	SDL_Window *game = rr_mtl_present::GameWindow();
	id<MTLDevice> device = (__bridge id<MTLDevice>)rr_mtl12::Device();
	if (!game || !device)
	{
		Fail("there is no window or no device");
		return false;
	}

	@autoreleasepool
	{
		const Uint32 flags = SDL_GetWindowFlags(game);
		int x, y, w, h;
		SDL_GetWindowPosition(game, &x, &y);
		SDL_GetWindowSize(game, &w, &h);
		const Uint32 wanted = SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN
			| (flags & (SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI));
		SDL_Window *made = SDL_CreateWindow(SDL_GetWindowTitle(game), x, y, w, h, wanted);
		if (!made)
		{
			Fail(SDL_GetError());
			return false;
		}
		SDL_MetalView view = SDL_Metal_CreateView(made);
		CAMetalLayer *layer = view ? (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view) : nil;
		if (!layer)
		{
			Fail(view ? "the window's view has no Metal layer" : SDL_GetError());
			if (view)
				SDL_Metal_DestroyView(view);
			SDL_DestroyWindow(made);
			return false;
		}
		// The frame's values as they are, BGRA8 without an sRGB conversion; what the display makes of them is the color
		// space's business (MatchWindowColors)
		layer.device = device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		// Only a render target, unless a test wants to read it back (-mtlcheck)
		g_check.on = rr_mtl_present::CheckDrawables();
		g_check.made = g_check.differed = g_check.missed = 0;
		layer.framebufferOnly = g_check.on ? NO : YES;
		layer.opaque = YES;
		layer.displaySyncEnabled = g_mtl.vsync;
		// What the layer has by default, and what deciding whether a drawable is free counts on (mtl_present_seen.h)
		layer.maximumDrawableCount = rr_mtl_present::kDrawables;

		g_mtl.metalWindow = made;
		g_mtl.view = view;
		g_mtl.layer = layer;
		rr_mtl_present::SetWindowIcon(made);
		SDL_ShowWindow(made);
		MatchWindowColors(true);

		g_mtl.gameWindow = game;
		g_mtl.hideGameWindow = true;
		rr_mtl_present::SetGameWindow(made);
		SDL_RaiseWindow(made);
		g_mtl.owned = true;
		g_mtl.seen = true;
		g_mtl.decide = rr_mtl_present::Seen();
		// No drawable of this layer is out, whatever became of another window's
		Reported(g_presentedSerial.load());
		g_timed = g_untimed = g_probesTimed = g_probesUntimed = 0;
		g_mtl.failure[0] = '\0';
		g_totals = PresentStats();
		Log("presenter: presenting on a Metal window in place of the game's OpenGL window (%s)",
			g_mtl.vsync ? "waits for the display" : "doesn't wait for the display");
	}
	return true;
}

// The game's own window back, as the Metal one was. quitting: the game is ending, so its window isn't shown again:
// nothing would draw into it before it is destroyed, and a fullscreen one would begin to move into a Space of its own.
void GiveBackWindow(bool idle, bool quitting)
{
	if (!g_mtl.owned)
		return;
	if (!idle)
		idle = Core_WaitIdle(2000, "giving the window back");
	(void)idle; // the layer has to go with its window either way; Metal keeps what its command buffers still use
	char what[160] = "gave the game's OpenGL window back";
	@autoreleasepool
	{
		Core_Device()->CreateRenderTargetView(nullptr, nullptr, Core_Rtv(kSlot));
		DestroyLayer();
		Core_Retire(g_check.frame);
		Core_Retire(g_check.shown);

		SDL_Window *metal = g_mtl.metalWindow;
		SDL_Window *game = g_mtl.gameWindow;
		if (game && quitting)
			rr_mtl_present::SetGameWindow(game);
		else if (game)
		{
			const Uint32 flags = SDL_GetWindowFlags(metal);
			int x, y, w, h;
			SDL_GetWindowPosition(metal, &x, &y);
			SDL_GetWindowSize(metal, &w, &h);
			if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
				SDL_SetWindowFullscreen(game, SDL_WINDOW_FULLSCREEN_DESKTOP);
			else
			{
				SDL_SetWindowFullscreen(game, 0);
				SDL_SetWindowSize(game, w, h);
				SDL_SetWindowPosition(game, x, y);
			}
			rr_mtl_present::SetGameWindow(game);
			SDL_ShowWindow(game);
			SDL_RaiseWindow(game);
		}
		if (metal)
			SDL_DestroyWindow(metal);
		g_mtl.metalWindow = g_mtl.gameWindow = nullptr;
		g_mtl.owned = false;
		// What the log says of the OpenGL window is what SDL says of it now, not what was meant
		if (quitting)
			snprintf(what, sizeof what, "closed the Metal window as the game quits; the game's OpenGL window is %s",
				!game ? "gone" : (SDL_GetWindowFlags(game) & SDL_WINDOW_SHOWN) ? "shown" : "hidden");
	}
	if (g_check.on)
		Log("presenter: %d drawables were read back and compared with the frames placed in them (-mtlcheck): %d differed, %d unread",
			g_check.made + g_check.missed, g_check.differed, g_check.missed);
	Log("presenter: %s; %d frames presented, %d skipped while the window was out of sight, %d without a drawable; %d of those presented went to a window the system called out of sight (%d shown, %d dropped); the layer took %.2f ms to give a drawable on average, %.1f ms at worst; Metal reported %d drawables with a time shown and %d without",
		what, g_totals.presented, g_totals.unseen, g_totals.noDrawable, g_totals.probes, g_probesTimed.load(), g_probesUntimed.load(),
		g_totals.acquireMs / std::max(1, g_totals.presented + g_totals.noDrawable), g_totals.worstAcquireMs, g_timed.load(), g_untimed.load());
}

} // namespace

bool Present_TakeWindow()
{
	if (g_mtl.owned)
		return true;
	// The driver starts with the video system, before the game's window is made: Present_Frame takes it with the first frame
	if (!rr_mtl_present::GameWindow())
	{
		Log("the game's window doesn't exist yet, so it's taken with the first frame");
		return true;
	}
	return CreateWindowAndLayer();
}

void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync, PlaceFrameFn place)
{
	g_traceSlot = -1;
	const bool trace = Present_Tracing();

	if (g_mtl.stopped || !source || !width || !height)
	{
		SubmitWithoutPresenting();
		return;
	}
	if (!g_mtl.owned && !CreateWindowAndLayer())
	{
		char message[360];
		snprintf(message, sizeof message,
			"Metal couldn't make its window to present on (%s).\nStart the game with -software, or set Renderer to Software in Video Options > Advanced.", g_mtl.failure);
		rr_mtl_present::Fatal(message);
	}

	@autoreleasepool
	{
		// SDL's view keeps the layer's drawables at the size of the window's content, in points (the window isn't a
		// high-resolution one), and this reads the layer's size back
		int drawableWidth = 0, drawableHeight = 0;
		SDL_Metal_GetDrawableSize(g_mtl.metalWindow, &drawableWidth, &drawableHeight);
		const Uint32 flags = SDL_GetWindowFlags(g_mtl.metalWindow);
		// Whether to present this frame (mtl_present_seen.h). A window the system doesn't call visible still gets a
		// frame when no drawable is out: the system may be wrong, and a window that was only just shown doesn't count as
		// visible until the system has put it on screen.
		static const bool force = rr_mtl_present::PresentUnseen();
		static const bool blind = rr_mtl_present::PresentBlind();
		bool reports = false; // Metal says what becomes of presented drawables (macOS 10.15.4)
		if (@available(macOS 10.15.4, *))
			reports = true;
		rr_mtl_present::Now now;
		now.ms = MsSince(g_start);
		now.usable = drawableWidth > 0 && drawableHeight > 0 && !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
		now.visible = WindowSeen() && !blind;
		now.force = force;
		// Without reports, only a window's first frame goes out regardless
		now.out = reports ? (int)std::min<uint64_t>(g_presentedSerial.load() - g_reportedSerial.load(), 1000) : g_totals.presented ? 1 : 0;
		const rr_mtl_present::Frame decided = rr_mtl_present::Decide(g_mtl.decide, now);
		const bool seen = now.usable && (now.visible || now.force);
		if (seen != g_mtl.seen)
		{
			g_mtl.seen = seen;
			Log(seen ? "presenter: the game window shows again, so frames are presented"
				: !now.usable ? "presenter: the game window is out of sight (minimized, hidden, or without a size), so nothing is presented until it shows"
				: "presenter: the system says none of the game window is on screen (covered, on another Space, or behind a locked screen), so a frame is only presented while a drawable is surely free");
		}
		if (decided == rr_mtl_present::Frame::Unseen)
		{
			g_stats.unseen++;
			g_totals.unseen++;
			SubmitWithoutPresenting();
			return;
		}
		// The layer gave nothing a moment ago, after a wait: don't hold every frame up for another
		if (decided == rr_mtl_present::Frame::Wait)
		{
			g_stats.noDrawable++;
			g_totals.noDrawable++;
			SubmitWithoutPresenting();
			return;
		}
		const bool probe = decided == rr_mtl_present::Frame::Probe;

		CAMetalLayer *layer = g_mtl.layer;
		if (g_mtl.vsync != vsync || layer.displaySyncEnabled != vsync)
		{
			layer.displaySyncEnabled = vsync;
			g_mtl.vsync = vsync;
			Log("presenter: %s", vsync ? "waits for the display" : "doesn't wait for the display");
		}
		const Clock::time_point acquireStart = Clock::now();
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		const double acquireMs = MsSince(acquireStart);
		g_stats.acquireMs += acquireMs;
		g_stats.worstAcquireMs = std::max(g_stats.worstAcquireMs, acquireMs);
		g_totals.acquireMs += acquireMs;
		g_totals.worstAcquireMs = std::max(g_totals.worstAcquireMs, acquireMs);
		id<MTLTexture> texture = drawable ? drawable.texture : nil;
		ComPtr<ID3D12Resource> back;
		if (!texture || FAILED(rr_mtl12::WrapTexture((__bridge void *)texture, &back)))
		{
			// No drawable came free in the layer's one-second wait (or its texture is of no format the driver draws
			// to): the frame isn't shown, and the layer is asked again after a pause that doubles up to four seconds,
			// or, if the window is out of sight, not until it is back
			g_stats.noDrawable++;
			g_totals.noDrawable++;
			rr_mtl_present::NoDrawable(g_mtl.decide, MsSince(g_start), decided);
			LOG_ONCE("presenter: the layer gave no drawable after %.0f ms, so the frame isn't shown", acquireMs);
			SubmitWithoutPresenting();
			return;
		}
		if (!rr_mtl_present::GotDrawable(g_mtl.decide, decided, MsSince(g_start), acquireMs))
			LOG_ONCE("presenter: the layer took %.0f ms to give a drawable for a window the system calls out of sight, so frames go to it less often for a while",
				acquireMs);
		if (g_mtl.width != (UINT)texture.width || g_mtl.height != (UINT)texture.height)
		{
			g_mtl.width = (UINT)texture.width;
			g_mtl.height = (UINT)texture.height;
			MatchWindowColors(false); // again, in case the window had none to give while it was off screen
			// What the system then does with them, which a log can say and only a look at the screen can show: on a
			// display with more pixels than points (a Retina display), it scales the layer up
			NSWindow *cocoa = ((__bridge NSView *)g_mtl.view).window;
			const double scale = cocoa ? cocoa.backingScaleFactor : 1.0;
			long refresh = 0;
			if (@available(macOS 12.0, *))
				refresh = cocoa.screen ? (long)cocoa.screen.maximumFramesPerSecond : 0;
			int points = 0, pointsHigh = 0;
			SDL_GetWindowSize(g_mtl.metalWindow, &points, &pointsHigh);
			Log("presenter: %ux%u drawables for a window of %dx%d points on a display with %.4g pixels per point (%s) and %ld frames per second",
				g_mtl.width, g_mtl.height, points, pointsHigh, scale,
				scale * points > g_mtl.width + 0.5 ? "the system scales the frame up" : "shown pixel for pixel", refresh);
		}

		// Integer and 1:1, where the frame fits the window; otherwise (and for Fit) the largest rectangle of its aspect ratio
		screen_scaling::Placement placement = screen_scaling::Compute((int)width, (int)height, (int)g_mtl.width, (int)g_mtl.height,
			rr_mtl_present::ScreenScalingMode());
		if (placement.fit)
			placement = screen_scaling::FitRect((int)width, (int)height, (int)g_mtl.width, (int)g_mtl.height);

		const Clock::time_point drawStart = Clock::now();
		ID3D12GraphicsCommandList *list = Core_List();
		bool placed = false, checking = false;
		D3D12_RESOURCE_DESC frameDesc = {}, shownDesc = {};
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kSlot);
		if (list)
		{
			// The drawable is cleared to black and the frame drawn in its place, which leaves source as it was
			back->SetName(L"drawable");
			Core_Device()->CreateRenderTargetView(back.Get(), nullptr, rtv);
			Core_Transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Core_FlushBarriers();
			placed = place && place(source, sourceState, back.Get(), rtv, placement, g_mtl.width, g_mtl.height);
			if (!placed)
			{
				const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
				list->ClearRenderTargetView(rtv, black, 0, nullptr);
				LOG_ONCE("couldn't place the frame in the drawable, so it shows black");
			}
			// -mtlcheck: copies of the frame and of the drawable it was placed in, to compare once they are done
			if (g_check.on && placed && (g_totals.presented < kCheckFirst || g_totals.presented % kCheckEvery == 0))
			{
				frameDesc = source->GetDesc();
				shownDesc = back->GetDesc();
				if (CheckBuffer(g_check.frame, g_check.frameBytes, (UINT64)CheckPitch(frameDesc.Width) * frameDesc.Height)
					&& CheckBuffer(g_check.shown, g_check.shownBytes, (UINT64)CheckPitch(shownDesc.Width) * shownDesc.Height))
				{
					CopyForCheck(source, sourceState, g_check.frame.Get());
					CopyForCheck(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, g_check.shown.Get());
					checking = true;
				}
			}
			Core_Transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			Core_FlushBarriers();
			// The view is read when a clear or a draw is recorded, and the wrapper must outlive the commands, not the frame
			Core_Device()->CreateRenderTargetView(nullptr, nullptr, rtv);
			Core_Retire(back);
			// The submission shows the drawable when its commands are done
			rr_mtl12::PresentWithNextSubmit((__bridge void *)drawable);
		}
		// Metal says, on a thread of its own, when the display took the drawable (or that it never did)
		const uint64_t serial = ++g_presentedSerial;
		if (@available(macOS 10.15.4, *))
		{
			[drawable addPresentedHandler:^(id<MTLDrawable> done) {
				const bool timed = done.presentedTime > 0;
				(timed ? g_timed : g_untimed)++;
				if (probe)
					(timed ? g_probesTimed : g_probesUntimed)++;
				Reported(serial);
			}];
		}
		const UINT64 fence = list ? Core_Submit("present") : 0;
		if (!list)
			SubmitWithoutPresenting(); // reopens the list
		g_stats.drawMs += MsSince(drawStart);
		if (!fence)
		{
			// Nothing was submitted, so the drawable goes back to the layer unshown
			rr_mtl12::PresentWithNextSubmit(nullptr);
			Reported(serial);
			return;
		}
		if (checking)
			CompareForCheck(fence, frameDesc, width, height, shownDesc, placement);
		g_stats.presented++;
		g_totals.presented++;
		g_stats.probes += probe;
		g_totals.probes += probe;
		g_traceSlot = 0;
		if (g_mtl.hideGameWindow)
		{
			g_mtl.hideGameWindow = false;
			if (g_mtl.gameWindow)
				SDL_HideWindow(g_mtl.gameWindow);
		}
		if (trace)
			Log("  placed in a %ux%u drawable at %d,%d %dx%d, submitted as fence %llu; acquired in %.2f ms",
				g_mtl.width, g_mtl.height, placement.x, placement.y, placement.w, placement.h, (unsigned long long)fence, acquireMs);
	}
}

void Present_ReleaseWindow()
{
	GiveBackWindow(false, false);
}

// The driver shuts down when the game ends (I_ShutdownGraphics), and the video system goes right after it
void Present_Shutdown(bool idle)
{
	GiveBackWindow(idle, true);
	g_mtl.stopped = true; // nothing presents after this
}

void Present_GetTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe)
{
	*copied = g_traceSlot;
	*shown = g_traceSlot;
	*copyframe = 0; // there is no frame id to report off the Xbox
	*showframe = 0;
}

void Present_LogPerf(double frames)
{
	(void)frames; // the times are per frame presented, which not every frame is
	const PresentStats &s = g_stats;
	const double acquired = s.presented > 0 ? s.presented : 1;
	char line[512];
	snprintf(line, sizeof line,
		"UWP perf:   D3D12: present: Metal layer (%ux%u, %s); %d frames presented, %d skipped while the window was out of sight, %d without a drawable; per presented frame acquire %.2f ms (worst %.2f), draw and submit %.2f ms\n",
		g_mtl.width, g_mtl.height, g_mtl.vsync ? "waits for the display" : "doesn't wait for the display",
		s.presented, s.unseen, s.noDrawable, s.acquireMs / acquired, s.worstAcquireMs, s.drawMs / acquired);
	rr_mtl_present::Output(line);
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

UINT64 Present_SwapChainBytes()
{
	if (!g_mtl.layer)
		return 0;
	return (UINT64)g_mtl.layer.maximumDrawableCount * g_mtl.width * g_mtl.height * 4;
}

} // namespace rr_d3d12
