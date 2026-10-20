/*
xbox_snd_music_voice_host_test.c - engine/platform/xbox/xbox_snd_music_voice.c against nxdk-ex's OpenAL
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
Divergence #183 (docs/r3d/design/x9-audio-apu-voices.md, stage C). The
music voice built for the host against the real library (its null
backend) and the real baked-music stream (xbox_music_adpcm.c, over a file
in memory). This file plays upstream's S_StreamBackgroundTrack (its loop,
line for line, with the hook where S_RawEntSamples is), the mixer's
chunks (Xbox_SndMusicVoice_Paint), the room stream's clock and the APU:
each frame the hardware voice's position moves on by what it played, at a
rate the test can set a little off the mix's. Checked: the start at the
stream frame of the paint time; every queued buffer the file's blocks, in
order, with no gap and no repeat, through a track's end and its loop into
a new stream (in other memory, and in the same); the raw channel silent
and its end where the voice's data runs out, so upstream's loop keeps
MAX_RAW_SAMPLES ahead and ends; the voice never running dry, with the APU
0.5% fast or slow; a pause stopping at the painted frame and the next
read starting again; an emptied raw channel; the clear; the voices going
away under an open stream; a stream that is not sunk left to upstream.
tools/host-tests/xbox_snd_music_voice.sh builds and runs it.
*/
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

#include "common.h"
#include "platform/xbox/xbox_snd_music_voice.h"
#include "platform/xbox/xbox_music_adpcm.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "al_source.h"
#include "al_buffer.h"
#include "apu_voice.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

void Con_Printf( const char *fmt, ... )
{
	va_list ap;

	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );
}

/* ---- the engine around it ---- */
snd_globals_t snd;
static rawchan_t s_raw;
static int s_raw_there = 1;

rawchan_t *S_FindRawChannel( int entnum, qboolean create )
{
	(void)create;
	return ( s_raw_there && entnum == S_RAW_SOUND_BACKGROUNDTRACK ) ? &s_raw : NULL;
}

static int s_voices_on = 1;
int Xbox_SndVoices_Active( void ) { return s_voices_on; }

static ALuint s_room, s_ring;
static int s_base_valid = 1;
unsigned Xbox_SndGpRoom_Source( void ) { return s_room; }
int Xbox_SndGpRoom_StreamFrame( int time, unsigned *frame )
{
	if( !s_base_valid )
		return 0;
	*frame = (unsigned)time; /* the streams' frame 0 is the mixer's time 0 */
	return 1;
}

/* ---- the tracks: random blocks (the APU decodes them, nothing here does) ---- */
#define RATE 44100u
#define CH   2u
#define BB   ( 36u * CH )

typedef struct
{
	uint8_t *file;
	uint32_t len;
	xbox_xwav_info_t info;
	int serial;
} track_t;

static track_t s_tracks[2];

static void make_track( track_t *t, uint32_t frames, int serial )
{
	const uint32_t blocks = ( frames + 63 ) / 64;
	uint32_t i;

	t->len = blocks * BB;
	t->file = malloc( t->len );
	for( i = 0; i < t->len; i++ )
		t->file[i] = (uint8_t)( rand() >> 4 );
	memset( &t->info, 0, sizeof( t->info ));
	t->info.tag = XBOX_XWAV_TAG_ADPCM;
	t->info.channels = CH;
	t->info.rate = RATE;
	t->info.block_bytes = BB;
	t->info.data_offset = 0;
	t->info.data_bytes = t->len;
	t->info.frames = frames;
	t->info.source_rate = RATE;
	t->info.source_frames = frames;
	t->info.chunk_bytes = 8192;
	t->serial = serial;
}

/* the block of a track this data is, or -1 */
static int find_block( const track_t *t, const uint8_t *data )
{
	uint32_t b;

	for( b = 0; b < t->len / BB; b++ )
		if( !memcmp( t->file + b * BB, data, BB ))
			return (int)b;
	return -1;
}

typedef struct { const track_t *t; uint32_t at; } memfile_t;

static int mem_read( void *ctx, void *dst, int bytes )
{
	memfile_t *m = ctx;
	uint32_t n = (uint32_t)bytes;

	if( m->at >= m->t->len )
		return 0;
	if( n > m->t->len - m->at )
		n = m->t->len - m->at;
	memcpy( dst, m->t->file + m->at, n );
	m->at += n;
	return (int)n;
}

static int mem_seek( void *ctx, uint32_t offset )
{
	((memfile_t *)ctx)->at = offset;
	return 0;
}

/* upstream's stream_t, as far as the hook sees it */
struct stream_s
{
	xbox_music_stream_t s;
	memfile_t mf;
	uint8_t chunk[8192];
	uint8_t ra[XBOX_MUSIC_READAHEAD_BYTES];
	const track_t *t;
};

struct xbox_music_stream_s *Xbox_MusicStream_Sunk( struct stream_s *stream )
{
	return ( stream && stream->s.sink ) ? &stream->s : NULL;
}

static void stream_open( struct stream_s *st, const track_t *t )
{
	xbox_music_io_t io;

	memset( &io, 0, sizeof( io ));
	st->mf.t = t;
	st->mf.at = 0;
	st->t = t;
	io.read = mem_read;
	io.seek = mem_seek;
	io.ctx = &st->mf;
	CHECK( Xbox_MusicStream_Init( &st->s, &t->info, &io, st->chunk, sizeof( st->chunk ), st->ra ));
	/* Music_OpenXwav's */
	if( Xbox_SndMusicVoice_Want( &t->info ))
		Xbox_MusicStream_SetSink( &st->s, Xbox_SndMusicVoice_Sink, &st->s );
}

/* ---- the queued buffers, as the source holds them ---- */
#define MAX_RAW 16384
#define RING    16384u /* nxdk-ex's AL_STREAM_RING_FRAMES */

typedef struct { ALuint id; const void *data; ALsizei size; uint8_t head[16]; } seen_t; /* head: the ring slot is reused */
static seen_t s_seen[2048];
static int s_nseen;
static int s_exp_track = -1;     /* the track the next buffer continues */
static int s_exp_block;
static int s_jump_ok;            /* a stop happened: the next buffer may start anywhere later */
static int s_bad_bufs, s_buf_count;

/* every buffer the library holds that this test has not seen: the file's
   blocks, continuing the last one (or block 0 of the next track) */
static void check_new_buffers( void )
{
	typedef struct { int serial, block, n; } nb_t;
	nb_t nb[128];
	int nn = 0, i, j;
	ALuint id;

	for( id = 1; id < 4096; id++ )
	{
		ALbuffer *b = al_buffer_get( id );
		int k, dup = 0, tr, blk = -1;

		if( !b || !b->in_use || b->ref_count <= 0 || !b->data_virt || b->id == s_ring )
			continue;
		for( k = 0; k < s_nseen; k++ )
			if( s_seen[k].id == id && s_seen[k].data == b->data_virt && s_seen[k].size == b->size && !memcmp( s_seen[k].head, b->data_virt, 16 ))
				dup = 1;
		if( dup )
			continue;
		for( k = 0; k < s_nseen; k++ )
			if( s_seen[k].id == id )
				break;
		if( k == s_nseen && s_nseen < 2048 )
			s_nseen++;
		s_seen[k].id = id;
		s_seen[k].data = b->data_virt;
		s_seen[k].size = b->size;
		memcpy( s_seen[k].head, b->data_virt, 16 );

		CHECK( b->size > 0 && b->size % (ALsizei)BB == 0 && b->frequency == (ALsizei)RATE && b->channels == (ALint)CH && b->bits == 4 );
		/* a copy, not a static buffer: one of those allocates a contiguous
		   tail page and runs wbinvd on every call, for data only the CPU
		   copies into the stream's ring */
		CHECK( !b->is_static );
		for( tr = 0; tr < 2 && blk < 0; tr++ )
		{
			blk = find_block( &s_tracks[tr], b->data_virt );
			if( blk >= 0 )
			{
				if( memcmp( b->data_virt, s_tracks[tr].file + blk * BB, (size_t)b->size ))
				{
					CHECK( !"a buffer is not the file's blocks in a row" );
					s_bad_bufs++;
				}
				if( nn < 128 )
				{
					nb[nn].serial = s_tracks[tr].serial;
					nb[nn].block = blk;
					nb[nn].n = b->size / (ALsizei)BB;
					nn++;
				}
			}
		}
		if( blk < 0 )
		{
			CHECK( !"a buffer holds no block of a track" );
			s_bad_bufs++;
		}
	}
	/* in play order: what continues the track playing, then the next
	   track's (a buffer never spans two), each by block */
#define NB_KEY( x ) (( (x).serial == s_exp_track && (x).block >= s_exp_block ) ? (x).block : 1000000 + (x).block )
	for( i = 0; i < nn; i++ )
		for( j = i + 1; j < nn; j++ )
			if( NB_KEY( nb[j] ) < NB_KEY( nb[i] ))
			{
				nb_t x = nb[i];
				nb[i] = nb[j];
				nb[j] = x;
			}
	for( i = 0; i < nn; i++ )
	{
		const int cont = nb[i].serial == s_exp_track && nb[i].block == s_exp_block;
		const int next_track = nb[i].block == 0; /* the files take turns: either one's start */
		const int jump = s_jump_ok && ( nb[i].serial != s_exp_track || nb[i].block >= s_exp_block );

		if( !( s_exp_track < 0 || cont || next_track || jump ))
		{
			printf( "  buffer track %d block %d (+%d), expected track %d block %d\n", nb[i].serial, nb[i].block, nb[i].n, s_exp_track, s_exp_block );
			CHECK( !"a buffer does not continue the last one" );
			s_bad_bufs++;
		}
		s_exp_track = nb[i].serial;
		s_exp_block = nb[i].block + nb[i].n;
		s_jump_ok = 0;
		s_buf_count++;
	}
}

/* ---- upstream's S_StreamBackgroundTrack loop, with the hook ---- */
static struct stream_s *s_bg;
static struct stream_s s_streams[2];
static int s_next_track, s_loop_same; /* the loop's next stream: the other object, or this one again */
static int s_cpu_adds, s_fill_runaway;

static void fill( int vol )
{
	static uint8_t raw[MAX_RAW];
	int guard = 0;

	if( !s_bg )
		return;
	if( s_raw.s_rawend < (uint)snd.soundtime )
		s_raw.s_rawend = (uint)snd.soundtime;

	while( s_raw.s_rawend < (uint)snd.soundtime + MAX_RAW )
	{
		const int bufferSamples = MAX_RAW - (int)( s_raw.s_rawend - (uint)snd.soundtime );
		int fileSamples = (int)( bufferSamples * ((float)RATE / SOUND_DMA_SPEED ));
		int fileBytes, r;

		if( ++guard > 400 )
		{
			s_fill_runaway++;
			CHECK( !"upstream's loop does not end" );
			return;
		}
		if( fileSamples <= 1 )
			return;
		fileBytes = fileSamples * 2 * (int)CH;
		if( fileBytes > (int)sizeof( raw ))
		{
			fileBytes = sizeof( raw );
			fileSamples = fileBytes / ( 2 * (int)CH );
		}
		r = Xbox_MusicStream_Read( &s_bg->s, fileBytes, raw );
		if( r < fileBytes )
		{
			fileBytes = r;
			fileSamples = r / ( 2 * (int)CH );
		}
		if( r > 0 )
		{
			if( !Xbox_SndMusicVoice_Samples( &s_raw, s_bg, (uint)fileSamples, vol ))
				s_cpu_adds++; /* S_RawEntSamples */
		}
		else
		{
			/* the loop: FS_FreeStream, FS_OpenStream( loopName ) */
			struct stream_s *n = s_loop_same ? s_bg : &s_streams[s_bg == &s_streams[0]];

			stream_open( n, &s_tracks[s_next_track++ % 2] );
			s_bg = n;
		}
	}
}

/* ---- the clocks and the APU ---- */
#define FRAME_MIX 576  /* 12 ms at 48 kHz */
#define MIXAHEAD  5760 /* _snd_mixahead 0.12 */

static double s_vpos;      /* frames the hardware voice played since its trigger */
static ALint s_last_state = AL_INITIAL;
static double s_rate_err;
static int s_paused;
static int s_dry_frames;   /* frames the source was not playing while it should */

static ALuint music_src( void ) { return Xbox_SndMusicVoice_DebugSource(); }

/* the stream frame a timed start or stop waits for */
static uint32_t timed_at( void )
{
	const ALsource *s = al_source_get( music_src());

	return s ? s->timed_sample : 0xFFFFFFFFu;
}

static ALint state_of( ALuint src )
{
	ALint st = 0;

	alGetSourcei( src, AL_SOURCE_STATE, &st );
	return st;
}

static ALint geti( ALuint src, ALenum p )
{
	ALint v = -12345;

	alGetSourcei( src, p, &v );
	return v;
}

/* one game frame: the device played FRAME_MIX, the voice its share, the
   voice tick, the paint's check, upstream's read loop */
static void frame( int vol )
{
	const ALuint src = music_src();
	ALsource *s = src ? al_source_get( src ) : NULL;
	ALint st;

	snd.soundtime += FRAME_MIX;
	snd.paintedtime = snd.soundtime + MIXAHEAD;
	if( s && s_last_state == AL_PLAYING && s->hw_voice_idx >= 0 )
	{
		s_vpos += FRAME_MIX * ( (double)RATE / SOUND_DMA_SPEED ) * ( 1.0 + s_rate_err );
		apu_voice_debug_set_position( (uint32_t)s->hw_voice_idx, (uint32_t)s_vpos % RING );
	}
	al_source_debug_clock( true, (uint32_t)snd.soundtime, 0u, false );
	alXboxUpdateVoices();
	st = src ? state_of( src ) : AL_INITIAL;
	if( st == AL_PLAYING && s_last_state != AL_PLAYING )
		s_vpos = 0.0; /* a trigger sets the position to 0 */
	s_last_state = st;

	Xbox_SndMusicVoice_Paint( s_paused );
	if( !s_paused )
		fill( vol );
	check_new_buffers();
}

static unsigned stat_field( const char *line, const char *after )
{
	const char *p = strstr( line, after );
	unsigned v = 99999;

	if( p )
		sscanf( p + strlen( after ), "%u", &v );
	return v;
}

/* the stats line's counts since the last call: gaps, full, no buffer, dropped */
static void stats( unsigned *gaps, unsigned *full, unsigned *nobuf, unsigned *dropped, unsigned *starts )
{
	char line[512];
	char *p;

	line[0] = 0;
	Xbox_SndMusicVoice_Stats( line, sizeof( line ));
	*starts = stat_field( line, "playing; " ) != 99999 ? stat_field( line, "playing; " ) : stat_field( line, "idle; " );
	p = strstr( line, "frames); " );
	*gaps = p ? (unsigned)strtoul( p + 9, NULL, 10 ) : 99999;
	p = strstr( line, "gaps, " );
	*full = p ? (unsigned)strtoul( p + 6, NULL, 10 ) : 99999;
	p = strstr( line, "full ring, " );
	*nobuf = p ? (unsigned)strtoul( p + 11, NULL, 10 ) : 99999;
	p = strstr( line, "no buffer, " );
	*dropped = p ? (unsigned)strtoul( p + 11, NULL, 10 ) : 99999;
}

static void setup( void )
{
	static int16_t pcm[4096];

	alGenBuffers( 1, &s_ring );
	alBufferData( s_ring, AL_FORMAT_STEREO16, pcm, (ALsizei)sizeof( pcm ), 48000 );
	alGenSources( 1, &s_room );
	alSourceQueueBuffers( s_room, 1, &s_ring );
	alSourcei( s_room, AL_LOOPING, AL_TRUE );
	alSourcePlay( s_room );
	CHECK( alGetError() == AL_NO_ERROR );
	al_source_debug_clock( true, 0u, 0u, false );
	alXboxUpdateVoices();

	make_track( &s_tracks[0], RATE * 2 + 1000 + 37, 0 );  /* 2 s, a short last chunk and block */
	make_track( &s_tracks[1], RATE * 3 / 2 + 77, 1 );
	memset( &s_raw, 0, sizeof( s_raw ));
	s_raw.entnum = S_RAW_SOUND_BACKGROUNDTRACK;
	s_raw.max_samples = MAX_RAW;
}

/* frames of steady play: the voice playing, its data never short */
static void play( int frames, int vol, int *min_ahead )
{
	int i;

	for( i = 0; i < frames; i++ )
	{
		frame( vol );
		if( state_of( music_src()) != AL_PLAYING )
			s_dry_frames++;
		else if( (int)( s_raw.s_rawend - (uint)snd.soundtime ) < *min_ahead )
			*min_ahead = (int)( s_raw.s_rawend - (uint)snd.soundtime );
		CHECK( geti( music_src(), AL_BUFFERS_QUEUED ) <= 64 );
		CHECK( s_raw.leftvol == 0 && s_raw.rightvol == 0 && s_raw.master_vol == 0 );
	}
}

static void test_start( void )
{
	const ALuint src = music_src();
	ALfloat g[2] = { -1.0f, -1.0f };
	unsigned gaps, full, nobuf, dropped, starts;

	printf( "[1] the first read starts the voice at the stream frame of the paint time\n" );
	CHECK( src != 0 );
	snd.soundtime = 0;
	snd.paintedtime = MIXAHEAD;
	stream_open( &s_streams[0], &s_tracks[0] );
	s_next_track = 1;
	s_bg = &s_streams[0];
	CHECK( Xbox_MusicStream_Sunk( s_bg ) != NULL ); /* given the sink at its open */
	fill( 200 );
	check_new_buffers();
	CHECK( s_cpu_adds == 0 );
	CHECK( state_of( src ) == AL_INITIAL );
	CHECK( al_source_debug_timed_pending( src ) == 1u );
	CHECK( timed_at() == MIXAHEAD ); /* the paint time, raw end 0 before it */
	CHECK( geti( src, AL_BUFFERS_QUEUED ) > 0 );
	CHECK( s_exp_track == 0 && s_buf_count > 0 );
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	CHECK( g[0] == 200 / 256.0f && g[1] == 200 / 256.0f );
	CHECK( geti( src, AL_XBOX_DIRECT_MODE ) == AL_TRUE );
	{
		ALfloat prio = 0.0f;

		alGetSourcef( src, AL_XBOX_PRIORITY, &prio );
		CHECK( prio == 1000.0f );
	}
	/* upstream's loop filled to MAX_RAW ahead of soundtime: from the
	   paint time, what is queued at the mix rate */
	CHECK( s_raw.s_rawend >= MAX_RAW && s_raw.s_rawend < MAX_RAW + 200 );
	CHECK( s_raw.leftvol == 0 && s_raw.rightvol == 0 && s_raw.master_vol == 0 );
	{
		/* the queue: from the paint time to the raw end */
		const double queued_mix = ( geti( src, AL_BUFFERS_QUEUED ) > 0 ) ? (double)( s_raw.s_rawend - MIXAHEAD ) : 0.0;

		CHECK( queued_mix > 9000.0 );
	}
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 1 && gaps == 0 && full == 0 && nobuf == 0 && dropped == 0 );
}

static void test_play( void )
{
	int min_ahead = 1 << 30;
	unsigned gaps, full, nobuf, dropped, starts;
	const int n0 = s_buf_count;

	printf( "[2] steady play through two loops: the voice fed, the file's blocks in order\n" );
	play( 10, 200, &min_ahead ); /* the start acts at soundtime 5760 */
	CHECK( state_of( music_src()) == AL_PLAYING );
	s_dry_frames = 0;
	min_ahead = 1 << 30;
	play( 600, 200, &min_ahead ); /* 7.2 s: track 0, track 1, track 0 again */
	CHECK( s_dry_frames == 0 );
	CHECK( min_ahead > MAX_RAW - 2 * FRAME_MIX - 200 );
	CHECK( s_next_track >= 3 );
	CHECK( s_buf_count > n0 + 100 );
	CHECK( s_bad_bufs == 0 && s_fill_runaway == 0 && s_cpu_adds == 0 );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 0 && gaps == 0 && full == 0 && nobuf == 0 && dropped == 0 );
}

static void test_drift( void )
{
	int min_ahead = 1 << 30;
	unsigned gaps, full, nobuf, dropped, starts;
	int k;

	printf( "[3] the APU 0.5%% fast, then slow: the raw end follows it, never dry, the queue bounded\n" );
	for( k = 0; k < 2; k++ )
	{
		s_rate_err = k ? -0.005 : 0.005;
		s_dry_frames = 0;
		min_ahead = 1 << 30;
		play( 1500, 200, &min_ahead ); /* 18 s: 90 ms of drift */
		CHECK( s_dry_frames == 0 );
		CHECK( min_ahead > MAX_RAW - 2 * FRAME_MIX - 300 );
		CHECK( (int)( s_raw.s_rawend - (uint)snd.soundtime ) < MAX_RAW + 2500 );
	}
	s_rate_err = 0.0;
	CHECK( s_bad_bufs == 0 && s_fill_runaway == 0 && s_cpu_adds == 0 );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 0 && gaps == 0 && full == 0 && nobuf == 0 && dropped == 0 );
}

static void test_loop_same( void )
{
	int min_ahead = 1 << 30;
	unsigned gaps, full, nobuf, dropped, starts;

	printf( "[4] the loop's stream in the freed one's memory: from its block 0 again\n" );
	s_loop_same = 1;
	s_dry_frames = 0;
	play( 600, 200, &min_ahead );
	s_loop_same = 0;
	CHECK( s_dry_frames == 0 );
	CHECK( s_bad_bufs == 0 && s_fill_runaway == 0 && s_cpu_adds == 0 );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 0 && gaps == 0 && full == 0 && nobuf == 0 && dropped == 0 );
}

static void test_pause( void )
{
	const ALuint src = music_src();
	unsigned gaps, full, nobuf, dropped, starts;
	int min_ahead = 1 << 30, i;

	printf( "[5] a pause stops the voice at the painted frame; the next read starts it again\n" );
	s_paused = 1;
	frame( 200 );
	CHECK( al_source_debug_timed_pending( src ) == 2u ); /* a timed stop */
	CHECK( timed_at() == (uint32_t)snd.paintedtime );
	for( i = 0; i < 30; i++ )
		frame( 200 );
	CHECK( state_of( src ) == AL_STOPPED );
	s_paused = 0;
	s_jump_ok = 1;
	frame( 200 );
	CHECK( state_of( src ) == AL_INITIAL && al_source_debug_timed_pending( src ) == 1u );
	CHECK( timed_at() == (uint32_t)snd.paintedtime ); /* upstream's clamp put the raw end at soundtime, before it */
	CHECK( geti( src, AL_BUFFERS_QUEUED ) > 0 );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 1 && full == 0 && nobuf == 0 && dropped == 0 );
	play( 20, 200, &min_ahead );
	s_dry_frames = 0;
	play( 100, 200, &min_ahead );
	CHECK( state_of( src ) == AL_PLAYING && s_dry_frames == 0 );
	CHECK( s_bad_bufs == 0 && s_cpu_adds == 0 );
}

static void test_emptied( void )
{
	const ALuint src = music_src();
	unsigned gaps, full, nobuf, dropped, starts;
	int min_ahead = 1 << 30;

	printf( "[6] the raw channel emptied (S_ClearRawChannels): stopped at the painted frame, then again from the read\n" );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	s_raw.s_rawend = 0;
	Xbox_SndMusicVoice_Paint( false );
	CHECK( al_source_debug_timed_pending( src ) == 2u );
	CHECK( timed_at() == (uint32_t)snd.paintedtime );
	s_jump_ok = 1;
	fill( 200 );
	check_new_buffers();
	CHECK( state_of( src ) == AL_INITIAL && al_source_debug_timed_pending( src ) == 1u );
	CHECK( timed_at() == (uint32_t)snd.paintedtime );
	stats( &gaps, &full, &nobuf, &dropped, &starts );
	CHECK( starts == 1 && full == 0 && nobuf == 0 && dropped == 0 );
	play( 20, 200, &min_ahead );
	s_dry_frames = 0;
	play( 50, 200, &min_ahead );
	CHECK( state_of( src ) == AL_PLAYING && s_dry_frames == 0 );

	printf( "[7] the volume follows the music volume at once\n" );
	frame( 77 );
	{
		ALfloat g[2] = { -1.0f, -1.0f };

		alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
		CHECK( g[0] == 77 / 256.0f && g[1] == 77 / 256.0f );
	}
	CHECK( s_bad_bufs == 0 && s_cpu_adds == 0 );
}

static void test_clear_and_off( void )
{
	const ALuint src = music_src();
	struct stream_s other;
	int min_ahead = 1 << 30;

	printf( "[8] the clear stops it now and gives every buffer back; the next read starts again\n" );
	Xbox_SndMusicVoice_Clear();
	CHECK( state_of( src ) == AL_STOPPED || state_of( src ) == AL_INITIAL );
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 0 );
	s_jump_ok = 1;
	play( 30, 200, &min_ahead );
	s_dry_frames = 0;
	play( 30, 200, &min_ahead );
	CHECK( state_of( src ) == AL_PLAYING && s_dry_frames == 0 );

	printf( "[9] a stream without the sink is upstream's\n" );
	stream_open( &other, &s_tracks[0] );
	Xbox_MusicStream_SetSink( &other.s, NULL, NULL );
	CHECK( !Xbox_SndMusicVoice_Samples( &s_raw, &other, 100, 200 ));
	CHECK( !Xbox_SndMusicVoice_Samples( &s_raw, NULL, 100, 200 ));

	printf( "[10] the voices go under an open stream: it decodes again, the read in hand is dropped\n" );
	s_voices_on = 0;
	Xbox_SndMusicVoice_Shutdown();
	CHECK( Xbox_SndMusicVoice_DebugSource() == 0 );
	CHECK( Xbox_SndMusicVoice_Samples( &s_raw, s_bg, 100, 200 ));
	CHECK( Xbox_MusicStream_Sunk( s_bg ) == NULL );
	CHECK( !Xbox_SndMusicVoice_Samples( &s_raw, s_bg, 100, 200 ));
	{
		xbox_xwav_info_t info = s_tracks[0].info;

		CHECK( !Xbox_SndMusicVoice_Want( &info )); /* off: a new track decodes */
	}
}

static void test_want( void )
{
	xbox_xwav_info_t info = s_tracks[0].info;

	printf( "[0] which tracks play here: ADPCM, mono or stereo\n" );
	CHECK( Xbox_SndMusicVoice_Want( &info ));
	info.channels = 1;
	CHECK( Xbox_SndMusicVoice_Want( &info ));
	info.tag = XBOX_XWAV_TAG_PCM;
	info.channels = 2;
	CHECK( !Xbox_SndMusicVoice_Want( &info ));
}

int main( void )
{
	ALCdevice *dev;
	ALCcontext *ctx;

	alXboxSetPoolSize( 64 * 1024 ); /* the console's (xbox_snd_al.c) */
	dev = alcOpenDevice( NULL );
	ctx = dev ? alcCreateContext( dev, NULL ) : NULL;

	if( !ctx || !alcMakeContextCurrent( ctx ))
	{
		printf( "xbox_snd_music_voice: no OpenAL context\n" );
		return 1;
	}
	srand( 1234 );
	setup();
	Xbox_SndMusicVoice_Init();
	test_want();
	test_start();
	test_play();
	test_drift();
	test_loop_same();
	test_pause();
	test_emptied();
	test_clear_and_off();

	alcMakeContextCurrent( NULL );
	alcDestroyContext( ctx );
	alcCloseDevice( dev );
	if( failures )
	{
		printf( "xbox_snd_music_voice: %d of %d checks failed\n", failures, checks );
		return 1;
	}
	printf( "xbox_snd_music_voice: all %d checks passed\n", checks );
	return 0;
}
