/*
nv2a_decal_util.h - X9-X item 14 step 12: the pure-arithmetic pieces of
ref/nv2a's decals (r_decals.c), factored out so they build and are
unit-tested on the host without pbkit/xboxkrnl/engine includes - the same
split nv2a_warp_util.h and nv2a_sky_util.h use.

What lives here, each a port of the reference function it names (root
clone, ref/gl/gl_decals.c and gl_rsurf.c):

- R_ClipInside, R_ClipIntersect, SHClip and R_DoDecalSHClip
  (gl_decals.c:228-363): the Sutherland-Hodgman clip of a face's polygon,
  already projected into the decal's [0,1] texture space, against the
  decal's four edges. Vertices are VERTEXSIZE (7) floats: x y z, decal
  s t, lightmap s t.
- R_LightmapCoord (gl_rsurf.c:130) without its atlas placement: where a
  point falls inside the face's own lightmap block, in luxels. ref/gl adds
  the block's position (light_s/light_t) and divides by BLOCK_SIZE; here
  the block's position is only known when the decal is drawn (the baked
  atlas, r_context.c), and the NV2A's linear lightmap stage takes texel
  coordinates, so the draw adds the block origin and does not divide.
*/
#ifndef NV2A_DECAL_UTIL_H
#define NV2A_DECAL_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* VERTEXSIZE, common/com_model.h:36. */
#define NV2A_DECAL_VERTEXSIZE 7

/* MAX_DECALCLIPVERT, gl_decals.c:20: the size of the reference's two
   static clip buffers (g_DecalClipVerts, g_DecalClipVerts2). */
#define NV2A_DECAL_MAX_CLIPVERT 32

/* The largest input polygon the clip accepts. Each of the four edges adds
   at most one vertex to a convex polygon, so an input of this size fills
   the buffers exactly. The reference has no such check: a face with more
   than 32 edges writes past g_DecalClipVerts in R_SetupDecalVertsForMSurface
   (gl_decals.c:200), and one with 29-32 edges can in SHClip. */
#define NV2A_DECAL_MAX_INPUT_VERTS ( NV2A_DECAL_MAX_CLIPVERT - 4 )

/* The four clip edges, gl_decals.c:31-34. */
#define NV2A_DECAL_LEFT_EDGE   0
#define NV2A_DECAL_RIGHT_EDGE  1
#define NV2A_DECAL_TOP_EDGE    2
#define NV2A_DECAL_BOTTOM_EDGE 3

/* R_ClipInside( vert, edge ), gl_decals.c:228. */
int nv2a_decal_clip_inside( const float *vert, int edge );

/* R_ClipIntersect( one, two, out, edge ), gl_decals.c:252, including its
   lightmap-t typo on the top/bottom edges (out[5] reads two[4], :297) -
   harmless there because R_DecalSetupVerts recomputes the lightmap
   coordinates after the clip (gl_decals.c:816-817), and kept so the clip
   stays the reference's. */
void nv2a_decal_clip_intersect( const float *one, const float *two, float *out, int edge );

/* SHClip( vert, vertCount, out, edge ), gl_decals.c:303. Returns the
   output vertex count. */
int nv2a_decal_sh_clip( const float *vert, int vertCount, float *out, int edge );

/* R_DoDecalSHClip, gl_decals.c:349: `verts` holds `count` vertices on
   entry and the clipped polygon on return; `scratch` is the second
   buffer. Both hold NV2A_DECAL_MAX_CLIPVERT vertices. Returns the clipped
   vertex count - 0 when the polygon misses the decal, or when `count` is
   above NV2A_DECAL_MAX_INPUT_VERTS (see above) or below 3. */
int nv2a_decal_clip_polygon( float verts[][NV2A_DECAL_VERTEXSIZE], float scratch[][NV2A_DECAL_VERTEXSIZE], int count );

/* R_LightmapCoord( v, surf, sample_size, coords ), gl_rsurf.c:130, in the
   face's block: ( DotProduct( v, lmvecs[k] ) + lmvecs[k][3]
   - lightmapmins[k] ) / sample_size + 0.5 for k = 0, 1 - the reference's
   expression before light_s/light_t and the division by BLOCK_SIZE. */
void nv2a_decal_lightmap_luxel( const float v[3], const float lmvecs[2][4], const short lightmapmins[2], float sample_size, float out[2] );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_DECAL_UTIL_H */
