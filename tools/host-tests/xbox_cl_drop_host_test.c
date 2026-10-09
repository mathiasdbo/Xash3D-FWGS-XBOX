/* Host test for engine/platform/xbox/xbox_cl_drop.h (docs/r3d/divergences.md
   #161): CL_ParseClientData's dropped-frame loop started at
   Xbox_CL_FirstDropToMark against upstream's loop from
   last_incoming_sequence + 1 (engine/client/parse/cl_parse.c, :1001-1012
   in the reference), on the frame fields it writes.

   For CL_UPDATE_BACKUP 16 (single player) and 64 (multiplayer), over the
   array cl.frames really is (MULTIPLAYER_BACKUP entries, of which the
   mask reaches the first `backup`), from random slot states (received,
   dropped, choked at -2, and the not-yet-received 0.0):
   1. every gap from 0 to 3 x backup, from random starting sequences;
   2. random gaps up to 2^20;
   3. the gaps the bug was about: last_incoming_sequence 0 against a
      first sequence near the netchan's 2^27 ceiling (a handful: each
      costs upstream's loop its full 134 million passes).
   The two runs must leave every slot's receivedtime and latency
   bit-identical, and the clamped run must visit at most `backup`
   sequences.
   4. (Codex review) the int range's edges, which a demo's sequence
      numbers can reach (cl_demo.c): every pair from a set around
      INT_MIN, -1/0/1 and INT_MAX, and short gaps across zero and up to
      INT_MAX. The helper's return is checked against a 64-bit model,
      and the loop's slots against the defined expected state - upstream's
      loop run in 64 bits over the gap when it is short, every slot it
      would reach (the last min(gap, backup) sequences) otherwise. The
      run.sh build traps on signed overflow, so an overflow in the helper
      fails the test.

   Built and run by tools/host-tests/run.sh:
     gcc -O2 -Wall -std=c99 -Iengine/platform/xbox \
       tools/host-tests/xbox_cl_drop_host_test.c -o xbox_cl_drop_test */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_cl_drop.h"

#define MULTIPLAYER_BACKUP 64 /* engine/common/netchan.h */

typedef struct
{
	double receivedtime;
	float latency;
} frame_t;

static int failures;
static long checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { if( failures++ < 20 ) { fprintf( stderr, "%s:%d: ", __FILE__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); fputc( '\n', stderr ); } } } while( 0 )

static uint32_t rng = 0x12345678u;

static uint32_t rnd( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void random_frames( frame_t *f )
{
	int k;

	for( k = 0; k < MULTIPLAYER_BACKUP; k++ )
	{
		switch( rnd() % 4 )
		{
		case 0: f[k].receivedtime = -1.0; break;
		case 1: f[k].receivedtime = -2.0; break; /* choked, cl_parse.c */
		case 2: f[k].receivedtime = 0.0; break;
		default: f[k].receivedtime = ( rnd() % 100000 ) / 7.0; break;
		}
		f[k].latency = ( rnd() % 1000 ) / 3.0f;
	}
}

/* upstream's loop, as cl_parse.c has it */
static void upstream_loop( frame_t *f, int mask, int last, int i )
{
	int j;

	if( i > last + 1 )
	{
		for( j = last + 1; j < i; j++ )
		{
			if( f[j & mask].receivedtime >= 0.0 )
			{
				f[j & mask].receivedtime = -1.0f;
				f[j & mask].latency = 0;
			}
		}
	}
}

/* the Xbox loop; returns the passes it made */
static long clamped_loop( frame_t *f, int backup, int last, int i )
{
	const int mask = backup - 1;
	long passes = 0;
	int j;

	if( Xbox_CL_FramesDropped( last, i ))
	{
		for( j = Xbox_CL_FirstDropToMark( last, i, backup ); j < i; j++ )
		{
			passes++;
			if( f[j & mask].receivedtime >= 0.0 )
			{
				f[j & mask].receivedtime = -1.0f;
				f[j & mask].latency = 0;
			}
		}
	}
	return passes;
}

static void compare( int backup, int last, int i )
{
	frame_t a[MULTIPLAYER_BACKUP], b[MULTIPLAYER_BACKUP];
	long passes;

	random_frames( a );
	memcpy( b, a, sizeof( a ));
	upstream_loop( a, backup - 1, last, i );
	passes = clamped_loop( b, backup, last, i );
	CHECK( !memcmp( a, b, sizeof( a )), "backup %d last %d incoming %d: frames differ", backup, last, i );
	CHECK( passes <= backup, "backup %d last %d incoming %d: %ld passes", backup, last, i, passes );
	CHECK( i <= last + 1 || passes == ( i - last - 1 < backup ? i - last - 1 : backup ),
		"backup %d last %d incoming %d: %ld passes", backup, last, i, passes );
}

/* The defined result of upstream's loop for incoming > last + 1 (in 64
   bits, so the model itself cannot overflow): every slot of the last
   min(gap, backup) sequences below incoming marked. */
static void expected_loop( frame_t *f, int backup, int64_t last, int64_t i )
{
	const int64_t gap = i - last - 1;
	const int64_t from = gap > backup ? i - backup : last + 1;
	int64_t j;

	for( j = from; j < i; j++ )
	{
		frame_t *slot = &f[(int)( j & ( backup - 1 ))];

		if( slot->receivedtime >= 0.0 )
		{
			slot->receivedtime = -1.0f;
			slot->latency = 0;
		}
	}
}

/* upstream's loop literally, its j widened so the model is defined
   across INT_MAX; only for short gaps */
static void upstream_loop64( frame_t *f, int backup, int64_t last, int64_t i )
{
	int64_t j;

	for( j = last + 1; j < i; j++ )
	{
		frame_t *slot = &f[(int)( j & ( backup - 1 ))];

		if( slot->receivedtime >= 0.0 )
		{
			slot->receivedtime = -1.0f;
			slot->latency = 0;
		}
	}
}

static void compare_edge( int backup, int last, int i )
{
	frame_t a[MULTIPLAYER_BACKUP], b[MULTIPLAYER_BACKUP], c[MULTIPLAYER_BACKUP];
	const int64_t gap = (int64_t)i - last - 1;
	const int start = Xbox_CL_FirstDropToMark( last, i, backup );
	int64_t want;
	long passes;

	/* the helper against its 64-bit model */
	if( gap > backup )
		want = (int64_t)i - backup;
	else if( gap >= 0 )
		want = (int64_t)last + 1;
	else
		want = i;
	CHECK( start == want, "backup %d last %d incoming %d: start %d, want %lld", backup, last, i, start, (long long)want );

	/* the guard against its 64-bit model, then the whole guard-plus-loop
	   path for a pair with no gap (INT_MAX included): nothing changes */
	CHECK( Xbox_CL_FramesDropped( last, i ) == ( gap > 0 ), "guard: last %d incoming %d", last, i );
	if( gap <= 0 )
	{
		random_frames( a );
		memcpy( b, a, sizeof( a ));
		passes = clamped_loop( b, backup, last, i );
		CHECK( passes == 0 && !memcmp( a, b, sizeof( a )), "edge: backup %d last %d incoming %d: no-gap path changed frames", backup, last, i );
		return; /* the caller's guard: no loop */
	}

	random_frames( a );
	memcpy( b, a, sizeof( a ));
	memcpy( c, a, sizeof( a ));
	expected_loop( a, backup, last, i );
	passes = clamped_loop( b, backup, last, i );
	CHECK( !memcmp( a, b, sizeof( a )), "edge: backup %d last %d incoming %d: frames differ", backup, last, i );
	CHECK( passes == ( gap < backup ? gap : backup ), "edge: backup %d last %d incoming %d: %ld passes", backup, last, i, passes );
	if( gap <= ( 1 << 20 ))
	{
		upstream_loop64( c, backup, last, i );
		CHECK( !memcmp( a, c, sizeof( a )), "edge: backup %d last %d incoming %d: model differs from upstream", backup, last, i );
	}
}

int main( void )
{
	static const int edges[] =
	{
		INT32_MIN, INT32_MIN + 1, INT32_MIN + 2, INT32_MIN + 17, INT32_MIN + 65, INT32_MIN + 1000,
		-100000, -65, -64, -17, -16, -3, -2, -1, 0, 1, 2, 15, 16, 17, 64, 65, 100000,
		( 1 << 27 ) - 1, ( 1 << 30 ),
		INT32_MAX - 1000, INT32_MAX - 65, INT32_MAX - 17, INT32_MAX - 2, INT32_MAX - 1, INT32_MAX,
	};
	const int nedges = (int)( sizeof( edges ) / sizeof( edges[0] ));
	static const int backups[] = { 16, MULTIPLAYER_BACKUP };
	int b, gap, n;

	for( b = 0; b < 2; b++ )
	{
		const int backup = backups[b];

		/* 1. every short gap, including none and a duplicate */
		for( gap = -2; gap <= 3 * backup; gap++ )
		{
			for( n = 0; n < 40; n++ )
			{
				const int last = (int)( rnd() % ( 1u << 27 ));

				compare( backup, last, last + 1 + gap );
			}
		}

		/* 2. random gaps */
		for( n = 0; n < 4000; n++ )
		{
			const int last = (int)( rnd() % ( 1u << 27 ));

			compare( backup, last, last + 1 + (int)( rnd() % ( 1u << 20 )));
		}

		/* 3. the first clientdata of a map: last 0, the server's random
		   initial sequence up to 2^27 - 1, plus the packets since */
		compare( backup, 0, ( 1 << 27 ) - 1 );
		compare( backup, 0, ( 1 << 27 ) + 5000 );
		compare( backup, 0, (int)( rnd() % ( 1u << 27 )) + 1 );

		/* 4. the int range's edges (Codex review) */
		for( n = 0; n < nedges; n++ )
		{
			int m;

			for( m = 0; m < nedges; m++ )
				compare_edge( backup, edges[n], edges[m] );
			/* short gaps from each edge, where they stay inside int */
			for( gap = -2; gap <= 3 * backup; gap++ )
			{
				const int64_t i = (int64_t)edges[n] + 1 + gap;

				if( i >= INT32_MIN && i <= INT32_MAX )
					compare_edge( backup, edges[n], (int)i );
			}
		}
		/* across zero, and ending exactly at INT_MAX */
		for( gap = 0; gap <= 3 * backup; gap++ )
		{
			compare_edge( backup, -gap / 2 - 1, gap - gap / 2 );
			compare_edge( backup, INT32_MAX - 1 - gap, INT32_MAX );
		}
	}

	if( failures )
	{
		fprintf( stderr, "xbox_cl_drop: %d of %ld checks failed\n", failures, checks );
		return 1;
	}
	printf( "xbox_cl_drop: %ld checks passed\n", checks );
	return 0;
}
