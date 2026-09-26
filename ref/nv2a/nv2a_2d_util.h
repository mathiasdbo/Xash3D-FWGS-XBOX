/*
nv2a_2d_util.h - X9-X Part C: the pure-arithmetic pieces of the 2D pass
(r_context.c's own flush, C4/C5/C6) factored out so they build and are
unit-tested on the host without pbkit/xboxkrnl/engine includes - see
tools/host-tests/nv2a_arena_host_test.c. The register pushes themselves
(which DO need pbkit) stay in r_context.c and call into these.
*/
#ifndef NV2A_2D_UTIL_H
#define NV2A_2D_UTIL_H

#include <stdint.h>
#include "nv2a_tri_util.h" /* NV2A_2D_MAX_DRAW_VERTS, nv2a_tri_chunk() - nv2a_2d_chunk_quads is now that generalisation with verts_per_prim=4 (X9-X step 4 Part A) */

#ifdef __cplusplus
extern "C" {
#endif

typedef nv2a_tri_chunk_emit_fn nv2a_2d_chunk_emit_fn;

/* Splits `vertex_count` vertices (already whole quads - a multiple of
   4) into chunks of at most NV2A_2D_MAX_DRAW_VERTS, each itself a
   multiple of 4, calling `emit(start, count, ctx)` once per chunk, in
   ascending order, start/count both relative to the ring cursor the
   caller already applied. A `vertex_count` that is not itself a
   multiple of 4 is a bug upstream of this function (a partial quad was
   appended) - the odd remainder is silently dropped rather than ever
   emitting a non-multiple-of-4 chunk. */
void nv2a_2d_chunk_quads( uint32_t vertex_count, nv2a_2d_chunk_emit_fn emit, void *ctx );

/* One post-split quad: a screen-space rect plus its own, now always
   within [0,1], texture-space rect. */
typedef struct
{
	float x, y, w, h;
	float s1, t1, s2, t2;
} nv2a_2d_tile_t;

/* C6: splits an s/t span that may extend outside [0,1] (a wrapped,
   tiled draw on a linear/unswizzled NV2A texture, which has no hardware
   wrap-addressing mode the way a normalized-coordinate texture would)
   into one quad per integer texture cell the span crosses, each
   remapped into [0,1] and given its proportional share of the
   screen-space rect (x,y,w,h assumed to map linearly onto the full
   s1..s2,t1..t2 span). Writes at most `max_tiles` entries to `out` and
   returns how many; a span that would need more is instead written as
   ONE tile covering the whole rect with s/t clamped into [0,1] (the
   "draw clamped" fallback), *clamped is set to 1, and the return value
   is 1 - the caller logs the DPrintf once, this function only signals
   it. A span that already fits in [0,1] on both axes returns exactly
   the input rect as a single tile (no rounding surprises for the
   common, non-wrapped case). */
uint32_t nv2a_2d_tile_split( float x, float y, float w, float h,
	float s1, float t1, float s2, float t2,
	nv2a_2d_tile_t *out, uint32_t max_tiles, int *clamped );

/* C5: the 2D viewport matrix (row-vector convention, v' = v * M),
   rebuilt at every 2D entry from the engine's own reported 2D size
   (gpGlobals->width/height) against the real back-buffer size
   (nv2a_width/nv2a_height) - NOT nv2a_matrix_viewport()'s own
   NDC-to-screen construction (r_context.c), which is a different matrix
   for a different (3D, z-ranged) purpose. Skip the draw entirely if
   engine_width/engine_height is 0 (the caller's own responsibility -
   this function still writes an all-zero row rather than divide by
   zero, so calling it unconditionally is harmless either way). */
void nv2a_2d_build_matrix( float out[4][4], float nv2a_width, float nv2a_height,
	float engine_width, float engine_height, float pixel_offset );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_2D_UTIL_H */
