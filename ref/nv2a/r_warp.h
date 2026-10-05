/*
r_warp.h - X9-X item 14 step 11: the entry points r_context.c calls into
ref/nv2a/r_warp.c, the port of ref/gl's water warp (GL_SubdivideSurface,
EmitWaterPolys, R_DrawWaterSurfaces - gl_rsurf.c) and fog (R_CheckFog,
R_DrawFog, R_AllowFog - gl_rmain.c; TriFog/TriFogParams - gl_triapi.c).
The per-frame and per-map state stays private to r_warp.c, as r_sky.h and
r_effects.h do.
*/
#ifndef R_WARP_H
#define R_WARP_H

#include "xash3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct model_s;
struct msurface_s;
struct mleaf_s;
struct cl_entity_s;

/* ref_interface_t's GL_SubdivideSurface (gl_rsurf.c:361), called by the
   engine for every SURF_DRAWTURB face as the map loads
   (Mod_LoadSurfaces, mod_bmodel.c:3490). */
void R_WarpSubdivideSurface( struct model_s *mod, struct msurface_s *fa );

/* R_NewMap: the water textures' fog colour and density (the WAD palette's
   entries 3 and 4, img_wad.c:585-592), looked up once per map. */
void R_WarpNewMap( const struct model_s *world );

/* The map's renderer data is freed (nv2a_free_map_resources): forget the
   per-map chains and fog; R_Shutdown also forgets the load accounting. */
void R_WarpFreeMap( void );
void R_WarpShutdown( void );

/* The world pass: queue one visible world water face (R_RecursiveWorldNode's
   texturechain, gl_rsurf.c:3526), then, after the world, draw the queue
   (R_DrawTextureChains' SURF_DRAWTURB chains, gl_rsurf.c:1553-1566) - or
   keep it for R_WarpDrawWaterSurfaces when sv_wateralpha < 1. */
void R_WarpAddWorldSurface( struct msurface_s *surf );
void R_WarpDrawWorld( void );

/* R_DrawWaterSurfaces, gl_rsurf.c:1630: the translucent world water, after
   the entities and the view model. `rvp_flags` is RI.rvp.flags. */
void R_WarpDrawWaterSurfaces( int rvp_flags );

/* R_DrawBrushModel's water faces for one entity (R_RenderBrushPoly ->
   EmitWaterPolys, gl_rsurf.c:1488-1497): queue each surviving face with
   whether it is seen from behind (CULL_BACKSIDE, drawn reversed), then
   draw them under the entity's render mode (R_SetRenderMode,
   gl_rsurf.c:1717). `blend` is tr.blend. The object transform is the
   entity's origin and angles. */
void R_WarpAddBrushSurface( struct msurface_s *surf, qboolean reverse );
void R_WarpDrawBrushEntity( struct cl_entity_s *e, float blend, qboolean rotated, const float mvp[4][4] );

/* R5 (divergences.md #147): the bytes R_WarpNewMap's static water vertex
   buffer takes for this world - the term ref/nv2a's per-map ledger and
   the baker's (Nv2aMapBudget.h) both add. */
uint32_t R_WarpGpuMapBytes( const struct model_s *world );

/* R5: builds that buffer for `world`, replacing any previous one - once the
   map's own GPU buffers are in place (r_context.c nv2a_try_pending_upload);
   R_WarpFreeMap frees it. */
void R_WarpGpuNewMap( const struct model_s *world );

/* The frame's fog, in the reference's order within R_RenderScene
   (gl_rmain.c:945-980): R_WarpSetupFrame is R_SetupFrame's
   glState.isFogEnabled sample (:517), R_WarpDrawFog is R_DrawFog (:780),
   R_WarpCheckFog is R_CheckFog (:655), run after the world. */
void R_WarpSetupFrame( const float *vieworigin );
void R_WarpDrawFog( void );
void R_WarpCheckFog( int rvp_flags, struct mleaf_s *viewleaf );

/* R_AllowFog, gl_rmain.c:42. */
void R_WarpAllowFog( qboolean allowed );

/* The skybox's fog (R_DrawSkyBox, gl_warp.c:334-338 and :361-365): off
   unless RI.fogSkybox, and at half density while the box draws. */
void R_WarpSkyFogBegin( void );
void R_WarpSkyFogEnd( void );

/* TriFog / TriFogParams, gl_triapi.c:266 and :328. */
void R_WarpTriFog( float flFogColor[3], float flStart, float flEnd, int bOn );
void R_WarpTriFogParams( float flDensity, int iFogSkybox );

/* For the frame-cost probe: water polygons and vertices emitted this
   frame (reset by R_WarpSetupFrame). */
typedef struct
{
	uint32_t faces;
	uint32_t polys;
	uint32_t verts;
} nv2a_warp_stats_t;
extern nv2a_warp_stats_t nv2a_warp_stats;

#ifdef __cplusplus
}
#endif

#endif /* R_WARP_H */
