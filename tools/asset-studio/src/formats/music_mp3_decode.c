/*
music_mp3_decode.c - a retail music track through the engine's own libmpg
Resonance3D (docs/r3d/divergences.md #171). See music_mp3_decode.h.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xash3d_types.h"
#include "libmpg.h"
#include "music_mp3_decode.h"

/* FS_ReadMpg / FS_SeekMpg (`snd_mp3.c:291-299` in the reference) over
   stdio: the seek returns the new position, or -1. */
static mpg_ssize_t ReadCb( void *file, void *buf, size_t count )
{
	return (mpg_ssize_t)fread( buf, 1, count, (FILE *)file );
}

static fs_offset_t SeekCb( void *file, fs_offset_t offset, int whence )
{
	if( fseek( (FILE *)file, (long)offset, whence ) != 0 )
		return -1;
	return (fs_offset_t)ftell( (FILE *)file );
}

static void SetErr( char *err, size_t size, const char *what, const char *path )
{
	if( err && size )
		snprintf( err, size, "%s: %s", path, what );
}

void Music_Mp3Free( music_mp3_t *m )
{
	free( m->pcm );
	m->pcm = NULL;
}

/* Seeks the open decoder to `target` and reads one chunk; scores it
   against the full decode. */
static void SeekProbe( void *mpeg, music_mp3_t *m, uint32_t target, byte *chunk )
{
	const uint32_t fb = 2u * (uint32_t)m->channels;
	size_t got = 0;
	uint32_t got_frames;
	int32_t off;

	m->seek_probes++;
	if( set_stream_pos( mpeg, (int)target ) == -1 )
	{
		m->seek_worst_offset = INT32_MAX;
		return;
	}
	if( (uint32_t)get_stream_pos( mpeg ) == target )
		m->seek_tell_exact++;

	read_mpeg_stream( mpeg, chunk, &got );
	got_frames = (uint32_t)( got / fb );
	if( got_frames == 0 )
	{
		if( target >= m->frames )
			m->seek_exact++; /* nothing past the end, as expected */
		return;
	}

	/* where in the full decode does this chunk sit? */
	for( off = 0; off <= 4608; off = off <= 0 ? -off + 1 : -off )
	{
		int64_t at = (int64_t)target + off;

		if( at < 0 || at + got_frames > m->frames )
			continue;
		if( !memcmp( chunk, m->pcm + at * m->channels, (size_t)got_frames * fb ))
		{
			if( off == 0 )
				m->seek_exact++;
			else if( abs( off ) > abs( m->seek_worst_offset ))
				m->seek_worst_offset = off;
			return;
		}
	}
	m->seek_worst_offset = INT32_MAX; /* no exact match anywhere near */
}

int Music_Mp3Decode( const char *path, music_mp3_t *out, char *err, size_t err_size )
{
	FILE *f;
	void *mpeg;
	int ret = 0, k;
	wavinfo_t sc;
	byte *chunk = NULL;
	size_t cap = 0, used = 0;
	uint32_t fb;
	int last_mismatched = 0;

	memset( out, 0, sizeof( *out ));
	f = fopen( path, "rb" );
	if( !f )
	{
		SetErr( err, err_size, "cannot open", path );
		return 0;
	}

	/* Stream_OpenMPG's sequence */
	if(( mpeg = create_decoder( &ret )) == NULL )
	{
		fclose( f );
		SetErr( err, err_size, "create_decoder failed", path );
		return 0;
	}
	if( ret )
	{
		close_decoder( mpeg );
		fclose( f );
		SetErr( err, err_size, "create_decoder reported a parameter error (the engine only logs it)", path );
		return 0;
	}
	memset( &sc, 0, sizeof( sc ));
	if( !open_mpeg_stream( mpeg, f, ReadCb, SeekCb, &sc ) || sc.channels < 1 || sc.channels > 2 )
	{
		close_decoder( mpeg );
		fclose( f );
		SetErr( err, err_size, "open_mpeg_stream failed", path );
		return 0;
	}

	out->rate = sc.rate;
	out->channels = sc.channels;
	out->chunk_bytes = OUTBUF_SIZE;
	fb = 2u * (uint32_t)sc.channels;
	chunk = (byte *)malloc( OUTBUF_SIZE );

	out->open_tell = get_stream_pos( mpeg );

	/* Stream_ReadMPG's reads, all of them, the failing one's bytes
	   included */
	for( ;; )
	{
		size_t got = 0;
		int r = read_mpeg_stream( mpeg, chunk, &got );

		if( got % fb )
		{
			SetErr( err, err_size, "libmpg returned a partial frame", path );
			goto fail;
		}
		if( used + got > cap )
		{
			size_t ncap = cap ? cap * 2 : (size_t)16 << 20;
			byte *n;

			while( ncap < used + got )
				ncap *= 2;
			n = (byte *)realloc( out->pcm, ncap );
			if( !n )
			{
				SetErr( err, err_size, "out of memory", path );
				goto fail;
			}
			out->pcm = (int16_t *)n;
			cap = ncap;
		}
		memcpy( (byte *)out->pcm + used, chunk, got );
		used += got;

		if( r != MP3_OK )
		{
			out->tail_bytes = (uint32_t)got;
			out->done_tell = get_stream_pos( mpeg );
			break;
		}
		out->ok_chunks++;
		if( got != OUTBUF_SIZE )
			out->short_ok_reads++;
		out->last_ok_tell = get_stream_pos( mpeg );
		last_mismatched = (size_t)out->last_ok_tell != used / fb;
		if( last_mismatched )
		{
			if( !out->tell_mismatches )
			{
				out->tell_first_chunk = out->ok_chunks;
				out->tell_first_value = (uint32_t)out->last_ok_tell;
			}
			out->tell_mismatches++;
		}
		if( used > (size_t)0x7FFFFFFF / 2 )
		{
			SetErr( err, err_size, "track too long", path );
			goto fail;
		}
	}
	out->frames = (uint32_t)( used / fb );
	/* The read that fills the last whole chunk has decoded the track's
	   last mp3 frame (or most of it), and mpg123_tell's gapless end
	   adjustment shows: on 9 of the 27 retail tracks it reports more than
	   was handed out there. The replay reports libmpg's own number at that
	   one point (r3dm's tell_last_chunk), so only the other reads must
	   agree with the count. */
	if( last_mismatched )
	{
		out->tell_mismatches--;
		out->last_ok_tell_differs = 1;
		if( !out->tell_mismatches )
			out->tell_first_chunk = out->tell_first_value = 0;
	}

	/* libmpg's seek, as a restored save uses it */
	for( k = 0; k < MUSIC_MP3_SEEK_PROBES && out->frames; k++ )
	{
		uint32_t target = (uint32_t)(( (uint64_t)out->frames * (uint32_t)k ) / ( MUSIC_MP3_SEEK_PROBES - 2 ));

		if( k == MUSIC_MP3_SEEK_PROBES - 2 )
			target = out->frames - 1000 < out->frames ? out->frames - 1000 : 0;
		else if( k == MUSIC_MP3_SEEK_PROBES - 1 )
			target = 12345 % out->frames;
		target += (uint32_t)k * 7u; /* off any frame or chunk grid */
		if( target >= out->frames )
			target = out->frames - 1;
		SeekProbe( mpeg, out, target, chunk );
	}

	free( chunk );
	close_decoder( mpeg );
	fclose( f );
	return 1;

fail:
	free( chunk );
	Music_Mp3Free( out );
	close_decoder( mpeg );
	fclose( f );
	return 0;
}

struct music_mp3_stream_s
{
	FILE *file;
	void *ptr;
	char temp[OUTBUF_SIZE];
	size_t pos;
	int buffsize;
};

music_mp3_stream_t *Music_Mp3StreamOpen( const char *path, int *rate, int *channels )
{
	music_mp3_stream_t *s;
	wavinfo_t sc;
	int ret;

	s = (music_mp3_stream_t *)calloc( 1, sizeof( *s ));
	if( !s || !( s->file = fopen( path, "rb" )))
	{
		free( s );
		return NULL;
	}
	if(( s->ptr = create_decoder( &ret )) == NULL || !open_mpeg_stream( s->ptr, s->file, ReadCb, SeekCb, &sc ))
	{
		if( s->ptr )
			close_decoder( s->ptr );
		fclose( s->file );
		free( s );
		return NULL;
	}
	*rate = sc.rate;
	*channels = sc.channels;
	return s;
}

int Music_Mp3StreamRead( music_mp3_stream_t *stream, int needBytes, void *buffer )
{
	/* Stream_ReadMPG, `snd_mp3.c:361-398` */
	int bytesWritten = 0;
	void *mpg = stream->ptr;

	while( 1 )
	{
		byte *data;
		int outsize;

		if( !stream->buffsize )
		{
			if( read_mpeg_stream( mpg, (byte*)stream->temp, &stream->pos ) != MP3_OK )
				break; /* there was end of the stream */
		}

		if( bytesWritten + stream->pos > (size_t)needBytes )
			outsize = ( needBytes - bytesWritten );
		else outsize = (int)stream->pos;

		data = (byte *)buffer + bytesWritten;
		memcpy( data, &stream->temp[stream->buffsize], outsize );
		bytesWritten += outsize;
		stream->pos -= outsize;
		stream->buffsize += outsize;

		if( bytesWritten >= needBytes )
			return bytesWritten;

		stream->buffsize = 0;
	}

	return 0;
}

int Music_Mp3StreamSetPos( music_mp3_stream_t *stream, int newpos )
{
	/* Stream_SetPosMPG, `snd_mp3.c:407-418` */
	if( set_stream_pos( stream->ptr, newpos ) != -1 )
	{
		stream->buffsize = 0;
		return 1;
	}
	return 0;
}

int Music_Mp3StreamGetPos( music_mp3_stream_t *stream )
{
	return get_stream_pos( stream->ptr );
}

void Music_Mp3StreamClose( music_mp3_stream_t *stream )
{
	close_decoder( stream->ptr );
	fclose( stream->file );
	free( stream );
}
