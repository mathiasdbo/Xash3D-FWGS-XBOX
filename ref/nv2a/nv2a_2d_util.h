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

/* Menu GPU-memory compression (docs/r3d/divergences.md #131). What an
   RGBA32 image's texels allow it to be stored as without changing a
   single sampled value:
   - NV2A_TEXELS_WHITE_ALPHA: every texel's R, G and B are 255 - stored as
     LU_IMAGE_A8, which samples as (1, 1, 1, a) (xemu
     hw/xbox/nv2a/pgraph/gl/constants.h: swizzle {ONE, ONE, ONE, RED};
     Cxbx-Reloaded XbConvert.cpp: X_D3DFMT_LIN_A8 "R=G=B=255").
   - NV2A_TEXELS_GREY_ALPHA: every texel has R == G == B - stored as
     LU_IMAGE_A8Y8, byte 0 = Y (= R), byte 1 = A, which samples as
     (Y, Y, Y, A) (xemu: GL_RG8, swizzle {RED, RED, RED, GREEN};
     Cxbx: X_D3DFMT_LIN_A8L8 "R=G=B=L").
   - NV2A_TEXELS_RGBA: anything else, kept A8B8G8R8.
   Exact by construction: the class is decided on every texel, so a
   single coloured texel keeps the whole image at 32 bpp. */
typedef enum
{
	NV2A_TEXELS_RGBA = 0,
	NV2A_TEXELS_GREY_ALPHA,
	NV2A_TEXELS_WHITE_ALPHA
} nv2a_texel_class_t;

nv2a_texel_class_t nv2a_2d_classify_rgba( const uint8_t *rgba, uint32_t texel_count );

/* Packs one row of `width` RGBA32 texels (bytes R,G,B,A) into A8 (one
   byte: A) or A8Y8 (two bytes: R, then A) - the layouts described above. */
void nv2a_2d_pack_row_a8( uint8_t *dst, const uint8_t *rgba, uint32_t width );
void nv2a_2d_pack_row_a8y8( uint8_t *dst, const uint8_t *rgba, uint32_t width );

/* log2(v) if v is a power of two in [1, 4096] (the NV2A's texture size
   limit; the FORMAT word's BASE_SIZE_U/V fields hold log2), else -1. */
int nv2a_2d_log2_pot( uint32_t v );

/* Bytes of mip level 0 of a width x height block-compressed image with
   `block_bytes`-byte 4x4 blocks (8 for DXT1, 16 for DXT3/DXT5), each
   axis rounded up to whole blocks - the level-0 term of img_dds.c's own
   Image_DXTCalcMipmapSize. */
uint32_t nv2a_2d_dxt_level0_bytes( uint32_t width, uint32_t height, uint32_t block_bytes );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_2D_UTIL_H */
