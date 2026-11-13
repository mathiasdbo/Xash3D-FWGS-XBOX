/*
xbox_snd_gproom.c - the room effect on the APU's GP: the room mix's stream and the kernel's parameters
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
Resonance3D (docs/r3d/divergences.md #180). The mixer's room buffer, the
sounds s_dsp.c puts the room on, is not processed on the CPU: it goes into
a ring of its own, which xbox_snd_al.c queues onto a second source buffer
for buffer with the dry ring (the same frame times, the same lengths) and
starts with the dry source in one alSourcePlayv, so both reach the APU's
mix on the same frame. The room source's direct path is muted (a lowpass at
gain 0) and its send goes at 1.0 into a slot holding the room kernel,
apu/gp_hldsp.asm, which adds the processed room mix to the dry one, scaled
by the master volume, as S_MixBufferWithGain did.

The kernel's parameters come from SX_R3D_GpParams (s_dsp.c), once per paint
chunk. A change is queued with alXboxEffectSlotAtSample at the stream frame
its chunk starts at, less one GP frame: the kernel applies a frame's
parameters from the next frame on. A change that cannot be queued (the
library holds 16) waits for the next chunk; a newer one replaces it. Before
the streams have started, and after a clear, changes are attached at once:
no room audio is playing then.
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_snd_al.h"
#include "platform/xbox/xbox_snd_al_queue.h"
#include "platform/xbox/xbox_snd_rate_cfg.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "platform/xbox/apu/gp_hldsp.h"

#define GPROOM_LEAD 32 /* one GP frame: the kernel's own lag */

static qboolean s_on;
static ALuint s_src, s_fx, s_slot, s_filter;
static ALuint s_bufs[XBOX_SND_AL_MAX_BUFFERS];
static ALuint s_free[XBOX_SND_AL_MAX_BUFFERS];
static int s_nfree;
static int16_t *s_ring;
static int s_ring_frames, s_speed;
static int s_sent[SXGP_PARAMS]; /* the last change queued or attached */
static qboolean s_sent_valid;
static int s_pend[SXGP_PARAMS]; /* a change not queued yet */
static int s_pend_time;
static qboolean s_pend_on;
static int s_base;              /* the frame time of the streams' frame 0 */
static qboolean s_base_valid;
static uint32_t s_n_timed, s_n_now, s_n_retry, s_n_starts; /* since the last Stats */

/* A config file's setting of `key`: true and its value when it has one */
static qboolean SndCfg_FileValue( const char *file, const char *key, char *val, int size )
{
	fs_offset_t len = 0;
	byte *data = FS_LoadFile( file, &len, false );
	char *text;
	qboolean found;

	if( !data )
		return false;
	text = Mem_Malloc( host.mempool, (size_t)len + 1 );
	memcpy( text, data, (size_t)len );
	text[len] = 0;
	Mem_Free( data );
	found = Xbox_SndCfg_Parse( text, key, val, size );
	Mem_Free( text );
	return found;
}

qboolean Xbox_BootCfg_Value( const char *key, char *val, int size )
{
	search_t *t;
	qboolean found;
	int i;

	found = SndCfg_FileValue( "config.cfg", key, val, size );
	if(( t = FS_Search( "userconfig.d/*.cfg", true, false )))
	{
		for( i = 0; i < t->numfilenames; i++ )
			found |= SndCfg_FileValue( t->filenames[i], key, val, size );
		Mem_Free( t );
	}
	return found;
}

qboolean Xbox_SndCfg_Hardware( void )
{
	char val[8] = "";

	Xbox_BootCfg_Value( "r3d_snd_hw", val, (int)sizeof( val ));
	return Q_atoi( val ) != 0;
}

qboolean Xbox_SndCfg_Asked( const char *key )
{
	search_t *t;
	qboolean asked = false;
	int i;

	// an emulator plays no AC97 and so nothing the GP outputs: the room, the sounds and the speech stay on the CPU mix
	if( xbox_snd_emulator && ( !Q_strcmp( key, "r3d_snd_gproom" ) || !Q_strcmp( key, "r3d_snd_voices" ) || !Q_strcmp( key, "r3d_snd_voices_speech" )))
		return false;
	// the menu's "Hardware audio" asks for every step at once
	if(( !Q_strcmp( key, "r3d_snd_gproom" ) || !Q_strcmp( key, "r3d_snd_voices" ) || !Q_strcmp( key, "r3d_snd_voices_speech" ))
		&& Xbox_SndCfg_Hardware())
		return true;
	if( !( t = FS_Search( "userconfig.d/*.cfg", true, false )))
		return false;
	for( i = 0; i < t->numfilenames && !asked; i++ )
	{
		fs_offset_t len = 0;
		byte *data = FS_LoadFile( t->filenames[i], &len, false );
		char *text, val[8] = "";

		if( !data )
			continue;
		text = Mem_Malloc( host.mempool, (size_t)len + 1 );
		memcpy( text, data, (size_t)len );
		text[len] = 0;
		Mem_Free( data );
		if( Xbox_SndCfg_Parse( text, key, val, sizeof( val )) && Q_atoi( val ) != 0 )
			asked = true;
		Mem_Free( text );
	}
	Mem_Free( t );
	return asked;
}

static void GpRoom_Delete( void )
{
	if( s_slot )
	{
		alAuxiliaryEffectSloti( s_slot, AL_EFFECTSLOT_EFFECT, 0 );
		if( s_src )
			alSource3i( s_src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL );
		alDeleteAuxiliaryEffectSlots( 1, &s_slot );
		s_slot = 0;
	}
	if( s_src )
	{
		alSourceStop( s_src );
		alSourcei( s_src, AL_BUFFER, 0 );
		alDeleteSources( 1, &s_src );
		s_src = 0;
	}
	if( s_bufs[0] )
	{
		alDeleteBuffers( XBOX_SND_AL_MAX_BUFFERS, s_bufs );
		memset( s_bufs, 0, sizeof( s_bufs ));
	}
	if( s_fx )
	{
		alDeleteEffects( 1, &s_fx );
		s_fx = 0;
	}
	if( s_filter )
	{
		alDeleteFilters( 1, &s_filter );
		s_filter = 0;
	}
	if( s_ring )
	{
		Mem_Free( s_ring );
		s_ring = NULL;
	}
	alGetError();
	s_nfree = 0;
}

/* Back to SX_RoomFX on the CPU: its delays are set up again from the cvars */
static void GpRoom_Off( const char *why )
{
	if( !s_on )
		return;
	Con_Printf( S_WARN "Audio: the room effect goes back to the CPU (%s)\n", why );
	s_on = false;
	sx_r3d_gp = false;
	SX_R3D_SetRate();
	if( s_slot )
		alAuxiliaryEffectSloti( s_slot, AL_EFFECTSLOT_EFFECT, 0 );
	if( s_src )
	{
		alSourceStop( s_src );
		alSourcei( s_src, AL_BUFFER, 0 );
	}
	alGetError();
}

/* nxdk-ex apu_gp.h (built in, engine/platform/xbox/openal/apu_gp.c) */
void apu_gp_debug_set_scratch_bytes( uint32_t bytes );

void Xbox_SndGpRoom_PreOpen( void )
{
	// #191: the GP's scratch sized for the one kernel this engine loads -
	// the room's delay lines (GP_HLDSP_K_SCRATCH_BYTES) past the 8 KiB
	// bootstrap image, and a page to spare - not the library's 512 KiB
	// default, sized for its own reverb and two echoes, which the engine
	// never loads: 368 KiB of contiguous memory back. Taken by the next
	// apu_gp_init, at alcOpenDevice.
	if( Xbox_SndCfg_Asked( "r3d_snd_gproom" ))
		apu_gp_debug_set_scratch_bytes( 0x2000u + GP_HLDSP_K_SCRATCH_BYTES + 0x1000u );
}

void Xbox_SndGpRoom_Init( int ring_frames, int speed )
{
	int i;

	if( s_on || !Xbox_SndCfg_Asked( "r3d_snd_gproom" ))
		return;
	alGetError();
	alGenSources( 1, &s_src );
	alGenBuffers( XBOX_SND_AL_MAX_BUFFERS, s_bufs );
	alGenFilters( 1, &s_filter );
	alGenEffects( 1, &s_fx );
	alGenAuxiliaryEffectSlots( 1, &s_slot );
	if( alGetError() != AL_NO_ERROR )
		goto fail;

	// the room mix as it was painted: at the origin, not panned again; no
	// direct path (it only reaches the mix through the kernel); the send at
	// 1.0, each channel on its own bin (nxdk-ex #47)
	alFilteri( s_filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS );
	alFilterf( s_filter, AL_LOWPASS_GAIN, 0.0f );
	alSourcei( s_src, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( s_src, AL_POSITION, 0.0f, 0.0f, 0.0f );
	alSourcei( s_src, AL_DIRECT_FILTER, (ALint)s_filter );
	alSourcei( s_src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE );
	alEffecti( s_fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM );
	alXboxEffectProgram( s_fx, (const ALuint *)gp_hldsp_image, GP_HLDSP_IMAGE_WORDS, 0 );
	alAuxiliaryEffectSloti( s_slot, AL_EFFECTSLOT_EFFECT, (ALint)s_fx ); // loads the kernel
	alSource3i( s_src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot, 0, AL_FILTER_NULL );
	if( alGetError() != AL_NO_ERROR )
		goto fail;

	s_ring = Mem_Calloc( sndpool, (size_t)ring_frames * 4 );
	s_ring_frames = ring_frames;
	s_speed = speed;
	for( i = 0; i < XBOX_SND_AL_MAX_BUFFERS; i++ )
		s_free[i] = s_bufs[i];
	s_nfree = XBOX_SND_AL_MAX_BUFFERS;
	s_sent_valid = s_pend_on = s_base_valid = false;
	s_on = true;
	sx_r3d_gp = true;
	Con_Printf( "Audio: the room effect runs on the APU's GP\n" );
	return;

fail:
	Con_Printf( S_ERROR "Audio: the room effect stays on the CPU (OpenAL setup failed)\n" );
	GpRoom_Delete();
}

void Xbox_SndGpRoom_Shutdown( void )
{
	if( s_on )
	{
		s_on = false;
		sx_r3d_gp = false;
	}
	GpRoom_Delete();
}

int Xbox_SndGpRoom_Active( void )
{
	return s_on;
}

unsigned Xbox_SndGpRoom_Source( void )
{
	return s_src;
}

unsigned Xbox_SndGpRoom_Slot( void )
{
	return s_on ? s_slot : 0;
}

int Xbox_SndGpRoom_StreamFrame( int time, unsigned *frame )
{
	if( !s_on || !s_base_valid )
		return 0;
	*frame = (unsigned)( time - s_base );
	return 1;
}

int Xbox_SndGpRoom_Upload( int pos, int frames, unsigned *buf )
{
	ALuint b;

	if( s_nfree <= 0 )
		return 0;
	b = s_free[--s_nfree];
	alBufferData( b, AL_FORMAT_STEREO16, s_ring + pos * 2, frames * 4, s_speed );
	if( alGetError() != AL_NO_ERROR )
	{
		s_free[s_nfree++] = b;
		return 0;
	}
	*buf = b;
	return 1;
}

void Xbox_SndGpRoom_Queue( unsigned buf )
{
	ALuint b = buf;

	alSourceQueueBuffers( s_src, 1, &b );
	if( alGetError() != AL_NO_ERROR )
	{
		s_free[s_nfree++] = b;
		GpRoom_Off( "a buffer could not be queued" );
	}
}

void Xbox_SndGpRoom_Unused( unsigned buf )
{
	s_free[s_nfree++] = buf;
}

void Xbox_SndGpRoom_Retire( void )
{
	ALint n = 0;

	if( !s_on )
		return;
	alGetSourcei( s_src, AL_BUFFERS_PROCESSED, &n );
	while( n-- > 0 )
	{
		ALuint b = 0;

		alSourceUnqueueBuffers( s_src, 1, &b );
		if( alGetError() != AL_NO_ERROR )
			break;
		s_free[s_nfree++] = b;
	}
}

/* The waiting change to the kernel: queued at its stream frame, or
   attached at once while no stream frame means anything */
static void GpRoom_Send( void )
{
	int i;

	if( !s_pend_on )
		return;
	for( i = 0; i < SXGP_PARAMS; i++ )
		alEffecti( s_fx, AL_XBOX_GP_PARAM0 + i, s_pend[i] );
	alGetError();
	if( s_base_valid )
	{
		const int frame = s_pend_time - s_base - GPROOM_LEAD;

		alXboxEffectSlotAtSample( s_slot, s_fx, s_src, frame > 0 ? (ALuint)frame : 0 );
	}
	else
		alAuxiliaryEffectSloti( s_slot, AL_EFFECTSLOT_EFFECT, (ALint)s_fx );
	if( alGetError() != AL_NO_ERROR )
	{
		s_n_retry++;
		return; // the library's queue is full: again at the next chunk
	}
	if( s_base_valid )
		s_n_timed++;
	else
		s_n_now++;
	memcpy( s_sent, s_pend, sizeof( s_sent ));
	s_sent_valid = true;
	s_pend_on = false;
}

void Xbox_SndGpRoom_Started( int time )
{
	if( !s_on )
		return;
	s_base = time;
	s_base_valid = true;
	s_n_starts++;
}

size_t Xbox_SndGpRoom_Stats( char *buf, size_t size )
{
	ALint peak = 0;
	int n;

	if( !s_on || size == 0 )
		return 0;
	alXboxGetHardwareStatus( AL_XBOX_GP_LOAD_PEAK, &peak );
	n = Q_snprintf( buf, size, "  room on the GP: load peak %d%%; changes %u timed, %u at once, %u retried; %u stream starts\n",
		peak, s_n_timed, s_n_now, s_n_retry, s_n_starts );
	s_n_timed = s_n_now = s_n_retry = s_n_starts = 0;
	return n > 0 ? Q_min( (size_t)n, size - 1 ) : 0;
}

void Xbox_SndGpRoom_Clear( void )
{
	int i;

	if( !s_on )
		return;
	alSourceStop( s_src );
	alSourcei( s_src, AL_BUFFER, 0 );
	alGetError();
	for( i = 0; i < XBOX_SND_AL_MAX_BUFFERS; i++ )
		s_free[i] = s_bufs[i];
	s_nfree = XBOX_SND_AL_MAX_BUFFERS;
	s_base_valid = false;
	GpRoom_Send();
}

void Xbox_SndGpRoom_Paint( const portable_samplepair_t *room, int start, int end, const int *params )
{
	const int mask = s_ring_frames - 1;
	const int *last;
	int t, i;

	if( !s_on )
		return;
	// the ring keeps 16 bits: the full range, the nearest to upstream's ints
	// (only its stages and the output clip them, to CLIP16's range); room
	// NULL: a chunk nothing was mixed into, silence (#208)
	if( !room )
	{
		for( t = start; t < end; )
		{
			const int at = t & mask;
			const int n = Q_min( end - t, s_ring_frames - at );

			memset( s_ring + at * 2, 0, n * 2 * sizeof( int16_t ));
			t += n;
		}
	}
	else for( t = start, i = 0; t < end; t++, i++ )
	{
		int16_t *out = s_ring + ( t & mask ) * 2;

		out[0] = (int16_t)bound( SHRT_MIN, room[i].left, SHRT_MAX );
		out[1] = (int16_t)bound( SHRT_MIN, room[i].right, SHRT_MAX );
	}

	last = s_pend_on ? s_pend : s_sent;
	if( !( s_pend_on || s_sent_valid ) || memcmp( params, last, sizeof( s_pend )))
	{
		memcpy( s_pend, params, sizeof( s_pend ));
		s_pend_time = start;
		s_pend_on = true;
	}
	GpRoom_Send();
}
