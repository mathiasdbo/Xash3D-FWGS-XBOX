/*
nv2a_sky_util_host_test.c - host test for ref/nv2a/nv2a_sky_util.c
(X9-X item 14 step 10, the skybox clipping and corner arithmetic ported
from ref/gl/gl_warp.c).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_sky_util_host_test.c ref/nv2a/nv2a_sky_util.c \
		-lm -o .sweep/host-tests/nv2a_sky_util_test && .sweep/host-tests/nv2a_sky_util_test

Expected values are worked out by hand from gl_warp.c's st_to_vec and
vec_to_st tables (cited per check), or are properties the reference's own
code guarantees (MakeSkyVec's corner projects back onto the side and (s,t)
that made it), not taken from the code under test.
*/
#include "nv2a_sky_util.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )
#define NEAR( a, b ) ( fabs((double)( a ) - (double)( b )) < 1e-4 )

static float poly[NV2A_SKY_MAX_CLIP_VERTS + 2][3];

static void set_quad( float a[3], float b[3], float c[3], float d[3] )
{
	memcpy( poly[0], a, sizeof( poly[0] ));
	memcpy( poly[1], b, sizeof( poly[1] ));
	memcpy( poly[2], c, sizeof( poly[2] ));
	memcpy( poly[3], d, sizeof( poly[3] ));
}

static int count_nonempty( const nv2a_sky_bounds_t *b )
{
	int i, n = 0;

	for( i = 0; i < NV2A_SKY_SIDES; i++ )
		n += !nv2a_sky_side_empty( b, i );
	return n;
}

/* R_ClearSkyBox: nothing drawn yet, every side skipped by R_DrawSkyBox. */
static void test_clear( void )
{
	nv2a_sky_bounds_t b;

	nv2a_sky_clear( &b );
	CHECK( count_nonempty( &b ) == 0 );
	CHECK( b.mins[0][0] == 9999999.0f && b.maxs[1][5] == -9999999.0f );
}

/* A 100x100 ceiling patch 100 units straight above the eye. Every clip
   piece sums to mostly +z, so all of it lands on side 4 (up). vec_to_st[4]
   = { -2, -1, 3 }: s = -y / z, t = -x / z, so s and t both span
   [-0.5, 0.5]. */
static void test_ceiling_patch( void )
{
	nv2a_sky_bounds_t b;
	float a[3] = { -50, -50, 100 }, c[3] = { 50, -50, 100 }, d[3] = { 50, 50, 100 }, e[3] = { -50, 50, 100 };

	nv2a_sky_clear( &b );
	set_quad( a, c, d, e );
	CHECK( nv2a_sky_clip_polygon( &b, 4, poly[0] ) == 1 );
	CHECK( count_nonempty( &b ) == 1 );
	CHECK( !nv2a_sky_side_empty( &b, 4 ));
	CHECK( NEAR( b.mins[0][4], -0.5f ) && NEAR( b.maxs[0][4], 0.5f ));
	CHECK( NEAR( b.mins[1][4], -0.5f ) && NEAR( b.maxs[1][4], 0.5f ));
}

/* A wall patch 100 units ahead on +x, centred on the horizon. Side 0;
   vec_to_st[0] = { -2, 3, 1 }: s = -y / x, t = z / x. */
static void test_horizon_wall( void )
{
	nv2a_sky_bounds_t b;
	float a[3] = { 100, -50, -25 }, c[3] = { 100, 50, -25 }, d[3] = { 100, 50, 25 }, e[3] = { 100, -50, 25 };

	nv2a_sky_clear( &b );
	set_quad( a, c, d, e );
	CHECK( nv2a_sky_clip_polygon( &b, 4, poly[0] ) == 1 );
	CHECK( count_nonempty( &b ) == 1 );
	CHECK( NEAR( b.mins[0][0], -0.5f ) && NEAR( b.maxs[0][0], 0.5f ));
	CHECK( NEAR( b.mins[1][0], -0.25f ) && NEAR( b.maxs[1][0], 0.25f ));
}

/* A ceiling 100 up spanning +-1000: the clip planes cut it at the cube's
   edges, so it covers the whole of side 4 ([-1,1] both ways) and the top
   of the four walls, and never the floor (side 5). On side 0 (+x) the
   points have x in [100, 1000]: t = z / x spans [0.1, 1] and s = -y / x
   spans [-1, 1]. */
static void test_ceiling_spans_sides( void )
{
	nv2a_sky_bounds_t b;
	float a[3] = { -1000, -1000, 100 }, c[3] = { 1000, -1000, 100 }, d[3] = { 1000, 1000, 100 }, e[3] = { -1000, 1000, 100 };
	int side;

	nv2a_sky_clear( &b );
	set_quad( a, c, d, e );
	CHECK( nv2a_sky_clip_polygon( &b, 4, poly[0] ) == 1 );
	CHECK( nv2a_sky_side_empty( &b, 5 ));
	for( side = 0; side < 5; side++ )
		CHECK( !nv2a_sky_side_empty( &b, side ));
	CHECK( NEAR( b.mins[0][4], -1.0f ) && NEAR( b.maxs[0][4], 1.0f ));
	CHECK( NEAR( b.mins[1][4], -1.0f ) && NEAR( b.maxs[1][4], 1.0f ));
	CHECK( NEAR( b.mins[1][0], 0.1f ) && NEAR( b.maxs[1][0], 1.0f ));
	CHECK( NEAR( b.mins[0][0], -1.0f ) && NEAR( b.maxs[0][0], 1.0f ));
}

/* MakeSkyVec, side 4 (st_to_vec[4] = { -2, -1, 3 }), farclip 7086 (zmax
   4096 * 1.73 truncated, R_GetFarClip): half = 3543, so (s,t) = (-0.5,
   -0.5) is x = -t*h = 1771.5, y = -s*h = 1771.5, z = h, plus the origin;
   texture coordinate ((s+1)/2, 1 - (t+1)/2) = (0.25, 0.75). */
static void test_make_vec_up( void )
{
	const float origin[3] = { 10, 20, 30 };
	float pos[3], st[2];

	nv2a_sky_make_vec( -0.5f, -0.5f, 4, 7086, origin, pos, st );
	CHECK( NEAR( pos[0], 1771.5f + 10 ) && NEAR( pos[1], 1771.5f + 20 ) && NEAR( pos[2], 3543.0f + 30 ));
	CHECK( NEAR( st[0], 0.25f ) && NEAR( st[1], 0.75f ));
}

/* The clamp-to-edge branch: coordinates outside [-1,1] clamp to [0,1]
   before the t flip. */
static void test_make_vec_clamp( void )
{
	const float origin[3] = { 0, 0, 0 };
	float pos[3], st[2];

	nv2a_sky_make_vec( 1.5f, -2.0f, 0, 4096, origin, pos, st );
	CHECK( NEAR( st[0], 1.0f ) && NEAR( st[1], 1.0f ));
	nv2a_sky_make_vec( -1.0f, 1.0f, 0, 4096, origin, pos, st );
	CHECK( NEAR( st[0], 0.0f ) && NEAR( st[1], 0.0f ));
}

/* The two tables are inverses: a small patch around MakeSkyVec's corner
   for (s, t) on side `axis` projects back onto that side, with bounds
   containing (s, t). Checked for every side at interior points. */
static void test_tables_round_trip( void )
{
	static const float pts[][2] = { { 0.0f, 0.0f }, { 0.4f, -0.3f }, { -0.7f, 0.6f }, { 0.2f, 0.8f } };
	const float origin[3] = { 0, 0, 0 };
	int axis;
	size_t k;

	for( axis = 0; axis < NV2A_SKY_SIDES; axis++ )
	{
		for( k = 0; k < sizeof( pts ) / sizeof( pts[0] ); k++ )
		{
			nv2a_sky_bounds_t b;
			float p0[3], p1[3], p2[3], st[2];

			nv2a_sky_make_vec( pts[k][0] - 0.01f, pts[k][1] - 0.01f, axis, 2000, origin, p0, st );
			nv2a_sky_make_vec( pts[k][0] + 0.01f, pts[k][1] - 0.01f, axis, 2000, origin, p1, st );
			nv2a_sky_make_vec( pts[k][0], pts[k][1] + 0.01f, axis, 2000, origin, p2, st );
			memcpy( poly[0], p0, sizeof( p0 ));
			memcpy( poly[1], p1, sizeof( p1 ));
			memcpy( poly[2], p2, sizeof( p2 ));

			nv2a_sky_clear( &b );
			CHECK( nv2a_sky_clip_polygon( &b, 3, poly[0] ) == 1 );
			CHECK( count_nonempty( &b ) == 1 );
			CHECK( !nv2a_sky_side_empty( &b, axis ));
			CHECK( b.mins[0][axis] <= pts[k][0] && b.maxs[0][axis] >= pts[k][0] - 0.0101f );
			CHECK( b.mins[1][axis] <= pts[k][1] + 0.0001f && b.maxs[1][axis] >= pts[k][1] );
		}
	}
}

/* ClipSkyPolygon's MAX_CLIP_VERTS guard (gl_warp.c:138 Host_Error): the
   port refuses the polygon and leaves the bounds alone. At the limit it
   still clips. */
static void test_vertex_limit( void )
{
	nv2a_sky_bounds_t b;
	int i, n;

	for( n = NV2A_SKY_MAX_CLIP_VERTS; n <= NV2A_SKY_MAX_CLIP_VERTS + 1; n++ )
	{
		/* a regular n-gon on the ceiling, radius 50, 100 up */
		for( i = 0; i < n; i++ )
		{
			double a = 2.0 * 3.14159265358979 * i / n;
			poly[i][0] = (float)( 50.0 * cos( a ));
			poly[i][1] = (float)( 50.0 * sin( a ));
			poly[i][2] = 100.0f;
		}
		nv2a_sky_clear( &b );
		if( n == NV2A_SKY_MAX_CLIP_VERTS )
		{
			CHECK( nv2a_sky_clip_polygon( &b, n, poly[0] ) == 1 );
			CHECK( !nv2a_sky_side_empty( &b, 4 ));
			CHECK( NEAR( b.maxs[0][4], 0.5f ) && NEAR( b.mins[1][4], -0.5f ));
		}
		else
		{
			CHECK( nv2a_sky_clip_polygon( &b, n, poly[0] ) == 0 );
			CHECK( count_nonempty( &b ) == 0 );
		}
	}
}

/* r_skyTexOrder, gl_warp.c:23. */
static void test_tex_order( void )
{
	CHECK( nv2a_sky_tex_order[0] == 0 && nv2a_sky_tex_order[1] == 2 && nv2a_sky_tex_order[2] == 1 );
	CHECK( nv2a_sky_tex_order[3] == 3 && nv2a_sky_tex_order[4] == 4 && nv2a_sky_tex_order[5] == 5 );
}

int main( void )
{
	test_clear();
	test_ceiling_patch();
	test_horizon_wall();
	test_ceiling_spans_sides();
	test_make_vec_up();
	test_make_vec_clamp();
	test_tables_round_trip();
	test_vertex_limit();
	test_tex_order();

	if( failures )
	{
		printf( "nv2a_sky_util: %d check(s) failed\n", failures );
		return 1;
	}
	printf( "nv2a_sky_util: all checks passed\n" );
	return 0;
}
