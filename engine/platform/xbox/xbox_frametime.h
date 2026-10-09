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

The third, xbox_ft_spans, holds the per-subsystem CPU spans (M2): the
engine (host.c, sv_main.c, sv_frame.c, sv_client.c, cl_main.c,
cl_parse.c, cl_view.c) and ref/nv2a wrap a call
in XBOX_FT_SPAN below, and the recorder stores the frame's totals as
kilocycle work counters.
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

/* The third shared object (2026-10-01): the per-subsystem CPU spans
   (XBOX_FT_S_*, xbox_frametime_util.h), accumulated in ticks over the
   frame; the recorder zeroes them at Host_Frame start and stores them as
   kilocycle work counters. Unlike the counts above, a span costs two
   rdtsc, so every site tests `on` first: with the recorder off (or
   host_frametime_spans 0) a span is one load and branch. */
typedef struct xbox_ft_spans_s
{
	volatile uint32_t on;   /* recording and host_frametime_spans: set at Host_Frame start only */
	uint32_t calls;         /* span closures this frame (XBOX_FT_W_SPAN_CALLS) */
	uint64_t t[XBOX_FT_S_COUNT];
} xbox_ft_spans_t;

extern xbox_ft_spans_t xbox_ft_spans;

/* Non-zero while S_ExtraUpdate mixes (engine/client/sound/s_main.c):
   S_PaintChannels runs from both mixer entry points, and this picks its
   spans' set - XBOX_FT_S_SND_* under k_snd, XBOX_FT_S_SNDX_* under
   k_snd_extra (docs/r3d/frametime-recorder.md, "Spans"). */
extern int xbox_ft_snd_extra;

/* The time-stamp counter (Pentium III: the core clock, 733 MHz on a
   retail unit; the recorder calibrates it against the kernel's
   performance counter rather than assuming the rate). */
static inline uint64_t xbox_ft_rdtsc( void )
{
	uint32_t lo, hi;

	__asm__ __volatile__( "rdtsc" : "=a"( lo ), "=d"( hi ));
	return ( (uint64_t)hi << 32 ) | lo;
}

/* One open span: its start and the renderer's waits so far. */
typedef struct xbox_ft_span_s
{
	uint64_t t0;    /* 0: not timing (the recorder was off at the start) */
	uint64_t w0;
} xbox_ft_span_t;

static inline uint64_t xbox_ft_wait_ticks( void )
{
	return xbox_ft_waits.gpu + xbox_ft_waits.vblank + xbox_ft_waits.swap;
}

static inline void xbox_ft_span_begin( xbox_ft_span_t *s )
{
	s->t0 = 0;
	if( xbox_ft_spans.on )
	{
		s->w0 = xbox_ft_wait_ticks();
		s->t0 = xbox_ft_rdtsc();
	}
}

/* Adds the span's time minus the waits inside it: a span is CPU work, so
   the GPU wait R_EndFrame or a full vertex ring spends inside the scene
   is not charged to it (it is the frame's GPU wait already). */
static inline void xbox_ft_span_end( const xbox_ft_span_t *s, int id )
{
	if( s->t0 )
	{
		const uint64_t d = xbox_ft_rdtsc() - s->t0;
		const uint64_t w = xbox_ft_wait_ticks() - s->w0;

		xbox_ft_spans.t[id] += d > w ? d - w : 0;
		xbox_ft_spans.calls++;
	}
}

/* Closes a first-use load opened with xbox_ft_span_begin (2026-10-06,
   XBOX_FT_W_SND_LOADS / XBOX_FT_W_TEX_LOADS): counts it and its size in
   KiB, rounded up, unconditionally like the renderer's counts, and adds
   its time minus the renderer's waits, in kilocycles, to `kc_id` when
   the span was timed (-1: no time counter). A load can run under any
   span (a sound's first word in k_cl_read, the next one in k_snd_mix),
   so its time is a plain counter, not a span with one parent. */
/* An open span's ticks so far minus the renderer's waits in them (0 when
   it is not timing), and the same in kilocycles. */
static inline uint64_t xbox_ft_span_ticks_since( const xbox_ft_span_t *s )
{
	uint64_t d, w;

	if( !s->t0 )
		return 0;
	d = xbox_ft_rdtsc() - s->t0;
	w = xbox_ft_wait_ticks() - s->w0;
	return d > w ? d - w : 0;
}

static inline uint32_t xbox_ft_span_kc_since( const xbox_ft_span_t *s )
{
	return (uint32_t)(( xbox_ft_span_ticks_since( s ) + 500u ) / 1000u );
}

/* Adds an open span's time so far, in kilocycles, to time counter `id`
   (XBOX_FT_W_*_KC): a time measured like a span that is not one. */
static inline void xbox_ft_time_add( const xbox_ft_span_t *s, int id )
{
	if( s->t0 )
		xbox_ft_work.c[id] += xbox_ft_span_kc_since( s );
}

/* Returns the load's kilocycles (0 when it was not timed), for a note. */
static inline uint32_t xbox_ft_load_end( const xbox_ft_span_t *s, int count_id, int kb_id, int kc_id, size_t bytes )
{
	const uint32_t kc = xbox_ft_span_kc_since( s );

	xbox_ft_work.c[count_id]++;
	xbox_ft_work.c[kb_id] += (uint32_t)(( bytes + 1023u ) / 1024u );
	if( kc_id >= 0 )
		xbox_ft_work.c[kc_id] += kc;
	return kc;
}

/* A named one-off event for the NOTES chunk (XBOX_FT_NOTE_*,
   xbox_frametime_util.h), credited to the frame in progress: a sound
   loaded in play, a slow server message. Nothing while not recording. */
void Xbox_FT_Note( uint32_t kind, const char *name, uint32_t kcyc, uint32_t bytes );

/* Load stages (2026-10-08, x9-perf-loadtime, docs/r3d/divergences.md
   #162): a stage of a level load - SV_SpawnServer, R_NewMap, the
   precache - timed by the wall clock with the file layer's kernel calls
   inside it (compat/xbox/xbox_io_stats.h), so a note can say how much of
   it waited on the disk. Xbox_FT_LoadEnd writes it as an
   XBOX_FT_NOTE_LOAD note now; Xbox_FT_LoadAccum adds it to accumulator
   `acc` (XBOX_FT_LACC_*), written as one note per kind at the next map
   start. Nothing while not recording: Begin reads no clock then. Unlike
   a span, a stage keeps the renderer's waits and the disk's in its time:
   a load's question is how long the player waits. */
typedef struct xbox_ft_load_s
{
	uint64_t t0;            /* 0: not timing */
	uint64_t io_ticks;
	uint64_t read_bytes;
	uint32_t opens, reads, seeks, probes, writes, meta;
} xbox_ft_load_t;

void Xbox_FT_LoadBegin( xbox_ft_load_t *l );
void Xbox_FT_LoadEnd( const xbox_ft_load_t *l, const char *stage );
void Xbox_FT_LoadAccum( const xbox_ft_load_t *l, int acc );

/* Two inserted lines around a region of upstream code, like
   XBOX_FT_SPAN_BEGIN/END; files that use them define all three as nothing
   outside an Xbox build (#162). */
#define XBOX_FT_LOAD_BEGIN( var ) xbox_ft_load_t var; Xbox_FT_LoadBegin( &var )
#define XBOX_FT_LOAD_END( var, stage ) Xbox_FT_LoadEnd( &var, ( stage ))
#define XBOX_FT_LOAD_ACCUM( var, acc ) Xbox_FT_LoadAccum( &var, ( acc ))

/* Times one statement as span `id` (XBOX_FT_S_*). Engine files that use
   it include this header under #if XASH_XBOX && !XASH_DEDICATED and
   define XBOX_FT_SPAN( id, stmt ) as plain `stmt` otherwise
   (docs/r3d/divergences.md #141), so a non-Xbox build compiles the
   upstream statement unchanged. */
#define XBOX_FT_SPAN( id, stmt ) \
	do { \
		xbox_ft_span_t xbox_ft_span_; \
		xbox_ft_span_begin( &xbox_ft_span_ ); \
		stmt; \
		xbox_ft_span_end( &xbox_ft_span_, ( id )); \
	} while( 0 )

/* The same for a run of statements: XBOX_FT_SPAN_BEGIN( var ) declares
   and opens span variable `var`, XBOX_FT_SPAN_END( var, id ) closes it
   as span `id`: two inserted lines time a region of upstream code
   without re-indenting it (sv_save.c). A path that returns between the
   two records nothing for that span. Files that use them define both as
   nothing outside an Xbox build (#141). */
#define XBOX_FT_SPAN_BEGIN( var ) xbox_ft_span_t var; xbox_ft_span_begin( &var )
#define XBOX_FT_SPAN_END( var, id ) xbox_ft_span_end( &var, ( id ))

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
