/*
xbox_frametime_client.c - the frame-time recorder's hooks in client code
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

/* Resonance3D (docs/r3d/divergences.md #141 (11)): see
   xbox_frametime_client.h. A dedicated build has no client: nothing here. */

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "client.h"
#include "xbox_frametime_client.h"

void Xbox_FT_CL_CloseMessage( xbox_ft_cl_msg_t *m, sizebuf_t *msg )
{
	const char *name = NULL;
	char number[32];
	uint64_t ticks;
	int i;

	if( m->cmd < 0 )
		return;

	// ticks, so the kilocycle division runs only for a slow message
	ticks = xbox_ft_span_ticks_since( &m->span );
	if( ticks >= (uint64_t)XBOX_FT_NOTE_MSG_KC * 1000u )
	{
		const uint32_t kc = (uint32_t)(( ticks + 500u ) / 1000u );

		if( m->cmd <= svc_lastmsg )
			name = svc_strings[m->cmd];
		else
		{
			// the lookup CL_ParseUserMessage does (cl_parse.c)
			for( i = 0; i < MAX_USER_MESSAGES; i++ )
			{
				if( clgame.msg[i].number == m->cmd )
				{
					name = clgame.msg[i].name;
					break;
				}
			}
		}

		if( COM_StringEmptyOrNULL( name ))
		{
			Q_snprintf( number, sizeof( number ), "svc %d", m->cmd );
			name = number;
		}

		Xbox_FT_Note( XBOX_FT_NOTE_MSG, name, kc, (uint32_t)( MSG_GetNumBytesRead( msg ) - m->start ));
	}
	m->cmd = -1;
}

byte *Xbox_FT_UI_LoadFile( const char *filename, int *pLength )
{
	byte *data;

	XBOX_FT_SPAN( XBOX_FT_S_UI_FILE, data = COM_LoadFileForMe( filename, pLength ));
	return data;
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
