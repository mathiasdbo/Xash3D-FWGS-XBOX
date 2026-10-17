/*
xbox_snd_al.h - the Xbox's sound output through OpenAL on the MCPX APU
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

/* Resonance3D (docs/r3d/divergences.md #178, X12 item 5a). One build
   carries two outputs for the mixer, chosen at boot: SDL's audio thread on
   the AC97 driver (xbox_audio_hal.c), upstream's path, and nxdk-ex's
   OpenAL (3rdparty/nxdk-ex/lib/openal), the mix streamed through one
   source on the APU. xbox_snd_al.c defines upstream's SNDDMA_* and sends
   them to one or the other; s_sdl2.c's are renamed SDLash_SNDDMA_* on the
   Xbox. */
#ifndef XBOX_SND_AL_H
#define XBOX_SND_AL_H

#include <stdint.h>
#include "xbox_audio_hal.h" /* xbox_audio_thread_stats_t */

/* SDL's output (engine/platform/sdl2/s_sdl2.c) */
qboolean SDLash_SNDDMA_Init( void );
void SDLash_SNDDMA_Shutdown( void );
void SDLash_SNDDMA_BeginPainting( void );
void SDLash_SNDDMA_Submit( void );
void SDLash_SNDDMA_Activate( qboolean active );

/* Set before SNDDMA_Init by S_Init from r3d_snd_backend (s_main.c):
   nonzero asks for OpenAL. */
extern int xbox_snd_want_al;

/* Nonzero while OpenAL is the open output */
int Xbox_SndAL_Active( void );

/* S_ClearBuffer's: drops the audio already queued on the source (it plays
   copies of the ring, which the clear does not reach). No-op under SDL. */
void Xbox_SndAL_Clear( void );

/* The output's audio-thread counters for the frame-time recorder, in
   XAudioR3D_ThreadStats' shape (#173): OpenAL's output thread when it is
   the output (cycles and passes of its pump, AC97 buffers, underruns),
   otherwise SDL's. */
void Xbox_Snd_ThreadStats( xbox_audio_thread_stats_t *out );

#endif /* XBOX_SND_AL_H */
