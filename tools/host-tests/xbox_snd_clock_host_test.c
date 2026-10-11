/*
xbox_snd_clock_host_test.c - the mixer's DMA clock across a device reopened at run time (divergence #173)
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
engine/platform/xbox/xbox_snd_clock.h against:
- upstream's S_GetSoundtime (s_main.c:1509-1536 in the reference, copied
  below): the same times and the same chops over random device walks;
- a simulated device: a 32768-frame ring whose position (snd.samplepos,
  two units a frame) advances by SDL's 1024-frame blocks and wraps, the
  way SDL_SoundCallback moves it. r3d_snd_rate's reopen is simulated at
  many logical times - just before, at and just after a ring wrap, and
  far into the session - with the new device starting at position 0 and
  perhaps a block or two played before the rebase: after it the clock
  reads the time played (converted to the new rate, the same instant in
  seconds), advances exactly with the device, and stays congruent with
  the device's position modulo the ring (the mixer writes frame t at
  t % fullsamples, the device plays samplepos / 2). The same reopen
  without the rebase - upstream's statics - is checked to jump, so the
  test sees the bug it guards against.

  gcc -O2 -Wall -std=c99 -Iengine/platform/xbox tools/host-tests/xbox_snd_clock_host_test.c -o snd_clock_test && ./snd_clock_test
*/

#include <stdio.h>
#include <stdlib.h>
#include "xbox_snd_clock.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

#define FULL  32768          /* frames: s_samplecount 0's 0x8000 */
#define UNITS ( FULL * 2 )   /* snd.samples, samplepos's modulus */
#define BLOCK 1024           /* SDL's callback, frames */

/* upstream's S_GetSoundtime, its statics as a struct */
typedef struct { int buffers, oldsamplepos; } up_clock_t;
static int up_time( up_clock_t *c, int samplepos, int fullsamples, int *paintedtime, int *chopped )
{
	*chopped = 0;
	if( samplepos < c->oldsamplepos )
	{
		c->buffers++;
		if( *paintedtime > 0x40000000 )
		{
			c->buffers = 0;
			*paintedtime = fullsamples;
			*chopped = 1;
		}
	}
	c->oldsamplepos = samplepos;
	return c->buffers * fullsamples + samplepos / 2;
}

static unsigned rng = 7;
static unsigned rnd( void ) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* the device: advances `frames` (at most one ring, as the engine polls at
   least that often) */
static int dev_advance( int pos, int frames )
{
	return ( pos + frames * 2 ) % UNITS;
}

static void test_matches_upstream( void )
{
	xbox_snd_clock_t x = { 0, 0 };
	up_clock_t u = { 0, 0 };
	int pos = 0, painted_u = 0, painted_x = 0, i, chops = 0;

	for( i = 0; i < 2000000; i++ )
	{
		int cu, cx, tu, tx;

		pos = dev_advance( pos, (int)( rnd() % ( FULL / 2 )));
		/* a paintedtime that reaches the chop threshold now and then */
		if( i % 100000 == 99999 )
			painted_u = painted_x = 0x40000000 + 5;
		tu = up_time( &u, pos, FULL, &painted_u, &cu );
		tx = Xbox_SndClock_Time( &x, pos, FULL, painted_x, &cx );
		if( cx )
			painted_x = FULL; /* the caller's part of the chop */
		CHECK( tu == tx && cu == cx && painted_u == painted_x );
		chops += cx;
		if( !cu && painted_u < 0x40000000 )
			painted_u = painted_x = tu; /* the mixer keeps up */
		if( failures > 20 )
			return;
	}
	CHECK( chops > 5 );
}

/* One run: play to logical time `t0` (frames) at `old_rate`, reopen at
   `new_rate` (the new device starts at 0 and plays `early` blocks before
   the rebase), then play on. Returns the jump the clock made across the
   reopen (0 when correct); `rebase` 0 runs upstream's behaviour. */
static int run_switch( int t0, int old_rate, int new_rate, int early, int rebase )
{
	xbox_snd_clock_t c = { 0, 0 };
	int pos = 0, t = 0, chop, played, expect, jump, k;

	/* play to t0 in blocks (and a remainder), polling every block */
	while( t < t0 )
	{
		const int step = t0 - t < BLOCK ? t0 - t : BLOCK;

		pos = dev_advance( pos, step );
		t = Xbox_SndClock_Time( &c, pos, FULL, t, &chop );
		CHECK( !chop );
	}
	CHECK( t == t0 && ( t % FULL ) * 2 == pos );

	/* the switch: read the time played under the lock, close, reopen */
	played = Xbox_SndClock_Time( &c, pos, FULL, t, &chop );
	pos = 0;
	for( k = 0; k < early; k++ )
		pos = dev_advance( pos, BLOCK );
	if( rebase )
	{
		played = Xbox_SndClock_Scale( played, old_rate, new_rate );
		pos = Xbox_SndClock_Rebase( &c, played, FULL );
		expect = played;
	}
	else expect = played; /* what a correct clock would read */
	t = Xbox_SndClock_Time( &c, pos, FULL, played, &chop );
	jump = t - expect;

	if( rebase )
	{
		/* seconds kept: the same instant, within one frame at the new rate */
		CHECK( (double)played / new_rate <= (double)t0 / old_rate + 1e-12 );
		CHECK( (double)t0 / old_rate - (double)played / new_rate < 1.0 / new_rate );
		/* congruent with the device, and it advances exactly with it,
		   across wraps */
		CHECK( ( t % FULL ) * 2 == pos );
		for( k = 0; k < 3 * FULL / BLOCK + 7; k++ )
		{
			const int before = t;

			pos = dev_advance( pos, BLOCK );
			t = Xbox_SndClock_Time( &c, pos, FULL, t, &chop );
			CHECK( t - before == BLOCK && ( t % FULL ) * 2 == pos && !chop );
		}
	}
	return jump;
}

static void test_switches( void )
{
	static const int bases[] = { 0, 1, 1023, 1024, FULL - BLOCK, FULL - 10, FULL - 1, FULL, FULL + 1, FULL + 5,
		2 * FULL - 1, 2 * FULL, 2 * FULL + 3, 123457, 1000003, 44100 * 600, 48000 * 3600 };
	static const int rates[][2] = { { 44100, 48000 }, { 48000, 44100 }, { 48000, 48000 } };
	int b, r, e, upstream_jumps = 0, runs = 0;

	for( b = 0; b < (int)( sizeof( bases ) / sizeof( bases[0] )); b++ )
		for( r = 0; r < 3; r++ )
			for( e = 0; e < 3; e++ )
			{
				CHECK( run_switch( bases[b], rates[r][0], rates[r][1], e, 1 ) == 0 );
				/* upstream's statics across the same reopen (rates equal, so
				   a correct clock would read the time played) */
				if( run_switch( bases[b], 48000, 48000, e, 0 ) != 0 )
					upstream_jumps++;
				runs++;
			}
	/* upstream's clock jumps at every switch the new device's position
	   lands below the old one's */
	CHECK( upstream_jumps > runs / 2 );
	printf( "snd_clock: %d switches rebased with no jump; upstream's statics jumped on %d of them\n", runs, upstream_jumps );
}

int main( void )
{
	test_matches_upstream();
	test_switches();
	printf( "snd_clock: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
