/*
nv2a_studio_util.h - X9-X step 5 Part C: the pure-arithmetic pieces of the
studio-model batch-state mapping (r_context.c/r_studio.c's per-mesh state
changes, design note section 6 / spec Part C) factored out so they build
and are unit-tested on the host, without pbkit/xboxkrnl/engine includes -
the same split nv2a_2d_util.h and nv2a_tri_util.h already use (see those
headers' own comments).

The register PUSHES themselves (which do need pbkit's nv_regs.h) stay in
r_context.c/r_studio.c; this file only decides WHAT the mesh's resulting
state should be, in terms of a small set of hardware constants mirrored
below (nv_regs.h is not available to a host build - deps/nxdk is not
always checked out, see tools/host-tests/run.sh's own header). Each mirror
is pinned against the real nv_regs.h macro by a STATIC_ASSERT in
r_context.c, which DOES have that header.
*/
#ifndef NV2A_STUDIO_UTIL_H
#define NV2A_STUDIO_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors of nv_regs.h's own values (nv_regs.h:256-273 per the X9-X step 5
   spec's own citations). Pinned against the real macros by a
   STATIC_ASSERT in r_context.c/r_studio.c. */
#define NV2A_STUDIO_SFACTOR_ONE       0x0001u /* NV097_SET_BLEND_FUNC_SFACTOR_V_ONE, nv_regs.h:257 */
#define NV2A_STUDIO_SFACTOR_SRC_ALPHA 0x0302u /* NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA, nv_regs.h:260 */
#define NV2A_STUDIO_DFACTOR_ONE       0x0001u /* NV097_SET_BLEND_FUNC_DFACTOR_V_ONE, nv_regs.h:273 */
#define NV2A_STUDIO_DFACTOR_ONE_MINUS_SRC_ALPHA 0x0303u /* NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA, nv_regs.h:277 */

/* GL_GREATER 0.5 as an 8-bit ALPHA_REF: a/255 > 0.5 holds for a >= 128,
   the same derivation nv2a_set_render_mode() already uses for GL_GREATER
   0.25 -> 0x3F (r_context.c). Masked skin alpha is 0 or 255 in practice
   (Image_LoadMDL's index-255 key), so any reference in [0,254] renders
   identically - design note section 6. */
#define NV2A_STUDIO_ALPHA_REF_MASKED 0x7Fu

/* Mesh render state the batch flush (r_context.c) needs to push - a
   subset of nv2a_2d_shadow_t's own fields, kept independent of that
   struct so this file never needs r_context.c's own type. */
typedef struct
{
	int      alpha_test_enable;
	uint32_t alpha_ref;
	int      blend_enable;
	uint32_t sfactor, dfactor;
	int      depth_mask;
} nv2a_studio_mesh_state_t;

/* Design note section 6 / gl_studio.c:2090-2123 (R_StudioDrawPoints's own
   per-mesh block): given the mesh's own face flags (STUDIO_NF_MASKED/
   STUDIO_NF_ADDITIVE - studio.h:76-77, mirrored below so this file need
   not include studio.h) and whether the ENTITY (not the mesh) is opaque
   (R_ModelOpaque(rendermode)), returns the resulting mesh state built on
   top of `current` (the render-mode-derived base state,
   R_StudioSetupRenderer/GL_StudioSetRenderMode's own output). MASKED and
   STUDIO_NF_ADDITIVE are mutually exclusive in the reference (if/else),
   and any other combination of flags returns `current` completely
   unchanged. */
nv2a_studio_mesh_state_t nv2a_studio_mesh_state( int face_flags, int entity_opaque, nv2a_studio_mesh_state_t current );

/* studio.h's own STUDIO_NF_MASKED/STUDIO_NF_ADDITIVE (0x0040/0x0020),
   mirrored here for the same host-build reason as the nv_regs.h mirrors
   above - pinned against the real macros by a STATIC_ASSERT where both
   headers are available (r_studio.c). */
#define NV2A_STUDIO_NF_ADDITIVE 0x0020
#define NV2A_STUDIO_NF_MASKED   0x0040

/* Design note section 2 / gl_studio.c:3322-3363 (R_DrawViewModel's own
   glDepthRange(gldepthmin, gldepthmin + 0.3*(gldepthmax-gldepthmin))):
   the same remap expressed as the z_min/z_max pair nv2a_matrix_viewport()
   already takes for the world (r_context.c:936, called with 0,65536 for
   the world) - see that function's own citation for why this is a exact
   sub-range rather than a new hardware register. gldepthmin/gldepthmax
   are 0/1 outside the developer overview mode this renderer never draws
   (gl_rmain.c:310-319), so the remapped range is always [0, 0.3*65536].
   Under the W-buffer this z reaches only the near/far clip, never the
   depth: the depth range itself is NV2A_VIEWMODEL_W_DIVISOR below
   (divergences.md #160). */
void nv2a_studio_viewmodel_zrange( float *z_min, float *z_max );

/* W-buffer depth precision (divergences.md #160).

   ref/nv2a draws with W-buffering (CONTROL0 Z_PERSPECTIVE, r_context.c)
   into pbkit's Z24S8 surface. The depth the NV2A tests and stores is then
   the interpolated w the vertex program wrote, floored to an integer
   (xemu hw/xbox/nv2a/pgraph/glsl/psh.c: `zvalue = <interpolated w>`,
   `gl_FragDepth = floor(zvalue) / 16777216.0 ...`), NOT the z of the
   projection. With w the raw view-axis depth, one depth step was one world
   unit: two studio-model layers closer than that (a security guard's
   vest over his shirt, teeth behind lips, a glove over a sleeve) got the
   same depth and the later-drawn one won under LEQUAL. ref/gl's 24-bit
   perspective z (zNear 4, gl_rmain.c:384) resolves about 0.0002 units at
   100 units. Multiplying the whole view-projection by
   NV2A_WBUFFER_SCALE multiplies w (so the depth) and leaves x/w, y/w,
   z/w and the perspective-correct interpolation (ratios of w) as they
   were: 1/1024 of a unit everywhere, and the largest eye distance in a
   +-4096 map, 8192 * sqrt(3) = 14189, times 1024 is still below the
   Z24 maximum 0xFFFFFF, so nothing the projection's far plane (8192)
   keeps is culled by the depth range. Power of two: the products and
   the fog and decal-bias rescales are exact. */
#define NV2A_WBUFFER_SCALE 1024.0f

/* R_DrawViewModel's glDepthRange(gldepthmin, gldepthmin + 0.3 * (gldepthmax
   - gldepthmin)) (gl_studio.c:3348) keeps the view model in front of the
   world: with zNear 4 a world fragment hides it only when nearer than
   4 / (1 - 0.3 * (1 - 4 / dv)), 4 to 5.7 units, which the near plane
   (4) all but rules out (ref/soft's linear stand-in: s_ziscale x 3,
   ref/soft/r_studio.c:2963-2965). A W-buffer ignores z, so the z-ranged viewport
   (nv2a_studio_viewmodel_zrange) never reached the depth. Its W-buffer
   form: the view model's w is scaled by 1/NV2A_VIEWMODEL_W_DIVISOR of the
   world's, so a view-model point dv units away tests as dv/16 - under the
   4-unit near plane for every dv below 64, i.e. the whole weapon - and
   still resolves 1/64 of a unit within the view model itself. */
#define NV2A_VIEWMODEL_W_DIVISOR 16.0f

/* ref/gl's projection near plane, gl_rmain.c:384 (zNear = 4.0f). */
#define NV2A_ZNEAR_REFERENCE 4.0f

/* The w scale for the world (and everything else drawn with its
   view-projection): NV2A_WBUFFER_SCALE when `enabled`, else 1 (the
   unscaled w this renderer used before #160). */
float nv2a_depth_w_scale( int enabled );

/* The view model's w scale: the world's divided by
   NV2A_VIEWMODEL_W_DIVISOR when `enabled`, else 1 (before #160 the view
   model's depth was its raw eye distance, like the world's). */
float nv2a_viewmodel_w_scale( int enabled );

/* Multiplies every element of the row-vector matrix `m` by `s`: a point's
   whole output (x, y, z, w) scales by s, so its x/w, y/w, z/w do not
   change and only w (the W-buffer depth) does. */
void nv2a_matrix_scale_all( float m[4][4], float s );

/* The integer a Z24 W-buffer stores for eye distance `w` drawn under
   `scale` (floor(w * scale), xemu psh.c), or -1 when the depth range
   [0, 0xFFFFFF] culls it - what the host test compares layers with. */
int32_t nv2a_wbuffer_depth( float w, float scale );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_STUDIO_UTIL_H */
