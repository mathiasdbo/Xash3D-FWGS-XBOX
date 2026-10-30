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
// Resonance3D: anamorphic 16:9 on the 640x480 output (ui-architecture.md
// section 8). 0 follows the Dashboard's widescreen setting, 1 is 4:3, 2 is
// 16:9; the index of the Display screen's "Aspect" row. Live: a change
// re-reports the screen size (VID_CheckChanges).
static CVAR_DEFINE_AUTO( r3d_vid_aspect, "0", FCVAR_ARCHIVE, "640x480 output's aspect: 0 as the Dashboard is set, 1 4:3, 2 16:9 (anamorphic)" );
// Resonance3D (#190): the output mode, read at boot before the configs run
// (vid_sdl2.c, R_Init_Video); a change here applies at the next start. The
// index of the Display screen's "Video mode" row.
static CVAR_DEFINE_AUTO( r3d_vid_mode, "0", FCVAR_ARCHIVE, "output mode from the next start: 0 640x480 (480p where the Dashboard allows it), 1 1280x720 where the Dashboard allows 720p" );
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

	if( vm.height == 720 )
		aspect = "16:9"; // square pixels: no anamorphic mapping (#190)
	else if( VID_XboxRenderWidth( 640, 480 ) != 640 )
		aspect = "16:9";
	else if( FBitSet( enc, VIDEO_WIDESCREEN ))
		aspect = "16:9 set, 4:3 drawn"; // r3d_vid_aspect 1, or a renderer that draws 1:1
	else if( FBitSet( enc, VIDEO_LETTERBOX ))
		aspect = "Letterbox";
	else aspect = "4:3";

	Cvar_FullSet( "vid_refresh", va( "%d", vm.refresh ), vid_refresh.flags );
	Cvar_FullSet( "vid_aspect", aspect, vid_aspect.flags );
}

/*
=================
VID_XboxRenderWidth

Resonance3D: the width the game is told the w x h output has. Xbox
widescreen at 480 lines is anamorphic: the framebuffer stays 640x480 and
the television stretches it to 16:9, so its pixels are 4:3 wide, not
square. Reported as 854x480, the whole client - V_AdjustFov's Hor+
widening (it reads the aspect from pixel sizes, cl_view.c), the HUD's
ScreenWidth anchors, mainui's height-anchored canvas - lays out a 16:9
screen, and ref/nv2a maps that onto the 640 real pixels: the 3D
viewport is always the whole framebuffer, the projection takes fov_x and
fov_y as given, and the 2D matrix scales the engine's size to the back
buffer's (nv2a_2d_build_matrix). ref/soft draws the engine's size 1:1
into its surface, so it keeps 640. HD modes (720p) have square pixels.
=================
*/
int VID_XboxRenderWidth( int w, int h )
{
	const char *ref_name = ref.dllFuncs.R_GetConfigName ? ref.dllFuncs.R_GetConfigName() : NULL;
	int aspect = (int)r3d_vid_aspect.value;

	if( w != 640 || h != 480 )
		return w;

	if( !ref_name || Q_strcmp( ref_name, "ref_nv2a" ))
		return w;

	if( aspect != 1 && aspect != 2 )
		aspect = FBitSet( XVideoGetEncoderSettings(), VIDEO_WIDESCREEN ) ? 2 : 1;

	return aspect == 2 ? (( h * 16 / 9 + 1 ) & ~1 ) : w;
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

#if XASH_XBOX
	if( FBitSet( r3d_vid_aspect.flags, FCVAR_CHANGED ))
	{
		const int w = window_width.value, h = window_height.value;

		ClearBits( r3d_vid_aspect.flags, FCVAR_CHANGED );

		// not under vid_scale's quarter-size render target
		if( refState.height == h )
		{
			VID_SetXboxVideoCvars(); // before the menu's VidInit reads it
			R_SaveVideoMode( w, h, VID_XboxRenderWidth( w, h ), h, vid_maximized.value );
		}
	}
#endif

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
	Cvar_RegisterVariable( &r3d_vid_aspect );
	Cvar_RegisterVariable( &r3d_vid_mode );
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
