/*
xbox_menu_fonts.c - the menu's fonts released as a map load starts and
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

/* Resonance3D (docs/r3d/divergences.md #127): see xbox_menu_fonts.h.
   A dedicated build has no client and no menu: nothing here. */

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include <xboxkrnl/xboxkrnl.h>
#include <hal/video.h> /* XVideoGetMode: 720p keeps the fonts released (#196) */
#include "xbox_frametime.h"
#include "xbox_menu_fonts.h"

#define MENU_RELEASE_CMD "menu_releasefonts"
#define MENU_RESTORE_CMD "menu_restorefonts"

static uint32_t Xbox_MenuFonts_FreePages( void )
{
	MM_STATISTICS ms;

	memset( &ms, 0, sizeof( ms ));
	ms.Length = sizeof( ms );
	if( !NT_SUCCESS( MmQueryStatistics( &ms )))
		return 0; /* unknown: treated as too little, the menu stays lazy */
	return ms.AvailablePages;
}

void Xbox_MenuFonts_LoadStarting( void )
{
	/* The menu registers the command (UI_Init); without it there is
	   nothing to release. */
	if( Cmd_Exists( MENU_RELEASE_CMD ))
		Cmd_ExecuteString( MENU_RELEASE_CMD );
}

void Xbox_MenuFonts_MapLoaded( void )
{
	xbox_ft_span_t span;
	uint32_t before, after;
	char note[XBOX_FT_NOTE_NAME_BYTES];

	/* The menu registers the command (UI_Init); a build or a moment
	   without it has nothing to restore. */
	if( !Cmd_Exists( MENU_RESTORE_CMD ))
		return;

	/* #196: at 720p the fonts stay released in play (1.9 MiB of atlases,
	   the owner's choice): the first pause-menu open after a load
	   rebuilds them, about 0.3 s, as before #127 */
	if( XVideoGetMode().height == 720 )
	{
		Xbox_FT_Note( XBOX_FT_NOTE_MENU, "fonts left released, 720p", 0, 0 );
		return;
	}

	before = Xbox_MenuFonts_FreePages();
	if( before < XBOX_MENU_FONTS_MIN_FREE_PAGES )
	{
		Con_Reportf( "%s: %u KiB free, menu fonts left to restore on first use\n", __func__, before * 4u );
		Q_snprintf( note, sizeof( note ), "fonts left released, %u KiB free", before * 4u );
		Xbox_FT_Note( XBOX_FT_NOTE_MENU, note, 0, 0 );
		return;
	}

	xbox_ft_span_begin( &span );
	Cmd_ExecuteString( MENU_RESTORE_CMD );
	after = Xbox_MenuFonts_FreePages();

	/* bytes: what the restore took from free memory (0 when nothing was
	   released, or something else freed more meanwhile) */
	Q_snprintf( note, sizeof( note ), "menu_restorefonts ran, %u KiB free before", before * 4u );
	Xbox_FT_Note( XBOX_FT_NOTE_MENU, note, xbox_ft_span_kc_since( &span ),
		before > after ? ( before - after ) * 4096u : 0u );
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
