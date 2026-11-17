// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk_present.cpp
/// \brief Shows the VibeRant driver's frames with a Vulkan swap chain on an SDL window of its own
///
/// The third implementation of d3d12_present.h, for the Vulkan backend. The game's window is made for OpenGL, which
/// the Software renderer's frames are shown with, and a window can't be both OpenGL's and Vulkan's. So when the driver
/// starts, the presenter makes a second window like the first for Vulkan, hides the first and makes the second the
/// game's window (sdlmain.h's `window`), which is what the video code sizes, makes fullscreen and takes input from.
/// When Software takes over, the first window comes back as the second one was and the second goes.
///
/// The swap chain is always the size of the window. The driver's placement (the same one the Direct3D 12 desktop
/// presenter uses) draws each frame into it: at Screen Scaling's Integer or 1:1 rectangle, or scaled with linear
/// filtering to the largest rectangle of the frame's aspect ratio.

#include "../d3d12_present.h"
#include "vk12.h"

#ifdef HAVE_VIBERANT_D3D
// Beside the Direct3D 12 presenter, which forwards to these (d3d12_present.h)
#define Present_TakeWindow VkPresent_TakeWindow
#define Present_Frame VkPresent_Frame
#define Present_ReleaseWindow VkPresent_ReleaseWindow
#define Present_Shutdown VkPresent_Shutdown
#define Present_GetTrace VkPresent_GetTrace
#define Present_LogPerf VkPresent_LogPerf
#define Present_Tracing VkPresent_Tracing
#define Present_FrameDone VkPresent_FrameDone
#define Present_SwapChainBytes VkPresent_SwapChainBytes
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "SDL.h"
#include "SDL_vulkan.h"

#include "../../../doomdef.h"
#include "../../../i_system.h"
#include "../../../screen.h"
#include "../../../sdl/sdlmain.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

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

// The swap chain's images get the view slots after the ones the other presenters and the tests use
constexpr UINT kFirstSlot = kRtvPlaceTest + 1;
constexpr UINT kMaxImages = kRtvCount - kFirstSlot;

struct PresentStats
{
	precise_t drawTime;
	precise_t acquireTime;
	double worstAcquireMs;
	precise_t presentTime;
	double worstPresentMs;
	int presentFailures, recreations;
};
PresentStats g_stats;

int g_tracedFrames = 0;
constexpr int kTraceFrames = 3;
INT32 g_traceSlot = -1;

struct Presenter
{
	bool stopped = false; // shut down, so nothing presents
	bool owned = false; // the Vulkan window and swap chain exist, and the Vulkan window is the game's window
	SDL_Window *vulkanWindow = nullptr;
	SDL_Window *gameWindow = nullptr; // the OpenGL window, hidden while this presents
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	bool vsync = true;
	bool outOfDate = false;
	UINT width = 0, height = 0;
	std::vector<ComPtr<ID3D12Resource>> buffers;
	std::vector<VkSemaphore> acquired; // one more than the images, used in turn
	std::vector<VkSemaphore> rendered; // one per image
	size_t frame = 0;
	bool hideGameWindow = false; // once the first frame is on the Vulkan window, so that there is always a window to see
	char failure[128] = "";
};
Presenter g_vk;

void SubmitWithoutPresenting()
{
	Core_Submit("present");
}

void Fail(const char *what)
{
	snprintf(g_vk.failure, sizeof g_vk.failure, "%s", what);
	Log("presenter: %s", what);
}

void DestroySemaphores()
{
	const VkDevice device = rr_vk12::Device();
	for (VkSemaphore semaphore : g_vk.acquired)
		vkDestroySemaphore(device, semaphore, nullptr);
	for (VkSemaphore semaphore : g_vk.rendered)
		vkDestroySemaphore(device, semaphore, nullptr);
	g_vk.acquired.clear();
	g_vk.rendered.clear();
}

// Makes (or makes again, for another size or sync mode) the swap chain of the window's size. The GPU must be idle.
bool CreateSwapchain(bool vsync)
{
	const VkPhysicalDevice physical = rr_vk12::PhysicalDevice();
	const VkDevice device = rr_vk12::Device();

	VkSurfaceCapabilitiesKHR caps;
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, g_vk.surface, &caps) != VK_SUCCESS)
	{
		Fail("couldn't read the surface's capabilities");
		return false;
	}
	int drawableWidth = 0, drawableHeight = 0;
	SDL_Vulkan_GetDrawableSize(g_vk.vulkanWindow, &drawableWidth, &drawableHeight);
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX)
	{
		extent.width = std::clamp<uint32_t>((uint32_t)std::max(1, drawableWidth), caps.minImageExtent.width, caps.maxImageExtent.width);
		extent.height = std::clamp<uint32_t>((uint32_t)std::max(1, drawableHeight), caps.minImageExtent.height, caps.maxImageExtent.height);
	}
	if (!extent.width || !extent.height)
	{
		Fail("the window has no area to present on");
		return false;
	}

	uint32_t count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, g_vk.surface, &count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, g_vk.surface, &count, formats.data());
	// The frame's own format, or the same with red and blue swapped; never an sRGB one, which would brighten it
	VkSurfaceFormatKHR chosen = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	for (VkFormat wanted : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
	{
		for (const VkSurfaceFormatKHR &format : formats)
			if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && chosen.format == VK_FORMAT_UNDEFINED)
				chosen = format;
	}
	if (chosen.format == VK_FORMAT_UNDEFINED)
	{
		Fail("the surface has neither an RGBA8 nor a BGRA8 format");
		return false;
	}

	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, g_vk.surface, &count, nullptr);
	std::vector<VkPresentModeKHR> modes(count);
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, g_vk.surface, &count, modes.data());
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // always there
	if (!vsync)
	{
		// Without the wait: frames shown at once, or failing that the newest one at each refresh
		for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
			if (std::find(modes.begin(), modes.end(), wanted) != modes.end())
				mode = wanted;
	}

	VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	info.surface = g_vk.surface;
	info.minImageCount = std::max<uint32_t>(caps.minImageCount, 3);
	if (caps.maxImageCount)
		info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
	info.imageFormat = chosen.format;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	for (VkCompositeAlphaFlagBitsKHR alpha : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR})
		if (caps.supportedCompositeAlpha & alpha)
			info.compositeAlpha = alpha;
	info.presentMode = mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = g_vk.swapchain;

	// The old one's images go first: nothing recorded refers to them any more. The last present may still hold its
	// semaphore and image, which the driver's fence doesn't cover.
	vkDeviceWaitIdle(device);
	g_vk.buffers.clear();
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
	if (g_vk.swapchain)
		vkDestroySwapchainKHR(device, g_vk.swapchain, nullptr);
	g_vk.swapchain = VK_NULL_HANDLE;
	if (result != VK_SUCCESS)
	{
		rr_vk12::NoteResult(result, "vkCreateSwapchainKHR");
		Fail("couldn't make the swap chain");
		return false;
	}
	g_vk.swapchain = swapchain;

	vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
	std::vector<VkImage> images(count);
	vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
	if (count > kMaxImages)
	{
		Fail("the swap chain has more images than the presenter has view slots for");
		return false;
	}
	g_vk.buffers.resize(count);
	for (uint32_t i = 0; i < count; i++)
	{
		if (FAILED(rr_vk12::WrapSwapchainImage(images[i], chosen.format, extent.width, extent.height, &g_vk.buffers[i])))
		{
			Fail("couldn't wrap a swap chain image");
			return false;
		}
		g_vk.buffers[i]->SetName(L"swap chain image");
		Core_Device()->CreateRenderTargetView(g_vk.buffers[i].Get(), nullptr, Core_Rtv(kFirstSlot + i));
	}

	DestroySemaphores();
	const VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	g_vk.acquired.resize(count + 1, VK_NULL_HANDLE);
	g_vk.rendered.resize(count, VK_NULL_HANDLE);
	for (VkSemaphore &made : g_vk.acquired)
		vkCreateSemaphore(device, &semaphore, nullptr, &made);
	for (VkSemaphore &made : g_vk.rendered)
		vkCreateSemaphore(device, &semaphore, nullptr, &made);

	g_vk.format = chosen.format;
	g_vk.mode = mode;
	g_vk.vsync = vsync;
	g_vk.width = extent.width;
	g_vk.height = extent.height;
	g_vk.outOfDate = false;
	g_stats.recreations++;
	Log("presenter: %ux%u swap chain, %u images, %s, %s", extent.width, extent.height, (unsigned)count,
		chosen.format == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "BGRA8",
		mode == VK_PRESENT_MODE_FIFO_KHR ? "waits for the display" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "newest frame at each refresh" : "immediate");
	return true;
}

void DestroySwapchain()
{
	const VkDevice device = rr_vk12::Device();
	g_vk.buffers.clear();
	if (device)
	{
		vkDeviceWaitIdle(device); // the last present may still hold its semaphore and image
		DestroySemaphores();
		if (g_vk.swapchain)
			vkDestroySwapchainKHR(device, g_vk.swapchain, nullptr);
	}
	g_vk.swapchain = VK_NULL_HANDLE;
	g_vk.width = g_vk.height = 0;
}

// Makes the Vulkan window as the game's window is, and the swap chain on it; then the game's window is hidden and the
// Vulkan one stands in for it
bool CreateWindowAndSwapchain()
{
	if (g_vk.owned)
		return true;
	if (!window || !rr_vk12::Device())
	{
		Fail("there is no window or no device");
		return false;
	}

	const Uint32 flags = SDL_GetWindowFlags(window);
	int x, y, w, h;
	SDL_GetWindowPosition(window, &x, &y);
	SDL_GetWindowSize(window, &w, &h);
	Uint32 wanted = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN
		| (flags & (SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI));
	SDL_Window *made = SDL_CreateWindow(SDL_GetWindowTitle(window), x, y, w, h, wanted);
	if (!made)
	{
		Fail(SDL_GetError());
		return false;
	}
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkBool32 supported = VK_FALSE;
	if (!SDL_Vulkan_CreateSurface(made, rr_vk12::Instance(), &surface)
		|| vkGetPhysicalDeviceSurfaceSupportKHR(rr_vk12::PhysicalDevice(), rr_vk12::QueueFamily(), surface, &supported) != VK_SUCCESS
		|| !supported)
	{
		Fail(surface ? "the device can't present to the window" : SDL_GetError());
		if (surface)
			vkDestroySurfaceKHR(rr_vk12::Instance(), surface, nullptr);
		SDL_DestroyWindow(made);
		return false;
	}

	g_vk.vulkanWindow = made;
	g_vk.surface = surface;
	VID_SetWindowIcon(made);
	SDL_ShowWindow(made);
	if (!CreateSwapchain(g_vk.vsync))
	{
		DestroySwapchain();
		vkDestroySurfaceKHR(rr_vk12::Instance(), surface, nullptr);
		SDL_DestroyWindow(made);
		g_vk.vulkanWindow = nullptr;
		g_vk.surface = VK_NULL_HANDLE;
		return false;
	}

	g_vk.gameWindow = window;
	g_vk.hideGameWindow = true;
	window = made;
	SDL_RaiseWindow(made);
	g_vk.owned = true;
	g_vk.failure[0] = '\0';
	Log("presenter: presenting on a Vulkan window in place of the game's OpenGL window");
	return true;
}

// The game's own window back, as the Vulkan one was
void GiveBackWindow(bool idle)
{
	if (!g_vk.owned)
		return;
	if (!idle)
		idle = Core_WaitIdle(2000, "giving the window back");
	(void)idle; // the swap chain has to go with its window either way
	DestroySwapchain();
	if (g_vk.surface)
		vkDestroySurfaceKHR(rr_vk12::Instance(), g_vk.surface, nullptr);
	g_vk.surface = VK_NULL_HANDLE;

	SDL_Window *vulkan = g_vk.vulkanWindow;
	SDL_Window *game = g_vk.gameWindow;
	if (game)
	{
		const Uint32 flags = SDL_GetWindowFlags(vulkan);
		int x, y, w, h;
		SDL_GetWindowPosition(vulkan, &x, &y);
		SDL_GetWindowSize(vulkan, &w, &h);
		if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
			SDL_SetWindowFullscreen(game, SDL_WINDOW_FULLSCREEN_DESKTOP);
		else
		{
			SDL_SetWindowFullscreen(game, 0);
			SDL_SetWindowSize(game, w, h);
			SDL_SetWindowPosition(game, x, y);
		}
		window = game;
		SDL_ShowWindow(game);
		SDL_RaiseWindow(game);
	}
	if (vulkan)
		SDL_DestroyWindow(vulkan);
	g_vk.vulkanWindow = g_vk.gameWindow = nullptr;
	g_vk.owned = false;
	Log("presenter: gave the game's OpenGL window back");
}

} // namespace

bool Present_TakeWindow()
{
	if (g_vk.owned)
		return true;
	// The driver starts with the video system, before the game's window is made: Present_Frame takes it with the first frame
	if (!window)
	{
		Log("the game's window doesn't exist yet, so it's taken with the first frame");
		return true;
	}
	return CreateWindowAndSwapchain();
}

void Present_Frame(ID3D12Resource *source, D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, bool vsync, PlaceFrameFn place)
{
	g_traceSlot = -1;
	const bool trace = Present_Tracing();

	if (g_vk.stopped || !source || !width || !height)
	{
		SubmitWithoutPresenting();
		return;
	}
	if (!g_vk.owned && !CreateWindowAndSwapchain())
		I_Error("Vulkan couldn't make its window to present on (%s).\nSet Renderer to Software in Video Options > Advanced.",
			g_vk.failure);

	int drawableWidth = 0, drawableHeight = 0;
	SDL_Vulkan_GetDrawableSize(g_vk.vulkanWindow, &drawableWidth, &drawableHeight);
	if (drawableWidth <= 0 || drawableHeight <= 0 || (SDL_GetWindowFlags(g_vk.vulkanWindow) & SDL_WINDOW_MINIMIZED))
	{
		LOG_ONCE("the game window has no area (minimized), so nothing is presented until it has one");
		SubmitWithoutPresenting();
		return;
	}

	precise_t t = I_GetPreciseTime();
	uint32_t index = 0;
	VkSemaphore acquired = VK_NULL_HANDLE;
	for (int attempt = 0;; attempt++)
	{
		if (g_vk.outOfDate || g_vk.vsync != vsync || g_vk.width != (UINT)drawableWidth || g_vk.height != (UINT)drawableHeight)
		{
			// Everything recorded and submitted may still use the old images
			Core_Submit("resize");
			Core_WaitIdle(2000, "making the swap chain again");
			if (!CreateSwapchain(vsync))
				I_Error("Vulkan couldn't make its swap chain (%s).\nSet Renderer to Software in Video Options > Advanced.", g_vk.failure);
			// A surface that dictates its size may not be the window's drawable size; take what it gave
			drawableWidth = (int)g_vk.width;
			drawableHeight = (int)g_vk.height;
		}
		acquired = g_vk.acquired[g_vk.frame % g_vk.acquired.size()];
		const VkResult result = vkAcquireNextImageKHR(rr_vk12::Device(), g_vk.swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &index);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
		{
			g_vk.outOfDate = result == VK_SUBOPTIMAL_KHR && attempt == 0 ? g_vk.outOfDate : false;
			break;
		}
		rr_vk12::NoteResult(result, "vkAcquireNextImageKHR");
		if (result != VK_ERROR_OUT_OF_DATE_KHR || attempt >= 2)
		{
			g_stats.presentFailures++;
			LOG_ONCE("vkAcquireNextImageKHR failed (%d)", (int)result);
			SubmitWithoutPresenting();
			return;
		}
		g_vk.outOfDate = true;
	}
	g_vk.frame++;
	precise_t now = I_GetPreciseTime();
	const double acquireMs = PreciseToMs(now - t);
	g_stats.acquireTime += now - t;
	g_stats.worstAcquireMs = std::max(g_stats.worstAcquireMs, acquireMs);

	// Integer and 1:1, where the frame fits the window; otherwise (and for Fit) the largest rectangle of its aspect ratio
	screen_scaling::Placement placement = screen_scaling::Compute((int)width, (int)height, (int)g_vk.width, (int)g_vk.height,
		cv_scr_scaling.value);
	if (placement.fit)
		placement = screen_scaling::FitRect((int)width, (int)height, (int)g_vk.width, (int)g_vk.height);

	ID3D12Resource *back = g_vk.buffers[index].Get();
	ID3D12GraphicsCommandList *list = Core_List();
	t = now;
	if (list)
	{
		// The buffer is cleared to black and the frame drawn in its place, which leaves source as it was
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kFirstSlot + index);
		Core_Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Core_FlushBarriers();
		if (!place || !place(source, sourceState, back, rtv, placement, g_vk.width, g_vk.height))
		{
			const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			list->ClearRenderTargetView(rtv, black, 0, nullptr);
			LOG_ONCE("couldn't place the frame in the swap chain, so it shows black");
		}
		Core_Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	}
	g_traceSlot = (INT32)index;
	// The submission waits for the image and says when it's ready to show
	rr_vk12::SetNextSubmitSemaphores(acquired, g_vk.rendered[index]);
	const UINT64 fence = list ? Core_Submit("present") : 0;
	if (!list)
		SubmitWithoutPresenting(); // reopens the list
	now = I_GetPreciseTime();
	g_stats.drawTime += now - t;
	if (!fence)
	{
		rr_vk12::SetNextSubmitSemaphores(VK_NULL_HANDLE, VK_NULL_HANDLE);
		g_vk.outOfDate = true; // the image was acquired and never shown: start over with a new swap chain
		return;
	}

	t = now;
	const VkResult result = rr_vk12::Present(g_vk.swapchain, index, g_vk.rendered[index]);
	now = I_GetPreciseTime();
	const double presentMs = PreciseToMs(now - t);
	g_stats.presentTime += now - t;
	g_stats.worstPresentMs = std::max(g_stats.worstPresentMs, presentMs);
	if (g_vk.hideGameWindow && result >= 0)
	{
		g_vk.hideGameWindow = false;
		if (g_vk.gameWindow)
			SDL_HideWindow(g_vk.gameWindow);
	}
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		g_vk.outOfDate = true;
	else if (result != VK_SUCCESS)
	{
		g_stats.presentFailures++;
		LOG_ONCE("vkQueuePresentKHR failed (%d)", (int)result);
	}
	if (trace)
		Log("  placed in the swap chain's image %u, submitted as fence %llu; acquired in %.2f ms; presented (%d, %.2f ms)",
			(unsigned)index, (unsigned long long)fence, acquireMs, (int)result, presentMs);
}

void Present_ReleaseWindow()
{
	GiveBackWindow(false);
}

void Present_Shutdown(bool idle)
{
	GiveBackWindow(idle);
	g_vk.stopped = true; // nothing presents after this
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
	const PresentStats &s = g_stats;
	const double n = frames > 0 ? frames : 1;
	I_OutputMsg("UWP perf:   D3D12: present: Vulkan swap chain (%u images, %ux%u, %s); per frame acquire %.2f ms (worst %.2f), draw %.2f ms, present %.2f ms (worst %.2f); %d present failures, %d swap chains made\n",
		(unsigned)g_vk.buffers.size(), g_vk.width, g_vk.height,
		g_vk.mode == VK_PRESENT_MODE_FIFO_KHR ? "waits for the display" : g_vk.mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate",
		PreciseToMs(s.acquireTime) / n, s.worstAcquireMs, PreciseToMs(s.drawTime) / n, PreciseToMs(s.presentTime) / n,
		s.worstPresentMs, s.presentFailures, s.recreations);
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
	return (UINT64)g_vk.buffers.size() * g_vk.width * g_vk.height * 4;
}

} // namespace rr_d3d12
