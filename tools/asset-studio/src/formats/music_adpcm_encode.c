/*
music_adpcm_encode.c - the Xbox ADPCM encoder for baked music
Resonance3D (docs/r3d/divergences.md #171)

Each block's header holds the exact source sample, so error never carries
from one block to the next. Per channel and block: every one of the 89
step indices is tried with greedy coding (each code the closest of the
quotient's magnitude, the one above and the smallest step the other way;
an index is dropped once its running error passes the fourth best), and
the four best start a trellis search (TrellisChannel) that can trade
error now for a better index later; the lower total error wins. Every
candidate is scored through the engine's own step, clamps included.
*/
#include <string.h>
#include "music_adpcm_encode.h"
#include "xbox_music_adpcm.h"

#define BLOCK_FRAMES ((int)XBOX_ADPCM_BLOCK_FRAMES)

/* The code for `target` from (sample, index): the truncated quotient
   |diff| * 4 / step and the next magnitude up, whichever the decoder
   lands closer to. */
static unsigned BestCode( int32_t sample, int32_t index, int32_t target, int32_t *out_sample, int32_t *out_index )
{
	const int32_t step = xbox_adpcm_step_table[index];
	int32_t diff = target - sample;
	unsigned sign = 0, mag, best = 0;
	int64_t best_err = -1;
	int k;

	if( diff < 0 )
	{
		sign = 8;
		diff = -diff;
	}
	mag = (unsigned)(( (int64_t)diff * 4 ) / step );
	if( mag > 7 )
		mag = 7;

	/* the quotient's magnitude, the one above, and the smallest step the
	   other way: near a rail at a high index that is the only code the
	   clamp holds still (a held -32768 at index 88: code 8 stays,
	   code 0 jumps 4095) */
	for( k = 0; k < 3; k++ )
	{
		const unsigned code = k == 2 ? ( sign ^ 8u ) : sign | ( mag + (unsigned)k );
		int32_t s = sample, i = index;
		int64_t e;

		if( k == 1 && mag + 1 > 7 )
			continue;
		Xbox_MusicAdpcm_Step( &s, &i, code );
		e = (int64_t)( target - s ) * ( target - s );
		if( best_err < 0 || e < best_err )
		{
			best_err = e;
			best = code;
			*out_sample = s;
			*out_index = i;
		}
	}

	return best;
}

/* One channel's 64 codes for step index `index`; stops early once the
   squared error passes `limit` (returns a value > limit then). */
static uint64_t EncodeChannel( const int16_t *pcm, int channels, int index, uint64_t limit, uint8_t *codes )
{
	int32_t sample = pcm[0], idx = index;
	uint64_t err = 0;
	int j;

	for( j = 0; j < BLOCK_FRAMES; j++ )
	{
		/* code j decodes to frame j + 1; frame 64 is the next block's
		   first, aimed at but not played */
		const int32_t target = pcm[( j + 1 ) * channels];
		int32_t s, i;
		unsigned code = BestCode( sample, idx, target, &s, &i );

		if( codes )
			codes[j] = (uint8_t)code;
		sample = s;
		idx = i;
		if( j + 1 < BLOCK_FRAMES )
		{
			err += (uint64_t)(( (int64_t)target - s ) * ( (int64_t)target - s ));
			if( err > limit )
				return err;
		}
	}

	return err;
}

/* The trellis: a beam of the TRELLIS_WIDTH best partial encodes (by
   squared error so far), each extended by a handful of codes per frame -
   the quotient's magnitude and its neighbours, the largest step (keeps
   the index up ahead of a jump), and the smallest step the other way. Two
   paths in the same (sample, index) state decode the rest identically,
   so only the cheaper is kept. Greedy coding cannot trade a little error
   now for a better index later; this can, which is where IMA loses most
   on loud material (ffmpeg's adpcm encoder has the same idea as
   -trellis). */
#define TRELLIS_WIDTH  8
#define TRELLIS_STARTS 4

typedef struct tnode_s
{
	int32_t sample, index;
	uint64_t err;
	uint8_t parent, code;
} tnode_t;

static void TrellisInsert( tnode_t *beam, int *count, const tnode_t *n )
{
	int k, at;

	for( k = 0; k < *count; k++ )
	{
		if( beam[k].sample == n->sample && beam[k].index == n->index )
		{
			if( beam[k].err <= n->err )
				return;
			/* the same state, cheaper: take its place and move it up */
			for( at = k; at > 0 && beam[at - 1].err > n->err; at-- )
				beam[at] = beam[at - 1];
			beam[at] = *n;
			return;
		}
	}
	if( *count == TRELLIS_WIDTH )
	{
		if( beam[TRELLIS_WIDTH - 1].err <= n->err )
			return;
		( *count )--;
	}
	for( at = *count; at > 0 && beam[at - 1].err > n->err; at-- )
		beam[at] = beam[at - 1];
	beam[at] = *n;
	( *count )++;
}

/* Encodes one channel's 63 played codes with the beam, starting from each
   of `starts` (step indices); returns the squared error and writes the 64
   codes (the last aimed at frame 64) and the chosen start index. */
static uint64_t TrellisChannel( const int16_t *pcm, int channels, const int *starts, int nstarts, int *out_index, uint8_t *codes )
{
	static const int played = BLOCK_FRAMES - 1;
	tnode_t beam[2][TRELLIS_WIDTH];
	uint8_t path_code[BLOCK_FRAMES - 1][TRELLIS_WIDTH];
	uint8_t path_parent[BLOCK_FRAMES - 1][TRELLIS_WIDTH];
	int count = 0, cur = 0, j, k, n;
	int32_t s, i;

	for( k = 0; k < nstarts; k++ )
	{
		beam[0][k].sample = pcm[0];
		beam[0][k].index = starts[k];
		beam[0][k].err = 0;
		beam[0][k].parent = (uint8_t)k;
		beam[0][k].code = 0;
	}
	count = nstarts;

	for( j = 0; j < played; j++ )
	{
		const int32_t target = pcm[( j + 1 ) * channels];
		tnode_t *from = beam[cur], *to = beam[cur ^ 1];
		int next = 0;

		for( n = 0; n < count; n++ )
		{
			const int32_t step = xbox_adpcm_step_table[from[n].index];
			int32_t diff = target - from[n].sample;
			unsigned sign = 0, mag, cand[5];
			int ncand = 0, c;

			if( diff < 0 )
			{
				sign = 8;
				diff = -diff;
			}
			mag = (unsigned)(( (int64_t)diff * 4 ) / step );
			if( mag > 7 )
				mag = 7;
			cand[ncand++] = sign | mag;
			if( mag > 0 ) cand[ncand++] = sign | ( mag - 1 );
			if( mag < 7 ) cand[ncand++] = sign | ( mag + 1 );
			if( mag < 6 ) cand[ncand++] = sign | 7u;
			cand[ncand++] = sign ^ 8u;

			for( c = 0; c < ncand; c++ )
			{
				tnode_t t;

				s = from[n].sample;
				i = from[n].index;
				Xbox_MusicAdpcm_Step( &s, &i, cand[c] );
				t.sample = s;
				t.index = i;
				t.err = from[n].err + (uint64_t)(( (int64_t)target - s ) * ( (int64_t)target - s ));
				t.parent = (uint8_t)n;
				t.code = (uint8_t)cand[c];
				TrellisInsert( to, &next, &t );
			}
		}

		for( k = 0; k < next; k++ )
		{
			path_code[j][k] = to[k].code;
			path_parent[j][k] = to[k].parent;
		}
		count = next;
		cur ^= 1;
	}

	/* the best path, back to its start */
	k = 0;
	for( j = played - 1; j >= 0; j-- )
	{
		codes[j] = path_code[j][k];
		k = path_parent[j][k];
	}
	*out_index = starts[k];

	/* the 64th code, aimed at the next block's first frame */
	codes[played] = (uint8_t)BestCode( beam[cur][0].sample, beam[cur][0].index, pcm[BLOCK_FRAMES * channels], &s, &i );
	return beam[cur][0].err;
}

uint64_t Music_AdpcmEncodeBlock( const int16_t *pcm, int channels, int *index_hint, uint8_t *out )
{
	uint64_t total = 0;
	int c;

	Xbox_MusicAdpcm_InitTables();
	memset( out, 0, XBOX_ADPCM_CHANNEL_BYTES * (size_t)channels );

	for( c = 0; c < channels; c++ )
	{
		const int16_t *p = pcm + c;
		uint64_t best_err = UINT64_MAX, top_err[TRELLIS_STARTS], trellis_err;
		int best_index = XBOX_ADPCM_MAX_INDEX + 1, i, d, j, ntop = 0, trellis_index;
		int top_index[TRELLIS_STARTS];
		uint8_t codes[BLOCK_FRAMES], trellis_codes[BLOCK_FRAMES];

		/* the hint first, then outwards from it, so the early exit bites
		   from the start */
		for( d = 0; d <= XBOX_ADPCM_MAX_INDEX; d++ )
		{
			for( j = 0; j < ( d ? 2 : 1 ); j++ )
			{
				uint64_t e;

				i = j ? index_hint[c] - d : index_hint[c] + d;
				if( i < 0 || i > XBOX_ADPCM_MAX_INDEX )
					continue;
				/* keep the TRELLIS_STARTS best; the early exit is against
				   the worst of them once there are that many */
				e = EncodeChannel( p, channels, i, ntop == TRELLIS_STARTS ? top_err[ntop - 1] : UINT64_MAX, NULL );
				/* ties go to the lower index, so the choice does not
				   depend on the search order the hint sets (and a bake is
				   the same on any number of threads) */
				if( ntop < TRELLIS_STARTS || e < top_err[ntop - 1] || ( e == top_err[ntop - 1] && i < top_index[ntop - 1] ))
				{
					int at;

					if( ntop < TRELLIS_STARTS )
						ntop++;
					for( at = ntop - 1; at > 0 && ( top_err[at - 1] > e || ( top_err[at - 1] == e && top_index[at - 1] > i )); at-- )
					{
						top_err[at] = top_err[at - 1];
						top_index[at] = top_index[at - 1];
					}
					top_err[at] = e;
					top_index[at] = i;
				}
			}
		}
		best_err = top_err[0];
		best_index = top_index[0];
		EncodeChannel( p, channels, best_index, UINT64_MAX, codes );

		/* the beam from the best starts; greedy stays when it is better
		   (the beam can prune the greedy path) */
		trellis_err = TrellisChannel( p, channels, top_index, ntop, &trellis_index, trellis_codes );
		if( trellis_err < best_err )
		{
			best_err = trellis_err;
			best_index = trellis_index;
			memcpy( codes, trellis_codes, sizeof( codes ));
		}
		index_hint[c] = best_index;
		total += best_err;

		/* header: sample, index, zero */
		out[c * 4 + 0] = (uint8_t)( (uint16_t)p[0] & 0xFFu );
		out[c * 4 + 1] = (uint8_t)( (uint16_t)p[0] >> 8 );
		out[c * 4 + 2] = (uint8_t)best_index;
		out[c * 4 + 3] = 0;

		/* codes: eight groups of four bytes per channel, channel 0's group
		   first; low nibble first */
		for( j = 0; j < BLOCK_FRAMES; j += 2 )
		{
			const int byte = j / 2;
			uint8_t *dst = out + channels * 4 + ( byte / 4 ) * 4 * channels + c * 4 + ( byte % 4 );

			*dst = (uint8_t)( codes[j] | ( codes[j + 1] << 4 ));
		}
	}

	return total;
}
