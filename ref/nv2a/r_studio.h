/*
r_studio.h - X9-X step 5: the entry points r_context.c calls into
ref/nv2a/r_studio.c (docs/r3d/design/x9-x-studio-models.md), analogous to
ref/soft's own r_studioint.h-declared set plus the handful of extra
setters r_context.c needs because this renderer keeps the studio state
struct (RI/tr's replacement) private to r_studio.c rather than a shared
r_local.h.
*/
#ifndef R_STUDIO_H
#define R_STUDIO_H

#include "xash3d_types.h"
#include "r_studioint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* R_Init/R_Shutdown, in that order - the heap-allocated studio_draw_state_t
   (design note section 5, R_AllocStudioStatics pattern,
   ref/soft/r_studio.c:113-125). R_StudioInit() also pulls the engine
   parms (viewent/elights/gamma tables) ref/soft's own R_Init pulls at the
   same point in start-up (r_main.c:1376-1381) - see r_studio.c's own
   comment on why that fetch lives here instead of a separate function. */
qboolean R_StudioAllocStatics( void );
void     R_StudioFreeStatics( void );
void     R_StudioInit( void );

/* R_NewMap. */
void R_StudioResetPlayerModels( void );

/* Assigned into gReffuncs by name (r_context.c). */
qboolean R_StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw );
void     R_StudioSetDrawInterface( r_studio_interface_t *pDraw );
float    R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time );
void     R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles );
void     Mod_StudioLoadTextures( model_t *mod, void *data );

/* Mod_ProcessRenderData(create=false) for mod_studio (gl_context.c:111,127). */
void Mod_StudioUnloadTextures( void *data );

/* RefGetParm's PARM_GET_STUDIO_HDR (gl_context.c:218). */
studiohdr_t *R_StudioGetHeader( void );

/* R_AddEntity's opaque/sort helpers (gl_studio.c:2384's whole function,
   not just the brush branch nv2a_brush_entity_render_mode ported). */
int R_GetEntityRenderMode( cl_entity_t *ent );

/* GL_RenderFrame's own draw order (Part B / gl_rmain.c:797-935,1103). */
void R_RunViewmodelEvents( void );
void R_DrawStudioModel( cl_entity_t *e );
void R_DrawViewModel( void );

/* Setters replacing what ref/gl's own caller (gl_rmain.c) writes directly
   into RI/tr before dispatching to R_DrawStudioModel/R_FindParentEntity -
   this renderer keeps that state private to r_studio.c (Part A's own
   "small local context" note), so r_context.c's draw loop calls these
   instead. */
void nv2a_studio_set_frame_context( int rvp_flags, const vec3_t vieworigin,
	const vec3_t vforward, const vec3_t vright, const vec3_t vup );
void nv2a_studio_begin_entity( cl_entity_t *e ); /* currententity/currentmodel = e/e->model */
void nv2a_studio_set_blend( float blend );       /* tr.blend's replacement, R_DrawEntitiesOnList's own per-entity value */

#ifdef __cplusplus
}
#endif

#endif /* R_STUDIO_H */
