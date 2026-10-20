/*
xbox_snd_music_voice.c - the baked music on an APU voice (X12 step 5b, stage C)
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
Resonance3D (docs/r3d/divergences.md #183; the design is
docs/r3d/design/x9-audio-apu-voices.md, stage C).

WHY. The baked track (#171) still costs the frame its decode and the raw
channel's copy (k_snd_music, 0.041 + 0.026 ms on the console, session
8c7db857) and its mix into the paint buffer; the APU decodes Xbox ADPCM
itself, so the blocks can go to a voice as the file holds them.

WHAT. One OpenAL source, direct mode at volume/256 on both outputs (the
paint buffer the raw channel is mixed into goes to the output without the
master gain, as the dry stream does: xbox_snd_al.c), priority above every
sound voice. A stream given the sink (Xbox_SndMusicVoice_Want, at its
open) decodes nothing: each block a pull of libmpg's chunk reaches comes
here once, in file order, and is copied into RING_BLOCKS slots, PENDING.
S_StreamBackgroundTrack reads as before; after each read the blocks that
cover the frames it handed out (Xbox_MusicStream_HandedFrame) go from the
pending ones onto the source as one buffer, or two where the ring wraps
(alBufferData's copy: the stream copies it into its own ring). A block is played whole: the frames of the last one upstream's
reader never hands out (the end of the track, under 64) play, and a start
part way into a block (a restored save) starts at the block - nxdk-ex
queues ADPCM in whole blocks.

THE CLOCK. The first buffer starts at the room stream's frame for the
raw channel's end, the time S_RawSamplesStereo would have written it at,
or the paint time if that is earlier (alXboxSourcePlayAtSample, #182).
From then on the raw channel's end is set to the time the voice's data
runs out: snd.soundtime plus what is queued and not yet played at the mix
rate. Upstream's loop then reads exactly what keeps that many frames
ahead (MAX_RAW_SAMPLES), and the APU's own rate, not a count of what was
read, decides when more is needed, so the voice never drifts dry.

GAPS. Upstream's music goes silent from the paint chunk where the game
pauses (S_MixRawChannels mixes nothing then), and where something empties
or takes the raw channel (S_ClearRawChannels, the rate switch,
S_FindRawChannel's replacement): each chunk checks for those and stops the
voice at the frame being painted. A source that played all it had (the
game stopped reading: a menu, the end of a track) has stopped. In each of
these cases the next read starts the voice again from what it hands out.
The raw channel is kept silent (no volume) so the CPU mixes nothing of it.
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_music_voice.h"
#include "platform/xbox/xbox_snd_voices.h"
#include "platform/xbox/xbox_snd_gproom.h"
#include "platform/xbox/xbox_music_adpcm.h"
#include "platform/xbox/xbox_music_stream.h"
#include "platform/xbox/xbox_snd_al_queue.h"
#include "platform/xbox/openal/al_limits.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#define RING_BLOCKS     384u /* 557 ms of 44.1 kHz: MAX_RAW_SAMPLES ahead at 48 kHz, and one pulled chunk */
#define SLOT_BYTES      ( 2u * XBOX_ADPCM_CHANNEL_BYTES )
#define MV_BUFS         64   /* nxdk-ex's AL_STREAM_MAX_QUEUE */

/* the library's buffer names hold them beside the mix's and the room
   stream's queues, with room left for the sounds on voices (al_limits.h):
   too few fail Init's alGenBuffers */
typedef char music_bufs_fit_the_pool[( MV_BUFS + 2 * XBOX_SND_AL_MAX_BUFFERS + 256 <= R3D_AL_BUFFERS ) ? 1 : -1];
#define MUSIC_PRIORITY  1000.0f

typedef struct
{
	ALuint buf;
	uint32_t ring_end; /* the ring counter past its blocks: the slots it frees */
	uint32_t frames;
} mvq_t;

static qboolean s_on;
static ALuint s_src;
static ALuint s_names[MV_BUFS];
static ALuint s_free[MV_BUFS];
static int s_nfree;
static mvq_t s_q[MV_BUFS]; /* the queued buffers, oldest first */
static int s_qn;
static uint32_t s_q_frames; /* frames in s_q */

static uint8_t s_ring[RING_BLOCKS * SLOT_BYTES];
/* ring counters (slot = counter % RING_BLOCKS): [r_free, r_queued) is on
   the source, [r_queued, r_pend) pending */
static uint32_t r_free, r_queued, r_pend;
static const void *s_owner;  /* the stream whose blocks are pending */
static uint32_t s_pend_block; /* the file's block at r_queued */
static uint32_t s_stride;     /* block_bytes of the queued and pending blocks */
static ALenum s_format;
static uint32_t s_rate, s_q_rate;

static const void *s_q_owner; /* the stream the source plays */
static uint32_t s_q_next;     /* its next block to queue */
static uint32_t s_q_handed;   /* its frame handed out at the last read */
static qboolean s_started;    /* the source plays, or waits for its frame */
static qboolean s_stopping;   /* a timed stop was sent: the next read stops it now */
static uint s_rawend_set;     /* the raw channel's end as this file last set it */
static int s_gain = -1;

static uint32_t s_n_starts, s_n_now, s_n_stops, s_n_bufs, s_n_gaps, s_n_full, s_n_nobuf, s_n_dropped;

/* S_StreamBackgroundTrack reads until the raw channel's end is
   MAX_RAW_SAMPLES past snd.soundtime, which does not move inside its loop:
   reads at one soundtime past this many are an end that never gets there.
   The music goes back to the CPU, and the stats line says why. */
#define GUARD_READS 512
static int s_guard_time = -1;
static uint32_t s_guard_n;
static qboolean s_guard_off;
static char s_guard_note[320];

static uint32_t Mv_ToMix( uint32_t frames )
{
	return s_q_rate ? (uint32_t)(( (uint64_t)frames * (uint32_t)SOUND_DMA_SPEED ) / s_q_rate ) : 0;
}

/* Buffers the source finished go back, and their slots */
static void Mv_Reap( void )
{
	ALint done = 0;

	alGetSourcei( s_src, AL_BUFFERS_PROCESSED, &done );
	while( done-- > 0 && s_qn > 0 )
	{
		ALuint b;

		alSourceUnqueueBuffers( s_src, 1, &b );
		if( alGetError() != AL_NO_ERROR )
			break;
		r_free = s_q[0].ring_end;
		s_q_frames -= s_q[0].frames;
		s_free[s_nfree++] = s_q[0].buf;
		memmove( &s_q[0], &s_q[1], (size_t)( s_qn - 1 ) * sizeof( s_q[0] ));
		s_qn--;
	}
	if( s_qn == 0 )
		r_free = r_queued;
}

/* Stopped now, every buffer back, AL_INITIAL (a timed start leaves a
   stopped source stopped until its frame, which reads as played out);
   the pending blocks stay */
static void Mv_Flush( void )
{
	alSourceStop( s_src );
	Mv_Reap();
	alSourcei( s_src, AL_BUFFER, 0 );
	alSourceRewind( s_src );
	alGetError();
	while( s_qn > 0 )
		s_free[s_nfree++] = s_q[--s_qn].buf;
	s_q_frames = 0;
	r_free = r_queued;
	s_started = s_stopping = false;
}

int Xbox_SndMusicVoice_Want( const struct xbox_xwav_info_s *info )
{
	return s_on && !s_guard_off && info->tag == XBOX_XWAV_TAG_ADPCM && ( info->channels == 1 || info->channels == 2 );
}

void Xbox_SndMusicVoice_Sink( void *ctx, uint32_t block, const uint8_t *data )
{
	const xbox_music_stream_t *ms = ctx;

	if( !s_on )
		return;

	// another stream, or a jump (a seek): what was pending is not played
	if( ctx != s_owner || block != s_pend_block + ( r_pend - r_queued ))
	{
		r_pend = r_queued;
		s_owner = ctx;
		s_pend_block = block;
	}
	if( r_pend == r_queued && r_queued == r_free )
	{
		// an empty ring takes any format
		s_stride = ms->info.block_bytes;
		s_rate = ms->info.rate;
		s_format = ms->info.channels == 2 ? AL_FORMAT_STEREO_XBOX_ADPCM : AL_FORMAT_MONO_XBOX_ADPCM;
	}
	if( ms->info.block_bytes != s_stride || ms->info.rate != s_rate || r_pend - r_free >= RING_BLOCKS )
	{
		// no room, or another format behind queued blocks: lost, and the
		// read that hands its frames out finds a gap
		s_n_full++;
		r_pend = r_queued;
		s_pend_block = block + 1;
		return;
	}
	memcpy( s_ring + ( r_pend % RING_BLOCKS ) * SLOT_BYTES, data, s_stride );
	r_pend++;
}

/* Pending blocks onto the source up to file block `end`: the frames queued */
static uint32_t Mv_Queue( const void *ms, uint32_t handed, uint32_t end )
{
	uint32_t frames = 0;

	// another stream, or this one moved back (a seek; a new stream in the
	// freed one's memory starts again at 0): from what is pending
	if( s_q_owner != ms || handed < s_q_handed )
	{
		s_q_owner = ms;
		s_q_next = ( s_owner == ms ) ? s_pend_block : end;
	}
	s_q_handed = handed;
	if( s_owner != ms )
		return 0;

	// the block a read ends inside is queued whole, so what is pending
	// starts at the next one to play, or later where the sink lost blocks
	if( s_pend_block > s_q_next && end > s_q_next )
		s_n_gaps++; // blocks the sink lost
	if( s_qn > 0 && s_rate != s_q_rate )
		return 0; // another rate behind the queue: wait for it to play out

	while( r_pend > r_queued && s_pend_block < end )
	{
		const uint32_t slot = r_queued % RING_BLOCKS;
		uint32_t n = Q_min( end - s_pend_block, r_pend - r_queued );
		ALuint b;

		n = Q_min( n, RING_BLOCKS - slot ); // to the ring's end, the rest next
		if( !s_nfree || s_qn >= MV_BUFS )
		{
			s_n_nobuf++;
			break;
		}
		b = s_free[--s_nfree];
		// a copy in CPU memory, as the dry stream's (xbox_snd_al.c): a
		// stream copies its buffers into its ring, so the APU never reads
		// them, and alBufferDataStatic would allocate a contiguous tail page
		// and write back the whole CPU cache (wbinvd) on every call
		alBufferData( b, s_format, s_ring + slot * SLOT_BYTES, (ALsizei)( n * s_stride ), (ALsizei)s_rate );
		alSourceQueueBuffers( s_src, 1, &b );
		if( alGetError() != AL_NO_ERROR )
		{
			s_free[s_nfree++] = b;
			s_n_nobuf++;
			break;
		}
		r_queued += n;
		s_pend_block += n;
		s_q[s_qn].buf = b;
		s_q[s_qn].ring_end = r_queued;
		s_q[s_qn].frames = n * XBOX_ADPCM_BLOCK_FRAMES;
		s_qn++;
		s_q_frames += n * XBOX_ADPCM_BLOCK_FRAMES;
		s_q_rate = s_rate;
		frames += n * XBOX_ADPCM_BLOCK_FRAMES;
		s_n_bufs++;
	}
	s_q_next = Q_max( s_q_next, s_pend_block );
	return frames;
}

qboolean Xbox_SndMusicVoice_Samples( rawchan_t *ch, struct stream_s *stream, uint samples, int vol )
{
	xbox_music_stream_t *ms = Xbox_MusicStream_Sunk( stream );
	ALint state = AL_INITIAL;
	uint32_t handed, end, queued;
	uint rawend;

	if( !ms )
		return false;
	if( !s_on )
	{
		// the voices went (an output restart): the stream decodes again
		// from its next byte; this read was not decoded and is dropped
		Xbox_MusicStream_SetSink( ms, NULL, NULL );
		s_n_dropped++;
		return true;
	}

	Mv_Reap();
	alGetSourcei( s_src, AL_SOURCE_STATE, &state );
	alGetError();
	// upstream started the raw channel again (it ran dry, was emptied), a
	// stop was sent, or the source played everything it had: from here
	if( s_started && ( s_stopping || ch->s_rawend != s_rawend_set || state == AL_STOPPED ))
	{
		Mv_Flush();
		s_n_stops++;
	}

	handed = Xbox_MusicStream_HandedFrame( ms );
	end = ( handed + XBOX_ADPCM_BLOCK_FRAMES - 1 ) / XBOX_ADPCM_BLOCK_FRAMES;
	queued = Mv_Queue( ms, handed, end );

	if( !s_started && s_qn > 0 )
	{
		// where S_RawSamplesStereo would have written it; after a stop of
		// this file's own, the chunk being painted
		const int t0 = ( ch->s_rawend == s_rawend_set ) ? snd.paintedtime : Q_max( (int)ch->s_rawend, snd.paintedtime );
		unsigned frame;

		alSourcef( s_src, AL_XBOX_PRIORITY, MUSIC_PRIORITY );
		if( Xbox_SndGpRoom_StreamFrame( t0, &frame ))
			alXboxSourcePlayAtSample( s_src, Xbox_SndGpRoom_Source(), frame );
		else
		{
			alSourcePlay( s_src );
			s_n_now++;
		}
		alGetError();
		s_started = true;
		s_n_starts++;
		state = AL_INITIAL;
		rawend = (uint)t0 + Mv_ToMix( s_q_frames );
	}
	else if( s_started && state == AL_INITIAL )
	{
		// still waiting for its frame: nothing played yet
		rawend = s_rawend_set + Mv_ToMix( queued );
	}
	else if( s_started )
	{
		ALint played = 0;

		alGetSourcei( s_src, AL_SAMPLE_OFFSET, &played );
		alGetError();
		played = bound( 0, played, (ALint)s_q_frames );
		rawend = (uint)snd.soundtime + Mv_ToMix( s_q_frames - (uint32_t)played );
	}
	else
	{
		// nothing could be queued: the time moves on as upstream's would,
		// so its loop ends
		rawend = Q_max( ch->s_rawend, (uint)snd.paintedtime ) + Mv_ToMix( samples ) + 1;
		s_n_dropped++;
	}
	if( !queued && s_started && end > s_q_next )
	{
		// handed out past what is queued (no buffer, a lost block): count
		// it as read, or upstream's loop would not end
		rawend = Q_max( rawend, ch->s_rawend + Mv_ToMix( samples ) + 1 );
		s_n_dropped++;
	}

	if( vol != s_gain )
	{
		const ALfloat g[2] = { vol / 256.0f, vol / 256.0f };

		alSourcefv( s_src, AL_XBOX_DIRECT_GAINS, g );
		alGetError();
		s_gain = vol;
	}

	// the CPU mixes nothing of it: S_SpatializeRawChannels gives a raw
	// channel without attenuation its master_vol each frame
	ch->master_vol = 0;
	ch->dist_mult = 0.0f;
	ch->leftvol = ch->rightvol = 0;

	if( snd.soundtime != s_guard_time )
	{
		s_guard_time = snd.soundtime;
		s_guard_n = 0;
	}
	if( ++s_guard_n > GUARD_READS )
	{
		Q_snprintf( s_guard_note, sizeof( s_guard_note ),
			"stuck: %u reads at soundtime %d (painted %d), rawend %u -> %u (set %u), state 0x%x, started %d stopping %d; "
			"%d queued (%u frames), handed %u, end %u, next %u, pending block %u, ring %u/%u/%u, read %u",
			s_guard_n, snd.soundtime, snd.paintedtime, ch->s_rawend, rawend, s_rawend_set, state, s_started, s_stopping,
			s_qn, s_q_frames, handed, end, s_q_next, s_pend_block, r_free, r_queued, r_pend, samples );
		Con_Printf( S_ERROR "Audio: the music voice is %s; the music goes back to the CPU\n", s_guard_note );
		Mv_Flush();
		Xbox_MusicStream_SetSink( ms, NULL, NULL );
		s_guard_off = true;
		// this loop ends; the raw channel was never written, so that much
		// is silence, then S_RawEntSamples fills it with the decoded track
		ch->s_rawend = (uint)snd.soundtime + ch->max_samples;
		return true;
	}
	ch->s_rawend = rawend;
	s_rawend_set = rawend;
	return true;
}

void Xbox_SndMusicVoice_Paint( qboolean paused )
{
	const rawchan_t *ch;
	unsigned frame;

	if( !s_on || !s_started || s_stopping )
		return;
	ch = S_FindRawChannel( S_RAW_SOUND_BACKGROUNDTRACK, false );
	if( !paused && ch && ch->s_rawend == s_rawend_set )
		return;
	if( Xbox_SndGpRoom_StreamFrame( snd.paintedtime, &frame ))
	{
		alXboxSourceStopAtSample( s_src, Xbox_SndGpRoom_Source(), frame );
		s_stopping = true;
	}
	else Mv_Flush();
	alGetError();
	s_n_stops++;
}

void Xbox_SndMusicVoice_Clear( void )
{
	if( !s_on )
		return;
	Mv_Flush();
}

size_t Xbox_SndMusicVoice_Stats( char *buf, size_t size )
{
	int n;

	if( !s_on || size == 0 )
		return 0;
	if( s_guard_off )
	{
		n = Q_snprintf( buf, size, "  music on a voice: off, %s\n", s_guard_note );
		return n > 0 ? Q_min( (size_t)n, size - 1 ) : 0;
	}
	n = Q_snprintf( buf, size, "  music on a voice: %s; %u started (%u not timed), %u stopped; %u buffers, %d queued (%u frames); %u gaps, %u blocks lost to a full ring, %u no buffer, %u reads not played\n",
		s_started ? "playing" : "idle", s_n_starts, s_n_now, s_n_stops, s_n_bufs, s_qn, s_q_frames, s_n_gaps, s_n_full, s_n_nobuf, s_n_dropped );
	s_n_starts = s_n_now = s_n_stops = s_n_bufs = s_n_gaps = s_n_full = s_n_nobuf = s_n_dropped = 0;
	return n > 0 ? Q_min( (size_t)n, size - 1 ) : 0;
}

unsigned Xbox_SndMusicVoice_DebugSource( void )
{
	return s_on ? s_src : 0;
}

void Xbox_SndMusicVoice_Init( void )
{
	const ALfloat zero[2] = { 0.0f, 0.0f };

	if( s_on || !Xbox_SndVoices_Active( ))
		return;
	alGetError();
	alGenSources( 1, &s_src );
	alGenBuffers( MV_BUFS, s_names );
	if( alGetError() != AL_NO_ERROR )
		goto fail;
	// the raw channel's place: the dry output, both sides, no room
	alSourcefv( s_src, AL_XBOX_DIRECT_GAINS, zero );
	alSourcef( s_src, AL_XBOX_PRIORITY, MUSIC_PRIORITY );
	if( alGetError() != AL_NO_ERROR )
		goto fail;
	for( s_nfree = 0; s_nfree < MV_BUFS; s_nfree++ )
		s_free[s_nfree] = s_names[s_nfree];
	s_qn = 0;
	s_q_frames = 0;
	r_free = r_queued = r_pend = 0;
	s_owner = s_q_owner = NULL;
	s_q_next = s_q_handed = s_pend_block = 0;
	s_started = s_stopping = false;
	s_gain = 0;
	s_q_rate = 0;
	s_guard_time = -1;
	s_guard_n = 0;
	s_guard_off = false;
	s_on = true;
	Con_Printf( "Audio: the baked music plays on an APU voice\n" );
	return;

fail:
	Con_Printf( S_ERROR "Audio: the music stays in the CPU mix (OpenAL setup failed)\n" );
	Xbox_SndMusicVoice_Shutdown();
}

void Xbox_SndMusicVoice_Shutdown( void )
{
	if( s_src )
	{
		alSourceStop( s_src );
		alSourcei( s_src, AL_BUFFER, 0 );
		alDeleteSources( 1, &s_src );
		s_src = 0;
	}
	if( s_names[0] )
		alDeleteBuffers( MV_BUFS, s_names );
	memset( s_names, 0, sizeof( s_names ));
	alGetError();
	s_nfree = s_qn = 0;
	s_on = false;
}
