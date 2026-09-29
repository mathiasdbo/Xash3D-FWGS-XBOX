/*
nv2a_sprite_util.h - X9-X step 6: the pure-arithmetic pieces of ref/nv2a's
sprite renderer (r_sprite.c, a port of ref/gl/gl_sprite.c), factored out
so they build and are unit-tested on the host without pbkit/xboxkrnl/
engine includes - the same split nv2a_studio_util.h uses for the studio
port (see that header's own comment).

Two things live here:

- The GL state gl_sprite.c's R_DrawSpriteModel sets around its draw
  (gl_sprite.c:400-455 before the quad, :545-561 for the sprite
  "lightmap" pass, :564-578 after), as a transform of a small GL-like
  state struct. r_sprite.c copies the result into nv2a_2d_desired, the
  shadow the TriAPI flush pushes (r_context.c).
- The frame choice of R_GetSpriteFrameInterpolant (gl_sprite.c:32) that
  needs no engine type: the group-frame walk (:100-133) and the angled
  frame index (:138).

The hardware constants are mirrored from nv_regs.h, which a host build
does not have; r_context.c pins each mirror against the real macro with a
STATIC_ASSERT, as it already does for nv2a_studio_util.h.
*/
#ifndef NV2A_SPRITE_UTIL_H
#define NV2A_SPRITE_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nv_regs.h mirrors (NV097_SET_BLEND_FUNC_*, NV097_SET_BLEND_FUNC_DFACTOR_*). */
#define NV2A_SPRITE_SFACTOR_ZERO                0x0000u /* NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO */
#define NV2A_SPRITE_SFACTOR_SRC_ALPHA           0x0302u /* NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA */
#define NV2A_SPRITE_DFACTOR_ONE                 0x0001u /* NV097_SET_BLEND_FUNC_DFACTOR_V_ONE */
#define NV2A_SPRITE_DFACTOR_SRC_COLOR           0x0300u /* NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR */
#define NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA 0x0303u /* NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA */

/* GL_GREATER DEFAULT_ALPHATEST (0.0f, gl_local.h) as an 8-bit ALPHA_REF. */
#define NV2A_SPRITE_ALPHA_REF_DEFAULT  0x00u
/* GL_GREATER 1/3 (gl_sprite.c:455): a/255 > 1/3 holds for a >= 86, and
   the hardware's GREATER against 85 passes exactly a >= 86 - the same
   derivation as NV2A_STUDIO_ALPHA_REF_MASKED (0.5 -> 0x7F). */
#define NV2A_SPRITE_ALPHA_REF_LIGHTMAP 0x55u

/* const.h's kRender* (common/const.h:689-694), mirrored for the host
   build and pinned in r_sprite.c, which has const.h. */
#define NV2A_SPRITE_RENDER_NORMAL        0
#define NV2A_SPRITE_RENDER_TRANS_COLOR   1
#define NV2A_SPRITE_RENDER_TRANS_TEXTURE 2
#define NV2A_SPRITE_RENDER_GLOW          3
#define NV2A_SPRITE_RENDER_TRANS_ALPHA   4
#define NV2A_SPRITE_RENDER_TRANS_ADD     5

/* The part of GL's state machine R_DrawSpriteModel touches. depth_func_equal
   stands for glDepthFunc( GL_EQUAL ) against the default GL_LEQUAL. */
typedef struct
{
	int      blend_enable;
	uint32_t sfactor, dfactor;
	int      depth_mask;
	int      depth_test;
	int      depth_func_equal;
	int      alpha_test_enable;
	uint32_t alpha_ref;
} nv2a_sprite_state_t;

/* gl_sprite.c:403-430 and :455: the render-mode switch, then
   pglEnable( GL_ALPHA_TEST ), then GL_GREATER 1/3 when the sprite has a
   'lightmap' (R_SpriteHasLightmap). Fields the reference leaves alone
   keep their value from `cur`. */
nv2a_sprite_state_t nv2a_sprite_begin_state( int rendermode, int has_lightmap, nv2a_sprite_state_t cur );

/* gl_sprite.c:548-554: the lightmap pass - blend on (unless r_lightmap,
   which shows the lightmap alone), GL_EQUAL, alpha test off,
   ZERO/SRC_COLOR. */
nv2a_sprite_state_t nv2a_sprite_lightmap_state( int r_lightmap, nv2a_sprite_state_t cur );

/* gl_sprite.c:559-561: back to GL_GREATER DEFAULT_ALPHATEST, GL_LEQUAL,
   blend off. */
nv2a_sprite_state_t nv2a_sprite_lightmap_end_state( nv2a_sprite_state_t cur );

/* gl_sprite.c:567-578: alpha test off, depth mask on, and for any mode but
   kRenderNormal blend off and the depth test back on. */
nv2a_sprite_state_t nv2a_sprite_end_state( int rendermode, nv2a_sprite_state_t cur );

/* gl_sprite.c:100-133 (FRAME_GROUP): which two frames of a group to blend
   at `time`, and by how much. `intervals` are the group's cumulative end
   times (Mod_SpriteLoadGroup guarantees each > 0). Writes the indices of
   the old (j) and current (i) frames; returns the lerp fraction, which
   is 1.0 (unused) when `do_interp` is 0 and then old == current. The
   arithmetic is float, as the reference's (`float time = gp_cl->time`). */
float nv2a_sprite_group_frames( const float *intervals, int numframes, float time, int do_interp, int *oldframe, int *curframe );

/* gl_sprite.c:138 (FRAME_ANGLED): which of the 8 angled frames faces a
   viewer whose yaw is `view_yaw`, for an entity yawed `ent_yaw`
   (degrees). Q_rint as xash3d_mathlib.h:92 defines it. */
int nv2a_sprite_angle_frame( float view_yaw, float ent_yaw );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_SPRITE_UTIL_H */
