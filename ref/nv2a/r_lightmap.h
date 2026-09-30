/*
r_lightmap.h - X9-X step 8: the entry points r_context.c calls into
ref/nv2a/r_lightmap.c, animated lightstyles and dynamic lights on world
and brush-entity faces - a port of ref/gl's R_CheckLightMap /
R_BuildLightMap / R_AddDynamicLights (gl_rsurf.c) and ref/common's
R_MarkLights (ref_light.c:94), writing into the baked lightmap atlas in
place instead of into a second, dynamic lightmap texture (divergences.md
#134).
*/
#ifndef R_LIGHTMAP_H
#define R_LIGHTMAP_H

#include <stdint.h>
#include "xash3d_types.h"
#include "lightstyle.h" /* lightstyle_t */

#ifdef __cplusplus
extern "C" {
#endif

struct model_s;
struct cl_entity_s;

/* One per SORTED face (the archive's order, like nv2a_map_faces): where
   MpakFaceInfo says the face's style-0 block sits in the atlas, and the
   block size the baker computed (MpakFaceInfo extents / 16 + 1). Filled by
   nv2a_build_map_tables from the faceInfo lump; x == NV2A_LM_BLOCK_NONE
   for a face with no block, or one R_LightmapUpdate found does not agree
   with the engine's own surface (then it is never rewritten). */
typedef struct
{
	uint16_t x, y;
	uint8_t  smax, tmax;
} nv2a_lm_block_t;

#define NV2A_LM_BLOCK_NONE 0xFFFFu

/* Per-face bytes the lightmap tables hold for a map, beyond the atlas:
   the block above, one "holds a dynamic light" byte and at most one
   two-byte entry in the styled-face list. r_context.c's per-map ledger
   and the baker's mirror (Nv2aMapBudget.h kNv2aLightmapFaceBytes) count
   this per face. */
#define NV2A_LM_FACE_BYTES ( sizeof( nv2a_lm_block_t ) + 1 + 2 )

/* What r_context.c knows about the current map and view, handed over
   once per frame. */
typedef struct
{
	struct model_s        *world;       /* the engine's world model, == nv2a_map_owner */
	uint32_t              *atlas;       /* the GPU-visible RGBA8 atlas (write-combined) */
	uint32_t               atlas_width, atlas_height, atlas_stride_words;
	nv2a_lm_block_t       *blocks;      /* [face_count], sorted order */
	const uint32_t        *remap;       /* ORIGINAL face -> sorted, [remap_count] */
	uint32_t               remap_count;
	uint32_t               face_count;
	int                    framecount;  /* this frame's number, never 0 */
	/* True when sorted face `sorted` is drawn this frame by the world
	   pass: PVS and frustum (r_context.c's own tests, not a copy). */
	qboolean (*face_in_view)( uint32_t sorted );
} nv2a_lm_view_t;

/* Per-frame cost counters, for r_speeds-style inspection over the
   debugger (tools/xemu-debug.py) - nothing reads them in the build. */
typedef struct
{
	uint32_t frames;
	uint32_t faces_built;       /* this frame */
	uint32_t luxels_built;      /* this frame */
	uint32_t cycles;            /* this frame, rdtsc */
	uint32_t max_cycles;        /* since the map loaded */
	uint32_t total_faces_built; /* since the map loaded */
	uint32_t styled_faces;      /* world faces with a style other than 0 */
	uint32_t dlit_faces;        /* world faces a dynamic light reached this frame */
	uint32_t held_faces;        /* blocks holding a dynamic light right now */
	uint32_t rejected_blocks;   /* blocks that did not match the engine's surface, at map load */
	uint32_t list_overflows;    /* since the map loaded */
	uint32_t full_rebuilds;     /* style 0 changed: every block rebuilt */
	uint32_t budget_skips;      /* since the map loaded: faces left for a later frame (NV2A_LM_FRAME_LUXELS) */
} nv2a_lm_stats_t;

extern nv2a_lm_stats_t nv2a_lm_stats;

/* Once per frame, from GL_RenderFrame, after the view (PVS, frustum) is
   known and BEFORE the first draw of the frame that samples the atlas:
   the GPU must not be reading the atlas while this writes it (see
   GL_RenderFrame's comment at the call). `brush` lists the brush
   entities this frame will draw with their lightmap (culled already). */
void R_LightmapUpdate( const nv2a_lm_view_t *view, struct cl_entity_s **brush, int num_brush );

/* The map's per-map lightmap state goes with its tables
   (nv2a_free_map_resources). */
void R_LightmapFreeMap( void );

/* ref_interface_t's CL_RunLightStyles (ref_api.h:593): ref/common's
   CL_RunLightStyles, then notes that g_lightstylevalue now holds this
   map's values - R_LightmapUpdate does nothing before that, since the
   table is all zero until the client's first CL_EmitEntities
   (cl_frame.c:1291-1300) and would otherwise rebuild the whole atlas
   black. */
void R_LightmapRunLightStyles( lightstyle_t *ls );

#ifdef __cplusplus
}
#endif

#endif /* R_LIGHTMAP_H */
