/*
xbox_music_stream.c - baked music tracks (.xwav) as an engine stream
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
Resonance3D (docs/r3d/divergences.md #171). Music decoded from mp3 every
frame cost 0.44-0.48 ms on the console (k_snd_music, route-pr193).
tools/asset-studio --music bakes each retail track, with the engine's own
libmpg, to media/<Track>.xwav (Xbox ADPCM, or PCM for a track named to
it); FS_OpenStream asks Xbox_MusicStream_Open before any of upstream's
formats, and a NULL from here leaves upstream's mp3 path to play the
track exactly as before.

The stream is xbox_music_adpcm.c's: upstream's Stream_ReadMPG replayed
over the baked frames in libmpg's chunks, so what S_StreamBackgroundTrack
receives - bytes, the dropped tail, positions - is what the mp3 path
gives it; volume, pause, looping and save/restore stay upstream's
(s_stream.c, unchanged). tools/asset-studio checks the replay against
the real libmpg on every track it bakes.

The mix rate (#173). When the engine mixes at 48 kHz (r3d_snd_rate) a
track baked at 48 kHz, media/<Track>.48k.xwav (tools/asset-studio
--music-rate 48000), is opened first, so S_RawSamplesStereo copies it
at a step of exactly 1; without one, the source-rate media/<Track>.xwav
plays, point-resampled to 48 kHz by that copy. At 44.1 kHz only the
source-rate file is looked for. Positions are in the source's frames
either way, so a save, a seek or a change of rate carries over.

A .xwav is used only when it is the bake of the file upstream would
open: the source its r3dm chunk names (extension and size) must be the
file FS_OpenStream's own search finds first. A stale bake, a mod's own
track over valve's, or a request for another format falls through to
upstream.

r3d_music_baked 0 sends every track to the mp3 path on the same disc (the
A/B); r3d_music_ab flips it and restarts the playing track at its
position; r3d_music_crc checks a track's two decodes on the console
against the baker's CRCs (music_crc.txt in the write directory).

The file reads (#172). The stream refilled a 9 KiB read-ahead with a
synchronous FS_Read every 11 frames, and on the console each refill cost
0.5-1.0 ms of its frame: half of k_snd_music's 0.13 ms a frame (session
8c7db857). The read-ahead is now two 16 KiB windows of the file
(xbox_music_adpcm.h), and when the .xwav is a plain file on disk the
windows are read through a handle of their own with overlapped,
unbuffered reads (compat/xbox/xbox_async_read.h): entering one window
queues the read of the next, the kernel transfers it while the game
runs, and the frame that needs it finds it landed. A file inside a pack,
a handle the kernel will not open that way, or a read it refuses or cuts
short sends the stream back to FS_Read, synchronously, for good.
Everything stays on the main thread: no worker, no lock, and nothing new
for the loading screen or a map change to race with - a track that stops
waits for its read in flight before its buffer is freed.
*/

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "client.h"
#include "sound.h" /* SOUND_DMA_SPEED: the mix rate picks the bake, #173 */
#include "soundlib.h"
#include "soundlib/libmpg/libmpg.h"
#include "xbox_frametime.h"
#include "xbox_music_adpcm.h"
#include "xbox_music_stream.h"
#include "xbox_async_read.h" /* compat/xbox: the overlapped reads, #172 */

/* Not archived on purpose: an A/B session that ended on 0 would otherwise
   keep the mp3 path on through the HDD's config.cfg, which shadows the
   disc's, and every later measurement would silently be the old one. */
static CVAR_DEFINE_AUTO( r3d_music_baked, "1", 0,
	"play media/*.xwav, baked from the mp3s by tools/asset-studio --music, in place of the mp3 (docs/r3d/divergences.md #171)" );

/* One allocation per open track: the stream_t upstream's callers hold,
   the private state, and the read-ahead on a page boundary (the kernel
   reads about three times faster into one, docs/r3d/lessons.md, the
   autosave entry; an unbuffered read needs a sector boundary). Freed
   whole by Stream_FreeXwav. */
typedef struct music_state_s
{
	stream_t stream;   /* first: the pointer FS_FreeStream frees is the allocation */
	xbox_music_stream_t s;
	r3d_async_read_t async; /* the overlapped reads' handle (#172), when async_on */
	qboolean async_on;
} music_state_t;

#define MUSIC_ALIGN XBOX_MUSIC_IO_ALIGN

/* The file reads, each counted for the recorder (music_* counters,
   xbox_frametime_util.h, #172). The synchronous pair reads the file_t
   upstream's stream holds; the overlapped pair its own handle. */
static void Music_CountRead( uint32_t bytes )
{
	xbox_ft_work.c[XBOX_FT_W_MUSIC_READS]++;
	xbox_ft_work.c[XBOX_FT_W_MUSIC_READ_KB] += ( bytes + 1023u ) / 1024u;
}

static int Music_IoRead( void *ctx, void *dst, int bytes )
{
	music_state_t *st = ctx;
	xbox_ft_span_t span;
	int got;

	xbox_ft_span_begin( &span );
	got = (int)FS_Read( st->stream.file, dst, bytes );
	xbox_ft_time_add( &span, XBOX_FT_W_MUSIC_IO_KC );
	Music_CountRead( (uint32_t)bytes );
	xbox_ft_work.c[XBOX_FT_W_MUSIC_WAITS]++; /* synchronous: the frame waits it out */
	return got;
}

static int Music_IoSeek( void *ctx, uint32_t offset )
{
	music_state_t *st = ctx;
	xbox_ft_span_t span;
	int r;

	xbox_ft_span_begin( &span );
	r = FS_Seek( st->stream.file, (fs_offset_t)offset, SEEK_SET ) == -1 ? -1 : 0;
	xbox_ft_time_add( &span, XBOX_FT_W_MUSIC_IO_KC );
	return r;
}

static int Music_AsyncStart( void *ctx, uint32_t offset, void *dst, uint32_t bytes )
{
	music_state_t *st = ctx;
	xbox_ft_span_t span;
	int r;

	xbox_ft_span_begin( &span );
	r = r3d_async_start( &st->async, offset, dst, bytes );
	xbox_ft_time_add( &span, XBOX_FT_W_MUSIC_IO_KC );
	if( r < 0 )
		return -1;
	Music_CountRead( bytes );
	if( r > 0 )
		xbox_ft_work.c[XBOX_FT_W_MUSIC_ASYNC]++;
	return 0;
}

static int Music_AsyncFinish( void *ctx, int wait )
{
	music_state_t *st = ctx;
	xbox_ft_span_t span;
	int r, waited;

	xbox_ft_span_begin( &span );
	r = r3d_async_finish( &st->async, wait, &waited );
	xbox_ft_time_add( &span, XBOX_FT_W_MUSIC_IO_KC );
	if( waited )
		xbox_ft_work.c[XBOX_FT_W_MUSIC_WAITS]++;
	return r == R3D_ASYNC_PENDING ? XBOX_MUSIC_IO_PENDING : r;
}

static int Stream_ReadXwav( stream_t *stream, int bytes, void *buffer )
{
	xbox_ft_span_t span;
	int r;

	xbox_ft_span_begin( &span );
	r = Xbox_MusicStream_Read( &((music_state_t *)stream->ptr)->s, bytes, buffer );
	xbox_ft_time_add( &span, XBOX_FT_W_MUSIC_READ_KC );
	return r;
}

static int Stream_SetPosXwav( stream_t *stream, int newpos )
{
	return Xbox_MusicStream_SetPos( &((music_state_t *)stream->ptr)->s, newpos );
}

static int Stream_GetPosXwav( stream_t *stream )
{
	return Xbox_MusicStream_GetPos( &((music_state_t *)stream->ptr)->s );
}

static void Stream_FreeXwav( stream_t *stream )
{
	music_state_t *st = stream->ptr;

	/* a read still landing writes into the read-ahead, which goes with
	   the allocation below: wait for it, then close its handle (#172) */
	if( st )
	{
		Xbox_MusicStream_Close( &st->s );
		if( st->async_on )
			r3d_async_close( &st->async );
	}

	/* Stream_FreeMPG's order (snd_mp3.c:439-454 in the reference); the
	   state and the read-ahead are in the same allocation as the stream */
	stream->ptr = NULL;
	if( stream->file )
	{
		FS_Close( stream->file );
		stream->file = NULL;
	}
	Mem_Free( stream );
}

static const streamfmt_t xwav_format =
{
	"xwav", NULL, Stream_ReadXwav, Stream_SetPosXwav, Stream_GetPosXwav, Stream_FreeXwav
};

static void Music_Note( const char *what, const char *filename, uint32_t kc, uint32_t bytes )
{
	char name[XBOX_FT_NOTE_NAME_BYTES];

	Q_snprintf( name, sizeof( name ), "%s %s", what, COM_FileWithoutPath( filename ));
	Xbox_FT_Note( XBOX_FT_NOTE_MUSIC, name, kc, bytes );
}

static int Music_Probe( void *ctx, const char *path, int64_t *size )
{
	const fs_offset_t n = FS_FileSize( path, false );

	(void)ctx;
	*size = n;
	return n >= 0;
}

/* Plays the .xwav `file` (FS_Open'd as `path`) for the request
   `filename`, or closes it and returns NULL with the note's reason in
   *why: the file does not parse, was baked for another libmpg chunk or
   from another file than the one upstream would open, or the stream
   refuses it. */
static stream_t *Music_OpenXwav( file_t *file, const char *path, const char *filename, const char *base, int requested,
	const char *const *formats, int nformats, uint32_t want_rate, const xbox_ft_span_t *span, const char **why )
{
	byte head[XBOX_XWAV_HEADER_READ];
	xbox_xwav_info_t info;
	xbox_xwav_err_t err;
	xbox_xwav_source_t source;
	xbox_music_io_t io;
	music_state_t *st;
	stream_t *stream;
	fs_offset_t len, got;
	byte *ra;

	len = FS_FileLength( file );
	got = FS_Read( file, head, sizeof( head ));
	err = got > 0 ? Xbox_MusicXwav_ParseHeader( head, (size_t)got, (uint32_t)len, &info ) : XBOX_XWAV_ERR_SHORT;
	if( err != XBOX_XWAV_OK || info.chunk_bytes != OUTBUF_SIZE || len > 0x7FFFFFFF )
	{
		Con_Reportf( S_WARN "%s: %s: %s\n", __func__, path,
			err != XBOX_XWAV_OK ? Xbox_MusicXwav_ErrorString( err ) : "not baked against this engine's libmpg chunk" );
		FS_Close( file );
		*why = "mp3 (bad xwav)";
		return NULL;
	}

	/* a rate-tagged name (<Track>.48k.xwav) must hold that rate (#173): a
	   file renamed from another bake would play point-resampled */
	if( want_rate && info.rate != want_rate )
	{
		Con_Reportf( S_WARN "%s: %s: %u Hz, not the %u Hz its name says - skipped\n", __func__, path, (unsigned)info.rate, (unsigned)want_rate );
		FS_Close( file );
		*why = "mp3 (bad xwav)";
		return NULL;
	}

	/* the bake of what upstream would open, or nothing */
	source = Xbox_MusicXwav_CheckSource( &info, base, requested, formats, nformats, Music_Probe, NULL );
	if( source != XBOX_XWAV_SOURCE_OK )
	{
		Con_Reportf( S_WARN "%s: %s: %s\n", __func__, path,
			source == XBOX_XWAV_SOURCE_NONE ? "no source file next to it" :
			source == XBOX_XWAV_SOURCE_FORMAT ? "the request opens another format than it was baked from" :
			"baked from another file than the one there now" );
		FS_Close( file );
		*why = "mp3 (stale xwav)";
		return NULL;
	}

	st = Mem_Calloc( host.soundpool, sizeof( *st ) + XBOX_MUSIC_READAHEAD_BYTES + MUSIC_ALIGN - 1 );
	stream = &st->stream;
	stream->file = file; /* the synchronous reads' (Music_IoRead) */
	ra = (byte *)((( (uintptr_t)( st + 1 )) + MUSIC_ALIGN - 1 ) & ~(uintptr_t)( MUSIC_ALIGN - 1 ));

	memset( &io, 0, sizeof( io ));
	io.read = Music_IoRead;
	io.seek = Music_IoSeek;
	io.ctx = st;

	/* #172: the windows read with overlapped reads on a handle of their
	   own, when the file FS_Open found is a plain file on disk (the same
	   search, FS_GetFullDiskPath: NULL for one inside a pack) */
	{
		const char *disk = FS_GetDiskPath( path, false );

		if( disk && r3d_async_open( &st->async, disk ) == 0 )
		{
			st->async_on = true;
			io.start = Music_AsyncStart;
			io.finish = Music_AsyncFinish;
		}
	}

	if( !Xbox_MusicStream_Init( &st->s, &info, &io, (uint8_t *)stream->temp, sizeof( stream->temp ), ra ))
	{
		Con_Reportf( S_WARN "%s: %s: stream refused\n", __func__, path );
		if( st->async_on )
			r3d_async_close( &st->async );
		Mem_Free( st );
		FS_Close( file );
		*why = "mp3 (bad xwav)";
		return NULL;
	}

	/* what Stream_OpenMPG sets (snd_mp3.c:344-349 in the reference), and
	   the format FS_OpenStream would set after its own openfunc */
	stream->file = file;
	stream->pos = 0;
	stream->buffsize = 0;
	stream->channels = (int)info.channels;
	stream->rate = (int)info.rate;
	stream->width = 2;
	stream->ptr = st;
	stream->type = WF_PCMDATA;
	stream->format = &xwav_format;

	{
		/* the codec, the read path and (#173) the file's rate */
		char what[48];

		Q_snprintf( what, sizeof( what ), "xwav %s %s %u", info.tag == XBOX_XWAV_TAG_ADPCM ? "adpcm" : "pcm",
			st->async_on ? "async" : "sync", (unsigned)info.rate );
		Music_Note( what, filename, xbox_ft_span_kc_since( span ), (uint32_t)len );
	}
	return stream;
}

/* Opens the bake for the mix rate, <name>.48k.xwav at 48 kHz (#173), and
   when there is none or it is refused, <name>.xwav; `xwav` names the one
   file to try instead (r3d_music_crc checks every one on the disc).
   `force` ignores r3d_music_baked (r3d_music_crc). NULL leaves the
   request to upstream's formats. */
static stream_t *Music_Open( const char *filename, qboolean force, const char *xwav )
{
	const char *formats[8];
	int nformats = 0, requested, ncand = 0, i;
	uint32_t cand_rate[2] = { 0, 0 }; /* the rate a candidate's name says it holds; 0: any */
	const streamfmt_t *format;
	char base[MAX_SYSPATH], cand[2][MAX_SYSPATH];
	const char *why = NULL;
	xbox_ft_span_t span;

	xbox_ft_span_begin( &span );

	for( format = sound.streamformat; format && format->ext && nformats < (int)ARRAYSIZE( formats ); format++ )
		formats[nformats++] = format->ext;
	requested = Xbox_MusicXwav_SplitName( filename, formats, nformats, base, sizeof( base ));

	if( xwav )
		Q_strncpy( cand[ncand++], xwav, sizeof( cand[0] ));
	else
	{
		if( SOUND_DMA_SPEED != SOUND_44k )
		{
			cand_rate[ncand] = (uint32_t)SOUND_DMA_SPEED;
			Q_snprintf( cand[ncand++], sizeof( cand[0] ), "%s.%dk.xwav", base, SOUND_DMA_SPEED / 1000 );
		}
		Q_snprintf( cand[ncand++], sizeof( cand[0] ), "%s.xwav", base );
	}

	for( i = 0; i < ncand; i++ )
	{
		file_t *file = FS_Open( cand[i], "rb", false );
		stream_t *stream;

		if( !file )
			continue; /* not baked: the normal case off the console, and upstream's own path */

		if( !force && !r3d_music_baked.value )
		{
			FS_Close( file );
			Music_Note( "mp3 (r3d_music_baked 0)", filename, 0, 0 );
			return NULL;
		}

		stream = Music_OpenXwav( file, cand[i], filename, base, requested, formats, nformats, cand_rate[i], &span, &why );
		if( stream )
			return stream;
	}

	if( why )
	{
		Con_Reportf( S_WARN "%s: no usable .xwav for %s - playing the original\n", __func__, filename );
		Music_Note( why, filename, 0, 0 );
	}
	return NULL;
}

stream_t *Xbox_MusicStream_Open( const char *filename )
{
	return Music_Open( filename, false, NULL );
}

/* ---- r3d_music_ab: flip r3d_music_baked and pick the track up where it was ---- */

static void Music_AB_f( void )
{
	char current[MAX_QPATH], loop[MAX_QPATH];
	int position = 0;
	const qboolean playing = S_StreamGetCurrentState( current, sizeof( current ), loop, sizeof( loop ), &position );

	Cvar_DirectSet( &r3d_music_baked, r3d_music_baked.value ? "0" : "1" );
	Con_Printf( "r3d_music_ab: music now %s\n", r3d_music_baked.value ? "BAKED (.xwav)" : "MP3 (libmpg)" );
	if( playing )
		S_StartBackgroundTrack( current, loop, position, false );
}

/* ---- r3d_music_crc: the console's two decodes against the bake ---- */

static file_t *crc_file;

static void Music_Print( const char *fmt, ... )
{
	char line[512];
	va_list args;

	va_start( args, fmt );
	Q_vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );
	Con_Printf( "%s", line );
	if( crc_file )
		FS_Write( crc_file, line, Q_strlen( line ));
}

static mpg_ssize_t Music_MpgRead( void *file, void *buf, size_t count )
{
	return FS_Read( (file_t *)file, buf, count );
}

static fs_offset_t Music_MpgSeek( void *file, fs_offset_t offset, int whence )
{
	/* FS_SeekMpg, snd_mp3.c:291-294 in the reference */
	return FS_Seek( (file_t *)file, offset, whence ) == -1 ? -1 : FS_Tell( (file_t *)file );
}

/* `name` is a track (Half-Life01, media/Half-Life01.mp3: the bake the
   mix rate picks) or a .xwav (media/Half-Life01.48k.xwav: that file). */
static void Music_CrcTrack( const char *name, qboolean dump )
{
	char base[MAX_SYSPATH], path[MAX_SYSPATH], xwav[MAX_SYSPATH];
	qboolean named_xwav;
	size_t len;
	byte *chunk;
	stream_t *baked;
	xbox_music_stream_t *s;
	file_t *mp3, *out = NULL;
	void *mpeg;
	wavinfo_t sc;
	uint32_t crc = 0, frames = 0, bcrc = 0, bframes = 0, chunk_frames;
	uint64_t t0, mp3_ticks, xwav_ticks;
	int ret;

	if( Q_strnicmp( name, "media/", 6 ))
		Q_snprintf( base, sizeof( base ), "media/%s", name );
	else Q_strncpy( base, name, sizeof( base ));
	named_xwav = !Q_stricmp( COM_FileExtension( base ), "xwav" );
	Q_strncpy( xwav, base, sizeof( xwav ));
	COM_StripExtension( base );
	/* a rate-tagged bake, <Track>.48k.xwav (#173): its track is <Track> */
	len = Q_strlen( base );
	if( named_xwav && len > 4 && base[len - 1] == 'k' && isdigit( (byte)base[len - 2] ) && isdigit( (byte)base[len - 3] ) && base[len - 4] == '.' )
		base[len - 4] = '\0';

	Q_snprintf( path, sizeof( path ), "%s.mp3", base );
	baked = Music_Open( path, true, named_xwav ? xwav : NULL );
	if( !baked )
	{
		Music_Print( "r3d_music_crc %s: no usable .xwav\n", base );
		return;
	}
	s = &((music_state_t *)baked->ptr)->s;
	chunk = (byte *)baked->temp;
	chunk_frames = sizeof( baked->temp ) / s->frame_bytes;

	/* the baked decode, the whole file; the clock runs around the decode
	   calls only (with their read-ahead refills), not the CRC */
	xwav_ticks = 0;
	while( bframes < s->info.frames )
	{
		uint32_t n;

		t0 = xbox_ft_rdtsc();
		n = Xbox_MusicStream_DecodeFrames( s, bframes, chunk_frames, (int16_t *)chunk );
		xwav_ticks += xbox_ft_rdtsc() - t0;
		if( !n )
			break;
		bcrc = Xbox_MusicCrc32( bcrc, chunk, n * s->frame_bytes );
		bframes += n;
	}

	/* libmpg, as Stream_OpenMPG and Stream_ReadMPG drive it, every read's
	   bytes (the failing one's too), as the baker does */
	mp3 = FS_Open( path, "rb", false );
	mpeg = mp3 ? create_decoder( &ret ) : NULL;
	if( !mpeg || !open_mpeg_stream( mpeg, mp3, Music_MpgRead, Music_MpgSeek, &sc ))
	{
		Music_Print( "r3d_music_crc %s: libmpg would not open %s\n", base, path );
		if( mpeg )
			close_decoder( mpeg );
		if( mp3 )
			FS_Close( mp3 );
		FS_FreeStream( baked );
		return;
	}
	if( dump )
	{
		char raw[MAX_SYSPATH];

		Q_snprintf( raw, sizeof( raw ), "music_crc_%s.raw", COM_FileWithoutPath( base ));
		out = FS_Open( raw, "wb", true );
	}
	mp3_ticks = 0;
	for( ;; )
	{
		size_t got = 0;
		int r;

		t0 = xbox_ft_rdtsc();
		r = read_mpeg_stream( mpeg, chunk, &got );
		mp3_ticks += xbox_ft_rdtsc() - t0;
		crc = Xbox_MusicCrc32( crc, chunk, got );
		frames += (uint32_t)( got / ( 2u * (uint32_t)sc.channels ));
		if( out && got )
			FS_Write( out, chunk, got );
		if( r != MP3_OK )
			break;
	}
	close_decoder( mpeg );
	FS_Close( mp3 );
	if( out )
		FS_Close( out );

	/* decode (with its file reads) per second of music: a decoder
	   microbenchmark, not k_snd_music (which also has the copy into the raw
	   channel). At 733 MHz, a frame of 1/60 s costs 1/60 of it. */
	Music_Print( "r3d_music_crc %s: mp3 %u frames crc %08x (bake %08x, %u) %s; xwav %u Hz %u frames crc %08x (bake %08x) %s;"
		" decode ms per second of music: mp3 %.2f, xwav %.2f (at 733 MHz);"
		" xwav reads %u (%s: queued %u, waited %u, fallbacks %u)\n",
		base, frames, crc, s->info.crc_source, s->info.source_frames,
		( frames == s->info.source_frames && crc == s->info.crc_source ) ? "SAME" : "DIFFERS",
		(unsigned)s->info.rate, bframes, bcrc, s->info.crc_decoded,
		( bframes == s->info.frames && bcrc == s->info.crc_decoded && !s->bad_blocks ) ? "SAME" : "DIFFERS",
		frames ? (double)mp3_ticks / 733333.0 / ( (double)frames / sc.rate ) : 0.0,
		bframes ? (double)xwav_ticks / 733333.0 / ( (double)bframes / s->info.rate ) : 0.0,
		s->io_reads, ((music_state_t *)baked->ptr)->async_on ? "async" : "sync", s->io_async, s->io_waits, s->io_fallbacks );

	FS_FreeStream( baked );
}

static void Music_Crc_f( void )
{
	const qboolean dump = Cmd_Argc() > 2 && !Q_stricmp( Cmd_Argv( 2 ), "dump" );

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "r3d_music_crc <track|all> [dump]\n" );
		return;
	}

	crc_file = FS_Open( "music_crc.txt", "a", true );
	if( !Q_stricmp( Cmd_Argv( 1 ), "all" ))
	{
		search_t *t = FS_Search( "media/*.xwav", true, false );
		int i;

		for( i = 0; t && i < t->numfilenames; i++ )
			Music_CrcTrack( t->filenames[i], dump );
		if( t )
			Mem_Free( t );
	}
	else Music_CrcTrack( Cmd_Argv( 1 ), dump );
	Music_Print( "r3d_music_crc: done\n" );

	if( crc_file )
	{
		FS_Close( crc_file );
		crc_file = NULL;
	}
}

void Xbox_Music_Init( void )
{
	Cvar_RegisterVariable( &r3d_music_baked );
	/* r3d_music_ab restarts the track through S_StartBackgroundTrack, which
	   takes the current key destination as the track's source
	   (s_stream.c:119 in the reference): run from the console, neither of
	   its pause-by-source rules applies to that track afterwards. A test
	   command; the bind runs it in game. */
	Cmd_AddCommand( "r3d_music_ab", Music_AB_f, "flip r3d_music_baked and restart the playing track at its position (docs/r3d/divergences.md #171)" );
	Cmd_AddCommand( "r3d_music_crc", Music_Crc_f, "check a baked track's two decodes on this machine against the baker's CRCs: <track|all> [dump] (#171)" );
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
