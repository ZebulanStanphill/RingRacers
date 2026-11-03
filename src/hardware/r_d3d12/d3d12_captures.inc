// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_captures.inc
/// \brief [UWP]: Fog and grouped water captures

// Included by r_d3d12.cpp: fog and grouped immutable water captures.

// Capture textures keep persistent SRVs. Replacement retires both resource and descriptor.
bool EnsureCaptureTexture(GpuTexture &texture, UINT width, UINT height, DXGI_FORMAT format)
{
	if (texture.res && texture.width == width && texture.height == height && texture.format == format) return true;
	const bool replacing = texture.res != nullptr;
	if (!CreateTextureStorage(&texture, 0, width, height, format, false, nullptr)) return false;
	texture.res->SetName(&texture == &g_waterScene ? L"water scene" : L"fog scene");
	if (replacing) g_stats.texturesDeleted++;
	return true;
}

bool EnsureWaterSine()
{
	if (g_waterSine.res) return true;
	INT32 waves[FINEANGLES];
	for (int i = 0; i < FINEANGLES; i++) waves[i] = FINESINE(i);
	if (!CreateTextureStorage(&g_waterSine, 0, FINEANGLES, 1, DXGI_FORMAT_R32_SINT, false, waves)) return false;
	g_waterSine.res->SetName(L"immutable water sine");
	g_stats.textureUploads++;
	g_stats.textureUploadBytes += sizeof waves;
	return true;
}

// One render-target transition pair surrounds all the growth strips.
void BeginCaptureRegions(GpuTexture &texture)
{
	Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Core_FlushBarriers();
}
void CopyRenderTargetRegion(GpuTexture &texture, const D3D12_BOX &box)
{
	D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
	from.pResource = g_rt.Get();
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	to.pResource = texture.res.Get();
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	Core_List()->CopyTextureRegion(&to, box.left, box.top, 0, &from, &box);
}
void EndCaptureRegions(GpuTexture &texture)
{
	if (&texture == &g_waterScene) g_testWaterRoundTrips++;
	Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
void SetPixelSrv(UINT parameter, GpuTexture &texture)
{
	Core_List()->SetGraphicsRootDescriptorTable(parameter, SrvGpu(texture.srv));
	g_stats.tableSets++;
}
// Allocate before copying: this can submit. The draw supersedes this slot with a combined b0/capture
// reservation, using the CPU snapshot below, then retags and restores it after other ring allocations.
bool SetPixelConstants(UINT parameter, const void *data, size_t size)
{
	UploadAllocation slot = g_constantRing.Alloc(256, 256);
	if (!slot) return false;
	memcpy(slot.cpu, data, size);
	memset(static_cast<UINT8 *>(slot.cpu) + size, 0, 256 - size);
	memcpy(parameter == 18 ? g_waterConstantData : g_fogConstantData, data, size);
	(parameter == 18 ? g_waterConstants : g_fogConstants) = slot;
	g_stats.constantUpdates++;
	return true;
}

bool BindCaptureState()
{
	if (g_waterRefraction)
	{
		if (!g_constantRing.Touch(g_waterConstants, 256)) return false;
		Core_List()->SetGraphicsRootConstantBufferView(18, g_waterConstants.gpu);
		SetPixelSrv(12, g_waterScene);
		SetPixelSrv(13, g_waterSine);
		g_stats.rootSets++;
	}
	if (g_fogRemap)
	{
		if (!g_constantRing.Touch(g_fogConstants, 256)) return false;
		Core_List()->SetGraphicsRootConstantBufferView(16, g_fogConstants.gpu);
		SetPixelSrv(11, g_fogScene);
		g_stats.rootSets++;
	}
	return true;
}

static void WaterCaptureBounds(const FOutVector *vertices, UINT count, const Mat4 &matrix,
	const int viewport[4], UINT height, D3D12_BOX &box)
{
	const float left = (float)viewport[0], top = (float)height - viewport[1] - viewport[3];
	float x0 = left + viewport[2], y0 = top + viewport[3], x1 = left, y1 = top;
	UINT inFront = 0;
	for (UINT i = 0; i < count; i++)
		inFront += matrix.m[3]*vertices[i].x + matrix.m[7]*vertices[i].y
			+ matrix.m[11]*vertices[i].z + matrix.m[15] > 0.0001f;
	if (!inFront)
	{
		box = {0, 0, 0, 0, 0, 1};
		return; // a fully clipped face cannot sample the scene
	}
	for (UINT i = 0; i < count; i++)
	{
		const FOutVector &v = vertices[i];
		const float w = matrix.m[3]*v.x + matrix.m[7]*v.y + matrix.m[11]*v.z + matrix.m[15];
		if (!(w > 0.0001f))
		{
			x0 = left; y0 = top; x1 = left + viewport[2]; y1 = top + viewport[3];
			break;
		}
		const float x = matrix.m[0]*v.x + matrix.m[4]*v.y + matrix.m[8]*v.z + matrix.m[12];
		const float y = matrix.m[1]*v.x + matrix.m[5]*v.y + matrix.m[9]*v.z + matrix.m[13];
		const float px = left + (x/w + 1)*0.5f*viewport[2];
		const float py = top + (1 - y/w)*0.5f*viewport[3];
		x0 = std::min(x0, px); y0 = std::min(y0, py);
		x1 = std::max(x1, px); y1 = std::max(y1, py);
	}
	box.left = (UINT)std::clamp(floorf(x0), left, left + viewport[2]);
	box.top = (UINT)std::clamp(floorf(y0) - 16, top, top + viewport[3]);
	box.right = (UINT)std::clamp(ceilf(x1), left, left + viewport[2]);
	box.bottom = (UINT)std::clamp(ceilf(y1) + 16, top, top + viewport[3]);
	box.front = 0; box.back = 1;
}

// wholeView: an add-on vertex stage may move the surface, so the CPU-projected
// bounds can't be trusted; capture its whole viewport instead.
static bool CaptureWaterScene(FSurfaceInfo *surface, FBITFIELD flags,
	const FOutVector *vertices, UINT count, bool wholeView = false)
{
	if (!CanDraw() || !surface || !g_rt || !vertices || !count)
		return false;
	D3D12_BOX needed;
	WaterCaptureBounds(vertices, count, Multiply(g_projection, g_modelview.back()), g_gl.viewport, g_rtHeight, needed);
	if (wholeView)
	{
		const UINT left = g_gl.viewport[0], top = g_rtHeight - g_gl.viewport[1] - g_gl.viewport[3];
		needed = {left, top, 0, left + g_gl.viewport[2], top + g_gl.viewport[3], 1};
	}
	if (needed.right <= needed.left || needed.bottom <= needed.top) return false;
	const bool resized = !g_waterScene.res || g_waterScene.width != g_rtWidth
		|| g_waterScene.height != g_rtHeight || g_waterScene.format != kScreenFormat;
	if (!EnsureCaptureTexture(g_waterScene, g_rtWidth, g_rtHeight, kScreenFormat)) return false;
	if (resized) g_waterCaptureReady = false;
	if (!EnsureWaterSine()) return false;
	// b5 and t9 stay separate from the palette, wipe and add-on shader slots.
	struct WaterConstants { float viewport[4]; INT32 blend; INT32 pad[3]; };
	const FBITFIELD blend = flags & (PF_Blending & ~(PF_Masked | PF_Occlude));
	INT32 mode = 0;
	if (blend == PF_Additive) mode = 1;
	else if (blend == PF_Subtractive) mode = 2;
	else if (blend == PF_ReverseSubtract) mode = 3;
	else if (blend == PF_Multiplicative) mode = 4;
	else if (blend == PF_Environment) mode = 5;
	const WaterConstants constants = {{(float)g_gl.viewport[0],
		(float)g_rtHeight - g_gl.viewport[1] - g_gl.viewport[3],
		(float)(g_gl.viewport[0] + g_gl.viewport[2] - 1),
		(float)(g_rtHeight - g_gl.viewport[1] - 1)}, mode, {0, 0, 0}};
	if (!SetPixelConstants(18, &constants, sizeof constants)) return false;
	// Grow an immutable capture only into pixels no earlier piece could have
	// touched. The old rectangle already includes every earlier polygon and
	// its ripple margin. Never overwrite it with the partially refracted scene.
	const bool fresh = !g_waterCaptureGrouped || !g_waterCaptureReady;
	D3D12_BOX bounds = needed;
	if (!fresh)
	{
		bounds.left = std::min(bounds.left, g_waterCapturedBounds.left);
		bounds.top = std::min(bounds.top, g_waterCapturedBounds.top);
		bounds.right = std::max(bounds.right, g_waterCapturedBounds.right);
		bounds.bottom = std::max(bounds.bottom, g_waterCapturedBounds.bottom);
	}
	if (fresh || bounds.left != g_waterCapturedBounds.left || bounds.top != g_waterCapturedBounds.top
		|| bounds.right != g_waterCapturedBounds.right || bounds.bottom != g_waterCapturedBounds.bottom)
	{
		BeginCaptureRegions(g_waterScene);
		const auto copy = [&](UINT left, UINT top, UINT right, UINT bottom)
		{
			if (right <= left || bottom <= top) return;
			const D3D12_BOX box = {left, top, 0, right, bottom, 1};
			CopyRenderTargetRegion(g_waterScene, box);
			g_stats.screens++;
			g_stats.waterCopies++;
			g_testWaterCopies++;
			g_stats.waterPixels += (UINT64)(right - left) * (bottom - top);
		};
		if (fresh)
			copy(bounds.left, bounds.top, bounds.right, bounds.bottom);
		else
		{
			const D3D12_BOX &old = g_waterCapturedBounds;
			copy(bounds.left, bounds.top, bounds.right, old.top);
			copy(bounds.left, old.bottom, bounds.right, bounds.bottom);
			copy(bounds.left, old.top, old.left, old.bottom);
			copy(old.right, old.top, bounds.right, old.bottom);
		}
		EndCaptureRegions(g_waterScene);
		g_waterCapturedBounds = bounds;
		g_waterCaptureReady = g_waterCaptureGrouped;
	}
	SetPixelSrv(12, g_waterScene);
	SetPixelSrv(13, g_waterSine);
	return true;
}

static void FogCaptureBounds(const FOutVector *vertices, UINT count, const Mat4 &matrix,
	const int viewport[4], UINT height, D3D12_BOX &box)
{
	const float left = (float)viewport[0], top = (float)height - viewport[1] - viewport[3];
	float x0 = left + viewport[2], y0 = top + viewport[3], x1 = left, y1 = top;
	UINT inFront = 0;
	for (UINT i = 0; i < count; i++)
		inFront += matrix.m[3]*vertices[i].x + matrix.m[7]*vertices[i].y
			+ matrix.m[11]*vertices[i].z + matrix.m[15] > 0.0001f;
	if (!inFront)
	{
		box = {0, 0, 0, 0, 0, 1};
		return;
	}
	for (UINT i = 0; i < count; i++)
	{
		const FOutVector &v = vertices[i];
		const float w = matrix.m[3]*v.x + matrix.m[7]*v.y + matrix.m[11]*v.z + matrix.m[15];
		if (!(w > 0.0001f))
		{
			x0 = left; y0 = top; x1 = left + viewport[2]; y1 = top + viewport[3];
			break;
		}
		const float x = matrix.m[0]*v.x + matrix.m[4]*v.y + matrix.m[8]*v.z + matrix.m[12];
		const float y = matrix.m[1]*v.x + matrix.m[5]*v.y + matrix.m[9]*v.z + matrix.m[13];
		const float px = left + (x/w + 1)*0.5f*viewport[2];
		const float py = top + (1 - y/w)*0.5f*viewport[3];
		x0 = std::min(x0, px); y0 = std::min(y0, py);
		x1 = std::max(x1, px); y1 = std::max(y1, py);
	}
	box.left = (UINT)std::clamp(floorf(x0), left, left + viewport[2]);
	box.top = (UINT)std::clamp(floorf(y0), top, top + viewport[3]);
	box.right = (UINT)std::clamp(ceilf(x1), left, left + viewport[2]);
	box.bottom = (UINT)std::clamp(ceilf(y1), top, top + viewport[3]);
	box.front = 0; box.back = 1;
}

static bool CaptureFogScene(FSurfaceInfo *surface, FBITFIELD flags,
	const FOutVector *vertices, UINT count)
{
	if (!CanDraw() || !surface || !g_rt || !vertices || !count)
		return false;
	D3D12_BOX box;
	FogCaptureBounds(vertices, count, Multiply(g_projection, g_modelview.back()), g_gl.viewport, g_rtHeight, box);
	if (box.right <= box.left || box.bottom <= box.top) return false;
	if (!EnsureCaptureTexture(g_fogScene, g_rtWidth, g_rtHeight, kScreenFormat)) return false;
	struct FogConstants { INT32 plane, lightnum; float viewRatio; INT32 pad; };
	const FogConstants constants = {(flags & PF_FogPlane) ? 1 : 0,
		R_AdjustLightLevel(std::clamp((INT32)surface->LightInfo.light_level >> 3, 0, 31)),
		(float)g_gl.viewport[2] / g_screenWidth, 0};
	if (!SetPixelConstants(16, &constants, sizeof constants)) return false;
	BeginCaptureRegions(g_fogScene);
	CopyRenderTargetRegion(g_fogScene, box);
	EndCaptureRegions(g_fogScene);
	SetPixelSrv(11, g_fogScene);
	g_stats.screens++;
	g_stats.fogCaptures++;
	g_testFogCopies++;
	return true;
}
