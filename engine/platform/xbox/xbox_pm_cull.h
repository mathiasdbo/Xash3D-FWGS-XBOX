/*
xbox_pm_cull.h - exact per-physent early-outs for the player movement's
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

/*
Resonance3D (docs/r3d/divergences.md #154). PM_PlayerTraceExt and
PM_TestPlayerPosition (engine/common/pm_trace.c) test every physent of the
list they are given, and the client's list is every solid entity of the
last packet (CL_AddLinksToPmove, cl_pmove.c), not the server's 256-unit
neighbourhood: on c1a0c the client's PM_Move ran ~340 hull checks a frame
against the server's ~40, for the same movement.

A hull check can change the result only through a CONTENTS_SOLID leaf:
PM_RecursiveHullCheck sets startsolid only in one (pm_trace.c:213 in the
reference), and sets fraction only where PM_HullPointContents of a split
point is one (:274, :296-313); a check that reaches no solid leaf leaves
fraction at 1 and startsolid clear, and PM_PlayerTraceExt keeps a
physent's result only when its fraction is lower than the best so far,
which starts at 1 (:518). PM_TestPlayerPosition's own loop returns a
physent only when its point is in a solid leaf (:629-641). So a physent
whose hull has no solid leaf near the segment (or the point) is skipped
with the result unchanged, bit for bit.

"Near" is a box per hull: the union, over the hull's solid leaves, of the
region the axial planes on the path to each leaf bound, each plane
loosened by XBOX_PM_CULL_MARGIN, then the whole grown by it again.
Loosened because PM_RecursiveHullCheck does not stay exactly inside a
node's region: the point where it splits a segment is DIST_EPSILON on the
near side of the plane, and it classifies that point from the far child
(:257-274), and every split point is a float lerp. A
non-axial plane does not bound anything, so a leaf behind only non-axial
planes on some axis makes the hull unbounded, and it is never skipped.
The bound is a property of the hull's tree alone; the box hull (a
physent's bounding box) and a studio hitbox hull are rebuilt for every
physent (PM_HullForBox, Mod_HullForStudio), so theirs is walked each time
(six nodes; a hitbox's planes are not axial, so it is never skipped),
and a brush model's is kept in a small cache keyed by the hull, emptied
whenever a model is loaded or freed. The world's is never computed: its
solid space is open around the map. The early-outs also serve the
client's traces outside PM_Move (EV_PlayerTrace, tempents), through the
same two functions.
*/

#ifndef XBOX_PM_CULL_H
#define XBOX_PM_CULL_H

/* Non-zero when a hull check of `hull` (hullcount 1, not SOLID_CUSTOM)
   along start_l -> end_l, both in the hull's own coordinates, cannot
   reach a solid leaf: PM_PlayerTraceExt can skip that physent. */
int Xbox_PMCull_TraceMisses( const model_t *model, const hull_t *hull, const vec3_t start_l, const vec3_t end_l );

/* The same for PM_TestPlayerPosition's point test of `hull` at `p`.
   `model` is the physent's brush model (NULL for a box or a hitbox): the
   world's hulls are never skipped, without walking them. */
int Xbox_PMCull_PointMisses( const model_t *model, const hull_t *hull, const vec3_t p );

/* Forgets every cached bound: a model is being loaded or freed
   (engine/common/model.c Mod_FreeModel, mod_bmodel.c Mod_LoadBrushModel). */
void Xbox_PMCull_Invalidate( void );

/* Bumped whenever a model is loaded or freed (Mod_LoadModel,
   Mod_FreeModel): the client's physent-list skip (xbox_cl_solid.c,
   docs/r3d/divergences.md #156) keys on it, since a rebuild reads the
   models' types, flags, bounds and clip hulls. */
extern uint32_t xbox_model_generation;

#endif /* XBOX_PM_CULL_H */
