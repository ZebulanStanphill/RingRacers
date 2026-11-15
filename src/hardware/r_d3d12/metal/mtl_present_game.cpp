// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl_present_game.cpp
/// \brief What the Metal presenter needs of the game (mtl_present_game.h), from a file that can include the game's headers

#include "mtl_present_game.h"

#include "SDL.h"

#include "../../../doomdef.h"
#include "../../../i_system.h"
#include "../../../screen.h"
#include "../../../sdl/sdlmain.h"

namespace rr_mtl_present
{

SDL_Window *GameWindow()
{
	return window;
}

void SetGameWindow(SDL_Window *target)
{
	window = target;
}

void SetWindowIcon(SDL_Window *target)
{
	VID_SetWindowIcon(target);
}

int ScreenScalingMode()
{
	return cv_scr_scaling.value;
}

void Output(const char *line)
{
	I_OutputMsg("%s", line);
}

void Fatal(const char *message)
{
	I_Error("%s", message);
}

} // namespace rr_mtl_present
