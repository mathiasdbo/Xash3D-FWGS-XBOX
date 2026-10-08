/* Host test for engine/platform/xbox/xbox_frametime_util.c - the frame
   ring, the statistics (1% low, guideline counts) and the file encoders.
   With path arguments it also writes small recordings through the same
   encoders - a version-2 file with work counters and a version-1 file
   without, and a level-change session both with the recorder's LOAD flag
   (v2) and without it (v1, as recorders before 2026-10-01 wrote it) -
   which tools/test_frametime_report.py then parses: the parser's fixtures
   come from the encoder the recorder runs, not from a hand-written copy
   of the format. The spans fixture (2026-10-01) is a current recording:
   every counter, the SPANS chunk, and known span times - and, from
   2026-10-05, the sv_game breakdown: its spans, g_* counters and a
   CLASSES chunk. */

#include <stdio.h>
#include <string.h>
#include "xbox_frametime_util.h"

static int failures;

/* Every encoder output goes through one stack buffer per test: sized
   from the largest chunk (docs/r3d/lessons.md, "NEVER size a scratch
   buffer from one of the things written into it"), with room to grow. */
#define TEST_BUF_BYTES 8192
typedef char test_buf_fits_spans[( XBOX_FT_SPANS_CHUNK_BYTES <= TEST_BUF_BYTES ) ? 1 : -1];
typedef char test_buf_fits_counters[( XBOX_FT_CHUNK_BYTES + 4 + XBOX_FT_W_COUNT * XBOX_FT_COUNTER_NAME_BYTES <= TEST_BUF_BYTES ) ? 1 : -1];

#define CHECK( cond ) do { if( !( cond )) { fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

static uint32_t rd32( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64( const uint8_t *p ) { return rd32( p ) | (uint64_t)rd32( p + 4 ) << 32; }

static void test_ring( void )
{
	xbox_ft_frame_t store[4], f;
	xbox_ft_ring_t ring;
	const xbox_ft_frame_t *s0, *s1;
	uint32_t c0, c1, i;

	xbox_ft_ring_init( &ring, store, 4 );
	CHECK( xbox_ft_ring_unflushed( &ring ) == 0 );
	CHECK( xbox_ft_ring_unflushed_spans( &ring, &s0, &c0, &s1, &c1 ) == 0 );
	CHECK( xbox_ft_ring_recent( &ring, 0 ) == NULL );

	for( i = 0; i < 3; i++ )
	{
		memset( &f, 0, sizeof( f ));
		f.total = 100 + i;
		xbox_ft_ring_push( &ring, &f );
		CHECK( f.seq == i );
	}
	CHECK( xbox_ft_ring_unflushed( &ring ) == 3 );
	CHECK( xbox_ft_ring_unflushed_spans( &ring, &s0, &c0, &s1, &c1 ) == 1 );
	CHECK( c0 == 3 && s0 == &store[0] && c1 == 0 );
	CHECK( xbox_ft_ring_recent( &ring, 0 )->total == 102 );
	CHECK( xbox_ft_ring_recent( &ring, 2 )->total == 100 );
	CHECK( xbox_ft_ring_recent( &ring, 3 ) == NULL );

	xbox_ft_ring_mark_flushed( &ring );
	CHECK( xbox_ft_ring_unflushed( &ring ) == 0 );

	/* Seq 3..5 land in slots 3, 0, 1: two spans. */
	for( i = 3; i < 6; i++ )
	{
		memset( &f, 0, sizeof( f ));
		f.total = 100 + i;
		xbox_ft_ring_push( &ring, &f );
	}
	CHECK( xbox_ft_ring_unflushed_spans( &ring, &s0, &c0, &s1, &c1 ) == 2 );
	CHECK( s0 == &store[3] && c0 == 1 && s0->seq == 3 );
	CHECK( s1 == &store[0] && c1 == 2 && s1[0].seq == 4 && s1[1].seq == 5 );
	CHECK( ring.lost == 0 );
	CHECK( xbox_ft_ring_recent( &ring, 3 )->seq == 2 );

	/* Two more without a flush (seq 6, 7): the ring holds 4, so seq 7
	   overwrites seq 3, which is lost; the flush point moves past it. */
	for( i = 6; i < 8; i++ )
	{
		memset( &f, 0, sizeof( f ));
		xbox_ft_ring_push( &ring, &f );
	}
	CHECK( ring.lost == 1 );
	CHECK( xbox_ft_ring_unflushed( &ring ) == 4 );
	CHECK( ring.flushed_seq == 4 );
	CHECK( xbox_ft_ring_unflushed_spans( &ring, &s0, &c0, &s1, &c1 ) >= 1 );
	CHECK( s0->seq == 4 );
	CHECK( c0 + c1 == 4 );
}

static void test_units( void )
{
	uint16_t flags = 0;
	xbox_ft_frame_t f;

	CHECK( xbox_ft_ticks_to_us( 733333333u, 733333333u ) == 1000000u );
	CHECK( xbox_ft_ticks_to_us( 12222222u, 733333333u ) == 16666u );
	CHECK( xbox_ft_ticks_to_us( 5, 0 ) == 0 );
	CHECK( xbox_ft_ticks_to_us( (uint64_t)1 << 50, 1000000u ) == 0xFFFFFFFFu );

	CHECK( xbox_ft_saturate( 5, &flags ) == 5 && flags == 0 );
	CHECK( xbox_ft_saturate( (uint64_t)1 << 33, &flags ) == 0xFFFFFFFFu && ( flags & XBOX_FT_F_SATURATED ));

	memset( &f, 0, sizeof( f ));
	f.host = 1000; f.gpu = 100; f.vblank = 200; f.swap = 50;
	CHECK( xbox_ft_frame_cpu( &f ) == 650 );
	f.gpu = 900;
	CHECK( xbox_ft_frame_cpu( &f ) == 0 );
}

static void test_stats( void )
{
	static xbox_ft_stats_t st;
	int i;

	xbox_ft_stats_reset( &st );
	CHECK( xbox_ft_stats_percentile_us( &st, 990 ) == 0 );

	/* 990 frames at 16.0 ms, 10 at 40.0 ms. */
	for( i = 0; i < 990; i++ )
		xbox_ft_stats_add( &st, 16000, 12000, 1000 );
	for( i = 0; i < 10; i++ )
		xbox_ft_stats_add( &st, 40000, 30000, 5000 );

	CHECK( st.frames == 1000 );
	CHECK( st.over_60 == 10 && st.over_30 == 10 );
	CHECK( st.worst_us == 40000 );
	CHECK( st.total_us == 990ull * 16000 + 10ull * 40000 );
	CHECK( st.cpu_us == 990ull * 12000 + 10ull * 30000 );
	/* Rank 990 is still a 16 ms frame (bin [16.0, 16.25)); rank 991 is a
	   40 ms one. */
	CHECK( xbox_ft_stats_percentile_us( &st, 990 ) == 16250 );
	CHECK( xbox_ft_stats_percentile_us( &st, 991 ) == 40250 );
	CHECK( xbox_ft_stats_percentile_us( &st, 1000 ) == 40250 );

	/* The guideline edges: exactly the period is not "over". */
	xbox_ft_stats_reset( &st );
	xbox_ft_stats_add( &st, XBOX_FT_GUIDE_60_US, 0, 0 );
	xbox_ft_stats_add( &st, XBOX_FT_GUIDE_60_US + 1, 0, 0 );
	xbox_ft_stats_add( &st, XBOX_FT_GUIDE_30_US, 0, 0 );
	xbox_ft_stats_add( &st, XBOX_FT_GUIDE_30_US + 1, 0, 0 );
	CHECK( st.over_60 == 3 && st.over_30 == 1 );

	/* A frame past the histogram's range: the percentile is the worst
	   frame, not the bin edge. */
	xbox_ft_stats_reset( &st );
	xbox_ft_stats_add( &st, 400000, 0, 0 );
	CHECK( xbox_ft_stats_percentile_us( &st, 990 ) == 400000 );
}

static void test_encoders( void )
{
	uint8_t buf[TEST_BUF_BYTES];
	xbox_ft_frame_t f;
	uint32_t i;

	/* Version 2: 328-byte records (32 + 20 counts, span_calls, 50
	   spans - 24 from 2026-10-01, 11 round-trip, 7 mixer and 8 scene
	   ones from 2026-10-02 - and world_walked, water_gpu and vp_loads
	   after them); 112 bytes for a file from before the spans (20). */
	CHECK( XBOX_FT_W_SPAN_CALLS == 20 && XBOX_FT_W_SPAN_FIRST == 21 && XBOX_FT_S_COUNT == 73 );
	CHECK( XBOX_FT_W_WORLD_WALKED == 94 && XBOX_FT_W_WATER_GPU == 95 && XBOX_FT_W_VP_LOADS == 96 );
	CHECK( XBOX_FT_W_G_ENTS == 97 && XBOX_FT_W_G_WORST_ENT == 112 && XBOX_FT_W_G_PUSH_REPLAYS == 113 );
	/* The first-use load counters (2026-10-06), then the deferred
	   savegame write's (2026-10-06, #153) close the record. */
	CHECK( XBOX_FT_W_SND_LOADS == 114 && XBOX_FT_W_SND_LOAD_KC == 116 && XBOX_FT_W_TEX_LOAD_KB == 118 && XBOX_FT_W_CL_PARSE_KC == 119 );
	CHECK( XBOX_FT_W_SAVE_PUMP_KB == 120 && XBOX_FT_W_SAVE_FLUSH_KC == 121 );
	/* The player movement's (2026-10-06, x9-perf-pmove) close it now:
	   every _SV counter with its _CL twin right after it. */
	CHECK( XBOX_FT_W_PM_SV_CMDS == 122 && XBOX_FT_W_PM_CL_SOLIDS == 125 && XBOX_FT_W_PM_SV_PHYSENTS == 126 );
	CHECK( XBOX_FT_W_PM_CL_PHYSENTS == XBOX_FT_W_PM_SV_PHYSENTS + 1 && XBOX_FT_W_PM_CL_VISENTS == XBOX_FT_W_PM_SV_VISENTS + 1 );
	CHECK( XBOX_FT_W_PM_CL_TRACES == XBOX_FT_W_PM_SV_TRACES + 1 && XBOX_FT_W_PM_CL_HULLS == XBOX_FT_W_PM_SV_HULLS + 1 );
	CHECK( XBOX_FT_W_PM_CL_TESTS == XBOX_FT_W_PM_SV_TESTS + 1 && XBOX_FT_W_PM_CL_PTESTS == XBOX_FT_W_PM_SV_PTESTS + 1 );
	CHECK( XBOX_FT_W_PM_CL_PCONT == XBOX_FT_W_PM_SV_PCONT + 1 && XBOX_FT_W_PM_CL_TEX == XBOX_FT_W_PM_SV_TEX + 1 );
	CHECK( XBOX_FT_W_PM_SV_SETUP_KC == 142 && XBOX_FT_W_PM_CL_POST_KC == 150 );
	CHECK( XBOX_FT_W_PM_SV_CULLS == 151 && XBOX_FT_W_PM_CL_CULLS == 152 );
	/* The client entity breakdown's (2026-10-07, x9-perf-clents) close
	   it now: five counts, eight times and the physent-list skips (#156). */
	CHECK( XBOX_FT_W_CE_ENTS == 153 && XBOX_FT_W_CE_VISENTS == 157 && XBOX_FT_W_CE_DELTA_KC == 158 );
	CHECK( XBOX_FT_W_CE_EVENTS_KC == 164 && XBOX_FT_W_CE_PITCH_KC == 165 && XBOX_FT_W_CE_SOLID_SKIPS == 166 );
	CHECK( XBOX_FT_W_COUNT == 167 );
	CHECK( xbox_ft_frame_bytes( XBOX_FT_W_COUNT ) == 700 && xbox_ft_frame_bytes( 0 ) == 32 );
	CHECK( xbox_ft_frame_bytes( XBOX_FT_W_SPAN_CALLS ) == 112 );
	CHECK( xbox_ft_encode_header( buf, 1024, 733000000u, 3375000u, 0x1122334455667788ull, 42, 2, "build x", XBOX_FT_W_COUNT ) == XBOX_FT_HEADER_BYTES );
	CHECK( rd32( buf + 8 ) == 2 && rd32( buf + 16 ) == 700 );
	/* Version 1 is the same header without counters. */
	CHECK( xbox_ft_encode_header( buf, 1024, 733000000u, 3375000u, 0x1122334455667788ull, 42, 2, "build x", 0 ) == XBOX_FT_HEADER_BYTES );
	CHECK( memcmp( buf, "R3DFTIME", 8 ) == 0 );
	CHECK( rd32( buf + 8 ) == 1 && rd32( buf + 12 ) == 128 && rd32( buf + 16 ) == 32 && rd32( buf + 20 ) == 1024 );
	CHECK( rd64( buf + 24 ) == 733000000u && rd64( buf + 32 ) == 3375000u );
	CHECK( rd64( buf + 40 ) == 0x1122334455667788ull && rd64( buf + 48 ) == 42 );
	CHECK( rd32( buf + 56 ) == 2 && rd32( buf + 60 ) == 0 );
	CHECK( strcmp( (const char *)buf + 64, "build x" ) == 0 && buf[127] == 0 );

	/* An over-long build id is truncated, NUL kept. */
	{
		char longname[100];
		memset( longname, 'a', sizeof( longname ) - 1 );
		longname[99] = 0;
		xbox_ft_encode_header( buf, 1, 1, 1, 1, 1, 1, longname, 0 );
		CHECK( buf[127] == 0 && buf[126] == 'a' );
	}

	memset( &f, 0, sizeof( f ));
	f.seq = 7; f.total = 0xAABBCCDD; f.host = 2; f.gpu = 3; f.vblank = 4; f.swap = 5; f.overlay = 6; f.flags = 0x8001;
	for( i = 0; i < XBOX_FT_W_COUNT; i++ )
		f.work[i] = 0x01000000u + i;
	CHECK( xbox_ft_encode_frame( buf, &f, 0 ) == XBOX_FT_FRAME_BYTES );
	CHECK( rd32( buf ) == 7 && buf[4] == 0xDD && buf[7] == 0xAA && rd32( buf + 24 ) == 6 );
	CHECK( buf[28] == 0x01 && buf[29] == 0x80 && buf[30] == 0 && buf[31] == 0 );
	CHECK( xbox_ft_encode_frame( buf, &f, XBOX_FT_W_COUNT ) == 700 );
	CHECK( rd32( buf ) == 7 && buf[28] == 0x01 && buf[29] == 0x80 );
	CHECK( rd32( buf + 32 ) == 0x01000000u && rd32( buf + 32 + 4 * XBOX_FT_W_LM_TEXELS ) == 0x01000000u + XBOX_FT_W_LM_TEXELS );
	CHECK( rd32( buf + 696 ) == 0x01000000u + XBOX_FT_W_COUNT - 1 );

	CHECK( xbox_ft_encode_frames_chunk_head( buf, 10, 3, 0 ) == 16 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_FRAMES && rd32( buf + 4 ) == 8 + 3 * 32 && rd32( buf + 8 ) == 10 && rd32( buf + 12 ) == 3 );
	CHECK( xbox_ft_encode_frames_chunk_head( buf, 10, 3, XBOX_FT_W_COUNT ) == 16 );
	CHECK( rd32( buf + 4 ) == 8 + 3 * 700 );

	/* The COUNTERS chunk: every name, NUL-padded to 16 bytes. */
	CHECK( xbox_ft_encode_counters( buf, XBOX_FT_W_COUNT ) == 8 + 4 + XBOX_FT_W_COUNT * 16 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_COUNTERS && rd32( buf + 4 ) == 4 + XBOX_FT_W_COUNT * 16 && rd32( buf + 8 ) == XBOX_FT_W_COUNT );
	CHECK( strcmp( (const char *)buf + 12, "draws" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PB_DWORDS, "pb_dwords" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SPRITES, "sprites" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SPAN_CALLS, "span_calls" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV_GAME ), "k_sv_game" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_CLIENT_STUDIO ), "k_client_studio" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SND ), "k_snd" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV_CLIENTDATA ), "k_sv_clientdata" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_CL_WPACKET ), "k_cl_wpacket" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SND_SPATIAL ), "k_snd_spatial" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SNDX_OUT ), "k_sndx_out" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_WORLD_WALK ), "k_world_walk" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_WARP_TRANS ), "k_warp_trans" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV_START ), "k_sv_start" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV_POTHER ), "k_sv_pother" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE ), "k_save" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_SAV ), "k_save_sav" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SCRSHOT ), "k_scrshot" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SHOT_WRITE ), "k_shot_write" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_UI ), "k_ui" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SND_MUSIC ), "k_snd_music" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_PUMP ), "k_save_pump" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_COUNT - 1 ), "k_save_pump" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_WORLD_WALKED, "world_walked" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_WATER_GPU, "water_gpu" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_VP_LOADS, "vp_loads" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_G_ENTS, "g_ents" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_G_FIND_VISITS, "g_find_visits" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_G_THINK_KC, "g_think_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_G_WORST_ENT, "g_worst_ent" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_G_PUSH_REPLAYS, "g_push_replays" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SND_LOADS, "snd_loads" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SND_LOAD_KB, "snd_load_kb" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SND_LOAD_KC, "snd_load_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_TEX_LOADS, "tex_loads" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_TEX_LOAD_KB, "tex_load_kb" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CL_PARSE_KC, "cl_parse_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SAVE_PUMP_KB, "save_pump_kb" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_SAVE_FLUSH_KC, "save_flush_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_SV_CMDS, "pm_sv_cmds" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_CL_REDO, "pm_cl_redo" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_CL_HULLS, "pm_cl_hulls" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_SV_TEX, "pm_sv_tex" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_SV_SETUP_KC, "pm_sv_setup_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_CL_QUERY_KC, "pm_cl_query_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_CL_POST_KC, "pm_cl_post_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_PM_CL_CULLS, "pm_cl_culls" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_ENTS, "ce_ents" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_NOCHANGE, "ce_nochange" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_VISENTS, "ce_visents" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_FIELDS_KC, "ce_fields_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_PROCESS_KC, "ce_process_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_EVENTS_KC, "ce_events_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_PITCH_KC, "ce_pitch_kc" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_CE_SOLID_SKIPS, "ce_solid_skips" ) == 0 );
	CHECK( XBOX_FT_W_CE_SOLID_SKIPS == XBOX_FT_W_COUNT - 1 );
	for( i = 0; i < XBOX_FT_W_COUNT; i++ )
		CHECK( xbox_ft_work_names[i] && strlen( xbox_ft_work_names[i] ) < XBOX_FT_COUNTER_NAME_BYTES && buf[12 + 16 * i + 15] == 0 );

	CHECK( xbox_ft_encode_map( buf, 1, 2, 3, 4, 5, 6, "c1a0e" ) == XBOX_FT_CHUNK_BYTES + XBOX_FT_MAP_BYTES );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_MAP && rd32( buf + 4 ) == XBOX_FT_MAP_BYTES );
	CHECK( rd64( buf + 8 ) == 1 && rd64( buf + 16 ) == 2 && rd32( buf + 24 ) == 3 && rd32( buf + 36 ) == 6 );
	CHECK( strcmp( (const char *)buf + 40, "c1a0e" ) == 0 );

	CHECK( xbox_ft_encode_calib( buf, 9, 10 ) == XBOX_FT_CHUNK_BYTES + XBOX_FT_CALIB_BYTES );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_CALIB && rd64( buf + 8 ) == 9 && rd64( buf + 16 ) == 10 );
	CHECK( xbox_ft_encode_end( buf, 11, 12 ) == XBOX_FT_CHUNK_BYTES + XBOX_FT_END_BYTES );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_END && rd64( buf + 8 ) == 11 && rd32( buf + 16 ) == 12 );
}

/* The spans: their names, the parent table, kilocycle rounding, the
   own-time split and the SPANS chunk. */
static void test_spans( void )
{
	uint8_t buf[TEST_BUF_BYTES];
	uint64_t span[XBOX_FT_S_COUNT], self[XBOX_FT_S_COUNT];
	uint32_t i;

	for( i = 0; i < XBOX_FT_S_COUNT; i++ )
	{
		const char *name = xbox_ft_work_names[XBOX_FT_W_SPAN_FIRST + i];

		CHECK( name[0] == 'k' && name[1] == '_' );
		/* A parent always comes first, so one pass in order sees it. */
		CHECK( xbox_ft_span_parent[i] < (int)i );
	}
	/* The tree the report and the docs describe. */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_INPUT] == -1 && xbox_ft_span_parent[XBOX_FT_S_CL_BEGIN] == -1 );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV] == -1 && xbox_ft_span_parent[XBOX_FT_S_CL] == -1 );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_GAME] == XBOX_FT_S_SV );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SCR] == XBOX_FT_S_CL && xbox_ft_span_parent[XBOX_FT_S_SND] == XBOX_FT_S_CL );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_CLIENT_STUDIO] == XBOX_FT_S_R_SCENE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_STUDIO_POINTS] == XBOX_FT_S_CLIENT_STUDIO );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SKIN_READ] == XBOX_FT_S_STUDIO_POINTS );
	/* The round trip's breakdown (2026-10-02). */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_RELIABLE] == XBOX_FT_S_SV_SEND && xbox_ft_span_parent[XBOX_FT_S_SV_FULLPACK] == XBOX_FT_S_SV_SEND );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_CLIENTDATA] == XBOX_FT_S_SV_SEND && xbox_ft_span_parent[XBOX_FT_S_SV_EMIT_ENTS] == XBOX_FT_S_SV_SEND );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_XMIT] == XBOX_FT_S_SV_SEND && xbox_ft_span_parent[XBOX_FT_S_SV_RUNCMD] == XBOX_FT_S_SV_READ );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_CL_PKT_ENTS] == XBOX_FT_S_CL_READ && xbox_ft_span_parent[XBOX_FT_S_CL_CLIENTDATA] == XBOX_FT_S_CL_READ );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_CL_CMD] == XBOX_FT_S_CL_BEGIN && xbox_ft_span_parent[XBOX_FT_S_CL_PREDMOVE] == XBOX_FT_S_CL_CMD );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_CL_WPACKET] == XBOX_FT_S_CL_BEGIN );
	/* The mixer's breakdown (2026-10-02): one set per mixer entry point. */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SND_SPATIAL] == XBOX_FT_S_SND && xbox_ft_span_parent[XBOX_FT_S_SND_MIX] == XBOX_FT_S_SND );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SND_DSP] == XBOX_FT_S_SND && xbox_ft_span_parent[XBOX_FT_S_SND_OUT] == XBOX_FT_S_SND );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SNDX_MIX] == XBOX_FT_S_SND_EXTRA && xbox_ft_span_parent[XBOX_FT_S_SNDX_DSP] == XBOX_FT_S_SND_EXTRA );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SNDX_OUT] == XBOX_FT_S_SND_EXTRA );
	/* The scene's breakdown (2026-10-02). */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_PVS] == XBOX_FT_S_R_SCENE && xbox_ft_span_parent[XBOX_FT_S_SKY] == XBOX_FT_S_WORLD );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_WORLD_WALK] == XBOX_FT_S_WORLD && xbox_ft_span_parent[XBOX_FT_S_WARP_WORLD] == XBOX_FT_S_WORLD );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_WORLD_DECALS] == XBOX_FT_S_WORLD && xbox_ft_span_parent[XBOX_FT_S_BRUSH] == XBOX_FT_S_R_SCENE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_WARP_BRUSH] == XBOX_FT_S_BRUSH && xbox_ft_span_parent[XBOX_FT_S_WARP_TRANS] == XBOX_FT_S_R_SCENE );
	/* The server game frame's breakdown (2026-10-05). */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_START] == XBOX_FT_S_SV_GAME && xbox_ft_span_parent[XBOX_FT_S_SV_PSTEP] == XBOX_FT_S_SV_GAME );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_PPUSH] == XBOX_FT_S_SV_GAME && xbox_ft_span_parent[XBOX_FT_S_SV_PTOSS] == XBOX_FT_S_SV_GAME );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SV_PNONE] == XBOX_FT_S_SV_GAME && xbox_ft_span_parent[XBOX_FT_S_SV_POTHER] == XBOX_FT_S_SV_GAME );
	/* The savegame's breakdown (2026-10-05). */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SAVE] == XBOX_FT_S_CL_BEGIN && xbox_ft_span_parent[XBOX_FT_S_SAVE_INIT] == XBOX_FT_S_SAVE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SAVE_ENTS] == XBOX_FT_S_SAVE && xbox_ft_span_parent[XBOX_FT_S_SAVE_HL1] == XBOX_FT_S_SAVE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SAVE_CLIENT] == XBOX_FT_S_SAVE && xbox_ft_span_parent[XBOX_FT_S_SAVE_GLOBALS] == XBOX_FT_S_SAVE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SAVE_AGE] == XBOX_FT_S_SAVE && xbox_ft_span_parent[XBOX_FT_S_SAVE_SAV] == XBOX_FT_S_SAVE );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SCRSHOT] == XBOX_FT_S_SCR && xbox_ft_span_parent[XBOX_FT_S_SHOT_READ] == XBOX_FT_S_SCRSHOT );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_SHOT_RESAMPLE] == XBOX_FT_S_SCRSHOT && xbox_ft_span_parent[XBOX_FT_S_SHOT_WRITE] == XBOX_FT_S_SCRSHOT );
	/* The menu and the music (2026-10-06). */
	CHECK( xbox_ft_span_parent[XBOX_FT_S_UI] == XBOX_FT_S_SCR && xbox_ft_span_parent[XBOX_FT_S_UI_FILE] == XBOX_FT_S_UI );
	CHECK( xbox_ft_span_parent[XBOX_FT_S_UI_PIC] == XBOX_FT_S_UI && xbox_ft_span_parent[XBOX_FT_S_SND_MUSIC] == XBOX_FT_S_SND );
	for( i = 0; i < XBOX_FT_S_COUNT; i++ )
		CHECK( xbox_ft_span_parent[i] < (int)i ); /* every parent before its children */

	/* Kilocycles, rounded half up, saturating. */
	CHECK( xbox_ft_span_kcyc( 0 ) == 0 && xbox_ft_span_kcyc( 499 ) == 0 && xbox_ft_span_kcyc( 500 ) == 1 );
	CHECK( xbox_ft_span_kcyc( 1499 ) == 1 && xbox_ft_span_kcyc( 733333333u ) == 733333 );
	CHECK( xbox_ft_span_kcyc( (uint64_t)1 << 50 ) == 0xFFFFFFFFu );

	/* Own time: a parent minus its children; the sum of the top level. */
	memset( span, 0, sizeof( span ));
	span[XBOX_FT_S_SV] = 600;
	span[XBOX_FT_S_SV_GAME] = 400;
	span[XBOX_FT_S_SV_SEND] = 50;
	span[XBOX_FT_S_CL] = 1000;
	span[XBOX_FT_S_SCR] = 900;
	span[XBOX_FT_S_R_SCENE] = 700;
	span[XBOX_FT_S_CLIENT_STUDIO] = 300;
	span[XBOX_FT_S_STUDIO_POINTS] = 120;
	span[XBOX_FT_S_SKIN_READ] = 200; /* ran outside its parent too: more than it */
	span[XBOX_FT_S_INPUT] = 7;
	CHECK( xbox_ft_span_self( span, self ) == 600 + 1000 + 7 );
	CHECK( self[XBOX_FT_S_SV] == 150 && self[XBOX_FT_S_SV_GAME] == 400 );
	CHECK( self[XBOX_FT_S_CL] == 100 && self[XBOX_FT_S_SCR] == 200 && self[XBOX_FT_S_R_SCENE] == 400 );
	CHECK( self[XBOX_FT_S_CLIENT_STUDIO] == 180 );
	CHECK( self[XBOX_FT_S_STUDIO_POINTS] == 0 ); /* clamped, not negative */
	CHECK( self[XBOX_FT_S_SKIN_READ] == 200 && self[XBOX_FT_S_INPUT] == 7 && self[XBOX_FT_S_SND] == 0 );

	/* The SPANS chunk: { span, parent } name pairs, "" at the top. */
	CHECK( xbox_ft_encode_spans( buf, XBOX_FT_W_COUNT ) == XBOX_FT_SPANS_CHUNK_BYTES );
	CHECK( XBOX_FT_SPANS_CHUNK_BYTES == 8 + 4 + 73 * 32 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_SPANS && rd32( buf + 4 ) == 4 + 73 * 32 && rd32( buf + 8 ) == 73 );
	/* The last pair is the last span, not world_walked after it. */
	CHECK( strcmp( (const char *)buf + 12 + 32 * 49, "k_warp_trans" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 49 + 16, "k_r_scene" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 55, "k_sv_pother" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 55 + 16, "k_sv_game" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 56, "k_save" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 56 + 16, "k_cl_begin" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 67, "k_shot_write" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 67 + 16, "k_scrshot" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 68, "k_ui" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 68 + 16, "k_scr" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 70, "k_ui_pic" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 70 + 16, "k_ui" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * 71, "k_snd_music" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 71 + 16, "k_snd" ) == 0 );
	/* The last pair is the last span, a top-level one (2026-10-06, #153). */
	CHECK( strcmp( (const char *)buf + 12 + 32 * 72, "k_save_pump" ) == 0 && buf[12 + 32 * 72 + 16] == 0 );
	CHECK( strcmp( (const char *)buf + 12, "k_input" ) == 0 && buf[12 + 16] == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * XBOX_FT_S_STUDIO_POINTS, "k_studio_points" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * XBOX_FT_S_STUDIO_POINTS + 16, "k_client_studio" ) == 0 );
	/* A file whose records stop before the spans has no SPANS chunk; one
	   cut inside them names only the ones it carries. */
	CHECK( xbox_ft_encode_spans( buf, XBOX_FT_W_SPAN_FIRST ) == 0 && xbox_ft_encode_spans( buf, 0 ) == 0 );
	CHECK( xbox_ft_encode_spans( buf, XBOX_FT_W_SPAN_FIRST + 3 ) == 8 + 4 + 3 * 32 && rd32( buf + 8 ) == 3 );
}

/* The CLASSES chunk (2026-10-05): its head, one 60-byte row per class,
   names NUL-padded and truncated, ticks rounded to kilocycles. */
static void test_classes( void )
{
	uint8_t buf[512];
	xbox_ft_class_t c[2];

	memset( c, 0, sizeof( c ));
	strcpy( c[0].name, "monster_scientist" );
	c[0].movetype = 4;
	c[0].runs = 10;
	c[0].thinks = 3;
	c[0].moves = 77;
	c[0].phys = 2499;
	c[0].think = 1500;
	c[0].worst = (uint64_t)1 << 50;
	memset( c[1].name, 'x', sizeof( c[1].name ) - 1 ); /* the longest name: 31 characters and its NUL */

	CHECK( XBOX_FT_CLASS_BYTES == 60 );
	CHECK( xbox_ft_encode_classes( buf, c, 0 ) == 12 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_CLASSES && rd32( buf + 4 ) == 4 && rd32( buf + 8 ) == 0 );
	CHECK( xbox_ft_encode_classes( buf, c, 2 ) == 12 + 2 * 60 );
	CHECK( rd32( buf + 4 ) == 4 + 2 * 60 && rd32( buf + 8 ) == 2 );
	CHECK( strcmp( (const char *)buf + 12, "monster_scientist" ) == 0 && buf[12 + 31] == 0 );
	CHECK( rd32( buf + 44 ) == 4 && rd32( buf + 48 ) == 10 && rd32( buf + 52 ) == 3 && rd32( buf + 56 ) == 77 );
	CHECK( rd32( buf + 60 ) == 2 && rd32( buf + 64 ) == 2 && rd32( buf + 68 ) == 0xFFFFFFFFu );
	CHECK( buf[72] == 'x' && buf[72 + 30] == 'x' && buf[72 + 31] == 0 && rd32( buf + 72 + 32 ) == 0 );
}

/* The NOTES chunk (2026-10-06): count, dropped, then 64-byte notes with
   the name NUL-padded and truncated to 47 characters. */
static void test_notes( void )
{
	uint8_t buf[512];
	xbox_ft_note_t n[2];

	memset( n, 0, sizeof( n ));
	n[0].seq = 77073;
	n[0].kind = XBOX_FT_NOTE_SOUND;
	n[0].kcyc = 219000;
	n[0].bytes = 130000;
	strcpy( n[0].name, "fvox/flatline.wav" );
	n[1].kind = XBOX_FT_NOTE_MSG;
	memset( n[1].name, 'y', sizeof( n[1].name ) - 1 );

	CHECK( XBOX_FT_NOTE_BYTES == 64 && XBOX_FT_NOTES_CHUNK_BYTES( 2 ) == 8 + 8 + 128 );
	CHECK( xbox_ft_encode_notes( buf, n, 0, 3 ) == 16 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_NOTES && rd32( buf + 4 ) == 8 && rd32( buf + 8 ) == 0 && rd32( buf + 12 ) == 3 );
	CHECK( xbox_ft_encode_notes( buf, n, 2, 0 ) == XBOX_FT_NOTES_CHUNK_BYTES( 2 ));
	CHECK( rd32( buf + 4 ) == 8 + 128 && rd32( buf + 8 ) == 2 && rd32( buf + 12 ) == 0 );
	CHECK( rd32( buf + 16 ) == 77073 && rd32( buf + 20 ) == XBOX_FT_NOTE_SOUND && rd32( buf + 24 ) == 219000 && rd32( buf + 28 ) == 130000 );
	CHECK( strcmp( (const char *)buf + 32, "fvox/flatline.wav" ) == 0 && buf[32 + 47] == 0 );
	CHECK( rd32( buf + 80 + 4 ) == XBOX_FT_NOTE_MSG && buf[80 + 16] == 'y' && buf[80 + 16 + 46] == 'y' && buf[80 + 16 + 47] == 0 );
}

/* Sessions and segments (v3, 2026-10-07): the label's sanitising, the
   session id, the file name and its 42-character budget, the rotation
   decision and the v3 header's bytes. */
static void test_segments( void )
{
	static uint8_t buf[XBOX_FT_SEGMENT_OPEN_BYTES + 64];
	char out[64];
	uint8_t id[XBOX_FT_SESSION_ID_BYTES], id2[XBOX_FT_SESSION_ID_BYTES];
	uint64_t in[3] = { 1, 2, 3 };
	xbox_ft_segment_t seg;
	size_t n;

	/* The label: kept, replaced (one '-' per run), trimmed, cut, empty. */
	CHECK( xbox_ft_label_sanitise( "pr190-clents", out ) == 12 && strcmp( out, "pr190-clents" ) == 0 );
	CHECK( xbox_ft_label_sanitise( "route_c0a0", out ) == 10 && strcmp( out, "route_c0a0" ) == 0 );
	CHECK( xbox_ft_label_sanitise( "PR #190: clents!", out ) && strcmp( out, "PR-190-clents" ) == 0 );
	CHECK( xbox_ft_label_sanitise( "  --x9 perf.saves__ ", out ) && strcmp( out, "x9-perf-saves" ) == 0 );
	CHECK( xbox_ft_label_sanitise( "", out ) == 8 && strcmp( out, XBOX_FT_LABEL_EMPTY ) == 0 );
	CHECK( xbox_ft_label_sanitise( NULL, out ) == 8 && strcmp( out, "untagged" ) == 0 );
	CHECK( xbox_ft_label_sanitise( " .:/ ", out ) == 8 && strcmp( out, "untagged" ) == 0 );
	CHECK( xbox_ft_label_sanitise( "abcdefghijklmnopqrstuvwxyz", out ) == XBOX_FT_LABEL_MAX && strcmp( out, "abcdefghijklmnopqrstuv" ) == 0 );
	/* Cut right after a separator: the separator goes too. */
	CHECK( xbox_ft_label_sanitise( "abcdefghijklmnopqrstu_vwxyz", out ) == 21 && strcmp( out, "abcdefghijklmnopqrstu" ) == 0 );

	/* The id: deterministic in its inputs, different for different ones,
	   and marked as a version-4 UUID. */
	xbox_ft_session_id( in, 3, id );
	xbox_ft_session_id( in, 3, id2 );
	CHECK( memcmp( id, id2, sizeof( id )) == 0 );
	in[1] = 4;
	xbox_ft_session_id( in, 3, id2 );
	CHECK( memcmp( id, id2, 4 ) != 0 );
	CHECK(( id[6] >> 4 ) == 4 && ( id[8] & 0xC0u ) == 0x80u );

	/* The name: ft_<label>_<8 hex>_<seq>.bin, 20 + the label. */
	memset( id, 0, sizeof( id ));
	id[0] = 0x0a; id[1] = 0xbc; id[2] = 0xde; id[3] = 0xf1; id[4] = 0xff;
	n = xbox_ft_segment_name( out, "pr190-clents", id, 7 );
	CHECK( n == 32 && strcmp( out, "ft_pr190-clents_0abcdef1_007.bin" ) == 0 );
	n = xbox_ft_segment_name( out, "", id, 0 );
	CHECK( strcmp( out, "ft_untagged_0abcdef1_000.bin" ) == 0 && n == 28 );
	/* The longest: a 22-character label and the last segment number
	   make exactly FATX's 42; a longer label or number cannot pass it. */
	n = xbox_ft_segment_name( out, "abcdefghijklmnopqrstuvwxyz0123", id, 5000 );
	CHECK( n == XBOX_FT_FILE_NAME_MAX && strlen( out ) == 42 );
	CHECK( strcmp( out, "ft_abcdefghijklmnopqrstuv_0abcdef1_999.bin" ) == 0 );
	n = xbox_ft_segment_name( out, "a b", id, 123 );
	CHECK( strcmp( out, "ft_a-b_0abcdef1_123.bin" ) == 0 );

	/* Rotation: at a map start only when the segment has frames; else at
	   the cap, none without one. */
	CHECK( xbox_ft_segment_rotation( 1, 0, 5000, 0 ) == 0 );
	CHECK( xbox_ft_segment_rotation( 1, 1, 10, 0 ) == XBOX_FT_SEG_MAP );
	CHECK( xbox_ft_segment_rotation( 1, 1, 1u << 30, 1024 ) == XBOX_FT_SEG_MAP );
	CHECK( xbox_ft_segment_rotation( 0, 100, 1023, 1024 ) == 0 );
	CHECK( xbox_ft_segment_rotation( 0, 100, 1024, 1024 ) == XBOX_FT_SEG_SIZE );
	CHECK( xbox_ft_segment_rotation( 0, 0, 4096, 1024 ) == XBOX_FT_SEG_SIZE );
	CHECK( xbox_ft_segment_rotation( 0, 100, (uint64_t)1 << 40, 0 ) == 0 );

	/* The v3 header: the v2 fields with version 3 and 256 bytes, then
	   the segment's. */
	memset( &seg, 0, sizeof( seg ));
	memcpy( seg.session, id, sizeof( id ));
	seg.systime = 0x01DC000000000001ull;
	strcpy( seg.label, "pr190-clents" );
	seg.seq = 4;
	seg.reason = XBOX_FT_SEG_SIZE;
	seg.first_frame = 12345;
	seg.tsc_open = 0x1122334455667788ull;
	memset( buf, 0xCC, sizeof( buf ));
	CHECK( xbox_ft_encode_header_v3( buf, 1024, 733000000u, 3375000u, 77, 42, 2, "build y", XBOX_FT_W_COUNT, &seg ) == XBOX_FT_HEADER_V3_BYTES );
	CHECK( memcmp( buf, "R3DFTIME", 8 ) == 0 && rd32( buf + 8 ) == 3 && rd32( buf + 12 ) == 256 && rd32( buf + 16 ) == 700 );
	CHECK( rd64( buf + 40 ) == 77 && rd64( buf + 48 ) == 42 && rd32( buf + 56 ) == 2 && strcmp( (const char *)buf + 64, "build y" ) == 0 );
	CHECK( memcmp( buf + 128, id, 16 ) == 0 );
	CHECK( rd32( buf + 144 ) == 4 && rd32( buf + 148 ) == XBOX_FT_SEG_SIZE && rd32( buf + 152 ) == 12345 && rd32( buf + 156 ) == 0 );
	CHECK( rd64( buf + 160 ) == 0x1122334455667788ull && rd64( buf + 168 ) == 0x01DC000000000001ull );
	CHECK( strcmp( (const char *)buf + 176, "pr190-clents" ) == 0 && buf[207] == 0 );
	{
		int zero = 1, i;

		for( i = 208; i < 256; i++ )
			zero &= buf[i] == 0;
		CHECK( zero && buf[256] == 0xCC );
	}
	/* What a segment opens with: the header, COUNTERS, SPANS - within
	   the bound the recorder's encode buffer is checked against. */
	n = xbox_ft_encode_segment_open( buf, 1024, 733000000u, 3375000u, 77, 42, 2, "build y", XBOX_FT_W_COUNT, &seg );
	CHECK( n == 256 + 8 + 4 + XBOX_FT_W_COUNT * 16 + 8 + 4 + XBOX_FT_S_COUNT * 32 && n <= XBOX_FT_SEGMENT_OPEN_BYTES );
	CHECK( rd32( buf + 256 ) == XBOX_FT_CHUNK_COUNTERS && rd32( buf + 256 + 12 + XBOX_FT_W_COUNT * 16 ) == XBOX_FT_CHUNK_SPANS );

	/* END says what follows; the plain encoder is "the session ended". */
	CHECK( xbox_ft_encode_segment_end( buf, 9, 3, XBOX_FT_SEG_MAP ) == 24 && rd32( buf + 16 ) == 3 && rd32( buf + 20 ) == XBOX_FT_SEG_MAP );
	CHECK( xbox_ft_encode_end( buf, 9, 3 ) == 24 && rd32( buf + 20 ) == XBOX_FT_SEG_SESSION );
}

/* A recording the Python parser test knows the contents of: 1 MHz TSC
   (a tick is a microsecond), map "c1a0" loaded in 2.5 s with 6000 free
   pages before and 5500 after, then 40 frames of 20 ms (16 CPU, 3 GPU),
   the 36th a 50 ms frame, frame seq 20 lost (a gap), two CALIB pairs, a
   second map "c1a0e" with 10 frames of 10 ms, and a clean END.
   `counters` 0 writes it as version 1. With counters (version 2) every
   frame seq i carries work[j] = 100 * ( j + 1 ) + i, frame 35 (the
   50 ms one) has 10 times the draws, and frame 33 is a flush frame (a
   mode-1 file write), with the same times. */
static int write_fixture( const char *path, uint32_t counters )
{
	uint8_t buf[TEST_BUF_BYTES];
	xbox_ft_frame_t f;
	uint32_t i, j;
	FILE *fp = fopen( path, "wb" );

	if( !fp )
		return 1;

	fwrite( buf, 1, xbox_ft_encode_header( buf, 1024, 1000000u, 1000000u, 0, 0, 1, "fixture", counters ), fp );
	if( counters )
		fwrite( buf, 1, xbox_ft_encode_counters( buf, counters ), fp );
	fwrite( buf, 1, xbox_ft_encode_map( buf, 1000000u, 2500000u, 6000, 5500, 16384, 0, "c1a0" ), fp );

	/* Two FRAMES chunks: seq 0..19 and 21..39 (20 lost). */
	fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 0, 20, counters ), fp );
	for( i = 0; i < 40; i++ )
	{
		if( i == 20 )
		{
			fwrite( buf, 1, xbox_ft_encode_calib( buf, 1400000u, 1400000u ), fp );
			fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 21, 19, counters ), fp );
			continue;
		}
		memset( &f, 0, sizeof( f ));
		f.seq = i;
		f.total = ( i == 35 ) ? 50000 : 20000;
		f.host = f.total - 1000;
		f.gpu = 3000;
		f.vblank = 0;
		f.swap = 0;
		f.flags = XBOX_FT_F_ACTIVE;
		if( counters )
		{
			for( j = 0; j < XBOX_FT_W_COUNT; j++ )
				f.work[j] = 100u * ( j + 1 ) + i;
			if( i == 35 )
				f.work[XBOX_FT_W_DRAWS] *= 10;
			if( i == 33 )
				f.flags |= XBOX_FT_F_FLUSH;
		}
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, counters ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_calib( buf, 2000000u, 2000000u ), fp );
	fwrite( buf, 1, xbox_ft_encode_map( buf, 5000000u, 1000000u, 5500, 5400, 16384, 0, "c1a0e" ), fp );
	fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 40, 10, counters ), fp );
	for( i = 40; i < 50; i++ )
	{
		memset( &f, 0, sizeof( f ));
		f.seq = i;
		f.total = 10000;
		f.host = 8000;
		f.gpu = 500;
		f.flags = XBOX_FT_F_ACTIVE | ( i == 45 ? XBOX_FT_F_MENU : 0 );
		for( j = 0; j < XBOX_FT_W_COUNT; j++ )
			f.work[j] = 100u * ( j + 1 ) + i;
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, counters ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_end( buf, 6000000u, 1 ), fp );
	return fclose( fp ) != 0;
}

/* The level-change session (tools/test_frametime_report.py, class
   LevelChange): 1 MHz TSC. Map "c1a0", seq 0..39 in play at 20 ms (host
   19 ms, GPU 3 ms), except
     seq 35  a 600 ms stall inside Host_Frame (host 599 ms): an autosave;
             when `flagged`, its spans (ms) are k_cl_begin 560 > k_save
             550 > k_save_sav 400, k_save_ents 100, k_save_init 20 - own
             times save_sav 400, save_ents 100, save 30, save_init 20,
             cl_begin 10; and first-use loads: snd_loads 2, snd_load_kb
             300, snd_load_kc 250, tex_loads 6, tex_load_kb 4096,
             cl_parse_kc 50 - and,
             at the end of the file, a NOTES chunk naming two events
             in it: sound fvox/flatline.wav 120 ms 131072 bytes, message
             svc_sound 200 ms 40 bytes; one more note dropped;
     seq 36, 37  when `flagged`, the deferred .sav write (2026-10-06,
             #153): k_save_pump 8 and 6 ms, save_pump_kb 64 and 40;
     seq 38  the changelevel frame: 500 ms, host 10 ms - 490 ms between
             host frames, the load; flagged XBOX_FT_F_LOAD when `flagged`,
             and then also the rest of that save flushed: save_pump_kb
             20, save_flush_kc 30;
     seq 39  the last frame still starting in play: 100 ms, host 10 ms -
             90 ms between frames, under the report's 100 ms shape rule.
   Then seq 40..44 loading (no flags), 100 ms each; map "c1a0e" with seq
   45..84 in play at 10 ms (host 8 ms), except seq 83, 300 ms with host
   10 ms: a long frame between host frames that no load follows - a stall,
   not a level change. Clean END. `flagged` writes version 2 with
   counters; otherwise version 1. */
static int write_fixture_level_change( const char *path, int flagged )
{
	uint8_t buf[TEST_BUF_BYTES];
	xbox_ft_frame_t f;
	uint32_t i, counters = flagged ? XBOX_FT_W_COUNT : 0;
	FILE *fp = fopen( path, "wb" );

	if( !fp )
		return 1;

	fwrite( buf, 1, xbox_ft_encode_header( buf, 1024, 1000000u, 1000000u, 0, 0, 1, "levelchange", counters ), fp );
	if( counters )
	{
		fwrite( buf, 1, xbox_ft_encode_counters( buf, counters ), fp );
		fwrite( buf, 1, xbox_ft_encode_spans( buf, counters ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_map( buf, 1000000u, 2000000u, 6000, 5500, 16384, 0, "c1a0" ), fp );
	fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 0, 45, counters ), fp );
	for( i = 0; i < 45; i++ )
	{
		memset( &f, 0, sizeof( f ));
		f.seq = i;
		f.total = 20000;
		f.host = 19000;
		f.gpu = 3000;
		f.flags = XBOX_FT_F_ACTIVE;
		if( i == 35 )
		{
			f.total = 600000;
			f.host = 599000;
			if( flagged )
			{
				f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_CL_BEGIN] = 560;
				f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE] = 550;
				f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_SAV] = 400;
				f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_ENTS] = 100;
				f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_INIT] = 20;
				/* first-use loads (2026-10-06) */
				f.work[XBOX_FT_W_SND_LOADS] = 2;
				f.work[XBOX_FT_W_SND_LOAD_KB] = 300;
				f.work[XBOX_FT_W_SND_LOAD_KC] = 250;
				f.work[XBOX_FT_W_TEX_LOADS] = 6;
				f.work[XBOX_FT_W_TEX_LOAD_KB] = 4096;
				f.work[XBOX_FT_W_CL_PARSE_KC] = 50;
			}
		}
		else if(( i == 36 || i == 37 ) && flagged )
		{
			f.work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SAVE_PUMP] = i == 36 ? 8 : 6;
			f.work[XBOX_FT_W_SAVE_PUMP_KB] = i == 36 ? 64 : 40;
		}
		else if( i == 38 )
		{
			f.total = 500000;
			f.host = 10000;
			if( flagged )
			{
				f.flags |= XBOX_FT_F_LOAD;
				f.work[XBOX_FT_W_SAVE_PUMP_KB] = 20;
				f.work[XBOX_FT_W_SAVE_FLUSH_KC] = 30;
			}
		}
		else if( i == 39 )
		{
			f.total = 100000;
			f.host = 10000;
		}
		else if( i >= 40 )
		{
			f.total = 100000;
			f.host = 90000;
			f.gpu = 0;
			f.flags = 0;
		}
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, counters ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_map( buf, 3000000u, 1500000u, 5500, 5400, 16384, 0, "c1a0e" ), fp );
	fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 45, 40, counters ), fp );
	for( i = 45; i < 85; i++ )
	{
		memset( &f, 0, sizeof( f ));
		f.seq = i;
		f.total = ( i == 83 ) ? 300000 : 10000;
		f.host = ( i == 83 ) ? 10000 : 8000;
		f.gpu = 500;
		f.flags = XBOX_FT_F_ACTIVE;
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, counters ), fp );
	}
	if( flagged )
	{
		/* NOTES (2026-10-06): seq 35 loaded a sound in a slow svc_sound
		   (1 MHz: kilocycles are ms); one more note was dropped. */
		xbox_ft_note_t n[2];

		memset( n, 0, sizeof( n ));
		n[0].seq = 35;
		n[0].kind = XBOX_FT_NOTE_SOUND;
		n[0].kcyc = 120;
		n[0].bytes = 131072;
		strcpy( n[0].name, "fvox/flatline.wav" );
		n[1].seq = 35;
		n[1].kind = XBOX_FT_NOTE_MSG;
		n[1].kcyc = 200;
		n[1].bytes = 40;
		strcpy( n[1].name, "svc_sound" );
		fwrite( buf, 1, xbox_ft_encode_notes( buf, n, 2, 1 ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_end( buf, 9000000u, 0 ), fp );
	return fclose( fp ) != 0;
}

/* The spans session (tools/test_frametime_report.py, class Spans): 1 MHz
   TSC, so a kilocycle is a millisecond. The recorder's own header,
   COUNTERS and SPANS chunks; map "c1a0c", seq 0..49 in play, total 20 ms,
   host 19 ms, GPU 3 ms (CPU 16 ms), span_calls 300, and spans (ms):
     input 1, cl_begin 1,
     sv 6 = sv_read 1 + sv_game 4 + sv_send 1,
     cl 7 = cl_read 1 + scr 5 + snd 1,
       scr 5 = r_scene 4 + r_end 1,
         r_scene 4 = world 1 + client_studio 2 + 1 own,
           client_studio 2 = studio_points 1 + 1 own
   - top level 15, so 1 ms of CPU no span saw. Two frames differ:
     seq 40  sv_game 10 (sv 12), host 25 ms, total 26 ms: CPU 22, still 1 unattributed;
     seq 41  client_studio 5 with studio_points 3 (r_scene 7, scr 8, cl 10),
             host 22 ms, total 23 ms: CPU 19, still 1 unattributed.
   sv_game's breakdown (2026-10-05): sv_game 4 = sv_start 1 + sv_pstep 2
   + 1 own (seq 40: sv_pstep 8, still 1 + 1 around it); every frame
   g_thinks 30, g_moves 200, g_worst_kc 1 (seq 40: 6), g_worst_cls 2
   (row 1, monster_scientist), g_worst_ent 57; and a CLASSES chunk at the
   end of the map: func_tracktrain (push, 50 runs, 5 thinks, 10 000 ticks,
   think 1 000, worst 500), monster_scientist (step, 100 runs, 20 thinks,
   400 moves, 106 000 ticks, think 90 000, worst 8 000) and info_target
   (none, nothing run: left out of the report).
   Player movement (2026-10-06), every frame: pm_sv_cmds 1, pm_cl_cmds 1,
   pm_cl_redo 0, pm_cl_solids 2, physents 5 server / 40 client, visents
   6 / 50, traces 8 / 9, hull checks 40 / 360, position tests 1 / 1,
   their point tests 5 / 40, point contents 3 / 3, texture traces 1 / 0,
   physents skipped 2 / 300;
   times (ms) server setup 1, PM_Move 2 (queries 1), think 1, touch 0,
   client solid list 1, PM_Move 3 (queries 2), PostRunCmd 1.
   Client entities (2026-10-07), every frame: k_cl_pkt_ents 1 (all of
   cl_read; k_cl_emit 0), ce_ents 120, ce_deltas 30,
   ce_nochange 2, ce_fields 150, ce_visents 40; times (ms) deltas 2 (field
   loop 1), CL_ProcessPacket 1, link 3 (CL_AddVisibleEntity 2), temp
   entities 1, events 0, ideal pitch 1; physent-list skips 2 (of
   pm_cl_solids 2).
   Clean END. */
static int write_fixture_spans( const char *path )
{
	uint8_t buf[TEST_BUF_BYTES];
	xbox_ft_frame_t f;
	uint32_t i;
	FILE *fp = fopen( path, "wb" );

	if( !fp )
		return 1;

	fwrite( buf, 1, xbox_ft_encode_header( buf, 1024, 1000000u, 1000000u, 0, 0, 1, "spans", XBOX_FT_W_COUNT ), fp );
	fwrite( buf, 1, xbox_ft_encode_counters( buf, XBOX_FT_W_COUNT ), fp );
	fwrite( buf, 1, xbox_ft_encode_spans( buf, XBOX_FT_W_COUNT ), fp );
	fwrite( buf, 1, xbox_ft_encode_map( buf, 1000000u, 2000000u, 6000, 5500, 16384, 0, "c1a0c" ), fp );
	fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, 0, 50, XBOX_FT_W_COUNT ), fp );
	for( i = 0; i < 50; i++ )
	{
		uint32_t *k;

		memset( &f, 0, sizeof( f ));
		f.seq = i;
		f.total = 20000;
		f.host = 19000;
		f.gpu = 3000;
		f.flags = XBOX_FT_F_ACTIVE;
		f.work[XBOX_FT_W_STUDIO_MODELS] = 10;
		f.work[XBOX_FT_W_SPAN_CALLS] = 300;
		f.work[XBOX_FT_W_WORLD_WALKED] = 500; /* a count after the spans */
		k = &f.work[XBOX_FT_W_SPAN_FIRST];
		k[XBOX_FT_S_INPUT] = 1;
		k[XBOX_FT_S_CL_BEGIN] = 1;
		k[XBOX_FT_S_SV] = 6;
		k[XBOX_FT_S_SV_READ] = 1;
		k[XBOX_FT_S_SV_GAME] = 4;
		k[XBOX_FT_S_SV_SEND] = 1;
		k[XBOX_FT_S_CL] = 7;
		k[XBOX_FT_S_CL_READ] = 1;
		k[XBOX_FT_S_SCR] = 5;
		k[XBOX_FT_S_R_SCENE] = 4;
		k[XBOX_FT_S_WORLD] = 1;
		k[XBOX_FT_S_CLIENT_STUDIO] = 2;
		k[XBOX_FT_S_STUDIO_POINTS] = 1;
		k[XBOX_FT_S_R_END] = 1;
		k[XBOX_FT_S_SND] = 1;
		k[XBOX_FT_S_SV_START] = 1;
		k[XBOX_FT_S_SV_PSTEP] = 2;
		f.work[XBOX_FT_W_G_THINKS] = 30;
		f.work[XBOX_FT_W_G_MOVES] = 200;
		f.work[XBOX_FT_W_G_WORST_KC] = 1;
		f.work[XBOX_FT_W_G_WORST_CLS] = 2;
		f.work[XBOX_FT_W_G_WORST_ENT] = 57;
		f.work[XBOX_FT_W_PM_SV_CMDS] = 1;
		f.work[XBOX_FT_W_PM_CL_CMDS] = 1;
		f.work[XBOX_FT_W_PM_CL_SOLIDS] = 2;
		f.work[XBOX_FT_W_PM_SV_PHYSENTS] = 5;
		f.work[XBOX_FT_W_PM_CL_PHYSENTS] = 40;
		f.work[XBOX_FT_W_PM_SV_VISENTS] = 6;
		f.work[XBOX_FT_W_PM_CL_VISENTS] = 50;
		f.work[XBOX_FT_W_PM_SV_TRACES] = 8;
		f.work[XBOX_FT_W_PM_CL_TRACES] = 9;
		f.work[XBOX_FT_W_PM_SV_HULLS] = 40;
		f.work[XBOX_FT_W_PM_CL_HULLS] = 360;
		f.work[XBOX_FT_W_PM_SV_TESTS] = 1;
		f.work[XBOX_FT_W_PM_CL_TESTS] = 1;
		f.work[XBOX_FT_W_PM_SV_PTESTS] = 5;
		f.work[XBOX_FT_W_PM_CL_PTESTS] = 40;
		f.work[XBOX_FT_W_PM_SV_PCONT] = 3;
		f.work[XBOX_FT_W_PM_CL_PCONT] = 3;
		f.work[XBOX_FT_W_PM_SV_TEX] = 1;
		f.work[XBOX_FT_W_PM_SV_SETUP_KC] = 1;
		f.work[XBOX_FT_W_PM_SV_MOVE_KC] = 2;
		f.work[XBOX_FT_W_PM_SV_QUERY_KC] = 1;
		f.work[XBOX_FT_W_PM_SV_THINK_KC] = 1;
		f.work[XBOX_FT_W_PM_CL_SOLID_KC] = 1;
		f.work[XBOX_FT_W_PM_CL_MOVE_KC] = 3;
		f.work[XBOX_FT_W_PM_CL_QUERY_KC] = 2;
		f.work[XBOX_FT_W_PM_CL_POST_KC] = 1;
		f.work[XBOX_FT_W_PM_SV_CULLS] = 2;
		f.work[XBOX_FT_W_PM_CL_CULLS] = 300;
		k[XBOX_FT_S_CL_PKT_ENTS] = 1; /* all of cl_read */
		f.work[XBOX_FT_W_CE_ENTS] = 120;
		f.work[XBOX_FT_W_CE_DELTAS] = 30;
		f.work[XBOX_FT_W_CE_NOCHANGE] = 2;
		f.work[XBOX_FT_W_CE_FIELDS] = 150;
		f.work[XBOX_FT_W_CE_VISENTS] = 40;
		f.work[XBOX_FT_W_CE_DELTA_KC] = 2;
		f.work[XBOX_FT_W_CE_FIELDS_KC] = 1;
		f.work[XBOX_FT_W_CE_PROCESS_KC] = 1;
		f.work[XBOX_FT_W_CE_LINK_KC] = 3;
		f.work[XBOX_FT_W_CE_ADD_KC] = 2;
		f.work[XBOX_FT_W_CE_TENTS_KC] = 1;
		f.work[XBOX_FT_W_CE_PITCH_KC] = 1;
		f.work[XBOX_FT_W_CE_SOLID_SKIPS] = 2;
		if( i == 40 )
		{
			k[XBOX_FT_S_SV_GAME] = 10;
			k[XBOX_FT_S_SV] = 12;
			k[XBOX_FT_S_SV_PSTEP] = 8;
			f.work[XBOX_FT_W_G_WORST_KC] = 6;
			f.host = 25000;
			f.total = 26000;
		}
		else if( i == 41 )
		{
			k[XBOX_FT_S_CLIENT_STUDIO] = 5;
			k[XBOX_FT_S_STUDIO_POINTS] = 3;
			k[XBOX_FT_S_R_SCENE] = 7;
			k[XBOX_FT_S_SCR] = 8;
			k[XBOX_FT_S_CL] = 10;
			f.host = 22000;
			f.total = 23000;
		}
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, XBOX_FT_W_COUNT ), fp );
	}
	{
		xbox_ft_class_t c[3];

		memset( c, 0, sizeof( c ));
		strcpy( c[0].name, "func_tracktrain" );
		c[0].movetype = 7; c[0].runs = 50; c[0].thinks = 5; c[0].phys = 10000; c[0].think = 1000; c[0].worst = 500;
		strcpy( c[1].name, "monster_scientist" );
		c[1].movetype = 4; c[1].runs = 100; c[1].thinks = 20; c[1].moves = 400;
		c[1].phys = 106000; c[1].think = 90000; c[1].worst = 8000;
		strcpy( c[2].name, "info_target" );
		fwrite( buf, 1, xbox_ft_encode_classes( buf, c, 3 ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_end( buf, 9000000u, 0 ), fp );
	return fclose( fp ) != 0;
}

/* The session fixtures (v3, 2026-10-07; tools/test_frametime_report.py,
   class Sessions). One recording, 1 MHz TSC, written twice: as the
   recorder's segments and as one file as the single-file recorder wrote
   it, so the report's stitched session can be compared with the single
   file field by field. Frames (`slow` adds 5 ms to every 20 ms frame):
     seq 0..9     before the first map: not active, 10 ms (host 9)
     seq 10..56   c1a0 in play, 20 ms (host 18, GPU 2); seq 33 a flush
                  frame; seq 50 a 400 ms stall inside Host_Frame (host
                  399) with a note (sound "fvox/flatline.wav" 120 ms)
     seq 57       the changelevel frame: LOAD, 500 ms, host 10
     seq 58       the last frame starting in play: 100 ms, host 10
     seq 59..63   loading: not active, 100 ms (host 90)
     seq 64..139  c1a0e in play, 20 ms; seq 80 lost (a gap, counted in
                  the END); seq 120 a 300 ms frame with host 10 (between
                  frames: a stall, no load after it) and a note (message
                  "svc_sound" 30 ms)
   Every frame carries the counters: draws = 100 + seq, k_sv_game = 4 ms
   (seq 50: 300), studio_models = 10, g_worst_cls = 1. Classes: c1a0
   monster_scientist 100 runs, 2000 phys kc; c1a0e func_door 40 runs
   (60 in the second part), 500 phys kc (300), worst 7 (9).
   Segments: 000 the menu frames 0..9 (END: a map follows), 001 MAP c1a0
   and 10..63, CLASSES (END: a map follows), 002 MAP c1a0e and 64..99,
   CLASSES of that part (END: the size cap), 003 MAP c1a0e CONTINUED,
   100..139, CLASSES (END: the session stopped); each note is in the
   segment that holds its frame, before the frames. A CALIB
   pair closes each segment's frames, as a flush writes it. The single
   file: a v2 header, COUNTERS, SPANS, then the same chunks without the
   headers, the ENDs but the last and the CONTINUED map, and with c1a0e's
   two CLASSES parts as the one chunk the single-file recorder wrote. */
static uint64_t sess_frame( uint32_t i, int slow, xbox_ft_frame_t *f )
{
	memset( f, 0, sizeof( *f ));
	f->seq = i;
	if( i < 10 )
	{
		f->total = 10000; f->host = 9000;
	}
	else if( i < 57 || ( i >= 64 && i < 140 ))
	{
		f->flags = XBOX_FT_F_ACTIVE;
		f->total = 20000 + ( slow ? 5000 : 0 );
		f->host = f->total - 2000;
		f->gpu = 2000;
		if( i == 33 )
			f->flags |= XBOX_FT_F_FLUSH;
		if( i == 50 )
		{
			f->total = 400000; f->host = 399000;
		}
		if( i == 120 )
		{
			f->total = 300000; f->host = 10000;
		}
	}
	else if( i == 57 || i == 58 )
	{
		f->flags = XBOX_FT_F_ACTIVE | ( i == 57 ? XBOX_FT_F_LOAD : 0 );
		f->total = i == 57 ? 500000 : 100000;
		f->host = 10000;
	}
	else
	{
		f->total = 100000; f->host = 90000;
	}
	f->work[XBOX_FT_W_DRAWS] = 100 + i;
	f->work[XBOX_FT_W_STUDIO_MODELS] = 10;
	f->work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV_GAME] = i == 50 ? 300 : 4;
	f->work[XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_SV] = i == 50 ? 300 : 4;
	f->work[XBOX_FT_W_G_WORST_CLS] = 1;
	return f->total;
}

/* Frames [a, b) as a FRAMES chunk, then a CALIB pair at the time after
   them. `*tsc` is the session's clock. Seq 80 is lost the way the ring
   loses one: the flush point moves past it, so the frames around it are
   two chunks and the gap is between them. */
static void sess_frames( FILE *fp, uint32_t a, uint32_t b, int slow, uint64_t *tsc )
{
	uint8_t buf[XBOX_FT_CHUNK_BYTES + 8u + 700u];
	xbox_ft_frame_t f;
	uint32_t i;

	for( i = a; i < b; i++ )
	{
		uint64_t t;

		if( i == a || i == 81 )
		{
			uint32_t end = ( i < 80 && 80 < b ) ? 80 : b;

			fwrite( buf, 1, xbox_ft_encode_frames_chunk_head( buf, i, end - i, XBOX_FT_W_COUNT ), fp );
		}
		t = sess_frame( i, slow, &f );
		*tsc += t;
		if( i == 80 )
			continue;
		fwrite( buf, 1, xbox_ft_encode_frame( buf, &f, XBOX_FT_W_COUNT ), fp );
	}
	fwrite( buf, 1, xbox_ft_encode_calib( buf, *tsc, *tsc ), fp );
}

static void sess_classes( FILE *fp, const char *name, uint32_t movetype, uint32_t runs, uint64_t phys, uint64_t worst )
{
	uint8_t buf[256];
	xbox_ft_class_t c[2];

	memset( c, 0, sizeof( c ));
	strcpy( c[0].name, "monster_scientist" );
	c[0].movetype = 4;
	strcpy( c[1].name, "func_door" );
	c[1].movetype = 7;
	c[strcmp( name, "func_door" ) == 0].runs = runs;
	c[strcmp( name, "func_door" ) == 0].phys = phys * 1000u;
	c[strcmp( name, "func_door" ) == 0].worst = worst * 1000u;
	fwrite( buf, 1, xbox_ft_encode_classes( buf, c, 2 ), fp );
}

static void sess_note( FILE *fp, uint32_t seq, uint32_t kind, uint32_t kc, const char *name )
{
	uint8_t buf[256];
	xbox_ft_note_t n;

	memset( &n, 0, sizeof( n ));
	n.seq = seq;
	n.kind = kind;
	n.kcyc = kc;
	n.bytes = 1000;
	strcpy( n.name, name );
	fwrite( buf, 1, xbox_ft_encode_notes( buf, &n, 1, 0 ), fp );
}

/* Writes the session's segments into directory `dir` (their names are
   the recorder's), or with `single` the one file at `dir`. */
static int write_session( const char *dir, const char *label, uint64_t id_input, int slow, int single )
{
	static uint8_t buf[XBOX_FT_SEGMENT_OPEN_BYTES];
	xbox_ft_segment_t seg;
	uint64_t tsc = 0, in[1] = { id_input };
	char name[XBOX_FT_FILE_NAME_MAX + 1], path[512];
	FILE *fp = NULL;
	uint32_t s;
	int bad = 0;

	memset( &seg, 0, sizeof( seg ));
	xbox_ft_session_id( in, 1, seg.session );
	seg.systime = 133000000000000000ull + id_input; /* 2022-06-22 */
	xbox_ft_label_sanitise( label, seg.label );

	if( single )
	{
		if( !( fp = fopen( dir, "wb" )))
			return 1;
		fwrite( buf, 1, xbox_ft_encode_header( buf, 1024, 1000000u, 1000000u, 0, 0, 1, "session", XBOX_FT_W_COUNT ), fp );
		fwrite( buf, 1, xbox_ft_encode_counters( buf, XBOX_FT_W_COUNT ), fp );
		fwrite( buf, 1, xbox_ft_encode_spans( buf, XBOX_FT_W_COUNT ), fp );
	}

	for( s = 0; s < 4; s++ )
	{
		static const uint32_t first[5] = { 0, 10, 64, 100, 140 };
		static const uint32_t reason[4] = { XBOX_FT_SEG_SESSION, XBOX_FT_SEG_MAP, XBOX_FT_SEG_MAP, XBOX_FT_SEG_SIZE };

		if( !single )
		{
			seg.seq = s;
			seg.reason = reason[s];
			seg.first_frame = first[s];
			seg.tsc_open = tsc;
			xbox_ft_segment_name( name, seg.label, seg.session, s );
			snprintf( path, sizeof( path ), "%s/%s", dir, name );
			if( !( fp = fopen( path, "wb" )))
				return 1;
			fwrite( buf, 1, xbox_ft_encode_segment_open( buf, 1024, 1000000u, 1000000u, 0, 0, 1, "session", XBOX_FT_W_COUNT, &seg ), fp );
		}
		if( s == 1 )
			fwrite( buf, 1, xbox_ft_encode_map( buf, tsc, 2000000u, 6000, 5500, 16384, 0, "c1a0" ), fp );
		else if( s == 2 )
			fwrite( buf, 1, xbox_ft_encode_map( buf, tsc, 1500000u, 5500, 5400, 16384, 0, "c1a0e" ), fp );
		else if( s == 3 && !single )
			fwrite( buf, 1, xbox_ft_encode_map( buf, tsc, 0, 0, 5300, 16384,
				XBOX_FT_MAP_F_NO_LOAD_START | XBOX_FT_MAP_F_CONTINUED, "c1a0e" ), fp );
		/* The notes come first in a flush, in the segment their frame is
		   flushed to; each names its frame. */
		if( s == 1 )
			sess_note( fp, 50, XBOX_FT_NOTE_SOUND, 120, "fvox/flatline.wav" );
		else if( s == 3 )
			sess_note( fp, 120, XBOX_FT_NOTE_MSG, 30, "svc_sound" );
		sess_frames( fp, first[s], first[s + 1], slow, &tsc );
		if( s == 1 )
			sess_classes( fp, "monster_scientist", 4, 100, 2000, 50 );
		else if( s == 2 && !single )
			sess_classes( fp, "func_door", 7, 40, 500, 7 );
		else if( s == 3 )
		{
			if( single ) /* the single-file recorder's one chunk for c1a0e */
				sess_classes( fp, "func_door", 7, 100, 800, 9 );
			else
				sess_classes( fp, "func_door", 7, 60, 300, 9 );
		}
		if( !single || s == 3 )
		{
			/* Seq 80 is lost in segment 002; the single file counts it at
			   its one END. */
			fwrite( buf, 1, xbox_ft_encode_segment_end( buf, tsc, s == 2 || ( single && s == 3 ) ? 1 : 0,
				single || s == 3 ? XBOX_FT_SEG_SESSION : reason[s + 1] ), fp );
		}
		if( !single )
		{
			bad |= fclose( fp ) != 0;
			fp = NULL;
		}
	}
	if( single )
		bad |= fclose( fp ) != 0;
	return bad;
}

int main( int argc, char **argv )
{
	test_ring();
	test_units();
	test_stats();
	test_encoders();
	test_spans();
	test_classes();
	test_notes();
	test_segments();

	/* argv[1]: the version-2 fixture, as a recorder from before the spans
	   wrote it (the 20 counts, no SPANS chunk); argv[2]: the version-1
	   one. */
	if( argc > 1 && write_fixture( argv[1], XBOX_FT_W_SPAN_CALLS ))
	{
		fprintf( stderr, "could not write %s\n", argv[1] );
		failures++;
	}
	if( argc > 2 && write_fixture( argv[2], 0 ))
	{
		fprintf( stderr, "could not write %s\n", argv[2] );
		failures++;
	}
	/* argv[3]: the level-change session with LOAD flags (v2); argv[4]:
	   the same without them (v1, an older recorder). */
	if( argc > 3 && write_fixture_level_change( argv[3], 1 ))
	{
		fprintf( stderr, "could not write %s\n", argv[3] );
		failures++;
	}
	if( argc > 4 && write_fixture_level_change( argv[4], 0 ))
	{
		fprintf( stderr, "could not write %s\n", argv[4] );
		failures++;
	}
	/* argv[5]: the spans session. */
	if( argc > 5 && write_fixture_spans( argv[5] ))
	{
		fprintf( stderr, "could not write %s\n", argv[5] );
		failures++;
	}
	/* argv[6]: a directory for the session fixtures' segments - the
	   "pr190-clents" session and a slower "pr189-base" one to compare
	   it with; argv[7]: the first session as one single-file recording. */
	if( argc > 6 && ( write_session( argv[6], "pr190-clents", 190, 0, 0 ) || write_session( argv[6], "pr189 base", 189, 1, 0 )))
	{
		fprintf( stderr, "could not write the sessions into %s\n", argv[6] );
		failures++;
	}
	if( argc > 7 && write_session( argv[7], "pr190-clents", 190, 0, 1 ))
	{
		fprintf( stderr, "could not write %s\n", argv[7] );
		failures++;
	}

	if( failures )
	{
		fprintf( stderr, "xbox_frametime_util: %d check(s) failed\n", failures );
		return 1;
	}
	printf( "xbox_frametime_util: all checks passed\n" );
	return 0;
}
