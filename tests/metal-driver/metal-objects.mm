// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// What the driver test's check of mtl12's presenter hooks needs and only Objective-C can make: the test itself is C++.
// Compiled with ARC.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// A texture like a drawable's: BGRA8, private to the GPU, and a render target. An id<MTLTexture> the caller owns.
void *MetalTest_NewTexture(void *device, unsigned width, unsigned height)
{
	@autoreleasepool
	{
		id<MTLDevice> metal = (__bridge id<MTLDevice>)device;
		MTLTextureDescriptor *info = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:width height:height mipmapped:NO];
		info.usage = MTLTextureUsageRenderTarget;
		info.storageMode = MTLStorageModePrivate;
		return (__bridge_retained void *)[metal newTextureWithDescriptor:info];
	}
}

static CAMetalLayer *layer;

// The next drawable of a layer that is on no window, which is as much of presenting as a program without a window can
// do. An id<CAMetalDrawable> the caller owns, and its texture, which the drawable owns; null if the layer has none
// to give.
void *MetalTest_NextDrawable(void *device, unsigned width, unsigned height, void **texture)
{
	@autoreleasepool
	{
		layer = [CAMetalLayer layer];
		layer.device = (__bridge id<MTLDevice>)device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.drawableSize = CGSizeMake(width, height);
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (!drawable)
			return nullptr;
		*texture = (__bridge void *)drawable.texture;
		return (__bridge_retained void *)drawable;
	}
}

void MetalTest_Release(void *object)
{
	// In a pool: Metal's objects autorelease others as they are deallocated (a texture its device, a drawable its layer)
	@autoreleasepool
	{
		if (object)
			CFRelease(object);
	}
}
