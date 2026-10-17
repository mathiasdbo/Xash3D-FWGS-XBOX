/*
xbox_igr.c - in-game reset: both triggers + Back + Start quits to the dashboard
(docs/r3d/divergences.md #179)

Retail titles implement the combo themselves (no BIOS does it for an nxdk
XBE), so without this the console had to be power-cycled to leave the game.
The combo only queues "quit": the engine shuts down as it does from the menu
(config.cfg written, the frame recorder's session closed), Host_Main returns,
and launcher.c's main launches the dashboard.
*/
#include <SDL.h>

#include "common.h"
#include "platform/platform.h"

// triggers are 0..32767 in SDL; most of the travel, as retail titles ask
#define XBOX_IGR_TRIGGER 24000

static qboolean xbox_igr_issued;

static qboolean Xbox_IGR_Held( SDL_GameController *gc )
{
	return SDL_GameControllerGetAxis( gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) > XBOX_IGR_TRIGGER
		&& SDL_GameControllerGetAxis( gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT ) > XBOX_IGR_TRIGGER
		&& SDL_GameControllerGetButton( gc, SDL_CONTROLLER_BUTTON_BACK )
		&& SDL_GameControllerGetButton( gc, SDL_CONTROLLER_BUTTON_START );
}

/*
=============
Xbox_IGR_Poll

after the frame's SDL events: the combo on any open pad queues "quit" once
=============
*/
void Xbox_IGR_Poll( void )
{
	int i, n;

	if( xbox_igr_issued || host.shutdown_issued )
		return;

	n = SDL_NumJoysticks();
	for( i = 0; i < n; i++ )
	{
		SDL_GameController *gc = SDL_GameControllerFromInstanceID( SDL_JoystickGetDeviceInstanceID( i ));

		if( gc && Xbox_IGR_Held( gc ))
		{
			xbox_igr_issued = true;
			Con_Printf( "in-game reset: quitting to the dashboard\n" );
			Cbuf_AddText( "quit\n" );
			return;
		}
	}
}
