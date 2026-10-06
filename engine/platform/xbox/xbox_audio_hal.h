/*
xbox_audio_hal.h - what the Resonance3D AC97 driver adds to nxdk's XAudio API

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: 2026 Resonance3D contributors
*/

/* Resonance3D (docs/r3d/divergences.md #150). xbox_audio_hal.c replaces
   nxdk's lib/hal/audio.c (same symbols, <hal/audio.h> unchanged); these
   are the calls it adds for the stall watchdog (xbox_audio_watchdog.c),
   plus the watchdog's own entry point. No kernel or SDL types, so engine
   code can include it. */

#ifndef XBOX_AUDIO_HAL_H
#define XBOX_AUDIO_HAL_H

#include <stdint.h>
#include "xbox_audio_hal_util.h" /* XBOX_ACI_KICK_* */

/* One DMA engine's registers (index 0: PCM out, 1: S/PDIF) and the
   driver's counters, for the watchdog's log line. */
typedef struct xbox_audio_hal_status_s
{
	uint32_t generation;  /* XAudioInit calls so far */
	uint32_t submitted;   /* buffers handed to the hardware (XAudioProvideSamples) since XAudioInit */
	uint32_t delivered;   /* buffers reported free to the callback (SDL's semaphore posts) */
	uint32_t retired[2];  /* buffers each engine has finished */
	uint32_t rescues;     /* extra reports made by KICK_ALL (xbox_aci_account) */
	uint32_t interrupts;  /* ISR entries */
	uint32_t dpcs;        /* accounting runs (DPCs) */
	uint32_t drains;      /* LVBCI raised: an engine played out its last valid buffer (underruns) */
	uint32_t kicks;       /* watchdog recoveries requested */
	uint32_t restarts;    /* register writes a recovery made to restart an engine */
	uint8_t  civ[2], lvi[2], cr[2];
	uint16_t sr[2], picb[2];
} xbox_audio_hal_status_t;

/* Fills *out; callable from any thread at PASSIVE_LEVEL. Before XAudioInit
   the registers read 0. */
void XAudioR3D_GetStatus( xbox_audio_hal_status_t *out );

/* The cheap per-frame view: returns nonzero while a callback is
   registered (SDL's device is open), with the submission count and the
   XAudioInit generation. */
int XAudioR3D_Progress( uint32_t *submitted, uint32_t *generation );

/* Runs the buffer accounting now, from the hardware's registers, as if an
   interrupt had come, with recovery level XBOX_ACI_KICK_LAST or
   XBOX_ACI_KICK_ALL (xbox_aci_account), and restarts an engine that is
   halted with buffers queued or not running at all. No-op before
   XAudioInit. */
void XAudioR3D_Kick( int level );

/* The engine side (xbox_audio_watchdog.c), called once a frame from
   Xbox_FrameTime_EndFrame: logs and kicks when the driver has been handed
   no buffer for a second while SDL's device is open. */
void Xbox_AudioWatchdog( void );

#endif /* XBOX_AUDIO_HAL_H */
