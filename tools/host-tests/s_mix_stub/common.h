/*
s_mix_stub/common.h - the engine surface engine/client/sound/s_mix.c compiles against, for its host test
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
tools/host-tests/s_mix_host_test.c includes the real s_mix.c; this
directory comes first on its include path and stands in for common.h,
sound.h and client.h (the other two are empty). Only the names s_mix.c
uses are here, with the engine's meaning but not its layout: the test
calls S_MixAudio, and the rest of the file only has to compile. The
constants S_MixAudio's callers read are copied from the engine headers
named next to them.
*/

#ifndef S_MIX_STUB_COMMON_H
#define S_MIX_STUB_COMMON_H

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>

typedef int qboolean;
typedef unsigned int uint;
#define false 0
#define true 1

/* public/xash3d_mathlib.h */
#define EQUAL_EPSILON 0.001f
#define Q_equal_e( a, b, e ) (((a) >= ((b) - (e))) && ((a) <= ((b) + (e))))
#define Q_equal( a, b ) Q_equal_e( a, b, EQUAL_EPSILON )
#define bound( min, num, max ) (( num ) >= ( min ) ? (( num ) < ( max ) ? ( num ) : ( max )) : ( min ))
/* common/xash3d_types.h */
#define SetBits( iBitVector, bits ) (( iBitVector ) = ( iBitVector ) | ( bits ))
#define FBitSet( iBitVector, bit ) (( iBitVector ) & ( bit ))

/* engine/client/sound.h */
#define PAINTBUFFER_SIZE 1024
#define S_RAW_SOUND_BACKGROUNDTRACK -2
#define CLIP16( x ) bound( SHRT_MIN + 8, x, SHRT_MAX - 8 )
#define MAX_CHANNEL_INAUDIBLE_TIME 0.5

/* sound flags and channels: values do not matter to the test */
#define SOUND_LOOPED 1
#define FL_CHAN_FINISHED 1
#define FL_CHAN_SENTENCE_FINISHED 2
#define FL_CHAN_LOCAL_SOUND 4
#define FL_CHAN_STATIC_SOUND 8
#define FL_CHAN_USE_LOOP 16
#define CHAN_VOICE 2
#define CHAN_STREAM 5
#define VOICE_LOOPBACK_INDEX -3
#define VOICE_LOCALCLIENT_INDEX -4
enum { key_console = 1, key_menu = 2 };

typedef struct { int left, right; } portable_samplepair_t;
typedef struct { int mouthopen; } mouth_t;
typedef struct { mouth_t mouth; } cl_entity_t;

typedef struct wavdata_s
{
	int rate, width, channels, flags, samples, loop_start;
	uint8_t *buffer;
} wavdata_t;

typedef struct sfx_s { wavdata_t *cache; } sfx_t;
typedef struct { int timecompress; sfx_t *sfx; } voxword_t;

typedef struct channel_s
{
	sfx_t *sfx;
	int flags, leftvol, rightvol, entchannel, entnum, basePitch, word_index;
	double sample, forced_end;
	float inauduble_free_time;
	voxword_t *words;
} channel_t;

typedef struct
{
	portable_samplepair_t *rawsamples;
	int entnum, leftvol, rightvol, max_samples;
	uint s_rawend;
} rawchan_t;

typedef struct
{
	channel_t *channels;
	rawchan_t **raw_channels;
	int total_channels, max_raw_channels, paintedtime, samples;
	void *buffer;
	struct { int speed; } format;
} snd_globals_t;

typedef struct { float value; } convar_t;

extern snd_globals_t snd;
typedef struct { int key_dest; } client_static_t;
extern client_static_t cls;
typedef struct { int background, paused; } client_t;
extern client_t cl;
typedef struct { double realtime; } host_parm_t;
extern host_parm_t host;
extern convar_t s_lerping, sys_timescale;

/* engine services the mixer calls; the test never reaches them */
wavdata_t *S_LoadSound( sfx_t *sfx );
void S_FreeChannel( channel_t *ch );
float S_GetMasterVolume( void );
int S_RetrieveAudioSamples( const wavdata_t *source, const void **output_buffer, int start_position, int num_samples, qboolean enable_looping );
cl_entity_t *CL_GetEntityByIndex( int index );
qboolean CL_IsInGame( void );
qboolean CL_IsPlayerIndex( int idx );
qboolean Host_IsSinglePlayerGame( void );
void SND_MoveMouth8( mouth_t *mouth, int pos, const wavdata_t *sc, int count, qboolean use_loop );
void SND_MoveMouth16( mouth_t *mouth, int pos, const wavdata_t *sc, int count, qboolean use_loop );
void SND_MoveMouthRaw( mouth_t *mouth, const portable_samplepair_t *buf, int count );
float VOX_ModifyPitch( channel_t *ch, float pitch );
void VOX_FreeWord( channel_t *ch );
void VOX_LoadWord( channel_t *ch );
void SX_RoomFX( portable_samplepair_t *paint, int num_samples );
void SNDDMA_BeginPainting( void );
void SNDDMA_Submit( void );

#endif /* S_MIX_STUB_COMMON_H */
