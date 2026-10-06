/*
xbox_frametime_svgame.c - the frame-time recorder's server game frame breakdown
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
Resonance3D (docs/r3d/divergences.md #141; what is measured and why:
xbox_frametime_svgame.h). The state behind the engine/server macros, and
the per-classname table the recorder writes as CLASSES chunks.

The table costs nothing until recording starts: it is allocated by
Xbox_FT_SvGame_Start and freed by Xbox_FT_SvGame_Stop, and an entity is
looked up only while the spans are on. A lookup is keyed by the
classname's string pointer (SV_GetString of the edict's string_t): one
hash probe in the common case; a new pointer costs one string compare per
known class, once per map, since the cache is cleared whenever the server
respawns (svs.spawncount) and the strings it points at may be freed.
*/

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "server.h"
#include "xbox_frametime.h"
#include "xbox_frametime_util.h"
#include "xbox_frametime_svgame.h"

xbox_ft_sv_t xbox_ft_sv;

#define XBOX_FT_SV_CACHE 512u   /* pointer -> row; a power of two over twice the rows */

typedef struct xbox_ft_sv_cache_s
{
	const char *key;
	int row;
} xbox_ft_sv_cache_t;

typedef struct xbox_ft_sv_table_s
{
	xbox_ft_class_t rows[XBOX_FT_CLASSES_MAX];
	uint32_t count;
	int spawncount;                     /* the server the cache's pointers belong to */
	xbox_ft_sv_cache_t cache[XBOX_FT_SV_CACHE];
	uint8_t encode[XBOX_FT_CHUNK_BYTES + 4u + XBOX_FT_CLASSES_MAX * XBOX_FT_CLASS_BYTES];
} xbox_ft_sv_table_t;

static xbox_ft_sv_table_t *ft_sv;

qboolean Xbox_FT_SvGame_Start( void )
{
	ft_sv = Z_Calloc( sizeof( *ft_sv ));
	if( !ft_sv )
		return false;
	ft_sv->spawncount = svs.spawncount;
	return true;
}

void Xbox_FT_SvGame_Stop( void )
{
	if( ft_sv )
		Z_Free( ft_sv );
	ft_sv = NULL;
}

void Xbox_FT_SvGame_BeginFrame( int in_play )
{
	/* A Host_Error longjmp out of SV_Physics would leave in_game and the
	   depths set; frames start outside it. */
	memset( &xbox_ft_sv, 0, sizeof( xbox_ft_sv ));
	xbox_ft_sv.worst_cls = -1;
	xbox_ft_sv.in_play = in_play;
}

void Xbox_FT_SvGame_CloseFrame( void )
{
	xbox_ft_work.c[XBOX_FT_W_G_THINK_KC] = xbox_ft_span_kcyc( xbox_ft_sv.ticks[XBOX_FT_SV_THINK] );
	xbox_ft_work.c[XBOX_FT_W_G_TOUCH_KC] = xbox_ft_span_kcyc( xbox_ft_sv.ticks[XBOX_FT_SV_TOUCH] );
	xbox_ft_work.c[XBOX_FT_W_G_MOVE_KC] = xbox_ft_span_kcyc( xbox_ft_sv.ticks[XBOX_FT_SV_MOVE] );
	xbox_ft_work.c[XBOX_FT_W_G_WORST_KC] = xbox_ft_span_kcyc( xbox_ft_sv.worst );
	xbox_ft_work.c[XBOX_FT_W_G_WORST_CLS] = xbox_ft_sv.worst_cls >= 0 ? (uint32_t)xbox_ft_sv.worst_cls + 1u : 0u;
	xbox_ft_work.c[XBOX_FT_W_G_WORST_ENT] = xbox_ft_sv.worst ? (uint32_t)xbox_ft_sv.worst_ent : 0u;
}

/* The row for a classname string, adding it the first time it is seen. */
static int FT_SvClassRow( const char *name )
{
	uint32_t h, i;
	int row;

	if( ft_sv->spawncount != svs.spawncount )
	{
		memset( ft_sv->cache, 0, sizeof( ft_sv->cache ));
		ft_sv->spawncount = svs.spawncount;
	}

	h = (uint32_t)(( (uintptr_t)name >> 2 ) * 2654435761u ) & ( XBOX_FT_SV_CACHE - 1 );
	for( i = 0; i < XBOX_FT_SV_CACHE; i++, h = ( h + 1 ) & ( XBOX_FT_SV_CACHE - 1 ))
	{
		if( ft_sv->cache[h].key == name )
			return ft_sv->cache[h].row;
		if( !ft_sv->cache[h].key )
			break;
	}

	/* A pointer not seen on this server: find the name, or add it. */
	for( row = 0; row < (int)ft_sv->count; row++ )
	{
		if( !Q_strncmp( ft_sv->rows[row].name, name, XBOX_FT_CLASS_NAME_BYTES - 1 ))
			break;
	}
	if( row == (int)ft_sv->count )
	{
		if( ft_sv->count < XBOX_FT_CLASSES_MAX - 1 )
		{
			Q_strncpy( ft_sv->rows[row].name, name, sizeof( ft_sv->rows[row].name ));
			ft_sv->count++;
		}
		else
		{
			/* The last row takes every class past the table's size. */
			row = XBOX_FT_CLASSES_MAX - 1;
			Q_strncpy( ft_sv->rows[row].name, "(other classes)", sizeof( ft_sv->rows[row].name ));
			ft_sv->count = XBOX_FT_CLASSES_MAX;
		}
	}

	/* A full cache (more pointers than XBOX_FT_SV_CACHE on one server)
	   just stops caching: the loop above found no free slot. */
	if( i < XBOX_FT_SV_CACHE )
	{
		ft_sv->cache[h].key = name;
		ft_sv->cache[h].row = row;
	}
	return row;
}

static int FT_SvMovetypeSpan( int movetype )
{
	switch( movetype )
	{
	case MOVETYPE_STEP:
	case MOVETYPE_PUSHSTEP:
		return XBOX_FT_S_SV_PSTEP;
	case MOVETYPE_PUSH:
		return XBOX_FT_S_SV_PPUSH;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
	case MOVETYPE_BOUNCEMISSILE:
		return XBOX_FT_S_SV_PTOSS;
	case MOVETYPE_NONE:
		return XBOX_FT_S_SV_PNONE;
	default:
		return XBOX_FT_S_SV_POTHER;
	}
}

void Xbox_FT_SvGame_EntityBegin( xbox_ft_sv_ent_t *e, edict_t *ent, int num )
{
	const char *name;

	e->movetype = ent->v.movetype;
	e->span = FT_SvMovetypeSpan( e->movetype );
	e->num = num;
	e->cls = -1;
	/* Read before the entity runs: it may free itself. */
	if( ft_sv && xbox_ft_sv.in_play )
	{
		name = ent->v.classname ? SV_GetString( ent->v.classname ) : NULL;
		e->cls = FT_SvClassRow( COM_StringEmptyOrNULL( name ) ? "(no classname)" : name );
	}
	e->think0 = xbox_ft_sv.ticks[XBOX_FT_SV_THINK];
	e->thinks0 = xbox_ft_work.c[XBOX_FT_W_G_THINKS];
	e->moves0 = xbox_ft_work.c[XBOX_FT_W_G_MOVES];
	e->t0 = xbox_ft_rdtsc();
}

void Xbox_FT_SvGame_EntityEnd( const xbox_ft_sv_ent_t *e )
{
	const uint64_t d = xbox_ft_rdtsc() - e->t0;

	xbox_ft_spans.t[e->span] += d;
	xbox_ft_spans.calls++;

	if( d > xbox_ft_sv.worst )
	{
		xbox_ft_sv.worst = d;
		xbox_ft_sv.worst_cls = e->cls;
		xbox_ft_sv.worst_ent = e->num;
	}

	if( e->cls >= 0 && ft_sv )
	{
		xbox_ft_class_t *c = &ft_sv->rows[e->cls];

		c->movetype = (uint32_t)e->movetype;
		c->runs++;
		c->thinks += xbox_ft_work.c[XBOX_FT_W_G_THINKS] - e->thinks0;
		c->moves += xbox_ft_work.c[XBOX_FT_W_G_MOVES] - e->moves0;
		c->phys += d;
		c->think += xbox_ft_sv.ticks[XBOX_FT_SV_THINK] - e->think0;
		if( d > c->worst )
			c->worst = d;
	}
}

size_t Xbox_FT_SvGame_EncodeClasses( const uint8_t **out )
{
	size_t bytes;
	uint32_t i;

	*out = NULL;
	if( !ft_sv || !ft_sv->count )
		return 0;

	bytes = xbox_ft_encode_classes( ft_sv->encode, ft_sv->rows, ft_sv->count );
	*out = ft_sv->encode;

	/* The next chunk covers the next map: the names stay (their indices
	   are what g_worst_cls refers to), the totals restart. */
	for( i = 0; i < ft_sv->count; i++ )
	{
		xbox_ft_class_t *c = &ft_sv->rows[i];

		c->runs = c->thinks = c->moves = 0;
		c->phys = c->think = c->worst = 0;
	}
	return bytes;
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
