/*
r_sky.h - X9-X item 14 step 10: the entry points r_context.c calls into
ref/nv2a/r_sky.c, the port of the skybox half of ref/gl/gl_warp.c
(R_SetupSky, R_UnloadSkybox, R_ClearSkyBox, R_AddSkyBoxSurface,
R_DrawSkyBox). Like r_effects.h, the per-frame state stays private to
r_sky.c.
*/
#ifndef R_SKY_H
#define R_SKY_H

#include "xash3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct model_s;
struct msurface_s;

/* ref_interface_t's R_SetupSky (gl_context.c:328): NULL unloads, else the
   six side textures the engine loaded (ref_common.c:103-150). */
void R_SetupSky( int *skyboxTextures );

/* gl_warp.c:312. */
void R_UnloadSkybox( void );

/* R_Shutdown: the texture table is gone (nv2a_tex_shutdown), so forget the
   side texnums without freeing them - a later R_UnloadSkybox must never
   free a slot a new table handed to someone else. */
void R_SkyShutdown( void );

/* RefGetParm's PARM_TEX_SKYBOX (gl_context.c:182). */
int R_SkyboxTexture( int side );

/* gl_warp.c:264, plus RI.cullorigin for this frame's clipping. */
void R_ClearSkyBox( const vec3_t cullorigin );

/* gl_warp.c:278: clip one visible SURF_DRAWSKY world face into the
   per-side bounds. `mod` is the world model whose edges the face uses. */
void R_AddSkyBoxSurface( const struct model_s *mod, const struct msurface_s *fa );

/* True once R_AddSkyBoxSurface took a face this frame - ref/gl's
   `skychain != NULL` (gl_rsurf.c:3718). */
qboolean R_SkyHasSurfaces( void );

/* gl_warp.c:332, through the TriAPI internals. Queues the side quads; the
   caller flushes. */
void R_DrawSkyBox( void );

#ifdef __cplusplus
}
#endif

#endif /* R_SKY_H */
