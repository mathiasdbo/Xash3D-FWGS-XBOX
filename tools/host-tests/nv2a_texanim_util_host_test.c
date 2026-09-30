/*
nv2a_texanim_util_host_test.c - host test for ref/nv2a/nv2a_texanim_util.c
(X9-X item 14 step 9, animated brush textures).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_texanim_util_host_test.c ref/nv2a/nv2a_texanim_util.c \
		-o .sweep/host-tests/nv2a_texanim_util_test && .sweep/host-tests/nv2a_texanim_util_test

The chains are built the way Mod_SequenceAnimatedTexture
(engine/common/mod_bmodel.c:3043) links them: frame k of an n-frame set
gets anim_total = n * ANIM_CYCLE (2), anim_min = k * 2, anim_max =
(k + 1) * 2, anim_next = frame (k + 1) % n, and every frame of each set
points at frame 0 of the other set through alternate_anims. Expected
frames are worked out by hand from R_TextureAnimation
(ref/gl/gl_rsurf.c:518): reletive = ticks % anim_total, then the frame
whose [anim_min, anim_max) holds it. The extra-frame cases are the same
as tools/asset-studio/tests/anim_texture_frames_test.cpp's, which tests
the baker's half of that rule.
*/
#include "nv2a_texanim_util.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

#define N NV2A_TEXANIM_NONE

static void link_set( nv2a_texanim_t *tab, const uint32_t *idx, int n, uint32_t alt0, int random )
{
	int k;
	for( k = 0; k < n; k++ )
	{
		nv2a_texanim_t *t = &tab[idx[k]];
		t->anim_total = (int16_t)( n * 2 );
		t->anim_min = (int16_t)( k * 2 );
		t->anim_max = (int16_t)(( k + 1 ) * 2 );
		t->anim_next = (uint16_t)idx[( k + 1 ) % n];
		t->alternate = (uint16_t)alt0;
		t->random = (uint8_t)random;
		t->width = 64;
		t->height = 64;
	}
}

static void blank( nv2a_texanim_t *tab, uint32_t count )
{
	uint32_t i;
	memset( tab, 0, sizeof( *tab ) * count );
	for( i = 0; i < count; i++ )
	{
		tab[i].anim_next = N;
		tab[i].alternate = N;
		tab[i].width = 64;
		tab[i].height = 64;
	}
}

static void test_ticks( void )
{
	CHECK( nv2a_texanim_ticks( 0.0 ) == 0 );
	CHECK( nv2a_texanim_ticks( 1.0 ) == 20 );      /* speed 20, gl_rsurf.c:546 */
	CHECK( nv2a_texanim_ticks( 0.099 ) == 1 );     /* 1.98 truncates */
	CHECK( nv2a_texanim_ticks( 3.26 ) == 65 );
	CHECK( nv2a_texanim_ticks( 1e12 ) == 2147483647 );
}

static void test_names( void )
{
	CHECK( nv2a_texanim_name_animated( "+0screen" ));
	CHECK( nv2a_texanim_name_animated( "-9x" ));
	CHECK( nv2a_texanim_name_animated( "+jx" ));
	CHECK( !nv2a_texanim_name_animated( "+kx" ));
	CHECK( !nv2a_texanim_name_animated( "0screen" ));
	CHECK( !nv2a_texanim_name_animated( "+" ));
	CHECK( !nv2a_texanim_name_animated( "" ));
	CHECK( !nv2a_texanim_name_animated( NULL ));
}

/* 0 = wall, 1..3 = +0..+2screen, 4..5 = +a..+bscreen */
static void test_time_frames( void )
{
	nv2a_texanim_t tab[6];
	const uint32_t frames[3] = { 1, 2, 3 }, alts[2] = { 4, 5 };

	blank( tab, 6 );
	link_set( tab, frames, 3, 4, 0 );
	link_set( tab, alts, 2, 1, 0 );

	/* not animated: the texture itself */
	CHECK( nv2a_texanim_resolve( tab, 6, 0, 0, 7, 0, 0, 0, NULL ) == 0 );
	CHECK( nv2a_texanim_resolve( tab, 6, 0, 1, 7, 0, 0, 0, NULL ) == 0 );

	/* anim_total 6: ticks 0,1 -> +0; 2,3 -> +1; 4,5 -> +2; 6 -> +0 */
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 0, 0, 0, 0, NULL ) == 1 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 1, 0, 0, 0, NULL ) == 1 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 2, 0, 0, 0, NULL ) == 2 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 5, 0, 0, 0, NULL ) == 3 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 6, 0, 0, 0, NULL ) == 1 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 65, 0, 0, 0, NULL ) == 3 ); /* 65 % 6 = 5 */
	/* starting from a later frame gives the same answer: the walk follows anim_next */
	CHECK( nv2a_texanim_resolve( tab, 6, 3, 0, 2, 0, 0, 0, NULL ) == 2 );

	/* entity frame non-zero: the alternate set, anim_total 4 */
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 1, 0, 0, 0, 0, NULL ) == 4 );
	CHECK( nv2a_texanim_resolve( tab, 6, 2, 1, 3, 0, 0, 0, NULL ) == 5 );
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 1, 4, 0, 0, 0, NULL ) == 4 );
	/* a surface drawn with an alternate texture and frame 0 animates its own set */
	CHECK( nv2a_texanim_resolve( tab, 6, 4, 0, 2, 0, 0, 0, NULL ) == 5 );
	/* ...and with frame non-zero switches to the +0 set */
	CHECK( nv2a_texanim_resolve( tab, 6, 4, 1, 2, 0, 0, 0, NULL ) == 2 );

	/* negative time: reletive < 0 matches no frame; the walk gives up after
	   MOD_FRAMES steps and returns the surface's own texture (gl_rsurf.c:557) */
	CHECK( nv2a_texanim_resolve( tab, 6, 2, 0, -1, 0, 0, 0, NULL ) == 2 );

	/* broken chain (anim_next NULL): the surface's own texture */
	tab[1].anim_next = N;
	CHECK( nv2a_texanim_resolve( tab, 6, 1, 0, 2, 0, 0, 0, NULL ) == 1 );

	/* out of range base: returned unchanged */
	CHECK( nv2a_texanim_resolve( tab, 6, 9, 0, 2, 0, 0, 0, NULL ) == 9 );
	CHECK( nv2a_texanim_resolve( NULL, 6, 1, 0, 2, 0, 0, 0, NULL ) == 1 );
}

/* A single +0 with a +a (a switchable light): anim_total 2, one frame each. */
static void test_switch( void )
{
	nv2a_texanim_t tab[2];
	const uint32_t on[1] = { 0 }, off[1] = { 1 };

	blank( tab, 2 );
	link_set( tab, on, 1, 1, 0 );
	link_set( tab, off, 1, 0, 0 );
	CHECK( nv2a_texanim_resolve( tab, 2, 0, 0, 12345, 0, 0, 0, NULL ) == 0 );
	CHECK( nv2a_texanim_resolve( tab, 2, 0, 1, 12345, 0, 0, 0, NULL ) == 1 );
	CHECK( nv2a_texanim_resolve( tab, 2, 1, 1, 12345, 0, 0, 0, NULL ) == 0 );
	CHECK( !nv2a_texanim_per_surface( tab, 2, 0, 0 ));

	/* an alternate with no +0 set at all: alternate_anims NULL, frame ignored */
	tab[0].alternate = N;
	CHECK( nv2a_texanim_resolve( tab, 2, 0, 1, 0, 0, 0, 0, NULL ) == 0 );
}

static void test_random_tiling( void )
{
	nv2a_texanim_t tab[4];
	const uint32_t tiles[4] = { 0, 1, 2, 3 };
	uint16_t rtable[NV2A_TEXANIM_MOD_FRAMES][NV2A_TEXANIM_MOD_FRAMES];
	int s, t;

	for( s = 0; s < NV2A_TEXANIM_MOD_FRAMES; s++ )
		for( t = 0; t < NV2A_TEXANIM_MOD_FRAMES; t++ )
			rtable[s][t] = (uint16_t)( s * 100 + t );

	blank( tab, 4 );
	link_set( tab, tiles, 4, N, 1 ); /* -0..-3, width = height = 64, anim_total 8 */

	CHECK( nv2a_texanim_per_surface( tab, 4, 0, 0 ));
	CHECK( nv2a_texanim_per_surface( tab, 4, 2, 1 )); /* no alternate: still the '-' set */

	/* texturemins (0, 0): tx = (0 + 64 << 16) / 64 % 20 = 65536 % 20 = 16,
	   ty = 16; rtable[16][16] = 1616; 1616 % 8 = 0 -> -0 */
	CHECK( nv2a_texanim_resolve( tab, 4, 0, 0, 0, 1, 0, 0, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 0 );
	/* texturemins (64, 128): tx = 65537 % 20 = 17, ty = 65538 % 20 = 18;
	   1718 % 8 = 6 -> anim_min 6: -3 */
	CHECK( nv2a_texanim_resolve( tab, 4, 0, 0, 0, 1, 64, 128, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 3 );
	/* negative texturemins: (-64 + 4194304) / 64 = 65535, % 20 = 15;
	   ty = 16: 1516 % 8 = 4 -> -2 */
	CHECK( nv2a_texanim_resolve( tab, 4, 0, 0, 0, 1, -64, 0, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 2 );
	/* time does not move a random tile */
	CHECK( nv2a_texanim_resolve( tab, 4, 0, 0, 999, 1, 64, 128, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 3 );
	/* chain-level (R_TextureAnim): '-' returns the texture itself */
	CHECK( nv2a_texanim_resolve( tab, 4, 1, 0, 0, 0, 64, 128, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 1 );
	/* zero width cannot be tiled */
	tab[0].width = 0;
	CHECK( nv2a_texanim_resolve( tab, 4, 0, 0, 0, 1, 64, 128, (const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable ) == 0 );
}

static int same( const uint16_t *got, uint32_t n, const uint16_t *want, uint32_t wn )
{
	return n == wn && ( n == 0 || memcmp( got, want, n * sizeof( *got )) == 0 );
}

static void test_extra_frames( void )
{
	uint16_t out[16];
	uint32_t n;

	{
		const char *names[] = { "wall", "+0screen", "+1screen", "+2screen", "+ascreen", "+0other", "+1other" };
		const uint8_t used[] = { 1, 1, 0, 0, 0, 0, 0 };
		const uint16_t want[] = { 2, 3, 4 };
		n = nv2a_texanim_extra_frames( names, used, 7, out, 16 );
		CHECK( same( out, n, want, 3 ));
	}
	{
		const char *names[] = { "+0~light", "+a~light" };
		const uint8_t used[] = { 0, 1 };
		const uint16_t want[] = { 0 };
		n = nv2a_texanim_extra_frames( names, used, 2, out, 16 );
		CHECK( same( out, n, want, 1 ));
	}
	{
		const char *names[] = { "-0out_w3", "-1out_w3", "-2out_w3" };
		const uint8_t used[] = { 1, 0, 1 };
		const uint16_t want[] = { 1 };
		n = nv2a_texanim_extra_frames( names, used, 3, out, 16 );
		CHECK( same( out, n, want, 1 ));
	}
	{
		const char *names[] = { "+0foo", "-1foo" };
		const uint8_t used[] = { 1, 0 };
		const uint16_t want[] = { 1 };
		n = nv2a_texanim_extra_frames( names, used, 2, out, 16 );
		CHECK( same( out, n, want, 1 ));
	}
	{
		const char *names[] = { "+kbad", "+0bad", "+", "", "-abad" };
		const uint8_t used[] = { 1, 0, 0, 0, 0 };
		const uint8_t used2[] = { 0, 0, 0, 0, 1 };
		const uint16_t want2[] = { 1 };
		CHECK( nv2a_texanim_extra_frames( names, used, 5, out, 16 ) == 0 );
		n = nv2a_texanim_extra_frames( names, used2, 5, out, 16 );
		CHECK( same( out, n, want2, 1 ));
	}
	{
		/* the engine's names are already cut to 15 characters */
		const char *names[] = { "+0abcdefghijklm", "+1abcdefghijklm" };
		const uint8_t used[] = { 1, 0 };
		const uint16_t want[] = { 1 };
		n = nv2a_texanim_extra_frames( names, used, 2, out, 16 );
		CHECK( same( out, n, want, 1 ));
	}
	{
		const char *names[] = { "+0a", "+1a", NULL };
		const uint8_t used[] = { 0, 0, 0 };
		CHECK( nv2a_texanim_extra_frames( names, used, 3, out, 16 ) == 0 );
	}
	{
		/* a full count is reported past out_cap, and nothing is written there */
		const char *names[] = { "+0s", "+1s", "+2s", "+3s" };
		const uint8_t used[] = { 1, 0, 0, 0 };
		out[1] = 0xBEEF;
		n = nv2a_texanim_extra_frames( names, used, 4, out, 1 );
		CHECK( n == 3 && out[0] == 1 && out[1] == 0xBEEF );
	}
}

int main( void )
{
	test_ticks();
	test_names();
	test_time_frames();
	test_switch();
	test_random_tiling();
	test_extra_frames();

	if( failures )
	{
		printf( "nv2a_texanim_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_texanim_util: all passed\n" );
	return 0;
}
