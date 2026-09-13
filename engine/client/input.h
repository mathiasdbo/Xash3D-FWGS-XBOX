/*
input.h - win32 input devices
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef INPUT_H
#define INPUT_H

/*
==============================================================

INPUT

==============================================================
*/

#include "keydefs.h"

//
// input.c
//
void IN_Init( void );
void Host_InputFrame( void );
void IN_Shutdown( void );
// R3D: on every other platform the client module is a separate DLL, so
// the engine's own IN_ActivateMouse/IN_DeactivateMouse/IN_MouseEvent
// (this file, below - real, multi-file-used engine input dispatch
// functions, each internally calling the CLIENT's own same-named
// function through clgame.dllFuncs, engine/client/dll_int/cl_game.c) and
// the HL SDK's own client-side functions of the exact same name
// (deps/hlsdk/cl_dll/inputw32.cpp, a real, required client export -
// tools/xbox-client-required-exports.txt) never collide: two DLLs, two
// private symbol tables. Xbox statically links everything into one
// binary, so without this the two real, both-needed definitions are a
// genuine duplicate-symbol link error - found running the first real
// combined server+client link (tools/xbox-game-link-check.sh hl
// --include-client). The client SDK's own names are the ones a real
// COM_GetProcAddress-style lookup resolves BY NAME (tools/xbox-client-
// required-exports.txt), so they must stay exactly as-is; these ENGINE
// functions have no such external-name requirement (every real caller
// - in_keys.c, in_touch.c, the SDL host_sdl*.c files - reaches them
// through this header's own declaration, never by looking up a string),
// so renaming their LINKER symbol only, not their C-level name any
// caller writes, is the correct, minimal fix.
#if XASH_XBOX
void IN_MouseEvent( int key, int down ) asm( "Host_IN_MouseEvent" );
void IN_ActivateMouse( void ) asm( "Host_IN_ActivateMouse" );
void IN_DeactivateMouse( void ) asm( "Host_IN_DeactivateMouse" );
#else
void IN_MouseEvent( int key, int down );
void IN_ActivateMouse( void );
void IN_DeactivateMouse( void );
#endif
void IN_MWheelEvent( int direction );
void IN_MouseSavePos( void );
void IN_MouseRestorePos( void );
void IN_ToggleClientMouse( int newstate, int oldstate );
void IN_GyroInit( void );
void IN_GyroCheckAvailability( void );
void IN_GyroEvent( vec3_t data );
void IN_GyroFinalizeMove( float *fw, float *side, float *dpitch, float *dyaw );
void IN_GyroDrawDebug( void );

uint IN_CollectInputDevices( void );
void IN_LockInputDevices( qboolean lock );
void IN_EngineAppendMove( float frametime, usercmd_t *cmd, qboolean active );

void IN_SetRelativeMouseMode( qboolean set );
void IN_SetMouseGrab( qboolean set );

extern convar_t m_yaw;
extern convar_t m_pitch;
extern convar_t touch_enable;

//
// in_touch.c
//
typedef enum
{
	event_down = 0,
	event_up,
	event_motion
} touchEventType;

#if XASH_NO_TOUCH
static inline void Touch_Draw( void ) { }
static inline void Touch_SetClientOnly( byte state ) { }
static inline void Touch_RemoveButton( const char *name, qboolean privileged ) { }
static inline void Touch_HideButtons( const char *name, unsigned char hide, qboolean privileged ) { }
static inline void Touch_AddClientButton( const char *name, const char *texture, const char *command, float x1, float y1, float x2, float y2, byte *color, int round, float aspect, int flags ) { }
static inline void Touch_AddDefaultButton( const char *name, const char *texturefile, const char *command, float x1, float y1, float x2, float y2, byte *color, int round, float aspect, int flags ) { }
static inline void Touch_WriteConfig( void ) { }
static inline void Touch_Init( void ) { }
static inline void Touch_Shutdown( void ) { }
static inline void Touch_GetMove( float * forward, float *side, float *pitch, float *yaw ) { }
static inline void Touch_ResetDefaultButtons( void ) { }
static inline int IN_TouchEvent( touchEventType type, int fingerID, float x, float y, float dx, float dy ) { return 0; }
static inline void Touch_KeyEvent( int key, int down ) { }
static inline qboolean Touch_WantVisibleCursor( void ) { return false; }
static inline void Touch_NotifyResize( void ) { }
#else
void Touch_Draw( void );
void Touch_SetClientOnly( byte state );
void Touch_RemoveButton( const char *name, qboolean privileged );
void Touch_HideButtons( const char *name, unsigned char hide, qboolean privileged );
void Touch_AddClientButton( const char *name, const char *texture, const char *command, float x1, float y1, float x2, float y2, byte *color, int round, float aspect, int flags );
void Touch_AddDefaultButton( const char *name, const char *texturefile, const char *command, float x1, float y1, float x2, float y2, byte *color, int round, float aspect, int flags );
void Touch_WriteConfig( void );
void Touch_Init( void );
void Touch_Shutdown( void );
void Touch_GetMove( float * forward, float *side, float *pitch, float *yaw );
void Touch_ResetDefaultButtons( void );
int IN_TouchEvent( touchEventType type, int fingerID, float x, float y, float dx, float dy );
void Touch_KeyEvent( int key, int down );
qboolean Touch_WantVisibleCursor( void );
void Touch_NotifyResize( void );
#endif

//
// in_osk.c
//
extern convar_t osk_enable;
void OSK_Init( void );
qboolean OSK_KeyEvent( int key, int down );
void OSK_EnableTextInput( qboolean enable, qboolean force );
void OSK_Draw( void );

//
// in_joy.c
//
enum
{
	JOY_HAT_CENTERED = 0,
	JOY_HAT_UP    = BIT(0),
	JOY_HAT_RIGHT = BIT(1),
	JOY_HAT_DOWN  = BIT(2),
	JOY_HAT_LEFT  = BIT(3),
	JOY_HAT_RIGHTUP   = JOY_HAT_RIGHT | JOY_HAT_UP,
	JOY_HAT_RIGHTDOWN = JOY_HAT_RIGHT | JOY_HAT_DOWN,
	JOY_HAT_LEFTUP    = JOY_HAT_LEFT  | JOY_HAT_UP,
	JOY_HAT_LEFTDOWN  = JOY_HAT_LEFT  | JOY_HAT_DOWN
};

typedef enum engineAxis_e
{
	JOY_AXIS_SIDE = 0,
	JOY_AXIS_FWD,
	JOY_AXIS_PITCH,
	JOY_AXIS_YAW,
	JOY_AXIS_RT,
	JOY_AXIS_LT,
	MAX_AXES,
} engineAxis_t;

typedef enum joy_calibration_state_s
{
	JOY_NOT_CALIBRATED = 0,
	JOY_CALIBRATING,
	JOY_FAILED_TO_CALIBRATE,
	JOY_CALIBRATED
} joy_calibration_state_t;

qboolean Joy_IsActive( void );
void Joy_SetCapabilities( qboolean have_gyro );
void Joy_SetCalibrationState( joy_calibration_state_t state );
void Joy_AxisMotionEvent( engineAxis_t engineAxis, short value );
void Joy_GyroEvent( vec3_t data );
void Joy_FinalizeMove( float *fw, float *side, float *dpitch, float *dyaw );
void Joy_DrawDebug( void );
void Joy_Init( void );
void Joy_Shutdown( void );

#endif//INPUT_H
