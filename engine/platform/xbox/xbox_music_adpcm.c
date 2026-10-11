/*
xbox_music_adpcm.c - baked music tracks (.xwav): block decoder, container
header, and the stream that replays upstream's mp3 reader
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
Resonance3D (docs/r3d/divergences.md #171). See xbox_music_adpcm.h for
the format and the stream contract. No engine headers: the host test and
the baker compile this file as it is.
*/

#include <stdio.h>
#include <string.h>
#include "xbox_music_adpcm.h"

const uint16_t xbox_adpcm_step_table[89] =
{
	7,     8,     9,     10,    11,    12,    13,    14,
	16,    17,    19,    21,    23,    25,    28,    31,
	34,    37,    41,    45,    50,    55,    60,    66,
	73,    80,    88,    97,    107,   118,   130,   143,
	157,   173,   190,   209,   230,   253,   279,   307,
	337,   371,   408,   449,   494,   544,   598,   658,
	724,   796,   876,   963,   1060,  1166,  1282,  1411,
	1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
	3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
	7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};

const int8_t xbox_adpcm_index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* diff_mag[i][c] is the shift-add magnitude for step index i and the
   code's low three bits c; next_index[i][c] the clamped next index. A
   lookup replaces three data-dependent branches per code, which the
   Pentium III mispredicts on audio. 89 * 8 * (4 + 1) = 3560 bytes. */
static int32_t adpcm_diff_mag[89][8];
static uint8_t adpcm_next_index[89][8];
static uint32_t crc_table[256];
static int tables_ready;

void Xbox_MusicAdpcm_InitTables( void )
{
	int i, c;

	if( tables_ready )
		return;

	for( i = 0; i <= XBOX_ADPCM_MAX_INDEX; i++ )
	{
		for( c = 0; c < 8; c++ )
		{
			int32_t step = xbox_adpcm_step_table[i];
			int32_t d = step >> 3;
			int n = i + xbox_adpcm_index_table[c];

			if( c & 1 ) d += step >> 2;
			if( c & 2 ) d += step >> 1;
			if( c & 4 ) d += step;
			adpcm_diff_mag[i][c] = d;
			adpcm_next_index[i][c] = (uint8_t)( n < 0 ? 0 : n > XBOX_ADPCM_MAX_INDEX ? XBOX_ADPCM_MAX_INDEX : n );
		}
	}

	for( i = 0; i < 256; i++ )
	{
		uint32_t r = (uint32_t)i;

		for( c = 0; c < 8; c++ )
			r = ( r & 1u ) ? ( r >> 1 ) ^ 0xEDB88320u : r >> 1;
		crc_table[i] = r;
	}

	tables_ready = 1;
}

void Xbox_MusicAdpcm_StepRef( int32_t *sample, int32_t *index, unsigned code )
{
	/* xemu's adpcm_decode_block (hw/xbox/mcpx/apu/vp/adpcm.h:104-114),
	   written out: the delta from the current index, then the index
	   moves, then both clamp. */
	int32_t step = xbox_adpcm_step_table[*index];
	int32_t delta = step >> 3;
	int32_t s, i;

	if( code & 1 ) delta += step >> 2;
	if( code & 2 ) delta += step >> 1;
	if( code & 4 ) delta += step;
	if( code & 8 ) delta = -delta;

	s = *sample + delta;
	i = *index + xbox_adpcm_index_table[code & 7];
	if( i < 0 ) i = 0;
	else if( i > XBOX_ADPCM_MAX_INDEX ) i = XBOX_ADPCM_MAX_INDEX;
	if( s > 32767 ) s = 32767;
	else if( s < -32768 ) s = -32768;
	*sample = s;
	*index = i;
}

/* The table form, inlined into the block loop below. `sign` is 0 or -1,
   so (d ^ sign) - sign negates without a branch. */
#define ADPCM_STEP( sample, index, code ) do { \
	const unsigned c_ = (code); \
	const int32_t sign_ = -(int32_t)(( c_ >> 3 ) & 1u ); \
	int32_t s_ = (sample) + (( adpcm_diff_mag[(index)][c_ & 7u] ^ sign_ ) - sign_ ); \
	(index) = adpcm_next_index[(index)][c_ & 7u]; \
	if( s_ > 32767 ) s_ = 32767; \
	else if( s_ < -32768 ) s_ = -32768; \
	(sample) = s_; \
} while( 0 )

void Xbox_MusicAdpcm_Step( int32_t *sample, int32_t *index, unsigned code )
{
	int32_t s = *sample, i = *index;

	Xbox_MusicAdpcm_InitTables();
	ADPCM_STEP( s, i, code );
	*sample = s;
	*index = i;
}

int Xbox_MusicAdpcm_DecodeBlock( const uint8_t *in, int channels, int16_t *out )
{
	int ch;

	Xbox_MusicAdpcm_InitTables();

	for( ch = 0; ch < channels; ch++ )
	{
		const uint8_t *h = in + ch * 4;

		if( h[2] > XBOX_ADPCM_MAX_INDEX || h[3] != 0 )
		{
			memset( out, 0, sizeof( *out ) * XBOX_ADPCM_BLOCK_FRAMES * (size_t)channels );
			return 0;
		}
	}

	for( ch = 0; ch < channels; ch++ )
	{
		const uint8_t *h = in + ch * 4;
		const uint8_t *codes = in + channels * 4 + ch * 4;
		int16_t *o = out + ch;
		int32_t sample = (int16_t)( h[0] | ( h[1] << 8 ));
		int32_t index = h[2];
		int group, k;

		/* frame 0 is the header sample */
		o[0] = (int16_t)sample;
		o += channels;

		/* eight groups of four bytes, eight codes each; the group's bytes
		   for this channel sit every 4 * channels bytes */
		for( group = 0; group < 8; group++, codes += 4 * channels )
		{
			for( k = 0; k < 4; k++ )
			{
				const unsigned b = codes[k];

				ADPCM_STEP( sample, index, b & 15u );
				*o = (int16_t)sample;
				o += channels;

				if( group == 7 && k == 3 )
					break; /* the 64th code is not a frame */

				ADPCM_STEP( sample, index, b >> 4 );
				*o = (int16_t)sample;
				o += channels;
			}
		}
	}

	return 1;
}

uint32_t Xbox_MusicCrc32( uint32_t crc, const void *data, size_t len )
{
	const uint8_t *p = (const uint8_t *)data;

	Xbox_MusicAdpcm_InitTables();
	crc = ~crc;
	while( len-- )
		crc = crc_table[( crc ^ *p++ ) & 0xFFu] ^ ( crc >> 8 );
	return ~crc;
}

/* ---- the container ---- */

static uint32_t rd16( const uint8_t *p ) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8 ); }
static uint32_t rd32( const uint8_t *p ) { return rd16( p ) | ( rd16( p + 2 ) << 16 ); }

static uint32_t BlocksFor( uint32_t frames )
{
	return (uint32_t)(( (uint64_t)frames + XBOX_ADPCM_BLOCK_FRAMES - 1 ) / XBOX_ADPCM_BLOCK_FRAMES );
}

xbox_xwav_err_t Xbox_MusicXwav_ParseHeader( const uint8_t *buf, size_t len, uint32_t file_len, xbox_xwav_info_t *out )
{
	int have_fmt = 0, have_r3dm = 0;
	uint64_t off = 12;
	uint32_t fmt_bits = 0, fmt_align = 0;

	memset( out, 0, sizeof( *out ));

	if( len > file_len )
		len = file_len;
	if( len < 12 )
		return XBOX_XWAV_ERR_SHORT;
	if( memcmp( buf, "RIFF", 4 ) || memcmp( buf + 8, "WAVE", 4 ))
		return XBOX_XWAV_ERR_MAGIC;

	for( ;; )
	{
		const uint8_t *c;
		uint32_t size;

		if( off + 8 > len )
			return have_fmt && have_r3dm ? XBOX_XWAV_ERR_DATA : have_fmt ? XBOX_XWAV_ERR_R3DM : XBOX_XWAV_ERR_FMT;

		c = buf + off;
		size = rd32( c + 4 );

		if( !memcmp( c, "data", 4 ))
		{
			if( !have_fmt )
				return XBOX_XWAV_ERR_FMT;
			if( !have_r3dm )
				return XBOX_XWAV_ERR_R3DM;
			out->data_offset = (uint32_t)( off + 8 );
			out->data_bytes = size;
			break;
		}

		if( off + 8 + size > len )
		{
			/* a chunk the reader needs must be inside the header read */
			if( !memcmp( c, "fmt ", 4 ))
				return XBOX_XWAV_ERR_FMT;
			if( !memcmp( c, "r3dm", 4 ))
				return XBOX_XWAV_ERR_R3DM;
			return XBOX_XWAV_ERR_SHORT;
		}

		if( !memcmp( c, "fmt ", 4 ))
		{
			const uint8_t *f = c + 8;

			if( size < 16 )
				return XBOX_XWAV_ERR_FMT;
			out->tag = rd16( f );
			out->channels = rd16( f + 2 );
			out->rate = rd32( f + 4 );
			fmt_align = rd16( f + 12 );
			fmt_bits = rd16( f + 14 );

			if( out->channels < 1 || out->channels > 2 || out->rate < 8000 || out->rate > 48000 )
				return XBOX_XWAV_ERR_CODEC;

			if( out->tag == XBOX_XWAV_TAG_ADPCM )
			{
				/* cbSize 2, then wSamplesPerBlock = 64 */
				if( size < 20 || rd16( f + 16 ) < 2 || rd16( f + 18 ) != XBOX_ADPCM_BLOCK_FRAMES )
					return XBOX_XWAV_ERR_CODEC;
				if( fmt_bits != 4 || fmt_align != XBOX_ADPCM_CHANNEL_BYTES * out->channels )
					return XBOX_XWAV_ERR_CODEC;
				out->block_bytes = XBOX_ADPCM_CHANNEL_BYTES * out->channels;
			}
			else if( out->tag == XBOX_XWAV_TAG_PCM )
			{
				if( fmt_bits != 16 || fmt_align != 2 * out->channels )
					return XBOX_XWAV_ERR_CODEC;
				out->block_bytes = XBOX_ADPCM_BLOCK_FRAMES * 2 * out->channels;
			}
			else return XBOX_XWAV_ERR_CODEC;

			have_fmt = 1;
		}
		else if( !memcmp( c, "r3dm", 4 ))
		{
			const uint8_t *r = c + 8;

			if( size < XBOX_XWAV_R3DM_BYTES || rd32( r ) != XBOX_XWAV_R3DM_VERSION )
				return XBOX_XWAV_ERR_R3DM;
			out->frames = rd32( r + 4 );
			out->source_rate = rd32( r + 8 );
			out->source_frames = rd32( r + 12 );
			out->crc_source = rd32( r + 16 );
			out->crc_decoded = rd32( r + 20 );
			out->chunk_bytes = rd32( r + 24 );
			out->source_bytes = rd32( r + 28 );
			memcpy( out->source_ext, r + 32, 8 );
			out->encoder_id = rd16( r + 40 );
			out->resampler_id = rd16( r + 42 );
			out->tell_last_chunk = rd32( r + 44 );

			if( out->source_ext[7] != '\0' || out->frames == 0 || out->chunk_bytes == 0
				|| out->source_rate < 8000 || out->source_rate > 48000 )
				return XBOX_XWAV_ERR_R3DM;
			have_r3dm = 1;
		}

		/* RIFF pads a chunk with an odd size to an even length */
		off += 8 + (uint64_t)size + ( size & 1u );
	}

	if( out->frames > 0x7FFFFFFFu / 4u )
		return XBOX_XWAV_ERR_R3DM; /* frame * 2 * channels must fit an int */
	if( out->rate != out->source_rate || out->source_frames != out->frames )
		return XBOX_XWAV_ERR_R3DM; /* see the header: source-rate bakes only, for now */
	if( out->chunk_bytes % ( 2u * out->channels ) || out->tell_last_chunk > out->frames )
		return XBOX_XWAV_ERR_R3DM;

	if( out->tag == XBOX_XWAV_TAG_ADPCM )
	{
		if( (uint64_t)out->data_bytes != (uint64_t)BlocksFor( out->frames ) * out->block_bytes )
			return XBOX_XWAV_ERR_DATA;
	}
	else if( (uint64_t)out->data_bytes != (uint64_t)out->frames * 2u * out->channels )
		return XBOX_XWAV_ERR_DATA;

	if( (uint64_t)out->data_offset + out->data_bytes > file_len )
		return XBOX_XWAV_ERR_DATA;

	return XBOX_XWAV_OK;
}

const char *Xbox_MusicXwav_ErrorString( xbox_xwav_err_t err )
{
	switch( err )
	{
	case XBOX_XWAV_OK: return "ok";
	case XBOX_XWAV_ERR_SHORT: return "truncated header";
	case XBOX_XWAV_ERR_MAGIC: return "not a RIFF/WAVE file";
	case XBOX_XWAV_ERR_FMT: return "bad or missing fmt chunk";
	case XBOX_XWAV_ERR_CODEC: return "unsupported codec, channels, rate or block layout";
	case XBOX_XWAV_ERR_R3DM: return "bad or missing r3dm chunk";
	case XBOX_XWAV_ERR_DATA: return "data chunk disagrees with the frame count";
	default: return "unknown error";
	}
}

static int LowerEq( const char *a, const char *b )
{
	for( ; *a && *b; a++, b++ )
	{
		int x = (unsigned char)*a, y = (unsigned char)*b;

		if( x >= 'A' && x <= 'Z' ) x += 'a' - 'A';
		if( y >= 'A' && y <= 'Z' ) y += 'a' - 'A';
		if( x != y )
			return 0;
	}
	return *a == *b;
}

int Xbox_MusicXwav_SplitName( const char *filename, const char *const *formats, int nformats, char *base, size_t base_size )
{
	const char *dot = NULL, *p;
	int i;

	if( !base_size )
		return -1;
	snprintf( base, base_size, "%s", filename );

	/* COM_FileExtension's rule (`crtlib.c:526-539` in the reference): the
	   text after the last dot, unless a '\\', '/' or ':' follows it */
	for( p = filename; *p; p++ )
	{
		if( *p == '.' )
			dot = p;
		else if( *p == '/' || *p == '\\' || *p == ':' )
			dot = NULL;
	}
	if( !dot || !dot[1] )
		return -1;

	for( i = 0; i < nformats; i++ )
	{
		if( LowerEq( dot + 1, formats[i] ))
		{
			if( (size_t)( dot - filename ) < base_size )
				base[dot - filename] = '\0';
			return i;
		}
	}
	return -1;
}

xbox_xwav_source_t Xbox_MusicXwav_CheckSource( const xbox_xwav_info_t *info, const char *base, int requested,
	const char *const *formats, int nformats, xbox_music_probe_fn probe, void *ctx )
{
	char path[512];
	int64_t size = -1;
	int i, found = -1;

	if( requested >= 0 && requested < nformats )
	{
		snprintf( path, sizeof( path ), "%s.%s", base, formats[requested] );
		if( probe( ctx, path, &size ))
			found = requested;
	}
	else
	{
		for( i = 0; i < nformats; i++ )
		{
			snprintf( path, sizeof( path ), "%s.%s", base, formats[i] );
			if( probe( ctx, path, &size ))
			{
				found = i;
				break;
			}
		}
	}

	if( found < 0 )
		return XBOX_XWAV_SOURCE_NONE;
	if( !LowerEq( formats[found], info->source_ext ))
		return XBOX_XWAV_SOURCE_FORMAT;
	if( size != (int64_t)info->source_bytes )
		return XBOX_XWAV_SOURCE_SIZE;
	return XBOX_XWAV_SOURCE_OK;
}

uint32_t Xbox_MusicXwav_ToSourceFrames( const xbox_xwav_info_t *info, uint32_t file_frames )
{
	if( info->rate == info->source_rate || !info->rate )
		return file_frames;
	return (uint32_t)(( (uint64_t)file_frames * info->source_rate ) / info->rate );
}

uint32_t Xbox_MusicXwav_ToFileFrames( const xbox_xwav_info_t *info, uint32_t source_frames )
{
	if( info->rate == info->source_rate || !info->source_rate )
		return source_frames;
	/* rounded up, so a position saved by the mp3 build comes back out of
	   a bake at a higher rate unchanged: ToSource( ToFile( p )) == p */
	return (uint32_t)(( (uint64_t)source_frames * info->rate + info->source_rate - 1 ) / info->source_rate );
}

/* ---- the stream ---- */

int Xbox_MusicStream_Init( xbox_music_stream_t *s, const xbox_xwav_info_t *info, const xbox_music_io_t *io,
	uint8_t *chunk, uint32_t chunk_capacity, uint8_t *readahead )
{
	memset( s, 0, sizeof( *s ));
	Xbox_MusicAdpcm_InitTables();

	s->info = *info;
	s->io = *io;
	s->chunk = chunk;
	s->ra = readahead;
	s->frame_bytes = 2u * info->channels;

	if( info->chunk_bytes == 0 || info->chunk_bytes > chunk_capacity || info->chunk_bytes % s->frame_bytes )
		return 0;
	if( info->block_bytes == 0 || info->block_bytes > XBOX_MUSIC_READAHEAD_BYTES )
		return 0;

	s->blocks = (uint32_t)(( (uint64_t)info->data_bytes + info->block_bytes - 1 ) / info->block_bytes );
	s->ra_capacity = XBOX_MUSIC_READAHEAD_BYTES / info->block_bytes;
	return 1;
}

/* The block's bytes, refilling the read-ahead from it when it is not
   there; NULL when the file cannot supply it. */
static const uint8_t *FetchBlock( xbox_music_stream_t *s, uint32_t block )
{
	uint32_t want, bytes, got, first_byte;

	if( block >= s->ra_first && block < s->ra_first + s->ra_count )
		return s->ra + ( block - s->ra_first ) * s->info.block_bytes;

	if( block >= s->blocks || s->io_failed )
		return NULL;

	want = s->blocks - block;
	if( want > s->ra_capacity )
		want = s->ra_capacity;
	first_byte = block * s->info.block_bytes;
	bytes = want * s->info.block_bytes;
	if( bytes > s->info.data_bytes - first_byte )
		bytes = s->info.data_bytes - first_byte; /* a PCM file's last block is short */

	s->ra_first = block;
	s->ra_count = 0;
	s->io_reads++;

	if( s->io.seek( s->io.ctx, s->info.data_offset + first_byte ) != 0 )
	{
		s->io_failed = 1;
		return NULL;
	}

	got = (uint32_t)s->io.read( s->io.ctx, s->ra, (int)bytes );
	if( got > bytes )
		got = 0;
	if( got < bytes )
	{
		/* keep the whole blocks that did arrive; the track ends after them */
		s->io_failed = 1;
		s->ra_count = got / s->info.block_bytes;
	}
	else s->ra_count = want;

	return s->ra_count ? s->ra : NULL;
}

uint32_t Xbox_MusicStream_DecodeFrames( xbox_music_stream_t *s, uint32_t first, uint32_t count, int16_t *out )
{
	const uint32_t ch = s->info.channels;
	uint32_t done = 0;

	if( first >= s->info.frames )
		return 0;
	if( count > s->info.frames - first )
		count = s->info.frames - first;

	while( done < count )
	{
		const uint32_t f = first + done;
		const uint32_t off = f % XBOX_ADPCM_BLOCK_FRAMES;
		const uint8_t *blk = FetchBlock( s, f / XBOX_ADPCM_BLOCK_FRAMES );
		uint32_t n = XBOX_ADPCM_BLOCK_FRAMES - off;

		if( !blk )
			break;
		if( n > count - done )
			n = count - done;

		if( s->info.tag == XBOX_XWAV_TAG_PCM )
		{
			/* little-endian int16 in the file and on both hosts */
			memcpy( out + done * ch, blk + off * s->frame_bytes, n * s->frame_bytes );
		}
		else if( off == 0 && n == XBOX_ADPCM_BLOCK_FRAMES )
		{
			if( !Xbox_MusicAdpcm_DecodeBlock( blk, (int)ch, out + done * ch ))
				s->bad_blocks++;
		}
		else
		{
			if( !Xbox_MusicAdpcm_DecodeBlock( blk, (int)ch, s->carry ))
				s->bad_blocks++;
			memcpy( out + done * ch, s->carry + off * ch, n * ch * sizeof( int16_t ));
		}
		done += n;
	}

	return done;
}

/* libmpg's read_mpeg_stream as Stream_ReadMPG sees it: fills the chunk
   and reports OK, or reports failure with however much it produced -
   mpg123_decode's `*done = mdone` happens on every exit
   (`mpg123.c:677-679` in the reference). */
static int PullChunk( xbox_music_stream_t *s, int *outsize )
{
	const uint32_t chunk_frames = s->info.chunk_bytes / s->frame_bytes;
	uint32_t got = Xbox_MusicStream_DecodeFrames( s, s->next_frame, chunk_frames, (int16_t *)s->chunk );

	s->next_frame += got;
	*outsize = (int)( got * s->frame_bytes );
	if( got != chunk_frames )
		s->ended = 1;
	return got == chunk_frames;
}

int Xbox_MusicStream_Read( xbox_music_stream_t *s, int need, void *buffer )
{
	/* Stream_ReadMPG (`snd_mp3.c:361-398` in the reference), line for
	   line, with PullChunk in place of read_mpeg_stream. */
	int bytesWritten = 0;

	while( 1 )
	{
		int outsize;

		if( !s->buffsize )
		{
			if( !PullChunk( s, &s->pos ))
				break; /* there was end of the stream */
		}

		/* check remaining size */
		if( bytesWritten + s->pos > need )
			outsize = ( need - bytesWritten );
		else outsize = s->pos;

		/* copy raw sample to output buffer */
		memcpy( (uint8_t *)buffer + bytesWritten, s->chunk + s->buffsize, (size_t)outsize );
		bytesWritten += outsize;
		s->pos -= outsize;
		s->buffsize += outsize;

		/* continue from this sample on a next call */
		if( bytesWritten >= need )
			return bytesWritten;

		s->buffsize = 0; /* no bytes remaining */
	}

	return 0;
}

int Xbox_MusicStream_SetPos( xbox_music_stream_t *s, int newpos )
{
	/* Stream_SetPosMPG: seek, then flush the chunk (`snd_mp3.c:407-418`).
	   mpg123_seek clamps a negative target to 0 (`mpg123.c:890`). A target
	   past the end succeeds there too - the reader's seek_frame stops at
	   the last frame and returns OK (`reader.c:569-576`), and mpg123_tell
	   reports the target (`mpg123.c:774-779`) - and the next read returns
	   0; here DecodeFrames returns nothing past `frames`, the same. */
	uint32_t f = Xbox_MusicXwav_ToFileFrames( &s->info, newpos < 0 ? 0u : (uint32_t)newpos );

	s->next_frame = f;
	s->buffsize = 0;
	s->seeked = 1;
	s->ended = 0;
	return 1;
}

int Xbox_MusicStream_GetPos( const xbox_music_stream_t *s )
{
	/* mpg123_tell after the read that filled the last whole chunk counts
	   part of the final mp3 frame's gapless trim (the baker records what
	   it said there, on a fresh open); everywhere else it is the frames
	   the decoder handed out */
	if( s->info.tell_last_chunk && !s->seeked && !s->ended )
	{
		const uint32_t chunk_frames = s->info.chunk_bytes / s->frame_bytes;
		const uint32_t last_whole = ( s->info.frames / chunk_frames ) * chunk_frames;

		if( last_whole && s->next_frame == last_whole )
			return (int)Xbox_MusicXwav_ToSourceFrames( &s->info, s->info.tell_last_chunk );
	}
	return (int)Xbox_MusicXwav_ToSourceFrames( &s->info, s->next_frame );
}
