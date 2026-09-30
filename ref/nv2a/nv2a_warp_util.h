/*
nv2a_warp_util.h - X9-X item 14 step 11: the pure-arithmetic pieces of
ref/nv2a's water warp and underwater fog (r_warp.c), factored out so they
build and are unit-tested on the host without pbkit/xboxkrnl/engine
includes - the same split nv2a_sky_util.h and nv2a_efx_util.h use.

What lives here, each a port of the reference function it names (root
clone c7768c48):

- SubdividePolygon_r (gl_rsurf.c:204): cut a face's polygon along axial
  SUBDIVIDE_SIZE (64) unit boundaries, so the per-vertex warp below has
  vertices close enough together to look like a wave.
- The per-vertex body of EmitWaterPolys (gl_rsurf.c:926): the wave height
  (entity scale) and the sin-table texture coordinates, both driven by
  cl.time through r_turbsin (engine/warpsin.h).
- The NV2A fog registers for GL_EXP and GL_LINEAR fog: NV097_SET_FOG_PARAMS
  words for the density or start/end R_DrawFog (gl_rmain.c:780) and TriFog
  (gl_triapi.c:266) hand glFog, and NV097_SET_FOG_COLOR's byte order.
*/
#ifndef NV2A_WARP_UTIL_H
#define NV2A_WARP_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SUBDIVIDE_SIZE, gl_local.h:65. Also the texture-coordinate divisor
   EmitWaterPolys applies (gl_rsurf.c:983-984). */
#define NV2A_WARP_SUBDIVIDE_SIZE 64

/* SubdividePolygon_r's own limit (gl_rsurf.c:212): numverts above this
   Host_Errors in the reference. */
#define NV2A_WARP_MAX_VERTS ( NV2A_WARP_SUBDIVIDE_SIZE - 4 )

/* One finished piece: `numverts` points, 3 floats each. */
typedef void (*nv2a_warp_poly_fn)( void *ctx, int numverts, const float *verts );

/* SubdividePolygon_r( loadmodel, warpface, numverts, verts ),
   gl_rsurf.c:204. `verts` holds `numverts` points, 3 floats each, and
   must have room for NV2A_WARP_SUBDIVIDE_SIZE points: the cut writes a
   copy of the first point past the last (gl_rsurf.c:229-231). Each final
   piece goes to `emit` in the reference's order (front half first).
   Returns 0 as soon as a polygon has more than NV2A_WARP_MAX_VERTS points,
   where the reference calls Host_Error (the pieces already emitted stay
   emitted), 1 otherwise. */
int nv2a_warp_subdivide( int numverts, float *verts, nv2a_warp_poly_fn emit, void *ctx );

/* r_turbsin[i & 255] (gl_rsurf.c:24, engine/warpsin.h). */
float nv2a_warp_turbsin( int i );

/* EmitWaterPolys' wave height (gl_rsurf.c:959-964): the vertex's z moved
   by the two-sine wave, or z unchanged when waveHeight is 0. `time` is
   gp_cl->time, kept a double as the reference's expression is. */
float nv2a_warp_wave_z( const float v[3], double time, float waveHeight );

/* EmitWaterPolys' texture coordinate (gl_rsurf.c:967-984): (os, ot) are
   the polygon's raw texture-space coordinates (R_TextureCoord for a
   SURF_DRAWTURB face: DotProduct( v, vecs ), no offset, no division,
   gl_rsurf.c:147-160), warped by the sin table unless `ripples`, then
   divided by SUBDIVIDE_SIZE. */
void nv2a_warp_texcoord( float os, float ot, double time, int ripples, float out_st[2] );

/* The per-frame form of the two functions above, for the draw loop. The
   reference's indices are (int)( double ), which the Xbox build compiles to
   x87 truncations (an FLDCW pair each, four per vertex). Here the time
   terms - time * 160, time * 171 and time * TURBSCALE - are split once per
   frame into an integer and a fraction, and each index is the integer plus
   a float sum, truncated with the same C semantics (toward zero) by
   nv2a_warp_trunc_sum. The float sum can round differently from the
   reference's double one right at an index boundary: then the index is off
   by one (the host test measures how often; one table step moves the wave
   by at most 0.1 unit). `time` is gp_cl->time. */
typedef struct
{
	int   base160, base171, basets;
	float frac160, frac171, fracts;
} nv2a_warp_frame_t;

void nv2a_warp_frame_init( nv2a_warp_frame_t *frame, double time );

/* (int)( base + a ) for an integer base and a float a, rounding toward
   zero as the C conversion does. */
int nv2a_warp_trunc_sum( int base, float a );

float nv2a_warp_wave_z_frame( const nv2a_warp_frame_t *frame, const float v[3], float waveHeight );
void nv2a_warp_texcoord_frame( const nv2a_warp_frame_t *frame, float os, float ot, float out_st[2] );

/* NV097_SET_FOG_PARAMS words 0 and 1 for GL_EXP fog, f = e^(-density * d):
   the hardware computes x + 2^(d * y * 16) - 1.5, so x = 1.5 and
   y = -density / (2 * ln 256) (xemu hw/xbox/nv2a/pgraph/glsl/vsh.c,
   FOG_MODE_EXP, which records the D3D driver's own derivation). */
void nv2a_fog_exp_params( float density, float out_params[2] );

/* The same for GL_EXP2, f = e^(-(density * d)^2): the hardware computes
   x + 2^(-d^2 * y^2 * 32) - 1.5, so x = 1.5 and
   y = -density / (2 * sqrt(ln 256)) (same source, FOG_MODE_EXP2). TriFog
   selects it when TriFogParams gave a density (gl_triapi.c:291-295). */
void nv2a_fog_exp2_params( float density, float out_params[2] );

/* The same for GL_LINEAR, f = (end - d) / (end - start): the hardware
   computes x + d * y - 1, so y = -1 / (end - start) and x = 1 - end * y
   (same source, FOG_MODE_LINEAR). end <= start is the caller's to reject
   (TriFog disables fog then, gl_triapi.c:273-277). */
void nv2a_fog_linear_params( float start, float end, float out_params[2] );

/* NV097_SET_FOG_COLOR from a GL fog colour in [0,1]: red in the low byte
   (nv_regs.h NV097_SET_FOG_COLOR_RED 0x000000FF), then green, blue; alpha
   is unused by the fog blend and left 0. Components are clamped. */
uint32_t nv2a_fog_pack_color( const float rgb[3] );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_WARP_UTIL_H */
