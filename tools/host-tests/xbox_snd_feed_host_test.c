/*
xbox_snd_feed_host_test.c - engine/platform/xbox/xbox_snd_feed.c against nxdk-ex's OpenAL
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
Divergence #184 (docs/r3d/design/x9-audio-apu-voices.md, stage B1). The
feed module built for the host against the real library (its null
backend: streaming sources, their rings, the timed starts, stops and
parameter changes; a test clock, al_source_debug_clock, stands in for the
room stream's position, apu_voice_debug_set_position for a voice's).
This file plays the room module (#180) and the mixer's walk: each "chunk"
is BeginPaint, the spans S_MixChannelToBuffer would hand over
(Xbox_SndFeed_SpanFrames from the walk's position), EndPaint. Checked: the
bytes in the source's ring are the bytes handed over, in order; the start
at the stream frame of the first span; pieces of FEED_BATCH_MS; gains and
pitch at once before the start and timed after it; an ended channel
playing out and reaped; a cut one stopping at its frame; a skipped one's
staged samples going at once; a source that ran dry starting again; one
that lost its voice handing the channel to the CPU; a word of another
format, a sound that is not speech, the sources running out, a channel
reused by a memset; off when not asked.
tools/host-tests/xbox_snd_feed.sh builds and runs it.
*/

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "common.h"
#include "platform/xbox/xbox_snd_feed.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_vox_stream.h" /* the stub: SOUND_R3D_WINDOW */

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include "al_source.h"
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

snd_globals_t snd;
uint64_t xbox_ft_snd_dma_ticks;
int xbox_ft_snd_extra;

/* ---- the room module (#180), as far as the feed sees it ---- */
static ALuint s_room, s_ring, s_slot;
static int s_room_on = 1, s_base;
static int s_asked = 1;

int Xbox_SndGpRoom_Active( void ) { return s_room_on; }
unsigned Xbox_SndGpRoom_Source( void ) { return s_room; }
unsigned Xbox_SndGpRoom_Slot( void ) { return s_slot; }
int Xbox_SndGpRoom_StreamFrame( int time, unsigned *frame )
{
	*frame = (unsigned)( time - s_base );
	return 1;
}
qboolean Xbox_SndCfg_Asked( const char *key ) { return s_asked && strcmp( key, "r3d_snd_voices_speech" ) == 0; }

/* ---- the sounds and the walk ---- */
#define OUT_RATE 48000
#define CHUNK    1024 /* output samples a paint */

static wavdata_t *make_sound( int samples, int width, int rate, uint flags )
{
	wavdata_t *w = calloc( 1, sizeof( *w ) + (size_t)samples * width );
	int i;

	w->samples = samples;
	w->width = (uint8_t)width;
	w->channels = 1;
	w->rate = (uint16_t)rate;
	w->type = WF_PCMDATA;
	w->size = (size_t)samples * width;
	w->flags = flags;
	for( i = 0; i < samples * width; i++ )
		w->buffer[i] = (uint8_t)( i * 7 + ( i >> 8 ) + 1 ); /* every byte a function of its place */
	return w;
}

static sfx_t s_sfx[3];
static voxword_t s_words[2];
static channel_t s_ch[12];
static int s_time = 100000; /* the mixer's frame time */

/* what the walk fed a channel: the source's data, in order */
static uint8_t s_fed[16][1 << 16];
static int s_fed_n[16];

static void chan_play( channel_t *ch, sfx_t *sfx, int lv, int rv, qboolean sentence )
{
	memset( ch, 0, sizeof( *ch ));
	ch->sfx = sfx;
	ch->leftvol = (short)lv;
	ch->rightvol = (short)rv;
	ch->words = sentence ? s_words : NULL;
	s_fed_n[ch - s_ch] = 0;
}

/* The walk of one channel through one chunk, as S_MixChannelToBuffer:
   true when every span went to the feed */
static qboolean walk( channel_t *ch, double pitch )
{
	const wavdata_t *w = ch->sfx->cache;
	const double rate = pitch * w->rate / OUT_RATE;
	const int idx = (int)( ch - s_ch );
	int left = CHUNK, offset = 0;
	qboolean all = true;

	while( left > 0 )
	{
		const int pos = (int)ch->sample;
		const int available = (int)w->samples - pos;
		int out = left, n;

		if( available <= 0 )
		{
			SetBits( ch->flags, FL_CHAN_FINISHED );
			break;
		}
		if( ceil( ch->sample + rate * left ) - pos > available )
			out = (int)ceil(( available - ( ch->sample - pos )) / rate );
		if( out > left )
			out = left;
		n = Xbox_SndFeed_SpanFrames( ch->sample, rate, out, available );
		if( Xbox_SndFeed_Take( ch, w, w->buffer + pos * w->width, available, rate, pitch, out, offset ))
		{
			memcpy( s_fed[idx] + s_fed_n[idx], w->buffer + pos * w->width, (size_t)( n * w->width ));
			s_fed_n[idx] += n * w->width;
		}
		else
			all = false;
		ch->sample += out * rate;
		offset += out;
		left -= out;
	}
	return all;
}

/* the stream at `pos` and one voice tick (alXboxUpdateVoices once a frame) */
static void tick( unsigned pos )
{
	al_source_debug_clock( true, pos, 0u, false );
	alXboxUpdateVoices();
}

/* one chunk at the mixer's time: the channels walked in order */
static void chunk( channel_t **list, int n, double pitch, qboolean *all )
{
	int i;

	Xbox_SndFeed_BeginPaint( s_time );
	for( i = 0; i < n; i++ )
	{
		qboolean a = walk( list[i], pitch );

		if( all )
			all[i] = a;
	}
	Xbox_SndFeed_EndPaint();
	s_time += CHUNK;
	snd.paintedtime = s_time; /* S_PaintChannels: a cut between paints lands here */
}

static ALint geti( ALuint src, ALenum p )
{
	ALint v = -12345;

	alGetSourcei( src, p, &v );
	return v;
}

static ALint state_of( ALuint src )
{
	return geti( src, AL_SOURCE_STATE );
}

static float pitch_of( ALuint src )
{
	ALfloat p = -1.0f;

	alGetSourcef( src, AL_PITCH, &p );
	return p;
}

static void gains_of( ALuint src, ALfloat g[2] )
{
	g[0] = g[1] = -1.0f;
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
}

/* the source's voice at `frames` into its data, and a tick */
static void voice_at( ALuint src, unsigned frames, unsigned clock )
{
	ALsource *s = al_source_get( src );

	if( s && s->hw_voice_idx >= 0 )
		apu_voice_debug_set_position( (uint32_t)s->hw_voice_idx, frames );
	tick( clock );
}

static unsigned frame_of( int time )
{
	return (unsigned)( time - s_base );
}

static void setup( void )
{
	static int16_t pcm[4096];

	alGenBuffers( 1, &s_ring );
	alBufferData( s_ring, AL_FORMAT_STEREO16, pcm, (ALsizei)sizeof( pcm ), 48000 );
	alGenSources( 1, &s_room );
	alSourceQueueBuffers( s_room, 1, &s_ring );
	alSourcei( s_room, AL_LOOPING, AL_TRUE );
	alGenAuxiliaryEffectSlots( 1, &s_slot );
	CHECK( alGetError() == AL_NO_ERROR );
	alSourcePlay( s_room );
	s_base = s_time - 20000; /* the mixer's time s_base is the streams' frame 0 */
	tick( 0 );

	s_sfx[0].cache = make_sound( 22050, 1, 11025, 0 );                  /* a word, 2 s */
	s_sfx[1].cache = make_sound( 4000, 1, 22050, 0 );                   /* another rate */
	s_sfx[2].cache = make_sound( 30000, 1, 11025, SOUND_R3D_WINDOW );   /* streamed speech */
	strcpy( s_sfx[0].name, "!HG_WORD" );
	strcpy( s_sfx[1].name, "!FAR_WAR" );
	strcpy( s_sfx[2].name, "scientist/speech" );
	s_words[0].sfx = &s_sfx[0];
	s_words[1].sfx = &s_sfx[1];
	snd.format.speed = OUT_RATE;
}

static void test_off( void )
{
	channel_t *a = &s_ch[0];
	channel_t *list[1] = { a };
	qboolean all[1] = { true };

	printf( "[1] off unless asked\n" );
	s_asked = 0;
	Xbox_SndFeed_Init();
	CHECK( !Xbox_SndFeed_Active());
	CHECK( strstr( Xbox_SndFeed_Why(), "not asked" ) != NULL );
	chan_play( a, &s_sfx[0], 200, 100, true );
	chunk( list, 1, 1.0, all );
	CHECK( !all[0] && Xbox_SndFeed_DebugSource( a ) == 0 );
	s_asked = 1;
	Xbox_SndFeed_Init();
	CHECK( Xbox_SndFeed_Active() && !strcmp( Xbox_SndFeed_Why(), "on" ));
	printf( "    -> nothing fed while off [%s]\n", failures ? "FAIL" : "PASS" );
}

static void test_start_and_data( void )
{
	channel_t *a = &s_ch[0];
	channel_t *list[1] = { a };
	qboolean all[1];
	const int f0 = failures, t0 = s_time;
	ALuint src;
	ALfloat g[2];
	const ALbuffer *ring;
	int i;

	printf( "[2] a sentence's first span: its own source, started at the span's frame, then pieces of 48 ms\n" );
	chan_play( a, &s_sfx[0], 128, 64, true );
	chunk( list, 1, 1.0, all );
	CHECK( all[0] );
	src = Xbox_SndFeed_DebugSource( a );
	CHECK( src != 0 );
	CHECK( state_of( src ) == AL_INITIAL && al_source_debug_timed_pending( src ) == 1u );
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 1 );                 /* the first chunk at once, for the start */
	CHECK( geti( src, AL_XBOX_SEND_PANNED ) == AL_TRUE && geti( src, AL_XBOX_VIRTUALIZE ) == AL_FALSE );
	gains_of( src, g );
	CHECK( g[0] == 0.5f && g[1] == 0.25f );                       /* set at once: the start is timed */
	CHECK( pitch_of( src ) == 1.0f );
	chunk( list, 1, 1.0, all );                                    /* 1024 staged */
	chunk( list, 1, 1.0, all );                                    /* 2048 */
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 1 );
	chunk( list, 1, 1.0, all );                                    /* 3072 >= 2304: a piece */
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 2 );

	/* the start acts at the frame of the first chunk; the ring then holds the data as fed */
	tick( frame_of( t0 ) - 600 );
	tick( frame_of( t0 ) - 100 );
	CHECK( state_of( src ) == AL_PLAYING );
	voice_at( src, 0, frame_of( t0 ));
	ring = al_source_play_buffer( al_source_get( src ));
	CHECK( ring != NULL && ring->frequency == 11025 && ring->bits == 8 );
	{
		int fed = 0, ok = 1;
		/* what is queued: 4 chunks of 1024 output samples at 11025/48000 */
		for( i = 0; i < s_fed_n[0]; i++ )
		{
			if( ((const uint8_t *)ring->data_virt )[i] != s_fed[0][i] )
			{
				ok = 0;
				break;
			}
			fed++;
		}
		CHECK( ok && fed == s_fed_n[0] );
		CHECK( s_fed_n[0] == (int)floor( 4 * CHUNK * 11025.0 / OUT_RATE ));
	}
	printf( "    -> timed start, gains and pitch set, 48 ms pieces, the ring holds the walk's bytes [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_params_end( void )
{
	channel_t *a = &s_ch[0];
	channel_t *list[1] = { a };
	qboolean all[1];
	const int f0 = failures;
	const ALuint src = Xbox_SndFeed_DebugSource( a );
	ALfloat g[2];
	int t;

	printf( "[3] gains and pitch timed once it has a start; the end plays out and is reaped\n" );
	a->leftvol = 255;
	t = s_time;
	chunk( list, 1, 0.5, all );
	gains_of( src, g );
	CHECK( g[0] == 0.5f && pitch_of( src ) == 1.0f );            /* wait for the chunk's frame */
	CHECK( al_source_debug_timed_params( src ) == 2u );
	tick( frame_of( t ) - 3000 );
	tick( frame_of( t ));
	gains_of( src, g );
	CHECK( g[0] == 255.0f / 256.0f && g[1] == 0.25f && pitch_of( src ) == 0.5f );

	/* the sentence ends: what is staged goes, the source plays it out */
	SetBits( a->flags, FL_CHAN_SENTENCE_FINISHED );
	{
		const ALint q = geti( src, AL_BUFFERS_QUEUED );

		Xbox_SndFeed_Free( a );
		CHECK( geti( src, AL_BUFFERS_QUEUED ) == q + 1 );        /* the staged rest */
	}
	CHECK( Xbox_SndFeed_DebugSource( a ) == 0 && a->engine_reserved[4] == 0 );
	CHECK( state_of( src ) == AL_PLAYING );
	Xbox_SndFeed_BeginPaint( s_time );
	CHECK( state_of( src ) == AL_PLAYING );                       /* still playing: not reaped */
	voice_at( src, 16383, frame_of( s_time ));                    /* past all its data */
	voice_at( src, 16383, frame_of( s_time ) + 10 );
	CHECK( state_of( src ) == AL_STOPPED );
	Xbox_SndFeed_BeginPaint( s_time );                            /* reaped */
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 0 );
	printf( "    -> timed changes, the rest flushed at the end, played out, reaped [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_cut_skip_dry_lost( void )
{
	channel_t *a = &s_ch[1], *b = &s_ch[2];
	channel_t *list[2] = { a, b };
	qboolean all[2];
	const int f0 = failures;
	ALuint sa, sb;
	int t;

	printf( "[4] a cut one stops at its frame; a skipped one's samples go now; dry starts again; lost goes to the CPU\n" );
	s_time += 8000; /* the mixer paints ahead of what the stream plays, as _snd_mixahead */
	chan_play( a, &s_sfx[2], 200, 200, false );                   /* streamed speech, not a sentence */
	chan_play( b, &s_sfx[0], 200, 200, true );
	chunk( list, 2, 1.0, all );
	CHECK( all[0] && all[1] );
	sa = Xbox_SndFeed_DebugSource( a );
	sb = Xbox_SndFeed_DebugSource( b );
	CHECK( sa && sb && sa != sb );

	/* a is cut short between paints: a stop at the frame painted to */
	Xbox_SndFeed_Free( a );
	CHECK( al_source_debug_timed_pending( sa ) == 2u );

	/* b skipped (the menu): its staged samples are queued at once */
	chunk( list + 1, 1, 1.0, all );                                /* 1024 staged */
	{
		const ALint q = geti( sb, AL_BUFFERS_QUEUED );

		chunk( NULL, 0, 1.0, NULL );                               /* not walked */
		CHECK( geti( sb, AL_BUFFERS_QUEUED ) == q + 1 );
	}

	/* b ran dry (played all it had, then the clock moved on): it starts again */
	tick( frame_of( s_time ) - 200 );
	tick( frame_of( s_time ));
	CHECK( state_of( sb ) == AL_PLAYING );
	voice_at( sb, 16383, frame_of( s_time ) + 400 );
	voice_at( sb, 16383, frame_of( s_time ) + 800 );
	CHECK( state_of( sb ) == AL_STOPPED );
	s_time += 8000;
	snd.soundtime = s_time - 2000;                                /* heard past what it was fed */
	t = s_time;
	{
		const int before = s_fed_n[2];
		const ALbuffer *ring;

		chunk( list + 1, 1, 1.0, all );
		CHECK( all[0] && Xbox_SndFeed_DebugSource( b ) == sb );
		CHECK( state_of( sb ) == AL_INITIAL && al_source_debug_timed_pending( sb ) == 1u );
		tick( frame_of( t ) - 500 );
		tick( frame_of( t ));
		CHECK( state_of( sb ) == AL_PLAYING );
		voice_at( sb, 0, frame_of( t ) + 1 );
		ring = al_source_play_buffer( al_source_get( sb ));
		CHECK( ring && s_fed_n[2] > before
			&& !memcmp( ring->data_virt, s_fed[2] + before, (size_t)( s_fed_n[2] - before )));   /* the new data, from its start */
	}

	/* b's voice is taken while its data is ahead: the CPU from the walk's position */
	s_time += 8000;
	chunk( list + 1, 1, 1.0, all );
	alSourceStop( sb );                                            /* as a preemption with AL_XBOX_VIRTUALIZE off */
	snd.soundtime = s_time - 4000;                                /* its data ends 4000 + a chunk ahead */
	chunk( list + 1, 1, 1.0, all );
	CHECK( !all[0] && Xbox_SndFeed_DebugSource( b ) == 0 && b->engine_reserved[5] == 1 );
	chunk( list + 1, 1, 1.0, all );
	CHECK( !all[0] );                                              /* and stays there */
	Xbox_SndFeed_Free( b );
	CHECK( b->engine_reserved[5] == 0 );
	printf( "    -> timed stop, skipped flush, dry restart, lost to the CPU [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_cpu_cases( void )
{
	channel_t *a = &s_ch[3], *c = &s_ch[4];
	channel_t *list[1] = { a };
	qboolean all[1];
	const int f0 = failures;
	ALuint sa;
	int i;

	printf( "[5] another format, not speech, the sources running out, a channel reused by a memset\n" );
	Xbox_SndFeed_Clear();
	chan_play( a, &s_sfx[0], 200, 200, true );
	chunk( list, 1, 1.0, all );
	sa = Xbox_SndFeed_DebugSource( a );
	CHECK( all[0] && sa );
	a->sfx = &s_sfx[1];                                           /* the next word: 22050 Hz */
	a->sample = 0;
	chunk( list, 1, 1.0, all );
	CHECK( !all[0] && Xbox_SndFeed_DebugSource( a ) == 0 && a->engine_reserved[5] == 1 );
	CHECK( state_of( sa ) == AL_INITIAL && al_source_debug_timed_pending( sa ) == 1u );   /* what it had still plays */
	Xbox_SndFeed_Free( a );

	/* not speech: a resident sound that is not a sentence is stage A's */
	{
		static sfx_t plain;
		channel_t *p = &s_ch[5];
		channel_t *pl[1] = { p };

		plain.cache = make_sound( 4000, 1, 11025, 0 );
		chan_play( p, &plain, 200, 200, false );
		chunk( pl, 1, 1.0, all );
		CHECK( !all[0] && Xbox_SndFeed_DebugSource( p ) == 0 );
		Xbox_SndFeed_Free( p );
		free( plain.cache );
	}

	/* the eight sources taken: a ninth speech channel stays on the CPU */
	Xbox_SndFeed_Clear();
	for( i = 0; i < 9; i++ )
	{
		channel_t *x = &s_ch[3 + i];
		channel_t *xl[1] = { x };

		chan_play( x, &s_sfx[0], 200, 200, true );
		chunk( xl, 1, 1.0, all );
		CHECK( all[0] == ( i < 8 ));
	}
	for( i = 0; i < 9; i++ )
		Xbox_SndFeed_Free( &s_ch[3 + i] );

	/* a static channel reused by a memset (SND_PickStaticChannel): its feed stops */
	Xbox_SndFeed_Clear();
	chan_play( c, &s_sfx[2], 200, 200, false );
	{
		channel_t *cl[1] = { c };

		chunk( cl, 1, 1.0, all );
	}
	sa = Xbox_SndFeed_DebugSource( c );
	CHECK( sa != 0 );
	memset( c, 0, sizeof( *c ));
	chunk( NULL, 0, 1.0, NULL );
	CHECK( al_source_debug_timed_pending( sa ) == 2u );
	printf( "    -> format to the CPU (gapless), plain sounds not taken, 8 sources, memset reuse stops [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

/* A failed upload is not queued, and the source's names stay its own:
   queued + free == FEED_BUFS throughout */
static void test_upload_failure( void )
{
	channel_t *a = &s_ch[6];
	channel_t *list[1] = { a };
	qboolean all[1];
	const int f0 = failures;
	ALuint src;
	int i;

	printf( "[6] a failed upload: not queued, the names accounted for\n" );
	Xbox_SndFeed_Clear();
	s_time += 8000;
	chan_play( a, &s_sfx[0], 200, 200, true );
	chunk( list, 1, 1.0, all );
	src = Xbox_SndFeed_DebugSource( a );
	CHECK( src && geti( src, AL_BUFFERS_QUEUED ) == 1 );
	for( i = 0; i < 6; i++ )
	{
		const ALint before = geti( src, AL_BUFFERS_QUEUED );
		const qboolean fail = i == 1 || i == 4;

		if( fail )
			Xbox_SndFeed_DebugFailUploads( 1 );
		chunk( list, 1, 1.0, all );
		chunk( list, 1, 1.0, all );
		chunk( list, 1, 1.0, all );                                /* a piece each third chunk */
		CHECK( all[0] && Xbox_SndFeed_DebugSource( a ) == src );
		CHECK( geti( src, AL_BUFFERS_QUEUED ) + Xbox_SndFeed_DebugFreeNames( a ) == 8 );
		CHECK( geti( src, AL_BUFFERS_QUEUED ) == before + ( fail ? 0 : 1 ));   /* a failed piece is not queued */
	}
	CHECK( geti( src, AL_BUFFERS_QUEUED ) == 5 );
	Xbox_SndFeed_Free( a );
	printf( "    -> failed uploads dropped, never queued; queued + free = 8 [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_stats( void )
{
	char line[512];
	size_t n;
	const int f0 = failures;

	printf( "[7] the stats line\n" );
	n = Xbox_SndFeed_Stats( line, sizeof( line ));
	CHECK( n > 0 && strstr( line, "speech fed:" ) && strstr( line, "after running dry" ));
	printf( "%s", line );
	Xbox_SndFeed_Shutdown();
	CHECK( !Xbox_SndFeed_Active() && Xbox_SndFeed_Stats( line, sizeof( line )) == 0 );
	printf( "    -> one line, nothing after the shutdown [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

int main( void )
{
	ALCdevice *dev = alcOpenDevice( NULL );
	ALCcontext *ctx;

	assert( dev );
	ctx = alcCreateContext( dev, NULL );
	alcMakeContextCurrent( ctx );
	alGetError();
	setup();
	test_off();
	test_start_and_data();
	test_params_end();
	test_cut_skip_dry_lost();
	test_cpu_cases();
	test_upload_failure();
	test_stats();
	al_source_debug_clock( false, 0u, 0u, false );
	alcMakeContextCurrent( NULL );
	alcDestroyContext( ctx );
	alcCloseDevice( dev );
	printf( "xbox_snd_feed: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
