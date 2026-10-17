/*
xbox_snd_al.c - the Xbox's sound output through OpenAL on the MCPX APU
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
Resonance3D (docs/r3d/divergences.md #178, X12 item 5a: OpenAL replaces
only the output). The engine still mixes every channel into snd.buffer;
the painted part of that ring goes, once per update, into OpenAL buffers
queued on one streaming source, the recipe of nxdk-ex's
lib/openal/docs/ENGINE_INTEGRATION.md 3.1. The library plays the source on
an APU voice and its own thread moves the APU's mix to the AC97 every
4 ms, so SDL's audio thread, its 1024-frame callbacks and its device lock
are gone; what the main thread does instead (the copies, the voice tick)
is timed as snd_dma_kc, and the library's thread as snd_thr_kc.

The mixer's DMA position follows the buffers the source has played
(xbox_snd_al_queue.h): BeginPainting, the first thing S_UpdateChannels
does before it reads the clock, collects them; Submit, after the paint,
queues what was painted up to snd.paintedtime. Both nest (S_TransferPaintBuffer
calls them inside S_UpdateChannels' own pair); the work is done at the
outermost level only.

The device opens once per boot: r3d_snd_rate's live reopen is refused
while OpenAL is the output (s_main.c), as a reopen of the APU has not been
verified on a console.
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_al.h"
#include "platform/xbox/xbox_snd_al_queue.h"
#include "platform/xbox/xbox_frametime.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

/* A buffer holds 256 to 2048 frames (5.3 to 43 ms at 48 kHz): an update
   queues what was painted since the last one, ~one frame of the game, in
   one buffer, or two across the ring's end. Less than 256 waits for the
   next update. With _snd_mixahead's 0.12 s ahead, 3 to 25 buffers are in
   flight; the library takes 64 on a source. */
#define XBOX_SND_AL_MIN_FRAMES 256
#define XBOX_SND_AL_MAX_FRAMES 2048
#define XBOX_SND_AL_POOL_BYTES ( 64 * 1024 )

int xbox_snd_want_al;

static int s_al_active;
static ALCdevice *s_al_device;
static ALCcontext *s_al_context;
static ALuint s_al_source;
static ALuint s_al_buffers[XBOX_SND_AL_MAX_BUFFERS];
static ALuint s_al_free[XBOX_SND_AL_MAX_BUFFERS];
static int s_al_nfree;
static xbox_snd_al_queue_t s_al_queue;
static int s_al_depth;     /* BeginPainting's nesting */
static qboolean s_al_paused;
static int s_al_starved;   /* the source ran out of data and stopped */
static qboolean s_al_cleared; /* stopped by Xbox_SndAL_Clear, not by running dry */
static char s_al_backend_name[64];
static char s_al_why[96]; /* the step XboxAL_Init failed at, for sndbackend.txt */

int Xbox_SndAL_Active( void )
{
	return s_al_active;
}

/* sndalprof.txt: where the main thread's output work (snd_dma_kc) goes,
   call by call, one line per window of updates and one at each clear (a
   map change). The console A/B (2026-10-16) found that work at 0.24 ms a
   frame against SDL's 0.006; this splits it. Cycles from rdtsc, always on
   under OpenAL (a few reads per update). A window's line goes to memory;
   the file is written only at a clear (a map change, inside the load) and at
   shutdown, so play never waits on the disk. */
enum
{
	ALP_RETIRE,  /* AL_BUFFERS_PROCESSED and the unqueues */
	ALP_UPLOAD,  /* alBufferData: the library's allocation and copy */
	ALP_QUEUE,   /* alSourceQueueBuffers */
	ALP_STATE,   /* AL_SOURCE_STATE, and a restart when it ran dry */
	ALP_UPDATE,  /* alXboxUpdateVoices */
	ALP_COUNT
};
static const char *const s_alp_name[ALP_COUNT] = { "retire", "upload", "queue", "state", "update" };
#define XBOX_SND_AL_PROF_WINDOW 1800 /* updates, ~30 s at 60 fps */

static struct
{
	uint64_t cycles[ALP_COUNT];
	uint32_t max[ALP_COUNT];    /* the longest single call of each */
	uint32_t calls[ALP_COUNT];
	uint32_t updates;           /* outermost Submits */
	uint64_t update_total;      /* BeginPainting's retire plus Submit, per update */
	uint32_t update_max;
	uint32_t frames;            /* sample frames uploaded */
	uint32_t window;            /* windows closed since the open */
} s_alp;
static char s_alp_text[8192]; /* closed windows' lines not yet in the file */
static size_t s_alp_len;
static uint32_t s_alp_dropped;  /* windows lost to a full s_alp_text */
static uint32_t s_alp_failed;   /* writes that could not open the file */
static qboolean s_alp_started;  /* the file has been created since the open */
static uint64_t s_alp_begin; /* this update's BeginPainting, for update_total */
static ALXBOXSTATS s_alp_st0; /* the library's counters at the window's start */
static uint64_t s_al_last_tick; /* rdtsc of the last alXboxUpdateVoices */
#define XBOX_SND_AL_TICK_CYCLES ( 733333u * 12u ) /* 12 ms: one tick a 60 fps frame, wherever its two updates fall */

static void XboxAL_ProfAdd( int phase, uint64_t t0 )
{
	const uint64_t d = xbox_ft_rdtsc() - t0;

	s_alp.cycles[phase] += d;
	s_alp.calls[phase]++;
	if( d > s_alp.max[phase] )
		s_alp.max[phase] = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
}

/* Appends to the window's line in t (size sz), keeping it terminated */
static void XboxAL_ProfCat( char *t, size_t sz, size_t *len, const char *fmt, ... ) FORMAT_CHECK( 4 );
static void XboxAL_ProfCat( char *t, size_t sz, size_t *len, const char *fmt, ... )
{
	va_list args;
	int n;

	if( *len >= sz - 1 )
		return;
	va_start( args, fmt );
	n = Q_vsnprintf( t + *len, sz - *len, fmt, args );
	va_end( args );
	*len = ( n < 0 || *len + (size_t)n > sz - 1 ) ? sz - 1 : *len + (size_t)n;
}

/* The kept lines to sndalprof.txt; on a failed open they stay for the next
   write, and the file says how many writes failed or windows were lost */
static void XboxAL_ProfFlush( void )
{
	file_t *f;

	if( !s_alp_len && !s_alp_dropped )
		return;
	f = FS_Open( "sndalprof.txt", s_alp_started ? "a" : "w", true );
	if( !f )
	{
		s_alp_failed++;
		return;
	}
	if( !s_alp_started )
		FS_Printf( f, "# us per update (mean), calls per update, longest call us; total = BeginPainting + Submit per update\n" );
	if( s_alp_failed || s_alp_dropped )
		FS_Printf( f, "# %u write(s) failed before this one; %u window(s) lost to a full buffer\n", s_alp_failed, s_alp_dropped );
	FS_Write( f, s_alp_text, s_alp_len );
	FS_Close( f );
	s_alp_started = true;
	s_alp_len = 0;
	s_alp_failed = s_alp_dropped = 0;
}

/* One line per window: per update, the mean us of each call kind (733.33
   MHz), the calls per update, and the longest single call. Every window
   moves the baselines on, written or not, so each line covers its own. */
static void XboxAL_ProfWrite( const char *why, qboolean flush )
{
	const double us = 1.0 / 733.333;
	ALXBOXSTATS st;
	char t[1024];
	size_t len = 0;
	int i;

	if( s_alp.updates )
	{
		XboxAL_ProfCat( t, sizeof( t ), &len, "%s window %u: %u updates, %.0f frames/upload, total %.1f us (max %.1f)",
			why, s_alp.window, s_alp.updates,
			s_alp.calls[ALP_UPLOAD] ? (double)s_alp.frames / s_alp.calls[ALP_UPLOAD] : 0.0,
			s_alp.update_total * us / s_alp.updates, s_alp.update_max * us );
		for( i = 0; i < ALP_COUNT; i++ )
			XboxAL_ProfCat( t, sizeof( t ), &len, "; %s %.1f x%.2f max %.1f", s_alp_name[i], s_alp.cycles[i] * us / s_alp.updates,
				(double)s_alp.calls[i] / s_alp.updates, s_alp.max[i] * us );
		memset( &st, 0, sizeof( st ));
		st.size = sizeof( st );
		alXboxGetStats( &st );
		XboxAL_ProfCat( t, sizeof( t ), &len, "; library update_calls %u update_max %.1f", st.update_calls, st.update_max_cycles * us );
		if( st.update_calls > s_alp_st0.update_calls )
		{
			// the voice tick per call: the streams brought up to date, the
			// voice manager with the APU service (the service also runs on a
			// voice start or stop, so its share can exceed the tick's)
			const double n = st.update_calls - s_alp_st0.update_calls;

			XboxAL_ProfCat( t, sizeof( t ), &len, " (per call: streams %.1f, voices %.1f, of it the APU service %.1f)",
				( st.update_stream_cycles - s_alp_st0.update_stream_cycles ) * us / n,
				( st.update_voice_cycles - s_alp_st0.update_voice_cycles ) * us / n,
				( st.update_service_cycles - s_alp_st0.update_service_cycles ) * us / n );
		}
		XboxAL_ProfCat( t, sizeof( t ), &len, "\n" );
		if( st.output_passes > s_alp_st0.output_passes )
		{
			// the output thread, per pass: the APU's mix read, its copy to
			// the codec, the stream scrub, and the rest (the codec's
			// registers, the pass itself)
			const double n = st.output_passes - s_alp_st0.output_passes;
			const double fifo = ( st.output_fifo_cycles - s_alp_st0.output_fifo_cycles ) * us / n;
			const double queue = ( st.output_queue_cycles - s_alp_st0.output_queue_cycles ) * us / n;
			const double scrub = ( st.output_scrub_cycles - s_alp_st0.output_scrub_cycles ) * us / n;
			const double total = ( st.output_cycles - s_alp_st0.output_cycles ) * us / n;

			XboxAL_ProfCat( t, sizeof( t ), &len, "  thread: %.2f passes/update, per pass %.1f us: fifo %.1f, queue %.1f, scrub %.1f, rest %.1f\n",
				n / s_alp.updates, total, fifo, queue, scrub, total - fifo - queue - scrub );
		}
		s_alp_st0 = st;
		if( s_alp_len + len <= sizeof( s_alp_text ))
		{
			memcpy( s_alp_text + s_alp_len, t, len );
			s_alp_len += len;
		}
		else s_alp_dropped++;

		memset( s_alp.cycles, 0, sizeof( s_alp.cycles ));
		memset( s_alp.max, 0, sizeof( s_alp.max ));
		memset( s_alp.calls, 0, sizeof( s_alp.calls ));
		s_alp.updates = s_alp.update_max = s_alp.frames = 0;
		s_alp.update_total = 0;
		s_alp.window++;
	}
	if( flush )
		XboxAL_ProfFlush();
}

/* Buffers the source has played: back to the free list, and the clock
   moves past them */
static void XboxAL_Retire( void )
{
	const uint64_t t0 = xbox_ft_rdtsc();
	ALint n = 0;

	alGetSourcei( s_al_source, AL_BUFFERS_PROCESSED, &n );
	while( n-- > 0 )
	{
		ALuint b = 0;

		alSourceUnqueueBuffers( s_al_source, 1, &b );
		if( alGetError() != AL_NO_ERROR )
			break;
		s_al_free[s_al_nfree++] = b;
		Xbox_SndALQ_Retire( &s_al_queue );
	}
	snd.samplepos = s_al_queue.played * 2;
	XboxAL_ProfAdd( ALP_RETIRE, t0 );
}

/* One buffer of the ring onto the source: nonzero only when the upload and
   the queueing both succeeded. Each call's error is read once, right after
   it: alGetError clears it, so a second read would hide a failed upload
   (sample memory exhausted) and commit a buffer that was never queued. */
static int XboxAL_Submit1( void *user, int pos, int frames )
{
	uint64_t t0;
	ALuint b;
	ALenum err;

	(void)user;
	if( s_al_nfree <= 0 )
		return 0;
	b = s_al_free[--s_al_nfree];
	t0 = xbox_ft_rdtsc();
	alBufferData( b, AL_FORMAT_STEREO16, snd.buffer + pos * 4, frames * 4, snd.format.speed );
	err = alGetError();
	XboxAL_ProfAdd( ALP_UPLOAD, t0 );
	s_alp.frames += frames;
	if( err == AL_NO_ERROR )
	{
		t0 = xbox_ft_rdtsc();
		alSourceQueueBuffers( s_al_source, 1, &b );
		err = alGetError();
		XboxAL_ProfAdd( ALP_QUEUE, t0 );
	}
	if( err != AL_NO_ERROR )
	{
		s_al_free[s_al_nfree++] = b;
		return 0;
	}
	return 1;
}

/* The painted, unqueued part of the ring onto the source */
static void XboxAL_Queue( void )
{
	Xbox_SndALQ_Fill( &s_al_queue, snd.paintedtime, XBOX_SND_AL_MIN_FRAMES, XBOX_SND_AL_MAX_FRAMES,
		XboxAL_Submit1, NULL );
}

/* S_ClearBuffer zeroes snd.buffer, which silences SDL's thread at once:
   it reads the ring. The source plays copies, so they are dropped here
   too, or up to _snd_mixahead of the sound before the clear (a demo, a
   video, the last map) would still play. */
void Xbox_SndAL_Clear( void )
{
	int i;

	if( !s_al_active || !s_al_source )
		return;
	XboxAL_ProfWrite( "clear", true );
	alSourceStop( s_al_source );
	alSourcei( s_al_source, AL_BUFFER, 0 ); // detaches every queued buffer
	alGetError();
	for( i = 0; i < XBOX_SND_AL_MAX_BUFFERS; i++ )
		s_al_free[i] = s_al_buffers[i];
	s_al_nfree = XBOX_SND_AL_MAX_BUFFERS;
	Xbox_SndALQ_Flush( &s_al_queue );
	snd.samplepos = s_al_queue.played * 2;
	s_al_cleared = true; // the stop that follows is not the source running dry
}

static void XboxAL_Shutdown( void )
{
	if( s_al_source )
	{
		alSourceStop( s_al_source );
		alSourcei( s_al_source, AL_BUFFER, 0 );
		alDeleteSources( 1, &s_al_source );
		s_al_source = 0;
	}
	if( s_al_buffers[0] )
	{
		alDeleteBuffers( XBOX_SND_AL_MAX_BUFFERS, s_al_buffers );
		memset( s_al_buffers, 0, sizeof( s_al_buffers ));
	}
	if( s_al_context )
	{
		alcMakeContextCurrent( NULL );
		alcDestroyContext( s_al_context );
		s_al_context = NULL;
	}
	if( s_al_device )
	{
		alcCloseDevice( s_al_device );
		s_al_device = NULL;
	}
	if( snd.buffer )
	{
		Mem_Free( snd.buffer );
		snd.buffer = NULL;
	}
	s_al_nfree = 0;
	s_al_depth = 0;
}

static qboolean XboxAL_Init( void )
{
	ALint backend = 0;
	int samplecount;

	s_al_why[0] = 0;
	// the queued buffers stay in CPU memory (nxdk-ex AL_XBOX_pool_size): the
	// APU pool holds only the voice table and the ITD buffers (32 KiB; the
	// stream's ring is a block of its own), so 64 KiB instead of the 2 MiB
	// default (the console A/B: 2.4 MiB less free than SDL). X12 5b (sounds
	// on voices, their APU copies in the pool) raises it.
	alXboxSetPoolSize( XBOX_SND_AL_POOL_BYTES );
	s_al_device = alcOpenDevice( NULL );
	if( !s_al_device )
	{
		Q_strncpy( s_al_why, "alcOpenDevice returned NULL", sizeof( s_al_why ));
		Con_Printf( S_ERROR "Audio: OpenAL: no device\n" );
		return false;
	}
	s_al_context = alcCreateContext( s_al_device, NULL );
	if( !s_al_context || !alcMakeContextCurrent( s_al_context ))
	{
		Q_snprintf( s_al_why, sizeof( s_al_why ), "no context (alcGetError 0x%x)", alcGetError( s_al_device ));
		Con_Printf( S_ERROR "Audio: OpenAL: no context\n" );
		goto fail;
	}

	// the null backend runs the library on a RAM model of the APU: no sound
	alXboxGetHardwareStatus( AL_XBOX_BACKEND, &backend );
	if( backend != AL_XBOX_BACKEND_MMIO )
	{
		Q_snprintf( s_al_why, sizeof( s_al_why ), "not on the APU (AL_XBOX_BACKEND %d)", backend );
		Con_Printf( S_ERROR "Audio: OpenAL: not on the APU (backend %d)\n", backend );
		goto fail;
	}

	alGetError();
	alGenSources( 1, &s_al_source );
	alGenBuffers( XBOX_SND_AL_MAX_BUFFERS, s_al_buffers );
	if( alGetError() != AL_NO_ERROR )
	{
		Q_strncpy( s_al_why, "alGenSources/alGenBuffers failed", sizeof( s_al_why ));
		Con_Printf( S_ERROR "Audio: OpenAL: no source or buffers\n" );
		goto fail;
	}
	// the mix is already stereo and panned: direct mode, both outputs at
	// full level, no 3D model, no reverb send (its default). A relative
	// source at the origin instead goes through the library's 5.1 pan,
	// which spreads it evenly over four speakers and folds that to stereo
	// 3 dB down (apu_spatial.c, apu_voice.c): the console A/B's OpenAL side
	// played everything 3 dB quieter than SDL.
	{
		const ALfloat unity[2] = { 1.0f, 1.0f };

		alSourcefv( s_al_source, AL_XBOX_DIRECT_GAINS, unity );
	}
	if( alGetError() != AL_NO_ERROR )
	{
		Q_strncpy( s_al_why, "AL_XBOX_DIRECT_GAINS failed", sizeof( s_al_why ));
		Con_Printf( S_ERROR "Audio: OpenAL: no direct mode\n" );
		goto fail;
	}
	for( s_al_nfree = 0; s_al_nfree < XBOX_SND_AL_MAX_BUFFERS; s_al_nfree++ )
		s_al_free[s_al_nfree] = s_al_buffers[s_al_nfree];

	// the ring as SDL's SNDDMA_Init makes it
	snd.format.speed    = SOUND_DMA_SPEED;
	snd.format.channels = 2;
	snd.format.width    = 2;
	samplecount = s_samplecount.value;
	if( !samplecount )
		samplecount = 0x8000;
	snd.samples         = samplecount * 2;
	snd.buffer          = Mem_Calloc( sndpool, snd.samples * 2 );
	snd.samplepos       = 0;

	Xbox_SndALQ_Init( &s_al_queue, snd.samples / 2, 0 );
	memset( &s_alp, 0, sizeof( s_alp ));
	s_alp_len = 0;
	s_alp_dropped = s_alp_failed = 0;
	s_alp_started = false;
	memset( &s_alp_st0, 0, sizeof( s_alp_st0 )); // the library's counters start at the open
	s_al_last_tick = 0;
	s_alp_begin = 0;
	s_al_depth = 0;
	s_al_paused = false;
	s_al_starved = 0;

	Con_Printf( "Using OpenAL audio: %s @ %d Hz\n", alGetString( AL_RENDERER ), SOUND_DMA_SPEED );
	Q_snprintf( s_al_backend_name, sizeof( s_al_backend_name ), "OpenAL (%s)", alGetString( AL_RENDERER ));
	snd.initialized = true;
	snd.backend_name = s_al_backend_name;
	return true;

fail:
	XboxAL_Shutdown();
	return false;
}

static void XboxAL_BeginPainting( void )
{
	xbox_ft_span_t span;

	uint64_t t0;

	if( s_al_depth++ > 0 || !s_al_source )
		return;
	t0 = xbox_ft_rdtsc();
	xbox_ft_span_begin( &span );
	XboxAL_Retire();
	xbox_ft_ticks_add( &span, &xbox_ft_snd_dma_ticks );
	s_alp_begin = xbox_ft_rdtsc() - t0;
}

static void XboxAL_Submit( void )
{
	xbox_ft_span_t span;
	ALint state = AL_PLAYING;
	uint64_t t0, t1, d;

	if( s_al_depth > 0 )
		s_al_depth--;
	if( s_al_depth > 0 || !s_al_source )
		return;
	t0 = xbox_ft_rdtsc();
	xbox_ft_span_begin( &span );
	XboxAL_Queue();
	t1 = xbox_ft_rdtsc();
	if( !s_al_paused && s_al_queue.count > 0 )
	{
		alGetSourcei( s_al_source, AL_SOURCE_STATE, &state );
		if( state != AL_PLAYING )
		{
			// it played everything it had (or has not started): a play
			// restarts the whole queue, so the played buffers go first
			if( state == AL_STOPPED && !s_al_cleared )
				s_al_starved++;
			s_al_cleared = false;
			XboxAL_Retire();
			if( s_al_queue.count > 0 )
				alSourcePlay( s_al_source );
		}
	}
	XboxAL_ProfAdd( ALP_STATE, t1 );
	t1 = xbox_ft_rdtsc();
	// The voice tick once a frame, not at both of a frame's updates (the
	// mix's and the extra paint pass): the stream itself is brought up to
	// date by the queries above. By time, so loads (extra paints, no
	// frames) still tick.
	if( t1 - s_al_last_tick >= XBOX_SND_AL_TICK_CYCLES )
	{
		s_al_last_tick = t1;
		alXboxUpdateVoices();
		XboxAL_ProfAdd( ALP_UPDATE, t1 );
	}
	xbox_ft_ticks_add( &span, &xbox_ft_snd_dma_ticks );

	d = xbox_ft_rdtsc() - t0 + s_alp_begin;
	s_alp_begin = 0;
	s_alp.update_total += d;
	if( d > s_alp.update_max )
		s_alp.update_max = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
	if( ++s_alp.updates >= XBOX_SND_AL_PROF_WINDOW )
		XboxAL_ProfWrite( "play", false ); // to memory: play does not touch the disk
}

static void XboxAL_Activate( qboolean active )
{
	if( !snd.initialized || !s_al_source )
		return;
	s_al_paused = !active;
	if( !active )
		alSourcePause( s_al_source );
	else if( s_al_queue.count > 0 )
		alSourcePlay( s_al_source );
}

void Xbox_Snd_ThreadStats( xbox_audio_thread_stats_t *out )
{
	ALXBOXSTATS st;

	if( !s_al_active )
	{
		XAudioR3D_ThreadStats( out );
		return;
	}
	memset( &st, 0, sizeof( st ));
	st.size = sizeof( st );
	alXboxGetStats( &st );
	out->cycles = (uint32_t)st.output_cycles;
	out->runs = st.output_passes;
	out->buffers = st.ac97_buffers;
	out->underruns = st.ac97_underruns;
}

/*
=======================================================================
upstream's SNDDMA_*, sent to the output chosen at boot
=======================================================================
*/
/* sndbackend.txt in the game's write directory: the output asked for and
   the one that opened, and why OpenAL did not. An XBE has no command line
   and so no engine.log; this is how a console run or xemu's disk says
   which side an A/B ran and what failed (read it over FTP, or with
   tools/xemu_hdd.py). Rewritten at every open. */
static void Xbox_Snd_WriteBackendNote( qboolean opened )
{
	file_t *f = FS_Open( "sndbackend.txt", "w", true );

	if( !f )
		return;
	FS_Printf( f, "asked %s\nopened %s\n", xbox_snd_want_al ? "al" : "sdl",
		!opened ? "none" : s_al_active ? "al" : "sdl" );
	if( xbox_snd_want_al && !s_al_active )
		FS_Printf( f, "openal failed: %s\n", s_al_why[0] ? s_al_why : "unknown" );
	if( s_al_active )
		FS_Printf( f, "renderer %s @ %d Hz\n", alGetString( AL_RENDERER ), SOUND_DMA_SPEED );
	FS_Close( f );
}

qboolean SNDDMA_Init( void )
{
	qboolean ok;

	s_al_active = false;
	if( xbox_snd_want_al )
	{
		if( XboxAL_Init( ))
		{
			s_al_active = true;
			Xbox_Snd_WriteBackendNote( true );
			return true;
		}
		Con_Printf( S_WARN "Audio: OpenAL did not open (%s), using SDL\n", s_al_why );
	}
	ok = SDLash_SNDDMA_Init();
	Xbox_Snd_WriteBackendNote( ok );
	return ok;
}

void SNDDMA_Shutdown( void )
{
	if( !s_al_active )
	{
		SDLash_SNDDMA_Shutdown();
		return;
	}
	Con_Printf( "Shutting down audio (OpenAL, the source ran dry %d times).\n", s_al_starved );
	XboxAL_ProfWrite( "shutdown", true );
	snd.initialized = false;
	XboxAL_Shutdown();
	s_al_active = false;
}

void SNDDMA_BeginPainting( void )
{
	if( s_al_active )
		XboxAL_BeginPainting();
	else
		SDLash_SNDDMA_BeginPainting();
}

void SNDDMA_Submit( void )
{
	if( s_al_active )
		XboxAL_Submit();
	else
		SDLash_SNDDMA_Submit();
}

void SNDDMA_Activate( qboolean active )
{
	if( s_al_active )
		XboxAL_Activate( active );
	else
		SDLash_SNDDMA_Activate( active );
}
