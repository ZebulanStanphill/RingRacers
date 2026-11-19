// DR. ROBOTNIK'S RING RACERS
// This program is free software distributed under the GNU General Public License, version 2.

#ifndef RR_VIBERANT_THREAD_H
#define RR_VIBERANT_THREAD_H

#include "r_viberant.h"
#include "../hw_drv.h"

namespace rr_viberant
{

struct PresentFrame;

// Reports a fatal immediately on the main thread, or hands it back from the worker.
void DriverFatal(const char *format, ...);

namespace impl
{

boolean VibeRant_Init(void);
void VibeRant_SetPalette(RGBA_t *palette);
void VibeRant_Draw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t Color);
void VibeRant_DrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, bool reduceEffects);
void VibeRant_DrawIndexedTriangles(FSurfaceInfo *surface, FOutVector *vertices, FUINT count, FBITFIELD flags, UINT32 *indices, UINT32 vertexCount, bool reduceEffects);
void VibeRant_SetBlend(FBITFIELD PolyFlags);
void VibeRant_ClearBuffer(FBOOLEAN ColorMask, FBOOLEAN DepthMask, FRGBAFloat *ClearColor);
void VibeRant_SetTexture(GLMipmap_t *texture);
void VibeRant_UpdateTexture(GLMipmap_t *texture);
void VibeRant_DeleteTexture(GLMipmap_t *pTexInfo);
void VibeRant_ReadRect(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
void VibeRant_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip);
void VibeRant_ClearMipMapCache(void);
void VibeRant_SetSpecialState(hwdspecialstate_t IdState, INT32 Value);
void VibeRant_SetTransform(FTransform *stransform);
INT32 VibeRant_GetTextureUsed(void);
boolean VibeRant_CompileShaders(void);
void VibeRant_CleanShaders(void);
void VibeRant_SetShader(int type, bool reduceEffects);
void VibeRant_UnSetShader(void);
void VibeRant_SetShaderInfo(hwdshaderinfo_t info, INT32 value);
void VibeRant_LoadCustomShader(int number, char *code, size_t size, boolean isfragment);
boolean VibeRant_CanRollSprite(void);
void VibeRant_SelectBackend(boolean vulkan, boolean validation, boolean software);
boolean VibeRant_Surface(INT32 w, INT32 h);
void VibeRant_FinishUpdate(INT32 waitvbl, INT32 effect, const PresentFrame *frame);
boolean VibeRant_TakeTextureFlushRequest(void);
boolean VibeRant_RequestSurfaceProbe(INT32 x, INT32 y);
boolean VibeRant_BeginSurfaceProbe(void);
void VibeRant_EndSurfaceProbe(void);
void VibeRant_SurfaceProbeSource(const char *kind, INT32 source);
void VibeRant_SurfaceProbeTexture(const char *name);
void VibeRant_SetPortalPlane(const float *worldPlane);
void VibeRant_SetPortalStencil(INT32 level);
boolean VibeRant_PortalsSupported(void);
boolean VibeRant_DrawPortalAperture(const FOutVector *v, INT32 level, boolean restore);
boolean VibeRant_DrawSpringMask(const FOutVector *v, INT32 count, boolean set);
void VibeRant_UseSpringMask(boolean on, INT32 level);
void VibeRant_ResetPortalDepth(INT32 level);
void VibeRant_FillVoid(void);
boolean VibeRant_ApplyFadeMap(const UINT8 *table, size_t size, UINT32 tableId, UINT8 strength);
void VibeRant_GetPresentTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe);
void VibeRant_ReleaseWindow(void);
void VibeRant_LogPerf(double seconds, INT32 effect, UINT32 shaderRolls, UINT32 cpuRolls, UINT32 geometryRolls, UINT32 lazyModels);
void VibeRant_GetCaptureCountsForTest(UINT64 *fogCopies, UINT64 *waterCopies, UINT64 *waterRoundTrips);
void VibeRant_GetReuseCountsForTest(UINT64 *placed, UINT64 *blocks, UINT64 *srvs);
void VibeRant_GetTextureStorageCountsForTest(UINT64 *placed, UINT64 *dedicated, UINT64 *committed);
boolean VibeRant_GetTexturePlacementForTest(UINT32 id, UINT32 *heap, UINT64 *offset,
	UINT64 *blockBytes, UINT64 *reportedBytes, UINT64 *alignment);
UINT32 VibeRant_GetAtlasPagesForTest(void);
void VibeRant_ReadPaletteRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
void VibeRant_ReadPresentedRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
boolean VibeRant_ReadPlacedRectForTest(INT32 mode, INT32 screenWidth, INT32 screenHeight, INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
boolean VibeRant_ReadFitRectForTest(INT32 screenWidth, INT32 screenHeight, INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data);
void VibeRant_SetRingBytesForTest(size_t vertex, size_t index, size_t constant);
void VibeRant_Shutdown(void);
void VibeRant_PostImgRedraw(float time, float type);
void VibeRant_FlushScreenTextures(void);
void VibeRant_StartScreenWipe(void);
void VibeRant_EndScreenWipe(void);
void VibeRant_DrawIntermissionBG(void);
void VibeRant_DoScreenWipe(void);
void VibeRant_MakeScreenTexture(void);
void VibeRant_MakeScreenFinalTexture(void);
void VibeRant_DrawScreenFinalTexture(int width, int height);
void VibeRant_SetScreenPalette(RGBA_t *palette);
void VibeRant_SetPaletteLookup(UINT8 *lut);
UINT32 VibeRant_CreateLightTable(RGBA_t *hw_lighttable);
void VibeRant_UpdateLightTable(UINT32 id, RGBA_t *hw_lighttable);
void VibeRant_ClearLightTables(void);
void VibeRant_DrawModel(model_t *model, INT32 frameIndex, float duration, float tics, INT32 nextFrameIndex, FTransform *pos,
	float hscale, float vscale, UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surface);
void VibeRant_CreateModelVBOs(model_t* model);
void VibeRant_RenderSkyDome(gl_sky_t* sky);

void BindTextureName(UINT32 name, bool brightmap);
int CachedPortalSupport();
bool TextureFlushPending();
bool SurfaceProbePending();
#ifdef HAVE_VIBERANT_D3D
bool PrepareFrame(PresentFrame &frame, INT32 effect);
#endif

} // namespace impl
} // namespace rr_viberant
#endif
