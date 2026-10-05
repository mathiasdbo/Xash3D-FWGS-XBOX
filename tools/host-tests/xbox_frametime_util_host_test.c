/* Host test for engine/platform/xbox/xbox_frametime_util.c - the frame
   ring, the statistics (1% low, guideline counts) and the file encoders.
   With path arguments it also writes small recordings through the same
   encoders - a version-2 file with work counters and a version-1 file
   without, and a level-change session both with the recorder's LOAD flag
   (v2) and without it (v1, as recorders before 2026-10-01 wrote it) -
   which tools/test_frametime_report.py then parses: the parser's fixtures
   come from the encoder the recorder runs, not from a hand-written copy
   of the format. The spans fixture (2026-10-01) is a current recording:
   every counter, the SPANS chunk, and known span times. */

#include <stdio.h>
#include <string.h>
#include "xbox_frametime_util.h"

static int failures;

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
	uint8_t buf[2048];
	xbox_ft_frame_t f;
	uint32_t i;

	/* Version 2: 328-byte records (32 + 20 counts, span_calls, 50
	   spans - 24 from 2026-10-01, 11 round-trip, 7 mixer and 8 scene
	   ones from 2026-10-02 - and world_walked, water_gpu and vp_loads
	   after them); 112 bytes for a file from before the spans (20). */
	CHECK( XBOX_FT_W_SPAN_CALLS == 20 && XBOX_FT_W_SPAN_FIRST == 21 && XBOX_FT_S_COUNT == 50 );
	CHECK( XBOX_FT_W_WORLD_WALKED == 71 && XBOX_FT_W_WATER_GPU == 72 && XBOX_FT_W_VP_LOADS == 73 && XBOX_FT_W_COUNT == 74 );
	CHECK( xbox_ft_frame_bytes( XBOX_FT_W_COUNT ) == 328 && xbox_ft_frame_bytes( 0 ) == 32 );
	CHECK( xbox_ft_frame_bytes( XBOX_FT_W_SPAN_CALLS ) == 112 );
	CHECK( xbox_ft_encode_header( buf, 1024, 733000000u, 3375000u, 0x1122334455667788ull, 42, 2, "build x", XBOX_FT_W_COUNT ) == XBOX_FT_HEADER_BYTES );
	CHECK( rd32( buf + 8 ) == 2 && rd32( buf + 16 ) == 328 );
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
	CHECK( xbox_ft_encode_frame( buf, &f, XBOX_FT_W_COUNT ) == 328 );
	CHECK( rd32( buf ) == 7 && buf[28] == 0x01 && buf[29] == 0x80 );
	CHECK( rd32( buf + 32 ) == 0x01000000u && rd32( buf + 32 + 4 * XBOX_FT_W_LM_TEXELS ) == 0x01000000u + XBOX_FT_W_LM_TEXELS );
	CHECK( rd32( buf + 324 ) == 0x01000000u + XBOX_FT_W_COUNT - 1 );

	CHECK( xbox_ft_encode_frames_chunk_head( buf, 10, 3, 0 ) == 16 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_FRAMES && rd32( buf + 4 ) == 8 + 3 * 32 && rd32( buf + 8 ) == 10 && rd32( buf + 12 ) == 3 );
	CHECK( xbox_ft_encode_frames_chunk_head( buf, 10, 3, XBOX_FT_W_COUNT ) == 16 );
	CHECK( rd32( buf + 4 ) == 8 + 3 * 328 );

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
	CHECK( strcmp( (const char *)buf + 12 + 16 * ( XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_COUNT - 1 ), "k_warp_trans" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_WORLD_WALKED, "world_walked" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_WATER_GPU, "water_gpu" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 16 * XBOX_FT_W_VP_LOADS, "vp_loads" ) == 0 );
	CHECK( XBOX_FT_W_VP_LOADS == XBOX_FT_W_COUNT - 1 );
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
	uint8_t buf[2048];
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
	CHECK( XBOX_FT_SPANS_CHUNK_BYTES == 8 + 4 + 50 * 32 );
	CHECK( rd32( buf ) == XBOX_FT_CHUNK_SPANS && rd32( buf + 4 ) == 4 + 50 * 32 && rd32( buf + 8 ) == 50 );
	/* The last pair is the last span, not world_walked after it. */
	CHECK( strcmp( (const char *)buf + 12 + 32 * 49, "k_warp_trans" ) == 0 && strcmp( (const char *)buf + 12 + 32 * 49 + 16, "k_r_scene" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12, "k_input" ) == 0 && buf[12 + 16] == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * XBOX_FT_S_STUDIO_POINTS, "k_studio_points" ) == 0 );
	CHECK( strcmp( (const char *)buf + 12 + 32 * XBOX_FT_S_STUDIO_POINTS + 16, "k_client_studio" ) == 0 );
	/* A file whose records stop before the spans has no SPANS chunk; one
	   cut inside them names only the ones it carries. */
	CHECK( xbox_ft_encode_spans( buf, XBOX_FT_W_SPAN_FIRST ) == 0 && xbox_ft_encode_spans( buf, 0 ) == 0 );
	CHECK( xbox_ft_encode_spans( buf, XBOX_FT_W_SPAN_FIRST + 3 ) == 8 + 4 + 3 * 32 && rd32( buf + 8 ) == 3 );
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
	uint8_t buf[2048];
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
     seq 38  the changelevel frame: 500 ms, host 10 ms - 490 ms between
             host frames, the load; flagged XBOX_FT_F_LOAD when `flagged`;
     seq 39  the last frame still starting in play: 100 ms, host 10 ms -
             90 ms between frames, under the report's 100 ms shape rule.
   Then seq 40..44 loading (no flags), 100 ms each; map "c1a0e" with seq
   45..84 in play at 10 ms (host 8 ms), except seq 83, 300 ms with host
   10 ms: a long frame between host frames that no load follows - a stall,
   not a level change. Clean END. `flagged` writes version 2 with
   counters; otherwise version 1. */
static int write_fixture_level_change( const char *path, int flagged )
{
	uint8_t buf[2048];
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
		}
		else if( i == 38 )
		{
			f.total = 500000;
			f.host = 10000;
			if( flagged )
				f.flags |= XBOX_FT_F_LOAD;
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
   Clean END. */
static int write_fixture_spans( const char *path )
{
	uint8_t buf[2048];
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
		if( i == 40 )
		{
			k[XBOX_FT_S_SV_GAME] = 10;
			k[XBOX_FT_S_SV] = 12;
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
	fwrite( buf, 1, xbox_ft_encode_end( buf, 9000000u, 0 ), fp );
	return fclose( fp ) != 0;
}

int main( int argc, char **argv )
{
	test_ring();
	test_units();
	test_stats();
	test_encoders();
	test_spans();

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

	if( failures )
	{
		fprintf( stderr, "xbox_frametime_util: %d check(s) failed\n", failures );
		return 1;
	}
	printf( "xbox_frametime_util: all checks passed\n" );
	return 0;
}
