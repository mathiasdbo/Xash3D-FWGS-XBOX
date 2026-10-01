/*
xbox_frametime_util.h - the frame-time recorder's pure parts: the frame
ring, the per-map statistics and the file format's byte layout
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
Nothing in here touches the engine, the kernel or the GPU, so the whole
file is host-tested (tools/host-tests/xbox_frametime_util_host_test.c) and
the same encoder writes the fixture tools/frametime-report.py's own tests
parse. The recorder that drives it is xbox_frametime.c.

File format (version 2; every integer little-endian, the Xbox's own byte
order - the encoder below writes bytes explicitly, so a host of either
endianness produces the same file). Version 1 is the same file without
work counters: 32-byte frame records and no COUNTERS chunk. The encoders
still write it when given zero counters (the tests' v1 fixture).

  header, XBOX_FT_HEADER_BYTES (128):
    0  char[8] magic "R3DFTIME"
    8  u32 version (2; 1 without counters)
    12 u32 header bytes (128)
    16 u32 frame record bytes (32 + 4 x the counter count; 32 in v1)
    20 u32 ring frames (the recorder's in-memory ring size)
    24 u64 TSC rate estimate at open, Hz (nominal until calibrated -
           the CALIB chunks are the real calibration)
    32 u64 performance-counter rate, Hz (KeQueryPerformanceFrequency)
    40 u64 TSC at open
    48 u64 performance counter at open
    56 u32 mode (host_frametime_analysis at open: 1 record, 2 overlay)
    60 u32 reserved (0)
    64 char[64] build id, NUL-padded
  then chunks until end of file, each { u32 type, u32 payload bytes,
  payload }; a reader skips a type it does not know by its length:
    1 FRAMES  u32 first sequence number, u32 count, count frame records
    2 MAP     u64 TSC at map start, u64 load ticks, u32 free pages when
              the load began, u32 free pages at map start, u32 total
              pages, u32 flags (bit 0: no load start seen - the first map
              after recording began), char[64] map name
    3 CALIB   u64 TSC, u64 performance counter (one per flush)
    4 END     u64 TSC, u32 frames lost to a full ring, u32 reserved -
              written when recording stops cleanly (a file without it
              was cut by a power-off or a crash)
    5 COUNTERS (v2, right after the header, before any FRAMES chunk)
              u32 count, then count x char[16] names, NUL-padded: the
              names of the work counters at the end of every frame
              record, in order (xbox_ft_work_names below). A reader
              finds counters by name, so a later version may append or
              reorder them without breaking it.
  frame record, XBOX_FT_FRAME_BYTES (32) + 4 x counters:
    0  u32 sequence number (consecutive; a gap is frames lost)
    4  u32 total ticks, Host_Frame start to the next Host_Frame start
    8  u32 host ticks, Host_Frame start to its end
    12 u32 GPU wait ticks (CPU blocked in pb_busy: the GPU's tail)
    16 u32 vblank wait ticks (pb_wait_for_vbl)
    20 u32 swap wait ticks (pb_finished: no free back buffer)
    24 u32 overlay ticks (the overlay's own drawing, inside host)
    28 u16 flags (XBOX_FT_F_*), u16 reserved
    32 u32 work counters[count] (v2): this frame's renderer work, see
       XBOX_FT_W_* - counts of things submitted, so unlike the times
       they are the same in xemu and on the console
  Every tick count saturates at 0xFFFFFFFF (5.8 s at 733 MHz) and sets
  XBOX_FT_F_SATURATED. CPU = host - gpu - vblank - swap; the rest of
  total (total - host) is the time between host frames: the engine's
  frame limiter (Host_Autosleep) and the platform loop.
*/

#ifndef XBOX_FRAMETIME_UTIL_H
#define XBOX_FRAMETIME_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define XBOX_FT_VERSION        2u
#define XBOX_FT_VERSION_NO_WORK 1u   /* a file without work counters */
#define XBOX_FT_HEADER_BYTES   128u
#define XBOX_FT_FRAME_BYTES    32u   /* the fixed part; v2 appends 4 bytes per counter */
#define XBOX_FT_COUNTER_NAME_BYTES 16u
#define XBOX_FT_CHUNK_BYTES    8u  /* type + payload length */
#define XBOX_FT_MAP_BYTES      96u
#define XBOX_FT_CALIB_BYTES    16u
#define XBOX_FT_END_BYTES      16u
#define XBOX_FT_NAME_BYTES     64u

#define XBOX_FT_CHUNK_FRAMES   1u
#define XBOX_FT_CHUNK_MAP      2u
#define XBOX_FT_CHUNK_CALIB    3u
#define XBOX_FT_CHUNK_END      4u
#define XBOX_FT_CHUNK_COUNTERS 5u

/* Per-frame work counters (v2). ref/nv2a adds to them where the work is
   submitted (xbox_frametime.h's xbox_ft_work); the recorder zeroes them
   at Host_Frame start and stores them in the frame record. Every one is a
   count, not a time, so it is identical in xemu and on the console.
   Append new ones before XBOX_FT_W_COUNT and give them a name in
   xbox_frametime_util.c. */
enum
{
	XBOX_FT_W_DRAWS = 0,     /* draw submissions: a world run, a 2D/TriAPI batch flush, a decal */
	XBOX_FT_W_PRIMS,         /* BEGIN/END blocks pushed (a draw splits at pbkit's 128-dword block) */
	XBOX_FT_W_PB_DWORDS,     /* pushbuffer dwords written, R_BeginFrame's pb_reset to R_EndFrame */
	XBOX_FT_W_TEX_BINDS,     /* texture stages programmed (NV097_SET_TEXTURE_OFFSET writes) */
	XBOX_FT_W_VERTS_WORLD,   /* world and brush-entity indices submitted */
	XBOX_FT_W_VERTS_STUDIO,  /* studio-model vertices (the TriAPI path, inside R_StudioDrawPoints) */
	XBOX_FT_W_VERTS_TRI,     /* other 3D TriAPI vertices: sprites, beams, particles, water, sky */
	XBOX_FT_W_VERTS_2D,      /* 2D vertices: HUD, console, menus, this overlay */
	XBOX_FT_W_VERTS_DECAL,   /* decal vertices */
	XBOX_FT_W_WORLD_FACES,   /* world faces left after PVS, frustum and backface culling */
	XBOX_FT_W_PVS_LEAVES,    /* leaves in the current PVS (a state: the count at its last recompute) */
	XBOX_FT_W_STUDIO_MODELS, /* studio models drawn */
	XBOX_FT_W_STUDIO_TRIS,   /* studio mesh triangles drawn */
	XBOX_FT_W_LM_FACES,      /* lightmap faces rebuilt (divergences.md #162) */
	XBOX_FT_W_LM_TEXELS,     /* lightmap texels (luxels) rebuilt */
	XBOX_FT_W_WATER_VERTS,   /* water vertices warped on the CPU (#166) */
	XBOX_FT_W_DECALS,        /* decals drawn (#167) */
	XBOX_FT_W_PARTICLES,     /* particles drawn (#161) */
	XBOX_FT_W_BEAMS,         /* beams drawn (#161) */
	XBOX_FT_W_SPRITES,       /* sprite models drawn */
	XBOX_FT_W_COUNT
};

/* The COUNTERS chunk's names, indexed by XBOX_FT_W_*; each fits in
   XBOX_FT_COUNTER_NAME_BYTES with its NUL. */
extern const char *const xbox_ft_work_names[XBOX_FT_W_COUNT];

/* Frame flags. */
#define XBOX_FT_F_ACTIVE     0x0001u /* in game: cls.state == ca_active, signon complete */
#define XBOX_FT_F_MENU       0x0002u /* a menu, the console or a pause has the screen */
#define XBOX_FT_F_FLUSH      0x0004u /* this frame wrote the file (its time includes the write) */
#define XBOX_FT_F_SATURATED  0x0008u /* a tick count did not fit in 32 bits */
#define XBOX_FT_F_OVERLAY    0x0010u /* the overlay was drawn in this frame */
/* This frame's Host_Frame queued a host state change - a changelevel, a
   map or savegame load, a new game, a disconnect (GameState->nextstate
   left != STATE_RUNFRAME, engine/common/host_state.c). The state machine
   runs it at the start of the next COM_Frame, before that frame's
   Host_Frame (host_state.c:205-224), so the whole load lands in THIS
   frame's time between host frames (total - host), while the frame still
   started in play. A reserved bit until 2026-10-01: older files leave it
   clear, and tools/frametime-report.py finds those frames by their shape
   instead. Not a format change - readers ignore flags they do not know. */
#define XBOX_FT_F_LOAD       0x0020u

#define XBOX_FT_MAP_F_NO_LOAD_START 0x0001u

/* The overlay's and the report's two guidelines, microseconds. A frame
   counts as "over" a guideline when it is strictly longer (a frame at
   exactly the 60 Hz period made the refresh). */
#define XBOX_FT_GUIDE_60_US  16667u
#define XBOX_FT_GUIDE_30_US  33333u

/* Histogram for the percentile: 0.25 ms bins up to 250 ms; slower frames
   land in the last bin (only its lower edge is known). */
#define XBOX_FT_HIST_BIN_US  250u
#define XBOX_FT_HIST_BINS    1000u

typedef struct xbox_ft_frame_s
{
	uint32_t seq;
	uint32_t total;
	uint32_t host;
	uint32_t gpu;
	uint32_t vblank;
	uint32_t swap;
	uint32_t overlay;
	uint16_t flags;
	uint16_t reserved;
	uint32_t work[XBOX_FT_W_COUNT];
} xbox_ft_frame_t;

/* The in-memory ring. `frames` is caller-owned storage of `capacity`
   entries (the recorder allocates it only while recording). `next_seq`
   numbers pushes; `flushed_seq` is the first sequence number not yet in
   the file. */
typedef struct xbox_ft_ring_s
{
	xbox_ft_frame_t *frames;
	uint32_t capacity;
	uint32_t next_seq;
	uint32_t flushed_seq;
	uint32_t lost;          /* frames overwritten before they were flushed */
} xbox_ft_ring_t;

void xbox_ft_ring_init( xbox_ft_ring_t *ring, xbox_ft_frame_t *storage, uint32_t capacity );
/* Stores `frame` with the next sequence number (written into it). When
   the oldest unflushed frame is overwritten it is counted in `lost` and
   flushed_seq moves past it. */
void xbox_ft_ring_push( xbox_ft_ring_t *ring, xbox_ft_frame_t *frame );
uint32_t xbox_ft_ring_unflushed( const xbox_ft_ring_t *ring );
/* The unflushed frames as at most two contiguous spans of the storage
   (the second is non-empty only when they wrap). Returns the span count. */
uint32_t xbox_ft_ring_unflushed_spans( const xbox_ft_ring_t *ring, const xbox_ft_frame_t **span0, uint32_t *count0, const xbox_ft_frame_t **span1, uint32_t *count1 );
void xbox_ft_ring_mark_flushed( xbox_ft_ring_t *ring );
/* The frame `back` pushes ago (0 = the latest), or NULL if the ring no
   longer holds it. */
const xbox_ft_frame_t *xbox_ft_ring_recent( const xbox_ft_ring_t *ring, uint32_t back );

/* Ticks to microseconds at `tsc_hz`, and a 64-bit tick span to a
   saturating 32-bit field. */
uint32_t xbox_ft_ticks_to_us( uint64_t ticks, uint64_t tsc_hz );
uint32_t xbox_ft_saturate( uint64_t ticks, uint16_t *flags );
/* CPU ticks of a frame: host minus its waits, never negative. */
uint32_t xbox_ft_frame_cpu( const xbox_ft_frame_t *frame );

/* Running statistics over a set of frames (the overlay keeps one per
   map), in microseconds. */
typedef struct xbox_ft_stats_s
{
	uint32_t frames;
	uint64_t total_us;
	uint64_t cpu_us;
	uint64_t gpu_us;
	uint32_t over_60;       /* frames longer than 16.7 ms */
	uint32_t over_30;       /* frames longer than 33.3 ms */
	uint32_t worst_us;
	uint32_t hist[XBOX_FT_HIST_BINS];
} xbox_ft_stats_t;

void xbox_ft_stats_reset( xbox_ft_stats_t *stats );
void xbox_ft_stats_add( xbox_ft_stats_t *stats, uint32_t total_us, uint32_t cpu_us, uint32_t gpu_us );
/* The frame time at or under which `permille`/1000 of the frames fall,
   from the histogram - the upper edge of the bin that crosses the rank
   (at most one bin, 0.25 ms, pessimistic). "1% low fps" is 1e6 over
   xbox_ft_stats_percentile_us( stats, 990 ): the frame rate the slowest
   1% of frames fall below. 0 with no frames. */
uint32_t xbox_ft_stats_percentile_us( const xbox_ft_stats_t *stats, uint32_t permille );

/* Byte encoders. Each writes exactly the bytes documented above into
   `out` (which must have room) and returns the count written.
   `counters` is how many work counters each frame record carries: the
   recorder passes XBOX_FT_W_COUNT; 0 writes a version-1 file (no
   COUNTERS chunk, 32-byte records). The header, the frames chunk heads
   and every frame of one file must be given the same value. */
size_t xbox_ft_frame_bytes( uint32_t counters );
size_t xbox_ft_encode_header( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters );
/* The COUNTERS chunk: chunk head, count, then xbox_ft_work_names[0..counters). */
size_t xbox_ft_encode_counters( uint8_t *out, uint32_t counters );
size_t xbox_ft_encode_frames_chunk_head( uint8_t *out, uint32_t first_seq, uint32_t count, uint32_t counters );
size_t xbox_ft_encode_frame( uint8_t *out, const xbox_ft_frame_t *frame, uint32_t counters );
size_t xbox_ft_encode_map( uint8_t *out, uint64_t tsc, uint64_t load_ticks, uint32_t free_before, uint32_t free_after, uint32_t total_pages, uint32_t flags, const char *name );
size_t xbox_ft_encode_calib( uint8_t *out, uint64_t tsc, uint64_t qpc );
size_t xbox_ft_encode_end( uint8_t *out, uint64_t tsc, uint32_t lost );

#endif /* XBOX_FRAMETIME_UTIL_H */
