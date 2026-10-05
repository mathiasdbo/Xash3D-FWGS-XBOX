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

/* EmitWaterPolys' wave (gl_rsurf.c:961-967) at (0, 0, 10), time 0: both
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

/* EmitWaterPolys' coordinates (gl_rsurf.c:969-984), time 0: (64, 0) ->
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

/* R5: vs_warp's quarter table folds back to engine/warpsin.h exactly -
   r_turbsin[j] == sign * r_turbsin[min( j', 128 - j' )], j' = j mod 128,
   sign -1 from 128 on - for all 256 entries, with no rounding. */
static void test_gpu_table_fold( void )
{
	float table[NV2A_WARP_GPU_TABLE_REGS][4];
	int j, bad = 0;

	nv2a_warp_gpu_table( table );
	for( j = 0; j < 256; j++ )
	{
		const int h = j >= 128;
		const int j2 = j - 128 * h;
		const int k = j2 < 128 - j2 ? j2 : 128 - j2;
		const float v = ( h ? -1.0f : 1.0f ) * table[k][0];

		if( v != nv2a_warp_turbsin( j ))
			bad++;
	}
	CHECK( bad == 0 );
	CHECK( table[64][0] == 4.0f && table[0][1] == 0.0f );
}

/* Mod_LooksLikeWaterTexture, mod_bmodel.c:2579-2591. */
static void test_looks_like_water( void )
{
	CHECK( nv2a_warp_looks_like_water( "!waterblue" ));
	CHECK( nv2a_warp_looks_like_water( "*lava1" ));
	CHECK( !nv2a_warp_looks_like_water( "*default" ));
	CHECK( nv2a_warp_looks_like_water( "water4b" ));
	CHECK( !nv2a_warp_looks_like_water( "WATER4b" )); /* Q_strncmp: case-sensitive */
	CHECK( nv2a_warp_looks_like_water( "LASER1" ));   /* Q_strnicmp: not */
	CHECK( nv2a_warp_looks_like_water( "laserbeam" ));
	CHECK( !nv2a_warp_looks_like_water( "lase" ));
	CHECK( !nv2a_warp_looks_like_water( "{water" ));
	CHECK( !nv2a_warp_looks_like_water( "sky" ));
}

static void test_gpu_bytes( void )
{
	CHECK( nv2a_warp_gpu_bytes( 0 ) == 0 );
	CHECK( nv2a_warp_gpu_bytes( 49836 ) == 49836u * 20u );
	CHECK( nv2a_warp_gpu_bytes( 1000000 ) == NV2A_WARP_GPU_MAX_VERTS * 20u );
	CHECK( sizeof( nv2a_warp_gpu_vertex_t ) == NV2A_WARP_GPU_VERTEX_BYTES );
}

/* A vertex coordinate: integers, halves and quarters (brush vertices and
   the 64-grid cuts), and arbitrary floats (a cut's interpolated points). */
static float rand_coord( unsigned *s, int range )
{
	const int kind = (int)( lcg( s ) % 4 );
	const int i = (int)( lcg( s ) % ( 2 * range )) - range;

	if( kind == 0 )
		return (float)i;
	if( kind == 1 )
		return (float)i * 0.5f;
	if( kind == 2 )
		return (float)i * 0.25f;
	return (float)i + (float)( lcg( s ) % 1000000 ) / 1000000.0f;
}

/* A client time: the first seconds of a map (the sums go below zero, so
   C's truncation toward zero matters), a play session, and a very long
   one (the bases pass 2^20). */
static double rand_time( unsigned *s )
{
	switch( lcg( s ) % 4 )
	{
	case 0:  return ( lcg( s ) % 3000 ) / 1000.0;
	case 1:  return 1.0 + ( lcg( s ) % 600000 ) / 1000.0;
	case 2:  return ( lcg( s ) % 7200000 ) / 1000.0;
	default: return 30000.0 + ( lcg( s ) % 70000000 ) / 1000.0;
	}
}

/* R5: vs_warp, emulated instruction for instruction, against the CPU
   path (nv2a_warp_wave_z_frame, nv2a_warp_texcoord_frame) - the same
   float operations in the same order, so every result must be identical -
   and against the reference's own double expressions
   (nv2a_warp_wave_z/nv2a_warp_texcoord, gl_rsurf.c:961-984), which the
   float sums can only miss by one table step at an index boundary. */
static void test_gpu_emulation( void )
{
	static const float heights[] = { 0.0f, 1.0f, -1.0f, 8.0f, -8.0f, 16.0f, -3.5f };
	float table[NV2A_WARP_GPU_TABLE_REGS][4];
	const uint8_t white[4] = { 255, 255, 255, 255 };
	unsigned seed = 4242u;
	long total = 0, cpu_differ = 0, ref_differ = 0, ref_far = 0, negative = 0;
	int k;

	nv2a_warp_gpu_table( table );
	for( k = 0; k < 400000; k++ )
	{
		const double time = rand_time( &seed );
		const float wh = heights[lcg( &seed ) % ( sizeof( heights ) / sizeof( heights[0] ))];
		float params[NV2A_WARP_GPU_PARAM_REGS][4];
		nv2a_warp_gpu_vertex_t v;
		nv2a_warp_frame_t fr;
		float gz, gst[2], cz, cst[2], rz, rst[2];

		v.xyz[0] = rand_coord( &seed, 4096 );
		v.xyz[1] = rand_coord( &seed, 4096 );
		v.xyz[2] = rand_coord( &seed, 4096 );
		v.os = rand_coord( &seed, 32768 );
		v.ot = rand_coord( &seed, 32768 );
		if( time * 160.0 + v.xyz[1] + v.xyz[0] < 0.0 || ( v.ot * 0.125 + time ) < 0.0 )
			negative++;

		nv2a_warp_frame_init( &fr, time );
		nv2a_warp_gpu_params( &fr, wh, white, params );
		nv2a_warp_gpu_emulate( (const float (*)[4])params, (const float (*)[4])table, &v, &gz, gst );

		cz = nv2a_warp_wave_z_frame( &fr, v.xyz, wh );
		nv2a_warp_texcoord_frame( &fr, v.os, v.ot, cst );
		rz = nv2a_warp_wave_z( v.xyz, time, wh );
		nv2a_warp_texcoord( v.os, v.ot, time, 0, rst );

		total += 3;
		cpu_differ += ( gz != cz ) + ( gst[0] != cst[0] ) + ( gst[1] != cst[1] );
		if( gz != rz ) { ref_differ++; if( fabs( gz - rz ) > fabs( wh ) * 0.25 + 1e-3 ) ref_far++; }
		if( gst[0] != rst[0] ) { ref_differ++; if( fabs( gst[0] - rst[0] ) * 64.0 > 0.25 ) ref_far++; }
		if( gst[1] != rst[1] ) { ref_differ++; if( fabs( gst[1] - rst[1] ) * 64.0 > 0.25 ) ref_far++; }
	}
	printf( "nv2a_warp_util: vs_warp emulation: %ld of %ld results differ from the CPU path; "
		"%ld from the reference's double sums (%ld more than one step); %ld vertices with a negative sum\n",
		cpu_differ, total, ref_differ, ref_far, negative );
	CHECK( cpu_differ == 0 );
	CHECK( ref_far == 0 );
	/* Under 0.5 %: the float sums' rounding grows with the operands, and
	   this sweep reaches texture coordinates of 32768 (an index sum near
	   170,000, whose float spacing is 1/64); the CPU path shares every
	   one of these, by the first check. */
	CHECK( ref_differ * 200 < total );
	CHECK( negative > 10000 );
}

/* Worked by hand against gl_rsurf.c:961-984 at time 1.0, where the sums
   are negative: v = ( -300.5, -200.25, 7 ), waveHeight 2:
   i1 = (int)( 160 - 200.25 - 300.5 ) = (int)( -340.75 ) = -340 -> & 255 = 172
   i2 = (int)( -1502.5 + 171 + 200.25 ) = (int)( -1131.25 ) = -1131 -> 149
   r_turbsin[172] = -r_turbsin[44] = -3.527685, r_turbsin[149] =
   -r_turbsin[21] = -1.971595; nv = ( 6.028405 * 0.8 ) + 4.472315 =
   9.295039; z = 7 + 18.590078. A floor instead of the truncation would
   read entries 171 and 148 instead. */
static void test_gpu_negative( void )
{
	float table[NV2A_WARP_GPU_TABLE_REGS][4];
	float params[NV2A_WARP_GPU_PARAM_REGS][4];
	const uint8_t c[4] = { 255, 128, 0, 51 };
	nv2a_warp_gpu_vertex_t v = { { -300.5f, -200.25f, 7.0f }, 0.0f, 0.0f };
	nv2a_warp_frame_t fr;
	float z, st[2];

	nv2a_warp_gpu_table( table );
	nv2a_warp_frame_init( &fr, 1.0 );
	nv2a_warp_gpu_params( &fr, 2.0f, c, params );
	nv2a_warp_gpu_emulate( (const float (*)[4])params, (const float (*)[4])table, &v, &z, st );
	CHECK( NEAR( z, 7.0 + 18.590078 ));
	CHECK( params[7][1] == 128.0f / 255.0f && params[7][3] == 51.0f / 255.0f );

	/* An exactly integral negative sum truncates to itself: time 0,
	   x + y = -256 -> index -256 & 255 = 0 -> r_turbsin[0] = 0; the
	   second index (int)( -640 - 128 ) = -768 & 255 = 0 too. */
	v.xyz[0] = -128.0f; v.xyz[1] = -128.0f; v.xyz[2] = 0.0f;
	nv2a_warp_frame_init( &fr, 0.0 );
	nv2a_warp_gpu_params( &fr, 1.0f, c, params );
	nv2a_warp_gpu_emulate( (const float (*)[4])params, (const float (*)[4])table, &v, &z, st );
	CHECK( z == 8.0f * 0.8f + 8.0f );
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
	test_gpu_table_fold();
	test_looks_like_water();
	test_gpu_bytes();
	test_gpu_emulation();
	test_gpu_negative();
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
