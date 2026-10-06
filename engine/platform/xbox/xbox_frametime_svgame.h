/*
xbox_frametime_svgame.h - the frame-time recorder's server game frame breakdown
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
Resonance3D (docs/r3d/divergences.md #141, the recorder; the sv_game
breakdown of 2026-10-05). k_sv_game - SV_Physics, the server's whole game
frame - had become the largest single CPU cost on the console with nothing
under it. This header is what engine/server includes to break it down:

  - the per-movetype spans (k_sv_pstep, k_sv_ppush...) and StartFrame's
    (k_sv_start), XBOX_FT_S_SV_* in xbox_frametime_util.h;
  - the g_* work counters: thinks, touches, SV_Move calls, the DLL's own
    traces, area-node candidates, links, push tests, walk moves, find
    scans - counted only while SV_Physics runs (xbox_ft_sv.in_game);
  - three cross-cutting timers - time inside the DLL's Think, its Touch,
    and SV_Move - which overlap each other and the spans (a think that
    traces counts in both), stored as g_*_kc counters;
  - the slowest single entity of each frame (g_worst_*), and per-classname
    totals the recorder writes as a CLASSES chunk at each map change.

Every engine/server site is a macro below; with the recorder off a count
is one load and branch (in_game is set only while SV_Physics runs and
the recorder is recording, and the timers also test xbox_ft_spans.on),
and outside an Xbox build every macro is the plain upstream statement. xbox_frametime_svgame.c owns the
state and the classname table.
*/

#ifndef XBOX_FRAMETIME_SVGAME_H
#define XBOX_FRAMETIME_SVGAME_H

#if XASH_XBOX && !XASH_DEDICATED

#include "xbox_frametime.h"

/* The cross-cutting timers. */
enum
{
	XBOX_FT_SV_THINK = 0,
	XBOX_FT_SV_TOUCH,
	XBOX_FT_SV_MOVE,
	XBOX_FT_SV_TIMERS
};

typedef struct xbox_ft_sv_s
{
	uint32_t in_game;                    /* non-zero inside SV_Physics */
	uint32_t depth[XBOX_FT_SV_TIMERS];   /* nesting: only the outermost call is timed */
	uint64_t ticks[XBOX_FT_SV_TIMERS];   /* this frame */
	uint64_t worst;                      /* the slowest entity this frame, ticks */
	int worst_cls;                       /* its class row, -1 none */
	int worst_ent;                       /* its edict number */
	int in_play;                         /* this frame counts towards the CLASSES rows */
} xbox_ft_sv_t;

extern xbox_ft_sv_t xbox_ft_sv;

typedef struct xbox_ft_sv_timer_s
{
	uint64_t t0;
	int state;      /* 0 nothing, 1 nested (counted, not timed), 2 timing */
} xbox_ft_sv_timer_t;

static inline void xbox_ft_sv_count( int w )
{
	if( xbox_ft_sv.in_game )
		xbox_ft_work.c[w]++;
}

static inline void xbox_ft_sv_add( int w, uint32_t n )
{
	if( xbox_ft_sv.in_game )
		xbox_ft_work.c[w] += n;
}

static inline void xbox_ft_sv_timer_begin( xbox_ft_sv_timer_t *t, int timer, int w )
{
	t->state = 0;
	if( !xbox_ft_sv.in_game )
		return;
	xbox_ft_work.c[w]++;
	if( !xbox_ft_spans.on )
		return;
	if( xbox_ft_sv.depth[timer]++ )
	{
		t->state = 1;
		return;
	}
	t->state = 2;
	t->t0 = xbox_ft_rdtsc();
}

static inline void xbox_ft_sv_timer_end( const xbox_ft_sv_timer_t *t, int timer )
{
	if( t->state == 2 )
	{
		xbox_ft_sv.ticks[timer] += xbox_ft_rdtsc() - t->t0;
		xbox_ft_spans.calls++;
	}
	if( t->state )
		xbox_ft_sv.depth[timer]--;
}

/* One entity's SV_Physics_Entity. */
typedef struct xbox_ft_sv_ent_s
{
	uint64_t t0;        /* 0: not timing */
	uint64_t think0;    /* xbox_ft_sv.ticks[XBOX_FT_SV_THINK] at the start */
	uint32_t thinks0, moves0;
	int span;           /* XBOX_FT_S_SV_P* of its movetype at the start */
	int movetype;
	int cls;            /* its class row, -1 when the frame is not counted */
	int num;            /* its edict number */
} xbox_ft_sv_ent_t;

struct edict_s;
void Xbox_FT_SvGame_EntityBegin( xbox_ft_sv_ent_t *e, struct edict_s *ent, int num );
void Xbox_FT_SvGame_EntityEnd( const xbox_ft_sv_ent_t *e );

/* The recorder's side (xbox_frametime.c). */
void Xbox_FT_SvGame_BeginFrame( int in_play );  /* every Host_Frame, recorder on or off */
void Xbox_FT_SvGame_CloseFrame( void );         /* before the work counters are copied */
qboolean Xbox_FT_SvGame_Start( void );          /* allocates the classname table */
void Xbox_FT_SvGame_Stop( void );
/* The CLASSES chunk for the rows so far, into an internal buffer (NULL
   and 0 when there is nothing to write); resets the rows' totals. */
size_t Xbox_FT_SvGame_EncodeClasses( const uint8_t **out );

/* SV_Physics: the game frame's bounds. */
#define XBOX_FT_SV_GAME_BEGIN() ( xbox_ft_sv.in_game = xbox_ft_waits.active )
#define XBOX_FT_SV_GAME_END()   ( xbox_ft_sv.in_game = 0 )

/* A plain count, XBOX_FT_W_G_*. */
#define XBOX_FT_SV_COUNT( w )   xbox_ft_sv_count( w )
#define XBOX_FT_SV_ADD( w, n )  xbox_ft_sv_add( ( w ), ( n ))

/* `stmt`, counted as `w` and timed as `timer` (XBOX_FT_SV_THINK...). */
#define XBOX_FT_SV_TIMED( timer, w, stmt ) \
	do { \
		xbox_ft_sv_timer_t xbox_ft_sv_t_; \
		xbox_ft_sv_timer_begin( &xbox_ft_sv_t_, ( timer ), ( w )); \
		stmt; \
		xbox_ft_sv_timer_end( &xbox_ft_sv_t_, ( timer )); \
	} while( 0 )

/* SV_Physics' per-entity call. */
#define XBOX_FT_SV_ENTITY( ent, num, stmt ) \
	do { \
		xbox_ft_sv_ent_t xbox_ft_sv_e_; \
		xbox_ft_sv_e_.t0 = 0; \
		xbox_ft_sv_count( XBOX_FT_W_G_ENTS ); \
		if( xbox_ft_spans.on ) \
			Xbox_FT_SvGame_EntityBegin( &xbox_ft_sv_e_, ( ent ), ( num )); \
		stmt; \
		if( xbox_ft_sv_e_.t0 ) \
			Xbox_FT_SvGame_EntityEnd( &xbox_ft_sv_e_ ); \
	} while( 0 )

#else /* !( XASH_XBOX && !XASH_DEDICATED ) */

#define XBOX_FT_SV_GAME_BEGIN()                ((void)0)
#define XBOX_FT_SV_GAME_END()                  ((void)0)
#define XBOX_FT_SV_COUNT( w )                  ((void)0)
#define XBOX_FT_SV_ADD( w, n )                 ((void)0)
#define XBOX_FT_SV_TIMED( timer, w, stmt )     stmt
#define XBOX_FT_SV_ENTITY( ent, num, stmt )    stmt

#endif /* XASH_XBOX && !XASH_DEDICATED */

#endif /* XBOX_FRAMETIME_SVGAME_H */
