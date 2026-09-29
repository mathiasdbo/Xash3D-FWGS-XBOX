/*
r_effects.h - X9-X step 7: the entry points r_context.c calls into
ref/nv2a/r_effects.c, the port of ref/gl/gl_beams.c and ref/gl/gl_rpart.c.
Like r_sprite.h, the per-frame view state stays private to r_effects.c and
r_context.c sets it through the setter below instead of a shared RI/tr.
*/
#ifndef R_EFFECTS_H
#define R_EFFECTS_H

#include "xash3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct particle_s;
struct beam_s;

/* R_Init: fetches the palette ref/gl keeps in tr.palette
   (PARM_GET_PALETTE_PTR, gl_opengl.c:1269), which CL_DrawParticles reads. */
void R_EfxInit( void );

/* GL_RenderFrame, once per real world pass: the RI.rvp fields, RI.v* /
   RI.cull_v* vectors and tr.frametime (gl_rmain.c:951) the effects read. */
void R_EfxSetFrameContext( int rvp_flags, const vec3_t vieworigin, const int viewport[4],
	const vec3_t vforward, const vec3_t vright, const vec3_t vup, double frametime );

/* ref_interface_t's efx implementation (ref_api.h:602-604), called back by
   the engine's CL_DrawEFX (cl_efx.c:2079), which GL_RenderFrame calls
   where R_DrawEntitiesOnList does (gl_rmain.c:861, :922). */
void CL_DrawBeams( int fTrans, struct beam_s *active_beams );
void CL_DrawParticles( double frametime, struct particle_s *cl_active_particles, float partsize );
void CL_DrawTracers( double frametime, struct particle_s *cl_active_tracers );

#ifdef __cplusplus
}
#endif

#endif /* R_EFFECTS_H */
