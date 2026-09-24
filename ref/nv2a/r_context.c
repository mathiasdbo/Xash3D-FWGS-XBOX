/*
r_context.c -- ref/nv2a bring-up (X9): a minimal ref_interface_t whose only
real job is proving pbkit can drive the NV2A through the actual engine
ref-module path, not just tools/x4-triangle's standalone program.

Every stub function below is copied verbatim in shape from ref/null/
r_context.c (upstream's own "run full client in headless environments"
module, and the proven precedent for "the whole ref_interface_t table can
be satisfied with trivial stubs plus a real R_Init_Video call"). Only
R_Init, R_Shutdown and R_EndFrame do real work: R_Init brings up pbkit and
uploads the shader/texture/vertex data, R_EndFrame draws one fixed
textured triangle every frame and presents it. The init/per-frame sequence
- pb_init, the vp20 program upload loop, the fp20 pixel-shader block, the
texture-stage register values, pb_wait_for_vbl/reset/target_back_buffer/
erase_depth_stencil_buffer/pb_finished/pb_show_front_screen - is
tools/x4-triangle/main.c's own already-used sequence, unmodified in
shape; see that file's own citations for where each piece originally came
from (deps/nxdk/samples/triangle and deps/nxdk/samples/mesh).

Deliberately out of scope for this bring-up (see docs/r3d/fork-plan.md's
X9 section and docs/r3d/divergences.md for the rest of the story): no
MPAK-derived geometry, no register combiner stages beyond the single one
X4 already proved (a plain textured triangle, no lighting), no VP20
skinning, no swizzled textures. This module uses the linear texture path,
whose X4 "verification" was itself wrong: tools/x4-triangle/main.c:60-65
feeds 0..1 UVs to an 8x8 LU_IMAGE texture, which NV2A addresses in texel
units, so it rendered a two-texel gradient that was eyeballed as a
checkerboard (main.c:150-152, "verified rendering the checkerboard
correctly"). X4's open swizzled-path defect (SZ_A8R8G8B8 + BASE_SIZE renders
a flat fill of texel (0,0)) is therefore still open AND was judged against a
wrong baseline; it is the same texture-addressing-convention class of
problem (which formats take texel vs normalized coordinates, and which are
sized by BASE_SIZE vs IMAGE_RECT - deps/nxdk/lib/pbkit/nv_regs.h:581-583)
that blacked out every map here, docs/r3d/divergences.md #100. Left
untouched in tools/x4-triangle on purpose.

One caveat that applies to every NV2A texture-addressing statement in this
file (stated here once rather than at each comment): both conventions -
linear textures take unnormalized texel coordinates and are sized by
IMAGE_RECT/CONTROL1, compressed textures are sized by the FORMAT word's
BASE_SIZE_U/V - are verified against xemu 0.8.136's source and corroborated
by nxdk's mesh sample, abaire/nxdk_pgraph_tests (whose goldens are hardware
captures) and Cxbx-Reloaded's XDK-derived headers, but have NOT been
measured on a physical Xbox by this project. xemu is not validation; a
real-console run is the only definitive close-out (divergences.md #100 (a)).
*/

#include <math.h>
#include <string.h>
#include "xash3d_types.h"
#include "const.h"
#include "entity_types.h" /* X9-M: ET_BEAM, for R_AddEntity */
#include "cvardef.h"
#include "ref_common.h"
#include "ref_params.h" /* X9-E: ref_viewpass_t's own real field layout and
                            RF_DRAW_WORLD - only forward-declared via
                            ref_api.h otherwise. ref/soft's own r_local.h
                            includes this same header for the same reason. */
#include "mpak_layout.h" /* filesystem/mpak_layout.h - types/constants only, see
                            nv2a_compute_mpak_layout()'s own comment for why the
                            offset-COMPUTING function body still needs its own
                            copy here rather than calling filesystem's real
                            Mpak_ComputeLayout() directly. */

#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

/* gEngfuncs/gpGlobals and GetRefAPI itself are ref/common/ref_context.c's
   job (it does `*funcs = gReffuncs;` against the plain, non-static table
   below) - xbox-ref-isolate.sh compiles ref/common alongside every module,
   ref/soft included, and ref/soft/r_context.c follows this exact same
   shape (a gReffuncs table only, no GetRefAPI of its own). An earlier
   version of this file copied ref/null's fully self-contained style
   instead (its own GetRefAPI + static gEngfuncs/gpGlobals) - ref/null
   never links ref/common at all (its own wscript's `use` list omits it),
   so that shape doesn't apply here; it produced a real duplicate-symbol
   link error (_nv2a_GetRefAPI defined by both this file and ref/common/
   ref_context.c) the first time this module was linked next to ref/soft. */

/*
========================================================================
Bring-up render state: one fixed textured triangle, tools/x4-triangle's
own vertex layout, checkerboard texture and vp20/fp20 shader pair.
========================================================================
*/

typedef struct
{
	float pos[3];
	float uv[2];
} __attribute__(( packed )) nv2a_test_vertex_t;

/* X9-C: the real map-geometry vertex format, adding a second UV pair for
   the lightmap stage. Position + diffuse UV stay first and in the SAME
   layout as nv2a_test_vertex_t on purpose - attribute slots 0 (position)
   and 9 (diffuse UV) are set up identically for both vertex formats, only
   the new slot 10 (lightmap UV, ref/nv2a/vs_lit.vs.cg's own TEXCOORD1 -
   confirmed by a real vp20compiler run to map there, not assumed) is new. */
typedef struct
{
	float pos[3];
	float uv0[2];
	float uv1[2];
} __attribute__(( packed )) nv2a_lit_vertex_t;

#define NV2A_TEST_TEX_SIZE_LOG2 3
#define NV2A_TEST_TEX_SIZE ( 1 << NV2A_TEST_TEX_SIZE_LOG2 ) /* 8x8 */
#define NV2A_TEST_MAXRAM 0x03FFAFFF

/* Same triangle and checkerboard as tools/x4-triangle/main.c, but with UVs
   in TEXEL units: the texture is bound LINEAR (LU_IMAGE_A8R8G8B8,
   nv2a_draw_test_triangle), and NV2A addresses linear textures
   unnormalized, 0..width x 0..height, like a GL rectangle texture (see the
   lightmap remap in nv2a_load_map_geometry for the evidence). X4's 0..1
   UVs (main.c:60-65) only ever covered texels (0..1, 0..1), so every
   screenshot of that triangle showed a smooth orange-to-white gradient - a
   bilinear blend of two texels - which X4/X9-A misread as "renders
   correctly". With texel-unit UVs and the mesh sample's own convolution
   filter constant or a plain-bilinear one, this triangle, booted live,
   no longer shows that 0..1-UV gradient - but still rendered as a smooth
   orange/white blend rather than a crisp 8x8 checkerboard, under both
   filters. Booted a third time with the bring-up-only NEAREST filter
   nv2a_draw_test_triangle() now sets (see its own FILTER comment), it
   renders a real, crisp 8x8 checkerboard (`.sweep/triangle_nearest.png`):
   the blend was bilinear/tent filtering a single-texel-period pattern,
   not a remaining UV bug. This triangle is the regression indicator for
   the texel-unit convention: any checker squares merging, or a return to
   a smooth gradient covering the WHOLE triangle, means linear-texture
   coordinates are broken again (the same bug that blacked out every map
   from X9-B to X9-I, docs/r3d/divergences.md #100).
   vs.inl (X4's own shader, tools/x4-triangle/vs.vs.cg:28 `result.tex0 =
   I.texcoord.xy;`) passes TEXCOORD0 through unscaled, so no shader change
   is involved. */
static const nv2a_test_vertex_t nv2a_test_verts[] =
{
	//  X     Y     Z       U                            V
	{{ -1.0f, -1.0f, 1.0f }, { 0.0f,                       (float)NV2A_TEST_TEX_SIZE }},
	{{  0.0f,  1.0f, 1.0f }, { 0.5f * NV2A_TEST_TEX_SIZE,  0.0f }},
	{{  1.0f, -1.0f, 1.0f }, { (float)NV2A_TEST_TEX_SIZE,  (float)NV2A_TEST_TEX_SIZE }},
};

#define MASK( mask, val ) ( ( ( val ) << ( __builtin_ffs( mask ) - 1 ) ) & ( mask ) )

static qboolean  nv2a_ready;
static uint32_t *nv2a_vb;
static void     *nv2a_tex;
static float     nv2a_viewport[4][4];
static int       nv2a_width, nv2a_height;

/* X9-B/C drew one hard-coded real face from a real baked map, instead of
   the fixed test triangle, once a map's own .mpak data was loaded. X9-D
   scales this to the whole map's own real geometry - the baked index
   buffer is already one flat, contiguous list of every face's own
   triangles concatenated in bake order (confirmed reading
   tools/asset-studio/src/formats/PakManager.cpp's own write sequence), so
   drawing all of it needs no per-FaceDrawCmd lookup at all; there is no
   longer a fixed vertex/index cap sized for one test face either - both
   buffers below are allocated at their real, per-map size. */
static qboolean  nv2a_map_ready;
static const model_t *nv2a_map_owner; /* which model_t's own Mod_ProcessRenderData(create=true)
                                          populated the state below - Mod_ProcessRenderData is
                                          called for EVERY model, world and non-world alike
                                          (engine/common/model.c), not just the map; without
                                          this, the first studio/sprite model loaded right after
                                          the world would wipe it on its own unrelated unload
                                          (codex review, real finding). */
static uint32_t *nv2a_map_vb;                       /* GPU-visible, nv2a_test_vertex_t[nv2a_map_vert_count] */
static uint32_t  nv2a_map_vert_count;
/* pbkit's own indexed-draw method (deps/nxdk/samples/mesh/main.c's
   draw_indices()) pushes index values inline into the pushbuffer as
   dwords - there is no separate GPU-resident index buffer to allocate,
   unlike the vertex array and the atlas below. Each dword packs TWO
   16-bit indices (deps/nxdk/samples/mesh/verts.h's own data - e.g.
   0x00010002 is indices {2, 1}, low half first) - codex review found an
   earlier version submitted one raw 32-bit "index" per dword instead,
   which silently zeroed every odd-positioned index's own upper 16 bits
   at the GPU. nv2a_map_dword_count counts DWORDS, not logical indices -
   an odd logical index count pads its final dword by repeating the last
   real index (a harmless degenerate triangle, not corrupt data). */
static uint32_t *nv2a_map_index_dwords;             /* Ordinary engine-pooled CPU memory (r_temppool,
                                                        Mem_Malloc/Mem_Free) - unlike nv2a_map_vb this
                                                        is only ever CPU-read into the pushbuffer, never
                                                        touched by the GPU directly, so it has no reason
                                                        to cost scarce contiguous write-combined memory
                                                        (codex review, real finding). Packed pairs,
                                                        sized to the real map's own iCount, not a fixed
                                                        bring-up cap. */
static uint32_t  nv2a_map_dword_count;

/* X9-K: real per-texture NV2A hardware-WRAP addressing, replacing the
   shared 2048x2048 DXT1 atlas (X9-B through X9-J) and its per-vertex
   atlas-rect remap - see this function's own MPAK v9 citation below and
   docs/r3d/fork-plan.md's X9-K entry for the full "architecture reversal"
   account. One contiguous GPU-visible buffer holds every texture's own
   DXT1 mip chain in directory order, each padded to 128 bytes (MPAK v10 -
   mpak_texture_info_t::dataLength rounded up to 128, summed) - MmAllocateContiguousMemoryEx, same allocation discipline the
   old atlas used. */
static void     *nv2a_map_texture_data;
/* Directory entry for one per-texture DXT1 image: byte offset into
   nv2a_map_texture_data (the running sum this module computes while
   parsing - mpak_texture_info_t itself stores no offset, matching the
   archive's own "byte layout order IS the format" convention one level
   down) plus the BASE_SIZE_U/V log2 fields NV097_SET_TEXTURE_FORMAT needs
   (a compressed texture's ONLY size on NV2A - see nv2a_draw_map_geometry's
   own stage-0 comment, unchanged reasoning from the atlas era). */
typedef struct
{
	fs_offset_t physOffset;
	uint32_t    log2Width;
	uint32_t    log2Height;
	uint32_t    mipLevels;  /* X9-N, MPAK v10: max(log2Width, log2Height) + 1 - the full chain to 1x1 */
} nv2a_map_texture_t;

/* X9-N, MPAK v10: byte length of a full DXT1 mip chain for a POT texture,
   base level down to 1x1 - every level takes whole 4x4 blocks, 8 bytes
   each, so a level under 4 texels on a side still costs one block per
   axis (the same size NV2A addresses for it). Mirrors the baker's
   buildDxt1MipChain (tools/asset-studio/src/formats/PakManager.cpp). */
static uint64_t nv2a_dxt1_chain_length( uint32_t log2w, uint32_t log2h, uint32_t *out_levels )
{
	uint32_t levels = ( log2w > log2h ? log2w : log2h ) + 1, l;
	uint64_t total = 0;

	for( l = 0; l < levels; l++ )
	{
		uint32_t w = ( l < log2w ) ? ( 1u << ( log2w - l )) : 1u;
		uint32_t h = ( l < log2h ) ? ( 1u << ( log2h - l )) : 1u;
		total += (uint64_t)(( w + 3 ) / 4 ) * (( h + 3 ) / 4 ) * 8;
	}
	if( out_levels )
		*out_levels = levels;
	return total;
}
/* Fixed-capacity static arrays, not a heap allocation sized off the
   header's own (validated) textureCount/batchCount the way nv2a_map_faces
   is sized off face_count - deliberately, matching this file's own
   established discipline for a bound this function cannot yet size
   exactly (the atlasWidth > 4096 check has the same shape: a hard cap,
   checked, not derived). 512 is a first, reasoned guess (retail HL1 maps
   commonly cite low hundreds of unique textures at most, and
   batchCount == textureCount architecturally - MpakHeader.h's own v9
   note), not a measured bound: the Gate phase re-bakes the real 125-map
   retail corpus and reports the true maximum, at which point this cap
   should be revisited with a cited number, the same way every other
   "reasonable guess pending real data" bound in this file already is. */
#define NV2A_MAP_MAX_TEXTURES 512
static nv2a_map_texture_t nv2a_map_textures[NV2A_MAP_MAX_TEXTURES];
static uint32_t  nv2a_map_texture_count;

/* One texture-draw batch: a contiguous run of SORTED-order faces (and the
   matching contiguous shared-index-buffer range) that all use the same
   texture - mirrors mpak_texture_batch_t (filesystem/mpak_layout.h)
   field-for-field. See nv2a_draw_map_geometry() for how this replaces the
   old single "draw the whole map, one texture" state setup with one
   texture bind per batch. */
typedef struct
{
	uint32_t textureIndex;
	uint32_t startFace;
	uint32_t faceCount;
	uint32_t startIndex;
	uint32_t indexCount;
} nv2a_map_batch_t;
#define NV2A_MAP_MAX_BATCHES 512 /* same "reasoned guess pending the Gate phase's real numbers" as
                                     NV2A_MAP_MAX_TEXTURES above - batchCount == textureCount
                                     architecturally, so this shares that cap's own reasoning. */
static nv2a_map_batch_t nv2a_map_batches[NV2A_MAP_MAX_BATCHES];
static uint32_t  nv2a_map_batch_count;

/* X9-K, the CRITICAL correctness bridge the whole batching change hinges
   on: ref/nv2a's own PVS marking (nv2a_mark_leaves) computes a face index
   from the ENGINE's own classic-lump-derived msurface_t array - ORIGINAL
   BSP face order, completely independent of and unaffected by this
   archive's own per-texture sort. Before X9-K that index doubled as a
   valid position into nv2a_map_faces/the vertex-remap output too, because
   BspParser's face loop was unsorted; now that mpak-side data is in
   SORTED order (grouped into the batches above), the two orders are two
   different numbers for the same face, and every place that bridges
   between them must translate explicitly instead of assuming "same face,
   same index" the way X9-G/X9-H's own comments (nv2a_mark_leaves,
   nv2a_draw_map_geometry's backface test) still literally say.
   nv2a_map_face_remap is parsed directly from the archive's new faceRemap
   lump (originalToSortedFaceIndex[], filesystem/mpak_layout.h's own v9
   note) - ORIGINAL face index -> SORTED position, exactly what
   nv2a_mark_leaves needs to translate world->surfaces' own index before
   writing nv2a_map_face_visible. nv2a_map_face_remap_count is that
   array's own length, i.e. the ORIGINAL BSP face count (self-describing
   from lenFaceRemap/4, per the same header note) - kept as its own
   variable rather than reusing nv2a_map_face_count (the SORTED/total
   count) because indexing INTO this array needs the array's own real
   bound, not a numerically-equal-by-construction different count, the
   same "bounds-check against the thing you're actually indexing"
   discipline this file applies everywhere else. */
static uint32_t *nv2a_map_face_remap;
static uint32_t  nv2a_map_face_remap_count;
/* The INVERSE of nv2a_map_face_remap - SORTED position -> ORIGINAL BSP
   face index - derived LOCALLY at load time (nv2a_load_map_geometry), by
   inverting the archive's own table; NOT a separate on-disk lump (the
   format only ships one direction - filesystem/mpak_layout.h's own v9
   note). Needed because nv2a_draw_map_geometry's own backface test reads
   world->surfaces[i] with `i` walking nv2a_map_faces in SORTED order -
   the exact same order-mismatch nv2a_mark_leaves' own remap exists to
   fix, just on the read side instead of the write side (see that
   function's own X9-H comment, "nv2a_map_faces[i] and world->surfaces[i]
   are the SAME face at the SAME index" - true before X9-K, false after,
   for this call site too). NULL = remap unavailable, or the archive's own
   table failed to invert as a clean bijection (nv2a_load_map_geometry
   validates this, not trusted blindly) - the backface test then degrades
   to "no backface test this batch", exactly like world == NULL already
   does, never a wrong-surface read. */
static uint32_t *nv2a_map_face_orig;

static void     *nv2a_map_lightmap;     /* GPU-visible, RGBA8, no format conversion needed -
                                            bound on texture stage 1 as LU_IMAGE_A8B8G8R8 (the
                                            baker's [R,G,B,A] byte order read as a little-endian
                                            word, see nv2a_draw_map_geometry's stage-1 comment),
                                            the same uncompressed LINEAR path X9-A's checkerboard
                                            uses on stage 0. That path was NOT "proven working"
                                            by X9-A as this comment once claimed: linear textures
                                            take texel-unit coordinates (file header), which the
                                            X9-A triangle and this lightmap both got wrong until
                                            X9-J, docs/r3d/divergences.md #100. */
static uint32_t  nv2a_map_lightmap_width, nv2a_map_lightmap_height, nv2a_map_lightmap_pitch;
static float     nv2a_map_viewproj[4][4];
static qboolean  nv2a_view_valid; /* X9-E: true only once GL_RenderFrame has
                                      processed a real world pass this
                                      session - R_EndFrame must not draw
                                      map geometry with whatever this
                                      matrix defaults to (all-zero BSS)
                                      before that first real frame, since
                                      the vertex shader's own w-divide
                                      turns an all-zero matrix into a
                                      divide by zero. Reset in R_Init()
                                      only - not tied to map load/unload,
                                      since a one-frame-stale-but-real
                                      matrix across a level transition is
                                      harmless. */
static qboolean  nv2a_view_captured_this_frame; /* X9-E, codex review, real
                                      finding: V_RenderView's own do/while
                                      loop (engine/client/cl_view.c) can
                                      call GL_RenderFrame more than once
                                      per real frame - a real client
                                      spectator/PIP inset pass is a second,
                                      real example, distinct RF_DRAW_WORLD
                                      viewport and all - and this module
                                      only ever draws ONE full-screen pass
                                      per frame (R_EndFrame), with no per-
                                      viewport rendering at all. Without
                                      this guard, a second pass would
                                      silently replace the primary camera
                                      with whatever that pass's own (likely
                                      tiny, off-center) view is and draw it
                                      full-screen - last-writer-wins,
                                      exactly backwards. Real per-viewport
                                      rendering is out of scope for this
                                      bring-up module; until it exists,
                                      only the FIRST real world pass each
                                      frame is captured, matching this
                                      project's own single, full-screen
                                      draw. Reset at the top of R_EndFrame,
                                      so it re-arms for the NEXT frame's
                                      own first GL_RenderFrame call, not
                                      the current one still in progress. */

/* X9-F: one entry per baked BSP face, built once at map load from the
   FaceDrawCmds lump (captured above) plus the already-remapped vertex
   positions in nv2a_map_vb - the baked format itself carries no bounding
   volume (tools/asset-studio/src/formats/MpakHeader.h's own MpakFaceInfo
   has planenum/texinfo/lightmap fields only, nothing spatial), so this is
   computed here, not read from the archive.

   dword_begin/dword_end address nv2a_map_index_dwords directly, snapped
   OUTWARD from the face's own [start_index,start_index+index_count) range
   to a multiple of 6 logical indices (= 3 dwords, since each dword packs
   two indices) - 6 is lcm(2,3), so a snapped boundary is simultaneously a
   dword boundary AND a whole-triangle boundary. A face can start at an
   odd logical index (real example, crossfire.mpak's own face 4: index
   45) where the enclosing dword also holds the tail of the PREVIOUS
   face's own triangles - submitting an unsnapped range would prepend a
   stray index and shift every triangle boundary in the draw, not just
   round a byte count. Over-draw from this snap is at most two triangles
   per emitted run; verified in Python against the real crossfire.mpak
   that snapping every face and re-merging contiguous runs reconstructs
   the exact same single 17,163-dword range this module already submits
   today when nothing is culled (docs/r3d/divergences.md #96). */
typedef struct
{
	vec3_t   mins, maxs;
	uint32_t dword_begin, dword_end;
	uint32_t index_begin, index_end; /* X9-M: this face's EXACT logical
	                                    index range [begin, end) - the
	                                    dword range above is snapped out to
	                                    6-index boundaries and can take in
	                                    a neighbouring face's triangles,
	                                    which is harmless between two world
	                                    faces of one texture but wrong
	                                    across a world/brush-entity
	                                    boundary (see nv2a_draw_exact_indices). */
	uint32_t texture;                /* X9-M: textureIndex of the batch
	                                    this sorted face belongs to, for
	                                    the brush-entity pass, which draws
	                                    one entity's faces across batches. */
	qboolean sky; /* X9-K: MpakFaceInfo::flags & MPAK_SURF_DRAWSKY for this
	                 face, read once at load (see the face-table build loop
	                 below) - ref/nv2a has never implemented a real skybox,
	                 and previously drew sky faces as ordinary textured
	                 geometry with the WAD's literal "sky" placeholder
	                 miptex; under the old shared-atlas remap that showed as
	                 one flat stretched patch (unremarkable), but under
	                 native WRAP addressing (X9-K) the same small texture
	                 tiles at high frequency across a typically-huge sky
	                 face - a real, visually obvious defect a live
	                 comparison against ref_soft caught (crossfire's sky
	                 rendered as a dense fence-like pattern instead of flat
	                 sky colour). A sky face is excluded from every draw run
	                 regardless of frustum/PVS visibility (see this file's
	                 own per-face cull check) - matching the reference's own
	                 minimum behaviour (never draw the sky's literal
	                 placeholder texture as normal geometry), short of
	                 implementing a real skybox, which stays future work. */
} nv2a_map_face_t;

static nv2a_map_face_t *nv2a_map_faces;      /* NULL = no per-face table this
                                                 map (never built, or the
                                                 FaceDrawCmds tiling failed
                                                 validation) - the draw path
                                                 falls back to submitting the
                                                 whole index buffer uncculled,
                                                 exactly like every X9 slice
                                                 before this one, rather than
                                                 drawing nothing. */
static uint32_t          nv2a_map_face_count;

/* X9-F: the real per-frame view frustum, built in GL_RenderFrame from the
   SAME forward/right/up/eye the view matrix already uses - see that
   function's own comment for why this means the frustum and the
   projection can never disagree about handedness or FOV the way two
   independently-derived cameras could. Port of ref/gl/gl_frustum.c's own
   GL_FrustumInitProj (LEFT/RIGHT/BOTTOM/TOP/FAR planes, inward-pointing
   normals, no near plane - the reference passes flZNear=0.0f from
   R_SetupFrustum and returns before setting one). */
static mplane_t nv2a_frustum[5];
#define NV2A_FRUSTUM_LEFT   0
#define NV2A_FRUSTUM_RIGHT  1
#define NV2A_FRUSTUM_BOTTOM 2
#define NV2A_FRUSTUM_TOP    3
#define NV2A_FRUSTUM_FAR    4

/* Bring-up diagnostic only, same discipline as debug_frame_count above -
   read via tools/xemu-debug.py mem, not part of the ref_interface_t
   contract. Updated once per real nv2a_draw_map_geometry() call. */
uint32_t nv2a_cull_faces_total;
uint32_t nv2a_cull_faces_visible;
uint32_t nv2a_cull_runs;
uint32_t nv2a_cull_dwords;
/* X9-H: counts only faces the frustum/PVS tests did NOT already reject
   but the backface test did - a LOGICAL face-classification count, not
   a guaranteed dword/bandwidth measurement (codex review, real finding:
   an earlier version of this comment overclaimed the latter). The
   run-merging logic below snaps each face's own index range OUTWARD to
   a 6-index boundary and then merges CONTIGUOUS runs - so a backface-
   culled face immediately adjacent (in bake order and dword range) to a
   surviving one can still have its own indices fall inside that
   neighbour's own submitted run, meaning this counter can increment
   with NO matching reduction in nv2a_cull_dwords for that specific
   face. It is still a real, useful number - how many faces this test
   alone judged backfacing, independent of whether their own bytes
   happened to piggyback on a surviving neighbour's run - just not the
   same thing as "dwords saved". A true bandwidth delta would need
   comparing nv2a_cull_dwords with this test on vs. off (two full passes
   or a shadow accumulator), not attempted here - disproportionate to
   what a bring-up counter needs to prove real geometry is being
   classified, which is what this one actually does. */
uint32_t nv2a_cull_faces_backface_only;

/* Mirrors ref/gl's own r_nocull (ref/gl/gl_local.h) in name and meaning -
   a private, module-local convar (CVAR_DEFINE_AUTO, the same pattern
   ref/soft/r_main.c uses for its own sw_ and r_novis convars), not one of
   the engine's shared convars. Exists so the live A/B check
   divergences.md #96 describes (culling on vs. off must render
   identically) is a real toggle, not an assertion. */
static CVAR_DEFINE_AUTO( nv2a_r_nocull, "0", 0, "ref_nv2a: disable frustum culling" );

/* X9-G: PVS culling, mirroring ref/gl's own r_novis (gl_rmain.c) in name
   and meaning - same private, module-local convar pattern as
   nv2a_r_nocull above, for the same reason (a real A/B toggle, not an
   assertion). CHECKVISBIT/MAX_MAP_LEAFS are ordinarily engine-internal
   (engine/common/mod_local.h - ref/gl includes that header directly,
   sharing a build with the engine); ref/nv2a is an isolated static-lib
   unit (tools/xbox-ref-isolate.sh, same isolation nv2a_mpak_geometry_t's
   own comment above already documents for the mpak layout types) with no
   reason to pull in mod_local.h's much larger surface (edict_t,
   entvars_t, studio.h) for two constants - a private, narrowly-scoped
   copy is the cheaper divergence, same call mod_local.h's real values
   are mirrored from verbatim.

   nv2a-xbox-specialist review, real finding: an earlier version of this
   block also mirrored mod_local.h's own FATPVS_RADIUS (8.0f) and used it
   as the R_FatPVS radius below - wrong constant. FATPVS_RADIUS is a
   SERVER-side value (engine/common/sv_game.c's own entity-visibility PVS/
   PHS checks); the CLIENT RENDERER'S own real radius is the shared
   r_pvs_radius cvar (engine/client/dll_int/ref_common.c, default 0.1, 80x
   smaller) - the exact one ref/gl (gl_rsurf.c) and ref/soft (r_main.c)
   both actually pass to their own R_FatPVS calls. At 8.0 instead of 0.1,
   Mod_FatPVS's own split-plane recursion (mod_bmodel.c) would OR together
   both children's PVS rows for every splitting plane within 8 world units
   of the camera - not just the boundary-straddling case the crossing-test
   above already handles - silently inflating the visible set in ordinary
   GoldSrc-density geometry (doorways, vents, stairwells) and defeating a
   real fraction of this slice's own cull ratio with no visual symptom to
   catch it. Fixed below by using r_pvs_radius directly - REAL, SHARED
   (DECLARE_ENGINE_SHARED_CVAR_LIST(), ref_common.h, already in scope),
   not a second private mirror - the cheapest correct form, and it stays
   in sync with whatever the user or a future tuning pass sets it to. */
#define NV2A_MAX_MAP_LEAFS 131072 /* common/bspfile.h's own MAX_MAP_LEAFS */
#define NV2A_CHECKVISBIT( vis, b ) ( (b) >= 0 ? (byte)(( vis )[( b ) >> 3] & ( 1 << (( b ) & 7 ))) : (byte)0 )

static CVAR_DEFINE_AUTO( nv2a_r_novis, "0", 0, "ref_nv2a: disable PVS culling" );

/* Mirrors ref/gl's own r_lockpvs (gl_local.h/gl_rsurf.c): a development
   aid that freezes the visibility table at its current contents so the
   camera can be moved around to inspect what a given PVS state actually
   covers, without a real leaf crossing recomputing it out from under the
   inspection. reference-renderer-specialist review flagged this as
   missing from an earlier version of this port. */
static CVAR_DEFINE_AUTO( nv2a_r_lockpvs, "0", 0, "ref_nv2a: freeze the PVS visibility table (dev aid)" );

/* One byte per baked face (nv2a_map_face_count entries), allocated
   alongside nv2a_map_faces at map load - see nv2a_load_map_geometry()'s
   own X9-G addition. NULL = no PVS table for this map (allocation
   failed, or nv2a_mark_leaves() has never matched WORLDMODEL to
   nv2a_map_owner) - nv2a_draw_map_geometry() treats that the same
   "fail open, draw it anyway" way nv2a_map_faces == NULL already does
   for frustum culling. */
static byte *nv2a_map_face_visible;

/* The engine's own already-loaded world model's notion of "which leaf is
   the camera in right now" - mnode_s/mleaf_s (common/com_model.h),
   reachable here as plain pointers with no extra include, since
   ref_common.h (included above) already pulls in engine/ref_api.h ->
   common/com_model.h for gEngfuncs's own function-pointer signatures.
   Reset to NULL in nv2a_free_map_resources() on every map unload/reload -
   see that function's own comment for why a stale pointer here would be
   a real dangling-pointer risk, not just a stale cache. */
static mleaf_t *nv2a_viewleaf;
static mleaf_t *nv2a_oldviewleaf;

/* X9-G, render-architecture-specialist review: which world model
   nv2a_mark_leaves() has already parity-checked against nv2a_map_faces
   (below) - reset alongside nv2a_viewleaf/nv2a_oldviewleaf on every
   unload, so a new map gets its own fresh check rather than skipping it
   because an old, different model's own pointer happened to match. */
static const model_t *nv2a_pvs_parity_checked_for;

/* The real PVS row for nv2a_viewleaf, filled by gEngfuncs.R_FatPVS() -
   same size and bit convention ref/gl's own RI.visbytes (gl_local.h)
   uses, sized off NV2A_MAX_MAP_LEAFS above rather than off
   mod_local.h's real MAX_MAP_LEAFS for the same "no extra include" reason
   the constant itself is mirrored, not shared. */
static byte nv2a_visbytes[( NV2A_MAX_MAP_LEAFS + 7 ) / 8];

/* X9-H: backface culling. Port of ref/gl's own default world-draw cull
   state: R_SetupGL(true) (gl_rmain.c - reference-renderer-specialist
   review, real correction: an earlier version of this comment named
   GL_SetupGL, which does not exist; the real function is R_SetupGL,
   called as R_SetupGL(true) from R_RenderScene) calls GL_Cull(GL_FRONT)
   once per frame, BEFORE R_MarkLeaves/R_DrawWorld/R_RecursiveWorldNode/
   R_DrawTextureChains run, and no GL_Cull call anywhere in gl_rsurf.c
   itself overrides it for world surfaces - confirmed by grepping every
   GL_Cull call site under ref/gl, not assumed - so R_CullSurface's own
   GL_FRONT branch (gl_cull.c:96-104) applies uniformly to every
   ordinary world surface X9 currently draws, sky surfaces included
   (R_RecursiveWorldNode's own SURF_DRAWSKY branch runs AFTER
   R_CullSurface, not instead of it) - confirmed true for the reference
   itself, not merely "doesn't matter yet for this port's own narrower
   scope": R_CullSurface is called identically for opaque, SURF_DRAWTURB
   and SURF_TRANSPARENT surfaces alike in R_RecursiveWorldNode
   (gl_rsurf.c:3457-3534); the type-based split into wateralpha/alpha-
   surface draw passes happens strictly LATER, in R_DrawTextureChains,
   on whatever already survived this same cull test - there is no
   surface-type exemption from backface culling anywhere in the
   reference's own cull decision. One real, disclosed simplification:
   the reference's own input is tr.modelorg (RI.cullorigin, set from
   rvp.vieworigin unless r_lockfrustum is active - gl_rmain.c), not raw
   vieworigin directly; this port's nv2a_eye_origin is captured straight
   from rvp->vieworigin, which is EQUAL to tr.modelorg for this fork's
   own real single-player path (no mirror/portal path exists here to
   reassign cullorigin differently, and r_lockfrustum defaults off) -
   an equivalence for the case that actually runs, not an identity, and
   worth re-checking if a lockfrustum-equivalent or a mirror/portal pass
   is ever added to this module. SURF_PLANEBACK is mirrored locally
   (common/bspfile.h's own value, BIT(1) = 0x0002) rather than pulling in
   bspfile.h for one constant - same "private, narrowly-scoped copy"
   precedent NV2A_MAX_MAP_LEAFS/NV2A_CHECKVISBIT already established
   above. BACKFACE_EPSILON and PlaneDiff() are NOT mirrored - both are
   already in scope verbatim (common/com_model.h via ref_common.h's own
   include of engine/ref_api.h; public/xash3d_mathlib.h via
   ref_common.h's own direct include) - reusing the real ones here is
   strictly cheaper than mirroring, and there is no isolation reason not
   to (they are header-only macros/constants, not linked symbols).

   nv2a-xbox-specialist review, real finding this comment did not
   originally disclose: the NV2A already has its OWN, always-on,
   hardware backface cull, unconditionally enabled since X9-A and never
   touched since (deps/nxdk/lib/pbkit/pbkit.c: NV20_TCL_PRIMITIVE_3D_
   CULL_FACE_ENABLE=1, FRONT_FACE=clockwise screen-space winding,
   CULL_FACE=back) - grepped the whole module for CULL/WINDING/
   FRONT_FACE and confirmed nothing here ever reprograms it. This CPU
   test is therefore a SECOND, independently-computed cull decision
   layered on top of an already-active one, not the introduction of
   backface culling where none existed - its real value is skipping the
   vertex-transform and pushbuffer-submission cost for faces the GPU's
   own post-transform winding test would have discarded anyway, not
   correctness the GPU test lacked. The two tests must AGREE (this CPU
   plane/SURF_PLANEBACK test vs. the GPU's screen-space winding test) or
   this test can create real holes in walls the GPU alone would have
   drawn - a one-directional failure mode (this test being too
   permissive is merely redundant with the GPU's own cull; too
   aggressive is a visible defect). By construction they should agree:
   the baker's own fan triangulation (tools/asset-studio's BspParser)
   walks each face's real edge loop the identical way ref/gl/ref/soft's
   own glpoly_t reconstruction does and preserves its rotational sense
   with no reversal, so SURF_PLANEBACK (computed against that same
   preserved winding) and the GPU's clockwise test are testing the same
   underlying geometry the same way. Real maps have also been rendering
   correctly under the GPU's own always-on cull since X9-B, which is
   circumstantial support. Neither is a substitute for actually
   checking: the concrete, cheap verification step for whoever next has
   live-boot access is an nv2a_r_nocullback 0 vs 1 A/B walk of a real
   level looking for any face that only appears with this test
   disabled - nobody has done that check yet, only reasoned about why
   it should pass. */
#define NV2A_SURF_PLANEBACK BIT( 1 )

/* Mirrors ref/gl's own r_nocull/r_novis precedent for this module's
   third cull test - a real, independent A/B toggle. No reference
   analog exists for this SPECIFIC name (ref/gl's own backface culling
   is controlled by glState.faceCull, an internal state variable, not a
   user-facing convar), so this is this module's own dev-aid, in the
   same spirit as nv2a_r_nocull/nv2a_r_novis rather than a port of a
   named reference convar.

   codex review, real finding, high confidence (0.99): this test's own
   correctness depends on agreeing with the NV2A's separate, always-on
   HARDWARE backface cull (see NV2A_SURF_PLANEBACK's own declaration
   comment) - a real, disclosed, but genuinely UNVERIFIED risk this
   session could not close (no live-boot access - divergences.md #98's
   own honest verification gap). Shipping a correctness-risking cull
   test ENABLED by default, with the one check that would confirm it
   safe explicitly not yet done, is not consistent with this project's
   own "compile and boot before calling anything done" discipline - so,
   default to "1" (DISABLED) rather than "0": this test exists in the
   tree, ready to enable and verify the moment live-boot access exists
   (an nv2a_r_nocullback 0 vs 1 A/B walk, exactly as already planned),
   but does not silently risk real geometry on every boot until that
   verification actually happens. Flip the default to "0" in a follow-up
   commit once that A/B walk confirms no face disappears with it
   enabled - cite the verification when that commit lands, the same way
   X9-F's own nv2a_r_nocull A/B was later confirmed live
   (divergences.md #96). */
static CVAR_DEFINE_AUTO( nv2a_r_nocullback, "1", 0, "ref_nv2a: disable backface culling (default ON - unverified live, see divergences.md #98)" );

/* X9-I: native alpha test for masked ('{'-prefixed - fences, grates,
   ladders, vents) world textures, now that XB's atlas actually carries
   real per-pixel alpha (tools/asset-studio, PR #123) instead of
   silently dropping it.

   Reference fidelity, checked rather than assumed (divergences.md
   #99): ref/gl and ref/soft disagree on the mechanism, and neither is
   ported here verbatim. ref/gl applies NO alpha test to a masked
   WORLD-brush surface outside ENGINE_QUAKE_COMPATIBLE
   (gl_rsurf.c:1561's own gating, and gl_rmain.c:832's own "quake-
   specific feature" comment on the deferred alpha pass that condition
   guards) - retail HL masked world brushes only get real cutout in
   ref/gl when tied to a separate entity with rendermode
   kRenderTransAlpha (gl_rsurf.c:1739/1752, GL_GREATER 0.25), which is
   how real GoldSrc maps actually use masked textures on worldspawn
   geometry (a mapping convention, not this renderer's own invention).
   ref/soft instead does unconditional per-texel masking with no
   entity/rendermode gate at all: a hardcoded sentinel palette index
   (TRANSPARENT_COLOR, r_local.h:588) baked in at decode time for any
   '{'-prefixed texture and skipped during span rasterization
   (r_scan.c, six call sites), independent of SURF_TRANSPARENT,
   quake-compat, or which entity (if any) owns the surface. X9 has no
   entity render list at all yet (only the static baked world model
   draws) - porting ref/gl's mechanism would mean building that
   machinery just for this, so this slice matches ref/soft's simpler,
   unconditional mechanism instead, applied per-face via
   SURF_TRANSPARENT (below), not glAlphaFunc's graded 0.25 threshold
   (BC1 alpha is already binary by construction here, so "graded"
   doesn't apply). A cheaper divergence than reference-porting the
   whole entity/rendermode path for one feature - see divergences.md
   #99 for the full account and what porting ref/gl's own mechanism
   would require if X9 ever gains real entity rendering.

   NV2A has a genuine fixed-function alpha-test
   stage - NV097_SET_ALPHA_TEST_ENABLE/_FUNC/_REF (deps/nxdk/lib/pbkit/
   nv_regs.h:223,245-254), the same registers D3D8's
   D3DRS_ALPHATESTENABLE/_ALPHAFUNC/_ALPHAREF map to on this hardware -
   not a shader instruction; fp20 on this GPU has no per-pixel
   conditional discard (ps_lit.inl compiles to register-combiner STATE,
   not an instruction stream). ps_lit.inl's own combiner setup routes
   the diffuse atlas's own real, UNDOUBLED alpha into the final
   fragment alpha the test stage compares against - ps_lit.ps.cg's own
   comment has the full account of why this had to be split out from
   color explicitly.

   codex review, real finding, confirmed by reading the actual compiled
   combiner output: the first version of this comment claimed alpha
   was "strictly binary, so doubling never matters" - true of the raw
   BC1-decoded texel values, but false of what the alpha-TEST stage
   actually samples. NV097_SET_TEXTURE_FILTER (this function's own
   texture-stage-0 setup, above) enables real bilinear filtering on the
   diffuse atlas, and a texture unit filters every channel it samples,
   alpha included - a fragment landing between a transparent and an
   opaque texel at a masked cutout's own edge genuinely gets an
   intermediate (non-binary) alpha, by design: that's what makes a
   filtered alpha test antialias a cutout edge instead of aliasing it.
   The bug the first version of this comment missed was not that
   intermediate values exist, but what doubling did to them: comparing
   a DOUBLED value against a reference meant to mean "50% opaque"
   silently moves the real pass/fail line to 25%, not 50% - real, not
   hypothetical, exactly codex's own finding. Fixed at the source
   (ps_lit.ps.cg), not by picking a different NV097_SET_ALPHA_REF to
   compensate for a scaling this path has no principled reason to keep:
   alpha no longer goes through NV097_SET_COMBINER_ALPHA_OCW_OP_
   SHIFTLEFTBY1 at all (confirmed in the real cgc/fp20compiler output,
   not assumed - the compiled combiner now reads `out.a =
   unsigned(tex0.a)`, no scale_by_two() on that channel), so
   NV097_SET_ALPHA_REF=0x80 now means what it always claimed to: a
   fragment passes when its real, correctly-filtered diffuse alpha is
   at least 50% opaque, and a bilinear-blended cutout edge antialiases
   around that boundary correctly instead of a silently shifted one.

   This is a GLOBAL per-draw-call toggle (NV2A has no per-triangle alpha
   test within one draw submission), not a per-face one, which would
   normally mean carefully bucketing masked-vs-opaque faces into
   contiguous index runs the way X9-F's frustum cull already does for
   visibility. That bucketing turns out to be unnecessary here: the
   atlas's own DXT1 encoder (tools/asset-studio's TextureOpt.cpp,
   compressToDXT's blockNeedsTransparency gate, same PR) only ever lets
   rgbcx assign a block's transparent selector when that SPECIFIC block
   contains a real alpha<128 source texel - confirmed not just by
   reading that code but by exhaustively decoding and comparing every
   texel of every successfully-baked map in the full 125-map retail
   corpus against its pre-compression source (121 of the 125 real
   retail .bsp maps that exist to check - the other 4 fail to bake for
   an unrelated, already-tracked reason, see fork-plan.md's XB section -
   0 opaque-source-but-transparent-decoded mismatches on any of the 121,
   tools/verify-atlas-alpha.py). That gate is a fix-or-no-op design, not
   a structural proof: TextureOpt.cpp's own fixOpaqueTexelsPunchedThrough
   explicitly ships a violation unfixed rather than guess if its own
   retry can't verify itself correct (TextureOpt.cpp's own comment: "A
   block that fails verification keeps whatever compressToDXT already
   produced"). So "cannot punch a hole in ordinary opaque geometry" is
   an empirical result over every retail map actually checked, not a
   guarantee that holds for arbitrary future content this baker hasn't
   seen - re-run tools/verify-atlas-alpha.py against anything new before
   trusting this same reasoning for it.

   Still shipped DISABLED by default, matching nv2a_r_nocullback's own
   precedent immediately above: this session has no live-boot access to
   actually watch a masked texture render correctly (or watch for any
   regression) with this on. Flip the default once that A/B check
   happens and cite it, the same way nv2a_r_nocullback's own comment
   documents what verification is still owed. */
static CVAR_DEFINE_AUTO( nv2a_r_noalphatest, "1", 0, "ref_nv2a: disable native alpha test for masked textures (default ON - unverified live)" );

/* X9-K: retro/point-filtered diffuse, independent of the lightmap's own
   filtering - mirrors ref/gl's own real, checked precedent: gl_texture_nearest
   (ref/gl/gl_opengl.c:11) and gl_lightmap_nearest (ref/gl/gl_image.c:83) are
   TWO SEPARATE cvars there, i.e. GoldSrc's own convention keeps the lightmap
   smoothly interpolated even in a nearest/retro diffuse mode - not one convar
   driving both stages. Named nv2a_r_ like every other module-local convar in
   this file (nv2a_r_nocull/novis/lockpvs/nocullback/noalphatest above), not
   ref/gl's own gl_texturemode: that name belongs to a differently-scoped,
   differently-typed convar on the OTHER ref backend and would be misleading
   reused here. Read fresh every nv2a_draw_map_geometry() call, same as
   nv2a_r_noalphatest above, so toggling it mid-session takes effect on the
   very next frame. NV097_SET_TEXTURE_FILTER's NEAREST value is 0x01012000
   (MIN=MAG=NEAREST, kernel field 1 as in Perfect Dark X's gfx_nv2a.cpp):
   X9-J's 0x01010000 passed in xemu but halted real hardware with an
   invalid-data error on method 0x1b14 - the kernel field (bits 13-15)
   must be non-zero. Default "0" (off, unchanged - bilinear -
   behaviour): this is a cosmetic dev/retro-look toggle, not a correctness
   fix pending verification like nv2a_r_nocullback/nv2a_r_noalphatest above,
   so it needs no "ship disabled until A/B'd" caution - the filter value
   itself is already proven, only which stage it's applied to is new here. */
static CVAR_DEFINE_AUTO( nv2a_r_texturenearest, "0", 0, "ref_nv2a: point-filter the diffuse stage only (lightmap always stays bilinear)" );

/* The real, live camera eye position, captured once per real frame by
   GL_RenderFrame alongside nv2a_map_viewproj/nv2a_frustum - needed here
   because nv2a_draw_map_geometry() (called later the same frame, from
   R_EndFrame) has no ref_viewpass_t of its own to read vieworigin from.
   Valid exactly when nv2a_view_valid is true - see that flag's own
   declaration comment for why a map loaded before the first real frame
   must never read whatever this defaults to (all-zero BSS is a
   perfectly valid, if wrong, coordinate here, unlike nv2a_map_viewproj's
   own divide-by-zero risk - so nv2a_view_valid still gates every use of
   this, for consistency, not because zero itself would crash anything). */
static vec3_t nv2a_eye_origin;

static void nv2a_matrix_viewport( float out[4][4], float x, float y, float width, float height, float z_min, float z_max )
{
	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = width / 2.0f;
	out[1][1] = height / -2.0f;
	out[2][2] = z_max - z_min;
	out[3][3] = 1.0f;
	out[3][0] = x + width / 2.0f;
	out[3][1] = y + height / 2.0f;
	out[3][2] = z_min;
}

/*
========================================================================
X9-B: real baked geometry, one real face.

tools/x4-triangle/vs.vs.cg's own shader (ref/nv2a/vs.inl) does exactly one
thing: `position = mul(float4(pos.xyz,1), m_viewport); position.xyz /=
position.w;` - a single row-vector matrix multiply plus the perspective
divide already baked in. nv2a_matrix_viewport() above is the flat,
w-always-1 NDC-to-screen half of that for the X9-A test triangle (whose
own vertices are hand-authored already inside [-1,1]). Real MPAK world
vertices need a real view + perspective transform PREPENDED before that
same NDC-to-screen matrix - no shader change needed, just a different
4x4 constant, so this is real (if minimal) camera math, not a hack.
========================================================================
*/

typedef struct { float x, y, z; } nv2a_vec3_t;

/* X9-E: sub/cross/normalize are gone - they only ever existed to
   re-derive orthonormal axes from an eye/target/up hint for X9-A
   through D's own fixed, one-time debug camera, removed now that the
   real per-frame camera gets already-orthonormal axes directly from
   the engine's own AngleVectors(). dot survives - nv2a_matrix_view_from_axes()
   still needs it for the translation row. */
static float nv2a_vec3_dot( nv2a_vec3_t a, nv2a_vec3_t b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* result = a * b (row-vector convention: v' = v * result = v * a * b). */
static void nv2a_matrix_multiply( float result[4][4], const float a[4][4], const float b[4][4] )
{
	int i, j, k;
	float tmp[4][4];

	for( i = 0; i < 4; i++ )
	{
		for( j = 0; j < 4; j++ )
		{
			float sum = 0.0f;
			for( k = 0; k < 4; k++ )
				sum += a[i][k] * b[k][j];
			tmp[i][j] = sum;
		}
	}
	memcpy( result, tmp, sizeof( tmp ));
}

/* X9-A through D used a look-at built from an eye/target/up HINT
   (D3DXMatrixLookAtLH-style: re-deriving orthonormal axes via
   normalize/cross from three arbitrary points) for their own fixed,
   one-time debug camera - removed in X9-E, since the real per-frame
   camera (GL_RenderFrame) already has real, orthonormal forward/right/
   up axes straight from the engine's own AngleVectors(), and re-
   deriving axes from a hint would be redundant and would not
   necessarily reproduce AngleVectors()'s own exact axes anyway. Same
   output shape a look-at matrix would have (right/up/forward become the
   matrix's own three column vectors, translation via negative dot
   products against eye) - still chains into
   nv2a_matrix_perspective()/nv2a_matrix_viewport() unchanged, only how
   the axes are OBTAINED differs. */
static void nv2a_matrix_view_from_axes( float out[4][4], nv2a_vec3_t eye, nv2a_vec3_t forward, nv2a_vec3_t right, nv2a_vec3_t up )
{
	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = right.x;   out[1][0] = right.y;   out[2][0] = right.z;
	out[0][1] = up.x;      out[1][1] = up.y;      out[2][1] = up.z;
	out[0][2] = forward.x; out[1][2] = forward.y; out[2][2] = forward.z;
	out[3][0] = -nv2a_vec3_dot( right, eye );
	out[3][1] = -nv2a_vec3_dot( up, eye );
	out[3][2] = -nv2a_vec3_dot( forward, eye );
	out[3][3] = 1.0f;
}

/* Standard left-handed row-vector perspective projection (matches
   D3DXMatrixPerspectiveFovLH): outputs NDC x,y in [-1,1] and z in [0,1]
   (near=0, far=1) in the SAME convention nv2a_matrix_viewport() above
   already expects as ITS OWN input - chaining lookat * perspective *
   nv2a_matrix_viewport reuses that function completely unchanged.
   xScale/yScale are precomputed by the caller either way: X9-A/D's own
   fixed debug camera used a literal 1.0 for yScale and derived xScale
   from a fixed aspect (a compile-time 90-degree FOV, no runtime tan()
   needed); X9-E's own real per-frame camera instead computes yScale
   from the engine's own real, live fov_y each frame.

   X9-F, codex review, real finding: this used to take (yScale, aspect)
   and derive xScale = yScale/aspect, silently assuming fov_x is always
   exactly the aspect-consistent angle for fov_y - true at this port's
   fixed 640x480/4:3 target under ordinary rendering (V_AdjustFov,
   engine/client/cl_view.c, returns early exactly at 4:3, and V_CalcFov's
   own math makes tan(fov_x/2) == aspect*tan(fov_y/2) exactly whenever it
   does), but NOT an invariant the reference engine itself relies on:
   V_ApplyRefUnderwater (cl_view.c) warps fov_x and fov_y by DIFFERENT,
   independent factors when set (gated behind RF_DRAW_CUBEMAP/
   RF_DRAW_OVERVIEW - see GL_RenderFrame's own frustum-construction
   comment for why that gate makes this specific warp unreachable on
   this port's own real, single-player-only world pass today, same as
   ref/nv2a's own already-documented per-viewport limit). ref/gl's own
   real projection (gl_rmain.c's R_SetupProjectionMatrix) never took this
   shortcut either - it derives xMax from tan(fov_x/2) and yMax from
   tan(fov_y/2) completely independently, matching the NEW frustum code
   below exactly. Taking xScale directly, the same way, removes the
   assumption entirely rather than merely documenting it - a real fix,
   not just a defensive comment, and closer to the reference than this
   function was before X9-F touched it at all. */
static void nv2a_matrix_perspective( float out[4][4], float xScale, float yScale, float zNear, float zFar )
{
	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = xScale;
	out[1][1] = yScale;
	out[2][2] = zFar / ( zFar - zNear );
	out[2][3] = 1.0f;
	out[3][2] = -zNear * zFar / ( zFar - zNear );
}

/*
========================================================================
X9-B: a private, narrowly-scoped mirror of filesystem/mpak_layout.c's own
Mpak_ComputeLayout() - NOT a call to it. ref/ and filesystem/ are separate
isolated static-lib units (tools/xbox-ref-isolate.sh), each only reachable
from the engine through its own function-pointer API (ref_api_t /
GetFSAPI), never directly from each other - so this module cannot link
against filesystem/mpak_layout.c's real function body, only #include its
header for the shared type/constant definitions (mpak_header_t,
mpak_vertex_t, MPAK_FLAG_NV2A_BLOCK, MPAK_SECTOR_SIZE, ...), which cost
nothing to reuse since they carry no linkage.

This mirror walks every section in the SAME order for the SAME reason the
real one does (no offset table - byte order IS the format), but only
CAPTURES the ones this slice actually reads (vertices, indices,
faceDrawCmds, the v9 per-texture directory/data/batches/faceRemap
quartet, lightmapAtlas); every other section still has to be stepped over
correctly to reach the ones that matter. X9-F adds faceDrawCmds to the
capture list - X9-D's own comment here used to say it was unneeded (the
whole shared index buffer draws with no per-face lookup at all), which
was true until frustum culling needed exactly that lookup to skip ranges.
X9-K replaces the single shared atlas lump with four v9 lumps in the SAME
relative position (filesystem/mpak_layout.h's own v9 note has the full
format account) - textureDir/textureData/batches/faceRemap, still Xbox-
target only, still walked in the same order the header declares their
length fields. Verified independently against a real baked archive
(crossfire.mpak, nv2a profile) by hand in Python before writing this - the
final cursor landed exactly on the real file size and the computed
offsets read back real, sane vertex/index data - see
docs/r3d/divergences.md's X9-B/X9-D entries. Deliberately simpler than the
real Mpak_ComputeLayout(): no fixed-record-size divisibility checks, no
overflow-hardened lightmap cross-validation - a corrupted archive fails a
bounds check here and this function returns false, not a crash, but the
diagnostics are coarser than the real implementation's. If mpak_layout.c's
own section order or record sizes ever change, this must change with it -
there is no compiler-enforced link between the two copies. */
typedef struct
{
	mpak_lump_t vertices;
	mpak_lump_t indices;
	mpak_lump_t faceDrawCmds; /* X9-F: captured for frustum culling - see
	                              nv2a_map_face_t's own declaration comment. */
	mpak_lump_t textureDir;   /* X9-K: mpak_texture_info_t[header->textureCount] */
	mpak_lump_t textureData;  /* X9-K: per-texture DXT1 blobs in directory order; v10: each a full mip chain, padded to 128 bytes */
	mpak_lump_t batches;      /* X9-K: mpak_texture_batch_t[header->batchCount] */
	mpak_lump_t faceRemap;    /* X9-K: uint32_t[original BSP face count], originalToSortedFaceIndex[] */
	mpak_lump_t lightmapAtlas;
	mpak_lump_t faceInfo;     /* X9-K: mpak_face_info_t[nv2a_map_face_count], SORTED order - only
	                              ::flags (MPAK_SURF_DRAWSKY) is read; this lump exists in every archive
	                              back to v6 but was never captured by this simplified layout mirror
	                              until the sky-skip fix needed it. */
} nv2a_mpak_geometry_t;

static qboolean nv2a_mpak_place( fs_offset_t fileSize, fs_offset_t *cursor, uint64_t length, mpak_lump_t *out )
{
	uint64_t padded;

	out->offset = *cursor;
	out->length = 0;

	if( length == 0 )
		return true;

	padded = (( length + ( MPAK_SECTOR_SIZE - 1 )) / MPAK_SECTOR_SIZE ) * MPAK_SECTOR_SIZE;
	if( *cursor < 0 || padded > (uint64_t)( fileSize - *cursor ))
		return false;

	out->length = (fs_offset_t)length;
	*cursor += (fs_offset_t)padded;
	return true;
}

static qboolean nv2a_compute_mpak_layout( const mpak_header_t *header, fs_offset_t fileSize, nv2a_mpak_geometry_t *out )
{
	fs_offset_t cursor = MPAK_SECTOR_SIZE; /* the header itself always occupies one full sector */
	mpak_lump_t skip;

	memset( out, 0, sizeof( *out ));

	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->vCount * sizeof( mpak_vertex_t ), &out->vertices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->iCount * sizeof( uint16_t ), &out->indices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenVisibility, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenNodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLeaves, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenMarksurfaces, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceDrawCmds, &out->faceDrawCmds )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenPlanes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenClipnodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenModels, &skip )) return false;

	if( !( header->flags & MPAK_TARGET_XBOX_FLAG ))
		return false; /* PC-target archive - no per-texture/batch lumps at all, nothing this slice can draw */

	/* X9-K: the per-texture DXT1 directory, its concatenated data blob, the
	   texture-draw batch table, and the original->sorted face-remap table -
	   in this exact order, the same order their length fields appear in
	   the header (filesystem/mpak_layout.h's own v9 note), replacing the
	   single lenAtlas placement this function used through X9-J. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenTextureDir, &out->textureDir )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenTextureData, &out->textureData )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenBatches, &out->batches )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceRemap, &out->faceRemap )) return false;

	/* X9-C: continue the walk far enough to reach lightmapAtlas - assetDict
	   and assetData are always empty under X2's own narrowed scope
	   (MpakHeader.h's own note) but still real cursor steps to skip if a
	   stale pre-narrowing archive ever declares them non-empty. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenAssetDict, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenAssetData, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenEntities, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLightmapAtlas, &out->lightmapAtlas )) return false;

	/* X9-K sky-skip fix: lenFaceInfo is the very next header field after
	   lenLightmapAtlas (mpak_layout.h's own field order) - one more step is
	   all that's needed to reach it; nothing after it (lenTexinfo onward)
	   is read by this module, so the walk still stops here. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceInfo, &out->faceInfo )) return false;

	return true;
}

/* tools/x4-triangle/main.c's own init_shader(), unmodified in shape - the
   vp20 program upload loop plus the fp20 pixel-shader block, both
   pre-compiled headers copied from that same directory (ref/nv2a/vs.inl,
   ref/nv2a/ps.inl - see those files for the cgc/vp20compiler/fp20compiler
   provenance). */
static void nv2a_init_shader( void )
{
	uint32_t *p;
	int i;

	uint32_t vs_program[] =
	{
		#include "vs.inl"
	};

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_START, 0 );
	p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
		| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0 );
	pb_end( p );

	for( i = 0; i < (int)( sizeof( vs_program ) / 16 ); i++ )
	{
		p = pb_begin();
		pb_push( p++, NV097_SET_TRANSFORM_PROGRAM, 4 );
		memcpy( p, &vs_program[i * 4], 4 * 4 );
		p += 4;
		pb_end( p );
	}

	p = pb_begin();
	#include "ps.inl"
	pb_end( p );
}

/* X9-C: the diffuse x lightmap x 2 program pair (ref/nv2a/vs_lit.vs.cg,
   ps_lit.ps.cg - both real, freshly-authored Cg sources compiled through
   the same cgc/vp20compiler/fp20compiler pipeline vs.inl/ps.inl already
   were, verified reproducible before being committed). The NV2A only
   holds one active vertex/pixel program at a time, so whichever draw path
   runs this frame re-uploads its OWN program right before drawing rather
   than both programs staying resident together - simple and correct for
   a bring-up slice with only two draw paths, at the cost of a small,
   irrelevant-for-correctness re-upload every frame. Otherwise identical
   in shape to nv2a_init_shader() above. */
static void nv2a_init_lit_shader( void )
{
	uint32_t *p;
	int i;

	uint32_t vs_program[] =
	{
		#include "vs_lit.inl"
	};

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_START, 0 );
	p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
		| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0 );
	pb_end( p );

	for( i = 0; i < (int)( sizeof( vs_program ) / 16 ); i++ )
	{
		p = pb_begin();
		pb_push( p++, NV097_SET_TRANSFORM_PROGRAM, 4 );
		memcpy( p, &vs_program[i * 4], 4 * 4 );
		p += 4;
		pb_end( p );
	}

	p = pb_begin();
	#include "ps_lit.inl"
	pb_end( p );
}

/* tools/x4-triangle/main.c's own init_texture(), LINEAR PATH ONLY - no
   swizzled copy, since that path is X4's own open, unresolved defect (see
   this file's header comment - and note that X4's linear path was never a
   correct baseline either: fed 0..1 UVs, a LINEAR texture only ever shows
   its texels (0..1, 0..1), which is why nv2a_test_verts above carries
   texel-unit UVs). Returns false (leaving nv2a_tex NULL)
   on allocation failure - a 64 MiB unified-memory console can genuinely
   run out of contiguous pages, and this must fail R_Init cleanly rather
   than memcpy into a null pointer (found in codex review). */
static qboolean nv2a_init_texture( void )
{
	uint32_t linear[NV2A_TEST_TEX_SIZE * NV2A_TEST_TEX_SIZE];
	int x, y;

	for( y = 0; y < NV2A_TEST_TEX_SIZE; y++ )
	{
		for( x = 0; x < NV2A_TEST_TEX_SIZE; x++ )
		{
			int checker = ( x ^ y ) & 1;
			/* A8R8G8B8: opaque white / opaque HEV orange */
			linear[y * NV2A_TEST_TEX_SIZE + x] = checker ? 0xFFFFFFFFu : 0xFFFF8000u;
		}
	}

	nv2a_tex = MmAllocateContiguousMemoryEx( sizeof( linear ), 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_tex )
		return false;

	memcpy( nv2a_tex, linear, sizeof( linear ));
	return true;
}

static void nv2a_set_attrib_pointer( unsigned int index, unsigned int format, unsigned int size, unsigned int stride, const void *data )
{
	uint32_t *p = pb_begin();
	p = pb_push1( p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format )
		| MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size )
		| MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride ));
	p = pb_push1( p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4, (uint32_t)data & 0x03ffffff );
	pb_end( p );
}

static void nv2a_draw_test_triangle( void )
{
	uint32_t *p;
	uint32_t tex_phys;
	int i;

	/* X9-C: the NV2A only holds one resident vertex/pixel program at a
	   time, and nv2a_draw_map_geometry() may have uploaded its own
	   two-texcoord/two-texture program since this path last ran - re-load
	   this path's own single-texture program every time it draws, matching
	   the same on-demand-upload discipline the lit path uses. */
	nv2a_init_shader();

	/* Texture stage 0: 8x8 A8R8G8B8, clamped, bilinear, LINEAR unswizzled
	   (LU_IMAGE_A8R8G8B8) - see tools/x4-triangle/main.c's own citations
	   for where each register value below comes from. */
	p = pb_begin();
	tex_phys = (uint32_t)nv2a_tex & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR ) /* see nv2a_bind_map_texture_stage0 */
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, ( NV2A_TEST_TEX_SIZE * 4 ) << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( NV2A_TEST_TEX_SIZE << 16 ) | NV2A_TEST_TEX_SIZE );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	/* MIN=NEAREST(1)/MAG=NEAREST(1), point-sampled, deliberately NOT the
	   mesh sample's 0x04074000 (a convolution filter; the map stages
	   use plain bilinear 0x02022000 since X9-L, divergences.md #102 -
	   this filter choice is local to the bring-up canary only). With
	   texel-unit UVs (above) and EITHER the convolution-kernel enum or a
	   plain-bilinear one (0x02020000), this triangle rendered as a smooth
	   orange/white blend, not a checkerboard - live-tested both ways.
	   Booted with NEAREST instead, it renders a crisp 8x8 checkerboard
	   (`.sweep/triangle_nearest.png`): the blend was bilinear/tent
	   filtering doing exactly what it does to a single-texel-period
	   pattern (every sample point between texel centers interpolates two
	   or four alternating colours), not a remaining UV bug, and a
	   point-sampled checkerboard is a far more sensitive regression
	   indicator for the texel-unit-addressing convention than a blended
	   one - any squares merging or a return to a two-texel gradient means
	   this convention broke again. */
	/* Real NV2A rejects a zero convolution-kernel field (bits 13-15)
	   with an invalid-data halt; xemu does not. */
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, 0x01012000 );
	pb_end( p );

	/* X9-I: this path shares the GPU with nv2a_draw_map_geometry(), which
	   conditionally enables NV097_SET_ALPHA_TEST_ENABLE (see
	   nv2a_r_noalphatest's own declaration comment) - a per-draw-call
	   GPU register, not per-function state, so R_EndFrame's fallback to
	   THIS function on a not-yet-ready/mid-transition frame (its own
	   comment names this a real, exercised case, not a hypothetical)
	   would otherwise inherit whatever alpha-test state the previous
	   frame's map draw left behind. Explicitly disabled here so this
	   path never depends on the other one's state, the same "read fresh
	   every call" discipline nv2a_draw_map_geometry() already applies to
	   itself. Harmless today either way (this checkerboard's own two
	   texel values, 0xFFFFFFFF and 0xFFFF8000, both line 846, are fully
	   opaque - alpha=0xFF either way, passes any GEQUAL-128 test
	   regardless), but correctness here shouldn't depend on that
	   coincidence. */
	p = pb_begin();
	p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
	pb_end( p );

	/* Shader constants: the same fixed viewport transform every frame. */
	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 16 );
	memcpy( p, nv2a_viewport, 16 * 4 );
	p += 16;
	pb_end( p );

	p = pb_begin();
	pb_push( p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
	for( i = 0; i < 16; i++ )
		*( p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	pb_end( p );

	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_test_vertex_t ), &nv2a_vb[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_test_vertex_t ), &nv2a_vb[3] );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
	p = pb_push1( p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK( NV097_DRAW_ARRAYS_COUNT, ( ARRAYSIZE( nv2a_test_verts ) - 1 ))
		| MASK( NV097_DRAW_ARRAYS_START_INDEX, 0 ));
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
	pb_end( p );

	pb_print( "ref_nv2a bring-up\n" );
	pb_draw_text_screen();
}

/* X9-F: port of ref/gl/gl_frustum.c's own GL_FrustumCullBox - the same
   signbits switch, literally, minus the clipFlags early-out (there is no
   BSP node hierarchy here to propagate a shrinking clip mask down through,
   so every face is tested against all 5 planes; see docs/r3d/fork-plan.md's
   X9-F entry for why that is the right scope for this slice). Returns true
   if the box is entirely outside at least one plane. */
static qboolean nv2a_frustum_cull_box( const vec3_t mins, const vec3_t maxs )
{
	int i;

	for( i = 0; i < 5; i++ )
	{
		const mplane_t *p = &nv2a_frustum[i];

		switch( p->signbits )
		{
		case 0: if( p->normal[0] * maxs[0] + p->normal[1] * maxs[1] + p->normal[2] * maxs[2] < p->dist ) return true; break;
		case 1: if( p->normal[0] * mins[0] + p->normal[1] * maxs[1] + p->normal[2] * maxs[2] < p->dist ) return true; break;
		case 2: if( p->normal[0] * maxs[0] + p->normal[1] * mins[1] + p->normal[2] * maxs[2] < p->dist ) return true; break;
		case 3: if( p->normal[0] * mins[0] + p->normal[1] * mins[1] + p->normal[2] * maxs[2] < p->dist ) return true; break;
		case 4: if( p->normal[0] * maxs[0] + p->normal[1] * maxs[1] + p->normal[2] * mins[2] < p->dist ) return true; break;
		case 5: if( p->normal[0] * mins[0] + p->normal[1] * maxs[1] + p->normal[2] * mins[2] < p->dist ) return true; break;
		case 6: if( p->normal[0] * maxs[0] + p->normal[1] * mins[1] + p->normal[2] * mins[2] < p->dist ) return true; break;
		case 7: if( p->normal[0] * mins[0] + p->normal[1] * mins[1] + p->normal[2] * mins[2] < p->dist ) return true; break;
		default: return false;
		}
	}

	return false;
}

/* The existing X9-D batching loop, unchanged in shape, factored out so
   X9-F's own run-merging pass (nv2a_draw_map_geometry() below) can call
   it once per contiguous visible run instead of once for the whole
   buffer - state setup (textures/attributes/constants) stays completely
   outside this function and outside the loop that calls it. */
static void nv2a_draw_index_range( uint32_t begin, uint32_t end )
{
	uint32_t i = begin;

	while( i < end )
	{
		uint32_t *p;
		uint32_t batch = end - i;
		if( batch > 120 )
			batch = 120;

		p = pb_begin();
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
		pb_push( p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, batch );
		memcpy( p, &nv2a_map_index_dwords[i], batch * sizeof( uint32_t ));
		p += batch;
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
		pb_end( p );

		i += batch;
	}
}

/* X9-M: draw the EXACT logical index range [begin, end) - no 6-index
   outward snap. Pairs of 16-bit indices go through the same packed
   NV097_ARRAY_ELEMENT16 path nv2a_draw_index_range() uses (it is
   NV20_TCL_PRIMITIVE_3D_INDEX_DATA, 0x1800); an odd leading or trailing
   index goes alone through NV097_ARRAY_ELEMENT32 (0x1808, nv_regs.h:518),
   which takes one full 32-bit index. `begin` and `end` must be
   whole-triangle boundaries (every face's own range is), and each
   BEGIN/END block carries a multiple of 3 indices so no triangle is split
   across blocks. Used by the brush-entity pass, where a snapped range could
   pull a world triangle along with a moving entity (or leave a static copy
   of an entity triangle in the world pass). */
static void nv2a_draw_exact_indices( uint32_t begin, uint32_t end )
{
	const uint16_t *idx16 = (const uint16_t *)nv2a_map_index_dwords;

	while( begin < end )
	{
		uint32_t *p;
		uint32_t n = end - begin, k, pairs;

		if( n > 234 ) /* 234 = 3 * 78: whole triangles; worst case (odd begin, odd end) is
		                 2 + 2 + 1 + 116 + 2 + 2 = 125 dwords, under pbkit's 128-dword block */
			n = 234;

		p = pb_begin();
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
		k = begin;
		if( k & 1 )
		{
			p = pb_push1( p, NV097_ARRAY_ELEMENT32, idx16[k] );
			k++;
		}
		pairs = ( begin + n - k ) / 2;
		if( pairs > 0 )
		{
			pb_push( p++, 0x40000000 | NV097_ARRAY_ELEMENT16, pairs );
			memcpy( p, &nv2a_map_index_dwords[k / 2], pairs * sizeof( uint32_t ));
			p += pairs;
			k += pairs * 2;
		}
		if( k < begin + n )
			p = pb_push1( p, NV097_ARRAY_ELEMENT32, idx16[k] );
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
		pb_end( p );

		begin += n;
	}
}

/* X9-K: snaps a whole BATCH's own [startIndex, startIndex+indexCount)
   LOGICAL index range outward to a dword-aligned, whole-triangle-aligned
   range, for direct submission through nv2a_draw_index_range() - the
   exact same 6-logical-index/3-dword snap nv2a_map_face_t's own
   declaration comment already establishes for a SINGLE face (6 = lcm(2,3):
   simultaneously a dword boundary, since each dword packs two indices,
   AND a whole-triangle boundary), applied here to a whole batch's own
   combined range instead. Used only when no per-face culling table exists
   for this map (or it doesn't cover this batch), or nv2a_r_nocull is set -
   the per-face path in nv2a_draw_map_geometry() uses nv2a_map_faces' own
   already-snapped dword_begin/dword_end per face instead.

   Codex review, real finding, now fixed at the source: through the first
   X9-K cut, neither this function's own outward snap NOR the per-face
   path's snap actually stayed inside this batch's own range - each
   batch's [startIndex, startIndex+indexCount) came straight from
   BspParser.cpp's face-sort/emit pass with no alignment guarantee at
   all, so an unaligned boundary let either snap round 1-2 triangles into
   the NEXT batch and draw them with the wrong (that batch's) bound
   texture, a visible seam at nearly every texture-batch boundary on a
   multi-textured map. The comment that used to stand here claimed the
   per-face path was "exact rather than merely an upper bound" - true of
   the snap arithmetic in isolation, false of what it actually stayed
   inside once each batch bound its own texture (X9-K). Fixed on the
   BAKER side, not here: tools/asset-studio/src/formats/BspParser.cpp now
   pads every batch's own contributed index run to an exact multiple of 6
   the moment that batch is known to be complete (absorbed into its own
   last real face's FaceDrawCmd.index_count, so the FaceDrawCmds table
   stays an exact, gapless partition of the index buffer - see that
   file's own comment at the padding site). Batches are contiguous by
   construction, so padding every batch's own end also aligns the very
   next batch's own start - which is what makes BOTH this function's
   outward snap and the per-face path's per-face snap exact again,
   never spilling past a batch boundary, for the same underlying reason
   this comment used to (incorrectly) assume for free. */
static void nv2a_batch_dword_range( uint32_t start_index, uint32_t index_count, uint32_t dword_count, uint32_t *out_begin, uint32_t *out_end )
{
	*out_begin = ( start_index / 6 ) * 3;
	*out_end   = (( start_index + index_count + 5 ) / 6 ) * 3;
	if( *out_end > dword_count )
		*out_end = dword_count;
}

/* X9-K codex review, real finding: this used to be inlined at the top of
   nv2a_draw_map_geometry()'s per-batch loop and pushed unconditionally,
   once per batch, BEFORE either draw branch below had checked whether
   that batch has anything surviving frustum+PVS culling to draw - on
   c1a0.mpak/crossfire.mpak (129/91 batches respectively) that is a
   guaranteed ~100+ texture-stage reprogram every frame regardless of
   camera position, independent of what X9-F/G/H's own culling work
   already established was actually visible. Split out here so both draw
   branches can call it lazily, exactly once, only once they know a real
   draw call is about to happen - see their own call sites below.

   Texture stage 0: this batch's OWN per-texture DXT1 image, natively
   hardware-WRAP-addressed - see this file's own header comment and
   nv2a_load_map_geometry's own top-of-function comment for the full "why
   per-texture instead of one shared atlas" account. Same COLOR/CONTROL0
   shape the old shared atlas used (X9-B through X9-J, that block's own
   citations for where each register value came from still apply) -
   OFFSET/BASE_SIZE_U/V are per-batch instead of per-map, and ADDRESS is
   WRAP (0x00010101) instead of CLAMP (0x00030303): the whole point of
   going per-texture is hardware tiling in silicon, no shader-side wrap
   and no atlas-rect remap needed. Stage 1 (the lightmap, bound once per
   frame, outside the batch loop) stays CLAMP - completely unaffected,
   out of scope for this change. CONTROL1 pitch / IMAGE_RECT are kept for
   the SAME reason the old shared-atlas path always kept them (ignored
   for a compressed format on the sources checked so far, but
   abaire/nxdk_pgraph_tests pushes them for every format including DXT1
   and its goldens are hardware captures, so this stays the
   tested/expected-harmless shape - divergences.md #100). FILTER is the
   one register nv2a_r_texturenearest actually changes - see that
   convar's own declaration comment for why this is stage-0-only,
   matching ref/gl's own gl_texture_nearest vs gl_lightmap_nearest
   precedent. */
static void nv2a_bind_map_texture_stage0( const nv2a_map_texture_t *tex )
{
	uint32_t *p;
	uint32_t tex_phys;
	uint32_t tex_width = 1u << tex->log2Width;
	uint32_t tex_pitch = tex_width * 2; /* DXT1: ceil(width/4)*8 == width*2 for a POT width >= 4 (nv2a_load_map_geometry's own texture-directory validation already guarantees this) */

	/* BORDER_SOURCE must be COLOR on every texture stage: left at 0
	   (BORDER_SOURCE_TEXTURE) every texture sampled at (u/2, v/2), on
	   xemu and real hardware alike - consistent with the GPU addressing
	   a surface with room for a texture border (the mechanism is
	   inferred; the halving itself was measured).
	   Measured by poking constant UVs into the live vertex buffer: a
	   (u, v) always came back as frac(u/2), frac(v/2). Perfect Dark X's
	   gfx_nv2a.cpp format word (0x0001122a) sets the same bit. */
	p = pb_begin();
	tex_phys = ( (uint32_t)nv2a_map_texture_data + (uint32_t)tex->physOffset ) & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, tex->mipLevels )
		| MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, tex->log2Width )
		| MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, tex->log2Height ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, tex_pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( tex_width << 16 ) | ( 1u << tex->log2Height ));
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00010101 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	/* X9-N: MIN=6 (TENT_TENT_LOD: bilinear within the two nearest mip
	   levels, blended - trilinear, GL_LINEAR_MIPMAP_LINEAR), MAG=2
	   (TENT, bilinear): 0x02062000. The nearest option is MIN=3
	   (BOX_NEARESTLOD, GL_NEAREST_MIPMAP_NEAREST), MAG=1: 0x01032000.
	   Both keep the non-zero convolution-kernel field (bits 13-15) real
	   hardware requires. The nxdk mesh sample's 0x04074000 is
	   MIN/MAG=CONVOLUTION_2D_LOD0 - xemu samples it as bilinear, real
	   hardware filters with the convolution kernel and smears (Perfect
	   Dark X's gfx_nv2a.cpp documents the same). */
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, nv2a_r_texturenearest.value ? 0x01032000 : 0x02062000 );
	pb_end( p );
}

/*
========================================================================
X9-M: brush entities (func_door, func_wall, func_illusionary, ladders...)

Every face the baker writes comes from LUMP_FACES, which holds the world
model's faces AND every submodel's (the `*N` brush entities). Before X9-M
this renderer drew all of them as static world geometry: with PVS on,
submodel faces were never marked visible (no world leaf lists them - the
PVS walk is not where they come from), so doors, ladders, fences and
chargers vanished; with PVS off they drew frozen at their compile-time
position, triggers included. This is the entity half ref/gl has and this
file did not: R_AddEntity/R_ClearScene keep the frame's brush-entity list
(gl_rmain.c:217,234), and nv2a_draw_brush_entities() walks it after the
world, solid list first, then the translucent list sorted like
R_TransEntityCompare (gl_rmain.c:86) - R_DrawEntitiesOnList's order
(gl_rmain.c). Each entity draws with its own transform
(R_RotateForEntity/R_TranslateForEntity, gl_rmain.c:448,474) and render
mode (R_SetRenderMode, gl_rsurf.c:1717). Studio models, sprites and beams
are accepted by R_AddEntity but not drawn yet - this renderer has no
studio or sprite path.
========================================================================
*/
#define NV2A_MAX_BRUSH_ENTITIES 512 /* per list, per frame; a HL map sends far fewer brush entities */
#define NV2A_SURF_DRAWTURB BIT( 4 ) /* common/bspfile.h:55's SURF_DRAWTURB - same "mirror the one engine
                                       msurface flag this file reads" pattern as NV2A_SURF_PLANEBACK */

#define NV2A_UNLIT_PAGE_SIZE 16 /* texels per side; pitch 64 bytes, LU_IMAGE's own alignment */

static cl_entity_t *nv2a_solid_brush_entities[NV2A_MAX_BRUSH_ENTITIES];
static cl_entity_t *nv2a_trans_brush_entities[NV2A_MAX_BRUSH_ENTITIES];
static int          nv2a_num_solid_brush_entities;
static int          nv2a_num_trans_brush_entities;
static vec3_t       nv2a_eye_forward; /* GL_RenderFrame's own AngleVectors forward, for CL_FxBlend's hologram fade */
static uint32_t    *nv2a_unlit_page;  /* 16x16 texels of 0x80 grey: ps_lit's lightmap x 2 then
                                         multiplies by 1.0, i.e. an unlit draw. Allocated once,
                                         never freed - 1 KiB, reused across maps. */

/* ref/gl's R_HasLightmap (gl_rsurf.c:1086) for one entity. */
static qboolean nv2a_entity_has_lightmap( const model_t *world, const cl_entity_t *e )
{
	if( r_fullbright->value || !world->lightdata )
		return false;

	if( e->curstate.effects & EF_FULLBRIGHT )
		return false;	// disabled by user

	// check for rendermode
	switch( e->curstate.rendermode )
	{
	case kRenderTransTexture:
		return FBitSet( e->model->flags, MODEL_LIQUID ) ? true : false;

	case kRenderTransColor:
	case kRenderTransAdd:
	case kRenderGlow:
		return false; // no lightmaps
	}

	return true;
}

/* Point texture stage 1 at the real lightmap page, or at the neutral page
   for an unlit entity. Only the offset/pitch/rect change; format, address
   mode and filter stay as nv2a_draw_map_geometry() set them. Returns false
   (nothing pushed) if the neutral page cannot be allocated. */
static qboolean nv2a_bind_lightmap_stage1( qboolean lit )
{
	uint32_t *p;
	uint32_t phys, pitch, w, h;

	if( lit )
	{
		phys  = (uint32_t)nv2a_map_lightmap & 0x03ffffff;
		pitch = nv2a_map_lightmap_pitch;
		w     = nv2a_map_lightmap_width;
		h     = nv2a_map_lightmap_height;
	}
	else
	{
		if( !nv2a_unlit_page )
		{
			int k;
			nv2a_unlit_page = MmAllocateContiguousMemoryEx( NV2A_UNLIT_PAGE_SIZE * NV2A_UNLIT_PAGE_SIZE * 4, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
			if( !nv2a_unlit_page )
				return false;
			for( k = 0; k < NV2A_UNLIT_PAGE_SIZE * NV2A_UNLIT_PAGE_SIZE; k++ )
				nv2a_unlit_page[k] = 0x80808080u;
		}
		phys  = (uint32_t)nv2a_unlit_page & 0x03ffffff;
		pitch = NV2A_UNLIT_PAGE_SIZE * 4;
		w = h = NV2A_UNLIT_PAGE_SIZE;
	}

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET + 0x40, phys );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1 + 0x40, pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT + 0x40, ( w << 16 ) | h );
	pb_end( p );
	return true;
}

/* True when sorted face `sorted` belongs to the WORLD model rather than a
   brush entity: its original BSP index falls inside the world model's
   own [firstmodelsurface, +nummodelsurfaces) range (mod_bmodel.c:2138,
   Mod_SetupSubmodels). No remap table (or no world) - fail open: treat it
   as world, so the world pass keeps drawing it, as it did before X9-M. */
static qboolean nv2a_map_face_is_world( const model_t *world, uint32_t sorted )
{
	uint32_t orig;

	if( !world || !nv2a_map_face_orig || sorted >= nv2a_map_face_count )
		return true;

	orig = nv2a_map_face_orig[sorted];
	return orig >= (uint32_t)world->firstmodelsurface
		&& orig < (uint32_t)world->firstmodelsurface + (uint32_t)world->nummodelsurfaces;
}

/* ref/gl's R_GetEntityRenderMode (gl_studio.c:2384), brush-model branch
   only - the only model type this list ever holds. A normal-rendermode
   brush model with '{' textures (MODEL_TRANSPARENT) sorts as TransAlpha. */
static int nv2a_brush_entity_render_mode( const cl_entity_t *ent )
{
	if( ent->curstate.rendermode == kRenderNormal && ent->model && FBitSet( ent->model->flags, MODEL_TRANSPARENT ))
		return kRenderTransAlpha;
	return ent->curstate.rendermode;
}

/* ref/gl's R_OpaqueEntity (gl_rmain.c:63). */
static qboolean nv2a_opaque_entity( const cl_entity_t *ent )
{
	if( nv2a_brush_entity_render_mode( ent ) == kRenderNormal )
	{
		switch( ent->curstate.renderfx )
		{
		case kRenderFxNone:
		case kRenderFxDeadPlayer:
		case kRenderFxLightMultiplier:
		case kRenderFxExplode:
			return true;
		}
	}
	return false;
}

/* ref/gl's CL_FxBlend (gl_rmain.c:1161), whole function. This renderer
   never draws cubemaps, so the RF_DRAW_CUBEMAP guards are always open. */
static int nv2a_fx_blend( cl_entity_t *e )
{
	int blend = 0;
	float offset = ((int)e->index ) * 363.0f; // Use ent index to de-sync these fx

	switch( e->curstate.renderfx )
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 2 + offset );
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 8 + offset );
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 2 + offset );
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 8 + offset );
		break;
	case kRenderFxFadeSlow:
		if( e->curstate.renderamt > 0 )
			e->curstate.renderamt -= 1;
		else e->curstate.renderamt = 0;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		if( e->curstate.renderamt > 3 )
			e->curstate.renderamt -= 4;
		else e->curstate.renderamt = 0;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		if( e->curstate.renderamt < 255 )
			e->curstate.renderamt += 1;
		else e->curstate.renderamt = 255;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		if( e->curstate.renderamt < 252 )
			e->curstate.renderamt += 4;
		else e->curstate.renderamt = 255;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = 20 * sin( gp_cl->time * 4 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = 20 * sin( gp_cl->time * 16 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = 20 * sin( gp_cl->time * 36 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = 20 * (sin( gp_cl->time * 2 ) + sin( gp_cl->time * 17 + offset ));
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = 20 * (sin( gp_cl->time * 16 ) + sin( gp_cl->time * 23 + offset ));
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
	{
		vec3_t tmp;
		float dist;

		VectorSubtract( e->origin, nv2a_eye_origin, tmp );
		dist = DotProduct( tmp, nv2a_eye_forward );

		// turn off distance fade
		if( e->curstate.renderfx == kRenderFxDistort )
			dist = 1;

		if( dist <= 0 )
		{
			blend = 0;
		}
		else
		{
			e->curstate.renderamt = 180;
			if( dist <= 100 ) blend = e->curstate.renderamt;
			else blend = (int) ((1.0f - ( dist - 100 ) * ( 1.0f / 400.0f )) * e->curstate.renderamt );
			blend += gEngfuncs.COM_RandomLong( -32, 31 );
		}
		break;
	}
	default:
		blend = e->curstate.renderamt;
		break;
	}

	blend = bound( 0, blend, 255 );

	return blend;
}

/* ref/gl's R_RankForRenderMode (gl_rmain.c:28). */
static int nv2a_rank_for_render_mode( int rendermode )
{
	switch( rendermode )
	{
	case kRenderTransTexture:
		return 1;	// draw second
	case kRenderTransAdd:
		return 2;	// draw third
	case kRenderGlow:
		return 3;	// must be last!
	}
	return 0;
}

/* ref/gl's R_TransEntityCompare (gl_rmain.c:86), brush entities only. */
static int nv2a_trans_entity_compare( const void *a, const void *b )
{
	cl_entity_t *ent1 = *(cl_entity_t **)a;
	cl_entity_t *ent2 = *(cl_entity_t **)b;
	int rendermode1 = nv2a_brush_entity_render_mode( ent1 );
	int rendermode2 = nv2a_brush_entity_render_mode( ent2 );
	vec3_t vecLen, org;
	float dist1, dist2;

	// sort by distance
	if( rendermode1 != kRenderTransAlpha )
	{
		VectorAverage( ent1->model->mins, ent1->model->maxs, org );
		VectorAdd( ent1->origin, org, org );
		VectorSubtract( nv2a_eye_origin, org, vecLen );
		dist1 = DotProduct( vecLen, vecLen );
	}
	else dist1 = 1000000000;

	if( rendermode2 != kRenderTransAlpha )
	{
		VectorAverage( ent2->model->mins, ent2->model->maxs, org );
		VectorAdd( ent2->origin, org, org );
		VectorSubtract( nv2a_eye_origin, org, vecLen );
		dist2 = DotProduct( vecLen, vecLen );
	}
	else dist2 = 1000000000;

	if( dist1 > dist2 )
		return -1;
	if( dist1 < dist2 )
		return 1;

	// then sort by rendermode
	if( nv2a_rank_for_render_mode( rendermode1 ) > nv2a_rank_for_render_mode( rendermode2 ))
		return 1;
	if( nv2a_rank_for_render_mode( rendermode1 ) < nv2a_rank_for_render_mode( rendermode2 ))
		return -1;

	return 0;
}

/* ref/gl's R_SetRenderMode (gl_rsurf.c:1717) on NV2A state. ref/gl
   modulates the texture by a vertex colour whose alpha is tr.blend; this
   combiner (ps_lit) has no colour input, so the blend amount goes in as
   NV097_SET_BLEND_COLOR's alpha and the blend factors use CONSTANT_ALPHA
   in place of SRC_ALPHA. That is exact for opaque texels; for a
   '{'-masked texture drawn in a translucent mode it ignores the texel's
   own alpha, which ref/gl would multiply in. Returns false for
   kRenderTransColor: ref/gl draws that as a flat colour with texturing
   off, and ps_lit has no untextured path - not drawn (divergences.md
   #103). A future port: its alpha is curstate.renderamt, not tr.blend
   (gl_rsurf.c:1724-1730). */
static qboolean nv2a_set_render_mode( int rendermode, float blend )
{
	uint32_t *p = pb_begin();
	uint32_t alpha = (uint32_t)( bound( 0.0f, blend, 1.0f ) * 255.0f + 0.5f );

	switch( rendermode )
	{
	case kRenderNormal:
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 ); /* R_DrawWorld/R_DrawBrushModel leave it off (gl_rsurf.c:3691,1931) */
		break;
	case kRenderTransColor:
		pb_end( p );
		return false;
	case kRenderTransAdd:
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_ALPHA );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE );
		p = pb_push1( p, NV097_SET_BLEND_COLOR, alpha << 24 );
		p = pb_push1( p, NV097_SET_DEPTH_MASK, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
		break;
	case kRenderTransAlpha:
		/* gl_rsurf.c:1742-1751, non-quake branch: alpha test, no blend,
		   GL_GREATER 0.25. */
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER );
		p = pb_push1( p, NV097_SET_ALPHA_REF, 0x3F ); /* GL_GREATER 0.25 passes a/255 > 0.25, i.e. a >= 64 */
		break;
	default: /* kRenderTransTexture, kRenderGlow */
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_ALPHA );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_CONSTANT_ALPHA );
		p = pb_push1( p, NV097_SET_BLEND_COLOR, alpha << 24 );
		p = pb_push1( p, NV097_SET_DEPTH_MASK, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
		break;
	}

	pb_end( p );
	return true;
}

/* Undo nv2a_set_render_mode(), back to the world pass's own state -
   ref/gl's own reset at the end of R_DrawBrushModel (gl_rsurf.c). */
static void nv2a_reset_render_mode( void )
{
	uint32_t *p = pb_begin();

	p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
	p = pb_push1( p, NV097_SET_DEPTH_MASK, 1 );
	if( nv2a_r_noalphatest.value )
	{
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
	}
	else
	{
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GEQUAL );
		p = pb_push1( p, NV097_SET_ALPHA_REF, 0x80 );
	}
	pb_end( p );
}

/* ref/gl's R_DrawBrushModel (gl_rsurf.c:1822) for one entity: bounds and
   frustum cull, object transform, per-face backface cull in model space
   (R_SortBrushModelSurfaces, gl_rsurf.c:1764), render mode, draw.
   Not ported: dynamic-light pushes, decals, fullbrights, detail
   textures, fog, polygon offset (gl_polyoffset_bmodels) and the
   kRenderTransTexture back-to-front face sort - none of which this
   renderer has for the world either yet. */
static void nv2a_draw_brush_entity( const model_t *world, cl_entity_t *e, float blend )
{
	const model_t *clmodel = e->model;
	vec3_t mins, maxs, modelorg, delta;
	vec3_t forward, right, up;
	float object[4][4], mvp[4][4];
	qboolean rotated;
	uint32_t *p;
	uint32_t bound_texture = 0xFFFFFFFFu;
	qboolean lit;
	int i;

	/* Only this map's own submodels share its baked faces; anything else
	   (a brush model from another BSP) has nothing to draw here. */
	if( !clmodel || clmodel->type != mod_brush || clmodel == world || clmodel->surfaces != world->surfaces )
		return;

	if( !VectorIsNull( e->angles ))
	{
		for( i = 0; i < 3; i++ )
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
		rotated = true;
	}
	else
	{
		VectorAdd( e->origin, clmodel->mins, mins );
		VectorAdd( e->origin, clmodel->maxs, maxs );
		rotated = false;
	}

	if( nv2a_frustum_cull_box( mins, maxs ))
		return;

	/* Object matrix in this file's row-vector convention (position *
	   object * viewproj): rows are the model's X/Y/Z axes in world space
	   and its origin - the transpose of Matrix4x4_CreateFromEntity's
	   columns, whose X/Y/Z axes are AngleVectors' forward, -right and up. */
	memset( object, 0, sizeof( object ));
	if( rotated )
	{
		AngleVectors( e->angles, forward, right, up );
		for( i = 0; i < 3; i++ )
		{
			object[0][i] = forward[i];
			object[1][i] = -right[i];
			object[2][i] = up[i];
		}
	}
	else
	{
		object[0][0] = object[1][1] = object[2][2] = 1.0f;
	}
	object[3][0] = e->origin[0];
	object[3][1] = e->origin[1];
	object[3][2] = e->origin[2];
	object[3][3] = 1.0f;
	nv2a_matrix_multiply( mvp, object, nv2a_map_viewproj );

	/* tr.modelorg: the eye in model space (gl_rsurf.c R_DrawBrushModel -
	   Matrix4x4_VectorITransform when rotated, a plain subtract when not). */
	VectorSubtract( nv2a_eye_origin, e->origin, delta );
	if( rotated )
	{
		modelorg[0] = DotProduct( delta, forward );
		modelorg[1] = -DotProduct( delta, right );
		modelorg[2] = DotProduct( delta, up );
	}
	else
	{
		VectorCopy( delta, modelorg );
	}

	/* R_HasLightmap: glass, additive and glow entities draw unlit
	   (R_DrawVBO( R_HasLightmap(), ...) / R_BlendLightmaps, gl_rsurf.c). */
	lit = nv2a_entity_has_lightmap( world, e );
	if( !lit && !nv2a_bind_lightmap_stage1( false ))
		lit = true; /* no neutral page: draw lit rather than not at all */

	if( !nv2a_set_render_mode( e->curstate.rendermode, blend ))
	{
		if( !lit )
			nv2a_bind_lightmap_stage1( true );
		return;
	}

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 20 );
	memcpy( p, mvp, 16 * 4 );
	p += 16;
	{
		static const float vs_lit_c4[4] = { 0.0f, 1.0f, 0.0f, 0.0f }; /* see nv2a_draw_map_geometry */
		memcpy( p, vs_lit_c4, sizeof( vs_lit_c4 ));
		p += 4;
	}
	pb_end( p );

	for( i = 0; i < clmodel->nummodelsurfaces; i++ )
	{
		uint32_t orig = (uint32_t)( clmodel->firstmodelsurface + i );
		const msurface_t *psurf;
		const nv2a_map_face_t *face;
		uint32_t sorted;

		if( orig >= (uint32_t)world->numsurfaces || orig >= nv2a_map_face_remap_count )
			continue;
		sorted = nv2a_map_face_remap[orig];
		if( sorted >= nv2a_map_face_count )
			continue;

		face = &nv2a_map_faces[sorted];
		psurf = &world->surfaces[orig];

		if( face->sky )
			continue;

		/* R_SortBrushModelSurfaces: water sides (non-quake branch). */
		if( FBitSet( psurf->flags, NV2A_SURF_DRAWTURB ))
		{
			if( psurf->plane->type != PLANE_Z && !FBitSet( e->curstate.effects, EF_WATERSIDES ))
				continue;
			if( mins[2] + 1.0f >= psurf->plane->dist )
				continue;
		}

		/* R_CullSurface's CULL_BACKSIDE, against the model-space eye.
		   ref/gl keeps a back face only for decals on TransTexture
		   entities; this renderer has no decals. */
		if( !nv2a_r_nocullback.value && !VectorIsNull( psurf->plane->normal ))
		{
			float dist = PlaneDiff( modelorg, psurf->plane );

			if( FBitSet( psurf->flags, NV2A_SURF_PLANEBACK ))
				dist = -dist;
			if( dist <= BACKFACE_EPSILON )
				continue;
		}

		if( face->texture >= nv2a_map_texture_count )
			continue;
		if( face->texture != bound_texture )
		{
			nv2a_bind_map_texture_stage0( &nv2a_map_textures[face->texture] );
			bound_texture = face->texture;
		}
		nv2a_draw_exact_indices( face->index_begin, face->index_end );
	}

	nv2a_reset_render_mode();
	if( !lit )
		nv2a_bind_lightmap_stage1( true );
}

/* ref/gl's R_DrawEntitiesOnList (gl_rmain.c), brush-model cases only:
   solid entities after the world, then translucent ones sorted by
   R_TransEntityCompare, each with tr.blend = CL_FxBlend/255 unless its
   rendermode is kRenderNormal ("draw as solid but sorted by distance").
   Restores the world view-projection afterwards. */
static void nv2a_draw_brush_entities( const model_t *world )
{
	uint32_t *p;
	int i;

	if( !world || !nv2a_map_faces || !nv2a_map_face_remap || !nv2a_map_face_orig )
		return;

	if( nv2a_num_solid_brush_entities == 0 && nv2a_num_trans_brush_entities == 0 )
		return;

	for( i = 0; i < nv2a_num_solid_brush_entities; i++ )
		nv2a_draw_brush_entity( world, nv2a_solid_brush_entities[i], 1.0f );

	qsort( nv2a_trans_brush_entities, nv2a_num_trans_brush_entities, sizeof( cl_entity_t * ), nv2a_trans_entity_compare );

	for( i = 0; i < nv2a_num_trans_brush_entities; i++ )
	{
		cl_entity_t *e = nv2a_trans_brush_entities[i];
		float blend;

		if( e->curstate.rendermode != kRenderNormal )
			blend = nv2a_fx_blend( e ) / 255.0f;
		else blend = 1.0f; // draw as solid but sorted by distance

		if( blend <= 0.0f )
			continue;

		nv2a_draw_brush_entity( world, e, blend );
	}

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 16 );
	memcpy( p, nv2a_map_viewproj, 16 * 4 );
	p += 16;
	pb_end( p );
}

/* X9-B: the real geometry path - same VP20 transform shader and the same
   two-texture-stage fp20 combiner X9-C already proved (no shader or
   combiner changes this slice), real per-map baked geometry, and an
   indexed draw over real baked vertices instead of X4's own plain
   sequential NV097_DRAW_ARRAYS. Batches indices in groups of <=120
   (deps/nxdk/samples/mesh/main.c's own NV20_TCL_PRIMITIVE_3D_INDEX_DATA
   limit) - written as the real, general batching loop from the start
   rather than a one-batch shortcut, which is what let X9-D scale this
   same function to a whole map's own dword count with no changes here at
   all. X9-F adds frustum culling ahead of the batching loop itself:
   nv2a_map_faces (built once at load) is walked in bake order, contiguous
   VISIBLE faces are merged into a single run, and each run is handed to
   nv2a_draw_index_range() above unchanged.

   X9-K: through X9-J, texture stage 0 was bound ONCE per frame (the
   single shared atlas every face used) and the cull/run-merge pass below
   walked the WHOLE map in one sweep. There is no more shared atlas - each
   texture-draw batch (nv2a_map_batches, built at load from the archive's
   own MpakTextureBatch table) uses its OWN independent per-texture DXT1
   image, so stage 0 must now be (re)bound once PER BATCH (via
   nv2a_bind_map_texture_stage0(), only once that batch is known to have
   at least one surviving draw call - see this function's own two draw
   branches below for the codex-review fix that made the bind
   conditional instead of unconditional per batch), and the
   cull/run-merge pass runs once per batch too, scoped to that batch's own
   [startFace, startFace+faceCount) sorted-order range - same algorithm,
   same nv2a_map_faces table, just bounded per-batch instead of over the
   whole map. Texture stage 1 (the lightmap) is completely unaffected -
   still bound exactly ONCE per frame, before this loop, exactly as it
   always was: the lightmap atlas is explicitly out of scope for this
   whole change (see this file's header comment and
   docs/r3d/fork-plan.md's X9-K entry). */
static void nv2a_draw_map_geometry( void )
{
	uint32_t *p;
	uint32_t tex_phys;
	uint32_t i, b;
	/* X9-H: the engine's own already-loaded world model, read directly
	   for its real per-surface plane/flags (backface test below) - the
	   SAME model_t X9-G's nv2a_mark_leaves() already trusts, guarded the
	   same way (world == nv2a_map_owner, never dereferenced otherwise).
	   NULL here just means "no backface test this call, draw whatever
	   frustum+PVS already allowed" - fail open, same discipline as every
	   other optional cull test in this function. */
	const model_t *world = gp_cl->models[1];

	if( world != nv2a_map_owner )
		world = NULL;

	/* This path's own two-texcoord/two-texture program, re-uploaded every
	   time it draws for the same reason nv2a_draw_test_triangle() now
	   re-uploads its own - see that function's own comment. */
	nv2a_init_lit_shader();

	/* X9-C: texture stage 1, the real RGBA8 lightmap - bound ONCE per
	   frame, here, before the per-batch loop below: unaffected by X9-K's
	   own per-texture-batch change (the lightmap atlas is explicitly out
	   of scope for that whole change - this file's header comment), so it
	   keeps its original "set up once, draw everything" shape while stage
	   0 (below) moves inside the loop. The SAME uncompressed-linear path
	   X9-A's own checkerboard used on stage 0 (not "proven" by it - see
	   nv2a_map_lightmap's declaration comment), just at stage 1's own
	   register offset (+0x40 per stage, tools/
	   x4-triangle/main.c's own documented per-stage addressing), and a
	   DIFFERENT color format (codex review, TWO rounds, real finding both
	   times - the first fix was itself wrong): the baker writes each
	   texel bytewise as R,G,B,A in memory (tools/asset-studio/src/
	   formats/BspParser.cpp's writeBlock(), `page.rgba[destOffset+0..3] =
	   R,G,B,A`). X9-A's own checkerboard uses LU_IMAGE_A8R8G8B8 correctly
	   because it writes native uint32_t INTEGER literals (0xFFFFFFFFu
	   etc.) matching that format's own packed-integer convention
	   (verified against the checkerboard's own "opaque HEV orange" value,
	   0xFFFF8000 -> A=FF,R=FF,G=80,B=00, a real orange), not raw bytes.
	   For a raw LITTLE-ENDIAN byte buffer, the format name has to match
	   the INTEGER that byte order forms when read as a little-endian
	   word, not the byte order itself directly: [R,G,B,A] in memory reads
	   as the word 0xAABBGGRR (byte0=R is the LOW byte), which is
	   LU_IMAGE_A8B8G8R8's own name (A=MSB..R=LSB), NOT LU_IMAGE_R8G8B8A8
	   (round 1's own fix - wrong, corrected in round 2: that name would
	   match memory order [A,B,G,R], not this buffer's real [R,G,B,A]). */
	p = pb_begin();
	tex_phys = (uint32_t)nv2a_map_lightmap & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET + 0x40, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT + 0x40,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR ) /* see nv2a_bind_map_texture_stage0 */
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1 + 0x40, nv2a_map_lightmap_pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT + 0x40, ( nv2a_map_lightmap_width << 16 ) | nv2a_map_lightmap_height );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS + 0x40, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0 + 0x40, 0x4003ffc0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER + 0x40, 0x02022000 );
	/* Defensive, not a behaviour change: pbkit's set_draw_buffer()
	   (pbkit.c, via pb_target_back_buffer every frame) already pushes
	   0x00110001 here - W-buffering, perspective-correct texture
	   interpolation and stencil write. Restating the exact same value
	   keeps the map path correct even if that pbkit call moves, the way
	   Perfect Dark X's gfx_nv2a.cpp nv2a_control0() does. */
	p = pb_push1( p, NV097_SET_CONTROL0,
		NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE | NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE
		| NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE );
	pb_end( p );

	/* X9-I: native alpha test (nv2a_r_noalphatest's own declaration
	   comment above has the full account of why this is safe to apply
	   globally, not just to masked faces, and why it still ships
	   disabled pending a live A/B). Pushed every call, not just once,
	   so toggling the convar mid-session actually takes effect instead
	   of latching whatever state a previous frame left - same reason
	   nv2a_r_novis/nv2a_r_nocullback are read fresh per face rather than
	   applied once at init. GEQUAL against 0x80 matches TextureOpt.cpp's
	   own compressToDXT encode-time boundary (`a < 128` forces the
	   transparent selector), so a texel this test lets through is
	   exactly one the baker itself decided was real content. */
	p = pb_begin();
	if( nv2a_r_noalphatest.value )
	{
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
	}
	else
	{
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GEQUAL );
		p = pb_push1( p, NV097_SET_ALPHA_REF, 0x80 );
	}
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 20 );
	memcpy( p, nv2a_map_viewproj, 16 * 4 );
	p += 16;
	{
		/* c[4] = (0, 1, 0, 0): vs_lit.inl's own `#const c[4] = 0 1`,
		   the literal it writes into each texcoord's .zw (q = 1). */
		static const float vs_lit_c4[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
		memcpy( p, vs_lit_c4, sizeof( vs_lit_c4 ));
		p += 4;
	}
	pb_end( p );

	p = pb_begin();
	pb_push( p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
	for( i = 0; i < 16; i++ )
		*( p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	pb_end( p );

	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[3] );
	/* Lightmap UV, attribute slot 10 - vs_lit.vs.cg's own TEXCOORD1,
	   confirmed by a real vp20compiler run (not assumed) to map there. */
	nv2a_set_attrib_pointer( 10, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[5] );

	nv2a_cull_faces_total = nv2a_map_face_count;
	nv2a_cull_faces_visible = 0;
	nv2a_cull_runs = 0;
	nv2a_cull_dwords = 0;
	nv2a_cull_faces_backface_only = 0;

	/* X9-K: one texture bind + one cull/run-merge pass PER BATCH, in
	   directory order - see this function's own top-of-function comment
	   for why a per-texture batch makes this restructuring necessary
	   (there is no more single shared atlas to bind once and cull over in
	   one sweep). The diagnostics above are frame-level totals,
	   accumulated ACROSS every batch, matching how they already summed
	   across the whole map before X9-K. */
	for( b = 0; b < nv2a_map_batch_count; b++ )
	{
		const nv2a_map_batch_t *batch = &nv2a_map_batches[b];
		const nv2a_map_texture_t *tex = &nv2a_map_textures[batch->textureIndex]; /* textureIndex validated < nv2a_map_texture_count at load */

		if( nv2a_map_faces == NULL || nv2a_r_nocull.value
			|| (uint64_t)batch->startFace + batch->faceCount > (uint64_t)nv2a_map_face_count )
		{
			/* No validated per-face table for this map, culling disabled
			   by nv2a_r_nocull, or (defensive - nv2a_load_map_geometry
			   deliberately does NOT cross-check startFace/faceCount
			   against a real face-count total at load time, see its own
			   batch-parsing comment) this batch's own range does not fit
			   inside the per-face table - submit this WHOLE BATCH as one
			   run via its own startIndex/indexCount, exactly what this
			   function did for the WHOLE MAP before X9-F/X9-K. No real
			   culling happens on this path, so `end > begin` is expected
			   to hold for every real batch - the check exists only so a
			   degenerate (zero-length) batch, which nv2a_load_map_geometry
			   does not forbid outright, costs no texture bind either,
			   matching this function's own "bind only once a real draw
			   call is about to happen" discipline (codex review, real
			   finding: this bind used to be unconditional, once per
			   batch, regardless of whether anything in it was visible -
			   129/91 guaranteed texture-stage reprograms per frame on
			   c1a0.mpak/crossfire.mpak, independent of camera position). */
			uint32_t begin, end;

			nv2a_batch_dword_range( batch->startIndex, batch->indexCount, nv2a_map_dword_count, &begin, &end );
			if( end > begin )
			{
				nv2a_bind_map_texture_stage0( tex );
				nv2a_draw_index_range( begin, end );
			}
			nv2a_cull_faces_visible += batch->faceCount;
			if( end > begin )
				nv2a_cull_runs++;
			nv2a_cull_dwords += end - begin;
		}
		else
		{
			/* X9-F/X9-G/X9-H's own single streaming run-merging pass,
			   UNCHANGED algorithm: accumulate a run of contiguous visible
			   faces' own dword ranges, flush it through
			   nv2a_draw_index_range() the moment a culled face (or the
			   end of this BATCH's own range) breaks the run - just
			   bounded to [batch->startFace, batch->startFace +
			   batch->faceCount) instead of the whole map (see this
			   function's own top-of-function comment for why a per-batch
			   texture bind makes this restructuring necessary). No run
			   array - the run is just two locals, same as before.
			   `stage0_bound` is new (codex review, real finding - see the
			   other draw branch's own comment above for the measured
			   cost): the texture bind now happens lazily, the first time
			   a run actually opens for this batch, so a batch with zero
			   surviving faces costs no texture-stage reprogram at all -
			   never per-run (only the bind is per-batch-at-most-once; the
			   run-merge/flush logic itself is completely unchanged). */
			uint32_t run_begin = 0, run_end = 0;
			qboolean have_run = false;
			qboolean stage0_bound = false;
			uint32_t end_face = batch->startFace + batch->faceCount;

			for( i = batch->startFace; i < end_face; i++ )
			{
				const nv2a_map_face_t *face = &nv2a_map_faces[i];
				/* X9-G: a face is drawn only if it survives BOTH cull tests -
				   frustum (X9-F, unchanged) and PVS (nv2a_map_face_visible,
				   this slice). nv2a_map_face_visible == NULL (no table for
				   this map) and nv2a_r_novis.value (the A/B toggle) both fall
				   through to "not PVS-culled", matching nv2a_r_nocull's own
				   already-established meaning for the frustum test. */
				qboolean pvs_culled = nv2a_map_face_visible != NULL
					&& !nv2a_r_novis.value
					&& !nv2a_map_face_visible[i];

				/* X9-K: a sky face is never drawn, regardless of frustum/PVS
				   visibility - see nv2a_map_face_t::sky's own declaration
				   comment for why (no real skybox exists in this file; this
				   is the minimum "don't draw it wrong" fix, not a skybox
				   implementation). Checked first - it's a single struct
				   field, cheaper than the frustum box test. */
				if( face->sky )
					continue;

				/* X9-M: brush-entity faces (func_door, func_wall, ladders,
				   triggers...) belong to the entity pass, which draws them
				   at the entity's own origin/angles and render mode - see
				   nv2a_draw_brush_entities(). ref/gl never draws them from
				   the world either: R_RecursiveWorldNode draws each world
				   node's own surface range (gl_rsurf.c:3507-3527; leaves
				   only mark visibility), and world nodes cover only the
				   world model's faces. Without the remap table
				   there is no way to tell, so the world pass keeps drawing
				   everything (fail open, never missing geometry). */
				if( !nv2a_map_face_is_world( world, i ))
					continue;

				/* X9-H, nv2a-xbox-specialist review, real finding: the
				   backface test (below) is the most expensive of the three -
				   the only one with a pointer chase off this map's own
				   arrays, into a separate, deduplicated plane pool - so it
				   must not run on a face the cheaper frustum/PVS tests
				   already reject. Short-circuit here, before paying for the
				   plane fetch at all. */
				if( nv2a_frustum_cull_box( face->mins, face->maxs ) || pvs_culled )
					continue;

				/* X9-K: `i` here is a SORTED-order position (this batch's own
				   index into nv2a_map_faces), NOT the ORIGINAL BSP face index
				   world->surfaces itself is ordered by - the exact same
				   order-mismatch nv2a_mark_leaves() translates through
				   nv2a_map_face_remap, on the read side instead of the write
				   side (see nv2a_map_face_orig's own declaration comment for
				   the full account). nv2a_map_face_orig == NULL (no table for
				   this map) fails open to "no backface test", same as
				   world == NULL already does - never a wrong-surface read.
				   The translated origIdx is bounds-checked against
				   world->numsurfaces exactly like the untranslated index
				   always was (X9-H's own "never trust a raw index past the
				   engine's own real numsurfaces" discipline, unchanged). */
				if( world != NULL && !nv2a_r_nocullback.value && nv2a_map_face_orig != NULL )
				{
					uint32_t origIdx = nv2a_map_face_orig[i];

					if( origIdx < (uint32_t)world->numsurfaces )
					{
						const msurface_t *surf = &world->surfaces[origIdx];

						if( !VectorIsNull( surf->plane->normal ))
						{
							float dist = PlaneDiff( nv2a_eye_origin, surf->plane );

							if( FBitSet( surf->flags, NV2A_SURF_PLANEBACK ))
								dist = -dist;

							if( dist <= BACKFACE_EPSILON )
							{
								nv2a_cull_faces_backface_only++;
								continue;
							}
						}
					}
				}

				nv2a_cull_faces_visible++;

				/* X9-M: runs are EXACT index ranges now (they were dword
				   ranges snapped out to 6 indices) - a snapped range could
				   reach a neighbouring brush-entity face's triangles, which
				   must never be drawn from the world. */
				if( have_run && face->index_begin == run_end )
				{
					run_end = face->index_end;
				}
				else
				{
					if( have_run )
					{
						nv2a_draw_exact_indices( run_begin, run_end );
						nv2a_cull_runs++;
						nv2a_cull_dwords += ( run_end - run_begin + 1 ) / 2;
					}
					if( !stage0_bound )
					{
						/* First surviving face of this batch (or the very
						   first face, if have_run was never true before) -
						   bind exactly once, strictly before the FIRST
						   nv2a_draw_index_range() call this batch will
						   ever make (that call only happens later, once
						   this run - or a later one - gets flushed), never
						   per-run. */
						nv2a_bind_map_texture_stage0( tex );
						stage0_bound = true;
					}
					run_begin = face->index_begin;
					run_end = face->index_end;
					have_run = true;
				}
			}

			if( have_run )
			{
				nv2a_draw_exact_indices( run_begin, run_end );
				nv2a_cull_runs++;
				nv2a_cull_dwords += ( run_end - run_begin + 1 ) / 2;
			}
		}
	}

	nv2a_draw_brush_entities( world );

	pb_print( "ref_nv2a: real baked geometry\n" );
	pb_draw_text_screen();
}

/*
========================================================================
ref_interface_t
========================================================================
*/

static void R_SimpleStub( void )
{
	;
}

static void R_SimpleStubInt( int unused )
{
	;
}

static void R_SimpleStubUInt( unsigned int unused )
{
	;
}

static void R_SimpleStubBool( qboolean unused )
{
	;
}

static qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw )
{
	return false;
}

static void R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw )
{
	;
}

static void R_GetDetailScaleForTexture( int texture, float *xScale, float *yScale )
{
	if( xScale ) *xScale = 1.0f;
	if( yScale ) *yScale = 1.0f;
}

static void R_SetDetailScaleForTexture( int texture, float xScale, float yScale )
{
	;
}

static void R_FillRenderAPI( render_api_t *api )
{
	;
}

static void R_FillTriAPI( triangleapi_t *api )
{
	;
}

/* Bring-up diagnostic only - not part of the ref_interface_t contract.
   Read directly out of guest memory (mem <addr> 4) via the module's own
   isolated symbol name (_nv2a_debug_marker / _nv2a_debug_pb_status in the
   XBE's own .map) to see exactly how far R_Init got without needing a
   live pad or the cursor/mon/regs debug commands this project's own
   tooling has repeatedly found unreliable. Values: 1 = entered R_Init,
   2 = R_Init_Video returned false, 3 = R_Init_Video returned true,
   4 = pb_init failed (debug_pb_status holds its status code), 6 = texture
   allocation failed, 7 = vertex buffer allocation failed, 5 = fully
   initialised. */
uint32_t debug_marker;
int32_t  debug_pb_status;
/* Increments once per R_EndFrame call that actually reaches the real
   draw path (nv2a_ready true) - the concrete answer to "is this module's
   own per-frame code running at all, repeatedly, in real time" that a
   single memory snapshot of debug_marker/nv2a_ready cannot give: those
   two only prove R_Init finished successfully once, not that the engine
   ever calls back into this module's own frame loop afterward. Read the
   same way as debug_marker (mem <addr> 4), twice with a real delay
   between reads - a live boot with the engine actually driving this
   module every frame shows this incrementing; a boot that never gets
   back here (stuck elsewhere before the first frame, or stuck in this
   same frame) shows it static. */
uint32_t debug_frame_count;

static qboolean pb_initialized;

/* One cleanup path for every failure branch below, and for normal
   shutdown - matching ref/soft's own unconditional R_Free_Video discipline
   (r_main.c:1351-1398) rather than gating it on how far init got (a real
   gap codex review found: the first version skipped R_Free_Video entirely
   on an R_Init_Video failure, and skipped it in R_Shutdown unless nv2a_ready
   was already true). pb_kill() is still gated on pb_initialized specifically,
   since pbkit's own contract has no documented "safe to call without a
   matching pb_init" guarantee. */
static void nv2a_free_resources( void )
{
	if( nv2a_vb )  { MmFreeContiguousMemory( nv2a_vb );  nv2a_vb  = NULL; }
	if( nv2a_tex ) { MmFreeContiguousMemory( nv2a_tex ); nv2a_tex = NULL; }
}

/* X9-B's own map-geometry resources - separate from the above because they
   come and go with map loads/unloads (Mod_ProcessRenderData), not with
   R_Init/R_Shutdown, though R_Shutdown must still free them if a map is
   loaded when the renderer itself is torn down. */
static void nv2a_free_map_resources( void )
{
	/* X9-M: the entity lists point into the client's edict array, which a
	   map change frees (CL_FreeEdicts) while loading-plaque frames can
	   still reach R_EndFrame - never carry them past the map they came
	   from. */
	nv2a_num_solid_brush_entities = 0;
	nv2a_num_trans_brush_entities = 0;
	nv2a_map_ready = false;
	nv2a_map_owner = NULL;
	nv2a_map_vert_count = 0;
	nv2a_map_dword_count = 0;
	nv2a_map_face_count = 0;
	nv2a_map_texture_count = 0;
	nv2a_map_batch_count = 0;
	nv2a_map_face_remap_count = 0;
	if( nv2a_map_vb )            { MmFreeContiguousMemory( nv2a_map_vb );            nv2a_map_vb            = NULL; }
	if( nv2a_map_index_dwords )  { Mem_Free( nv2a_map_index_dwords );                nv2a_map_index_dwords  = NULL; }
	if( nv2a_map_faces )         { Mem_Free( nv2a_map_faces );                       nv2a_map_faces         = NULL; }
	if( nv2a_map_face_visible )  { Mem_Free( nv2a_map_face_visible );                nv2a_map_face_visible  = NULL; }
	if( nv2a_map_texture_data )  { MmFreeContiguousMemory( nv2a_map_texture_data );  nv2a_map_texture_data  = NULL; }
	if( nv2a_map_face_remap )    { Mem_Free( nv2a_map_face_remap );                  nv2a_map_face_remap    = NULL; }
	if( nv2a_map_face_orig )     { Mem_Free( nv2a_map_face_orig );                   nv2a_map_face_orig     = NULL; }
	if( nv2a_map_lightmap )      { MmFreeContiguousMemory( nv2a_map_lightmap );      nv2a_map_lightmap      = NULL; }

	/* X9-G: both point into the outgoing world model's own leafs[] array
	   (freed by the engine as part of this same unload) - clearing them
	   here, on every unload, is what keeps nv2a_mark_leaves() from ever
	   comparing against or dereferencing a dangling pointer left over
	   from a previous map. */
	nv2a_viewleaf = NULL;
	nv2a_oldviewleaf = NULL;

	/* codex review round 2, real finding, high confidence: this was
	   missing. The engine keeps the world model in FIXED mod_known[0]
	   storage across level changes (engine/common/model.c) - a real map
	   transition can hand back the exact same model_t* pointer value a
	   previous, different map already used, not just a coincidentally-
	   similar one. Without this reset, nv2a_mark_leaves()'s own
	   `nv2a_pvs_parity_checked_for != world` guard would wrongly read as
	   "already checked" for every map AFTER the first one loaded this
	   session, silently skipping the one check that exists specifically
	   to catch a baker/loader face-count mismatch - exactly the failure
	   the check above this comment was added to fail open against. */
	nv2a_pvs_parity_checked_for = NULL;
}

static qboolean R_Init( void )
{
	qboolean video_ok;

	debug_marker = 1;
	nv2a_view_valid = false; /* X9-E: no real GL_RenderFrame call yet this init */

	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_nocull );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_novis );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_lockpvs );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_nocullback );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_noalphatest );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_texturenearest );

	/* Codex review, X9-D: nv2a_map_index_dwords (CPU-only, see its own
	   declaration comment) belongs in ordinary engine-pooled memory, not
	   contiguous GPU-visible memory - r_temppool is the same real,
	   engine-backed allocator ref/soft and ref/gl already use for their
	   own CPU-side data (ref/soft/r_main.c, ref/gl/gl_opengl.c). */
	r_temppool = Mem_AllocPool( "ref_nv2a zone" );

	video_ok = gEngfuncs.R_Init_Video( REF_SOFTWARE );
	debug_marker = video_ok ? 3 : 2;

	if( !video_ok )
	{
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	/* A nonzero pb_init() is NOT reliably a clean no-op on this exact
	   nxdk version (codex review, real finding, deliberately not
	   "fixed" here): pb_init's own internal failure paths
	   (deps/nxdk/lib/pbkit/pbkit.c ~2297-2366) can return -2/-3 after
	   allocating some but not all of its own contiguous buffers without
	   freeing them, and -5 after installing the GPU interrupt handler
	   and writing GPU registers, all without calling pb_kill() itself.
	   Calling pb_kill() here regardless would be wrong (codex's own
	   point): pbkit has no documented contract for "safe to tear down
	   after a specific negative return", so guessing at extra cleanup
	   risks tearing down state pbkit itself never finished setting up.
	   The correct fix is in nxdk (pb_init made atomic on every failure
	   path), tracked as a real, deferred limitation - see
	   docs/r3d/divergences.md #88 - not attempted in this vendored
	   dependency for a bring-up milestone. */
	if(( debug_pb_status = pb_init()) != 0 )
	{
		debug_marker = 4;
		gEngfuncs.Con_Printf( "ref_nv2a: pb_init failed (%d)\n", debug_pb_status );
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}
	pb_initialized = true;

	pb_show_front_screen();

	nv2a_width  = pb_back_buffer_width();
	nv2a_height = pb_back_buffer_height();

	nv2a_init_shader();

	if( !nv2a_init_texture() )
	{
		debug_marker = 6;
		pb_kill();
		pb_initialized = false;
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	nv2a_vb = MmAllocateContiguousMemoryEx( sizeof( nv2a_test_verts ), 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_vb )
	{
		/* pb_kill() before freeing nv2a_tex, matching R_Shutdown's own
		   ordering - no frame has been submitted yet at this point in
		   R_Init, so there is no in-flight GPU reference to race here,
		   but keeping one consistent teardown order avoids relying on
		   that reasoning holding at every call site forever. */
		debug_marker = 7;
		pb_kill();
		pb_initialized = false;
		nv2a_free_resources();
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}
	memcpy( nv2a_vb, nv2a_test_verts, sizeof( nv2a_test_verts ));

	nv2a_matrix_viewport( nv2a_viewport, 0, 0, (float)nv2a_width, (float)nv2a_height, 0, 65536.0f );

	debug_marker = 5;
	nv2a_ready = true;
	return true;
}

static void R_Shutdown( void )
{
	nv2a_ready = false;

	/* pb_kill() FIRST: it starts the pushbuffer queue and waits for the
	   GPU to go idle before returning (deps/nxdk/lib/pbkit/pbkit.c's own
	   teardown), and R_EndFrame's own pb_finished() swap is asynchronous -
	   freeing the vertex/texture pages first could let a still-in-flight
	   frame have the NV2A fetch from memory already handed back (found in
	   codex review). Only free them once the GPU is provably idle. */
	if( pb_initialized )
	{
		pb_kill();
		pb_initialized = false;
	}

	nv2a_free_resources();
	nv2a_free_map_resources();
	Mem_FreePool( &r_temppool );

	gEngfuncs.R_Free_Video();
}

static const char *R_GetConfigName( void )
{
	return "ref_nv2a";
}

static qboolean R_SetDisplayTransform( ref_screen_rotation_t rotate, int x, int y, float scale_x, float scale_y )
{
	return true;
}

static void GL_SetupAttributes( int safegl )
{
	;
}

/* X9-M: ref/gl's R_AddEntity (gl_rmain.c:234). Only brush models are
   kept - this renderer draws nothing else yet - but the accept/reject
   rules (r_drawentities, EF_NODRAW, fully faded translucents) and the
   solid/translucent split are the reference's, so the lists hold exactly
   what ref/gl's would for brush entities. */
static qboolean R_AddEntity( struct cl_entity_s *clent, int type )
{
	if( !r_drawentities->value )
		return false; // not allow to drawing

	if( FBitSet( clent->curstate.effects, EF_NODRAW ))
		return false; // done

	if( clent->curstate.rendermode != kRenderNormal && nv2a_fx_blend( clent ) <= 0 )
		return true; // invisible

	if( type == ET_BEAM || !clent->model || clent->model->type != mod_brush )
		return true; // accepted, but this renderer has no path for it yet

	if( nv2a_opaque_entity( clent ))
	{
		if( nv2a_num_solid_brush_entities >= NV2A_MAX_BRUSH_ENTITIES )
			return false;
		nv2a_solid_brush_entities[nv2a_num_solid_brush_entities++] = clent;
	}
	else
	{
		if( nv2a_num_trans_brush_entities >= NV2A_MAX_BRUSH_ENTITIES )
			return false;
		nv2a_trans_brush_entities[nv2a_num_trans_brush_entities++] = clent;
	}

	return true;
}

/* X9-M: ref/gl's R_ClearScene (gl_rmain.c:217). */
static void R_ClearScene( void )
{
	nv2a_num_solid_brush_entities = 0;
	nv2a_num_trans_brush_entities = 0;

	// clear the scene befor start new frame
	if( gEngfuncs.drawFuncs->R_ClearScene != NULL )
		gEngfuncs.drawFuncs->R_ClearScene();
}

static void R_ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	/* X9-M: CL_FreeEdicts (cl_game.c) calls this with allocate = false
	   right before freeing the edict array the entity lists point into;
	   R_ClearScene does not run again until the next active frame. */
	if( !allocate )
	{
		nv2a_num_solid_brush_entities = 0;
		nv2a_num_trans_brush_entities = 0;
	}
}

static const byte *R_GetTextureOriginalBuffer( unsigned int idx )
{
	return NULL;
}

static int GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	return 0;
}

static void GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	;
}

static void R_SetupSky( int *skytextures )
{
	;
}

static void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	;
}

static void FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	;
}

static int WorldToScreen( const vec3_t world, vec3_t screen )
{
	VectorClear( screen );
	return 0;
}

static qboolean VID_ScreenShot( const char *filename, int shot_type )
{
	return false;
}

static qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	return false;
}

static void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	;
}

static int R_CreateDecalList( struct decallist_s *pList )
{
	return 0;
}

static float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	return 0.0f;
}

static void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	;
}

static void R_SetSkyCloudsTextures( int solidskyTexture, int alphaskyTexture )
{
	;
}

static void GL_SubdivideSurface( model_t *mod, msurface_t *fa )
{
	;
}

/* Cheap, side-effect-free pre-check: does this buffer even look like a
   real, nv2a-profile MPAK archive? Used by Mod_ProcessRenderData BEFORE
   touching any already-loaded map state, since that callback fires for
   EVERY model (studio, sprite, every brush submodel), not just the world
   (codex review, real finding) - a later, unrelated model's own buffer
   must fail this and leave existing map geometry alone, not silently
   free it first and find out afterward that there was nothing to
   replace it with. */
static qboolean nv2a_mpak_header_valid( const byte *buffer, size_t buffersize, const mpak_header_t **out_header )
{
	const mpak_header_t *header;

	if( buffersize < MPAK_DESCRIPTOR_OFFSET + sizeof( mpak_header_t ))
		return false;

	header = (const mpak_header_t *)( buffer + MPAK_DESCRIPTOR_OFFSET );

	if( memcmp( header->magic, MPAK_LAYOUT_MAGIC, sizeof( header->magic )) != 0 )
		return false;
	if( header->version != MPAK_LAYOUT_VERSION || header->headerSize != sizeof( mpak_header_t ))
		return false;
	if( !( header->flags & MPAK_FLAG_NV2A_BLOCK ))
		return false; /* a real archive, just not baked with the profile this slice reads */

	*out_header = header;
	return true;
}

/* Reads the WHOLE map's real geometry (every baked vertex, the whole
   shared index buffer, every unique per-texture DXT1 image, the whole
   lightmap atlas) out of a real, nv2a-profile-baked .mpak buffer and
   prepares it to be drawn in place of the X9-A test triangle: a real,
   per-map-sized remapped vertex buffer (position + native diffuse UV +
   lightmap UV), a real per-map-sized packed index-dword buffer, every
   texture's own DXT1 bytes copied verbatim into ONE contiguous buffer
   (already in the right hardware layout - no un-swizzle needed, see this
   file's own header comment and docs/r3d/divergences.md's X9-B entry),
   the real lightmap atlas copied verbatim, and the texture-directory/
   batch tables that tell nv2a_draw_map_geometry() which contiguous face/
   index range uses which texture. The camera is NOT set up here as of
   X9-E - see GL_RenderFrame's own real, live, per-frame camera and
   nv2a_view_valid's own declaration comment for why. Caller
   (Mod_ProcessRenderData) has already confirmed the header itself is a
   real nv2a-profile archive via nv2a_mpak_header_valid() - this function
   only has to validate everything PAST the header.

   X9-K: there is no more per-vertex atlas-rect remap. Through X9-J every
   texture shared one 2048x2048 atlas and `atlas_uv = rect.xy +
   frac(raw_uv) * rect.zw` was a real, disclosed approximation for a
   tiling face (frac() collapses every repeat to the same fractional
   value, so a face using more than one texture repeat sampled a flat,
   non-repeating patch instead of tiling) - unacceptable on ordinary
   tiling floors/walls, and subdividing tiled faces at bake time to fix it
   overflows the u16 vertex-index limit on large outdoor maps. Each source
   texture is now independently resampled to power-of-two and addressed
   with NV2A's own native hardware WRAP mode (nv2a_draw_map_geometry's own
   per-batch ADDRESS push), which tiles for free at the pixel level, no
   shader-side wrap and no per-vertex remap needed - v.u/v.v are used
   AS-IS below. This is exact, not an approximation: BspParser.cpp's own
   UV computation (`rawU/rawV / loc.width/height`, loc.width/height being
   the texture's NATIVE pre-resample pixel size, PakManager.cpp:20-21)
   already normalizes by the texture's real native size, independent of
   any later resample - the identical reasoning ref/gl's own
   GL_ResampleTexture + normalized-UV combination already relies on
   (reference-renderer-specialist review: specifically ref/gl's
   GL_ARB_TEXTURE_NPOT_EXT-unsupported fallback path, gl_image.c:467 -
   ref/gl's default/common path never resamples world textures at all,
   since real hardware supports NPOT; this NV2A port hits the fallback
   case unconditionally because NV2A itself has no NPOT support to fall
   back FROM).
   Returns false (leaving the caller to keep drawing the X9-A triangle)
   for anything past the header that doesn't parse as expected, or that
   would exceed NV2A_MAP_BUDGET_BYTES - never crashes on a truncated,
   malformed, or just very large archive. */
/* A coarse guard against a malformed or just implausibly large archive
   attempting a doomed giant allocation - NOT a real accounting of the
   Xbox's own 64MiB total budget (codex review, real finding: the
   engine's own static state, pbkit's front/back/depth buffers, every
   other subsystem's own allocations, and allocator fragmentation are
   all real, all already resident by the time this function runs, and
   NONE of them are visible to this narrowly-scoped ref module - a
   proper admission-control budget belongs at BAKE time, in
   tools/asset-studio, where the real total per-map cost is known before
   a single byte reaches the console, not guessed at here from four
   numbers this one function happens to compute). The buffer this
   function is CALLED with is already real and already resident though
   (Mod_ProcessRenderData's own contract - engine/common/model.c's
   Mod_LoadModel FS_LoadFile's the whole archive before calling in), so
   it belongs in the sum this function CAN check. Cross-checked in
   Python against the real crossfire.mpak (docs/r3d/divergences.md #91):
   19,480 verts * 28B + 34,326 indices/2*4B + a real 2048x2048 DXT1 atlas
   + a real 1024x1024 RGBA8 lightmap page + the archive's own 10.5MB on
   disk totals ~16.6MB - comfortably under this cap, with real margin
   left to catch a corrupt or absurdly oversized archive before it can
   hurt a 64MiB console, without pretending this one function can see
   the platform's whole memory picture. */
#define NV2A_MAP_BUDGET_BYTES ( 24u * 1024u * 1024u )

/* The baker's own fixed lightmap page edge, tools/asset-studio/src/
   formats/BspParser.cpp:25 (`constexpr uint32_t LM_PAGE_SIZE = 1024`) -
   every lu/lv the baker emits is divided by exactly THIS constant
   (BspParser.cpp:853-854), never by the archive's own header fields, so
   nv2a_load_map_geometry's validation below must compare
   header->lightmapWidth/Height against this literal, not merely against
   each other (codex review: a square-but-not-1024 page would otherwise
   pass validation and still sample with the wrong divisor). */
#define NV2A_LM_PAGE_SIZE 1024u

static qboolean nv2a_load_map_geometry( const mpak_header_t *header, const byte *buffer, size_t buffersize )
{
	nv2a_mpak_geometry_t geo;
	uint32_t vert_count, index_count, dword_count;
	uint32_t i;
	uint64_t vb_bytes, idx_bytes, total_bytes;
	uint32_t texture_count, batch_count;

	if( !nv2a_compute_mpak_layout( header, (fs_offset_t)buffersize, &geo ))
		return false;

	if( geo.textureDir.length == 0 || geo.textureData.length == 0 || geo.batches.length == 0
		|| geo.faceRemap.length == 0 || geo.lightmapAtlas.length == 0 )
		return false;

	vert_count = header->vCount;
	index_count = header->iCount;
	if( vert_count < 3 || index_count == 0 )
		return false;
	if( (uint64_t)vert_count * sizeof( mpak_vertex_t ) != (uint64_t)geo.vertices.length )
		return false;
	if( (uint64_t)index_count * sizeof( uint16_t ) != (uint64_t)geo.indices.length )
		return false;

	dword_count = ( index_count + 1 ) / 2;
	vb_bytes = (uint64_t)vert_count * sizeof( nv2a_lit_vertex_t );
	idx_bytes = (uint64_t)dword_count * sizeof( uint32_t );
	/* buffersize (the source archive, already resident per
	   Mod_ProcessRenderData's own contract) is real, known cost this
	   function CAN account for - see NV2A_MAP_BUDGET_BYTES's own comment
	   for what this check does and does not cover. */
	/* X9-F: the per-face table is 4x its own on-disk FaceDrawCmds size
	   (nv2a_map_face_t is 32 bytes; each on-disk FaceDrawCmd is 8) - real
	   memory this function can and should account for, same as every
	   other buffer below. X9-G, nv2a-xbox-specialist review: the new
	   per-face PVS visibility table (nv2a_map_face_visible, 1 byte/face =
	   faceDrawCmds.length/8) belongs in this same ledger for the same
	   reason - it was being allocated (further below) without ever
	   passing through this check, the one inconsistency this file's own
	   "real memory this function CAN and should account for" rule was
	   supposed to rule out. */
	/* X9-K: geo.atlas.length -> geo.textureData.length (the atlas is gone -
	   one contiguous buffer now holds every texture's own DXT1 bytes
	   instead), plus geo.faceRemap.length * 2 for nv2a_map_face_remap[]
	   and its locally-derived inverse nv2a_map_face_orig[] (both real,
	   per-map heap allocations this function should account for, same
	   reasoning as the per-face culling table two lines above). The fixed
	   nv2a_map_textures[]/nv2a_map_batches[] arrays are static BSS, not a
	   per-map allocation, so - like nv2a_visbytes above them - they do not
	   belong in this per-map ledger. */
	total_bytes = vb_bytes + idx_bytes + (uint64_t)geo.faceDrawCmds.length * 4
		+ (uint64_t)geo.faceDrawCmds.length / 8
		+ (uint64_t)geo.faceRemap.length * 2
		+ (uint64_t)geo.textureData.length + (uint64_t)geo.lightmapAtlas.length + (uint64_t)buffersize;
	if( total_bytes > NV2A_MAP_BUDGET_BYTES )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map exceeds NV2A_MAP_BUDGET_BYTES (%u > %u bytes) - not drawing its real geometry\n",
			(unsigned int)total_bytes, (unsigned int)NV2A_MAP_BUDGET_BYTES );
		return false;
	}

	/* Every baked vertex is remapped 1:1 - the shared index buffer already
	   references vertices by their final position in this same array
	   (that's what makes shared indexing possible at all), so unlike the
	   old one-face slice there is no "which vertices does this face touch"
	   deduplication to do here. */
	for( i = 0; i < vert_count; i++ )
	{
		mpak_vertex_t v;

		memcpy( &v, buffer + geo.vertices.offset + (uint64_t)i * sizeof( mpak_vertex_t ), sizeof( v ));

		/* X9-D still only handles vertices on lightmap page 0 - multi-page
		   lightmap support stays explicitly deferred (same limit X9-C
		   already documented), so a vertex on any other page must fail the
		   whole load cleanly rather than silently sample the wrong page. */
		if( v.lightmap_layer != 0 )
			return false;
	}

	/* Every raw index must reference a real slot in the vertex array above
	   - malformed data must fail cleanly here, not be trusted into a GPU
	   attribute-array read out of bounds later. */
	for( i = 0; i < index_count; i++ )
	{
		uint16_t idx;

		memcpy( &idx, buffer + geo.indices.offset + (uint64_t)i * sizeof( uint16_t ), sizeof( idx ));
		if( idx >= vert_count )
			return false;
	}

	/* X9-K: the per-texture DXT1 directory and its matching texture-draw
	   batch table replace the single shared atlas this function validated
	   here through X9-J - see filesystem/mpak_layout.h's own v9 note and
	   nv2a_map_texture_t/nv2a_map_batch_t's own declaration comments
	   (above) for the full format account. Unlike the old atlas (one pair
	   of dimensions, one power-of-two check), there are now up to
	   header->textureCount independent images and header->batchCount
	   independent draw ranges to validate - the SAME "never trust a
	   dimension independent of the lump's own real, bounds-checked
	   length" discipline the atlas check always used (codex review, real
	   finding, X9-B round 2), just applied once per directory/batch entry
	   instead of once for the whole map. Both counts are checked against
	   this build's fixed-capacity arrays FIRST (NV2A_MAP_MAX_TEXTURES/
	   NV2A_MAP_MAX_BATCHES's own declaration comments have the full
	   "reasoned guess pending the Gate phase's real numbers" account),
	   fail closed exactly like every other header field here, before a
	   single entry is parsed. */
	if( header->textureCount == 0 || header->textureCount > NV2A_MAP_MAX_TEXTURES )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map has %u unique textures, this build's fixed directory holds at most %u - not drawing its real geometry\n",
			(unsigned int)header->textureCount, (unsigned int)NV2A_MAP_MAX_TEXTURES );
		return false;
	}
	if( header->batchCount == 0 || header->batchCount > NV2A_MAP_MAX_BATCHES )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map has %u texture batches, this build's fixed table holds at most %u - not drawing its real geometry\n",
			(unsigned int)header->batchCount, (unsigned int)NV2A_MAP_MAX_BATCHES );
		return false;
	}
	if( (uint64_t)header->textureCount * sizeof( mpak_texture_info_t ) != (uint64_t)geo.textureDir.length
		|| (uint64_t)header->batchCount * sizeof( mpak_texture_batch_t ) != (uint64_t)geo.batches.length )
		return false;

	texture_count = header->textureCount;
	batch_count = header->batchCount;

	/* Parse + validate the texture directory: a compressed texture's ONLY
	   size on NV2A is the FORMAT word's BASE_SIZE_U/V log2 fields
	   (deps/nxdk/lib/pbkit/nv_regs.h:581-583 - see nv2a_draw_map_geometry's
	   own stage-0 comment, unchanged reasoning from the atlas era), so
	   width/height must be exact powers of two matching their own paired
	   log2 field, at least one real 4x4 DXT1 block (log2 >= 2), and no
	   larger than NV2A's real 4096 texture edge (log2 <= 12 - the same
	   cap the old atlas check used). dataLength must be the exact byte
	   count of that size's full DXT1 mip chain (MPAK v10,
	   nv2a_dxt1_chain_length), and the running sum of every entry's own
	   dataLength rounded up to 128 bytes must land EXACTLY on the texture-data
	   lump's own real, bounds-checked length after the last entry -
	   refusing the archive beats sampling garbage: a wrong log2 or a
	   short running sum would not crash, it would silently bind the wrong
	   bytes as a texture, or read past nv2a_map_texture_data into
	   whatever memory follows it. physOffset (this entry's own byte
	   offset into nv2a_map_texture_data) is that running sum BEFORE this
	   entry's own padded dataLength is added - mpak_texture_info_t itself stores
	   no offset field, matching the archive's own "byte layout order IS
	   the format" convention one level down (filesystem/mpak_layout.h's
	   own v9 note). */
	{
		uint64_t running = 0;

		for( i = 0; i < texture_count; i++ )
		{
			uint64_t base = geo.textureDir.offset + (uint64_t)i * sizeof( mpak_texture_info_t );
			uint32_t width, height, log2w, log2h, dataLength;

			memcpy( &width,      buffer + base + 0,  sizeof( width ));
			memcpy( &height,     buffer + base + 4,  sizeof( height ));
			memcpy( &log2w,      buffer + base + 8,  sizeof( log2w ));
			memcpy( &log2h,      buffer + base + 12, sizeof( log2h ));
			memcpy( &dataLength, buffer + base + 16, sizeof( dataLength ));

			if( log2w < 2 || log2w > 12 || log2h < 2 || log2h > 12
				|| width != ( 1u << log2w ) || height != ( 1u << log2h )
				|| (uint64_t)dataLength != nv2a_dxt1_chain_length( log2w, log2h, NULL ))
			{
				gEngfuncs.Con_Printf( "ref_nv2a: texture %u (%ux%u) failed validation - not drawing this map's real geometry\n",
					(unsigned int)i, (unsigned int)width, (unsigned int)height );
				return false;
			}

			nv2a_map_textures[i].physOffset = (uint32_t)running;
			nv2a_map_textures[i].log2Width  = log2w;
			nv2a_map_textures[i].log2Height = log2h;
			nv2a_dxt1_chain_length( log2w, log2h, &nv2a_map_textures[i].mipLevels );

			/* v10: each texture's data is zero-padded to 128 bytes, Xbox
			   D3D's own texture base alignment. v9's single levels were
			   always 128-aligned by accident: GoldSrc miptex sizes are
			   multiples of 16, so every POT texture was >= 16x16 = 128 bytes. */
			running += ( (uint64_t)dataLength + 127 ) & ~(uint64_t)127;
		}

		if( running != (uint64_t)geo.textureData.length )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: texture directory's own summed, 128-aligned dataLength (%llu) does not match the texture-data lump's real length (%u) - not drawing this map's real geometry\n",
				(unsigned long long)running, (unsigned int)geo.textureData.length );
			return false;
		}
	}

	/* Parse + validate the texture-draw batch table: batches are a strict
	   partition of the shared index buffer, contiguous by construction
	   (MpakTextureBatch's own doc comment, tools/asset-studio/src/
	   formats/MpakHeader.h) - the SAME running-cursor validation the
	   FaceDrawCmds tiling check further below already applies to
	   individual faces, one level up, applied here to whole batches.
	   textureIndex must name a real directory entry (there is no "no
	   texture" fallback to degrade to - an out-of-range index would bind
	   whatever memory happens to follow the directory, so this fails
	   closed like every other structural check here). startFace/faceCount
	   are deliberately NOT cross-checked against a real face-count total
	   in this loop - the per-face culling table they index into
	   (nv2a_map_faces) is still optional and does not exist yet at this
	   point in the load (see its own declaration comment);
	   nv2a_draw_map_geometry() bounds-checks startFace+faceCount against
	   nv2a_map_face_count again, at the point it actually uses them,
	   before ever indexing with them. */
	{
		uint64_t face_cursor = 0, index_cursor = 0;

		for( i = 0; i < batch_count; i++ )
		{
			uint64_t base = geo.batches.offset + (uint64_t)i * sizeof( mpak_texture_batch_t );
			uint32_t textureIndex, startFace, faceCount, startIndex, indexCount;

			memcpy( &textureIndex, buffer + base + 0,  sizeof( textureIndex ));
			memcpy( &startFace,    buffer + base + 4,  sizeof( startFace ));
			memcpy( &faceCount,    buffer + base + 8,  sizeof( faceCount ));
			memcpy( &startIndex,   buffer + base + 12, sizeof( startIndex ));
			memcpy( &indexCount,   buffer + base + 16, sizeof( indexCount ));

			if( textureIndex >= texture_count || (uint64_t)startFace != face_cursor || (uint64_t)startIndex != index_cursor
				|| index_cursor + indexCount > (uint64_t)index_count )
			{
				gEngfuncs.Con_Printf( "ref_nv2a: texture batch %u failed validation - not drawing this map's real geometry\n", (unsigned int)i );
				return false;
			}

			nv2a_map_batches[i].textureIndex = textureIndex;
			nv2a_map_batches[i].startFace    = startFace;
			nv2a_map_batches[i].faceCount    = faceCount;
			nv2a_map_batches[i].startIndex   = startIndex;
			nv2a_map_batches[i].indexCount   = indexCount;

			face_cursor  += faceCount;
			index_cursor += indexCount;
		}

		if( index_cursor != (uint64_t)index_count )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: texture batches cover %llu of %u real indices - not drawing this map's real geometry\n",
				(unsigned long long)index_cursor, (unsigned int)index_count );
			return false;
		}
	}

	/* Same "don't trust dimensions independent of the lump's own real,
	   bounds-checked length" discipline the atlas check above already
	   established (codex review, X9-B round 2) - RGBA8 is a plain 4 bytes
	   per texel, no block rounding needed. */
	{
		uint64_t expected_lm = (uint64_t)header->lightmapWidth * header->lightmapHeight * header->lightmapLayers * 4;

		if( header->lightmapWidth == 0 || header->lightmapHeight == 0 || header->lightmapLayers == 0
			|| header->lightmapFormat != 0 /* 0 = RGBA8, the only format anything bakes today */
			|| expected_lm != (uint64_t)geo.lightmapAtlas.length )
			return false;

		/* The baker normalizes BOTH lu and lv by ONE fixed compile-time
		   constant, LM_PAGE_SIZE = 1024 (tools/asset-studio/src/formats/
		   BspParser.cpp:25,853-854) - not by whatever this archive's own
		   header->lightmapWidth/Height happen to say. The remap loop below
		   undoes that divisor using the header fields (matching what stage
		   1 pushes to IMAGE_RECT), which is only the correct inverse when
		   those fields actually equal 1024: a square-but-not-1024 page
		   (e.g. 512x512, codex review) would pass a width==height check yet
		   still be scaled by the wrong divisor, silently sampling every
		   luxel from roughly the wrong quadrant of the page instead of
		   failing closed. Comparing directly against the baker's own
		   constant closes that gap; if a future baker ever emits a
		   different page size this reads its own header->lightmapWidth/
		   Height as the divisor, not a hardcoded 1024, so this check would
		   need to move with it. */
		if( header->lightmapWidth != NV2A_LM_PAGE_SIZE || header->lightmapHeight != NV2A_LM_PAGE_SIZE )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: lightmap page %ux%u is not %ux%u - the baker's lu/lv are normalized by a fixed %u-luxel divisor, not drawing its real geometry\n",
				(unsigned int)header->lightmapWidth, (unsigned int)header->lightmapHeight,
				NV2A_LM_PAGE_SIZE, NV2A_LM_PAGE_SIZE, NV2A_LM_PAGE_SIZE );
			return false;
		}
	}

	nv2a_map_vb = MmAllocateContiguousMemoryEx( (uint32_t)vb_bytes, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_vb )
	{
		/* codex review, real finding: a validated, real nv2a-profile
		   archive that fails an allocation here fell back to the test
		   triangle with no diagnostic at all - the NV2A_MAP_BUDGET_BYTES
		   check above only guards against a corrupt/absurd archive, not
		   real contiguous-memory fragmentation on an already-running
		   console, so this path is genuinely reachable and worth a real
		   message when it happens. */
		gEngfuncs.Con_Printf( "ref_nv2a: map vertex buffer allocation failed (%u bytes)\n", (unsigned int)vb_bytes );
		return false;
	}
	{
		nv2a_lit_vertex_t *dst = (nv2a_lit_vertex_t *)nv2a_map_vb;

		for( i = 0; i < vert_count; i++ )
		{
			mpak_vertex_t v;

			memcpy( &v, buffer + geo.vertices.offset + (uint64_t)i * sizeof( mpak_vertex_t ), sizeof( v ));

			dst[i].pos[0] = v.x;
			dst[i].pos[1] = v.y;
			dst[i].pos[2] = v.z;
			/* X9-K: no more atlas-rect remap - v.u/v.v are used AS-IS.
			   They are already in the exact format NV2A's native WRAP
			   addressing needs: BspParser.cpp normalizes them by the
			   TEXTURE's own native (pre-resample) pixel size
			   (PakManager.cpp:20-21), so they carry real, unclamped repeat
			   counts for a tiling face (a face spanning 6 texture repeats
			   reads u in [0,6), not folded into [0,1) the way the old
			   atlas-rect frac() remap required) - hardware WRAP tiles
			   exactly that range in silicon, no shader-side wrap needed.
			   See this function's own top-of-function comment for the
			   full before/after account. */
			dst[i].uv0[0] = v.u;
			dst[i].uv0[1] = v.v;
			/* The lightmap is bound as a LINEAR texture (LU_IMAGE_A8B8G8R8 +
			   CONTROL1 pitch + IMAGE_RECT, stage 1 in nv2a_draw_map_geometry),
			   and NV2A addresses linear textures in UNNORMALIZED texel
			   coordinates (0..width, 0..height), like GL rectangle textures -
			   not 0..1 (xemu hw/xbox/nv2a/pgraph/glsl/psh.c: `rect_tex[i] =
			   f.linear` and `norm(coord) = coord / textureSize()` for linear
			   stages only; deps/nxdk/samples/mesh/verts.h feeds integer texel
			   UVs such as {44,143} to its 256x256 LU_IMAGE_A8R8G8B8 texture,
			   main.c:145-150, and vs.vs.cg passes them through unscaled). The
			   baker normalizes lu/lv by LM_PAGE_SIZE (tools/asset-studio/src/
			   formats/BspParser.cpp:853-854) and its flatten() guarantees
			   lightmapWidth == lightmapHeight == LM_PAGE_SIZE, never trimmed
			   (BspParser.cpp:160-182, enforced at load above), so header->
			   lightmapWidth/Height - the same values stage 1 pushes to
			   IMAGE_RECT (nv2a_map_lightmap_width/height are assigned from
			   them further down, after this loop) - are the exact inverse of
			   that divisor. Fed 0..1, clamp + bilinear only ever blended page
			   texels (0,0) and (1,0), i.e. the ramp strip's values 0 and 1: a
			   lightmap-only debug shader produced exactly two values across
			   the whole frame, 0 (254,979 px) and 1 (50,263 px), while the
			   GPU-visible uv1 and the baked luxels at those texels were
			   correct (mean 105/255). uv0 (above) needs no equivalent
			   descale: it is already normalized by the diffuse texture's
			   own native pixel size at bake time (this function's own
			   top-of-function comment), independent of the lightmap's
			   separate LM_PAGE_SIZE convention. Multi-page lightmaps
			   (lightmap_layer != 0) stay rejected at
			   load (above); with texel coordinates a page > 0 would need a
			   per-page V offset or a separate binding, not just a scale.
			   docs/r3d/divergences.md #100. */
			dst[i].uv1[0] = v.lu * (float)header->lightmapWidth;
			dst[i].uv1[1] = v.lv * (float)header->lightmapHeight;
		}
	}
	nv2a_map_vert_count = vert_count;

	/* X9-K: ONE contiguous buffer for every texture's own DXT1 bytes,
	   back-to-back in directory order - replaces the single shared-atlas
	   allocation this function used through X9-J. Copied verbatim, same
	   "already in the right hardware layout, no un-swizzle needed"
	   reasoning the old atlas copy cited (this file's own header
	   comment). nv2a_map_textures[].physOffset (computed in the
	   validation pass above) is each texture's own byte offset into THIS
	   buffer. */
	nv2a_map_texture_data = MmAllocateContiguousMemoryEx( geo.textureData.length, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_texture_data )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map texture-data allocation failed (%u bytes)\n", (unsigned int)geo.textureData.length );
		nv2a_free_map_resources();
		return false;
	}
	memcpy( nv2a_map_texture_data, buffer + geo.textureData.offset, geo.textureData.length );
	nv2a_map_texture_count = texture_count;
	nv2a_map_batch_count   = batch_count;

	nv2a_map_lightmap = MmAllocateContiguousMemoryEx( geo.lightmapAtlas.length, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_lightmap )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map lightmap allocation failed (%u bytes)\n", (unsigned int)geo.lightmapAtlas.length );
		nv2a_free_map_resources();
		return false;
	}
	memcpy( nv2a_map_lightmap, buffer + geo.lightmapAtlas.offset, geo.lightmapAtlas.length );
	nv2a_map_lightmap_width  = header->lightmapWidth;
	nv2a_map_lightmap_height = header->lightmapHeight;
	nv2a_map_lightmap_pitch  = nv2a_map_lightmap_width * 4; /* RGBA8, LU_IMAGE - no block rounding */

	/* Pack pairs of raw indices into dwords for NV20_TCL_PRIMITIVE_3D_
	   INDEX_DATA (see nv2a_map_index_dwords's own declaration comment for
	   why - low half first, matching deps/nxdk/samples/mesh/verts.h's own
	   data). An odd index_count pads its final dword's high half by
	   repeating the last real index. Unlike X9-B/C there is no local-index
	   remap here - every raw index already names its final slot in
	   nv2a_map_vb directly, so this is a straight widen-and-pack walk over
	   the whole buffer. */
	nv2a_map_index_dwords = Mem_Malloc( r_temppool, (size_t)idx_bytes );
	if( !nv2a_map_index_dwords )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map index buffer allocation failed (%u bytes)\n", (unsigned int)idx_bytes );
		nv2a_free_map_resources();
		return false;
	}
	nv2a_map_dword_count = dword_count;
	for( i = 0; i < dword_count; i++ )
	{
		uint16_t lo16, hi16;
		uint32_t lo, hi;

		memcpy( &lo16, buffer + geo.indices.offset + (uint64_t)( i * 2 ) * sizeof( uint16_t ), sizeof( lo16 ));
		lo = lo16;
		if( i * 2 + 1 < index_count )
		{
			memcpy( &hi16, buffer + geo.indices.offset + (uint64_t)( i * 2 + 1 ) * sizeof( uint16_t ), sizeof( hi16 ));
			hi = hi16;
		}
		else
		{
			hi = lo;
		}
		nv2a_map_index_dwords[i] = lo | ( hi << 16 );
	}

	/* X9-E: the camera is no longer computed here. nv2a_map_viewproj is
	   now written every real frame by GL_RenderFrame's own real
	   vieworigin/viewangles/fov - see nv2a_view_valid's own declaration
	   comment for why a map loaded before the first real frame still
	   draws safely (falls back to the test triangle) instead of ever
	   reading whatever this matrix defaults to. */

	/* X9-F: build the per-face bounding-box/index-range table for
	   frustum culling, from the FaceDrawCmds lump captured above. The
	   tiling is VALIDATED here, not trusted - a running-cursor check
	   confirming every start_index lines up with the end of the
	   previous face, every index_count is a non-zero multiple of 3, and
	   the final cursor lands exactly on index_count, matching what was
	   independently confirmed in Python against the real crossfire.mpak
	   (docs/r3d/divergences.md #96). On any failure (a stale archive, or
	   a future baker change this module's own layout mirror has not
	   caught up with), nv2a_map_faces is left NULL and
	   nv2a_draw_map_geometry() falls back to submitting the whole index
	   buffer unculled - the map still draws exactly as it does today,
	   just without this optimisation, never a black screen. */
	if( geo.faceDrawCmds.length != 0 && ( geo.faceDrawCmds.length % 8 ) == 0 )
	{
		uint32_t face_count = (uint32_t)( geo.faceDrawCmds.length / 8 );
		nv2a_map_face_t *faces = Mem_Malloc( r_temppool, (size_t)face_count * sizeof( nv2a_map_face_t ));

		if( !faces )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: face-culling table allocation failed (%u bytes) - drawing this map unculled\n",
				(unsigned int)( (uint64_t)face_count * sizeof( nv2a_map_face_t )));
		}
		else
		{
			uint32_t expect_cursor = 0;
			qboolean tiling_ok = true;
			nv2a_lit_vertex_t *verts = (nv2a_lit_vertex_t *)nv2a_map_vb;
			/* X9-K sky-skip fix: MpakFaceInfo is one record per face, in the
			   SAME (sorted) order FaceDrawCmds already uses - fails open
			   (every face's .sky left false, drawn same as before this fix)
			   on a stale archive whose faceInfo lump doesn't have exactly
			   face_count records, never blocking the load over an
			   optional, cosmetic-only check. */
			qboolean faceinfo_ok = geo.faceInfo.length == (uint64_t)face_count * sizeof( mpak_face_info_t );

			for( i = 0; i < face_count; i++ )
			{
				uint32_t start_index, face_indices, k;

				memcpy( &start_index,  buffer + geo.faceDrawCmds.offset + (uint64_t)i * 8,     sizeof( start_index ));
				memcpy( &face_indices, buffer + geo.faceDrawCmds.offset + (uint64_t)i * 8 + 4, sizeof( face_indices ));

				if( faceinfo_ok )
				{
					mpak_face_info_t info;

					memcpy( &info, buffer + geo.faceInfo.offset + (uint64_t)i * sizeof( mpak_face_info_t ), sizeof( info ));
					faces[i].sky = ( info.flags & MPAK_SURF_DRAWSKY ) != 0;
				}
				else
				{
					faces[i].sky = false;
				}

				if( face_indices == 0 || ( face_indices % 3 ) != 0 || start_index != expect_cursor
					|| (uint64_t)start_index + face_indices > index_count )
				{
					tiling_ok = false;
					break;
				}

				ClearBounds( faces[i].mins, faces[i].maxs );
				for( k = start_index; k < start_index + face_indices; k++ )
				{
					uint16_t vi;

					memcpy( &vi, buffer + geo.indices.offset + (uint64_t)k * sizeof( uint16_t ), sizeof( vi ));
					AddPointToBounds( verts[vi].pos, faces[i].mins, faces[i].maxs );
				}

				/* Snap outward to a multiple of 6 logical indices (= 3
				   dwords) - see nv2a_map_face_t's own declaration
				   comment for why 6, not 2 or 3 alone, is the right
				   granularity for a packed two-indices-per-dword
				   buffer. Codex review, real finding: the format
				   imposes no even-triangle-count constraint on the
				   MAP TOTAL, only on each face's own count (a
				   multiple of 3), so index_count itself can be
				   6k+3 - dword_count (the buffer's own real
				   allocation, ceil(index_count/2)) is then NOT a
				   multiple of 3, and the last face's outward snap
				   can round past it (index_count==3: dword_count==2,
				   but the unclamped snap gives 3) - a real one-dword
				   out-of-bounds read submitted straight to the GPU
				   as two bogus vertex indices. dword_count is always
				   the correct upper bound regardless (it already
				   accounts for the odd-index_count padding
				   half-dword nv2a_map_index_dwords itself fills in),
				   so clamping to it is not a workaround, it is the
				   exact right end position for whichever face is
				   actually last. */
				faces[i].dword_begin = ( start_index / 6 ) * 3;
				faces[i].index_begin = start_index;
				faces[i].index_end   = start_index + face_indices;
				faces[i].texture     = 0xFFFFFFFFu; /* filled from the batch table below; stays invalid if no valid batch covers it */
				faces[i].dword_end   = (( start_index + face_indices + 5 ) / 6 ) * 3;
				if( faces[i].dword_end > dword_count )
					faces[i].dword_end = dword_count;

				expect_cursor = start_index + face_indices;
			}

			if( tiling_ok && expect_cursor == index_count )
			{
				nv2a_map_faces = faces;
				nv2a_map_face_count = face_count;

				/* X9-M: each sorted face's own texture, from the batch
				   that covers it. Batches the draw loop would reject
				   (range past face_count) are skipped here too; their
				   faces keep 0xFFFFFFFF and the entity pass skips them,
				   as the world pass skips the batch. */
				{
					uint32_t b2, f2;
					for( b2 = 0; b2 < nv2a_map_batch_count; b2++ )
					{
						const nv2a_map_batch_t *bt = &nv2a_map_batches[b2];
						if( (uint64_t)bt->startFace + bt->faceCount > (uint64_t)face_count )
							continue;
						for( f2 = bt->startFace; f2 < bt->startFace + bt->faceCount; f2++ )
							faces[f2].texture = bt->textureIndex;
					}
				}

				/* X9-K: parse the archive's own originalToSortedFaceIndex[]
				   table (nv2a_map_face_remap - ORIGINAL BSP face index ->
				   SORTED position) and derive its LOCAL inverse
				   (nv2a_map_face_orig - SORTED position -> ORIGINAL BSP
				   face index), needed by nv2a_mark_leaves() and
				   nv2a_draw_map_geometry()'s own backface test
				   respectively - see both arrays' own declaration comments
				   above for the full "same face, different index after
				   sorting" account this bridges. Gated on tiling_ok/
				   face_count (this branch) since the SORTED half of this
				   table only makes sense as a position into nv2a_map_faces,
				   which does not exist otherwise. Every entry is validated
				   as part of a genuine bijection - in range, and no two
				   original faces claiming the same sorted slot - before
				   being trusted; a table that fails this (a stale archive,
				   or a future baker bug) fails OPEN, same discipline as a
				   FaceDrawCmds tiling failure: both remap arrays stay NULL,
				   and nv2a_mark_leaves()/nv2a_draw_map_geometry() already
				   treat that exactly like "no table for this map" -
				   frustum(+backface)-only culling, never a wrong-surface
				   read, never a black screen. */
				if( geo.faceRemap.length > 0
					&& ( (uint64_t)geo.faceRemap.length % sizeof( uint32_t )) == 0
					&& ( (uint64_t)geo.faceRemap.length / sizeof( uint32_t )) == (uint64_t)face_count )
				{
					uint32_t remap_count = (uint32_t)( (uint64_t)geo.faceRemap.length / sizeof( uint32_t ));
					uint32_t *remap = Mem_Malloc( r_temppool, (size_t)remap_count * sizeof( uint32_t ));
					uint32_t *orig  = remap ? Mem_Malloc( r_temppool, (size_t)face_count * sizeof( uint32_t )) : NULL;

					if( !remap || !orig )
					{
						gEngfuncs.Con_Printf( "ref_nv2a: face-remap table allocation failed - PVS culling disabled for this map\n" );
						if( remap ) Mem_Free( remap );
					}
					else
					{
						qboolean remap_ok = true;
						uint32_t j;

						for( j = 0; j < face_count; j++ )
							orig[j] = 0xFFFFFFFFu; /* sentinel: no original face has claimed this sorted slot yet */

						for( j = 0; j < remap_count; j++ )
						{
							uint32_t sortedIdx;

							memcpy( &sortedIdx, buffer + geo.faceRemap.offset + (uint64_t)j * sizeof( uint32_t ), sizeof( sortedIdx ));
							if( sortedIdx >= face_count || orig[sortedIdx] != 0xFFFFFFFFu )
							{
								remap_ok = false;
								break;
							}
							remap[j] = sortedIdx;
							orig[sortedIdx] = j;
						}

						if( remap_ok )
						{
							nv2a_map_face_remap = remap;
							nv2a_map_face_remap_count = remap_count;
							nv2a_map_face_orig = orig;
						}
						else
						{
							gEngfuncs.Con_Printf( "ref_nv2a: face-remap table is not a valid bijection - PVS culling disabled for this map\n" );
							Mem_Free( remap );
							Mem_Free( orig );
						}
					}
				}
				else
				{
					gEngfuncs.Con_Printf( "ref_nv2a: face-remap table length does not match this map's own face count - PVS culling disabled for this map\n" );
				}

				/* X9-G: the per-face PVS visibility table, index-parallel
				   with nv2a_map_faces (both walk the archive's one real
				   LUMP_FACES in the same order - see
				   nv2a_map_face_visible's own declaration comment).
				   Defaults to all-visible (nv2a_mark_leaves() has not
				   run yet for this map) so the very first frame draws
				   the frustum-culled set rather than nothing, matching
				   nv2a_map_faces == NULL's own "fail open" fallback in
				   spirit - GL_RenderFrame always calls nv2a_mark_leaves()
				   before R_EndFrame ever draws, so this default is a
				   safety net, not the expected steady state. Allocation
				   failure is not fatal to the load: the map still draws,
				   frustum-culled only, exactly as it did before X9-G. */
				nv2a_map_face_visible = Mem_Malloc( r_temppool, (size_t)face_count );
				if( nv2a_map_face_visible )
					memset( nv2a_map_face_visible, 1, (size_t)face_count );
				else
					gEngfuncs.Con_Printf( "ref_nv2a: PVS visibility table allocation failed (%u bytes) - frustum culling only\n",
						(unsigned int)face_count );
			}
			else
			{
				gEngfuncs.Con_Printf( "ref_nv2a: FaceDrawCmds tiling did not validate - drawing this map unculled\n" );
				Mem_Free( faces );
			}
		}
	}

	return true;
}

/* Called for EVERY model the engine loads or unloads - the world, every
   inline brush submodel, every studio model, every sprite
   (engine/common/model.c) - not just the map. Only ever touches the
   current map-geometry state for the ONE model that actually owns it:
   an unload is ignored unless it's the exact model that populated it
   (nv2a_map_owner), and a load only replaces it once the new buffer has
   already been confirmed to be a real nv2a-profile archive - a later,
   unrelated model's own load or unload leaves whatever the world already
   set up completely alone (codex review, real finding: the first version
   freed unconditionally on every call, so the first studio/sprite model
   loaded right after the world silently wiped it). */
static qboolean Mod_ProcessRenderData( model_t *mod, qboolean create, const byte *buffer, size_t buffersize )
{
	const mpak_header_t *header;

	if( !create )
	{
		if( mod == nv2a_map_owner )
			nv2a_free_map_resources();
		return true;
	}

	if( nv2a_mpak_header_valid( buffer, buffersize, &header ))
	{
		nv2a_free_map_resources();
		if( nv2a_load_map_geometry( header, buffer, buffersize ))
		{
			nv2a_map_ready = true;
			nv2a_map_owner = mod;
		}
	}

	return true; /* the model load itself always succeeds regardless; drawing
	                real geometry is opportunistic - a model that isn't a real
	                nv2a-profile map archive just leaves whatever (if anything)
	                the world already set up untouched */
}

static void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	;
}

static void CL_DrawParticles( double frametime, particle_t *particles, float partsize )
{
	;
}

static void CL_DrawTracers( double frametime, particle_t *tracers )
{
	;
}

static void CL_DrawBeams( int fTrans, BEAM *beams )
{
	;
}

static intptr_t RefGetParm( int parm, int arg )
{
	if( parm == PARM_GL_CONTEXT_TYPE )
		return CONTEXT_TYPE_SOFTWARE;

	return 0;
}

static int GL_FindTexture( const char *name )
{
	return 0;
}

static const char *GL_TextureName( unsigned int texnum )
{
	return NULL;
}

static const byte *GL_TextureData( unsigned int texnum )
{
	return NULL;
}

static int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	return 0;
}

static int GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	return 0;
}

static void R_OverrideTextureSourceSize( unsigned int textnum, unsigned int srcWidth, unsigned int srcHeight )
{
	;
}

static void GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	;
}

static void GL_Bind( int tmu, unsigned int texnum )
{
	;
}

/* X9-G: real PVS culling, closing the last half of #91/#95's own
   carried-forward scope limit ("no visibility/frustum culling" - X9-F
   closed the frustum half, this closes visibility). Port of
   ref/gl/gl_rmain.c's own R_FindViewLeaf plus ref/gl/gl_rsurf.c's own
   R_MarkLeaves, folded into one function since this module has no
   separate per-frame setup pass to host the first half the way ref/gl
   does - RI.viewleaf/RI.oldviewleaf exist for several OTHER ref/gl
   consumers (decal placement, ambient sound, sky) this module does not
   implement, so nv2a_viewleaf/nv2a_oldviewleaf only ever need to serve
   this one function, called once per real frame from GL_RenderFrame
   (see nv2a_view_captured_this_frame's own guard for why that really is
   at most once per frame).

   codex review, real citation correction - THIRD round, the previous two
   were each wrong in turn: the loop below that marks faces visible by
   walking each PVS-visible leaf's own firstmarksurface/nummarksurfaces
   is NOT what R_MarkLeaves does (it only walks mleaf_t::parent chains,
   stamping node->visframe for a later top-down walk to consume) - that
   correction stands. But the SECOND correction (crediting only the
   disused, overview-only R_DrawTopViewLeaf, and claiming the normal
   single-player path "does not derive visibility from leaf marksurfaces
   at all") was itself wrong, confirmed by reading R_RecursiveWorldNode
   in full (gl_rsurf.c:3457-3496) and R_CullSurface (gl_cull.c:66-77):
   the SAME function that does the node-based firstsurface/numsurfaces
   walk ALSO, when its recursive descent reaches an actual leaf
   (`if( node->contents < 0 )`, gl_rsurf.c:3482-3489), stamps
   `mark[i]->visframe = tr.framecount` for every one of THAT LEAF's own
   marksurfaces - literally the identical "for a PVS-visible leaf, mark
   its own marksurfaces" step this port's loop performs. The later
   node-based walk (gl_rsurf.c:3507-3516) then calls R_CullSurface on
   each of a NODE's own firstsurface/numsurfaces range, which rejects
   any surface whose visframe was NOT stamped this way
   (`surf->visframe != tr.framecount`, gl_cull.c:70) - so a surface
   compiled onto a given node only actually gets drawn if it was ALSO
   reached through some visible leaf's own marksurfaces list. The node
   walk is ref/gl's own iteration/draw-chain-building order; the leaf-
   marksurface stamp is what actually DECIDES per-surface visibility,
   and this port's loop computes exactly that decision directly, with no
   need for the node-based iteration on top of it (X9-F's own
   nv2a_map_faces is already a flat per-face table, not a node tree, so
   there is no draw-chain-building order to replicate - only the
   visibility decision matters here). The visible-face SET this produces
   is therefore not merely "provably equivalent" to some indirect
   reference mechanism, it performs the SAME concrete step
   (gl_rsurf.c:3487-3488) the reference's own normal, currently-active
   single-player path performs - traced through the real, current
   ref/gl source both times this comment was corrected, not assumed
   from a plausible-sounding shape either time.

   One further, deliberate, accepted difference from the reference - the
   FCVAR_CHANGED handling above sets `recompute = true` rather than
   nulling nv2a_viewleaf the way R_MarkLeaves' own `RI.viewleaf = NULL`
   does (see that block's own comment for why nulling doesn't work here,
   and `recompute`'s own declaration comment for why it is intentionally
   a SEPARATE flag from `force` - codex review, real finding: an earlier
   version reused `force` for this, which also wrongly enabled the
   merged R_FatPVS(test,...) call below on every convar toggle even when
   the crossing test never ran); the reference's null forces exactly one
   fully-UNculled frame on ANY nv2a_r_novis toggle regardless of
   direction (novis latches true off the null, independent of the new
   convar value), whereas this port's `recompute` only forces a
   RECOMPUTE - a toggle from on to off draws a correctly-culled frame
   immediately, with no stray unculled frame in between. Different
   observable behaviour for exactly one frame, only at the moment a
   debug/A-B convar is toggled by a developer - judged a real
   improvement, not a bug, and not worth reproducing the reference's own
   incidental quirk to match it. */

/* Needs no baker work and no private mirror of the engine's own node/
   leaf/visdata loader (unlike nv2a_compute_mpak_layout()'s own geometry
   mirror above): the v7 "classic core" lumps (MpakHeader.h's own v6->v7
   note) make a map archive a real, valid BSP30 file, so the engine loads
   a real mnode_t/mleaf_t/marksurfaces/visdata tree for it through its
   completely standard, unmodified Mod_LoadBmodelLumps path - the exact
   same tree ref/soft's own R_MarkLeaves-equivalent (r_main.c) already
   reads via gp_cl->models[1]. nv2a_map_faces[i] and world->surfaces[i]
   are the SAME face at the SAME index: both are built by walking the
   archive's one real LUMP_FACES in write order (MpakFaceInfo's own doc
   comment, MpakHeader.h) - one by the baker into FaceDrawCmds, the other
   by the engine's own loader into msurface_t - so a marksurface pointer's
   offset from world->surfaces is directly a valid nv2a_map_faces index,
   no translation table needed. */
static void nv2a_mark_leaves( const vec3_t vieworigin )
{
	const model_t *world = gp_cl->models[1];
	qboolean novis = false;
	qboolean force = false; /* codex review, real finding: this must stay
	                            true ONLY for the real crossing-line-
	                            contents case below (it gates the SECOND,
	                            merged R_FatPVS call further down) - the
	                            reference's own FCVAR_CHANGED path nulls
	                            RI.viewleaf instead, which SKIPS that same
	                            crossing-test block entirely (RI.viewleaf
	                            is NULL, so `if (RI.viewleaf != NULL)`
	                            never runs), meaning `force` stays false
	                            there too. An earlier version of this port
	                            set `force = true` directly on
	                            FCVAR_CHANGED to force a recompute (see
	                            nv2a_recompute below for why that still
	                            can't null nv2a_viewleaf the way the
	                            reference does) - but reusing this SAME
	                            flag also mistakenly enabled the merged
	                            R_FatPVS(test,...) call on every convar
	                            toggle, even when `test` was never
	                            confirmed to be a genuinely different,
	                            non-solid leaf - merging in a degenerate
	                            test point's PVS row (e.g. one that lands
	                            in solid space, where Mod_FatPVS fills all
	                            bits set) could silently defeat culling
	                            until the next real leaf crossing. */
	qboolean recompute = false; /* separate from `force` - true forces the
	                                same-leaf skip check below to fall
	                                through to a real recompute, WITHOUT
	                                also implying the crossing test found
	                                a real second leaf to merge in. */
	mleaf_t *leaf = NULL;
	vec3_t test;
	uint32_t i;

	/* world != nv2a_map_owner covers both "no map loaded yet" and the
	   (currently unreachable, since this port has no inline-brushmodel
	   consumer of this path) case of a non-world model owning the last
	   loaded geometry - either way, there is no world model whose
	   leafs/marksurfaces this function could safely walk.

	   X9-K, real finding while wiring up the face-remap fix below: this
	   guard must ALSO bail on `!nv2a_map_face_remap` - NOT just fold that
	   into the per-face translate-or-skip check further down the way it
	   might look sufficient to. This function unconditionally memsets the
	   WHOLE nv2a_map_face_visible array to 0 below, then relies ENTIRELY
	   on the leaf/marksurface loop to mark anything visible again; with
	   no remap table, every entry in that loop would translate to
	   "nothing to mark" (see the loop's own comment), and the array would
	   stay all-zero for the rest of the session - every face culled,
	   exactly backwards from this file's own "no table for this map"
	   fail-open discipline (nv2a_map_faces == NULL, nv2a_map_face_visible
	   == NULL right above, and nv2a_r_novis all degrade to MORE geometry
	   drawn, never less). Bailing out HERE instead leaves
	   nv2a_map_face_visible exactly as nv2a_load_map_geometry()
	   initialized it - all-visible - which is the correct degraded
	   state. */
	if( !nv2a_map_face_visible || !nv2a_map_face_remap || !world || world != nv2a_map_owner )
		return;

	/* X9-G, render-architecture-specialist review: nv2a_map_faces[i] and
	   world->surfaces[i] being the same face at the same index rests on
	   an unenforced cross-subsystem invariant (this baker's own face loop
	   and the engine's Mod_LoadSurfaces independently walking the same
	   LUMP_FACES the same way) - the pointer-range check below catches a
	   garbage offset, but not a silent reordering. This won't catch a
	   reordering either, but it is a near-zero-cost check for the far
	   more likely failure - the two loaders disagreeing on HOW MANY faces
	   exist at all - logged once per map, not every call. */
	if( nv2a_pvs_parity_checked_for != world )
	{
		nv2a_pvs_parity_checked_for = world;
		if( (uint32_t)world->numsurfaces != nv2a_map_face_count )
		{
			/* codex review, real finding: a mismatch here means the index-
			   bridging invariant this whole function depends on
			   (nv2a_map_faces[i] == world->surfaces[i]) is not just
			   unverifiable, it is KNOWN FALSE for this map - continuing
			   to compute visibility from marksurface offsets under a
			   known-false assumption could zero out nv2a_map_face_visible
			   entries for perfectly valid, currently-visible faces
			   (silently missing geometry), the opposite of this file's
			   own established "fail open" discipline for every other
			   PVS/frustum failure mode. Disable PVS culling for this map
			   PERMANENTLY (not just this call) by freeing the table and
			   returning - every future call hits the same "no table"
			   guard at the top of this function, and
			   nv2a_draw_map_geometry() already treats NULL exactly like
			   nv2a_map_faces == NULL: frustum-only, never a black screen
			   and never missing geometry a visible leaf should show. */
			gEngfuncs.Con_Printf( "ref_nv2a: WARNING - world->numsurfaces (%d) != nv2a_map_face_count (%u); "
				"disabling PVS culling for this map (frustum culling only)\n",
				world->numsurfaces, nv2a_map_face_count );
			Mem_Free( nv2a_map_face_visible );
			nv2a_map_face_visible = NULL;
			return;
		}
	}

	if( FBitSet( nv2a_r_novis.flags, FCVAR_CHANGED ))
	{
		/* Force a recompute below even if the camera has not moved to a
		   new leaf since last frame - toggling the convar must be visible
		   immediately, not on the next real leaf crossing. Setting
		   `recompute` (not `force` - see its own declaration comment for
		   why the two must stay separate) directly, rather than
		   nv2a_viewleaf = NULL (the reference's own RI.viewleaf = NULL),
		   is deliberate: unlike ref/gl, this function recomputes
		   nv2a_viewleaf itself a few lines below on every call, so
		   clearing it here would just be overwritten immediately and
		   never actually reach the skip check that needs it. */
		ClearBits( nv2a_r_novis.flags, FCVAR_CHANGED );
		recompute = true;
	}

	/* R_FindViewLeaf's own job (gl_rmain.c) - this module has no separate
	   per-frame pass to host it in, so it runs here, immediately before
	   the crossing test below that depends on its result being current
	   for THIS frame, not last frame's. */
	nv2a_viewleaf = gEngfuncs.Mod_PointInLeaf( vieworigin, world->nodes, (model_t *)world );

	/* R_MarkLeaves' own crossing-line-contents merge (gl_rsurf.c): a
	   camera sitting exactly on a leaf boundary (a door, a water surface)
	   can read as one leaf while a point a few units off it reads as
	   another - test a point offset toward solid ground and force a
	   recompute if that lands in a genuinely different, non-solid leaf,
	   so the PVS used this frame is not missing whatever that other leaf
	   can see. */
	VectorCopy( vieworigin, test );
	if( nv2a_viewleaf != NULL )
	{
		if( nv2a_viewleaf->contents == CONTENTS_EMPTY )
			test[2] -= 16.0f;
		else
			test[2] += 16.0f;

		leaf = gEngfuncs.Mod_PointInLeaf( test, world->nodes, (model_t *)world );

		if( leaf->contents != CONTENTS_SOLID && nv2a_viewleaf != leaf )
			force = true;
	}

	if( nv2a_viewleaf == nv2a_oldviewleaf && nv2a_viewleaf != NULL && !force && !recompute )
		return; /* same leaf as last call - the visibility table already reflects it */

	if( nv2a_r_lockpvs.value )
		return; /* frozen for inspection - same position ref/gl's own r_lockpvs check uses */

	nv2a_oldviewleaf = nv2a_viewleaf;

	/* reference-renderer-specialist review: the reference's own novis
	   OR-chain (gl_rsurf.c) has a fourth term, RF_DRAW_OVERVIEW, dropped
	   here - deliberately, not an oversight, same reasoning GL_RenderFrame
	   already gives for dropping the same flag from its own frustum
	   construction below: RF_DRAW_OVERVIEW requires CL_IsDevOverviewMode(),
	   itself gated on dev_overview>0 AND (host_developer or
	   cls.spectator) - unreachable in this fork's own real, single-
	   player-only, Xbox-networking-disabled configuration today. Cited
	   here rather than re-argued, since the call site is different but
	   the fact being relied on is the same one. */
	if( nv2a_r_novis.value || !nv2a_viewleaf || !world->visdata )
		novis = true;

	gEngfuncs.R_FatPVS( vieworigin, r_pvs_radius->value, nv2a_visbytes, false, novis );
	if( force && !novis )
		gEngfuncs.R_FatPVS( test, r_pvs_radius->value, nv2a_visbytes, true, novis );

	memset( nv2a_map_face_visible, 0, (size_t)nv2a_map_face_count );

	for( i = 0; i < (uint32_t)world->numleafs; i++ )
	{
		const mleaf_t *vleaf;
		int j;

		if( !NV2A_CHECKVISBIT( nv2a_visbytes, i ))
			continue;

		/* leafs[0] is the shared CONTENTS_SOLID leaf and carries no
		   visdata bit of its own - bit i always names leafs[i+1], the
		   same off-by-one ref/gl's own R_MarkLeaves uses. */
		vleaf = &world->leafs[i + 1];
		for( j = 0; j < vleaf->nummarksurfaces; j++ )
		{
			/* Bounds-checked against world->surfaces/numsurfaces directly
			   (no bare ptrdiff_t subtraction trusted on its own) - same
			   "malformed/unexpected data fails cleanly, never an
			   out-of-range write" discipline this file already applies to
			   every archive-sourced index above. */
			const msurface_t *surf = vleaf->firstmarksurface[j];
			uint32_t idx;

			if( surf < world->surfaces || surf >= world->surfaces + world->numsurfaces )
				continue;

			idx = (uint32_t)( surf - world->surfaces );

			/* X9-K, the CRITICAL correctness fix this whole per-texture
			   batching change hinges on - see nv2a_map_face_remap's own
			   declaration comment for the full account. `idx` above is an
			   ORIGINAL BSP face index (world->surfaces comes from the
			   engine's own classic-lump-derived msurface_t array,
			   completely independent of and unaffected by this archive's
			   own per-texture sort) - it must be translated through the
			   archive's own originalToSortedFaceIndex[] table BEFORE it
			   can index nv2a_map_face_visible, which is now in SORTED
			   order. nv2a_map_face_remap is guaranteed non-NULL here (this
			   function's own top-of-function guard already returned
			   otherwise - see that guard's own comment for why bailing
			   THERE, before the memset below ever runs, is what keeps a
			   missing table fail-OPEN rather than culling every face).
			   Bounds-checked twice regardless, matching the reference
			   fix's own two-step shape exactly and this file's "never
			   trust an archive-sourced index blindly" discipline
			   everywhere else: first against nv2a_map_face_remap_count
			   (the ORIGINAL face count - the real bound for indexing INTO
			   nv2a_map_face_remap itself), then the translated result
			   against nv2a_map_face_count (the SORTED/total count - the
			   real bound for nv2a_map_face_visible). */
			if( idx < nv2a_map_face_remap_count )
			{
				uint32_t sortedIdx = nv2a_map_face_remap[idx];

				if( sortedIdx < nv2a_map_face_count )
					nv2a_map_face_visible[sortedIdx] = 1;
			}
		}
	}
}

/* X9-E: the real per-frame camera, replacing the fixed one-time debug
   camera nv2a_load_map_geometry() used to compute. Called by the engine
   (engine/client/cl_view.c's own V_RenderView, BEFORE R_EndFrame runs
   later in the same frame - confirmed by reading the real call order,
   not assumed) with the real, live vieworigin/viewangles/fov for a
   pass. NOT guaranteed once per frame - V_RenderView's own do/while
   loop can call this more than once (a real client spectator/PIP inset
   pass is a second, real example) - see nv2a_view_captured_this_frame's
   own declaration comment for why only the first real world pass each
   frame is used. */
static void GL_RenderFrame( const struct ref_viewpass_s *rvp )
{
	vec3_t forward, right, up;
	nv2a_vec3_t f, r, u, eye;
	float view[4][4], proj[4][4], ndc_to_screen[4][4], combined[4][4];
	float xscale, yscale;
	int i;

	if( !nv2a_ready )
		return;

	/* The menu's own playermodel preview (engine/client/dll_int/
	   cl_gameui.c's pfnRenderScene) also calls GL_RenderFrame, with an
	   unrelated tiny-preview camera, AFTER the real world pass but
	   BEFORE R_EndFrame - it would silently become the last writer of
	   nv2a_map_viewproj this frame if not filtered out here. It clears
	   RF_DRAW_WORLD on its own pass specifically so real world passes
	   can be told apart from it. */
	if( !FBitSet( rvp->flags, RF_DRAW_WORLD ))
		return;

	/* codex review, real finding: without this, a second real world
	   pass in the SAME frame (a spectator/PIP inset view, its own
	   distinct and much smaller rvp->viewport) would replace the
	   primary camera here and then get drawn FULL-SCREEN by R_EndFrame,
	   which has no per-viewport rendering at all - last-writer-wins,
	   backwards from what should be shown. Keep the first real world
	   pass each frame instead. */
	if( nv2a_view_captured_this_frame )
		return;
	nv2a_view_captured_this_frame = true;

	/* X9-G: recompute (or confirm unchanged) which of this map's faces
	   are PVS-visible from here, before this frame's own draw call reads
	   nv2a_map_face_visible. No-ops cheaply when the camera has not
	   crossed a leaf boundary - see nv2a_mark_leaves()'s own early
	   return. */
	nv2a_mark_leaves( rvp->vieworigin );

	/* X9-H: the real eye position nv2a_draw_map_geometry()'s own
	   per-face backface test reads later this same frame (from
	   R_EndFrame) - captured here since that function has no
	   ref_viewpass_t of its own. */
	VectorCopy( rvp->vieworigin, nv2a_eye_origin );

	AngleVectors( (float *)rvp->viewangles, forward, right, up );
	VectorCopy( forward, nv2a_eye_forward );
	f.x = forward[0]; f.y = forward[1]; f.z = forward[2];
	r.x = right[0];   r.y = right[1];   r.z = right[2];
	u.x = up[0];      u.y = up[1];      u.z = up[2];
	eye.x = rvp->vieworigin[0]; eye.y = rvp->vieworigin[1]; eye.z = rvp->vieworigin[2];

	/* Deliberately using the real `right` axis AngleVectors() gives,
	   NOT its negation: this is the geometrically correct choice (world-
	   right maps to positive screen-X, given nv2a_matrix_viewport()'s
	   own already-proven Y-flip), and matches every other real ref
	   module's own convention (ref/soft/r_main.c's AngleVectors() call).
	   The OLD fixed debug camera's own nv2a_matrix_lookat() derived its
	   own col0 axis as cross(up,zaxis), which for the same orientation
	   is the LITERAL NEGATION of AngleVectors()'s own `right` - an
	   arbitrary choice nobody had ever visually checked, since a fixed
	   debug camera never needed to agree with a real player's own sense
	   of left/right. This is a real, deliberate correction: X9-E's own
	   first live screenshot is expected to look horizontally mirrored
	   relative to every X9-A through D screenshot, not a regression. */
	nv2a_matrix_view_from_axes( view, eye, f, r, u );

	/* xScale and yScale are each taken directly from their own real,
	   live fov_x/fov_y (both in degrees - confirmed: engine/client/
	   cl_view.c's own V_CalcFov returns RAD2DEG(...)) - not derived from
	   one another via the viewport's aspect ratio. See
	   nv2a_matrix_perspective's own declaration comment (X9-F, codex
	   review) for why deriving xScale from fov_y+aspect used to be an
	   unstated assumption rather than a real invariant. */
	xscale = 1.0f / tanf( (float)( DEG2RAD( rvp->fov_x ) * 0.5 ));
	yscale = 1.0f / tanf( (float)( DEG2RAD( rvp->fov_y ) * 0.5 ));
	nv2a_matrix_perspective( proj, xscale, yscale, 1.0f, 8192.0f );
	nv2a_matrix_viewport( ndc_to_screen, 0, 0, (float)nv2a_width, (float)nv2a_height, 0, 65536.0f );

	nv2a_matrix_multiply( combined, view, proj );
	nv2a_matrix_multiply( nv2a_map_viewproj, combined, ndc_to_screen );

	/* X9-F: the view frustum, from the SAME forward/right/up/eye the view
	   matrix above was just built from - so the cull and the projection
	   can never disagree about handedness or FOV the way two
	   independently-derived cameras could. Port of ref/gl/gl_frustum.c's
	   own GL_FrustumInitProj: inward-pointing normals, dist = eye . normal
	   (the FAR plane excepted - its own point is out along forward, not
	   the eye itself), no near plane (the reference passes flZNear=0.0f
	   from R_SetupFrustum and returns before setting one). Far is THIS
	   module's own literal 8192.0f, matching the nv2a_matrix_perspective()
	   call just above - not the reference's gp_movevars->zmax*1.73f, which
	   would let the cull disagree with what this projection actually
	   clips.

	   This uses rvp->fov_x/fov_y directly, same as the projection above
	   now does (X9-F, codex review) - so LEFT/RIGHT here and the
	   projection's own xScale always describe the exact same horizontal
	   angle, with no aspect-ratio-derived assumption between them, even
	   if fov_x and fov_y were ever independently perturbed (as
	   V_ApplyRefUnderwater, engine/client/cl_view.c, really does - by
	   different, non-matching factors on each - but only when
	   RF_DRAW_CUBEMAP or RF_DRAW_OVERVIEW is set on the pass; verified by
	   reading V_RenderView's own call order that neither bit is ever set
	   on the ordinary single-player world pass this filters down to
	   above: RF_DRAW_CUBEMAP is set only inside ref/gl's/ref/soft's own
	   internal env-map capture, a pass this module does not implement,
	   and RF_DRAW_OVERVIEW requires CL_IsDevOverviewMode(), itself gated
	   on dev_overview>0 AND (host_developer or spectator) - a developer/
	   spectator-only mode, unreachable in this fork's own real, single-
	   player-only, Xbox-networking-disabled configuration today). */
	{
		float xs, xc, ys, yc;
		vec3_t normal, farpoint;

		SinCos( DEG2RAD( rvp->fov_x ) * 0.5f, &xs, &xc );
		VectorMAM( xs, forward, -xc, right, normal );
		VectorCopy( normal, nv2a_frustum[NV2A_FRUSTUM_LEFT].normal );
		nv2a_frustum[NV2A_FRUSTUM_LEFT].dist = DotProduct( rvp->vieworigin, normal );

		VectorMAM( xs, forward, xc, right, normal );
		VectorCopy( normal, nv2a_frustum[NV2A_FRUSTUM_RIGHT].normal );
		nv2a_frustum[NV2A_FRUSTUM_RIGHT].dist = DotProduct( rvp->vieworigin, normal );

		SinCos( DEG2RAD( rvp->fov_y ) * 0.5f, &ys, &yc );
		VectorMAM( ys, forward, -yc, up, normal );
		VectorCopy( normal, nv2a_frustum[NV2A_FRUSTUM_BOTTOM].normal );
		nv2a_frustum[NV2A_FRUSTUM_BOTTOM].dist = DotProduct( rvp->vieworigin, normal );

		VectorMAM( ys, forward, yc, up, normal );
		VectorCopy( normal, nv2a_frustum[NV2A_FRUSTUM_TOP].normal );
		nv2a_frustum[NV2A_FRUSTUM_TOP].dist = DotProduct( rvp->vieworigin, normal );

		VectorNegate( forward, normal );
		VectorMA( rvp->vieworigin, 8192.0f, forward, farpoint );
		VectorCopy( normal, nv2a_frustum[NV2A_FRUSTUM_FAR].normal );
		nv2a_frustum[NV2A_FRUSTUM_FAR].dist = DotProduct( normal, farpoint );

		for( i = 0; i < 5; i++ )
		{
			nv2a_frustum[i].type = PlaneTypeForNormal( nv2a_frustum[i].normal );
			nv2a_frustum[i].signbits = SignbitsForPlane( nv2a_frustum[i].normal );
		}
	}

	nv2a_view_valid = true;
}

static void GL_OrthoBounds( const float *mins, const float *maxs )
{
	;
}

static qboolean R_SpeedsMessage( char *out, size_t size )
{
	return false;
}

static byte *Mod_GetCurrentVis( void )
{
	return NULL;
}

static void Color4f( float r, float g, float b, float a )
{
	;
}

static void Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	;
}

static void Vertex3fv( const float *worldPnt )
{
	;
}

static void Vertex3f( float x, float y, float z )
{
	;
}

static void CullFace( TRICULLSTYLE mode )
{
	;
}

static void VGUI_SetupDrawing( qboolean rect )
{
	;
}

/* The one real per-frame entry point: draws and presents the fixed test
   triangle every frame, at the same point ref/soft's own R_EndFrame does
   its real work (R_BlitScreen) - see this file's header comment for why
   R_BeginFrame/R_RenderScene stay no-ops for this bring-up. */
static void R_EndFrame( void )
{
	if( !nv2a_ready )
		return;

	debug_frame_count++;

	/* X9-E, codex review: R_EndFrame runs LAST in a frame, after every
	   GL_RenderFrame call this same frame was ever going to get - so
	   resetting nv2a_view_captured_this_frame here re-arms it for the
	   NEXT frame's own first real world pass, without affecting anything
	   about the frame that just finished. */
	nv2a_view_captured_this_frame = false;

	pb_wait_for_vbl();
	pb_reset();
	pb_target_back_buffer();

	pb_erase_depth_stencil_buffer( 0, 0, nv2a_width, nv2a_height );
	pb_fill( 0, 0, nv2a_width, nv2a_height, 0x00000000 );
	pb_erase_text_screen();

	while( pb_busy())
		;

	/* X9-E: nv2a_map_viewproj is now written by GL_RenderFrame's own
	   real per-frame camera, not computed once at map-load time - a map
	   loaded before the engine has ever called GL_RenderFrame with a
	   real world pass (loading/plaque frames) must not draw with
	   whatever that matrix defaults to (all-zero BSS - a divide by zero
	   in the vertex shader's own w-divide), so this falls back to the
	   already-proven test triangle until nv2a_view_valid is real. */
	if( nv2a_map_ready && nv2a_view_valid )
		nv2a_draw_map_geometry();
	else
		nv2a_draw_test_triangle();

	while( pb_busy())
		;

	while( pb_finished())
		;
}

const ref_interface_t gReffuncs =
{
	.R_Init                = R_Init,
	.R_Shutdown            = R_Shutdown,
	.R_GetConfigName       = R_GetConfigName,
	.R_SetDisplayTransform = R_SetDisplayTransform,

	.GL_SetupAttributes = GL_SetupAttributes,
	.GL_InitExtensions  = R_SimpleStub,
	.GL_ClearExtensions = R_SimpleStub,

	.R_GammaChanged       = R_SimpleStubBool,
	.R_BeginFrame         = R_SimpleStubBool,
	.R_RenderScene        = R_SimpleStub,
	.R_EndFrame           = R_EndFrame,
	.R_PushScene          = R_SimpleStub,
	.R_PopScene           = R_SimpleStub,
	.GL_BackendStartFrame = R_SimpleStub,
	.GL_BackendEndFrame   = R_SimpleStub,

	.R_ClearScreen    = R_SimpleStub,
	.R_AllowFog       = R_SimpleStubBool,
	.GL_SetRenderMode = R_SimpleStubInt,

	.R_AddEntity      = R_AddEntity,
	.R_ProcessEntData = R_ProcessEntData,

	.R_ShowTextures = R_SimpleStub,

	.R_GetTextureOriginalBuffer = R_GetTextureOriginalBuffer,
	.GL_LoadTextureFromBuffer   = GL_LoadTextureFromBuffer,
	.GL_ProcessTexture          = GL_ProcessTexture,
	.R_SetupSky                 = R_SetupSky,

	.R_Set2DMode      = R_SimpleStubBool,
	.R_DrawStretchPic = R_DrawStretchPic,
	.FillRGBA         = FillRGBA,
	.WorldToScreen    = WorldToScreen,

	.VID_ScreenShot  = VID_ScreenShot,
	.VID_CubemapShot = VID_CubemapShot,

	/* ref/common's own real implementations (ref_light.c, declared in
	   ref_common.h) - NOT local stubs. A local static stub with either
	   name here was a real, isolated duplicate-symbol link error: this
	   module compiles ref/common alongside itself (xbox-ref-isolate.sh),
	   and llvm-objcopy --redefine-syms renames both the local stub and
	   ref/common's real function to the same _nv2a_-prefixed name. */
	.R_LightPoint = R_LightPoint,

	.R_DecalShoot      = R_DecalShoot,
	.R_DecalRemoveAll  = R_SimpleStubInt,
	.R_CreateDecalList = R_CreateDecalList,
	.R_ClearAllDecals  = R_SimpleStub,

	.R_StudioEstimateFrame = R_StudioEstimateFrame,
	.R_StudioLerpMovement  = R_StudioLerpMovement,
	.R_StudioFillAPI          = R_StudioFillAPI,
	.R_StudioSetDrawInterface = R_StudioSetDrawInterface,

	.R_SetSkyCloudsTextures     = R_SetSkyCloudsTextures,
	.GL_SubdivideSurface = GL_SubdivideSurface,
	.CL_RunLightStyles   = CL_RunLightStyles,

	.Mod_ProcessRenderData  = Mod_ProcessRenderData,
	.Mod_StudioLoadTextures = Mod_StudioLoadTextures,

	.CL_DrawParticles = CL_DrawParticles,
	.CL_DrawTracers   = CL_DrawTracers,
	.CL_DrawBeams     = CL_DrawBeams,

	.RefGetParm = RefGetParm,

	.R_GetDetailScaleForTexture = R_GetDetailScaleForTexture,
	.R_SetDetailScaleForTexture = R_SetDetailScaleForTexture,

	.GL_CreateTexture = GL_CreateTexture,
	.GL_FindTexture  = GL_FindTexture,
	.GL_TextureName  = GL_TextureName,
	.GL_TextureData  = GL_TextureData,
	.GL_LoadTexture  = GL_LoadTexture,
	.GL_FreeTexture  = R_SimpleStubUInt,
	.R_OverrideTextureSourceSize = R_OverrideTextureSourceSize,

	.GL_UpdateTexture = GL_UpdateTexture,

	.GL_Bind = GL_Bind,

	.GL_RenderFrame    = GL_RenderFrame,
	.GL_OrthoBounds    = GL_OrthoBounds,
	.R_SpeedsMessage   = R_SpeedsMessage,
	.Mod_GetCurrentVis = Mod_GetCurrentVis,
	.R_NewMap          = R_SimpleStub,
	.R_ClearScene      = R_ClearScene,

	.TriRenderMode = R_SimpleStubInt,
	.Begin         = R_SimpleStubInt,
	.End           = R_SimpleStub,
	.Color4f       = Color4f,
	.Color4ub      = Color4ub,
	.Vertex3fv     = Vertex3fv,
	.Vertex3f      = Vertex3f,
	.CullFace      = CullFace,

	.R_FillRenderAPI = R_FillRenderAPI,
	.R_FillTriAPI    = R_FillTriAPI,

	.VGUI_SetupDrawing   = VGUI_SetupDrawing,
};
