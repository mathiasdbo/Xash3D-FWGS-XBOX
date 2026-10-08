/*
r_studio_bridge.h - X9-X step 5: the narrow, private glue between
r_context.c (the TriAPI internals, the batch-state shadow and the frame's
own entity lists - all file-scope statics there) and the new r_studio.c
(the CPU-skinning studio-model port, docs/r3d/design/x9-x-studio-models.md).
Not an engine-facing header (unlike nv2a_texture.h/nv2a_2d_util.h/
nv2a_tri_util.h) and not host-tested - both sides already need pbkit's
own types transitively (ref_common.h's chain), so there is nothing to gain
by keeping this pure. r_context.c #includes this near its own top (so the
extended nv2a_2d_shadow_t below is the ONE definition both files use);
r_studio.c includes it for the TriAPI entry points and the small set of
r_context.c-owned globals it needs to read/write.
*/
#ifndef R_STUDIO_BRIDGE_H
#define R_STUDIO_BRIDGE_H

#include "xash3d_types.h"
#include "triangleapi.h" /* TRICULLSTYLE */
#include "nv2a_tri_util.h" /* nv2a_tri_vert_t, X9-X step 11 */
#include "nv2a_warp_util.h" /* NV2A_WARP_GPU_PARAM_REGS, R5 */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-list entity cap (X9-M), shared now that R_AddEntity accepts
   mod_studio into the SAME two lists as brush entities (X9-X step 5 Part
   B) - a HL map plus its NPCs/items still sends far fewer than this. */
#define NV2A_MAX_ENTITIES 512

extern cl_entity_t *nv2a_solid_entities[NV2A_MAX_ENTITIES];
extern cl_entity_t *nv2a_trans_entities[NV2A_MAX_ENTITIES];
extern int          nv2a_num_solid_entities;
extern int          nv2a_num_trans_entities;

/* X9-X step 7: the frame's ET_BEAM entities (gl_rmain.c:255-266),
   walked by CL_DrawBeams (r_effects.c). */
extern cl_entity_t *nv2a_beam_entities[NV2A_MAX_ENTITIES];
extern int          nv2a_num_beam_entities;

/* X9-X step 4 Part C's own nv2a_2d_shadow_t, moved here (from r_context.c)
   so r_studio.c's per-mesh state changes (design note section 6) write
   the SAME shadow the flush (r_context.c) actually pushes - exactly as
   ref/gl's GL_StudioSetRenderMode and gl_rsurf.c's R_SetRenderMode share
   ONE GL state machine. Two fields added this step: `alpha_ref` (the
   flush used to push a hardcoded ALPHA_REF 0 at every re-entry; studio's
   own MASKED meshes need 0x7F - design note section 6) and
   `viewmodel_depthrange` (section 2: which of the two z-ranged
   view-projection matrices the 3D re-entry pushes at constant 96). */
typedef struct
{
	qboolean blend_enable;
	uint32_t sfactor, dfactor;
	qboolean alpha_test_enable;
	uint32_t alpha_ref;
	qboolean depth_mask;
	qboolean cull_enable;
	qboolean viewmodel_depthrange;
	/* Only meaningful in nv2a_2d_batch_state/nv2a_2d_pushed (never in
	   nv2a_2d_desired, which has no draw of its own yet) - r_context.c's
	   own comment on this struct, unchanged by this move. */
	int      space; /* nv2a_imm_space_t, kept as a plain int here so this
	                    header does not need r_context.c's own enum */
	uint32_t op;
	/* X9-X step 6 (sprites, r_sprite.c): kRenderGlow sprites draw with
	   the depth test off (gl_sprite.c:415) and the sprite 'lightmap' pass
	   with glDepthFunc( GL_EQUAL ) (:551). Both 3D-only, and both zero by
	   default so every earlier writer of this struct keeps its meaning
	   (depth test on, pbkit's LEQUAL - pbkit.c:3161). Last, after `op`,
	   so the positional initializer of nv2a_2d_desired stays valid. */
	qboolean depth_test_disable;
	qboolean depth_func_equal;
	/* X9-X step 11: the GL fog state (glEnable( GL_FOG ), glFog*) a batch
	   was appended under - snapshotted from nv2a_fog_current at append,
	   only in nv2a_2d_batch_state/nv2a_2d_pushed; nv2a_2d_desired's copy
	   is never read, so a caller that saves and restores nv2a_2d_desired
	   around a draw can never roll the fog state back. */
	struct nv2a_fog_hw_s
	{
		qboolean enable;
		int      mode;      /* NV2A_FOG_MODE_* */
		float    density;
		float    start, end;
		uint32_t color;     /* NV097_SET_FOG_COLOR byte order, nv2a_fog_pack_color */
	} fog;
} nv2a_2d_shadow_t;

typedef struct nv2a_fog_hw_s nv2a_fog_hw_t;

/* GL_EXP, GL_EXP2, GL_LINEAR. */
#define NV2A_FOG_MODE_EXP    0
#define NV2A_FOG_MODE_EXP2   1
#define NV2A_FOG_MODE_LINEAR 2

/* X9-X step 11: the current GL fog state, written only by r_warp.c (the
   port of R_DrawFog/R_CheckFog/R_AllowFog/TriFog) and read by every draw:
   TriAPI batches snapshot it at append, the world and brush-entity path
   (vs_lit/ps_lit) push it when they enter their own state. */
extern nv2a_fog_hw_t nv2a_fog_current;

extern nv2a_2d_shadow_t nv2a_2d_desired;

/* r_context.c's own TriAPI internals (X9-X step 4 Part D) - exposed
   (no longer static) so r_studio.c can call them directly, exactly as
   ref/soft/r_studio.c calls TriBegin/TriTexCoord2f/TriColor4ub/
   TriVertex3fv/TriEnd/TriCullFace/TriRenderMode. Same names except for
   the "Tri" prefix, which r_context.c already dropped from everything
   but TriRenderMode (X9-X step 4 Part D's own naming). */
void Begin( int primitiveCode );
void End( void );
void Vertex3f( float x, float y, float z );
void Vertex3fv( const float *worldPnt );
void TexCoord2f( float u, float v );
void Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a );
void Color4f( float r, float g, float b, float a );
void CullFace( TRICULLSTYLE mode );
void TriRenderMode( int mode );

/* r_context.c's own R_CullBox equivalent (X9-F), needed by
   R_StudioComputeBBox (R_CullModel, ref/gl/gl_cull.c:43). */
qboolean nv2a_frustum_cull_box( const vec3_t mins, const vec3_t maxs );

/* r_context.c's own CL_FxBlend port (ref/gl's gl_rmain.c:1161), reused by
   r_studio.c instead of a second copy - R_StudioSetRenderamt needs it. */
int nv2a_fx_blend( cl_entity_t *e );

/* r_context.c's own stored copy of the frame's entity array (from
   R_ProcessEntData) - CL_GetEntityByIndex's own bounds-checked slot
   (ref/soft/r_local.h:513), needed by R_StudioClientEvents/
   R_FindParentEntity/R_DrawStudioModel. */
cl_entity_t *nv2a_get_entity_by_index( int index );

/* X9-X step 5 Part C: applies nv2a_studio_mesh_state() (nv2a_studio_util.h)
   to nv2a_2d_desired on top of whatever GL_StudioSetRenderMode /
   R_StudioSetupRenderer already set, and restores it afterwards -
   R_StudioDrawPoints's own per-mesh MASKED/ADDITIVE block
   (gl_studio.c:2090-2123). `entity_opaque` is R_ModelOpaque() of the
   CURRENT entity's rendermode, computed by the caller (r_studio.c). */
void nv2a_studio_apply_mesh_state( int face_flags, int entity_opaque );
void nv2a_studio_restore_mesh_state( void );

/* X9-X step 6: r_context.c's own R_WorldToScreen port (NDC x,y, returns
   true when the point is behind the eye) - ref/gl's gl_sprite.c reaches
   it through its TriWorldToScreen (gl_triapi.c:223), which r_sprite.c
   ports on top of this. */
int nv2a_world_to_screen( const vec3_t world, vec3_t screen );

/* Design note section 2: forces a flush/re-entry (r_context.c's own
   nv2a_2d_flush) whenever this changes, so the 3D branch's constant-96
   push picks the view-model z-ranged matrix instead of the world's. */
void nv2a_studio_set_viewmodel_depthrange( int on );

/* X9-X step 7: ref/gl's Mod_GetCurrentVis (gl_rsurf.c:112), which
   R_BeamCull and R_DrawRing pass to Mod_BoxVisible - NULL (all visible)
   until nv2a_mark_leaves has a real PVS row. */
byte *nv2a_mod_get_current_vis( void );

/* X9-X step 7 (divergences.md #133): GL_REPEAT for the TriAPI's 3D
   primitives while on - r_effects.c's R_BeamDraw turns it on around its
   Begin/End. While it is on the TriAPI stages its triangles and appends
   them in runs of up to 64 (#159), under the state current then: a caller must not
   change the render mode, texture, cull or fog between its Begin and
   End while repeat is on. */
void nv2a_tri_set_texture_repeat( qboolean on );

/* X9-X step 11: bind one of the map's own DXT1 directory textures (the
   world's, nv2a_map_textures) for the TriAPI's next primitives instead of
   the texture-table texture GL_Bind selected - normalized coordinates,
   the world's own WRAP addressing, mip chain and filter. r_warp.c draws
   the water with it. NV2A_TRI_NO_MAP_TEXTURE returns to GL_Bind's. */
#define NV2A_TRI_NO_MAP_TEXTURE 0xFFFFFFFFu
void nv2a_tri_bind_map_texture( uint32_t dir );

/* X9-X step 11: the map directory entry a surface of the current world
   draws with (the baked face's own batch texture), or
   NV2A_TRI_NO_MAP_TEXTURE when there is no per-face table or no entry. */
struct msurface_s;
uint32_t nv2a_surface_map_texture( const struct msurface_s *surf );

/* X9-X step 11: submits the TriAPI's pending batch (r_context.c's
   nv2a_2d_flush), for callers outside r_context.c that must draw before
   state they do not own changes. */
void nv2a_tri_flush( void );

/* X9-X step 11: whole 3D primitives (quads, or triangles) straight into
   the TriAPI's batch, with the current colour - Begin/Vertex3f/End's
   output without its assembler (r_warp.c's per-vertex hot path). Texture
   coordinates are in the bound texture's units (normalized for a map
   directory texture). */
void nv2a_tri_append_3d( qboolean quads, nv2a_tri_vert_t *v, uint32_t n );

/* R5 (divergences.md #147): r_warp.c's water through vs_warp instead of
   nv2a_tri_append_3d - r_context.c's half (the program, the constants and
   the GPU state). One nv2a_wgpu_begin/nv2a_wgpu_end pair draws one
   entity's, or the world chain's, water faces from the map's static water
   buffer `vb`, transformed by `mvp` (NULL: the world's view-projection),
   under the current desired state, fog and colour; per face the
   parameters (nv2a_warp_gpu_params), the texture (the map directory entry
   or GL_Bind's, as the TriAPI resolves it; false: none, skip the face) and
   the draws. nv2a_wgpu_begin is false when the frame cannot take a draw. */
qboolean nv2a_wgpu_begin( const void *vb, const float (*mvp)[4] );
void nv2a_wgpu_set_params( const float params[NV2A_WARP_GPU_PARAM_REGS][4] );
void nv2a_wgpu_set_wave_height( float waveHeight );
qboolean nv2a_wgpu_bind( void );
void nv2a_wgpu_draw_quads( uint32_t first, uint32_t count );
void nv2a_wgpu_draw_indices( qboolean quads, const uint16_t *idx, uint32_t n );
void nv2a_wgpu_end( void );

/* The current Color4ub/Color4f colour, as Vertex3f takes it. */
void nv2a_tri_current_color( uint8_t rgba[4] );

#ifdef __cplusplus
}
#endif

#endif /* R_STUDIO_BRIDGE_H */
