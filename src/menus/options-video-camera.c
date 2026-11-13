// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  menus/options-video-camera.c
/// \brief Camera Position: third or first person for each local player

#include "../k_menu.h"
#include "../r_main.h" // cv_chasecam

// The rows show cv_dummycameraposition, whose values read "3rd Person" and
// "1st Person". The setting itself stays chasecam, with the On and Off values
// that the console, Lua scripts and config files already use for it.
menuitem_t OPTIONS_VideoCamera[] =
{

	{IT_STRING | IT_CVAR, "Player 1", "Player 1's view: behind the kart, or from the driver's seat.",
		NULL, {.cvar = &cv_dummycameraposition[0]}, 0, 0},

	{IT_STRING | IT_CVAR, "Player 2", "Player 2's view: behind the kart, or from the driver's seat.",
		NULL, {.cvar = &cv_dummycameraposition[1]}, 0, 0},

	{IT_STRING | IT_CVAR, "Player 3", "Player 3's view: behind the kart, or from the driver's seat.",
		NULL, {.cvar = &cv_dummycameraposition[2]}, 0, 0},

	{IT_STRING | IT_CVAR, "Player 4", "Player 4's view: behind the kart, or from the driver's seat.",
		NULL, {.cvar = &cv_dummycameraposition[3]}, 0, 0},

};

// Show what chasecam is now. It can change under the menu: the console, a
// config file or a Lua script may set it.
static void M_SyncCameraPosition(void)
{
	UINT8 i;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		if (cv_dummycameraposition[i].value != cv_chasecam[i].value)
			CV_StealthSetValue(&cv_dummycameraposition[i], cv_chasecam[i].value);
	}
}

// A row was changed: move that player's chasecam.
void CameraPosition_OnChange(void)
{
	UINT8 i;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		if (cv_chasecam[i].value != cv_dummycameraposition[i].value)
			CV_SetValue(&cv_chasecam[i], cv_dummycameraposition[i].value);
	}
}

static void M_CameraPositionTick(void)
{
	M_OptionsTick();
	M_SyncCameraPosition();
}

menu_t OPTIONS_VideoCameraDef = {
	sizeof (OPTIONS_VideoCamera) / sizeof (menuitem_t),
	&OPTIONS_VideoDef,
	0,
	OPTIONS_VideoCamera,
	48, 80,
	SKINCOLOR_PLAGUE, 0,
	MBF_DRAWBGWHILEPLAYING,
	NULL,
	2, 5,
	M_DrawGenericOptions,
	M_DrawOptionsCogs,
	M_CameraPositionTick,
	M_SyncCameraPosition,
	NULL,
	NULL,
};
