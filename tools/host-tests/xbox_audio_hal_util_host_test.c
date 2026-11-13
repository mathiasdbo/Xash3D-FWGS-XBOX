/* Host test for engine/platform/xbox/xbox_audio_hal_util.c (divergence
   #150): the AC97 buffer accounting and the stall watchdog's decision.

   Besides unit cases, it simulates the whole pipeline the console runs -
   nxdk's SDL Xbox audio driver (two buffers, the first not queued, one
   semaphore post per freed buffer, Play then Wait), the two AC97 DMA
   engines (PCM out and S/PDIF, which play the same buffers) - under the
   two controller behaviours an underrun can have:

     HALT - the AC97/ICH specification's (and QEMU's, so xemu's): at the
            end of the last valid buffer the engine raises LVBCI, sets DCH
            and stops with PICB 0; a later LVI write restarts it at the
            next descriptor.
     LOOP - what nxdk's ISR guard (lib/hal/audio.c, `analogDrained`,
            commit 33e5ebb) assumes: the engine replays the last buffer,
            raising BCIS|LVBCI after each pass, until LVI moves.

   and with two accountings: nxdk's original ISR (buffer counters plus the
   drained flag, transcribed from lib/hal/audio.c) and the replacement's
   (xbox_aci_account, as aci_dpc_routine runs it). The audio
   thread is made late - at startup and once mid-run - which is an
   underrun. Under HALT the original accounting deadlocks for good (the
   bug); the replacement must keep playing under both behaviours, and must
   never let the thread refill a buffer an engine has not finished.

   gcc -O2 -Wall -std=c99 -Iengine/platform/xbox \
       tools/host-tests/xbox_audio_hal_util_host_test.c \
       engine/platform/xbox/xbox_audio_hal_util.c -o xbox_audio_hal_util_test */

#include <stdio.h>
#include <string.h>
#include "xbox_audio_hal_util.h"

static int failures;

#define CHECK( cond ) do { if( !( cond )) { fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

/* ---- unit cases -------------------------------------------------------- */

static void test_finished( void )
{
	// nothing in flight
	CHECK( xbox_aci_finished( 5, 5, 3, 4, XBOX_ACI_FORCE ) == 0 );
	// two in flight (seq 5 at desc 5, seq 6 at desc 6), engine on desc 5
	CHECK( xbox_aci_finished( 7, 5, 5, 6, 0 ) == 0 );
	CHECK( xbox_aci_finished( 7, 5, 5, 6, XBOX_ACI_HALTED_EMPTY | XBOX_ACI_LAST_DONE ) == 0 ); // CIV is not the last
	// moved on to desc 6: seq 5 done
	CHECK( xbox_aci_finished( 7, 5, 6, 6, 0 ) == 1 );
	// ...and seq 6 halted empty: both done
	CHECK( xbox_aci_finished( 7, 5, 6, 6, XBOX_ACI_HALTED_EMPTY ) == 2 );
	// LVBCI latched, but CIV moved since: the LVBCI was seq 5's (HALT, LVI
	// written between the drain and the ISR) - seq 6 has just started
	CHECK( xbox_aci_finished( 7, 5, 6, 6, XBOX_ACI_LAST_DONE ) == 1 );
	// LVBCI with CIV still on the oldest, last buffer: done
	CHECK( xbox_aci_finished( 6, 5, 5, 5, XBOX_ACI_LAST_DONE ) == 1 );
	CHECK( xbox_aci_finished( 6, 5, 5, 5, 0 ) == 0 );
	CHECK( xbox_aci_finished( 6, 5, 5, 5, XBOX_ACI_FORCE ) == 1 );
	// replaying an already retired buffer (LOOP): CIV behind - nothing,
	// whatever the evidence
	CHECK( xbox_aci_finished( 7, 6, 5, 6, XBOX_ACI_LAST_DONE | XBOX_ACI_HALTED_EMPTY | XBOX_ACI_FORCE ) == 0 );
	CHECK( xbox_aci_finished( 6, 6, 5, 5, XBOX_ACI_LAST_DONE ) == 0 );
	// CIV one past everything handed over: all done, never more
	CHECK( xbox_aci_finished( 7, 5, 7, 6, XBOX_ACI_FORCE ) == 2 );
	CHECK( xbox_aci_finished( 7, 5, 8, 6, XBOX_ACI_FORCE ) == 0 );
	// the descriptor ring wraps at 32, the counts at 2^32
	CHECK( xbox_aci_finished( 33, 31, 0, 0, 0 ) == 1 );
	CHECK( xbox_aci_finished( 33, 31, 0, 0, XBOX_ACI_HALTED_EMPTY ) == 2 );
	CHECK( xbox_aci_finished( 1u, 0xFFFFFFFFu, 0, 0, XBOX_ACI_HALTED_EMPTY ) == 2 );
	// a full ring is ambiguous: nothing
	CHECK( xbox_aci_finished( 32, 0, 5, 31, XBOX_ACI_FORCE ) == 0 );

	CHECK( xbox_aci_both_finished( 10, 12, 9 ) == 1 );
	CHECK( xbox_aci_both_finished( 12, 10, 9 ) == 1 );
	CHECK( xbox_aci_both_finished( 9, 12, 9 ) == 0 );
	CHECK( xbox_aci_both_finished( 1u, 2u, 0xFFFFFFFFu ) == 2 );
}

static void test_account( void )
{
	xbox_aci_books_t b;
	uint32_t civ[2], lvi[2], ev[2] = { 0, 0 };

	// seq 5 and 6 in flight on both engines; PCM out moved on to 6, S/PDIF
	// still on 5: nothing both have finished
	memset( &b, 0, sizeof( b ));
	b.retired[0] = b.retired[1] = b.reported = 5;
	civ[0] = 6; lvi[0] = 6; civ[1] = 5; lvi[1] = 6;
	CHECK( xbox_aci_account( &b, 7, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 0 );
	CHECK( b.retired[0] == 6 && b.retired[1] == 5 && b.reported == 5 );
	// S/PDIF catches up
	civ[1] = 6;
	CHECK( xbox_aci_account( &b, 7, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 1 );
	CHECK( b.reported == 6 );
	// KICK_LAST: seq 6, the last, counts as played on both
	CHECK( xbox_aci_account( &b, 7, civ, lvi, ev, XBOX_ACI_KICK_LAST ) == 1 );
	CHECK( b.reported == 7 && b.rescues == 0 );
	// nothing left: KICK_LAST reports nothing, KICK_ALL rescues once,
	// outside the books
	CHECK( xbox_aci_account( &b, 7, civ, lvi, ev, XBOX_ACI_KICK_LAST ) == 0 );
	CHECK( xbox_aci_account( &b, 7, civ, lvi, ev, XBOX_ACI_KICK_ALL ) == 1 );
	CHECK( b.reported == 7 && b.rescues == 1 );
	CHECK( b.retired[0] == 7 && b.retired[1] == 7 );
	// KICK_ALL with buffers in flight writes them off: no rescue
	civ[0] = civ[1] = 7; lvi[0] = lvi[1] = 8;
	CHECK( xbox_aci_account( &b, 9, civ, lvi, ev, XBOX_ACI_KICK_ALL ) == 2 );
	CHECK( b.reported == 9 && b.rescues == 1 );

	// an emulator's S/PDIF engine never leaves descriptor 0 (xemu, xbased): after PCM out has
	// finished XBOX_ACI_SPDIF_DEAD_AFTER buffers it is written off, and PCM alone is waited for
	memset( &b, 0, sizeof( b ));
	civ[0] = 1; lvi[0] = 7; civ[1] = 0; lvi[1] = 7;
	CHECK( xbox_aci_account( &b, 8, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 0 ); // 1 buffer: not yet
	CHECK( !b.spdif_dead && b.retired[0] == 1 && b.reported == 0 );
	civ[0] = 5;
	CHECK( xbox_aci_account( &b, 8, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 5 );
	CHECK( b.spdif_dead && b.reported == 5 );
	civ[0] = 7;
	CHECK( xbox_aci_account( &b, 8, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 2 );
	CHECK( b.reported == 7 );

	// the console: S/PDIF moves with PCM out, so it is never written off
	memset( &b, 0, sizeof( b ));
	civ[0] = 5; lvi[0] = 7; civ[1] = 1; lvi[1] = 7;
	CHECK( xbox_aci_account( &b, 8, civ, lvi, ev, XBOX_ACI_KICK_NONE ) == 1 );
	CHECK( !b.spdif_dead && b.retired[1] == 1 && b.reported == 1 );
}

static void test_watchdog( void )
{
	xbox_aci_watchdog_t wd;

	memset( &wd, 0, sizeof( wd ));
	CHECK( xbox_aci_watchdog_step( &wd, 10.0, 5, 1, 1.0 ) == 0 ); // arms
	CHECK( xbox_aci_watchdog_step( &wd, 10.9, 5, 1, 1.0 ) == 0 );
	CHECK( xbox_aci_watchdog_step( &wd, 11.0, 5, 1, 1.0 ) == 1 ); // first stall
	CHECK( xbox_aci_watchdog_step( &wd, 11.5, 5, 1, 1.0 ) == 0 ); // re-armed
	CHECK( xbox_aci_watchdog_step( &wd, 12.0, 5, 1, 1.0 ) == 2 ); // still stalled
	CHECK( xbox_aci_watchdog_step( &wd, 12.1, 6, 1, 1.0 ) == 0 ); // progress resets
	CHECK( xbox_aci_watchdog_step( &wd, 13.0, 6, 1, 1.0 ) == 0 );
	CHECK( xbox_aci_watchdog_step( &wd, 13.2, 6, 1, 1.0 ) == 1 );
	// paused / closed: never a stall, and the clock restarts on resume
	CHECK( xbox_aci_watchdog_step( &wd, 20.0, 6, 0, 1.0 ) == 0 );
	CHECK( xbox_aci_watchdog_step( &wd, 20.5, 6, 1, 1.0 ) == 0 );
	CHECK( xbox_aci_watchdog_step( &wd, 20.9, 6, 1, 1.0 ) == 0 );
	CHECK( xbox_aci_watchdog_step( &wd, 21.5, 6, 1, 1.0 ) == 1 );
	// a long hitch (a level load) with progress across it is no stall
	CHECK( xbox_aci_watchdog_step( &wd, 40.0, 900, 1, 1.0 ) == 0 );
}

/* ---- pipeline simulation ----------------------------------------------- */

enum { HALT, LOOP };
enum { ORIGINAL, REPLACEMENT };

#define SR_DCH   0x01u
#define SR_LVBCI 0x04u
#define SR_BCIS  0x08u

typedef struct
{
	uint32_t civ, lvi, picb, sr;
	int      halted;   /* HALT: stopped at the end of the last valid buffer */
	int      stale;    /* LOOP: replaying the last buffer */
	uint32_t finished; /* ground truth: buffers really played out */
	uint32_t drains;   /* underruns: a buffer played out with nothing queued after it */
	/* the replacement's state */
	int      latch;
	/* the original's state */
	uint32_t count;
	int      drained;
} engine_t;

typedef struct
{
	int      model, hal;
	engine_t e[2];
	int      started;
	uint32_t submitted;
	xbox_aci_books_t books; /* replacement: the DPC's books */
	int      force;     /* replacement: the next accounting run's XBOX_ACI_KICK_* */
	int      drop_posts; /* semaphore posts still to lose (a failing SDL_SemPost) */
	uint32_t sem;       /* the SDL driver's playsem */
	int      next_buffer;
	int      blocked;   /* thread waiting on sem */
	int      thread_started;
	uint32_t plays;
	int      unsafe;    /* a buffer refilled before both engines finished it */
} sim_t;

static void sim_lvi_write( sim_t *s, engine_t *e, uint32_t v )
{
	e->lvi = v;
	if( s->model == HALT && e->halted )
	{
		e->civ = ( e->civ + 1 ) & 31;
		e->halted = 0;
		e->picb = 2048;
		e->sr &= ~SR_DCH;
	}
}

/* XAudioProvideSamples: both descriptors, then both LVIs. */
static void sim_provide( sim_t *s )
{
	const uint32_t idx = s->submitted & 31;
	int i;

	for( i = 0; i < 2; i++ )
		sim_lvi_write( s, &s->e[i], idx );
	if( s->hal == ORIGINAL )
	{
		s->e[0].count++;
		s->e[1].count++;
	}
	s->submitted++;
}

/* The DPC's accounting: the replacement's (xbox_audio_hal.c) or nxdk's. */
static void sim_interrupt( sim_t *s )
{
	int i;

	if( s->hal == REPLACEMENT )
	{
		uint32_t civ[2], lvi[2], ev[2], n;

		// the ISR
		for( i = 0; i < 2; i++ )
		{
			engine_t *e = &s->e[i];
			if( e->sr & SR_LVBCI )
				e->latch = 1;
			e->sr &= SR_DCH; // write-1-to-clear of what was read
		}
		// the DPC, as aci_dpc_routine gathers it (RPBM: started)
		for( i = 0; i < 2; i++ )
		{
			engine_t *e = &s->e[i];
			ev[i] = 0;
			if(( e->sr & SR_DCH ) && e->picb == 0 && s->started )
				ev[i] |= XBOX_ACI_HALTED_EMPTY;
			if( e->latch )
				ev[i] |= XBOX_ACI_LAST_DONE;
			e->latch = 0;
			civ[i] = e->civ;
			lvi[i] = e->lvi;
		}
		n = xbox_aci_account( &s->books, s->submitted, civ, lvi, ev, s->force );
		s->force = XBOX_ACI_KICK_NONE;
		while( n && s->drop_posts )
		{
			n--;
			s->drop_posts--;
		}
		s->sem += n;
		return;
	}

	// nxdk lib/hal/audio.c ISR, transcribed (analog then digital)
	{
		int wait_completed = 0;
		for( i = 0; i < 2; i++ )
		{
			engine_t *e = &s->e[i], *o = &s->e[!i];
			uint32_t sr = e->sr & ( SR_LVBCI | SR_BCIS | SR_DCH );
			if( !sr )
				continue;
			{
				int waiting = e->count > o->count;
				if(( sr & SR_BCIS ) && !e->drained )
				{
					if( e->count > 0 )
					{
						wait_completed |= waiting;
						e->count--;
					}
				}
				e->drained = ( sr & SR_LVBCI ) != 0;
			}
			e->sr &= SR_DCH;
		}
		if( wait_completed )
			s->sem++;
	}
}

/* One engine reaches the end of the buffer it is playing. */
static void sim_buffer_end( sim_t *s, engine_t *e )
{
	if( !s->started || e->halted )
		return;

	if( e->stale )
	{
		// LOOP: the end of a replay pass - nothing new was played
		if( e->civ != e->lvi )
		{
			e->civ = ( e->civ + 1 ) & 31;
			e->stale = 0;
			e->sr |= SR_BCIS;
		}
		else e->sr |= SR_BCIS | SR_LVBCI;
		sim_interrupt( s );
		return;
	}

	e->finished++;
	if( e->civ != e->lvi )
	{
		e->civ = ( e->civ + 1 ) & 31;
		e->sr |= SR_BCIS;
	}
	else
	{
		e->drains++;
		e->sr |= SR_BCIS | SR_LVBCI;
		if( s->model == HALT )
		{
			e->halted = 1;
			e->picb = 0;
			e->sr |= SR_DCH;
		}
		else e->stale = 1;
	}
	sim_interrupt( s );
}

/* The SDL audio thread runs until it blocks in WaitDevice. */
static void sim_thread( sim_t *s )
{
	int guard = 0;

	if( !s->thread_started )
		return;
	if( s->blocked )
	{
		if( !s->sem )
			return;
		s->sem--;
		s->blocked = 0;
	}
	while( guard++ < 8 )
	{
		// PlayDevice: the buffer refilled now was last handed over two
		// submissions ago; both engines must have played it
		const uint32_t seq = s->submitted;
		if( seq >= 2 && ( s->e[0].finished < seq - 1 || s->e[1].finished < seq - 1 ))
			s->unsafe = 1;
		sim_provide( s );
		s->next_buffer ^= 1;
		s->plays++;
		// WaitDevice
		if( !s->sem )
		{
			s->blocked = 1;
			return;
		}
		s->sem--;
	}
}

/* Runs `events` buffer periods. The thread starts after `start_delay`
   periods and misses `late_len` periods from `late_at` on (an underrun);
   `skew` lets it act between the two engines' buffer ends. Returns how
   many buffers the thread queued in the last 20 periods (0 = stalled);
   *tail_drains gets the underruns in those periods (a pipeline that
   recovered has none - one that runs a buffer behind drains every time,
   which this model's instant refills would otherwise hide). */
static uint32_t sim_run_lossy( int model, int hal, int start_delay, int late_at, int late_len, int skew,
	int lose_at, int lose_posts, int *unsafe, uint32_t *tail_drains, uint32_t *kicks )
{
	sim_t s;
	uint32_t plays_before_tail = 0, drains_before_tail = 0;
	uint32_t wd_plays = 0, wd_stalls = 0;
	int t, wd_idle = 0;
	const int events = 200;
	// the watchdog, in buffer periods: 5 of them stand for its second
	const int wd_timeout = 5;

	*kicks = 0;

	memset( &s, 0, sizeof( s ));
	s.model = model;
	s.hal = hal;
	// XAudioInit leaves both engines paused: DCH set, PICB 0
	s.e[0].sr = s.e[1].sr = SR_DCH;

	// XBOXAUDIO_OpenDevice: buffer 1 queued at descriptor 0, then play
	sim_provide( &s );
	if( hal == REPLACEMENT )
	{
		// an accounting run before XAudioPlay must report nothing
		sim_interrupt( &s );
		if( s.sem )
			s.unsafe = 1;
	}
	s.started = 1;
	s.e[0].sr = s.e[1].sr = 0;
	s.e[0].picb = s.e[1].picb = 2048;

	for( t = 0; t < events; t++ )
	{
		const int late = t >= late_at && t < late_at + late_len;

		if( t == start_delay )
		{
			s.thread_started = 1;
			sim_thread( &s ); // its first Play, before this period's buffer ends
		}
		if( t == events - 20 )
		{
			plays_before_tail = s.plays;
			drains_before_tail = s.e[0].drains + s.e[1].drains;
		}
		if( t == lose_at )
			s.drop_posts = lose_posts;

		sim_buffer_end( &s, &s.e[0] );
		if( skew && !late )
			sim_thread( &s );
		sim_buffer_end( &s, &s.e[1] );
		if( !late )
			sim_thread( &s );

		// Xbox_AudioWatchdog, on the submission count (the replacement only)
		if( hal == REPLACEMENT && s.thread_started )
		{
			if( s.submitted != wd_plays )
			{
				wd_plays = s.submitted;
				wd_idle = 0;
				wd_stalls = 0;
			}
			else if( ++wd_idle >= wd_timeout )
			{
				wd_idle = 0;
				wd_stalls++;
				s.force = wd_stalls >= 4 ? XBOX_ACI_KICK_ALL : XBOX_ACI_KICK_LAST;
				sim_interrupt( &s );
				( *kicks )++;
				sim_thread( &s );
			}
		}
	}

	*unsafe = s.unsafe;
	*tail_drains = s.e[0].drains + s.e[1].drains - drains_before_tail;
	return s.plays - plays_before_tail;
}

static uint32_t sim_run( int model, int hal, int start_delay, int late_at, int late_len, int skew, int *unsafe, uint32_t *tail_drains )
{
	uint32_t kicks, plays;

	plays = sim_run_lossy( model, hal, start_delay, late_at, late_len, skew, 1000, 0, unsafe, tail_drains, &kicks );
	// nothing was lost: the watchdog never had a reason to fire
	if( hal == REPLACEMENT && kicks )
		*unsafe = 1;
	return plays;
}

static void test_pipeline( void )
{
	static const struct { int start_delay, late_at, late_len; } cases[] =
	{
		{ 0, 1000, 0 }, // never late
		{ 3, 1000, 0 }, // the audio thread first runs 3 buffers after XAudioPlay
		{ 0, 50, 3 },   // late once, mid-run
		{ 2, 80, 1 },
		{ 0, 30, 1 },
	};
	int model, skew, unsafe;
	uint32_t drains;
	size_t i;

	for( model = HALT; model <= LOOP; model++ )
	for( skew = 0; skew <= 1; skew++ )
	for( i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
	{
		const uint32_t tail = sim_run( model, REPLACEMENT, cases[i].start_delay, cases[i].late_at, cases[i].late_len, skew, &unsafe, &drains );
		if( tail < 15 || unsafe || drains )
			fprintf( stderr, "replacement: model %s skew %d case %u: %u buffers and %u underruns in the last 20 periods, unsafe %d\n",
				model == HALT ? "HALT" : "LOOP", skew, (unsigned)i, (unsigned)tail, (unsigned)drains, unsafe );
		CHECK( tail >= 15 );
		CHECK( drains == 0 );
		CHECK( !unsafe );
	}

	// The bug, reproduced: nxdk's accounting is fine while nothing is late,
	// and under LOOP; under HALT one late period stalls it for good.
	CHECK( sim_run( HALT, ORIGINAL, 0, 1000, 0, 0, &unsafe, &drains ) >= 15 );
	CHECK( sim_run( HALT, ORIGINAL, 3, 1000, 0, 0, &unsafe, &drains ) == 0 );
	CHECK( sim_run( HALT, ORIGINAL, 0, 50, 3, 0, &unsafe, &drains ) == 0 );
	CHECK( sim_run( LOOP, ORIGINAL, 3, 1000, 0, 0, &unsafe, &drains ) >= 15 );
	CHECK( sim_run( LOOP, ORIGINAL, 0, 50, 3, 0, &unsafe, &drains ) >= 15 );

	// Two semaphore posts lost (a failing SDL_SemPost - nothing in the
	// replacement loses one): the thread waits for good on a buffer
	// already reported. KICK_LAST cannot help (nothing unreported is
	// left); the fourth stall's KICK_ALL rescue wakes it. One lost post
	// leaves SDL a single buffer in flight - alive but underrunning every
	// buffer, with no stall for the watchdog to see; that is the known
	// limit of a rescue that cannot see SDL's credits.
	for( model = HALT; model <= LOOP; model++ )
	{
		uint32_t kicks;
		const uint32_t tail = sim_run_lossy( model, REPLACEMENT, 0, 1000, 0, 0, 60, 2, &unsafe, &drains, &kicks );
		if( kicks != 4 || tail < 8 || unsafe )
			fprintf( stderr, "lost posts: model %s: %u buffers and %u underruns in the last 20 periods, %u kicks, unsafe %d\n",
				model == HALT ? "HALT" : "LOOP", (unsigned)tail, (unsigned)drains, (unsigned)kicks, unsafe );
		CHECK( kicks == 4 );
		CHECK( tail >= 8 ); // alive: back to one buffer in flight (LOOP: half rate)
		CHECK( !unsafe );
	}
}


/* ---- the bus masters' reset across open/close cycles (#173) ------------ */

/* A model of one AC97 bus master's CR and SR: writing CR = 0 pauses it and
   DCH comes up `halt_after` polls later (never, for -1); writing RR to an
   engine that has not halted wedges it for good (what a live reopen did to
   the console's PCM DMA); RR on a halted engine resets it, and the
   hardware clears RR `rr_after` polls later (never, for -1). An engine
   that was reset plays again on CR = RUN. */
typedef struct
{
	int running, dch, rr, halt_after, rr_after, wedged, resets, polls_to_halt, polls_to_rr;
	unsigned order; /* CR writes seen: 1 pause, then 2 reset */
} model_box_t;

typedef struct
{
	model_box_t box[2];
	unsigned stalled_us;
} model_t;

static model_box_t *model_box( model_t *m, unsigned offset, unsigned *reg )
{
	const int i = offset >= XBOX_ACI_BOX_SPDIF ? 1 : 0;

	*reg = offset - ( i ? XBOX_ACI_BOX_SPDIF : XBOX_ACI_BOX_PCM_OUT );
	return &m->box[i];
}

static uint8_t model_read8( void *ctx, unsigned offset )
{
	model_t *m = ctx;
	unsigned reg;
	model_box_t *b = model_box( m, offset, &reg );

	if( reg == XBOX_ACI_REG_SR )
	{
		if( !b->running && !b->dch && b->polls_to_halt >= 0 && b->polls_to_halt-- == 0 )
			b->dch = 1;
		return (uint8_t)( b->dch ? XBOX_ACI_SR_DCH : 0 );
	}
	if( reg == XBOX_ACI_REG_CR )
	{
		if( b->rr && b->polls_to_rr >= 0 && b->polls_to_rr-- == 0 )
			b->rr = 0;
		return (uint8_t)(( b->rr ? XBOX_ACI_CR_RR : 0 ) | ( b->running ? 1 : 0 ));
	}
	return 0;
}

static void model_write8( void *ctx, unsigned offset, uint8_t v )
{
	model_t *m = ctx;
	unsigned reg;
	model_box_t *b = model_box( m, offset, &reg );

	if( reg != XBOX_ACI_REG_CR )
		return;
	if( v & XBOX_ACI_CR_RR )
	{
		if( !b->dch || b->running )
			b->wedged = 1;
		b->rr = 1;
		b->polls_to_rr = b->rr_after;
		b->resets++;
		b->order = b->order * 10 + 2;
		return;
	}
	if( v & 1 )
	{
		b->running = 1;
		b->dch = 0;
		return;
	}
	b->running = 0;
	b->polls_to_halt = b->halt_after;
	b->order = b->order * 10 + 1;
}

static void model_stall( void *ctx, unsigned us )
{
	( (model_t *)ctx )->stalled_us += us;
}

static void model_init( model_t *m, int running, int halt_after, int rr_after )
{
	int i;

	memset( m, 0, sizeof( *m ));
	for( i = 0; i < 2; i++ )
	{
		m->box[i].running = running;
		m->box[i].dch = !running;
		m->box[i].halt_after = halt_after;
		m->box[i].rr_after = rr_after;
	}
}

/* nxdk's XAudioInit: RR straight away, then wait for it to clear */
static void nxdk_reset( model_t *m )
{
	model_write8( m, XBOX_ACI_BOX_PCM_OUT + XBOX_ACI_REG_CR, XBOX_ACI_CR_RESET );
	model_write8( m, XBOX_ACI_BOX_SPDIF + XBOX_ACI_REG_CR, XBOX_ACI_CR_RESET );
}

static void test_reset_engines( void )
{
	const xbox_aci_mmio_t io0 = { model_read8, model_write8, model_stall, NULL };
	xbox_aci_mmio_t io = io0;
	model_t m;
	int cycle, i;

	io.ctx = &m;

	/* the model sees the bug: nxdk's sequence on a playing engine wedges it */
	model_init( &m, 1, 3, 2 );
	nxdk_reset( &m );
	CHECK( m.box[0].wedged && m.box[1].wedged );

	/* boot: nothing has run, both halted - reset straight away */
	model_init( &m, 0, 0, 2 );
	CHECK( xbox_aci_reset_engines( &io, 50000, 10 ) == 0 );
	CHECK( !m.box[0].wedged && !m.box[1].wedged && m.box[0].resets == 1 && m.box[1].resets == 1 );

	/* 50 open/close cycles, each closing on engines still playing, with
	   halt and RR-clear delays from none to longer than a poll */
	for( cycle = 0; cycle < 50; cycle++ )
	{
		model_init( &m, 1, cycle % 7, cycle % 5 );
		CHECK( xbox_aci_reset_engines( &io, 50000, 10 ) == 0 );
		for( i = 0; i < 2; i++ )
		{
			CHECK( !m.box[i].wedged );
			CHECK( m.box[i].resets == 1 && m.box[i].order == 12 ); /* paused, then reset */
			CHECK( !m.box[i].rr );
		}
		/* the next open plays: CR = RUN */
		model_write8( &m, XBOX_ACI_BOX_PCM_OUT + XBOX_ACI_REG_CR, 0x1d );
		CHECK( m.box[0].running && !m.box[0].wedged );
	}

	/* PCM out never halts: it is left un-reset (reported), S/PDIF is
	   reset, and the wait is bounded */
	model_init( &m, 1, -1, 1 );
	m.box[1].halt_after = 2;
	CHECK( xbox_aci_reset_engines( &io, 50000, 10 ) == 1u );
	CHECK( !m.box[0].wedged && m.box[0].resets == 0 && m.box[1].resets == 1 && !m.box[1].wedged );
	CHECK( m.stalled_us <= 50000 + 10 + 100 );

	/* RR never clears on S/PDIF: reported, bounded */
	model_init( &m, 1, 1, -1 );
	m.box[0].rr_after = 0;
	CHECK( xbox_aci_reset_engines( &io, 50000, 10 ) == 2u );
	CHECK( m.stalled_us <= 2 * ( 50000 + 10 ) + 100 );

	/* a zero poll interval is taken as 1 us: still bounded */
	model_init( &m, 1, -1, -1 );
	CHECK( xbox_aci_reset_engines( &io, 100, 0 ) == 3u );
	CHECK( m.stalled_us <= 2 * 101 );
}

/* The rings (#173, Codex review): XAudioInit may zero the descriptor
   lists and clear SR only once both engines have halted - a running
   engine fetches its ring. clear_rings records whether it ran while an
   engine could still fetch. */
typedef struct
{
	model_t *m;
	int calls, unsafe;
} rings_t;

static void model_clear_rings( void *ctx )
{
	rings_t *r = ctx;
	int i;

	r->calls++;
	for( i = 0; i < 2; i++ )
		if( r->m->box[i].running || !r->m->box[i].dch || r->m->box[i].wedged )
			r->unsafe++;
}

/* nxdk's XAudioInit order: the rings first, then the reset */
static void nxdk_prepare( model_t *m, rings_t *r )
{
	model_clear_rings( r );
	nxdk_reset( m );
	(void)m;
}

static void test_prepare( void )
{
	const xbox_aci_mmio_t io0 = { model_read8, model_write8, model_stall, NULL };
	xbox_aci_mmio_t io = io0;
	model_t m;
	rings_t r;
	int cycle;

	io.ctx = &m;

	/* the old order on a playing engine writes the rings under it */
	model_init( &m, 1, 3, 2 );
	memset( &r, 0, sizeof( r ));
	r.m = &m;
	nxdk_prepare( &m, &r );
	CHECK( r.unsafe > 0 );

	/* 50 reopens on playing engines: the rings are written once, after
	   both halted and were reset */
	for( cycle = 0; cycle < 50; cycle++ )
	{
		model_init( &m, 1, cycle % 7, cycle % 5 );
		memset( &r, 0, sizeof( r ));
		r.m = &m;
		CHECK( xbox_aci_prepare( &io, model_clear_rings, &r, 50000, 10 ) == 0 );
		CHECK( r.calls == 1 && r.unsafe == 0 );
		CHECK( !m.box[0].wedged && !m.box[1].wedged );
	}

	/* an engine that never halts: the rings are not touched at all, and
	   the failure is returned */
	model_init( &m, 1, -1, 1 );
	m.box[1].halt_after = 1;
	memset( &r, 0, sizeof( r ));
	r.m = &m;
	CHECK( xbox_aci_prepare( &io, model_clear_rings, &r, 50000, 10 ) == 1u );
	CHECK( r.calls == 0 );
	model_init( &m, 1, 1, -1 ); /* RR never clears */
	memset( &r, 0, sizeof( r ));
	r.m = &m;
	CHECK( xbox_aci_prepare( &io, model_clear_rings, &r, 50000, 10 ) != 0 );
	CHECK( r.calls == 0 );
}

int main( void )
{
	test_finished();
	test_account();
	test_watchdog();
	test_pipeline();
	test_reset_engines();
	test_prepare();

	if( failures )
	{
		fprintf( stderr, "xbox_audio_hal_util: %d failure(s)\n", failures );
		return 1;
	}
	printf( "xbox_audio_hal_util: all checks passed\n" );
	return 0;
}
