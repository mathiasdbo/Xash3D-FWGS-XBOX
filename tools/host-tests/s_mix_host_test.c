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

Divergence #173 mixes at 48 kHz on the Xbox, where no engine rate is on
the 2^-32 grid (11025 / 48000 = 147 / 640) and #145's fixed-point loop
takes the step and the position rounded down to it. The 48 kHz part runs
both builds at SOUND_DMA_SPEED 48000 and checks:
- every engine-like call takes the fixed-point loop;
- every output's source index (a ramp source whose sample is its index)
  against the exact floor( frac + i * rate ) of the double values passed,
  computed in 128-bit integers: the Xbox loop never reads past it, is at
  most one behind, and only where the exact position is within the
  call's accumulated rounding of an integer; how often it and upstream's
  double loop differ from the exact index is printed;
- whole channels through S_MixChannelToBuffer at 44.1 and 48 kHz: an
  11025 Hz sound of N samples lasts N / 11025 s at both (the output count
  is N * rate / 11025 to within two samples), a 1 kHz tone in it comes out
  at 1 kHz (and 1.5 kHz at pitch 150) by its zero crossings, and nothing
  past the sound's last sample is ever mixed (a full-scale sentinel
  follows it).
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "common.h" /* s_mix_stub: portable_samplepair_t, the stubbed engine */
#include "platform/xbox/xbox_frametime.h"

void mix_upstream( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_xbox( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_lerp_upstream( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
void mix_lerp_xbox( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples );
int mix_channel_upstream( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch );
int mix_channel_xbox( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch );
int s_mix_test_fixed_step( double offset_frac, double rate_scale );

/* the mix rate both builds read (s_mix_stub/common.h): upstream's unless a
   test sets 48 kHz */
int s_mix_test_dma_speed = SOUND_44k;

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
/* engine/client/sound/s_main.c's, without looping (the channel tests'
   sounds do not loop) */
int S_RetrieveAudioSamples( const wavdata_t *source, const void **output_buffer, int start_position, int num_samples, qboolean enable_looping )
{
	int available = source->samples - start_position;

	if( available < 0 )
		available = 0;
	if( num_samples > available )
		num_samples = available;
	if( num_samples <= 0 )
		return 0;
	*output_buffer = source->buffer + start_position * source->width * source->channels;
	return num_samples;
}
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

/* the first 50 failures are printed: a mutant (mix48_mutants.py) can fail
   millions of checks, and printing them all takes minutes */
#define CHECK( cond ) do { checks++; if( !( cond )) { if( failures++ < 50 ) printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

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

/* ---- the 48 kHz mix (#173) ---- */

/* floor( f + i * r ) for the doubles f and r taken as exact binary
   fractions, in 128-bit integers at a scale of 2^-K, K the finer of
   their last bits' (53 significant bits each). 0 when that needs K above
   112 (an f below about 2^-60: skipped); i * r stays under 2^125. */
typedef unsigned __int128 u128;
static int exact_scale( double v )
{
	int e;

	if( v == 0.0 )
		return 0;
	frexp( v, &e ); /* v = m * 2^e, 0.5 <= m < 1 */
	return 53 - e;  /* v's last bit is 2^-( 53 - e ) */
}
static int exact_indices( double f, double r, int n, uint32_t *idx, uint32_t *frac_hi )
{
	int K = exact_scale( f ) > exact_scale( r ) ? exact_scale( f ) : exact_scale( r );
	u128 F, R;
	int i;

	if( K < 32 )
		K = 32;
	if( K > 112 )
		return 0;
	F = (u128)ldexp( f, K ); /* integers below 2^113: exact */
	R = (u128)ldexp( r, K );
	for( i = 0; i < n; i++ )
	{
		const u128 sum = F + (u128)i * R;

		idx[i] = (uint32_t)( sum >> K );
		/* the fraction in units of 2^-32: how far past the integer it is */
		frac_hi[i] = (uint32_t)(( sum & ((( (u128)1 ) << K ) - 1 )) >> ( K - 32 ));
	}
	return 1;
}

static int16_t ramp[4096];
static portable_samplepair_t ra[PAINTBUFFER_SIZE + 2], rb[PAINTBUFFER_SIZE + 2]; /* one past the longest call, checked untouched */
static uint32_t ex_idx[PAINTBUFFER_SIZE + 1], ex_frac[PAINTBUFFER_SIZE + 1];

static void test_48k_indices( void )
{
	static const int vol[2] = { 256, 256 }; /* ( s * 256 ) >> 8 == s: the output is the index */
	static const int engine_rates[] = { 11025, 22050, 44100 };
	static const float timescales[] = { 1.0f, 0.9f, 1.3f };
	unsigned calls = 0, outs = 0, xbox_diff = 0, up_diff = 0, skipped = 0, fixed = 0, i;
	int j;

	for( j = 0; j < 4096; j++ )
		ramp[j] = (int16_t)j;
	s_mix_test_dma_speed = SOUND_48k;

	for( i = 0; i < 60000; i++ )
	{
		const int base = ( i % 4 ) == 0 ? 100 : ( i % 4 ) == 1 ? 95 + 10 * (int)( rnd() & 1 ) : ( i % 4 ) == 2 ? 94 + (int)( rnd() % 16 ) : 50 + (int)( rnd() % 206 );
		const float ts = timescales[rnd() % 3];
		const float pitch_mult = ( ts + 1 ) / 2;
		const float vox = (float)( base * 0.01 );
		volatile float pitch_f = vox * pitch_mult;
		const double pitch = pitch_f;
		/* S_MixChannelToBuffer's rate at 48 kHz */
		const double rate = pitch * engine_rates[rnd() % 3] / (double)SOUND_48k;
		const double frac = ( i & 7 ) == 0 ? 0.0 : channel_frac( rate );
		const int n = 1 + (int)( rnd() % ( PAINTBUFFER_SIZE + 1 ));
		int k;

		if( Q_equal( rate, 1.0 ) && Q_equal( frac, 0.0 ))
			continue;
		if( !exact_indices( frac, rate, n, ex_idx, ex_frac ))
		{
			skipped++;
			continue;
		}
		calls++;
		if( s_mix_test_fixed_step( frac, rate ))
			fixed++;
		memset( ra, 0, sizeof( ra ));
		memset( rb, 0, sizeof( rb ));
		mix_upstream( ra, vol, ramp, 1, 2, frac, rate, n );
		mix_xbox( rb, vol, ramp, 1, 2, frac, rate, n );
		for( k = 0; k < n; k++ )
		{
			const uint32_t x = (uint32_t)rb[k].left, u = (uint32_t)ra[k].left;

			outs++;
			if( u != ex_idx[k] )
				up_diff++;
			if( x != ex_idx[k] )
			{
				xbox_diff++;
				/* one behind, at an exact position within the call's
				   rounding (k + 1 steps of 2^-32, plus the start's) of
				   the integer below it */
				CHECK( x + 1 == ex_idx[k] && ex_frac[k] <= (uint32_t)k + 2 );
			}
			CHECK( x <= ex_idx[k] ); /* never past the exact index */
		}
		CHECK( rb[n].left == 0 ); /* nothing past the call's outputs */
	}
	CHECK( calls > 50000 && fixed == calls && skipped < calls / 100 );
	/* measured: 668 of 30.8 million with SSE2 doubles, 53 with x87 ones
	   (the calls differ: channel_frac's sums round differently), every one
	   a position whose exact value is on or just past an integer */
	CHECK( xbox_diff * 10000.0 < outs ); /* rarer than one in 10,000 */
	printf( "s_mix 48 kHz: %u engine-like calls (%u skipped), %u outputs; index differs from the exact one: xbox %u, upstream %u\n",
		calls, skipped, outs, xbox_diff, up_diff );
	s_mix_test_dma_speed = SOUND_44k;
}

/* One 11025 Hz mono 16-bit sound of `n` samples (a 1 kHz tone, then a
   full-scale sentinel) through a whole channel at `out_rate`. Returns the
   outputs mixed before the channel finished; *crossings counts the
   tone's rising zero crossings and *peak the largest |output|. */
static int16_t tone[11025 * 3 + 4096];
static portable_samplepair_t chan_out[PAINTBUFFER_SIZE + 1];

static int run_channel( int xbox, int out_rate, double pitch, int n, int *crossings, int *peak )
{
	wavdata_t wav;
	sfx_t sfx;
	channel_t ch;
	int total = 0, guard, prev = 0, k;

	memset( &wav, 0, sizeof( wav ));
	wav.rate = 11025;
	wav.width = 2;
	wav.channels = 1;
	wav.samples = n;
	wav.buffer = (uint8_t *)tone;
	sfx.cache = &wav;
	memset( &ch, 0, sizeof( ch ));
	ch.sfx = &sfx;
	ch.leftvol = ch.rightvol = 255;
	*crossings = 0;
	*peak = 0;
	s_mix_test_dma_speed = out_rate;
	for( guard = 0; guard < 100000 && !FBitSet( ch.flags, FL_CHAN_FINISHED ); guard++ )
	{
		int got;

		memset( chan_out, 0, sizeof( chan_out ));
		got = xbox ? mix_channel_xbox( chan_out, &ch, PAINTBUFFER_SIZE, out_rate, pitch )
			: mix_channel_upstream( chan_out, &ch, PAINTBUFFER_SIZE, out_rate, pitch );
		for( k = 0; k < got; k++ )
		{
			const int v = chan_out[k].left;

			if( prev < 0 && v >= 0 )
				( *crossings )++;
			prev = v;
			if( abs( v ) > *peak )
				*peak = abs( v );
		}
		total += got;
	}
	s_mix_test_dma_speed = SOUND_44k;
	return total;
}

/* ---- s_lerping's interpolation on the fixed-point counter (#174) ---- */

/* The Xbox loop against upstream's on one call over the same buffer. The
   interpolated value may differ from upstream's truncated double: 16-bit,
   by the 15-bit weight's error times the step between the two samples
   (at most 65535 / 32768, under 2) plus one for the rounding; 8-bit, by
   one (upstream drops the fraction, the Xbox keeps 8 bits of it). An
   output differs by that times the volume, plus the shift's rounding.
   Returns the largest difference seen. */
static int lerp_close( int channels, int width, double frac, double rate, int n, int lvol, int rvol )
{
	const int vol[2] = { lvol, rvol };
	const int bound_l = ( width == 1 ? lvol : (( 3 * lvol ) >> 8 )) + 2;
	const int bound_r = ( width == 1 ? rvol : (( 3 * rvol ) >> 8 )) + 2;
	int i, worst = 0;

	memset( pa, 0, sizeof( pa ));
	memset( pb, 0, sizeof( pb ));
	mix_lerp_upstream( pa, vol, source, channels, width, frac, rate, n );
	mix_lerp_xbox( pb, vol, source, channels, width, frac, rate, n );
	for( i = 0; i < n; i++ )
	{
		const int dl = abs( pa[i].left - pb[i].left ), dr = abs( pa[i].right - pb[i].right );

		CHECK( dl <= bound_l && dr <= bound_r );
		if(( dl > bound_l || dr > bound_r ) && getenv( "S_MIX_DEBUG" ))
			printf( "  ch %d w %d frac %.17g rate %.17g n %d i %d vol %d/%d: up %d/%d xbox %d/%d\n", channels, width, frac, rate, n, i, lvol, rvol,
				pa[i].left, pa[i].right, pb[i].left, pb[i].right );
		if( dl > worst ) worst = dl;
		if( dr > worst ) worst = dr;
	}
	if( n < PAINTBUFFER_SIZE + 1 )
		CHECK( pb[n].left == 0 && pb[n].right == 0 ); /* nothing past the call's outputs */
	return worst;
}

static void test_lerp_calls( void )
{
	static const int engine_rates[] = { 11025, 22050, 44100 };
	static const float timescales[] = { 1.0f, 0.9f, 1.3f };
	static const int mix_rates[] = { SOUND_44k, SOUND_48k };
	unsigned calls = 0, i;
	int worst16 = 0, worst8 = 0, m;

	for( m = 0; m < 2; m++ )
	{
		s_mix_test_dma_speed = mix_rates[m];
		for( i = 0; i < 40000; i++ )
		{
			const int base = ( i % 4 ) == 0 ? 100 : ( i % 4 ) == 1 ? 95 + 10 * (int)( rnd() & 1 ) : 50 + (int)( rnd() % 206 );
			const float ts = timescales[rnd() % 3];
			const float pitch_mult = ( ts + 1 ) / 2;
			volatile float pitch_f = (float)( base * 0.01 ) * pitch_mult;
			const double rate = (double)pitch_f * engine_rates[rnd() % 3] / (double)mix_rates[m];
			const double frac = channel_frac( rate );
			const int n = 1 + (int)( rnd() % ( PAINTBUFFER_SIZE + 1 ));
			const int k = (int)( rnd() % 4 );
			int w;

			if( Q_equal( rate, 1.0 ) && Q_equal( frac, 0.0 ))
				continue;
			calls++;
			w = lerp_close( 1 + ( k >> 1 ), 1 + ( k & 1 ), frac, rate, n, (int)( rnd() % 256 ), (int)( rnd() % 256 ));
			if(( k & 1 ) && w > worst16 ) worst16 = w;
			if( !( k & 1 ) && w > worst8 ) worst8 = w;
		}
	}
	s_mix_test_dma_speed = SOUND_44k;
	/* the off-grid double fallback, and a carry on the first step */
	lerp_close( 1, 2, 0.1, 0.95 * 22050 / 44100.0, PAINTBUFFER_SIZE, 255, 200 );
	lerp_close( 2, 1, 0.999999, 0.5, PAINTBUFFER_SIZE, 255, 200 );
	CHECK( calls > 70000 );
	printf( "s_mix lerp: %u engine-like calls at 44.1 and 48 kHz; largest output difference from upstream's lerp: 16-bit %d, 8-bit %d (volume 255 = one 8-bit step)\n",
		calls, worst16, worst8 );
}

/* A whole channel with s_lerping on: the lookahead sample is inside the
   sound (a full-scale sentinel follows it and must never reach the
   output), and the sound lasts as long and keeps its pitch. */
static void test_lerp_channels( void )
{
	static const int rates[] = { SOUND_44k, SOUND_48k };
	static const double pitches[] = { 1.0, 1.5, 0.95 };
	const int n = 11025 * 3;
	int k, r, pi, b;

	for( k = 0; k < n; k++ )
		tone[k] = (int16_t)lrint( 10000.0 * sin( 2.0 * 3.14159265358979323846 * 1000.0 * ( k + 0.5 ) / 11025.0 ));
	for( ; k < (int)( sizeof( tone ) / sizeof( tone[0] )); k++ )
		tone[k] = 32767;

	s_lerping.value = 1.0f;
	for( r = 0; r < 2; r++ )
		for( pi = 0; pi < 3; pi++ )
			for( b = 0; b < 2; b++ )
			{
				int crossings, peak;
				const int total = run_channel( b, rates[r], pitches[pi], n, &crossings, &peak );
				const double expect = (double)n * rates[r] / ( 11025.0 * pitches[pi] );
				const double hz = crossings / ((double)total / rates[r] );

				/* the lerp stops one source sample short of the point
				   loop (S_MixChannelToBuffer's lookahead cap) */
				CHECK( fabs( total - expect ) <= 2.0 + rates[r] / ( 11025.0 * pitches[pi] ));
				CHECK( fabs( hz - 1000.0 * pitches[pi] ) < 1000.0 * pitches[pi] * 0.002 );
				CHECK( peak <= 9961 );
			}
	s_lerping.value = 0.0f;
}

/* The aliases a point-sampled 11 or 22 kHz sound gets at the mix rate, and
   what the interpolation leaves of them: a sine in a sound at `src_rate`
   through a whole channel, a Blackman-Harris window, a power spectrum.
   Images are the source rate's own (k * src_rate +- f, which a 44.1 kHz
   mix of an 11/22 kHz sound already has, at integer ratios); everything
   else that stands out is inharmonic. */
#define SPEC_N 32768
static double spec_re[SPEC_N], spec_im[SPEC_N], spec_pow[SPEC_N / 2];
static int8_t tone8[1 << 16];
static int16_t tone16[1 << 16];
static portable_samplepair_t spec_out[SPEC_N + 2048];

static void fft( double *re, double *im, int n )
{
	int i, j, len;

	for( i = 1, j = 0; i < n; i++ )
	{
		int bit = n >> 1;

		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
		{
			double t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for( len = 2; len <= n; len <<= 1 )
	{
		const double ang = -2.0 * 3.14159265358979323846 / len;

		for( i = 0; i < n; i += len )
			for( j = 0; j < len / 2; j++ )
			{
				const double wr = cos( ang * j ), wi = sin( ang * j );
				const double ur = re[i + j], ui = im[i + j];
				const double vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
				const double vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;

				re[i + j] = ur + vr; im[i + j] = ui + vi;
				re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
			}
	}
}

typedef struct { double image_db, inharm_db, inharm_hz, noise_db; } spurs_t;

static spurs_t measure_spurs( int xbox, int out_rate, int lerp, int src_rate, int width, double hz )
{
	const int skip = 1024, len = (int)( sizeof( tone16 ) / sizeof( tone16[0] )) - 4;
	wavdata_t wav;
	sfx_t sfx;
	channel_t ch;
	spurs_t sp;
	int got = 0, i, k, tone_bin = (int)( hz * SPEC_N / out_rate + 0.5 );
	double tone_p = 0.0, total = 0.0;

	for( i = 0; i < len + 4; i++ )
	{
		const double v = sin( 2.0 * 3.14159265358979323846 * hz * i / src_rate );

		tone16[i] = (int16_t)lrint( 16000.0 * v );
		tone8[i] = (int8_t)lrint( 100.0 * v );
	}
	memset( &wav, 0, sizeof( wav ));
	wav.rate = src_rate;
	wav.width = width;
	wav.channels = 1;
	wav.samples = len;
	wav.buffer = width == 1 ? (uint8_t *)tone8 : (uint8_t *)tone16;
	sfx.cache = &wav;
	memset( &ch, 0, sizeof( ch ));
	ch.sfx = &sfx;
	ch.leftvol = ch.rightvol = 255;
	s_mix_test_dma_speed = out_rate;
	s_lerping.value = lerp ? 1.0f : 0.0f;
	memset( spec_out, 0, sizeof( spec_out ));
	while( got < SPEC_N + skip && !FBitSet( ch.flags, FL_CHAN_FINISHED ))
	{
		const int want = SPEC_N + skip - got < PAINTBUFFER_SIZE ? SPEC_N + skip - got : PAINTBUFFER_SIZE;

		got += xbox ? mix_channel_xbox( spec_out + got, &ch, want, out_rate, 1.0 )
			: mix_channel_upstream( spec_out + got, &ch, want, out_rate, 1.0 );
	}
	s_lerping.value = 0.0f;
	s_mix_test_dma_speed = SOUND_44k;
	CHECK( got == SPEC_N + skip );

	for( i = 0; i < SPEC_N; i++ )
	{
		const double x = 2.0 * 3.14159265358979323846 * i / ( SPEC_N - 1 );
		const double w = 0.35875 - 0.48829 * cos( x ) + 0.14128 * cos( 2 * x ) - 0.01168 * cos( 3 * x );

		spec_re[i] = spec_out[skip + i].left * w;
		spec_im[i] = 0.0;
	}
	fft( spec_re, spec_im, SPEC_N );
	for( i = 0; i < SPEC_N / 2; i++ )
		spec_pow[i] = spec_re[i] * spec_re[i] + spec_im[i] * spec_im[i];
	for( i = tone_bin - 6; i <= tone_bin + 6; i++ )
		if( spec_pow[i] > tone_p ) tone_p = spec_pow[i];

	sp.image_db = sp.inharm_db = sp.noise_db = -200.0;
	sp.inharm_hz = 0.0;
	for( i = (int)( 50.0 * SPEC_N / out_rate ); i < SPEC_N / 2 && i * (double)out_rate / SPEC_N <= 20000.0; i++ )
	{
		const double f = i * (double)out_rate / SPEC_N;
		const double db = 10.0 * log10( spec_pow[i] / tone_p + 1e-30 );
		int image = 0;

		if( abs( i - tone_bin ) <= 8 )
			continue;
		total += spec_pow[i];
		for( k = 1; k <= 8 && !image; k++ )
			image = fabs( f - ( k * src_rate - hz )) * SPEC_N / out_rate <= 8.0 || fabs( f - ( k * src_rate + hz )) * SPEC_N / out_rate <= 8.0;
		if( image && db > sp.image_db )
			sp.image_db = db;
		if( !image && db > sp.inharm_db )
		{
			sp.inharm_db = db;
			sp.inharm_hz = f;
		}
	}
	/* everything but the tone, against the tone (the window's noise
	   bandwidth is the same for both, so it cancels) */
	{
		double tone_sum = 0.0;

		for( i = tone_bin - 6; i <= tone_bin + 6; i++ )
			tone_sum += spec_pow[i];
		sp.noise_db = 10.0 * log10( total / tone_sum + 1e-30 );
	}
	return sp;
}

static void test_lerp_spectra( void )
{
	static const struct { int src_rate, width; double hz; } cases[] =
	{
		{ 11025, 2, 500.0 }, { 11025, 2, 1000.0 }, { 11025, 2, 2000.0 }, { 11025, 2, 4000.0 },
		{ 22050, 2, 1000.0 }, { 22050, 2, 4000.0 }, { 22050, 2, 8000.0 },
		{ 11025, 1, 1000.0 }, { 22050, 1, 4000.0 },
	};
	unsigned c;

	printf( "s_mix spectra (dB re the tone; image = k*src+-f, inharmonic = anything else; all = every non-tone bin to 20 kHz summed)\n" );
	printf( "  source        tone    | 44.1k point (upstream) | 48k point (xbox)              | 48k lerp (xbox)               | 48k lerp (upstream)\n" );
	for( c = 0; c < sizeof( cases ) / sizeof( cases[0] ); c++ )
	{
		const spurs_t u44 = measure_spurs( 0, SOUND_44k, 0, cases[c].src_rate, cases[c].width, cases[c].hz );
		const spurs_t p48 = measure_spurs( 1, SOUND_48k, 0, cases[c].src_rate, cases[c].width, cases[c].hz );
		const spurs_t l48 = measure_spurs( 1, SOUND_48k, 1, cases[c].src_rate, cases[c].width, cases[c].hz );
		const spurs_t lu48 = measure_spurs( 0, SOUND_48k, 1, cases[c].src_rate, cases[c].width, cases[c].hz );

		printf( "  %5d Hz %2d-bit %5.0f Hz | img %6.1f inh %6.1f   | img %6.1f inh %6.1f all %6.1f | img %6.1f inh %6.1f all %6.1f | img %6.1f inh %6.1f\n",
			cases[c].src_rate, cases[c].width * 8, cases[c].hz, u44.image_db, u44.inharm_db,
			p48.image_db, p48.inharm_db, p48.noise_db, l48.image_db, l48.inharm_db, l48.noise_db, lu48.image_db, lu48.inharm_db );

		/* measured (16-bit, the Xbox loop at 48 kHz): the interpolation
		   takes the inharmonic aliases down 17-36 dB in an 11 kHz sound
		   (4 kHz to 500 Hz) and 11.5-33 dB in a 22 kHz one (8 to
		   1 kHz), the source's own images 25-27 dB at
		   500 Hz-1 kHz and 5 dB at 4 kHz in an 11 kHz sound (linear
		   interpolation's sinc^2 response is weak near the source's
		   Nyquist). Checked with margin: 10 dB and 4 dB. The Xbox loop
		   does what upstream's does to within 1.5 dB. */
		if( cases[c].width == 2 )
		{
			const double worst_p = p48.image_db > p48.inharm_db ? p48.image_db : p48.inharm_db;
			const double worst_l = l48.image_db > l48.inharm_db ? l48.image_db : l48.inharm_db;

			CHECK( l48.inharm_db < p48.inharm_db - 10.0 );
			CHECK( worst_l < worst_p - 4.0 );
			CHECK( fabs( l48.noise_db - lu48.noise_db ) < 1.5 );
			CHECK( fabs( l48.inharm_db - lu48.inharm_db ) < 1.5 );
		}
		CHECK( l48.noise_db < p48.noise_db - 4.0 );
		CHECK( u44.inharm_db < -55.0 ); /* nothing inharmonic at upstream's integer ratios */
	}
}

static void test_48k_channels( void )
{
	static const int rates[] = { SOUND_44k, SOUND_48k };
	static const double pitches[] = { 1.0, 1.5, 0.95 };
	const int n = 11025 * 3;
	int k, r, pi, b;

	for( k = 0; k < n; k++ )
		tone[k] = (int16_t)lrint( 10000.0 * sin( 2.0 * 3.14159265358979323846 * 1000.0 * ( k + 0.5 ) / 11025.0 ));
	for( ; k < (int)( sizeof( tone ) / sizeof( tone[0] )); k++ )
		tone[k] = 32767; /* past the sound: must never be mixed */

	for( r = 0; r < 2; r++ )
		for( pi = 0; pi < 3; pi++ )
			for( b = 0; b < 2; b++ )
			{
				int crossings, peak;
				const int total = run_channel( b, rates[r], pitches[pi], n, &crossings, &peak );
				const double expect = (double)n * rates[r] / ( 11025.0 * pitches[pi] );
				const double seconds = (double)total / rates[r];
				const double hz = crossings / seconds;

				CHECK( fabs( total - expect ) <= 2.0 );
				CHECK( fabs( hz - 1000.0 * pitches[pi] ) < 1000.0 * pitches[pi] * 0.002 );
				CHECK( peak <= 9961 ); /* -10000 * 255 >> 8: the sentinel (32639) never mixed */
				if( r == 1 && b == 1 )
					printf( "s_mix 48 kHz channel, pitch %.2f: %d outputs for %d samples (%.6f s, expected %.6f), tone %.2f Hz\n",
						pitches[pi], total, n, seconds, (double)n / ( 11025.0 * pitches[pi] ), hz );
			}
}

int main( void )
{
	static const int src_rates[] = { 11025, 22050, 44100, 8000, 16000, 32000, 48000, 96000 };
	unsigned fixed = 0, flt = 0, engine_calls = 0, engine_fixed = 0, i;

	setvbuf( stdout, NULL, _IONBF, 0 ); /* a trap (an undefined-behaviour check) loses nothing printed before it */

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

	/* the 48 kHz mix (#173); then the 44.1 kHz calls above again, to show
	   the rate's switch left them bit-exact */
	test_48k_indices();
	test_48k_channels();
	/* s_lerping on the fixed-point counter (#174) */
	test_lerp_calls();
	test_lerp_channels();
	test_lerp_spectra();
	CHECK( s_mix_test_dma_speed == SOUND_44k );
	CHECK( !s_mix_test_fixed_step( 0.0, 0.95 * 22050 / 44100.0 ));

	printf( "s_mix: %d checks, %d failures (engine-like calls %u, fixed-point %u; stress set fixed-point %u, float %u)\n", checks, failures, engine_calls, engine_fixed, fixed, flt );
	return failures != 0;
}
