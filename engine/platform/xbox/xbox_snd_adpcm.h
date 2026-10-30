/*
xbox_snd_adpcm.h - resident sounds kept as Xbox ADPCM
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

/* Resonance3D (docs/r3d/divergences.md #200). A map's sounds stay in
   memory whole, as the file had them: 1.9-4.0 MiB a map at 720p, where
   the console has 5-10 MiB to spare during a load. With
   "r3d_snd_adpcm 1", S_LoadSound turns each mono sound into Xbox ADPCM
   (xbox_music_adpcm.h: 36 bytes a 64-frame block, 0.5625 bytes a frame
   against the 8-bit file's 1 and a 16-bit one's 2). The APU's voices
   play the blocks as they are (#182, AL_FORMAT_MONO_XBOX_ADPCM); the
   CPU mixer, the mouths and the sentence trims read them decoded, a
   window at a time, through S_RetrieveAudioSamples.

This file has no engine dependency: the host test
(tools/host-tests/xbox_snd_adpcm_host_test.c) runs it against the music
decoder. */
#ifndef XBOX_SND_ADPCM_H
#define XBOX_SND_ADPCM_H

#include <stddef.h>
#include <stdint.h>
#include "xbox_music_adpcm.h"

/* wavdata_t.flags: buffer[] holds size bytes of Xbox ADPCM blocks, mono;
   samples, rate and loop_start are the sound's as loaded, width reads 2
   (what S_RetrieveAudioSamples hands out). Beside #177's SOUND_R3D_WINDOW
   (BIT 24) and SOUND_R3D_HEADER (BIT 25). */
#define SOUND_R3D_ADPCM ( 1u << 26 )

/* Sounds shorter than this stay PCM: two blocks, where the padding of the
   last one stops costing more than it saves */
#define XBOX_SND_ADPCM_MIN_FRAMES 128u

/* Bytes of `frames` mono frames: whole blocks, the last padded */
size_t Xbox_SndAdpcm_Bytes( uint32_t frames );

/* Encodes `frames` mono frames into Xbox_SndAdpcm_Bytes( frames ) bytes
   at `out`. `width` 1: the sound cache's unsigned 8-bit bytes (sound.h
   S_SAMPLE8 on the Xbox), scaled by 256 to the 16-bit scale the mixer
   and the APU play both widths at; 2: int16. Frames past the end are
   coded as 0. Each block's header holds its first frame exactly, so no
   error carries from one block to the next and any block decodes alone.
   `out` may be `pcm` itself: block k writes bytes [36k, 36k + 36) after
   reading frames up to 64k + 64, which an 8-bit or 16-bit buffer holds
   at or past byte 64k. */
void Xbox_SndAdpcm_Encode( const void *pcm, int width, uint32_t frames, uint8_t *out );

/* Decodes frames [first, first + count) of `blocks` into `out`. */
void Xbox_SndAdpcm_Decode( const uint8_t *blocks, uint32_t first, uint32_t count, int16_t *out );

#endif /* XBOX_SND_ADPCM_H */
