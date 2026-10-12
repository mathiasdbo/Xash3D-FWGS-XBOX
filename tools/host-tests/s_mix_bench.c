/*
s_mix_bench.c - the cost per mixed sample of the Xbox mixer loops (divergence #174)
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
Not a test: run.sh does not run it. It links the same two s_mix.c builds as
s_mix_host_test.c (s_mix_host_test_tu.c, XASH_XBOX 0 and 1) and times each
loop on the calls a channel makes: PAINTBUFFER_SIZE outputs from an 11 or
22 kHz sound, 8 or 16 bit, mono or stereo, at the 48 kHz mix (#173) and at
upstream's 44.1 kHz (where #145's run path takes the integer ratios).
Only the ratios between rows mean anything; a host is not a Pentium III.

  gcc -O2 [-m32 -march=pentium3 -mfpmath=387] ... (see the build line in
  docs/r3d/divergences.md #174)
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "platform/xbox/xbox_frametime.h"

void mix_upstream( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_xbox( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_lerp_upstream( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_lerp_xbox( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
int mix_channel_upstream( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch );
int mix_channel_xbox( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch );

int s_mix_test_dma_speed = SOUND_44k;
snd_globals_t snd;
client_static_t cls;
client_t cl;
host_parm_t host;
convar_t s_lerping, sys_timescale;
xbox_ft_waits_t xbox_ft_waits;
xbox_ft_spans_t xbox_ft_spans;
int xbox_ft_snd_extra;
wavdata_t *S_LoadSound( sfx_t *sfx ) { return NULL; }
void S_FreeChannel( channel_t *ch ) { }
float S_GetMasterVolume( void ) { return 1.0f; }
int S_RetrieveAudioSamples( const wavdata_t *source, const void **output_buffer, int start_position, int num_samples, qboolean enable_looping ) { return 0; }
cl_entity_t *CL_GetEntityByIndex( int index ) { return NULL; }
qboolean CL_IsInGame( void ) { return true; }
qboolean CL_IsPlayerIndex( int idx ) { return false; }
qboolean Host_IsSinglePlayerGame( void ) { return true; }
void SND_MoveMouth8( mouth_t *mouth, int pos, const wavdata_t *sc, int count, qboolean use_loop ) { }
void SND_MoveMouth16( mouth_t *mouth, int pos, const wavdata_t *sc, int count, qboolean use_loop ) { }
void SND_MoveMouthRaw( mouth_t *mouth, const portable_samplepair_t *buf, int count ) { }
float VOX_ModifyPitch( channel_t *ch, float pitch ) { return pitch; }
void VOX_FreeWord( channel_t *ch ) { }
void VOX_LoadWord( channel_t *ch ) { }
void SX_RoomFX( portable_samplepair_t *paint, int num_samples ) { }
void SNDDMA_BeginPainting( void ) { }
void SNDDMA_Submit( void ) { }

typedef void ( *mix_fn )( portable_samplepair_t *, const int *, const void *, int, int, double, double, int );

static int16_t src[1 << 16];
static portable_samplepair_t out[PAINTBUFFER_SIZE + 1];

static double now( void )
{
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ns per output sample, best of five runs of `reps` calls */
static double bench( mix_fn fn, int mix_rate, int src_rate, int channels, int width, int reps )
{
	static const int vol[2] = { 200, 180 };
	const double rate = (double)src_rate / mix_rate;
	double best = 1e30;
	int run, r;

	s_mix_test_dma_speed = mix_rate;
	for( run = 0; run < 5; run++ )
	{
		const double t0 = now();

		for( r = 0; r < reps; r++ )
		{
			/* the fraction a channel carries between calls */
			const double frac = ( r & 7 ) * 0.125;

			fn( out, vol, src, channels, width, frac, rate, PAINTBUFFER_SIZE );
		}
		{
			const double t = ( now() - t0 ) * 1e9 / ((double)reps * PAINTBUFFER_SIZE );

			if( t < best )
				best = t;
		}
	}
	s_mix_test_dma_speed = SOUND_44k;
	return best;
}

int main( int argc, char **argv )
{
	static const struct { int src_rate, channels, width; } cases[] =
	{
		{ 11025, 1, 1 }, { 22050, 1, 1 }, { 11025, 1, 2 }, { 22050, 1, 2 }, { 22050, 2, 2 },
	};
	const int reps = argc > 1 ? atoi( argv[1] ) : 4000;
	unsigned c, i;

	for( i = 0; i < sizeof( src ) / sizeof( src[0] ); i++ )
		src[i] = (int16_t)( rand() ^ ( rand() << 8 ));

	printf( "ns per output sample (best of 5, %d calls of %d outputs); ratios only\n", reps, PAINTBUFFER_SIZE );
	printf( "source             | 44.1k point: upstream  xbox(runs) | 48k point: upstream  xbox | 48k lerp: upstream  xbox | xbox lerp / xbox point at 48k\n" );
	for( c = 0; c < sizeof( cases ) / sizeof( cases[0] ); c++ )
	{
		const int sr = cases[c].src_rate, ch = cases[c].channels, w = cases[c].width;
		const double u44 = bench( mix_upstream, SOUND_44k, sr, ch, w, reps );
		const double x44 = bench( mix_xbox, SOUND_44k, sr, ch, w, reps );
		const double u48 = bench( mix_upstream, SOUND_48k, sr, ch, w, reps );
		const double x48 = bench( mix_xbox, SOUND_48k, sr, ch, w, reps );
		const double ul48 = bench( mix_lerp_upstream, SOUND_48k, sr, ch, w, reps );
		const double xl48 = bench( mix_lerp_xbox, SOUND_48k, sr, ch, w, reps );

		printf( "%5d Hz %2d-bit %s |        %6.2f      %6.2f   |       %6.2f  %6.2f   |      %6.2f  %6.2f   | %.2fx\n",
			sr, w * 8, ch == 1 ? "mono  " : "stereo", u44, x44, u48, x48, ul48, xl48, xl48 / x48 );
	}
	return 0;
}
