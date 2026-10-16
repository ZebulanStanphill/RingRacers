// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_screens.inc
/// \brief [UWP]: Screen copies, wipes, the intermission background, the final screen, post image distortion and fade maps

// Included by r_d3d12.cpp. A screen copy is an ordinary texture object with a persistent SRV, so it follows the
// texture rules: replacing or deleting one retires its resource and SRV index with the fence of the work recorded
// so far. The fade map's copy of the screen, the wipe mask and the fade maps are GpuTextures retired the same way.
//
// The wipe and fade map passes draw into the screen with its depth buffer still bound, so they are fixed-function
// pipelines on D24S8 with depth and stencil off. Neither changes the render target, but both change the pipeline,
// root arguments, viewport and topology, so each ends in BindRenderTarget, which makes every cache stale.

UINT32 g_screenTexture = 0, g_startScreenWipe = 0, g_endScreenWipe = 0, g_finalScreenTexture = 0;

// Must match WipeConstants in shaders.hlsl. The regular draw constants at b0 stay untouched.
struct WipeConstants
{
	float screenSize[2], screenFix[2];
	INT32 mode, wiggle, reverse, pad;
};
static_assert(sizeof(WipeConstants) == 32, "WipeConstants must match shaders.hlsl");
GpuTexture g_wipeMask; // R8_UNORM: the raw lump's bytes
std::vector<UINT8> g_wipeMaskData;

// Must match FadeMapConstants in shaders.hlsl
struct FadeMapConstants
{
	INT32 row, pad[3];
};
static_assert(sizeof(FadeMapConstants) == 16, "FadeMapConstants must match shaders.hlsl");
struct FadeMap
{
	GpuTexture texture; // 256 columns of palette indices, a row per strength
	UINT rows = 0;
};
std::unordered_map<UINT32, FadeMap> g_fadeMaps;
// The screen as it was before the fade map, which the pass reads while it draws into the screen
GpuTexture g_fadeScreen;

int ScreenTextureSize()
{
	// Use a power of two texture, dammit
	if (g_screenWidth <= 512)
		return 512;
	else if (g_screenWidth <= 1024)
		return 1024;
	return 2048;
}

// Copies the top-left width x height of the screen into target. The screen goes to COPY_SOURCE and the target to
// COPY_DEST in one batch, and both go back in one batch with the next draw, clear, copy or submission.
void CopyScreenInto(GpuTexture &target, UINT width, UINT height)
{
	D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
	from.pResource = g_rt.Get();
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	to.pResource = target.res.Get();
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box = {0, 0, 0, width, height, 1};

	Transition(target, D3D12_RESOURCE_STATE_COPY_DEST);
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Core_FlushBarriers();
	Core_List()->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
	Transition(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	g_stats.screenCopies++;
}

// glCopyTexImage2D or glCopyTexSubImage2D of the bottom-left of the screen to a texture texsize pixels square.
// Direct3D's rows go down, so here the screen's top-left goes to the texture's top-left, and drawing it needs t
// flipped.
void CopyScreenToTexture(UINT32 &name)
{
	const int texsize = ScreenTextureSize();
	const bool firstTime = (name == 0) || !FindTexture(name);

	if (firstTime)
		name = GenTexture();

	BindTexture(0, name);
	Texture *texture = FindTexture(name);

	if (firstTime)
	{
		texture->magFilter = texture->minFilter = FILTER_NEAREST;
		texture->repeatS = texture->repeatT = false;
		texture->sampler = UINT32_MAX;
	}

	if (!texture->res || texture->width != (UINT)texsize || texture->format != kScreenFormat)
	{
		if (!CreateTextureStorage(texture, name, texsize, texsize, kScreenFormat, false, nullptr))
			return;
	}

	if (CanDraw())
		CopyScreenInto(*texture, std::min<UINT>(g_rtWidth, texsize), std::min<UINT>(g_rtHeight, texsize));
	g_stats.screens++;
}

// Draws a quad of the screen's texture with OpenGL's texture coordinates, whose t goes up from the bottom of the
// screen, at yfix
void DrawScreenQuad(const float vertices[12], const float st[8], float yfix, bool wipe = false)
{
	FOutVector v[4];

	for (int i = 0; i < 4; i++)
	{
		v[i].x = vertices[i*3];
		v[i].y = vertices[i*3 + 1];
		v[i].z = vertices[i*3 + 2];
		v[i].s = st[i*2];
		v[i].t = yfix - st[i*2 + 1];
	}

	DrawArrays(LAYOUT_POLY, PRIM_FAN, v, 4, wipe);
}

// The wipe and fade map passes: a triangle over the screen, opaque, with the screen's depth buffer bound but unused
PipelineKey FullscreenPassKey(PSKind ps)
{
	PipelineKey key = {};
	key.vs = VS_FULLSCREEN;
	key.ps = ps;
	key.layout = LAYOUT_NONE;
	key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	key.rtFormat = kScreenFormat;
	key.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	key.blend = 1 << 15; // opaque, all channels
	// Neither depth nor stencil, with valid fixed-function values like the prewarmed pipelines
	key.depth = 2ULL | (8ULL << 4) | (1ULL << 8) | (1ULL << 12) | (1ULL << 16) | (255ULL << 20) | (255ULL << 28);
	return key;
}

// Everything but the pass's own root arguments
void BeginFullscreenPass(ID3D12PipelineState *pso, float width, float height)
{
	ID3D12GraphicsCommandList *list = Core_List();
	const D3D12_VIEWPORT viewport = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
	const D3D12_RECT scissor = {0, 0, (LONG)g_rtWidth, (LONG)g_rtHeight};

	list->SetPipelineState(pso);
	// VSFullscreen reads b0's depth; a draw's constants may not be the ones bound
	list->SetGraphicsRootConstantBufferView(0, g_zeroConstants->GetGPUVirtualAddress());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	g_stats.psoChanges++;
	g_stats.rootSets++;
}

void EndFullscreenPass()
{
	Core_FlushBarriers();
	Core_List()->DrawInstanced(3, 1, 0, 0);
	g_stats.draws++;
	g_stats.vertices += 3;
	// Back to drawing the screen: everything this pass set, and the caches of it, is stale
	BindRenderTarget();
}

// A constant buffer for a pass's root CBV. Allocate before recording the pass: it can submit.
UploadAllocation AllocatePassConstants(const void *data, size_t size)
{
	UploadAllocation slot = g_constantRing.Alloc(256, 256);

	if (!slot)
		return slot;
	memcpy(slot.cpu, data, size);
	memset(static_cast<UINT8 *>(slot.cpu) + size, 0, 256 - size);
	g_stats.constantUpdates++;
	return slot;
}

// The saved screen contains all views; the shader clamps sampling to this view.
FOutVector PostImageVertex(int x, int y, const int viewport[4], int screenHeight, int textureSize)
{
	FOutVector v = {};
	v.x = 2.0f * x - 1.0f;
	v.y = 2.0f * y - 1.0f;
	v.s = (viewport[0] + (float)x * viewport[2]) / textureSize;
	v.t = (screenHeight - viewport[1] - (float)y * viewport[3]) / textureSize;
	return v;
}

void D3D_PostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2])
{
	(void)points; // The UWP shader uses Software's per-pixel sampling, not Legacy GL's grid.
	const int texsize = ScreenTextureSize();
	const GLState saved = g_gl;
	g_postImage = true;
	g_gl.depthTest = false;
	g_gl.alphaTest = false;
	g_gl.blend = false;
	g_gl.cullFace = false;
	SetColor(kWhite);
	g_gl.texture2D = true;
	g_gl.texEnv = TE_REPLACE;
	BindTexture(0, g_screenTexture);
	FOutVector vertices[4] = {
		PostImageVertex(0, 0, g_gl.viewport, g_screenHeight, texsize),
		PostImageVertex(1, 0, g_gl.viewport, g_screenHeight, texsize),
		PostImageVertex(1, 1, g_gl.viewport, g_screenHeight, texsize),
		PostImageVertex(0, 1, g_gl.viewport, g_screenHeight, texsize),
	};
	DrawArrays(LAYOUT_POLY, PRIM_FAN, vertices, 4);
	g_postImage = false;
	g_gl = saved;
	g_stats.screens++;
	g_stats.postImages++;
}

// Sryder:	This needs to be called whenever the screen changes resolution in order to reset the screen textures to use
//			a new size
void D3D_FlushScreenTextures(void)
{
	SetWaterSurface(0);
	for (GpuTexture *capture : {&g_waterScene, &g_waterSine, &g_fogScene})
	{
		if (capture->res) g_stats.texturesDeleted++;
		RetireTexture(*capture);
	}
	g_waterRefraction = g_fogRemap = false;
	g_waterConstants = g_fogConstants = {};
	g_waterCapturedBounds = {};
	// The lists in flight keep using them; their resources and views go when the fence says so
	if (g_fadeScreen.res)
		g_stats.texturesDeleted++;
	RetireTexture(g_fadeScreen);
	if (g_wipeMask.res)
		g_stats.texturesDeleted++;
	RetireTexture(g_wipeMask);
	g_wipeMaskData.clear();
	DeleteTextureObject(g_screenTexture);
	DeleteTextureObject(g_startScreenWipe);
	DeleteTextureObject(g_endScreenWipe);
	DeleteTextureObject(g_finalScreenTexture);
	g_screenTexture = 0;
	g_startScreenWipe = 0;
	g_endScreenWipe = 0;
	g_finalScreenTexture = 0;
}

// Create Screen to fade from
void D3D_StartScreenWipe(void)
{
	CopyScreenToTexture(g_startScreenWipe);
	g_texDownloaded = g_startScreenWipe;
}

// Create Screen to fade to
void D3D_EndScreenWipe(void)
{
	CopyScreenToTexture(g_endScreenWipe);
	g_texDownloaded = g_endScreenWipe;
}

const float kScreenVerts[12] =
{
	-1.0f, -1.0f, 1.0f,
	-1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, -1.0f, 1.0f
};

// The texture coordinates of the screen in its texture, in OpenGL's t
void ScreenFix(float fix[8], float *yfix)
{
	const int texsize = ScreenTextureSize();
	const float xfix = 1/((float)(texsize)/((float)((g_screenWidth))));
	*yfix = 1/((float)(texsize)/((float)((g_screenHeight))));

	fix[0] = 0.0f;
	fix[1] = 0.0f;
	fix[2] = 0.0f;
	fix[3] = *yfix;
	fix[4] = xfix;
	fix[5] = *yfix;
	fix[6] = xfix;
	fix[7] = 0.0f;
}

// Draw the last scene under the intermission
void D3D_DrawIntermissionBG(void)
{
	float fix[8], yfix;

	ScreenFix(fix, &yfix);

	ClearScreenColor();
	ClearScreenDepth();

	BindTexture(0, g_screenTexture);
	SetColor(kWhite);

	DrawScreenQuad(kScreenVerts, fix, yfix);

	g_texDownloaded = g_screenTexture;
	g_stats.screens++;
}

// [UWP]: Read the RHI's raw mask bytes separately, leaving r_opengl.c's palette-red mask for the fallback.
bool UploadWipeMask()
{
	if (g_wipetype >= 100 || g_wipeframe >= 100)
		return false;
	char name[9];
	snprintf(name, sizeof name, "FADE%02u%02u", (unsigned)g_wipetype, (unsigned)g_wipeframe);
	const lumpnum_t lump = W_CheckNumForName(name);
	if (lump == LUMPERROR)
		return false;

	const size_t size = W_LumpLength(lump);
	UINT width, height;
	switch (size)
	{
		case 256000: width = 640; height = 400; break;
		case 64000: width = 320; height = 200; break;
		case 16000: width = 160; height = 100; break;
		case 4000: width = 80; height = 50; break;
		default: return false;
	}

	g_wipeMaskData.resize(size);
	W_ReadLump(lump, g_wipeMaskData.data());

	// Through the staging ring, in order with the draws around it: an earlier wipe frame keeps its mask
	bool stored;
	if (g_wipeMask.res && g_wipeMask.width == width && g_wipeMask.height == height)
		stored = UploadTexture(g_wipeMask, g_wipeMaskData.data());
	else
	{
		const bool replacing = g_wipeMask.res != nullptr;
		stored = CreateTextureStorage(&g_wipeMask, 0, width, height, DXGI_FORMAT_R8_UNORM, false, g_wipeMaskData.data());
		if (stored)
		{
			g_wipeMask.res->SetName(L"wipe mask");
			if (replacing)
				g_stats.texturesDeleted++;
		}
	}
	if (!stored)
		return false;
	g_stats.textureUploads++;
	g_stats.textureUploadBytes += size;
	return true;
}

// [UWP]: Match hwr2/postprocess_wipe.cpp and its shader instead of r_opengl.c's simple crossfade.
bool DrawWipeFull()
{
	Texture *start = FindTexture(g_wipereverse ? g_endScreenWipe : g_startScreenWipe);
	Texture *end = FindTexture(g_wipereverse ? g_startScreenWipe : g_endScreenWipe);
	if (!CanDraw() || !start || !end || !start->res || !end->res || g_screenWidth <= 0 || g_screenHeight <= 0)
		return false;

	ID3D12PipelineState *pso = FindOrCreatePipeline(FullscreenPassKey(PS_WIPEFULL));
	if (!pso)
		return false;
	if (!UploadWipeMask())
		return false;

	WipeConstants c = {};
	c.screenSize[0] = (float)g_screenWidth;
	c.screenSize[1] = (float)g_screenHeight;
	c.screenFix[0] = (float)g_screenWidth / ScreenTextureSize();
	c.screenFix[1] = (float)g_screenHeight / ScreenTextureSize();
	c.mode = F_WipeIsToBlack(g_wipemode) ? 2 : F_WipeIsToWhite(g_wipemode) ? 3 : F_WipeIsToInvert(g_wipemode) ? 1 : 0;
	c.wiggle = g_wipeencorewiggle;
	c.reverse = g_wipereverse;
	// b1 is separate from b0 and from the draws' constants, and no other ring allocation follows
	UploadAllocation constants = AllocatePassConstants(&c, sizeof c);
	if (!constants)
		return false;

	ClearScreenColor();
	ClearScreenDepth();

	ID3D12GraphicsCommandList *list = Core_List();
	BeginFullscreenPass(pso, c.screenSize[0], c.screenSize[1]);
	list->SetGraphicsRootDescriptorTable(1, SrvGpu(start->srv)); // t0
	list->SetGraphicsRootDescriptorTable(2, SamplerGpu(0)); // s0: point, clamped
	list->SetGraphicsRootDescriptorTable(3, SrvGpu(end->srv)); // t1
	// t2-t4 are reserved for palette rendering.
	list->SetGraphicsRootDescriptorTable(8, SrvGpu(g_wipeMask.srv)); // t5
	list->SetGraphicsRootConstantBufferView(14, constants.gpu); // b1
	g_stats.tableSets += 4;
	g_stats.rootSets++;
	g_constantRing.Touch(constants, 256);
	EndFullscreenPass();
	return true;
}

// Do screen fades!
void D3D_DoScreenWipe(void)
{
	Present_Immediately(); // applies to both the full wipe and the fallback below
	if (DrawWipeFull())
	{
		g_texDownloaded = g_endScreenWipe;
		g_stats.screens++;
		g_stats.wipes++;
		return;
	}
	D3D12_LOG_ONCE("wipe: the full wipe isn't available; drawing the two-pass wipe (logged once)");
	g_stats.wipeFallbacks++;
	// Keep the two-pass Legacy GL wipe if the full wipe's resources are unavailable.
	float fix[8], yfix;
	const UINT32 fademaskdownloaded = g_texDownloaded; // the fade mask that has been set

	ScreenFix(fix, &yfix);

	ClearScreenColor();
	ClearScreenDepth();

	D3D_SetBlend(PF_Modulated|PF_NoDepthTest);
	g_gl.texture2D = true;

	// Draw the original screen
	BindTexture(0, g_startScreenWipe);
	SetColor(kWhite);
	DrawScreenQuad(kScreenVerts, fix, yfix);

	D3D_SetBlend(PF_Modulated|PF_Translucent|PF_NoDepthTest);

	// Draw the end screen that fades in
	g_gl.texture2D = true;
	BindTexture(0, g_endScreenWipe);
	g_gl.texEnv = TE_REPLACE;

	// The fade mask, on the second texture unit, modulated
	BindTexture(1, fademaskdownloaded);
	DrawScreenQuad(kScreenVerts, fix, yfix, g_program < 0);

	g_texDownloaded = g_endScreenWipe;
	g_stats.screens++;
}

// Create a texture from the screen.
void D3D_MakeScreenTexture(void)
{
	CopyScreenToTexture(g_screenTexture);
	g_texDownloaded = g_screenTexture;
}

void D3D_MakeScreenFinalTexture(void)
{
	CopyScreenToTexture(g_finalScreenTexture);
	g_texDownloaded = g_finalScreenTexture;
}

void D3D_DrawScreenFinalTexture(int width, int height)
{
	float fix[8], yfix;
	float xoff = 1, yoff = 1; // xoffset and yoffset for the polygon to have black bars around the screen
	float off[12];

	ScreenFix(fix, &yfix);

	const float origaspect = (float)g_screenWidth / g_screenHeight;
	const float newaspect = (float)width / height;
	if (origaspect < newaspect)
	{
		xoff = origaspect / newaspect;
		yoff = 1;
	}
	else if (origaspect > newaspect)
	{
		xoff = 1;
		yoff = newaspect / origaspect;
	}

	off[0] = -xoff;
	off[1] = -yoff;
	off[2] = 1.0f;
	off[3] = -xoff;
	off[4] = yoff;
	off[5] = 1.0f;
	off[6] = xoff;
	off[7] = yoff;
	off[8] = 1.0f;
	off[9] = xoff;
	off[10] = -yoff;
	off[11] = 1.0f;

	SetViewport(0, 0, width, height);

	FRGBAFloat clearColour = {0.0f, 0.0f, 0.0f, 1.0f};
	D3D_ClearBuffer(true, false, &clearColour);
	BindTexture(0, g_finalScreenTexture);

	SetColor(kWhite);
	DrawScreenQuad(off, fix, yfix);
	g_texDownloaded = g_finalScreenTexture;
	g_stats.screens++;
}

// Software's in-place V_OffsetIntoFadeMap remap, before the HUD and screen-palette postprocess. Returns false, having
// drawn nothing, if the pass can't run, so the caller keeps its overlay fallback.
bool ApplyFadeMap(const UINT8 *table, size_t size, UINT32 tableId, UINT8 strength)
{
	// Smooth-mode textures use the local palette, not the master lookup's colors.
	// Keep the legacy overlay in that mode, even if a previous lookup is still alive.
	if (!CanDraw() || !g_paletteRendering || !g_paletteLookup.res || !g_masterPalette.res
		|| !table || size < 256 || size % 256 || size / 256 > 256)
		return false;

	const UINT rows = (UINT)(size / 256);
	auto found = g_fadeMaps.find(tableId);
	if (found == g_fadeMaps.end() || found->second.rows != rows)
	{
		FadeMap &fade = g_fadeMaps[tableId];
		const bool replacing = fade.texture.res != nullptr;
		if (!CreateTextureStorage(&fade.texture, tableId, 256, rows, DXGI_FORMAT_R8_UNORM, false, table))
		{
			if (!replacing)
				g_fadeMaps.erase(tableId);
			return false;
		}
		fade.texture.res->SetName(L"fade map");
		fade.rows = rows;
		if (replacing)
			g_stats.texturesDeleted++;
		g_stats.textureUploads++;
		g_stats.textureUploadBytes += size;
		found = g_fadeMaps.find(tableId);
	}

	// Use the full render-target dimensions, without the screen-copy machinery's
	// power-of-two square limit or its changes to emulated GL texture bindings.
	if (!g_fadeScreen.res || g_fadeScreen.width != g_rtWidth || g_fadeScreen.height != g_rtHeight
		|| g_fadeScreen.format != kScreenFormat)
	{
		const bool replacing = g_fadeScreen.res != nullptr;
		if (!CreateTextureStorage(&g_fadeScreen, 0, g_rtWidth, g_rtHeight, kScreenFormat, false, nullptr))
			return false;
		g_fadeScreen.res->SetName(L"fade map screen");
		if (replacing)
			g_stats.texturesDeleted++;
	}

	FadeMapConstants c = {};
	c.row = (INT32)std::min<UINT>(strength, rows - 1);
	ID3D12PipelineState *pso = FindOrCreatePipeline(FullscreenPassKey(PS_FADEMAP));
	if (!pso)
		return false;
	// The last of the ring allocations, and every failure exit precedes the copy and draw: an overlay fallback sees
	// the original.
	UploadAllocation constants = AllocatePassConstants(&c, sizeof c);
	if (!constants)
		return false;

	CopyScreenInto(g_fadeScreen, g_rtWidth, g_rtHeight);
	ID3D12GraphicsCommandList *list = Core_List();
	BeginFullscreenPass(pso, (float)g_rtWidth, (float)g_rtHeight);
	list->SetGraphicsRootDescriptorTable(1, SrvGpu(g_fadeScreen.srv)); // t0: the screen before the pass
	list->SetGraphicsRootDescriptorTable(6, SrvGpu(g_paletteLookup.srv)); // t2
	list->SetGraphicsRootDescriptorTable(9, SrvGpu(found->second.texture.srv)); // t6
	list->SetGraphicsRootDescriptorTable(10, SrvGpu(MasterPaletteSrv())); // t7
	list->SetGraphicsRootConstantBufferView(15, constants.gpu); // b2
	g_stats.tableSets += 4;
	g_stats.rootSets++;
	g_constantRing.Touch(constants, 256);
	EndFullscreenPass();
	// The emulated GL/2D state and depth contents are unchanged; this is not a wipe
	// and must not present immediately.
	g_stats.screens += 2;
	g_stats.fadeMaps++;
	return true;
}
