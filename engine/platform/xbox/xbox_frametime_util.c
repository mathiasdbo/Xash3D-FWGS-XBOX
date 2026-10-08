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
	"k_sv_reliable", "k_sv_fullpack", "k_sv_clientdata", "k_sv_emit_ents", "k_sv_xmit",
	"k_sv_runcmd",
	"k_cl_pkt_ents", "k_cl_clientdata",
	"k_cl_cmd", "k_cl_predmove", "k_cl_wpacket",
	"k_snd_spatial", "k_snd_mix", "k_snd_dsp", "k_snd_out",
	"k_sndx_mix", "k_sndx_dsp", "k_sndx_out",
	"k_pvs", "k_sky", "k_world_walk", "k_warp_world", "k_world_decals",
	"k_brush", "k_warp_brush", "k_warp_trans",
	"k_sv_start", "k_sv_pstep", "k_sv_ppush", "k_sv_ptoss", "k_sv_pnone", "k_sv_pother",
	"k_save", "k_save_init", "k_save_ents", "k_save_hl1", "k_save_client", "k_save_globals",
	"k_save_age", "k_save_sav",
	"k_scrshot", "k_shot_read", "k_shot_resample", "k_shot_write",
	"k_ui", "k_ui_file", "k_ui_pic", "k_snd_music",
	"k_save_pump",
	/* the counts after the spans */
	"world_walked", "water_gpu", "vp_loads",
	"g_ents", "g_thinks", "g_touches", "g_moves", "g_dll_traces", "g_clip_ents",
	"g_links", "g_push_tests", "g_walkmoves", "g_find_visits",
	"g_think_kc", "g_touch_kc", "g_move_kc", "g_worst_kc", "g_worst_cls", "g_worst_ent",
	"g_push_replays",
	"snd_loads", "snd_load_kb", "snd_load_kc", "tex_loads", "tex_load_kb",
	"cl_parse_kc",
	"save_pump_kb", "save_flush_kc",
	"pm_sv_cmds", "pm_cl_cmds", "pm_cl_redo", "pm_cl_solids",
	"pm_sv_physents", "pm_cl_physents", "pm_sv_visents", "pm_cl_visents",
	"pm_sv_traces", "pm_cl_traces", "pm_sv_hulls", "pm_cl_hulls",
	"pm_sv_tests", "pm_cl_tests", "pm_sv_ptests", "pm_cl_ptests",
	"pm_sv_pcont", "pm_cl_pcont", "pm_sv_tex", "pm_cl_tex",
	"pm_sv_setup_kc", "pm_sv_move_kc", "pm_sv_query_kc", "pm_sv_think_kc", "pm_sv_touch_kc",
	"pm_cl_solid_kc", "pm_cl_move_kc", "pm_cl_query_kc", "pm_cl_post_kc",
	"pm_sv_culls", "pm_cl_culls",
	"ce_ents", "ce_deltas", "ce_nochange", "ce_fields", "ce_visents",
	"ce_delta_kc", "ce_fields_kc", "ce_process_kc",
	"ce_link_kc", "ce_add_kc", "ce_tents_kc", "ce_events_kc", "ce_pitch_kc",
	"ce_solid_skips",
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
	[XBOX_FT_S_SV_RELIABLE]   = XBOX_FT_S_SV_SEND,
	[XBOX_FT_S_SV_FULLPACK]   = XBOX_FT_S_SV_SEND,
	[XBOX_FT_S_SV_CLIENTDATA] = XBOX_FT_S_SV_SEND,
	[XBOX_FT_S_SV_EMIT_ENTS]  = XBOX_FT_S_SV_SEND,
	[XBOX_FT_S_SV_XMIT]       = XBOX_FT_S_SV_SEND,
	[XBOX_FT_S_SV_RUNCMD]     = XBOX_FT_S_SV_READ,
	[XBOX_FT_S_CL_PKT_ENTS]   = XBOX_FT_S_CL_READ,
	[XBOX_FT_S_CL_CLIENTDATA] = XBOX_FT_S_CL_READ,
	[XBOX_FT_S_CL_CMD]        = XBOX_FT_S_CL_BEGIN,
	[XBOX_FT_S_CL_PREDMOVE]   = XBOX_FT_S_CL_CMD,
	[XBOX_FT_S_CL_WPACKET]    = XBOX_FT_S_CL_BEGIN,
	[XBOX_FT_S_SND_SPATIAL]   = XBOX_FT_S_SND,
	[XBOX_FT_S_SND_MIX]       = XBOX_FT_S_SND,
	[XBOX_FT_S_SND_DSP]       = XBOX_FT_S_SND,
	[XBOX_FT_S_SND_OUT]       = XBOX_FT_S_SND,
	[XBOX_FT_S_SNDX_MIX]      = XBOX_FT_S_SND_EXTRA,
	[XBOX_FT_S_SNDX_DSP]      = XBOX_FT_S_SND_EXTRA,
	[XBOX_FT_S_SNDX_OUT]      = XBOX_FT_S_SND_EXTRA,
	[XBOX_FT_S_PVS]           = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_SKY]           = XBOX_FT_S_WORLD,
	[XBOX_FT_S_WORLD_WALK]    = XBOX_FT_S_WORLD,
	[XBOX_FT_S_WARP_WORLD]    = XBOX_FT_S_WORLD,
	[XBOX_FT_S_WORLD_DECALS]  = XBOX_FT_S_WORLD,
	[XBOX_FT_S_BRUSH]         = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_WARP_BRUSH]    = XBOX_FT_S_BRUSH,
	[XBOX_FT_S_WARP_TRANS]    = XBOX_FT_S_R_SCENE,
	[XBOX_FT_S_SV_START]      = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SV_PSTEP]      = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SV_PPUSH]      = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SV_PTOSS]      = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SV_PNONE]      = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SV_POTHER]     = XBOX_FT_S_SV_GAME,
	[XBOX_FT_S_SAVE]          = XBOX_FT_S_CL_BEGIN,
	[XBOX_FT_S_SAVE_INIT]     = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_ENTS]     = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_HL1]      = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_CLIENT]   = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_GLOBALS]  = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_AGE]      = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SAVE_SAV]      = XBOX_FT_S_SAVE,
	[XBOX_FT_S_SCRSHOT]       = XBOX_FT_S_SCR,
	[XBOX_FT_S_SHOT_READ]     = XBOX_FT_S_SCRSHOT,
	[XBOX_FT_S_SHOT_RESAMPLE] = XBOX_FT_S_SCRSHOT,
	[XBOX_FT_S_SHOT_WRITE]    = XBOX_FT_S_SCRSHOT,
	[XBOX_FT_S_UI]            = XBOX_FT_S_SCR,
	[XBOX_FT_S_UI_FILE]       = XBOX_FT_S_UI,
	[XBOX_FT_S_UI_PIC]        = XBOX_FT_S_UI,
	[XBOX_FT_S_SND_MUSIC]     = XBOX_FT_S_SND,
	[XBOX_FT_S_SAVE_PUMP]     = -1,
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
	p = put_u32( p, counters ? XBOX_FT_VERSION_SINGLE : XBOX_FT_VERSION_NO_WORK );
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
	if( n > XBOX_FT_S_COUNT )
		n = XBOX_FT_S_COUNT; /* the counts after the spans are not spans */

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
	return xbox_ft_encode_segment_end( out, tsc, lost, XBOX_FT_SEG_SESSION );
}

size_t xbox_ft_encode_segment_end( uint8_t *out, uint64_t tsc, uint32_t lost, uint32_t next )
{
	uint8_t *p = out;

	p = put_u32( p, XBOX_FT_CHUNK_END );
	p = put_u32( p, XBOX_FT_END_BYTES );
	p = put_u64( p, tsc );
	p = put_u32( p, lost );
	p = put_u32( p, next );
	return (size_t)( p - out );
}

size_t xbox_ft_encode_classes( uint8_t *out, const xbox_ft_class_t *classes, uint32_t count )
{
	uint8_t *p = out;
	uint32_t i;

	p = put_u32( p, XBOX_FT_CHUNK_CLASSES );
	p = put_u32( p, 4u + count * XBOX_FT_CLASS_BYTES );
	p = put_u32( p, count );
	for( i = 0; i < count; i++ )
	{
		const xbox_ft_class_t *c = &classes[i];

		p = put_name( p, c->name, XBOX_FT_CLASS_NAME_BYTES );
		p = put_u32( p, c->movetype );
		p = put_u32( p, c->runs );
		p = put_u32( p, c->thinks );
		p = put_u32( p, c->moves );
		p = put_u32( p, xbox_ft_span_kcyc( c->phys ));
		p = put_u32( p, xbox_ft_span_kcyc( c->think ));
		p = put_u32( p, xbox_ft_span_kcyc( c->worst ));
	}
	return (size_t)( p - out );
}

size_t xbox_ft_encode_notes( uint8_t *out, const xbox_ft_note_t *notes, uint32_t count, uint32_t dropped )
{
	uint8_t *p = out;
	uint32_t i;

	p = put_u32( p, XBOX_FT_CHUNK_NOTES );
	p = put_u32( p, 8u + count * XBOX_FT_NOTE_BYTES );
	p = put_u32( p, count );
	p = put_u32( p, dropped );
	for( i = 0; i < count; i++ )
	{
		const xbox_ft_note_t *n = &notes[i];

		p = put_u32( p, n->seq );
		p = put_u32( p, n->kind );
		p = put_u32( p, n->kcyc );
		p = put_u32( p, n->bytes );
		p = put_name( p, n->name, XBOX_FT_NOTE_NAME_BYTES );
	}
	return (size_t)( p - out );
}

/* Sessions and segments (version 3, 2026-10-07): the layout and the name
   budget are in xbox_frametime_util.h. */

static int label_char_ok( char c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_';
}

static int label_edge( char c )
{
	return c == '-' || c == '_';
}

size_t xbox_ft_label_sanitise( const char *label, char *out )
{
	size_t n = 0;
	int replaced = 0;

	for( ; label && *label && n < XBOX_FT_LABEL_MAX; label++ )
	{
		if( label_char_ok( *label ))
		{
			if( n == 0 && label_edge( *label ))
				continue; /* no leading separator */
			out[n++] = *label;
			replaced = 0;
		}
		else if( n > 0 && !replaced )
		{
			/* One '-' for a whole run of other characters: "PR #190" is
			   "PR-190", not "PR--190". */
			out[n++] = '-';
			replaced = 1;
		}
	}
	while( n > 0 && label_edge( out[n - 1] ))
		n--;
	if( n == 0 )
	{
		memcpy( out, XBOX_FT_LABEL_EMPTY, sizeof( XBOX_FT_LABEL_EMPTY ));
		return sizeof( XBOX_FT_LABEL_EMPTY ) - 1;
	}
	out[n] = 0;
	return n;
}

/* splitmix64's finaliser: every input bit reaches every output bit. */
static uint64_t mix64( uint64_t z )
{
	z = ( z ^ ( z >> 30 )) * 0xBF58476D1CE4E5B9ull;
	z = ( z ^ ( z >> 27 )) * 0x94D049BB133111EBull;
	return z ^ ( z >> 31 );
}

void xbox_ft_session_id( const uint64_t *inputs, uint32_t n, uint8_t id[XBOX_FT_SESSION_ID_BYTES] )
{
	uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xD1B54A32D192ED03ull;
	uint32_t i;

	for( i = 0; i < n; i++ )
	{
		a = mix64( a ^ inputs[i] );
		b = mix64( b + inputs[i] + a );
	}
	put_u64( id, a );
	put_u64( id + 8, b );
	/* RFC 4122's random-UUID marks, so the full id prints as one: version
	   4 in byte 6's high nibble, variant 10 in byte 8's top bits. The
	   name's 8 hex digits are bytes 0-3, untouched by either. */
	id[6] = (uint8_t)(( id[6] & 0x0Fu ) | 0x40u );
	id[8] = (uint8_t)(( id[8] & 0x3Fu ) | 0x80u );
}

size_t xbox_ft_segment_name( char *out, const char *label, const uint8_t id[XBOX_FT_SESSION_ID_BYTES], uint32_t seq )
{
	static const char hex[] = "0123456789abcdef";
	char *p = out;
	uint32_t i;

	memcpy( p, "ft_", 3 );
	p += 3;
	p += xbox_ft_label_sanitise( label, p );
	*p++ = '_';
	for( i = 0; i < XBOX_FT_SESSION_HEX / 2; i++ )
	{
		*p++ = hex[id[i] >> 4];
		*p++ = hex[id[i] & 15];
	}
	*p++ = '_';
	if( seq > XBOX_FT_SEGMENT_SEQ_MAX )
		seq = XBOX_FT_SEGMENT_SEQ_MAX;
	*p++ = (char)( '0' + seq / 100 );
	*p++ = (char)( '0' + seq / 10 % 10 );
	*p++ = (char)( '0' + seq % 10 );
	memcpy( p, ".bin", 5 );
	p += 4;
	return (size_t)( p - out );
}

uint32_t xbox_ft_segment_rotation( int map_start, uint32_t seg_frames, uint64_t seg_bytes, uint64_t cap_bytes )
{
	if( map_start )
		return seg_frames ? XBOX_FT_SEG_MAP : 0;
	if( cap_bytes && seg_bytes >= cap_bytes )
		return XBOX_FT_SEG_SIZE;
	return 0;
}

size_t xbox_ft_encode_header_v3( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters, const xbox_ft_segment_t *seg )
{
	uint8_t *p = out + xbox_ft_encode_header( out, ring_frames, tsc_hz, qpc_hz, tsc_open, qpc_open, mode, build, counters );

	/* The v2 header with this version and size. */
	put_u32( out + 8, XBOX_FT_VERSION );
	put_u32( out + 12, XBOX_FT_HEADER_V3_BYTES );

	memcpy( p, seg->session, XBOX_FT_SESSION_ID_BYTES );
	p += XBOX_FT_SESSION_ID_BYTES;
	p = put_u32( p, seg->seq );
	p = put_u32( p, seg->reason );
	p = put_u32( p, seg->first_frame );
	p = put_u32( p, 0 );
	p = put_u64( p, seg->tsc_open );
	p = put_u64( p, seg->systime );
	p = put_name( p, seg->label, XBOX_FT_LABEL_BYTES );
	memset( p, 0, XBOX_FT_HEADER_V3_BYTES - (size_t)( p - out ));
	return XBOX_FT_HEADER_V3_BYTES;
}

size_t xbox_ft_encode_segment_open( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters, const xbox_ft_segment_t *seg )
{
	size_t n = xbox_ft_encode_header_v3( out, ring_frames, tsc_hz, qpc_hz, tsc_open, qpc_open, mode, build, counters, seg );

	n += xbox_ft_encode_counters( out + n, counters );
	n += xbox_ft_encode_spans( out + n, counters );
	return n;
}
