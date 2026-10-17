/*
vox_stub/client.h - the client state the sound files read, for xbox_vox_stream_host_test
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

/* Only the names the sound files use, with the engine's meaning (the
   mouth's fields are common/cl_entity.h's, the voice indices
   engine/client/voice.h's). One client state for both builds; the
   entities, whose mouths the test compares, are per build
   (CL_GetEntityByIndex is renamed). */

#ifndef VOX_STUB_CLIENT_H
#define VOX_STUB_CLIENT_H

#include "sound_api.h"

typedef struct cl_entity_s
{
	mouth_t mouth;
} cl_entity_t;

typedef enum { ca_disconnected = 0, ca_connecting, ca_connected, ca_validate, ca_active, ca_cinematic } connstate_t;
enum { key_console = 0, key_game, key_menu, key_message };

typedef struct client_static_s
{
	connstate_t state;
	int key_dest;
} client_static_t;

typedef struct client_s
{
	int servercount;
	qboolean background;
	qboolean paused;
} client_t;

typedef struct clgame_s
{
	sound_interface_t soundFuncs;
} clgame_static_t;

typedef struct ref_state_s
{
	vec3_t vieworg;
} ref_state_t;

extern client_static_t cls;
extern client_t cl;
extern clgame_static_t clgame;
extern ref_state_t refState;
extern convar_t sys_timescale;

#define VOICE_LOOPBACK_INDEX    ( -2 )
#define VOICE_LOCALCLIENT_INDEX ( -1 )

cl_entity_t *CL_GetEntityByIndex( int index );
qboolean CL_GetEntitySpatialization( channel_t *ch );
qboolean CL_IsInGame( void );
qboolean CL_IsPlayerIndex( int idx );
qboolean Host_IsSinglePlayerGame( void );
void SX_RoomFX( portable_samplepair_t *paint, int num_samples );
void SX_ClearState( void );
void SNDDMA_BeginPainting( void );
void SNDDMA_Submit( void );
float S_GetMasterVolume( void );

#endif /* VOX_STUB_CLIENT_H */
