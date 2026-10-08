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


/* ---- the splitter against its libm form (x9-perf-efx, #159) ---------

   wrap_cell_range lost its floorf/ceilf calls (nxdk's switch the x87
   rounding mode twice each). Below is nv2a_efx_wrap_primitive as it was
   before that change (nv2a_efx_util.c at 0a78dd20, its wrap section
   verbatim but for the public function's name), and the test feeds both
   the same primitives and compares every emitted byte: random
   coordinates over many cells, coordinates on exact cell boundaries,
   zeros of both signs, negative ranges, tiny and power-of-two and
   non-power-of-two cell sizes, and values past 2^24 where every float is
   an integer. The run script builds it twice, with SSE and with x87
   floats. */

/* A clipped polygon: a triangle or quad gains at most one vertex per
   clipping plane, and a piece is clipped by four (two per axis). */
#define WRAP_POLY_MAX 12

typedef struct
{
	nv2a_tri_vert_t v[WRAP_POLY_MAX];
	uint32_t        n;
} wrap_poly_t;

static float wrap_coord( const nv2a_tri_vert_t *v, int axis )
{
	return axis ? v->v : v->u;
}

static void wrap_lerp( nv2a_tri_vert_t *out, const nv2a_tri_vert_t *a, const nv2a_tri_vert_t *b, float t )
{
	int i;

	out->x = a->x + ( b->x - a->x ) * t;
	out->y = a->y + ( b->y - a->y ) * t;
	out->z = a->z + ( b->z - a->z ) * t;
	out->u = a->u + ( b->u - a->u ) * t;
	out->v = a->v + ( b->v - a->v ) * t;

	for( i = 0; i < 4; i++ )
	{
		float c = (float)a->rgba[i] + ((float)b->rgba[i] - (float)a->rgba[i] ) * t;
		out->rgba[i] = (uint8_t)( c + 0.5f );
	}
}

/* Sutherland-Hodgman against one axis-aligned plane: keeps the side
   where sign * ( coord - bound ) >= 0. An intersection vertex is placed
   exactly on the bound, so the pieces of neighbouring cells share their
   edge bit for bit. */
static void wrap_clip( wrap_poly_t *out, const wrap_poly_t *in, int axis, float bound, float sign )
{
	uint32_t i;

	out->n = 0;

	for( i = 0; i < in->n; i++ )
	{
		const nv2a_tri_vert_t *a = &in->v[i];
		const nv2a_tri_vert_t *b = &in->v[( i + 1 ) % in->n];
		float da = sign * ( wrap_coord( a, axis ) - bound );
		float db = sign * ( wrap_coord( b, axis ) - bound );

		if( da >= 0.0f && out->n < WRAP_POLY_MAX )
			out->v[out->n++] = *a;

		if(( da >= 0.0f ) != ( db >= 0.0f ) && out->n < WRAP_POLY_MAX )
		{
			nv2a_tri_vert_t *m = &out->v[out->n++];

			wrap_lerp( m, a, b, da / ( da - db ));
			if( axis ) m->v = bound;
			else m->u = bound;
		}
	}
}

/* The cells [first, last] a coordinate range touches. A range ending
   exactly on a boundary does not reach into the next cell: [0, 1] is one
   cell, as GL_REPEAT samples a texture coordinate of exactly 1.0 at the
   texture's last texel edge, not the next copy's first. */
static void wrap_cell_range( float lo, float hi, float cell, int *first, int *last )
{
	*first = (int)floorf( lo / cell );
	*last = (int)ceilf( hi / cell ) - 1;
	if( *last < *first )
		*last = *first;
}

static void wrap_shift( nv2a_tri_vert_t *v, uint32_t n, float du, float dv )
{
	uint32_t i;

	for( i = 0; i < n; i++ )
	{
		v[i].u -= du;
		v[i].v -= dv;
	}
}

static void ref_wrap_primitive( const nv2a_tri_vert_t *v, uint32_t n, float cell_u, float cell_v,
	nv2a_efx_wrap_emit_fn emit, void *ctx )
{
	nv2a_tri_vert_t shifted[4];
	float umin, umax, vmin, vmax;
	int u0, u1, v0, v1, cu, cv;
	uint32_t i;

	if( n == 0 || n > 4 )
		return;

	if( !( cell_u > 0.0f ) || !( cell_v > 0.0f ))
	{
		emit( v, n, 0, ctx );
		return;
	}

	umin = umax = v[0].u;
	vmin = vmax = v[0].v;
	for( i = 1; i < n; i++ )
	{
		if( v[i].u < umin ) umin = v[i].u;
		if( v[i].u > umax ) umax = v[i].u;
		if( v[i].v < vmin ) vmin = v[i].v;
		if( v[i].v > vmax ) vmax = v[i].v;
	}

	wrap_cell_range( umin, umax, cell_u, &u0, &u1 );
	wrap_cell_range( vmin, vmax, cell_v, &v0, &v1 );

	/* One cell (the common case: a beam segment spans a fraction of its
	   texture), a point or line, or a span past the cap: shift whole. */
	if(( u0 == u1 && v0 == v1 ) || n < 3
		|| u1 - u0 >= NV2A_EFX_WRAP_MAX_CELLS || v1 - v0 >= NV2A_EFX_WRAP_MAX_CELLS )
	{
		memcpy( shifted, v, n * sizeof( *v ));
		wrap_shift( shifted, n, (float)u0 * cell_u, (float)v0 * cell_v );
		emit( shifted, n, 0, ctx );
		return;
	}

	for( cv = v0; cv <= v1; cv++ )
	{
		wrap_poly_t in, a, b;

		memcpy( in.v, v, n * sizeof( *v ));
		in.n = n;

		wrap_clip( &a, &in, 1, (float)cv * cell_v, 1.0f );
		wrap_clip( &b, &a, 1, (float)( cv + 1 ) * cell_v, -1.0f );
		if( b.n < 3 )
			continue;

		for( cu = u0; cu <= u1; cu++ )
		{
			wrap_poly_t c, d;
			uint32_t k;

			wrap_clip( &c, &b, 0, (float)cu * cell_u, 1.0f );
			wrap_clip( &d, &c, 0, (float)( cu + 1 ) * cell_u, -1.0f );
			if( d.n < 3 )
				continue;

			wrap_shift( d.v, d.n, (float)cu * cell_u, (float)cv * cell_v );

			/* A clipped convex polygon keeps the input's winding; fan it
			   from its first vertex. */
			for( k = 1; k + 1 < d.n; k++ )
			{
				nv2a_tri_vert_t tri[3];

				tri[0] = d.v[0];
				tri[1] = d.v[k];
				tri[2] = d.v[k + 1];
				emit( tri, 3, 1, ctx );
			}
		}
	}
}

static nv2a_tri_vert_t cmp_v[2][MAX_OUT * 4];
static uint32_t        cmp_n[2][MAX_OUT];
static int             cmp_split[2][MAX_OUT];
static int             cmp_count[2];

static void collect_a( const nv2a_tri_vert_t *v, uint32_t n, int split, void *ctx )
{
	int *side = (int *)ctx;
	int k = cmp_count[*side];

	if( k >= MAX_OUT )
		return;
	memcpy( &cmp_v[*side][k * 4], v, n * sizeof( *v ));
	cmp_n[*side][k] = n;
	cmp_split[*side][k] = split;
	cmp_count[*side]++;
}

static uint32_t lcg_state = 12345u;

static uint32_t lcg( void )
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return lcg_state >> 8;
}

/* A coordinate for cell size c: mostly random over +-8 cells, sometimes
   exactly on a boundary, a signed zero, a tiny value or a huge one. */
static float rand_coord( float c )
{
	uint32_t r = lcg() % 16u;
	float f = (float)( lcg() % 1000000u ) / 1000000.0f;

	switch( r )
	{
	case 0: return (float)((int)( lcg() % 17u ) - 8 ) * c;
	case 1: return 0.0f;
	case 2: return -0.0f;
	case 3: return ( f - 0.5f ) * 1e-6f;
	case 4: return (float)( lcg() % 2000000u ) * 16.0f * c; /* past 2^24 cells for small c */
	case 5: return ( f - 0.5f ) * 4.0f * c;
	default: return ( f - 0.5f ) * 16.0f * c;
	}
}

static int same_output( void )
{
	int i;

	if( cmp_count[0] != cmp_count[1] )
		return 0;
	for( i = 0; i < cmp_count[0]; i++ )
	{
		if( cmp_n[0][i] != cmp_n[1][i] || cmp_split[0][i] != cmp_split[1][i] )
			return 0;
		if( memcmp( &cmp_v[0][i * 4], &cmp_v[1][i * 4], cmp_n[0][i] * sizeof( nv2a_tri_vert_t )) != 0 )
			return 0;
	}
	return 1;
}

static void test_wrap_matches_libm_form( void )
{
	static const float cells[] = { 1.0f, 2.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 24.0f, 100.0f, 0.75f, 3.3f };
	long prims = 0, pieces = 0, cut = 0, differ = 0;
	int side0 = 0, side1 = 1;
	int i;

	for( i = 0; i < 400000; i++ )
	{
		nv2a_tri_vert_t v[4];
		uint32_t n = 3 + ( lcg() & 1u ), k;
		float cu = cells[lcg() % ( sizeof( cells ) / sizeof( cells[0] ))];
		float cv = cells[lcg() % ( sizeof( cells ) / sizeof( cells[0] ))];

		/* A beam segment's shape half the time: u across [0, cu], v a
		   short run somewhere along the scroll. */
		if( lcg() & 1u )
		{
			float v0 = rand_coord( cv ), dv = (float)( lcg() % 1000u ) / 1000.0f * cv;
			for( k = 0; k < n; k++ )
				v[k] = vert( (float)( lcg() % 1000u ), (float)( lcg() % 1000u ), ( k & 1u ) ? cu : 0.0f, v0 + ( k >= 2 ? dv : 0.0f ), (unsigned char)( lcg() & 255u ));
		}
		else
		{
			for( k = 0; k < n; k++ )
				v[k] = vert( (float)( lcg() % 1000u ), (float)( lcg() % 1000u ), rand_coord( cu ), rand_coord( cv ), (unsigned char)( lcg() & 255u ));
		}

		cmp_count[0] = cmp_count[1] = 0;
		ref_wrap_primitive( v, n, cu, cv, collect_a, &side0 );
		nv2a_efx_wrap_primitive( v, n, cu, cv, collect_a, &side1 );
		prims++;
		pieces += cmp_count[1];
		if( cmp_count[1] && cmp_split[1][0] )
			cut++;
		if( !same_output())
			differ++;
	}

	printf( "nv2a_efx_util: %ld primitives (%ld cut, %ld pieces) against the libm form, %ld different\n",
		prims, cut, pieces, differ );
	CHECK( differ == 0 );
	CHECK( cut > 1000 && prims - cut > 1000 ); /* both paths exercised */
}

int main( void )
{
	test_frac_noise();
	test_sine_noise();
	test_wrap_one_cell();
	test_wrap_split();
	test_wrap_matches_libm_form();

	if( failures )
	{
		printf( "nv2a_efx_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_efx_util: all checks passed\n" );
	return 0;
}
