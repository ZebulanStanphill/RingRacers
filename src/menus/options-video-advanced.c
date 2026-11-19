// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  menus/options-video-gl.c
/// \brief OpenGL Options

#include "../k_menu.h"
#include "../r_main.h"	// cv_skybox
#include "../hardware/hw_main.h"	// gl consvars
#include "../i_video.h" // rendermode

extern consvar_t cv_menuframeskip;

// [UWP]: Driver Thread is a VibeRant D3D setting.
#ifdef _UWP
extern consvar_t cv_grdriverthread;
#endif

// see vaopt_e
menuitem_t OPTIONS_VideoAdvanced[] =
{
	{IT_HEADER, "Performance...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Draw Distance", "How far objects can be drawn. A tradeoff between performance & visibility.",
		NULL, {.cvar = &cv_drawdist}, 0, 0},

	{IT_STRING | IT_CVAR, "Weather Draw Distance", "Affects how far weather visuals can be drawn. Lower values improve performance.",
		NULL, {.cvar = &cv_drawdist_precip}, 0, 0},

	{IT_STRING | IT_CVAR, "Enable Skyboxes", "Turning this off may improve performance, but reduces courses' background details.",
		NULL, {.cvar = &cv_skybox}, 0, 0},

	{IT_STRING | IT_CVAR, "Parallel Software", "Uses multiple CPU cores for the software renderer if available, for a FPS boost.",
		NULL, {.cvar = &cv_parallelsoftware}, 0, 0},

	{IT_STRING | IT_CVAR, "Extra Frame Skipping", "Skip 3D rendering frames while the menu is open.",
		NULL, {.cvar = &cv_menuframeskip}, 0, 0},


	{IT_HEADER, "Rendering Backend...", "Watch people get confused anyway!!",
		NULL, {NULL}, 0, 0},

// [ZE]: VibeRant builds describe VibeRant D3D here, not Legacy GL.
#ifdef HAVE_VIBERANT
	{IT_STRING | IT_SPACE, "VibeRant D3D is an enhanced port of", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "the Legacy GL renderer to Direct3D 12,", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "featuring 3D models, true perspective,", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "& high parity with Software.", "",
		NULL, {NULL}, 0, 0},
#else
	{IT_STRING | IT_SPACE, "Legacy GL supports 3D models and true", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "perspective, but many visual features", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "are missing/broken.", "",
		NULL, {NULL}, 0, 0},
// [ZE]: End of the Legacy GL description that VibeRant builds replace.
#endif

	{IT_SPACE | IT_NOTHING, NULL, NULL,
		NULL, {NULL}, 0, 0},

// [ZE]: VibeRant builds point bug reports at the fork.
#ifdef HAVE_VIBERANT
	{IT_STRING | IT_SPACE, "\x85Report visual bugs to GitHub.com/", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "\x85ZebulanStanphill/RingRacers", "",
		NULL, {NULL}, 0, 0},
#else
	{IT_STRING | IT_SPACE, "\x85Legacy GL will eventually be replaced.", "",
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SPACE, "\x85Please don't report visual bugs!", "",
		NULL, {NULL}, 0, 0}, // Unless you've got an MR to fix them.
// [ZE]: End of the bug report text that VibeRant builds replace.
#endif

	{IT_SPACE | IT_NOTHING, NULL, NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Renderer", "If you don't know why you're changing this, leave it on Software!",
		NULL, {.cvar = &cv_renderer}, 0, 0},

// [ZE]: VibeRant builds name these options after VibeRant D3D.
#ifdef HAVE_VIBERANT
	{IT_HEADER, "VibeRant D3D Options...", "Watch people get confused anyway!!",
#else
	{IT_HEADER, "Legacy GL Options...", "Watch people get confused anyway!!",
// [ZE]: End of the Legacy GL header that VibeRant builds replace.
#endif
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "3D Models", "Use 3D models instead of sprites when applicable.",
		NULL, {.cvar = &cv_glmodels}, 0, 0},

	{IT_STRING | IT_CVAR, "Shaders", "Use GLSL Shaders. Turning them off increases performance at the expanse of visual quality.",
		NULL, {.cvar = &cv_glshaders}, 0, 0},

	// [ZE]: Palette rendering replaces a spacer here, as a setting of the hardware renderers.
	{IT_STRING | IT_CVAR, "Palette Rendering", "Limit colors to the palette, and light with its colormaps, like Software. Needs shaders.",
		NULL, {.cvar = &cv_glpaletterendering}, 0, 0},

#ifdef HAVE_VIBERANT
	// [ZE]: Texture Quality only picks Legacy GL's OpenGL texture format;
	// VibeRant always uses 32-bit textures, so its row offers the filter.
	{IT_STRING | IT_CVAR, "Texture Filter", "Nearest keeps texels sharp like Software. Mipmap modes smooth distant textures.",
		NULL, {.cvar = &cv_glfiltermode}, 0, 0},
#else
	{IT_STRING | IT_CVAR, "Texture Quality", "Texture depth. Higher values are recommended.",
		NULL, {.cvar = &cv_scr_depth}, 0, 0},

	/*
	{IT_STRING | IT_CVAR, "Texture Filter", "Texture Filter. Nearest is recommended.",
		NULL, {.cvar = &cv_glfiltermode}, 0, 0},
	*/
// [ZE]: End of the Texture Quality row that VibeRant builds replace with Texture Filter.
#endif

	{IT_STRING | IT_CVAR, "Anisotropic", "Lower values will improve performance at a minor quality loss.",
		NULL, {.cvar = &cv_glanisotropicmode}, 0, 0},
// [ZE]: Upstream had a spacer row here; the fork dropped it to make room for the added rows.

	{IT_STRING | IT_CVAR, "Sprite Billboarding", "Adjusts sprites when viewed from above or below to not make them appear flat.",
		NULL, {.cvar = &cv_glspritebillboarding}, 0, 0},

	{IT_STRING | IT_CVAR, "Software Perspective", "Emulates Software shearing when looking up or down. Not recommended.",
		NULL, {.cvar = &cv_glshearing}, 0, 0},

#ifdef _UWP
	// [UWP]: Last, as the setting an average player needs least
	{IT_STRING | IT_CVAR, "Batching", "Improves hardware rendering speed. Turn off to check visual problems. Resets on launch.",
		NULL, {.cvar = &cv_glbatching}, 0, 0},
	// [UWP]: This setting applies at the next frame boundary.
	{IT_STRING | IT_CVAR, "Driver Thread", "Draws on a second CPU thread. Turn off to compare speed or check visual problems.",
		NULL, {.cvar = &cv_grdriverthread}, 0, 0},
#endif
};

menu_t OPTIONS_VideoAdvancedDef = {
	sizeof (OPTIONS_VideoAdvanced) / sizeof (menuitem_t),
	&OPTIONS_VideoDef,
	0,
	OPTIONS_VideoAdvanced,
	48, 80,
	SKINCOLOR_PLAGUE, 0,
	MBF_DRAWBGWHILEPLAYING,
	NULL,
	2, 5,
	M_DrawGenericOptions,
	M_DrawOptionsCogs,
	M_OptionsTick,
	NULL,
	NULL,
	NULL,
};

void M_RefreshAdvancedVideoOptions(void)
{
// [ZE]: Describe the renderer the setting names, and show the running driver's options, in VibeRant builds.
#if defined(HAVE_VIBERANT) && !defined(_UWP)
	// The description is of the VibeRant renderer the setting names. Software names none, so with it the description
	// is of the first one this build lists (cv_renderer_t), which is the one a new config uses.
	INT32 described = cv_renderer.value;
	if (described == 1)
	{
#if defined(HAVE_VIBERANT_D3D)
		described = 3;
#elif defined(HAVE_VIBERANT_VULKAN)
		described = 4;
#else
		described = 5;
#endif
	}
	boolean shown_d3d = described != 2;
	boolean shown_vulkan = described == 4;
	boolean running_d3d = HWR_VibeRant();
	OPTIONS_VideoAdvanced[vaopt_spacer3].text = shown_vulkan ? "VibeRant Vulkan is an enhanced port" : shown_d3d ? "VibeRant D3D is an enhanced port of" : "Legacy GL supports 3D models and true";
	OPTIONS_VideoAdvanced[vaopt_spacer4].text = shown_vulkan ? "of the Legacy GL renderer to Vulkan," : shown_d3d ? "the Legacy GL renderer to Direct3D 12," : "perspective, but many visual features";
	OPTIONS_VideoAdvanced[vaopt_spacer5].text = shown_d3d ? "featuring 3D models, true perspective," : "are missing/broken.";
	OPTIONS_VideoAdvanced[vaopt_spacer6].text = shown_d3d ? "& high parity with Software." : "";
	OPTIONS_VideoAdvanced[vaopt_renderer - 3].text = shown_d3d ? "\x85Report visual bugs to GitHub.com/" : "\x85Legacy GL will eventually be replaced.";
	OPTIONS_VideoAdvanced[vaopt_renderer - 2].text = shown_d3d ? "\x85ZebulanStanphill/RingRacers" : "\x85Please don't report visual bugs!";
	OPTIONS_VideoAdvanced[vaopt_legacygl_begin].text = running_d3d ? (HWR_Vulkan() ? "VibeRant Vulkan Options..." : "VibeRant D3D Options...") : "Legacy GL Options...";
#ifdef HAVE_VIBERANT_METAL
	if (described == 5)
	{
		OPTIONS_VideoAdvanced[vaopt_spacer3].text = "VibeRant Metal is an enhanced port";
		OPTIONS_VideoAdvanced[vaopt_spacer4].text = "of the Legacy GL renderer to Metal,";
	}
	if (HWR_Metal())
		OPTIONS_VideoAdvanced[vaopt_legacygl_begin].text = "VibeRant Metal Options...";
#endif
	OPTIONS_VideoAdvanced[vaopt_texturequal].text = running_d3d ? "Texture Filter" : "Texture Quality";
	OPTIONS_VideoAdvanced[vaopt_texturequal].tooltip = running_d3d ? "Nearest keeps texels sharp like Software. Mipmap modes smooth distant textures." : "Texture depth. Higher values are recommended.";
	OPTIONS_VideoAdvanced[vaopt_texturequal].itemaction.cvar = running_d3d ? &cv_glfiltermode : &cv_scr_depth;
#endif
	OPTIONS_VideoAdvancedDef.numitems = rendermode == render_opengl
		? sizeof (OPTIONS_VideoAdvanced) / sizeof (menuitem_t)
		: vaopt_legacygl_begin;
}
