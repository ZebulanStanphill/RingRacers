// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl_present_game.h
/// \brief What the Metal presenter needs of the game
///
/// mtl_present.mm is Objective-C++, and the game's headers and Apple's don't go into one file (they disagree about
/// booleans, and Apple's define words the game uses as names). So the presenter reaches the game through these few
/// functions, which mtl_present_game.cpp, a C++ file with the game's headers, implements.

#ifndef RR_MTL_PRESENT_GAME_H
#define RR_MTL_PRESENT_GAME_H

struct SDL_Window;

namespace rr_mtl_present
{

// sdlmain.h's `window`: the window the video code sizes, makes fullscreen, and takes input from
SDL_Window *GameWindow();
void SetGameWindow(SDL_Window *target);

// Gives another window the game's icon
void SetWindowIcon(SDL_Window *target);

// Options > Video > Screen Scaling (cv_scr_scaling): a screen_scaling::Mode
int ScreenScalingMode();

// -mtlunseen: present to the window even while the system says none of it is on screen, which is how a test gets
// frames onto the display of a Mac whose screen is locked or asleep
bool PresentUnseen();

// A line of the log as it is (I_OutputMsg)
void Output(const char *line);

// Stops the game with an error (I_Error)
[[noreturn]] void Fatal(const char *message);

} // namespace rr_mtl_present

#endif
