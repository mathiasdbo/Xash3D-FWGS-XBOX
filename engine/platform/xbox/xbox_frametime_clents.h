/*
xbox_frametime_clents.h - the frame-time recorder's client entity breakdown
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
Resonance3D (docs/r3d/divergences.md #141, the recorder; the client
entity breakdown of 2026-10-07, x9-perf-clents). The client's side of
the local server message cost 0.5-0.7 ms a frame on the console in two
spans with little under them: k_cl_pkt_ents (CL_ParsePacketEntities:
every entity's delta, the frame's bookkeeping, CL_ProcessPacket and a
CL_SetSolidEntities) and k_cl_emit (CL_EmitEntities: linking the visible
entities, temp entities, events). This header counts that work and
splits that time:

  - XBOX_FT_W_CE_*: the entities in each parsed frame, the deltas read
    off the wire (MSG_ReadDeltaEntity calls that walk the fields), those
    that changed no field, the fields decoded, and the entities the link
    passes handed to CL_AddVisibleEntity that it accepted;
  - the XBOX_FT_W_CE_*_KC times, accumulated here in ticks (the
    renderer's waits taken out, like a span) and stored in kilocycles
    when the frame closes. ce_add_kc is part of ce_link_kc. ce_fields_kc
    times the field loop of every MSG_ReadDeltaEntity, ce_delta_kc only
    the frame's deltas (CL_DeltaEntity): in play the first holds the
    second's field loops, but at a signon the baselines and static
    entities add field time with no delta time.

With the recorder off a count is one add and a timer one load and branch
of xbox_ft_spans.on; outside an Xbox build every macro is the plain
upstream statement or nothing.
*/

#ifndef XBOX_FRAMETIME_CLENTS_H
#define XBOX_FRAMETIME_CLENTS_H

#if XASH_XBOX && !XASH_DEDICATED

#include "xbox_frametime.h"

/* The timers, stored as XBOX_FT_W_CE_*_KC. */
enum
{
	XBOX_FT_CE_DELTA = 0, /* MSG_ReadDeltaEntity from CL_DeltaEntity */
	XBOX_FT_CE_FIELDS,    /*   its field loop */
	XBOX_FT_CE_PROCESS,   /* CL_ProcessPacket */
	XBOX_FT_CE_LINK,      /* CL_LinkPlayers + CL_LinkPacketEntities */
	XBOX_FT_CE_ADD,       /*   their CL_AddVisibleEntity calls */
	XBOX_FT_CE_TENTS,     /* the client's CreateEntities + CL_TempEntUpdate */
	XBOX_FT_CE_EVENTS,    /* CL_FireEvents */
	XBOX_FT_CE_PITCH,     /* CL_SetIdealPitch */
	XBOX_FT_CE_TIMERS
};

typedef struct xbox_ft_ce_s
{
	uint64_t ticks[XBOX_FT_CE_TIMERS];
} xbox_ft_ce_t;

extern xbox_ft_ce_t xbox_ft_ce;

/* Stores the timers into the XBOX_FT_W_CE_*_KC counters and zeroes them
   (FT_CloseFrame, before the counters are copied). */
void Xbox_FT_ClEnts_CloseFrame( void );

static inline void xbox_ft_ce_timer_end( const xbox_ft_span_t *s, int timer )
{
	if( s->t0 )
	{
		xbox_ft_ce.ticks[timer] += xbox_ft_span_ticks_since( s );
		xbox_ft_spans.calls++;
	}
}

/* `stmt`, timed as `timer`. */
#define XBOX_FT_CE_TIMED( timer, stmt ) \
	do { \
		xbox_ft_span_t xbox_ft_ce_s_; \
		xbox_ft_span_begin( &xbox_ft_ce_s_ ); \
		stmt; \
		xbox_ft_ce_timer_end( &xbox_ft_ce_s_, ( timer )); \
	} while( 0 )

/* A run of statements timed as `timer`: two inserted lines, no re-indent. */
#define XBOX_FT_CE_BEGIN( var ) xbox_ft_span_t var; xbox_ft_span_begin( &var )
#define XBOX_FT_CE_END( var, timer ) xbox_ft_ce_timer_end( &var, ( timer ))

/* Adds `n` to counter `w`. */
#define XBOX_FT_CE_ADD( w, n ) ( xbox_ft_work.c[( w )] += (uint32_t)( n ))

/* One delta's field walk, `expr` returning how many fields had their
   change bit set (Delta_ReadFieldRuns): counts the delta, its fields,
   and the delta as unchanged when that is 0. A statement. */
#define XBOX_FT_CE_DELTA( expr ) \
	do { \
		const uint32_t xbox_ft_ce_n_ = (uint32_t)( expr ); \
		xbox_ft_work.c[XBOX_FT_W_CE_DELTAS]++; \
		xbox_ft_work.c[XBOX_FT_W_CE_FIELDS] += xbox_ft_ce_n_; \
		if( !xbox_ft_ce_n_ ) \
			xbox_ft_work.c[XBOX_FT_W_CE_NOCHANGE]++; \
	} while( 0 )

/* CL_AddVisibleEntity( ent, type ) from a link pass: timed as ce_add,
   counted into ce_visents when it accepts the entity. */
#define XBOX_FT_CE_ADD_VISIBLE( call ) \
	do { \
		xbox_ft_span_t xbox_ft_ce_s_; \
		xbox_ft_span_begin( &xbox_ft_ce_s_ ); \
		if( call ) \
			xbox_ft_work.c[XBOX_FT_W_CE_VISENTS]++; \
		xbox_ft_ce_timer_end( &xbox_ft_ce_s_, XBOX_FT_CE_ADD ); \
	} while( 0 )

#else /* !( XASH_XBOX && !XASH_DEDICATED ) */

#define XBOX_FT_CE_TIMED( timer, stmt )       stmt
#define XBOX_FT_CE_BEGIN( var )
#define XBOX_FT_CE_END( var, timer )
#define XBOX_FT_CE_ADD( w, n )                ((void)0)
#define XBOX_FT_CE_DELTA( expr )              ((void)( expr ))
#define XBOX_FT_CE_ADD_VISIBLE( call )        ((void)( call ))

#endif /* XASH_XBOX && !XASH_DEDICATED */

#endif /* XBOX_FRAMETIME_CLENTS_H */
