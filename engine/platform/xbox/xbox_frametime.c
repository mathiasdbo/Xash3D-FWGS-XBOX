/*
xbox_frametime.c - frame-time recorder and overlay (host_frametime_analysis)
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
Resonance3D (docs/r3d/divergences.md #141, fork-plan item 14 step 13). It
replaces the temporary probe patched into ref/nv2a/r_context.c for item
12's hardware session and, unlike cl_showfps (kept as upstream has it),
records every frame.

  host_frametime_analysis 0  nothing: one compare per Host_Frame, no memory
  host_frametime_analysis 1  record every frame to frametime/ft_NNN.bin
  host_frametime_analysis 2  record, and draw the overlay over the HUD

Each frame is flagged by the state it starts in (in play, menu), plus
XBOX_FT_F_LOAD when its Host_Frame queued a changelevel or other load:
the load then runs before the next Host_Frame, inside this frame's
"between frames" time, so such a frame is load time, not play.

What a frame record holds (the byte layout is xbox_frametime_util.h):
  total   Host_Frame start to the next Host_Frame start (TSC)
  host    Host_Frame start to its end
  gpu     time the CPU spun on pb_busy() inside the renderer: the part of
          the GPU's work still queued when the CPU needed the GPU idle
  vblank  time in pb_wait_for_vbl()
  swap    time spinning on pb_finished() (all back buffers queued)
  cpu     = host - gpu - vblank - swap: the engine's own work this frame
  work    (format v2) the renderer's work counters for the frame - draws,
          pushbuffer dwords, texture binds, vertices by source, lightmap
          texels rebuilt, models, decals, particles... (XBOX_FT_W_*,
          xbox_frametime_util.h). ref/nv2a adds to xbox_ft_work where it
          submits the work; they are counts, so a run in xemu measures
          them exactly as the console would, unlike the times.
The NV2A has no GPU-side timer this code can read: pbkit binds no context
DMA for NV097_GET_REPORT or the back-end semaphore (pbkit.c:2643-2659,
2728-2734 bind none to the kelvin object's report/semaphore slots), and
NV_PTIMER is a CPU-read clock like the TSC. So "gpu" is not the GPU's busy
time: the GPU runs in parallel with the CPU from the first pb_end() of the
frame (pb_end kicks the DMA, pbkit.c:1824-1850). It is the GPU's excess
over the CPU - near zero when the CPU is the bottleneck (the GPU finished
before the CPU asked), and the frame's GPU-bound share when it is not.

The file lives in the engine's write directory (E:\UDATA\<title id>\valve\
on the Xbox, filesystem_engine.c), flushed with fsync every
XBOX_FT_FLUSH_SECONDS, when half the ring is waiting, before a map chunk
and when recording stops - a power-off loses at most the last few
seconds. tools/frametime-report.py fetches and reads it.
*/

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "client.h"
#include "xbox_frametime.h"
#include "xbox_frametime_util.h"
#include "xbox_frametime_svgame.h"
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

/* Not archived on purpose: a recorder that survives a reboot keeps
   writing to the drive until someone remembers it. The owner turns it on
   per session in userconfig.d, where it is visible. */
static CVAR_DEFINE_AUTO( host_frametime_analysis, "0", FCVAR_FILTERABLE,
	"frame-time analysis: 0 off, 1 record to frametime/ft_NNN.bin, 2 record and draw the overlay" );

/* The per-subsystem CPU spans (M2): on by default whenever the recorder
   is, so a recording has them; 0 measures what they cost (the same view
   with and without - each closed span is two rdtsc, counted per frame in
   span_calls). Read at each Host_Frame start. */
static CVAR_DEFINE_AUTO( host_frametime_spans, "1", FCVAR_FILTERABLE,
	"with host_frametime_analysis on: 1 time the per-subsystem CPU spans (k_* counters), 0 leave them at zero" );

xbox_ft_waits_t xbox_ft_waits;
xbox_ft_work_t xbox_ft_work;
xbox_ft_spans_t xbox_ft_spans;
int xbox_ft_snd_extra;

/* 1024 frames x 420 bytes (32 + 97 work counters: 20 counts, span_calls,
   56 spans and 20 later counts) = 420 KiB ring, plus the per-map
   histogram (4 KiB), a 52.5 KiB encode buffer and the server classname
   table (~30 KiB, xbox_frametime_svgame.c): ~507 KiB, allocated when
   recording starts and freed when it stops. At 72 fps (fps_max) the ring holds 14 s; a flush
   every 5 s or at half full keeps it from overrunning unless a single
   flush stalls for 7 s. */
#define XBOX_FT_RING_FRAMES     1024u
#define XBOX_FT_FLUSH_SECONDS   5u
#define XBOX_FT_ENCODE_FRAMES   128u
#define XBOX_FT_WARMUP_FRAMES   30u     /* left out of a map's statistics, as item 12's report did */
#define XBOX_FT_MAX_FILE_BYTES  ( 64u * 1024u * 1024u )  /* ~1.4 hours at 60 fps (12.4 KiB/s) */
#define XBOX_FT_NOMINAL_TSC_HZ  733333333u /* until calibrated: the retail Pentium III clock */

/* The overlay. The graph shows the last XBOX_FT_GRAPH_FRAMES frames as
   columns of XBOX_FT_GRAPH_GROUP frames each, drawn as the slowest frame
   of the group: a third of the quads of one column per frame, and a
   spike still shows at its full height. Groups are aligned to sequence
   numbers, so a finished column keeps its height as the graph scrolls. */
#define XBOX_FT_GRAPH_FRAMES    120
#define XBOX_FT_GRAPH_GROUP     3
#define XBOX_FT_GRAPH_COLS      ( XBOX_FT_GRAPH_FRAMES / XBOX_FT_GRAPH_GROUP )
#define XBOX_FT_GRAPH_COL_PX    6
#define XBOX_FT_GRAPH_H_PX      100
#define XBOX_FT_GRAPH_MS        50.0f   /* the graph's full height */
#define XBOX_FT_NOW_FRAMES      30      /* the "now" line and the counters average the last half second */
#define XBOX_FT_TEXT_FRAMES     15      /* the text is reformatted 4 times a second, not every frame */
#define XBOX_FT_TEXT_LINES      7
#define XBOX_FT_SPAN_LINE_TOP   6       /* the spans line: the six largest own times, then what no span saw */
/* A load is credited to a LOAD-flagged frame at most this many frames
   before the first loading frame: the changelevel frame, then the one
   frame that still starts active (the client has not seen the reconnect
   yet), then the loading frames. */
#define XBOX_FT_TRIGGER_FRAMES  2u

typedef struct xbox_ft_state_s
{
	int mode;                   /* the mode in effect; 0 = not recording */
	file_t *file;
	char filename[64];
	uint64_t file_bytes;

	void *block;                /* the one allocation: ring + stats + encode buffer */
	xbox_ft_ring_t ring;
	xbox_ft_stats_t *map_stats;
	uint8_t *encode;

	uint64_t qpc_hz, tsc_open, qpc_open;
	uint64_t tsc_hz;            /* live calibration */

	qboolean in_frame;          /* a frame is open (BeginFrame ran, its record not pushed) */
	uint64_t frame_start, frame_end;
	xbox_ft_frame_t cur;

	uint64_t last_flush;

	/* Map tracking. */
	qboolean was_active, was_loading, load_seen;
	uint64_t load_begin;
	/* The last frame flagged XBOX_FT_F_LOAD: where the load it queued
	   began, and the frame count when it was flagged. A load that starts
	   within XBOX_FT_TRIGGER_FRAMES frames is timed from it. */
	qboolean trigger_pending;
	uint64_t trigger_start;
	uint32_t trigger_frame;
	uint32_t frames_begun;
	uint32_t free_before;       /* free pages at the last flush before the load */
	uint32_t last_free_pages;
	uint32_t map_frames;        /* active frames since the map started */
	char map_name[XBOX_FT_NAME_BYTES];

	/* The overlay's text, formatted every XBOX_FT_TEXT_FRAMES frames. */
	uint32_t text_age;
	char text[XBOX_FT_TEXT_LINES][96];
} xbox_ft_state_t;

static xbox_ft_state_t ft;

static void FT_MemoryPages( uint32_t *avail, uint32_t *total )
{
	MM_STATISTICS ms;

	memset( &ms, 0, sizeof( ms ));
	ms.Length = sizeof( ms );
	if( NT_SUCCESS( MmQueryStatistics( &ms )))
	{
		*avail = ms.AvailablePages;
		*total = ms.TotalPhysicalPages;
	}
	else
	{
		*avail = *total = 0;
	}
}

static void FT_Calibrate( uint64_t tsc, uint64_t qpc )
{
	/* A quarter of a second of history is enough for a rate good to
	   well under 0.1%; the file keeps every calibration pair, so the
	   report never depends on this estimate. */
	if( ft.qpc_hz && qpc - ft.qpc_open >= ft.qpc_hz / 4 )
		ft.tsc_hz = ( tsc - ft.tsc_open ) * (double)ft.qpc_hz / (double)( qpc - ft.qpc_open );
}

static qboolean FT_Write( const void *data, size_t bytes )
{
	if( !ft.file )
		return false;
	if( FS_Write( ft.file, data, bytes ) != (fs_offset_t)bytes )
	{
		Con_Printf( S_ERROR "frametime: write to %s failed, recording stopped\n", ft.filename );
		FS_Close( ft.file );
		ft.file = NULL;
		return false;
	}
	ft.file_bytes += bytes;
	return true;
}

/* Writes every unflushed frame as one FRAMES chunk, then a CALIB pair,
   and syncs. */
static void FT_Flush( void )
{
	const xbox_ft_frame_t *span[2];
	uint32_t count[2], n, s, i, done;
	uint64_t tsc, qpc;
	/* The larger of the two chunk heads written from it: a FRAMES head
	   (16 bytes) and a whole CALIB chunk (24). It was 16 until
	   2026-10-05, so every flush wrote 8 bytes past it - over this
	   function's saved registers when the stack alignment put them there
	   (an access violation in the caller once it kept a pointer in ESI
	   across the call, docs/r3d/lessons.md). */
	uint8_t head[XBOX_FT_CHUNK_BYTES + XBOX_FT_CALIB_BYTES];

	STATIC_ASSERT( sizeof( head ) >= XBOX_FT_CHUNK_BYTES + 8u, "FT_Flush: head holds a FRAMES chunk head" );
	STATIC_ASSERT( sizeof( head ) >= XBOX_FT_CHUNK_BYTES + XBOX_FT_CALIB_BYTES, "FT_Flush: head holds a CALIB chunk" );

	if( !ft.file )
		return;

	n = xbox_ft_ring_unflushed( &ft.ring );
	if( n )
	{
		xbox_ft_ring_unflushed_spans( &ft.ring, &span[0], &count[0], &span[1], &count[1] );
		if( !FT_Write( head, xbox_ft_encode_frames_chunk_head( head, ft.ring.flushed_seq, n, XBOX_FT_W_COUNT )))
			return;
		for( s = 0; s < 2; s++ )
		{
			for( i = 0; i < count[s]; i += done )
			{
				uint32_t k;
				size_t bytes = 0;

				done = count[s] - i;
				if( done > XBOX_FT_ENCODE_FRAMES )
					done = XBOX_FT_ENCODE_FRAMES;
				for( k = 0; k < done; k++ )
					bytes += xbox_ft_encode_frame( ft.encode + bytes, &span[s][i + k], XBOX_FT_W_COUNT );
				if( !FT_Write( ft.encode, bytes ))
					return;
			}
		}
		xbox_ft_ring_mark_flushed( &ft.ring );
	}

	tsc = xbox_ft_rdtsc();
	qpc = KeQueryPerformanceCounter();
	FT_Calibrate( tsc, qpc );
	if( !FT_Write( head, xbox_ft_encode_calib( head, tsc, qpc )))
		return;

	FS_Flush( ft.file );
	ft.last_flush = tsc;
	ft.cur.flags |= XBOX_FT_F_FLUSH;

	FT_MemoryPages( &ft.last_free_pages, &i );

	if( ft.file_bytes >= XBOX_FT_MAX_FILE_BYTES )
	{
		Con_Printf( S_WARN "frametime: %s reached %u MiB, recording stopped\n", ft.filename, XBOX_FT_MAX_FILE_BYTES >> 20 );
		Cvar_DirectSet( &host_frametime_analysis, "0" );
	}
}

/* The server game frame's per-classname rows since the last ones
   (xbox_frametime_svgame.c), closing the map they belong to. */
static void FT_WriteClasses( void )
{
	const uint8_t *classes;
	size_t bytes = Xbox_FT_SvGame_EncodeClasses( &classes );

	if( bytes )
		FT_Write( classes, bytes );
}

static void FT_Stop( void )
{
	uint8_t end[XBOX_FT_CHUNK_BYTES + XBOX_FT_END_BYTES];

	if( ft.file )
	{
		/* The frame in progress is not pushed: it has no end yet. */
		FT_Flush();
		FT_WriteClasses();
		if( FT_Write( end, xbox_ft_encode_end( end, xbox_ft_rdtsc(), ft.ring.lost )))
			FS_Flush( ft.file );
		if( ft.file )
			FS_Close( ft.file );
		Con_Printf( "frametime: stopped, %s (%u KiB, %u frames lost)\n", ft.filename, (uint32_t)( ft.file_bytes >> 10 ), ft.ring.lost );
	}

	xbox_ft_waits.active = 0;
	xbox_ft_spans.on = 0;
	Xbox_FT_SvGame_Stop();
	Z_Free( ft.block );
	memset( &ft, 0, sizeof( ft ));
}

static qboolean FT_Start( int mode )
{
	uint8_t header[XBOX_FT_HEADER_BYTES];
	uint8_t counters[XBOX_FT_CHUNK_BYTES + 4 + XBOX_FT_W_COUNT * XBOX_FT_COUNTER_NAME_BYTES];
	uint8_t spans[XBOX_FT_SPANS_CHUNK_BYTES];
	char build[64];
	size_t ring_bytes = XBOX_FT_RING_FRAMES * sizeof( xbox_ft_frame_t );
	uint32_t total_pages;
	int i;

	memset( &ft, 0, sizeof( ft ));

	/* The first unused name: a new session never overwrites the file
	   from the last one, which may not have been fetched yet. */
	for( i = 0; i < 1000; i++ )
	{
		Q_snprintf( ft.filename, sizeof( ft.filename ), "frametime/ft_%03d.bin", i );
		if( !FS_FileExists( ft.filename, true ))
			break;
	}
	if( i == 1000 )
	{
		Con_Printf( S_ERROR "frametime: frametime/ft_000.bin to ft_999.bin all exist - fetch and delete them\n" );
		return false;
	}

	ft.file = FS_Open( ft.filename, "wb", true );
	if( !ft.file )
	{
		Con_Printf( S_ERROR "frametime: couldn't create %s\n", ft.filename );
		return false;
	}

	ft.block = Z_Calloc( ring_bytes + sizeof( xbox_ft_stats_t ) + XBOX_FT_ENCODE_FRAMES * xbox_ft_frame_bytes( XBOX_FT_W_COUNT ));
	xbox_ft_ring_init( &ft.ring, (xbox_ft_frame_t *)ft.block, XBOX_FT_RING_FRAMES );
	ft.map_stats = (xbox_ft_stats_t *)( (byte *)ft.block + ring_bytes );
	ft.encode = (uint8_t *)ft.block + ring_bytes + sizeof( xbox_ft_stats_t );
	if( !Xbox_FT_SvGame_Start( ))
	{
		Con_Printf( S_ERROR "frametime: no memory for the classname table\n" );
		FT_Stop();
		return false;
	}

	ft.qpc_hz = KeQueryPerformanceFrequency();
	ft.tsc_open = xbox_ft_rdtsc();
	ft.qpc_open = KeQueryPerformanceCounter();
	ft.tsc_hz = XBOX_FT_NOMINAL_TSC_HZ;
	ft.last_flush = ft.tsc_open;
	ft.mode = mode;
	FT_MemoryPages( &ft.last_free_pages, &total_pages );

	/* The XBE header's link timestamp identifies the build on the disc. */
	Q_snprintf( build, sizeof( build ), "Resonance3D " XASH_VERSION " xbe %08lX",
		(unsigned long)CURRENT_XBE_HEADER->TimeDateStamp );
	xbox_ft_encode_header( header, XBOX_FT_RING_FRAMES, ft.tsc_hz, ft.qpc_hz, ft.tsc_open, ft.qpc_open, (uint32_t)mode, build, XBOX_FT_W_COUNT );
	if( !FT_Write( header, sizeof( header ))
		|| !FT_Write( counters, xbox_ft_encode_counters( counters, XBOX_FT_W_COUNT ))
		|| !FT_Write( spans, xbox_ft_encode_spans( spans, XBOX_FT_W_COUNT )))
	{
		FT_Stop();
		return false;
	}

	xbox_ft_waits.active = 1;
	Con_Printf( "frametime: recording to %s (%s)\n", ft.filename, build );
	return true;
}

/* Map tracking, at the start of each frame. A load is the run of frames
   in which the client is connected (or the loading plaque is up) but
   not yet active; it begins with the frame BEFORE the first of them,
   because the command that started it (a changelevel, `map`, a menu
   New Game) and the server's whole spawn ran inside that frame. */
static void FT_TrackMap( uint64_t now, qboolean active )
{
	qboolean loading = !active && (( cls.state >= ca_connecting && cls.state <= ca_active ) || cls.disable_screen );

	if( loading && !ft.was_loading )
	{
		/* From the frame that queued the load when there is one: its
		   time after Host_Frame is the load itself (#142, the M0
		   finding). Otherwise the frame being closed - the one before
		   this. */
		if( ft.trigger_pending && ft.frames_begun - ft.trigger_frame <= XBOX_FT_TRIGGER_FRAMES )
			ft.load_begin = ft.trigger_start;
		else
			ft.load_begin = ft.in_frame ? ft.frame_start : now;
		ft.trigger_pending = false;
		ft.free_before = ft.last_free_pages;
		ft.load_seen = true;
	}

	if( active && !ft.was_active )
	{
		uint8_t chunk[XBOX_FT_CHUNK_BYTES + XBOX_FT_MAP_BYTES];
		uint32_t free_pages, total_pages;
		uint32_t flags = ft.load_seen ? 0 : XBOX_FT_MAP_F_NO_LOAD_START;

		FT_MemoryPages( &free_pages, &total_pages );
		Q_strncpy( ft.map_name, clgame.mapname, sizeof( ft.map_name ));

		/* Frames before the map chunk, so the file stays in order; the
		   per-classname rows close the map before it. */
		FT_Flush();
		FT_WriteClasses();
		FT_Write( chunk, xbox_ft_encode_map( chunk, now, ft.load_seen ? now - ft.load_begin : 0,
			ft.load_seen ? ft.free_before : 0, free_pages, total_pages, flags, ft.map_name ));
		ft.last_free_pages = free_pages;
		ft.load_seen = false;
		ft.map_frames = 0;
		xbox_ft_stats_reset( ft.map_stats );
	}

	ft.was_active = active;
	ft.was_loading = loading;
}

static void FT_CloseFrame( uint64_t now )
{
	xbox_ft_frame_t *f = &ft.cur;
	uint32_t total_us;

	f->total = xbox_ft_saturate( now - ft.frame_start, &f->flags );
	f->host = xbox_ft_saturate( ft.frame_end > ft.frame_start ? ft.frame_end - ft.frame_start : 0, &f->flags );
	f->gpu = xbox_ft_saturate( xbox_ft_waits.gpu, &f->flags );
	f->vblank = xbox_ft_saturate( xbox_ft_waits.vblank, &f->flags );
	f->swap = xbox_ft_saturate( xbox_ft_waits.swap, &f->flags );
	Xbox_FT_SvGame_CloseFrame();
	memcpy( f->work, xbox_ft_work.c, sizeof( xbox_ft_work.c ));
	f->work[XBOX_FT_W_SPAN_CALLS] = xbox_ft_spans.calls;
	{
		int s;

		for( s = 0; s < XBOX_FT_S_COUNT; s++ )
			f->work[XBOX_FT_W_SPAN_FIRST + s] = xbox_ft_span_kcyc( xbox_ft_spans.t[s] );
	}

	xbox_ft_ring_push( &ft.ring, f );

	/* The frame that queued a load is load time, not play (its time after
	   Host_Frame is the load): out of the overlay's map statistics, as
	   the report leaves it out of its own. */
	if( FBitSet( f->flags, XBOX_FT_F_ACTIVE ) && !FBitSet( f->flags, XBOX_FT_F_MENU | XBOX_FT_F_LOAD ))
	{
		if( ++ft.map_frames > XBOX_FT_WARMUP_FRAMES )
		{
			total_us = xbox_ft_ticks_to_us( f->total, ft.tsc_hz );
			xbox_ft_stats_add( ft.map_stats, total_us,
				xbox_ft_ticks_to_us( xbox_ft_frame_cpu( f ), ft.tsc_hz ),
				xbox_ft_ticks_to_us( f->gpu, ft.tsc_hz ));
		}
	}
}

void Xbox_FrameTime_Init( void )
{
	Cvar_RegisterVariable( &host_frametime_analysis );
	Cvar_RegisterVariable( &host_frametime_spans );
	/* r3d_membench (#142) rides on this init rather than a call site of
	   its own in host.c: the same platform, the same lifetime. */
	Xbox_MemBench_Init();
}

void Xbox_FrameTime_BeginFrame( void )
{
	int want = (int)host_frametime_analysis.value;
	uint64_t now;
	qboolean active;

	/* S_ExtraUpdate sets it around its mix; a Host_Error longjmp out of
	   the mixer would leave it set and charge every later mix to the
	   k_sndx_* spans. Frames start outside the mixer. */
	xbox_ft_snd_extra = 0;

	/* The whole cost when off (and the server game frame's state reset
	   for the same reason as xbox_ft_snd_extra: a longjmp out of
	   SV_Physics would leave in_game set). */
	if( want == 0 && ft.mode == 0 )
	{
		Xbox_FT_SvGame_BeginFrame( 0 );
		return;
	}

	now = xbox_ft_rdtsc();

	if( want < 0 || want > 2 )
		want = 2;
	if( want != ft.mode )
	{
		if( want == 0 )
		{
			FT_Stop();
			Xbox_FT_SvGame_BeginFrame( 0 );
			return;
		}
		if( ft.mode == 0 && !FT_Start( want ))
		{
			Cvar_DirectSet( &host_frametime_analysis, "0" );
			Xbox_FT_SvGame_BeginFrame( 0 );
			return;
		}
		ft.mode = want;
	}

	if( ft.in_frame )
		FT_CloseFrame( now );

	/* The next frame. Its flags describe the state it starts in (and
	   XBOX_FT_F_LOAD, set at its end, what it queued). */
	memset( &ft.cur, 0, sizeof( ft.cur ));
	ft.frames_begun++;
	active = cls.state == ca_active && cls.signon == SIGNONS;
	if( active )
		ft.cur.flags |= XBOX_FT_F_ACTIVE;
	if( cls.key_dest != key_game || cl.paused )
		ft.cur.flags |= XBOX_FT_F_MENU;
	/* After FT_CloseFrame stored the last frame's server game totals:
	   they restart here, and this frame counts towards the CLASSES rows
	   when it starts in play. */
	Xbox_FT_SvGame_BeginFrame( active && !FBitSet( ft.cur.flags, XBOX_FT_F_MENU ));

	FT_TrackMap( now, active );

	if( xbox_ft_ring_unflushed( &ft.ring ) >= XBOX_FT_RING_FRAMES / 2
		|| now - ft.last_flush >= (uint64_t)XBOX_FT_FLUSH_SECONDS * ft.tsc_hz )
		FT_Flush();

	xbox_ft_waits.gpu = xbox_ft_waits.vblank = xbox_ft_waits.swap = 0;
	memset( &xbox_ft_work, 0, sizeof( xbox_ft_work ));
	memset( xbox_ft_spans.t, 0, sizeof( xbox_ft_spans.t ));
	xbox_ft_spans.calls = 0;
	/* Only here, between frames, so no span is open when it changes. */
	xbox_ft_spans.on = host_frametime_spans.value != 0.0f;
	ft.frame_start = now;
	ft.frame_end = now;
	ft.in_frame = true;
}

#include "xbox_audio_hal.h" // Xbox_AudioWatchdog, docs/r3d/divergences.md #150

void Xbox_FrameTime_EndFrame( void )
{
	// not the recorder's: the audio stall watchdog (#150) needs a call
	// once a frame and this is the Xbox's own per-frame hook
	Xbox_AudioWatchdog();

	if( !ft.mode )
		return;

	ft.frame_end = xbox_ft_rdtsc();

	/* Host_RunFrame acts on nextstate right after Host_Frame returns, and
	   COM_Frame runs the load before the next Host_Frame
	   (engine/common/host_state.c:151-180, :205-224): flag this frame as
	   the one whose remaining time is that load. */
	if( GameState->nextstate != STATE_RUNFRAME )
	{
		ft.cur.flags |= XBOX_FT_F_LOAD;
		ft.trigger_pending = true;
		ft.trigger_start = ft.frame_start;
		ft.trigger_frame = ft.frames_begun;
	}
}

void Xbox_FrameTime_Shutdown( void )
{
	if( ft.mode )
		FT_Stop();
}

static float FT_Ms( uint64_t ticks )
{
	return ft.tsc_hz ? (float)( (double)ticks * 1000.0 / (double)ft.tsc_hz ) : 0.0f;
}

static void FT_Fill( float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	if( w > 0.0f && h > 0.0f )
		ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y, w, h, r, g, b, a );
}

/* A count in at most five characters: 812, 12.3k, 1.23M. */
static const char *FT_Count( char *buf, size_t size, uint32_t v )
{
	if( v < 10000u )
		Q_snprintf( buf, size, "%u", v );
	else if( v < 1000000u )
		Q_snprintf( buf, size, "%u.%uk", v / 1000u, ( v % 1000u ) / 100u );
	else
		Q_snprintf( buf, size, "%u.%02uM", v / 1000000u, ( v % 1000000u ) / 10000u );
	return buf;
}

/* The spans line: the XBOX_FT_SPAN_LINE_TOP spans with the most time of
   their own (a span's time minus its children's) over the same half
   second, in ms, largest first, then `none`: CPU no span saw. Own times,
   not inclusive ones, so the line adds up: the shown values plus the
   rest of the spans plus `none` is the `now` line's cpu. Short labels -
   each glyph is a quad. */
static void FT_FormatSpans( const uint64_t *work, uint64_t cpu_ticks, uint32_t n )
{
	static const char *const label[] =
	{
		"input", "clbeg", "sv", "svread", "game", "send",
		"cl", "cldll", "clread", "pred", "emit",
		"scr", "scene", "lm", "world", "cstud", "pts", "skin",
		"efx", "tri", "hud", "sndx", "rend", "snd",
		"rel", "fpack", "scdat", "sdelta", "sxmit", "runcmd",
		"cdelta", "ccdat", "cmd", "pmove", "wpkt",
		"sspat", "smix", "sdsp", "sout", "xmix", "xdsp", "xout",
		"pvs", "sky", "walk", "wwarp", "wdec", "brush", "bwarp", "twarp",
		"start", "pstep", "ppush", "ptoss", "pnone", "pother",
	};
	/* A span added without a label is a compile error, not a NULL. */
	STATIC_ASSERT( ARRAYSIZE( label ) == XBOX_FT_S_COUNT, "one overlay label per span" );
	uint64_t span[XBOX_FT_S_COUNT], self[XBOX_FT_S_COUNT], top;
	char *out = ft.text[XBOX_FT_TEXT_LINES - 1];
	const size_t size = sizeof( ft.text[0] );
	size_t len;
	uint32_t i, k;

	for( i = 0; i < XBOX_FT_S_COUNT; i++ )
		span[i] = work[XBOX_FT_W_SPAN_FIRST + i];
	top = xbox_ft_span_self( span, self );
	if( !top )
	{
		Q_snprintf( out, size, "spans off (host_frametime_spans 0)" );
		return;
	}

	/* Q_snprintf returns -1 when it truncates (crtlib.c Q_vsnprintf), so
	   the length is re-read rather than summed. */
	Q_strncpy( out, "self ms", size );
	len = Q_strlen( out );
	for( k = 0; k < XBOX_FT_SPAN_LINE_TOP && len + 1 < size; k++ )
	{
		uint32_t best = XBOX_FT_S_COUNT;

		for( i = 0; i < XBOX_FT_S_COUNT; i++ )
		{
			if( self[i] && ( best == XBOX_FT_S_COUNT || self[i] > self[best] ))
				best = i;
		}
		if( best == XBOX_FT_S_COUNT )
			break;
		Q_snprintf( out + len, size - len, " %s %.1f", label[best], FT_Ms( self[best] * 1000u ) / n );
		len = Q_strlen( out );
		self[best] = 0;
	}
	if( len + 1 < size )
		Q_snprintf( out + len, size - len, "  none %.1f", cpu_ticks > top * 1000u ? FT_Ms( cpu_ticks - top * 1000u ) / n : 0.0f );
}

/* The overlay's text, formatted into ft.text every XBOX_FT_TEXT_FRAMES
   frames rather than every frame: the numbers are half-second averages
   anyway, and the float formatting was a measurable part of the
   overlay's cost. */
static void FT_FormatText( void )
{
	const xbox_ft_stats_t *st = ft.map_stats;
	uint64_t total = 0, cpu = 0, gpu = 0, rest = 0, overlay = 0, work[XBOX_FT_W_COUNT];
	uint32_t i, j, n = 0, last = 0;
	uint32_t w[XBOX_FT_W_COUNT];
	char a[9][16]; /* one per count in a line: every argument is formatted before the call */

	memset( work, 0, sizeof( work ));
	for( i = 0; i < XBOX_FT_NOW_FRAMES; i++ )
	{
		const xbox_ft_frame_t *f = xbox_ft_ring_recent( &ft.ring, i );
		uint32_t c;

		if( !f )
			break;
		if( i == 0 )
			last = f->total;
		c = xbox_ft_frame_cpu( f );
		total += f->total;
		cpu += c;
		gpu += f->gpu;
		rest += f->total > (uint64_t)c + f->gpu ? f->total - c - f->gpu : 0;
		overlay += f->overlay;
		for( j = 0; j < XBOX_FT_W_COUNT; j++ )
			work[j] += f->work[j];
		n++;
	}

	for( i = 0; i < XBOX_FT_TEXT_LINES; i++ )
		ft.text[i][0] = 0;
	if( !n )
		return;

	for( j = 0; j < XBOX_FT_W_COUNT; j++ )
		w[j] = (uint32_t)(( work[j] + n / 2 ) / n );

	{
		float avg = FT_Ms( total ) / n;

		Q_snprintf( ft.text[0], sizeof( ft.text[0] ), "now %5.1f fps %5.1f ms  cpu %4.1f  gpu %4.1f  wait %4.1f  (last %.1f)",
			avg > 0.0f ? 1000.0f / avg : 0.0f, avg, FT_Ms( cpu ) / n, FT_Ms( gpu ) / n, FT_Ms( rest ) / n, FT_Ms( last ));
	}

	if( st && st->frames )
	{
		double avg_ms = (double)st->total_us / st->frames / 1000.0;
		uint32_t p99 = xbox_ft_stats_percentile_us( st, 990 );

		Q_snprintf( ft.text[1], sizeof( ft.text[1] ), "map %5.1f fps  1%% low %5.1f  cpu %4.1f gpu %4.1f  >16.7 %.0f%% >33.3 %.0f%%  worst %.0f",
			avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0, p99 ? 1e6 / p99 : 0.0,
			(double)st->cpu_us / st->frames / 1000.0, (double)st->gpu_us / st->frames / 1000.0,
			100.0 * st->over_60 / st->frames, 100.0 * st->over_30 / st->frames, st->worst_us / 1000.0 );
	}

	/* The work counters, averaged over the same half second. */
	Q_snprintf( ft.text[2], sizeof( ft.text[2] ), "draws %s  prims %s  pb %s dw  tex %s  faces %s/%s  leaves %s",
		FT_Count( a[0], sizeof( a[0] ), w[XBOX_FT_W_DRAWS] ), FT_Count( a[1], sizeof( a[1] ), w[XBOX_FT_W_PRIMS] ),
		FT_Count( a[2], sizeof( a[2] ), w[XBOX_FT_W_PB_DWORDS] ), FT_Count( a[3], sizeof( a[3] ), w[XBOX_FT_W_TEX_BINDS] ),
		FT_Count( a[4], sizeof( a[4] ), w[XBOX_FT_W_WORLD_FACES] ), FT_Count( a[5], sizeof( a[5] ), w[XBOX_FT_W_WORLD_WALKED] ),
		FT_Count( a[6], sizeof( a[6] ), w[XBOX_FT_W_PVS_LEAVES] ));
	Q_snprintf( ft.text[3], sizeof( ft.text[3] ), "verts world %s  studio %s  tri %s  2d %s  decal %s",
		FT_Count( a[0], sizeof( a[0] ), w[XBOX_FT_W_VERTS_WORLD] ), FT_Count( a[1], sizeof( a[1] ), w[XBOX_FT_W_VERTS_STUDIO] ),
		FT_Count( a[2], sizeof( a[2] ), w[XBOX_FT_W_VERTS_TRI] ), FT_Count( a[3], sizeof( a[3] ), w[XBOX_FT_W_VERTS_2D] ),
		FT_Count( a[4], sizeof( a[4] ), w[XBOX_FT_W_VERTS_DECAL] ));
	Q_snprintf( ft.text[4], sizeof( ft.text[4] ), "mdl %s/%s tri  lm %s/%s tx  water %s  dec %s  part %s  beam %s  spr %s",
		FT_Count( a[0], sizeof( a[0] ), w[XBOX_FT_W_STUDIO_MODELS] ), FT_Count( a[1], sizeof( a[1] ), w[XBOX_FT_W_STUDIO_TRIS] ),
		FT_Count( a[2], sizeof( a[2] ), w[XBOX_FT_W_LM_FACES] ), FT_Count( a[3], sizeof( a[3] ), w[XBOX_FT_W_LM_TEXELS] ),
		FT_Count( a[4], sizeof( a[4] ), w[XBOX_FT_W_WATER_VERTS] ), FT_Count( a[5], sizeof( a[5] ), w[XBOX_FT_W_DECALS] ),
		FT_Count( a[6], sizeof( a[6] ), w[XBOX_FT_W_PARTICLES] ), FT_Count( a[7], sizeof( a[7] ), w[XBOX_FT_W_BEAMS] ),
		FT_Count( a[8], sizeof( a[8] ), w[XBOX_FT_W_SPRITES] ));
	{
		/* The file name without "frametime/": every glyph is a quad. */
		const char *file = Q_strrchr( ft.filename, '/' );

		Q_snprintf( ft.text[5], sizeof( ft.text[5] ), "%s  %s  overlay %.2f ms", ft.map_name, file ? file + 1 : ft.filename, FT_Ms( overlay ) / n );
	}

	FT_FormatSpans( work, cpu, n );
}

/* The overlay: the frame-time graph (CPU, GPU wait, and the rest of the
   frame - vblank, swap and the time between host frames - stacked per
   column), lines at 16.7 ms (60 fps) and 33.3 ms (30 fps), a legend, the
   numbers and the work counters. Drawn from V_PostRender over the HUD with
   FillRGBA and the console font, the same 2D path SCR_DrawNetGraph uses,
   so the renderer's batching applies: every FillRGBA shares the white
   texture and the text the font texture.

   Its cost is the quads: one per bar segment and one per glyph (spaces
   are free, CL_DrawCharacter). The graph draws XBOX_FT_GRAPH_COLS
   columns of XBOX_FT_GRAPH_GROUP frames rather than one per frame,
   segments of zero height are skipped, and the text is formatted only
   every XBOX_FT_TEXT_FRAMES frames. */
void Xbox_FrameTime_DrawOverlay( void )
{
	const float x0 = 16.0f, y0 = 16.0f;
	const float gw = XBOX_FT_GRAPH_COLS * XBOX_FT_GRAPH_COL_PX;
	const float base = y0 + XBOX_FT_GRAPH_H_PX;
	const float px_per_tick = ft.tsc_hz ? XBOX_FT_GRAPH_H_PX * 1000.0f / ( XBOX_FT_GRAPH_MS * (float)ft.tsc_hz ) : 0.0f;
	rgba_t white = { 255, 255, 255, 255 };
	uint64_t start;
	uint32_t newest, held, col, i;
	int fh = 0, ty;

	if( ft.mode != 2 || cls.state != ca_active )
		return;

	start = xbox_ft_rdtsc();

	if( ft.text_age == 0 )
		FT_FormatText();
	if( ++ft.text_age >= XBOX_FT_TEXT_FRAMES )
		ft.text_age = 0;

	Con_DrawStringLen( "0", NULL, &fh );
	if( fh <= 0 )
		fh = 12;

	FT_Fill( x0 - 6.0f, y0 - 6.0f, gw + 40.0f, XBOX_FT_GRAPH_H_PX + 12.0f + fh * ( XBOX_FT_TEXT_LINES + 1 ) + 4, 0, 0, 0, 150 );

	/* The columns, newest on the right. Column `col` is the group of
	   sequence numbers [g * GROUP, g * GROUP + GROUP) with g = the newest
	   group - col; it shows its slowest frame. */
	held = ft.ring.next_seq < ft.ring.capacity ? ft.ring.next_seq : ft.ring.capacity;
	newest = ft.ring.next_seq - 1;
	for( col = 0; held && col < XBOX_FT_GRAPH_COLS; col++ )
	{
		const uint32_t g = newest / XBOX_FT_GRAPH_GROUP - col;
		const xbox_ft_frame_t *worst = NULL;
		float x, y, cpu, gpu, rest;

		if( newest / XBOX_FT_GRAPH_GROUP < col )
			break; /* before the first frame */

		for( i = 0; i < XBOX_FT_GRAPH_GROUP; i++ )
		{
			const uint32_t seq = g * XBOX_FT_GRAPH_GROUP + i;
			const xbox_ft_frame_t *f;

			if( seq > newest || newest - seq >= held )
				continue;
			f = xbox_ft_ring_recent( &ft.ring, newest - seq );
			if( f && ( !worst || f->total > worst->total ))
				worst = f;
		}
		if( !worst )
			break; /* older than the ring */

		cpu = xbox_ft_frame_cpu( worst ) * px_per_tick;
		gpu = worst->gpu * px_per_tick;
		rest = worst->total * px_per_tick - cpu - gpu;
		x = x0 + gw - ( col + 1 ) * XBOX_FT_GRAPH_COL_PX;

		/* Bottom up, each segment clipped to the graph; FT_Fill drops
		   the empty ones. */
		y = base;
		if( cpu > y - y0 ) cpu = y - y0;
		FT_Fill( x, y - cpu, XBOX_FT_GRAPH_COL_PX, cpu, 60, 200, 90, 230 );
		y -= cpu;
		if( gpu > y - y0 ) gpu = y - y0;
		FT_Fill( x, y - gpu, XBOX_FT_GRAPH_COL_PX, gpu, 255, 190, 40, 230 );
		y -= gpu;
		if( rest > y - y0 ) rest = y - y0;
		FT_Fill( x, y - rest, XBOX_FT_GRAPH_COL_PX, rest, 110, 120, 170, 200 );
	}

	/* The guidelines: 2 px on whole pixels and 4 px wider than the bars
	   on each side, so they stay visible where the bars reach them (a
	   1 px line on a fractional row was in the picture but lost against
	   ~33 ms bars in the first xemu look), labelled with their fps. */
	{
		const float px_per_ms = XBOX_FT_GRAPH_H_PX / XBOX_FT_GRAPH_MS;
		const int y60 = (int)( base - 16.667f * px_per_ms + 0.5f );
		const int y30 = (int)( base - 33.333f * px_per_ms + 0.5f );
		rgba_t c60 = { 90, 220, 255, 255 }, c30 = { 255, 70, 70, 255 };

		FT_Fill( x0 - 4.0f, (float)y60 - 1.0f, gw + 8.0f, 2.0f, c60[0], c60[1], c60[2], 255 );
		FT_Fill( x0 - 4.0f, (float)y30 - 1.0f, gw + 8.0f, 2.0f, c30[0], c30[1], c30[2], 255 );
		FT_Fill( x0, base, gw, 1.0f, 200, 200, 200, 160 );
		Con_DrawString( (int)( x0 + gw + 6.0f ), y60 - fh / 2, "60", c60 );
		Con_DrawString( (int)( x0 + gw + 6.0f ), y30 - fh / 2, "30", c30 );
	}

	/* The legend: a square in each bar colour, then its label. FillRGBA
	   keeps the colour; the console font would tint a coloured label. */
	ty = (int)base + 4;
	{
		static const struct { const char *label; byte r, g, b; } keys[] =
		{
			{ "CPU", 60, 200, 90 },
			{ "GPU wait", 255, 190, 40 },
			{ "waits/vsync", 110, 120, 170 },
		};
		float lx = x0;
		const float sq = fh > 4 ? (float)( fh - 4 ) : (float)fh;

		for( i = 0; i < ARRAYSIZE( keys ); i++ )
		{
			FT_Fill( lx, (float)ty + ( fh - sq ) * 0.5f, sq, sq, keys[i].r, keys[i].g, keys[i].b, 255 );
			lx += sq + 4.0f;
			lx += Con_DrawString( (int)lx, ty, keys[i].label, white ) + 14.0f;
		}
	}
	ty += fh;

	for( i = 0; i < XBOX_FT_TEXT_LINES; i++ )
	{
		if( ft.text[i][0] )
			Con_DrawString( (int)x0, ty, ft.text[i], white );
		ty += fh;
	}

	/* This frame's overlay cost; the text shows its half-second average. */
	ft.cur.overlay = xbox_ft_saturate( xbox_ft_rdtsc() - start, &ft.cur.flags );
	ft.cur.flags |= XBOX_FT_F_OVERLAY;
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
