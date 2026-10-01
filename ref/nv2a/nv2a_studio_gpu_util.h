/*
nv2a_studio_gpu_util.h - the pure half of ref/nv2a's GPU studio path
(docs/r3d/divergences.md #143): R_StudioDrawPoints' per-vertex skinning
and lighting moved from the CPU (gl_studio.c:1951-2160, ported in
r_studio.c) to the VP20 program vs_studio.vp.

This header owns everything that is arithmetic or layout and nothing that
touches pbkit, the engine or GPU memory, so tools/host-tests/
nv2a_studio_gpu_host_test.c can check it on the host:
  - the constant layout the program reads (c[n] is hardware constant
    n + 96: vp20compiler main.c:516) and the bone palette's slots;
  - the static geometry built once per submodel from the .mdl's tricmds:
    unique (vertex, normal, s, t) tuples per mesh, an indexed triangle
    list with the TriAPI assembler's own winding (nv2a_tri_util.c), and
    the palette (the bones a submodel, or failing that a mesh, uses);
  - the gamma table (LightToTexGamma) as 32 piecewise-linear segments,
    with its worst error against the real table;
  - the per-draw and per-mesh constants;
  - an exact C port of R_StudioLighting's colour result (the CPU path)
    and a float emulation of the program's arithmetic, so the host test
    can measure how often the two differ, and by how much.
*/
#ifndef NV2A_STUDIO_GPU_UTIL_H
#define NV2A_STUDIO_GPU_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* c[n] in vs_studio.vp is hardware constant n + 96. */
#define NV2A_SGPU_HW_C0          96u

/* The program's own constants (c[] numbering). c[0..4] belong to vs_tri/
   vs_lit and are never written by this path. */
#define NV2A_SGPU_C_MVP          5u   /* c[5..8] */
#define NV2A_SGPU_C_LIGHT        9u   /* ( ambient + shadelight, shadelight, 0, 1 ) */
#define NV2A_SGPU_C_MESH         10u  /* c[10..12], per mesh */
#define NV2A_SGPU_MESH_REGS      3u
#define NV2A_SGPU_C_FIXED        13u  /* c[13..15], constant */
#define NV2A_SGPU_FIXED_REGS     3u
#define NV2A_SGPU_C_LUT          16u  /* c[16..31] */
#define NV2A_SGPU_LUT_REGS       16u
#define NV2A_SGPU_LUT_SEGMENT    32u  /* table entries per segment; two segments per register */
#define NV2A_SGPU_C_END          32u  /* first c[] past the program's own constants: hardware 128 */

/* The palette: every hardware constant the program's own block and
   vs_tri/vs_lit's c[0..4] leave free. 24 bones in hardware 0..95
   (rows 0..71, light vectors 72..95) and 16 in 128..191 (rows 128..175,
   light vectors 176..191): 4 registers a bone, 40 bones. The retail
   census needs 37 at most once a submodel that needs more is split into
   per-mesh palettes (controller.mdl, 64 bones in one submodel). */
#define NV2A_SGPU_MAX_PALETTE    40u
#define NV2A_SGPU_LOW_BONES      24u

#define NV2A_SGPU_NO_PALETTE     0xFFFFu

/* Hardware slot of palette entry p's first matrix row (rows are p's slot
   +0, +1, +2) and of its light vector. */
uint32_t nv2a_sgpu_matrix_slot( uint32_t p );
uint32_t nv2a_sgpu_light_slot( uint32_t p );

/* The static vertex: 40 bytes, fetched as attributes 0 (position), 2
   (normal), 1 (the two A0 addresses) and 9 (raw s, t). */
typedef struct
{
	float pos[3];
	float norm[3];
	float addr[2]; /* matrix slot - 96, light slot - 96: the program's ARL operands */
	float st[2];   /* the tricmd's s, t, or HalfToFloat of them for STUDIO_NF_UV_COORDS */
} nv2a_sgpu_vertex_t;

typedef struct
{
	uint32_t first_index; /* into the submodel's index array; always even */
	uint32_t index_count; /* whole triangles, even (one degenerate pad triangle when 3*tris is odd) */
	uint32_t tris;        /* the mesh's real triangles */
	uint16_t palette;     /* NV2A_SGPU_NO_PALETTE: more bones than the palette holds, or normals
	                         outside the mesh's own range - CPU path */
	uint16_t uv_float;    /* built with HalfToFloat coordinates (STUDIO_NF_UV_COORDS) */
} nv2a_sgpu_mesh_t;

typedef struct
{
	uint32_t count;
	uint8_t  bone[NV2A_SGPU_MAX_PALETTE]; /* model bone index per palette entry, ascending */
} nv2a_sgpu_palette_t;

/* What the builder reads: one submodel of a studiohdr_t, already
   resolved to pointers (mstudiomodel_t's vertinfoindex/vertindex/
   norminfoindex/normindex and each mstudiomesh_t's triindex). */
typedef struct
{
	int             numverts;
	const uint8_t  *vertbone;
	const float    *verts;     /* numverts * 3 */
	int             numnorms;
	const uint8_t  *normbone;
	const float    *norms;     /* numnorms * 3 */
	int             nummesh;
	const int16_t *const *tricmds; /* per mesh */
	const uint8_t  *uv_float;  /* per mesh: STUDIO_NF_UV_COORDS under skin family 0 */
	/* per mesh: mstudiomesh_t.numnorms, or NULL to skip the check below.
	   The CPU path lights normals mesh by mesh, each with ITS mesh's face
	   flags (gl_studio.c:2012-2045), and a vertex reads
	   lightvalues[its normal] (:1433); the program lights a vertex with
	   the drawing mesh's flags. The two agree only when every normal a
	   mesh's tricmds use lies in that mesh's own normal range, so a mesh
	   that reaches outside it stays on the CPU (NV2A_SGPU_NO_PALETTE). */
	const int      *mesh_numnorms;
	/* studiohdr_t.numbones: a vertex or normal bone at or past it fails
	   the scan (it would index bonestransform/blightvec out of range);
	   0 skips the check. */
	int             numbones;
	float ( *half_to_float )( uint16_t h );
} nv2a_sgpu_source_t;

typedef struct
{
	uint32_t vertices;      /* unique vertices over all meshes */
	uint32_t indices;       /* u16 indices over all meshes, padded */
	uint32_t palettes;      /* 1 for a submodel palette, else nummesh */
	uint32_t max_mesh_verts;/* the largest mesh's tricmd vertex count (scratch sizing) */
} nv2a_sgpu_sizes_t;

/* Scratch the builder needs for a source whose largest mesh has
   max_mesh_verts tricmd vertices. */
size_t nv2a_sgpu_scratch_bytes( uint32_t max_mesh_verts );

/* Pass 1: the largest mesh's tricmd vertex count. Returns 0 (fails) for a
   malformed source: an index outside numverts/numnorms, a bone outside
   numbones, or a mesh of more
   than 65535 tricmd vertices. (A strip or fan shorter than 3 is not an
   error: it draws nothing on either path.) */
int nv2a_sgpu_scan( const nv2a_sgpu_source_t *src, uint32_t *max_mesh_verts );

/* Pass 2 (needs the scratch): the exact sizes. */
int nv2a_sgpu_measure( const nv2a_sgpu_source_t *src, void *scratch, nv2a_sgpu_sizes_t *sizes );

/* Pass 3: fills the arrays sized by nv2a_sgpu_measure (meshes has
   src->nummesh entries). Indices are absolute into `verts`. */
int nv2a_sgpu_build( const nv2a_sgpu_source_t *src, void *scratch, nv2a_sgpu_vertex_t *verts, uint16_t *indices,
	nv2a_sgpu_mesh_t *meshes, nv2a_sgpu_palette_t *palettes, const nv2a_sgpu_sizes_t *sizes );

/* The gamma table as the program reads it: segment k covers table indices
   32k..32k+31 with base = T[32k] / 1023.0f and slope = ( T[e] - T[32k] ) /
   1023.0f / ( e - 32k ), e = min( 32k + 32, 1020 ) (illum * 4 never
   exceeds 1020). `table` is the engine's lightgammatable, or NULL for
   ENGINE_LINEAR_GAMMA_SPACE (identity, gl_local.h:660-666). Returns the
   largest difference, in units of the colour byte (x255), between the
   segment value and T[i] / 1023.0f over i = 0..1020. */
float nv2a_sgpu_build_lut( const uint32_t *table, float out[NV2A_SGPU_LUT_REGS][4] );

/* The program's evaluation of the table at index i (0..1020), in the
   same float operations. */
float nv2a_sgpu_lut_eval( const float lut[NV2A_SGPU_LUT_REGS][4], uint32_t i );

/* c[13..15]. */
void nv2a_sgpu_fixed_consts( float out[NV2A_SGPU_FIXED_REGS][4] );

/* c[9], and the flat-shade illum (g_studio.ambientlight +
   g_studio.shadelight * 0.8f, gl_studio.c:1341) the mesh constants
   carry. */
void nv2a_sgpu_light_consts( float ambient, float shadelight, float out[4], float *flat_illum );

/* c[10..12] for one mesh. `face_flags` is g_nFaceFlags; `alpha` is the
   colour byte R_StudioSetColorBegin gives Color4ub ( blend * 255 );
   s_scale/t_scale are what the CPU path multiplies the tricmd s,t by
   (1/width, 1/height, or 1 for UV_COORDS) and tex_w/tex_h the bound
   texture's coordinate scale (nv2a_tex_coord_scale). */
void nv2a_sgpu_mesh_consts( int face_flags, float flat_illum, const float lightcolor[3], uint8_t alpha,
	float s_scale, float t_scale, float tex_w, float tex_h, float out[NV2A_SGPU_MESH_REGS][4] );

/* STUDIO_NF_* this header needs (engine/studio.h:71-94). */
#define NV2A_SGPU_NF_FLATSHADE  0x0001
#define NV2A_SGPU_NF_CHROME     0x0002
#define NV2A_SGPU_NF_FULLBRIGHT 0x0004
#define NV2A_SGPU_NF_UV_COORDS  ( 1U << 31 )

/* SHADE_LAMBERT (r_studio.c:39, ref/soft/r_local.h:62). */
#define NV2A_SGPU_SHADE_LAMBERT 1.4953241

/* The CPU path's colour bytes for one normal (r_studio.c
   R_StudioLighting :1593-1645, then VectorScale( lightcolor, lv ) and
   R_LightLambert's VectorScale( color, 255, out ) with no local lights).
   `table` as for nv2a_sgpu_build_lut. */
void nv2a_sgpu_ref_color( const uint32_t *table, float ambient, float shadelight, const float lightcolor[3],
	int face_flags, const float normal[3], const float blightvec[3], uint8_t out[3] );

/* The program's arithmetic for the same inputs, operation for operation
   (vs_studio.vp's lighting block), in IEEE single precision. */
void nv2a_sgpu_vp_color( const float lut[NV2A_SGPU_LUT_REGS][4], float ambient, float shadelight, const float lightcolor[3],
	int face_flags, const float normal[3], const float blightvec[3], uint8_t out[3] );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_STUDIO_GPU_UTIL_H */
