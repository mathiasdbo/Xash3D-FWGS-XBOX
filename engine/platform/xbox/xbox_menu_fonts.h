/*
xbox_menu_fonts.h - the menu's fonts released as a map load starts and
rebuilt as it ends
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #127). The menu gives its font
atlases, font files and background back while a map loads, and rebuilt
the fonts on first use - the first pause-menu open, which cost that frame
over 300 ms on the console. The engine now drives both ends through two
commands the menu registers on the Xbox (3rdparty/mainui BaseMenu.cpp):
"menu_releasefonts" as every load begins, before the server loads the map,
and "menu_restorefonts" once the client has loaded it, behind the loading
plaque, so the rebuild is part of the load. Client code only: include it
under #if XASH_XBOX in a client file, after common.h.
*/

#ifndef XBOX_MENU_FONTS_H
#define XBOX_MENU_FONTS_H

/* Physical memory that must be free for the restore to run, in 4 KiB
   pages: 8 MiB. Restored, the fonts take 1.0-1.7 MiB of free memory
   (xemu, MmQueryStatistics: atlases that overflow ref/nv2a's
   preallocated texture arena, font files, glyph tables; atlases inside
   the arena do not show). Since divergence #157 ref/nv2a copies each
   cached atlas straight from the file buffer, so a restore's lowest free
   memory is at most ~0.3 MiB below where it ends (xemu, against ~0.8 MiB
   when every atlas was decoded to RGBA32 first); an atlas over 1024 on a
   side or a coloured one still takes the decode, and a cache miss (the
   first boot after the cache format changes) renders the atlas in
   mainui, 2.5-3 MiB more. Below this the fonts
   stay released, and the menu restores them on first use as before. */
#define XBOX_MENU_FONTS_MIN_FREE_PAGES ( 8u * 256u )

/* SCR_BeginLoadingPlaque (engine/client/cl_scrn.c), for a load that is
   not a background map: runs the menu's "menu_releasefonts". Host frame
   only, never from inside a menu callback. */
void Xbox_MenuFonts_LoadStarting( void );

/* CL_RegisterResources (engine/client/parse/cl_parse.c), after the
   renderer's new-map setup and Mod_FreeUnused: the client's map load
   is done, its memory peak behind it. Runs "menu_restorefonts" when at
   least XBOX_MENU_FONTS_MIN_FREE_PAGES are free. */
void Xbox_MenuFonts_MapLoaded( void );

#endif /* XBOX_MENU_FONTS_H */
