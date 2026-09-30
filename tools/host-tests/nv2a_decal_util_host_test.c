/*
nv2a_decal_util_host_test.c - host test for ref/nv2a/nv2a_decal_util.c
(X9-X item 14 step 12: the decal clip and lightmap arithmetic ported from
ref/gl/gl_decals.c and gl_rsurf.c).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_decal_util_host_test.c ref/nv2a/nv2a_decal_util.c \
		-lm -o .sweep/host-tests/nv2a_decal_util_test && .sweep/host-tests/nv2a_decal_util_test

Expected values are worked out by hand from the reference's expressions
(cited per check) or are properties the clip must have (the result lies in
the decal's [0,1] square, its area is the overlap's), not taken from the
code under test.
*/
#include "nv2a_decal_util.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )
#define NEAR( a, b ) ( fabs((double)( a ) - (double)( b )) < 1e-4 )

typedef float vert_t[NV2A_DECAL_VERTEXSIZE];

/* A vertex on the z = 0 plane whose decal (s, t) is (x, y) / 16 - a
   16x16-unit decal at the origin's corner, as R_SetupDecalVertsForMSurface
   would project it with a unit basis scaled by 1/16. */
static void set_vert( float *v, float x, float y )
{
	v[0] = x;
	v[1] = y;
	v[2] = 0.0f;
	v[3] = x / 16.0f;
	v[4] = y / 16.0f;
	v[5] = v[6] = 0.0f;
}

/* Shoelace area in the decal's (s, t). */
static double st_area( vert_t *v, int n )
{
	double a = 0.0;
	int i;

	for( i = 0; i < n; i++ )
	{
		const float *p = v[i], *q = v[( i + 1 ) % n];
		a += (double)p[3] * q[4] - (double)q[3] * p[4];
	}
	return fabs( a ) * 0.5;
}

static int all_inside_unit( vert_t *v, int n )
{
	int i;

	for( i = 0; i < n; i++ )
	{
		if( v[i][3] < -1e-5f || v[i][3] > 1.0f + 1e-5f || v[i][4] < -1e-5f || v[i][4] > 1.0f + 1e-5f )
			return 0;
		/* position and (s, t) stay consistent: the clip lerps both */
		if( !NEAR( v[i][0], v[i][3] * 16.0f ) || !NEAR( v[i][1], v[i][4] * 16.0f ))
			return 0;
	}
	return 1;
}

static void test_inside_edges( void )
{
	float v[NV2A_DECAL_VERTEXSIZE];

	/* R_ClipInside, gl_decals.c:228-249: strict inequalities. */
	set_vert( v, 8.0f, 8.0f );
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_LEFT_EDGE ) == 1 );
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_RIGHT_EDGE ) == 1 );
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_TOP_EDGE ) == 1 );
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_BOTTOM_EDGE ) == 1 );

	set_vert( v, 0.0f, 16.0f ); /* s == 0, t == 1: both on the boundary, outside */
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_LEFT_EDGE ) == 0 );
	CHECK( nv2a_decal_clip_inside( v, NV2A_DECAL_BOTTOM_EDGE ) == 0 );
	CHECK( nv2a_decal_clip_inside( v, 7 ) == 0 ); /* not an edge */
}

static void test_intersect( void )
{
	float one[NV2A_DECAL_VERTEXSIZE], two[NV2A_DECAL_VERTEXSIZE], out[NV2A_DECAL_VERTEXSIZE];

	/* A segment from s = -0.5 to s = 1.5 crosses the left edge at t = 0.25
	   of the way (gl_decals.c:268): x = -8 + 0.25 * 32 = 0. */
	set_vert( one, -8.0f, 4.0f );
	set_vert( two, 24.0f, 12.0f );
	one[6] = 2.0f;
	two[6] = 6.0f;
	nv2a_decal_clip_intersect( one, two, out, NV2A_DECAL_LEFT_EDGE );
	CHECK( NEAR( out[0], 0.0f ));
	CHECK( NEAR( out[1], 6.0f ));
	CHECK( NEAR( out[3], 0.0f ) && NEAR( out[5], 0.0f )); /* :269 */
	CHECK( NEAR( out[4], 6.0f / 16.0f ));
	CHECK( NEAR( out[6], 3.0f )); /* :279, lerped */

	/* The right edge at s = 1: t = (-0.5 - 1) / (-0.5 - 1.5) = 0.75. */
	nv2a_decal_clip_intersect( one, two, out, NV2A_DECAL_RIGHT_EDGE );
	CHECK( NEAR( out[0], 16.0f ));
	CHECK( NEAR( out[3], 1.0f ) && NEAR( out[5], 1.0f ));

	/* The top edge's lightmap-s term reads two[4] (:297): out[5] =
	   one[5] + ( two[4] - one[5] ) * t, not two[5]. */
	set_vert( one, 4.0f, -8.0f );
	set_vert( two, 12.0f, 24.0f );
	one[5] = 10.0f;
	two[5] = 20.0f;
	nv2a_decal_clip_intersect( one, two, out, NV2A_DECAL_TOP_EDGE ); /* t = 0.25 */
	CHECK( NEAR( out[1], 0.0f ));
	CHECK( NEAR( out[4], 0.0f ) && NEAR( out[6], 0.0f ));
	CHECK( NEAR( out[5], 10.0f + ( two[4] - 10.0f ) * 0.25f ));
}

static void test_polygon( void )
{
	vert_t v[NV2A_DECAL_MAX_CLIPVERT], scratch[NV2A_DECAL_MAX_CLIPVERT];
	int n;

	/* Wholly inside: unchanged, same order (SHClip copies p when p and s
	   are both inside). */
	set_vert( v[0], 4, 4 ); set_vert( v[1], 12, 4 ); set_vert( v[2], 12, 12 ); set_vert( v[3], 4, 12 );
	n = nv2a_decal_clip_polygon( v, scratch, 4 );
	CHECK( n == 4 );
	CHECK( NEAR( v[0][0], 4 ) && NEAR( v[0][1], 4 ) && NEAR( v[2][0], 12 ) && NEAR( v[2][1], 12 ));

	/* Wholly outside: nothing (R_DecalCreate then unlinks, gl_decals.c:564). */
	set_vert( v[0], 20, 20 ); set_vert( v[1], 30, 20 ); set_vert( v[2], 30, 30 ); set_vert( v[3], 20, 30 );
	CHECK( nv2a_decal_clip_polygon( v, scratch, 4 ) == 0 );

	/* A wall much bigger than the decal: exactly the decal's square. */
	set_vert( v[0], -100, -100 ); set_vert( v[1], 100, -100 ); set_vert( v[2], 100, 100 ); set_vert( v[3], -100, 100 );
	n = nv2a_decal_clip_polygon( v, scratch, 4 );
	CHECK( n == 4 );
	CHECK( all_inside_unit( v, n ));
	CHECK( NEAR( st_area( v, n ), 1.0 ));

	/* Half overlap: the wall ends at x = 8, so the decal keeps s <= 0.5. */
	set_vert( v[0], -100, -100 ); set_vert( v[1], 8, -100 ); set_vert( v[2], 8, 100 ); set_vert( v[3], -100, 100 );
	n = nv2a_decal_clip_polygon( v, scratch, 4 );
	CHECK( n == 4 );
	CHECK( all_inside_unit( v, n ));
	CHECK( NEAR( st_area( v, n ), 0.5 ));

	/* A triangle cutting a corner off: the overlap is the triangle
	   (0,0)-(8,0)-(0,8) in units, 32 of 256 square units. */
	set_vert( v[0], -8, -8 ); set_vert( v[1], 24, -8 ); set_vert( v[2], -8, 24 );
	n = nv2a_decal_clip_polygon( v, scratch, 3 );
	CHECK( n >= 3 && n <= 7 );
	CHECK( all_inside_unit( v, n ));
	/* Hand area: the triangle's hypotenuse is x + y = 16, so its overlap
	   with [0,16]^2 is the square's lower-left half: 128 of 256. */
	CHECK( fabs( st_area( v, n ) - 0.5 ) < 1e-4 );

	/* Degenerate and oversized inputs are refused, not overrun. */
	CHECK( nv2a_decal_clip_polygon( v, scratch, 2 ) == 0 );
	CHECK( nv2a_decal_clip_polygon( v, scratch, NV2A_DECAL_MAX_INPUT_VERTS + 1 ) == 0 );
}

/* An input at the size limit, every vertex outside a different edge in
   turn, never writes past the buffers: a 28-gon around the decal. */
static void test_limit( void )
{
	vert_t v[NV2A_DECAL_MAX_CLIPVERT + 1], scratch[NV2A_DECAL_MAX_CLIPVERT + 1];
	const int n = NV2A_DECAL_MAX_INPUT_VERTS;
	int i, out;

	memset( v, 0, sizeof( v ));
	memset( scratch, 0, sizeof( scratch ));
	for( i = 0; i < n; i++ )
	{
		double a = 2.0 * 3.14159265358979 * i / n;
		set_vert( v[i], (float)( 8.0 + 9.0 * cos( a )), (float)( 8.0 + 9.0 * sin( a )));
	}
	/* guard entries past the buffers' logical end */
	for( i = 0; i < NV2A_DECAL_VERTEXSIZE; i++ )
		v[NV2A_DECAL_MAX_CLIPVERT][i] = scratch[NV2A_DECAL_MAX_CLIPVERT][i] = 12345.0f;

	out = nv2a_decal_clip_polygon( v, scratch, n );
	CHECK( out > 0 && out <= NV2A_DECAL_MAX_CLIPVERT );
	CHECK( all_inside_unit( v, out ));
	CHECK( v[NV2A_DECAL_MAX_CLIPVERT][0] == 12345.0f && scratch[NV2A_DECAL_MAX_CLIPVERT][0] == 12345.0f );
}

static void test_lightmap( void )
{
	/* R_LightmapCoord (gl_rsurf.c:130) before light_s and BLOCK_SIZE: a
	   face whose lmvecs are the axes with offsets (8, -4), lightmapmins
	   (-16, 32), sample size 16. v = (40, 100, 0):
	   s = (40 + 8 + 16) / 16 + 0.5 = 4.5; t = (100 - 4 - 32) / 16 + 0.5 = 4.5. */
	const float lmvecs[2][4] = { { 1, 0, 0, 8 }, { 0, 1, 0, -4 } };
	const short mins[2] = { -16, 32 };
	const float v[3] = { 40, 100, 0 };
	float out[2];

	nv2a_decal_lightmap_luxel( v, lmvecs, mins, 16.0f, out );
	CHECK( NEAR( out[0], 4.5f ));
	CHECK( NEAR( out[1], 4.5f ));

	/* The face's first luxel centre (s - mins == 0) is 0.5, as the baker's
	   (rawU - texturemins) / 16 + 0.5 (BspParser.cpp) - the block origin is
	   added by the caller. */
	{
		const float v0[3] = { -24, 36, 0 };
		nv2a_decal_lightmap_luxel( v0, lmvecs, mins, 16.0f, out );
		CHECK( NEAR( out[0], 0.5f ));
		CHECK( NEAR( out[1], 0.5f ));
	}

	/* An 8-unit sample size (Mod_SampleSizeForFace can return it) halves
	   the luxel step. */
	nv2a_decal_lightmap_luxel( v, lmvecs, mins, 8.0f, out );
	CHECK( NEAR( out[0], 8.5f ));
}

int main( void )
{
	test_inside_edges();
	test_intersect();
	test_polygon();
	test_limit();
	test_lightmap();

	if( failures )
	{
		printf( "nv2a_decal_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_decal_util: all checks passed\n" );
	return 0;
}
