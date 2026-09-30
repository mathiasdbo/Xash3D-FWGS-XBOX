/*
r_decals.h - X9-X item 14 step 12: decals for ref/nv2a (r_decals.c), a
port of ref/gl's gl_decals.c, drawn by r_context.c through the lit
pipeline (the world's own vertex program, the decal texture on stage 0,
the lightmap atlas on stage 1).
*/
#ifndef R_DECALS_H
#define R_DECALS_H

#ifdef __cplusplus
extern "C" {
#endif

struct model_s;
struct msurface_s;
struct cl_entity_s;
struct decallist_s;

/* ---- r_decals.c: ref/gl's gl_decals.c ---- */

/* R_Init: registers gl_polyoffset (gl_opengl.c:15) and clears the pool,
   as R_Init's R_ClearDecals does (gl_opengl.c:1282). */
void R_DecalInit( void );

/* R_ClearDecals (gl_decals.c:57), from R_NewMap (gl_context.c:403). */
void R_ClearDecals( void );

/* GL_BuildLightmaps' decal-chain reset (gl_rsurf.c:3932-3944), from
   R_NewMap after R_ClearDecals: every brush model's surfaces forget their
   decal lists. */
void R_DecalClearSurfaceChains( void );

/* ref_interface_t entries (gl_context.c:520-522) and render_api's
   R_EntityRemoveDecals (gl_context.c:451). */
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, float *pos, int flags, float scale );
void R_DecalRemoveAll( int textureIndex );
int  R_CreateDecalList( struct decallist_s *pList );
void R_ClearAllDecals( void );
void R_EntityRemoveDecals( struct model_s *mod );

/* R_DrawWorld's DrawDecalsBatch (gl_rsurf.c:3711): the decals of every
   world surface the world pass drew this frame, after it. */
void R_DecalDrawWorld( void );

/* R_RenderDecalsForSurface (gl_rsurf.c:1384) for a brush entity's face:
   batched for kRenderNormal, kept for DrawSurfaceDecals otherwise.
   `backside` is R_SortBrushModelSurfaces' CULL_BACKSIDE, which only a
   kRenderTransTexture entity's face with decals keeps (gl_rsurf.c:1796). */
void R_DecalAddBrushSurface( const struct cl_entity_s *e, struct msurface_s *surf, int backside );

/* R_DrawBrushModel's DrawDecalsBatch (gl_rsurf.c:1914), and the
   DrawSurfaceDecals calls R_RenderDecalsForSurface made for a
   non-kRenderNormal entity: everything R_DecalAddBrushSurface kept for
   `e`, then the list is empty again. */
void R_DecalDrawBrushEntity( struct cl_entity_s *e, float blend );

/* gl_polyoffset's value (gl_opengl.c:15): DrawDecalsBatch and
   DrawSurfaceDecals push GL_PushPolygonOffset( -1.0f, -gl_polyoffset )
   while it is non-zero (gl_decals.c:873, 999). */
float R_DecalPolyOffset( void );

/* Per-frame counters, for inspection over the debugger. */
typedef struct
{
	unsigned int surfaces;    /* surfaces whose decals were drawn this frame */
	unsigned int decals;      /* decal polygons drawn this frame */
	unsigned int verts;       /* their vertices */
	unsigned int dropped;     /* polygons refused this frame: vertex buffer full */
	unsigned int in_use;      /* pool entries linked to a surface (updated when drawn) */
} nv2a_decal_stats_t;

extern nv2a_decal_stats_t nv2a_decal_stats;

/* ---- r_context.c: the GPU side ---- */

/* True when the world pass drew `surf` this frame and it faces the eye
   (R_CullSurface's CULL_BACKSIDE test, gl_cull.c:83-110, which drops a
   face - and so its decals - seen from behind). */
int nv2a_decal_world_surface_drawn( const struct msurface_s *surf );

/* True when `surf`, of the entity being drawn, faces the eye in model
   space (the same test with tr.modelorg). */
int nv2a_decal_entity_surface_front( const struct msurface_s *surf, const float modelorg[3] );

/* DrawDecalsBatch / DrawSurfaceDecals' state for `e` (gl_decals.c:
   856-875, 989-1000), with the colour R_SetRenderMode gives the entity
   (gl_rsurf.c:1717) as the decal program's constant. World decals pass
   the world entity. Returns 0 (and draws nothing) when there is no
   vertex buffer. */
int  nv2a_decal_gpu_begin( const struct cl_entity_s *e, float blend );

/* DrawSingleDecal (gl_decals.c:826) for one polygon: `numverts` vertices
   of VERTEXSIZE floats (x y z, decal s t, lightmap luxel s t within the
   face's block) on `surf`, textured with engine texture `texture`. */
void nv2a_decal_gpu_draw( int texture, const float *verts, int numverts, const struct msurface_s *surf );

/* Undoes nv2a_decal_gpu_begin (gl_decals.c:953-978, 1007-1018): the next
   lit draw re-enters the lit state, the TriAPI its own. */
void nv2a_decal_gpu_end( void );

#ifdef __cplusplus
}
#endif

#endif /* R_DECALS_H */
