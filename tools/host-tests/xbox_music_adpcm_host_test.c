/* Host test for engine/platform/xbox/xbox_music_adpcm.c and the baker's
   encoder (divergence #171).

   - Tables: the step table is the IMA one, and the table step agrees with
     the shift-add reference for every (index, code) pair at every
     interesting sample value, clamps included.
   - Block decode: against a verbatim copy of the decoder the console's APU
     is emulated with (xemu's hw/xbox/mcpx/apu/vp/adpcm.h, ADPCM-XQ, BSD
     licence, below) taking 64 of its 65 frames as xemu's VP does, on
     random and hand-built blocks, mono and stereo; refused headers.
   - Round trip through the encoder: silence decodes to exact zeros; a
     full-scale square, a 1 kHz sine and white noise clear SNR floors.
   - Header parser: a well-formed file, then truncations, wrong tags,
     odd-sized chunks, wrong data lengths, missing chunks.
   - Stream: over an in-memory file, reads of many sizes against the
     closed form of upstream's Stream_ReadMPG (whole chunks only, the
     request that runs out returns 0 and so does everything after it),
     seeks to any frame including inside the last block and the last
     chunk, positions, rate conversion, a short read, and the PCM codec.

   gcc -O2 -Wall -std=c99 -Iengine/platform/xbox -Itools/asset-studio/src/formats \
       tools/host-tests/xbox_music_adpcm_host_test.c engine/platform/xbox/xbox_music_adpcm.c \
       tools/asset-studio/src/formats/music_adpcm_encode.c -lm -o xbox_music_adpcm_test */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_music_adpcm.h"
#include "music_adpcm_encode.h"

#define TWO_PI 6.283185307179586

static int failures;
#define CHECK( c ) do { if( !( c )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c ); failures++; } } while( 0 )

static uint32_t rng = 0x12345678u;
static uint32_t Rand( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* ---- xemu's decoder, verbatim apart from formatting (ADPCM-XQ) ----

   Copyright (c) David Bryant. All rights reserved.
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met: redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer;
   redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution;
   neither the name of Conifer Software nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission. THIS SOFTWARE IS PROVIDED BY
   THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
   IMPLIED WARRANTIES ARE DISCLAIMED. */
static const uint16_t xemu_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
	494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
	2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
	10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int xemu_index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static int xemu_adpcm_decode_block( int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels )
{
#define CLIP( data, min, max ) \
	if(( data ) > ( max )) data = max; \
	else if(( data ) < ( min )) data = min;
	const uint16_t *step_table = xemu_step_table;
	const int *index_table = xemu_index_table;
	int samples = 1, chunks;
	int32_t pcmdata[2];
	int8_t index[2];

	if( inbufsize < (uint32_t)channels * 4 )
		return 0;
	for( int ch = 0; ch < channels; ch++ )
	{
		*outbuf++ = pcmdata[ch] = (int16_t)( inbuf[0] | ( inbuf[1] << 8 ));
		index[ch] = inbuf[2];
		if( index[ch] < 0 || index[ch] > 88 || inbuf[3] )
			return 0;
		inbufsize -= 4;
		inbuf += 4;
	}
	chunks = inbufsize / ( channels * 4 );
	samples += chunks * 8;
	while( chunks-- )
	{
		for( int ch = 0; ch < channels; ++ch )
		{
			for( int i = 0; i < 4; ++i )
			{
				int step = step_table[index[ch]], delta = step >> 3;
				if( *inbuf & 1 ) delta += ( step >> 2 );
				if( *inbuf & 2 ) delta += ( step >> 1 );
				if( *inbuf & 4 ) delta += step;
				if( *inbuf & 8 ) delta = -delta;
				pcmdata[ch] += delta;
				index[ch] += index_table[*inbuf & 0x7];
				CLIP( index[ch], 0, 88 );
				CLIP( pcmdata[ch], -32768, 32767 );
				outbuf[i * 2 * channels] = pcmdata[ch];
				step = step_table[index[ch]], delta = step >> 3;
				if( *inbuf & 0x10 ) delta += ( step >> 2 );
				if( *inbuf & 0x20 ) delta += ( step >> 1 );
				if( *inbuf & 0x40 ) delta += step;
				if( *inbuf & 0x80 ) delta = -delta;
				pcmdata[ch] += delta;
				index[ch] += index_table[( *inbuf >> 4 ) & 0x7];
				CLIP( index[ch], 0, 88 );
				CLIP( pcmdata[ch], -32768, 32767 );
				outbuf[( i * 2 + 1 ) * channels] = pcmdata[ch];
				inbuf++;
			}
			outbuf++;
		}
		outbuf += channels * 7;
	}
	return samples;
#undef CLIP
}

/* ---- tables ---- */

static void TestTables( void )
{
	static const int32_t samples[] = { -32768, -32767, -30000, -1000, -1, 0, 1, 1000, 30000, 32766, 32767 };
	int i, c, k;

	CHECK( xbox_adpcm_step_table[0] == 7 );
	CHECK( xbox_adpcm_step_table[88] == 32767 );
	for( i = 1; i < 89; i++ )
		CHECK( xbox_adpcm_step_table[i] > xbox_adpcm_step_table[i - 1] );
	/* entry for entry the table of the decoder xemu's APU uses (below) */
	CHECK( !memcmp( xbox_adpcm_step_table, xemu_step_table, sizeof( xemu_step_table )));
	for( i = 0; i < 8; i++ )
		CHECK( xbox_adpcm_index_table[i] == xemu_index_table[i] );

	for( i = 0; i <= 88; i++ )
		for( c = 0; c < 16; c++ )
			for( k = 0; k < (int)( sizeof( samples ) / sizeof( samples[0] )); k++ )
			{
				int32_t s1 = samples[k], i1 = i, s2 = samples[k], i2 = i;

				Xbox_MusicAdpcm_StepRef( &s1, &i1, (unsigned)c );
				Xbox_MusicAdpcm_Step( &s2, &i2, (unsigned)c );
				CHECK( s1 == s2 && i1 == i2 );
				CHECK( s1 >= -32768 && s1 <= 32767 && i1 >= 0 && i1 <= 88 );
			}

	/* the delta, signed by bit 3, and the index moves */
	{
		int32_t s = 0, ix = 0;
		Xbox_MusicAdpcm_Step( &s, &ix, 7 );  /* step 7: 0 + 3 + 1 + 7 = 11, index +8 */
		CHECK( s == 11 && ix == 8 );
		Xbox_MusicAdpcm_Step( &s, &ix, 15 ); /* step 16: -(2 + 4 + 8 + 16) = -30 */
		CHECK( s == 11 - 30 && ix == 16 );
		Xbox_MusicAdpcm_Step( &s, &ix, 0 );  /* step 34: 34 >> 3 = 4, index -1 */
		CHECK( s == -19 + 4 && ix == 15 );
	}
	{
		int32_t s = 32767, ix = 88;
		Xbox_MusicAdpcm_Step( &s, &ix, 7 );
		CHECK( s == 32767 && ix == 88 );
		s = -32768;
		Xbox_MusicAdpcm_Step( &s, &ix, 15 );
		CHECK( s == -32768 && ix == 88 );
		ix = 0;
		Xbox_MusicAdpcm_Step( &s, &ix, 3 );
		CHECK( ix == 0 );
	}
}

/* ---- block decode ---- */

static void RandomBlock( uint8_t *b, int channels )
{
	int i;

	for( i = 0; i < 36 * channels; i++ )
		b[i] = (uint8_t)Rand();
	for( i = 0; i < channels; i++ )
	{
		b[i * 4 + 2] = (uint8_t)( Rand() % 89 );
		b[i * 4 + 3] = 0;
	}
}

static void TestBlockDecode( void )
{
	int t, channels, i;

	for( channels = 1; channels <= 2; channels++ )
	{
		for( t = 0; t < 20000; t++ )
		{
			uint8_t b[72];
			int16_t ours[128], ref[130];
			int r;

			RandomBlock( b, channels );
			/* some extremes: header at the rails, all-ones codes */
			if( t % 7 == 0 )
				for( i = channels * 4; i < 36 * channels; i++ )
					b[i] = ( t & 8 ) ? 0x77 : 0xFF;
			if( t % 11 == 0 )
			{
				b[0] = 0xFF; b[1] = 0x7F;
			}

			r = Xbox_MusicAdpcm_DecodeBlock( b, channels, ours );
			CHECK( r == 1 );
			CHECK( xemu_adpcm_decode_block( ref, b, 36u * channels, channels ) == 65 );
			/* xemu's VP plays frames 0..63 of the 65 (vp.c:1015-1016) */
			if( memcmp( ours, ref, sizeof( int16_t ) * 64 * channels ))
			{
				CHECK( !"block differs from xemu's decoder" );
				break;
			}
		}
	}

	/* the header sample is frame 0; channel 1's first code is in the
	   second 4-byte group of the stereo block */
	{
		uint8_t b[72];
		int16_t out[128];

		memset( b, 0, sizeof( b ));
		b[0] = 0x34; b[1] = 0x12;               /* ch0 header 0x1234, index 0 */
		b[4] = 0xCC; b[5] = 0xED; b[6] = 10;    /* ch1 header -0x1234, index 10 */
		b[8] = 0x07;                            /* ch0 code 0: +11 */
		b[12] = 0x0F;                           /* ch1 code 0: -(step 19: 2+4+9+19 = 34) */
		CHECK( Xbox_MusicAdpcm_DecodeBlock( b, 2, out ) == 1 );
		CHECK( out[0] == 0x1234 && out[1] == -0x1234 );
		CHECK( out[2] == 0x1234 + 11 && out[3] == -0x1234 - 34 );
	}

	/* refused headers decode to silence, as xemu's returns nothing */
	{
		uint8_t b[72];
		int16_t out[128];

		RandomBlock( b, 2 );
		b[4 + 2] = 89;
		memset( out, 0x55, sizeof( out ));
		CHECK( Xbox_MusicAdpcm_DecodeBlock( b, 2, out ) == 0 );
		for( i = 0; i < 128; i++ )
			CHECK( out[i] == 0 );
		RandomBlock( b, 1 );
		b[3] = 1;
		CHECK( Xbox_MusicAdpcm_DecodeBlock( b, 1, out ) == 0 );
	}

	/* CRC-32 check value */
	CHECK( Xbox_MusicCrc32( 0, "123456789", 9 ) == 0xCBF43926u );
	CHECK( Xbox_MusicCrc32( Xbox_MusicCrc32( 0, "1234", 4 ), "56789", 5 ) == 0xCBF43926u );
}

/* ---- encode and decode a signal ---- */

/* Encodes `frames` interleaved frames into blocks; returns the bytes. */
static uint8_t *EncodeAll( const int16_t *pcm, uint32_t frames, int channels, uint32_t *out_bytes )
{
	const uint32_t blocks = ( frames + 63 ) / 64;
	int16_t *padded = calloc(( blocks * 64 + 1 ) * channels, sizeof( int16_t ));
	uint8_t *out = malloc( blocks * 36u * channels );
	int hint[2] = { 0, 0 };
	uint32_t b;

	memcpy( padded, pcm, frames * channels * sizeof( int16_t ));
	for( b = 0; b < blocks; b++ )
		Music_AdpcmEncodeBlock( padded + b * 64 * channels, channels, hint, out + b * 36u * channels );
	free( padded );
	*out_bytes = blocks * 36u * channels;
	return out;
}

static double SnrDb( const int16_t *a, const int16_t *b, size_t n )
{
	double sig = 0, noise = 0;
	size_t i;

	for( i = 0; i < n; i++ )
	{
		sig += (double)a[i] * a[i];
		noise += ( (double)a[i] - b[i] ) * ( (double)a[i] - b[i] );
	}
	if( noise == 0 )
		return 200.0;
	return 10.0 * log10( sig / noise );
}

static double RoundTrip( const int16_t *pcm, uint32_t frames, int channels, int16_t *dec )
{
	uint32_t bytes, b;
	uint8_t *enc = EncodeAll( pcm, frames, channels, &bytes );
	int16_t *full = malloc(( frames + 64 ) * channels * sizeof( int16_t ));
	double snr;

	for( b = 0; b < bytes / ( 36u * channels ); b++ )
		CHECK( Xbox_MusicAdpcm_DecodeBlock( enc + b * 36u * channels, channels, full + b * 64 * channels ) == 1 );
	memcpy( dec, full, frames * channels * sizeof( int16_t ));
	snr = SnrDb( pcm, dec, (size_t)frames * channels );
	free( full );
	free( enc );
	return snr;
}

static void TestRoundTrip( void )
{
	const uint32_t n = 44100;
	int16_t *pcm = malloc( n * 2 * sizeof( int16_t ));
	int16_t *dec = malloc( n * 2 * sizeof( int16_t ));
	uint32_t i;
	double snr;

	memset( pcm, 0, n * 2 * sizeof( int16_t ));
	RoundTrip( pcm, n, 2, dec );
	for( i = 0; i < n * 2; i++ )
		CHECK( dec[i] == 0 );

	for( i = 0; i < n; i++ )
	{
		pcm[i * 2] = ( i / 50 ) & 1 ? 32767 : -32768; /* 441 Hz full-scale square */
		pcm[i * 2 + 1] = (int16_t)( 20000.0 * sin( TWO_PI * 1000.0 * i / 44100.0 ));
	}
	/* The floors sit about 2 dB under what encoder 1 measures (13.0, 39.9,
	   17.2 dB): they catch a broken encoder or decoder, not a retune. A
	   full-scale square is IMA's worst case - every edge is a 65535 jump
	   the step needs several frames to climb. */
	snr = RoundTrip( pcm, n, 2, dec );
	printf( "  full-scale square + 1 kHz sine: %.1f dB\n", snr );
	CHECK( snr > 11.0 );

	for( i = 0; i < n; i++ )
		pcm[i] = (int16_t)( 20000.0 * sin( TWO_PI * 1000.0 * i / 44100.0 ));
	snr = RoundTrip( pcm, n, 1, dec );
	printf( "  1 kHz sine, mono: %.1f dB\n", snr );
	CHECK( snr > 37.5 );

	for( i = 0; i < n * 2; i++ )
		pcm[i] = (int16_t)(( Rand() & 0x3FFF ) - 0x2000);
	snr = RoundTrip( pcm, n, 2, dec );
	printf( "  white noise: %.1f dB\n", snr );
	CHECK( snr > 15.0 );

	free( pcm );
	free( dec );
}

/* ---- an in-memory .xwav ---- */

typedef struct memfile_s
{
	uint8_t *data;
	uint32_t len;
	uint32_t pos;
	int short_from;   /* > 0: reads deliver nothing at or past this offset */
	int seeks, reads;
} memfile_t;

static int MemRead( void *ctx, void *dst, int bytes )
{
	memfile_t *f = ctx;
	uint32_t end = f->short_from > 0 && (uint32_t)f->short_from < f->len ? (uint32_t)f->short_from : f->len;
	int n = 0;

	f->reads++;
	if( f->pos < end )
	{
		n = (int)( end - f->pos );
		if( n > bytes )
			n = bytes;
		memcpy( dst, f->data + f->pos, (size_t)n );
		f->pos += (uint32_t)n;
	}
	return n;
}

static int MemSeek( void *ctx, uint32_t offset )
{
	memfile_t *f = ctx;

	f->seeks++;
	if( offset > f->len )
		return -1;
	f->pos = offset;
	return 0;
}

static void Put16( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); }
static void Put32( uint8_t *p, uint32_t v ) { Put16( p, v ); Put16( p + 2, v >> 16 ); }

/* A file as the baker lays it out (tools/asset-studio MusicBake.cpp):
   RIFF, fmt, r3dm, data. `junk` > 0 puts a chunk of that (odd) size
   between fmt and r3dm. */
static uint8_t *BuildXwav( int tag, int channels, uint32_t rate, uint32_t source_rate, uint32_t frames,
	uint32_t chunk_bytes, const uint8_t *data, uint32_t data_bytes, int junk, uint32_t *out_len )
{
	const uint32_t fmt_size = tag == XBOX_XWAV_TAG_ADPCM ? 20 : 16;
	const uint32_t junk_total = junk > 0 ? 8 + (uint32_t)junk + ( junk & 1 ) : 0;
	const uint32_t len = 12 + 8 + fmt_size + junk_total + 8 + XBOX_XWAV_R3DM_BYTES + 8 + data_bytes;
	uint8_t *f = calloc( len, 1 ), *p = f;

	memcpy( p, "RIFF", 4 ); Put32( p + 4, len - 8 ); memcpy( p + 8, "WAVE", 4 ); p += 12;
	memcpy( p, "fmt ", 4 ); Put32( p + 4, fmt_size ); p += 8;
	Put16( p, (uint32_t)tag ); Put16( p + 2, (uint32_t)channels ); Put32( p + 4, rate );
	if( tag == XBOX_XWAV_TAG_ADPCM )
	{
		Put32( p + 8, rate * 36u * channels / 64u ); Put16( p + 12, 36u * channels ); Put16( p + 14, 4 );
		Put16( p + 16, 2 ); Put16( p + 18, 64 );
	}
	else
	{
		Put32( p + 8, rate * 2u * channels ); Put16( p + 12, 2u * channels ); Put16( p + 14, 16 );
	}
	p += fmt_size;
	if( junk > 0 )
	{
		memcpy( p, "LIST", 4 ); Put32( p + 4, (uint32_t)junk ); p += 8 + junk + ( junk & 1 );
	}
	memcpy( p, "r3dm", 4 ); Put32( p + 4, XBOX_XWAV_R3DM_BYTES ); p += 8;
	Put32( p, XBOX_XWAV_R3DM_VERSION ); Put32( p + 4, frames ); Put32( p + 8, source_rate );
	Put32( p + 12, rate == source_rate ? frames : (uint32_t)(( (uint64_t)frames * source_rate ) / rate ));
	Put32( p + 16, 0x11111111u ); Put32( p + 20, 0x22222222u ); Put32( p + 24, chunk_bytes ); Put32( p + 28, 123456 );
	memcpy( p + 32, "mp3", 4 ); Put16( p + 40, MUSIC_ADPCM_ENCODER_ID ); Put16( p + 42, 0 );
	p += XBOX_XWAV_R3DM_BYTES;
	memcpy( p, "data", 4 ); Put32( p + 4, data_bytes ); p += 8;
	memcpy( p, data, data_bytes );
	*out_len = len;
	return f;
}

static void TestHeader( void )
{
	uint8_t data[36 * 2 * 3];
	uint32_t len, i;
	uint8_t *f;
	xbox_xwav_info_t info;

	memset( data, 0, sizeof( data ));
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 150, 8192, data, sizeof( data ), 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK );
	CHECK( info.tag == XBOX_XWAV_TAG_ADPCM && info.channels == 2 && info.rate == 44100 && info.block_bytes == 72 );
	CHECK( info.frames == 150 && info.source_frames == 150 && info.chunk_bytes == 8192 && info.source_bytes == 123456 );
	CHECK( info.crc_source == 0x11111111u && info.crc_decoded == 0x22222222u );
	CHECK( !strcmp( info.source_ext, "mp3" ) && info.encoder_id == MUSIC_ADPCM_ENCODER_ID );
	CHECK( info.data_offset == len - sizeof( data ) && info.data_bytes == sizeof( data ));

	/* every truncation fails, never reads past the buffer (the sanitizer
	   would trap), and the file length alone also truncates */
	for( i = 0; i < len; i++ )
	{
		xbox_xwav_info_t again;

		/* a header read that holds the data chunk's 8-byte header is
		   enough: the samples are streamed */
		if( i >= info.data_offset )
			CHECK( Xbox_MusicXwav_ParseHeader( f, i, len, &again ) == XBOX_XWAV_OK && again.frames == 150 );
		else CHECK( Xbox_MusicXwav_ParseHeader( f, i, len, &again ) != XBOX_XWAV_OK );
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, i, &again ) != XBOX_XWAV_OK );
	}

	f[0] = 'X';
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_MAGIC );
	f[0] = 'R';
	f[20] = 0x11; /* tag: IMA ADPCM, not Xbox ADPCM */
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_CODEC );
	f[20] = 0x69;
	f[22] = 3; /* channels */
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_CODEC );
	f[22] = 2;
	f[38] = 65; /* wSamplesPerBlock */
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_CODEC );
	f[38] = 64;
	f[32] = 36; /* nBlockAlign for mono, in a stereo file */
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_CODEC );
	f[32] = 72;
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK );
	free( f );

	/* an odd-sized chunk before r3dm: skipped with its pad byte */
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 150, 8192, data, sizeof( data ), 7, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK && info.frames == 150 );
	free( f );

	/* a data length that is not the frame count's blocks */
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 129, 8192, data, sizeof( data ), 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK ); /* 129 needs 3 blocks */
	free( f );
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 128, 8192, data, sizeof( data ), 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_DATA ); /* 128 is 2 blocks */
	free( f );
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 193, 8192, data, sizeof( data ), 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_DATA ); /* 193 needs 4 */
	free( f );

	/* PCM: 2 * channels per frame exactly */
	f = BuildXwav( XBOX_XWAV_TAG_PCM, 2, 44100, 44100, 54, 8192, data, 216, 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK && info.block_bytes == 256 );
	free( f );
	f = BuildXwav( XBOX_XWAV_TAG_PCM, 2, 44100, 44100, 55, 8192, data, 216, 0, &len );
	CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_DATA );
	free( f );

	/* r3dm: version, chunk size not a frame multiple, a frame count the
	   rates disagree with, an extension without its NUL */
	f = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 44100, 44100, 150, 8192, data, sizeof( data ), 0, &len );
	{
		uint8_t *r = f + 12 + 8 + 20 + 8;

		r[0] = 2;
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_R3DM );
		r[0] = 1;
		Put32( r + 24, 8190 );
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_R3DM );
		Put32( r + 24, 8192 );
		Put32( r + 12, 151 );
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_R3DM );
		Put32( r + 12, 150 );
		memset( r + 32, 'x', 8 );
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_R3DM );
		memcpy( r + 32, "mp3\0\0\0\0\0", 8 );
		memcpy( r - 8, "r3dX", 4 ); /* no r3dm at all */
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_ERR_R3DM );
		memcpy( r - 8, "r3dm", 4 );
		CHECK( Xbox_MusicXwav_ParseHeader( f, len, len, &info ) == XBOX_XWAV_OK );
	}
	free( f );

	CHECK( strlen( Xbox_MusicXwav_ErrorString( XBOX_XWAV_ERR_DATA )) > 0 );
}

/* ---- the stream ---- */

typedef struct track_s
{
	uint8_t *file;
	uint32_t len;
	int16_t *pcm;       /* the expected decode, `frames` frames */
	xbox_xwav_info_t info;
} track_t;

static void MakeTrack( track_t *t, int tag, int channels, uint32_t frames, uint32_t rate, uint32_t source_rate, uint32_t chunk_bytes )
{
	int16_t *src = malloc((size_t)( frames + 64 ) * channels * sizeof( int16_t ));
	uint8_t *data;
	uint32_t data_bytes, i, b;

	for( i = 0; i < ( frames + 64 ) * (uint32_t)channels; i++ )
		src[i] = (int16_t)( 12000.0 * sin( i * 0.013 ) + (int)( Rand() % 2001 ) - 1000 );

	t->pcm = malloc((size_t)( frames + 64 ) * channels * sizeof( int16_t ));
	if( tag == XBOX_XWAV_TAG_ADPCM )
	{
		data = EncodeAll( src, frames, channels, &data_bytes );
		for( b = 0; b < data_bytes / ( 36u * channels ); b++ )
			Xbox_MusicAdpcm_DecodeBlock( data + b * 36u * channels, channels, t->pcm + b * 64 * channels );
	}
	else
	{
		data_bytes = frames * 2u * channels;
		data = malloc( data_bytes );
		memcpy( data, src, data_bytes ); /* little-endian host */
		memcpy( t->pcm, src, data_bytes );
	}
	t->file = BuildXwav( tag, channels, rate, source_rate, frames, chunk_bytes, data, data_bytes, 0, &t->len );
	CHECK( Xbox_MusicXwav_ParseHeader( t->file, t->len, t->len, &t->info ) == XBOX_XWAV_OK );
	free( data );
	free( src );
}

static void FreeTrack( track_t *t )
{
	free( t->file );
	free( t->pcm );
}

typedef struct opened_s
{
	xbox_music_stream_t s;
	memfile_t mf;
	uint8_t chunk[8192];
	uint8_t ra[XBOX_MUSIC_READAHEAD_BYTES];
} opened_t;

static void Open( opened_t *o, track_t *t )
{
	xbox_music_io_t io;

	memset( &o->mf, 0, sizeof( o->mf ));
	o->mf.data = t->file;
	o->mf.len = t->len;
	io.read = MemRead;
	io.seek = MemSeek;
	io.ctx = &o->mf;
	CHECK( Xbox_MusicStream_Init( &o->s, &t->info, &io, o->chunk, sizeof( o->chunk ), o->ra ));
}

/* Reads with sizes from `sizes` (cycled) starting at frame `start`
   (after a SetPos when non-zero) and checks every result against the
   closed form of Stream_ReadMPG over chunks of chunk_bytes:
   - a request is served iff the bytes asked for so far fit inside the
     whole chunks left from the start, start + floor((N - start) / cf) * cf
     frames;
   - a served request returns the next bytes of the decode;
   - the first refused request returns 0, and so does every later one;
   - the position is start + (chunks pulled) * cf, the whole track after
     the refusal. */
static void CheckReads( track_t *t, uint32_t start_frame, const int *sizes, int nsizes, int seek_first )
{
	/* a seek is asked for in source frames; the frame it lands on is the
	   file frame that converts back to them */
	const uint32_t start = seek_first ?
		Xbox_MusicXwav_ToFileFrames( &t->info, Xbox_MusicXwav_ToSourceFrames( &t->info, start_frame )) : start_frame;
	opened_t *o = malloc( sizeof( *o ));
	const uint32_t fb = 2u * t->info.channels;
	const uint32_t cb = t->info.chunk_bytes, cf = cb / fb;
	const uint32_t N = t->info.frames;
	const uint32_t s0 = start > N ? N : start;
	const uint32_t end_pos = start > N ? start : N; /* past the end the position stays the target, as libmpg's */
	const uint64_t servable = (uint64_t)(( N - s0 ) / cf ) * cb;
	uint64_t asked = 0;
	uint8_t *buf = malloc( 20000 );
	int k, refused = 0;

	Open( o, t );
	if( seek_first )
	{
		CHECK( Xbox_MusicStream_SetPos( &o->s, (int)Xbox_MusicXwav_ToSourceFrames( &t->info, start_frame )) == 1 );
		CHECK( (uint32_t)Xbox_MusicStream_GetPos( &o->s ) == Xbox_MusicXwav_ToSourceFrames( &t->info, start ));
	}

	for( k = 0; k < 200000 && !( refused && k > 4 ); k++ )
	{
		const int need = sizes[k % nsizes];
		int r = Xbox_MusicStream_Read( &o->s, need, buf );

		if( !refused && asked + (uint64_t)need <= servable )
		{
			CHECK( r == need );
			if( r != need || memcmp( buf, (const uint8_t *)t->pcm + (uint64_t)s0 * fb + asked, (size_t)need ))
			{
				CHECK( !"read differs from the decode" );
				break;
			}
			asked += (uint64_t)need;
			/* chunks pulled so far: ceil(asked / cb) */
			CHECK( (uint32_t)Xbox_MusicStream_GetPos( &o->s ) ==
				Xbox_MusicXwav_ToSourceFrames( &t->info, s0 + (uint32_t)(( asked + cb - 1 ) / cb ) * cf ));
		}
		else
		{
			CHECK( r == 0 );
			refused = 1;
			CHECK( (uint32_t)Xbox_MusicStream_GetPos( &o->s ) == Xbox_MusicXwav_ToSourceFrames( &t->info, end_pos ));
		}
	}
	CHECK( refused );
	CHECK( o->s.bad_blocks == 0 );
	free( buf );
	free( o );
}

static void TestStream( void )
{
	static const int engine_like[] = { 16384, 16384, 2940, 2944, 2936, 3000, 4 };
	static const int odd[] = { 1, 3, 4, 7, 100, 4095, 4096, 4097, 8191, 8192, 8193, 5000 * 4, 2 };
	track_t t;
	int k, i;
	uint32_t p;

	/* stereo ADPCM, libmpg's real chunk (OUTBUF_SIZE 8192 = 2048 frames),
	   a length with a short last chunk and a short last block */
	MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 2048 * 5 + 1000 + 37, 44100, 44100, 8192 );
	CheckReads( &t, 0, engine_like, 7, 0 );
	CheckReads( &t, 0, odd, 13, 0 );
	for( k = 1; k <= 5000; k += 37 )
	{
		int one[1];

		one[0] = k * 4;
		CheckReads( &t, 0, one, 1, 0 );
	}
	for( i = 0; i < 300; i++ )
	{
		p = Rand() % ( t.info.frames + 100 );
		if( i < 6 )
			p = (uint32_t[]){ 0, 63, 64, 2048 * 5, t.info.frames - 1, t.info.frames }[i];
		CheckReads( &t, p, odd, 13, 1 );
		CheckReads( &t, p, engine_like, 7, 1 );
	}
	/* a whole number of chunks: the read that wants one more byte is refused */
	FreeTrack( &t );
	MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 2048 * 3, 44100, 44100, 8192 );
	CheckReads( &t, 0, engine_like, 7, 0 );
	{
		static const int exact[] = { 8192 };
		CheckReads( &t, 0, exact, 1, 0 );
	}
	FreeTrack( &t );

	/* a short track: one partial chunk, so nothing is ever served */
	MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 2000, 44100, 44100, 8192 );
	CheckReads( &t, 0, odd, 13, 0 );
	FreeTrack( &t );

	/* mono, a small chunk so many chunk boundaries fall inside blocks */
	MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 1, 9999, 22050, 22050, 200 );
	CheckReads( &t, 0, odd, 13, 0 );
	for( i = 0; i < 100; i++ )
		CheckReads( &t, Rand() % 10000, odd, 13, 1 );
	FreeTrack( &t );

	/* PCM codec through the same reader */
	MakeTrack( &t, XBOX_XWAV_TAG_PCM, 2, 2048 * 4 + 999, 44100, 44100, 8192 );
	CheckReads( &t, 0, engine_like, 7, 0 );
	for( i = 0; i < 100; i++ )
		CheckReads( &t, Rand() % t.info.frames, odd, 13, 1 );
	FreeTrack( &t );

	/* a file at another rate than its source (a 48 kHz rebake, X12 step 2)
	   is refused for now - the replay of libmpg's chunks holds only at the
	   source rate - but the position conversions it will need are here
	   already: both ways, round down / up, monotonic */
	{
		xbox_xwav_info_t r48;
		uint8_t *f48;
		uint32_t len48;
		uint8_t data48[( 48000 / 64 + 1 ) * 72];

		memset( data48, 0, sizeof( data48 ));
		f48 = BuildXwav( XBOX_XWAV_TAG_ADPCM, 2, 48000, 44100, 48000, 8192, data48, ( 48000 + 63 ) / 64 * 72, 0, &len48 );
		CHECK( Xbox_MusicXwav_ParseHeader( f48, len48, len48, &r48 ) == XBOX_XWAV_ERR_R3DM );
		free( f48 );

		memset( &r48, 0, sizeof( r48 ));
		r48.rate = 48000;
		r48.source_rate = 44100;
		for( p = 0; p < 48000; p += 7 )
		{
			uint32_t s = Xbox_MusicXwav_ToSourceFrames( &r48, p );
			uint32_t back = Xbox_MusicXwav_ToFileFrames( &r48, s );

			CHECK( s == (uint32_t)(( (uint64_t)p * 44100 ) / 48000 ));
			CHECK( back <= p && p - back <= 2 );
		}
		for( p = 0; p < 44100; p += 13 )
		{
			uint32_t f = Xbox_MusicXwav_ToFileFrames( &r48, p );

			CHECK( Xbox_MusicXwav_ToSourceFrames( &r48, f ) == p ); /* source -> file -> source is exact */
		}
	}

	/* SetPos: negative is the start; past the end, as libmpg's seek: the
	   position is the target and the next read returns 0 */
	{
		opened_t *o = malloc( sizeof( *o ));
		uint8_t buf[64];

		MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 2048 * 2 + 5, 44100, 44100, 8192 );
		Open( o, &t );
		CHECK( Xbox_MusicStream_SetPos( &o->s, -5 ) == 1 && Xbox_MusicStream_GetPos( &o->s ) == 0 );
		CHECK( Xbox_MusicStream_Read( &o->s, 64, buf ) == 64 && !memcmp( buf, t.pcm, 64 ));
		CHECK( Xbox_MusicStream_SetPos( &o->s, (int)t.info.frames + 77 ) == 1 && Xbox_MusicStream_GetPos( &o->s ) == (int)t.info.frames + 77 );
		CHECK( Xbox_MusicStream_Read( &o->s, 64, buf ) == 0 );
		CHECK( Xbox_MusicStream_Read( &o->s, 64, buf ) == 0 );
		CHECK( Xbox_MusicStream_SetPos( &o->s, 4 ) == 1 && Xbox_MusicStream_Read( &o->s, 64, buf ) == 64 && !memcmp( buf, t.pcm + 8, 64 ));
		free( o );
		FreeTrack( &t );
	}

	/* the read-ahead: one read per 128 stereo blocks */
	{
		opened_t *o = malloc( sizeof( *o ));
		int16_t *all;

		MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 128 * 64 * 3 + 10, 44100, 44100, 8192 );
		all = malloc( t.info.frames * 4u );
		Open( o, &t );
		CHECK( Xbox_MusicStream_DecodeFrames( &o->s, 0, t.info.frames, all ) == t.info.frames );
		CHECK( !memcmp( all, t.pcm, t.info.frames * 4u ));
		CHECK( o->s.io_reads == 4 && o->mf.reads == 4 );
		free( all );
		free( o );
		FreeTrack( &t );
	}

	/* a read that comes back short ends the track at the last whole block
	   it delivered, as a failed libmpg read would */
	{
		opened_t *o = malloc( sizeof( *o ));
		uint8_t *buf = malloc( 8192 );
		int served = 0;

		MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 2048 * 8, 44100, 44100, 8192 );
		Open( o, &t );
		o->mf.short_from = (int)( t.info.data_offset + 72 * 200 + 5 ); /* inside block 200 */
		while( Xbox_MusicStream_Read( &o->s, 8192, buf ) == 8192 )
		{
			CHECK( !memcmp( buf, (uint8_t *)t.pcm + served * 8192, 8192 ));
			served++;
		}
		CHECK( served == 200 * 64 / 2048 ); /* 6 whole chunks before block 200 */
		CHECK( o->s.io_failed == 1 );
		CHECK( Xbox_MusicStream_Read( &o->s, 8192, buf ) == 0 );
		free( buf );
		free( o );
		FreeTrack( &t );
	}

	/* tell_last_chunk: libmpg's own position after the last whole chunk of
	   a fresh open (its gapless end adjustment), reported at that one
	   point only - not after a seek, not once the stream has ended */
	{
		opened_t *o = malloc( sizeof( *o ));
		uint8_t *buf = malloc( 8192 );
		const uint32_t frames = 2048 * 3 + 500;
		uint8_t *r3dm;
		int pos;

		MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, frames, 44100, 44100, 8192 );
		r3dm = t.file + 12 + 8 + 20 + 8;
		CHECK( !memcmp( r3dm - 8, "r3dm", 4 ));
		Put32( r3dm + 44, frames - 100 );
		CHECK( Xbox_MusicXwav_ParseHeader( t.file, t.len, t.len, &t.info ) == XBOX_XWAV_OK );
		CHECK( t.info.tell_last_chunk == frames - 100 );
		Open( o, &t );
		CHECK( Xbox_MusicStream_Read( &o->s, 8192, buf ) == 8192 && Xbox_MusicStream_GetPos( &o->s ) == 2048 );
		CHECK( Xbox_MusicStream_Read( &o->s, 8192, buf ) == 8192 && Xbox_MusicStream_GetPos( &o->s ) == 4096 );
		CHECK( Xbox_MusicStream_Read( &o->s, 4, buf ) == 4 );
		pos = Xbox_MusicStream_GetPos( &o->s );
		CHECK( pos == (int)frames - 100 );
		CHECK( Xbox_MusicStream_Read( &o->s, 8188, buf ) == 8188 && Xbox_MusicStream_GetPos( &o->s ) == (int)frames - 100 );
		CHECK( Xbox_MusicStream_Read( &o->s, 4, buf ) == 0 && Xbox_MusicStream_GetPos( &o->s ) == (int)frames );
		/* after a seek the count is the position, there too */
		CHECK( Xbox_MusicStream_SetPos( &o->s, 0 ) == 1 );
		for( k = 0; k < 3; k++ )
			CHECK( Xbox_MusicStream_Read( &o->s, 8192, buf ) == 8192 );
		CHECK( Xbox_MusicStream_GetPos( &o->s ) == 2048 * 3 );
		/* a value past the end is refused */
		Put32( r3dm + 44, frames + 1 );
		CHECK( Xbox_MusicXwav_ParseHeader( t.file, t.len, t.len, &t.info ) == XBOX_XWAV_ERR_R3DM );
		free( buf );
		free( o );
		FreeTrack( &t );
	}

	/* Init refuses a chunk larger than the buffer or not a frame multiple */
	{
		opened_t *o = malloc( sizeof( *o ));
		xbox_music_io_t io = { MemRead, MemSeek, NULL };

		MakeTrack( &t, XBOX_XWAV_TAG_ADPCM, 2, 100, 44100, 44100, 8192 );
		CHECK( !Xbox_MusicStream_Init( &o->s, &t.info, &io, o->chunk, 4096, o->ra ));
		t.info.chunk_bytes = 8190;
		CHECK( !Xbox_MusicStream_Init( &o->s, &t.info, &io, o->chunk, 8192, o->ra ));
		free( o );
		FreeTrack( &t );
	}
}

/* ---- which file a request names, and whether a .xwav stands in for it ---- */

typedef struct fakefs_s
{
	const char *names[8];
	int64_t sizes[8];
	int n;
	int probes;
} fakefs_t;

static int FakeProbe( void *ctx, const char *path, int64_t *size )
{
	fakefs_t *fs = ctx;
	int i;

	fs->probes++;
	for( i = 0; i < fs->n; i++ )
	{
		if( !strcmp( fs->names[i], path ))
		{
			*size = fs->sizes[i];
			return 1;
		}
	}
	return 0;
}

static void TestDiscovery( void )
{
	static const char *const formats[] = { "mp3", "wav", "ogg", "opus" }; /* stream_game, snd_utils.c:51-57 */
	char base[64];
	xbox_xwav_info_t info;
	fakefs_t fs;

	CHECK( Xbox_MusicXwav_SplitName( "media/Half-Life01.mp3", formats, 4, base, sizeof( base )) == 0 && !strcmp( base, "media/Half-Life01" ));
	CHECK( Xbox_MusicXwav_SplitName( "media/Half-Life01.MP3", formats, 4, base, sizeof( base )) == 0 && !strcmp( base, "media/Half-Life01" ));
	CHECK( Xbox_MusicXwav_SplitName( "media/gamestartup", formats, 4, base, sizeof( base )) == -1 && !strcmp( base, "media/gamestartup" ));
	CHECK( Xbox_MusicXwav_SplitName( "media/a.b", formats, 4, base, sizeof( base )) == -1 && !strcmp( base, "media/a.b" ));
	CHECK( Xbox_MusicXwav_SplitName( "x.wav", formats, 4, base, sizeof( base )) == 1 && !strcmp( base, "x" ));
	CHECK( Xbox_MusicXwav_SplitName( "my.dir/track", formats, 4, base, sizeof( base )) == -1 && !strcmp( base, "my.dir/track" ));
	CHECK( Xbox_MusicXwav_SplitName( "my.mp3\\track", formats, 4, base, sizeof( base )) == -1 );
	CHECK( Xbox_MusicXwav_SplitName( "c.mp3:x", formats, 4, base, sizeof( base )) == -1 );
	CHECK( Xbox_MusicXwav_SplitName( "trailing.", formats, 4, base, sizeof( base )) == -1 );

	memset( &info, 0, sizeof( info ));
	memcpy( info.source_ext, "mp3", 4 );
	info.source_bytes = 1000;

	/* the requested format, present and the baked size */
	memset( &fs, 0, sizeof( fs ));
	fs.names[0] = "media/T.mp3"; fs.sizes[0] = 1000; fs.n = 1;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 0, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_OK );
	/* another size: a stale bake, or another file of that name */
	fs.sizes[0] = 1001;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 0, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_SIZE );
	/* asked for the .wav: upstream would only try the wav */
	fs.sizes[0] = 1000;
	fs.names[1] = "media/T.wav"; fs.sizes[1] = 5; fs.n = 2;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 1, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_FORMAT );
	/* no extension: the first format present wins, in upstream's order */
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", -1, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_OK );
	fs.names[0] = "media/T.ogg";
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", -1, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_FORMAT );
	/* nothing there */
	fs.n = 0;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", -1, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_NONE );
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 0, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_NONE );
	/* a requested extension probes only that format */
	fs.probes = 0;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 2, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_NONE && fs.probes == 1 );
	/* case-insensitive extension match against r3dm */
	memcpy( info.source_ext, "MP3", 4 );
	fs.names[0] = "media/T.mp3"; fs.sizes[0] = 1000; fs.n = 1;
	CHECK( Xbox_MusicXwav_CheckSource( &info, "media/T", 0, formats, 4, FakeProbe, &fs ) == XBOX_XWAV_SOURCE_OK );
}

int main( void )
{
	TestTables();
	TestBlockDecode();
	TestRoundTrip();
	TestHeader();
	TestStream();
	TestDiscovery();

	if( failures )
	{
		printf( "xbox_music_adpcm: %d failure(s)\n", failures );
		return 1;
	}
	printf( "xbox_music_adpcm: all passed\n" );
	return 0;
}
