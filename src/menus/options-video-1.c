// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  menus/options-video-1.c
/// \brief Video Options

#include "../k_menu.h"
// [ZE]: I_VideoAlwaysFullscreen, for removing the Fullscreen option on Xbox.
#include "../i_video.h"
#include "../v_video.h" // cv_globalgamma
#include "../r_fps.h" // fps cvars

// options menu
menuitem_t OPTIONS_Video[] =
{

	{IT_STRING | IT_SUBMENU, "Resolution...", "Change the aspect ratio and image quality.",
		NULL, {.submenu = &OPTIONS_VideoModesDef}, 0, 0},

	{IT_NOTHING|IT_SPACE, NULL, NULL,
		NULL, {NULL}, 0, 0},

#if (defined (__unix__) && !defined (MSDOS)) || defined (UNIXCOMMON) || defined (HAVE_SDL)
	{IT_STRING | IT_CVAR, "Fullscreen", "Play on the big screen or in a small window.",
		NULL, {.cvar = &cv_fullscreen}, 0, 0},
#endif

	// [ZE]: The Screen Scaling setting, which fits, uses whole pixel multiples, or shows pixels at 1:1.
	{IT_STRING | IT_CVAR, "Screen Scaling", "Fit fills the screen. Integer keeps pixels even. 1:1 shows them at their real size.",
		NULL, {.cvar = &cv_scr_scaling}, 0, 0},

	{IT_STRING | IT_CVAR, "V-Sync", "Reduce image tearing and judder.",
		NULL, {.cvar = &cv_vidwait}, 0, 0},

	{IT_STRING | IT_CVAR, "FPS Cap", "Limit the frame rate. Higher values may consume more CPU usage.",
		NULL, {.cvar = &cv_fpscap}, 0, 0},

	{IT_STRING | IT_CVAR, "Screen Effect", "Uses a special effect when displaying the game.",
		NULL, {.cvar = &cv_scr_effect}, 0, 0},
		
	{IT_STRING | IT_SUBMENU, "Color Profile...", "Adjust the color profile of the game's display.",
		NULL, {.submenu = &OPTIONS_VideoColorProfileDef}, 0, 0},

	// [ZE]: Replaces a spacer row with the Camera Position submenu, since the camera setting is saved per player.
	{IT_STRING | IT_SUBMENU, "Camera Position...", "Give each local player a 3rd person or 1st person view.",
		NULL, {.submenu = &OPTIONS_VideoCameraDef}, 0, 0},

	{IT_NOTHING|IT_SPACE, NULL, NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_SUBMENU, "Advanced...", "Advanced performance options and experimental rendering features.",
		NULL, {.submenu = &OPTIONS_VideoAdvancedDef}, 0, 0},

};

menu_t OPTIONS_VideoDef = {
	sizeof (OPTIONS_Video) / sizeof (menuitem_t),
	&OPTIONS_MainDef,
	0,
	OPTIONS_Video,
	48, 80-8,
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

// [ZE]: Compact the live items so removing a setting leaves no empty menu row.
static INT16 M_RemoveVideoCvar(menuitem_t *items, INT16 count, const consvar_t *cvar)
{
	INT16 i, j;

	for (i = 0; i < count; i++)
	{
		if ((items[i].status & IT_TYPE) != IT_CVAR || items[i].itemaction.cvar != cvar)
			continue;

		for (j = i; j + 1 < count; j++)
			items[j] = items[j + 1];
		return count - 1;
	}

	return count;
}

void M_RefreshVideoOptions(void)
{
	if (I_VideoAlwaysFullscreen())
		OPTIONS_VideoDef.numitems = M_RemoveVideoCvar(
			OPTIONS_Video, OPTIONS_VideoDef.numitems, &cv_fullscreen);
}
