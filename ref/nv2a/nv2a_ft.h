/*
nv2a_ft.h - ref/nv2a's side of the frame-time recorder's work counters
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
Resonance3D (docs/r3d/divergences.md #141, format v2). The recorder in
engine/platform/xbox/xbox_frametime.c zeroes xbox_ft_work at Host_Frame
start and stores it in the frame record; this renderer adds to it where
the work is submitted. Each site is one add to a global - no test of
whether the recorder is on, which would cost as much as the add. With the
recorder off the counts accumulate and wrap, harmlessly. What each
counter means: XBOX_FT_W_* in xbox_frametime_util.h.

Not upstream: ref/gl keeps the r_stats counters (gl_local.h) for r_speeds;
these are this renderer's own, for the file and the overlay. Several sit
next to an existing per-feature stats block (nv2a_lm_stats,
nv2a_warp_stats, nv2a_decal_stats, the efx/sprite/studio r_stats ports),
which keep their own lifetimes and are unchanged.
*/

#ifndef NV2A_FT_H
#define NV2A_FT_H

#include "platform/xbox/xbox_frametime.h"

/* The per-subsystem CPU spans (M2, xbox_ft_spans) are timed with
   XBOX_FT_SPAN / xbox_ft_span_begin+end from xbox_frametime.h: k_lm,
   k_world, k_efx, k_tri_hooks (r_context.c), k_client_studio,
   k_studio_points (r_studio.c), k_skin_read (r_image.c). Unlike the
   counts these test the recorder first - two rdtsc are not free. */

#define NV2A_FT_ADD( id, n )  ( xbox_ft_work.c[( id )] += (uint32_t)( n ))
#define NV2A_FT_SET( id, v )  ( xbox_ft_work.c[( id )] = (uint32_t)( v ))

/* The counter a 3D TriAPI vertex lands in: XBOX_FT_W_VERTS_TRI, switched
   to XBOX_FT_W_VERTS_STUDIO by r_studio.c around a model's mesh loop
   (R_StudioDrawPoints) - the studio port draws through the TriAPI. */
extern uint32_t nv2a_ft_imm3d_counter;

/* r_studio.c's running studio-model count (ref/gl's r_stats
   c_studio_models_drawn), which the game client also increments. */
uint32_t nv2a_studio_models_drawn( void );

/* x9-perf-cpuprof (XBOX_FT_W_ST_*): r_studio.c's studio times, summed in
   ticks, rounded into the counters at R_EndFrame; and the per-model note
   thresholds, cleared at R_NewMap. */
void nv2a_ft_studio_close_frame( void );
void nv2a_ft_studio_new_map( void );

#endif /* NV2A_FT_H */
