/*
xbox_frametime_client.h - the frame-time recorder's hooks in client code
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
Resonance3D (docs/r3d/divergences.md #141 (11)). Kept out of the upstream
files they serve so those carry only the call sites: the per-message
timing of CL_ParseServerMessage (engine/client/parse/cl_parse.c) and the
timed COM_LoadFile in the menu's engine-function table
(engine/client/dll_int/cl_gameui.c). Client code only: include it under
#if XASH_XBOX in a client file, after common.h.
*/

#ifndef XBOX_FRAMETIME_CLIENT_H
#define XBOX_FRAMETIME_CLIENT_H

#include "xbox_frametime.h"

/* One server message being timed: CL_ParseServerMessage closes the
   previous one at the top of each pass of its loop, so the last message
   is closed before the loop's end-of-message break, and opens the next
   once its command byte is read. */
typedef struct xbox_ft_cl_msg_s
{
	xbox_ft_span_t span;
	int cmd;        /* -1: no message open */
	size_t start;   /* the message's first byte in the packet */
} xbox_ft_cl_msg_t;

/* Closes the open message, if any: one that took XBOX_FT_NOTE_MSG_KC or
   more becomes a NOTES entry named by its svc_* or user-message name. */
void Xbox_FT_CL_CloseMessage( xbox_ft_cl_msg_t *m, sizebuf_t *msg );

/* Opens message `cmd`, which starts at byte `start` of the packet. */
static inline void Xbox_FT_CL_OpenMessage( xbox_ft_cl_msg_t *m, size_t start, int cmd )
{
	xbox_ft_span_begin( &m->span );
	m->start = start;
	m->cmd = cmd;
}

/* COM_LoadFileForMe timed as the span k_ui_file, for the menu's
   engine-function table. */
byte *Xbox_FT_UI_LoadFile( const char *filename, int *pLength );

#endif /* XBOX_FRAMETIME_CLIENT_H */
