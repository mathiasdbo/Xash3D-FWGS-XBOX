/*
nv2a_sprite_util_host_test.c - host test for ref/nv2a/nv2a_sprite_util.c
(X9-X step 6, the sprite renderer's GL-state mapping and frame choice).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_sprite_util_host_test.c ref/nv2a/nv2a_sprite_util.c \
		-o .sweep/host-tests/nv2a_sprite_util_test && .sweep/host-tests/nv2a_sprite_util_test

Expected values are worked out from gl_sprite.c by hand (cited per check),
not from the code under test.
*/
#include "nv2a_sprite_util.h"
#include <stdio.h>
#include <math.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

/* R_SetupGL's tail state (gl_rmain.c:568-572) plus GL's defaults: what a
   sprite normally inherits. */
static nv2a_sprite_state_t base_state( void )
{
	nv2a_sprite_state_t s;
	s.blend_enable = 0;
	s.sfactor = 0x1234u; /* sentinels: a mode that does not set them must keep them */
	s.dfactor = 0x5678u;
	s.depth_mask = 1;
	s.depth_test = 1;
	s.depth_func_equal = 0;
	s.alpha_test_enable = 0;
	s.alpha_ref = NV2A_SPRITE_ALPHA_REF_DEFAULT;
	return s;
}

static void test_begin_state( void )
{
	nv2a_sprite_state_t s;

	/* kRenderNormal: blend off, nothing else but the alpha test (:422-430). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_NORMAL, 0, base_state());
	CHECK( !s.blend_enable && s.depth_mask && s.depth_test && s.alpha_test_enable );
	CHECK( s.sfactor == 0x1234u && s.dfactor == 0x5678u && s.alpha_ref == 0 );

	/* kRenderTransAlpha: depth mask off, then TransTexture's blend (:406-413). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_ALPHA, 0, base_state());
	CHECK( s.blend_enable && !s.depth_mask && s.depth_test );
	CHECK( s.sfactor == NV2A_SPRITE_SFACTOR_SRC_ALPHA && s.dfactor == NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA );

	/* kRenderTransTexture/Color keep the depth mask (:409-413). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_TEXTURE, 0, base_state());
	CHECK( s.blend_enable && s.depth_mask && s.depth_test );
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_COLOR, 0, base_state());
	CHECK( s.blend_enable && s.depth_mask && s.dfactor == NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA );

	/* kRenderGlow: no depth test, additive, depth mask off (:414-421). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_GLOW, 0, base_state());
	CHECK( s.blend_enable && !s.depth_mask && !s.depth_test && s.alpha_test_enable );
	CHECK( s.sfactor == NV2A_SPRITE_SFACTOR_SRC_ALPHA && s.dfactor == NV2A_SPRITE_DFACTOR_ONE );

	/* kRenderTransAdd: like Glow but depth-tested (:417-421). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_ADD, 0, base_state());
	CHECK( s.blend_enable && !s.depth_mask && s.depth_test && s.dfactor == NV2A_SPRITE_DFACTOR_ONE );

	/* Unknown mode takes the default branch (:423). */
	s = nv2a_sprite_begin_state( 42, 0, base_state());
	CHECK( !s.blend_enable && s.alpha_test_enable );

	/* 'lightmap' sprites test alpha GREATER 1/3 (:455). */
	s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_ALPHA, 1, base_state());
	CHECK( s.alpha_ref == NV2A_SPRITE_ALPHA_REF_LIGHTMAP && s.alpha_test_enable );
	/* 85 = floor(255/3): GREATER 85 passes a = 86 and up, a/255 > 1/3 likewise. */
	CHECK( NV2A_SPRITE_ALPHA_REF_LIGHTMAP == 85u );
}

static void test_lightmap_and_end( void )
{
	nv2a_sprite_state_t s = nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_TRANS_ALPHA, 1, base_state());

	/* :548-553 */
	s = nv2a_sprite_lightmap_state( 0, s );
	CHECK( s.blend_enable && s.depth_func_equal && !s.alpha_test_enable );
	CHECK( s.sfactor == NV2A_SPRITE_SFACTOR_ZERO && s.dfactor == NV2A_SPRITE_DFACTOR_SRC_COLOR );
	CHECK( !nv2a_sprite_lightmap_state( 1, s ).blend_enable ); /* r_lightmap shows it alone */

	/* :559-561 */
	s = nv2a_sprite_lightmap_end_state( s );
	CHECK( !s.blend_enable && !s.depth_func_equal && s.alpha_ref == NV2A_SPRITE_ALPHA_REF_DEFAULT );

	/* :567-578: TransAlpha restores the depth mask and turns blend off. */
	s = nv2a_sprite_end_state( NV2A_SPRITE_RENDER_TRANS_ALPHA, s );
	CHECK( !s.alpha_test_enable && s.depth_mask && !s.blend_enable && s.depth_test );

	/* Glow gets its depth test back. */
	s = nv2a_sprite_end_state( NV2A_SPRITE_RENDER_GLOW, nv2a_sprite_begin_state( NV2A_SPRITE_RENDER_GLOW, 0, base_state()));
	CHECK( s.depth_test && s.depth_mask && !s.blend_enable && !s.alpha_test_enable );

	/* kRenderNormal leaves blend and the depth test as they were (:573). */
	{
		nv2a_sprite_state_t in = base_state();
		in.blend_enable = 1;
		in.depth_test = 0;
		s = nv2a_sprite_end_state( NV2A_SPRITE_RENDER_NORMAL, in );
		CHECK( s.blend_enable && !s.depth_test && s.depth_mask );
	}
}

static void test_group_frames( void )
{
	/* Three frames of 0.1 s each: cumulative ends 0.1, 0.2, 0.3. */
	const float iv[3] = { 0.1f, 0.2f, 0.3f };
	int o, c;
	float lerp;

	/* t = 0.05: the loop breaks at i = 0 with j still numframes - 1 = 2,
	   jtime 0, jinterval = iv[1] - iv[0] = 0.1 -> lerp 0.5. */
	lerp = nv2a_sprite_group_frames( iv, 3, 0.05f, 1, &o, &c );
	CHECK( c == 0 && o == 2 );
	CHECK( fabsf( lerp - 0.5f ) < 1e-4f );

	/* t = 0.15: i = 1, j = 0, jtime 0.1, jinterval 0.1 -> lerp 0.5. */
	lerp = nv2a_sprite_group_frames( iv, 3, 0.15f, 1, &o, &c );
	CHECK( c == 1 && o == 0 && fabsf( lerp - 0.5f ) < 1e-4f );

	/* t = 0.25: the loop runs out (i = 2), j = 1, jtime 0.2. */
	lerp = nv2a_sprite_group_frames( iv, 3, 0.25f, 1, &o, &c );
	CHECK( c == 2 && o == 1 && fabsf( lerp - 0.5f ) < 1e-4f );

	/* Wraps: t = 0.95 is 0.05 into the fourth cycle (targettime, :112). */
	nv2a_sprite_group_frames( iv, 3, 0.95f, 1, &o, &c );
	CHECK( c == 0 );

	/* No interpolation: old = current, lerp 1 (:128). */
	lerp = nv2a_sprite_group_frames( iv, 3, 0.15f, 0, &o, &c );
	CHECK( c == 1 && o == 1 && lerp == 1.0f );

	/* One-frame group: both 0 (the caller draws it alone). */
	{
		const float one[1] = { 0.5f };
		nv2a_sprite_group_frames( one, 1, 0.3f, 1, &o, &c );
		CHECK( o == 0 && c == 0 );
	}
}

static void test_angle_frame( void )
{
	/* Facing the entity head on: view yaw = entity yaw + 180 ->
	   (180 + 45) / 45 = 5, rint 5, 5 - 4 = 1. */
	CHECK( nv2a_sprite_angle_frame( 180.0f, 0.0f ) == 1 );
	/* Same yaw: 45/45 = 1 -> 1 - 4 = -3 & 7 = 5. */
	CHECK( nv2a_sprite_angle_frame( 0.0f, 0.0f ) == 5 );
	/* 90 degrees: (90+45)/45 = 3 -> -1 & 7 = 7. */
	CHECK( nv2a_sprite_angle_frame( 90.0f, 0.0f ) == 7 );
	/* Negative argument rounds away from zero (Q_rint): (-100+45)/45 =
	   -1.222 -> -1 -> -5 & 7 = 3. */
	CHECK( nv2a_sprite_angle_frame( -100.0f, 0.0f ) == 3 );
	/* Always in 0..7. */
	{
		int k, ok = 1;
		for( k = -720; k <= 720; k += 7 )
		{
			int f = nv2a_sprite_angle_frame( (float)k, 33.0f );
			if( f < 0 || f > 7 ) ok = 0;
		}
		CHECK( ok );
	}
}

int main( void )
{
	test_begin_state();
	test_lightmap_and_end();
	test_group_frames();
	test_angle_frame();

	if( failures )
	{
		printf( "nv2a_sprite_util: %d check(s) failed\n", failures );
		return 1;
	}
	printf( "nv2a_sprite_util: all checks passed\n" );
	return 0;
}
