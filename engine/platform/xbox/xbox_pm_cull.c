/*
xbox_pm_cull.c - exact per-physent early-outs for the player movement's
hull traces and position tests
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

/* Resonance3D (docs/r3d/divergences.md #154). Why a skipped physent leaves
   the result unchanged, and what the bound is: xbox_pm_cull.h. */

#include "common.h"

#if XASH_XBOX

#include "mod_local.h"
#include "xbox_pm_cull.h"

/* How far outside a node's axial planes PM_RecursiveHullCheck can take a
   point it classifies (DIST_EPSILON, 1/32, plus the rounding of nested
   lerps, under a unit at the engine's coordinate range), with room to
   spare; and the same again around the whole bound, for the split points'
   rounding outside the segment's own box. A larger margin only skips
   fewer physents. */
#define XBOX_PM_CULL_MARGIN    8.0f
#define XBOX_PM_CULL_INFINITY  1.0e30f
#define XBOX_PM_CULL_BUDGET    4096   /* nodes walked before a hull is called unbounded (the world's) */
#define XBOX_PM_CULL_STACK     128    /* pending nodes; deeper is called unbounded */
#define XBOX_PM_CULL_SLOTS     512    /* cached hulls, a power of two (16 KiB) */
#define XBOX_PM_CULL_PROBES    8

/* What a hull's tree allows. */
enum
{
	XBOX_PM_CULL_UNBOUNDED = 0, /* a solid leaf the axial planes do not bound, or a tree not walked: never skipped */
	XBOX_PM_CULL_BOUNDED,       /* every solid leaf is inside lo..hi */
	XBOX_PM_CULL_NO_SOLID,      /* no solid leaf at all */
};

typedef struct xbox_pm_bound_s
{
	byte state;
	/* PM_RecursiveHullCheck answers "empty" before reading a node when
	   firstclipnode >= lastclipnode (pm_trace.c:217 in the reference);
	   PM_HullPointContents has no such test and walks the tree. */
	byte trace_empty;
	float lo[3], hi[3];
} xbox_pm_bound_t;

/* 32 bytes: one Pentium III cache line each. */
typedef struct xbox_pm_slot_s
{
	const hull_t *key;
	xbox_pm_bound_t b;
} xbox_pm_slot_t;

static xbox_pm_slot_t pm_cull_cache[XBOX_PM_CULL_SLOTS] __attribute__(( aligned( 32 )));
static qboolean pm_cull_dirty;
uint32_t xbox_model_generation; /* docs/r3d/divergences.md #156 */

void Xbox_PMCull_Invalidate( void )
{
	/* Mod_FreeModel calls this for every model freed at a map change;
	   only the first one after a bound was stored has anything to clear. */
	if( !pm_cull_dirty )
		return;
	memset( pm_cull_cache, 0, sizeof( pm_cull_cache ));
	pm_cull_dirty = false;
}

/* A plane distance the bound can use: not a NaN, not huge. A NaN would
   compare false everywhere and make a region look empty. */
static qboolean PMCull_Finite( float d )
{
	return d > -XBOX_PM_CULL_INFINITY && d < XBOX_PM_CULL_INFINITY;
}

typedef struct xbox_pm_pending_s
{
	int num;
	float lo[3], hi[3];
} xbox_pm_pending_t;

/* Walks the hull's tree from its first clip node the way
   PM_RecursiveHullCheck and PM_HullPointContents descend it, keeping each
   node's region as a box bounded by the axial planes above it (loosened
   by the margin), and unites the regions of the solid leaves. */
static __attribute__(( noinline )) void PMCull_Walk( const hull_t *hull, xbox_pm_bound_t *b )
{
	/* static: 3.5 KiB, kept off the stack of the game DLL's PM_Move; the
	   walk neither recurses nor runs on another thread. */
	static xbox_pm_pending_t stack[XBOX_PM_CULL_STACK];
	const qboolean bsp2 = world.version == QBSP2_VERSION;
	int depth = 0, visits = 0, solid = 0, i;

	b->state = XBOX_PM_CULL_UNBOUNDED;
	b->trace_empty = hull->firstclipnode >= 0 && hull->firstclipnode >= hull->lastclipnode;
	if( !hull->planes || !hull->clipnodes16 )
		return;

	for( i = 0; i < 3; i++ )
	{
		b->lo[i] = XBOX_PM_CULL_INFINITY;
		b->hi[i] = -XBOX_PM_CULL_INFINITY;
		stack[0].lo[i] = -XBOX_PM_CULL_INFINITY;
		stack[0].hi[i] = XBOX_PM_CULL_INFINITY;
	}
	stack[0].num = hull->firstclipnode;
	depth = 1;

	while( depth > 0 )
	{
		const xbox_pm_pending_t cur = stack[--depth];
		const mplane_t *plane;
		int c0, c1;

		if( cur.num < 0 )
		{
			if( cur.num != CONTENTS_SOLID )
				continue;
			for( i = 0; i < 3; i++ )
			{
				if( cur.lo[i] <= -XBOX_PM_CULL_INFINITY || cur.hi[i] >= XBOX_PM_CULL_INFINITY )
					return; /* a solid leaf no axial plane bounds on this axis */
				b->lo[i] = Q_min( b->lo[i], cur.lo[i] );
				b->hi[i] = Q_max( b->hi[i], cur.hi[i] );
			}
			solid++;
			continue;
		}

		/* A node PM_RecursiveHullCheck would stop at with Host_Error
		   (:225-226), or a tree too large or deep for this walk: left to
		   the reference code. */
		if( cur.num < hull->firstclipnode || cur.num > hull->lastclipnode )
			return;
		if( ++visits > XBOX_PM_CULL_BUDGET || depth + 2 > XBOX_PM_CULL_STACK )
			return;

		if( bsp2 )
		{
			plane = hull->planes + hull->clipnodes32[cur.num].planenum;
			c0 = hull->clipnodes32[cur.num].children[0];
			c1 = hull->clipnodes32[cur.num].children[1];
		}
		else
		{
			plane = hull->planes + hull->clipnodes16[cur.num].planenum;
			c0 = hull->clipnodes16[cur.num].children[0];
			c1 = hull->clipnodes16[cur.num].children[1];
		}

		{
			xbox_pm_pending_t front = cur, back = cur;

			front.num = c0;
			back.num = c1;
			/* PlaneDiff (xash3d_mathlib.h) reads p[type] - dist for an
			   axial plane, so child 0 holds p[type] >= dist and child 1
			   p[type] < dist; a loosened region left with nothing in it
			   cannot be reached. A non-axial plane bounds nothing. */
			if( plane->type < 3 )
			{
				const int t = plane->type;

				if( !PMCull_Finite( plane->dist ))
					return;
				front.lo[t] = Q_max( cur.lo[t], plane->dist - XBOX_PM_CULL_MARGIN );
				back.hi[t] = Q_min( cur.hi[t], plane->dist + XBOX_PM_CULL_MARGIN );
				if( front.lo[t] <= front.hi[t] )
					stack[depth++] = front;
				if( back.lo[t] <= back.hi[t] )
					stack[depth++] = back;
			}
			else
			{
				stack[depth++] = front;
				stack[depth++] = back;
			}
		}
	}

	if( !solid )
	{
		b->state = XBOX_PM_CULL_NO_SOLID;
		return;
	}

	for( i = 0; i < 3; i++ )
	{
		b->lo[i] -= XBOX_PM_CULL_MARGIN;
		b->hi[i] += XBOX_PM_CULL_MARGIN;
	}
	b->state = XBOX_PM_CULL_BOUNDED;
}

/* The box hull and the studio hitbox hulls are rebuilt for every physent
   (PM_HullForBox, Mod_HullForStudio): they share the box's six clip nodes
   (mod_bmodel.c, BOX_CLIPNODES_INITIALIZER), so they are never cached. */
static qboolean PMCull_Transient( const hull_t *hull )
{
	return hull->clipnodes16 == (const mclipnode16_t *)box_clipnodes16
		|| hull->clipnodes32 == (const mclipnode32_t *)box_clipnodes32;
}

/* What PMCull_Walk finds in the box's chain, without walking it: node i
   tests plane i, node 0 is p[0] >= dist (empty) or node 1, node 1 is
   p[0] >= dist (node 2) or empty, and so on to node 5, whose front is the
   one solid leaf - so with planes of types 0,0,1,1,2,2 the solid region
   is planes[2a+1].dist <= p[a] < planes[2a].dist. PM_HullForBox's planes
   are those (PM_InitBoxHull); a studio hitbox's are not axial (type 5,
   Mod_SetStudioHullPlane), and the walk would find that leaf unbounded. */
static void PMCull_Box( const hull_t *hull, xbox_pm_bound_t *b )
{
	int a;

	b->trace_empty = 0;
	b->state = XBOX_PM_CULL_UNBOUNDED;
	if( !hull->planes || hull->firstclipnode != 0 || hull->lastclipnode != 5 )
		return;
	for( a = 0; a < 6; a++ )
	{
		if( hull->planes[a].type != a >> 1 || !PMCull_Finite( hull->planes[a].dist ))
			return;
	}
	for( a = 0; a < 3; a++ )
	{
		const float lo = hull->planes[a * 2 + 1].dist - XBOX_PM_CULL_MARGIN;
		const float hi = hull->planes[a * 2].dist + XBOX_PM_CULL_MARGIN;

		if( lo > hi )
		{
			b->state = XBOX_PM_CULL_NO_SOLID;
			return;
		}
		b->lo[a] = lo - XBOX_PM_CULL_MARGIN;
		b->hi[a] = hi + XBOX_PM_CULL_MARGIN;
	}
	b->state = XBOX_PM_CULL_BOUNDED;
}

static const xbox_pm_bound_t pm_cull_unbounded; /* state 0: never skipped */

static const xbox_pm_bound_t *PMCull_Bound( const model_t *model, const hull_t *hull, xbox_pm_bound_t *scratch )
{
	xbox_pm_slot_t *s;
	uint32_t h, i;

	if( PMCull_Transient( hull ))
	{
		PMCull_Box( hull, scratch );
		return scratch;
	}

	/* The world's tree has open solid space around the map, so it is never
	   bounded; walking its 4,096-node budget to find that out would land
	   on the first trace with each hull - the first crouch, say. */
	if( model && FBitSet( model->flags, MODEL_WORLD ))
		return &pm_cull_unbounded;

	/* The top bits of the product: the low ones of a multiply depend only
	   on the low bits of the pointer. */
	h = (uint32_t)(((uintptr_t)hull >> 2 ) * 2654435761u ) >> 23;
	for( i = 0; i < XBOX_PM_CULL_PROBES; i++ )
	{
		s = &pm_cull_cache[( h + i ) & ( XBOX_PM_CULL_SLOTS - 1 )];
		if( s->key == hull )
			return &s->b;
		if( !s->key )
			break;
	}

	/* A free slot, or the probes are full: the home slot is replaced. */
	if( i == XBOX_PM_CULL_PROBES )
		s = &pm_cull_cache[h & ( XBOX_PM_CULL_SLOTS - 1 )];
	PMCull_Walk( hull, &s->b );
	s->key = hull;
	pm_cull_dirty = true;
	return &s->b;
}

int Xbox_PMCull_TraceMisses( const model_t *model, const hull_t *hull, const vec3_t start_l, const vec3_t end_l )
{
	xbox_pm_bound_t scratch;
	const xbox_pm_bound_t *b;
	int i;

	if( !hull )
		return 0;
	b = PMCull_Bound( model, hull, &scratch );
	if( b->trace_empty || b->state == XBOX_PM_CULL_NO_SOLID )
		return 1; /* no solid leaf at all, whatever the segment */
	if( b->state != XBOX_PM_CULL_BOUNDED )
		return 0;
	/* A NaN or infinite coordinate on any axis skips nothing: the
	   reference's split point is then NaN, which no bound describes. */
	for( i = 0; i < 3; i++ )
	{
		if( !PMCull_Finite( start_l[i] ) || !PMCull_Finite( end_l[i] ))
			return 0;
	}
	for( i = 0; i < 3; i++ )
	{
		if( start_l[i] < b->lo[i] && end_l[i] < b->lo[i] )
			return 1;
		if( start_l[i] > b->hi[i] && end_l[i] > b->hi[i] )
			return 1;
	}
	return 0;
}

int Xbox_PMCull_PointMisses( const model_t *model, const hull_t *hull, const vec3_t p )
{
	xbox_pm_bound_t scratch;
	const xbox_pm_bound_t *b;
	int i;

	if( !hull )
		return 0;
	b = PMCull_Bound( model, hull, &scratch );
	if( b->state == XBOX_PM_CULL_NO_SOLID )
		return 1;
	if( b->state != XBOX_PM_CULL_BOUNDED )
		return 0;
	for( i = 0; i < 3; i++ )
	{
		if( p[i] < b->lo[i] || p[i] > b->hi[i] )
			return 1;
	}
	return 0;
}

#endif /* XASH_XBOX */
