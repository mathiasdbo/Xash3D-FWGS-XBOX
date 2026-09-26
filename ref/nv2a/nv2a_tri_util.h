/*
nv2a_tri_util.h - X9-X step 4 Part A: the pure-arithmetic pieces of the
TriAPI (r_context.c's Begin/Vertex3f/End) factored out so the primitive
decomposition builds and is unit-tested on the host, without pbkit,
xboxkrnl or engine includes - the same split nv2a_2d_util.h already uses
for the 2D pass (see that header's own comment). The register pushes
themselves stay in r_context.c and call into these.

Winding and primitive-splitting rules cited in each function follow the
GL 1.x spec (glBegin/glEnd), since the client DLL and the engine's own
r_showhull/netgraph/r_showtree callers were all written against that
spec through ref/gl's TriAPI - see ref/gl/gl_triapi.c:81-113 for the
TRI_* -> GL primitive mapping this header's topology enum mirrors.
*/
#ifndef NV2A_TRI_UTIL_H
#define NV2A_TRI_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single NV097_DRAW_ARRAYS instruction can express at most 256
   vertices (the COUNT field is 8 bits, MASK(COUNT, n-1) - nv_regs.h's
   own NV097_DRAW_ARRAYS_COUNT). Shared with the 2D pass - see
   nv2a_2d_util.h, which now defines this in terms of nv2a_tri_chunk(). */
#define NV2A_2D_MAX_DRAW_VERTS 256u

/* The engine-facing TRI_* constants (common/triangleapi.h:27-34) mapped
   onto how each is actually assembled into hardware list primitives -
   NOT the hardware op codes themselves (those are chosen per emitted
   primitive: POINTS/LINES/TRIANGLES/QUADS, see nv2a_tri_output_op()). */
typedef enum
{
	NV2A_TRI_LIST_POINTS,
	NV2A_TRI_LIST_LINES,
	NV2A_TRI_LIST_TRIANGLES,
	NV2A_TRI_LIST_QUADS,
	NV2A_TRI_FAN,   /* also serves TRI_POLYGON - same assembly, GL treats them identically */
	NV2A_TRI_STRIP,
	NV2A_TRI_QUAD_STRIP
} nv2a_tri_topology_t;

/* One staged vertex - the assembler's input unit. World or 2D space,
   colour always present (Color4f/Color4ub latch into rgba before the
   vertex is pushed - see r_context.c's Vertex3f). */
typedef struct
{
	float   x, y, z;
	float   u, v;
	uint8_t rgba[4];
} nv2a_tri_vert_t;

/* Called once per COMPLETE list primitive the assembler emits, in push
   order. n is 1 (points), 2 (lines), 3 (triangles, from LIST_TRIANGLES,
   FAN or STRIP) or 4 (quads, from LIST_QUADS or QUAD_STRIP). Never
   called with an incomplete trailing primitive - see nv2a_tri_end(). */
typedef void (*nv2a_tri_emit_fn)( const nv2a_tri_vert_t *v, uint32_t n, void *ctx );

/* O(1)-memory assembler state: never buffers more than one primitive's
   worth of vertices (v0 plus the last two, for FAN/STRIP/QUAD_STRIP -
   see nv2a_tri_begin()'s own comment). */
typedef struct
{
	nv2a_tri_topology_t topology;
	uint32_t            count; /* vertices pushed so far this Begin/End */
	nv2a_tri_vert_t     v0;    /* FAN/POLYGON anchor (topology's vertex 0) */
	nv2a_tri_vert_t     buf[4]; /* rolling window - see nv2a_tri_push() per topology */
	nv2a_tri_emit_fn    emit;
	void               *ctx;
} nv2a_tri_asm_t;

/* Starts assembling a new primitive stream. `emit`/`ctx` may be NULL to
   silently discard every push (a caller can still call push()/end() on a
   dropped Begin without a NULL check at every call site). */
void nv2a_tri_begin( nv2a_tri_asm_t *a, nv2a_tri_topology_t topology, nv2a_tri_emit_fn emit, void *ctx );

/* Stages one vertex, emitting any list primitive it completes. See
   nv2a_tri_util.c for the winding of each topology:
   - LIST_POINTS/LIST_LINES/LIST_TRIANGLES/LIST_QUADS: fixed-size groups.
   - FAN (and POLYGON): (v0, v[i-1], v[i]) for i >= 2.
   - STRIP: (v[i], v[i+1], v[i+2]) for even i, (v[i+1], v[i], v[i+2]) for
     odd i.
   - QUAD_STRIP: quad k is (v[2k], v[2k+1], v[2k+3], v[2k+2]). */
void nv2a_tri_push( nv2a_tri_asm_t *a, const nv2a_tri_vert_t *v );

/* Ends the primitive stream. Any trailing vertices that never completed
   a primitive (fewer than 3 for FAN/STRIP, fewer than 2/3/4 for
   LINES/TRIANGLES/QUADS) are dropped, exactly as GL drops them - never
   emitted, never carried into the next Begin. */
void nv2a_tri_end( nv2a_tri_asm_t *a );

/* The hardware BEGIN_END op (NV097_SET_BEGIN_END_OP_*, named in
   r_context.c which has the pbkit include) each topology's OUTPUT list
   uses - callers key their draw's op off this, not off `topology`
   itself, since FAN/STRIP both output TRIANGLES and QUAD_STRIP outputs
   QUADS. Returned as the number of vertices per output primitive (1, 2,
   3 or 4) so callers needing both the count and the register value can
   derive the latter with their own named constant per verts-per-prim,
   without this pbkit-free file naming a pbkit constant itself. */
uint32_t nv2a_tri_verts_per_output_prim( nv2a_tri_topology_t topology );

typedef void (*nv2a_tri_chunk_emit_fn)( uint32_t start, uint32_t count, void *ctx );

/* Splits `vertex_count` vertices (already whole output primitives - a
   multiple of `verts_per_prim`) into chunks of at most
   NV2A_2D_MAX_DRAW_VERTS, each itself a multiple of `verts_per_prim`,
   calling `emit(start, count, ctx)` once per chunk, in ascending order,
   start/count both relative to whatever base the caller already applied.
   A `vertex_count` that is not itself a multiple of `verts_per_prim` is a
   bug upstream of this function - the remainder is silently dropped
   rather than ever emitting a partial-primitive chunk. `verts_per_prim`
   of 3 caps a chunk at 255 (256 is not a multiple of 3), matching the
   spec's own worked example (300 triangle vertices -> 255 + 45). */
void nv2a_tri_chunk( uint32_t vertex_count, uint32_t verts_per_prim, nv2a_tri_chunk_emit_fn emit, void *ctx );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TRI_UTIL_H */
