/* Host test for ref/nv2a/nv2a_shot_util.h (divergence #153): the
   savegame thumbnail's packed 2x2 average must give the bytes of the
   per-channel loop VID_ScreenShot ran before (r_context.c, #139):
   dst[k] = ( a[k] + b[k] + c[k] + d[k] + 2 ) >> 2 for B, G, R, then 255.
   Every lane sum from 0 to 1020 in every channel, with the other
   channels random, plus ten million random quads.

   gcc -O2 -Wall -std=c99 -Iref/nv2a tools/host-tests/nv2a_shot_util_host_test.c -o nv2a_shot_util_test */

#include <stdio.h>
#include <string.h>
#include "nv2a_shot_util.h"

static uint32_t rng = 0x9E3779B9u;
static uint32_t next_rand( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint32_t reference( uint32_t a, uint32_t b, uint32_t c, uint32_t d )
{
	uint8_t pa[4], pb[4], pc[4], pd[4], out[4];
	uint32_t r;
	int k;

	memcpy( pa, &a, 4 ); memcpy( pb, &b, 4 ); memcpy( pc, &c, 4 ); memcpy( pd, &d, 4 );
	for( k = 0; k < 3; k++ )
		out[k] = (uint8_t)(( pa[k] + pb[k] + pc[k] + pd[k] + 2 ) >> 2 );
	out[3] = 0xFF;
	memcpy( &r, out, 4 );
	return r;
}

int main( void )
{
	long checks = 0, fails = 0;
	int ch, s;
	long i;

	/* every sum of the four bytes of one channel: split s over the four */
	for( ch = 0; ch < 4; ch++ )
	for( s = 0; s <= 1020; s++ )
	for( i = 0; i < 64; i++ )
	{
		uint32_t q[4];
		int left = s, k;

		for( k = 0; k < 4; k++ )
		{
			int room = 255 * ( 3 - k ); /* what the later bytes can still hold */
			int lo = left - room > 0 ? left - room : 0;
			int hi = left < 255 ? left : 255;
			int v = k == 3 ? left : lo + (int)( next_rand() % (uint32_t)( hi - lo + 1 ));

			q[k] = ( next_rand() & ~( 0xFFu << ( ch * 8 ))) | ( (uint32_t)v << ( ch * 8 ));
			left -= v;
		}
		checks++;
		if( nv2a_shot_avg4( q[0], q[1], q[2], q[3] ) != reference( q[0], q[1], q[2], q[3] ))
		{
			if( fails++ < 5 )
				printf( "FAIL ch %d sum %d: %08x %08x %08x %08x\n", ch, s, q[0], q[1], q[2], q[3] );
		}
	}

	for( i = 0; i < 10000000; i++ )
	{
		uint32_t a = next_rand(), b = next_rand(), c = next_rand(), d = next_rand();

		checks++;
		if( nv2a_shot_avg4( a, b, c, d ) != reference( a, b, c, d ))
		{
			if( fails++ < 5 )
				printf( "FAIL random: %08x %08x %08x %08x\n", a, b, c, d );
		}
	}

	printf( "nv2a_shot_util: %ld checks, %ld failures\n", checks, fails );
	return fails ? 1 : 0;
}
