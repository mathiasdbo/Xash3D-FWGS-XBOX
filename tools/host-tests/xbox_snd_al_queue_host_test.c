/*
xbox_snd_al_queue_host_test.c - the OpenAL output's ring bookkeeping (#178)

engine/platform/xbox/xbox_snd_al_queue.h against a model of its two sides:
the mixer as S_UpdateChannels drives it (BeginPainting, S_GetSoundtime on
the Xbox's clock, a paint up to soundtime + mixahead, Submit) and a
streaming source that plays its queued buffers at a fixed rate and reports
each one processed once it has played all of it. Every painted frame
carries its own frame time, and every buffer is a copy, as alBufferData
makes one, so the test reads back what the source played.

Checked: the source plays every frame time exactly once and in order -
across ring wraps, game frames of every length, and stalls long enough to
starve it (the clock then stands still, so nothing painted is skipped) -
the clock never passes the painted time, the frames in flight stay within
the ring and match the queue, and a clock restart (the chop) keeps the
bookkeeping consistent.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_snd_al_queue.h"
#include "xbox_snd_clock.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

#define RING      32768
#define MIXAHEAD  5760 /* _snd_mixahead 0.12 at 48 kHz */
#define MIN_F     256
#define MAX_F     2048

typedef struct
{
	int *data; /* the buffer's copy of the ring */
	int len;
	int done;  /* frames played */
} buf_t;

static int ring[RING];
static buf_t src[XBOX_SND_AL_MAX_BUFFERS * 2];
static int src_head, src_count;
static int played_next; /* the frame time the source must play next */
static int played_total, starved;
static int processed;    /* buffers fully played, not yet collected */

static void src_play( int frames )
{
	while( frames > 0 && src_count > 0 )
	{
		buf_t *b = &src[src_head];

		while( frames > 0 && b->done < b->len )
		{
			CHECK( b->data[b->done] == played_next );
			if( b->data[b->done] != played_next )
				played_next = b->data[b->done];
			played_next++;
			played_total++;
			b->done++;
			frames--;
		}
		if( b->done == b->len )
		{
			free( b->data );
			src_head = ( src_head + 1 ) % ( XBOX_SND_AL_MAX_BUFFERS * 2 );
			src_count--;
			processed++;
		}
	}
	if( frames > 0 )
		starved++;
}

/* one S_UpdateChannels, `paint` 0 when the game stalls (no update) */
static xbox_snd_al_queue_t q;
static xbox_snd_clock_t clk;
static int paintedtime, soundtime;

/* The source side of a submit: an upload that fails every fail_every-th
   call (0: never), as alBufferData does when sample memory runs out. A
   failed one leaves no buffer queued and nothing taken from the free count. */
static int fail_every, submits, failed, free_bufs = XBOX_SND_AL_MAX_BUFFERS;

static int submit( void *user, int pos, int n )
{
	buf_t *b;

	(void)user;
	CHECK( pos + n <= RING );
	CHECK( n <= MAX_F );
	if( free_bufs <= 0 || ( fail_every && ++submits % fail_every == 0 ))
	{
		failed++;
		return 0;
	}
	b = &src[( src_head + src_count ) % ( XBOX_SND_AL_MAX_BUFFERS * 2 )];
	b->data = malloc( n * sizeof( int ));
	memcpy( b->data, ring + pos, n * sizeof( int ));
	b->len = n;
	b->done = 0;
	src_count++;
	free_bufs--;
	return 1;
}

static void update( void )
{
	int endtime, chop;

	/* BeginPainting: collect the played buffers */
	while( processed > 0 )
	{
		processed--;
		CHECK( Xbox_SndALQ_Retire( &q ) > 0 );
		free_bufs++;
	}
	soundtime = Xbox_SndClock_Time( &clk, q.played * 2, RING, paintedtime, &chop );
	CHECK( !chop );
	CHECK( soundtime <= paintedtime );

	/* the paint */
	endtime = soundtime + MIXAHEAD;
	endtime -= ( endtime - paintedtime ) & 3;
	for( ; paintedtime < endtime; paintedtime++ )
		ring[paintedtime % RING] = paintedtime;

	/* Submit: queue what was painted */
	Xbox_SndALQ_Fill( &q, paintedtime, MIN_F, MAX_F, submit, NULL );
	CHECK( q.count == src_count );
	CHECK( free_bufs + q.count == XBOX_SND_AL_MAX_BUFFERS );
	CHECK( Xbox_SndALQ_InFlight( &q ) <= RING );
	CHECK(( q.played + Xbox_SndALQ_InFlight( &q )) % RING == q.queued % RING );
	CHECK( q.queued <= paintedtime );
}

static void reset( void )
{
	while( src_count > 0 )
	{
		free( src[src_head].data );
		src_head = ( src_head + 1 ) % ( XBOX_SND_AL_MAX_BUFFERS * 2 );
		src_count--;
	}
	memset( &clk, 0, sizeof( clk ));
	paintedtime = soundtime = 0;
	played_next = played_total = starved = processed = 0;
	src_head = 0;
	fail_every = submits = failed = 0;
	free_bufs = XBOX_SND_AL_MAX_BUFFERS;
	Xbox_SndALQ_Init( &q, RING, 0 );
}

/* steady play: 30-144 fps game frames for about 40 s (60 ring wraps) */
static void test_steady( void )
{
	int f;

	reset();
	srand( 1 );
	update();
	for( f = 0; f < 3000; f++ )
	{
		src_play( 333 + rand() % 1300 );
		update();
	}
	CHECK( played_total > 30 * RING );
	CHECK( played_next == played_total );
	printf( "steady: %d frames played, starved %d times\n", played_total, starved );
}

/* stalls: the game stops updating for 0.2-2 s (a load, a hitch) */
static void test_stalls( void )
{
	int f, s;

	reset();
	srand( 2 );
	update();
	for( f = 0; f < 400; f++ )
	{
		src_play( 800 );
		update();
		if( f % 50 == 25 )
		{
			for( s = 0; s < 10 + rand() % 90; s++ )
				src_play( 960 ); /* no update: the source plays out and starves */
			CHECK( src_count == 0 );
			update();
		}
	}
	CHECK( starved > 0 );
	CHECK( played_next == played_total ); /* nothing skipped, nothing repeated */
	printf( "stalls: %d frames played, starved %d times\n", played_total, starved );
}

/* uploads that fail (sample memory exhausted): nothing is committed for a
   failed one, the free count is untouched, and the frames go out at a later
   update - still every frame once, in order */
static void test_upload_failures( void )
{
	xbox_snd_al_queue_t before;
	int f;

	reset();
	srand( 3 );
	fail_every = 3;
	update();
	for( f = 0; f < 1500; f++ )
	{
		src_play( 400 + rand() % 900 );
		update();
	}
	CHECK( failed > 100 );
	CHECK( played_total > 10 * RING );
	CHECK( played_next == played_total );

	/* one failing fill on its own: the queue is exactly as it was */
	fail_every = 1;
	before = q;
	paintedtime += 1024;
	Xbox_SndALQ_Fill( &q, paintedtime, MIN_F, MAX_F, submit, NULL );
	CHECK( q.count == before.count && q.queued == before.queued && q.played == before.played );
	CHECK( free_bufs + q.count == XBOX_SND_AL_MAX_BUFFERS );
	printf( "upload failures: %d failed, %d frames played\n", failed, played_total );
}

/* S_StopAllSounds -> S_ClearBuffer: the ring is zeroed (-1 here), the
   source stopped and its queue detached, the bookkeeping flushed. Nothing
   painted before the clear may play after it, the play resumes exactly at
   the painted time, and the clock keeps moving (no stall behind it) */
static void test_clear( void )
{
	int f, i, clears = 0, before;

	reset();
	srand( 4 );
	update();
	for( f = 0; f < 600; f++ )
	{
		src_play( 500 + rand() % 800 );
		update();
		if( f % 100 == 50 )
		{
			while( processed > 0 )
			{
				processed--;
				Xbox_SndALQ_Retire( &q );
				free_bufs++;
			}
			for( i = 0; i < RING; i++ )
				ring[i] = -1;
			while( src_count > 0 ) /* AL_BUFFER 0: every queued copy dropped */
			{
				free( src[src_head].data );
				src_head = ( src_head + 1 ) % ( XBOX_SND_AL_MAX_BUFFERS * 2 );
				src_count--;
			}
			free_bufs = XBOX_SND_AL_MAX_BUFFERS;
			before = played_total;
			CHECK( Xbox_SndALQ_Flush( &q ) >= 0 );
			CHECK( q.count == 0 && q.played == q.queued % RING );
			played_next = paintedtime; /* the first frame after must be this one */
			clears++;
			update();
			src_play( 2000 );
			CHECK( played_total > before ); /* the clock and the play go on */
		}
	}
	CHECK( clears == 6 );
	printf( "clear: %d clears, %d frames played\n", clears, played_total );
}

/* the painted time restarts (S_GetSoundtime's chop): the queue follows it */
static void test_chop( void )
{
	xbox_snd_al_queue_t t;
	int pos, before, n;

	/* 2^30 frames in, two buffers in flight */
	Xbox_SndALQ_Init( &t, RING, 0x40000000 - 3000 );
	n = Xbox_SndALQ_Next( &t, 0x40000000, MIN_F, MAX_F, &pos );
	Xbox_SndALQ_Commit( &t, n );
	n = Xbox_SndALQ_Next( &t, 0x40000000, MIN_F, MAX_F, &pos );
	Xbox_SndALQ_Commit( &t, n );
	before = Xbox_SndALQ_InFlight( &t );
	CHECK( before == 3000 );

	/* the chop: paintedtime = fullsamples */
	n = Xbox_SndALQ_Next( &t, RING, MIN_F, MAX_F, &pos );
	CHECK( n == 0 );
	CHECK( t.queued == RING );
	CHECK( Xbox_SndALQ_InFlight( &t ) == before );
	CHECK(( t.played + before ) % RING == t.queued % RING );
	n = Xbox_SndALQ_Next( &t, RING + 1000, MIN_F, MAX_F, &pos );
	CHECK( n == 1000 && pos == 0 );

	/* a jump forward of more than a ring is followed the same way */
	Xbox_SndALQ_Init( &t, RING, 0 );
	CHECK( Xbox_SndALQ_Next( &t, 3 * RING + 77, MIN_F, MAX_F, &pos ) == 0 );
	CHECK( t.queued == 3 * RING + 77 && t.played == 77 );
}

/* the pieces: less than the minimum waits, the ring's end splits a buffer */
static void test_pieces( void )
{
	xbox_snd_al_queue_t t;
	int pos, n;

	Xbox_SndALQ_Init( &t, RING, 0 );
	CHECK( Xbox_SndALQ_Next( &t, 100, MIN_F, MAX_F, &pos ) == 0 );
	n = Xbox_SndALQ_Next( &t, 5000, MIN_F, MAX_F, &pos );
	CHECK( n == MAX_F && pos == 0 );

	Xbox_SndALQ_Init( &t, RING, RING - 100 );
	n = Xbox_SndALQ_Next( &t, RING + 500, MIN_F, MAX_F, &pos );
	CHECK( n == 100 && pos == RING - 100 ); /* the last 100 go as they are */
	Xbox_SndALQ_Commit( &t, n );
	n = Xbox_SndALQ_Next( &t, RING + 500, MIN_F, MAX_F, &pos );
	CHECK( n == 500 && pos == 0 );

	/* every buffer in flight: none free */
	Xbox_SndALQ_Init( &t, RING, 0 );
	for( n = 0; n < XBOX_SND_AL_MAX_BUFFERS; n++ )
	{
		CHECK( Xbox_SndALQ_Next( &t, ( n + 1 ) * MIN_F, MIN_F, MIN_F, &pos ) == MIN_F );
		Xbox_SndALQ_Commit( &t, MIN_F );
	}
	CHECK( Xbox_SndALQ_Next( &t, RING, MIN_F, MAX_F, &pos ) == 0 );
	CHECK( Xbox_SndALQ_Retire( &t ) == MIN_F && t.played == MIN_F );
}

int main( void )
{
	test_pieces();
	test_chop();
	test_steady();
	test_stalls();
	test_upload_failures();
	test_clear();
	reset();
	printf( "snd_al_queue: %d checks, %d failures\n", checks, failures );
	return failures ? 1 : 0;
}
