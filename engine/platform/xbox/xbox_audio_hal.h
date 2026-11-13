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
	uint32_t reset_stuck; /* re-initialisations that found an engine that would not halt, left un-reset (#173) */
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

/* 1 when the S/PDIF engine plays (a console), 0 when it does not (xemu, xbased): see xbox_audio_hal.c.
   Call before the audio is opened; it plays two silent buffers. */
int XAudioR3D_SpdifRuns( void );

/* Runs the buffer accounting now, from the hardware's registers, as if an
   interrupt had come, with recovery level XBOX_ACI_KICK_LAST or
   XBOX_ACI_KICK_ALL (xbox_aci_account), and restarts an engine that is
   halted with buffers queued or not running at all. No-op before
   XAudioInit. */
void XAudioR3D_Kick( int level );

/* The audio thread's own CPU time (docs/r3d/divergences.md #173), for the
   frame-time recorder. SDL_SoundCallback (engine/platform/sdl2/s_sdl2.c)
   calls XAudioR3D_ThreadMark first thing, on SDL's audio thread, after
   SDL has taken its mixer lock (deps/nxdk/lib/sdl/SDL2/src/audio/
   SDL_audio.c:743-747); the next XAudioProvideSamples on that thread
   closes the interval. It holds the callback's copy out of the engine's
   ring and, when the engine mixes at another rate than the device's
   48 kHz, SDL's conversion: SDL_AudioStreamPut converts and resamples the
   whole callback buffer, SDL_AudioStreamGet copies one device buffer out
   (SDL_audio.c:751-769). Not in it: the lock wait before the callback,
   the semaphore wait after PlayDevice, and the copy of a second device
   buffer when one callback's conversion yields two (one callback in
   about eleven at 44.1 kHz). An interrupt or DPC that lands inside the
   interval is counted in it. */
void XAudioR3D_ThreadMark( void );

/* Cumulative since boot, wrapping at 2^32: TSC cycles of the closed
   intervals above, how many closed, buffers handed to the hardware
   (every XAudioProvideSamples, the first silent ones included), and
   LVBCI interrupts from the PCM-out engine (it played out its last valid
   buffer: an underrun). The reader subtracts two readings. */
typedef struct xbox_audio_thread_stats_s
{
	uint32_t cycles;
	uint32_t runs;
	uint32_t buffers;
	uint32_t underruns;
} xbox_audio_thread_stats_t;

void XAudioR3D_ThreadStats( xbox_audio_thread_stats_t *out );

/* Before SDL_CloseAudioDevice (#173: r3d_snd_rate reopens the device at
   run time): SDL waits for its audio thread to see the shutdown flag
   (SDL_audio.c:1130-1138), and a thread blocked on a completion the
   hardware never reports - xemu, or a stalled engine - would never see
   it, with the watchdog stuck on the same main thread. This arms a 10 ms
   periodic kernel timer whose DPC reports a free buffer to the callback,
   so the thread wakes, sees the flag and exits. The next XAudioInit
   (SDL's CloseDevice calls it) cancels the timer. No-op before
   XAudioInit. */
void XAudioR3D_BeginClose( void );

/* Nonzero when the last XAudioInit found a bus master that would not halt
   (#173): it left the hardware alone, and the device must not be used -
   SNDDMA_Init fails the open on it. */
int XAudioR3D_InitFailed( void );

/* The engine side (xbox_audio_watchdog.c), called once a frame from
   Xbox_FrameTime_EndFrame: logs and kicks when the driver has been handed
   no buffer for a second while SDL's device is open. */
void Xbox_AudioWatchdog( void );

#endif /* XBOX_AUDIO_HAL_H */
