/*
nv2a_arena_host_test.c - host test for ref/nv2a/nv2a_arena.c,
ref/nv2a/nv2a_2d_util.c, ref/nv2a/nv2a_tri_util.c and
ref/nv2a/nv2a_studio_util.c (X9-X Part C, step 4 Part A and step 5 Part
C): all four are deliberately free of pbkit/xboxkrnl/engine includes, so
they build and are checked here on the host rather than only ever
exercised for the first time on real hardware. Not part of any Xbox
build; run it on the host from the repository root, in the MSYS2
MinGW-64 shell:

  gcc -O1 -Wall -std=c99 -Iref/nv2a \
      tools/host-tests/nv2a_arena_host_test.c ref/nv2a/nv2a_arena.c ref/nv2a/nv2a_2d_util.c ref/nv2a/nv2a_tri_util.c ref/nv2a/nv2a_studio_util.c \
      -lm -o /tmp/nv2a_arena_test && /tmp/nv2a_arena_test

Exit status is nonzero iff any check failed (the failure count is also
printed), matching tools/host-tests/printf_host_test.c's own convention.
*/
#include "nv2a_arena.h"
#include "nv2a_2d_util.h"
#include "nv2a_tri_util.h"
#include "nv2a_studio_util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* dup, dup2, close, fileno - used only by the CHECK self-test below */

static int fails, total;

#define CHECK( cond, msg ) do { total++; if( !( cond )) { fails++; printf( "FAIL: %s\n", msg ); } } while( 0 )

/* ---- nv2a_arena ---- */

static void test_arena_alignment( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 4096 );
	size_t o1 = nv2a_arena_alloc( a, 1 );   /* rounds up to one granule */
	size_t o2 = nv2a_arena_alloc( a, 129 ); /* rounds up to two granules */

	CHECK( o1 == 0, "alignment: first alloc starts at offset 0" );
	CHECK( o1 % NV2A_ARENA_GRANULE == 0, "alignment: offset is granule-aligned" );
	CHECK( o2 == NV2A_ARENA_GRANULE, "alignment: a 1-byte request reserves exactly one granule" );
	CHECK( nv2a_arena_used( a ) == 3 * NV2A_ARENA_GRANULE, "alignment: used bytes rounded up (128 + 256)" );

	nv2a_arena_destroy( a );
}

static void test_arena_exact_fit( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 256 );
	size_t o = nv2a_arena_alloc( a, 256 );

	CHECK( o == 0, "exact fit: whole arena in one allocation" );
	CHECK( nv2a_arena_alloc( a, 1 ) == NV2A_ARENA_FAILED, "exact fit: nothing left after consuming it all" );
	CHECK( nv2a_arena_free_bytes( a ) == 0, "exact fit: zero free bytes remain" );

	nv2a_arena_destroy( a );
}

static void test_arena_full( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 256 );

	CHECK( nv2a_arena_alloc( a, 128 ) != NV2A_ARENA_FAILED, "full: first 128 fits" );
	CHECK( nv2a_arena_alloc( a, 128 ) != NV2A_ARENA_FAILED, "full: second 128 fits" );
	CHECK( nv2a_arena_alloc( a, 1 ) == NV2A_ARENA_FAILED, "full: a full arena refuses even the smallest request" );
	CHECK( nv2a_arena_largest_free( a ) == 0, "full: no free block remains" );

	nv2a_arena_destroy( a );
}

static void test_arena_first_fit_order( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 512 );
	size_t o0 = nv2a_arena_alloc( a, 128 );
	size_t o1 = nv2a_arena_alloc( a, 128 );
	size_t o2;

	nv2a_arena_free( a, o0 ); /* free list: [0,128) then [256,512) */
	o2 = nv2a_arena_alloc( a, 128 );

	CHECK( o2 == o0, "first-fit: the earliest (lowest-offset) free block wins, not the largest" );
	(void)o1;
	nv2a_arena_destroy( a );
}

static void test_arena_coalesce_right( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 384 );
	size_t o0 = nv2a_arena_alloc( a, 128 );
	size_t o1 = nv2a_arena_alloc( a, 128 );
	size_t o2 = nv2a_arena_alloc( a, 128 );

	nv2a_arena_free( a, o1 );
	nv2a_arena_free( a, o2 ); /* [128,256) then [256,384): must merge into one [128,384) */

	CHECK( nv2a_arena_largest_free( a ) == 256, "coalesce right: two adjacent frees merge into one 256-byte block" );
	(void)o0;
	nv2a_arena_destroy( a );
}

static void test_arena_coalesce_left( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 384 );
	size_t o0 = nv2a_arena_alloc( a, 128 );
	size_t o1 = nv2a_arena_alloc( a, 128 );
	size_t o2 = nv2a_arena_alloc( a, 128 );

	nv2a_arena_free( a, o0 );
	nv2a_arena_free( a, o1 ); /* [0,128) then [128,256): must merge, growing leftward from o1's own free */

	CHECK( nv2a_arena_largest_free( a ) == 256, "coalesce left: freeing into an existing left-hand free block merges it" );
	(void)o2;
	nv2a_arena_destroy( a );
}

static void test_arena_coalesce_both( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 384 );
	size_t o0 = nv2a_arena_alloc( a, 128 );
	size_t o1 = nv2a_arena_alloc( a, 128 );
	size_t o2 = nv2a_arena_alloc( a, 128 );

	nv2a_arena_free( a, o0 );
	nv2a_arena_free( a, o2 );
	nv2a_arena_free( a, o1 ); /* the middle block: must merge with both neighbours into the whole 384 bytes */

	CHECK( nv2a_arena_largest_free( a ) == 384, "coalesce both: the middle free reunites the whole arena" );
	nv2a_arena_destroy( a );
}

static void test_arena_fragment_then_coalesce( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 512 );
	size_t o0 = nv2a_arena_alloc( a, 128 );
	size_t o1 = nv2a_arena_alloc( a, 128 );
	size_t o2 = nv2a_arena_alloc( a, 128 );
	size_t o3 = nv2a_arena_alloc( a, 128 );
	size_t re;

	/* Fragment: free every other block, largest free is still just one granule. */
	nv2a_arena_free( a, o1 );
	nv2a_arena_free( a, o3 );
	CHECK( nv2a_arena_largest_free( a ) == 128, "fragment: two non-adjacent frees do not coalesce" );

	/* Re-fill one of the gaps, then free its neighbours: coalescing must
	   still work correctly on a re-used offset, not just a virgin one. */
	re = nv2a_arena_alloc( a, 128 );
	CHECK( re == o1, "fragment: first-fit re-uses the lower gap" );
	nv2a_arena_free( a, o0 );
	nv2a_arena_free( a, re );
	CHECK( nv2a_arena_largest_free( a ) == 256, "fragment-then-coalesce: re-freeing re-used offsets still merges correctly" );

	nv2a_arena_destroy( a );
	(void)o2;
}

static void test_arena_free_unknown( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 256 );
	size_t o0 = nv2a_arena_alloc( a, 128 );

	CHECK( nv2a_arena_free( a, 4096 ) == 0, "free unknown: an offset never handed out is refused" );
	CHECK( nv2a_arena_free( a, o0 ) == 1, "free unknown: freeing a real allocation still works" );
	CHECK( nv2a_arena_free( a, o0 ) == 0, "free unknown: freeing it again (double free) is refused" );

	nv2a_arena_destroy( a );
}

static void test_arena_zero_size( void )
{
	nv2a_arena_t *a = nv2a_arena_init( 256 );

	CHECK( nv2a_arena_alloc( a, 0 ) == NV2A_ARENA_FAILED, "zero-size: always fails rather than silently returning offset 0" );
	CHECK( nv2a_arena_used( a ) == 0, "zero-size: no bytes consumed by a failed request" );

	nv2a_arena_destroy( a );
}

/* ---- nv2a_2d_util ---- */

/* Plain file-scope callback + context struct, rather than a GCC nested
   function, so this test builds with any C99 compiler. */
typedef struct
{
	uint32_t starts[8];
	uint32_t counts[8];
	int n;
} chunk_capture_t;

static void capture_chunk( uint32_t start, uint32_t count, void *ctx )
{
	chunk_capture_t *cap = (chunk_capture_t *)ctx;
	if( cap->n < 8 )
	{
		cap->starts[cap->n] = start;
		cap->counts[cap->n] = count;
		cap->n++;
	}
}

static void test_chunk_quads( void )
{
	chunk_capture_t cap;

	/* 600 vertices = 150 quads: chunks of 256, 256, 88 (all multiples of 4). */
	memset( &cap, 0, sizeof( cap ));
	nv2a_2d_chunk_quads( 600, capture_chunk, &cap );

	CHECK( cap.n == 3, "chunk: 600 vertices split into 3 draws" );
	CHECK( cap.starts[0] == 0 && cap.counts[0] == 256, "chunk: first draw is 0..256" );
	CHECK( cap.starts[1] == 256 && cap.counts[1] == 256, "chunk: second draw is 256..512" );
	CHECK( cap.starts[2] == 512 && cap.counts[2] == 88, "chunk: remainder draw is 88 (a multiple of 4)" );

	memset( &cap, 0, sizeof( cap ));
	nv2a_2d_chunk_quads( 256, capture_chunk, &cap );
	CHECK( cap.n == 1 && cap.starts[0] == 0 && cap.counts[0] == 256, "chunk: exactly 256 vertices is one draw, not two" );
}

static void test_tile_split_no_wrap( void )
{
	nv2a_2d_tile_t out[8];
	int clamped = -1;
	uint32_t n = nv2a_2d_tile_split( 10.0f, 20.0f, 100.0f, 50.0f, 0.0f, 0.0f, 1.0f, 1.0f, out, 8, &clamped );

	CHECK( n == 1, "tile: an already-[0,1] span needs no splitting" );
	CHECK( !clamped, "tile: not reported as clamped" );
	CHECK( out[0].x == 10.0f && out[0].y == 20.0f && out[0].w == 100.0f && out[0].h == 50.0f, "tile: rect passed through unchanged" );
	CHECK( out[0].s1 == 0.0f && out[0].s2 == 1.0f && out[0].t1 == 0.0f && out[0].t2 == 1.0f, "tile: texture coords passed through unchanged" );
}

static void test_tile_split_wrap( void )
{
	nv2a_2d_tile_t out[8];
	int clamped = -1;
	/* s spans [-0.5, 1.5]: three cells, [-1,0), [0,1), [1,2), clipped to the span. */
	uint32_t n = nv2a_2d_tile_split( 0.0f, 0.0f, 200.0f, 10.0f, -0.5f, 0.0f, 1.5f, 1.0f, out, 8, &clamped );

	CHECK( n == 3, "tile: a 2-unit-wide s span splits into 3 cells" );
	CHECK( !clamped, "tile: three cells is well within the cap" );

	CHECK( out[0].s1 == 0.5f && out[0].s2 == 1.0f, "tile: first (partial) cell remapped to [0.5,1]" );
	CHECK( out[1].s1 == 0.0f && out[1].s2 == 1.0f, "tile: middle (whole) cell remapped to [0,1]" );
	CHECK( out[2].s1 == 0.0f && out[2].s2 == 0.5f, "tile: last (partial) cell remapped to [0,0.5]" );

	/* Screen-space width split proportionally: total span is 2.0 texture
	   units over 200 screen units, so each texture unit is 100 pixels -
	   the first and last (half-unit) cells are 50px, the middle is 100px. */
	CHECK( out[0].w == 50.0f && out[1].w == 100.0f && out[2].w == 50.0f, "tile: screen width split proportionally to each cell's texture-space width" );
	CHECK( out[0].x == 0.0f && out[1].x == 50.0f && out[2].x == 150.0f, "tile: screen x offsets are contiguous, left to right" );
}

static void test_tile_split_cap( void )
{
	nv2a_2d_tile_t out[4];
	int clamped = 0;
	/* A huge span that would need far more than max_tiles(=4) cells. */
	uint32_t n = nv2a_2d_tile_split( 0.0f, 0.0f, 100.0f, 100.0f, -5.0f, -5.0f, 500.0f, 500.0f, out, 4, &clamped );

	CHECK( n == 1, "tile cap: over the limit collapses to a single draw" );
	CHECK( clamped, "tile cap: reported as clamped" );
	CHECK( out[0].s1 == 0.0f && out[0].t1 == 0.0f && out[0].s2 == 1.0f && out[0].t2 == 1.0f, "tile cap: texture coords bound into [0,1]" );
	CHECK( out[0].x == 0.0f && out[0].w == 100.0f, "tile cap: screen rect is still the whole original quad" );
}

static void test_2d_matrix( void )
{
	float m[4][4];

	nv2a_2d_build_matrix( m, 640.0f, 480.0f, 640.0f, 480.0f, 0.0f );
	CHECK( m[0][0] == 1.0f && m[1][1] == 1.0f, "matrix: back buffer matching the engine's 2D size scales 1:1" );
	CHECK( m[3][2] == 32768.0f && m[3][3] == 1.0f, "matrix: fixed z and w rows" );
	CHECK( m[3][0] == 0.0f && m[3][1] == 0.0f, "matrix: default pixel offset is 0" );

	nv2a_2d_build_matrix( m, 320.0f, 240.0f, 640.0f, 480.0f, -0.5f );
	CHECK( m[0][0] == 0.5f && m[1][1] == 0.5f, "matrix: a smaller back buffer than the engine's 2D size scales down" );
	CHECK( m[3][0] == -0.5f && m[3][1] == -0.5f, "matrix: pixel offset applied to both x and y" );

	memset( m, 0x7F, sizeof( m )); /* poison, so a silent no-op would be caught below */
	nv2a_2d_build_matrix( m, 640.0f, 480.0f, 0.0f, 480.0f, 0.0f );
	CHECK( m[0][0] == 0.0f, "matrix: a zero engine width never divides by zero (all-zero matrix instead)" );
}

/* ---- nv2a_tri_util (X9-X step 4 Part A) ---- */

/* Captures each emitted list primitive by the PUSH INDEX (0,1,2,...)
   staged into vertex.x, rather than by value - lets every topology test
   below assert the exact vertex ordering the spec calls out (e.g. fan's
   (0,2,3)) instead of just a triangle count. */
typedef struct
{
	int      idx[8][4];
	uint32_t n_per_prim[8];
	int      n;
} tri_capture_t;

static void capture_tri( const nv2a_tri_vert_t *v, uint32_t n, void *ctx )
{
	tri_capture_t *cap = (tri_capture_t *)ctx;
	uint32_t i;

	if( cap->n >= 8 )
		return;

	for( i = 0; i < n; i++ )
		cap->idx[cap->n][i] = (int)v[i].x;
	cap->n_per_prim[cap->n] = n;
	cap->n++;
}

static void push_indexed( nv2a_tri_asm_t *a, int count )
{
	int i;

	for( i = 0; i < count; i++ )
	{
		nv2a_tri_vert_t v;
		memset( &v, 0, sizeof( v ));
		v.x = (float)i;
		nv2a_tri_push( a, &v );
	}
}

static void test_tri_fan( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_FAN, capture_tri, &cap );
	push_indexed( &a, 5 );
	nv2a_tri_end( &a );

	CHECK( cap.n == 3, "fan: 5 vertices give 3 triangles" );
	CHECK( cap.n_per_prim[0] == 3 && cap.idx[0][0] == 0 && cap.idx[0][1] == 1 && cap.idx[0][2] == 2, "fan: triangle 0 is (0,1,2)" );
	CHECK( cap.idx[1][0] == 0 && cap.idx[1][1] == 2 && cap.idx[1][2] == 3, "fan: triangle 1 is (0,2,3)" );
	CHECK( cap.idx[2][0] == 0 && cap.idx[2][1] == 3 && cap.idx[2][2] == 4, "fan: triangle 2 is (0,3,4)" );
	CHECK( nv2a_tri_verts_per_output_prim( NV2A_TRI_FAN ) == 3, "fan: outputs TRIANGLES (3 verts/prim)" );
}

static void test_tri_strip( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_STRIP, capture_tri, &cap );
	push_indexed( &a, 5 );
	nv2a_tri_end( &a );

	CHECK( cap.n == 3, "strip: 5 vertices give 3 triangles" );
	CHECK( cap.idx[0][0] == 0 && cap.idx[0][1] == 1 && cap.idx[0][2] == 2, "strip: triangle 0 is (0,1,2)" );
	CHECK( cap.idx[1][0] == 2 && cap.idx[1][1] == 1 && cap.idx[1][2] == 3, "strip: triangle 1 is (2,1,3) (odd winding swap)" );
	CHECK( cap.idx[2][0] == 2 && cap.idx[2][1] == 3 && cap.idx[2][2] == 4, "strip: triangle 2 is (2,3,4)" );
}

static void test_tri_quad_strip( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_QUAD_STRIP, capture_tri, &cap );
	push_indexed( &a, 6 );
	nv2a_tri_end( &a );

	CHECK( cap.n == 2, "quad-strip: 6 vertices give 2 quads" );
	CHECK( cap.n_per_prim[0] == 4 && cap.idx[0][0] == 0 && cap.idx[0][1] == 1 && cap.idx[0][2] == 3 && cap.idx[0][3] == 2,
		"quad-strip: quad 0 is (0,1,3,2)" );
	CHECK( cap.idx[1][0] == 2 && cap.idx[1][1] == 3 && cap.idx[1][2] == 5 && cap.idx[1][3] == 4,
		"quad-strip: quad 1 is (2,3,5,4)" );
	CHECK( nv2a_tri_verts_per_output_prim( NV2A_TRI_QUAD_STRIP ) == 4, "quad-strip: outputs QUADS (4 verts/prim)" );
}

static void test_tri_list_quads_drops_trailing( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_LIST_QUADS, capture_tri, &cap );
	push_indexed( &a, 5 ); /* one whole quad plus one trailing, dropped vertex */
	nv2a_tri_end( &a );

	CHECK( cap.n == 1, "quads: 5 vertices give exactly 1 quad, the trailing vertex is dropped" );
	CHECK( cap.idx[0][0] == 0 && cap.idx[0][1] == 1 && cap.idx[0][2] == 2 && cap.idx[0][3] == 3, "quads: the one quad is (0,1,2,3)" );
}

static void test_tri_list_triangles_drops_trailing( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_LIST_TRIANGLES, capture_tri, &cap );
	push_indexed( &a, 4 );
	nv2a_tri_end( &a );

	CHECK( cap.n == 1, "triangles: 4 vertices give exactly 1 triangle" );
	CHECK( cap.idx[0][0] == 0 && cap.idx[0][1] == 1 && cap.idx[0][2] == 2, "triangles: the one triangle is (0,1,2)" );
}

static void test_tri_list_lines_drops_trailing( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;

	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_begin( &a, NV2A_TRI_LIST_LINES, capture_tri, &cap );
	push_indexed( &a, 3 );
	nv2a_tri_end( &a );

	CHECK( cap.n == 1, "lines: 3 vertices give exactly 1 line" );
	CHECK( cap.idx[0][0] == 0 && cap.idx[0][1] == 1, "lines: the one line is (0,1)" );
}

static void test_tri_fan_and_strip_too_short( void )
{
	nv2a_tri_asm_t a;
	tri_capture_t cap;
	int n;

	for( n = 0; n <= 2; n++ )
	{
		memset( &cap, 0, sizeof( cap ));
		nv2a_tri_begin( &a, NV2A_TRI_FAN, capture_tri, &cap );
		push_indexed( &a, n );
		nv2a_tri_end( &a );
		CHECK( cap.n == 0, "fan: fewer than 3 vertices emits nothing" );

		memset( &cap, 0, sizeof( cap ));
		nv2a_tri_begin( &a, NV2A_TRI_STRIP, capture_tri, &cap );
		push_indexed( &a, n );
		nv2a_tri_end( &a );
		CHECK( cap.n == 0, "strip: fewer than 3 vertices emits nothing" );
	}
}

static void test_tri_chunk( void )
{
	chunk_capture_t cap;

	/* 300 triangle vertices: 256 is not a multiple of 3, so the cap is
	   255 (85 whole triangles) then the 45-vertex (15 triangle) remainder. */
	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_chunk( 300, 3, capture_chunk, &cap );
	CHECK( cap.n == 2, "tri chunk: 300 triangle vertices split into 2 draws" );
	CHECK( cap.starts[0] == 0 && cap.counts[0] == 255, "tri chunk: first draw is 255 (a multiple of 3)" );
	CHECK( cap.starts[1] == 255 && cap.counts[1] == 45, "tri chunk: remainder draw is 45" );

	/* 256 quad vertices: 256 IS a multiple of 4, so it fits in one draw. */
	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_chunk( 256, 4, capture_chunk, &cap );
	CHECK( cap.n == 1 && cap.starts[0] == 0 && cap.counts[0] == 256, "tri chunk: 256 quad vertices is one draw" );

	/* 257 point vertices: 256 + 1. */
	memset( &cap, 0, sizeof( cap ));
	nv2a_tri_chunk( 257, 1, capture_chunk, &cap );
	CHECK( cap.n == 2, "tri chunk: 257 point vertices split into 2 draws" );
	CHECK( cap.starts[0] == 0 && cap.counts[0] == 256, "tri chunk: first draw is 256 points" );
	CHECK( cap.starts[1] == 256 && cap.counts[1] == 1, "tri chunk: remainder draw is 1 point" );
}

/* ---- nv2a_studio_util (X9-X step 5 Part C) ---- */

static nv2a_studio_mesh_state_t base_state( void )
{
	nv2a_studio_mesh_state_t s;
	s.alpha_test_enable = 0;
	s.alpha_ref = 0;
	s.blend_enable = 0;
	s.sfactor = 0;
	s.dfactor = 0;
	s.depth_mask = 1;
	return s;
}

static void test_studio_mesh_state_masked( void )
{
	nv2a_studio_mesh_state_t out = nv2a_studio_mesh_state( NV2A_STUDIO_NF_MASKED, 1, base_state());

	CHECK( out.alpha_test_enable == 1, "masked: alpha test on" );
	CHECK( out.alpha_ref == NV2A_STUDIO_ALPHA_REF_MASKED, "masked: ref 0x7F" );
	CHECK( out.depth_mask == 1, "masked: depth mask on" );
	CHECK( out.blend_enable == 0, "masked: blend untouched" );
}

static void test_studio_mesh_state_additive_opaque( void )
{
	nv2a_studio_mesh_state_t out = nv2a_studio_mesh_state( NV2A_STUDIO_NF_ADDITIVE, 1, base_state());

	CHECK( out.blend_enable == 1, "additive opaque: blend on" );
	CHECK( out.sfactor == NV2A_STUDIO_SFACTOR_ONE, "additive opaque: sfactor ONE" );
	CHECK( out.dfactor == NV2A_STUDIO_DFACTOR_ONE, "additive opaque: dfactor ONE" );
	CHECK( out.depth_mask == 0, "additive opaque: depth mask off" );
}

static void test_studio_mesh_state_additive_translucent( void )
{
	nv2a_studio_mesh_state_t out = nv2a_studio_mesh_state( NV2A_STUDIO_NF_ADDITIVE, 0, base_state());

	CHECK( out.blend_enable == 1, "additive translucent: blend on" );
	CHECK( out.sfactor == NV2A_STUDIO_SFACTOR_SRC_ALPHA, "additive translucent: sfactor SRC_ALPHA" );
	CHECK( out.dfactor == NV2A_STUDIO_DFACTOR_ONE, "additive translucent: dfactor ONE" );
	CHECK( out.depth_mask == 1, "additive translucent: depth mask untouched (still the base state's)" );
}

static void test_studio_mesh_state_plain( void )
{
	nv2a_studio_mesh_state_t in = base_state();
	nv2a_studio_mesh_state_t out = nv2a_studio_mesh_state( 0, 1, in );

	CHECK( memcmp( &in, &out, sizeof( in )) == 0, "plain mesh: state left completely unchanged" );
}

static void test_studio_viewmodel_zrange( void )
{
	float z_min = -1.0f, z_max = -1.0f;

	nv2a_studio_viewmodel_zrange( &z_min, &z_max );
	CHECK( z_min == 0.0f, "viewmodel z range: near plane is 0" );
	CHECK( z_max == 0.3f * 65536.0f, "viewmodel z range: far plane is 0.3 x 65536" );
}

/* Per the lesson "NEVER trust a new tool's exit code without testing its
   failure path too" (docs/r3d/lessons.md): prove CHECK's own failure path
   actually runs - increments `fails` and `total`, prints FAIL - rather
   than being a no-op that would make every check above pass regardless
   of what it tested. The deliberate failure is undone ONLY once it has
   actually been observed; if CHECK's failure path turns out to be
   broken, `fails` is left nonzero so main()'s exit code still reports
   it, instead of an unconditional decrement papering over exactly the
   breakage this self-test exists to catch. CHECK's own printf is
   suppressed for the deliberate failure (stdout redirected to the null
   device for the duration of the call) so a healthy run does not also
   emit a "FAIL:" line that would make log-based failure scanning
   ambiguous. */
static void test_check_macro_failure_path( void )
{
	int before_fails = fails;
	int before_total = total;
	int saved_stdout_fd;

	fflush( stdout );
	saved_stdout_fd = dup( fileno( stdout ) );
	freopen( "NUL", "w", stdout ); /* this is a Windows target (MinGW-w64); "/dev/null" is not a valid path here */

	CHECK( 0, "self-test: this failure is EXPECTED - verified below, then undone" );

	fflush( stdout );
	dup2( saved_stdout_fd, fileno( stdout ) );
	close( saved_stdout_fd );

	if( fails == before_fails + 1 && total == before_total + 1 )
	{
		/* CHECK's failure path is proven to work: undo the deliberate,
		   proven-detected failure so it doesn't fail the whole suite. */
		fails = before_fails;
		total = before_total;
	}
	else
	{
		/* CHECK did NOT increment fails/total by exactly 1 - the
		   assertion mechanism itself is broken. Leave the counts as
		   CHECK left them (plus this note) so main()'s exit code
		   reflects the real breakage instead of silently returning
		   success. */
		fails++;
		total++;
		printf( "FAIL: self-test: CHECK's failure path did not increment fails/total by exactly 1 (fails %d, expected %d; total %d, expected %d)\n",
			fails - 1, before_fails + 1, total - 1, before_total + 1 );
	}
}

int main( void )
{
	test_check_macro_failure_path();

	test_arena_alignment();
	test_arena_exact_fit();
	test_arena_full();
	test_arena_first_fit_order();
	test_arena_coalesce_right();
	test_arena_coalesce_left();
	test_arena_coalesce_both();
	test_arena_fragment_then_coalesce();
	test_arena_free_unknown();
	test_arena_zero_size();

	test_chunk_quads();
	test_tile_split_no_wrap();
	test_tile_split_wrap();
	test_tile_split_cap();
	test_2d_matrix();

	test_tri_fan();
	test_tri_strip();
	test_tri_quad_strip();
	test_tri_list_quads_drops_trailing();
	test_tri_list_triangles_drops_trailing();
	test_tri_list_lines_drops_trailing();
	test_tri_fan_and_strip_too_short();
	test_tri_chunk();

	test_studio_mesh_state_masked();
	test_studio_mesh_state_additive_opaque();
	test_studio_mesh_state_additive_translucent();
	test_studio_mesh_state_plain();
	test_studio_viewmodel_zrange();

	printf( "%d checks, %d failures\n", total, fails );
	return fails != 0;
}
