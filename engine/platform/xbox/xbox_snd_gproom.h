/*
xbox_snd_gproom.h - the room effect on the APU's GP: the room mix's stream and the kernel's parameters
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

/* Resonance3D (docs/r3d/divergences.md #180). With "r3d_snd_gproom 1" in a
   userconfig.d cfg and OpenAL as the output, the mixer hands its room mix
   to Xbox_SndGpRoom_Paint instead of running SX_RoomFX on it
   (s_mix.c): a second ring, queued buffer for buffer with the dry one onto
   a second source that sends into the room kernel
   (apu/gp_hldsp.asm), and each change of the kernel's parameters
   (SX_R3D_GpParams) queued at the stream frame it was painted for. The
   rest of the calls are xbox_snd_al.c's, at the dry stream's matching
   steps; all are no-ops while the room effect is not on. */
#ifndef XBOX_SND_GPROOM_H
#define XBOX_SND_GPROOM_H

#include "sound.h" /* portable_samplepair_t */

/* After the dry source exists: on when the cfg asks and the source, the
   kernel and its slot are all set up (then sx_r3d_gp is set) */
void Xbox_SndGpRoom_Init( int ring_frames, int speed );
void Xbox_SndGpRoom_Shutdown( void );
int Xbox_SndGpRoom_Active( void );

/* The room source, for the dry stream's alSourcePlayv / Pausev */
unsigned Xbox_SndGpRoom_Source( void );
/* The kernel's slot, for the sends of sounds on voices of their own
   (xbox_snd_voices.c, #182) */
unsigned Xbox_SndGpRoom_Slot( void );
/* The streams' frame that plays the mixer's frame time `time`: nonzero
   while the streams run from a known start (not before the first start,
   nor after a clear until the next) */
int Xbox_SndGpRoom_StreamFrame( int time, unsigned *frame );

/* The ring's frames [pos, pos + frames) uploaded into a free buffer of the
   room source: nonzero when that worked, the buffer in *buf */
int Xbox_SndGpRoom_Upload( int pos, int frames, unsigned *buf );
/* That buffer queued (after the dry one was); a failure turns the room
   effect off, as the two streams would no longer line up */
void Xbox_SndGpRoom_Queue( unsigned buf );
/* That buffer not queued after all (the dry one failed) */
void Xbox_SndGpRoom_Unused( unsigned buf );

/* The room source's played buffers back to the free list */
void Xbox_SndGpRoom_Retire( void );
/* Both sources were started from a stop with one alSourcePlayv; their
   stream frame 0 is the mixer's frame time `time` */
void Xbox_SndGpRoom_Started( int time );
/* The dry source was stopped and emptied (Xbox_SndAL_Clear): the room
   source too, and the parameter changes not sent yet are sent now */
void Xbox_SndGpRoom_Clear( void );

/* From S_PaintChannels, per chunk, in place of SX_RoomFX and the mix of the
   room buffer: the room mix of frame times [start, end) and the kernel's
   parameters for it */
void Xbox_SndGpRoom_Paint( const portable_samplepair_t *room, int start, int end, const int *params );

/* One line for sndalprof.txt: the GP's load peak since the last call, the
   changes queued at a stream frame, attached at once and retried, the
   streams' starts; 0 while the room effect is not on */
size_t Xbox_SndGpRoom_Stats( char *buf, size_t size );

/* "key 1" (any nonzero value) in a userconfig.d cfg */
qboolean Xbox_SndCfg_Asked( const char *key );
/* r3d_snd_hw (the Audio menu's "Hardware audio"), as the boot configs set
   it: config.cfg, where the menu saves it, then the userconfig.d files,
   the last setting winning. On, it asks for r3d_snd_gproom,
   r3d_snd_voices and r3d_snd_voices_speech, and the OpenAL output. */
qboolean Xbox_SndCfg_Hardware( void );
/* A setting as the boot configs leave it, before they run: config.cfg,
   then the userconfig.d files, the last one winning. True and its value
   in val when one of them sets it. Also read for r3d_vid_mode (#190). */
qboolean Xbox_BootCfg_Value( const char *key, char *val, int size );
/* Before alcOpenDevice: the GP scratch only the room kernel needs (#191) */
void Xbox_SndGpRoom_PreOpen( void );

#endif /* XBOX_SND_GPROOM_H */
