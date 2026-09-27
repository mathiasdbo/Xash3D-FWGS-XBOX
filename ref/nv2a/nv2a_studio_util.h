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
   (gl_rmain.c:310-319), so the remapped range is always [0, 0.3*65536]. */
void nv2a_studio_viewmodel_zrange( float *z_min, float *z_max );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_STUDIO_UTIL_H */
