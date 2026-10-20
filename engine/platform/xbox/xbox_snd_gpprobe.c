/*
xbox_snd_gpprobe.c - what a stereo send delivers to a GP kernel, on a console
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
Resonance3D (docs/r3d/divergences.md #180, the room effect on the APU's GP).
The room kernel (a port of s_dsp.c) can only be bit-exact if the room mix
reaches it unchanged: 16-bit stereo queued on a source, played by a VP voice
at pitch 1, sent at 1.0 into bins 2/3 (nxdk-ex #47: a stereo source's sends
into a kernel at 0 dB per channel), read by the kernel as 24-bit words.

With "r3d_snd_gpprobe 1" in a userconfig.d cfg, the first OpenAL open plays a
second of pseudo-random stereo through apu/gp_hlprobe.asm twice and captures
the final mix with the output callback (copy mode, for the probe only):
- mode 0, the kernel's input as the codec takes it (top 16 bits): every
  frame must equal what was queued;
- mode 1, the low 8 bits the codec drops, moved up: all zero.
The result goes to gpprobe.txt in the write directory. Boot then goes on as
usual, back on the zero-copy output.
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_al.h"
#include "platform/xbox/xbox_snd_gproom.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "platform/xbox/apu/gp_hlprobe.h"

#define PROBE_RATE   48000
#define PROBE_FRAMES PROBE_RATE              /* the pattern: one second */
#define PROBE_CAP    ( PROBE_RATE * 2 )      /* the capture: two */
#define PROBE_MATCH  16                      /* frames that locate the pattern */
#define PROBE_ANCHOR 12000                   /* ... taken from its middle: a voice may ramp in */

static int16_t *s_pat;     /* PROBE_FRAMES stereo frames */
static int16_t *s_cap;     /* PROBE_CAP stereo frames */
static volatile uint32_t s_cap_n;
static volatile int s_cap_on;

static void AL_APIENTRY GpProbe_Capture( ALshort *samples, ALsizei frames, ALvoid *user )
{
	uint32_t n = s_cap_n, k = (uint32_t)frames;

	(void)user;
	if( !s_cap_on )
		return;
	if( k > PROBE_CAP - n )
		k = PROBE_CAP - n;
	memcpy( s_cap + 2 * n, samples, 4 * k );
	s_cap_n = n + k;
	if( s_cap_n >= PROBE_CAP )
		s_cap_on = 0;
}

/* One pass: the kernel in `mode`, the pattern played once, the mix captured */
static void GpProbe_Pass( ALuint fx, ALuint slot, ALuint src, int mode )
{
	ALint state = AL_PLAYING;
	int waited = 0;

	alEffecti( fx, AL_XBOX_GP_PARAM0 + 0, mode );
	alAuxiliaryEffectSloti( slot, AL_EFFECTSLOT_EFFECT, (ALint)fx );
	Platform_Sleep( 50 ); // the kernel loaded and running before the sound starts

	s_cap_n = 0;
	s_cap_on = 1;
	alSourcePlay( src );
	while( state == AL_PLAYING && waited < 3000 )
	{
		Platform_Sleep( 10 );
		waited += 10;
		alXboxUpdateVoices();
		alGetSourcei( src, AL_SOURCE_STATE, &state );
	}
	Platform_Sleep( 100 ); // what the codec still had queued
	s_cap_on = 0;
	alSourceStop( src );
}

/* The capture as raw 16-bit stereo, for a look on the PC */
static void GpProbe_Dump( const char *name )
{
	file_t *f = FS_Open( name, "wb", true );

	if( !f )
		return;
	FS_Write( f, s_cap, 4 * s_cap_n );
	FS_Close( f );
}

static void GpProbe_Report( file_t *f )
{
	uint32_t n = s_cap_n, k, i, at = n, bad = 0, first = 0, last = 0, maxd = 0;

	// mode 0: find the pattern by frames from its middle, then compare every
	// frame of it the capture holds
	for( k = PROBE_ANCHOR; k + PROBE_MATCH <= n && at == n; k++ )
	{
		if( memcmp( s_cap + 2 * k, s_pat + 2 * PROBE_ANCHOR, 4 * PROBE_MATCH ) == 0 )
			at = k - PROBE_ANCHOR;
	}
	if( at == n )
	{
		FS_Printf( f, "mode 0: pattern NOT FOUND in %u captured frames (first frames %d %d %d %d)\n",
			n, s_cap[0], s_cap[1], s_cap[2], s_cap[3] );
		return;
	}
	for( i = 0; at + i < n && i < PROBE_FRAMES; i++ )
	{
		int dl = abs( s_cap[2 * ( at + i )] - s_pat[2 * i] );
		int dr = abs( s_cap[2 * ( at + i ) + 1] - s_pat[2 * i + 1] );

		if( dl || dr )
		{
			if( !bad )
				first = i;
			last = i;
			bad++;
			maxd = Q_max( maxd, (uint32_t)Q_max( dl, dr ));
		}
	}
	FS_Printf( f, "mode 0: pattern at capture frame %u, %u of %u frames compared, %u differ (max %u LSB)",
		at, i, PROBE_FRAMES, bad, maxd );
	if( bad )
		FS_Printf( f, ", the first at frame %u (got %d %d, sent %d %d), the last at %u", first, s_cap[2 * ( at + first )],
			s_cap[2 * ( at + first ) + 1], s_pat[2 * first], s_pat[2 * first + 1], last );
	FS_Printf( f, "\n" );
}

static void GpProbe_ReportLow( file_t *f )
{
	uint32_t n = s_cap_n, i, nz = 0, first = 0;

	for( i = 0; i < 2 * n; i++ )
	{
		if( s_cap[i] )
		{
			if( !nz )
				first = i;
			nz++;
		}
	}
	FS_Printf( f, "mode 1: %u captured frames, %u samples with low bits set", n, nz );
	if( nz )
		FS_Printf( f, ", first at sample %u: %d", first, s_cap[first] );
	FS_Printf( f, "\n" );
}

void Xbox_SndAL_GpProbe( void )
{
	ALuint fx = 0, slot = 0, src = 0, buf = 0;
	ALXBOXSTATS st0, st1;
	ALint peak = 0;
	uint32_t seed = 0x5180u, i;
	file_t *f;

	if( !Xbox_SndAL_Active() || !Xbox_SndCfg_Asked( "r3d_snd_gpprobe" ))
		return;
	f = FS_Open( "gpprobe.txt", "w", true );
	if( !f )
		return;
	FS_Printf( f, "GP transport probe (divergence #180): stereo 16-bit at 48 kHz, send 1.0, kernel gp_hlprobe\n" );

	s_pat = Mem_Malloc( host.mempool, 4 * PROBE_FRAMES );
	s_cap = Mem_Malloc( host.mempool, 4 * PROBE_CAP );
	for( i = 0; i < 2 * PROBE_FRAMES; i++ )
	{
		seed = seed * 1664525u + 1013904223u;
		s_pat[i] = (int16_t)( seed >> 16 );
	}

	alGetError();
	alGenBuffers( 1, &buf );
	alBufferData( buf, AL_FORMAT_STEREO16, s_pat, 4 * PROBE_FRAMES, PROBE_RATE );
	alGenSources( 1, &src );
	alSourcei( src, AL_BUFFER, (ALint)buf );
	alSourcei( src, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( src, AL_POSITION, 0.0f, 0.0f, 0.0f );
	alSourcei( src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE );
	alGenEffects( 1, &fx );
	alEffecti( fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM );
	alXboxEffectProgram( fx, (const ALuint *)gp_hlprobe_image, GP_HLPROBE_IMAGE_WORDS, 0 );
	alGenAuxiliaryEffectSlots( 1, &slot );
	alSource3i( src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL );
	if( alGetError() != AL_NO_ERROR )
	{
		FS_Printf( f, "setup failed (alGetError after the effect, slot and source)\n" );
		goto done;
	}

	memset( &st0, 0, sizeof( st0 ));
	st0.size = sizeof( st0 );
	alXboxGetStats( &st0 );
	alXboxOutputCallback( GpProbe_Capture, NULL ); // copy mode: the final mix comes through the callback

	GpProbe_Pass( fx, slot, src, 0 );
	GpProbe_Report( f );
	GpProbe_Dump( "gpprobe_m0.raw" );
	GpProbe_Pass( fx, slot, src, 1 );
	GpProbe_ReportLow( f );
	GpProbe_Dump( "gpprobe_m1.raw" );

	alXboxOutputCallback( NULL, NULL ); // back to zero-copy
	alXboxGetHardwareStatus( AL_XBOX_GP_LOAD_PEAK, &peak );
	memset( &st1, 0, sizeof( st1 ));
	st1.size = sizeof( st1 );
	alXboxGetStats( &st1 );
	FS_Printf( f, "GP load peak %d%%; during the probe %u underruns, %u padded buffers\n", peak,
		st1.ac97_underruns - st0.ac97_underruns, st1.ac97_padded - st0.ac97_padded );

done:
	if( slot )
	{
		alAuxiliaryEffectSloti( slot, AL_EFFECTSLOT_EFFECT, 0 );
		alSource3i( src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL );
		alDeleteAuxiliaryEffectSlots( 1, &slot );
	}
	if( src )
	{
		alSourceStop( src );
		alSourcei( src, AL_BUFFER, 0 );
		alDeleteSources( 1, &src );
	}
	if( fx )
		alDeleteEffects( 1, &fx );
	if( buf )
		alDeleteBuffers( 1, &buf );
	FS_Printf( f, "done (alGetError 0x%x after the teardown)\n", alGetError( ));
	FS_Close( f );
	Mem_Free( s_cap );
	Mem_Free( s_pat );
	s_cap = s_pat = NULL;
}
