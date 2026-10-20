/*
voices_stub/common.h - what engine/platform/xbox/xbox_snd_voices.c and xbox_snd_music_voice.c need from the engine
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

/* tools/host-tests/xbox_snd_voices_host_test.c: the engine's types as
   common/sound_api.h and engine/common/common.h define them, cut to the
   fields the voice module reads. */
#ifndef VOICES_STUB_COMMON_H
#define VOICES_STUB_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef int qboolean;
typedef unsigned int uint;
#define false 0
#define true 1

#define bound( min, num, max ) (( num ) >= ( min ) ? (( num ) < ( max ) ? ( num ) : ( max )) : ( min ))
#define Q_min( a, b ) (( a ) < ( b ) ? ( a ) : ( b ))
#define Q_max( a, b ) (( a ) > ( b ) ? ( a ) : ( b ))
#define FBitSet( iBitVector, bit ) (( iBitVector ) & ( bit ))
#define SetBits( iBitVector, bits ) (( iBitVector ) = ( iBitVector ) | ( bits ))
#define ClearBits( iBitVector, bits ) (( iBitVector ) = ( iBitVector ) & ~( bits ))
#define BIT( n ) ( 1U << ( n ))
#define Q_snprintf snprintf
#define S_WARN "Warning: "
#define S_ERROR "Error: "

void Con_Printf( const char *fmt, ... );

/* engine/common/common.h */
enum { WF_UNKNOWN = 0, WF_PCMDATA, WF_MPGDATA };
#define SOUND_LOOPED BIT( 0 )

typedef struct wavdata_s
{
	size_t size;
	uint loop_start;
	uint samples;
	uint type;
	uint flags;
	uint16_t rate;
	uint8_t width;
	uint8_t channels;
	uint8_t buffer[];
} wavdata_t;

/* common/sound_api.h */
#define FL_CHAN_USE_LOOP          BIT( 0 )
#define FL_CHAN_STATIC_SOUND      BIT( 1 )
#define FL_CHAN_LOCAL_SOUND       BIT( 2 )
#define FL_CHAN_SENTENCE_FINISHED BIT( 4 )
#define FL_CHAN_FINISHED          BIT( 5 )

typedef struct { int left, right; } portable_samplepair_t;
typedef struct sfx_s { char name[64]; wavdata_t *cache; } sfx_t;
typedef struct { sfx_t *sfx; } voxword_t;
typedef struct channel_s
{
	sfx_t *sfx;
	uint flags;
	short leftvol, rightvol;
	double sample;
	voxword_t *words;
	uintptr_t engine_reserved[8];
} channel_t;

/* common/sound_api.h, engine/client/sound.h: the raw channel and the
   clocks, for xbox_snd_music_voice.c (#183) */
typedef struct rawchan_s
{
	short entnum;
	short master_vol;
	short leftvol, rightvol;
	float dist_mult;
	volatile uint s_rawend;
	size_t max_samples;
} rawchan_t;
typedef struct { int paintedtime, soundtime; } snd_globals_t;
extern snd_globals_t snd;
#define SOUND_DMA_SPEED 48000
#define S_RAW_SOUND_BACKGROUNDTRACK -2
rawchan_t *S_FindRawChannel( int entnum, qboolean create );

#endif
