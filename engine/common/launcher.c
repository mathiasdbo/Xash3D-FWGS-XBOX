/*
launcher.c - direct xash3d launcher
Copyright (C) 2015 Mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#if XASH_ENABLE_MAIN
#include "build.h"
#include "common.h"

#if XASH_SDLMAIN
#include <SDL.h>
#endif

#if XASH_XBOX
#include <hal/xbox.h>
#endif

#ifndef XASH_GAMEDIR
#define XASH_GAMEDIR "valve" // !!! Replace with your default (base) game directory !!!
#endif

static int  szArgc;
static char **szArgv;

static void Sys_ChangeGame( const char *progname )
{
	// stub
}

int main( int argc, char **argv )
{
#if XASH_PSVITA
	// inject -dev -console into args if required
	szArgc = PSVita_GetArgv( argc, argv, &szArgv );
#elif XASH_IOS
	IOS_LaunchDialog();
	szArgc = IOS_GetArgs( &szArgv );
#else
	szArgc = argc;
	szArgv = argv;
#endif // XASH_PSVITA
#if XASH_XBOX
	// The engine has shut down (quit, the menu, the in-game reset, an error):
	// back to the dashboard. Returning would end in nxdk's exit(), a quick
	// reboot with no launch data. Divergence #179.
	Host_Main( szArgc, szArgv, XASH_GAMEDIR, 0, Sys_ChangeGame );
	XLaunchXBE( NULL );
	return 0; // XLaunchXBE returns only if it failed
#else
	return Host_Main( szArgc, szArgv, XASH_GAMEDIR, 0, Sys_ChangeGame );
#endif
}
#endif // XASH_ENABLE_MAIN
