/*
xbox_save_stream.c - the bytes of a savegame's .sav, produced a chunk at a time
Resonance3D (docs/r3d/divergences.md #153); see xbox_save_stream.h
*/
#include <string.h>
#include "xbox_save_stream.h"

void xbox_save_stream_init( xbox_save_stream_t *s, const xbox_save_stream_io_t *io,
	const void *head, uint32_t head_len, uint32_t count,
	void *chunk, uint32_t cap, uint32_t align )
{
	memset( s, 0, sizeof( *s ));
	s->io = *io;
	s->head = head;
	s->head_len = head_len;
	s->count = count;
	s->record_pos = XBOX_SAVE_STREAM_RECORD_BYTES;
	s->chunk = chunk;
	s->cap = cap;
	s->align = align ? align : 1u;
}

void xbox_save_stream_close( xbox_save_stream_t *s )
{
	if( s->in_open )
	{
		if( s->in )
			s->io.close( s->io.ctx, s->in );
		s->in = NULL;
		s->in_open = 0;
		s->in_left = 0;
	}
}

static uint32_t min_u32( uint32_t a, uint32_t b )
{
	return a < b ? a : b;
}

uint32_t xbox_save_stream_fill( xbox_save_stream_t *s, uint32_t max_opens )
{
	s->opens = 0;

	while( s->len < s->cap && !s->done && !s->failed )
	{
		uint32_t n;

		/* 1. the header, tokens and globals (SaveGameSlot's seven writes) */
		if( s->head_pos < s->head_len )
		{
			n = min_u32( s->head_len - s->head_pos, s->cap - s->len );
			memcpy( s->chunk + s->len, s->head + s->head_pos, n );
			s->head_pos += n;
			s->len += n;
			continue;
		}

		/* 2. the open file's name and length (DirectoryCopy's two writes) */
		if( s->record_pos < XBOX_SAVE_STREAM_RECORD_BYTES )
		{
			n = min_u32( XBOX_SAVE_STREAM_RECORD_BYTES - s->record_pos, s->cap - s->len );
			memcpy( s->chunk + s->len, s->record + s->record_pos, n );
			s->record_pos += n;
			s->len += n;
			continue;
		}

		/* 3. its bytes (FS_FileCopy), read on the next aligned offset */
		if( s->in_left > 0 )
		{
			const uint32_t off = ( s->len + s->align - 1u ) & ~( s->align - 1u );
			int32_t got;

			if( off >= s->cap )
				break; /* the chunk ends here: the next fill reads at offset 0 */

			n = min_u32( (uint32_t)s->in_left, s->cap - off );
			got = s->io.read( s->io.ctx, s->in, s->chunk + off, (int32_t)n );
			if( got != (int32_t)n )
			{
				/* FS_FileCopy stops on a short read (io.c); the .sav would
				   hold fewer bytes than the length written before them */
				s->failed = 1;
				break;
			}
			if( off != s->len )
				memmove( s->chunk + s->len, s->chunk + off, n );
			s->len += n;
			s->in_left -= (int32_t)n;
			continue;
		}

		/* 4. the file is copied: close it (DirectoryCopy's FS_Close) */
		xbox_save_stream_close( s );

		/* 5. the next file */
		if( s->next < s->count )
		{
			int32_t length = 0;

			if( max_opens && s->opens >= max_opens )
				break;

			memset( s->record, 0, XBOX_SAVE_STREAM_NAME_BYTES );
			s->in = s->io.open( s->io.ctx, s->next, (char *)s->record, XBOX_SAVE_STREAM_NAME_BYTES, &length );
			s->record[XBOX_SAVE_STREAM_NAME_BYTES - 1u] = 0;
			if( !s->in )
				length = 0; /* FS_FileLength( NULL ) */
			/* written as given; FS_FileCopy copies nothing for a size <= 0 */
			memcpy( s->record + XBOX_SAVE_STREAM_NAME_BYTES, &length, 4 );
			s->record_pos = 0;
			s->in_open = 1;
			s->in_left = length > 0 ? length : 0;
			s->next++;
			s->opens++;
			continue;
		}

		s->done = 1;
	}

	return s->len;
}

void xbox_save_stream_consumed( xbox_save_stream_t *s )
{
	s->len = 0;
}
