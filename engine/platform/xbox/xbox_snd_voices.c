/*
xbox_snd_voices.c - Half-Life's effects on the APU's voices (X12 step 5b, stage A)
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
Resonance3D (docs/r3d/divergences.md #182; the design is
docs/r3d/design/x9-audio-apu-voices.md, stage A).

A voice is one of VOICE_SLOTS OpenAL sources made at Init, each set up
once as the room stream is (#180): direct mode, its direct path muted by a
lowpass at gain 0, its send at 1.0 into the room kernel's slot, panned by
the direct gains (nxdk-ex AL_XBOX_SEND_PANNED), so the kernel's input is
the channel weighted per ear as S_MixChannelToBuffer weights it into the
room buffer: leftvol / 256 and rightvol / 256 on the 16-bit scale both
sample widths are mixed at. The 8-bit sounds are the cache's own unsigned
bytes (#181), played in place by alBufferDataStatic; each sound gets one
OpenAL buffer, made the first time a voice plays it and deleted before
S_FreeSound frees its samples.

A channel takes a voice the first time it mixes: the source's position is
the channel's (a restored save starts part way in), and it starts with
alXboxSourcePlayAtSample at the room stream's frame for the paint chunk,
so it is heard when the CPU mix would have been. From then on the mixer
walks the channel without mixing it, and each chunk this file hands the
voice the channel's volumes and pitch when they change, timed to the
chunk's stream frame as the start is (alXboxSourcefvAtSample), so a volume
or pan change is heard with the chunk it belongs to. A channel freed
because its sound ended leaves its voice to play out; one freed early
(replaced, stopped) stops at the frame the mixer is at. A channel the
mixer skips (paused by the menu, inaudible) has its voice paused until it
mixes again. A voice found stopped while its channel still plays was
never given hardware, or lost it: the channel then mixes on the CPU from
the walk's position, and stays there.

The channel holds its voice as a token in engine_reserved[2] (#177 keeps
[0..1]); upstream reuses a static channel with a memset and no
S_FreeChannel (SND_PickStaticChannel's replacement case), so every chunk
each voice checks its channel still holds its token. engine_reserved[3]
marks a channel that stays on the CPU.
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_voices.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_vox_stream.h"
#include "platform/xbox/xbox_snd_adpcm.h" // #200: SOUND_R3D_ADPCM
#include "platform/xbox/openal/al_limits.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#define VOICE_SLOTS   64   /* sources; the library's 120 hardware voices are shared with the streams */

/* the library's source pool holds them beside the mix, the room stream and
   the music (al_limits.h): a pool too small fails Init's alGenSources */
typedef char voice_slots_fit_the_pool[( VOICE_SLOTS + 3 <= R3D_AL_SOURCES ) ? 1 : -1];
#define VOICE_BUFS    1024 /* sounds with an OpenAL buffer, open addressing (a map holds 200-300) */
#define TOKEN_SLOT    2    /* channel_t.engine_reserved[] */
#define TOKEN_CPU     3
#define CPU_MARK      1u
#define STREAM_PRIORITY 1000.0f /* the dry and room streams: above every voice (0..1) */

typedef enum
{
	V_FREE,    /* no sound */
	V_OWNED,   /* its channel's: started or waiting for its frame */
	V_ORPHAN   /* its channel is gone: playing out, or waiting for a timed stop */
} vstate_t;

typedef struct
{
	ALuint src;
	vstate_t state;
	channel_t *ch;
	uintptr_t token;
	const sfx_t *sfx;
	ALuint buf;
	short lv, rv;      /* the volumes last sent */
	qboolean loop;     /* AL_LOOPING as last sent */
	float pitch;       /* the pitch last sent */
	qboolean walked;   /* mixed this chunk */
	qboolean paused;   /* paused here (a start still waiting for its frame is not) */
	qboolean pending;  /* its timed start has not been seen acting yet (AL_INITIAL) */
} voice_t;

typedef struct
{
	const sfx_t *sfx;   /* NULL: empty; VBUF_GONE: deleted (keeps probe chains) */
	const wavdata_t *wav;
	ALuint buf;
} vbuf_t;

#define VBUF_GONE ((const sfx_t *)(uintptr_t)1)

static qboolean s_on;
static voice_t s_voice[VOICE_SLOTS];
static vbuf_t s_buf[VOICE_BUFS];
static ALuint s_filter;
static uintptr_t s_next_token = 1;
static int s_start; /* the chunk's frame time */
static qboolean s_painting; /* between BeginPaint and EndPaint */
static uint32_t s_n_starts, s_n_now, s_n_lost, s_n_cpu, s_n_full, s_n_stops, s_n_bufs, s_n_updates, s_n_nobuf;
static char s_why[96] = "off: not set up (OpenAL did not open)"; /* why Init left them off */
static int s_peak;

static voice_t *Voice_Of( const channel_t *ch )
{
	uintptr_t t = ch->engine_reserved[TOKEN_SLOT];
	voice_t *v;

	if( !t )
		return NULL;
	v = &s_voice[( t - 1 ) % VOICE_SLOTS];
	return ( v->state == V_OWNED && v->ch == ch && v->token == t ) ? v : NULL;
}

/* The source back to unused: stopped, no buffer */
static void Voice_Release( voice_t *v )
{
	alSourceStop( v->src );
	alSourcei( v->src, AL_BUFFER, 0 );
	alGetError();
	if( v->state == V_OWNED && v->ch && v->ch->engine_reserved[TOKEN_SLOT] == v->token )
		v->ch->engine_reserved[TOKEN_SLOT] = 0;
	v->state = V_FREE;
	v->ch = NULL;
	v->sfx = NULL;
	v->buf = 0;
	v->paused = false;
	v->pending = false;
}

/* Where a cut lands: in a chunk, its start; between paints - S_StartSound
   replacing the sound, S_StopSound - the frame the mixer painted to,
   snd.paintedtime, where the CPU mix cuts it and where the replacing
   sound starts (the next chunk's start). It was the last chunk's start:
   up to a chunk early, a gap before every repeated weapon shot. */
static int Voice_CutFrame( void )
{
	return s_painting ? s_start : snd.paintedtime;
}

/* Stops at the stream frame the mixer is at (now when the streams have no
   known start), and leaves the channel */
static void Voice_StopTimed( voice_t *v )
{
	unsigned frame;

	if( Xbox_SndGpRoom_StreamFrame( Voice_CutFrame(), &frame ))
		alXboxSourceStopAtSample( v->src, Xbox_SndGpRoom_Source(), frame );
	else
		alSourceStop( v->src );
	alGetError();
	s_n_stops++;
	v->pending = false; // a start still waiting is replaced: it never plays
	if( v->ch && v->ch->engine_reserved[TOKEN_SLOT] == v->token )
		v->ch->engine_reserved[TOKEN_SLOT] = 0;
	v->state = V_ORPHAN;
	v->ch = NULL;
}

static uint32_t Buf_Hash( const sfx_t *sfx )
{
	uintptr_t p = (uintptr_t)sfx;

	return (uint32_t)(( p >> 4 ) * 2654435761u ) % VOICE_BUFS;
}

static vbuf_t *Buf_Find( const sfx_t *sfx )
{
	uint32_t i = Buf_Hash( sfx ), n;

	for( n = 0; n < VOICE_BUFS; n++, i = ( i + 1 ) % VOICE_BUFS )
	{
		if( s_buf[i].sfx == sfx )
			return &s_buf[i];
		if( !s_buf[i].sfx )
			return NULL;
	}
	return NULL;
}

/* The voices playing b's buffer stop now and drop it, then it is deleted */
static void Buf_Delete( vbuf_t *b )
{
	int i;

	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		if( s_voice[i].state != V_FREE && s_voice[i].buf == b->buf )
		{
			if( s_voice[i].ch )
				s_voice[i].ch->engine_reserved[TOKEN_CPU] = CPU_MARK; // its samples are going
			Voice_Release( &s_voice[i] );
		}
	}
	alDeleteBuffers( 1, &b->buf );
	alGetError();
	b->sfx = VBUF_GONE;
	b->wav = NULL;
	b->buf = 0;
}

/* The OpenAL buffer playing sc in place: made the first time, made again
   if the sound was loaded anew */
static ALuint Buf_For( const sfx_t *sfx, const wavdata_t *sc )
{
	vbuf_t *b = Buf_Find( sfx );
	ALenum fmt;
	ALuint buf = 0;
	uint32_t i, n;

	if( b && b->wav == sc )
		return b->buf;
	if( b )
		Buf_Delete( b );
	if( FBitSet( sc->flags, SOUND_R3D_ADPCM ))
		fmt = AL_FORMAT_MONO_XBOX_ADPCM; // #200: the blocks as they are, decoded by the APU
	else if( sc->channels == 1 )
		fmt = sc->width == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
	else
		fmt = sc->width == 1 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
	alGenBuffers( 1, &buf );
	if( alGetError() != AL_NO_ERROR )
		return 0;
	alBufferDataStatic( buf, fmt, sc->buffer, FBitSet( sc->flags, SOUND_R3D_ADPCM ) ? (ALsizei)sc->size
		: (ALsizei)( sc->samples * sc->width * sc->channels ), sc->rate );
	if( FBitSet( sc->flags, SOUND_LOOPED ) && sc->loop_start < sc->samples )
	{
		// S_RetrieveAudioSamples' loop: back to loop_start at the end
		const ALint lp[2] = { (ALint)sc->loop_start, (ALint)sc->samples };

		alBufferiv( buf, AL_LOOP_POINTS_SOFT, lp );
	}
	if( alGetError() != AL_NO_ERROR )
	{
		alDeleteBuffers( 1, &buf );
		alGetError();
		return 0;
	}
	for( i = Buf_Hash( sfx ), n = 0; n < VOICE_BUFS; n++, i = ( i + 1 ) % VOICE_BUFS )
	{
		if( !s_buf[i].sfx || s_buf[i].sfx == VBUF_GONE )
		{
			s_buf[i].sfx = sfx;
			s_buf[i].wav = sc;
			s_buf[i].buf = buf;
			s_n_bufs++;
			return buf;
		}
	}
	alDeleteBuffers( 1, &buf ); // the table is full: this sound stays on the CPU
	alGetError();
	return 0;
}

/* upstream's S_AdjustLoopedSamplePosition (s_main.c, static there) */
static int Voice_LoopedPosition( const wavdata_t *sc, int pos, qboolean loop )
{
	if( loop && FBitSet( sc->flags, SOUND_LOOPED ) && pos >= (int)sc->samples )
	{
		const int ls = Q_min( (int)sc->loop_start, (int)sc->samples );
		const int range = (int)sc->samples - ls;

		pos = range > 0 ? ls + ( pos - ls ) % range : (int)sc->samples;
	}
	return pos;
}

/* A pitch or gains change at the stream frame of the chunk being painted
   (now when the streams have no known start, or before the voice starts:
   its start is timed) */
static void Voice_Set( voice_t *v, ALenum param, const ALfloat *values, qboolean timed )
{
	unsigned frame;

	if( timed && Xbox_SndGpRoom_StreamFrame( s_start, &frame ))
		alXboxSourcefvAtSample( v->src, param, values, Xbox_SndGpRoom_Source(), frame );
	else
		alSourcefv( v->src, param, values );
	s_n_updates++;
}

static void Voice_Volumes( voice_t *v, const channel_t *ch, qboolean timed )
{
	const short lv = (short)bound( 0, ch->leftvol, 255 ), rv = (short)bound( 0, ch->rightvol, 255 );

	if( lv != v->lv || rv != v->rv )
	{
		// S_MixChannelToBuffer's ( sample * vol ) >> 8 on the 16-bit scale
		const ALfloat g[2] = { lv / 256.0f, rv / 256.0f };

		Voice_Set( v, AL_XBOX_DIRECT_GAINS, g, timed );
		v->lv = lv;
		v->rv = rv;
	}
}

static void Voice_Pitch( voice_t *v, double pitch, qboolean timed )
{
	// upstream's rate is pitch * sound rate / mixer rate: the APU plays the
	// buffer at pitch * its rate
	const float p = (float)pitch;

	if( p != v->pitch )
	{
		const ALfloat ap = p > 0.0f ? p : 1.0f / 256.0f;

		Voice_Set( v, AL_PITCH, &ap, timed );
		v->pitch = p;
	}
}

static qboolean Voice_Eligible( const channel_t *ch, const wavdata_t *sc )
{
	return !ch->words && sc && sc == ch->sfx->cache && sc->type == WF_PCMDATA
		&& !FBitSet( sc->flags, SOUND_R3D_WINDOW | SOUND_R3D_HEADER )
		&& ( sc->width == 1 || sc->width == 2 ) && ( sc->channels == 1 || sc->channels == 2 )
		&& sc->samples > 0 && sc->rate > 0;
}

/* A voice for ch, started at the chunk's frame; false: it mixes on the CPU */
static qboolean Voice_Start( channel_t *ch, const wavdata_t *sc, double pitch )
{
	const qboolean loop = FBitSet( ch->flags, FL_CHAN_USE_LOOP ) && FBitSet( sc->flags, SOUND_LOOPED );
	voice_t *v = NULL;
	unsigned frame;
	ALuint buf;
	int i, used = 0, pos;

	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		if( s_voice[i].state == V_FREE && !v )
			v = &s_voice[i];
		else if( s_voice[i].state != V_FREE )
			used++;
	}
	if( !v )
	{
		s_n_full++;
		return false; // no source free now: on the CPU, and stays there
	}
	if( !( buf = Buf_For( ch->sfx, sc )))
	{
		s_n_nobuf++; // no buffer name free (al_limits.h) or no memory for its tail page
		return false;
	}

	alSourceRewind( v->src ); // AL_INITIAL until its timed start acts
	alSourcei( v->src, AL_BUFFER, (ALint)buf );
	alSourcei( v->src, AL_LOOPING, loop ? AL_TRUE : AL_FALSE );
	v->loop = loop;
	pos = Voice_LoopedPosition( sc, (int)ch->sample, loop );
	// #200: an ADPCM voice starts on a block (the APU is not probed inside
	// one): up to 63 frames early, a restored save's or a reused channel's
	// part-way start
	if( FBitSet( sc->flags, SOUND_R3D_ADPCM ))
		pos -= pos % (int)XBOX_ADPCM_BLOCK_FRAMES;
	alSourcei( v->src, AL_SAMPLE_OFFSET, pos < (int)sc->samples ? pos : 0 );
	// the louder ear ranks it among the voices (0..1); the streams hold
	// STREAM_PRIORITY, so a sound never takes the mix's own voice
	alSourcef( v->src, AL_XBOX_PRIORITY, bound( 0, Q_max( ch->leftvol, ch->rightvol ), 255 ) / 255.0f );
	if( alGetError() != AL_NO_ERROR )
	{
		alSourcei( v->src, AL_BUFFER, 0 );
		alGetError();
		return false;
	}
	v->lv = v->rv = -1;
	v->pitch = -1.0f;
	Voice_Volumes( v, ch, false );
	Voice_Pitch( v, pitch, false );

	v->pending = false;
	if( Xbox_SndGpRoom_StreamFrame( s_start, &frame ))
	{
		alXboxSourcePlayAtSample( v->src, Xbox_SndGpRoom_Source(), frame );
		v->pending = true;
	}
	else
	{
		alSourcePlay( v->src ); // the streams have no start yet: nothing to line up with
		s_n_now++;
	}
	if( alGetError() != AL_NO_ERROR )
	{
		alSourcei( v->src, AL_BUFFER, 0 );
		alGetError();
		return false;
	}

	v->state = V_OWNED;
	v->ch = ch;
	v->token = (uintptr_t)( v - s_voice ) + 1 + VOICE_SLOTS * ( s_next_token++ );
	v->sfx = ch->sfx;
	v->buf = buf;
	v->walked = true;
	v->paused = false;
	ch->engine_reserved[TOKEN_SLOT] = v->token;
	s_n_starts++;
	if( used + 1 > s_peak )
		s_peak = used + 1;
	return true;
}

qboolean Xbox_SndVoices_Channel( channel_t *ch, const wavdata_t *sc, double pitch )
{
	voice_t *v;
	ALint st = AL_PLAYING;

	if( !s_on || ch->engine_reserved[TOKEN_CPU] == CPU_MARK )
		return false;
	if( !( v = Voice_Of( ch )))
	{
		if( Voice_Eligible( ch, sc ) && Voice_Start( ch, sc, pitch ))
			return true;
		ch->engine_reserved[TOKEN_CPU] = CPU_MARK;
		s_n_cpu++;
		return false;
	}

	v->walked = true;
	alGetSourcei( v->src, AL_SOURCE_STATE, &st );
	if( st != AL_INITIAL )
		v->pending = false;
	if( st == AL_STOPPED )
	{
		// no hardware voice, or it was taken: the CPU from the walk's
		// position (the walk is ahead of the voice: it has not ended)
		Voice_Release( v );
		ch->engine_reserved[TOKEN_CPU] = CPU_MARK;
		s_n_lost++;
		return false;
	}
	if( v->paused )
	{
		alSourcePlay( v->src ); // a paused source resumes where it was
		v->paused = false;
	}
	{
		// SND_STOP_LOOPING clears FL_CHAN_USE_LOOP: upstream plays on to the
		// end, as the APU does with AL_LOOPING off
		const qboolean loop = FBitSet( ch->flags, FL_CHAN_USE_LOOP ) && FBitSet( sc->flags, SOUND_LOOPED );

		if( loop != v->loop )
		{
			alSourcei( v->src, AL_LOOPING, loop ? AL_TRUE : AL_FALSE );
			v->loop = loop;
		}
	}
	Voice_Volumes( v, ch, true );
	Voice_Pitch( v, pitch, true );
	alGetError();
	return true;
}

void Xbox_SndVoices_BeginPaint( int start )
{
	int i;

	s_start = start;
	s_painting = true;
	if( !s_on )
		return;
	if( !Xbox_SndGpRoom_Active())
	{
		// the room went back to the CPU: no kernel to send into
		Xbox_SndVoices_Clear();
		s_on = false;
		Con_Printf( S_WARN "Audio: sounds go back to the CPU mix (the room left the GP)\n" );
		return;
	}
	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		voice_t *v = &s_voice[i];
		ALint st = AL_PLAYING;

		v->walked = false;
		if( v->state != V_ORPHAN )
			continue;
		alGetSourcei( v->src, AL_SOURCE_STATE, &st );
		if( st != AL_INITIAL )
			v->pending = false;
		// played out, stopped, or a start that will never act (replaced by
		// a timed stop); a short sound freed before its frame came still waits
		if( st == AL_STOPPED || ( st == AL_INITIAL && !v->pending ))
			Voice_Release( v );
	}
}

void Xbox_SndVoices_EndPaint( void )
{
	int i;

	if( !s_on )
	{
		s_painting = false;
		return;
	}
	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		voice_t *v = &s_voice[i];

		if( v->state != V_OWNED || v->walked )
			continue;
		if( v->ch->engine_reserved[TOKEN_SLOT] != v->token )
		{
			Voice_StopTimed( v ); // the channel was reused without a free
			continue;
		}
		if( !v->paused )
		{
			ALint st = AL_INITIAL;

			alGetSourcei( v->src, AL_SOURCE_STATE, &st );
			if( st == AL_PLAYING )
			{
				alSourcePause( v->src ); // skipped: upstream's position waits too
				v->paused = true;
			}
		}
	}
	alGetError();
	s_painting = false;
}

void Xbox_SndVoices_Free( channel_t *ch )
{
	voice_t *v;

	if( !s_on )
		return;
	if(( v = Voice_Of( ch )))
	{
		if( FBitSet( ch->flags, FL_CHAN_FINISHED ) && !v->paused )
		{
			// ended: the walk is ahead of what is heard, so the voice plays on
			// to its end by itself (and does not loop: the walk did not)
			if( v->loop )
				alSourcei( v->src, AL_LOOPING, AL_FALSE );
			v->loop = false;
			ch->engine_reserved[TOKEN_SLOT] = 0;
			v->state = V_ORPHAN;
			v->ch = NULL;
		}
		else
			Voice_StopTimed( v );
	}
	ch->engine_reserved[TOKEN_SLOT] = 0;
	ch->engine_reserved[TOKEN_CPU] = 0;
}

void Xbox_SndVoices_FreeSound( const sfx_t *sfx )
{
	vbuf_t *b;

	if( !s_on || !( b = Buf_Find( sfx )))
		return;
	Buf_Delete( b );
}

void Xbox_SndVoices_Clear( void )
{
	int i;

	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		if( s_voice[i].state != V_FREE )
			Voice_Release( &s_voice[i] );
	}
}

size_t Xbox_SndVoices_Stats( char *buf, size_t size )
{
	int n, i, used = 0;

	if( !s_on || size == 0 )
		return 0;
	for( i = 0; i < VOICE_SLOTS; i++ )
		used += s_voice[i].state != V_FREE;
	n = Q_snprintf( buf, size, "  sounds on voices: %d now, peak %d of %d; %u started (%u not timed), %u timed stops, %u to the CPU (%u lost a voice, %u no source free, %u no buffer); %u new buffers, %u volume/pitch updates\n",
		used, s_peak, VOICE_SLOTS, s_n_starts, s_n_now, s_n_stops, s_n_cpu + s_n_lost + s_n_full, s_n_lost, s_n_full, s_n_nobuf, s_n_bufs, s_n_updates );
	s_n_starts = s_n_now = s_n_lost = s_n_cpu = s_n_full = s_n_stops = s_n_bufs = s_n_updates = s_n_nobuf = 0;
	s_peak = used;
	return n > 0 ? Q_min( (size_t)n, size - 1 ) : 0;
}

const char *Xbox_SndVoices_Why( void )
{
	return s_on ? "on" : s_why;
}

void Xbox_SndVoices_Init( void )
{
	const ALuint slot = Xbox_SndGpRoom_Slot();
	ALenum err;
	int i;

	if( s_on )
		return;
	if( !Xbox_SndCfg_Asked( "r3d_snd_voices" ))
	{
		Q_snprintf( s_why, sizeof( s_why ), "off: not asked (r3d_snd_voices 1 in a userconfig.d cfg)" );
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
	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		voice_t *v = &s_voice[i];
		const ALfloat zero[2] = { 0.0f, 0.0f };

		memset( v, 0, sizeof( *v ));
		alGenSources( 1, &v->src );
		if(( err = alGetError()) != AL_NO_ERROR )
		{
			Q_snprintf( s_why, sizeof( s_why ), "failed: source %d of %d (AL error 0x%x)", i, VOICE_SLOTS, err );
			goto fail;
		}
		// as the room stream (#180): only through the kernel, which the
		// channel's volumes reach panned
		alSourcefv( v->src, AL_XBOX_DIRECT_GAINS, zero );
		alSourcei( v->src, AL_XBOX_SEND_PANNED, AL_TRUE );
		alSourcei( v->src, AL_DIRECT_FILTER, (ALint)s_filter );
		alSourcei( v->src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE );
		alSource3i( v->src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL );
		alSourcei( v->src, AL_XBOX_VIRTUALIZE, AL_FALSE ); // no voice: AL_STOPPED, and the CPU mixes it
		if(( err = alGetError()) != AL_NO_ERROR )
		{
			Q_snprintf( s_why, sizeof( s_why ), "failed: source %d's setup (AL error 0x%x)", i, err );
			goto fail;
		}
	}
	memset( s_buf, 0, sizeof( s_buf ));
	// the library's own score for the streams is about 10-30; with every
	// hardware voice in use (xemu has 64) a sound must not take theirs
	alSourcef( Xbox_SndGpRoom_Source(), AL_XBOX_PRIORITY, STREAM_PRIORITY );
	alGetError();
	s_on = true;
	Con_Printf( "Audio: sounds play on the APU's voices (%d sources)\n", VOICE_SLOTS );
	return;

fail:
	Con_Printf( S_ERROR "Audio: sounds stay in the CPU mix (OpenAL setup failed)\n" );
	Xbox_SndVoices_Shutdown();
}

void Xbox_SndVoices_Shutdown( void )
{
	int i;

	Xbox_SndVoices_Clear();
	for( i = 0; i < VOICE_BUFS; i++ )
	{
		if( s_buf[i].buf )
			alDeleteBuffers( 1, &s_buf[i].buf );
	}
	memset( s_buf, 0, sizeof( s_buf ));
	for( i = 0; i < VOICE_SLOTS; i++ )
	{
		if( s_voice[i].src )
			alDeleteSources( 1, &s_voice[i].src );
		s_voice[i].src = 0;
	}
	if( s_filter )
		alDeleteFilters( 1, &s_filter );
	s_filter = 0;
	alGetError();
	s_on = false;
}

int Xbox_SndVoices_Active( void )
{
	return s_on;
}

unsigned Xbox_SndVoices_DebugSource( const channel_t *ch )
{
	const voice_t *v = s_on ? Voice_Of( ch ) : NULL;

	return v ? v->src : 0;
}

float Xbox_SndVoices_StreamPriority( void )
{
	return s_on ? STREAM_PRIORITY : -1.0f;
}
