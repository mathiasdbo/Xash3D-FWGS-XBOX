/*
nv2a_warp_util_host_test.c - host test for ref/nv2a/nv2a_warp_util.c
(X9-X item 14 step 11: the water subdivision and warp arithmetic ported
from ref/gl/gl_rsurf.c, and the NV2A fog register math).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a -Iengine \
		tools/host-tests/nv2a_warp_util_host_test.c ref/nv2a/nv2a_warp_util.c \
		-lm -o .sweep/host-tests/nv2a_warp_util_test && .sweep/host-tests/nv2a_warp_util_test

Expected values are worked out by hand from the reference's expressions
and engine/warpsin.h's table (cited per check), or are properties the
reference guarantees (the pieces tile the polygon), not taken from the
code under test.
*/
#include "nv2a_warp_util.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )
#define NEAR( a, b ) ( fabs((double)( a ) - (double)( b )) < 1e-4 )

/* Collects the pieces nv2a_warp_subdivide emits. */
typedef struct
{
	int   count;
	int   verts[64];
	float mins[64][3], maxs[64][3];
	double area; /* sum of the pieces' xy areas (shoelace) */
} pieces_t;

static void collect( void *ctx, int numverts, const float *verts )
{
	pieces_t *p = (pieces_t *)ctx;
	double a = 0.0;
	int i, k;

	if( p->count >= 64 )
		return;
	p->verts[p->count] = numverts;
	for( k = 0; k < 3; k++ )
	{
		p->mins[p->count][k] = 1e9f;
		p->maxs[p->count][k] = -1e9f;
	}
	for( i = 0; i < numverts; i++ )
	{
		const float *v = verts + i * 3;
		const float *w = verts + (( i + 1 ) % numverts ) * 3;

		for( k = 0; k < 3; k++ )
		{
			if( v[k] < p->mins[p->count][k] ) p->mins[p->count][k] = v[k];
			if( v[k] > p->maxs[p->count][k] ) p->maxs[p->count][k] = v[k];
		}
		a += (double)v[0] * w[1] - (double)w[0] * v[1];
	}
	p->area += fabs( a ) * 0.5;
	p->count++;
}

static float buf[NV2A_WARP_SUBDIVIDE_SIZE][3];

static void set_square( float x0, float y0, float x1, float y1 )
{
	memset( buf, 0, sizeof( buf ));
	buf[0][0] = x0; buf[0][1] = y0;
	buf[1][0] = x1; buf[1][1] = y0;
	buf[2][0] = x1; buf[2][1] = y1;
	buf[3][0] = x0; buf[3][1] = y1;
}

/* engine/warpsin.h: 4 * sin( i * 2pi / 256 ) - index 0 is 0, 64 is the
   peak 4.0, 69 is 3.969920, and the index wraps at 256 (& 255). */
static void test_turbsin( void )
{
	CHECK( nv2a_warp_turbsin( 0 ) == 0.0f );
	CHECK( NEAR( nv2a_warp_turbsin( 64 ), 4.0 ));
	CHECK( NEAR( nv2a_warp_turbsin( 69 ), 3.969920 ));
	CHECK( nv2a_warp_turbsin( 256 ) == nv2a_warp_turbsin( 0 ));
	CHECK( nv2a_warp_turbsin( -1 ) == nv2a_warp_turbsin( 255 ));
	CHECK( NEAR( nv2a_warp_turbsin( 192 ), -4.0 ));
}

/* SubdividePolygon_r: a 128x128 face is cut at x = 64 (the mid point
   rounded to the 64 grid), then each half at y = 64 - four 64x64 quads,
   front (x >= 64) half first. */
static void test_subdivide_grid( void )
{
	pieces_t p;
	int i;

	memset( &p, 0, sizeof( p ));
	set_square( 0, 0, 128, 128 );
	CHECK( nv2a_warp_subdivide( 4, buf[0], collect, &p ) == 1 );
	CHECK( p.count == 4 );
	for( i = 0; i < p.count && i < 4; i++ )
	{
		CHECK( p.verts[i] == 4 );
		CHECK( NEAR( p.maxs[i][0] - p.mins[i][0], 64 ));
		CHECK( NEAR( p.maxs[i][1] - p.mins[i][1], 64 ));
	}
	CHECK( NEAR( p.mins[0][0], 64 )); /* the front of the x cut first */
	CHECK( NEAR( p.area, 128.0 * 128.0 ));
}

/* No cut lands 8 or more units inside a 32x32 face at the origin: the
   centre 16 rounds to the grid line 0, 0 units from mins - one piece. */
static void test_subdivide_small( void )
{
	pieces_t p;

	memset( &p, 0, sizeof( p ));
	set_square( 0, 0, 32, 32 );
	CHECK( nv2a_warp_subdivide( 4, buf[0], collect, &p ) == 1 );
	CHECK( p.count == 1 && p.verts[0] == 4 );
}

/* An off-grid 200x70 face from (-30, 10): x cuts at 64 (centre 70 rounds
   to 64) and so on - whatever the cuts, the pieces tile the face exactly
   and none is wider than 64 + 2 * 8 on an axis the cut could reach. */
static void test_subdivide_tiles( void )
{
	pieces_t p;
	int i;

	memset( &p, 0, sizeof( p ));
	set_square( -30, 10, 170, 80 );
	CHECK( nv2a_warp_subdivide( 4, buf[0], collect, &p ) == 1 );
	CHECK( p.count > 1 );
	CHECK( NEAR( p.area, 200.0 * 70.0 ));
	for( i = 0; i < p.count; i++ )
		CHECK( p.maxs[i][0] - p.mins[i][0] <= 64.0f + 16.0f );
}

/* A triangle split by the x = 64 line: one triangle and one quad. */
static void test_subdivide_triangle( void )
{
	pieces_t p;

	memset( &p, 0, sizeof( p ));
	memset( buf, 0, sizeof( buf ));
	buf[0][0] = 0;   buf[0][1] = 0;
	buf[1][0] = 128; buf[1][1] = 0;
	buf[2][0] = 0;   buf[2][1] = 40;
	CHECK( nv2a_warp_subdivide( 3, buf[0], collect, &p ) == 1 );
	CHECK( p.count == 2 );
	CHECK( p.verts[0] + p.verts[1] == 7 );
	CHECK( NEAR( p.area, 128.0 * 40.0 * 0.5 ));
}

/* gl_rsurf.c:212: more than SUBDIVIDE_SIZE - 4 points is refused. */
static void test_subdivide_limit( void )
{
	pieces_t p;

	memset( &p, 0, sizeof( p ));
	memset( buf, 0, sizeof( buf ));
	CHECK( nv2a_warp_subdivide( NV2A_WARP_MAX_VERTS + 1, buf[0], collect, &p ) == 0 );
	CHECK( p.count == 0 );
}

/* EmitWaterPolys' wave (gl_rsurf.c:959-964) at (0, 0, 10), time 0: both
   sine indices are 0, so nv = ( 0 + 8 ) * 0.8 + ( 0 + 8 ) = 14.4 and
   z = 14.4 * waveHeight + 10. waveHeight 0 leaves z alone. At time 0.1
   and (1, 2): index 1 = (int)( 16 + 3 ) = 19 -> 1.798445; index 2 =
   (int)( 5 + 17.1 - 2 ) = 20 -> 1.885585: nv = ( 1.885585 + 8 ) * 0.8 +
   9.798445 = 17.706913. */
static void test_wave( void )
{
	float v[3] = { 0, 0, 10 };
	float w[3] = { 1, 2, 5 };

	CHECK( nv2a_warp_wave_z( v, 0.0, 0.0f ) == 10.0f );
	CHECK( NEAR( nv2a_warp_wave_z( v, 0.0, 2.0f ), 38.8 ));
	CHECK( NEAR( nv2a_warp_wave_z( v, 0.0, -1.0f ), 10.0 - 14.4 ));
	CHECK( NEAR( nv2a_warp_wave_z( w, 0.1, 1.0f ), 5.0 + 17.706913 ));
}

/* EmitWaterPolys' coordinates (gl_rsurf.c:966-984), time 0: (64, 0) ->
   s = 64 + r_turbsin[(int)( 0 * TURBSCALE )] = 64, t = 0 +
   r_turbsin[(int)( 8 * 40.74366 ) & 255] = r_turbsin[325 & 255 = 69] =
   3.969920; both / 64. With ripples the coordinates are only divided. */
static void test_texcoord( void )
{
	float st[2];

	nv2a_warp_texcoord( 64.0f, 0.0f, 0.0, 0, st );
	CHECK( NEAR( st[0], 1.0 ));
	CHECK( NEAR( st[1], 3.969920 / 64.0 ));

	nv2a_warp_texcoord( 64.0f, 32.0f, 0.0, 1, st );
	CHECK( NEAR( st[0], 1.0 ) && NEAR( st[1], 0.5 ));
}

/* nv2a_warp_trunc_sum is (int)( base + a ): toward zero on both sides. */
static void test_trunc_sum( void )
{
	CHECK( nv2a_warp_trunc_sum( 10, 0.5f ) == 10 );
	CHECK( nv2a_warp_trunc_sum( 10, -0.5f ) == 9 );
	CHECK( nv2a_warp_trunc_sum( -10, 0.5f ) == -9 );  /* -9.5 -> -9 */
	CHECK( nv2a_warp_trunc_sum( -10, -0.5f ) == -10 ); /* -10.5 -> -10 */
	CHECK( nv2a_warp_trunc_sum( 0, -0.5f ) == 0 );
	CHECK( nv2a_warp_trunc_sum( -3, 3.0f ) == 0 );
	CHECK( nv2a_warp_trunc_sum( -3, 2.0f ) == -1 );
	CHECK( nv2a_warp_trunc_sum( 5, -7.25f ) == -2 ); /* -2.25 -> -2 */
}

/* The per-frame form against the reference expression over a sweep of
   times (map start to two hours) and vertices across a map's range,
   including fractional clip points: every result must equal the
   reference's or be one table step away, and the one-step cases must be
   rare (boundary rounding of a float sum instead of a double one). */
static unsigned lcg( unsigned *s ) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

static void test_frame_agreement( void )
{
	unsigned seed = 12345u;
	long total = 0, differ = 0, far = 0;
	int k;

	for( k = 0; k < 200000; k++ )
	{
		double time = ( lcg( &seed ) % 7200000 ) / 1000.0;
		float v[3];
		float a[2], b[2];
		nv2a_warp_frame_t fr;
		float za, zb;

		v[0] = (float)(( (int)( lcg( &seed ) % 16384 ) - 8192 ) / (int)( 1 + lcg( &seed ) % 4 ));
		v[1] = (float)(( (int)( lcg( &seed ) % 16384 ) - 8192 ) / (int)( 1 + lcg( &seed ) % 4 ));
		v[2] = (float)(( (int)( lcg( &seed ) % 8192 ) - 4096 ));
		nv2a_warp_frame_init( &fr, time );

		za = nv2a_warp_wave_z( v, time, 1.0f );
		zb = nv2a_warp_wave_z_frame( &fr, v, 1.0f );
		total++;
		if( za != zb ) { differ++; if( fabs( za - zb ) > 0.25 ) far++; }

		nv2a_warp_texcoord( v[0], v[1], time, 0, a );
		nv2a_warp_texcoord_frame( &fr, v[0], v[1], b );
		total += 2;
		if( a[0] != b[0] ) { differ++; if( fabs( a[0] - b[0] ) * 64.0 > 0.25 ) far++; }
		if( a[1] != b[1] ) { differ++; if( fabs( a[1] - b[1] ) * 64.0 > 0.25 ) far++; }
	}
	printf( "nv2a_warp_util: per-frame form differs in %ld of %ld results (%ld more than one step)\n", differ, total, far );
	CHECK( far == 0 );
	CHECK( differ * 1000 < total ); /* under 0.1 % */
}

/* The hardware's own fog formulas (header): EXP x + 2^( d * y * 16 ) - 1.5
   must give e^( -density * d ), EXP2 x + 2^( -d^2 * y^2 * 32 ) - 1.5
   e^( -( density * d )^2 ); LINEAR x + d * y - 1 must give
   ( end - d ) / ( end - start ). */
static void test_fog_params( void )
{
	float p[2];
	double d;

	nv2a_fog_exp_params( 0.001f, p );
	for( d = 0.0; d <= 3000.0; d += 500.0 )
		CHECK( NEAR( p[0] + pow( 2.0, d * p[1] * 16.0 ) - 1.5, exp( -0.001 * d )));

	nv2a_fog_exp2_params( 0.002f, p );
	for( d = 0.0; d <= 1500.0; d += 250.0 )
		CHECK( NEAR( p[0] + pow( 2.0, -d * d * p[1] * p[1] * 32.0 ) - 1.5, exp( -( 0.002 * d ) * ( 0.002 * d ))));

	nv2a_fog_linear_params( 100.0f, 500.0f, p );
	CHECK( NEAR( p[0] + 100.0 * p[1] - 1.0, 1.0 ));
	CHECK( NEAR( p[0] + 300.0 * p[1] - 1.0, 0.5 ));
	CHECK( NEAR( p[0] + 500.0 * p[1] - 1.0, 0.0 ));
}

/* nv_regs.h: RED 0x000000FF, GREEN 0x0000FF00, BLUE 0x00FF0000. */
static void test_fog_color( void )
{
	float c[3] = { 1.0f, 0.5f, 0.0f };
	float d[3] = { 2.0f, -1.0f, 0.2f };

	CHECK( nv2a_fog_pack_color( c ) == 0x000080FFu );
	CHECK( nv2a_fog_pack_color( d ) == (( 51u << 16 ) | 0xFFu ));
}

int main( void )
{
	test_turbsin();
	test_subdivide_grid();
	test_subdivide_small();
	test_subdivide_tiles();
	test_subdivide_triangle();
	test_subdivide_limit();
	test_wave();
	test_texcoord();
	test_trunc_sum();
	test_frame_agreement();
	test_fog_params();
	test_fog_color();

	if( failures )
	{
		printf( "nv2a_warp_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_warp_util: all passed\n" );
	return 0;
}
