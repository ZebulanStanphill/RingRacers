// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2000 by DooM Legacy Team.
// Copyright (C) 1996 by id Software, Inc.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_system.h
/// \brief System specific interface stuff.

#ifndef __I_SYSTEM__
#define __I_SYSTEM__

#include "d_ticcmd.h"
#include "d_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**	\brief max quit functions
*/
#define MAX_QUIT_FUNCS     16


/**	\brief Graphic system had started up
*/
extern UINT8 graphics_started;

/**	\brief Keyboard system is up and run
*/
extern UINT8 keyboard_started;

/** \brief Set to true when inside a signal handler that will exit the program. */
extern boolean g_in_exiting_signal_handler;

/**	\brief	The I_GetFreeMem function

	\param	total	total memory in the system

	\return	free memory in the system
*/
UINT32 I_GetFreeMem(UINT32 *total);

/**	\brief	Returns precise time value for performance measurement. The precise
            time should be a monotonically increasing counter, and will wrap.
			precise_t is internally represented as an unsigned integer and
			integer arithmetic may be used directly between values of precise_t.
  */
precise_t I_GetPreciseTime(void);

/** \brief  Get the precision of precise_t in units per second. Invocations of
            this function for the program's duration MUST return the same value.
  */
UINT64 I_GetPrecisePrecision(void);

/** \brief  Get the current time in rendering tics, including fractions.
*/
double I_GetFrameTime(void);

/**	\brief	Sleeps for the given duration in milliseconds. Depending on the
            operating system's scheduler, the calling thread may give up its
			time slice for a longer duration. The implementation should give a
			best effort to sleep for the given duration, without spin-locking.
			Calling code should check the current precise time after sleeping
			and not assume the thread has slept for the expected duration.

	\return	void
*/
void I_Sleep(UINT32 ms);

boolean I_CheckFrameCap(precise_t start, precise_t end);

/**	\brief Get events

	Called by D_SRB2Loop,
	called before processing each tic in a frame.
	Quick syncronous operations are performed here.
	Can call D_PostEvent.
*/
void I_GetEvent(void);

/**	\brief Asynchronous interrupt functions should maintain private queues
	that are read by the synchronous functions
	to be converted into events.
*/
void I_OsPolling(void);

/**	\brief Called by M_Responder when quit is selected, return exit code 0
*/
FUNCNORETURN void ATTRNORETURN I_Quit(void);

typedef enum
{
	EvilForce = -1,
	//Constant
	ConstantForce = 0,
	//Ramp
	RampForce,
	//Periodics
	SquareForce,
	SineForce,
	TriangleForce,
	SawtoothUpForce,
	SawtoothDownForce,
	//MAX
	NumberofForces,
} FFType;

struct JoyFF_t
{
	INT32 ForceX; ///< The X of the Force's Vel
	INT32 ForceY; ///< The Y of the Force's Vel
	//All
	UINT32 Duration; ///< The total duration of the effect, in microseconds
	INT32 Gain; //< /The gain to be applied to the effect, in the range from 0 through 10,000.
	//All, CONSTANTFORCE -10,000 to 10,000
	INT32 Magnitude; ///< Magnitude of the effect, in the range from 0 through 10,000.
	//RAMPFORCE
	INT32 Start; ///< Magnitude at the start of the effect, in the range from -10,000 through 10,000.
	INT32 End; ///< Magnitude at the end of the effect, in the range from -10,000 through 10,000.
	//PERIODIC
	INT32 Offset; ///< Offset of the effect.
	UINT32 Phase; ///< Position in the cycle of the periodic effect at which playback begins, in the range from 0 through 35,999
	UINT32 Period; ///< Period of the effect, in microseconds.
};

/**	\brief	Forcefeedback for the first joystick

	\param	Type   what kind of Effect
	\param	Effect Effect Info

	\return	void
*/

void I_Tactile(FFType Type, const JoyFF_t *Effect);

/**	\brief	Forcefeedback for the second joystick

	\param	Type   what kind of Effect
	\param	Effect Effect Info

	\return	void
*/
void I_Tactile2(FFType Type, const JoyFF_t *Effect);

/**	\brief	Forcefeedback for the third joystick

\param	Type   what kind of Effect
\param	Effect Effect Info

\return	void
*/
void I_Tactile3(FFType Type, const JoyFF_t *Effect);

/**	\brief	Forcefeedback for the fourth joystick

\param	Type   what kind of Effect
\param	Effect Effect Info

\return	void
*/
void I_Tactile4(FFType Type, const JoyFF_t *Effect);

/**	\brief to set up the first joystick scale
*/
void I_JoyScale(void);

/**	\brief to set up the second joystick scale
*/
void I_JoyScale2(void);

/**	\brief to set up the third joystick scale
*/
void I_JoyScale3(void);

/**	\brief to set up the fourth joystick scale
*/
void I_JoyScale4(void);

// Called by D_SRB2Main.

/// Startup input subsystems.
void I_StartupInput(void);

/**	\brief to startup the first joystick
*/
void I_InitJoystick1(void);

/**	\brief to startup the second joystick
*/
void I_InitJoystick2(void);

/**	\brief to startup the third joystick
*/
void I_InitJoystick3(void);

/**	\brief to startup the fourth joystick
*/
void I_InitJoystick4(void);

/**	\brief return the number of joystick on the system
*/
INT32 I_NumJoys(void);

/**	\brief	The *I_GetJoyName function

	\param	joyindex	which joystick

	\return	joystick name
*/
const char *I_GetJoyName(INT32 joyindex);

#ifndef NOMUMBLE
#include "p_mobj.h" // mobj_t
#include "s_sound.h" // listener_t
/** \brief to update Mumble of Player Postion
*/
void I_UpdateMumble(const mobj_t *mobj, const listener_t listener);
#endif

/**	\brief Startup the mouse
*/
void I_StartupMouse(void);

/**	\brief  setup timer irq and user timer routine.
*/
void I_StartupTimer(void);

/**	\brief sample quit function
*/
typedef void (*quitfuncptr)();

/**	\brief	add a list of functions to call at program cleanup

	\param	(*func)()	funcction to call at program cleanup

	\return	void
*/
void I_AddExitFunc(void (*func)());

/**	\brief	The I_RemoveExitFunc function

	\param	(*func)()	function to remove from the list

	\return	void
*/
void I_RemoveExitFunc(void (*func)());

/**	\brief Setup signal handler, plus stuff for trapping errors and cleanly exit.
*/
INT32 I_StartupSystem(void);

/**	\brief Shutdown systems
*/
void I_ShutdownSystem(void);

/**	\brief	The I_GetDiskFreeSpace function

	\param	freespace	a INT64 pointer to hold the free space amount

	\return	void
*/
void I_GetDiskFreeSpace(INT64 *freespace);

/**	\brief find out the user's name
*/
char *I_GetUserName(void);

/**	\brief	The I_mkdir function

	\param	dirname	string of mkidr
	\param	unixright	unix right

	\return status of new folder
*/
INT32 I_mkdir(const char *dirname, INT32 unixright);

/**	\brief Find main WAD
		\return path to main WAD
*/
const char *I_LocateWad(void);

/**	\brief First Joystick's events
*/
void I_GetJoystickEvents(UINT8 index);

/**	\brief Checks if the mouse needs to be grabbed
*/
void I_UpdateMouseGrab(void);

char *I_GetEnv(const char *name);

INT32 I_PutEnv(char *variable);

/** \brief Put data in system clipboard
*/
INT32 I_ClipboardCopy(const char *data, size_t size);

/** \brief Retrieve data from system clipboard
*/
const char *I_ClipboardPaste(void);

void I_RegisterSysCommands(void);

void I_CursedWindowMovement(int xd, int yd);

boolean I_HasOpenURL(void);

void I_OpenURL(const char *data);

#ifdef _UWP
// [UWP]: Time I_OutputMsg has spent writing messages to the log file and debugger, to see how much logging slows startup
extern precise_t i_uwplogtime;

// [UWP]: Implemented by the launcher using MemoryManager. Returns 0 on success,
// otherwise an HRESULT. Reports the actual process budget and whether this is Xbox.
int I_UWPGetMemoryBudget(UINT64 *limit, UINT64 *usage, int *isXbox);

// [UWP]: Poll the gamepads directly, including while startup is busy on the main thread.
int I_UWPIsSkipButtonDown(void);

// [UWP]: Logs the process budget. GlobalMemoryStatusEx reports physical memory, not this budget.
void I_UWPLogMemoryLimit(void);

// [UWP]: Exits with a pop-up explaining how to set the app type to Game if Direct3D 12 hardware isn't available
// (sdl/i_uwpd3d12.cpp). Logs signals that might tell the App and Game app types apart.
void I_UWPRequireGameMode(void);

// [UWP]: Starts sampling what the main thread is doing, for E:\ringracers\uwp-profile.txt (sdl/i_uwpprofile.cpp).
// Call from the main thread.
void I_UWPStartProfiler(void);

// [UWP]: How many samples the profiler took, and how long they paused the main thread, since the last call
void I_UWPTakeProfilerStats(INT32 *samples, double *pausedms);

// [UWP]: Logs the CPU sets Windows lists for the game, which should show which cores are the game's own when Xbox
// runs it as a game. Call from the main thread.
void I_UWPLogCpuSets(void);

// [UWP]: How many processors Xbox marks as the game's own, leaving out those it keeps for the system, or 0 if it
// doesn't mark any
unsigned int I_UWPUsableProcessors(void);

// [UWP]: How Xbox has given out processors: 1 if some are the game's own (Game resources), 0 if it holds some for
// another process and none for the game (App resources: seen on Xbox One X every time the app type was App), or -1
// if it doesn't say
int I_UWPCpuSetMode(void);

// [UWP]: Logs crashes, with their call stacks, to the log, which the game's signal handler misses on Xbox.
// Call from the main thread.
void I_UWPInstallCrashHandler(void);

// [UWP]: Counts which processor the main thread is running on, and writes which ones it was on since the last
// call and how often, like "2 (60%), 3 (40%)"
void I_UWPNoteProcessor(void);
void I_UWPTakeProcessorStats(char *buffer, size_t size);

// [UWP]: What the main thread is doing, for the profiler
typedef enum
{
	UWPPROF_OTHER,
	UWPPROF_LOGIC, // game tics, and building bots' ticcmds
	UWPPROF_3D, // rendering the 3D view
	UWPPROF_2D, // the rest of D_Display: the HUD, menus, wipes, and passing the software screen to the GPU
	UWPPROF_DISPLAY, // I_FinishUpdate: drawing the 2D layer, scaling it to the screen, and presenting
	NUMUWPPROFPHASES
} uwpprofphase_t;
extern volatile INT32 i_uwpprofphase;

// [UWP]: Whether a level is being played (not paused), for the profiler, which only counts samples taken then
extern volatile INT32 i_uwpprofinlevel;

// [UWP]: The points of a Software frame ANGLE's GPU work is timed between (sdl/i_uwpangle.cpp)
typedef enum
{
	UWPANGLE_SCREEN_START, // before VID_DisplaySoftwareScreen
	UWPANGLE_SCREEN_END, // after it
	UWPANGLE_FINISH_START, // the start of I_FinishUpdate's RHI part
	UWPANGLE_PRESENT_START, // just before the present
	UWPANGLE_PRESENT_END, // just after it, but the GPU only reaches it with the next frame's commands
	UWPANGLE_NUMMARKS
} uwpanglemark_t;

// [UWP]: Takes a GPU timestamp from ANGLE's Direct3D 11 device. Does nothing unless the Software renderer is drawing
// with ANGLE's context current. Call from the main thread.
void I_UWPAngleMark(uwpanglemark_t mark);

// [UWP]: ANGLE's GPU time on Software's frames, in milliseconds summed over the frames timed
typedef struct
{
	INT32 frames; // timed
	INT32 notready; // dropped because the GPU hadn't finished them when they were read
	INT32 disjoint; // dropped because the timer's frequency changed
	double screen; // uploading the software screen
	double finish; // flushing the 2D layer, upscaling and ImGui
	double idle; // from the present until the next frame's commands reach the GPU
	double total; // screen and finish, excluding idle
	double worst; // total, of the slowest frame, excluding idle
} uwpanglestats_t;

// [UWP]: Takes the stats since the last call. Returns whether any frames were timed.
boolean I_UWPTakeAngleStats(uwpanglestats_t *stats);

// [UWP]: Releases the timing queries and ANGLE's device, before the OpenGL ES context is deleted
void I_UWPShutdownAngleTiming(void);
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#if defined(_UWP) && defined(__cplusplus)
// [UWP]: Sets what the main thread is doing, for the profiler, until the end of the scope
struct UWPProfPhase
{
	INT32 previous;

	explicit UWPProfPhase(INT32 phase) : previous(i_uwpprofphase) { i_uwpprofphase = phase; }
	~UWPProfPhase() { i_uwpprofphase = previous; }
};
#endif

#endif
