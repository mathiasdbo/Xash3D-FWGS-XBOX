/*
s_mix_host_test.c - the Xbox's pitched mixer loops against upstream's (divergence #145)
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
Divergence #145 (1) replaces s_mix.c's four pitched (resampling) loops on
the Xbox and claims the paint buffer comes out bit for bit the same.
s_mix_host_test_tu.c is the real s_mix.c built twice - XASH_XBOX 0 gives
upstream's loops (mix_upstream), XASH_XBOX 1 ours (mix_xbox) - and this
drives both S_MixAudio builds with the same calls and compares the
buffers: the source rates and pitches the engine produces (11/22/44 kHz
at PITCH_NORM, which take the fixed-point loop, and random pitches and
odd rates, which take the float one), the fractions S_MixChannelToBuffer
passes, and every buffer length up to PAINTBUFFER_SIZE + 1.

  run.sh builds it twice: once with SSE2 doubles, once with x87 ones
  (-mfpmath=387), the Pentium III's arithmetic.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "common.h" /* s_mix_stub: portable_samplepair_t, the stubbed engine */
#include "platform/xbox/xbox_frametime.h"

void mix_upstream( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_xbox( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
int s_mix_test_fixed_step( double offset_frac, double rate_scale );

/* What both s_mix.c builds link against; S_MixAudio reaches none of it. */
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

static int failures, checks;

#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

/* Source samples, int16 and int8 views of the same bytes: a sound's data
   as S_RetrieveAudioSamples hands it to S_MixAudio. Large enough for
   PAINTBUFFER_SIZE + 1 stereo output samples at the highest rate below. */
static int16_t source[1 << 16];
static portable_samplepair_t pa[PAINTBUFFER_SIZE + 1], pb[PAINTBUFFER_SIZE + 1];

static uint32_t rng = 12345u;
static uint32_t rnd( void )
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

/* One call through both builds over the same starting buffer. */
static int same_mix( int channels, int width, double frac, double rate, int n, int lvol, int rvol )
{
	const int vol[2] = { lvol, rvol };
	uint32_t i;

	for( i = 0; i < PAINTBUFFER_SIZE + 1; i++ )
		pa[i].left = pa[i].right = (int)( rnd() & 0xFFFF ) - 0x8000;
	memcpy( pb, pa, sizeof( pa ));
	mix_upstream( pa, vol, source, channels, width, frac, rate, n );
	mix_xbox( pb, vol, source, channels, width, frac, rate, n );
	return memcmp( pa, pb, sizeof( pa )) == 0;
}

/* The fraction S_MixChannelToBuffer passes: chan->sample - floor(), with
   chan->sample a sum of out_count * rate steps. */
static double channel_frac( double rate )
{
	double sample = 0.0;
	int k, steps = (int)( rnd() % 6 );

	for( k = 0; k < steps; k++ )
		sample += (double)( 1 + rnd() % 1024 ) * rate;
	return sample - floor( sample );
}

int main( void )
{
	static const int src_rates[] = { 11025, 22050, 44100, 8000, 16000, 32000, 48000, 96000 };
	unsigned fixed = 0, flt = 0, engine_calls = 0, engine_fixed = 0, i;

	for( i = 0; i < sizeof( source ) / sizeof( source[0] ); i++ )
		source[i] = (int16_t)rnd();

	/* The 32.32 loop takes exactly the cases where upstream's double
	   arithmetic cannot round: 2^-32-grid values. */
	CHECK( s_mix_test_fixed_step( 0.0, 0.5 ) && s_mix_test_fixed_step( 0.75, 0.25 ));
	CHECK( s_mix_test_fixed_step( 0.5, 1.0 ) && s_mix_test_fixed_step( 0.0, 2.0 ));
	CHECK( !s_mix_test_fixed_step( 0.0, 0.95 * 22050 / 44100.0 ));   /* a pitched voice */
	CHECK( !s_mix_test_fixed_step( 0.1, 0.5 ));                      /* off the grid */
	CHECK( !s_mix_test_fixed_step( 0.0, 1048576.0 ) && !s_mix_test_fixed_step( 1.0, 0.5 ));
	CHECK( !s_mix_test_fixed_step( -0.25, 0.5 ) && !s_mix_test_fixed_step( 0.0, -0.5 ));

	/* Edges: one sample, the whole paint buffer, a fraction that carries
	   on the first step, rates at and around one source sample per output
	   sample (1.0 with a fraction is pitched; 1.0 without is not). */
	{
		static const double rates[] = { 0.25, 0.5, 1.0, 0.999, 1.0005, 2.0, 2.5, 0.475, 0.525 };
		static const double fracs[] = { 0.0, 0.25, 0.5, 0.75, 0.999999, 0.1, 0.5 + 1.0 / 4294967296.0 };
		static const int lens[] = { 1, 2, 3, 4, 735, PAINTBUFFER_SIZE, PAINTBUFFER_SIZE + 1 };
		unsigned r, f, l, k;

		for( r = 0; r < sizeof( rates ) / sizeof( rates[0] ); r++ )
			for( f = 0; f < sizeof( fracs ) / sizeof( fracs[0] ); f++ )
				for( l = 0; l < sizeof( lens ) / sizeof( lens[0] ); l++ )
					for( k = 0; k < 4; k++ )
						CHECK( same_mix( 1 + ( k >> 1 ), 1 + ( k & 1 ), fracs[f], rates[r], lens[l], 255, 128 ));
	}

	/* The run path: steps of exactly 1/2, 1/4, 1/8 and 1/16 from any
	   point of the 2^-32 grid, so the first run is anything from one
	   output to a whole run, and runs cut by the end of the buffer. */
	for( i = 0; i < 40000; i++ )
	{
		static const double steps[] = { 0.5, 0.25, 0.125, 0.0625 };
		const uint32_t f32 = ( i & 3 ) == 0 ? 0u : ( i & 3 ) == 1 ? 0x80000000u : ( rnd() << 8 ) ^ rnd();
		const double frac = f32 / 4294967296.0;
		const double rate = steps[rnd() % 4];
		const int n = 1 + (int)( rnd() % ( PAINTBUFFER_SIZE + 1 ));
		const int k = (int)( rnd() % 4 );

		CHECK( s_mix_test_fixed_step( frac, rate ));
		CHECK( same_mix( 1 + ( k >> 1 ), 1 + ( k & 1 ), frac, rate, n, (int)( rnd() % 256 ), (int)( rnd() % 256 )));
	}

	/* What the engine produces. Its pitch is a float - VOX_ModifyPitch
	   takes and returns one and sys_timescale's pitch_mult is one
	   (s_mix.c, S_MixNormalChannelsToRoombuffer) - so at 11, 22 and
	   44 kHz the rate and the channel's position stay on the 2^-32 grid:
	   PITCH_NORM, a pitched voice (95, 105), a weapon's random 94-109, any
	   pitch up to 255, at sys_timescale 1, 0.9 and 1.3. Every such call
	   takes the fixed-point loop. */
	for( i = 0; i < 100000; i++ )
	{
		static const int engine_rates[] = { 11025, 22050, 44100 };
		static const float timescales[] = { 1.0f, 0.9f, 1.3f };
		const int base = ( i % 4 ) == 0 ? 100 : ( i % 4 ) == 1 ? 95 + 10 * (int)( rnd() & 1 ) : ( i % 4 ) == 2 ? 94 + (int)( rnd() % 16 ) : 50 + (int)( rnd() % 206 );
		const float ts = timescales[rnd() % 3];
		const float pitch_mult = ( ts + 1 ) / 2;
		const float vox = (float)( base * 0.01 );
		volatile float pitch_f = vox * pitch_mult; /* rounded to float, as the Xbox's SSE1 does */
		const double pitch = pitch_f;
		const double rate = pitch * engine_rates[rnd() % 3] / 44100.0;
		const double frac = channel_frac( rate );
		const int n = 1 + (int)( rnd() % ( PAINTBUFFER_SIZE + 1 ));
		const int k = (int)( rnd() % 4 );

		if( Q_equal( rate, 1.0 ) && Q_equal( frac, 0.0 ))
			continue; /* the unpitched loops: not #145's */
		engine_calls++;
		if( s_mix_test_fixed_step( frac, rate ))
			engine_fixed++;
		CHECK( same_mix( 1 + ( k >> 1 ), 1 + ( k & 1 ), frac, rate, n, (int)( rnd() % 256 ), (int)( rnd() % 256 )));
	}
	CHECK( engine_calls > 90000 && engine_fixed == engine_calls );

	/* A stress set for the float loop: double pitches between 50% and
	   255% and eight source rates (8/16/32/48/96 kHz files are off the
	   grid at most pitches). */
	for( i = 0; i < 200000; i++ )
	{
		const double pitch = ( i & 1 ) ? 1.0 : 0.5 + ( rnd() % 2051 ) / 1000.0;
		const double rate = pitch * src_rates[rnd() % 8] / 44100.0;
		const double frac = channel_frac( rate );
		const int n = 1 + (int)( rnd() % ( PAINTBUFFER_SIZE + 1 ));
		const int k = (int)( rnd() % 4 );

		if( Q_equal( rate, 1.0 ) && Q_equal( frac, 0.0 ))
			continue;
		if( s_mix_test_fixed_step( frac, rate ))
			fixed++;
		else flt++;
		CHECK( same_mix( 1 + ( k >> 1 ), 1 + ( k & 1 ), frac, rate, n, (int)( rnd() % 256 ), (int)( rnd() % 256 )));
	}
	/* Both loops were exercised, each many times. */
	CHECK( fixed > 10000 && flt > 10000 );

	/* Rates no game produces (an extreme sys_timescale): the float loop
	   takes upstream's truncation from a sum of 2, so a huge rate costs
	   one step a sample, as upstream does, not a long loop. One output
	   sample, so no source sample past the first is read. */
	{
		static const double huge[] = { 3.75, 1048576.0, 1048576.0 + 1.0 / 3.0, 2147483648.0 };
		unsigned h, k;

		for( h = 0; h < sizeof( huge ) / sizeof( huge[0] ); h++ )
			for( k = 0; k < 4; k++ )
				CHECK( same_mix( 1 + ( k >> 1 ), 1 + ( k & 1 ), 0.1, huge[h], 1, 200, 100 ));
	}

	printf( "s_mix: %d checks, %d failures (engine-like calls %u, fixed-point %u; stress set fixed-point %u, float %u)\n", checks, failures, engine_calls, engine_fixed, fixed, flt );
	return failures != 0;
}
