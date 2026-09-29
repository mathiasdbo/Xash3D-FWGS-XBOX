/*
r_sprite.h - X9-X step 6: the entry points r_context.c (and r_studio.c)
call into ref/nv2a/r_sprite.c, the port of ref/gl/gl_sprite.c. Like
r_studio.h, this renderer keeps the per-frame view state private to the
file that uses it, so r_context.c sets it through the setters below
instead of writing a shared RI/tr.
*/
#ifndef R_SPRITE_H
#define R_SPRITE_H

#include "xash3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* R_Init, after R_StudioInit: registers r_traceglow (ref/gl registers it
   in GL_InitCommands, gl_opengl.c:1149) and fetches the view entity
   (PARM_GET_VIEWENT_PTR) R_CullModel compares against. */
void R_SpriteInit( void );

/* GL_RenderFrame, once per real world pass: the RI.rvp fields and RI.v*
   vectors gl_sprite.c reads. */
void R_SpriteSetFrameContext( int rvp_flags, const vec3_t vieworigin, const vec3_t viewangles,
	const int viewport[4], const vec3_t vforward, const vec3_t vright, const vec3_t vup );

/* tr.blend's replacement, set per entity by the entity-list walk
   (R_DrawEntitiesOnList, gl_rmain.c:797-912). */
void R_SpriteSetBlend( float blend );

/* gl_sprite.c:365. */
void R_DrawSpriteModel( cl_entity_t *e );

/* ref/gl's own TriSpriteTexture (gl_triapi.c:242): binds a sprite frame's
   texture for a TriAPI draw inside the renderer - the glow shell's chrome
   sprite and the studio shadow quad (gl_studio.c:2830, :1554). The
   client's pTriAPI->SpriteTexture is the engine's own copy
   (cl_game.c:3290), which binds through GL_Bind too. */
int TriSpriteTexture( model_t *pSpriteModel, int frame );

#ifdef __cplusplus
}
#endif

#endif /* R_SPRITE_H */
