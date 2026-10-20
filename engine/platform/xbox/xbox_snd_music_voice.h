/*
xbox_snd_music_voice.h - the baked music on an APU voice (X12 step 5b, stage C)
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

/* Resonance3D (docs/r3d/divergences.md #183, the design in
   docs/r3d/design/x9-audio-apu-voices.md, stage C). While the sounds are
   on voices (#182), a baked ADPCM track (#171) opened from then on is not
   decoded: its stream hands each block of the file to Xbox_SndMusicVoice_Sink,
   and S_StreamBackgroundTrack, in place of S_RawEntSamples, hands the
   blocks covering what it read to one OpenAL source, which the APU
   decodes and plays on the dry output. Upstream's loop still reads the
   stream (its positions, loops, saves and stops are unchanged); the raw
   channel is kept silent, its end set to when the voice's data runs out. */
#ifndef XBOX_SND_MUSIC_VOICE_H
#define XBOX_SND_MUSIC_VOICE_H

#include "sound.h"

struct xbox_xwav_info_s;
struct stream_s;

/* After Xbox_SndVoices_Init: on while the sounds are on voices */
void Xbox_SndMusicVoice_Init( void );
void Xbox_SndMusicVoice_Shutdown( void );

/* Xbox_MusicStream_Open: true when a track of this format is to play
   here (its stream then gets the sink) */
int Xbox_SndMusicVoice_Want( const struct xbox_xwav_info_s *info );

/* The stream's sink: `ctx` is its xbox_music_stream_t */
void Xbox_SndMusicVoice_Sink( void *ctx, uint32_t block, const uint8_t *data );

/* S_StreamBackgroundTrack, after a read of `samples` frames from
   `stream` into the raw channel `ch`, at volume `vol` (0..255): true when
   this played them, false when the read is upstream's to add */
qboolean Xbox_SndMusicVoice_Samples( rawchan_t *ch, struct stream_s *stream, uint samples, int vol );

/* S_PaintChannels, each chunk: the voice stops at the frame being painted
   when the game pauses (upstream stops mixing the raw channel) or the raw
   channel was emptied or taken */
void Xbox_SndMusicVoice_Paint( qboolean paused );

/* Xbox_SndAL_Clear: stops now */
void Xbox_SndMusicVoice_Clear( void );

/* One line for sndalprof.txt since the last call; 0 while off */
size_t Xbox_SndMusicVoice_Stats( char *buf, size_t size );

/* The source, 0 while off (tools/host-tests/xbox_snd_music_voice_host_test.c) */
unsigned Xbox_SndMusicVoice_DebugSource( void );

#endif /* XBOX_SND_MUSIC_VOICE_H */
