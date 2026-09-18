/*
vid_common.c - common vid component
Copyright (C) 2018 a1batross, Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "mod_local.h"
#include "input.h"
#include "vid_common.h"
#include "platform/platform.h"
#if XASH_XBOX
#include <hal/video.h> // XVideoGetMode, XVideoGetEncoderSettings - UI_GetVideoInfo
#endif

static CVAR_DEFINE_AUTO( vid_mode, "0", FCVAR_RENDERINFO, "current video mode index (used only for storage)" );
static CVAR_DEFINE_AUTO( vid_rotate, "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen rotation (0-3)" );
static CVAR_DEFINE_AUTO( vid_scale, "1.0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "pixel scale" );

CVAR_DEFINE_AUTO( vid_maximized, "0", FCVAR_RENDERINFO, "window maximized state, read-only" );
CVAR_DEFINE( vid_fullscreen, "fullscreen", DEFAULT_FULLSCREEN, FCVAR_RENDERINFO|FCVAR_VIDRESTART, "fullscreen state (0 windowed, 1 fullscreen, 2 borderless)" );
CVAR_DEFINE( window_width, "width", "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen width" );
CVAR_DEFINE( window_height, "height", "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen height" );
CVAR_DEFINE( vid_width, "vid_width", "0", FCVAR_READ_ONLY, "actual window viewport size" );
CVAR_DEFINE( vid_height, "vid_height", "0", FCVAR_READ_ONLY, "actual window viewport size" );
#if XASH_XBOX
// XM.1 item 7: real Xbox display state for the Display screen's
// read-only "Video output" row (VID_SetXboxVideoCvars, below) - plain
// read-only cvars, the same primitive vid_width/vid_height already are,
// not a new engine export (see that function's own comment for why).
// Guarded, unlike vid_width/vid_height: those are real and populated on
// every platform, but these two have exactly one caller
// (VideoOptions.cpp's own Xbox-only "Video output" row) and exactly one
// real populator (VID_SetXboxVideoCvars, called only from vid_sdl2.c's
// Xbox arm) - Codex review round 2 caught the first draft registering
// them everywhere with a permanent, meaningless "0"/"" on every other
// platform, reserving two global cvar names a downstream game or mod
// could otherwise register for its own purpose (Cvar_RegisterVariable's
// own duplicate-name check would reject it), for values nothing there
// ever computes for real. Xbox-only, cheapest divergence rule.
CVAR_DEFINE( vid_refresh, "vid_refresh", "0", FCVAR_READ_ONLY, "actual display refresh rate in Hz" );
CVAR_DEFINE( vid_aspect, "vid_aspect", "", FCVAR_READ_ONLY, "actual display aspect/format" );
#endif // XASH_XBOX

glwstate_t	glw_state;

/*
=================
R_SaveVideoMode
=================
*/
void R_SaveVideoMode( int w, int h, int render_w, int render_h, qboolean maximized )
{
	string temp;

	if( !w || !h || !render_w || !render_h )
	{
		host.renderinfo_changed = false;
		return;
	}

	host.window_center_x = w / 2;
	host.window_center_y = h / 2;

	Q_snprintf( temp, sizeof( temp ), "%d", w );
	Cvar_DirectSet( &window_width, temp );

	Q_snprintf( temp, sizeof( temp ), "%d", h );
	Cvar_DirectSet( &window_height, temp );

	Q_snprintf( temp, sizeof( temp ), "%d", render_w );
	Cvar_FullSet( "vid_width", temp, vid_width.flags );

	Q_snprintf( temp, sizeof( temp ), "%d", render_h );
	Cvar_FullSet( "vid_height", temp, vid_width.flags  );

	Cvar_DirectSet( &vid_maximized, maximized ? "1" : "0" );
	
	// immediately drop changed state or we may trigger
	// video subsystem to reapply settings
	host.renderinfo_changed = false;

	refState.scale_x = (float)render_w / w;
	refState.scale_y = (float)render_h / h;

	if( refState.width == render_w && refState.height == render_h )
		return;

	refState.width = render_w;
	refState.height = render_h;

	// check for 4:3 or 5:4
	refState.wideScreen = render_w * 3 != render_h * 4 && render_w * 4 != render_h * 5;

	SCR_VidInit(); // tell client.dll that vid_mode has changed
}

/*
=================
VID_GetModeString
=================
*/
const char *VID_GetModeString( int vid_mode )
{
	if( vid_mode < 0 || vid_mode >= R_MaxVideoModes() )
		return NULL;

	vidmode_t *vidmode = R_GetVideoMode( vid_mode );
	if( !vidmode )
		return NULL;

	return vidmode->desc;
}

#if XASH_XBOX
/*
=================
VID_SetXboxVideoCvars

XM.1 item 7: a read-only "Video output" row on the Xbox Display screen
needs real hardware state, not a literal string (Codex review of the
plan, round 2 - the design canvas's own "640x480 - 4:3 - 60 Hz, set in
Dashboard" is the NTSC 4:3 case, not a fixed string). Refresh comes from
XVideoGetMode() - after X5's own XVideoSetMode(640, 480, 32,
REFRESH_DEFAULT) call, .refresh is the real negotiated 50 or 60, not the
literal 0 that call passed in (video.c's XVideoListModes resolves it
against the console's real TV standard). Aspect comes from
XVideoGetEncoderSettings()'s VIDEO_WIDESCREEN/VIDEO_LETTERBOX bits
(hal/video.h:34-41) - a real, cached hardware query
(AvSendTVEncoderOption), not a stub. Width/height need no new cvar:
vid_width/vid_height already exist and already mirror the real,
X5-clamped 640x480.

Read-only cvars, not a new ui_enginefuncs_t export - Codex review round 1
caught the first draft of this doing exactly that: `menu_int.h` is
explicitly commented "menu interface is freezed at version 0.75", and
while appending past that point is the established way to grow it
(`pfnRandomFloat`/`pfnGetModeString`/`COM_SaveFile` were all added the
same way), `udll_int.cpp`'s `GetMenuAPI` copies `sizeof(ui_enginefuncs_t)`
bytes using mainui's OWN compiled size, not the engine's - on the PC
dynamic-module boundary (this header is shared by every platform, not
just Xbox's single static link), a newer mainui loaded by an older
engine would read past the end of the engine's shorter table and later
call whatever garbage landed in the new slot as a function pointer. This
project's Xbox build links everything from one source tree at once and
could never hit that path, but the header itself is upstream-shared, so
the fix is the same one Codex named: use the primitive this codebase
already trusts for exactly this class of value (vid_width/vid_height
are read-only cvars already), not a new struct field.
=================
*/
void VID_SetXboxVideoCvars( void )
{
	VIDEO_MODE vm = XVideoGetMode();
	DWORD enc = XVideoGetEncoderSettings();
	const char *aspect;

	if( FBitSet( enc, VIDEO_WIDESCREEN ))
		aspect = "16:9";
	else if( FBitSet( enc, VIDEO_LETTERBOX ))
		aspect = "Letterbox";
	else aspect = "4:3";

	Cvar_FullSet( "vid_refresh", va( "%d", vm.refresh ), vid_refresh.flags );
	Cvar_FullSet( "vid_aspect", aspect, vid_aspect.flags );
}
#endif // XASH_XBOX

/*
==================
VID_CheckChanges

check vid modes and fullscreen
==================
*/
void VID_CheckChanges( void )
{
	if( FBitSet( cl_allow_levelshots.flags, FCVAR_CHANGED ))
	{
		//GL_FreeTexture( cls.loadingBar );
		SCR_RegisterTextures(); // reload 'lambda' image
		ClearBits( cl_allow_levelshots.flags, FCVAR_CHANGED );
	}

	if( host.renderinfo_changed )
	{
		if( VID_SetMode( ))
		{
			SCR_VidInit(); // tell the client.dll what vid_mode has changed
		}
		else
		{
			Sys_Error( "Can't re-initialize video subsystem\n" );
		}
		host.renderinfo_changed = false;
	}
}

/*
===============
VID_SetDisplayTransform

notify ref dll about screen transformations
===============
*/
void VID_SetDisplayTransform( int *render_w, int *render_h )
{
	uint rotate = vid_rotate.value;

	if( rotate < REF_ROTATE_NONE || rotate > REF_ROTATE_CCW )
		rotate = REF_ROTATE_NONE;

	if( ref.dllFuncs.R_SetDisplayTransform( rotate, 0, 0, vid_scale.value, vid_scale.value ))
	{
		if( rotate & 1 )
		{
			int swap = *render_w;

			*render_w = *render_h;
			*render_h = swap;
		}

		*render_h /= vid_scale.value;
		*render_w /= vid_scale.value;

		ref.rotation = rotate;
	}
	else
	{
		Con_Printf( S_WARN "failed to setup screen transform\n" );

		ref.rotation = REF_ROTATE_NONE;
	}
}

static void VID_Mode_f( void )
{
	int w, h;

	switch( Cmd_Argc() )
	{
	case 2:
	{
		vidmode_t *vidmode = R_GetVideoMode( Q_atoi( Cmd_Argv( 1 )) );
		if( !vidmode )
		{
			Con_Printf( S_ERROR "unable to set mode, backend returned null\n" );
			return;
		}

		w = vidmode->width;
		h = vidmode->height;
		break;
	}
	case 3:
	{
		w = Q_atoi( Cmd_Argv( 1 ));
		h = Q_atoi( Cmd_Argv( 2 ));
		break;
	}
	default:
		Msg( S_USAGE "vid_mode <modenum>|<width height>\n" );
		return;
	}

	R_ChangeDisplaySettings( w, h, bound( 0, vid_fullscreen.value, WINDOW_MODE_COUNT - 1 ));
}

void VID_Init( void )
{
	// system screen width and height (don't suppose for change from console at all)
	Cvar_RegisterVariable( &window_width );
	Cvar_RegisterVariable( &window_height );

	Cvar_RegisterVariable( &vid_mode );
	Cvar_RegisterVariable( &vid_rotate );
	Cvar_RegisterVariable( &vid_scale );
	Cvar_RegisterVariable( &vid_fullscreen );
	Cvar_RegisterVariable( &vid_maximized );
	Cvar_RegisterVariable( &vid_width );
	Cvar_RegisterVariable( &vid_height );
#if XASH_XBOX
	Cvar_RegisterVariable( &vid_refresh );
	Cvar_RegisterVariable( &vid_aspect );
#endif
	Cvar_Get( "_window_xpos", "-1", FCVAR_RENDERINFO, "deprecated cvar" );
	Cvar_Get( "_window_ypos", "-1", FCVAR_RENDERINFO, "deprecated cvar" );

	// a1ba: planned to be named vid_mode for compability
	// but supported mode list is filled by backends, so numbers are not portable any more
	Cmd_AddRestrictedCommand( "vid_setmode", VID_Mode_f, "display video mode" );
	Cmd_AddCommand( "vid_info", VID_Info_f, "show vid component info" );

	V_Init(); // init gamma
	R_Init(); // init renderer
}
