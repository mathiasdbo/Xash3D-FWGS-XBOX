/*
xbox_snd_voices.h - Half-Life's effects on the APU's voices (X12 step 5b, stage A)
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

/* Resonance3D (docs/r3d/divergences.md #182, the design in
   docs/r3d/design/x9-audio-apu-voices.md). With "r3d_snd_voices 1" in a
   userconfig.d cfg, OpenAL as the output and the room effect on the GP
   (#180), a channel that is not a sentence and whose sound is resident
   plays on an OpenAL source of its own: the sound pool's samples in place,
   its send into the room kernel panned by the channel's volumes, started
   and stopped at the room stream's frame the mixer painted it for. The
   mixer still walks the channel (S_MixChannelToBuffer with no buffer), so
   its position, its end and everything that reads them are upstream's;
   only the mixing is gone. A channel the library gives no voice, or whose
   voice is taken, mixes on the CPU from where the walk is. All calls are
   no-ops while it is off. */
#ifndef XBOX_SND_VOICES_H
#define XBOX_SND_VOICES_H

#include "sound.h"

/* After Xbox_SndGpRoom_Init: on when the cfg asks and the room is on the GP */
void Xbox_SndVoices_Init( void );
void Xbox_SndVoices_Shutdown( void );
int Xbox_SndVoices_Active( void );
/* "on", or why Xbox_SndVoices_Init left them off (for sndbackend.txt) */
const char *Xbox_SndVoices_Why( void );
/* AL_XBOX_PRIORITY for the dry stream while on (above every voice), -1
   (the library's own score) while off */
float Xbox_SndVoices_StreamPriority( void );

/* S_PaintChannels, around the normal channels of the chunk starting at
   frame time `start`: Begin reaps the voices that ended, End pauses the
   voices of channels the mixer skipped and stops those of channels that
   are gone */
void Xbox_SndVoices_BeginPaint( int start );
void Xbox_SndVoices_EndPaint( void );

/* S_MixNormalChannelsToRoombuffer, for a channel about to mix (not a
   sentence) from `sc` at `pitch`: true when a voice plays it, and the
   mixer then only walks it */
qboolean Xbox_SndVoices_Channel( channel_t *ch, const wavdata_t *sc, double pitch );

/* S_FreeChannel: a sound that ended plays out on its voice, one cut short
   stops at the frame the mixer is at */
void Xbox_SndVoices_Free( channel_t *ch );

/* S_FreeSound, before the samples are freed: no voice reads them after */
void Xbox_SndVoices_FreeSound( const sfx_t *sfx );

/* Xbox_SndAL_Clear: every voice stops now */
void Xbox_SndVoices_Clear( void );

/* One line for sndalprof.txt since the last call; 0 while off */
size_t Xbox_SndVoices_Stats( char *buf, size_t size );

/* The source playing ch's sound, 0 if none (tools/host-tests/xbox_snd_voices_host_test.c) */
unsigned Xbox_SndVoices_DebugSource( const channel_t *ch );

#endif /* XBOX_SND_VOICES_H */
