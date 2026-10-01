/*
xbox_frametime.h - the frame-time recorder's interface (engine and ref/nv2a)
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
Resonance3D (docs/r3d/divergences.md #141, fork-plan item 14 step 13).

`host_frametime_analysis` selects the frame analysis: 0 none (default),
1 record, 2 record + overlay. The recorder (xbox_frametime.c) times each
Host_Frame with the Pentium III's time-stamp counter; the renderer adds
the time it spent blocked, which only it can see: the waits below.

The one shared object is xbox_ft_waits, defined in the engine and written
by ref/nv2a. Every other engine service reaches a renderer through
ref_api_t, but this is five integers per frame on the Xbox's single static
image, and a ref_api_t/ref_interface_t member would be an ABI change for
every renderer (the divergence entry explains the choice). A renderer that
never writes it (ref_null, ref_soft) records zero waits: its whole frame
counts as CPU.

The second shared object, xbox_ft_work (format v2), follows the same
pattern for the renderer's work counters: ref/nv2a adds to
xbox_ft_work.c[XBOX_FT_W_*] where it submits the work (a draw, a
texture-stage write, a lightmap rebuild...), unconditionally - a plain
add, cheaper than testing whether the recorder is on - and the recorder
zeroes the array at Host_Frame start and copies it into the frame record
when the frame closes. With the recorder off nobody zeroes it and the
counts just wrap. The list and its meaning are in xbox_frametime_util.h.
*/

#ifndef XBOX_FRAMETIME_H
#define XBOX_FRAMETIME_H

#include <stdint.h>
#include "xbox_frametime_util.h" /* XBOX_FT_W_*: the work counters' indices */

typedef struct xbox_ft_waits_s
{
	/* Non-zero while recording. The renderer's wait sites test this one
	   word and read the TSC only when it is set, so a disabled recorder
	   costs one load and branch per site. */
	volatile uint32_t active;
	uint32_t reserved;
	/* Ticks spent this frame (the recorder zeroes them at frame start). */
	uint64_t gpu;     /* spinning on pb_busy(): the GPU still had queued work */
	uint64_t vblank;  /* in pb_wait_for_vbl() */
	uint64_t swap;    /* spinning on pb_finished(): no free back buffer */
} xbox_ft_waits_t;

extern xbox_ft_waits_t xbox_ft_waits;

typedef struct xbox_ft_work_s
{
	uint32_t c[XBOX_FT_W_COUNT];
} xbox_ft_work_t;

extern xbox_ft_work_t xbox_ft_work;

/* The time-stamp counter (Pentium III: the core clock, 733 MHz on a
   retail unit; the recorder calibrates it against the kernel's
   performance counter rather than assuming the rate). */
static inline uint64_t xbox_ft_rdtsc( void )
{
	uint32_t lo, hi;

	__asm__ __volatile__( "rdtsc" : "=a"( lo ), "=d"( hi ));
	return ( (uint64_t)hi << 32 ) | lo;
}

/* Engine side (xbox_frametime.c). */
void Xbox_FrameTime_Init( void );          /* registers the cvar */
void Xbox_FrameTime_BeginFrame( void );    /* Host_Frame, after Host_FilterTime */
void Xbox_FrameTime_EndFrame( void );      /* Host_Frame, last */
void Xbox_FrameTime_DrawOverlay( void );   /* V_PostRender, the 2D pass */
void Xbox_FrameTime_Shutdown( void );      /* Host_ShutdownWithReason */

/* xbox_membench.c (#142): registers `r3d_membench`; called from
   Xbox_FrameTime_Init. */
void Xbox_MemBench_Init( void );

#endif /* XBOX_FRAMETIME_H */
