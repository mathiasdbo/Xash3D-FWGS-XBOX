/*
nv2a_sky_util.h - X9-X item 14 step 10: the pure-arithmetic pieces of
ref/nv2a's skybox (r_sky.c, a port of the skybox half of ref/gl/gl_warp.c),
factored out so they build and are unit-tested on the host without
pbkit/xboxkrnl/engine includes - the same split nv2a_efx_util.h and
nv2a_sprite_util.h use.

What lives here, each a port of the gl_warp.c function it names:

- R_ClearSkyBox (gl_warp.c:264): empty per-side s/t bounds.
- ClipSkyPolygon + DrawSkyPolygon (gl_warp.c:126, :78): clip one sky face,
  relative to the eye, against the six planes that split space into the
  cube's sides, and grow each side's s/t bounds by what lands on it.
- MakeSkyVec (gl_warp.c:221): one corner of a side's quad - the world
  position and the texture coordinate.
- r_skyTexOrder (gl_warp.c:23): which loaded image each side binds.

Only the reference's GL_CLAMPTOEDGE_EXT branch of MakeSkyVec is ported:
every ref/nv2a texture samples with CLAMP_TO_EDGE (TEXTURE_ADDRESS
0x00030303, r_context.c's 2D flush), which is the case that branch covers.
*/
#ifndef NV2A_SKY_UTIL_H
#define NV2A_SKY_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* SKYBOX_MAX_SIDES, engine/ref_api.h:117. */
#define NV2A_SKY_SIDES 6

/* MAX_CLIP_VERTS, gl_warp.c:21 - the reference Host_Errors past it. */
#define NV2A_SKY_MAX_CLIP_VERTS 128

/* RI.skyMins/skyMaxs (gl_local.h:153-154): [0] is s, [1] is t, per side. */
typedef struct
{
	float mins[2][NV2A_SKY_SIDES];
	float maxs[2][NV2A_SKY_SIDES];
} nv2a_sky_bounds_t;

/* r_skyTexOrder, gl_warp.c:23: side i binds skyboxTextures[order[i]],
   where the textures are in the engine's suffix order rt, bk, lf, ft, up,
   dn (ref_common.c:33). */
extern const int nv2a_sky_tex_order[NV2A_SKY_SIDES];

/* R_ClearSkyBox, gl_warp.c:264. */
void nv2a_sky_clear( nv2a_sky_bounds_t *bounds );

/* ClipSkyPolygon( nump, vecs, 0 ), gl_warp.c:126, with DrawSkyPolygon
   (gl_warp.c:78) as its leaf. `vecs` holds `nump` points relative to the
   eye, 3 floats each, and must have room for one more point: the clip
   writes a copy of the first point past the last (gl_warp.c:179). Returns
   0 and changes nothing when nump exceeds NV2A_SKY_MAX_CLIP_VERTS, where
   the reference calls Host_Error (gl_warp.c:138) - the caller reports it;
   1 otherwise. Not reentrant: the per-stage split buffers are static (see
   the .c for why). */
int nv2a_sky_clip_polygon( nv2a_sky_bounds_t *bounds, int nump, float *vecs );

/* The test R_DrawSkyBox skips a side with (gl_warp.c:346): true when the
   side received nothing this frame. */
int nv2a_sky_side_empty( const nv2a_sky_bounds_t *bounds, int side );

/* MakeSkyVec, gl_warp.c:221, clamp-to-edge branch: the world position of
   (s, t) on side `axis` of a box of half-size `farclip >> 1` around
   `origin`, and its texture coordinate in [0,1] (t flipped as the
   reference does). `farclip` is RI.farClip as the reference truncates it
   (an int). */
void nv2a_sky_make_vec( float s, float t, int axis, int farclip, const float origin[3], float out_pos[3], float out_st[2] );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_SKY_UTIL_H */
