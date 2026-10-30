/*
xbox_snd_adpcm_host_test.c - divergence #200: the load-time ADPCM encoder
and the windowed decoder of engine/platform/xbox/xbox_snd_adpcm.c

- every block the encoder writes is one the APU plays: index 0..88, the
  fourth byte zero, its header sample the source's frame exactly;
- decoding through xemu's reconstruction (Xbox_MusicAdpcm_StepRef, the
  shift-add form) gives the same frames as Xbox_SndAdpcm_Decode;
- any window [first, first + count) decodes to the same frames as the
  whole sound, across and inside blocks;
- encoding in place (out == pcm, 8-bit and 16-bit) writes the same bytes
  as into a separate buffer;
- the error stays under a signal-to-noise floor on tones, sweeps, noise,
  clicks at the rails and silence (silence decodes to exact zeros), and
  frames past the end are coded as 0;
- the speed on this host, printed for the record.
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xbox_snd_adpcm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;

#define CHECK( cond, ... ) do { if( !( cond )) { failures++; printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } while( 0 )

static uint32_t rng = 12345u;
static uint32_t Rand( void ) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* The reference: xemu's per-code step, block by block */
static void DecodeRef( const uint8_t *blocks, uint32_t frames, int16_t *out )
{
	uint32_t b, j, nb = ( frames + 63 ) / 64;

	for( b = 0; b < nb; b++ )
	{
		const uint8_t *in = blocks + b * 36u;
		int32_t s = (int16_t)( in[0] | ( in[1] << 8 )), i = in[2];

		out[b * 64] = (int16_t)s;
		for( j = 1; j < 64; j++ )
		{
			const unsigned code = ( in[4 + ( j - 1 ) / 2] >> ((( j - 1 ) & 1 ) * 4 )) & 15u;

			Xbox_MusicAdpcm_StepRef( &s, &i, code );
			out[b * 64 + j] = (int16_t)s;
		}
	}
}

static double SnrDb( const int32_t *ref, const int16_t *got, uint32_t n )
{
	double sig = 0, err = 0;
	uint32_t i;

	for( i = 0; i < n; i++ )
	{
		sig += (double)ref[i] * ref[i];
		err += ( (double)ref[i] - got[i] ) * ( (double)ref[i] - got[i] );
	}
	if( err == 0 )
		return 999.0;
	return 10.0 * log10( sig / err );
}

/* One sound: 8-bit unsigned (width 1) or int16 (width 2) built from `src`
   (16-bit scale values; 8-bit takes the top byte) */
static void RunCase( const char *name, const int32_t *src, uint32_t frames, int width, double min_snr )
{
	const size_t bytes = Xbox_SndAdpcm_Bytes( frames ), pcm_bytes = (size_t)frames * width;
	const uint32_t padded = (uint32_t)( bytes / 36u ) * 64u;
	uint8_t *pcm = malloc( pcm_bytes + 64 ), *blocks = malloc( bytes ), *inplace = malloc( pcm_bytes + bytes + 64 );
	int16_t *dec = malloc( padded * sizeof( int16_t )), *ref = malloc( padded * sizeof( int16_t )), *win = malloc( padded * sizeof( int16_t ));
	int32_t *want = malloc( frames * sizeof( int32_t ) + 4 );
	uint32_t i, b, trial;

	CHECK( bytes == ( frames + 63u ) / 64u * 36u, "%s: bytes %u", name, (unsigned)bytes );
	for( i = 0; i < frames; i++ )
	{
		if( width == 1 )
		{
			int v = src[i] >> 8;

			v = v < -128 ? -128 : v > 127 ? 127 : v;
			pcm[i] = (uint8_t)( v + 128 );
			want[i] = v * 256;
		}
		else
		{
			int32_t v = src[i] < -32768 ? -32768 : src[i] > 32767 ? 32767 : src[i];

			( (int16_t *)pcm )[i] = (int16_t)v;
			want[i] = v;
		}
	}

	Xbox_SndAdpcm_Encode( pcm, width, frames, blocks );

	/* the APU's header rules, the exact first frame */
	for( b = 0; b < bytes / 36u; b++ )
	{
		const uint8_t *h = blocks + b * 36u;
		const int32_t s = (int16_t)( h[0] | ( h[1] << 8 ));

		CHECK( h[2] <= 88 && h[3] == 0, "%s: block %u header %u %u", name, b, h[2], h[3] );
		CHECK( s == ( b * 64u < frames ? want[b * 64u] : 0 ), "%s: block %u starts %d", name, b, s );
	}

	/* the engine's decoder against the reference reconstruction */
	Xbox_SndAdpcm_Decode( blocks, 0, padded, dec );
	DecodeRef( blocks, padded, ref );
	CHECK( !memcmp( dec, ref, padded * sizeof( int16_t )), "%s: decode differs from xemu's steps", name );

	/* windows */
	for( trial = 0; trial < 200 && frames > 0; trial++ )
	{
		uint32_t first = Rand() % frames, count = 1 + Rand() % ( frames - first );

		if( trial < 4 )
		{
			first = trial == 0 ? 0 : trial == 1 ? 63 % frames : trial == 2 ? 64 % frames : frames - 1;
			count = frames - first;
		}
		memset( win, 0x55, count * sizeof( int16_t ));
		Xbox_SndAdpcm_Decode( blocks, first, count, win );
		CHECK( !memcmp( win, dec + first, count * sizeof( int16_t )), "%s: window %u+%u differs", name, first, count );
	}

	/* in place */
	memcpy( inplace, pcm, pcm_bytes );
	Xbox_SndAdpcm_Encode( inplace, width, frames, inplace );
	CHECK( !memcmp( inplace, blocks, bytes < pcm_bytes ? bytes : pcm_bytes ), "%s: in-place encode differs", name );

	/* frames past the end are coded as 0: the same blocks as the sound
	   with silence written out to the end of its last block */
	{
		uint8_t *full = calloc( (size_t)padded * width + 1, 1 ), *fblocks = malloc( bytes );

		memcpy( full, pcm, pcm_bytes );
		if( width == 1 )
			memset( full + pcm_bytes, 128, (size_t)( padded - frames ));
		Xbox_SndAdpcm_Encode( full, width, padded, fblocks );
		CHECK( !memcmp( fblocks, blocks, bytes ), "%s: the padding is not coded as silence", name );
		free( full ); free( fblocks );
	}

	{
		const double snr = SnrDb( want, dec, frames );

		printf( "  %-26s %-6s %7u frames  %6u -> %6u bytes  SNR %6.1f dB\n", name, width == 1 ? "8-bit" : "16-bit",
			frames, (unsigned)pcm_bytes, (unsigned)bytes, snr > 998 ? 999.0 : snr );
		CHECK( snr >= min_snr, "%s: SNR %.1f dB under %.1f", name, snr, min_snr );
	}

	free( pcm ); free( blocks ); free( inplace ); free( dec ); free( ref ); free( win ); free( want );
}

int main( void )
{
	const uint32_t N = 22050;
	int32_t *s = malloc( ( N + 8 ) * sizeof( int32_t ));
	uint32_t i;
	int w;

	for( w = 1; w <= 2; w++ )
	{
		for( i = 0; i < N; i++ ) s[i] = (int32_t)( 12000.0 * sin( 2 * M_PI * 440.0 * i / 22050.0 ));
		RunCase( "tone 440 Hz", s, N, w, 25.0 );
		for( i = 0; i < N; i++ ) s[i] = (int32_t)( 30000.0 * sin( 2 * M_PI * ( 50.0 + 5000.0 * i / N ) * i / 22050.0 ));
		RunCase( "sweep 50-5050 Hz loud", s, N, w, 18.0 );
		for( i = 0; i < N; i++ ) s[i] = (int32_t)( 600.0 * sin( 2 * M_PI * 220.0 * i / 11025.0 ));
		RunCase( "quiet tone", s, N, w, 18.0 );
		for( i = 0; i < N; i++ ) s[i] = (int32_t)( Rand() % 40000 ) - 20000;
		RunCase( "white noise", s, N, w, 6.0 );
		for( i = 0; i < N; i++ ) s[i] = (( i / 300 ) & 1 ) ? 32767 : -32768;
		RunCase( "square at the rails", s, N, w, 14.0 );
		for( i = 0; i < N; i++ ) s[i] = (int32_t)( 20000.0 * exp( -(double)( i % 2000 ) / 200.0 ) * sin( i * 0.3 ));
		RunCase( "decaying clicks", s, N, w, 22.0 );
		for( i = 0; i < N; i++ ) s[i] = 0;
		RunCase( "silence", s, N, w, 998.0 );
		for( i = 0; i < 37; i++ ) s[i] = (int32_t)( 8000.0 * sin( i * 0.5 ));
		RunCase( "37 frames", s, 37, w, 15.0 );
		for( i = 0; i < 128; i++ ) s[i] = (int32_t)( 8000.0 * sin( i * 0.2 ));
		RunCase( "128 frames, whole blocks", s, 128, w, 15.0 );
		s[0] = -12345;
		RunCase( "1 frame", s, 1, w, 998.0 );
	}

	/* speed: a minute of 8-bit 22 kHz noise-ish speech-like content */
	{
		const uint32_t n = 22050u * 60u;
		uint8_t *pcm = malloc( n ), *out = malloc( Xbox_SndAdpcm_Bytes( n ));
		int16_t *dec = malloc( 4096 * sizeof( int16_t ));
		clock_t t0, t1, t2;
		uint32_t k;

		for( k = 0; k < n; k++ )
			pcm[k] = (uint8_t)( 128 + (int)( 60.0 * sin( k * 0.05 ) * sin( k * 0.0007 )) + (int)( Rand() % 9 ) - 4 );
		t0 = clock();
		Xbox_SndAdpcm_Encode( pcm, 1, n, out );
		t1 = clock();
		for( k = 0; k + 4096 <= n; k += 4096 )
			Xbox_SndAdpcm_Decode( out, k, 4096, dec );
		t2 = clock();
		printf( "  speed here: encode %.1f ms, decode %.1f ms for %u frames (%.2f MiB of 8-bit -> %.2f MiB)\n",
			1000.0 * ( t1 - t0 ) / CLOCKS_PER_SEC, 1000.0 * ( t2 - t1 ) / CLOCKS_PER_SEC, n,
			n / 1048576.0, Xbox_SndAdpcm_Bytes( n ) / 1048576.0 );
		free( pcm ); free( out ); free( dec );
	}

	free( s );
	if( failures )
	{
		printf( "xbox_snd_adpcm: %d failure(s)\n", failures );
		return 1;
	}
	printf( "xbox_snd_adpcm: ok\n" );
	return 0;
}
