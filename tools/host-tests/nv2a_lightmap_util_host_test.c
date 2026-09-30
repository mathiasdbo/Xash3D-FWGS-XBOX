/*
nv2a_lightmap_util_host_test.c - host test for ref/nv2a/nv2a_lightmap_util.c
(X9-X step 8, animated lightstyles and dynamic lights).

Build and run from the repository root (MinGW-64 shell):

	gcc -O1 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_lightmap_util_host_test.c ref/nv2a/nv2a_lightmap_util.c \
		-o .sweep/host-tests/nv2a_lightmap_util_test && .sweep/host-tests/nv2a_lightmap_util_test

Expected values are worked out by hand from ref/gl/gl_rsurf.c
(R_BuildLightMap, R_AddDynamicLights, R_CheckLightMap) and from what the
baker writes (LightmapAtlasPacker.h writeBlock: the sample bytes
verbatim), cited per check - not taken from the code under test.
*/
#include "nv2a_lightmap_util.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

static int values[256];      /* g_lightstylevalue */
static int nscale[256];      /* normalized, as r_lightmap.c builds it per frame */

static void set_style( int style, int value )
{
	values[style] = value;
	nscale[style] = nv2a_lm_normalized_scale( value );
}

static void test_normalized_scale( void )
{
	CHECK( nv2a_lm_normalized_scale( 264 ) == 256 );     /* 'm': the baked bytes, unchanged */
	CHECK( nv2a_lm_normalized_scale( 0 ) == 0 );         /* 'a' */
	CHECK( nv2a_lm_normalized_scale( -5 ) == 0 );
	CHECK( nv2a_lm_normalized_scale( 528 ) == 512 );     /* twice normal */
	CHECK( nv2a_lm_normalized_scale( 256 ) == 248 );     /* an empty style (ref_light.c:55): 65668/264 = 248.7 -> 248 */
	CHECK( nv2a_lm_normalized_scale( 550 ) == 533 );     /* 'z': (140800 + 132) / 264 = 533.8 -> 533 */
	CHECK( nv2a_lm_normalized_scale( 65536 ) == 63550 ); /* r_fullbright's 256 * 256, no overflow */
	/* clamped, not overflowed: (8388607 * 256 + 132) / 264 = 8134407 */
	CHECK( nv2a_lm_normalized_scale( NV2A_LM_MAX_STYLE_VALUE ) == 8134407 );
	CHECK( nv2a_lm_normalized_scale( 0x7FFFFFFF ) == 8134407 );
}

/* A face at the normal style value must come out as the baker wrote it:
   the LUMP_LIGHTING bytes verbatim (writeBlock), alpha 255. */
static void test_normal_style_reproduces_baked_bytes( void )
{
	enum { SMAX = 3, TMAX = 2, SIZE = SMAX * TMAX };
	const uint8_t styles[4] = { 0, 255, 255, 255 };
	uint8_t samples[SIZE * 3];
	uint32_t bl[SIZE * 3];
	uint32_t atlas[4 * 3]; /* stride 4 words, 3 rows: the last word of each row and row 2 must stay untouched */
	int i, s, t;

	for( i = 0; i < SIZE * 3; i++ )
		samples[i] = (uint8_t)( i * 37 + 1 ); /* covers 1..255, including 0xFF-ish values */
	samples[4] = 255;
	samples[5] = 0;

	set_style( 0, 264 );
	for( i = 0; i < 12; i++ )
		atlas[i] = 0xDEADBEEFu;

	nv2a_lm_accumulate_styles( bl, samples, SIZE, styles, nscale );
	nv2a_lm_pack_block( atlas, 4, bl, SMAX, TMAX );

	for( t = 0; t < TMAX; t++ )
	{
		for( s = 0; s < SMAX; s++ )
		{
			const uint8_t *src = &samples[( t * SMAX + s ) * 3];
			const uint32_t want = src[0] | ( src[1] << 8 ) | ( (uint32_t)src[2] << 16 ) | 0xFF000000u;

			CHECK( atlas[t * 4 + s] == want );
		}
		CHECK( atlas[t * 4 + 3] == 0xDEADBEEFu ); /* past smax: not written */
	}
	for( i = 8; i < 12; i++ )
		CHECK( atlas[i] == 0xDEADBEEFu );         /* past tmax: not written */
}

/* Several styles sum (gl_rsurf.c:744-759); the style list ends at the
   first 255 even if later slots hold data; slots are smax*tmax apart. */
static void test_styles_sum( void )
{
	enum { SIZE = 2 };
	const uint8_t styles[4] = { 0, 32, 255, 5 }; /* slot 3 after a 255: ignored */
	uint8_t samples[4 * SIZE * 3];
	uint32_t bl[SIZE * 3];
	uint32_t out[SIZE];
	int i;

	for( i = 0; i < (int)sizeof( samples ); i++ )
		samples[i] = 100;
	samples[0] = 10;          /* slot 0, luxel 0, red */
	samples[SIZE * 3] = 20;   /* slot 1, luxel 0, red */

	set_style( 0, 264 );
	set_style( 32, 0 );       /* a switchable light, off ('a') */
	set_style( 5, 264 );
	nv2a_lm_accumulate_styles( bl, samples, SIZE, styles, nscale );
	CHECK( bl[0] == 10u * 256u );   /* style 32 off adds nothing */
	CHECK( bl[1] == 100u * 256u );

	set_style( 32, 550 );     /* 'z' */
	nv2a_lm_accumulate_styles( bl, samples, SIZE, styles, nscale );
	CHECK( bl[0] == 10u * 256u + 20u * 533u );
	CHECK( bl[1] == 100u * 256u + 100u * 533u );
	nv2a_lm_pack_block( out, SIZE, bl, SIZE, 1 );
	CHECK(( out[0] & 0xFF ) == ( 10u * 256u + 20u * 533u ) >> 8 );   /* 51 */
	CHECK((( out[0] >> 8 ) & 0xFF ) == 255u );                        /* 100*256 + 100*533 = 78900 -> 308, clamped */

	/* samples NULL: nothing added (the reference's `&& surf->samples`), but still zeroed */
	bl[0] = 12345;
	nv2a_lm_accumulate_styles( bl, NULL, SIZE, styles, nscale );
	CHECK( bl[0] == 0 && bl[5] == 0 );

	/* The first style assigns: whatever the scratch held before is gone
	   (the reference's memset, folded in). */
	bl[0] = 0xFFFFFFF0u;
	bl[5] = 0xFFFFFFF0u;
	nv2a_lm_accumulate_styles( bl, samples, SIZE, styles, nscale );
	CHECK( bl[0] == 10u * 256u + 20u * 533u );
	CHECK( bl[5] == 100u * 256u + 100u * 533u );

	/* No style at all: zeroed too. */
	{
		const uint8_t none[4] = { 255, 255, 255, 255 };

		bl[0] = 7;
		bl[5] = 9;
		nv2a_lm_accumulate_styles( bl, samples, SIZE, none, nscale );
		CHECK( bl[0] == 0 && bl[5] == 0 );
	}
}

static void test_styles_changed_and_cached( void )
{
	const uint8_t one[4]  = { 0, 255, 255, 255 };
	const uint8_t two[4]  = { 0, 12, 255, 255 };
	const uint8_t tail[4] = { 0, 255, 12, 255 }; /* slot 2 is past the list's end */
	int cached[4] = { 0, 0, 0, 0 };

	set_style( 0, 264 );
	set_style( 12, 100 );

	/* Baked state: slot 0 normal, later slots 0 (the Xbox atlas holds slot 0 only). */
	nv2a_lm_baked_cached( two, cached );
	CHECK( cached[0] == 264 && cached[1] == 0 );
	CHECK( nv2a_lm_styles_changed( one, cached, values ) == 0 );
	CHECK( nv2a_lm_styles_changed( two, cached, values ) == 1 );  /* style 12 is at 100, the block was built at 0 */
	CHECK( nv2a_lm_styles_changed( tail, cached, values ) == 0 ); /* never looks past the first 255 */

	nv2a_lm_update_cached( two, cached, values );
	CHECK( cached[0] == 264 && cached[1] == 100 );
	CHECK( nv2a_lm_styles_changed( two, cached, values ) == 0 );

	set_style( 0, 256 );
	CHECK( nv2a_lm_styles_changed( one, cached, values ) == 1 );

	/* update leaves slots past the list alone, as R_UpdateSurfaceCachedLight does */
	cached[2] = 777;
	nv2a_lm_update_cached( two, cached, values );
	CHECK( cached[2] == 777 );
}

/* R_AddDynamicLights, gl_rsurf.c:607-660, sample_size 16, sample_frac 1. */
static void test_dlight( void )
{
	enum { SMAX = 20, TMAX = 3 };
	uint32_t bl[SMAX * TMAX * 3];

	/* Too dim to reach the plane: nothing touched. */
	memset( bl, 0, sizeof( bl ));
	CHECK( nv2a_lm_add_dlight( bl, SMAX, TMAX, 16.0f, 1, 0.0f, 0.0f, 10.0f, 20.0f, 255, 255, 255 ) == 0 );
	CHECK( bl[0] == 0 );

	/* Light over luxel (0,0), rad 100, minlight 0, white. */
	memset( bl, 0, sizeof( bl ));
	CHECK( nv2a_lm_add_dlight( bl, SMAX, TMAX, 16.0f, 1, 0.0f, 0.0f, 100.0f, 0.0f, 255, 128, 0 ) == 1 );
	/* (0,0): dist 0 -> (100 * 256) * 255 / 256 = 25500 red, 25600*128/256 = 12800 green, 0 blue */
	CHECK( bl[0] == 25500u && bl[1] == 12800u && bl[2] == 0u );
	/* (1,0): sd 16, td 0 -> dist 16 -> 84 * 256 = 21504; * 255 / 256 = 21420 */
	CHECK( bl[3] == 21420u );
	/* (1,1): sd 16, td 16 -> dist 16 + 8 = 24 -> 76 * 256 = 19456 -> 19380 */
	CHECK( bl[( 1 + SMAX ) * 3] == 19380u );
	/* (6,0): sd 96 -> dist 96 < 100 -> 4 * 256 = 1024 -> 1020. half = 101/16 = 6.31 so s1 = 6. */
	CHECK( bl[6 * 3] == 1020u );
	/* (7,0): outside the scanned range and dist 112 >= 100 anyway */
	CHECK( bl[7 * 3] == 0u );

	/* minlight 20 shrinks the radius the falloff covers to rad - 20 = 80:
	   (5,0) is at dist 80, not < 80, so it is not lit; (4,0) at 64 is. */
	memset( bl, 0, sizeof( bl ));
	nv2a_lm_add_dlight( bl, SMAX, TMAX, 16.0f, 1, 0.0f, 0.0f, 100.0f, 20.0f, 255, 255, 255 );
	CHECK( bl[5 * 3] == 0u );
	CHECK( bl[4 * 3] == (uint32_t)(( 36 * 256 ) * 255 / 256 ));

	/* The scan window is clamped to the block: a light past the far
	   corner must not write outside smax x tmax. */
	{
		uint32_t guard[SMAX * TMAX * 3 + 3];
		memset( guard, 0, sizeof( guard ));
		nv2a_lm_add_dlight( guard, SMAX, TMAX, 16.0f, 1, 16.0f * ( SMAX - 1 ), 16.0f * ( TMAX - 1 ), 300.0f, 0.0f, 255, 255, 255 );
		CHECK( guard[SMAX * TMAX * 3] == 0u && guard[SMAX * TMAX * 3 + 2] == 0u );
		CHECK( guard[( SMAX * TMAX - 1 ) * 3] == (uint32_t)( 300 * 256 * 255 / 256 ));
	}
}

/* The texel: bl >> 8, clamped to 255 (bl * 256 >> 14 capped at 1023, >> 2). */
static void test_pack_clamp( void )
{
	const uint32_t bl[3] = { 255u << 8, 256u << 8, 0xFFFFFFFFu };
	uint32_t out = 0;

	nv2a_lm_pack_block( &out, 1, bl, 1, 1 );
	CHECK( out == ( 255u | ( 255u << 8 ) | ( 255u << 16 ) | 0xFF000000u ));
	CHECK( nv2a_lm_texel( 127u << 8 ) == 127u );
	CHECK( nv2a_lm_texel(( 127u << 8 ) + 255u ) == 127u );
}

int main( void )
{
	test_normalized_scale();
	test_normal_style_reproduces_baked_bytes();
	test_styles_sum();
	test_styles_changed_and_cached();
	test_dlight();
	test_pack_clamp();

	if( failures )
	{
		printf( "%d failure(s)\n", failures );
		return 1;
	}
	printf( "nv2a_lightmap_util: all tests passed\n" );
	return 0;
}
