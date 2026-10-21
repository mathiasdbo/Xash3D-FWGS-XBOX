/*
xbox_snd_feed.h - Half-Life's speech fed onto the APU's voices (X12 step 5b, stage B1)
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

/* Resonance3D (docs/r3d/divergences.md #184, the design in
   docs/r3d/design/x9-audio-apu-voices.md section 7). With
   "r3d_snd_voices_speech 1" in a userconfig.d cfg, OpenAL as the output
   and the room effect on the GP (#180), a speech channel - a sentence, or
   a sound #177 streams from a window - plays on a streaming OpenAL source
   of its own. The mixer still walks it (S_MixChannelToBuffer, the window,
   the trims, timecompress, VOX_LoadWord, the mouth): where it would mix a
   span, it hands the span's source samples to the source instead, so the
   data the APU plays is what upstream mixes, in order. Its start, its
   volumes and its pitch are timed to the room stream's frame the mixer
   painted them for. A channel whose next word has another format, one
   that gets no source, or one whose source loses its voice mixes on the
   CPU from where the walk is. All calls are no-ops while it is off. */

#ifndef XBOX_SND_FEED_H
#define XBOX_SND_FEED_H

#include "sound.h"
#include <math.h>

/* After Xbox_SndGpRoom_Init: on when the cfg asks and the room is on the GP */
void Xbox_SndFeed_Init( void );
void Xbox_SndFeed_Shutdown( void );
int Xbox_SndFeed_Active( void );

/* "on", or why Xbox_SndFeed_Init left it off (for sndbackend.txt) */
const char *Xbox_SndFeed_Why( void );

/* S_PaintChannels, around the normal channels of the chunk starting at
   frame time `start`: Begin reaps the sources that played out, End stops
   those of channels reused without a free */
void Xbox_SndFeed_BeginPaint( int start );
void Xbox_SndFeed_EndPaint( void );

/* The source frames a span hands over: from the walk's position `sample`
   to where an untimecompressed walk would be after `out_count` outputs at
   `rate`, computed as S_MixChannelToBuffer advances chan->sample, so the
   next span (timecompress 0) starts exactly where this one ends; at most
   the `available` frames S_RetrieveAudioSamples gave */
static inline int Xbox_SndFeed_SpanFrames( double sample, double rate, int out_count, int available )
{
	int n = (int)( floor( sample + out_count * rate ) - floor( sample ));

	return n < 0 ? 0 : n > available ? available : n;
}

/* S_MixChannelToBuffer, in place of mixing a span: the walk is at
   ch->sample, `audio` holds `available` frames of `wav` from there, and
   the mix would write `out_count` output samples at `offset` in the chunk
   at `rate`, the mixer's pitch being `pitch`. True when the span went to
   ch's source (the walk then advances without mixing); false: mix it on
   the CPU, as from then on for this channel. */
qboolean Xbox_SndFeed_Take( channel_t *ch, const wavdata_t *wav, const void *audio, int available,
	double rate, double pitch, int out_count, int offset );

/* S_FreeChannel: a channel that ended plays what it was fed, one cut short
   stops at the frame the mixer is at */
void Xbox_SndFeed_Free( channel_t *ch );

/* Xbox_SndAL_Clear: every source stops now */
void Xbox_SndFeed_Clear( void );

/* One line for sndalprof.txt since the last call; 0 while off */
size_t Xbox_SndFeed_Stats( char *buf, size_t size );

/* The source fed ch's samples, 0 if none (tools/host-tests/xbox_snd_feed_host_test.c) */
unsigned Xbox_SndFeed_DebugSource( const channel_t *ch );
/* Tests: the buffer names ch's feed holds unqueued (-1: none); the next
   `n` uploads fail as alBufferData does (an invalid format) */
int Xbox_SndFeed_DebugFreeNames( const channel_t *ch );
void Xbox_SndFeed_DebugFailUploads( int n );

#endif /* XBOX_SND_FEED_H */
