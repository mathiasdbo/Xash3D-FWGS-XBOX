/*
r_studio_gpu.h - ref/nv2a's GPU studio path (docs/r3d/divergences.md
#143): the private glue between r_studio.c (which decides, per submodel
and per mesh, whether R_StudioDrawPoints draws on the CPU or the GPU),
r_studio_gpu.c (the static per-submodel geometry and the GPU memory it
lives in) and r_context.c (the program, constants and draws).
*/
#ifndef R_STUDIO_GPU_H
#define R_STUDIO_GPU_H

#include "xash3d_types.h"
#include "studio.h"
#include "nv2a_studio_gpu_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The cache's GPU budget, charged by the bake-time fit check
   (tools/asset-studio/src/formats/Nv2aMapFit.h, kXboxStudioGpuCacheBytes;
   change the two together). Backed on demand in chunks of
   NV2A_STUDIO_GPU_CHUNK_BYTES, flushed with the skin cache before each
   map's GPU upload. A submodel's vertices are 40 bytes each: the largest
   retail submodel, loader.mdl's body, is under 128 KiB; a scientist is
   43 KiB over all seven of its submodels (host test, every retail model).
   The index lists, meshes and palettes live on the heap
   (NV2A_STUDIO_GPU_HEAP_BYTES at most): the CPU pushes the indices into
   the pushbuffer, and reading write-combined memory back is slow. */
#define NV2A_STUDIO_GPU_CACHE_BYTES   ( 1024u * 1024u )
#define NV2A_STUDIO_GPU_CHUNK_BYTES   ( 256u * 1024u )
#define NV2A_STUDIO_GPU_HEAP_BYTES    ( 256u * 1024u )
#define NV2A_STUDIO_GPU_MAX_ENTRIES   256u
/* Builds per frame: a build walks every tricmd of the submodel once, so a
   view that suddenly shows many new models spreads them over a few frames
   (drawn by the CPU path until then) - the skin cache's own rule
   (NV2A_STUDIO_SKIN_UPLOADS_PER_FRAME). */
#define NV2A_STUDIO_GPU_BUILDS_PER_FRAME 2u
#define NV2A_STUDIO_GPU_EVICT_MIN_AGE    2u
/* A submodel that could not get memory is not retried for this long. */
#define NV2A_STUDIO_GPU_RETRY_FRAMES     60u

typedef struct
{
	const nv2a_sgpu_vertex_t  *vb;          /* in a cache chunk (GPU-visible) */
	const uint32_t            *index_dwords;/* heap: two u16 indices per dword */
	const nv2a_sgpu_mesh_t    *meshes;
	const nv2a_sgpu_palette_t *palettes;
	int                        nummesh;
} nv2a_sgpu_geometry_t;

typedef struct
{
	uint32_t entries;      /* submodels with geometry resident */
	uint32_t gpu_used;     /* vertex bytes allocated within the backed chunks */
	uint32_t gpu_backed;   /* bytes of chunks backed */
	uint32_t heap_used;    /* index/mesh/palette bytes */
	uint32_t builds;       /* geometry builds since the last flush */
	uint32_t evictions;
	uint32_t failures;     /* builds refused (no room, too big, malformed) */
} nv2a_sgpu_cache_stats_t;

extern nv2a_sgpu_cache_stats_t nv2a_sgpu_cache_stats;

/* r_studio_gpu.c */
void nv2a_sgpu_cache_init( void );
void nv2a_sgpu_cache_shutdown( void );   /* GPU idle */
void nv2a_sgpu_cache_flush( void );      /* between frames (the skin cache's flush point) */
void nv2a_sgpu_cache_forget( const void *hdr ); /* a studio model is being freed */

/* The submodel's geometry, built on first use; NULL means draw it on the
   CPU this time. `ptexture`/`pskinref` are skin family 0's, as the CPU
   path resolves them (only STUDIO_NF_UV_COORDS is read: the
   coordinates' format). */
const nv2a_sgpu_geometry_t *nv2a_sgpu_cache_get( const studiohdr_t *hdr, const mstudiomodel_t *sub,
	const mstudiotexture_t *ptexture, int numtextures, const short *pskinref, int numskinref, uint32_t frame );

/* r_context.c */
/* Starts a submodel: loads c[5..9] (view-projection for the current
   view-model depth range, the light terms) and c[88..93] (the entity's
   local lights, nv2a_sgpu_elight_consts) when they differ from what the
   hardware holds, and, when lut_serial changed, the gamma table. Returns
   false when nothing may be drawn (the TriAPI's own Begin gate:
   renderer, frame or camera not ready). */
qboolean nv2a_sgpu_begin_submodel( const nv2a_sgpu_geometry_t *geo, const float light[4],
	const float lut[NV2A_SGPU_LUT_REGS][4], uint32_t lut_serial,
	const float elights[NV2A_SGPU_ELIGHT_REGS][4] );
/* Loads one palette: 3 matrix rows (bonestransform) and the light vector
   (blightvec) of every entry. */
void nv2a_sgpu_load_palette( const nv2a_sgpu_palette_t *pal, const float (*bonestransform)[3][4], const float (*blightvec)[3] );
/* Draws one mesh with the current GL-like state (nv2a_2d_desired, the
   bound texture, the fog). `mesh_consts` is c[10..12] without the bound
   texture's scale, which this fills in ([2][2], [2][3]). */
void nv2a_sgpu_draw_mesh( const nv2a_sgpu_geometry_t *geo, int mesh, float mesh_consts[NV2A_SGPU_MESH_REGS][4] );

#ifdef __cplusplus
}
#endif

#endif /* R_STUDIO_GPU_H */
