/*
nv2a_skin_cache_host_test.c - host test for ref/nv2a/nv2a_skin_cache.c
(the studio skin cache's LRU/budget bookkeeping and its 16 bpp packers,
docs/r3d/design/x9-x-studio-models.md "Memory"). Like nv2a_arena.c
underneath it, the module is free of pbkit/xboxkrnl/engine includes, so
its rules are checked here rather than first exercised on a console that
is out of memory. Run it on the host from the repository root, in the
MSYS2 MinGW-64 shell:

  gcc -O1 -Wall -std=c99 -Iref/nv2a \
      tools/host-tests/nv2a_skin_cache_host_test.c ref/nv2a/nv2a_skin_cache.c ref/nv2a/nv2a_arena.c \
      -o /tmp/nv2a_skin_cache_test && /tmp/nv2a_skin_cache_test

Exit status is nonzero iff any check failed (the failure count is also
printed), matching tools/host-tests/nv2a_arena_host_test.c.
*/
#include "nv2a_skin_cache.h"
#include "nv2a_arena.h"
#include <stdio.h>
#include <string.h>

static int fails, total;

#define CHECK( cond, msg ) do { total++; if( !( cond )) { fails++; printf( "FAIL: %s\n", msg ); } } while( 0 )

#define G NV2A_ARENA_GRANULE

/* Records every eviction callback, in order. */
typedef struct
{
	uint32_t ids[64];
	uint32_t count;
} evict_log_t;

static void record_evict( void *ctx, uint32_t id )
{
	evict_log_t *log = (evict_log_t *)ctx;
	if( log->count < 64 )
		log->ids[log->count] = id;
	log->count++;
}

/* A fake chunk backing: records which chunks are backed, and can be told
   to refuse (the console out of contiguous memory). */
typedef struct
{
	int backed[16];
	int grows, releases;
	int refuse;
} backing_t;

static backing_t backing;

static int fake_grow( void *ctx, uint32_t index )
{
	backing_t *b = (backing_t *)ctx;
	if( b->refuse )
		return 0;
	b->backed[index] = 1;
	b->grows++;
	return 1;
}

static void fake_release( void *ctx, uint32_t index )
{
	backing_t *b = (backing_t *)ctx;
	b->backed[index] = 0;
	b->releases++;
}

static nv2a_skin_cache_t *make_chunked( size_t budget, size_t chunk, uint32_t uploads, size_t frame_bytes, uint32_t min_age )
{
	memset( &backing, 0, sizeof( backing ));
	return nv2a_skin_cache_init( budget, chunk, 16, uploads, frame_bytes, min_age, fake_grow, fake_release, &backing );
}

/* One chunk the size of the whole budget - the LRU tests' single span -
   and a generous per-frame allowance so they are not gated by it. */
static nv2a_skin_cache_t *make_cache( size_t capacity, uint32_t min_age )
{
	return make_chunked( capacity, capacity, 100, 1u << 30, min_age );
}

static void test_basic_acquire( void )
{
	nv2a_skin_cache_t *c = make_cache( 4 * G, 2 );
	size_t off = 99;

	CHECK( nv2a_skin_cache_acquire( c, 1, G, 10, &off, NULL, NULL ) == NV2A_SKIN_OK, "basic: first acquire succeeds" );
	CHECK( off == 0, "basic: first skin at offset 0" );
	CHECK( nv2a_skin_cache_is_resident( c, 1 ), "basic: id is resident" );
	CHECK( !nv2a_skin_cache_is_resident( c, 2 ), "basic: another id is not" );
	CHECK( nv2a_skin_cache_used( c ) == G, "basic: used bytes" );
	CHECK( nv2a_skin_cache_resident_count( c ) == 1, "basic: resident count" );
	CHECK( nv2a_skin_cache_acquire( c, 1, G, 10, &off, NULL, NULL ) == NV2A_SKIN_BAD_ID, "basic: acquiring a resident id is a caller bug" );
	CHECK( nv2a_skin_cache_acquire( c, 16, G, 10, &off, NULL, NULL ) == NV2A_SKIN_BAD_ID, "basic: id past max_ids rejected" );
	CHECK( nv2a_skin_cache_acquire( c, 3, 0, 10, &off, NULL, NULL ) == NV2A_SKIN_BAD_ID, "basic: zero bytes rejected" );
	CHECK( nv2a_skin_cache_acquire( c, 3, 5 * G, 10, &off, NULL, NULL ) == NV2A_SKIN_TOO_BIG, "basic: bigger than a chunk is TOO_BIG" );
	nv2a_skin_cache_destroy( c );
}

static void test_lru_eviction_order( void )
{
	nv2a_skin_cache_t *c = make_cache( 3 * G, 2 );
	evict_log_t log;
	size_t off;

	memset( &log, 0, sizeof( log ));
	nv2a_skin_cache_acquire( c, 1, G, 1, &off, record_evict, &log );
	nv2a_skin_cache_acquire( c, 2, G, 2, &off, record_evict, &log );
	nv2a_skin_cache_acquire( c, 3, G, 3, &off, record_evict, &log );
	nv2a_skin_cache_touch( c, 1, 4 ); /* 1 is now the most recent; 2 the least */

	CHECK( nv2a_skin_cache_acquire( c, 4, G, 10, &off, record_evict, &log ) == NV2A_SKIN_OK, "lru: full cache evicts to make room" );
	CHECK( log.count == 1 && log.ids[0] == 2, "lru: the least recently used (2) is the one evicted" );
	CHECK( !nv2a_skin_cache_is_resident( c, 2 ) && nv2a_skin_cache_is_resident( c, 1 ) && nv2a_skin_cache_is_resident( c, 3 ),
		"lru: only the victim left the cache" );
	CHECK( nv2a_skin_cache_eviction_count( c ) == 1, "lru: eviction counted" );

	/* Layout is now [1][4][3]. A two-granule skin at frame 20: evicting
	   3 then 1 frees two granules that are NOT adjacent (4 sits between
	   them), so fragmentation forces 4 out as well - oldest first each
	   time, and the allocation lands once the holes coalesce. */
	CHECK( nv2a_skin_cache_acquire( c, 5, 2 * G, 20, &off, record_evict, &log ) == NV2A_SKIN_OK, "lru: a two-granule skin fits after eviction" );
	CHECK( log.count == 4 && log.ids[1] == 3 && log.ids[2] == 1 && log.ids[3] == 4, "lru: 3 (frame 3), 1 (frame 4), 4 (frame 10) - oldest first, through fragmentation" );
	CHECK( off == 0 && nv2a_skin_cache_resident_count( c ) == 1, "lru: coalesced hole used" );
	nv2a_skin_cache_destroy( c );
}

static void test_min_age_protects_recent( void )
{
	nv2a_skin_cache_t *c = make_cache( 2 * G, 2 );
	evict_log_t log;
	size_t off;

	memset( &log, 0, sizeof( log ));
	nv2a_skin_cache_acquire( c, 1, G, 5, &off, record_evict, &log );
	nv2a_skin_cache_acquire( c, 2, G, 5, &off, record_evict, &log );

	/* frame 6: both were used one frame ago - younger than min_age 2. */
	CHECK( nv2a_skin_cache_acquire( c, 3, G, 6, &off, record_evict, &log ) == NV2A_SKIN_NO_ROOM, "age: one-frame-old skins are not evictable" );
	CHECK( log.count == 0, "age: nothing evicted on NO_ROOM" );

	nv2a_skin_cache_touch( c, 2, 6 );
	/* frame 7: 1 is two frames old, 2 only one. */
	CHECK( nv2a_skin_cache_acquire( c, 3, G, 7, &off, record_evict, &log ) == NV2A_SKIN_OK, "age: a two-frame-old skin is evictable" );
	CHECK( log.count == 1 && log.ids[0] == 1, "age: only the old enough one went" );

	/* Frame counter wrap: last used at 0xFFFFFFFF, now 1 -> age 2. */
	nv2a_skin_cache_touch( c, 2, 0xFFFFFFFFu );
	nv2a_skin_cache_touch( c, 3, 1 );
	CHECK( nv2a_skin_cache_acquire( c, 4, G, 1, &off, record_evict, &log ) == NV2A_SKIN_OK, "age: wrap-around age computed unsigned" );
	CHECK( log.count == 2 && log.ids[1] == 2, "age: the pre-wrap skin is the victim" );
	nv2a_skin_cache_destroy( c );
}

static void test_no_pointless_eviction( void )
{
	nv2a_skin_cache_t *c = make_cache( 4 * G, 1 );
	evict_log_t log;
	size_t off;

	memset( &log, 0, sizeof( log ));
	nv2a_skin_cache_acquire( c, 1, G, 1, &off, record_evict, &log );
	nv2a_skin_cache_acquire( c, 2, 3 * G, 5, &off, record_evict, &log );
	/* frame 5: 1 is evictable (age 4) but 2 is not (age 0) - freeing 1
	   gives one granule, the request needs two: evict nothing. */
	CHECK( nv2a_skin_cache_acquire( c, 3, 2 * G, 5, &off, record_evict, &log ) == NV2A_SKIN_NO_ROOM, "pointless: not enough reclaimable" );
	CHECK( log.count == 0 && nv2a_skin_cache_is_resident( c, 1 ), "pointless: the eligible skin was kept" );
	nv2a_skin_cache_destroy( c );
}

static void test_frame_allowance( void )
{
	nv2a_skin_cache_t *c = make_chunked( 64 * G, 64 * G, 2, 3 * G, 2 );
	size_t off;

	CHECK( nv2a_skin_cache_acquire( c, 1, 8 * G, 1, &off, NULL, NULL ) == NV2A_SKIN_OK, "allowance: first upload of a frame goes even past the byte cap" );
	CHECK( nv2a_skin_cache_acquire( c, 2, G, 1, &off, NULL, NULL ) == NV2A_SKIN_FRAME_CAP, "allowance: second upload over the byte cap is deferred" );
	CHECK( nv2a_skin_cache_acquire( c, 2, G, 2, &off, NULL, NULL ) == NV2A_SKIN_OK, "allowance: next frame resets it" );
	CHECK( nv2a_skin_cache_acquire( c, 3, G, 2, &off, NULL, NULL ) == NV2A_SKIN_OK, "allowance: second upload under both caps goes" );
	CHECK( nv2a_skin_cache_acquire( c, 4, G, 2, &off, NULL, NULL ) == NV2A_SKIN_FRAME_CAP, "allowance: third upload hits the count cap" );
	CHECK( !nv2a_skin_cache_is_resident( c, 4 ), "allowance: a deferred skin is not resident" );
	nv2a_skin_cache_destroy( c );
}

static void test_detach_and_abandon( void )
{
	nv2a_skin_cache_t *c = make_cache( 2 * G, 2 );
	size_t off = 0, detached = 99;

	nv2a_skin_cache_acquire( c, 1, G, 1, &off, NULL, NULL );
	CHECK( nv2a_skin_cache_detach( c, 1, &detached ) == 1 && detached == off, "detach: reports the offset" );
	CHECK( !nv2a_skin_cache_is_resident( c, 1 ), "detach: id forgotten" );
	CHECK( nv2a_skin_cache_used( c ) == G, "detach: storage still held until free_offset" );
	CHECK( nv2a_skin_cache_detach( c, 1, &detached ) == 0, "detach: second detach is a no-op" );
	nv2a_skin_cache_free_offset( c, detached );
	CHECK( nv2a_skin_cache_used( c ) == 0, "detach: free_offset releases it" );

	/* A detached id can be acquired again (slot reuse) and is never an
	   eviction victim while detached. */
	CHECK( nv2a_skin_cache_acquire( c, 1, G, 2, &off, NULL, NULL ) == NV2A_SKIN_OK, "detach: id reusable" );

	nv2a_skin_cache_acquire( c, 2, G, 2, &off, NULL, NULL );
	nv2a_skin_cache_abandon( c, 2 );
	CHECK( !nv2a_skin_cache_is_resident( c, 2 ) && nv2a_skin_cache_used( c ) == G, "abandon: storage released at once" );
	CHECK( nv2a_skin_cache_resident_count( c ) == 1, "abandon: resident count" );
	nv2a_skin_cache_destroy( c );
}

static void test_budget_never_exceeded( void )
{
	/* Pseudo-random churn: used never exceeds capacity, and resident
	   count always matches what is_resident reports. */
	nv2a_skin_cache_t *c = make_chunked( 32 * G, 8 * G, 3, 8 * G, 2 );
	uint32_t seed = 12345, frame, ok = 1;

	for( frame = 1; frame < 2000; frame++ )
	{
		uint32_t k;
		for( k = 0; k < 4; k++ )
		{
			uint32_t id, n, i;
			size_t off;

			seed = seed * 1103515245u + 12345u;
			id = ( seed >> 16 ) % 16;
			if( nv2a_skin_cache_is_resident( c, id ))
				nv2a_skin_cache_touch( c, id, frame );
			else
				nv2a_skin_cache_acquire( c, id, (( seed >> 8 ) % 6 + 1 ) * G, frame, &off, NULL, NULL );

			if( nv2a_skin_cache_used( c ) > nv2a_skin_cache_backed( c ) || nv2a_skin_cache_backed( c ) > nv2a_skin_cache_capacity( c ))
				ok = 0;
			for( i = n = 0; i < 16; i++ )
				n += nv2a_skin_cache_is_resident( c, i ) ? 1 : 0;
			if( n != nv2a_skin_cache_resident_count( c ))
				ok = 0;
		}
	}
	CHECK( ok, "churn: never over budget, counts consistent" );
	CHECK( nv2a_skin_cache_eviction_count( c ) > 0, "churn: evictions actually happened" );
	nv2a_skin_cache_destroy( c );
}

static void test_growth_on_demand( void )
{
	/* Budget 3 chunks of 2 granules. */
	nv2a_skin_cache_t *c = make_chunked( 6 * G + G / 2, 2 * G, 100, 1u << 30, 2 );
	evict_log_t log;
	size_t off;

	memset( &log, 0, sizeof( log ));
	CHECK( nv2a_skin_cache_capacity( c ) == 6 * G, "grow: budget rounds down to whole chunks" );
	CHECK( nv2a_skin_cache_backed( c ) == 0 && backing.grows == 0, "grow: nothing backed before the first skin" );

	CHECK( nv2a_skin_cache_acquire( c, 1, G, 1, &off, record_evict, &log ) == NV2A_SKIN_OK && off == 0, "grow: first skin backs chunk 0" );
	CHECK( backing.grows == 1 && backing.backed[0], "grow: exactly one chunk backed" );
	nv2a_skin_cache_acquire( c, 2, G, 1, &off, record_evict, &log );
	CHECK( off == G && backing.grows == 1, "grow: fits the backed chunk, no new one" );
	CHECK( nv2a_skin_cache_acquire( c, 3, 2 * G, 1, &off, record_evict, &log ) == NV2A_SKIN_OK && off == 2 * G,
		"grow: a full chunk backs the next, offset = chunk index * chunk bytes" );
	nv2a_skin_cache_acquire( c, 4, G, 1, &off, record_evict, &log );
	CHECK( off == 4 * G && backing.grows == 3, "grow: third chunk" );
	CHECK( nv2a_skin_cache_backed( c ) == 6 * G && nv2a_skin_cache_used( c ) == 5 * G, "grow: backed vs used" );

	nv2a_skin_cache_acquire( c, 5, G, 1, &off, record_evict, &log );
	/* At the budget: frame 1 skins are too young to evict at frame 2. */
	CHECK( nv2a_skin_cache_acquire( c, 6, G, 2, &off, record_evict, &log ) == NV2A_SKIN_NO_ROOM && backing.grows == 3,
		"grow: never past the budget" );
	CHECK( nv2a_skin_cache_acquire( c, 6, G, 3, &off, record_evict, &log ) == NV2A_SKIN_OK && log.count == 1,
		"grow: at the budget, evicts instead" );
	CHECK( nv2a_skin_cache_acquire( c, 7, 3 * G, 3, &off, record_evict, &log ) == NV2A_SKIN_TOO_BIG, "grow: a skin bigger than a chunk is TOO_BIG" );
	nv2a_skin_cache_destroy( c );
	CHECK( backing.releases == 3, "grow: destroy releases every backed chunk" );
}

static void test_grow_refused( void )
{
	nv2a_skin_cache_t *c = make_chunked( 4 * G, 2 * G, 100, 1u << 30, 2 );
	evict_log_t log;
	size_t off;

	memset( &log, 0, sizeof( log ));
	nv2a_skin_cache_acquire( c, 1, 2 * G, 1, &off, record_evict, &log );
	backing.refuse = 1;
	CHECK( nv2a_skin_cache_acquire( c, 2, G, 1, &off, record_evict, &log ) == NV2A_SKIN_NO_ROOM,
		"refused: no memory for a chunk and nothing evictable is NO_ROOM, not a crash" );
	CHECK( nv2a_skin_cache_backed( c ) == 2 * G, "refused: the refused chunk is not counted as backed" );
	CHECK( nv2a_skin_cache_acquire( c, 2, G, 5, &off, record_evict, &log ) == NV2A_SKIN_OK && log.count == 1 && off == 0,
		"refused: falls back to evicting within what is backed" );
	backing.refuse = 0;
	nv2a_skin_cache_destroy( c );
}

static void test_trim_and_flush( void )
{
	nv2a_skin_cache_t *c = make_chunked( 6 * G, 2 * G, 100, 1u << 30, 2 );
	evict_log_t log;
	size_t off, held;

	memset( &log, 0, sizeof( log ));
	nv2a_skin_cache_acquire( c, 1, 2 * G, 1, &off, record_evict, &log ); /* chunk 0 */
	nv2a_skin_cache_acquire( c, 2, 2 * G, 1, &off, record_evict, &log ); /* chunk 1 */
	nv2a_skin_cache_acquire( c, 3, 2 * G, 1, &off, record_evict, &log ); /* chunk 2 */

	nv2a_skin_cache_abandon( c, 2 );
	CHECK( nv2a_skin_cache_trim( c ) == 1 && !backing.backed[1] && backing.backed[0] && backing.backed[2],
		"trim: only the empty chunk is released" );

	nv2a_skin_cache_detach( c, 3, &held ); /* still waiting for the GPU */
	nv2a_skin_cache_flush( c, record_evict, &log );
	CHECK( log.count == 1 && log.ids[0] == 1, "flush: every resident skin evicted, whatever its age" );
	CHECK( !nv2a_skin_cache_is_resident( c, 1 ) && nv2a_skin_cache_resident_count( c ) == 0, "flush: nothing resident" );
	CHECK( !backing.backed[0] && backing.backed[2], "flush: a chunk holding a detached, unfreed offset stays backed" );
	nv2a_skin_cache_free_offset( c, held );
	CHECK( nv2a_skin_cache_trim( c ) == 1 && nv2a_skin_cache_backed( c ) == 0, "flush: released once that offset is freed" );

	CHECK( nv2a_skin_cache_acquire( c, 4, G, 9, &off, record_evict, &log ) == NV2A_SKIN_OK && backing.backed[0],
		"flush: the cache grows again on the next skin" );
	nv2a_skin_cache_destroy( c );
}

static void test_packers( void )
{
	const uint8_t rgba[4 * 5] = {
		0, 0, 0, 0,
		255, 255, 255, 255,
		255, 0, 0, 127,
		0, 255, 0, 128,
		0, 0, 255, 255,
	};
	uint16_t out[5];

	nv2a_pack_r5g6b5( rgba, out, 5 );
	CHECK( out[0] == 0x0000, "565: black" );
	CHECK( out[1] == 0xFFFF, "565: white" );
	CHECK( out[2] == 0xF800, "565: red in bits 15-11" );
	CHECK( out[3] == 0x07E0, "565: green in bits 10-5" );
	CHECK( out[4] == 0x001F, "565: blue in bits 4-0" );

	nv2a_pack_a1r5g5b5( rgba, out, 5 );
	CHECK( out[0] == 0x0000, "1555: transparent black" );
	CHECK( out[1] == 0xFFFF, "1555: opaque white" );
	CHECK( out[2] == 0x7C00, "1555: alpha 127 is transparent, red in bits 14-10" );
	CHECK( out[3] == 0x83E0, "1555: alpha 128 is opaque, green in bits 9-5" );
	CHECK( out[4] == 0x801F, "1555: blue in bits 4-0" );

	{
		const uint8_t mid[4] = { 132, 132, 132, 255 };
		nv2a_pack_r5g6b5( mid, out, 1 );
		/* 132*31/255 = 16.05 -> 16; 132*63/255 = 32.6 -> 33 (rounded, not truncated to 32) */
		CHECK( out[0] == (( 16 << 11 ) | ( 33 << 5 ) | 16 ), "565: rounds to nearest" );
	}
}

static void test_indexed_lut( void )
{
	uint8_t pal[768], gamma[256];
	uint16_t lut[256], out[4];
	const uint8_t idx[4] = { 0, 1, 254, 255 };
	int i;

	memset( pal, 0, sizeof( pal ));
	pal[0] = 255; pal[1] = 0; pal[2] = 0;         /* 0: red */
	pal[3] = 10;  pal[4] = 20; pal[5] = 30;       /* 1: dark, gamma'd below */
	pal[254 * 3 + 2] = 255;                        /* 254: blue */
	pal[255 * 3 + 0] = 255; pal[255 * 3 + 1] = 255; pal[255 * 3 + 2] = 255; /* 255: white in the file */
	for( i = 0; i < 256; i++ )
		gamma[i] = (uint8_t)( 255 - i ); /* an obviously non-identity table */

	nv2a_skin_build_lut( pal, gamma, 0, lut );
	nv2a_skin_expand_indexed( idx, lut, out, 4 );
	CHECK( out[0] == 0x07FF, "lut texgamma: red through an inverting table is cyan (0,255,255)" );
	{
		const uint8_t g[4] = { 245, 235, 225, 255 };
		uint16_t want;
		nv2a_pack_r5g6b5( g, &want, 1 );
		CHECK( out[1] == want, "lut texgamma: every channel goes through the gamma table" );
	}
	CHECK( out[3] == 0x0000, "lut texgamma: index 255 is an ordinary colour (white -> black via the table), not transparent" );

	nv2a_skin_build_lut( pal, NULL, 0, lut );
	CHECK( lut[0] == 0xF800 && lut[255] == 0xFFFF, "lut: NULL gamma is identity" );

	nv2a_skin_build_lut( pal, gamma, 1, lut );
	nv2a_skin_expand_indexed( idx, lut, out, 4 );
	CHECK( out[0] == 0xFC00, "lut masked: raw colour, no gamma, opaque (A1 + red)" );
	CHECK( out[2] == 0x801F, "lut masked: index 254 opaque blue" );
	CHECK( out[3] == 0x0000, "lut masked: index 255 transparent black whatever the file says" );
}

int main( void )
{
	test_indexed_lut();
	test_basic_acquire();
	test_lru_eviction_order();
	test_min_age_protects_recent();
	test_no_pointless_eviction();
	test_frame_allowance();
	test_detach_and_abandon();
	test_budget_never_exceeded();
	test_growth_on_demand();
	test_grow_refused();
	test_trim_and_flush();
	test_packers();

	printf( "%d/%d checks passed\n", total - fails, total );
	return fails ? 1 : 0;
}
