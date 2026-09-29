/*
nv2a_efx_util_host_test.c - host test for ref/nv2a/nv2a_efx_util.c
(X9-X step 7, the effects renderer's beam noise and texture-repeat
splitter).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_efx_util_host_test.c ref/nv2a/nv2a_efx_util.c \
		-lm -o .sweep/host-tests/nv2a_efx_util_test && .sweep/host-tests/nv2a_efx_util_test

Expected values are worked out from gl_beams.c by hand (cited per check),
or are properties GL_REPEAT itself guarantees, not taken from the code
under test.
*/
#include "nv2a_efx_util.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )
#define NEAR( a, b ) ( fabs((double)( a ) - (double)( b )) < 1e-4 )

/* ---- noise ---------------------------------------------------------- */

static int   rng_calls;
static float rng_value;

static float fixed_random( float lo, float hi )
{
	rng_calls++;
	CHECK( lo == -0.125f && hi == 0.125f ); /* gl_beams.c:50 */
	return rng_value;
}

static void test_frac_noise( void )
{
	float noise[NV2A_EFX_NOISE_DIVISIONS + 1];
	int i;

	/* All-zero random: with both ends zero (R_BeamDraw, gl_beams.c:912-913)
	   every midpoint is the average of two zeros. */
	memset( noise, 0, sizeof( noise ));
	rng_calls = 0;
	rng_value = 0.0f;
	nv2a_efx_frac_noise( noise, NV2A_EFX_NOISE_DIVISIONS, fixed_random );
	for( i = 0; i <= NV2A_EFX_NOISE_DIVISIONS; i++ )
		CHECK( noise[i] == 0.0f );
	/* One random draw per interior point: 63 for 64 divisions. */
	CHECK( rng_calls == NV2A_EFX_NOISE_DIVISIONS - 1 );

	/* divs = 4, random always 0.1, ends 0:
	   noise[2] = 0 + 4*0.1 = 0.4; then FracNoise(&noise[2], 2):
	   noise[3] = (0.4 + 0)/2 + 2*0.1 = 0.4; then FracNoise(noise, 2):
	   noise[1] = (0 + 0.4)/2 + 2*0.1 = 0.4. */
	memset( noise, 0, sizeof( noise ));
	rng_value = 0.1f;
	nv2a_efx_frac_noise( noise, 4, fixed_random );
	CHECK( NEAR( noise[2], 0.4 ));
	CHECK( NEAR( noise[3], 0.4 ));
	CHECK( NEAR( noise[1], 0.4 ));
	CHECK( noise[0] == 0.0f && noise[4] == 0.0f );

	/* divs < 2 touches nothing (gl_beams.c:47). */
	noise[0] = 7.0f;
	rng_calls = 0;
	nv2a_efx_frac_noise( noise, 1, fixed_random );
	CHECK( rng_calls == 0 && noise[0] == 7.0f );
}

static void test_sine_noise( void )
{
	float noise[NV2A_EFX_NOISE_DIVISIONS + 1];

	noise[NV2A_EFX_NOISE_DIVISIONS] = 123.0f;
	nv2a_efx_sine_noise( noise, NV2A_EFX_NOISE_DIVISIONS );
	CHECK( NEAR( noise[0], 0.0 ));
	CHECK( NEAR( noise[32], 1.0 ));                 /* sin(pi/2) */
	CHECK( NEAR( noise[16], sin( 3.14159265 / 4 ))); /* sin(pi/4) */
	CHECK( noise[NV2A_EFX_NOISE_DIVISIONS] == 123.0f ); /* only i < divs, gl_beams.c:64 */
}

/* ---- texture repeat ------------------------------------------------- */

#define MAX_OUT 256

static nv2a_tri_vert_t out_v[MAX_OUT * 4];
static uint32_t        out_n[MAX_OUT];
static int             out_split[MAX_OUT];
static int             out_count;

static void collect( const nv2a_tri_vert_t *v, uint32_t n, int split, void *ctx )
{
	(void)ctx;
	if( out_count >= MAX_OUT )
		return;
	memcpy( &out_v[out_count * 4], v, n * sizeof( *v ));
	out_n[out_count] = n;
	out_split[out_count] = split;
	out_count++;
}

static nv2a_tri_vert_t vert( float x, float y, float u, float v, unsigned char a )
{
	nv2a_tri_vert_t r;
	r.x = x; r.y = y; r.z = 0.0f;
	r.u = u; r.v = v;
	r.rgba[0] = r.rgba[1] = r.rgba[2] = 255;
	r.rgba[3] = a;
	return r;
}

static double tri_area( const nv2a_tri_vert_t *t )
{
	return 0.5 * (( t[1].x - t[0].x ) * ( t[2].y - t[0].y ) - ( t[2].x - t[0].x ) * ( t[1].y - t[0].y ));
}

/* Every emitted texture coordinate must lie in the first cell - what the
   clamped linear texture can sample. */
static void check_all_in_cell( float cu, float cv )
{
	int i;
	uint32_t k;

	for( i = 0; i < out_count; i++ )
	{
		for( k = 0; k < out_n[i]; k++ )
		{
			const nv2a_tri_vert_t *p = &out_v[i * 4 + k];
			CHECK( p->u >= -1e-4f && p->u <= cu + 1e-4f );
			CHECK( p->v >= -1e-4f && p->v <= cv + 1e-4f );
		}
	}
}

static void test_wrap_one_cell( void )
{
	nv2a_tri_vert_t tri[3];

	/* A triangle inside cell (0, 3) of a 16x64 texture - V 200..240 texels,
	   the fourth copy - is shifted whole by 3*64, keeping n and order. */
	tri[0] = vert( 0, 0, 2.0f, 200.0f, 255 );
	tri[1] = vert( 1, 0, 14.0f, 200.0f, 255 );
	tri[2] = vert( 0, 1, 2.0f, 240.0f, 255 );
	out_count = 0;
	nv2a_efx_wrap_primitive( tri, 3, 16.0f, 64.0f, collect, NULL );
	CHECK( out_count == 1 && out_n[0] == 3 && out_split[0] == 0 );
	CHECK( NEAR( out_v[0].v, 8.0 ) && NEAR( out_v[2].v, 48.0 ));
	CHECK( NEAR( out_v[0].u, 2.0 ) && NEAR( out_v[1].u, 14.0 ));
	CHECK( out_v[0].x == 0 && out_v[1].x == 1 );

	/* A range ending exactly on a boundary is one cell: V [0, 64] and
	   U [0, 16] are the whole texture once (a beam's U is always 0..1). */
	tri[0] = vert( 0, 0, 0.0f, 0.0f, 255 );
	tri[1] = vert( 1, 0, 16.0f, 0.0f, 255 );
	tri[2] = vert( 0, 1, 0.0f, 64.0f, 255 );
	out_count = 0;
	nv2a_efx_wrap_primitive( tri, 3, 16.0f, 64.0f, collect, NULL );
	CHECK( out_count == 1 && out_split[0] == 0 );
	CHECK( NEAR( out_v[2].v, 64.0 ) && NEAR( out_v[1].u, 16.0 ));

	/* Negative coordinates shift up: V -0.75..-0.25 of a normalized
	   texture is cell -1, so it becomes 0.25..0.75. */
	tri[0] = vert( 0, 0, 0.0f, -0.75f, 255 );
	tri[1] = vert( 1, 0, 1.0f, -0.75f, 255 );
	tri[2] = vert( 0, 1, 0.0f, -0.25f, 255 );
	out_count = 0;
	nv2a_efx_wrap_primitive( tri, 3, 1.0f, 1.0f, collect, NULL );
	CHECK( out_count == 1 && NEAR( out_v[0].v, 0.25 ) && NEAR( out_v[2].v, 0.75 ));

	/* A quad inside one cell stays a quad (the TriAPI keeps its QUADS op). */
	{
		nv2a_tri_vert_t q[4];
		q[0] = vert( 0, 0, 0.0f, 1.2f, 255 );
		q[1] = vert( 1, 0, 1.0f, 1.2f, 255 );
		q[2] = vert( 1, 1, 1.0f, 1.8f, 255 );
		q[3] = vert( 0, 1, 0.0f, 1.8f, 255 );
		out_count = 0;
		nv2a_efx_wrap_primitive( q, 4, 1.0f, 1.0f, collect, NULL );
		CHECK( out_count == 1 && out_n[0] == 4 && out_split[0] == 0 );
		CHECK( NEAR( out_v[0].v, 0.2 ) && NEAR( out_v[3].v, 0.8 ));
	}
}

static void test_wrap_split( void )
{
	nv2a_tri_vert_t q[4];
	double area = 0.0;
	int i;

	/* A beam segment quad: U 0..1, V 0.5..2.5 over positions y 0..2
	   (v = y + 0.5), normalized texture. It crosses V = 1 and V = 2, so
	   three cells: V 0.5..1 (y 0..0.5), 1..2 (y 0.5..1.5), 2..2.5
	   (y 1.5..2), each a rectangle cut into two triangles. */
	q[0] = vert( 0, 0, 0.0f, 0.5f, 0 );
	q[1] = vert( 1, 0, 1.0f, 0.5f, 0 );
	q[2] = vert( 1, 2, 1.0f, 2.5f, 200 );
	q[3] = vert( 0, 2, 0.0f, 2.5f, 200 );
	out_count = 0;
	nv2a_efx_wrap_primitive( q, 4, 1.0f, 1.0f, collect, NULL );
	CHECK( out_count == 6 );
	check_all_in_cell( 1.0f, 1.0f );

	for( i = 0; i < out_count; i++ )
	{
		const nv2a_tri_vert_t *t = &out_v[i * 4];
		uint32_t k;

		CHECK( out_n[i] == 3 && out_split[i] == 1 );
		/* Winding kept: the input quad is counter-clockwise in x,y. */
		CHECK( tri_area( t ) > 0.0 );
		area += tri_area( t );

		/* GL_REPEAT samples v - floor(v); the piece must carry exactly
		   that for its own position (v = y + 0.5 before the shift), and
		   colour must follow position (alpha = 100 * y). */
		for( k = 0; k < 3; k++ )
		{
			double orig_v = t[k].y + 0.5;
			double frac = orig_v - floor( orig_v );
			/* a vertex on a boundary may carry 0 or 1 - both sample the
			   same texel edge under GL_REPEAT's own continuity */
			CHECK( NEAR( t[k].v, frac ) || ( NEAR( frac, 0.0 ) && NEAR( t[k].v, 1.0 )));
			CHECK( abs( (int)t[k].rgba[3] - (int)( 100.0 * t[k].y + 0.5 )) <= 1 );
		}
	}
	/* The pieces tile the quad exactly: 1 x 2. */
	CHECK( NEAR( area, 2.0 ));

	/* A triangle crossing both U and V boundaries of a 16x64 texture:
	   U 8..24 (cells 0,1), V 32..96 (cells 0,1). Area in x,y is kept
	   and every piece lands in the first cell. */
	{
		nv2a_tri_vert_t t[3];
		t[0] = vert( 0, 0, 8.0f, 32.0f, 255 );
		t[1] = vert( 1, 0, 24.0f, 32.0f, 255 );
		t[2] = vert( 0, 1, 8.0f, 96.0f, 255 );
		out_count = 0;
		nv2a_efx_wrap_primitive( t, 3, 16.0f, 64.0f, collect, NULL );
		CHECK( out_count >= 3 );
		check_all_in_cell( 16.0f, 64.0f );
		area = 0.0;
		for( i = 0; i < out_count; i++ )
		{
			CHECK( tri_area( &out_v[i * 4] ) >= 0.0 );
			area += tri_area( &out_v[i * 4] );
		}
		CHECK( NEAR( area, 0.5 ));
	}

	/* Beyond the cap the primitive is emitted whole, shifted to its first
	   cell (clamped look, never a runaway loop). */
	{
		nv2a_tri_vert_t t[3];
		t[0] = vert( 0, 0, 0.0f, 0.5f, 255 );
		t[1] = vert( 1, 0, 1.0f, 0.5f, 255 );
		t[2] = vert( 0, 1, 0.0f, 0.5f + NV2A_EFX_WRAP_MAX_CELLS + 1, 255 );
		out_count = 0;
		nv2a_efx_wrap_primitive( t, 3, 1.0f, 1.0f, collect, NULL );
		CHECK( out_count == 1 && out_split[0] == 0 && NEAR( out_v[0].v, 0.5 ));
	}

	/* Lines and points are never split, only shifted. */
	{
		nv2a_tri_vert_t l[2];
		l[0] = vert( 0, 0, 0.0f, 1.5f, 255 );
		l[1] = vert( 1, 0, 0.0f, 3.5f, 255 );
		out_count = 0;
		nv2a_efx_wrap_primitive( l, 2, 1.0f, 1.0f, collect, NULL );
		CHECK( out_count == 1 && out_n[0] == 2 && NEAR( out_v[0].v, 0.5 ) && NEAR( out_v[1].v, 2.5 ));
	}

	/* A zero cell size (no texture size known) passes through untouched. */
	{
		nv2a_tri_vert_t t[3];
		t[0] = vert( 0, 0, 0.0f, 5.5f, 255 );
		t[1] = vert( 1, 0, 1.0f, 5.5f, 255 );
		t[2] = vert( 0, 1, 0.0f, 9.5f, 255 );
		out_count = 0;
		nv2a_efx_wrap_primitive( t, 3, 0.0f, 1.0f, collect, NULL );
		CHECK( out_count == 1 && NEAR( out_v[0].v, 5.5 ));
	}
}

int main( void )
{
	test_frac_noise();
	test_sine_noise();
	test_wrap_one_cell();
	test_wrap_split();

	if( failures )
	{
		printf( "nv2a_efx_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_efx_util: all checks passed\n" );
	return 0;
}
