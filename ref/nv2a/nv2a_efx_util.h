/*
nv2a_efx_util.h - X9-X step 7: the pure-arithmetic pieces of ref/nv2a's
effects renderer (r_effects.c, a port of ref/gl/gl_beams.c and
ref/gl/gl_rpart.c), factored out so they build and are unit-tested on the
host without pbkit/xboxkrnl/engine includes - the same split
nv2a_sprite_util.h uses for the sprite port.

Two things live here:

- The beam noise generators, FracNoise and SineNoise (gl_beams.c:43-68),
  with the engine's COM_RandomFloat passed in so a test can drive it.
- Texture repeat for the TriAPI. ref/gl samples beam and tracer sprites
  with GL_REPEAT (the engine loads world sprite frames with no texture
  flags, cl_game.c:1210 sets them for HUD sprites only), and every beam
  scrolls its V coordinate over many repeats: R_DrawSegs advances it by
  length * 0.01 per unit of beam (gl_beams.c:167-173), so a 500-unit
  beam spans five copies of its texture. ref/nv2a's 2D texture manager
  stores linear textures, which the NV2A can only clamp. The same
  problem R_DrawStretchPic solves by splitting a tiled quad per cell
  (r_context.c, divergences.md #100) is solved here for an arbitrary
  TriAPI primitive: it is clipped at every texture-cell boundary it
  crosses, and each piece is shifted back into the first cell.
*/
#ifndef NV2A_EFX_UTIL_H
#define NV2A_EFX_UTIL_H

#include <stdint.h>
#include "nv2a_tri_util.h" /* nv2a_tri_vert_t */

#ifdef __cplusplus
extern "C" {
#endif

/* gl_beams.c:25 - "don't touch - many tripmines cause the crash when it
   equal 128". */
#define NV2A_EFX_NOISE_DIVISIONS 64

/* gEngfuncs.COM_RandomFloat's signature. */
typedef float (*nv2a_efx_random_fn)( float flLow, float flHigh );

/* FracNoise (gl_beams.c:43): fractal noise, power-of-two wavelength.
   Fills noise[1 .. divs-1] from noise[0] and noise[divs], which the
   caller sets (R_BeamDraw zeroes both, gl_beams.c:912-913). */
void nv2a_efx_frac_noise( float *noise, int divs, nv2a_efx_random_fn random_float );

/* SineNoise (gl_beams.c:59): noise[i] = sin( i * pi / divs ) for
   i < divs; noise[divs] is not written. */
void nv2a_efx_sine_noise( float *noise, int divs );

/* A primitive's texture coordinates may span at most this many cells
   per axis before the splitter gives up and clamps instead (it then
   emits the primitive whole, shifted to its first cell). No beam gets
   near it: a segment's V span is length * 0.01 / (segments - 1) and
   R_DrawSegs keeps at least two segments; this only bounds the work a
   malformed input can cause. */
#define NV2A_EFX_WRAP_MAX_CELLS 64

/* Called for each piece: `n` is the input's own vertex count when the
   primitive fit in one cell (`split` 0, the vertices only shifted), or 3
   when it was clipped (`split` 1, always triangles, in the input's
   winding). */
typedef void (*nv2a_efx_wrap_emit_fn)( const nv2a_tri_vert_t *v, uint32_t n, int split, void *ctx );

/* Emits primitive `v` (n vertices: 1 point, 2 line, 3 triangle or 4
   quad, as nv2a_tri_util.c emits them) so that every emitted texture
   coordinate lies in [0, cell_u] x [0, cell_v] - the texture's own size
   in the units the vertices carry (texels for a linear texture, 1 for a
   normalized one) - sampling what GL_REPEAT would sample. Position and
   colour are interpolated linearly along the clipped edges, which is
   exact: texture coordinates are affine over a triangle in world space.
   Points and lines are shifted by their first cell, never split. A
   non-positive cell size emits the primitive unchanged. */
void nv2a_efx_wrap_primitive( const nv2a_tri_vert_t *v, uint32_t n, float cell_u, float cell_v,
	nv2a_efx_wrap_emit_fn emit, void *ctx );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_EFX_UTIL_H */
