/*
xbox_snd_feed.c - Half-Life's speech fed onto the APU's voices (X12 step 5b, stage B1)
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
Resonance3D (docs/r3d/divergences.md #184; the design is
docs/r3d/design/x9-audio-apu-voices.md, section 7).

A feed is one of FEED_VOICES streaming OpenAL sources made at Init, set up
as stage A's voices are (#182): direct mode, its direct path muted by a
lowpass at gain 0, its send at 1.0 into the room kernel's slot, panned by
the direct gains, so the kernel's input is the channel weighted per ear as
S_MixChannelToBuffer would have mixed it into the room buffer.

A speech channel (a sentence, or a sound #177 streams from a window) takes
a feed the first time the walk reaches a span of it. From then on the walk
runs as upstream's - S_RetrieveAudioSamples (the window or the cache),
S_AdjustNumSamples, the trims, VOX_LoadWord, the mouth - and where
S_MixChannelToBuffer would call S_MixAudio, Xbox_SndFeed_Take copies the
span's source samples, floor( sample_frac + out_count * rate ) of them from
the walk's position, into the feed's staging buffer instead. The next span
starts where the walk then is, so timecompress's skip, the trims, a word's
end and the next word's start are in the data as upstream mixes them. The
staged samples go onto the source as one buffer every FEED_BATCH_MS of
output (and at once for a start, an end, or a channel the mixer skipped).

The source's first data starts at the room stream's frame for its span's
paint time (alXboxSourcePlayAtSample); after that the data is gapless. The
channel's volumes and pitch, when they change, go to the source at the
stream frame of the chunk being painted (alXboxSourcefvAtSample): upstream's
own granularity (pitch per paint call, volume per frame) on the data's
clock. The APU resamples at the buffer's rate times the pitch, which is
upstream's rate.

A channel freed because it ended plays out what it was fed; one freed early
stops at the frame the mixer is at. A channel the mixer skips feeds nothing
and its source runs dry; walked again, the source starts again at that
chunk's frame. A source found stopped while the stream still has its data
ahead lost its voice: the channel mixes on the CPU from the walk's position
(the 0.12 s it held is lost). A next word of another rate or width, a
channel with no feed free, and stereo or other widths stay on the CPU.

The channel holds its feed as a token in engine_reserved[4]; [5] marks a
channel that stays on the CPU (#177 keeps [0..1], #182 [2..3]).
*/

#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_feed.h"
#include "platform/xbox/xbox_snd_voices.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_vox_stream.h"
#include "platform/xbox/xbox_frametime.h"
#include "platform/xbox/openal/al_limits.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#define FEED_VOICES    8     /* speech channels on sources at once (the design's FEED_VOICES) */
#define FEED_BUFS      8     /* buffers in flight on one source: 0.38 s of 48 ms pieces */
#define FEED_STAGE     8192  /* bytes staged before they go onto the source */
#define FEED_BATCH_MS  48    /* output time a piece covers (FEED_BATCH_MS in the design, 7.5) */
#define TOKEN_FEED     4     /* channel_t.engine_reserved[] */
#define TOKEN_FEED_CPU 5
#define CPU_MARK       1u
#define FEED_PRIORITY  2.0f  /* above every sound voice (0..1), below the streams (1000) */

/* the library's pool holds them beside the 64 sound voices, the mix, the
   room stream and the music (al_limits.h), and their buffer names beside
   the voices', the streams' and the music's */
typedef char feed_slots_fit_the_pool[( FEED_VOICES + 64 + 3 <= R3D_AL_SOURCES ) ? 1 : -1];
typedef char feed_bufs_fit_the_pool[( FEED_VOICES * FEED_BUFS + 64 + 2 * 32 + 512 <= R3D_AL_BUFFERS ) ? 1 : -1];

typedef enum
{
	F_FREE,    /* no channel */
	F_OWNED,   /* its channel's */
	F_ORPHAN   /* its channel is gone: playing out, or waiting for a timed stop */
} fstate_t;

typedef struct
{
	ALuint src;
	fstate_t state;
	channel_t *ch;
	uintptr_t token;
	int rate, width;           /* the format the source holds */
	ALuint names[FEED_BUFS];
	ALuint free_names[FEED_BUFS];
	int n_free;
	byte stage[FEED_STAGE];
	int staged;                /* bytes staged */
	int staged_out;            /* output samples they cover */
	int stage_offset;          /* the chunk offset of the first staged sample (a start's frame) */
	qboolean started;          /* given a start since the source last stopped */
	qboolean pending;          /* its timed start has not been seen acting yet (AL_INITIAL) */
	qboolean have_end;         /* end_frame is known (the streams run from a known start) */
	unsigned end_frame;        /* the room stream frame where what it was fed ends */
	int paint;                 /* the chunk it was last set up in */
	short lv, rv;              /* the volumes last sent */
	float pitch;               /* the pitch last sent */
} feed_t;

static qboolean s_on;
static feed_t s_feed[FEED_VOICES];
static ALuint s_filter;
static uintptr_t s_next_token = 1;
static int s_start;   /* the chunk's frame time */
static int s_paint;   /* counts the chunks */
static qboolean s_painting; /* between BeginPaint and EndPaint */
static int s_batch;   /* FEED_BATCH_MS in output samples */
static int s_lost_margin;
static uint32_t s_n_starts, s_n_now, s_n_dry, s_n_lost, s_n_format, s_n_full, s_n_stops, s_n_pieces, s_n_dropped, s_n_updates;
static uint64_t s_bytes, s_cycles;
static char s_why[96] = "off: not set up (OpenAL did not open)";
static int s_dbg_fail_uploads; /* tests: the next uploads fail (Xbox_SndFeed_DebugFailUploads) */
static int s_peak;

static feed_t *Feed_Of( const channel_t *ch )
{
	uintptr_t t = ch->engine_reserved[TOKEN_FEED];
	feed_t *f;

	if( !t )
		return NULL;
	f = &s_feed[( t - 1 ) % FEED_VOICES];
	return ( f->state == F_OWNED && f->ch == ch && f->token == t ) ? f : NULL;
}

/* The buffers the source has played back to the free list */
static void Feed_Reclaim( feed_t *f )
{
	ALint done = 0;

	alGetSourcei( f->src, AL_BUFFERS_PROCESSED, &done );
	while( done-- > 0 && f->n_free < FEED_BUFS )
	{
		ALuint b = 0;

		alSourceUnqueueBuffers( f->src, 1, &b );
		if( alGetError() != AL_NO_ERROR || !b )
			break;
		f->free_names[f->n_free++] = b;
	}
}

/* Stopped, its queue back, no channel */
static void Feed_Release( feed_t *f )
{
	int i;

	alSourceStop( f->src );
	Feed_Reclaim( f );
	alSourcei( f->src, AL_BUFFER, 0 ); // a stopped source's whole queue is processed: this empties it
	alGetError();
	f->n_free = FEED_BUFS;
	for( i = 0; i < FEED_BUFS; i++ )
		f->free_names[i] = f->names[i];
	if( f->state == F_OWNED && f->ch && f->ch->engine_reserved[TOKEN_FEED] == f->token )
		f->ch->engine_reserved[TOKEN_FEED] = 0;
	f->state = F_FREE;
	f->ch = NULL;
	f->staged = f->staged_out = 0;
	f->started = f->pending = f->have_end = false;
}

/* The staged samples onto the source; the start at the frame of the first
   one when it has none yet */
static void Feed_Flush( feed_t *f )
{
	unsigned frame;
	ALuint b;

	if( !f->staged )
		return;
	Feed_Reclaim( f );
	if( !f->n_free )
	{
		s_n_dropped++; // FEED_BUFS pieces ahead of the voice: it is not playing them
		f->staged = f->staged_out = 0;
		return;
	}
	b = f->free_names[--f->n_free];
	alGetError(); // an earlier call's error must not read as this one's
	// the upload and the queue checked apart: a failed upload is never
	// queued (the name may hold an earlier piece the library would play
	// again), and only a failed queue gives the name back
	alBufferData( b, s_dbg_fail_uploads > 0 ? 0 : f->width == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16, f->stage, f->staged, f->rate );
	if( s_dbg_fail_uploads > 0 )
		s_dbg_fail_uploads--;
	if( alGetError() == AL_NO_ERROR )
	{
		alSourceQueueBuffers( f->src, 1, &b );
		if( alGetError() == AL_NO_ERROR )
			b = 0;
	}
	if( b )
	{
		f->free_names[f->n_free++] = b;
		s_n_dropped++;
		f->staged = f->staged_out = 0;
		return;
	}
	s_n_pieces++;
	s_bytes += (uint64_t)f->staged;
	f->staged = f->staged_out = 0;
	if( !f->started )
	{
		f->pending = false;
		if( Xbox_SndGpRoom_StreamFrame( s_start + f->stage_offset, &frame ))
		{
			alXboxSourcePlayAtSample( f->src, Xbox_SndGpRoom_Source(), frame );
			f->pending = true;
		}
		else
		{
			alSourcePlay( f->src ); // the streams have no start yet: nothing to line up with
			s_n_now++;
		}
		alGetError();
		f->started = true;
		s_n_starts++;
	}
}

/* A pitch or gains change at the stream frame of the chunk being painted;
   at once before the source has a start (its start is timed) */
static void Feed_Set( feed_t *f, ALenum param, const ALfloat *values )
{
	unsigned frame;

	if( f->started && Xbox_SndGpRoom_StreamFrame( s_start, &frame ))
		alXboxSourcefvAtSample( f->src, param, values, Xbox_SndGpRoom_Source(), frame );
	else
		alSourcefv( f->src, param, values );
	s_n_updates++;
}

static void Feed_Params( feed_t *f, const channel_t *ch, double pitch )
{
	const short lv = (short)bound( 0, ch->leftvol, 255 ), rv = (short)bound( 0, ch->rightvol, 255 );
	const float p = (float)pitch;

	if( lv != f->lv || rv != f->rv )
	{
		// S_MixChannelToBuffer's ( sample * vol ) >> 8 on the 16-bit scale
		const ALfloat g[2] = { lv / 256.0f, rv / 256.0f };

		Feed_Set( f, AL_XBOX_DIRECT_GAINS, g );
		f->lv = lv;
		f->rv = rv;
	}
	if( p != f->pitch )
	{
		const ALfloat ap = p > 0.0f ? p : 1.0f / 256.0f;

		Feed_Set( f, AL_PITCH, &ap );
		f->pitch = p;
	}
}

/* Stops at the stream frame the mixer is at (now when the streams have no
   known start), and leaves the channel: between paints, snd.paintedtime,
   as xbox_snd_voices.c's Voice_CutFrame */
static void Feed_StopTimed( feed_t *f )
{
	unsigned frame;

	f->staged = f->staged_out = 0;
	if( f->started && Xbox_SndGpRoom_StreamFrame( s_painting ? s_start : snd.paintedtime, &frame ))
		alXboxSourceStopAtSample( f->src, Xbox_SndGpRoom_Source(), frame );
	else
		alSourceStop( f->src );
	alGetError();
	s_n_stops++;
	if( f->ch && f->ch->engine_reserved[TOKEN_FEED] == f->token )
		f->ch->engine_reserved[TOKEN_FEED] = 0;
	f->state = F_ORPHAN;
	f->ch = NULL;
}

/* What it was fed plays out; the channel goes on elsewhere (or is gone) */
static void Feed_Orphan( feed_t *f )
{
	Feed_Flush( f );
	if( f->ch && f->ch->engine_reserved[TOKEN_FEED] == f->token )
		f->ch->engine_reserved[TOKEN_FEED] = 0;
	f->state = F_ORPHAN;
	f->ch = NULL;
	if( !f->started )
		Feed_Release( f ); // never given data
}

static qboolean Feed_Format( const wavdata_t *wav )
{
	return wav->channels == 1 && ( wav->width == 1 || wav->width == 2 ) && wav->rate > 0;
}

static feed_t *Feed_Start( channel_t *ch, const wavdata_t *wav )
{
	feed_t *f = NULL;
	int i, used = 0;

	for( i = 0; i < FEED_VOICES; i++ )
	{
		if( s_feed[i].state == F_FREE && !f )
			f = &s_feed[i];
		else if( s_feed[i].state != F_FREE )
			used++;
	}
	if( !f )
	{
		s_n_full++;
		return NULL;
	}
	alSourceRewind( f->src ); // AL_INITIAL, its queue processed
	alSourcei( f->src, AL_BUFFER, 0 );
	alSourcei( f->src, AL_LOOPING, AL_FALSE );
	alGetError();
	f->rate = wav->rate;
	f->width = wav->width;
	f->lv = f->rv = -1;
	f->pitch = -1.0f;
	f->staged = f->staged_out = 0;
	f->started = f->pending = f->have_end = false;
	f->paint = s_paint - 1;
	f->state = F_OWNED;
	f->ch = ch;
	f->token = (uintptr_t)( f - s_feed ) + 1 + FEED_VOICES * ( s_next_token++ );
	ch->engine_reserved[TOKEN_FEED] = f->token;
	if( used + 1 > s_peak )
		s_peak = used + 1;
	return f;
}

/* Once a chunk, before the channel's first span in it: a source that
   stopped ran dry (it starts again) or lost its voice (false: the CPU) */
static qboolean Feed_Chunk( feed_t *f, const channel_t *ch, double pitch )
{
	ALint st = AL_PLAYING;

	f->paint = s_paint;
	alGetSourcei( f->src, AL_SOURCE_STATE, &st );
	if( st != AL_INITIAL )
		f->pending = false;
	if( f->started && st == AL_STOPPED )
	{
		unsigned now;

		if( f->have_end && Xbox_SndGpRoom_StreamFrame( snd.soundtime, &now )
			&& (int)( f->end_frame - now ) > s_lost_margin )
		{
			s_n_lost++; // stopped with its data still ahead: the voice was taken
			return false;
		}
		// played all it had: the next span starts it again, from AL_INITIAL
		// as a first start (a stopped source keeps AL_STOPPED while a timed
		// start waits, which would read as stopped again)
		Feed_Reclaim( f );
		alSourceRewind( f->src );
		f->started = false;
		s_n_dry++;
	}
	Feed_Params( f, ch, pitch );
	alGetError();
	return true;
}

static qboolean Feed_Speech( const channel_t *ch, const wavdata_t *wav )
{
	return ch->words || FBitSet( wav->flags, SOUND_R3D_WINDOW | SOUND_R3D_HEADER );
}

qboolean Xbox_SndFeed_Take( channel_t *ch, const wavdata_t *wav, const void *audio, int available,
	double rate, double pitch, int out_count, int offset )
{
	uint64_t t0;
	feed_t *f;
	int n, bytes;
	const byte *src = audio;
	unsigned frame;

	if( !s_on || ch->engine_reserved[TOKEN_FEED_CPU] == CPU_MARK || !Feed_Speech( ch, wav ))
		return false;
	t0 = xbox_ft_rdtsc();
	if( !( f = Feed_Of( ch )))
	{
		if( !Feed_Format( wav ) || !( f = Feed_Start( ch, wav )))
		{
			if( !Feed_Format( wav ))
				s_n_format++;
			ch->engine_reserved[TOKEN_FEED_CPU] = CPU_MARK;
			return false;
		}
	}
	else if( wav->rate != f->rate || wav->width != f->width || wav->channels != 1 )
	{
		// a source holds one format: this word on, the CPU, gaplessly after
		// what the source was fed
		Feed_Orphan( f );
		ch->engine_reserved[TOKEN_FEED_CPU] = CPU_MARK;
		s_n_format++;
		return false;
	}
	if( f->paint != s_paint && !Feed_Chunk( f, ch, pitch ))
	{
		Feed_Release( f );
		ch->engine_reserved[TOKEN_FEED_CPU] = CPU_MARK;
		return false;
	}

	// the source samples the span covers: from the walk's position to
	// where an untimecompressed walk would be
	n = Xbox_SndFeed_SpanFrames( ch->sample, rate, out_count, available );
	if( !f->staged )
		f->stage_offset = offset;
	bytes = n * f->width;
	while( bytes > 0 )
	{
		int c = FEED_STAGE - f->staged;

		if( c <= 0 )
		{
			Feed_Flush( f );
			f->stage_offset = offset;
			continue;
		}
		if( c > bytes )
			c = bytes;
		memcpy( f->stage + f->staged, src, (size_t)c );
		f->staged += c;
		src += c;
		bytes -= c;
	}
	f->staged_out += out_count;
	if(( f->have_end = Xbox_SndGpRoom_StreamFrame( s_start + offset + out_count, &frame )))
		f->end_frame = frame;
	if( !f->started || f->staged_out >= s_batch )
		Feed_Flush( f );
	s_cycles += xbox_ft_rdtsc() - t0;
	return true;
}

void Xbox_SndFeed_BeginPaint( int start )
{
	int i;

	s_start = start;
	s_paint++;
	s_painting = true;
	if( !s_on )
		return;
	if( !Xbox_SndGpRoom_Active())
	{
		Xbox_SndFeed_Clear();
		s_on = false;
		Con_Printf( S_WARN "Audio: speech goes back to the CPU mix (the room left the GP)\n" );
		return;
	}
	for( i = 0; i < FEED_VOICES; i++ )
	{
		feed_t *f = &s_feed[i];
		ALint st = AL_PLAYING;

		if( f->state != F_ORPHAN )
			continue;
		alGetSourcei( f->src, AL_SOURCE_STATE, &st );
		if( st != AL_INITIAL )
			f->pending = false;
		if( st == AL_STOPPED || ( st == AL_INITIAL && !f->pending ))
			Feed_Release( f );
	}
	alGetError();
}

void Xbox_SndFeed_EndPaint( void )
{
	int i;

	if( !s_on )
	{
		s_painting = false;
		return;
	}
	for( i = 0; i < FEED_VOICES; i++ )
	{
		feed_t *f = &s_feed[i];

		if( f->state != F_OWNED || f->paint == s_paint )
			continue;
		if( f->ch->engine_reserved[TOKEN_FEED] != f->token )
			Feed_StopTimed( f ); // the channel was reused without a free
		else
			Feed_Flush( f ); // skipped: what it has plays now, then it runs dry
	}
	alGetError();
	s_painting = false;
}

void Xbox_SndFeed_Free( channel_t *ch )
{
	feed_t *f;

	if( !s_on )
		return;
	if(( f = Feed_Of( ch )))
	{
		if( FBitSet( ch->flags, FL_CHAN_FINISHED | FL_CHAN_SENTENCE_FINISHED ))
			Feed_Orphan( f ); // ended: the walk is ahead of what is heard
		else
			Feed_StopTimed( f );
	}
	ch->engine_reserved[TOKEN_FEED] = 0;
	ch->engine_reserved[TOKEN_FEED_CPU] = 0;
}

void Xbox_SndFeed_Clear( void )
{
	int i;

	for( i = 0; i < FEED_VOICES; i++ )
	{
		if( s_feed[i].state != F_FREE )
			Feed_Release( &s_feed[i] );
	}
}

size_t Xbox_SndFeed_Stats( char *buf, size_t size )
{
	int n, i, used = 0;

	if( !s_on || size == 0 )
		return 0;
	for( i = 0; i < FEED_VOICES; i++ )
		used += s_feed[i].state != F_FREE;
	n = Q_snprintf( buf, size, "  speech fed: %d now, peak %d of %d; %u starts (%u not timed, %u after running dry), %u timed stops, %u to the CPU (%u lost a voice, %u another format, %u no source free); %u pieces, %u KiB, %u dropped; %u volume/pitch updates; the feed %u kc\n",
		used, s_peak, FEED_VOICES, s_n_starts, s_n_now, s_n_dry, s_n_stops, s_n_lost + s_n_format + s_n_full, s_n_lost, s_n_format, s_n_full,
		s_n_pieces, (unsigned)( s_bytes / 1024u ), s_n_dropped, s_n_updates, (unsigned)( s_cycles / 1000u ));
	s_n_starts = s_n_now = s_n_dry = s_n_lost = s_n_format = s_n_full = s_n_stops = s_n_pieces = s_n_dropped = s_n_updates = 0;
	s_bytes = s_cycles = 0;
	s_peak = used;
	return n > 0 ? Q_min( (size_t)n, size - 1 ) : 0;
}

const char *Xbox_SndFeed_Why( void )
{
	return s_on ? "on" : s_why;
}

int Xbox_SndFeed_Active( void )
{
	return s_on;
}

unsigned Xbox_SndFeed_DebugSource( const channel_t *ch )
{
	const feed_t *f = Feed_Of( ch );

	return f ? f->src : 0u;
}

int Xbox_SndFeed_DebugFreeNames( const channel_t *ch )
{
	const feed_t *f = Feed_Of( ch );

	return f ? f->n_free : -1;
}

void Xbox_SndFeed_DebugFailUploads( int n )
{
	s_dbg_fail_uploads = n;
}

void Xbox_SndFeed_Init( void )
{
	const ALuint slot = Xbox_SndGpRoom_Slot();
	ALenum err;
	int i;

	if( s_on )
		return;
	if( !Xbox_SndCfg_Asked( "r3d_snd_voices_speech" ))
	{
		Q_snprintf( s_why, sizeof( s_why ), "off: not asked (r3d_snd_voices_speech 1 in a userconfig.d cfg)" );
		return;
	}
	if( !Xbox_SndGpRoom_Active() || !slot )
	{
		Q_snprintf( s_why, sizeof( s_why ), "off: the room is not on the GP (active %d, slot %u)", Xbox_SndGpRoom_Active(), slot );
		return;
	}
	alGetError();
	alGenFilters( 1, &s_filter );
	alFilteri( s_filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS );
	alFilterf( s_filter, AL_LOWPASS_GAIN, 0.0f );
	if(( err = alGetError()) != AL_NO_ERROR )
	{
		Q_snprintf( s_why, sizeof( s_why ), "failed: the lowpass filter (AL error 0x%x)", err );
		goto fail;
	}
	for( i = 0; i < FEED_VOICES; i++ )
	{
		feed_t *f = &s_feed[i];
		const ALfloat zero[2] = { 0.0f, 0.0f };
		int k;

		memset( f, 0, sizeof( *f ));
		alGenSources( 1, &f->src );
		alGenBuffers( FEED_BUFS, f->names );
		if(( err = alGetError()) != AL_NO_ERROR )
		{
			Q_snprintf( s_why, sizeof( s_why ), "failed: feed %d of %d (AL error 0x%x)", i, FEED_VOICES, err );
			goto fail;
		}
		for( k = 0; k < FEED_BUFS; k++ )
			f->free_names[k] = f->names[k];
		f->n_free = FEED_BUFS;
		// as stage A's voices (#182): only through the kernel, panned
		alSourcefv( f->src, AL_XBOX_DIRECT_GAINS, zero );
		alSourcei( f->src, AL_XBOX_SEND_PANNED, AL_TRUE );
		alSourcei( f->src, AL_DIRECT_FILTER, (ALint)s_filter );
		alSourcei( f->src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE );
		alSource3i( f->src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL );
		alSourcei( f->src, AL_XBOX_VIRTUALIZE, AL_FALSE ); // no voice: AL_STOPPED, and the CPU mixes it
		alSourcef( f->src, AL_XBOX_PRIORITY, FEED_PRIORITY );
		if(( err = alGetError()) != AL_NO_ERROR )
		{
			Q_snprintf( s_why, sizeof( s_why ), "failed: feed %d's setup (AL error 0x%x)", i, err );
			goto fail;
		}
	}
	s_batch = FEED_BATCH_MS * snd.format.speed / 1000;
	s_lost_margin = snd.format.speed / 50; // 20 ms: the stream clock against soundtime
	// the room stream above every source (#182 sets it too, when it is on)
	alSourcef( Xbox_SndGpRoom_Source(), AL_XBOX_PRIORITY, 1000.0f );
	alGetError();
	s_on = true;
	Con_Printf( "Audio: speech is fed to the APU's voices (%d sources)\n", FEED_VOICES );
	return;

fail:
	Con_Printf( S_ERROR "Audio: speech stays in the CPU mix (OpenAL setup failed)\n" );
	Xbox_SndFeed_Shutdown();
}

void Xbox_SndFeed_Shutdown( void )
{
	int i;

	Xbox_SndFeed_Clear();
	for( i = 0; i < FEED_VOICES; i++ )
	{
		feed_t *f = &s_feed[i];

		if( f->src )
			alDeleteSources( 1, &f->src );
		if( f->names[0] )
			alDeleteBuffers( FEED_BUFS, f->names );
		memset( f, 0, sizeof( *f ));
	}
	if( s_filter )
		alDeleteFilters( 1, &s_filter );
	s_filter = 0;
	alGetError();
	s_on = false;
}
