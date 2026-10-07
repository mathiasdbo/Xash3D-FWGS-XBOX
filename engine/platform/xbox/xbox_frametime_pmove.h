/*
xbox_frametime_pmove.h - the frame-time recorder's player movement breakdown
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
Resonance3D (docs/r3d/divergences.md #141, the recorder; the player
movement breakdown of 2026-10-06, x9-perf-pmove). Player movement ran
about 0.8 ms a frame on the console, in two spans with nothing under
them: k_sv_runcmd (the server's SV_RunCmd) and k_cl_predmove (the
client's prediction). Both run the game's PM_Move, which asks the engine
for traces, position tests and point contents (engine/common/pm_trace.c)
against a physent list the engine builds for it (SV_SetupPMove,
CL_SetSolidEntities). This header splits that time and counts that work:

  - which side is moving: xbox_ft_pm.side is 1 while the server's
    PM_Move runs and 2 while the client's does (XBOX_FT_PM_MOVE), so a
    query counts for the side that asked and the client's weapon and
    event traces (outside PM_Move) are not counted;
  - the XBOX_FT_W_PM_* counts: moves, physents, traces, the physents
    those traces checked, position tests, point contents, texture traces;
  - the XBOX_FT_W_PM_*_KC times, accumulated here in ticks (the renderer's
    waits taken out, like a span) and stored in kilocycles when the frame
    closes. They overlap: the query time is part of the move time.

The work counters are xbox_frametime_util.h's; every _SV counter has its
_CL twin at the next index, so a site picks its side by adding
side - 1. With the recorder off a count is one load and branch of
`side`, and a timer one load and branch of xbox_ft_spans.on; outside an
Xbox build every macro is the plain upstream statement or nothing.
*/

#ifndef XBOX_FRAMETIME_PMOVE_H
#define XBOX_FRAMETIME_PMOVE_H

#if XASH_XBOX && !XASH_DEDICATED

#include "xbox_frametime.h"

/* The timers, stored as XBOX_FT_W_PM_*_KC. */
enum
{
	XBOX_FT_PM_SV_SETUP = 0,
	XBOX_FT_PM_SV_MOVE,
	XBOX_FT_PM_SV_QUERY,
	XBOX_FT_PM_SV_THINK,
	XBOX_FT_PM_SV_TOUCH,
	XBOX_FT_PM_CL_SOLID,
	XBOX_FT_PM_CL_MOVE,
	XBOX_FT_PM_CL_QUERY,
	XBOX_FT_PM_CL_POST,
	XBOX_FT_PM_TIMERS
};

typedef struct xbox_ft_pm_s
{
	uint32_t side;     /* 0 no PM_Move running, 1 the server's, 2 the client's */
	uint32_t depth;    /* queries open (one calls another): only the outermost is timed */
	uint64_t ticks[XBOX_FT_PM_TIMERS];
} xbox_ft_pm_t;

extern xbox_ft_pm_t xbox_ft_pm;

/* Stores the timers into the XBOX_FT_W_PM_*_KC counters and zeroes them
   (FT_CloseFrame, before the counters are copied). */
void Xbox_FT_PMove_CloseFrame( void );

static inline void xbox_ft_pm_timer_end( const xbox_ft_span_t *s, int timer )
{
	if( s->t0 )
	{
		xbox_ft_pm.ticks[timer] += xbox_ft_span_ticks_since( s );
		xbox_ft_spans.calls++;
	}
}

/* `stmt`, timed as `timer`. */
#define XBOX_FT_PM_TIMED( timer, stmt ) \
	do { \
		xbox_ft_span_t xbox_ft_pm_s_; \
		xbox_ft_span_begin( &xbox_ft_pm_s_ ); \
		stmt; \
		xbox_ft_pm_timer_end( &xbox_ft_pm_s_, ( timer )); \
	} while( 0 )

/* A run of statements timed as `timer`: two inserted lines, no re-indent. */
#define XBOX_FT_PM_BEGIN( var ) xbox_ft_span_t var; xbox_ft_span_begin( &var )
#define XBOX_FT_PM_END( var, timer ) xbox_ft_pm_timer_end( &var, ( timer ))

/* The game DLL's PM_Move on side `which` (1 server, 2 client) with the
   playermove_t `pm`, timed as `timer`; the queries it makes count for
   that side, and its physent and visent lists' sizes are added to the
   side's pm_*_physents and pm_*_visents. */
#define XBOX_FT_PM_MOVE( which, timer, pm, stmt ) \
	do { \
		const uint32_t xbox_ft_pm_old_ = xbox_ft_pm.side; \
		xbox_ft_work.c[XBOX_FT_W_PM_SV_PHYSENTS + ( which ) - 1] += (uint32_t)( pm )->numphysent; \
		xbox_ft_work.c[XBOX_FT_W_PM_SV_VISENTS + ( which ) - 1] += (uint32_t)( pm )->numvisent; \
		xbox_ft_pm.side = ( which ); \
		XBOX_FT_PM_TIMED( ( timer ), stmt ); \
		xbox_ft_pm.side = xbox_ft_pm_old_; \
	} while( 0 )

/* Adds `n` to the _SV counter `w` or to its _CL twin, by side; nothing
   outside a PM_Move. */
static inline void xbox_ft_pm_add( int w, uint32_t n )
{
	if( xbox_ft_pm.side )
		xbox_ft_work.c[w + (int)xbox_ft_pm.side - 1] += n;
}

#define XBOX_FT_PM_COUNT( w )   xbox_ft_pm_add( ( w ), 1u )
#define XBOX_FT_PM_ADD( w, n )  xbox_ft_pm_add( ( w ), (uint32_t)( n ))
/* A plain count with no side (pm_cl_solids, pm_*_cmds). */
#define XBOX_FT_PM_COUNT_ALWAYS( w ) ( xbox_ft_work.c[( w )]++ )

/* One engine query: counted for the moving side as `w` (an _SV counter),
   and timed into that side's query timer when it is the outermost one.
   XBOX_FT_PM_QUERY( w ) is one declaration at the top of the function;
   the cleanup attribute closes it at every return, so a function with
   several return statements needs no other change. */
typedef struct xbox_ft_pm_query_s
{
	xbox_ft_span_t span;
	uint32_t timer;   /* 0: not timing; else the query timer + 1 */
} xbox_ft_pm_query_t;

static inline xbox_ft_pm_query_t xbox_ft_pm_query_begin( int w )
{
	xbox_ft_pm_query_t q;

	q.timer = 0;
	q.span.t0 = 0;
	if( !xbox_ft_pm.side )
		return q;
	xbox_ft_work.c[w + (int)xbox_ft_pm.side - 1]++;
	if( xbox_ft_pm.depth++ == 0 && xbox_ft_spans.on )
	{
		q.timer = ( xbox_ft_pm.side == 1 ? XBOX_FT_PM_SV_QUERY : XBOX_FT_PM_CL_QUERY ) + 1;
		xbox_ft_span_begin( &q.span );
	}
	return q;
}

static inline void xbox_ft_pm_query_end( xbox_ft_pm_query_t *q )
{
	if( q->timer )
		xbox_ft_pm_timer_end( &q->span, (int)q->timer - 1 );
	/* A query that began inside a move opened a depth level; one outside
	   (side 0) did not. The move's side cannot change under a query. */
	if( xbox_ft_pm.side && xbox_ft_pm.depth )
		xbox_ft_pm.depth--;
}

#define XBOX_FT_PM_QUERY( w ) \
	xbox_ft_pm_query_t xbox_ft_pm_q_ __attribute__(( cleanup( xbox_ft_pm_query_end ))) = xbox_ft_pm_query_begin( w )

#else /* !( XASH_XBOX && !XASH_DEDICATED ) */

#define XBOX_FT_PM_TIMED( timer, stmt )        stmt
#define XBOX_FT_PM_BEGIN( var )
#define XBOX_FT_PM_END( var, timer )
#define XBOX_FT_PM_MOVE( which, timer, pm, stmt ) stmt
#define XBOX_FT_PM_COUNT( w )                  ((void)0)
#define XBOX_FT_PM_ADD( w, n )                 ((void)0)
#define XBOX_FT_PM_COUNT_ALWAYS( w )           ((void)0)
#define XBOX_FT_PM_QUERY( w )

#endif /* XASH_XBOX && !XASH_DEDICATED */

#endif /* XBOX_FRAMETIME_PMOVE_H */
