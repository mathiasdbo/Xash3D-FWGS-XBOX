/*
xbox_snd_adpcm.c - resident sounds kept as Xbox ADPCM
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
Resonance3D (docs/r3d/divergences.md #200). The encoder runs at load time
on the console, so it is the music baker's (#171) without its trellis:
per block, greedy coding (each code the quotient's magnitude or the one
above, whichever the decoder lands closer to) from the step index the
previous block ended on, as a streaming IMA encoder carries it; the first
block tries all 89 and keeps the lowest squared error. Every candidate is
scored through the music decoder's own step, clamps included, so the
error measured is what the APU and Xbox_SndAdpcm_Decode play.
*/
#include <string.h>
#include "xbox_snd_adpcm.h"

#define BLOCK_FRAMES ((uint32_t)XBOX_ADPCM_BLOCK_FRAMES)
/* A block codes from the step index the previous block ended on. Where
   that leaves its error above 1/64 of its energy (an SNR under 18 dB: an
   attack after a quiet block, a jump the step cannot follow), it also
   tries every SEARCH_GRID-th index and those around the best of them.
   Host test (8-bit): decaying clicks 12.5 -> 25.3 dB, a square at the
   rails 9.8 -> 15.7, noise 16.0 -> 16.9, the encode 30 -> 34 ms a minute;
   searching every block gains under 1 dB more for 4.7 times the time */
#ifndef SEARCH_SNR_SHIFT
#define SEARCH_SNR_SHIFT 6
#endif
#ifndef SEARCH_GRID
#define SEARCH_GRID  4
#endif

size_t Xbox_SndAdpcm_Bytes( uint32_t frames )
{
	return (size_t)(( frames + BLOCK_FRAMES - 1 ) / BLOCK_FRAMES ) * XBOX_ADPCM_CHANNEL_BYTES;
}

/* The decoder's step as tables (the music decoder's form, built here from
   the same step and index tables; the host test holds the result to
   xemu's shift-add steps): the magnitude for (index, the code's low
   three bits), and the clamped next index. The encoder evaluates two
   codes a frame for five indices a block; a function call each was
   most of its time. */
static int32_t diff_mag[XBOX_ADPCM_MAX_INDEX + 1][8];
static uint8_t next_index[XBOX_ADPCM_MAX_INDEX + 1][8];
static int tables_ready;

static void InitTables( void )
{
	int i, c;

	if( tables_ready )
		return;
	for( i = 0; i <= XBOX_ADPCM_MAX_INDEX; i++ )
	{
		for( c = 0; c < 8; c++ )
		{
			const int32_t step = xbox_adpcm_step_table[i];
			int32_t d = step >> 3;
			const int n = i + xbox_adpcm_index_table[c];

			if( c & 1 ) d += step >> 2;
			if( c & 2 ) d += step >> 1;
			if( c & 4 ) d += step;
			diff_mag[i][c] = d;
			next_index[i][c] = (uint8_t)( n < 0 ? 0 : n > XBOX_ADPCM_MAX_INDEX ? XBOX_ADPCM_MAX_INDEX : n );
		}
	}
	tables_ready = 1;
}

static int32_t Clamp16( int32_t s )
{
	return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}

/* |diff| * 4 / step, about, to 7: the IMA quantizer's three compares (a
   division a frame was a third of the encoder's time) */
static unsigned Quotient( int32_t diff, int32_t step )
{
	unsigned mag = 0;

	if( diff >= step )
	{
		mag = 4;
		diff -= step;
	}
	if( diff >= ( step >> 1 ))
	{
		mag |= 2;
		diff -= step >> 1;
	}
	if( diff >= ( step >> 2 ))
		mag |= 1;
	return mag;
}

/* The code for `target` from (sample, index): the quotient's
   magnitude or the one above, whichever the decoder lands closer to;
   *sample and *index move to the decoder's state after it */
static unsigned BestCode( int32_t *sample, int32_t *index, int32_t target )
{
	const int32_t step = xbox_adpcm_step_table[*index];
	const int32_t *mag_of = diff_mag[*index];
	const int32_t diff = target - *sample;
	int32_t s0, e0;
	unsigned mag;

	if( diff >= 0 )
	{
		mag = Quotient( diff, step );
		s0 = Clamp16( *sample + mag_of[mag] );
		e0 = target - s0;
		if( mag < 7 )
		{
			const int32_t s1 = Clamp16( *sample + mag_of[mag + 1] ), e1 = target - s1;

			if( (int64_t)e1 * e1 < (int64_t)e0 * e0 )
			{
				mag++;
				s0 = s1;
			}
		}
		*sample = s0;
		*index = next_index[*index][mag];
		return mag;
	}
	mag = Quotient( -diff, step );
	s0 = Clamp16( *sample - mag_of[mag] );
	e0 = target - s0;
	if( mag < 7 )
	{
		const int32_t s1 = Clamp16( *sample - mag_of[mag + 1] ), e1 = target - s1;

		if( (int64_t)e1 * e1 < (int64_t)e0 * e0 )
		{
			mag++;
			s0 = s1;
		}
	}
	*sample = s0;
	*index = next_index[*index][mag];
	return 8u | mag;
}

/* The 64 codes of one block from `index` (the last aimed at t[64], the
   next block's first frame, and not played); the squared error of the 63
   played frames, stopping early past `limit`. *end_index: where the
   decoder ends. */
static uint64_t EncodeBlock( const int32_t *t, int32_t index, uint64_t limit, uint8_t *codes, int32_t *end_index )
{
	int32_t sample = t[0], idx = index;
	uint64_t err = 0;
	uint32_t j;

	for( j = 0; j < BLOCK_FRAMES; j++ )
	{
		const unsigned code = BestCode( &sample, &idx, t[j + 1] );

		if( codes )
			codes[j] = (uint8_t)code;
		if( j + 1 < BLOCK_FRAMES )
		{
			const int32_t e = t[j + 1] - sample;

			err += (uint64_t)( (int64_t)e * e );
			if( err > limit )
				return err;
		}
	}
	if( end_index )
		*end_index = idx;
	return err;
}

void Xbox_SndAdpcm_Encode( const void *pcm, int width, uint32_t frames, uint8_t *out )
{
	const uint32_t blocks = ( frames + BLOCK_FRAMES - 1 ) / BLOCK_FRAMES;
	int32_t hint = -1;
	uint32_t b, j;

	InitTables();

	for( b = 0; b < blocks; b++ )
	{
		/* the block's 64 frames and the next block's first, read before
		   anything is written (`out` may be `pcm`) */
		int32_t t[BLOCK_FRAMES + 1];
		uint8_t codes[BLOCK_FRAMES], *dst = out + (size_t)b * XBOX_ADPCM_CHANNEL_BYTES;
		uint64_t best_err = UINT64_MAX;
		int32_t best = 0, lo, hi, i, end = 0, best_end = 0, pass;
		uint64_t sig;

		for( j = 0; j <= BLOCK_FRAMES; j++ )
		{
			const uint32_t f = b * BLOCK_FRAMES + j;

			if( f >= frames )
				t[j] = 0;
			else if( width == 1 )
				t[j] = ( (int32_t)( (const uint8_t *)pcm )[f] - 128 ) * 256;
			else
				t[j] = ( (const int16_t *)pcm )[f];
		}

		/* the hint first; ties go to it. The first block has none and
		   searches the grid */
		if( hint >= 0 )
		{
			best_err = EncodeBlock( t, hint, UINT64_MAX, NULL, &best_end );
			best = hint;
		}
		for( j = 1, sig = 0; j < BLOCK_FRAMES; j++ )
			sig += (uint64_t)( (int64_t)t[j] * t[j] );
		for( pass = hint >= 0 && best_err <= ( sig >> SEARCH_SNR_SHIFT ) ? 2 : 0; pass < 2; pass++ )
		{
			const int32_t step = pass == 0 ? SEARCH_GRID : 1;

			if( pass == 0 )
			{
				lo = 0;
				hi = XBOX_ADPCM_MAX_INDEX;
			}
			else
			{
				lo = best - SEARCH_GRID + 1;
				hi = best + SEARCH_GRID - 1;
				if( lo < 0 )
					lo = 0;
				if( hi > XBOX_ADPCM_MAX_INDEX )
					hi = XBOX_ADPCM_MAX_INDEX;
			}
			for( i = lo; i <= hi; i += step )
			{
				uint64_t e;

				if( i == hint || ( pass == 1 && i % SEARCH_GRID == 0 ))
					continue;
				e = EncodeBlock( t, i, best_err, NULL, &end );
				if( e < best_err )
				{
					best_err = e;
					best = i;
					best_end = end;
				}
			}
		}
		EncodeBlock( t, best, UINT64_MAX, codes, &best_end );
		hint = best_end;

		/* header: the first frame, the index, zero; then the codes, low nibble first */
		dst[0] = (uint8_t)( (uint16_t)t[0] & 0xFFu );
		dst[1] = (uint8_t)( (uint16_t)t[0] >> 8 );
		dst[2] = (uint8_t)best;
		dst[3] = 0;
		for( j = 0; j < BLOCK_FRAMES; j += 2 )
			dst[4 + j / 2] = (uint8_t)( codes[j] | ( codes[j + 1] << 4 ));
	}
}

void Xbox_SndAdpcm_Decode( const uint8_t *blocks, uint32_t first, uint32_t count, int16_t *out )
{
	int16_t tmp[XBOX_ADPCM_BLOCK_FRAMES];

	while( count > 0 )
	{
		const uint32_t b = first / BLOCK_FRAMES, at = first % BLOCK_FRAMES;
		const uint8_t *in = blocks + (size_t)b * XBOX_ADPCM_CHANNEL_BYTES;
		uint32_t n = BLOCK_FRAMES - at;

		if( n > count )
			n = count;
		if( at == 0 && n == BLOCK_FRAMES )
			Xbox_MusicAdpcm_DecodeBlock( in, 1, out );
		else
		{
			Xbox_MusicAdpcm_DecodeBlock( in, 1, tmp );
			memcpy( out, tmp + at, n * sizeof( *out ));
		}
		out += n;
		first += n;
		count -= n;
	}
}
