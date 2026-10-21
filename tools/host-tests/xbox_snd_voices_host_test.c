/*
xbox_snd_voices_host_test.c - engine/platform/xbox/xbox_snd_voices.c against nxdk-ex's OpenAL
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
Divergence #182 (docs/r3d/design/x9-audio-apu-voices.md, stage A). The
voice module built for the host against the real library (its null
backend: sources, buffers, voices, the timed starts and stops; a test
clock, al_source_debug_clock, stands in for the room stream's position).
This file plays the room module (#180) and the mixer: each "chunk" is
BeginPaint, Xbox_SndVoices_Channel for the channels the mixer would mix,
EndPaint, and the voice tick. Checked: what goes on a voice and what stays
on the CPU, the source as the channel sets it up (buffer, loop, offset,
gains, pitch, the send), the start at the stream's frame, a sound that
ends playing out, one cut short stopping at its frame, a short sound freed
before its frame, a static channel reused by a memset, a skipped channel
pausing and resuming, a lost voice handing the channel to the CPU, the
loop turned off, S_FreeSound, the clear, the sources running out.
tools/host-tests/xbox_snd_voices.sh builds and runs it.
*/
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

#include "common.h"
#include "platform/xbox/xbox_snd_voices.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_vox_stream.h" /* the stub: SOUND_R3D_WINDOW */

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include "al_source.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

void Con_Printf( const char *fmt, ... )
{
	va_list ap;

	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );
}

snd_globals_t snd; /* the mixer's times: paintedtime places a cut made between paints */

/* ---- the room module (#180), as far as the voices see it ---- */
static ALuint s_room, s_ring, s_slot, s_fx;
static int s_room_on = 1, s_base_valid = 1, s_base;

int Xbox_SndGpRoom_Active( void ) { return s_room_on; }
unsigned Xbox_SndGpRoom_Source( void ) { return s_room; }
unsigned Xbox_SndGpRoom_Slot( void ) { return s_slot; }
int Xbox_SndGpRoom_StreamFrame( int time, unsigned *frame )
{
	if( !s_base_valid )
		return 0;
	*frame = (unsigned)( time - s_base );
	return 1;
}
qboolean Xbox_SndCfg_Asked( const char *key ) { return strcmp( key, "r3d_snd_voices" ) == 0; }

/* ---- the sounds ---- */
static wavdata_t *make_sound( int samples, int width, int channels, int rate, qboolean looped, int loop_start )
{
	wavdata_t *w = calloc( 1, sizeof( *w ) + (size_t)samples * width * channels );

	w->samples = samples;
	w->width = (uint8_t)width;
	w->channels = (uint8_t)channels;
	w->rate = (uint16_t)rate;
	w->type = WF_PCMDATA;
	w->size = (size_t)samples * width * channels;
	w->flags = looped ? SOUND_LOOPED : 0;
	w->loop_start = (uint)loop_start;
	memset( w->buffer, width == 1 ? 0x80 : 0, w->size );
	return w;
}

static sfx_t s_sfx[4];
static channel_t s_ch[70];

static void chan_play( channel_t *ch, sfx_t *sfx, int lv, int rv, uint flags )
{
	memset( ch, 0, sizeof( *ch )); /* S_StartSound's memset */
	ch->sfx = sfx;
	ch->leftvol = (short)lv;
	ch->rightvol = (short)rv;
	ch->flags = flags;
}

/* the stream at `pos` and one voice tick (alXboxUpdateVoices once a frame) */
static void tick( unsigned pos )
{
	al_source_debug_clock( true, pos, 0u, false );
	alXboxUpdateVoices();
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

/* one chunk at frame time t: the given channels mix (in that order) */
static void chunk( int t, channel_t **mix, int n, const double *pitch )
{
	int i;

	Xbox_SndVoices_BeginPaint( t );
	for( i = 0; i < n; i++ )
		mix[i]->engine_reserved[7] = Xbox_SndVoices_Channel( mix[i], mix[i]->sfx->cache, pitch ? pitch[i] : 1.0 );
	Xbox_SndVoices_EndPaint();
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
	alGenEffects( 1, &s_fx );
	CHECK( alGetError() == AL_NO_ERROR );
	tick( 0 );

	s_sfx[0].cache = make_sound( 4410, 1, 1, 11025, false, 0 );    /* an 8-bit effect, 0.4 s */
	s_sfx[1].cache = make_sound( 22050, 2, 1, 22050, true, 1000 ); /* a 16-bit loop */
	s_sfx[2].cache = make_sound( 100, 1, 2, 22050, false, 0 );     /* a short stereo click */
	s_sfx[3].cache = make_sound( 4410, 1, 1, 11025, false, 0 );
	strcpy( s_sfx[0].name, "a" );
	strcpy( s_sfx[1].name, "loop" );
	strcpy( s_sfx[2].name, "click" );
	strcpy( s_sfx[3].name, "b" );
}

static void test_start( void )
{
	channel_t *a = &s_ch[0];
	channel_t *list[1] = { a };
	const double pitch = 1.25;
	ALuint src;
	ALfloat g[2] = { -1.0f, -1.0f }, p = 0.0f, prio = -1.0f;

	printf( "[1] a channel takes a voice: set up as the channel is, started at the stream's frame\n" );
	alSourcePlay( s_room );
	s_base = 1000;            /* the streams' frame 0 is the mixer's frame time 1000 */
	tick( 0 );
	chan_play( a, &s_sfx[0], 128, 64, 0 );
	a->sample = 300;          /* a restored save: part way in */
	chunk( 1000 + 2048, list, 1, &pitch );
	CHECK( a->engine_reserved[7] == 1 );
	src = Xbox_SndVoices_DebugSource( a );
	CHECK( src != 0 );
	CHECK( state_of( src ) == AL_INITIAL );                   /* waits for stream frame 2048 */
	CHECK( al_source_debug_timed_pending( src ) == 1u );
	CHECK( geti( src, AL_SAMPLE_OFFSET ) == 300 );
	CHECK( geti( src, AL_LOOPING ) == AL_FALSE );
	CHECK( geti( src, AL_XBOX_SEND_PANNED ) == AL_TRUE );
	CHECK( geti( src, AL_XBOX_DIRECT_MODE ) == AL_TRUE );
	CHECK( geti( src, AL_XBOX_VIRTUALIZE ) == AL_FALSE );
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	CHECK( g[0] == 0.5f && g[1] == 0.25f );                    /* 128 / 256, 64 / 256 */
	alGetSourcef( src, AL_PITCH, &p );
	CHECK( p == 1.25f );
	alGetSourcef( src, AL_XBOX_PRIORITY, &prio );
	CHECK( prio > 0.5f && prio < 0.51f );                      /* 128 / 255 */
	{
		ALint fmt_bits = geti( (ALuint)geti( src, AL_BUFFER ), AL_BITS );
		ALint freq = geti( (ALuint)geti( src, AL_BUFFER ), AL_FREQUENCY );

		(void)fmt_bits; (void)freq;
	}
	tick( 1500 );
	CHECK( state_of( src ) == AL_INITIAL );                   /* 548 ahead */
	tick( 2040 );                                              /* within the half update: starts */
	CHECK( state_of( src ) == AL_PLAYING );

	/* the next chunks: a volume change reaches the voice at its chunk's frame, as the start did */
	a->leftvol = 255;
	chunk( 1000 + 3072, list, 1, &pitch );
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	CHECK( g[0] == 0.5f && g[1] == 0.25f );                    /* frame 3072: 1032 ahead, waits */
	CHECK( al_source_debug_timed_params( src ) == 1u );
	tick( 3000 );                                              /* 960 an update: the window is 480 */
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	CHECK( g[0] == 255.0f / 256.0f && g[1] == 0.25f );
	CHECK( Xbox_SndVoices_DebugSource( a ) == src );
	chunk( 1000 + 3584, list, 1, &pitch );                     /* the same ones send nothing */
	CHECK( al_source_debug_timed_params( src ) == 0u );

	/* gains and pitch together: both wait, then land on one tick */
	a->leftvol = 64;
	a->rightvol = 192;
	{
		const double half = 0.5;

		chunk( 1000 + 4096, list, 1, &half );
	}
	CHECK( al_source_debug_timed_params( src ) == 2u );
	tick( 3500 );                                              /* 596 ahead, the window 250 */
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	alGetSourcef( src, AL_PITCH, &p );
	CHECK( g[0] == 255.0f / 256.0f && g[1] == 0.25f && p == 1.25f );
	tick( 4000 );                                              /* 96 ahead */
	alGetSourcefv( src, AL_XBOX_DIRECT_GAINS, g );
	alGetSourcef( src, AL_PITCH, &p );
	CHECK( g[0] == 0.25f && g[1] == 0.75f && p == 0.5f );
	CHECK( al_source_debug_timed_params( src ) == 0u );
	printf( "    -> buffer, offset, loop, gains, pitch, priority, panned send; timed start [%s]\n", failures ? "FAIL" : "PASS" );
}

static void test_end_and_cut( void )
{
	channel_t *a = &s_ch[0], *b = &s_ch[1], *c = &s_ch[2], *d = &s_ch[3];
	channel_t *list[2];
	ALuint sa = Xbox_SndVoices_DebugSource( a ), sb, sc, sd;
	const int f0 = failures;

	printf( "[2] an ended sound plays out; a cut one stops at its frame; a short one freed before its frame still plays\n" );
	/* a ends (the walk is ahead of the voice): the voice plays on */
	SetBits( a->flags, FL_CHAN_FINISHED );
	Xbox_SndVoices_Free( a );
	CHECK( a->engine_reserved[2] == 0 && Xbox_SndVoices_DebugSource( a ) == 0 );
	chunk( 1000 + 4096, NULL, 0, NULL );
	CHECK( state_of( sa ) == AL_PLAYING );                    /* still playing out */
	alSourceStop( sa );                                        /* its data ran out */
	chunk( 1000 + 5120, NULL, 0, NULL );
	CHECK( geti( sa, AL_BUFFER ) == 0 );                       /* reaped */

	/* b is cut short (replaced, stopped): it stops at the frame of the chunk */
	chan_play( b, &s_sfx[3], 200, 200, 0 );
	list[0] = b;
	chunk( 1000 + 6144, list, 1, NULL );
	sb = Xbox_SndVoices_DebugSource( b );
	tick( 5632 );
	tick( 6144 );                                              /* 512 an update: the window is 256 */
	CHECK( state_of( sb ) == AL_PLAYING );
	Xbox_SndVoices_BeginPaint( 1000 + 7168 );
	Xbox_SndVoices_Free( b );                                  /* no FL_CHAN_FINISHED */
	Xbox_SndVoices_EndPaint();
	CHECK( al_source_debug_timed_pending( sb ) == 2u );        /* a stop at stream frame 7168 */
	tick( 6500 );
	CHECK( state_of( sb ) == AL_PLAYING );
	tick( 7168 );
	CHECK( state_of( sb ) == AL_STOPPED );
	chunk( 1000 + 8192, NULL, 0, NULL );
	CHECK( geti( sb, AL_BUFFER ) == 0 );

	/* c: 100 samples, over within its first chunk, freed before its frame */
	chan_play( c, &s_sfx[2], 255, 255, 0 );
	list[0] = c;
	chunk( 1000 + 9216, list, 1, NULL );
	sc = Xbox_SndVoices_DebugSource( c );
	SetBits( c->flags, FL_CHAN_FINISHED );
	Xbox_SndVoices_Free( c );
	chunk( 1000 + 10240, NULL, 0, NULL );                     /* its frame (9216) not reached */
	CHECK( state_of( sc ) == AL_INITIAL && geti( sc, AL_BUFFER ) != 0 );
	tick( 9216 );
	CHECK( state_of( sc ) == AL_PLAYING );                    /* it is heard */
	alSourceStop( sc );
	chunk( 1000 + 10240, NULL, 0, NULL );
	CHECK( geti( sc, AL_BUFFER ) == 0 );

	/* d is cut between paints - S_StartSound replacing it, as each held
	   machine-gun shot replaces the last: at the frame the mixer painted
	   to (snd.paintedtime), where the next sound starts, not at the start
	   of the last chunk painted (11264), a chunk early */
	chan_play( d, &s_sfx[3], 200, 200, 0 );
	list[0] = d;
	chunk( 1000 + 11264, list, 1, NULL );
	sd = Xbox_SndVoices_DebugSource( d );
	tick( 10752 );
	tick( 11264 );
	CHECK( state_of( sd ) == AL_PLAYING );
	snd.paintedtime = 1000 + 12288;                            /* the chunk painted to here */
	Xbox_SndVoices_Free( d );                                  /* outside any paint */
	tick( 11776 );
	CHECK( state_of( sd ) == AL_PLAYING );                    /* not at 11264 */
	tick( 12288 );
	CHECK( state_of( sd ) == AL_STOPPED );                    /* at 12288 */
	chunk( 1000 + 12288, NULL, 0, NULL );
	CHECK( geti( sd, AL_BUFFER ) == 0 );
	printf( "    -> plays out, timed stop, a short sound's start kept [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_reuse_pause_lost( void )
{
	channel_t *a = &s_ch[3];
	channel_t *list[1] = { a };
	ALuint s;
	const int f0 = failures;

	printf( "[3] a static channel reused by a memset; a skipped channel pauses and resumes; a lost voice goes to the CPU\n" );
	chan_play( a, &s_sfx[1], 100, 100, FL_CHAN_USE_LOOP );
	chunk( 1000 + 11264, list, 1, NULL );
	s = Xbox_SndVoices_DebugSource( a );
	CHECK( geti( s, AL_LOOPING ) == AL_TRUE );
	tick( 11264 );
	CHECK( state_of( s ) == AL_PLAYING );
	/* skipped (the menu, inaudible): paused, then on again */
	chunk( 1000 + 12288, NULL, 0, NULL );
	CHECK( state_of( s ) == AL_PAUSED );
	chunk( 1000 + 13312, list, 1, NULL );
	CHECK( state_of( s ) == AL_PLAYING && a->engine_reserved[7] == 1 );
	/* SND_STOP_LOOPING: the loop goes off on the voice too */
	ClearBits( a->flags, FL_CHAN_USE_LOOP );
	chunk( 1000 + 14336, list, 1, NULL );
	CHECK( geti( s, AL_LOOPING ) == AL_FALSE );
	SetBits( a->flags, FL_CHAN_USE_LOOP );
	chunk( 1000 + 15360, list, 1, NULL );
	CHECK( geti( s, AL_LOOPING ) == AL_TRUE );
	/* SND_PickStaticChannel's replacement: a memset, no S_FreeChannel */
	chan_play( a, &s_sfx[1], 100, 100, FL_CHAN_USE_LOOP );
	chunk( 1000 + 16384, NULL, 0, NULL );                     /* the old voice sees its channel gone */
	CHECK( al_source_debug_timed_pending( s ) == 2u );
	tick( 16384 );
	CHECK( state_of( s ) == AL_STOPPED );
	chunk( 1000 + 17408, list, 1, NULL );                     /* the new sound takes a voice of its own */
	CHECK( Xbox_SndVoices_DebugSource( a ) != 0 );
	s = Xbox_SndVoices_DebugSource( a );
	tick( 17408 );
	/* the voice taken from it: the channel mixes on the CPU from now on */
	alSourceStop( s );
	chunk( 1000 + 18432, list, 1, NULL );
	CHECK( a->engine_reserved[7] == 0 && Xbox_SndVoices_DebugSource( a ) == 0 );
	chunk( 1000 + 19456, list, 1, NULL );
	CHECK( a->engine_reserved[7] == 0 );
	Xbox_SndVoices_Free( a );
	CHECK( a->engine_reserved[3] == 0 );
	printf( "    -> memset reuse stops the old voice, pause/resume, loop off/on, lost voice to the CPU [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_cpu_cases( void )
{
	channel_t *a = &s_ch[4];
	channel_t *list[1] = { a };
	voxword_t words[2] = { { &s_sfx[0] }, { NULL } };
	wavdata_t *win = make_sound( 100, 1, 1, 22050, false, 0 );
	sfx_t wsfx = { "window", NULL };
	const int f0 = failures;

	printf( "[4] what stays on the CPU: a sentence, a streamed window, no stream start yet, the room off\n" );
	chan_play( a, &s_sfx[0], 100, 100, 0 );
	a->words = words;
	chunk( 1000 + 20480, list, 1, NULL );
	CHECK( a->engine_reserved[7] == 0 && a->engine_reserved[3] == 1 );
	win->flags |= SOUND_R3D_WINDOW;
	wsfx.cache = win;
	chan_play( a, &wsfx, 100, 100, 0 );
	chunk( 1000 + 20480, list, 1, NULL );
	CHECK( a->engine_reserved[7] == 0 );
	/* no known stream start: plays at once, still on a voice */
	s_base_valid = 0;
	chan_play( a, &s_sfx[0], 100, 100, 0 );
	chunk( 1000 + 21504, list, 1, NULL );
	CHECK( a->engine_reserved[7] == 1 && state_of( Xbox_SndVoices_DebugSource( a )) == AL_PLAYING );
	s_base_valid = 1;
	Xbox_SndVoices_Free( a );
	free( win );
	printf( "    -> sentence, window, untimed start [%s]\n", failures > f0 ? "FAIL" : "PASS" );
}

static void test_free_sound_clear_full( void )
{
	channel_t *list[70];
	ALuint s;
	int i, on = 0;
	const int f0 = failures;
	char line[512];

	printf( "[5] S_FreeSound, the clear, the sources running out\n" );
	tick( 22000 ); /* past [4]'s timed stop (stream frame 20504): its voice is reaped below */
	chan_play( &s_ch[5], &s_sfx[3], 100, 100, 0 );
	list[0] = &s_ch[5];
	chunk( 1000 + 22528, list, 1, NULL );
	s = Xbox_SndVoices_DebugSource( &s_ch[5] );
	Xbox_SndVoices_FreeSound( &s_sfx[3] );                    /* its samples are going */
	CHECK( geti( s, AL_BUFFER ) == 0 && s_ch[5].engine_reserved[3] == 1 );
	chunk( 1000 + 23552, list, 1, NULL );
	CHECK( s_ch[5].engine_reserved[7] == 0 );                 /* on the CPU now */
	Xbox_SndVoices_Free( &s_ch[5] );

	for( i = 0; i < 70; i++ )
	{
		chan_play( &s_ch[i], &s_sfx[0], 100, 100, 0 );
		list[i] = &s_ch[i];
	}
	chunk( 1000 + 24576, list, 70, NULL );
	for( i = 0; i < 70; i++ )
		on += (int)s_ch[i].engine_reserved[7];
	CHECK( on == 64 );                                         /* VOICE_SLOTS; the rest mix on the CPU */
	if( on != 64 )
		printf( "    %d on voices\n", on );
	Xbox_SndVoices_Stats( line, sizeof( line ));
	printf( "%s", line );
	CHECK( strstr( line, "peak 64 of 64" ) != NULL );
	Xbox_SndVoices_Clear();
	for( i = 0; i < 70; i++ )
		CHECK( Xbox_SndVoices_DebugSource( &s_ch[i] ) == 0 );
	/* the room leaves the GP: everything back to the CPU */
	chan_play( &s_ch[0], &s_sfx[0], 100, 100, 0 );
	s_room_on = 0;
	chunk( 1000 + 25600, list, 1, NULL );
	CHECK( s_ch[0].engine_reserved[7] == 0 && !Xbox_SndVoices_Active());
	printf( "    -> buffer dropped before the samples, clear, 64 sources, room off [%s]\n", failures > f0 ? "FAIL" : "PASS" );
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
	Xbox_SndVoices_Init();
	CHECK( Xbox_SndVoices_Active());
	CHECK( Xbox_SndVoices_StreamPriority() == 1000.0f );
	test_start();
	test_end_and_cut();
	test_reuse_pause_lost();
	test_cpu_cases();
	test_free_sound_clear_full();
	Xbox_SndVoices_Shutdown();
	al_source_debug_clock( false, 0u, 0u, false );
	alcMakeContextCurrent( NULL );
	alcDestroyContext( ctx );
	alcCloseDevice( dev );
	printf( "xbox_snd_voices: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
