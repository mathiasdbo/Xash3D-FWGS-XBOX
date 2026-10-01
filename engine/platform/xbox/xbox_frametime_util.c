/*
xbox_frametime_util.c - the frame-time recorder's pure parts
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

/* Resonance3D (docs/r3d/divergences.md #141). Layout and meaning of every
   field: xbox_frametime_util.h. No engine, kernel or GPU dependency - this
   file is compiled for the host by tools/host-tests/run.sh. */

#include <string.h>
#include "xbox_frametime_util.h"

const char *const xbox_ft_work_names[XBOX_FT_W_COUNT] =
{
	"draws", "prims", "pb_dwords", "tex_binds",
	"v_world", "v_studio", "v_tri", "v_2d", "v_decal",
	"world_faces", "pvs_leaves",
	"studio_models", "studio_tris",
	"lm_faces", "lm_texels", "water_verts",
	"decals", "particles", "beams", "sprites",
	"span_calls",
	/* XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_*, in that enum's order */
	"k_input", "k_cl_begin",
	"k_sv", "k_sv_read", "k_sv_game", "k_sv_send",
	"k_cl", "k_cl_dll", "k_cl_read", "k_cl_pred", "k_cl_emit",
	"k_scr", "k_r_scene", "k_lm", "k_world", "k_client_studio", "k_studio_points", "k_skin_read",
	"k_efx", "k_tri_hooks", "k_hud", "k_snd_extra", "k_r_end",
	"k_snd",
};

/* A name list one short (or one long) of the enum is a compile error,
   not a shifted file. */
typedef char xbox_ft_names_match_enum[( sizeof( xbox_ft_work_names ) / sizeof( xbox_ft_work_names[0] ) == XBOX_FT_W_COUNT ) ? 1 : -1];

const int8_t xbox_ft_span_parent[XBOX_FT_S_COUNT] =
{
	[XBOX_FT_S_INPUT]         = -1,
	[XBOX_FT_S_CL_BEGIN]      = -1,
	[XBOX_FT_S_SV]            = -1,
	[XBOX_FT_S_SV_READ]       = XBOX_FT_S_SV,
	[XBOX_FT_S_SV_GAME]       = XBOX_FT_S_SV,
	[XBOX_FT_S_SV_SEND]       = XBOX_FT_S_SV,
	[XBOX_FT_S_CL]            = -1,
	[XBOX_FT_S_CL_DLL]        = XBOX_FT_S_CL,
	[XBOX_FT_S_CL_READ]       = XBOX_FT_S_CL,
	[XBOX_FT_S_CL_PRED]       = XBOX_FT_S_CL,
	[XBOX_FT_S_CL_EMIT]       = XBOX_FT_S_CL,
	[XBOX_FT_S_SCR]           = XBOX_FT_S_CL,
	[XBOX_FT_S_R_SCENE]       = XBOX_FT_S_SCR,
	[XBOX_FT_S_LM]            = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_WORLD]         = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_CLIENT_STUDIO] = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_STUDIO_POINTS] = XBOX_FT_S_CLIENT_STUDIO,
	[XBOX_FT_S_SKIN_READ]     = XBOX_FT_S_STUDIO_POINTS,
	[XBOX_FT_S_EFX]           = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_TRI_HOOKS]     = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_HUD]           = XBOX_FT_S_SCR,
	[XBOX_FT_S_SND_EXTRA]     = XBOX_FT_S_SCR,
	[XBOX_FT_S_R_END]         = XBOX_FT_S_SCR,
	[XBOX_FT_S_SND]           = XBOX_FT_S_CL,
};

uint32_t xbox_ft_span_kcyc( uint64_t ticks )
{
	uint64_t k = ( ticks + 500u ) / 1000u;

	return k > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)k;
}

uint64_t xbox_ft_span_self( const uint64_t *span, uint64_t *self )
{
	uint64_t children[XBOX_FT_S_COUNT];
	uint64_t top = 0;
	int i;

	memset( children, 0, sizeof( children ));
	for( i = 0; i < XBOX_FT_S_COUNT; i++ )
	{
		if( xbox_ft_span_parent[i] < 0 )
			top += span[i];
		else
			children[xbox_ft_span_parent[i]] += span[i];
	}
	for( i = 0; i < XBOX_FT_S_COUNT; i++ )
		self[i] = span[i] > children[i] ? span[i] - children[i] : 0;
	return top;
}

void xbox_ft_ring_init( xbox_ft_ring_t *ring, xbox_ft_frame_t *storage, uint32_t capacity )
{
	ring->frames = storage;
	ring->capacity = capacity;
	ring->next_seq = 0;
	ring->flushed_seq = 0;
	ring->lost = 0;
}

void xbox_ft_ring_push( xbox_ft_ring_t *ring, xbox_ft_frame_t *frame )
{
	if( ring->capacity == 0 )
		return;

	/* The slot about to be reused still holds an unflushed frame: the
	   file will never see it. Counted, and the flush point moves on so
	   the next flush writes a consecutive run (the reader sees the gap
	   in the sequence numbers). */
	if( ring->next_seq - ring->flushed_seq >= ring->capacity )
	{
		ring->flushed_seq++;
		ring->lost++;
	}

	frame->seq = ring->next_seq;
	ring->frames[ring->next_seq % ring->capacity] = *frame;
	ring->next_seq++;
}

uint32_t xbox_ft_ring_unflushed( const xbox_ft_ring_t *ring )
{
	return ring->next_seq - ring->flushed_seq;
}

uint32_t xbox_ft_ring_unflushed_spans( const xbox_ft_ring_t *ring, const xbox_ft_frame_t **span0, uint32_t *count0, const xbox_ft_frame_t **span1, uint32_t *count1 )
{
	uint32_t n = xbox_ft_ring_unflushed( ring );
	uint32_t first, to_end;

	*span0 = *span1 = NULL;
	*count0 = *count1 = 0;

	if( n == 0 || ring->capacity == 0 )
		return 0;

	first = ring->flushed_seq % ring->capacity;
	to_end = ring->capacity - first;

	*span0 = &ring->frames[first];
	if( n <= to_end )
	{
		*count0 = n;
		return 1;
	}

	*count0 = to_end;
	*span1 = &ring->frames[0];
	*count1 = n - to_end;
	return 2;
}

void xbox_ft_ring_mark_flushed( xbox_ft_ring_t *ring )
{
	ring->flushed_seq = ring->next_seq;
}

const xbox_ft_frame_t *xbox_ft_ring_recent( const xbox_ft_ring_t *ring, uint32_t back )
{
	uint32_t held = ring->next_seq < ring->capacity ? ring->next_seq : ring->capacity;

	if( back >= held )
		return NULL;

	return &ring->frames[( ring->next_seq - 1 - back ) % ring->capacity];
}

uint32_t xbox_ft_ticks_to_us( uint64_t ticks, uint64_t tsc_hz )
{
	uint64_t us;

	if( tsc_hz == 0 )
		return 0;

	/* ticks < 2^32 here in practice (frame fields); a 64-bit product
	   overflows only past ~2^44 ticks (6.6 hours at 733 MHz), where the
	   division first keeps it in range. */
	if( ticks < ( (uint64_t)1 << 44 ))
		us = ticks * 1000000u / tsc_hz;
	else
		us = ticks / tsc_hz * 1000000u;

	return us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us;
}

uint32_t xbox_ft_saturate( uint64_t ticks, uint16_t *flags )
{
	if( ticks > 0xFFFFFFFFu )
	{
		*flags |= XBOX_FT_F_SATURATED;
		return 0xFFFFFFFFu;
	}
	return (uint32_t)ticks;
}

uint32_t xbox_ft_frame_cpu( const xbox_ft_frame_t *frame )
{
	uint64_t waits = (uint64_t)frame->gpu + frame->vblank + frame->swap;

	return waits >= frame->host ? 0 : (uint32_t)( frame->host - waits );
}

void xbox_ft_stats_reset( xbox_ft_stats_t *stats )
{
	memset( stats, 0, sizeof( *stats ));
}

void xbox_ft_stats_add( xbox_ft_stats_t *stats, uint32_t total_us, uint32_t cpu_us, uint32_t gpu_us )
{
	uint32_t bin = total_us / XBOX_FT_HIST_BIN_US;

	if( bin >= XBOX_FT_HIST_BINS )
		bin = XBOX_FT_HIST_BINS - 1;

	stats->frames++;
	stats->total_us += total_us;
	stats->cpu_us += cpu_us;
	stats->gpu_us += gpu_us;
	if( total_us > XBOX_FT_GUIDE_60_US )
		stats->over_60++;
	if( total_us > XBOX_FT_GUIDE_30_US )
		stats->over_30++;
	if( total_us > stats->worst_us )
		stats->worst_us = total_us;
	stats->hist[bin]++;
}

uint32_t xbox_ft_stats_percentile_us( const xbox_ft_stats_t *stats, uint32_t permille )
{
	uint64_t rank, seen = 0;
	uint32_t i;

	if( stats->frames == 0 )
		return 0;
	if( permille > 1000 )
		permille = 1000;

	/* The smallest bin edge with at least ceil(frames * permille / 1000)
	   frames at or under it. */
	rank = ( (uint64_t)stats->frames * permille + 999 ) / 1000;
	if( rank == 0 )
		rank = 1;

	for( i = 0; i < XBOX_FT_HIST_BINS; i++ )
	{
		seen += stats->hist[i];
		if( seen >= rank )
		{
			/* The last bin is open-ended: its only known bound is the
			   worst frame itself. */
			if( i == XBOX_FT_HIST_BINS - 1 )
				return stats->worst_us;
			return ( i + 1 ) * XBOX_FT_HIST_BIN_US;
		}
	}
	return stats->worst_us;
}

static uint8_t *put_u16( uint8_t *p, uint16_t v )
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)( v >> 8 );
	return p + 2;
}

static uint8_t *put_u32( uint8_t *p, uint32_t v )
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)( v >> 8 );
	p[2] = (uint8_t)( v >> 16 );
	p[3] = (uint8_t)( v >> 24 );
	return p + 4;
}

static uint8_t *put_u64( uint8_t *p, uint64_t v )
{
	p = put_u32( p, (uint32_t)v );
	return put_u32( p, (uint32_t)( v >> 32 ));
}

/* A NUL-padded fixed field; always leaves at least one NUL. */
static uint8_t *put_name( uint8_t *p, const char *s, size_t field )
{
	size_t n = s ? strlen( s ) : 0;

	if( n > field - 1 )
		n = field - 1;
	memset( p, 0, field );
	if( n )
		memcpy( p, s, n );
	return p + field;
}

size_t xbox_ft_frame_bytes( uint32_t counters )
{
	if( counters > XBOX_FT_W_COUNT )
		counters = XBOX_FT_W_COUNT;
	return XBOX_FT_FRAME_BYTES + 4u * counters;
}

size_t xbox_ft_encode_header( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters )
{
	uint8_t *p = out;

	memcpy( p, "R3DFTIME", 8 );
	p += 8;
	p = put_u32( p, counters ? XBOX_FT_VERSION : XBOX_FT_VERSION_NO_WORK );
	p = put_u32( p, XBOX_FT_HEADER_BYTES );
	p = put_u32( p, (uint32_t)xbox_ft_frame_bytes( counters ));
	p = put_u32( p, ring_frames );
	p = put_u64( p, tsc_hz );
	p = put_u64( p, qpc_hz );
	p = put_u64( p, tsc_open );
	p = put_u64( p, qpc_open );
	p = put_u32( p, mode );
	p = put_u32( p, 0 );
	p = put_name( p, build, 64 );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_counters( uint8_t *out, uint32_t counters )
{
	uint8_t *p = out;
	uint32_t i;

	if( counters > XBOX_FT_W_COUNT )
		counters = XBOX_FT_W_COUNT;
	p = put_u32( p, XBOX_FT_CHUNK_COUNTERS );
	p = put_u32( p, 4u + counters * XBOX_FT_COUNTER_NAME_BYTES );
	p = put_u32( p, counters );
	for( i = 0; i < counters; i++ )
		p = put_name( p, xbox_ft_work_names[i], XBOX_FT_COUNTER_NAME_BYTES );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_spans( uint8_t *out, uint32_t counters )
{
	uint8_t *p = out;
	uint32_t i, n;

	if( counters > XBOX_FT_W_COUNT )
		counters = XBOX_FT_W_COUNT;
	if( counters <= XBOX_FT_W_SPAN_FIRST )
		return 0;
	n = counters - XBOX_FT_W_SPAN_FIRST;

	p = put_u32( p, XBOX_FT_CHUNK_SPANS );
	p = put_u32( p, 4u + n * 2u * XBOX_FT_COUNTER_NAME_BYTES );
	p = put_u32( p, n );
	for( i = 0; i < n; i++ )
	{
		const int parent = xbox_ft_span_parent[i];

		p = put_name( p, xbox_ft_work_names[XBOX_FT_W_SPAN_FIRST + i], XBOX_FT_COUNTER_NAME_BYTES );
		p = put_name( p, parent < 0 ? "" : xbox_ft_work_names[XBOX_FT_W_SPAN_FIRST + parent], XBOX_FT_COUNTER_NAME_BYTES );
	}
	return (size_t)( p - out );
}

size_t xbox_ft_encode_frames_chunk_head( uint8_t *out, uint32_t first_seq, uint32_t count, uint32_t counters )
{
	uint8_t *p = out;

	p = put_u32( p, XBOX_FT_CHUNK_FRAMES );
	p = put_u32( p, 8u + count * (uint32_t)xbox_ft_frame_bytes( counters ));
	p = put_u32( p, first_seq );
	p = put_u32( p, count );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_frame( uint8_t *out, const xbox_ft_frame_t *frame, uint32_t counters )
{
	uint8_t *p = out;
	uint32_t i;

	p = put_u32( p, frame->seq );
	p = put_u32( p, frame->total );
	p = put_u32( p, frame->host );
	p = put_u32( p, frame->gpu );
	p = put_u32( p, frame->vblank );
	p = put_u32( p, frame->swap );
	p = put_u32( p, frame->overlay );
	p = put_u16( p, frame->flags );
	p = put_u16( p, 0 );
	if( counters > XBOX_FT_W_COUNT )
		counters = XBOX_FT_W_COUNT;
	for( i = 0; i < counters; i++ )
		p = put_u32( p, frame->work[i] );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_map( uint8_t *out, uint64_t tsc, uint64_t load_ticks, uint32_t free_before, uint32_t free_after, uint32_t total_pages, uint32_t flags, const char *name )
{
	uint8_t *p = out;

	p = put_u32( p, XBOX_FT_CHUNK_MAP );
	p = put_u32( p, XBOX_FT_MAP_BYTES );
	p = put_u64( p, tsc );
	p = put_u64( p, load_ticks );
	p = put_u32( p, free_before );
	p = put_u32( p, free_after );
	p = put_u32( p, total_pages );
	p = put_u32( p, flags );
	p = put_name( p, name, XBOX_FT_NAME_BYTES );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_calib( uint8_t *out, uint64_t tsc, uint64_t qpc )
{
	uint8_t *p = out;

	p = put_u32( p, XBOX_FT_CHUNK_CALIB );
	p = put_u32( p, XBOX_FT_CALIB_BYTES );
	p = put_u64( p, tsc );
	p = put_u64( p, qpc );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_end( uint8_t *out, uint64_t tsc, uint32_t lost )
{
	uint8_t *p = out;

	p = put_u32( p, XBOX_FT_CHUNK_END );
	p = put_u32( p, XBOX_FT_END_BYTES );
	p = put_u64( p, tsc );
	p = put_u32( p, lost );
	p = put_u32( p, 0 );
	return (size_t)( p - out );
}
