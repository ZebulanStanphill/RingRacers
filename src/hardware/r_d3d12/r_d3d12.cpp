// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file r_d3d12.cpp
/// \brief [UWP]: Legacy GL's driver for Direct3D 12 (VibeRant D3D12), presented with its own swap chain
///
/// GL-state emulation and textured polygon/line draws ported from the former Direct3D 11 driver, using fence-retired
/// upload rings, persistent texture descriptors and cached pipeline states. Advanced passes arrive in later steps.
/// The S1 diagnostic hue is retained, disabled, for future presentation diagnostics.

#include "d3d12_core.h"
#include "d3d12_present.h"
#include "d3d12_pipeline.h"
#include "d3d12_prewarm.h"
#include "d3d12_upload.h"
#include "mips.h"
#include "custom_shader.h"
#include "../hw_glsl.h"
#include <d3dcompiler.h>
#include "texture_repack.h"
#include "r_d3d12_shaders/RootSignature.h"

#include <cmath>
#include <cfloat>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "SDL.h"

#include "../../doomdef.h"
#include "../../command.h"
#include "../../i_system.h"
#include "../../m_misc.h"
#include "../../screen.h"
#include "../../p_tick.h"
#include "../../r_fps.h"
#include "../../r_main.h"
#include "../../doomstat.h"
#include "../../f_finale.h"
#include "../../w_wad.h"
#include "../hw_drv.h"
#include "../hw_main.h"
#include "../hw_glob.h"
#include "../hw_md2.h"
#include "../../m_fixed.h"
#include "r_d3d12.h"

// doomdef.h's
#undef min
#undef max
#undef clamp

// v_video.h's, which this driver doesn't otherwise need
extern "C" RGBA_t *pMasterPalette;

namespace
{

using namespace rr_d3d12;

void Log(const char *format, ...)
{
	char message[2048];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	Core_Log("%s", message);
}

// What the driver did since the last perf summary
struct Stats
{
	int frames;
	int clears;
	UINT64 draws, vertices, indices, constantUpdates, psoChanges, rootSets, tableSets;
	// Not drawn yet: calls by driver function
	int lines, polygons, batches, skies, models;
	UINT64 texturesCreated, textureUploads, textureUploadBytes, texturesDeleted;
	int flushes;
	int skyBuilds, modelFallbacks, modelLerps;
	UINT64 mipsGenerated;
	double mipMs, textureCreateMs;
	int screens; // screen textures, wipes, postimg, the intermission background and the final screen
	int screenCopies, wipes, wipeFallbacks, postImages, fadeMapFallbacks;
	UINT64 waterCopies, waterPixels, fogCaptures;
	int states; // blending, clipping, transforms, special states, shaders, palettes and light tables
	int portalApertures, portalDepthResets, portalStencilLevels, portalMaxLevel, fadeMaps;
	int lightTablesCreated, lightTablesUpdated, paletteUploads, paletteSteps;
	precise_t paletteTime;
	precise_t presentTime;
};
Stats g_stats;

double PreciseToMs(precise_t t)
{
	return t * 1000.0 / I_GetPrecisePrecision();
}

// S1 presentation diagnostic, retained for S10.
constexpr bool kDiagnosticHue = false;
UINT64 g_hueFrame = 0;

bool g_deviceLost = false;
bool g_shutdown = false;

INT32 g_screenWidth = 0, g_screenHeight = 0; // r_opengl.c's screen_width and screen_height

// Column-major, like OpenGL's
struct Mat4
{
	float m[16];
};

constexpr Mat4 kIdentity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};

Mat4 Multiply(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;

	for (int column = 0; column < 4; column++)
	{
		for (int row = 0; row < 4; row++)
		{
			r.m[column*4 + row] =
				a.m[0*4 + row] * b.m[column*4 + 0] +
				a.m[1*4 + row] * b.m[column*4 + 1] +
				a.m[2*4 + row] * b.m[column*4 + 2] +
				a.m[3*4 + row] * b.m[column*4 + 3];
		}
	}
	return r;
}

// OpenGL's matrix stacks, of which Legacy GL uses the modelview's
enum { MODE_MODELVIEW, MODE_PROJECTION };
std::vector<Mat4> g_modelview(1, kIdentity);
Mat4 g_projection = kIdentity;
int g_matrixMode = MODE_MODELVIEW;

Mat4 &CurrentMatrix()
{
	return g_matrixMode == MODE_PROJECTION ? g_projection : g_modelview.back();
}

void MatLoadIdentity()
{
	CurrentMatrix() = kIdentity;
}

void MatMultiply(const float *m)
{
	Mat4 b;
	memcpy(b.m, m, sizeof b.m);
	CurrentMatrix() = Multiply(CurrentMatrix(), b);
}

void MatScale(float x, float y, float z)
{
	Mat4 &m = CurrentMatrix();

	for (int i = 0; i < 4; i++)
	{
		m.m[0*4 + i] *= x;
		m.m[1*4 + i] *= y;
		m.m[2*4 + i] *= z;
	}
}

void MatTranslate(float x, float y, float z)
{
	Mat4 &m = CurrentMatrix();

	for (int i = 0; i < 4; i++)
		m.m[3*4 + i] += m.m[0*4 + i] * x + m.m[1*4 + i] * y + m.m[2*4 + i] * z;
}

// glRotatef
void MatRotate(float angle, float x, float y, float z)
{
	const float mag = sqrtf(x*x + y*y + z*z);
	Mat4 r = kIdentity;

	if (mag <= 1.0e-4f)
		return;

	x /= mag;
	y /= mag;
	z /= mag;

	const float radians = angle * (float)(M_PI / 180.0);
	const float s = sinf(radians);
	const float c = cosf(radians);
	const float one_c = 1.0f - c;

#define R(row, column) r.m[(column)*4 + (row)]
	R(0, 0) = (one_c * x * x) + c;
	R(0, 1) = (one_c * x * y) - z * s;
	R(0, 2) = (one_c * z * x) + y * s;
	R(1, 0) = (one_c * x * y) + z * s;
	R(1, 1) = (one_c * y * y) + c;
	R(1, 2) = (one_c * y * z) - x * s;
	R(2, 0) = (one_c * z * x) - y * s;
	R(2, 1) = (one_c * y * z) + x * s;
	R(2, 2) = (one_c * z * z) + c;
#undef R

	CurrentMatrix() = Multiply(CurrentMatrix(), r);
}

[[maybe_unused]] void MatPush()
{
	if (g_matrixMode == MODE_MODELVIEW)
		g_modelview.push_back(g_modelview.back());
}

[[maybe_unused]] void MatPop()
{
	if (g_matrixMode == MODE_MODELVIEW && g_modelview.size() > 1)
		g_modelview.pop_back();
}

constexpr float kFarClippingPlane = 32768.0f;
float g_nearClippingPlane = NZCLIP_PLANE;
const float g_fov = 90.0f;

// r_opengl.c's GLPerspective
void Perspective(float fovy, float aspect)
{
	float m[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.0f,
		0.0f, 0.0f, 0.0f, 0.0f,
	};
	const float zNear = g_nearClippingPlane;
	const float zFar = kFarClippingPlane;
	const float radians = (float)(fovy / 2.0f * M_PI / 180.0f);
	const float sine = (float)sin(radians);
	const float deltaZ = zFar - zNear;

	if ((fabsf(deltaZ) < 1.0E-36f) || sine == 0.0f || aspect == 0.0f)
		return;

	const float cotangent = cosf(radians) / sine;
	m[0] = cotangent / aspect;
	m[5] = cotangent;
	m[10] = -(zFar + zNear) / deltaZ;
	m[14] = -2.0f * zNear * zFar / deltaZ;
	MatMultiply(m);
}

// ==========================================================================
//                                                               OPENGL STATE
// ==========================================================================

enum BlendFactor : UINT8 { BF_ZERO, BF_ONE, BF_SRC_ALPHA, BF_ONE_MINUS_SRC_ALPHA, BF_SRC_COLOR, BF_DST_COLOR, BF_ONE_MINUS_DST_COLOR };
enum BlendEquation : UINT8 { BE_ADD, BE_SUBTRACT, BE_REVERSE_SUBTRACT };
enum AlphaFunc { AF_ALWAYS = 0, AF_GREATER = 1, AF_NOTEQUAL = 2 };
enum TexEnv { TE_MODULATE, TE_REPLACE };

// What r_opengl.c would have set
struct GLState
{
	UINT8 blendSrc = BF_ONE, blendDst = BF_ZERO, blendEquation = BE_ADD;
	bool blend = false;
	int alphaFunc = AF_ALWAYS;
	float alphaRef = 0.0f;
	bool alphaTest = false;
	bool polygonOffset = false;
	bool depthTest = false;
	bool depthAlways = false; // GL_ALWAYS rather than GL_LEQUAL
	bool depthMask = true;
#ifdef _UWP
	bool stencil = false;
	UINT8 stencilFunc = D3D12_COMPARISON_FUNC_EQUAL, stencilRef = 0;
	UINT8 stencilFail = D3D12_STENCIL_OP_KEEP, stencilDepthFail = D3D12_STENCIL_OP_KEEP;
	UINT8 stencilPass = D3D12_STENCIL_OP_KEEP, stencilReadMask = 255, stencilWriteMask = 255;
	UINT8 colorWriteMask = 7; // RGB; preserve the target's opaque alpha
	float clipPlane[4] = {0, 0, 0, 1}; // camera space, or an always-positive disabled plane
	float fullscreenDepth = 0;
#endif
	int texEnv = TE_MODULATE; // of texture unit 0
	bool texture2D = false; // texturing enabled on unit 0
	bool cullFace = false;
	bool cullFront = false;
	bool lighting = false; // GL_LIGHTING, for models without shaders
	float lightDir[3] = {0.0f, 1.0f, 0.0f}; // GL_LIGHT0's eye-space direction
	float material[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // the ambient and diffuse material
	UINT8 color[4] = {255, 255, 255, 255}; // glColor
	float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	int viewport[4] = {0, 0, 0, 0}; // x, y from the bottom (as in OpenGL), width, height
};
GLState g_gl;
FBITFIELD g_currentPolyFlags = 0;

RGBA_t g_palette[256];

void SetViewport(int x, int y, int width, int height)
{
	g_gl.viewport[0] = x;
	g_gl.viewport[1] = y;
	g_gl.viewport[2] = width;
	g_gl.viewport[3] = height;
}

void SetColor(const UINT8 *color)
{
	memcpy(g_gl.color, color, sizeof g_gl.color);
}

[[maybe_unused]] const UINT8 kWhite[4] = {255, 255, 255, 255};


struct ProgramUniforms
{
	float poly[4], tint[4], fade[4];
	float lighting, fadeStart, fadeEnd;
	float lightDir[3];
	float lightContrast, lightBacklight;
	float leveltime;
};
ProgramUniforms g_uniforms[NUMBASESHADERS];

hwdshaderoption_t g_allowShaders = HWD_SHADEROPTION_OFF; // gl_allowshaders
bool g_shadersEnabled = false; // gl_shadersenabled
bool g_modelLighting = false;
int g_shaderCurrent = -1; // gl_shaderstate.current, as its type, or -1 for NULL
int g_shaderType = 0; // gl_shaderstate.type
bool g_shaderChanged = false; // gl_shaderstate.changed
bool g_postImage = false;
bool g_waterRefraction = false;
bool g_fogRemap = false;
int g_program = -1; // the program glUseProgram last set, as its type, or -1 for none

INT32 g_shaderLeveltime = 0;
float g_shaderLightX = 0.0f, g_shaderLightY = 0.0f, g_shaderLightZ = 0.0f;
INT32 g_shaderLightContrast = 0, g_shaderLightBacklight = 0;


// Must match DrawConstants in shaders.hlsl
struct Constants
{
	float projection[16];
	float modelview[16];
	float poly[4];
	float tint[4];
	float fade[4];
	float color[4];
	float lightDir[4];
	float ffLight[4];
	float lighting, fadeStart, fadeEnd, lightContrast;
	float lightBacklight, leveltime, alphaRef;
	INT32 alphaFunc;
	INT32 texMode;
	INT32 palette; // palette rendering, with a light table
	INT32 pad[2];
#ifdef _UWP
	float clipPlane[4];
	float fullscreenDepth, portalPad[3];
	float postImageRect[4];
	float postImageInfo[4];
#endif
};
#ifdef _UWP
static_assert(sizeof(Constants) == 336, "Constants must match DrawConstants in shaders.hlsl");
#else
static_assert(sizeof(Constants) == 272, "Constants must match DrawConstants in shaders.hlsl");
#endif


bool ProgramExists(int type)
{
	// Add-on programs replace a base type; invalid replacements use the built-in shader.
	return type >= 0 && type < NUMBASESHADERS;
}
void SetUniforms(const FSurfaceInfo *surface, const float *poly, const float *tint, const float *fade)
{
	static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	if (!g_shadersEnabled || g_shaderCurrent < 0)
		return;

	if (!ProgramExists(g_shaderCurrent))
	{
		g_program = -1;
		return;
	}

	if (g_shaderChanged)
	{
		g_program = g_shaderCurrent;
		g_shaderChanged = false;
	}

	if (g_program < 0)
		return;

	ProgramUniforms &u = g_uniforms[g_program];
	boolean directional = false;

	memcpy(u.poly, poly ? poly : white, sizeof u.poly);
	memcpy(u.tint, tint ? tint : white, sizeof u.tint);
	memcpy(u.fade, fade ? fade : white, sizeof u.fade);

	if (surface != NULL)
	{
		u.lighting = (float)surface->LightInfo.light_level;
		u.fadeStart = (float)surface->LightInfo.fade_start;
		u.fadeEnd = (float)surface->LightInfo.fade_end;
		directional = surface->LightInfo.directional;
	}
	else
	{
		u.lighting = 255.0f;
		u.fadeStart = 0.0f;
		u.fadeEnd = 31.0f;
	}

	if (directional)
	{
		u.lightDir[0] = g_shaderLightX;
		u.lightDir[1] = g_shaderLightY;
		u.lightDir[2] = g_shaderLightZ;
		u.lightContrast = (float)g_shaderLightContrast;
		u.lightBacklight = (float)g_shaderLightBacklight;
	}
	else
	{
		u.lightDir[0] = u.lightDir[1] = u.lightDir[2] = 0.0f;
		u.lightContrast = u.lightBacklight = 0.0f;
	}

	u.leveltime = ((float)g_shaderLeveltime) / TICRATE;
}

enum Filter : UINT8 { FILTER_NEAREST, FILTER_LINEAR, FILTER_LINEAR_MIPMAP_LINEAR, FILTER_NEAREST_MIPMAP_LINEAR };

#include "d3d12_arenas.inc"

struct GpuTexture
{
	ComPtr<ID3D12Resource> res;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
	UINT srv = UINT32_MAX;
	UINT width = 0, height = 0, mips = 1;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT64 bytes = 0, allocatedBytes = 0;
	SizeClassArena::Block heapBlock; // a block of a shared texture heap, or
	ComPtr<ID3D12Heap> heap; // a heap of its own (see CreateDedicatedTexture), or neither when committed
};
IndexFreeList g_srvIndices;
UINT g_srvHighWater = 4;
UINT64 g_srvReuses = 0;
UINT64 g_textureAllocatedBytes = 0;
bool g_textureFlushRequested = false, g_budgetTextureFlush = false, g_textureFlushed = false;
precise_t g_lastTextureFlush = 0;
DXGI_QUERY_VIDEO_MEMORY_INFO g_localMemory = {};
bool g_localMemoryValid = false;
void RetireTexture(GpuTexture &texture);

// An OpenGL texture object
struct Texture : GpuTexture
{
	UINT32 id = 0;
	char probeName[9] = {}; // filled only during a requested surface probe

	// Its parameters, with OpenGL's defaults
	UINT8 minFilter = FILTER_NEAREST_MIPMAP_LINEAR, magFilter = FILTER_LINEAR;
	bool repeatS = true, repeatT = true;
	float maxLod = 1000.0f;
	int anisotropy = 1;
	UINT sampler = UINT32_MAX; // shared cache index; UINT32_MAX to look up again

	// In r_opengl.c's list of textures (TexCacheHead), which Flush deletes
	bool listed = false;
	GLMipmap_t *mipmap = nullptr;
	UINT32 listFormat = 0, listWidth = 0, listHeight = 0;
};

// Keyed by name, as in OpenGL. Pointers to them stay valid until they're erased.
std::unordered_map<UINT32, Texture> g_textures;
UINT32 g_nextTextureName = 1;
UINT64 g_textureBytes = 0;

// r_opengl.c's NOTEXTURE_NUM, the texture bound to stop texturing (it never gets any data)
constexpr UINT32 kNoTexture = 0xFFFFFFFFu;

UINT32 g_texDownloaded = 0; // r_opengl.c's tex_downloaded
Texture *g_units[2] = {nullptr, nullptr}; // what's bound to texture units 0 and 1

// Legacy GL's settings for new textures
UINT8 g_minFilter = FILTER_LINEAR, g_magFilter = FILTER_LINEAR;
bool g_mipmap = false;
int g_anisotropicFilter = 0;
constexpr int kMaximumAnisotropy = 16;

std::vector<RGBA_t> g_textureBuffer;

Texture *FindTexture(UINT32 name)
{
	auto it = g_textures.find(name);
	return it == g_textures.end() ? nullptr : &it->second;
}

UINT32 GenTexture()
{
	UINT32 name;

	do
	{
		name = g_nextTextureName++;
		if (g_nextTextureName == kNoTexture)
			g_nextTextureName = 1;
	} while (name == 0 || name == kNoTexture || g_textures.count(name));

	g_textures[name].id = name;
	return name;
}

void BindTexture(int unit, UINT32 name)
{
	g_units[unit] = (name == 0 || name == kNoTexture) ? nullptr : FindTexture(name);
}

// glDeleteTextures: what it's bound to reverts to no texture
void DeleteTextureObject(UINT32 name)
{
	auto it = g_textures.find(name);

	if (it == g_textures.end())
		return;

	for (Texture *&unit : g_units)
	{
		if (unit == &it->second)
			unit = nullptr;
	}

	if (it->second.res)
		g_stats.texturesDeleted++;
	RetireTexture(it->second);
	g_textures.erase(it);
}

void SetNoTexture(int unit)
{
	if (g_texDownloaded != kNoTexture)
	{
		BindTexture(unit, kNoTexture);
		g_texDownloaded = kNoTexture;
	}
}

// glTexParameteri on unit 0's texture
void SetRepeatS(bool repeat)
{
	if (g_units[0] && g_units[0]->repeatS != repeat)
	{
		g_units[0]->repeatS = repeat;
		g_units[0]->sampler = UINT32_MAX;
	}
}

void SetRepeatT(bool repeat)
{
	if (g_units[0] && g_units[0]->repeatT != repeat)
	{
		g_units[0]->repeatT = repeat;
		g_units[0]->sampler = UINT32_MAX;
	}
}


// Palette rendering, from SRB2: shaders turn texels back into palette indices with a lookup table, light them through
// the surface's light table, and the finished frame is turned into the screen's palette
bool g_paletteRendering = false;
GpuTexture g_paletteLookup; // Texture3D, indexed [b][g][r]
GpuTexture g_screenPalette; // 256x1: the palette the finished frame is shown in
GpuTexture g_masterPalette; // 256x1: uncorrected colors, for fade maps
RGBA_t g_screenPaletteColors[256];
std::unordered_map<UINT32, GpuTexture> g_lightTables;
// Unlikely values for the uninitialized LightTableIds of surfaces that never get one
constexpr UINT32 kFirstLightTableId = 0x4C540001;
UINT32 g_nextLightTableId = kFirstLightTableId;
GpuTexture *g_currentLightTable = nullptr;
// Where the finished frame is turned into the screen's palette, keeping the screen as drawn
GpuTexture g_postTexture;
UINT g_screenSrv = UINT32_MAX; // g_rt as a shader resource, for that last step
// The lookup table and light table last set at t2 and t3 on the open list, as descriptor handles
UINT64 g_boundPaletteTables[2] = {};
void Flush();
GpuTexture g_waterScene, g_waterSine, g_fogScene;
UploadAllocation g_waterConstants, g_fogConstants;
UINT64 g_testWaterCopies = 0, g_testFogCopies = 0, g_testWaterRoundTrips = 0;
UINT8 g_waterConstantData[32] = {}, g_fogConstantData[16] = {};
static bool g_waterCaptureGrouped = false, g_waterCaptureReady = false;
static D3D12_BOX g_waterCapturedBounds = {};
static void SetWaterSurface(INT32 state)
{
	g_waterCaptureGrouped = state != 0;
	if (state != 2) g_waterCaptureReady = false;
}
bool BindCaptureState();
void SetLightTable(UINT32 id)
{
	if (!id)
		return;
	auto it = g_lightTables.find(id);
	g_currentLightTable = it != g_lightTables.end() ? &it->second : nullptr;
}
struct CustomProgram
{
	ComPtr<ID3DBlob> vertex, pixel, refraction;
};
struct CustomShader
{
	std::string vertex, fragment;
	CustomProgram programs[NUMLAYOUTS];
	bool dirty = false, valid = false;
	UINT16 generation = 0;
};
CustomShader g_customShaders[NUMBASESHADERS];
UINT16 g_customGeneration = 0;
const CustomProgram *CustomDrawProgram(Layout layout)
{
	if (g_postImage || g_allowShaders != HWD_SHADEROPTION_ON || g_program < 0 || g_program >= NUMBASESHADERS
		|| layout >= NUMLAYOUTS)
		return nullptr;
	const CustomShader &shader = g_customShaders[g_program];
	return shader.valid ? &shader.programs[layout] : nullptr;
}
void SetBlendMode(FBITFIELD flags)
{
	// Set blending function
	if (flags & PF_Fog)
	{
		// Sryder: Fog
		// multiplies input colour by input alpha, and destination colour by input colour, then adds them
		g_gl.blendSrc = BF_SRC_ALPHA;
		g_gl.blendDst = BF_SRC_COLOR;
	}
	else
	{
		boolean blending = true;
		switch (flags & (PF_Blending & ~(PF_Masked | PF_Occlude)))
		{
			case PF_Translucent:
				g_gl.blendSrc = BF_SRC_ALPHA; // alpha = level of transparency
				g_gl.blendDst = BF_ONE_MINUS_SRC_ALPHA;
				break;
			case PF_Additive:
			case PF_Subtractive:
			case PF_ReverseSubtract:
				g_gl.blendSrc = BF_SRC_ALPHA; // src * alpha + dest
				g_gl.blendDst = BF_ONE;
				break;
			case PF_Environment:
				g_gl.blendSrc = BF_ONE;
				g_gl.blendDst = BF_ONE_MINUS_SRC_ALPHA;
				break;
			case PF_Multiplicative:
				g_gl.blendSrc = BF_DST_COLOR;
				g_gl.blendDst = BF_ZERO;
				break;
			case PF_Invert:
				// [UWP]: Unlike r_opengl.c, invert the destination for Encore's white quad.
				g_gl.blendSrc = BF_ONE_MINUS_DST_COLOR;
				g_gl.blendDst = BF_ZERO;
				break;
			default:
				blending = false;
				break;
		}

		if (!blending)
		{
			if (flags & PF_Masked)
			{
				g_gl.blendSrc = BF_SRC_ALPHA; // 0 alpha = holes in texture
				g_gl.blendDst = BF_ZERO;
			}
			else
			{
				g_gl.blendSrc = BF_ONE; // the same as no blending
				g_gl.blendDst = BF_ZERO;
			}
		}
	}

	// Set blending equation
	switch (flags & (PF_Blending & ~(PF_Masked | PF_Occlude)))
	{
		case PF_Subtractive & PF_Blending:
			g_gl.blendEquation = BE_SUBTRACT;
			break;
		case PF_ReverseSubtract & PF_Blending:
			g_gl.blendEquation = BE_REVERSE_SUBTRACT;
			break;
		default:
			g_gl.blendEquation = BE_ADD;
			break;
	}

	// Alpha test
	if (flags & PF_Fog)
	{
		g_gl.alphaFunc = AF_ALWAYS; // Don't discard zero alpha fragments
		g_gl.alphaRef = 0.0f;
	}
	else
	{
		switch (flags & (PF_Blending & ~(PF_Masked | PF_Occlude)))
		{
			case PF_Translucent:
			case PF_Additive:
			case PF_Subtractive:
			case PF_ReverseSubtract:
			case PF_Environment:
			case PF_Multiplicative:
			case PF_Invert:
				g_gl.alphaFunc = AF_NOTEQUAL;
				g_gl.alphaRef = 0.0f;
				break;
			default:
				g_gl.alphaFunc = AF_GREATER;
				g_gl.alphaRef = 0.5f;
				break;
		}
	}
}
void D3D_SetBlend(FBITFIELD);
void D3D_FlushScreenTextures();
void SetStates()
{
	g_gl.texture2D = true;
	g_gl.texEnv = TE_MODULATE;

	g_gl.alphaTest = true;
	g_gl.alphaFunc = AF_NOTEQUAL;
	g_gl.alphaRef = 0.0f;

	g_gl.blend = true;

	g_gl.depthTest = true;
	g_gl.depthMask = true;
	g_gl.depthAlways = false;

	// this set CurrentPolyFlags to the actual configuration
	g_currentPolyFlags = 0xffffffff;
	D3D_SetBlend(0);

	SetNoTexture(0);

	g_gl.lighting = false;

	// bp : when no t&l :)
	MatLoadIdentity();
	MatScale(1.0f, 1.0f, -1.0f);
}

// ==========================================================================
//                                                               RENDER TARGET
// ==========================================================================

constexpr DXGI_FORMAT kScreenFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // the swap chain's too (see d3d12_present.cpp)

ComPtr<ID3D12Resource> g_rt; // RENDER_TARGET, except while the present copy reads it
ComPtr<ID3D12Resource> g_depth; // DEPTH_WRITE
UINT g_rtWidth = 0, g_rtHeight = 0;
ComPtr<ID3D12Resource> g_readRectBuffer;
UINT64 g_readRectBytes = 0, g_readRectFence = 0;

// Shader-visible descriptors are persistent and never rewritten while in use.
ComPtr<ID3D12RootSignature> g_root;
ComPtr<ID3D12DescriptorHeap> g_srvHeap, g_samplerHeap;
ComPtr<ID3D12Resource> g_zeroConstants;
UploadRing g_vertexRing, g_indexRing, g_constantRing;
UploadRing g_textureRing;
GpuTexture g_blackTexture;
std::unordered_map<UINT32, UINT> g_samplers;
UINT g_nextSampler = 1;
UINT g_srvStep = 0, g_samplerStep = 0;
UINT64 g_boundTables[4] = {};
size_t g_testVertexBytes = 0, g_testIndexBytes = 0, g_testConstantBytes = 0;
Constants g_lastConstants = {};
bool g_constantsValid = false;
UploadAllocation g_constantAllocation, g_customViewConstants;
UINT64 g_customViewFrame = UINT64_MAX;
UINT g_customViewHeight = 0;
std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash> g_pipelines;
PipelineKey g_lastKey = {};
ID3D12PipelineState *g_lastPipeline = nullptr, *g_boundPipeline = nullptr;
// The topology, stencil reference and viewport last set on the open list; g_dynamicBound is false after it reopens
D3D12_PRIMITIVE_TOPOLOGY g_boundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
UINT g_boundStencilRef = 0;
int g_boundViewport[4] = {};
bool g_dynamicBound = false;
UINT64 g_psoCreated = 0;
double g_psoMs = 0, g_psoWorstMs = 0;

D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT index)
{
	auto handle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * g_srvStep;
	return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT index)
{
	auto handle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (UINT64)index * g_srvStep;
	return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpu(UINT index)
{
	auto handle = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (UINT64)index * g_samplerStep;
	return handle;
}
void Transition(GpuTexture &texture, D3D12_RESOURCE_STATES state)
{
	if (texture.state == state) return;
	Core_Transition(texture.res.Get(), texture.state, state);
	texture.state = state;
}
void RetireTexture(GpuTexture &texture)
{
	if (!texture.res) return;
	// Both the resource and its descriptor cover all work recorded so far, including this open list.
	Core_Retire(texture.res);
	Core_Retire(texture.heap);
	if (texture.heapBlock.valid)
	{
		g_textureArena.Free(texture.heapBlock, Core_PendingFence());
		g_pendingTextureBytes.emplace(Core_PendingFence(), texture.heapBlock.size);
		texture.heapBlock = {};
	}
	if (texture.srv >= 4 && texture.srv != UINT32_MAX)
		g_srvIndices.Free(texture.srv, Core_PendingFence());
	g_textureBytes -= texture.bytes;
	g_textureAllocatedBytes -= texture.allocatedBytes;
	texture.srv = UINT32_MAX;
	texture.bytes = texture.allocatedBytes = 0;
}
UINT FullMipCount(UINT width, UINT height)
{
	UINT count = 1;
	while (width > 1 || height > 1) { width = std::max(1u, width / 2); height = std::max(1u, height / 2); ++count; }
	return count;
}
bool UploadTexture(GpuTexture &texture, const void *data)
{
	std::vector<MipLevelRGBA8> chain;
	if (texture.mips > 1)
	{
		LARGE_INTEGER frequency, begin, end;
		QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
		chain = GenerateMipChainRGBA8(static_cast<const uint8_t *>(data), texture.width, texture.height);
		QueryPerformanceCounter(&end);
		g_stats.mipMs += (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart;
		g_stats.mipsGenerated += chain.size() - 1;
	}
	const auto desc = texture.res->GetDesc();
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(texture.mips);
	std::vector<UINT> rows(texture.mips);
	std::vector<UINT64> rowBytes(texture.mips);
	UINT64 bytes = 0;
	Core_Device()->GetCopyableFootprints(&desc, 0, texture.mips, 0, footprints.data(), rows.data(), rowBytes.data(), &bytes);
	// Alloc can submit/reopen the list. Do it before recording transitions.
	auto allocation = g_textureRing.Alloc((size_t)bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	if (!allocation) return false;
	auto *dst = static_cast<UINT8 *>(allocation.cpu);
	for (UINT mip = 0; mip < texture.mips; ++mip)
	{
		const auto *src = chain.empty() ? static_cast<const UINT8 *>(data) : chain[mip].pixels.data();
		for (UINT row = 0; row < rows[mip]; ++row)
			memcpy(dst + footprints[mip].Offset + (size_t)row * footprints[mip].Footprint.RowPitch,
				src + (size_t)row * rowBytes[mip], (size_t)rowBytes[mip]);
		footprints[mip].Offset += allocation.offset;
	}
	Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
	Core_FlushBarriers();
	for (UINT mip = 0; mip < texture.mips; ++mip)
	{
		D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
		from.pResource = g_textureRing.Resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		from.PlacedFootprint = footprints[mip];
		to.pResource = texture.res.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = mip;
		Core_List()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
	}
	Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}
// [UWP]: The Xbox One driver keeps a 4-byte state word for each buffer and each
// non-render-target texture at offset + align_up(end of its data, 256), which
// GetResourceAllocationInfo doesn't count. When the data fills the reported size
// (such as 256x256 RGBA8 with one mip), the word lands on whatever follows the
// resource: CreatePlacedResource writes 0 there and each later transition writes
// 0 or 1. In the diagnostic app that established this, committed textures of such
// sizes instead lost their last texel to the word, so committed storage isn't a
// safe fallback either: a 1024x1024 texture would come back with its bottom-right
// texel zeroed. Every texture is therefore placed with room after its data:
// - shared heaps, with kTextureGuardBytes after each block (CreateTextureResource);
// - a heap of its own with a spare 64 KB page, when the shared heaps can't take it
//   (CreateDedicatedTexture).
// Committed textures remain only as a last resort for when even that fails.
constexpr UINT64 kTextureGuardBytes = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr UINT64 kDedicatedTexturePadBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
// A texture at offset 0 of a heap of its own, sized for the reported allocation plus a spare page.
bool CreateDedicatedTexture(GpuTexture &texture, const D3D12_RESOURCE_DESC &desc, const D3D12_RESOURCE_ALLOCATION_INFO &info)
{
	if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || !info.Alignment || info.Alignment > kDedicatedTexturePadBytes) return false;
	D3D12_HEAP_DESC heapDesc = {};
	heapDesc.SizeInBytes = (info.SizeInBytes + kDedicatedTexturePadBytes - 1) / kDedicatedTexturePadBytes * kDedicatedTexturePadBytes
		+ kDedicatedTexturePadBytes;
	heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapDesc.Alignment = kDedicatedTexturePadBytes;
	heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
	ComPtr<ID3D12Heap> heap;
	if (FAILED(Core_Device()->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return false;
	if (FAILED(Core_Device()->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&texture.res))))
	{
		texture.res.Reset();
		return false;
	}
	heap->SetName(L"dedicated texture heap");
	texture.heap = heap;
	texture.allocatedBytes = heapDesc.SizeInBytes;
	++g_dedicatedCreated;
	return true;
}
// Heaps remain alive until shutdown, including while freed blocks await a fence.
// Records the aliasing barrier a placed texture needs, so the list must be open.
bool CreateTextureResource(GpuTexture &texture, D3D12_RESOURCE_DESC desc)
{
	CollectTextureBlocks();
	// Small alignment requires the top mip's tiled footprint to fit in 64 KB.
	// Thin textures can exceed sixteen 4 KB tiles even when their raw bytes fit.
	const UINT tileSize = desc.Format == DXGI_FORMAT_R8_UNORM ? 64
		: (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R32_SINT) ? 32 : 0;
	const bool smallEligible = tileSize && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
		&& desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1
		&& desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN
		&& !(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
		&& ((desc.Width + tileSize - 1) / tileSize) * (((UINT64)desc.Height + tileSize - 1) / tileSize) <= 16;
	D3D12_RESOURCE_ALLOCATION_INFO info = {};
	bool smallPlacement = false;
	if (smallEligible)
	{
		desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
		info = Core_Device()->GetResourceAllocationInfo(0, 1, &desc);
		smallPlacement = info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT && info.SizeInBytes != UINT64_MAX;
	}
	if (!smallPlacement) { desc.Alignment = 0; info = Core_Device()->GetResourceAllocationInfo(0, 1, &desc); }
	// The guard goes after the reported size; alignment rounds it up to the next 4 KB or 64 KB boundary anyway.
	const size_t blockBytes = info.SizeInBytes && info.SizeInBytes <= SizeClassArena::MaxClass - kTextureGuardBytes
		&& (info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
			|| info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
		? (size_t)(info.SizeInBytes + kTextureGuardBytes) : 0;
	if (blockBytes)
	{
		auto block = g_textureArena.Allocate(blockBytes, (size_t)info.Alignment);
		if (block.needsChunk)
		{
			D3D12_HEAP_DESC heapDesc = {}; heapDesc.SizeInBytes = kTextureHeapBytes;
			heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
			ComPtr<ID3D12Heap> heap;
			const HRESULT hr = Core_Device()->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap));
			if (SUCCEEDED(hr))
			{
				heap->SetName(L"shared texture placement heap");
				g_textureArena.AddChunk();
				g_textureHeaps.push_back(heap);
				Log("texture heap #%u (16 MB)", (unsigned)g_textureHeaps.size());
				block = g_textureArena.Allocate(blockBytes, (size_t)info.Alignment);
			}
		}
		if (block.valid)
		{
			const HRESULT hr = Core_Device()->CreatePlacedResource(g_textureHeaps[block.chunk].Get(), block.offset, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.res));
			if (SUCCEEDED(hr))
			{
				texture.heapBlock = block; texture.allocatedBytes = block.size;
				g_textureHeapUsed += block.size; g_textureHeapPeak = std::max(g_textureHeapPeak, g_textureHeapUsed);
				++g_placedCreated; if (smallPlacement) ++g_smallPlaced; else ++g_normalPlaced;
				// The block may have held another texture whose work is done.
				Core_FlushBarriers();
				D3D12_RESOURCE_BARRIER alias = {}; alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
				alias.Aliasing.pResourceAfter = texture.res.Get();
				Core_List()->ResourceBarrier(1, &alias);
				return true;
			}
			texture.res.Reset();
			g_textureArena.Free(block, Core_CompletedFence());
		}
	}
	// Over the arena's largest class, or placement failed: a padded heap of its own (see above).
	desc.Alignment = 0;
	if (!blockBytes || info.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
		info = Core_Device()->GetResourceAllocationInfo(0, 1, &desc);
	if (CreateDedicatedTexture(texture, desc, info)) return true;
	// Last resort; on Xbox One the driver's state word can zero this texture's last texel.
	D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.res)), "Create committed texture")) return false;
	texture.allocatedBytes = info.SizeInBytes;
	if (!g_committedCreated++)
		Log("committed texture: %llux%u could not be placed; its last texel is unreliable on Xbox One",
			(unsigned long long)desc.Width, (unsigned)desc.Height);
	return true;
}
// A persistent SRV index, or UINT32_MAX (after asking for a texture flush) if there are none
UINT AllocateSrv()
{
	g_srvIndices.Collect(Core_CompletedFence());
	const UINT index = g_srvIndices.Allocate();
	if (index == UINT32_MAX)
	{
		D3D12_LOG_ONCE("persistent SRV heap exhausted; requesting Legacy GL texture flush");
		g_textureFlushRequested = true;
		return index;
	}
	if (index < g_srvHighWater) ++g_srvReuses;
	g_srvHighWater = std::max(g_srvHighWater, index + 1);
	return index;
}
bool CreateTextureStorage(GpuTexture *texture, UINT id, UINT width, UINT height, DXGI_FORMAT format,
	bool mipmapped, const void *data, bool black = false)
{
	const UINT index = black ? 3 : AllocateSrv();
	if (index == UINT32_MAX)
		return false;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
	desc.MipLevels = (UINT16)(mipmapped ? FullMipCount(width, height) : 1);
	desc.Format = format; desc.SampleDesc.Count = 1;
	GpuTexture replacement; replacement.srv = index; replacement.mips = desc.MipLevels;
	LARGE_INTEGER frequency, begin, end;
	QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
	if (!CreateTextureResource(replacement, desc))
	{
		if (!black)
			g_srvIndices.Free(index, Core_PendingFence());
		return false;
	}
	wchar_t name[64]; swprintf(name, sizeof name / sizeof *name, L"texture %u", id);
	replacement.res->SetName(name);
	replacement.width = width; replacement.height = height; replacement.format = format;
	for (UINT mip = 0, w = width, h = height; mip < replacement.mips; ++mip, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
		replacement.bytes += (UINT64)w * h * (format == DXGI_FORMAT_R8_UNORM ? 1 : 4);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Format = format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = replacement.mips;
	Core_Device()->CreateShaderResourceView(replacement.res.Get(), &srv, SrvCpu(index));
	QueryPerformanceCounter(&end);
	g_stats.textureCreateMs += (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart;
	if (data && !UploadTexture(replacement, data))
	{
		Core_Retire(replacement.res);
		Core_Retire(replacement.heap);
		if (replacement.heapBlock.valid)
		{
			g_textureArena.Free(replacement.heapBlock, Core_PendingFence());
			g_pendingTextureBytes.emplace(Core_PendingFence(), replacement.heapBlock.size);
		}
		if (!black)
			g_srvIndices.Free(index, Core_PendingFence());
		return false;
	}
	if (!data) Transition(replacement, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	RetireTexture(*texture); *texture = replacement;
	g_textureBytes += texture->bytes; g_textureAllocatedBytes += texture->allocatedBytes;
	g_stats.texturesCreated++;
	return true;
}

UINT GetSampler(Texture *texture)
{
	if (texture->sampler != UINT32_MAX)
		return texture->sampler;

	const UINT32 lod = texture->maxLod >= 15.0f ? 15 : (UINT32)texture->maxLod;
	const UINT32 anisotropy = (UINT32)std::min(std::max(texture->anisotropy, 1), kMaximumAnisotropy);
	const UINT32 key = texture->minFilter | (texture->magFilter << 2) | (texture->repeatS ? 1 << 4 : 0)
		| (texture->repeatT ? 1 << 5 : 0) | (lod << 6) | (anisotropy << 10);

	auto it = g_samplers.find(key);
	if (it != g_samplers.end())
		return texture->sampler = it->second;

	const bool mipmap = texture->minFilter == FILTER_LINEAR_MIPMAP_LINEAR || texture->minFilter == FILTER_NEAREST_MIPMAP_LINEAR;
	const bool minLinear = texture->minFilter == FILTER_LINEAR || texture->minFilter == FILTER_LINEAR_MIPMAP_LINEAR;
	const bool magLinear = texture->magFilter == FILTER_LINEAR;
	D3D12_SAMPLER_DESC desc = {};

	if (anisotropy > 1 && texture->minFilter != FILTER_NEAREST)
	{
		desc.Filter = D3D12_FILTER_ANISOTROPIC;
		desc.MaxAnisotropy = anisotropy;
	}
	else
	{
		static const D3D12_FILTER filters[2][2][2] = // [minLinear][magLinear][mipmap]
		{
			{
				{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR},
				{D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR},
			},
			{
				{D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
				{D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR},
			},
		};
		desc.Filter = filters[minLinear][magLinear][mipmap];
		desc.MaxAnisotropy = 1;
	}

	desc.AddressU = texture->repeatS ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.AddressV = texture->repeatT ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.MinLOD = 0.0f;
	// Without a mipmap filter, OpenGL samples only the base level
	desc.MaxLOD = mipmap ? (float)lod : 0.0f;
	if (mipmap && lod == 15)
		desc.MaxLOD = D3D12_FLOAT32_MAX;

	if (g_nextSampler >= 2048)
	{
		D3D12_LOG_ONCE("persistent sampler heap exhausted; using default sampler");
		return 0;
	}
	const UINT index = g_nextSampler++;
	auto handle = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * g_samplerStep;
	Core_Device()->CreateSampler(&desc, handle);
	g_samplers[key] = index;
	return texture->sampler = index;
}

// r_opengl.c's UpdateTexture
void UpdateTextureInternal(GLMipmap_t *pTexInfo)
{
	UINT32 num = pTexInfo->downloaded;
	const INT32 w = pTexInfo->width, h = pTexInfo->height;
	const UINT8 *pImgData = (const UINT8 *)pTexInfo->data;
	const void *ptex = NULL;

	// Generate a new texture name.
	if (!num || !FindTexture(num))
	{
		num = GenTexture();
		pTexInfo->downloaded = num;
	}

	if (w > 0 && h > 0 && pImgData)
	{
		if ((pTexInfo->format == GL_TEXFMT_P_8) || (pTexInfo->format == GL_TEXFMT_AP_88))
		{
			g_textureBuffer.resize((size_t)w * h);
			RGBA_t *tex = g_textureBuffer.data();
			ptex = tex;

			for (INT32 j = 0; j < h; j++)
			{
				for (INT32 i = 0; i < w; i++)
				{
					if ((*pImgData == HWR_PATCHES_CHROMAKEY_COLORINDEX) && (pTexInfo->flags & TF_CHROMAKEYED))
					{
						tex[w*j+i].rgba = 0;
						pTexInfo->flags |= TF_TRANSPARENT; // there is a hole in it
					}
					else
						tex[w*j+i] = g_palette[*pImgData];

					pImgData++;

					if (pTexInfo->format == GL_TEXFMT_AP_88)
					{
						if (!(pTexInfo->flags & TF_CHROMAKEYED))
							tex[w*j+i].s.alpha = *pImgData;
						pImgData++;
					}
				}
			}
		}
		else if (pTexInfo->format == GL_TEXFMT_RGBA)
		{
			// Directly upload the texture data without any kind of conversion.
			ptex = pImgData;
		}
		else if (pTexInfo->format == GL_TEXFMT_ALPHA_INTENSITY_88)
		{
			g_textureBuffer.resize((size_t)w * h);
			RGBA_t *tex = g_textureBuffer.data();
			ptex = tex;

			for (INT32 j = 0; j < h; j++)
			{
				for (INT32 i = 0; i < w; i++)
				{
					tex[w*j+i].s.red = tex[w*j+i].s.green = tex[w*j+i].s.blue = *pImgData++;
					tex[w*j+i].s.alpha = *pImgData++;
				}
			}
		}
		else if (pTexInfo->format == GL_TEXFMT_ALPHA_8) // Used for fade masks
		{
			g_textureBuffer.resize((size_t)w * h);
			RGBA_t *tex = g_textureBuffer.data();
			ptex = tex;

			for (INT32 j = 0; j < h; j++)
			{
				for (INT32 i = 0; i < w; i++)
				{
					// 255 because the fade mask is modulated with the screen texture, so alpha affects it while the colours don't
					tex[w*j+i].s.red = tex[w*j+i].s.green = tex[w*j+i].s.blue = 255;
					tex[w*j+i].s.alpha = *pImgData++;
				}
			}
		}
		else
			D3D12_LOG_ONCE("UpdateTexture: bad format %d (logged once)", (int)pTexInfo->format);
	}

	if (!(pTexInfo->flags & TF_BRIGHTMAP))
	{
		g_texDownloaded = 0; // force update
		SetNoTexture(1); // will be assigned later, if needed
	}

	const int unit = (pTexInfo->flags & TF_BRIGHTMAP) ? 1 : 0;
	BindTexture(unit, num);
	g_texDownloaded = num;

	Texture *texture = FindTexture(num);

	// disable texture filtering on any texture that has holes so there's no dumb borders or blending issues
	if (pTexInfo->flags & TF_TRANSPARENT)
	{
		texture->magFilter = FILTER_NEAREST;
		texture->minFilter = FILTER_NEAREST;
	}
	else
	{
		texture->magFilter = g_magFilter;
		texture->minFilter = g_minFilter;
	}

	if (ptex)
	{
		const bool mipmapped = g_mipmap;

		bool uploaded;
		if (texture->res && texture->width == (UINT)w && texture->height == (UINT)h
			&& texture->mips == (mipmapped ? FullMipCount(w, h) : 1))
			uploaded = UploadTexture(*texture, ptex);
		else
			uploaded = CreateTextureStorage(texture, texture->id, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, mipmapped, ptex);

		if (uploaded)
		{
			g_stats.textureUploads++;
			g_stats.textureUploadBytes += texture->bytes;
		}

		if (mipmapped)
		{
			if (pTexInfo->flags & TF_TRANSPARENT)
				texture->maxLod = 0.0f; // No mippmaps on transparent stuff
			else
				texture->maxLod = (pTexInfo->format == GL_TEXFMT_ALPHA_INTENSITY_88 || pTexInfo->format == GL_TEXFMT_ALPHA_8) ? 4.0f : 5.0f;
		}
	}

	texture->repeatS = (pTexInfo->flags & TF_WRAPX) != 0;
	texture->repeatT = (pTexInfo->flags & TF_WRAPY) != 0;
	texture->anisotropy = g_anisotropicFilter;
	texture->sampler = UINT32_MAX;
}

// r_opengl.c's SetTexture
void SetTextureInternal(GLMipmap_t *pTexInfo)
{
	if (!pTexInfo)
	{
		SetNoTexture(0);
		return;
	}
	else if (pTexInfo->downloaded)
	{
		if (pTexInfo->downloaded != g_texDownloaded)
		{
			if (!(pTexInfo->flags & TF_BRIGHTMAP))
			{
				g_texDownloaded = 0; // force update
				SetNoTexture(1); // will be assigned later, if needed
			}

			BindTexture((pTexInfo->flags & TF_BRIGHTMAP) ? 1 : 0, pTexInfo->downloaded);
			g_texDownloaded = pTexInfo->downloaded;
		}
	}
	else
	{
		UpdateTextureInternal(pTexInfo);

		Texture *texture = FindTexture(pTexInfo->downloaded);
		if (texture)
		{
			texture->listed = true;
			texture->mipmap = pTexInfo;
			texture->listFormat = pTexInfo->format;
			texture->listWidth = pTexInfo->width;
			texture->listHeight = pTexInfo->height;
		}
	}
}

// r_opengl.c's Flush
void Flush()
{
	std::vector<UINT32> names;

	for (auto &entry : g_textures)
	{
		if (entry.second.listed)
			names.push_back(entry.first);
	}

	for (UINT32 name : names)
	{
		Texture *texture = FindTexture(name);
		if (texture->mipmap)
			texture->mipmap->downloaded = 0;
		DeleteTextureObject(name);
	}

	g_texDownloaded = 0;
	g_textureBuffer.clear();
	g_textureBuffer.shrink_to_fit();
	g_stats.flushes++;
	if (g_budgetTextureFlush)
	{
		Log("over the video memory budget (usage %.1f MB, budget %.1f MB); flushed Legacy GL's textures",
			g_localMemory.CurrentUsage / 1048576.0, g_localMemory.Budget / 1048576.0);
		g_budgetTextureFlush = false;
	}
}


bool InitDrawing()
{
	ID3D12Device *device = Core_Device();
	if (Core_Failed(device->CreateRootSignature(0, g_RootSignature, sizeof g_RootSignature,
		IID_PPV_ARGS(&g_root)), "CreateRootSignature")) return false;
	g_root->SetName(L"Legacy GL root signature");
	D3D12_DESCRIPTOR_HEAP_DESC heap = {};
	heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap.NumDescriptors = 65536;
	heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (Core_Failed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_srvHeap)), "Create SRV heap")) return false;
	g_srvHeap->SetName(L"persistent SRV heap");
	heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heap.NumDescriptors = 2048;
	if (Core_Failed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_samplerHeap)), "Create sampler heap")) return false;
	g_samplerHeap->SetName(L"persistent sampler heap");
	g_srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	g_samplerStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	auto handle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
	const UINT step = g_srvStep;
	device->CreateShaderResourceView(nullptr, &srv, handle);
	handle.ptr += step;
	srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_R8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
	srv.Texture3D.MipLevels = 1;
	device->CreateShaderResourceView(nullptr, &srv, handle);
	handle.ptr += step;
	srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_R32_SINT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(nullptr, &srv, handle);
	g_srvIndices.Reset(4, 65536 - 4);
	InitArenas();
	g_samplers.clear(); g_nextSampler = 1;
	D3D12_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	device->CreateSampler(&sampler, g_samplerHeap->GetCPUDescriptorHandleForHeapStart());
	D3D12_HEAP_PROPERTIES upload = {};
	upload.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC buffer = {};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = 1024; // Covers every unused b1-b5, rounded to 256 bytes.
	buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (Core_Failed(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_zeroConstants)), "Create zero constants")) return false;
	g_zeroConstants->SetName(L"zero constants b0-b5");
	void *cpu = nullptr;
	const D3D12_RANGE none = {0, 0};
	if (Core_Failed(g_zeroConstants->Map(0, &none, &cpu), "Map zero constants")) return false;
	memset(cpu, 0, 1024);
	g_zeroConstants->Unmap(0, nullptr);
	if (!g_textureRing.Init(32 << 20, L"texture staging ring")) return false;
	const UINT32 black = 0xFF000000; // OpenGL's incomplete texture: (0, 0, 0, 1)
	if (!CreateTextureStorage(&g_blackTexture, 0, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, &black, true)) return false;
	return g_vertexRing.Init(g_testVertexBytes ? g_testVertexBytes : 16 << 20, L"vertex ring") && g_indexRing.Init(g_testIndexBytes ? g_testIndexBytes : 8 << 20, L"index ring")
		&& g_constantRing.Init(g_testConstantBytes ? g_testConstantBytes : 4 << 20, L"constant ring");
}

void BindViewport()
{
	const D3D12_VIEWPORT viewport = {(float)g_gl.viewport[0],
		(float)((int)g_rtHeight - (g_gl.viewport[1] + g_gl.viewport[3])),
		(float)std::max(g_gl.viewport[2], 0), (float)std::max(g_gl.viewport[3], 0), 0, 1};
	Core_List()->RSSetViewports(1, &viewport);
	// The former Direct3D 11 driver's rasterizer left ScissorEnable false.
	const D3D12_RECT scissor = {0, 0, (LONG)g_rtWidth, (LONG)g_rtHeight};
	Core_List()->RSSetScissorRects(1, &scissor);
}

int ScreenTextureSize();

bool ApplyConstants(bool custom)
{
	Constants c = {};
	memcpy(c.projection, g_projection.m, sizeof c.projection);
	memcpy(c.modelview, g_modelview.back().m, sizeof c.modelview);
	memcpy(c.clipPlane, g_gl.clipPlane, sizeof c.clipPlane);
	c.fullscreenDepth = g_gl.fullscreenDepth;
	if (g_postImage)
	{
		// The saved screen contains all views; the shader clamps sampling to this view
		const float size = (float)ScreenTextureSize();
		c.postImageRect[0] = g_gl.viewport[0] / size;
		c.postImageRect[1] = (g_screenHeight - g_gl.viewport[1] - g_gl.viewport[3]) / size;
		c.postImageRect[2] = (g_gl.viewport[0] + g_gl.viewport[2]) / size;
		c.postImageRect[3] = (g_screenHeight - g_gl.viewport[1]) / size;
		c.postImageInfo[0] = (float)leveltime;
		c.postImageInfo[1] = (float)postimgtype[R_GetViewNumber()];
		c.postImageInfo[2] = g_screenWidth / size;
		c.postImageInfo[3] = g_screenHeight / size;
	}
	if (g_program >= 0)
	{
		const ProgramUniforms &u = g_uniforms[g_program];
		memcpy(c.poly, u.poly, sizeof c.poly);
		memcpy(c.tint, u.tint, sizeof c.tint);
		memcpy(c.fade, u.fade, sizeof c.fade);
		memcpy(c.lightDir, u.lightDir, sizeof u.lightDir);
		c.lighting = u.lighting; c.fadeStart = u.fadeStart; c.fadeEnd = u.fadeEnd;
		c.lightContrast = u.lightContrast; c.lightBacklight = u.lightBacklight; c.leveltime = u.leveltime;
	}
	if (g_gl.lighting)
	{
		memcpy(c.color, g_gl.material, sizeof c.color);
		memcpy(c.ffLight, g_gl.lightDir, sizeof g_gl.lightDir);
		c.ffLight[3] = 1;
	}
	else for (int i = 0; i < 4; i++) c.color[i] = g_gl.color[i] / 255.0f;
	c.alphaFunc = g_gl.alphaTest ? g_gl.alphaFunc : AF_ALWAYS;
	c.alphaRef = g_gl.alphaRef;
	c.texMode = g_gl.texture2D && g_units[0] && g_units[0]->res
		? (g_gl.texEnv == TE_MODULATE ? 1 : 2) : 0;
	c.palette = (g_paletteRendering && g_paletteLookup.res && g_currentLightTable) ? 1 : 0;
	const bool updateView = custom && (!g_customViewConstants || g_customViewFrame != g_hueFrame
		|| g_customViewHeight != g_rtHeight || g_customViewConstants.generation != g_constantRing.Generation());
	if (updateView || g_waterRefraction || g_fogRemap || !g_constantsValid || g_constantAllocation.generation != g_constantRing.Generation()
		|| memcmp(&c, &g_lastConstants, sizeof c) != 0)
	{
		// Reserve b0, capture and optional b4 together: Alloc can wrap and reopen the list.
		const size_t viewOffset = (g_waterRefraction || g_fogRemap) ? 768 : 512;
		auto slot = g_constantRing.Alloc(viewOffset + (custom ? 256 : 0), 256);
		if (!slot.cpu) return false;
		memcpy(slot.cpu, &c, sizeof c);
		memset(static_cast<UINT8 *>(slot.cpu) + sizeof c, 0, 512 - sizeof c);
		if (g_waterRefraction || g_fogRemap)
		{
			UploadAllocation capture = {static_cast<UINT8 *>(slot.cpu) + 512, slot.gpu + 512, slot.offset + 512, slot.generation};
			memset(capture.cpu, 0, 256);
			memcpy(capture.cpu, g_waterRefraction ? g_waterConstantData : g_fogConstantData, g_waterRefraction ? 32 : 16);
			(g_waterRefraction ? g_waterConstants : g_fogConstants) = capture;
		}
		if (custom && (updateView || g_customViewConstants.generation != slot.generation))
		{
			g_customViewConstants = {static_cast<UINT8 *>(slot.cpu) + viewOffset, slot.gpu + viewOffset,
				slot.offset + viewOffset, slot.generation};
			const float view[4] = {(float)g_rtHeight, 0, 0, 0};
			memset(g_customViewConstants.cpu, 0, 256);
			memcpy(g_customViewConstants.cpu, view, sizeof view);
			g_customViewFrame = g_hueFrame; g_customViewHeight = g_rtHeight;
			g_stats.constantUpdates++;
		}
		g_constantAllocation = slot;
		g_lastConstants = c;
		g_constantsValid = true;
		g_stats.constantUpdates++;
	}
	if (custom)
	{
		if (!g_constantRing.Touch(g_customViewConstants, 256)) return false;
		Core_List()->SetGraphicsRootConstantBufferView(17, g_customViewConstants.gpu);
		g_stats.rootSets++;
	}
	Core_List()->SetGraphicsRootConstantBufferView(0, g_constantAllocation.gpu);
	g_stats.rootSets++;
	return true;
}

// The pipeline for key, made on first use. A failure is cached too, so it isn't retried every draw.
ID3D12PipelineState *FindOrCreatePipeline(const PipelineKey &key)
{
	auto found = g_pipelines.find(key);
	if (found == g_pipelines.end())
	{
		D3D12_SHADER_BYTECODE vertex = {}, pixel = {};
		if (key.custom)
		{
			const int type = key.vs - NUMVS;
			if (type <= SHADER_DEFAULT || type >= NUMBASESHADERS || key.layout >= NUMLAYOUTS
				|| (key.ps != NUMPS + type * 2 && key.ps != NUMPS + type * 2 + 1) || key.custom != g_customGeneration)
				return nullptr;
			const CustomProgram &program = g_customShaders[type].programs[key.layout];
			const auto &ps = (key.ps - NUMPS) % 2 ? program.refraction : program.pixel;
			if (!program.vertex || !ps) return nullptr;
			vertex = {program.vertex->GetBufferPointer(), program.vertex->GetBufferSize()};
			pixel = {ps->GetBufferPointer(), ps->GetBufferSize()};
		}
		const auto desc = BuildPipelineDesc(g_root.Get(), key, &vertex, &pixel);
		ComPtr<ID3D12PipelineState> created;
		LARGE_INTEGER frequency = {}, begin = {}, end = {};
		QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
		const HRESULT hr = Core_Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&created));
		QueryPerformanceCounter(&end);
		const double ms = frequency.QuadPart > 0 ? (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart : 0;
		g_psoCreated++; g_psoMs += ms; g_psoWorstMs = std::max(g_psoWorstMs, ms);
		char customVs[64] = {}, customPs[64] = {};
		if (key.custom)
		{
			snprintf(customVs, sizeof customVs, "custom VS (shader %u)", (unsigned)(key.vs - NUMVS));
			snprintf(customPs, sizeof customPs, "custom PS (shader %u%s)", (unsigned)(key.vs - NUMVS),
				(key.ps - NUMPS) % 2 ? " refraction" : "");
		}
		if (g_psoCreated <= 64 || key.custom)
			Log("PSO #%u %s+%s layout %u topology %u blend 0x%X depth 0x%llX raster 0x%X: %.1f ms (prewarm %s)",
				(unsigned)g_psoCreated, key.custom ? customVs : ShaderName((VSKind)key.vs), key.custom ? customPs : ShaderName((PSKind)key.ps), key.layout, key.topologyType,
				key.blend, (unsigned long long)key.depth, key.raster, ms, Prewarm_Done() ? "done" : "running");
		if (Core_Failed(hr, "CreateGraphicsPipelineState")) created.Reset();
		else created->SetName(L"Legacy GL cached pipeline");
		found = g_pipelines.emplace(key, created).first;
	}
	return found->second.Get();
}

// Shared by normal draws and portal preflight, including the fullscreen depth reset.
PipelineKey StatePipelineKey(Layout layout, VSKind vs, PSKind ps, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	PipelineKey key = {};
	key.vs = vs;
	key.ps = ps;
	key.layout = layout;
	key.topologyType = topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
		: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	key.rtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	key.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	if (g_gl.blend && !g_waterRefraction && !g_fogRemap
		&& !(g_gl.blendSrc == BF_ONE && g_gl.blendDst == BF_ZERO && g_gl.blendEquation == BE_ADD))
		key.blend = 1 | (g_gl.blendSrc << 1) | (g_gl.blendDst << 4) | (g_gl.blendEquation << 7);
	key.blend |= g_gl.colorWriteMask << 10;
	key.depth = (g_gl.depthTest ? 1 : 0) | (g_gl.depthAlways ? 2 : 0) | (g_gl.depthMask ? 4 : 0)
		| ((UINT64)g_gl.stencil << 3) | ((UINT64)g_gl.stencilFunc << 4)
		| ((UINT64)g_gl.stencilFail << 8) | ((UINT64)g_gl.stencilDepthFail << 12)
		| ((UINT64)g_gl.stencilPass << 16) | ((UINT64)g_gl.stencilReadMask << 20)
		| ((UINT64)g_gl.stencilWriteMask << 28);
	key.raster = (g_gl.cullFace ? (g_gl.cullFront ? 2 : 1) : 0) | (g_gl.polygonOffset ? 4 : 0);
	return key;
}

// Same shader selector as the former Direct3D 11 driver, with add-on bytecode in the cached PSO.
// Whether anything besides a clear has been drawn since the driver started
bool g_drewSinceStart = false;

bool ApplyState(Layout layout, D3D12_PRIMITIVE_TOPOLOGY topology, bool wipe = false)
{
	g_drewSinceStart = true;
	VSKind vs;
	PSKind ps;
	if (g_program >= 0)
	{
		switch (g_program)
		{
			case SHADER_DEFAULT: ps = PS_DEFAULT; break;
			case SHADER_MODEL_LIGHTING: ps = PS_MODELLIGHT; break;
			case SHADER_WATER: ps = g_waterRefraction ? PS_WATERREFRACTION : PS_WATER; break;
			case SHADER_FOG: ps = g_fogRemap ? PS_FOGREMAP : PS_FOG; break;
			case SHADER_SKY: ps = PS_SKY; break;
			default: ps = PS_SOFTWARE; break;
		}
		vs = layout == LAYOUT_SKY ? VS_SKY : layout == LAYOUT_MODEL
			? (g_program == SHADER_MODEL_LIGHTING ? VS_MODELLIGHT : VS_MODEL)
			: (g_program == SHADER_SPRITECLIPHACK ? VS_POLYCLIP : VS_POLY);
	}
	else
	{
		ps = wipe ? PS_WIPE : PS_FIXED;
		vs = layout == LAYOUT_SKY ? VS_SKY : layout == LAYOUT_MODEL ? VS_MODEL : wipe ? VS_WIPE : VS_POLY;
	}
	if (g_postImage) { vs = VS_POSTIMAGE; ps = PS_POSTIMAGE; }
	if (layout == LAYOUT_NONE)
	{
		vs = VS_FULLSCREEN;
		ps = PS_FIXED;
	}
	const CustomProgram *custom = CustomDrawProgram(layout);
	PipelineKey key = StatePipelineKey(layout, vs, ps, topology);
	if (custom)
	{
		key.vs = NUMVS + g_program;
		key.ps = NUMPS + g_program * 2 + (g_waterRefraction && custom->refraction ? 1 : 0);
		key.custom = g_customGeneration;
	}
	ID3D12PipelineState *pso = nullptr;
	if (g_lastPipeline && key == g_lastKey) pso = g_lastPipeline;
	else
	{
		pso = FindOrCreatePipeline(key);
		if (!pso) return false;
		g_lastKey = key; g_lastPipeline = pso;
	}
	// Constants may submit a full ring and reopen the list: bind the PSO afterwards.
	if (!ApplyConstants(custom != nullptr) || !BindCaptureState()) return false;
	if (g_boundPipeline != pso)
	{
		Core_List()->SetPipelineState(pso); g_boundPipeline = pso; g_stats.psoChanges++;
	}
	for (int unit = 0; unit < 2; ++unit)
	{
		Texture *texture = g_units[unit];
		const bool valid = texture && texture->res;
		const D3D12_GPU_DESCRIPTOR_HANDLE handles[] = {
			SrvGpu(valid ? texture->srv : 3), SamplerGpu(valid ? GetSampler(texture) : 0)};
		for (int kind = 0; kind < 2; ++kind)
		{
			const int slot = unit * 2 + kind;
			if (g_boundTables[slot] == handles[kind].ptr) continue;
			Core_List()->SetGraphicsRootDescriptorTable(1 + slot, handles[kind]);
			g_boundTables[slot] = handles[kind].ptr;
			g_stats.tableSets++;
		}
	}
	if (g_paletteRendering)
	{
		// The lookup table at t2 and light table at t3; without them, the null views a list begins with
		const D3D12_GPU_DESCRIPTOR_HANDLE handles[2] = {
			SrvGpu(g_paletteLookup.res ? g_paletteLookup.srv : 1), SrvGpu(g_currentLightTable ? g_currentLightTable->srv : 0)};
		const UINT parameters[2] = {6, 5};
		for (int slot = 0; slot < 2; ++slot)
		{
			if (g_boundPaletteTables[slot] == handles[slot].ptr) continue;
			Core_List()->SetGraphicsRootDescriptorTable(parameters[slot], handles[slot]);
			g_boundPaletteTables[slot] = handles[slot].ptr;
			g_stats.tableSets++;
		}
	}
	// Each costs a driver call, so set only what changed since the list began
	if (!g_dynamicBound || g_boundTopology != topology)
	{
		Core_List()->IASetPrimitiveTopology(topology);
		g_boundTopology = topology;
	}
	if (!g_dynamicBound || g_boundStencilRef != g_gl.stencilRef)
	{
		Core_List()->OMSetStencilRef(g_gl.stencilRef);
		g_boundStencilRef = g_gl.stencilRef;
	}
	if (!g_dynamicBound || memcmp(g_boundViewport, g_gl.viewport, sizeof g_boundViewport) != 0)
	{
		BindViewport();
		memcpy(g_boundViewport, g_gl.viewport, sizeof g_boundViewport);
	}
	g_dynamicBound = true;
	return true;
}

bool CanDraw() { return Core_List() && g_rt && !g_deviceLost && !g_shutdown; }
enum Primitive { PRIM_FAN, PRIM_LINES, PRIM_STRIP };

// One requested frame, one pixel copied after each world draw into a slot of a single readback buffer, read back
// once the frame is recorded (the log format is the former Direct3D 11 driver's). The screen is never sampled, and no
// copies, allocations or maps occur when the probe is inactive.
constexpr UINT kProbeCapacity = 8192;
// A placed footprint starts at a multiple of 512 (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT); one texel takes a 256-byte pitch
constexpr UINT kProbeSlotBytes = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
struct SurfaceProbeRecord
{
	const char *kind;
	INT32 source;
	UINT32 texture;
	char name[9];
	FBITFIELD flags;
	int shader;
	UINT8 colorMask;
	bool depthTest, depthWrite, depthAlways;
	float alpha;
	UINT count;
	FOutVector vertices[8];
};
bool g_probePending = false, g_probeActive = false, g_probeOverflow = false;
UINT g_probePercentX = 15, g_probePercentY = 35, g_probeX, g_probeY;
const char *g_probeKind = "draw";
INT32 g_probeSource = -1;
ComPtr<ID3D12Resource> g_probeBuffer;
std::vector<SurfaceProbeRecord> g_probeRecords;

void CaptureSurfaceProbe(const char *fallback, const FOutVector *vertices, UINT count)
{
	if (!g_probeActive) return;
	if (g_probeRecords.size() == kProbeCapacity)
	{
		g_probeOverflow = true;
		return;
	}
	SurfaceProbeRecord record = {};
	record.kind = strcmp(g_probeKind, "draw") ? g_probeKind : fallback;
	record.source = g_probeSource;
	record.flags = g_currentPolyFlags;
	record.shader = g_program;
	record.colorMask = g_gl.colorWriteMask;
	record.depthTest = g_gl.depthTest;
	record.depthWrite = g_gl.depthMask;
	record.depthAlways = g_gl.depthAlways;
	record.alpha = g_program >= 0 ? g_uniforms[g_program].poly[3] : g_gl.color[3]/255.0f;
	record.count = vertices ? count : 0;
	if (vertices) memcpy(record.vertices, vertices, std::min(count, 8u)*sizeof(*vertices));
	if (g_units[0])
	{
		record.texture = g_units[0]->id;
		memcpy(record.name, g_units[0]->probeName, sizeof record.name);
	}
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = g_rt.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = g_probeBuffer.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Offset = (UINT64)g_probeRecords.size() * kProbeSlotBytes;
	destination.PlacedFootprint.Footprint = {kScreenFormat, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
	const D3D12_BOX pixel = {g_probeX, g_probeY, 0, g_probeX + 1, g_probeY + 1, 1};
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Core_FlushBarriers();
	Core_List()->CopyTextureRegion(&destination, 0, 0, 0, &source, &pixel);
	Core_Transition(g_rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	g_probeRecords.push_back(record);
	g_probeKind = "draw";
	g_probeSource = -1; // an unlabelled draw must not inherit the previous map source
}

bool BeginSurfaceProbe(void)
{
	if (!g_probePending) return false;
	g_probePending = false;
	if (!CanDraw() || !g_rtWidth || !g_rtHeight) return false;
	g_probeX = std::min(g_rtWidth-1, (UINT)((UINT64)g_rtWidth*g_probePercentX/100));
	g_probeY = std::min(g_rtHeight-1, (UINT)((UINT64)g_rtHeight*g_probePercentY/100));
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = (UINT64)kProbeCapacity * kProbeSlotBytes;
	desc.Height = 1;
	desc.DepthOrArraySize = desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probeBuffer)), "CreateCommittedResource (surface probe)"))
		return false;
	g_probeBuffer->SetName(L"Surface probe readback");
	g_probeRecords.clear();
	g_probeRecords.reserve(kProbeCapacity);
	g_probeOverflow = false; g_probeActive = true;
	g_probeKind = "draw"; g_probeSource = -1;
	for (auto &texture : g_textures) texture.second.probeName[0] = '\0';
	Log("surface probe: begin pixel=(%u,%u) screen=%ux%u format=%d", g_probeX, g_probeY,
		g_rtWidth, g_rtHeight, (int)kScreenFormat);
	return true;
}

void EndSurfaceProbe(void)
{
	if (!g_probeActive) return;
	g_probeActive = false;
	const UINT draws = (UINT)g_probeRecords.size();
	UINT changes = 0;
	bool read = !draws;
	if (draws)
	{
		const UINT64 fence = Core_Submit("readback");
		void *data = nullptr;
		const D3D12_RANGE range = {0, (SIZE_T)draws * kProbeSlotBytes};
		if (fence && Core_Wait(fence, 2000, "the surface probe")
			&& !Core_Failed(g_probeBuffer->Map(0, &range, &data), "Map (surface probe)"))
		{
			read = true;
			UINT32 previous = 0;
			UINT logged = 0;
			for (UINT i = 0; i < draws; ++i)
			{
				UINT32 pixel;
				memcpy(&pixel, (const UINT8 *)data + (size_t)i*kProbeSlotBytes, 4);
				if (i && pixel == previous) continue;
				previous = pixel; ++changes;
				if (logged++ >= 256) continue;
				const SurfaceProbeRecord &r = g_probeRecords[i];
				Log("surface probe: sample=%u raw=0x%08X kind=%s source=%d texture=%u name=%.8s flags=0x%lx shader=%d mask=%u depth=(%d,%d,%d) alpha=%.3f vertices=%u",
					i, pixel, r.kind, r.source, r.texture, r.name, (unsigned long)r.flags, r.shader,
					r.colorMask, r.depthTest, r.depthWrite, r.depthAlways, r.alpha, r.count);
				for (UINT j = 0; j < std::min(r.count, 8u); ++j)
					Log("surface probe: sample=%u v%u=(%.2f,%.2f,%.2f;%.4f,%.4f)", i, j,
						r.vertices[j].x, r.vertices[j].z, r.vertices[j].y, r.vertices[j].s, r.vertices[j].t);
			}
			const D3D12_RANGE written = {0, 0};
			g_probeBuffer->Unmap(0, &written);
		}
	}
	if (read)
		Log("surface probe: end draws=%u color_changes=%u rows_omitted=%u overflow=%d",
			draws, changes, changes > 256 ? changes-256 : 0, g_probeOverflow);
	else
		Log("surface probe: readback failed after %u draws", draws);
	Core_Retire(g_probeBuffer);
	std::vector<SurfaceProbeRecord>().swap(g_probeRecords);
}

bool DrawUploaded(Layout layout, D3D12_PRIMITIVE_TOPOLOGY topology, const void *vertices, UINT count,
	const void *indices, UINT indexCount, bool wipe = false, DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT)
{
	const UINT strides[] = {sizeof(FOutVector), sizeof(gl_skyvertex_t), 32};
	const UINT stride = strides[layout];
	const UINT indexStride = indexFormat == DXGI_FORMAT_R16_UINT ? sizeof(UINT16) : sizeof(UINT32);
	if (!CanDraw() || !count || !vertices) return false;
	auto vb = g_vertexRing.Alloc((size_t)count * stride, stride);
	if (!vb.cpu) return false;
	memcpy(vb.cpu, vertices, (size_t)count * stride);
	UploadAllocation ib = {};
	if (indexCount)
	{
		ib = g_indexRing.Alloc((size_t)indexCount * indexStride, indexStride);
		if (!ib.cpu) return false;
		memcpy(ib.cpu, indices, (size_t)indexCount * indexStride);
	}
	if (!ApplyState(layout, topology, wipe)) return false;
	// Allocations can submit while another ring is full. Retag all inputs for the draw's actual list.
	if (!g_vertexRing.Touch(vb, (size_t)count * stride)) return false;
	if (indexCount && !g_indexRing.Touch(ib, (size_t)indexCount * indexStride)) return false;
	if (!g_constantRing.Touch(g_constantAllocation, 512)) return false;
	const D3D12_VERTEX_BUFFER_VIEW view = {vb.gpu, count * stride, stride};
	Core_List()->IASetVertexBuffers(0, 1, &view);
	if (indexCount)
	{
		const D3D12_INDEX_BUFFER_VIEW indexView = {ib.gpu, indexCount * indexStride, indexFormat};
		Core_List()->IASetIndexBuffer(&indexView);
	}
	Core_FlushBarriers();
	if (indexCount) Core_List()->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
	else Core_List()->DrawInstanced(count, 1, 0, 0);
	g_stats.draws++; g_stats.vertices += count; g_stats.indices += indexCount;
	return true;
}

bool DrawFanUploaded(Layout layout, const void *vertices, UINT count, bool wipe);

void DrawArrays(Layout layout, Primitive primitive, const void *vertices, UINT count, bool wipe = false)
{
	bool drawn;
	if (primitive == PRIM_FAN)
	{
		if (count < 3) return;
		if (count <= 65535)
			drawn = DrawFanUploaded(layout, vertices, count, wipe);
		else
		{
			std::vector<UINT32> indices;
			indices.reserve((size_t)(count - 2) * 3);
			for (UINT i = 1; i + 1 < count; i++)
			{
				indices.push_back(0); indices.push_back(i); indices.push_back(i + 1);
			}
			drawn = DrawUploaded(layout, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertices, count,
				indices.data(), (UINT)indices.size(), wipe);
		}
	}
	else drawn = DrawUploaded(layout, primitive == PRIM_LINES ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
		vertices, primitive == PRIM_LINES ? count & ~1u : count, nullptr, 0, wipe);
	if (drawn)
		CaptureSurfaceProbe("polygon", layout == LAYOUT_POLY ? (const FOutVector *)vertices : nullptr, count);
}

#include "d3d12_models.inc"

bool DrawFanUploaded(Layout layout, const void *vertices, UINT count, bool wipe)
{
	if (!CanDraw() || !EnsureFanIndices()) return false;
	const UINT strides[] = {sizeof(FOutVector), sizeof(gl_skyvertex_t), sizeof(ModelVertex)};
	const UINT stride = strides[layout];
	auto vb = g_vertexRing.Alloc((size_t)count * stride, stride);
	if (!vb) return false;
	memcpy(vb.cpu, vertices, (size_t)count * stride);
	if (!ApplyState(layout, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, wipe)) return false;
	if (!g_vertexRing.Touch(vb, (size_t)count * stride) || !g_constantRing.Touch(g_constantAllocation, 512)) return false;
	const D3D12_VERTEX_BUFFER_VIEW view = {vb.gpu, count * stride, stride};
	const D3D12_INDEX_BUFFER_VIEW indices = {g_fanIndices.allocation.gpu, g_fanIndices.bytes, DXGI_FORMAT_R16_UINT};
	Core_List()->IASetVertexBuffers(0, 1, &view); Core_List()->IASetIndexBuffer(&indices);
	Core_FlushBarriers();
	Core_List()->DrawIndexedInstanced((count - 2) * 3, 1, 0, 0, 0);
	g_stats.draws++; g_stats.vertices += count; g_stats.indices += (count - 2) * 3;
	return true;
}

// Whenever the list is (re)opened: restore every root argument and dynamic binding.
void BindRenderTarget()
{
	memset(g_boundTables, 0, sizeof g_boundTables);
	memset(g_boundPaletteTables, 0, sizeof g_boundPaletteTables);
	g_srvIndices.Collect(Core_CompletedFence());
	g_staticArena.Collect(Core_CompletedFence());
	CollectTextureBlocks();
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list || !g_rt)
		return;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Core_Rtv(kRtvScreen), dsv = Core_Dsv(kDsvScreen);
	list->OMSetRenderTargets(1, &rtv, FALSE, g_depth ? &dsv : nullptr);
	if (g_root && g_zeroConstants)
	{
		ID3D12DescriptorHeap *heaps[] = {g_srvHeap.Get(), g_samplerHeap.Get()};
		list->SetDescriptorHeaps(2, heaps);
		list->SetGraphicsRootSignature(g_root.Get());
		const auto base = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
		const auto sampler = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
		const UINT step = g_srvStep;
		for (UINT param = 1; param <= 13; param++)
		{
			auto handle = base;
			if (param == 1 || param == 3) handle.ptr += 3 * step;
			if (param == 6) handle.ptr += step; // t2: Texture3D R8
			if (param == 13) handle.ptr += 2 * step; // t10: Texture2D R32_SINT
			list->SetGraphicsRootDescriptorTable(param, param == 2 || param == 4 ? sampler : handle);
			g_stats.tableSets++;
		}
		list->SetGraphicsRootConstantBufferView(0, g_zeroConstants->GetGPUVirtualAddress());
		for (UINT param = 14; param <= 18; param++)
			list->SetGraphicsRootConstantBufferView(param, g_zeroConstants->GetGPUVirtualAddress());
		g_stats.rootSets += 6;
	}
	BindViewport();
	g_boundPipeline = nullptr;
	g_dynamicBound = false;
	// Root arguments were reset, but the cached constants retain their contents until the ring wraps.
	// ApplyConstants always rebinds b0; each draw retags the cached slot for its pending fence.

}

#include "d3d12_palette.inc"

// A full turn every 240 frames, about 1.5 degrees a frame
void DiagnosticHue(float rgb[3])
{
	const float hue = (float)(g_hueFrame % 240) / 40.0f; // 0 to 6
	const float x = 1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f);
	switch ((int)hue)
	{
		case 0: rgb[0] = 1.0f; rgb[1] = x; rgb[2] = 0.0f; break;
		case 1: rgb[0] = x; rgb[1] = 1.0f; rgb[2] = 0.0f; break;
		case 2: rgb[0] = 0.0f; rgb[1] = 1.0f; rgb[2] = x; break;
		case 3: rgb[0] = 0.0f; rgb[1] = x; rgb[2] = 1.0f; break;
		case 4: rgb[0] = x; rgb[1] = 0.0f; rgb[2] = 1.0f; break;
		default: rgb[0] = 1.0f; rgb[1] = 0.0f; rgb[2] = x; break;
	}
}

void ClearScreenColor()
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list || !g_rt)
		return;
	// The screen's alpha stays 1, as in the former Direct3D 11 driver
	float color[4] = {g_gl.clearColor[0], g_gl.clearColor[1], g_gl.clearColor[2], 1.0f};
	if (kDiagnosticHue)
	{
		float hue[3];
		DiagnosticHue(hue);
		for (int i = 0; i < 3; i++)
			color[i] = 0.5f * color[i] + 0.5f * hue[i];
	}
	Core_FlushBarriers();
	list->ClearRenderTargetView(Core_Rtv(kRtvScreen), color, 0, nullptr);
	g_stats.clears++;
}

void ClearScreenDepth()
{
	ID3D12GraphicsCommandList *list = Core_List();
	if (!list || !g_depth)
		return;
	Core_FlushBarriers();
	list->ClearDepthStencilView(Core_Dsv(kDsvScreen), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
}

bool CreateRenderTarget(UINT width, UINT height)
{
	if (g_rt && g_rtWidth == width && g_rtHeight == height)
		return true;

	if (g_rt)
	{
		// Resizing: let the GPU finish with the old targets. They're retired either way, so a timeout is safe.
		Core_Submit("resize");
		Core_WaitIdle(1000, "a resize");
	}
	// The view of the old screen goes with it. The palette step's target is remade at the new size when next needed.
	RetireScreenSrv();
	RetireTexture(g_postTexture);
	Core_Retire(g_rt);
	Core_Retire(g_depth);
	g_rtWidth = g_rtHeight = 0;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = kScreenFormat;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ID3D12Device *device = Core_Device();
	if (Core_Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
		nullptr, IID_PPV_ARGS(&g_rt)), "CreateCommittedResource (screen)"))
	{
		g_rt.Reset();
		return false;
	}

	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	D3D12_CLEAR_VALUE depthClear = {};
	depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthClear.DepthStencil.Depth = 1.0f;
	depthClear.DepthStencil.Stencil = 0;
	if (Core_Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&depthClear, IID_PPV_ARGS(&g_depth)), "CreateCommittedResource (depth)"))
	{
		// Neither has been used on the GPU
		g_rt.Reset();
		g_depth.Reset();
		return false;
	}
	g_rt->SetName(L"screen");
	g_depth->SetName(L"depth");
	device->CreateRenderTargetView(g_rt.Get(), nullptr, Core_Rtv(kRtvScreen));
	device->CreateDepthStencilView(g_depth.Get(), nullptr, Core_Dsv(kDsvScreen));

	g_rtWidth = width;
	g_rtHeight = height;
	BindRenderTarget();

	ID3D12GraphicsCommandList *list = Core_List();
	if (list)
	{
		const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		list->ClearRenderTargetView(Core_Rtv(kRtvScreen), black, 0, nullptr);
	}
	ClearScreenDepth();

	Log("render target %ux%u (format %d)", width, height, (int)kScreenFormat);
	return true;
}

// r_opengl.c's SetModelView, so far without the matrices
void SetModelView(INT32 w, INT32 h)
{
	// The screen textures need to be flushed if the width or height change so that they be remade for the correct size
	if (g_screenWidth != w || g_screenHeight != h)
	{
		D3D_FlushScreenTextures();
		Log("screen size %dx%d", (int)w, (int)h);
	}

	g_screenWidth = w;
	g_screenHeight = h;
	CreateRenderTarget(w, h);

	SetViewport(0, 0, w, h);

	g_matrixMode = MODE_PROJECTION;
	MatLoadIdentity();

	g_matrixMode = MODE_MODELVIEW;
	MatLoadIdentity();

	Perspective(g_fov, 1.0f);
}

// ==========================================================================
//                                                            DRIVER FUNCTIONS
// ==========================================================================

boolean D3D_Init(void)
{
	if (!Core_Device())
	{
		Log("starting Legacy GL's Direct3D 12 driver (drawing with textures)");
		Core_SetListBegin(BindRenderTarget);
		if (!Core_Init())
		{
			return false;
		}
		if (!InitDrawing())
		{
			D3D12_Shutdown();
			return false;
		}
		Prewarm_Start(Core_Device(), g_root.Get());
	}
	// Every time Legacy GL starts, since switching to Software gave the window back to ANGLE. If it can't be taken,
	// Legacy GL can't present: failing here makes the game fall back to Software.
	return Present_TakeWindow();
}

void D3D_SetPalette(RGBA_t *palette)
{
	g_stats.states++;
	if (palette && memcmp(g_palette, palette, sizeof g_palette))
	{
		memcpy(g_palette, palette, sizeof g_palette);
		Flush();
	}
}

void D3D_FinishUpdate(INT32 waitvbl)
{
	D3D12_FinishUpdate(waitvbl);
}

void D3D_Draw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t Color)
{
	FOutVector p[4];
	float angle;

	g_gl.texture2D = false;

	// This is the preferred, 'modern' way of rendering lines -- creating a polygon.
	if (fabsf(v2->x - v1->x) > FLT_EPSILON)
		angle = (float)atan((v2->y-v1->y)/(v2->x-v1->x));
	else
		angle = (float)(M_PI / 2.0);
	const float dx = (float)sin(angle) / (float)g_screenWidth;
	const float dy = (float)cos(angle) / (float)g_screenHeight;

	memset(p, 0, sizeof p);
	p[0].x = v1->x - dx;  p[0].y = -(v1->y + dy); p[0].z = 1;
	p[1].x = v2->x - dx;  p[1].y = -(v2->y + dy); p[1].z = 1;
	p[2].x = v2->x + dx;  p[2].y = -(v2->y - dy); p[2].z = 1;
	p[3].x = v1->x + dx;  p[3].y = -(v1->y - dy); p[3].z = 1;

	SetColor(&Color.s.red);
	DrawArrays(LAYOUT_POLY, PRIM_FAN, p, 4);
	g_stats.lines++;

	g_gl.texture2D = true;
}

#include "d3d12_captures.inc"

void PreparePolygon(FSurfaceInfo *pSurf, FBITFIELD PolyFlags, const FOutVector *vertices, UINT count)
{
	static float poly[4] = {0, 0, 0, 0};
	static float tint[4] = {0, 0, 0, 0};
	static float fade[4] = {0, 0, 0, 0};

	// Coronas read back the depth buffer in r_opengl.c, unless it can't (GLF_NOZBUFREAD), which is what this does
	if (PolyFlags & PF_Corona)
	{
		D3D12_LOG_ONCE("drawing coronas as if the depth buffer can't be read (logged once)");
		PolyFlags &= ~(PF_NoDepthTest|PF_Corona);
	}

	D3D_SetBlend(PolyFlags);

	if (pSurf)
	{
		// If modulated, mix the surface colour to the texture
		if (g_currentPolyFlags & PF_Modulated)
			SetColor(&pSurf->PolyColor.s.red);

		// If the surface is either modulated or colormapped, or both
		if (g_currentPolyFlags & (PF_Modulated | PF_ColorMapped))
		{
			poly[0] = pSurf->PolyColor.s.red / 255.0f;
			poly[1] = pSurf->PolyColor.s.green / 255.0f;
			poly[2] = pSurf->PolyColor.s.blue / 255.0f;
			poly[3] = pSurf->PolyColor.s.alpha / 255.0f;
		}

		// Only if the surface is colormapped
		if (g_currentPolyFlags & PF_ColorMapped)
		{
			SetLightTable(pSurf->LightTableId);

			tint[0] = pSurf->TintColor.s.red / 255.0f;
			tint[1] = pSurf->TintColor.s.green / 255.0f;
			tint[2] = pSurf->TintColor.s.blue / 255.0f;
			tint[3] = pSurf->TintColor.s.alpha / 255.0f;

			fade[0] = pSurf->FadeColor.s.red / 255.0f;
			fade[1] = pSurf->FadeColor.s.green / 255.0f;
			fade[2] = pSurf->FadeColor.s.blue / 255.0f;
			fade[3] = pSurf->FadeColor.s.alpha / 255.0f;
		}
	}

	SetUniforms(pSurf, poly, tint, fade);
	const FBITFIELD blend = PolyFlags & (PF_Blending & ~(PF_Masked | PF_Occlude));
	const bool refractingBlend = blend == PF_Translucent || blend == PF_Additive
		|| blend == PF_Subtractive || blend == PF_ReverseSubtract
		|| blend == PF_Multiplicative || blend == PF_Environment;
	// Custom water keeps its own surface color; its composite variant blends
	// that over the refracted scene, like PSWaterRefraction does for PSWater.
	const CustomProgram *customWater = CustomDrawProgram(LAYOUT_POLY);
	g_waterRefraction = (PolyFlags & PF_Ripple) && g_program == SHADER_WATER
		&& refractingBlend && !cv_reducevfx.value && (!customWater || customWater->refraction)
		&& CaptureWaterScene(pSurf, PolyFlags, vertices, count,
			customWater && !g_customShaders[SHADER_WATER].vertex.empty());
	g_fogRemap = (PolyFlags & PF_Fog) && g_program == SHADER_FOG
		&& !CustomDrawProgram(LAYOUT_POLY) && CaptureFogScene(pSurf, PolyFlags, vertices, count);
}

void D3D_DrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags)
{

	PreparePolygon(pSurf, PolyFlags, pOutVerts, iNumPts);

	if (PolyFlags & PF_WireFrame)
		DrawArrays(LAYOUT_POLY, PRIM_LINES, pOutVerts, (UINT)iNumPts);
	else
		DrawArrays(LAYOUT_POLY, PRIM_FAN, pOutVerts, (UINT)iNumPts);
	g_stats.polygons++;

	if (PolyFlags & PF_RemoveYWrap)
		SetRepeatT(true);

	if (PolyFlags & PF_ForceWrapX)
		SetRepeatS(false);

	if (PolyFlags & PF_ForceWrapY)
		SetRepeatT(false);
}

void D3D_DrawIndexedTriangles(FSurfaceInfo *surface, FOutVector *vertices, FUINT count, FBITFIELD flags, UINT32 *indices)
{
	g_stats.batches++;
	if (!CanDraw() || !count || !indices) return;
	UINT vertexCount = 0;
	for (FUINT i = 0; i < count; i++) vertexCount = std::max(vertexCount, indices[i] + 1);
	PreparePolygon(surface, flags, vertices, vertexCount);
	if (DrawUploaded(LAYOUT_POLY, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertices, vertexCount, indices, count))
		CaptureSurfaceProbe("batch", vertices, vertexCount);
}



void D3D_SetBlend(FBITFIELD PolyFlags)
{
	g_waterRefraction = false;
#ifdef _UWP
	// Sky occluders and sprite silhouettes only write depth. Blend factors can
	// change while both draws are Invisible, so they cannot enforce this mask.
	g_gl.colorWriteMask = (PolyFlags & PF_Invisible) ? 0 : 7;
#endif
	g_fogRemap = false;
	const FBITFIELD Xor = g_currentPolyFlags ^ PolyFlags;

	if (Xor & (PF_Blending|PF_RemoveYWrap|PF_ForceWrapX|PF_ForceWrapY|PF_Occlude|PF_NoTexture|PF_Modulated|PF_NoDepthTest|PF_Decal|PF_Invisible))
	{
		if (Xor & PF_Blending) // if blending mode must be changed
			SetBlendMode(PolyFlags & PF_Blending);

		if (Xor & PF_NoAlphaTest)
			g_gl.alphaTest = !(PolyFlags & PF_NoAlphaTest);

		if (Xor & PF_Decal)
			g_gl.polygonOffset = (PolyFlags & PF_Decal) != 0;

		if (Xor & PF_NoDepthTest)
			g_gl.depthAlways = (PolyFlags & PF_NoDepthTest) != 0;

		if ((Xor & PF_RemoveYWrap) && (PolyFlags & PF_RemoveYWrap))
			SetRepeatT(false);

		if ((Xor & PF_ForceWrapX) && (PolyFlags & PF_ForceWrapX))
			SetRepeatS(true);

		if ((Xor & PF_ForceWrapY) && (PolyFlags & PF_ForceWrapY))
			SetRepeatT(true);

		if (Xor & PF_Modulated)
			g_gl.texEnv = (PolyFlags & PF_Modulated) ? TE_MODULATE : TE_REPLACE;

		if (Xor & PF_Occlude) // depth test but (no) depth write
			g_gl.depthMask = (PolyFlags & PF_Occlude) != 0;

#ifndef _UWP
		if (Xor & PF_Invisible)
		{
			if (PolyFlags & PF_Invisible)
			{
				g_gl.blendSrc = BF_ZERO; // transparent blending
				g_gl.blendDst = BF_ONE;
			}
			else if ((PolyFlags & PF_Blending) == PF_Masked)
			{
				g_gl.blendSrc = BF_SRC_ALPHA;
				g_gl.blendDst = BF_ZERO;
			}
		}

#endif

		if (PolyFlags & PF_NoTexture)
			SetNoTexture(0);
	}
	g_currentPolyFlags = PolyFlags;
}

void D3D_ClearBuffer(FBOOLEAN ColorMask, FBOOLEAN DepthMask, FRGBAFloat *ClearColor)
{
	if (ColorMask && ClearColor)
	{
		g_gl.clearColor[0] = ClearColor->red;
		g_gl.clearColor[1] = ClearColor->green;
		g_gl.clearColor[2] = ClearColor->blue;
		g_gl.clearColor[3] = ClearColor->alpha;
	}

	D3D_SetBlend(DepthMask ? PF_Occlude | g_currentPolyFlags : g_currentPolyFlags&~PF_Occlude);

	if (ColorMask)
	{
		ClearScreenColor();
		CaptureSurfaceProbe("clear", nullptr, 0);
	}
	if (DepthMask)
		ClearScreenDepth();
}

void D3D_SetTexture(GLMipmap_t *texture)
{
	SetTextureInternal(texture);
}

void D3D_UpdateTexture(GLMipmap_t *texture)
{
	UpdateTextureInternal(texture);
}

void D3D_DeleteTexture(GLMipmap_t *pTexInfo)
{
	if (!pTexInfo)
		return;
	else if (pTexInfo->downloaded)
		DeleteTextureObject(pTexInfo->downloaded);

	pTexInfo->downloaded = 0;
}

// Reads from the screen or, for the tests, the palette step's target; both rest in RENDER_TARGET
void ReadRectFrom(ID3D12Resource *screen, INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data)
{
	if (!CanDraw() || !dst_data || width <= 0 || height <= 0)
		return;

	// OpenGL's y goes up from the bottom of the screen.
	const INT64 top = (INT64)g_rtHeight - ((INT64)y + height);
	if (x < 0 || top < 0 || (INT64)x + width > g_rtWidth || top + height > g_rtHeight)
	{
		Log("ReadRect(%d, %d, %d, %d) is outside the %ux%u screen", (int)x, (int)y, (int)width, (int)height, g_rtWidth, g_rtHeight);
		return;
	}

	// A timed-out copy must finish before its cached destination can be reused.
	if (g_readRectFence && !Core_Wait(g_readRectFence, 500, "the previous ReadRect"))
		return;
	g_readRectFence = 0;
	const UINT pitch = (UINT)(((UINT64)width * 4 + 255) & ~UINT64(255));
	const UINT64 bytes = (UINT64)pitch * height;
	if (!g_readRectBuffer || g_readRectBytes < bytes)
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> buffer;
		if (Core_Failed(Core_Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)), "CreateCommittedResource (ReadRect)"))
			return;
		g_readRectBuffer = buffer;
		g_readRectBytes = bytes;
		g_readRectBuffer->SetName(L"ReadRect readback");
	}

	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = screen;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = g_readRectBuffer.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint = {kScreenFormat, (UINT)width, (UINT)height, 1, pitch};
	const D3D12_BOX box = {(UINT)x, (UINT)top, 0, (UINT)(x + width), (UINT)(top + height), 1};
	Core_Transition(screen, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Core_FlushBarriers();
	Core_List()->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
	Core_Transition(screen, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	g_readRectFence = Core_Submit("readback");
	if (!g_readRectFence || !Core_Wait(g_readRectFence, 500, "ReadRect"))
		return;

	void *data = nullptr;
	const D3D12_RANGE read = {0, (SIZE_T)bytes};
	if (Core_Failed(g_readRectBuffer->Map(0, &read, &data), "Map (ReadRect)"))
		return;
	const bool bgra = kScreenFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
	UINT8 *rgb = reinterpret_cast<UINT8 *>(dst_data);
	// Top row first, like r_opengl.c leaves it.
	for (INT32 row = 0; row < height; row++)
	{
		const UINT8 *sourceRow = static_cast<const UINT8 *>(data) + (size_t)row * pitch;
		for (INT32 column = 0; column < width; column++)
		{
			const UINT8 *pixel = sourceRow + column * 4;
			const UINT8 r = bgra ? pixel[2] : pixel[0], g = pixel[1], b = bgra ? pixel[0] : pixel[2];
			if (dst_stride == width*3)
			{
				UINT8 *out = rgb + (size_t)row * dst_stride + column * 3;
				out[0] = r;
				out[1] = g;
				out[2] = b;
			}
			else
				dst_data[row*width + column] = (UINT16)(((r>>3)<<11) | ((g>>2)<<5) | (b>>3));
		}
	}
	const D3D12_RANGE written = {0, 0};
	g_readRectBuffer->Unmap(0, &written);
}

void D3D_ReadRect(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data)
{
	ReadRectFrom(g_rt.Get(), x, y, width, height, dst_stride, dst_data);
}

void D3D_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip)
{
	SetViewport(minx, g_screenHeight-maxy, maxx-minx, maxy-miny);
	g_nearClippingPlane = nearclip;

	g_matrixMode = MODE_PROJECTION;
	MatLoadIdentity();
	Perspective(g_fov, 1.0f);
	g_matrixMode = MODE_MODELVIEW;
}

void D3D_ClearMipMapCache(void)
{
	Flush();
}

void D3D_SetSpecialState(hwdspecialstate_t IdState, INT32 Value)
{
	switch (IdState)
	{
#ifdef _UWP
		case HWD_SET_WATER_SURFACE:
			SetWaterSurface(Value);
			break;
#endif
		case HWD_SET_MODEL_LIGHTING:
			// Set around each model, so not logged
			g_modelLighting = Value;
			break;

		case HWD_SET_SHADERS:
			if (g_allowShaders != (hwdshaderoption_t)Value)
				Log("shaders %s", Value == HWD_SHADEROPTION_OFF ? "off" : Value == HWD_SHADEROPTION_ON ? "on" : "on (no custom)");
			g_allowShaders = (hwdshaderoption_t)Value;
			break;

		case HWD_SET_TEXTUREFILTERMODE:
			switch (Value)
			{
				case HWD_SET_TEXTUREFILTER_TRILINEAR:
					g_minFilter = FILTER_LINEAR_MIPMAP_LINEAR;
					g_magFilter = FILTER_LINEAR;
					g_mipmap = true;
					break;
				case HWD_SET_TEXTUREFILTER_BILINEAR:
					g_minFilter = g_magFilter = FILTER_LINEAR;
					g_mipmap = false;
					break;
				case HWD_SET_TEXTUREFILTER_POINTSAMPLED:
					g_minFilter = g_magFilter = FILTER_NEAREST;
					g_mipmap = false;
					break;
				case HWD_SET_TEXTUREFILTER_MIXED1:
					g_minFilter = FILTER_NEAREST;
					g_magFilter = FILTER_LINEAR;
					g_mipmap = false;
					break;
				case HWD_SET_TEXTUREFILTER_MIXED2:
					g_minFilter = FILTER_LINEAR;
					g_magFilter = FILTER_NEAREST;
					g_mipmap = false;
					break;
				case HWD_SET_TEXTUREFILTER_MIXED3:
					g_minFilter = FILTER_LINEAR_MIPMAP_LINEAR;
					g_magFilter = FILTER_NEAREST;
					g_mipmap = true;
					break;
				default:
					g_magFilter = FILTER_LINEAR;
					g_minFilter = FILTER_NEAREST;
			}
			Log("texture filter mode %d", (int)Value);
			{
				Flush(); //??? if we want to change filter mode by texture, remove this
			}
			break;

		case HWD_SET_PALETTE_RENDERING:
			if (g_paletteRendering != (Value != 0))
				Log("palette rendering %s", Value ? "on" : "off");
			g_paletteRendering = Value != 0;
			memset(g_boundPaletteTables, 0, sizeof g_boundPaletteTables);
			break;

		case HWD_SET_TEXTUREANISOTROPICMODE:
			g_anisotropicFilter = std::min((int)Value, kMaximumAnisotropy);
			Log("anisotropic filtering %d", g_anisotropicFilter);
			{
				Flush(); //??? if we want to change filter mode by texture, remove this
			}
			break;

		default:
			break;
	}
}

void D3D_SetTransform(FTransform *stransform)
{
	static boolean special_splitscreen;
	boolean shearing = false;
	float used_fov;

	MatLoadIdentity();

	if (stransform)
	{
		used_fov = stransform->fovxangle;
		if (stransform->mirror)
			MatScale(-stransform->scalex, stransform->scaley, -stransform->scalez);
		else if (stransform->flip)
			MatScale(stransform->scalex, -stransform->scaley, -stransform->scalez);
		else
			MatScale(stransform->scalex, stransform->scaley, -stransform->scalez);

		if (stransform->roll)
			MatRotate(stransform->rollangle, 0.0f, 0.0f, 1.0f);
		MatRotate(stransform->anglex       , 1.0f, 0.0f, 0.0f);
		MatRotate(stransform->angley+270.0f, 0.0f, 1.0f, 0.0f);
		MatTranslate(-stransform->x, -stransform->z, -stransform->y);

		special_splitscreen = (stransform->splitscreen == 1);
		shearing = stransform->shearing;
	}
	else
	{
		used_fov = g_fov;
		MatScale(1.0f, 1.0f, -1.0f);
	}

	g_matrixMode = MODE_PROJECTION;
	MatLoadIdentity();

	// Simulate Software's y-shearing
	// https://zdoom.org/wiki/Y-shearing
	if (shearing)
	{
		float fdy = stransform->viewaiming * 2;
		if (stransform->flip)
			fdy *= -1.0f;
		MatTranslate(0.0f, -fdy/BASEVIDHEIGHT, 0.0f);
	}

	if (special_splitscreen)
	{
		used_fov = (float)(atan(tan(used_fov*M_PI/360)*0.8)*360/M_PI);
		Perspective(used_fov, 2.0f);
	}
	else
		Perspective(used_fov, 1.0f);

	g_matrixMode = MODE_MODELVIEW;
}

INT32 D3D_GetTextureUsed(void)
{
	INT32 res = 0;

	for (const auto &entry : g_textures)
	{
		const Texture &texture = entry.second;

		if (!texture.listed)
			continue;

		// Figure out the correct bytes-per-pixel for this texture
		// I don't know which one the game actually _uses_ but this
		// follows format2bpp in hw_cache.c
		int bpp = 1;
		const UINT32 format = texture.listFormat;
		if (format == GL_TEXFMT_RGBA)
			bpp = 4;
		else if (format == GL_TEXFMT_ALPHA_INTENSITY_88 || format == GL_TEXFMT_AP_88)
			bpp = 2;

		// Add it up!
		res += texture.listHeight*texture.listWidth*bpp;
	}

	return res;
}

#include "d3d12_screens.inc"

static void RetireCustomPipelines(int type = 0)
{
	for (auto it = g_pipelines.begin(); it != g_pipelines.end();)
	{
		if (it->first.custom && (!type || it->first.vs == NUMVS + type))
		{
			Core_Retire(it->second);
			it = g_pipelines.erase(it);
		}
		else ++it;
	}
	g_lastKey = {};
	g_lastPipeline = g_boundPipeline = nullptr;
}

static void AdvanceCustomGeneration()
{
	RetireCustomPipelines();
	// Zero is reserved for built-ins; retiring every custom PSO also makes wrap safe.
	if (!++g_customGeneration) ++g_customGeneration;
}

static bool CompileCustomStage(const std::string &hlsl, const char *profile, ComPtr<ID3DBlob> &code)
{
	ComPtr<ID3DBlob> errors;
	const HRESULT result = D3DCompile(hlsl.data(), hlsl.size(), "Legacy GL add-on", nullptr, nullptr,
		"main", profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (errors)
		Log("custom %s: %.*s", profile, (int)std::min<size_t>(errors->GetBufferSize(), 8192), (const char *)errors->GetBufferPointer());
	return !Core_Failed(result, "D3DCompile (custom shader)");
}

static PipelineKey CustomValidationKey(int type, Layout layout, bool refraction = false)
{
	PipelineKey key = {};
	key.vs = NUMVS + type;
	key.ps = NUMPS + type * 2 + (refraction ? 1 : 0);
	key.custom = g_customGeneration;
	key.layout = layout;
	key.blend = 7 << 10; // opaque RGB, preserving target alpha like normal draws
	key.depth = 5ULL | (UINT64(D3D12_COMPARISON_FUNC_EQUAL) << 4)
		| (UINT64(D3D12_STENCIL_OP_KEEP) << 8) | (UINT64(D3D12_STENCIL_OP_KEEP) << 12)
		| (UINT64(D3D12_STENCIL_OP_KEEP) << 16) | (255ULL << 20) | (255ULL << 28);
	// Depth test and writes, LESS_EQUAL; valid stencil defaults even while disabled.
	return key;
}

boolean D3D_CompileShaders(void)
{
	// A bad add-on must not disable the built-in shaders or palette rendering.
	if (!Core_Device()) return true;
	bool dirty = false;
	for (const CustomShader &shader : g_customShaders) dirty = dirty || shader.dirty;
	if (dirty) AdvanceCustomGeneration();
	for (int type = 1; type < NUMBASESHADERS; type++)
	{
		CustomShader &shader = g_customShaders[type];
		if (!shader.dirty) continue;
		shader.dirty = false; shader.valid = false;
		for (CustomProgram &program : shader.programs) program = {};
		if (shader.vertex.empty() && shader.fragment.empty()) continue;
		const std::string vertex = shader.vertex.empty() ? gl_shadersources[type].vertex : shader.vertex;
		const std::string fragment = shader.fragment.empty() ? gl_shadersources[type].fragment : shader.fragment;
		CustomProgram programs[NUMLAYOUTS];
		bool valid = true;
		for (int layout = 0; layout < NUMLAYOUTS; layout++)
		{
			LegacyShaderHLSL output; std::string error;
			if (!TranslateLegacyShader(vertex, fragment, layout, output, error, false))
			{
				Log("custom shader %d: %s; keeping the built-in shader", type, error.c_str());
				valid = false; break;
			}
			if (!CompileCustomStage(output.vertex, "vs_5_1", programs[layout].vertex)
				|| !CompileCustomStage(output.fragment, "ps_5_1", programs[layout].pixel))
			{
				valid = false; break;
			}
		}
		if (!valid) continue;
		for (int layout = 0; layout < NUMLAYOUTS; layout++) shader.programs[layout] = std::move(programs[layout]);
		// Validate bytecode, root bindings and all input layouts before selecting an add-on.
		for (int layout = 0; layout < NUMLAYOUTS; layout++)
			if (!FindOrCreatePipeline(CustomValidationKey(type, (Layout)layout))) { valid = false; break; }
		if (!valid)
		{
			Log("custom shader %d: pipeline validation failed; keeping the built-in shader", type);
			RetireCustomPipelines(type);
			for (CustomProgram &program : shader.programs) program = {};
			continue;
		}
		if (type == SHADER_WATER)
		{
			// Same vertex stage and inputs; without it, custom water draws unrefracted.
			LegacyShaderHLSL output; std::string error;
			CustomProgram &program = shader.programs[LAYOUT_POLY];
			if (!TranslateLegacyShader(vertex, fragment, LAYOUT_POLY, output, error, true))
				Log("custom water refraction: %s; drawing custom water without refraction", error.c_str());
			else if (!CompileCustomStage(output.fragment, "ps_5_1", program.refraction)
				|| !FindOrCreatePipeline(CustomValidationKey(type, LAYOUT_POLY, true)))
			{
				program.refraction.Reset();
				Log("custom water refraction failed to compile; drawing custom water without refraction");
			}
		}
		shader.valid = true; shader.generation = g_customGeneration;
		Log("custom shader %d compiled for polygon, sky and model layouts", type);
	}
	g_shaderChanged = true;
	g_lastKey = {}; g_lastPipeline = g_boundPipeline = nullptr;
	return true;
}

void D3D_CleanShaders(void)
{
	AdvanceCustomGeneration();
	for (CustomShader &shader : g_customShaders) shader = {};
	g_customViewConstants = {};
	g_customViewFrame = UINT64_MAX;
	g_shaderChanged = true;
}

void D3D_SetShader(int type)
{
	g_waterRefraction = false;
#ifdef _UWP
	if (type == SHADER_WATER && cv_reducevfx.value) type = SHADER_FLOOR;
#endif
	g_fogRemap = false;
	if (type == SHADER_NONE)
	{
		g_shaderCurrent = -1;
		g_shaderType = 0;
		g_program = -1;
		g_shadersEnabled = false;
		return;
	}

	if (g_allowShaders != HWD_SHADEROPTION_OFF)
	{
		// If using model lighting, set the appropriate shader.
		if (type == SHADER_MODEL && g_modelLighting
			&& !(g_allowShaders == HWD_SHADEROPTION_ON && g_customShaders[SHADER_MODEL].valid
				&& !g_customShaders[SHADER_MODEL_LIGHTING].valid))
			type = SHADER_MODEL_LIGHTING;

		if (g_shaderCurrent < 0 || type != g_shaderType)
		{
			g_shaderCurrent = type;
			g_shaderType = type;
			g_shaderChanged = true;
		}

		if (!ProgramExists(type))
			D3D12_LOG_ONCE("SetShader: invalid shader type %d (logged once)", type);

		g_shadersEnabled = ProgramExists(type);
		return;
	}

	g_shadersEnabled = false;
}

void D3D_UnSetShader(void)
{
	g_waterRefraction = false;
	g_fogRemap = false;
	g_shaderCurrent = -1;
	g_shaderType = 0;
	g_program = -1;
	g_shadersEnabled = false;
}

void D3D_SetShaderInfo(hwdshaderinfo_t info, INT32 value)
{
	switch (info)
	{
		case HWD_SHADERINFO_LEVELTIME:
			g_shaderLeveltime = value;
			break;
		case HWD_SHADERINFO_LIGHT_X:
			g_shaderLightX = FixedToFloat(value);
			break;
		case HWD_SHADERINFO_LIGHT_Y:
			g_shaderLightY = FixedToFloat(value);
			break;
		case HWD_SHADERINFO_LIGHT_Z:
			g_shaderLightZ = FixedToFloat(value);
			break;
		case HWD_SHADERINFO_LIGHT_CONTRAST:
			g_shaderLightContrast = value;
			break;
		case HWD_SHADERINFO_LIGHT_BACKLIGHT:
			g_shaderLightBacklight = value;
			break;
		default:
			break;
	}
}

void D3D_LoadCustomShader(int number, char *code, size_t size, boolean isfragment)
{
	if (number <= SHADER_DEFAULT || number >= NUMBASESHADERS || !code || size > 1024*1024)
	{
		Log("invalid custom shader %d (%u bytes); keeping the built-in shader", number, (unsigned)size);
		return;
	}
	std::string source(code, size);
	while (!source.empty() && source.back() == 0) source.pop_back();
	CustomShader &shader = g_customShaders[number];
	(isfragment ? shader.fragment : shader.vertex) = source;
	shader.dirty = true;
	// Billboard sprites use the clipping variant of the sprite shader.
	if (number == SHADER_SPRITE)
	{
		CustomShader &clip = g_customShaders[SHADER_SPRITECLIPHACK];
		(isfragment ? clip.fragment : clip.vertex) = std::move(source);
		clip.dirty = true;
	}
}

#define DRIVERFUNCTION(name) {#name, reinterpret_cast<void *>(&D3D_##name)}
const struct
{
	const char *name;
	void *function;
} kDriverFunctions[] =
{
	DRIVERFUNCTION(Init),
	DRIVERFUNCTION(SetPalette),
	DRIVERFUNCTION(FinishUpdate),
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

void *D3D12_GetDriverFunction(const char *name)
{
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		I_OutputMsg("UWP: VibeRant backend: D3D12\n");
	}

	for (const auto &function : kDriverFunctions)
	{
		if (strcmp(function.name, name) == 0)
			return function.function;
	}
	Log("no driver function %s", name);
	return nullptr;
}

boolean D3D12_Surface(INT32 w, INT32 h)
{
	static bool first = true;

	if (first)
	{
		Log("setting up a %dx%d screen", (int)w, (int)h);
		first = false;
	}

	if (!Core_Device() || g_shutdown)
	{
		Log("no device to draw a %dx%d screen with", (int)w, (int)h);
		return false;
	}

	glanisotropicmode_cons_t[1].value = 16; // the former Direct3D 11 driver's kMaximumAnisotropy

	SetModelView(w, h);
	SetStates();
	ClearScreenColor();
	ClearScreenDepth();

	HWR_Startup();
	return true;
}

void D3D12_FinishUpdate(INT32 waitvbl)
{
	const bool vsync = waitvbl != 0; // the swap chain's sync interval

	if (!Core_Device() || !g_rt || g_deviceLost || g_shutdown)
		return;

	Prewarm_LogDone();
	// Frames that are only a clear, made while the renderer starts, would show black between the loading screen
	// and the intro. Until something is drawn, the window keeps the loading screen; the clears stay in the list.
	if (!g_drewSinceStart)
		return;
	const bool trace = Present_Tracing();
	if (trace)
		Log("presenting frame %d: %d clears, %d polygons, %d batches, %d models so far", g_stats.frames,
			g_stats.clears, g_stats.polygons, g_stats.batches, g_stats.models);

	// With palette rendering, the screen shown is the finished frame turned into the screen's palette
	const bool palette = PalettePostprocess();
	const precise_t start = I_GetPreciseTime();
	Present_Frame(palette ? g_postTexture.res.Get() : g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, g_rtWidth, g_rtHeight, vsync);
	g_stats.presentTime += I_GetPreciseTime() - start;
	g_stats.frames++;
	g_vertexRing.EndFrame(); g_indexRing.EndFrame(); g_constantRing.EndFrame();
	g_textureRing.EndFrame();
	IDXGIAdapter3 *adapter = Core_MemoryAdapter();
	g_localMemoryValid = adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &g_localMemory));
	if (g_localMemoryValid && g_localMemory.CurrentUsage > g_localMemory.Budget) g_textureFlushRequested = true;
	g_hueFrame++;

	if (trace)
	{
		Log("frame %d done: %d errors, %u command allocators, %u releases pending", g_stats.frames, Core_Errors(),
			(unsigned)Core_Allocators(), (unsigned)Core_PendingReleases());
		Present_FrameDone();
	}

	if (Core_DeviceRemoved())
	{
		g_deviceLost = true;
		const HRESULT removed = Core_RemovedReason();
		Core_LogDeviceRemoved();
		I_Error("Legacy GL's Direct3D 12 device stopped working (0x%08X).\nSet Renderer to Software in Video Options > Advanced if it keeps happening.",
			(unsigned)removed);
	}

	// The next frame starts at the new hue, whether or not the game clears it
	if (kDiagnosticHue)
		ClearScreenColor();
}

boolean D3D12_TakeTextureFlushRequest(void)
{
	const bool requested = g_textureFlushRequested;
	g_textureFlushRequested = false;
	if (!requested)
		return false;
	const precise_t now = I_GetPreciseTime();
	if (g_textureFlushed && now - g_lastTextureFlush < 5 * I_GetPrecisePrecision())
		return false;
	g_lastTextureFlush = now;
	g_textureFlushed = true;
	g_budgetTextureFlush = g_localMemoryValid && g_localMemory.CurrentUsage > g_localMemory.Budget;
	// Descriptor exhaustion already logged its reason; only budget requests use Flush's budget message.
	return true;
}

boolean D3D12_RequestSurfaceProbe(INT32 x, INT32 y)
{
	if (x < 0 || x > 100 || y < 0 || y > 100 || g_probePending || g_probeActive) return false;
	g_probePercentX = x; g_probePercentY = y; g_probePending = true;
	return true;
}

boolean D3D12_BeginSurfaceProbe(void)
{
	return BeginSurfaceProbe();
}

void D3D12_EndSurfaceProbe(void)
{
	EndSurfaceProbe();
}

void D3D12_SurfaceProbeSource(const char *kind, INT32 source)
{
	if (!g_probeActive) return;
	g_probeKind = kind; g_probeSource = source;
}

void D3D12_SurfaceProbeTexture(const char *name)
{
	if (!g_probeActive || !g_units[0]) return;
	snprintf(g_units[0]->probeName, sizeof g_units[0]->probeName, "%.8s", name);
}

// Unknown until the current device's complete portal pipeline preflight succeeds or fails.
int g_portalsSupported = -1;

void D3D12_SetPortalPlane(const float *worldPlane)
{
	if (!worldPlane)
	{
		g_gl.clipPlane[0] = g_gl.clipPlane[1] = g_gl.clipPlane[2] = 0;
		g_gl.clipPlane[3] = 1;
		return;
	}
	// Inverse transpose of the current CAMERA's affine modelview. Set this
	// after SetTransform(view), before model transforms are pushed. Shaders
	// evaluate it against eye positions after the full modelview, including
	// each model's local rotation/scale/translation. Supports roll and flips.
	const float *m = g_modelview.back().m;
	const float det = m[0]*(m[5]*m[10]-m[9]*m[6])
		- m[4]*(m[1]*m[10]-m[9]*m[2]) + m[8]*(m[1]*m[6]-m[5]*m[2]);
	float *p = g_gl.clipPlane;
	p[0] = ((m[5]*m[10]-m[9]*m[6])*worldPlane[0]
		+ (m[9]*m[2]-m[1]*m[10])*worldPlane[1]
		+ (m[1]*m[6]-m[5]*m[2])*worldPlane[2]) / det;
	p[1] = ((m[8]*m[6]-m[4]*m[10])*worldPlane[0]
		+ (m[0]*m[10]-m[8]*m[2])*worldPlane[1]
		+ (m[4]*m[2]-m[0]*m[6])*worldPlane[2]) / det;
	p[2] = ((m[4]*m[9]-m[8]*m[5])*worldPlane[0]
		+ (m[8]*m[1]-m[0]*m[9])*worldPlane[1]
		+ (m[0]*m[5]-m[4]*m[1])*worldPlane[2]) / det;
	p[3] = worldPlane[3] - p[0]*m[12] - p[1]*m[13] - p[2]*m[14];
}

void D3D12_SetPortalStencil(INT32 level)
{
	g_gl.stencil = level > 0;
	g_gl.stencilRef = (UINT8)level;
	g_gl.stencilPass = D3D12_STENCIL_OP_KEEP;
	g_gl.colorWriteMask = 7;
	if (level > 0)
	{
		g_stats.portalStencilLevels++;
		g_stats.portalMaxLevel = std::max(g_stats.portalMaxLevel, (int)g_gl.stencilRef);
	}
}

boolean D3D12_PortalsSupported(void)
{
	if (!CanDraw() || !g_depth || !g_root)
		return false;
	if (g_portalsSupported >= 0)
		return g_portalsSupported != 0;
	const GLState saved = g_gl;
	g_gl.stencil = g_gl.depthTest = true;
	g_gl.colorWriteMask = 0;
	g_gl.cullFace = g_gl.polygonOffset = g_gl.alphaTest = g_gl.texture2D = g_gl.blend = false;
	bool ok = true;
	// The former Direct3D 11 driver's six mask/depth variants, and the reset with its actual fullscreen shader/layout.
	for (int pass = 0; pass < 6; pass++)
	{
		g_gl.depthAlways = pass == 2 || pass == 3 || pass == 5;
		g_gl.depthMask = pass == 2 || pass == 3 || pass == 4;
		g_gl.stencilPass = pass == 0 ? D3D12_STENCIL_OP_INCR_SAT
			: pass == 3 ? D3D12_STENCIL_OP_DECR_SAT : D3D12_STENCIL_OP_KEEP;
		const PipelineKey key = StatePipelineKey(LAYOUT_POLY, VS_POLY, PS_FIXED, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ok = FindOrCreatePipeline(key) != nullptr && ok;
	}
	g_gl.depthAlways = g_gl.depthMask = true;
	g_gl.stencilPass = D3D12_STENCIL_OP_KEEP;
	const PipelineKey reset = StatePipelineKey(LAYOUT_NONE, VS_FULLSCREEN, PS_FIXED, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ok = FindOrCreatePipeline(reset) != nullptr && ok;
	g_gl = saved;
	g_portalsSupported = ok ? 1 : 0;
	Log("portals %s: seven pipelines preflighted", ok ? "supported" : "unavailable");
	return ok;
}

boolean D3D12_DrawPortalAperture(const FOutVector *v, INT32 level, boolean restore)
{
	if (!CanDraw())
		return false;
	const FOutVector triangles[6] = {v[0],v[1],v[2],v[0],v[2],v[3]};
	const GLState saved = g_gl;
	const int program = g_program;
	g_gl.stencil = g_gl.depthTest = true;
	g_gl.stencilRef = (UINT8)(restore ? level+1 : level);
	g_gl.stencilPass = restore ? D3D12_STENCIL_OP_DECR_SAT : D3D12_STENCIL_OP_INCR_SAT;
	g_gl.depthAlways = g_gl.depthMask = restore != 0;
	g_gl.colorWriteMask = 0;
	g_gl.cullFace = g_gl.polygonOffset = g_gl.alphaTest = g_gl.texture2D = g_gl.blend = false;
	g_program = -1;
	const bool drawn = DrawUploaded(LAYOUT_POLY, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triangles, 6, nullptr, 0);
	g_gl = saved;
	g_program = program;
	if (drawn)
		g_stats.portalApertures++;
	return drawn;
}

void D3D12_ResetPortalDepth(INT32 level)
{
	if (!CanDraw())
		return;
	// ClearDepthStencilView ignores stencil. Write far depth only at the child's stencil level.
	const GLState saved = g_gl;
	const int program = g_program;
	g_gl.stencil = g_gl.depthTest = g_gl.depthAlways = g_gl.depthMask = true;
	g_gl.stencilRef = (UINT8)level;
	g_gl.stencilPass = D3D12_STENCIL_OP_KEEP;
	g_gl.colorWriteMask = 0;
	g_gl.fullscreenDepth = 1;
	g_gl.cullFace = g_gl.polygonOffset = g_gl.alphaTest = g_gl.texture2D = g_gl.blend = false;
	g_program = -1;
	if (ApplyState(LAYOUT_NONE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
		&& g_constantRing.Touch(g_constantAllocation, 512))
	{
		Core_FlushBarriers();
		Core_List()->DrawInstanced(3, 1, 0, 0);
		g_stats.draws++;
		g_stats.vertices += 3;
		g_stats.portalDepthResets++;
	}
	g_gl = saved;
	g_program = program;
	// Restore root arguments, targets and viewport, and invalidate every dynamic binding cache.
	BindRenderTarget();
}

boolean D3D12_ApplyFadeMap(const UINT8 *table, size_t size, UINT32 tableId, UINT8 strength)
{
	if (ApplyFadeMap(table, size, tableId, strength))
		return true;
	g_stats.fadeMapFallbacks++;
	return false;
}

void D3D12_GetPresentTrace(INT32 *copied, INT32 *shown, UINT32 *copyframe, UINT32 *showframe)
{
	Present_GetTrace(copied, shown, copyframe, showframe);
}

void D3D12_ReleaseWindow(void)
{
	Present_ReleaseWindow();
}

void D3D12_LogPerf(double seconds)
{
	const Stats &s = g_stats;
	const double n = s.frames ? s.frames : 1;

	if (!Core_Device())
		return;

	const CoreStats core = Core_TakeStats();
	I_OutputMsg("UWP perf:   D3D12: %.1f frames/s; %.2f submissions per frame (mid-frame: ring %d, readback %d, upload %d, other %d); %.1f barriers per frame; %d command allocators (%d made); %u releases pending; %d errors\n",
		s.frames / seconds, core.submissions / n, core.ringSubmissions, core.readbackSubmissions, core.uploadSubmissions,
		core.otherSubmissions, core.barriers / n, (int)Core_Allocators(), core.allocatorsCreated,
		(unsigned)Core_PendingReleases(), Core_Errors());
	I_OutputMsg("UWP perf:   D3D12: fence waits %d, %.2f ms total (worst %.2f), %d timeouts; presenting %.2f ms per frame\n",
		core.waits, core.waitMs, core.worstWaitMs, core.waitTimeouts, PreciseToMs(s.presentTime) / n);
	Present_LogPerf(n);
	I_OutputMsg("UWP perf:   D3D12: per frame %.1f clears, %.0f polygons, %.0f batches, %.1f lines; %.0f draws, %.0f vertices, %.0f indices, %.0f constant updates (root CBV ring)\n",
		s.clears / n, s.polygons / n, s.batches / n, s.lines / n, s.draws / n, s.vertices / n, s.indices / n, s.constantUpdates / n);
	I_OutputMsg("UWP perf:   D3D12: pipeline: %.0f PSO changes/frame; cache %u alive, %llu created (%.1f ms total, worst %.1f ms); %.0f root CBV sets, %.0f table sets/frame; prewarm %s\n",
		s.psoChanges / n, (unsigned)g_pipelines.size(), (unsigned long long)g_psoCreated, g_psoMs, g_psoWorstMs,
		s.rootSets / n, s.tableSets / n, Prewarm_Done() ? "done" : "running");
	const RingStats vertex = g_vertexRing.TakeStats(), index = g_indexRing.TakeStats(), constants = g_constantRing.TakeStats();
	I_OutputMsg("UWP perf:   D3D12: rings: vertex/index/constant peak MB per frame %.2f/%.2f/%.2f; wraps %u/%u/%u; stalls %u (%.2f ms); grows %u\n",
		vertex.peakBytes / 1048576.0, index.peakBytes / 1048576.0, constants.peakBytes / 1048576.0,
		vertex.wraps, index.wraps, constants.wraps, vertex.stalls + index.stalls + constants.stalls,
		vertex.stallMs + index.stallMs + constants.stallMs, vertex.grows + index.grows + constants.grows);
	const RingStats staging = g_textureRing.TakeStats();
	I_OutputMsg("UWP perf:   D3D12: textures: %llu created, %llu uploads (%.2f MB), %llu deleted, %d flushes, %u alive; SRVs used %u/65536, samplers %u/2048; allocated %.2f MB vs requested %.2f MB; staging ring peak %.2f MB, %u wraps, %u stalls (%.2f ms), %u grows\n",
		(unsigned long long)s.texturesCreated, (unsigned long long)s.textureUploads, s.textureUploadBytes / 1048576.0,
		(unsigned long long)s.texturesDeleted, s.flushes, (unsigned)std::count_if(g_textures.begin(), g_textures.end(),
			[](const auto &entry) { return !!entry.second.res; }),
		g_srvIndices.Used() + 4, g_nextSampler, g_textureAllocatedBytes / 1048576.0, g_textureBytes / 1048576.0,
		staging.peakBytes / 1048576.0, staging.wraps, staging.stalls, staging.stallMs, staging.grows);
	I_OutputMsg("UWP perf:   D3D12: screens: %.1f per frame (%.1f copies, %.1f barriers); wipes %d full, %d two-pass; post image %d; fade maps %d drawn, %d left to the overlay\n",
		s.screens / n, s.screenCopies / n, core.barriers / n, s.wipes, s.wipeFallbacks, s.postImages, s.fadeMaps, s.fadeMapFallbacks);
	I_OutputMsg("UWP perf:   D3D12: water: %.1f copies/frame, %.2f MP/frame; fog: %.1f captures/frame\n", s.waterCopies / n, s.waterPixels / n / 1000000.0, s.fogCaptures / n);
	I_OutputMsg("UWP perf:   D3D12: portals: %.1f apertures/frame, %.1f depth resets/frame, %.1f stencil levels/frame, maximum level %d\n",
		s.portalApertures / n, s.portalDepthResets / n, s.portalStencilLevels / n, s.portalMaxLevel);

	if (g_paletteRendering || s.lightTablesCreated || s.lightTablesUpdated || s.paletteUploads)
		I_OutputMsg("UWP perf:   D3D12: palette: rendering %s; %u light tables alive, %d created, %d updated; %d LUT/palette uploads; %.2f post passes per frame, %.2f ms per frame\n",
			g_paletteRendering ? "on" : "off", (unsigned)g_lightTables.size(), s.lightTablesCreated, s.lightTablesUpdated,
			s.paletteUploads, s.paletteSteps / n, PreciseToMs(s.paletteTime) / n);
	I_OutputMsg("UWP perf:   D3D12: models %.1f/frame, skies %.1f/frame; fallbacks %d, lerps %d, sky rebuilds %d; static arena %u chunks (%.2f MB), %u dedicated buffers; mips %llu generated (%.2f CPU ms)\n",
		s.models / n, s.skies / n, s.modelFallbacks, s.modelLerps, s.skyBuilds,
		(unsigned)StaticChunkCount(), StaticChunkCount() * kStaticChunkBytes / 1048576.0,
		(unsigned)g_staticDedicated, (unsigned long long)s.mipsGenerated, s.mipMs);
#ifdef _UWP
	I_OutputMsg("UWP perf:   D3D12: models: %u loaded lazily during drawing\n", (unsigned)HWR_UWPTakeLazyModelLoads());
#endif
	I_OutputMsg("UWP perf:   D3D12: texture heaps: %llu placed, %llu dedicated, %llu committed created; %u heaps (%.2f MB used, %.2f MB allocated, peak %.2f MB); placements 4 KB %llu, 64 KB %llu; creation %.2f CPU ms\n",
		(unsigned long long)g_placedCreated, (unsigned long long)g_dedicatedCreated, (unsigned long long)g_committedCreated, (unsigned)g_textureHeaps.size(),
		g_textureHeapUsed / 1048576.0, g_textureHeaps.size() * kTextureHeapBytes / 1048576.0, g_textureHeapPeak / 1048576.0,
		(unsigned long long)g_smallPlaced, (unsigned long long)g_normalPlaced, s.textureCreateMs);

	DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
	IDXGIAdapter3 *adapter = Core_MemoryAdapter();
	const bool localValid = adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
	const UINT64 targets = (g_rt ? (UINT64)g_rtWidth * g_rtHeight * 4 : 0) + (g_depth ? (UINT64)g_rtWidth * g_rtHeight * 4 : 0)
		+ (g_postTexture.res ? (UINT64)g_postTexture.width * g_postTexture.height * 4 : 0) + Present_SwapChainBytes();
	if (localValid)
		I_OutputMsg("UWP perf:   D3D12: memory: local budget %.1f, usage %.1f MB; render targets/swap chain ~%.1f MB\n",
			local.Budget / 1048576.0, local.CurrentUsage / 1048576.0, targets / 1048576.0);
	else
		I_OutputMsg("UWP perf:   D3D12: memory: local unavailable; render targets/swap chain ~%.1f MB\n", targets / 1048576.0);

	memset(&g_stats, 0, sizeof g_stats);
}

void D3D12_GetCaptureCountsForTest(UINT64 *fogCopies, UINT64 *waterCopies, UINT64 *waterRoundTrips)
{
	*fogCopies = g_testFogCopies;
	*waterCopies = g_testWaterCopies;
	*waterRoundTrips = g_testWaterRoundTrips;
}

void D3D12_GetReuseCountsForTest(UINT64 *placed, UINT64 *blocks, UINT64 *srvs)
{
	*placed = g_placedCreated; *blocks = g_textureArena.Reuses(); *srvs = g_srvReuses;
}

void D3D12_GetTextureStorageCountsForTest(UINT64 *placed, UINT64 *dedicated, UINT64 *committed)
{
	*placed = g_placedCreated; *dedicated = g_dedicatedCreated; *committed = g_committedCreated;
}

boolean D3D12_GetTexturePlacementForTest(UINT32 id, UINT32 *heap, UINT64 *offset,
	UINT64 *blockBytes, UINT64 *reportedBytes, UINT64 *alignment)
{
	const auto *texture = FindTexture(id);
	if (!texture || !texture->res || !texture->heapBlock.valid) return false;
	const auto desc = texture->res->GetDesc();
	const auto info = Core_Device()->GetResourceAllocationInfo(0, 1, &desc);
	*heap = texture->heapBlock.chunk; *offset = texture->heapBlock.offset;
	*blockBytes = texture->heapBlock.size; *reportedBytes = info.SizeInBytes; *alignment = info.Alignment;
	return true;
}

void D3D12_ReadPaletteRectForTest(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16 *dst_data)
{
	if (PalettePostprocess())
		ReadRectFrom(g_postTexture.res.Get(), x, y, width, height, dst_stride, dst_data);
}

void D3D12_SetRingBytesForTest(size_t vertex, size_t index, size_t constant)
{
	g_testVertexBytes = vertex;
	g_testIndexBytes = index;
	g_testConstantBytes = constant;
}

void D3D12_Shutdown(void)
{
	if (g_shutdown || !Core_Device())
		return;
	g_shutdown = true;
	Prewarm_Stop();

	// Nothing is released until the GPU has finished with it; if it doesn't finish, D3D12's objects are kept
	const bool idle = Core_WaitIdle(2000, "shutting down");
	Log("shutting down (GPU %s)", idle ? "idle" : "still busy");
	Present_Shutdown(idle); // the swap chain, then ANGLE's device
	if (idle)
	{
		g_rt.Reset();
		g_depth.Reset();
	}
	else
	{
		Core_Retire(g_rt);
		Core_Retire(g_depth);
	}
	if (idle)
		g_readRectBuffer.Reset();
	else
	{
		Core_Retire(g_readRectBuffer);
		g_readRectBuffer.Reset();
	}
	g_readRectBytes = g_readRectFence = 0;
	Core_SetListBegin(nullptr);
	g_vertexRing.Shutdown(); g_indexRing.Shutdown(); g_constantRing.Shutdown();
	g_textureRing.Shutdown();
	while (!g_textures.empty()) DeleteTextureObject(g_textures.begin()->first);
	D3D_ClearLightTables();
	RetireTexture(g_paletteLookup);
	RetireTexture(g_screenPalette);
	RetireTexture(g_masterPalette);
	RetireTexture(g_postTexture);
	RetireScreenSrv();
	D3D_FlushScreenTextures();
	for (auto &fade : g_fadeMaps) RetireTexture(fade.second.texture);
	g_fadeMaps.clear();
	RetireTexture(g_blackTexture);
	g_samplers.clear();
	for (auto &pipeline : g_pipelines) Core_Retire(pipeline.second);
	g_pipelines.clear(); g_lastPipeline = g_boundPipeline = nullptr;
	g_portalsSupported = -1;
	Core_Retire(g_root); Core_Retire(g_srvHeap); Core_Retire(g_samplerHeap); Core_Retire(g_zeroConstants);
	ReleaseModelBuffers();
	ShutdownArenas();
	Core_Shutdown(idle);
}
