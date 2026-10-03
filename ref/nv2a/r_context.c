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
#include <stdlib.h> /* malloc/free for load temporaries - see nv2a_read_map_lumps */
#include <string.h>
#include "xash3d_types.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR - not pulled in transitively here, unlike ref/soft/ref/gl's own r_local.h/gl_local.h */
#include "protocol.h" /* PROTOCOL_VERSION, for the no-world backdrop's version string (ref/gl's gl_local.h includes it too) */
#include "entity_types.h" /* X9-M: ET_BEAM, for R_AddEntity */
#include "cvardef.h"
#include "pmove.h" /* X9-X step 4 Part E: the real movevars_s definition (gp_movevars->zmax) - ref_common.h only forward-declares it */
#include "ref_common.h"
#include "nv2a_texture.h" /* X9-X: the texture manager's own table/entry points (r_image.c) */
#include "nv2a_2d_util.h" /* X9-X: the 2D pass's pure-arithmetic pieces (nv2a_2d_util.c), host-tested */
#include "nv2a_tri_util.h" /* X9-X step 4: the TriAPI's primitive assembler (nv2a_tri_util.c), host-tested */
#include "nv2a_studio_util.h" /* X9-X step 5 Part C: the studio batch-state mapping (nv2a_studio_util.c), host-tested */
#include "r_studio_bridge.h" /* X9-X step 5: the private glue with r_studio.c - nv2a_2d_shadow_t now lives here */
#include "r_studio.h" /* X9-X step 5: r_studio.c's own entry points */
#include "r_studio_gpu.h" /* divergences.md #143: the GPU studio path's draws, below */
#include "nv2a_sprite_util.h" /* X9-X step 6: the sprite GL-state mirrors pinned below */
#include "r_sprite.h" /* X9-X step 6: r_sprite.c's own entry points (gl_sprite.c port) */
#include "r_effects.h" /* X9-X step 7: r_effects.c's own entry points (gl_beams.c/gl_rpart.c port) */
#include "nv2a_efx_util.h" /* X9-X step 7: the TriAPI texture-repeat splitter */
#include "nv2a_lightmap_util.h" /* X9-X step 8: NV2A_LM_MAX_LUXELS_PER_AXIS */
#include "r_lightmap.h" /* X9-X step 8: lightstyles and dynamic lights (R_CheckLightMap/R_BuildLightMap port) */
#include "nv2a_texanim_util.h" /* X9-X item 14 step 9: R_TextureAnimation in texture-index space, host-tested */
#include "r_sky.h" /* X9-X item 14 step 10: the skybox (gl_warp.c port) */
#include "r_warp.h" /* X9-X item 14 step 11: water warp and fog (gl_rsurf.c/gl_rmain.c port) */
#include "nv2a_warp_util.h" /* X9-X item 14 step 11: the fog register math */
#include "r_decals.h" /* X9-X item 14 step 12: decals (gl_decals.c port) */
#include "ref_params.h" /* X9-E: ref_viewpass_t's own real field layout and
                            RF_DRAW_WORLD - only forward-declared via
                            ref_api.h otherwise. ref/soft's own r_local.h
                            includes this same header for the same reason. */
#include "mpak_layout.h" /* filesystem/mpak_layout.h - types/constants only, see
                            nv2a_compute_mpak_layout()'s own comment for why the
                            offset-COMPUTING function body still needs its own
                            copy here rather than calling filesystem's real
                            Mpak_ComputeLayout() directly. Pulls in
                            mpak_texture_dir.h too (the v10 texture-directory
                            validation RULE, unlike the layout/offset math
                            above, IS shared code - see that header's own
                            comment for why it, unlike Mpak_ComputeLayout, is
                            safe to actually call from here: it is header-only,
                            never linked, so #include-ing it costs nothing this
                            module's isolation (tools/xbox-ref-isolate.sh)
                            would object to). */

#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include "platform/xbox/xbox_frametime.h" /* X9-X item 14 step 13: the frame-time recorder's wait counters (divergences.md #141) */
#include "nv2a_ft.h" /* ... and its work counters (format v2) */

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
Render state
========================================================================
*/

/* X9-C: the real map-geometry vertex format: position (attribute slot 0),
   diffuse UV (slot 9) and lightmap UV (slot 10, ref/nv2a/vs_lit.vs.cg's own
   TEXCOORD1 - confirmed by a real vp20compiler run to map there, not
   assumed). */
typedef struct
{
	float pos[3];
	float uv0[2];
	float uv1[2];
} __attribute__(( packed )) nv2a_lit_vertex_t;

#define NV2A_TEST_MAXRAM 0x03FFAFFF

#define MASK( mask, val ) ( ( ( val ) << ( __builtin_ffs( mask ) - 1 ) ) & ( mask ) )

static qboolean  nv2a_ready;
static int       nv2a_width, nv2a_height;

/* X9-B/C drew one hard-coded real face from a real baked map, instead of
   the fixed bring-up test triangle (since removed), once a map's own .mpak
   data was loaded. X9-D
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
static uint32_t *nv2a_map_vb;                       /* GPU-visible, nv2a_lit_vertex_t[nv2a_map_vert_count] */
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
static uint32_t *nv2a_map_index_dwords;             /* Ordinary CPU memory (malloc/free since X9-U;
                                                        r_temppool before) - unlike nv2a_map_vb this
                                                        is only ever CPU-read into the pushbuffer, never
                                                        touched by the GPU directly, so it has no reason
                                                        to cost scarce contiguous write-combined memory
                                                        (codex review, real finding). Packed pairs,
                                                        sized to the real map's own iCount, not a fixed
                                                        bring-up cap. */
static uint32_t  nv2a_map_dword_count;

/* X9-T: the current map's renderer data, still to be built. X9-U: all of
   it - the tables and the three contiguous GPU buffers - is built at
   R_NewMap, from the archive file, after the engine has freed its copy of
   the map (engine/common/model.c's Mod_LoadModel frees it right after
   Mod_ProcessRenderData). Mod_ProcessRenderData only validates the header
   and keeps it, with the first sector for nv2a_load_map_geometry's
   identity check. Measured in xemu (temporary probe): pool_party had 4.0
   MiB free with its 15.5 MiB archive held, and ~19.5 MiB once it is
   freed; on rocket_frenzy building the tables beside the engine's 8.7 MiB
   prefix alone did not fit.

   R_NewMap runs after the map's precaches but before the client frees the
   previous map's unused models (Mod_FreeUnused, cl_parse.c), so a failed
   upload keeps the tables and is retried once, from the next R_EndFrame
   (nv2a-xbox-specialist, X9-T review). */
static struct
{
	qboolean    pending;
	qboolean    retried;       /* the one retry after a failed R_NewMap upload was spent */
	qboolean    tables_built;  /* X9-U: nv2a_load_map_geometry has run for this map */
	mpak_header_t header;      /* X9-U: the descriptor Mod_ProcessRenderData validated */
	byte        first_sector[MPAK_SECTOR_SIZE]; /* X9-U: the engine's first sector, for the identity check */
	char        name[64];      /* "maps/<name>.mpak" - the whole archive (MPAK v12) */
	fs_offset_t file_length;   /* its length when the tables were read; checked again at reopen */
	fs_offset_t vertices_offset;
	uint32_t    vertex_count;
	uint64_t    vb_bytes;
	fs_offset_t texture_offset;
	fs_offset_t texture_bytes;
	fs_offset_t lightmap_offset;
	fs_offset_t lightmap_bytes;
	uint32_t    lightmap_width, lightmap_height;
} nv2a_pending_upload;

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

/* X9-N, MPAK v10: byte length of a full DXT1 mip chain for a POT texture -
   Mpak_Dxt1ChainLength (filesystem/mpak_texture_dir.h), shared with the PC
   tests and the baker's verify pass rather than this module's own copy
   (fork-plan.md's X9 item 2 follow-up; see that header's own file comment
   for why sharing it this way, not by linking filesystem/mpak_layout.c, is
   what this module's ref/filesystem isolation allows). */
/* Fixed-capacity static arrays, not a heap allocation sized off the
   header's own (validated) textureCount/batchCount the way nv2a_map_faces
   is sized off face_count - deliberately, matching this file's own
   established discipline for a bound this function cannot yet size
   exactly (the atlasWidth > 4096 check has the same shape: a hard cap,
   checked, not derived). 512 is a first, reasoned guess (retail HL1 maps
   commonly cite low hundreds of unique textures at most, and
   batchCount was == textureCount architecturally - MpakHeader.h's own
   v9 note - until X9-S split batches at vertex-chunk boundaries too, so
   a multi-chunk map can have a few more batches than textures), not a
   measured bound: the Gate phase re-bakes the real 125-map
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
                                     NV2A_MAP_MAX_TEXTURES above - batchCount ~= textureCount
                                     (a few more on a multi-chunk map, X9-S), so this shares
                                     that cap's own reasoning; the baker enforces it. */
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

/* X9-X step 8: each SORTED face's style-0 block in the lightmap atlas and
   its size, from MpakFaceInfo (lightmapBlockX/Y, extents / 16 + 1) - what
   r_lightmap.c needs to rebuild a face's lightmap in place. NULL = no
   table (allocation failed, or no usable faceInfo lump): the atlas then
   stays exactly as baked. */
static nv2a_lm_block_t *nv2a_map_face_lm;

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
/* X9-X step 5 Part C: view*proj WITHOUT the viewport (GL_RenderFrame's own
   `combined`, persisted) - the view-model depth-range matrix
   (nv2a_2d_flush) re-combines this with a viewport whose z range is
   [0, 0.3*65536] instead of the world's own [0, 65536], rather than
   rebuilding the camera from scratch. */
static float     nv2a_view_proj_combined[4][4];
/* X9-X step 4 Part E: the SAME camera as nv2a_map_viewproj above, but built
   with ref/common's own Matrix4x4_* helpers (M*v, column-vector convention)
   instead of nv2a_matrix_multiply()'s row-vector one - GetMatrix/
   WorldToScreen/ScreenToWorld port ref/gl's own functions verbatim
   (gl_rmain.c:137-186,369-425), which read exactly this representation.
   Filled in GL_RenderFrame, reset to identity in R_Init only - same reset
   discipline as nv2a_view_valid below, which still gates every use of all
   three (GL_RenderFrame runs before any of GetMatrix/WorldToScreen/
   ScreenToWorld can be called with real data). Do NOT derive these by
   transposing nv2a_map_viewproj - it is a different convention (row-vector
   vs column-vector), not one matrix read two ways.
   The projection's zNear/zFar are now the SAME 1/8192 nv2a_map_viewproj
   itself renders with, not ref/gl's own R_GetFarClip-derived values - see
   this matrix's own construction, below, for why (codex review, real
   finding: CFrustum::CalculateFrustum genuinely reads GetMatrix's near/far
   planes for particle culling, divergences.md #119 point 2). */
static matrix4x4 nv2a_gl_worldview;
static matrix4x4 nv2a_gl_projection;
static matrix4x4 nv2a_gl_worldviewproj;

/* Tentative (no-initializer) forward declarations - the real ones, each
   WITH its initializer, are Part C's own (the 2D/TriAPI immediate-mode
   pass, defined well below GL_RenderFrame in this file); a tentative
   declaration and its later real definition name the same object in C.
   Needed here only because GL_RenderFrame (X9-X step 4 Part F) now
   flushes/dirties/resets this pass's own state right after the world
   draw - defined above Part C itself. */
static qboolean nv2a_2d_dirty;
static byte     nv2a_2d_color[4];
static qboolean nv2a_in2d;
static void     nv2a_2d_flush( void );
static void     nv2a_2d_reset_shadow_for_3d( void );

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
/* X9-X: the frame R_BeginFrame opened (cleared, pushbuffer reset) and
   R_EndFrame presents; whether GL_RenderFrame drew the world into it; and
   whether the no-world backdrop (nv2a_draw_no_world_backdrop) is still
   owed to this frame. */
static qboolean  nv2a_frame_open;
static qboolean  nv2a_world_drawn_this_frame;
static qboolean  nv2a_no_world_backdrop_pending;
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
	/* X9-U: 24 bytes, was 52 - 41,000 faces on rocket_frenzy. The bounds
	   are the face's own, rounded outward to whole units (GoldSrc
	   coordinates fit an int16), so culling stays conservative; the
	   dword range the table used to carry was never read. */
	int16_t  mins[3], maxs[3];
	uint32_t index_begin;            /* X9-M: this face's EXACT logical
	                                    index range [begin, begin + count) -
	                                    exact, not snapped to dwords, so a
	                                    run never takes in a neighbouring
	                                    brush-entity face's triangles (see
	                                    nv2a_draw_exact_indices). */
	uint16_t index_count;
	uint16_t texture;                /* X9-M: textureIndex of the batch
	                                    this sorted face belongs to, for
	                                    the brush-entity pass, which draws
	                                    one entity's faces across batches;
	                                    NV2A_FACE_NO_TEXTURE if no valid
	                                    batch covers it. */
	uint8_t  chunk;                  /* X9-S, MPAK v11: this face's vertex
	                                    chunk, an index into
	                                    nv2a_map_chunk_base[] - its u16
	                                    indices are relative to that base, so
	                                    the vertex attribute arrays must
	                                    point there before it draws
	                                    (nv2a_bind_vertex_base). */
	uint8_t  sky; /* X9-K: MpakFaceInfo::flags & MPAK_SURF_DRAWSKY for this
	                 face, read once at load - ref/nv2a has never implemented
	                 a real skybox, so a sky face is excluded from every draw
	                 run regardless of frustum/PVS visibility, matching the
	                 reference's own minimum behaviour (never draw the sky's
	                 literal placeholder texture as normal geometry). */
	uint8_t  turb; /* X9-X step 11: MpakFaceInfo::flags & MPAK_SURF_DRAWTURB -
	                  a water face, drawn warped through r_warp.c instead of
	                  from the baked batch (fits the struct's padding: still
	                  24 bytes). */
} nv2a_map_face_t;

#define NV2A_FACE_NO_TEXTURE 0xFFFFu
STATIC_ASSERT( NV2A_MAP_MAX_TEXTURES < NV2A_FACE_NO_TEXTURE, "a texture index must fit nv2a_map_face_t::texture below the sentinel" );

/* X9-U: the distinct vertex-chunk bases of the current map, in the order
   faces first use them; nv2a_map_face_t::chunk indexes this. A retail map
   has at most three chunks (X9-S). */
#define NV2A_MAP_MAX_CHUNKS 16
static uint32_t nv2a_map_chunk_base[NV2A_MAP_MAX_CHUNKS];
static uint32_t nv2a_map_chunk_count;

static inline uint32_t nv2a_face_index_end( const nv2a_map_face_t *face )
{
	return face->index_begin + face->index_count;
}

static inline uint32_t nv2a_face_vertex_base( const nv2a_map_face_t *face )
{
	return nv2a_map_chunk_base[face->chunk];
}

/* nv2a_frustum_cull_box takes float bounds. */
qboolean nv2a_frustum_cull_box( const vec3_t mins, const vec3_t maxs ); /* X9-X step 5: exposed for r_studio.c (R_CullModel), r_studio_bridge.h */

/* R3 (divergences.md #146): one frustum plane as the world walk tests
   it. GL_FrustumCullBox's signbits switch (gl_frustum.c:119-156, ported
   in nv2a_frustum_cull_box) picks, per axis, mins when the normal's
   component is negative and maxs otherwise; here that choice is made
   once per frame as an index into the face's six bounds (0-2 mins, 3-5
   maxs), so the per-face test has no indirect jump - the switch inside
   the five-plane loop was one jump site with five targets in turn, a
   misprediction per plane on the Pentium III. The sum is the switch
   case's own, term for term and in the same order, so every result is
   the same float compare. */
typedef struct
{
	float   normal[3];
	float   dist;
	uint8_t corner[3];
} nv2a_cull_plane_t;

STATIC_ASSERT( offsetof( nv2a_map_face_t, maxs ) == offsetof( nv2a_map_face_t, mins ) + 3 * sizeof( int16_t ), "nv2a_frustum_face_outside reads mins and maxs as one array of six" );

/* True when the face's box is entirely outside one of the first `count`
   planes - nv2a_frustum_cull_box's answer for the same box with nocull
   off (nv2a_frustum_prepare_planes gives the count). */
static inline qboolean nv2a_frustum_face_outside( const nv2a_cull_plane_t *planes, uint32_t count, const nv2a_map_face_t *face )
{
	int16_t ib[6];
	float b[6];
	uint32_t i;

	memcpy( ib, (const byte *)face + offsetof( nv2a_map_face_t, mins ), sizeof( ib ));
	for( i = 0; i < 6; i++ )
		b[i] = ib[i];

	for( i = 0; i < count; i++ )
	{
		const nv2a_cull_plane_t *p = &planes[i];

		if( p->normal[0] * b[p->corner[0]] + p->normal[1] * b[p->corner[1]] + p->normal[2] * b[p->corner[2]] < p->dist )
			return true;
	}
	return false;
}

static inline qboolean nv2a_frustum_cull_face( const nv2a_map_face_t *face )
{
	const vec3_t mins = { face->mins[0], face->mins[1], face->mins[2] };
	const vec3_t maxs = { face->maxs[0], face->maxs[1], face->maxs[2] };

	return nv2a_frustum_cull_box( mins, maxs );
}

/* Nv2aMapBudget.h's kNv2aMapFaceBytes mirrors this size for the bake-time
   budget; change both together. */
STATIC_ASSERT( sizeof( nv2a_map_face_t ) == 24, "nv2a_map_face_t size changed - update kNv2aMapFaceBytes in tools/asset-studio/src/formats/Nv2aMapBudget.h" );

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

/* R3 (divergences.md #146): the world faces in the PVS, so the world
   walk visits those instead of every face of the map each frame. ref/gl
   never descends into a node R_MarkLeaves did not stamp
   (R_RecursiveWorldNode, gl_rsurf.c:3463; the stamping loop is
   gl_rsurf.c:3788-3801), and rejects a surface of a stamped node that no
   visible leaf marked with one compare (CULL_VISFRAME, gl_cull.c:70-71);
   its node boxes also frustum-cull whole subtrees (clipflags,
   gl_rsurf.c:3465-3477), which this walk still does per face. Rebuilt by
   nv2a_mark_leaves every time it rewrites nv2a_map_face_visible, so the
   two always agree: the sorted indices i, ascending, with
   nv2a_map_face_visible[i] set, not sky (nv2a_map_face_t::sky) and a
   world face (nv2a_map_face_is_world) - the three per-face tests the
   walk made before the frustum. The last two never change within a map,
   so nv2a_map_face_listable holds them, one byte per face, and a rebuild
   reads two byte arrays. Batches are a contiguous partition of
   the faces in order (validated at load), so batch b's faces are the
   slice [b ? end[b-1] : 0, end[b]). nv2a_pvs_faces_world is the world
   the list was built for; NULL, or nv2a_r_novis, sends the walk over
   every face as before. uint16_t indices: a map with more than 65,536
   faces gets no list (BSP30's own limit is 65,535). */
static uint16_t      *nv2a_pvs_faces;
static byte          *nv2a_map_face_listable; /* !sky && world face; filled with nv2a_map_face_plane */
static uint32_t       nv2a_pvs_batch_end[NV2A_MAP_MAX_BATCHES];
static const model_t *nv2a_pvs_faces_world;

/* R3 (#146): each sorted face's plane for the back-face test - an index
   into world->planes with NV2A_FACE_PLANE_BACK for SURF_PLANEBACK, or
   NV2A_FACE_PLANE_NONE where the walk makes no test (no surface, or a
   null normal: R_CullSurface's VectorIsNull, gl_cull.c:83). Read from
   the engine's msurface_t once per map (nv2a_fill_face_planes), because
   following surface -> plane for every visible face every frame is two
   dependent loads into large, unordered arrays. A surface's plane and
   SURF_PLANEBACK are set once, by Mod_LoadSurfaces (mod_bmodel.c:3323-
   3344 upstream, 3403-3424 in this tree), and never change. NV2A_FACE_PLANE_SURF: read the surface at
   draw time, as before (a plane outside world->planes, or an index past
   NV2A_FACE_PLANE_INDEX_MAX). Valid for nv2a_map_face_plane_world. */
#define NV2A_FACE_PLANE_BACK      0x8000u
#define NV2A_FACE_PLANE_INDEX_MAX 0x7FFDu
#define NV2A_FACE_PLANE_SURF      0xFFFEu
#define NV2A_FACE_PLANE_NONE      0xFFFFu
static uint16_t      *nv2a_map_face_plane;
static const model_t *nv2a_map_face_plane_world;

/* The three tables' bytes per face, for nv2a_load_map_geometry's ledger
   and Nv2aMapBudget.h's kNv2aPvsListFaceBytes. */
#define NV2A_PVS_LIST_FACE_BYTES ( 2 * sizeof( uint16_t ) + 1 )

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

/* X9-X step 7: nv2a_visbytes holds a real PVS row (R_FatPVS has filled it
   for the current map). Until then Mod_GetCurrentVis returns NULL, which
   Mod_BoxVisible treats as all-visible (mod_bmodel.c:1313) - fail open,
   as the rest of this file's culling does. */
static qboolean nv2a_visbytes_valid;

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

/* X9-N2: the two knobs ref/gl has for how soon mipmapped textures blur -
   gl_anisotropy (default "8") and gl_texture_lodbias (default "0.0"),
   ref/gl/gl_opengl.c:9-10. Without anisotropy a floor seen at a glancing
   angle drops to a smaller mip level within a few units, which real
   hardware showed as a visible resolution change just ahead of the
   player. NV2A's per-stage anisotropy is CONTROL0's 2-bit ANISOTROPY
   field (nv_regs.h:595, log2 of the ratio: 0 = 1x ... 3 = 8x). The
   default is 2x, the only level proven on real hardware (Perfect Dark X's
   gfx_nv2a.cpp sets 0x10 on mipmapped textures and caps at 2x); 4 and 8
   are selectable to test. The LOD bias goes in FILTER bits 0-12 as signed
   fixed point with 8 fraction bits, clamped to +-3. */
static CVAR_DEFINE_AUTO( nv2a_r_anisotropy, "2", 0, "ref_nv2a: anisotropic filtering for mipmapped textures: 1, 2, 4 or 8 (2 is hardware-proven)" );
static CVAR_DEFINE_AUTO( nv2a_r_lodbias, "0", 0, "ref_nv2a: mip LOD bias, -3..3 (negative = sharper, later switch to smaller levels)" );

/* X9-X, C5: the 2D viewport matrix's own M[3][0]/M[3][1] - unverified
   live (design doc's own note). 0 until a real Xbox run judges the
   TF_NEAREST console font against -0.5 (the classic "sample texel
   centres" half-pixel offset some GPUs need for a point-filtered
   blit) - see docs/r3d/x9-x-textures-and-2d.md's verification item 9. */
static CVAR_DEFINE_AUTO( nv2a_r_2d_pixeloffset, "0", 0, "ref_nv2a: 2D viewport pixel-centre offset, unverified live" );

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
	mpak_lump_t faceVertexBase; /* X9-S, MPAK v11: uint32_t per sorted face, its vertex-chunk
	                               base; length 0 on a single-chunk map (every base 0) */
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

	/* MPAK v12 (filesystem/mpak_layout.c): every classic BSP lump first -
	   the prefix filesystem/mpak.c serves the engine as the map's .bsp -
	   then every renderer-only section, which is all this module reads. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenVisibility, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenNodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLeaves, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenMarksurfaces, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenPlanes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenClipnodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenModels, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenEntities, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenTexinfo, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenVertexes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenEdges, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenSurfedges, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaces, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLighting, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenMiptex, &skip )) return false;

	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->vCount * sizeof( mpak_vertex_t ), &out->vertices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->iCount * sizeof( uint16_t ), &out->indices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceDrawCmds, &out->faceDrawCmds )) return false;

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
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLightmapAtlas, &out->lightmapAtlas )) return false;
	/* X9-K sky-skip fix: only MpakFaceInfo::flags is read. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceInfo, &out->faceInfo )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenExtraLightBlocks, &skip )) return false;
	/* X9-S (MPAK v11): the per-face vertex-chunk bases, the last lump this
	   mirror reads. The v13 PHS sections follow it; they are the engine's
	   (Mod_ReadBakedPHS), and this walk has no end-of-file check to trip. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceVertexBase, &out->faceVertexBase )) return false;

	return true;
}

/* X9-X: the NV2A holds one resident vertex/pixel program at a time -
   every function that loads a different one clears this so the 2D/TriAPI
   pass's own nv2a_init_2d_shader()/nv2a_init_tri_shader() (defined with
   the rest of Part F, below) knows to re-upload before its next flush.
   One enum rather than a bool per program (X9-X step 4 Part B): the 2D
   pass now shares its own upload-on-demand discipline with the TriAPI's
   own vertex program, and each must invalidate the OTHER's residency, not
   just its own. Declared this early only because nv2a_init_lit_shader()
   below needs to clear it. */
typedef enum
{
	NV2A_RESIDENT_NONE,
	NV2A_RESIDENT_2D,
	NV2A_RESIDENT_TRI,
	NV2A_RESIDENT_STUDIO /* divergences.md #143, vs_studio */
} nv2a_resident_program_t;

static nv2a_resident_program_t nv2a_resident_program;

/* divergences.md #143: true while the GPU holds the studio path's program,
   combiner, stage-1-off and array formats (nv2a_sgpu_enter). Cleared by
   every other program upload (the three nv2a_init_*_shader functions, the
   decal pass's pixel program) and at the start of each frame. The
   constants it relies on (hardware 0..95 and 101..191) are written by no
   other path, so they survive another path's draws in between. */
static qboolean nv2a_sgpu_entered;

/* X9-X step 11: the fog state last pushed (nv2a_push_fog); invalid after
   any pixel-program upload, which resets the final combiner. */
static nv2a_fog_hw_t nv2a_fog_pushed;
static qboolean      nv2a_fog_pushed_valid;

/* X9-X D1: the 2D pass's own vertex ring - allocated in R_Init, freed in
   R_Shutdown (both well above where the rest of Part F is defined
   below), so its pointer needs to be declared this early too. */
static void *nv2a_2d_ring;

/* X9-X item 14 step 12: the decals' per-frame vertex buffer (the decal
   section below R_Init has the account), allocated with the 2D ring. */
#define NV2A_DECAL_FRAME_VERTS 2048
static nv2a_lit_vertex_t *nv2a_decal_vb;
static uint32_t           nv2a_decal_vb_used;

/* divergences.md #143 (resident programs): every vertex program this
   renderer runs is held in the NV2A's transform program memory at once -
   136 instruction slots (xemu hw/xbox/nv2a/nv2a_regs.h
   NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) - uploaded once, by
   nv2a_upload_vertex_programs at R_Init, and a switch is one
   NV097_SET_TRANSFORM_PROGRAM_START (nv_regs.h:770) naming the program's
   first slot. xemu stores that slot in NV_PGRAPH_CSV0_C (pgraph/pgraph.c,
   SET_TRANSFORM_PROGRAM_START) and runs from it up to the first
   instruction carrying FLD_FINAL (pgraph/glsl/vsh.c
   set_programmable_vsh_state); vp20compiler sets FLD_FINAL (dword 3,
   bit 0: deps/nxdk/tools/vp20compiler/main.c:164) on each program's last
   instruction only (main.c:549), so programs placed back to back each
   stop where they should (checked on every .inl below). They are
   position independent: VP1.1 has no branches, and constant addresses
   are absolute or relative to A0, never to the program counter. This is
   how the XDK keeps several vertex shaders resident
   (LoadVertexShader/SelectVertexShader).

   Before this, every program switch re-uploaded the program through the
   pushbuffer: ~280 dwords for vs_studio, and for vs_lit/vs_2d/vs_tri one
   pbkit block per instruction, each pb_end a write-combining flush and an
   MMIO spin (pbkit.c pb_start/pb_cache_flush). On the console the
   pushbuffer grew from 34 to 59 KiB a frame with the GPU studio path
   (PR #175).

   Nothing else writes program memory: SET_TRANSFORM_PROGRAM is pushed
   only by nv2a_upload_vertex_programs, pbkit never pushes it (pb_init
   included), and ref/nv2a never selects the fixed-function transform. A
   future path that loads a program of its own must call
   nv2a_upload_vertex_programs again afterwards. */
static const uint32_t nv2a_vp_lit_words[] =
{
	#include "vs_lit.inl"
};
static const uint32_t nv2a_vp_2d_words[] =
{
	#include "vs_2d.inl"
};
static const uint32_t nv2a_vp_tri_words[] =
{
	#include "vs_tri.inl"
};
static const uint32_t nv2a_vp_studio_words[] =
{
	#include "vs_studio.inl"
};

typedef enum
{
	NV2A_VP_LIT,    /* the world and brush entities (vs_lit.vs.cg) */
	NV2A_VP_2D,     /* the 2D pass (vs_2d.vs.cg) */
	NV2A_VP_TRI,    /* the TriAPI in 3D (vs_tri.vs.cg) */
	NV2A_VP_STUDIO, /* the GPU studio path (vs_studio.vp, divergences.md #143) */
	NV2A_VP_COUNT   /* also "none selected yet" */
} nv2a_vp_t;

#define NV2A_VP_SLOTS 136u /* NV2A_MAX_TRANSFORM_PROGRAM_LENGTH */
STATIC_ASSERT( sizeof( nv2a_vp_lit_words ) % 16 == 0 && sizeof( nv2a_vp_2d_words ) % 16 == 0
	&& sizeof( nv2a_vp_tri_words ) % 16 == 0 && sizeof( nv2a_vp_studio_words ) % 16 == 0,
	"a vertex program instruction is four dwords" );
STATIC_ASSERT(( sizeof( nv2a_vp_lit_words ) + sizeof( nv2a_vp_2d_words ) + sizeof( nv2a_vp_tri_words )
	+ sizeof( nv2a_vp_studio_words )) / 16 <= NV2A_VP_SLOTS,
	"the resident vertex programs no longer fit the NV2A's 136 program slots together" );

static const struct
{
	const uint32_t *words;
	uint32_t       instructions;
} nv2a_vp_table[NV2A_VP_COUNT] =
{
	{ nv2a_vp_lit_words,    sizeof( nv2a_vp_lit_words ) / 16 },
	{ nv2a_vp_2d_words,     sizeof( nv2a_vp_2d_words ) / 16 },
	{ nv2a_vp_tri_words,    sizeof( nv2a_vp_tri_words ) / 16 },
	{ nv2a_vp_studio_words, sizeof( nv2a_vp_studio_words ) / 16 },
};

static uint32_t  nv2a_vp_start[NV2A_VP_COUNT]; /* each program's first slot */
static nv2a_vp_t nv2a_vp_selected = NV2A_VP_COUNT;

/* The register-combiner "pixel programs" (ps_*.inl) are state, not
   memory: a switch re-pushes the whole set. Tracked so that a vertex
   program switch between two users of the same combiners (vs_tri and
   vs_studio both draw with ps_2d) pushes nothing for them. */
typedef enum
{
	NV2A_PS_NONE,
	NV2A_PS_LIT,
	NV2A_PS_2D,
	NV2A_PS_DECAL /* the decal pass pushes ps_decal.inl itself and records it */
} nv2a_ps_t;

static nv2a_ps_t nv2a_ps_selected = NV2A_PS_NONE;

/* Loads every program into program memory, back to back from slot 0, in
   blocks of up to 25 instructions (125 dwords: pbkit blocks stay under
   128). The load pointer advances by one slot per four dwords whatever
   the block boundaries (xemu pgraph/pgraph.c SET_TRANSFORM_PROGRAM).
   The transform engine is put in program mode with context writes off
   first, as every load sequence known to work on the console does
   (nxdk's samples, tools/x4-triangle, this file before resident
   programs): pb_init leaves it in its reset mode, and nothing documents
   that the hardware accepts program writes in fixed-function mode
   (nv2a-xbox-specialist review; xemu does not care). */
static void nv2a_upload_vertex_programs( void )
{
	uint32_t *p;
	uint32_t slot = 0;
	int v;

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
		| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0 );
	pb_end( p );

	for( v = 0; v < NV2A_VP_COUNT; v++ )
	{
		uint32_t i = 0, count = nv2a_vp_table[v].instructions;

		nv2a_vp_start[v] = slot;
		while( i < count )
		{
			uint32_t n = count - i, k;

			if( n > 25 )
				n = 25;
			p = pb_begin();
			for( k = 0; k < n; k++ )
			{
				pb_push( p++, NV097_SET_TRANSFORM_PROGRAM, 4 );
				memcpy( p, &nv2a_vp_table[v].words[( i + k ) * 4], 4 * 4 );
				p += 4;
			}
			pb_end( p );
			i += n;
		}
		slot += count;
	}

	nv2a_vp_selected = NV2A_VP_COUNT; /* the next select sets the execution mode too */
}

static void nv2a_select_vertex_program( nv2a_vp_t vp )
{
	uint32_t *p;

	if( nv2a_vp_selected == vp )
		return;

	p = pb_begin();
	if( nv2a_vp_selected == NV2A_VP_COUNT )
	{
		p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
			MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
			| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
		p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	}
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_START, nv2a_vp_start[vp] );
	pb_end( p );
	nv2a_vp_selected = vp;
}

/* ps_lit or ps_2d. Each upload resets the final combiner to its unfogged
   form, so the fog is pushed again after one (nv2a_push_fog). */
static void nv2a_select_pixel_program( nv2a_ps_t ps )
{
	uint32_t *p;

	if( nv2a_ps_selected == ps )
		return;

	p = pb_begin();
	if( ps == NV2A_PS_LIT )
	{
		#include "ps_lit.inl"
	}
	else
	{
		#include "ps_2d.inl"
	}
	pb_end( p );
	nv2a_ps_selected = ps;
	nv2a_fog_pushed_valid = false;
}

/* X9-C: the diffuse x lightmap x 2 program pair (ref/nv2a/vs_lit.vs.cg,
   ps_lit.ps.cg - both real, freshly-authored Cg sources compiled through
   the same cgc/vp20compiler/fp20compiler pipeline vs.inl/ps.inl already
   were, verified reproducible before being committed). Selected, not
   uploaded (nv2a_vp_table). */
static void nv2a_init_lit_shader( void )
{
	nv2a_resident_program = NV2A_RESIDENT_NONE;
	nv2a_sgpu_entered = false;

	nv2a_select_vertex_program( NV2A_VP_LIT );
	nv2a_select_pixel_program( NV2A_PS_LIT );
}

/* X9-X step 11: the GL fog state (nv2a_fog_current, r_warp.c) in
   hardware. The vertex programs write the eye depth to FOG (vs_lit/vs_tri),
   the fog unit turns it into the fog factor for NV097_SET_FOG_MODE with
   NV097_SET_FOG_PARAMS (nv2a_warp_util.c), and the final combiner blends
   with the fog colour: A = the fog factor (FOG, alpha), B = the colour the
   general combiners left in V0 (the pixel programs' own final D source),
   C = the fog colour (FOG, rgb) - A*B + (1-A)*C, GL's own fog blend. Off,
   the final combiner is exactly what ps_2d.inl/ps_lit.inl push (D = V0).
   NV097_SET_FOG_GEN_MODE is not written: with a vertex program the fog
   distance is the program's FOG output (xemu hw/xbox/nv2a/pgraph/glsl/
   vsh.c reads oFog.x); not yet checked on hardware. */
static uint32_t nv2a_final_combiner_cw0( qboolean fog )
{
	if( !fog )
		return MASK( NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4 );

	return MASK( NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, 0x3 ) | MASK( NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 1 )
		| MASK( NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, 0x4 )
		| MASK( NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x3 );
}

static void nv2a_push_fog( const nv2a_fog_hw_t *fog )
{
	nv2a_fog_hw_t want;
	uint32_t *p;

	memset( &want, 0, sizeof( want ));
	if( fog && fog->enable && ( fog->mode != NV2A_FOG_MODE_LINEAR || fog->end > fog->start ))
		want = *fog;

	if( nv2a_fog_pushed_valid && !memcmp( &want, &nv2a_fog_pushed, sizeof( want )))
		return;

	p = pb_begin();
	if( want.enable )
	{
		float params[3] = { 0.0f, 0.0f, 0.0f };
		uint32_t mode;

		switch( want.mode )
		{
		case NV2A_FOG_MODE_LINEAR:
			nv2a_fog_linear_params( want.start, want.end, params );
			mode = NV097_SET_FOG_MODE_V_LINEAR;
			break;
		case NV2A_FOG_MODE_EXP2:
			nv2a_fog_exp2_params( want.density, params );
			mode = NV097_SET_FOG_MODE_V_EXP2;
			break;
		case NV2A_FOG_MODE_EXP:
		default:
			nv2a_fog_exp_params( want.density, params );
			mode = NV097_SET_FOG_MODE_V_EXP;
			break;
		}

		p = pb_push1( p, NV097_SET_FOG_MODE, mode );
		pb_push( p++, NV097_SET_FOG_PARAMS, 3 );
		memcpy( p, params, sizeof( params ));
		p += 3;
		p = pb_push1( p, NV097_SET_FOG_COLOR, want.color );
	}
	p = pb_push1( p, NV097_SET_FOG_ENABLE, want.enable ? 1 : 0 );
	p = pb_push1( p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, nv2a_final_combiner_cw0( want.enable ));
	pb_end( p );

	nv2a_fog_pushed = want;
	nv2a_fog_pushed_valid = true;
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

/* X9-X: the 3D state the world and the brush entities expect. pbkit
   re-pushes depth-test enable only when the depth/stencil target changes
   (pbkit.c:1680-1707), so R_BeginFrame's pb_target_back_buffer does not
   restore it, and a frame's 2D pass (depth off, cull off, blending) would
   otherwise carry into the next frame's world draw. Menu and console-only
   frames never call R_Set2DMode(false), so every 3D draw sets this itself
   (nv2a-xbox-specialist, X9-X design). */
static void nv2a_set_3d_state( void )
{
	uint32_t *p = pb_begin();
	p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, 1 );
	p = pb_push1( p, NV097_SET_DEPTH_MASK, 1 );
	p = pb_push1( p, NV097_SET_CULL_FACE_ENABLE, 1 );
	p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
	pb_end( p );
}

/* X9-F: port of ref/gl/gl_frustum.c's own GL_FrustumCullBox - the same
   signbits switch, literally, minus the clipFlags early-out (there is no
   BSP node hierarchy here to propagate a shrinking clip mask down through,
   so every face is tested against all 5 planes; see docs/r3d/fork-plan.md's
   X9-F entry for why that is the right scope for this slice). Returns true
   if the box is entirely outside at least one plane. */
qboolean nv2a_frustum_cull_box( const vec3_t mins, const vec3_t maxs )
{
	int i;

	/* X9-O: ref/gl's GL_FrustumCullBox returns "not culled" under r_nocull
	   (gl_frustum.c:106) - the world's per-face test and the brush-entity
	   bounds test both come through here, as both do in ref/gl. */
	if( nv2a_r_nocull.value )
		return false;

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

/* R3 (#146): nv2a_frustum's planes for nv2a_frustum_face_outside, and how
   many of them to test. nv2a_frustum_cull_box returns "not culled" at the
   first plane whose signbits is out of range (its switch's default) -
   unreachable, since SignbitsForPlane gives 0-7, but kept: the planes
   from there on are not tested. */
static uint32_t nv2a_frustum_prepare_planes( nv2a_cull_plane_t out[5] )
{
	uint32_t i;

	for( i = 0; i < 5; i++ )
	{
		const mplane_t *p = &nv2a_frustum[i];

		if( p->signbits > 7 )
			break;
		out[i].normal[0] = p->normal[0];
		out[i].normal[1] = p->normal[1];
		out[i].normal[2] = p->normal[2];
		out[i].dist = p->dist;
		out[i].corner[0] = ( p->signbits & 1 ) ? 0 : 3;
		out[i].corner[1] = ( p->signbits & 2 ) ? 1 : 4;
		out[i].corner[2] = ( p->signbits & 4 ) ? 2 : 5;
	}
	return i;
}

/* The existing X9-D batching loop, unchanged in shape, factored out so
   X9-F's own run-merging pass (nv2a_draw_map_geometry() below) can call
   it once per contiguous visible run instead of once for the whole
   buffer - state setup (textures/attributes/constants) stays completely
   outside this function and outside the loop that calls it. */
static void nv2a_draw_index_range( uint32_t begin, uint32_t end )
{
	uint32_t i = begin;

	NV2A_FT_ADD( XBOX_FT_W_DRAWS, 1 );
	NV2A_FT_ADD( XBOX_FT_W_VERTS_WORLD, ( end - begin ) * 2u ); /* two 16-bit indices per dword */
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
		NV2A_FT_ADD( XBOX_FT_W_PRIMS, 1 );

		i += batch;
	}
}

/* X9-S (MPAK v11): the vertex chunk the attribute arrays currently point
   at. A map whose vertices outgrow 16-bit indices is split into chunks of
   at most 65,536; each face's indices are relative to its chunk's base
   (nv2a_map_face_t::vertex_base). Pointing position/uv0/uv1 at the base
   is the base-vertex draw Xbox D3D implements for SetIndices'
   BaseVertexIndex - three register writes, done only when a draw moves
   to a different chunk. */
static uint32_t nv2a_bound_vertex_base;

/* True while the GPU still holds nv2a_enter_lit_state()'s state. The
   world pass sets it; nv2a_2d_flush's re-entry (the only other path that
   swaps the program, turns stage 1 off and repoints the attribute arrays)
   and R_BeginFrame clear it. nv2a_bound_vertex_base above is only a valid
   shadow while this is true - the TriAPI re-entry moves the same array
   slots onto its ring without touching it. */
static qboolean nv2a_lit_state_current;

static void nv2a_bind_vertex_base( uint32_t base )
{
	const uint32_t *v = nv2a_map_vb + (size_t)base * ( sizeof( nv2a_lit_vertex_t ) / sizeof( uint32_t ));

	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_lit_vertex_t ), &v[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &v[3] );
	nv2a_set_attrib_pointer( 10, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &v[5] );
	{
		/* Defensive, pending a real-hardware run on a multi-chunk map:
		   invalidate the vertex fetch cache after moving the arrays, as
		   Perfect Dark X's gfx_nv2a.cpp does before drawing from a
		   different vertex range. Moving array offsets mid-frame is new
		   in X9-S, and xemu cannot tell us whether the cache is keyed by
		   address. */
		uint32_t *p = pb_begin();
		p = pb_push1( p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0 );
		pb_end( p );
	}
	nv2a_bound_vertex_base = base;
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

	NV2A_FT_ADD( XBOX_FT_W_DRAWS, 1 );
	NV2A_FT_ADD( XBOX_FT_W_VERTS_WORLD, end > begin ? end - begin : 0 );
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
		NV2A_FT_ADD( XBOX_FT_W_PRIMS, 1 );

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

/* X9-N2: nv2a_r_anisotropy -> CONTROL0's ANISOTROPY field (log2 ratio). */
static uint32_t nv2a_anisotropy_bits( void )
{
	int level = (int)nv2a_r_anisotropy.value;
	uint32_t log2 = level >= 8 ? 3 : level >= 4 ? 2 : level >= 2 ? 1 : 0;
	return MASK( NV097_SET_TEXTURE_CONTROL0_ANISOTROPY, log2 );
}

/* X9-N2: nv2a_r_lodbias -> FILTER bits 0-12, signed, 8 fraction bits. */
static uint32_t nv2a_lod_bias_bits( void )
{
	float bias = bound( -3.0f, nv2a_r_lodbias.value, 3.0f );
	int fixed = (int)( bias * 256.0f + ( bias < 0.0f ? -0.5f : 0.5f ));
	return (uint32_t)fixed & 0x1FFFu;
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
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
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
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 | nv2a_anisotropy_bits() );
	/* X9-N: MIN=6 (TENT_TENT_LOD: bilinear within the two nearest mip
	   levels, blended - trilinear, GL_LINEAR_MIPMAP_LINEAR), MAG=2
	   (TENT, bilinear): 0x02062000. The nearest option is MIN=3
	   (BOX_NEARESTLOD, GL_NEAREST_MIPMAP_NEAREST), MAG=1: 0x01032000.
	   Both keep the non-zero convolution-kernel field (bits 13-15) real
	   hardware requires. The nxdk mesh sample's 0x04074000 is
	   MIN/MAG=CONVOLUTION_2D_LOD0 - xemu samples it as bilinear, real
	   hardware filters with the convolution kernel and smears (Perfect
	   Dark X's gfx_nv2a.cpp documents the same). */
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, ( nv2a_r_texturenearest.value ? 0x01032000 : 0x02062000 ) | nv2a_lod_bias_bits() );
	pb_end( p );
}

/*
========================================================================
X9-X item 14 step 9: animated brush textures

ref/gl draws each surface with R_TextureAnimation( surf )
(gl_rsurf.c:518, called from R_RenderBrushPoly, gl_rsurf.c:1486): the
alternate set (+a..+j) when the entity's frame is non-zero, the +0..+9
frames by time, and the '-' random tiles by the surface's texturemins.
The chains are the engine's (Mod_SequenceAnimatedTexture,
mod_bmodel.c:3043), on the world model's texture_t list.

Here a face carries a texture DIRECTORY index (its batch's), not a
texture_t. nv2a_texanim_build maps the two once per map:
- a directory entry a face draws belongs to the engine texture that
  face's texinfo names (every BSP face is both an engine msurface_t and a
  baked face, joined by nv2a_map_face_remap);
- the frames no face draws follow the drawn entries, in engine texture
  order, as the baker appended them (tools/asset-studio/src/formats/
  AnimTextureFrames.h) - found with nv2a_texanim_extra_frames, the same
  rule. The directory must then hold exactly drawn + extra entries; an
  archive baked before the frames were kept holds only the drawn ones,
  and its animated textures stay on the frame the face names (what this
  renderer drew before), with one console line saying so.
The draw loops then ask nv2a_texanim_dir_for which directory entry to
bind instead of the face's own: once per texture batch in the world pass
(the frame is the same for every face of a batch), per face only for the
'-' random tiles, and per face with a one-entry cache in the brush-entity
pass (whose faces arrive in face order, not batch order).
========================================================================
*/
STATIC_ASSERT( MOD_FRAMES == NV2A_TEXANIM_MOD_FRAMES, "nv2a_texanim_resolve indexes ref/common's rtable[MOD_FRAMES][MOD_FRAMES]" );

static nv2a_texanim_t  nv2a_texanim[NV2A_MAP_MAX_TEXTURES];        /* per engine texture, world->textures[] order */
static uint16_t        nv2a_texanim_dir[NV2A_MAP_MAX_TEXTURES];    /* engine texture -> directory entry, NV2A_TEXANIM_NONE if not baked */
static uint16_t        nv2a_texanim_of_dir[NV2A_MAP_MAX_TEXTURES]; /* directory entry -> engine texture, NV2A_TEXANIM_NONE if none or shared */
static uint32_t        nv2a_texanim_count;     /* world->numtextures once the tables are built and usable, else 0 */
static const model_t  *nv2a_texanim_built_for; /* the world the tables were built for (reset per map, like nv2a_pvs_parity_checked_for) */
static int             nv2a_texanim_ticks_now; /* (int)( cl.time * 20 ), set once per frame (gl_rsurf.c:549) */

static void nv2a_texanim_reset( void )
{
	nv2a_texanim_count = 0;
	nv2a_texanim_built_for = NULL;
}

/* Index of `tex` in world->textures, NV2A_TEXANIM_NONE if it is not there
   (load time only: a linear scan, starting at `hint`). */
static uint32_t nv2a_texanim_index_of( const model_t *world, const texture_t *tex, uint32_t hint )
{
	uint32_t n = (uint32_t)world->numtextures, k;

	if( !tex )
		return NV2A_TEXANIM_NONE;
	for( k = 0; k < n; k++ )
	{
		uint32_t j = ( hint + k ) % n;
		if( world->textures[j] == tex )
			return j;
	}
	return NV2A_TEXANIM_NONE;
}

static void nv2a_texanim_build( const model_t *world )
{
	uint32_t n, t, o, d, faces_n, drawn = 0, extra_n, animated = 0;
	uint32_t numtexinfo;
	uint16_t *texinfo_tex = NULL, *extras = NULL;
	const char **names = NULL;
	uint8_t *used = NULL;
	uint32_t hint = 0;
	qboolean consistent = true;

	nv2a_texanim_built_for = world;
	nv2a_texanim_count = 0;

	if( !world || !world->textures || world->numtextures <= 0 || !world->surfaces || !world->texinfo || world->numtexinfo <= 0 )
		return;
	n = (uint32_t)world->numtextures;
	numtexinfo = (uint32_t)world->numtexinfo;
	if( n > NV2A_MAP_MAX_TEXTURES || !nv2a_map_faces || !nv2a_map_face_remap )
		return;

	for( d = 0; d < NV2A_MAP_MAX_TEXTURES; d++ )
		nv2a_texanim_of_dir[d] = NV2A_TEXANIM_NONE;

	/* The engine's chains, turned into indices. */
	for( t = 0; t < n; t++ )
	{
		const texture_t *tx = world->textures[t];
		nv2a_texanim_t *e = &nv2a_texanim[t];

		memset( e, 0, sizeof( *e ));
		e->anim_next = NV2A_TEXANIM_NONE;
		e->alternate = NV2A_TEXANIM_NONE;
		nv2a_texanim_dir[t] = NV2A_TEXANIM_NONE;
		if( !tx )
			continue;
		/* anim_* are at most 10 frames * ANIM_CYCLE (mod_bmodel.c:3134);
		   anything past int16 is not a chain the engine built. */
		if( tx->anim_total > 0 && tx->anim_total <= 0x7FFF && tx->anim_min >= 0 && tx->anim_max <= 0x7FFF && tx->anim_min <= tx->anim_max )
		{
			e->anim_total = (int16_t)tx->anim_total;
			e->anim_min = (int16_t)tx->anim_min;
			e->anim_max = (int16_t)tx->anim_max;
		}
		e->width = (uint16_t)( tx->width > 0xFFFF ? 0 : tx->width );
		e->height = (uint16_t)( tx->height > 0xFFFF ? 0 : tx->height );
		e->random = tx->name[0] == '-';
		e->anim_next = (uint16_t)nv2a_texanim_index_of( world, tx->anim_next, t );
		e->alternate = (uint16_t)nv2a_texanim_index_of( world, tx->alternate_anims, t );
		if( e->anim_total || e->alternate != NV2A_TEXANIM_NONE )
			animated++;
	}
	if( !animated )
		return; /* nothing to animate: leave nv2a_texanim_count 0, every lookup returns at once */

	texinfo_tex = (uint16_t *)malloc( numtexinfo * sizeof( *texinfo_tex ) + n * ( sizeof( *extras ) + sizeof( *names ) + sizeof( *used )));
	if( !texinfo_tex )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: no memory for the animated-texture tables; textures stay on their first frame\n" );
		return;
	}
	names = (const char **)( texinfo_tex + numtexinfo + ( numtexinfo & 1 )); /* keep the pointers 4-aligned */
	extras = (uint16_t *)( names + n );
	used = (uint8_t *)( extras + n );
	memset( used, 0, n );

	for( o = 0; o < numtexinfo; o++ )
	{
		uint32_t idx = nv2a_texanim_index_of( world, world->texinfo[o].texture, hint );
		texinfo_tex[o] = (uint16_t)idx;
		if( idx != NV2A_TEXANIM_NONE )
			hint = idx;
	}
	for( t = 0; t < n; t++ )
		names[t] = world->textures[t] ? world->textures[t]->name : NULL;

	/* Every face joins its engine texture to its directory entry. */
	faces_n = (uint32_t)world->numsurfaces < nv2a_map_face_remap_count ? (uint32_t)world->numsurfaces : nv2a_map_face_remap_count;
	for( o = 0; o < faces_n && consistent; o++ )
	{
		const msurface_t *surf = &world->surfaces[o];
		uint32_t sorted = nv2a_map_face_remap[o];
		ptrdiff_t ti;

		if( !surf->texinfo || sorted >= nv2a_map_face_count )
			continue;
		ti = surf->texinfo - world->texinfo;
		if( ti < 0 || (uint32_t)ti >= numtexinfo )
			continue;
		t = texinfo_tex[ti];
		d = nv2a_map_faces[sorted].texture;
		if( t >= n || d >= nv2a_map_texture_count )
			continue;

		used[t] = 1;
		if( nv2a_texanim_dir[t] == NV2A_TEXANIM_NONE )
			nv2a_texanim_dir[t] = (uint16_t)d;
		else if( nv2a_texanim_dir[t] != d )
			consistent = false; /* one miptex in two directory entries: not an archive this code understands */
		if( nv2a_texanim_of_dir[d] == NV2A_TEXANIM_NONE )
			nv2a_texanim_of_dir[d] = (uint16_t)t;
		else if( nv2a_texanim_of_dir[d] != t )
			nv2a_texanim_of_dir[d] = NV2A_TEXANIM_NONE - 1; /* shared (the baker's missing-texture placeholder): never animated */
		if( d + 1 > drawn )
			drawn = d + 1;
	}

	if( !consistent )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: %s: faces of one texture use two texture-directory entries; animated textures disabled\n", world->name );
		free( texinfo_tex );
		return;
	}

	for( d = 0; d < NV2A_MAP_MAX_TEXTURES; d++ )
		if( nv2a_texanim_of_dir[d] == NV2A_TEXANIM_NONE - 1 )
			nv2a_texanim_of_dir[d] = NV2A_TEXANIM_NONE;

	extra_n = nv2a_texanim_extra_frames( names, used, n, extras, n );
	if( extra_n && drawn + extra_n == nv2a_map_texture_count )
	{
		for( o = 0; o < extra_n; o++ )
		{
			nv2a_texanim_dir[extras[o]] = (uint16_t)( drawn + o );
			nv2a_texanim_of_dir[drawn + o] = extras[o];
		}
	}
	else if( extra_n )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: %s: the archive has %u texture(s), not %u drawn + %u animation frame(s) - baked before those frames were kept? Re-bake it; animated textures stay on the frame each face names\n",
			world->name, nv2a_map_texture_count, drawn, extra_n );
	}

	free( texinfo_tex );
	nv2a_texanim_count = n;
	gEngfuncs.Con_Reportf( "ref_nv2a: %u animated texture(s), %u frame(s) only the chains reach\n", animated, extra_n );
}

/* The directory entry to bind for a face whose own entry is `dir`, now -
   R_TextureAnimation (gl_rsurf.c:518) through nv2a_texanim_resolve.
   `alternate` is the entity's curstate.frame != 0; `surf` is the face's
   engine surface, needed only for the '-' random tiles (NULL takes the
   R_TextureAnim branch, which leaves them on the face's own tile). The
   face's own entry whenever the answer is not in the archive. */
static uint32_t nv2a_texanim_dir_for( uint32_t dir, qboolean alternate, const msurface_t *surf )
{
	uint32_t t, r;
	const nv2a_texanim_t *e;

	if( !nv2a_texanim_count || dir >= NV2A_MAP_MAX_TEXTURES )
		return dir;
	t = nv2a_texanim_of_dir[dir];
	if( t >= nv2a_texanim_count )
		return dir;
	e = &nv2a_texanim[t];
	if( !e->anim_total && ( !alternate || e->alternate == NV2A_TEXANIM_NONE ))
		return dir; /* the common case: not animated */

	r = nv2a_texanim_resolve( nv2a_texanim, nv2a_texanim_count, t, alternate, nv2a_texanim_ticks_now,
		surf != NULL, surf ? surf->texturemins[0] : 0, surf ? surf->texturemins[1] : 0,
		(const uint16_t( * )[NV2A_TEXANIM_MOD_FRAMES])rtable );
	if( r >= nv2a_texanim_count || nv2a_texanim_dir[r] >= nv2a_map_texture_count )
		return dir;
	return nv2a_texanim_dir[r];
}

/* Whether faces drawn with directory entry `dir` each show a different
   frame (the '-' random tiles), so they cannot share one bind. */
static qboolean nv2a_texanim_dir_per_surface( uint32_t dir, qboolean alternate )
{
	uint32_t t;

	if( !nv2a_texanim_count || dir >= NV2A_MAP_MAX_TEXTURES )
		return false;
	t = nv2a_texanim_of_dir[dir];
	return t < nv2a_texanim_count && nv2a_texanim_per_surface( nv2a_texanim, nv2a_texanim_count, t, alternate );
}

/* Once per rendered frame, before the world and entity passes. */
static void nv2a_texanim_begin_frame( const model_t *world )
{
	if( world && world != nv2a_texanim_built_for )
		nv2a_texanim_build( world );
	nv2a_texanim_ticks_now = nv2a_texanim_ticks( gp_cl->time );
}

/*
========================================================================
X9-M/X9-X step 5: brush AND studio entities (func_door, func_wall,
func_illusionary, ladders, NPCs, items, the view model...)

Every face the baker writes comes from LUMP_FACES, which holds the world
model's faces AND every submodel's (the `*N` brush entities). Before X9-M
this renderer drew all of them as static world geometry: with PVS on,
submodel faces were never marked visible (no world leaf lists them - the
PVS walk is not where they come from), so doors, ladders, fences and
chargers vanished; with PVS off they drew frozen at their compile-time
position, triggers included. This is the entity half ref/gl has and this
file did not: R_AddEntity/R_ClearScene keep the frame's entity lists
(gl_rmain.c:217,234), and nv2a_draw_entity_list() (X9-X step 5, replacing
the brush-only nv2a_draw_brush_entities()) walks them after the world,
solid list first, then the translucent list sorted like
R_TransEntityCompare (gl_rmain.c:86) - R_DrawEntitiesOnList's order
(gl_rmain.c). Each brush entity draws with its own transform
(R_RotateForEntity/R_TranslateForEntity, gl_rmain.c:448,474) and render
mode (R_SetRenderMode, gl_rsurf.c:1717); each studio entity draws through
R_DrawStudioModel (r_studio.c), each sprite through R_DrawSpriteModel
(r_sprite.c, X9-X step 6). Beams go to a third list, drawn by CL_DrawBeams
(r_effects.c, X9-X step 7) when the engine's CL_DrawEFX calls back.
========================================================================
*/
/* NV2A_MAX_ENTITIES itself now lives in r_studio_bridge.h (included
   above), shared with r_studio.c's own R_FindParentEntity. */
#define NV2A_SURF_DRAWTURB BIT( 4 ) /* common/bspfile.h:55's SURF_DRAWTURB - same "mirror the one engine
                                       msurface flag this file reads" pattern as NV2A_SURF_PLANEBACK */

#define NV2A_UNLIT_PAGE_SIZE 16 /* texels per side; pitch 64 bytes, LU_IMAGE's own alignment */

cl_entity_t *nv2a_solid_entities[NV2A_MAX_ENTITIES]; /* X9-X step 5: exposed, r_studio_bridge.h (R_FindParentEntity) */
cl_entity_t *nv2a_trans_entities[NV2A_MAX_ENTITIES];
int          nv2a_num_solid_entities;
int          nv2a_num_trans_entities;
/* X9-X step 7: ref/gl's draw_list->beam_entities (gl_local.h:172), the
   ET_BEAM entities CL_DrawBeams (r_effects.c) walks. */
cl_entity_t *nv2a_beam_entities[NV2A_MAX_ENTITIES];
int          nv2a_num_beam_entities;
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
   mode and filter stay as nv2a_enter_lit_state() set them. Returns false
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
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
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

/* X9-X step 11 (r_studio_bridge.h): the directory entry the baked face of
   a surface of the current world draws with. */
uint32_t nv2a_surface_map_texture( const msurface_t *surf )
{
	const model_t *world = gp_cl->models[1];
	uint32_t orig, sorted;

	if( !surf || world != nv2a_map_owner || !world->surfaces || !nv2a_map_faces || !nv2a_map_face_remap )
		return NV2A_TRI_NO_MAP_TEXTURE;
	if( surf < world->surfaces || surf >= world->surfaces + world->numsurfaces )
		return NV2A_TRI_NO_MAP_TEXTURE;

	orig = (uint32_t)( surf - world->surfaces );
	if( orig >= nv2a_map_face_remap_count )
		return NV2A_TRI_NO_MAP_TEXTURE;
	sorted = nv2a_map_face_remap[orig];
	if( sorted >= nv2a_map_face_count || nv2a_map_faces[sorted].texture >= nv2a_map_texture_count )
		return NV2A_TRI_NO_MAP_TEXTURE;
	return nv2a_map_faces[sorted].texture;
}

/* X9-X step 5: nv2a_brush_entity_render_mode (the brush-only branch of
   ref/gl's R_GetEntityRenderMode, gl_studio.c:2384) is gone - both callers
   below now use the whole function (r_studio.c, includes the brush branch
   verbatim plus the studio one), since this list holds mod_studio entities
   too as of this step. */

/* ref/gl's R_OpaqueEntity (gl_rmain.c:63). */
static qboolean nv2a_opaque_entity( cl_entity_t *ent )
{
	if( R_GetEntityRenderMode( ent ) == kRenderNormal )
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
int nv2a_fx_blend( cl_entity_t *e ) /* X9-X step 5: exposed, r_studio_bridge.h (R_StudioSetRenderamt) */
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
	int rendermode1 = R_GetEntityRenderMode( ent1 );
	int rendermode2 = R_GetEntityRenderMode( ent2 );
	vec3_t vecLen, org;
	float dist1, dist2;

	/* gl_rmain.c:97,105: a studio entity is always sorted by distance
	   (`model->type != mod_brush ||`) - only a mod_brush entity in
	   TransAlpha skips the distance sort (it draws back-to-front by its
	   own face order instead). */
	if( ent1->model->type != mod_brush || rendermode1 != kRenderTransAlpha )
	{
		VectorAverage( ent1->model->mins, ent1->model->maxs, org );
		VectorAdd( ent1->origin, org, org );
		VectorSubtract( nv2a_eye_origin, org, vecLen );
		dist1 = DotProduct( vecLen, vecLen );
	}
	else dist1 = 1000000000;

	if( ent2->model->type != mod_brush || rendermode2 != kRenderTransAlpha )
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

/* X9-Y: kRenderTransColor's synthetic diffuse texel - a 16x16 page of one
   repeated RGBA8 value, the same allocation shape as nv2a_unlit_page just
   above, rewritten per draw to the entity's own rendercolor/renderamt.
   Every real face's baked UVs were meant for that face's REAL texture, not
   this one, but that does not matter: every texel here is identical, so
   any UV samples the same value. Format matches nv2a_map_lightmap's own
   citation above (LU_IMAGE_A8B8G8R8 names a little-endian word whose LOW
   byte is R, i.e. bytes [R,G,B,A] in memory - this writes the same byte
   layout, not a packed native-endian literal). */
static uint32_t *nv2a_flat_color_page;

/* Binds the page above to stage 0 with exactly the RGBA ps_lit's diffuse
   sampler will read. Returns false only if the one-time contiguous
   allocation fails. */
static qboolean nv2a_bind_flat_color_stage0( byte r, byte g, byte b, byte a )
{
	uint32_t *p;
	uint32_t phys, word;
	int k;

	if( !nv2a_flat_color_page )
	{
		nv2a_flat_color_page = MmAllocateContiguousMemoryEx( NV2A_UNLIT_PAGE_SIZE * NV2A_UNLIT_PAGE_SIZE * 4, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
		if( !nv2a_flat_color_page )
			return false;
	}

	word = (uint32_t)r | ( (uint32_t)g << 8 ) | ( (uint32_t)b << 16 ) | ( (uint32_t)a << 24 );
	for( k = 0; k < NV2A_UNLIT_PAGE_SIZE * NV2A_UNLIT_PAGE_SIZE; k++ )
		nv2a_flat_color_page[k] = word;

	phys = (uint32_t)nv2a_flat_color_page & 0x03ffffff;
	p = pb_begin();
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, phys );
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, ( NV2A_UNLIT_PAGE_SIZE * 4 ) << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( NV2A_UNLIT_PAGE_SIZE << 16 ) | NV2A_UNLIT_PAGE_SIZE );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, 0x01012000 ); /* nearest - every texel is identical; never a zero convolution-kernel field (divergences.md #102) */
	pb_end( p );
	return true;
}

/* ref/gl's R_SetRenderMode (gl_rsurf.c:1717) on NV2A state. ref/gl
   modulates the texture by a vertex colour whose alpha is tr.blend; this
   combiner (ps_lit) has no colour input, so the blend amount goes in as
   NV097_SET_BLEND_COLOR's alpha and the blend factors use CONSTANT_ALPHA
   in place of SRC_ALPHA. That is exact for opaque texels; for a
   '{'-masked texture drawn in a translucent mode it ignores the texel's
   own alpha, which ref/gl would multiply in.

   kRenderTransColor (gl_rsurf.c:1724-1730): ref/gl draws that as a flat
   colour with texturing off, using curstate.renderamt directly rather
   than tr.blend (the `blend` parameter below, which every other case
   uses). ps_lit has no untextured path, so this binds
   nv2a_bind_flat_color_stage0() instead - with the lightmap stage already
   pointed at the neutral page (nv2a_entity_has_lightmap's kRenderTransColor
   case, called by nv2a_draw_brush_entity before this function), ps_lit.ps.cg
   computes diffuse.rgb * light.rgb * 2 == diffuse.rgb (light.rgb == 0.5
   doubled to 1) and outputs diffuse.a unmodified, so the fragment IS
   exactly (r,g,b,a) and real SRC_ALPHA/ONE_MINUS_SRC_ALPHA blending
   reproduces the reference exactly - not the CONSTANT_ALPHA approximation
   the other translucent modes below need. */
static qboolean nv2a_set_render_mode( const cl_entity_t *e, float blend )
{
	uint32_t *p = pb_begin();
	uint32_t alpha = (uint32_t)( bound( 0.0f, blend, 1.0f ) * 255.0f + 0.5f );

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 ); /* R_DrawWorld/R_DrawBrushModel leave it off (gl_rsurf.c:3691,1931) */
		break;
	case kRenderTransColor:
	{
		byte a = (byte)bound( 0, e->curstate.renderamt, 255 );

		pb_end( p ); /* nv2a_bind_flat_color_stage0 opens its own block - these pairs don't nest */

		if( !nv2a_bind_flat_color_stage0( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, a ))
			return false; /* no contiguous memory for the synthetic texture - draw nothing rather than the wrong thing */

		p = pb_begin();
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
		pb_end( p );
		return true;
	}
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
		/* divergences.md #103 point 5: CONSTANT_ALPHA blends a '{'-masked
		   texture's fully-transparent DXT1 texel (decoded as plain black,
		   confirmed by the baker's own transparent-texel convention -
		   X9-Q's rgbcx encode notes above) at the ENTITY's blend amount
		   instead of not drawing it, painting a solid black window/grate
		   cutout where ref/gl "draws nothing there". A real per-fragment
		   alpha *= entity blend would need a combiner change (ps_lit has
		   no such constant input yet); this alpha-test instead discards
		   only the fully-transparent sentinel texel (ps_lit.ps.cg's
		   diffuse.a passes the real, unmodified, never-doubled texel alpha
		   through), which is what removes the visible black smudge in the
		   common case (a masked window/grate at its default, non-fading
		   blend). Left open: a bilinear-filtered edge texel's own
		   fractional alpha still blends at the uniform CONSTANT_ALPHA
		   amount rather than its own value once inside the pass band. */
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER );
		p = pb_push1( p, NV097_SET_ALPHA_REF, 0x01 );
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

/* The GPU state every lit draw (the world, and each brush entity) draws
   under: depth/cull (nv2a_set_3d_state), the vs_lit/ps_lit program, stage 1
   on the lightmap page, CONTROL0, the world alpha test, constant 96 = the
   world's viewproj, and the attribute arrays on the map's vertex chunk 0.
   Was the head of nv2a_draw_map_geometry(); factored out so a brush entity
   drawn after the TriAPI pass took the GPU (nv2a_2d_flush's re-entry: the
   tri program, stage 1 off, alpha GREATER, arrays on the TriAPI ring) can
   put it back - see nv2a_lit_state_current. */
static void nv2a_enter_lit_state( void )
{
	uint32_t *p;
	uint32_t tex_phys;
	uint32_t i;

	nv2a_set_3d_state();

	/* This path's own two-texcoord/two-texture program and combiners,
	   selected again: the 2D/TriAPI pass and the studio path select their
	   own in between (resident programs, nv2a_vp_table). */
	nv2a_init_lit_shader();
	/* X9-X step 11: the world and brush entities are fogged like
	   everything else 3D (R_DrawFog enables GL_FOG before R_DrawWorld). */
	nv2a_push_fog( &nv2a_fog_current );

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
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
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

	/* Position (slot 0), diffuse UV (slot 9) and lightmap UV (slot 10 -
	   vs_lit.vs.cg's own TEXCOORD1, confirmed by a real vp20compiler run,
	   not assumed, to map there), pointed at vertex chunk 0; draws of
	   faces in another chunk re-point them (X9-S). */
	nv2a_bind_vertex_base( 0 );

	nv2a_lit_state_current = true;
}

/* ref/gl's R_DrawBrushModel (gl_rsurf.c:1822) for one entity: bounds and
   frustum cull, object transform, per-face backface cull in model space
   (R_SortBrushModelSurfaces, gl_rsurf.c:1764), render mode, draw.
   Not ported: fullbrights, detail textures, polygon offset
   (gl_polyoffset_bmodels) and the kRenderTransTexture back-to-front face
   sort. Dynamic lights (step 8), fog (step 11) and decals (step 12) have
   landed since this list was written. */
/* R_DrawBrushModel's entry checks and cull (gl_rsurf.c:1822-1855): only
   this map's own submodels share its baked faces - anything else (a brush
   model from another BSP) has nothing to draw here - and an entity whose
   bounds are outside the frustum is skipped. X9-X step 8: shared with the
   lightmap update, which must agree with this on which entities draw. */
static qboolean nv2a_brush_entity_visible( const model_t *world, const cl_entity_t *e, qboolean *rotated_out, vec3_t mins_out )
{
	const model_t *clmodel = e->model;
	vec3_t mins, maxs;
	qboolean rotated;
	int i;

	if( !world || !clmodel || clmodel->type != mod_brush || clmodel == world || clmodel->surfaces != world->surfaces )
		return false;

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

	if( rotated_out )
		*rotated_out = rotated;
	if( mins_out )
		VectorCopy( mins, mins_out );

	return !nv2a_frustum_cull_box( mins, maxs );
}

static void nv2a_draw_brush_entity( const model_t *world, cl_entity_t *e, float blend )
{
	const model_t *clmodel = e->model;
	vec3_t mins, modelorg, delta;
	vec3_t forward, right, up;
	float object[4][4], mvp[4][4];
	qboolean rotated;
	uint32_t *p;
	uint32_t bound_texture = 0xFFFFFFFFu;
	uint32_t anim_src = 0xFFFFFFFFu, anim_dir = 0xFFFFFFFFu; /* X9-X item 14 step 9: last face entry -> frame entry */
	qboolean anim_per_surface = false;
	qboolean alternate = e->curstate.frame != 0.0f; /* gl_rsurf.c:522: RI.currententity->curstate.frame */
	qboolean lit;
	int i;

	if( !nv2a_brush_entity_visible( world, e, &rotated, mins ))
		return;

	/* Everything below draws under the world pass's state and only
	   rebinds what differs per entity. Since a48b415e (X9-X step 5) this
	   runs from the entity lists, interleaved with studio models and after
	   pfnDrawNormalTriangles, and any TriAPI flush in between re-enters its
	   own program, turns stage 1 off and moves the attribute arrays onto
	   its ring - so a brush entity that followed a VISIBLE studio model
	   indexed the TriAPI ring with the tri program and did not appear,
	   while one that followed a culled studio model drew: doors, the tram
	   and grates came and went with camera angle and player position.
	   ref/gl's R_DrawBrushModel (gl_rsurf.c:1822) likewise sets up its
	   own draw state rather than inheriting the previous entity's. */
	if( !nv2a_lit_state_current )
		nv2a_enter_lit_state();

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

	if( !nv2a_set_render_mode( e, blend ))
	{
		if( !lit )
			nv2a_bind_lightmap_stage1( true );
		return;
	}

	/* X9-X step 11: R_DrawBrushModel's R_AllowFog( false ) for additive
	   entities (gl_rsurf.c:1884-1888), restored below. */
	if( e->curstate.rendermode == kRenderTransAdd )
		R_WarpAllowFog( false );
	nv2a_push_fog( &nv2a_fog_current );

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

		/* X9-X step 11: a water face goes to r_warp.c, drawn warped after
		   the entity's other faces (R_RenderBrushPoly's SURF_DRAWTURB
		   branch, gl_rsurf.c:1488). R_SortBrushModelSurfaces keeps its
		   CULL_BACKSIDE (no lightmapped water, gl_rsurf.c:1787-1793) and
		   R_DrawBrushModel draws it reversed (cull_type == CULL_BACKSIDE,
		   :1491), so it shows from below the surface too - tested here
		   whatever nv2a_r_nocullback says, since that only stands in for
		   dropping faces. A face the engine could not subdivide (no
		   pieces) draws unwarped below, as before. */
		if( FBitSet( psurf->flags, NV2A_SURF_DRAWTURB ) && psurf->polys )
		{
			qboolean reverse = false;

			if( !nv2a_r_nocull.value && !VectorIsNull( psurf->plane->normal ))
			{
				float dist = PlaneDiff( modelorg, psurf->plane );

				if( FBitSet( psurf->flags, NV2A_SURF_PLANEBACK ))
					dist = -dist;
				reverse = dist <= BACKFACE_EPSILON;
			}
			R_WarpAddBrushSurface( (msurface_t *)psurf, reverse );
			continue;
		}

		/* R_CullSurface's CULL_BACKSIDE, against the model-space eye.
		   X9-X item 14 step 12: R_SortBrushModelSurfaces keeps a back face
		   of a kRenderTransTexture entity when it has decals
		   (gl_rsurf.c:1796) and R_RenderBrushPoly draws those decals in
		   reverse (gl_rsurf.c:1396). The face itself is not drawn here:
		   ref/gl's GL_FRONT culling drops it. Whether a face is a back
		   face for its decals does not depend on nv2a_r_nocullback, which
		   only decides whether the CPU drops back faces before the GPU. */
		if( !nv2a_decal_entity_surface_front( psurf, modelorg ))
		{
			if( psurf->pdecals && e->curstate.rendermode == kRenderTransTexture )
				R_DecalAddBrushSurface( e, (msurface_t *)psurf, true );
			if( !nv2a_r_nocullback.value )
				continue;
		}
		else if( psurf->pdecals )
		{
			/* R_RenderBrushPoly's R_RenderDecalsForSurface (gl_rsurf.c:1384) */
			R_DecalAddBrushSurface( e, (msurface_t *)psurf, false );
		}

		if( e->curstate.rendermode != kRenderTransColor ) /* stage 0 is already the flat colour page - real faces never bound */
		{
			uint32_t draw_dir;

			if( face->texture >= nv2a_map_texture_count )
				continue;
			/* X9-X item 14 step 9: R_TextureAnimation, resolved once per
			   run of faces sharing an entry (per face for random tiles). */
			if( face->texture != anim_src )
			{
				anim_src = face->texture;
				anim_per_surface = nv2a_texanim_dir_per_surface( anim_src, alternate );
				anim_dir = anim_per_surface ? anim_src : nv2a_texanim_dir_for( anim_src, alternate, NULL );
			}
			draw_dir = anim_per_surface ? nv2a_texanim_dir_for( anim_src, alternate, psurf ) : anim_dir;
			if( draw_dir != bound_texture )
			{
				nv2a_bind_map_texture_stage0( &nv2a_map_textures[draw_dir] );
				bound_texture = draw_dir;
			}
		}
		if( nv2a_face_vertex_base( face ) != nv2a_bound_vertex_base ) /* X9-S: entity faces may sit in any chunk */
			nv2a_bind_vertex_base( nv2a_face_vertex_base( face ));
		nv2a_draw_exact_indices( face->index_begin, nv2a_face_index_end( face ));
	}

	/* X9-X item 14 step 12: the entity's decals, under its render mode
	   and matrix (R_DrawBrushModel's DrawDecalsBatch, gl_rsurf.c:1914,
	   and the DrawSurfaceDecals R_RenderDecalsForSurface calls for a
	   non-kRenderNormal entity - here after its faces, r_decals.c). */
	R_DecalDrawBrushEntity( e, blend );

	nv2a_reset_render_mode();
	if( !lit )
		nv2a_bind_lightmap_stage1( true );

	if( e->curstate.rendermode == kRenderTransAdd )
		R_WarpAllowFog( true );

	/* X9-X step 11: the entity's water faces, queued above, through the
	   TriAPI - which must fully re-enter its own state after this
	   function's direct pushes (lessons.md: a draw that depends on state
	   it never sets). */
	nv2a_2d_dirty = true;
	XBOX_FT_SPAN( XBOX_FT_S_WARP_BRUSH, R_WarpDrawBrushEntity( e, blend, rotated ));
}

/* Brush's own direct constant-96 push (nv2a_draw_brush_entity) leaves the
   TriAPI/world's shared transform constant pointed at that ENTITY's own
   matrix - restores it to the world's (ref/gl's own equivalent restore,
   this function's previous single-block form). Only needed once a brush
   entity actually drew (a studio-only frame never touches constant 96 by
   this path at all - R_DrawStudioModel's own pushes go through the
   TriAPI's nv2a_2d_flush, keyed off nv2a_map_viewproj/the view-model
   matrix already, never this one). */
static void nv2a_restore_world_constant96( void )
{
	uint32_t *p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 16 );
	memcpy( p, nv2a_map_viewproj, 16 * 4 );
	p += 16;
	pb_end( p );
}

/* ref/gl's R_DrawEntitiesOnList (gl_rmain.c), the solid-entity half:
   brush entities draw directly (their own GPU state push,
   nv2a_draw_brush_entity - flushed/marked dirty around it per spec Part B
   item 4, so a studio entity sharing the SAME TriAPI ring never inherits
   a stale batch), studio entities through R_DrawStudioModel (r_studio.c).
   Called from GL_RenderFrame BEFORE pfnDrawNormalTriangles, matching
   gl_rmain.c:797-830's own order (X9-X step 5 - was brush-only,
   nv2a_draw_brush_entities, called from inside nv2a_draw_map_geometry;
   moved here per spec Part B item 1). */
static void nv2a_draw_solid_entities( const model_t *world )
{
	qboolean can_draw_brush = ( world != NULL && nv2a_map_faces != NULL && nv2a_map_face_remap != NULL && nv2a_map_face_orig != NULL );
	qboolean drew_brush = false;
	int i;

	for( i = 0; i < nv2a_num_solid_entities; i++ )
	{
		cl_entity_t *e = nv2a_solid_entities[i];

		if( e->model->type == mod_studio )
		{
			nv2a_studio_set_blend( 1.0f );
			nv2a_studio_begin_entity( e );
			R_DrawStudioModel( e );
		}
		else if( e->model->type == mod_brush && can_draw_brush )
		{
			nv2a_2d_flush();
			XBOX_FT_SPAN( XBOX_FT_S_BRUSH, nv2a_draw_brush_entity( world, e, 1.0f ));
			nv2a_2d_dirty = true;
			drew_brush = true;
		}
	}

	if( drew_brush )
		nv2a_restore_world_constant96();

	/* X9-X step 6: "draw sprites seperately, because of alpha blending"
	   (gl_rmain.c:837-856) - a second walk of the solid list, after every
	   brush and studio model in it. tr.blend is R_DrawEntitiesOnList's
	   initial 1.0 here (gl_rmain.c:799): a solid-list entity is
	   kRenderNormal, which nothing in the solid pass changes it for. */
	for( i = 0; i < nv2a_num_solid_entities; i++ )
	{
		cl_entity_t *e = nv2a_solid_entities[i];

		if( e->model->type == mod_sprite )
		{
			R_SpriteSetBlend( 1.0f );
			R_DrawSpriteModel( e );
		}
	}
}

/* The translucent half of the same reference function, sorted by
   R_TransEntityCompare, each with blend = CL_FxBlend/255 unless its
   rendermode is kRenderNormal ("draw as solid but sorted by distance").
   Called from GL_RenderFrame AFTER pfnDrawNormalTriangles, BEFORE
   pfnDrawTransparentTriangles (gl_rmain.c:865-912's own order). */
static void nv2a_draw_trans_entities( const model_t *world )
{
	qboolean can_draw_brush = ( world != NULL && nv2a_map_faces != NULL && nv2a_map_face_remap != NULL && nv2a_map_face_orig != NULL );
	qboolean drew_brush = false;
	int i;

	if( nv2a_num_trans_entities == 0 )
		return;

	qsort( nv2a_trans_entities, nv2a_num_trans_entities, sizeof( cl_entity_t * ), nv2a_trans_entity_compare );

	for( i = 0; i < nv2a_num_trans_entities; i++ )
	{
		cl_entity_t *e = nv2a_trans_entities[i];
		float blend;

		if( e->curstate.rendermode != kRenderNormal )
			blend = nv2a_fx_blend( e ) / 255.0f;
		else blend = 1.0f; // draw as solid but sorted by distance

		if( blend <= 0.0f )
			continue;

		if( e->model->type == mod_studio )
		{
			nv2a_studio_set_blend( blend );
			nv2a_studio_begin_entity( e );
			R_DrawStudioModel( e );
		}
		else if( e->model->type == mod_sprite )
		{
			R_SpriteSetBlend( blend ); /* X9-X step 6, gl_rmain.c:901-903 */
			R_DrawSpriteModel( e );
		}
		else if( e->model->type == mod_brush && can_draw_brush )
		{
			nv2a_2d_flush();
			XBOX_FT_SPAN( XBOX_FT_S_BRUSH, nv2a_draw_brush_entity( world, e, blend ));
			nv2a_2d_dirty = true;
			drew_brush = true;
		}
	}

	if( drew_brush )
		nv2a_restore_world_constant96();
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
	uint32_t i, b;
	qboolean world_alternate;
	xbox_ft_span_t walk_span; /* k_world_walk: the batch/face walk below */
	/* R3 (#146): the per-face tests' switches, read once - nothing the
	   walk calls sets a cvar or rebuilds the PVS - and the frustum's
	   planes in the walk's form. */
	qboolean pvs_on, use_list, frustum_on, backface_on;
	nv2a_cull_plane_t cull_planes[5];
	uint32_t cull_plane_count;
	/* X9-H: the engine's own already-loaded world model, read directly
	   for its real per-surface plane/flags (backface test below) - the
	   SAME model_t X9-G's nv2a_mark_leaves() already trusts, guarded the
	   same way (world == nv2a_map_owner, never dereferenced otherwise).
	   NULL here just means "no backface test this call, draw whatever
	   frustum+PVS already allowed" - fail open, same discipline as every
	   other optional cull test in this function. */
	const model_t *world = gp_cl->models[1];

	nv2a_enter_lit_state();

	if( world != nv2a_map_owner )
		world = NULL;

	/* X9-X item 14 step 9: R_DrawWorld sets RI.currententity to the world
	   entity (gl_rsurf.c R_DrawWorld); its frame selects the alternate
	   textures for world faces, as for any brush entity. */
	{
		const cl_entity_t *world_ent = nv2a_get_entity_by_index( 0 );
		world_alternate = world_ent && world_ent->curstate.frame != 0.0f;
	}

	nv2a_cull_faces_total = nv2a_map_face_count;
	nv2a_cull_faces_visible = 0;
	nv2a_cull_runs = 0;
	nv2a_cull_dwords = 0;
	nv2a_cull_faces_backface_only = 0;

	xbox_ft_span_begin( &walk_span );

	/* X9-G's PVS test: no table, or nv2a_r_novis, culls nothing. With the
	   test on and a list built for this world, the walk visits the list
	   (R3, #146): the faces the PVS, sky and world tests would let through,
	   in the same order, so every later test, every run and every draw is
	   the same. X9-F's frustum test is off under nv2a_r_nocull
	   (gl_frustum.c:106), and X9-H's back-face test under either
	   nv2a_r_nocullback or nv2a_r_nocull (gl_cull.c:76), or without the
	   world or its face-order table. */
	pvs_on = nv2a_map_face_visible != NULL && !nv2a_r_novis.value;
	use_list = pvs_on && world != NULL && nv2a_pvs_faces_world == world;
	frustum_on = !nv2a_r_nocull.value;
	backface_on = world != NULL && !nv2a_r_nocullback.value && !nv2a_r_nocull.value && nv2a_map_face_orig != NULL;
	cull_plane_count = nv2a_frustum_prepare_planes( cull_planes );

	/* X9-K: one texture bind + one cull/run-merge pass PER BATCH, in
	   directory order - see this function's own top-of-function comment
	   for why a per-texture batch makes this restructuring necessary
	   (there is no more single shared atlas to bind once and cull over in
	   one sweep). The diagnostics above are frame-level totals,
	   accumulated ACROSS every batch, matching how they already summed
	   across the whole map before X9-K. */
	for( b = 0; b < nv2a_map_batch_count; b++ )
	{
		const nv2a_map_batch_t *batch = &nv2a_map_batches[b]; /* textureIndex validated < nv2a_map_texture_count at load */

		/* X9-O: nv2a_r_nocull no longer takes this path. It used to, and
		   the whole-batch draw below ignores everything the per-face path
		   applies - sky skipping, the world/brush-entity split, PVS - so
		   "culling off" drew every trigger volume and every door frozen at
		   its compile-time position. It now only disables the frustum and
		   backface tests, like ref/gl's r_nocull (gl_frustum.c:106,
		   gl_cull.c:76); this path is left for a map with no usable
		   per-face table. */
		if( nv2a_map_faces == NULL
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
				/* X9-S: a batch lies inside one vertex chunk; without a
				   usable per-face table only single-chunk maps load at all,
				   so base 0 is then correct. */
				uint32_t base = ( nv2a_map_faces && batch->startFace < nv2a_map_face_count )
					? nv2a_face_vertex_base( &nv2a_map_faces[batch->startFace] ) : 0;
				if( base != nv2a_bound_vertex_base )
					nv2a_bind_vertex_base( base );
				/* X9-X item 14 step 9: no per-face table here, so the '-'
				   random tiles stay on the face's own tile (R_TextureAnim). */
				nv2a_bind_map_texture_stage0( &nv2a_map_textures[nv2a_texanim_dir_for( batch->textureIndex, world_alternate, NULL )] );
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
			/* X9-X item 14 step 9: the entry this batch draws now
			   (resolved at its first surviving face, so a batch nothing
			   survives in costs no lookup), and the one bound. A '-'
			   random-tile batch resolves per face instead, and a new
			   entry ends the current run. */
			uint32_t batch_dir = NV2A_TEXANIM_NONE, bound_dir = NV2A_TEXANIM_NONE;
			qboolean per_surface = false;

			/* R3 (#146): the batch's slice of the visible-face list, or
			   every face of the batch. */
			const uint16_t *list = NULL;
			uint32_t k, count;

			if( use_list )
			{
				uint32_t first = b ? nv2a_pvs_batch_end[b - 1] : 0;

				list = &nv2a_pvs_faces[first];
				count = nv2a_pvs_batch_end[b] - first;
			}
			else
			{
				count = batch->faceCount;
			}

			NV2A_FT_ADD( XBOX_FT_W_WORLD_WALKED, count );
			for( k = 0; k < count; k++ )
			{
				const nv2a_map_face_t *face;

				i = list ? list[k] : batch->startFace + k;
				face = &nv2a_map_faces[i];

				/* The listed faces have passed the next three tests
				   already (nv2a_build_pvs_faces). */
				if( !list )
				{
					/* X9-K: a sky face is never drawn by this pass - see
					   nv2a_map_face_t::sky; nv2a_draw_sky draws the
					   skybox for it. Checked first - it's a single struct
					   field. */
					if( face->sky )
						continue;

					/* X9-M: brush-entity faces (func_door, func_wall,
					   ladders, triggers...) belong to the entity pass,
					   which draws them at the entity's own origin/angles
					   and render mode - see nv2a_draw_brush_entity().
					   ref/gl never draws them from the world either:
					   R_RecursiveWorldNode draws each world node's own
					   surface range (gl_rsurf.c:3507-3527; leaves only
					   mark visibility), and world nodes cover only the
					   world model's faces. Without the remap table there
					   is no way to tell, so the world pass keeps drawing
					   everything (fail open, never missing geometry). */
					if( !nv2a_map_face_is_world( world, i ))
						continue;

					/* X9-G: PVS. R3 (#146): before the frustum test, which
					   cost a face the PVS had already rejected a five-plane
					   box test; both are pure, so the order changes no
					   result. ref/gl rejects such a surface with one
					   compare (CULL_VISFRAME, gl_cull.c:70-71), or never
					   reaches it (gl_rsurf.c:3463). */
					if( pvs_on && !nv2a_map_face_visible[i] )
						continue;
				}

				/* X9-F: the frustum (GL_FrustumCullBox, gl_frustum.c:102,
				   per face here rather than per node). */
				if( frustum_on && nv2a_frustum_face_outside( cull_planes, cull_plane_count, face ))
					continue;

				/* X9-H: R_CullSurface's back-face test (gl_cull.c:83-110),
				   last because it is the only one that reads outside this
				   map's own arrays. `i` is a SORTED position, not the
				   ORIGINAL BSP index world->surfaces is ordered by - X9-K's
				   nv2a_map_face_orig translates it (see that table's
				   declaration); R3 reads the plane through
				   nv2a_map_face_plane, filled from the same surfaces. */
				if( backface_on )
				{
					uint32_t ref = nv2a_map_face_plane_world == world ? nv2a_map_face_plane[i] : NV2A_FACE_PLANE_SURF;
					const mplane_t *plane = NULL;
					qboolean back = false;

					if( ref == NV2A_FACE_PLANE_SURF )
					{
						uint32_t origIdx = nv2a_map_face_orig[i];

						if( origIdx < (uint32_t)world->numsurfaces )
						{
							const msurface_t *surf = &world->surfaces[origIdx];

							if( !VectorIsNull( surf->plane->normal ))
							{
								plane = surf->plane;
								back = FBitSet( surf->flags, NV2A_SURF_PLANEBACK ) != 0;
							}
						}
					}
					else if( ref != NV2A_FACE_PLANE_NONE )
					{
						plane = &world->planes[ref & ~NV2A_FACE_PLANE_BACK];
						back = ( ref & NV2A_FACE_PLANE_BACK ) != 0;
					}

					if( plane != NULL )
					{
						float dist = PlaneDiff( nv2a_eye_origin, plane );

						if( back )
							dist = -dist;

						if( dist <= BACKFACE_EPSILON )
						{
							nv2a_cull_faces_backface_only++;
							continue;
						}
					}
				}

				/* X9-X item 14 step 11: a water face is drawn warped by
				   r_warp.c after the world (R_DrawTextureChains' SURF_DRAWTURB
				   chains, gl_rsurf.c:1553-1566), not from its baked, unwarped
				   triangles - once the engine subdivided it (polys set) and
				   agrees it is water, and it has a directory texture to bind
				   (else it keeps its baked draw). Skipping it ends the run. */
				if( face->turb && face->texture < nv2a_map_texture_count && world != NULL && nv2a_map_face_orig != NULL && nv2a_map_face_orig[i] < (uint32_t)world->numsurfaces )
				{
					msurface_t *surf = &world->surfaces[nv2a_map_face_orig[i]];

					if( FBitSet( surf->flags, NV2A_SURF_DRAWTURB ) && surf->polys )
					{
						R_WarpAddWorldSurface( surf );
						continue;
					}
				}

				nv2a_cull_faces_visible++;

				/* X9-X item 14 step 9: which frame this face shows. */
				{
					uint32_t face_dir;

					if( batch_dir == NV2A_TEXANIM_NONE )
					{
						per_surface = nv2a_texanim_dir_per_surface( batch->textureIndex, world_alternate );
						batch_dir = per_surface ? batch->textureIndex : nv2a_texanim_dir_for( batch->textureIndex, world_alternate, NULL );
					}
					face_dir = batch_dir;
					if( per_surface && world != NULL && nv2a_map_face_orig != NULL && nv2a_map_face_orig[i] < (uint32_t)world->numsurfaces )
						face_dir = nv2a_texanim_dir_for( batch->textureIndex, world_alternate, &world->surfaces[nv2a_map_face_orig[i]] );

					if( have_run && face_dir != bound_dir )
					{
						nv2a_draw_exact_indices( run_begin, run_end );
						nv2a_cull_runs++;
						nv2a_cull_dwords += ( run_end - run_begin + 1 ) / 2;
						have_run = false;
					}
					if( stage0_bound && face_dir != bound_dir )
					{
						nv2a_bind_map_texture_stage0( &nv2a_map_textures[face_dir] );
						bound_dir = face_dir;
					}
					else if( !stage0_bound )
					{
						bound_dir = face_dir; /* bound below, with the vertex base, before the first draw */
					}
				}

				/* X9-M: runs are EXACT index ranges now (they were dword
				   ranges snapped out to 6 indices) - a snapped range could
				   reach a neighbouring brush-entity face's triangles, which
				   must never be drawn from the world. */
				if( have_run && face->index_begin == run_end )
				{
					run_end = nv2a_face_index_end( face );
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
						nv2a_bind_map_texture_stage0( &nv2a_map_textures[bound_dir] ); /* X9-X item 14 step 9: the frame, not always tex */
						/* X9-S: the baker never lets a batch span two vertex
						   chunks, so one base bind per batch covers every run. */
						if( nv2a_face_vertex_base( face ) != nv2a_bound_vertex_base )
							nv2a_bind_vertex_base( nv2a_face_vertex_base( face ));
						stage0_bound = true;
					}
					run_begin = face->index_begin;
					run_end = nv2a_face_index_end( face );
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
	xbox_ft_span_end( &walk_span, XBOX_FT_S_WORLD_WALK );

	/* X9-X item 14 step 11: the world's water, queued above, through the
	   TriAPI - a full re-entry after the direct pushes above, submitted
	   before anything else draws, as ref/gl draws it inside
	   R_DrawTextureChains. Translucent water stays queued for
	   R_WarpDrawWaterSurfaces. */
	nv2a_2d_dirty = true;
	XBOX_FT_SPAN( XBOX_FT_S_WARP_WORLD, { R_WarpDrawWorld(); nv2a_2d_flush(); } );

	/* X9-X step 5, spec Part B item 1: brush/studio entities no longer
	   draw from inside this function - GL_RenderFrame now calls
	   nv2a_draw_solid_entities/nv2a_draw_trans_entities itself, at the
	   points in its own draw order gl_rmain.c's R_DrawEntitiesOnList
	   actually calls them (before/after pfnDrawNormalTriangles). */
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

/* X9-X step 5: R_StudioFillAPI/R_StudioSetDrawInterface are now real,
   implemented in r_studio.c and declared by r_studio.h (included above) -
   assigned into gReffuncs by name below, nothing to do here. */

static void R_GetDetailScaleForTexture( int texture, float *xScale, float *yScale )
{
	if( xScale ) *xScale = 1.0f;
	if( yScale ) *yScale = 1.0f;
}

static void R_SetDetailScaleForTexture( int texture, float xScale, float yScale )
{
	;
}

/* ref/gl's R_FillRenderAPI (gl_context.c:440) fills the render_api_t
   custom renderers (XashXT-style mods) call; Half-Life's client never
   does. X9-X item 14 step 12: R_EntityRemoveDecals (gl_context.c:451), the
   one entry this renderer implements. The GL-only ones (texture arrays,
   texture matrices, DrawSingleDecal and R_DecalSetupVerts, which draw
   outside this renderer's own state) stay NULL. */
static void R_FillRenderAPI( render_api_t *api )
{
	api->R_EntityRemoveDecals = R_EntityRemoveDecals;
}

/* Defined later in the file, alongside the rest of X9-X step 4 Part E -
   forward-declared here only so R_FillTriAPI (used well before that point
   by nothing else, but kept at its own long-established spot in the
   ref_interface_t stub group) can reference them. */
void TexCoord2f( float u, float v ); /* X9-X step 5: TriAPI internals below exposed for r_studio.c - r_studio_bridge.h */
static void Fog( float flFogColor[3], float flStart, float flEnd, int bOn );
static void ScreenToWorld( const float *screen, float *world );
static void GetMatrix( const int pname, float *matrix );
static void FogParams( float flDensity, int iFogSkybox );

/* X9-X step 4 Part E: R_FillTriAPI fills the 5 slots ref/gl's own does
   (gl_context.c:466-473) - Begin/End/Vertex3f/Vertex3fv/RenderMode/
   Color4f/Color4ub/CullFace/SpriteTexture/WorldToScreen/BoxInPVS/
   LightAtPoint/Color4fRendermode are either the ENGINE's own
   (CL_FillTriAPI, ref_common.c:473-492) or wired straight from
   gReffuncs by that same function - not this one's job. */
static void R_FillTriAPI( triangleapi_t *api )
{
	if( !api )
		return;

	api->TexCoord2f    = TexCoord2f;
	api->Fog           = Fog;
	api->ScreenToWorld = ScreenToWorld;
	api->GetMatrix     = GetMatrix;
	api->FogParams     = FogParams;
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
	nv2a_num_solid_entities = 0;
	nv2a_num_trans_entities = 0;
	nv2a_num_beam_entities = 0;
	nv2a_pending_upload.pending = false; /* X9-T */
	nv2a_tri_bind_map_texture( NV2A_TRI_NO_MAP_TEXTURE ); /* X9-X step 11: the directory is about to go */
	R_WarpFreeMap();
	nv2a_map_ready = false;
	nv2a_map_owner = NULL;
	nv2a_map_vert_count = 0;
	nv2a_map_dword_count = 0;
	nv2a_map_face_count = 0;
	nv2a_map_texture_count = 0;
	nv2a_map_batch_count = 0;
	nv2a_map_face_remap_count = 0;
	nv2a_map_chunk_count = 0; /* X9-U */
	/* X9-U: the per-map tables come from malloc, not the zone: Mem_Malloc
	   calls Sys_Error when it runs out (engine/common/zone.c), and a map
	   too big for what is free must fall back to a frame with no world
	   (nv2a-xbox-specialist, X9-T review; rocket_frenzy ended the game at
	   its index buffer). */
	if( nv2a_map_vb )            { MmFreeContiguousMemory( nv2a_map_vb );            nv2a_map_vb            = NULL; }
	if( nv2a_map_index_dwords )  { free( nv2a_map_index_dwords );                nv2a_map_index_dwords  = NULL; }
	if( nv2a_map_faces )         { free( nv2a_map_faces );                       nv2a_map_faces         = NULL; }
	if( nv2a_map_face_visible )  { free( nv2a_map_face_visible );                nv2a_map_face_visible  = NULL; }
	if( nv2a_pvs_faces )         { free( nv2a_pvs_faces );                       nv2a_pvs_faces         = NULL; }
	if( nv2a_map_face_plane )    { free( nv2a_map_face_plane );                  nv2a_map_face_plane    = NULL; }
	if( nv2a_map_face_listable ) { free( nv2a_map_face_listable );               nv2a_map_face_listable = NULL; }
	nv2a_pvs_faces_world = NULL;      /* R3 (#146): the world pointer can come back for */
	nv2a_map_face_plane_world = NULL; /* the next map (fixed mod_known[0] storage, below) */
	if( nv2a_map_texture_data )  { MmFreeContiguousMemory( nv2a_map_texture_data );  nv2a_map_texture_data  = NULL; }
	if( nv2a_map_face_remap )    { free( nv2a_map_face_remap );                  nv2a_map_face_remap    = NULL; }
	if( nv2a_map_face_orig )     { free( nv2a_map_face_orig );                   nv2a_map_face_orig     = NULL; }
	if( nv2a_map_face_lm )       { free( nv2a_map_face_lm );                     nv2a_map_face_lm       = NULL; }
	R_LightmapFreeMap(); /* X9-X step 8: its per-map state describes the atlas freed above */
	if( nv2a_map_lightmap )      { MmFreeContiguousMemory( nv2a_map_lightmap );      nv2a_map_lightmap      = NULL; }

	/* X9-G: both point into the outgoing world model's own leafs[] array
	   (freed by the engine as part of this same unload) - clearing them
	   here, on every unload, is what keeps nv2a_mark_leaves() from ever
	   comparing against or dereferencing a dangling pointer left over
	   from a previous map. */
	nv2a_viewleaf = NULL;
	nv2a_oldviewleaf = NULL;
	nv2a_visbytes_valid = false; /* X9-X step 7: Mod_GetCurrentVis fails open until the next map's first R_FatPVS */

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
	nv2a_texanim_reset(); /* X9-X item 14 step 9: same fixed-storage reason as the line above */
}

static qboolean R_Init( void )
{
	qboolean video_ok;

	debug_marker = 1;
	nv2a_view_valid = false; /* X9-E: no real GL_RenderFrame call yet this init */
	/* X9-X step 4 Part E: GL's own initial matrix-stack state (identity on
	   both stacks) until the first real GL_RenderFrame fills these for
	   real - matches nv2a_view_valid's own reset discipline above. */
	Matrix4x4_LoadIdentity( nv2a_gl_worldview );
	Matrix4x4_LoadIdentity( nv2a_gl_projection );
	Matrix4x4_LoadIdentity( nv2a_gl_worldviewproj );

	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_nocull );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_novis );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_lockpvs );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_nocullback );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_noalphatest );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_texturenearest );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_anisotropy );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_lodbias );
	gEngfuncs.Cvar_RegisterVariable( &nv2a_r_2d_pixeloffset );

	/* X9-X item 14 step 9: the random-tiling table R_TextureAnimation
	   reads (gl_rsurf.c:536), filled where ref/gl's R_Init fills it
	   (gl_opengl.c:1250). */
	GL_InitRandomTable();

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

	/* divergences.md #143: every vertex program, once (nv2a_vp_table).
	   pb_init has just reset the GPU, so nothing is selected yet. */
	nv2a_upload_vertex_programs();
	nv2a_ps_selected = NV2A_PS_NONE;
	nv2a_resident_program = NV2A_RESIDENT_NONE;
	nv2a_sgpu_entered = false;

	nv2a_width  = pb_back_buffer_width();
	nv2a_height = pb_back_buffer_height();

	/* X9-X D1: the texture manager's own arena/table, and the 2D pass's
	   own vertex ring - both allocated once, here, before any map. */
	nv2a_2d_ring = MmAllocateContiguousMemoryEx( NV2A_2D_VERTEX_RING_BYTES, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_2d_ring )
	{
		debug_marker = 8;
		pb_kill();
		pb_initialized = false;
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	/* X9-X item 14 step 12: the decal vertex buffer. Without it decals are
	   placed and saved but not drawn (nv2a_decal_gpu_begin). */
	nv2a_decal_vb = MmAllocateContiguousMemoryEx( NV2A_DECAL_FRAME_VERTS * sizeof( nv2a_lit_vertex_t ), 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_decal_vb )
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: no memory for the decal vertex buffer - decals will not draw\n" );
	nv2a_decal_vb_used = 0;

	if( !nv2a_tex_init())
	{
		debug_marker = 9;
		pb_kill();
		pb_initialized = false;
		MmFreeContiguousMemory( nv2a_2d_ring );
		nv2a_2d_ring = NULL;
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	/* divergences.md #131: the NV2A samples DXT1/3/5 natively, so tell
	   imagelib to hand compressed DDS data through instead of rejecting it
	   (img_dds.c Image_LoadDDS: "silently rejected" without this flag) -
	   the same call ref/gl makes when the GPU reports S3TC
	   (gl_opengl.c:1108-1109). nv2a_tex_upload_compressed takes it from
	   there; the baked menu background tiles are its only user today. */
	gEngfuncs.Image_AddCmdFlags( IL_DDS_HARDWARE );

	/* X9-X step 5, design note section 5: the studio state (heap-allocated,
	   R_AllocStudioStatics pattern) - allocated last, right before success,
	   so every earlier failure path above is unaffected. */
	if( !R_StudioAllocStatics())
	{
		debug_marker = 10;
		pb_kill();
		pb_initialized = false;
		nv2a_tex_shutdown();
		MmFreeContiguousMemory( nv2a_2d_ring );
		nv2a_2d_ring = NULL;
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}
	R_StudioInit();
	nv2a_sgpu_cache_init(); /* divergences.md #143: bookkeeping only, nothing backed until a build */
	R_SpriteInit(); /* X9-X step 6: r_traceglow and the view entity (r_sprite.h) */
	R_EfxInit(); /* X9-X step 7: the palette CL_DrawParticles reads (r_effects.h) */
	R_DecalInit(); /* X9-X item 14 step 12: gl_polyoffset and R_ClearDecals, gl_opengl.c:1181,1282 */

	debug_marker = 5;
	nv2a_ready = true;
	return true;
}

static void R_Shutdown( void )
{
	nv2a_ready = false;

	R_StudioFreeStatics();

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

	/* X9-X D7: every texture slot, then the arena/table, after pb_kill -
	   the GPU is idle by now, matching nv2a_tex_shutdown()'s own comment. */
	nv2a_tex_shutdown();
	nv2a_sgpu_cache_shutdown(); /* divergences.md #143, GPU idle since pb_kill */
	R_SkyShutdown(); /* X9-X item 14 step 10: its texnums were slots of that table */
	R_WarpShutdown(); /* X9-X item 14 step 11 */
	if( nv2a_2d_ring )
	{
		MmFreeContiguousMemory( nv2a_2d_ring );
		nv2a_2d_ring = NULL;
	}
	if( nv2a_decal_vb )
	{
		MmFreeContiguousMemory( nv2a_decal_vb );
		nv2a_decal_vb = NULL;
	}

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

/* X9-M/X9-X steps 5-7: ref/gl's R_AddEntity (gl_rmain.c:234). Brush,
   studio and sprite models are kept (this renderer's three draw paths)
   and beams go to their own list (step 7, gl_rmain.c:255-266); alias
   models (out of scope) are still accepted but not drawn - the
   accept/reject rules (r_drawentities, EF_NODRAW, fully faded
   translucents) and the solid/translucent split are the reference's
   own, unconditional on model type. */
static qboolean R_AddEntity( struct cl_entity_s *clent, int type )
{
	if( !r_drawentities->value )
		return false; // not allow to drawing

	if( FBitSet( clent->curstate.effects, EF_NODRAW ))
		return false; // done

	if( clent->curstate.rendermode != kRenderNormal && nv2a_fx_blend( clent ) <= 0 )
		return true; // invisible

	if( type == ET_BEAM )
	{
		if( nv2a_num_beam_entities >= NV2A_MAX_ENTITIES )
		{
			gEngfuncs.Con_Printf( S_ERROR "Too many beams %d!\n", nv2a_num_beam_entities );
			return false;
		}

		nv2a_beam_entities[nv2a_num_beam_entities] = clent;
		nv2a_num_beam_entities++;

		return true;
	}

	if( !clent->model || ( clent->model->type != mod_brush && clent->model->type != mod_studio && clent->model->type != mod_sprite ))
		return true; // accepted, but this renderer has no draw path for it yet (alias)

	if( nv2a_opaque_entity( clent ))
	{
		if( nv2a_num_solid_entities >= NV2A_MAX_ENTITIES )
			return false;
		nv2a_solid_entities[nv2a_num_solid_entities++] = clent;
	}
	else
	{
		if( nv2a_num_trans_entities >= NV2A_MAX_ENTITIES )
			return false;
		nv2a_trans_entities[nv2a_num_trans_entities++] = clent;
	}

	return true;
}

/* X9-M: ref/gl's R_ClearScene (gl_rmain.c:217). */
static void R_ClearScene( void )
{
	nv2a_num_solid_entities = 0;
	nv2a_num_trans_entities = 0;
	nv2a_num_beam_entities = 0;

	// clear the scene befor start new frame
	if( gEngfuncs.drawFuncs->R_ClearScene != NULL )
		gEngfuncs.drawFuncs->R_ClearScene();
}

/* X9-X step 5: ref/soft's own CL_GetEntityByIndex (r_local.h:513) reads
   tr.entities/tr.max_entities, filled here the same way ref/soft's own
   R_ProcessEntData does - R_StudioClientEvents/R_FindParentEntity/
   R_DrawStudioModel need the bounds-checked lookup. */
static cl_entity_t *nv2a_entities;
static unsigned int nv2a_max_entities;

cl_entity_t *nv2a_get_entity_by_index( int index )
{
	if( unlikely( index < 0 || (unsigned int)index >= nv2a_max_entities || !nv2a_entities ))
		return NULL;
	return &nv2a_entities[index];
}

/* X9-X step 5 Part C: applies/restores nv2a_studio_mesh_state()
   (nv2a_studio_util.h, host-tested) on nv2a_2d_desired - r_studio.c's own
   R_StudioDrawPoints calls these around every mesh (design note section
   6). Saving the pre-mesh snapshot (rather than hand-restoring individual
   fields) keeps this a plain apply/restore pair regardless of which
   fields nv2a_studio_mesh_state() happens to touch. */
/* nv2a_studio_util.h's own mirrors of nv_regs.h (kept host-buildable
   without pbkit) pinned against the real macros - this file DOES have
   nv_regs.h (pbkit.h, included above). */
STATIC_ASSERT( NV2A_STUDIO_SFACTOR_ONE == NV097_SET_BLEND_FUNC_SFACTOR_V_ONE, "NV2A_STUDIO_SFACTOR_ONE drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_STUDIO_SFACTOR_SRC_ALPHA == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA, "NV2A_STUDIO_SFACTOR_SRC_ALPHA drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_STUDIO_DFACTOR_ONE == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE, "NV2A_STUDIO_DFACTOR_ONE drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_STUDIO_DFACTOR_ONE_MINUS_SRC_ALPHA == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA, "NV2A_STUDIO_DFACTOR_ONE_MINUS_SRC_ALPHA drifted from nv_regs.h" );
/* nv2a_sprite_util.h's mirrors (X9-X step 6), same reason. */
STATIC_ASSERT( NV2A_SPRITE_SFACTOR_ZERO == NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO, "NV2A_SPRITE_SFACTOR_ZERO drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_SPRITE_SFACTOR_SRC_ALPHA == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA, "NV2A_SPRITE_SFACTOR_SRC_ALPHA drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_SPRITE_DFACTOR_ONE == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE, "NV2A_SPRITE_DFACTOR_ONE drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_SPRITE_DFACTOR_SRC_COLOR == NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR, "NV2A_SPRITE_DFACTOR_SRC_COLOR drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA, "NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA drifted from nv_regs.h" );
/* nv2a_texture.h's per-slot colour formats (r_image.c has no pbkit include). */
STATIC_ASSERT( NV2A_TEX_COLOR_LU_A1R5G5B5 == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5, "NV2A_TEX_COLOR_LU_A1R5G5B5 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_LU_R5G6B5 == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5, "NV2A_TEX_COLOR_LU_R5G6B5 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_LU_A8B8G8R8 == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8, "NV2A_TEX_COLOR_LU_A8B8G8R8 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_LU_A8 == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8, "NV2A_TEX_COLOR_LU_A8 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_LU_A8Y8 == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8, "NV2A_TEX_COLOR_LU_A8Y8 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_L_DXT1 == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, "NV2A_TEX_COLOR_L_DXT1 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_L_DXT23 == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8, "NV2A_TEX_COLOR_L_DXT23 drifted from nv_regs.h" );
STATIC_ASSERT( NV2A_TEX_COLOR_L_DXT45 == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8, "NV2A_TEX_COLOR_L_DXT45 drifted from nv_regs.h" );

static nv2a_2d_shadow_t nv2a_studio_pre_mesh_state;

void nv2a_studio_apply_mesh_state( int face_flags, int entity_opaque )
{
	nv2a_studio_mesh_state_t base, out;

	nv2a_studio_pre_mesh_state = nv2a_2d_desired;

	base.alpha_test_enable = nv2a_2d_desired.alpha_test_enable;
	base.alpha_ref         = nv2a_2d_desired.alpha_ref;
	base.blend_enable      = nv2a_2d_desired.blend_enable;
	base.sfactor           = nv2a_2d_desired.sfactor;
	base.dfactor           = nv2a_2d_desired.dfactor;
	base.depth_mask        = nv2a_2d_desired.depth_mask;

	out = nv2a_studio_mesh_state( face_flags, entity_opaque, base );

	nv2a_2d_desired.alpha_test_enable = out.alpha_test_enable;
	nv2a_2d_desired.alpha_ref         = out.alpha_ref;
	nv2a_2d_desired.blend_enable      = out.blend_enable;
	nv2a_2d_desired.sfactor           = out.sfactor;
	nv2a_2d_desired.dfactor           = out.dfactor;
	nv2a_2d_desired.depth_mask        = out.depth_mask;
}

void nv2a_studio_restore_mesh_state( void )
{
	nv2a_2d_desired = nv2a_studio_pre_mesh_state;
}

/* X9-X step 5 Part C / design note section 2. */
void nv2a_studio_set_viewmodel_depthrange( int on )
{
	nv2a_2d_desired.viewmodel_depthrange = on ? true : false;
}

static void R_ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	/* X9-M: CL_FreeEdicts (cl_game.c) calls this with allocate = false
	   right before freeing the edict array the entity lists point into;
	   R_ClearScene does not run again until the next active frame. */
	if( allocate )
	{
		nv2a_entities = entities;
		nv2a_max_entities = max_entities;
	}
	else
	{
		nv2a_num_solid_entities = 0;
		nv2a_num_trans_entities = 0;
		nv2a_num_beam_entities = 0;
		nv2a_entities = NULL;
		nv2a_max_entities = 0;
	}
}

/* X9-X D3: R_GetTextureOriginalBuffer, GL_LoadTextureFromBuffer and every
   other GL_*Texture* entry point are now real, implemented in r_image.c
   and declared by nv2a_texture.h (included above) - gReffuncs assigns
   them directly by that external name, nothing to do here. R_DrawStretchPic
   and FillRGBA (C3/C4) are real too, defined below beside the rest of the
   2D pass, so they can call its batching helpers without a forward
   declaration for every one of them. */

static void GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	;
}

/* X9-X item 14 step 10: R_SetupSky is real now, in r_sky.c (r_sky.h). */

/* GL enums TriGetMatrix/GetMatrix read (gl_export.h:63-64) - the one
   deliberate exception to "no new literal NV097 value outside nv_regs.h":
   these are not NV097 registers at all, they are the GL enum values the
   SDK client passes to pTriAPI->GetMatrix, cited here instead. */
#define NV2A_GL_MODELVIEW_MATRIX  0x0BA6
#define NV2A_GL_PROJECTION_MATRIX 0x0BA7

/* X9-X step 4 Part E: port of R_WorldToScreen (gl_rmain.c:137-163 /
   ref/soft's own r_main.c:214-235, byte-for-byte identical) onto
   nv2a_gl_worldviewproj - NDC x,y with no viewport and no y flip,
   screen[2]=0, exactly as the reference leaves it (TriWorldToScreen,
   gl_triapi.c:223-233, is a DIFFERENT, pixel-space function for ref/gl's
   own internal beam drawing - not ported here, out of scope per the
   spec). This single function serves BOTH ref_interface_t's own
   WorldToScreen (autoaim/cl_showents, cl_game.c:841/cl_scrn.c:184) and,
   through the engine's own passthrough wrapper (cl_game.c's
   TriWorldToScreen calling ref.dllFuncs.WorldToScreen directly), the
   TriAPI's own WorldToScreen slot - CL_FillTriAPI never calls this
   module's R_FillTriAPI for that slot, it wires the engine's own
   TriWorldToScreen wrapper there instead (ref_common.c:473-492's own
   TriAPI table), which in turn does nothing but call this. */
static int WorldToScreen( const vec3_t world, vec3_t screen )
{
	float w;

	if( !world || !screen )
		return true;

	screen[0] = nv2a_gl_worldviewproj[0][0] * world[0] + nv2a_gl_worldviewproj[0][1] * world[1] + nv2a_gl_worldviewproj[0][2] * world[2] + nv2a_gl_worldviewproj[0][3];
	screen[1] = nv2a_gl_worldviewproj[1][0] * world[0] + nv2a_gl_worldviewproj[1][1] * world[1] + nv2a_gl_worldviewproj[1][2] * world[2] + nv2a_gl_worldviewproj[1][3];
	w         = nv2a_gl_worldviewproj[3][0] * world[0] + nv2a_gl_worldviewproj[3][1] * world[1] + nv2a_gl_worldviewproj[3][2] * world[2] + nv2a_gl_worldviewproj[3][3];
	screen[2] = 0.0f;

	if( w < 0.001f )
		return true;

	screen[0] /= w;
	screen[1] /= w;
	return false;
}

/* X9-X step 6: r_sprite.c's TriWorldToScreen (gl_triapi.c:223) builds on
   this (r_studio_bridge.h). */
int nv2a_world_to_screen( const vec3_t world, vec3_t screen )
{
	return WorldToScreen( world, screen );
}

/* X9-X step 4 Part E: port of R_ScreenToWorld (gl_rmain.c:172-186), same
   in both real refs. triangleapi_t's own field, not ref_interface_t's -
   only CL_DrawEFX/particleman-adjacent SDK code would call it; nothing
   in this fork's own client build does yet. */
static void ScreenToWorld( const float *screen, float *world )
{
	matrix4x4 inv;
	float w;

	if( !world || !screen )
		return;

	Matrix4x4_Invert_Full( inv, nv2a_gl_worldviewproj );

	world[0] = screen[0] * inv[0][0] + screen[1] * inv[0][1] + screen[2] * inv[0][2] + inv[0][3];
	world[1] = screen[0] * inv[1][0] + screen[1] * inv[1][1] + screen[2] * inv[1][2] + inv[1][3];
	world[2] = screen[0] * inv[2][0] + screen[1] * inv[2][1] + screen[2] * inv[2][2] + inv[2][3];
	w        = screen[0] * inv[3][0] + screen[1] * inv[3][1] + screen[2] * inv[3][2] + inv[3][3];

	if( w != 0.0f )
	{
		world[0] /= w;
		world[1] /= w;
		world[2] /= w;
	}
}

/* X9-X step 4 Part E: port of TriGetMatrix (gl_triapi.c:305-309, a thin
   pglGetFloatv), returning what the CURRENT matrix stack would hold - the
   2D ortho/identity pair R_Set2DMode(true) itself loads (gl_draw.c:
   152-168) while nv2a_in2d, or the real 3D worldview/projection
   GL_RenderFrame filled otherwise. Any other pname leaves `matrix`
   untouched, as glGetFloatv does for an invalid enum. */
static void GetMatrix( const int pname, float *matrix )
{
	matrix4x4 m;

	if( !matrix )
		return;

	if( nv2a_in2d )
	{
		if( pname == NV2A_GL_PROJECTION_MATRIX )
		{
			Matrix4x4_CreateOrtho( m, 0.0f, (float)gpGlobals->width, (float)gpGlobals->height, 0.0f, -99999.0f, 99999.0f );
			Matrix4x4_ToArrayFloatGL( m, matrix );
		}
		else if( pname == NV2A_GL_MODELVIEW_MATRIX )
		{
			Matrix4x4_LoadIdentity( m );
			Matrix4x4_ToArrayFloatGL( m, matrix );
		}
		return;
	}

	if( pname == NV2A_GL_MODELVIEW_MATRIX )
		Matrix4x4_ToArrayFloatGL( nv2a_gl_worldview, matrix );
	else if( pname == NV2A_GL_PROJECTION_MATRIX )
		Matrix4x4_ToArrayFloatGL( nv2a_gl_projection, matrix );
}

/* X9-X step 4 Part E: Fog/FogParams - no-ops until X9-X item 14 step 11,
   now TriFog/TriFogParams ported in r_warp.c (divergence #138 closes
   #119's fog point). */
static void Fog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
	R_WarpTriFog( flFogColor, flStart, flEnd, bOn ); /* X9-X item 14 step 11: gl_triapi.c:266 */
}

static void FogParams( float flDensity, int iFogSkybox )
{
	R_WarpTriFogParams( flDensity, iFogSkybox ); /* gl_triapi.c:328 */
}

/*
X9-X item 14 step 12: VID_ScreenShot, ref/gl's (gl_backend.c:548) with
the back buffer in place of glReadPixels. The engine asks from V_PostRender,
after the HUD and before R_EndFrame (cl_view.c:573-576), so the frame is
still the back buffer pbkit is rendering to: the TriAPI's pending batch is
flushed and the GPU waited for, then the rows are read from
pb_back_buffer() - pbkit's LE_A8R8G8B8 surface (pbkit.c:61), bytes
B,G,R,X in memory, which is PF_BGRA_32 once X is forced to 255 (ref/soft
does the same, Get8888PixelAt, r_glblit.c:720: a 32-bit BMP keeps its
alpha, img_bmp.c:461). The rows are top-down already, so no IMAGE_FLIP_Y
(glReadPixels is bottom-up; ref/soft copies bottom-up then flips).

The shot types, as gl_backend.c:565-583: VID_SCREENSHOT and VID_SNAPSHOT
(direct paths) as read; VID_LEVELSHOT and VID_MINISHOT resampled to 480 or
200 lines high at the screen's aspect (640x480 -> 640x480, 267x200 -
the savegame thumbnail, save/<name>.bmp from `saveshot`, sv_save.c:1791);
VID_MAPSHOT to 1024x768, 8-bit. The size is the framebuffer's
(nv2a_width x nv2a_height), the picture actually rendered; ref/gl reads
gpGlobals->width x height rounded up to 4, the same 640x480 here.

Divergence (docs/r3d/divergences.md #139): VID_MINISHOT reads the buffer
at half size, each pixel the average of a 2x2 block, before the engine's
resample to 267x200 - 300 KiB of r_temppool instead of the full 1.2 MiB,
because saving happens mid-game on maps with little memory to spare and
the zone allocator stops the game (Sys_Error) rather than fail a shot. It
still reads every pixel. A full-size shot (screenshot, snapshot,
levelshot) takes the whole 1.2 MiB, but only when the player asks. The
write-combined framebuffer is uncached, 307200 separate 32-bit reads a
shot: estimated at tens of milliseconds, once per shot, not measured on
hardware. The aspect is the framebuffer's pixel ratio, right while the
output is 4:3 only.
*/
static qboolean VID_ScreenShot( const char *filename, int shot_type )
{
	uint flags = 0;
	int width = 0, height = 0;
	int src_w, src_h, step, x, y;
	const byte *fb;
	uint32_t pitch;
	rgbdata_t *r_shot;
	qboolean result;

	if( !nv2a_ready || !pb_initialized || !filename )
		return false;

	/* The frame's pending 2D/TriAPI batch, then everything pushed so far.
	   Outside a frame there is nothing to read: R_EndFrame's pb_finished()
	   has already moved pb_back_buffer() on to the next target. */
	if( !nv2a_frame_open )
		return false;
	nv2a_2d_flush_and_wait();

	fb = (const byte *)pb_back_buffer();
	pitch = pb_back_buffer_pitch();
	src_w = nv2a_width;
	src_h = nv2a_height;
	if( !fb || src_w <= 0 || src_h <= 0 )
		return false;

	step = ( shot_type == VID_MINISHOT ) ? 2 : 1;

	r_shot = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	r_shot->width = src_w / step;
	r_shot->height = src_h / step;
	r_shot->flags = IMAGE_HAS_COLOR;
	r_shot->type = PF_BGRA_32;
	r_shot->size = r_shot->width * r_shot->height * gEngfuncs.Image_GetPFDesc( r_shot->type )->bpp;
	r_shot->palette = NULL;
	r_shot->buffer = Mem_Malloc( r_temppool, r_shot->size );

	// get screen frame
	for( y = 0; y < (int)r_shot->height; y++ )
	{
		byte *dst = r_shot->buffer + (size_t)y * r_shot->width * 4;

		if( step == 1 )
		{
			const byte *src = fb + (size_t)y * pitch;

			for( x = 0; x < (int)r_shot->width; x++, src += 4, dst += 4 )
			{
				uint32_t px;

				memcpy( &px, src, 4 ); /* one uncached 32-bit read */
				px |= 0xFF000000u;
				memcpy( dst, &px, 4 );
			}
		}
		else
		{
			const byte *row0 = fb + (size_t)( y * 2 ) * pitch;
			const byte *row1 = row0 + pitch;

			for( x = 0; x < (int)r_shot->width; x++, dst += 4 )
			{
				uint32_t a, b, c, d;
				int k;

				memcpy( &a, row0 + x * 8, 4 );
				memcpy( &b, row0 + x * 8 + 4, 4 );
				memcpy( &c, row1 + x * 8, 4 );
				memcpy( &d, row1 + x * 8 + 4, 4 );
				for( k = 0; k < 3; k++ )
				{
					const int sh = k * 8;
					dst[k] = (byte)(((( a >> sh ) & 0xFF ) + (( b >> sh ) & 0xFF ) + (( c >> sh ) & 0xFF ) + (( d >> sh ) & 0xFF ) + 2 ) >> 2 );
				}
				dst[3] = 0xFF;
			}
		}
	}

	switch( shot_type )
	{
	case VID_SCREENSHOT:
		break;
	case VID_SNAPSHOT:
		gEngfuncs.fsapi->AllowDirectPaths( true );
		break;
	case VID_LEVELSHOT:
	case VID_MINISHOT:
		flags |= IMAGE_RESAMPLE;
		height = shot_type == VID_MINISHOT ? 200 : 480;
		width = Q_rint( height * ((double)src_w / src_h )); /* the screen's aspect, as ref/gl's r_shot->width / height */
		break;
	case VID_MAPSHOT:
		flags |= IMAGE_RESAMPLE|IMAGE_QUANTIZE;	// GoldSrc request overviews in 8-bit format
		height = 768;
		width = 1024;
		break;
	}

	gEngfuncs.Image_Process( &r_shot, width, height, flags, 0.0f );

	// write image
	result = gEngfuncs.FS_SaveImage( filename, r_shot );
	gEngfuncs.fsapi->AllowDirectPaths( false );			// always reset after store screenshot
	gEngfuncs.FS_FreeImage( r_shot );

	return result;
}

/* ref/gl's VID_CubemapShot (gl_backend.c:600) renders six extra views
   (R_DrawCubemapView) for the `envshot`/`skyshot` developer commands. This
   renderer draws one world view per frame (GL_RenderFrame keeps the
   first), so, as ref/soft (r_context.c:306), it has none: the engine
   prints "Unable to write". */
static qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	return false;
}

/* X9-X item 14 step 12: R_DecalShoot, R_DecalRemoveAll, R_CreateDecalList
   and R_ClearAllDecals are real, in r_decals.c (r_decals.h). */

/* X9-X step 5: R_StudioEstimateFrame/R_StudioLerpMovement are now real,
   implemented in r_studio.c (whole-function ports of gl_studio.c:561,498)
   and declared by r_studio.h. */

static void R_SetSkyCloudsTextures( int solidskyTexture, int alphaskyTexture )
{
	;
}

/* X9-X item 14 step 11: gl_rsurf.c:361, ported in r_warp.c. */
static void GL_SubdivideSurface( model_t *mod, msurface_t *fa )
{
	R_WarpSubdivideSurface( mod, fa );
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
   numbers this one function happens to compute) - see
   nv2a_load_map_geometry for the terms. The ledger is still not what is
   free: measured in xemu, ~32.8 MiB is free before any map loads, and the
   engine's own world structures and a map's precaches (~7-10 MiB) are
   resident by the time this check runs at R_NewMap (docs/r3d/fork-plan.md,
   X9 items 10 and 11). */
#define NV2A_MAP_BUDGET_BYTES ( 24u * 1024u * 1024u )

/* The baker's own fixed lightmap page width, tools/asset-studio/src/
   formats/BspParser.cpp's LM_PAGE_SIZE (1024). Every lu the baker emits
   is divided by exactly THIS constant, never by the archive's own header
   field, so nv2a_load_map_geometry's validation below must compare
   header->lightmapWidth against this literal (codex review: a page of
   another width would otherwise pass validation and still sample with
   the wrong divisor). lv is different since X9-R: it is divided by the
   atlas's own trimmed height, which the header does carry, so the height
   is only range-checked (4..NV2A_LM_MAX_HEIGHT). */
#define NV2A_LM_PAGE_SIZE 1024u

/* X9-T: the tallest lightmap page the baker ships (LightmapAtlasPacker.h
   LM_PAGE_MAX_HEIGHT) - one 1024-wide page grows up to this many rows
   instead of spilling onto a second layer, which this renderer never
   binds. 4096 is the NV2A's own texture size limit; IMAGE_RECT's height
   field is 16 bits (nv_regs.h NV097_SET_TEXTURE_IMAGE_RECT_HEIGHT). */
#define NV2A_LM_MAX_HEIGHT 4096u


/* Reads `bytes` at `offset` of `f` into `dst`; false on any short read. */
static qboolean nv2a_read_at( file_t *f, fs_offset_t offset, void *dst, fs_offset_t bytes )
{
	if( gEngfuncs.fsapi->Seek( f, offset, SEEK_SET ) != 0 )
		return false;
	return gEngfuncs.fsapi->Read( f, dst, (size_t)bytes ) == bytes;
}

/* X9-T: builds the vertex buffer, the texture data and the lightmap from
   the map archive on disk. Called from R_NewMap, after the engine freed
   its copy of the archive. The lightmap is allocated first: it is the one
   whose contiguous allocation failed under memory pressure (and it
   usually is the largest), so it gets the first pick. Vertices are read
   1,024 at a time and converted to nv2a_lit_vertex_t on the way in (pos,
   diffuse UV, and lightmap UV scaled to texels - LINEAR textures are
   addressed in texel units, docs/r3d/divergences.md #100). Any failure
   frees what was built and leaves the map not ready (no world is drawn),
   exactly as a failed load always has. */
static qboolean nv2a_upload_map_gpu_data( void )
{
	file_t *f;
	mpak_vertex_t *chunk;
	uint32_t done;
	qboolean ok = false;

	f = gEngfuncs.fsapi->Open( nv2a_pending_upload.name, "rb", false );
	if( !f )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: could not reopen %s for its GPU data\n", nv2a_pending_upload.name );
		return false;
	}
	if( gEngfuncs.fsapi->FileLength( f ) != nv2a_pending_upload.file_length )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: %s changed since its tables were read\n", nv2a_pending_upload.name );
		gEngfuncs.fsapi->Close( f );
		return false;
	}

	nv2a_map_lightmap = MmAllocateContiguousMemoryEx( (uint32_t)nv2a_pending_upload.lightmap_bytes, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	nv2a_map_texture_data = nv2a_map_lightmap
		? MmAllocateContiguousMemoryEx( (uint32_t)nv2a_pending_upload.texture_bytes, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE ) : NULL;
	nv2a_map_vb = nv2a_map_texture_data
		? MmAllocateContiguousMemoryEx( (uint32_t)nv2a_pending_upload.vb_bytes, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE ) : NULL;
	chunk = nv2a_map_vb ? malloc( 1024 * sizeof( mpak_vertex_t )) : NULL; /* malloc: Mem_Malloc cannot fail softly (zone.c Sys_Error) */
	if( !chunk )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map GPU buffer allocation failed (lightmap %u, textures %u, vertices %u bytes)\n",
			(unsigned int)nv2a_pending_upload.lightmap_bytes, (unsigned int)nv2a_pending_upload.texture_bytes,
			(unsigned int)nv2a_pending_upload.vb_bytes );
		goto done;
	}

	if( !nv2a_read_at( f, nv2a_pending_upload.lightmap_offset, nv2a_map_lightmap, nv2a_pending_upload.lightmap_bytes )
		|| !nv2a_read_at( f, nv2a_pending_upload.texture_offset, nv2a_map_texture_data, nv2a_pending_upload.texture_bytes ))
	{
		gEngfuncs.Con_Printf( "ref_nv2a: short read of %s's lightmap/texture data\n", nv2a_pending_upload.name );
		goto done;
	}

	for( done = 0; done < nv2a_pending_upload.vertex_count; )
	{
		nv2a_lit_vertex_t *dst = (nv2a_lit_vertex_t *)nv2a_map_vb + done;
		uint32_t n = nv2a_pending_upload.vertex_count - done, k;

		if( n > 1024 )
			n = 1024;
		if( !nv2a_read_at( f, nv2a_pending_upload.vertices_offset + (fs_offset_t)done * sizeof( mpak_vertex_t ),
			chunk, (fs_offset_t)n * sizeof( mpak_vertex_t )))
		{
			gEngfuncs.Con_Printf( "ref_nv2a: short read of %s's vertices\n", nv2a_pending_upload.name );
			goto done;
		}
		for( k = 0; k < n; k++ )
		{
			dst[k].pos[0] = chunk[k].x;
			dst[k].pos[1] = chunk[k].y;
			dst[k].pos[2] = chunk[k].z;
			/* uv0: already normalized by the texture's native size at bake
			   time, hardware-WRAP-addressed as is (X9-K). */
			dst[k].uv0[0] = chunk[k].u;
			dst[k].uv0[1] = chunk[k].v;
			/* uv1: the baker normalizes lu by the atlas width and lv by
			   its (trimmed, X9-R) height; the LINEAR lightmap stage wants
			   texels, so scale back by the same header values stage 1
			   pushes to IMAGE_RECT (X9-J, divergences.md #100). */
			dst[k].uv1[0] = chunk[k].lu * (float)nv2a_pending_upload.lightmap_width;
			dst[k].uv1[1] = chunk[k].lv * (float)nv2a_pending_upload.lightmap_height;
		}
		done += n;
	}
	ok = true;

done:
	free( chunk );
	gEngfuncs.fsapi->Close( f );
	if( !ok )
	{
		if( nv2a_map_vb ) { MmFreeContiguousMemory( nv2a_map_vb ); nv2a_map_vb = NULL; }
		if( nv2a_map_texture_data ) { MmFreeContiguousMemory( nv2a_map_texture_data ); nv2a_map_texture_data = NULL; }
		if( nv2a_map_lightmap ) { MmFreeContiguousMemory( nv2a_map_lightmap ); nv2a_map_lightmap = NULL; }
	}
	return ok;
}

/* MPAK v12: the renderer-only sections nv2a_build_map_tables() reads,
   copied out of the archive file into temporaries, since the engine's
   buffer is now only the classic-lump prefix (filesystem/mpak.c). Each is
   freed once the tables are built; none outlives the load. The indices
   are the exception: they are read straight into nv2a_map_index_dwords,
   whose packed layout (two u16 per dword, low half first) IS the
   archive's own u16 array on a little-endian CPU. faceInfo is read in
   windows (it is 44 bytes a face, 1.7 MB on rocket_frenzy, and only its
   flags are used), and the vertices only as positions, 12 of their 48
   bytes, for the per-face bounds. */
#define NV2A_FACEINFO_WINDOW 256u

typedef struct
{
	file_t           *file;             /* the whole archive, "maps/<name>.mpak" */
	float            *positions;        /* vec3 per vertex, for the per-face bounds */
	byte             *texture_dir;      /* mpak_texture_info_t[header->textureCount] */
	byte             *batches;          /* mpak_texture_batch_t[header->batchCount] */
	byte             *face_draw_cmds;   /* 2 x uint32_t per sorted face */
	byte             *face_vertex_base; /* uint32_t per sorted face, or NULL (single chunk) */
	byte             *face_remap;       /* uint32_t per original face */
	mpak_face_info_t *face_info_window; /* NV2A_FACEINFO_WINDOW records */
} nv2a_map_lumps_t;

/* Reads one whole lump into a new malloc'd block. A zero-length lump
   leaves *out NULL and succeeds. The load's temporaries use malloc, not
   Mem_Malloc: the zone allocator calls Sys_Error when it runs out
   (engine/common/zone.c), and a map too big for what is free should fall
   back to a frame with no world, not end the game (nv2a-xbox-specialist,
   X9-T review). */
static qboolean nv2a_read_lump( file_t *f, const mpak_lump_t *lump, byte **out )
{
	*out = NULL;
	if( lump->length == 0 )
		return true;
	*out = malloc( (size_t)lump->length );
	return *out && nv2a_read_at( f, lump->offset, *out, lump->length );
}

static void nv2a_free_map_lumps( nv2a_map_lumps_t *lumps )
{
	free( lumps->positions );
	free( lumps->texture_dir );
	free( lumps->batches );
	free( lumps->face_draw_cmds );
	free( lumps->face_vertex_base );
	free( lumps->face_remap );
	free( lumps->face_info_window );
	memset( lumps, 0, sizeof( *lumps ));
}

/* Fills `lumps` (and nv2a_map_index_dwords) from the archive. Vertices
   are streamed 1,024 at a time: only their positions are kept, and a
   vertex on any lightmap layer but 0 fails the load, as it always has
   (this renderer binds one lightmap page). */
static qboolean nv2a_read_map_lumps( file_t *f, const mpak_header_t *header, const nv2a_mpak_geometry_t *geo, nv2a_map_lumps_t *lumps )
{
	const uint32_t vert_count = header->vCount, index_count = header->iCount;
	const uint32_t dword_count = ( index_count + 1 ) / 2;
	mpak_vertex_t *chunk;
	uint32_t done;

	lumps->file = f;
	lumps->positions = malloc( (size_t)vert_count * 3 * sizeof( float ));
	chunk = lumps->positions ? malloc( 1024 * sizeof( mpak_vertex_t )) : NULL;
	if( !chunk )
		return false;
	for( done = 0; done < vert_count; )
	{
		uint32_t n = vert_count - done, k;

		if( n > 1024 )
			n = 1024;
		if( !nv2a_read_at( f, geo->vertices.offset + (fs_offset_t)done * sizeof( mpak_vertex_t ), chunk, (fs_offset_t)n * sizeof( mpak_vertex_t )))
			break;
		for( k = 0; k < n; k++ )
		{
			if( chunk[k].lightmap_layer != 0 )
				break;
			lumps->positions[( done + k ) * 3 + 0] = chunk[k].x;
			lumps->positions[( done + k ) * 3 + 1] = chunk[k].y;
			lumps->positions[( done + k ) * 3 + 2] = chunk[k].z;
		}
		if( k != n )
			break;
		done += n;
	}
	free( chunk );
	if( done != vert_count )
		return false;

	nv2a_map_index_dwords = malloc( (size_t)dword_count * sizeof( uint32_t ));
	if( !nv2a_map_index_dwords )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map index buffer allocation failed (%u bytes)\n", (unsigned int)( dword_count * sizeof( uint32_t )));
		return false;
	}
	nv2a_map_dword_count = dword_count;
	if( !nv2a_read_at( f, geo->indices.offset, nv2a_map_index_dwords, geo->indices.length ))
		return false;
	/* An odd index_count pads its final dword's high half by repeating
	   the last real index (see nv2a_map_index_dwords's own comment).
	   memcpy, not a uint16_t store: the storage is uint32_t. */
	if( index_count & 1 )
		memcpy( (byte *)nv2a_map_index_dwords + (size_t)index_count * 2, (const byte *)nv2a_map_index_dwords + (size_t)( index_count - 1 ) * 2, 2 );

	lumps->face_info_window = malloc( NV2A_FACEINFO_WINDOW * sizeof( mpak_face_info_t ));
	return lumps->face_info_window
		&& nv2a_read_lump( f, &geo->textureDir, &lumps->texture_dir )
		&& nv2a_read_lump( f, &geo->batches, &lumps->batches )
		&& nv2a_read_lump( f, &geo->faceDrawCmds, &lumps->face_draw_cmds )
		&& nv2a_read_lump( f, &geo->faceVertexBase, &lumps->face_vertex_base )
		&& nv2a_read_lump( f, &geo->faceRemap, &lumps->face_remap );
}

static qboolean nv2a_build_map_tables( const mpak_header_t *header, const nv2a_mpak_geometry_t *geo_in, const nv2a_map_lumps_t *lumps );

/* Loads a map's renderer data from its archive, "maps/<name>.mpak"
   (MPAK v12: the whole file; the engine only loaded the classic prefix),
   after checking it starts with the engine's first sector. Checks the
   ledger before anything is allocated, reads the renderer-only lumps
   (nv2a_read_map_lumps), builds every CPU-side table from them
   (nv2a_build_map_tables), then frees the temporaries. X9-U: called from
   R_NewMap (nv2a_try_pending_upload), after the engine has freed its copy
   of the map, just before the GPU buffers are built. */
static qboolean nv2a_load_map_geometry( const mpak_header_t *header, const byte *engine_first_sector, const char *archive_name )
{
	nv2a_mpak_geometry_t geo;
	nv2a_map_lumps_t lumps;
	file_t *f;
	uint32_t vert_count, index_count, dword_count;
	uint64_t vb_bytes, idx_bytes, total_bytes;
	byte first_sector[MPAK_SECTOR_SIZE];
	qboolean ok = false;

	memset( &lumps, 0, sizeof( lumps ));
	f = gEngfuncs.fsapi->Open( archive_name, "rb", false );
	if( !f )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: could not open %s\n", archive_name );
		return false;
	}

	/* The .mpak must be the archive the engine just loaded the .bsp view
	   of: both names come from the same search path today, but a loose
	   file under either name would otherwise pair one archive's offsets
	   with another's bytes (asset-pipeline and nv2a-xbox specialists, X9-T
	   review). The first sector holds the dheader_t and the descriptor. */
	if( !nv2a_read_at( f, 0, first_sector, MPAK_SECTOR_SIZE )
		|| memcmp( first_sector, engine_first_sector, MPAK_SECTOR_SIZE ) != 0 )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: %s is not the archive the map was loaded from\n", archive_name );
		goto done;
	}
	nv2a_pending_upload.file_length = gEngfuncs.fsapi->FileLength( f );

	if( !nv2a_compute_mpak_layout( header, nv2a_pending_upload.file_length, &geo ))
		goto done;

	if( geo.textureDir.length == 0 || geo.textureData.length == 0 || geo.batches.length == 0
		|| geo.faceRemap.length == 0 || geo.lightmapAtlas.length == 0 )
		goto done;

	vert_count = header->vCount;
	index_count = header->iCount;
	if( vert_count < 3 || index_count == 0 )
		goto done;
	if( (uint64_t)vert_count * sizeof( mpak_vertex_t ) != (uint64_t)geo.vertices.length )
		goto done;
	if( (uint64_t)index_count * sizeof( uint16_t ) != (uint64_t)geo.indices.length )
		goto done;

	dword_count = ( index_count + 1 ) / 2;
	vb_bytes = (uint64_t)vert_count * sizeof( nv2a_lit_vertex_t );
	idx_bytes = (uint64_t)dword_count * sizeof( uint32_t );
	/* X9-F: the per-face table is one nv2a_map_face_t per on-disk
	   FaceDrawCmd (8 bytes each). X9-S: counted as face_count * sizeof,
	   not the old "4x the lump" - the struct grew from 32 to 52 bytes
	   (X9-K/M/S) while that multiplier stood still. Real
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
	/* X9-T: the peak, not the sum. The tables below live from here on;
	   this load's temporaries (nv2a_read_map_lumps - the indices are
	   already in `tables`, faceInfo's window is too small to count) live
	   only until it returns, and the three GPU buffers are built after
	   that, so the two never coexist. X9-U: this runs at R_NewMap, after
	   the engine has freed its copy of the map, so that copy is no longer
	   part of either side. */
	{
		/* X9-X step 8: + the lightmap tables (NV2A_LM_FACE_BYTES per face:
		   nv2a_map_face_lm here, r_lightmap.c's held flags and styled-face
		   list at the first frame), mirrored by Nv2aMapBudget.h's
		   kNv2aLightmapFaceBytes. */
		/* R3 (#146): + the visible-face list, the face planes (a uint16_t
		   each per face) and the listable bytes, mirrored by Nv2aMapBudget.h's
		   kNv2aPvsListFaceBytes. */
		const uint64_t tables = idx_bytes + (uint64_t)( geo.faceDrawCmds.length / 8 ) * sizeof( nv2a_map_face_t )
			+ (uint64_t)geo.faceDrawCmds.length / 8
			+ (uint64_t)geo.faceRemap.length * 2
			+ (uint64_t)( geo.faceDrawCmds.length / 8 ) * NV2A_LM_FACE_BYTES
			+ (uint64_t)( geo.faceDrawCmds.length / 8 ) * NV2A_PVS_LIST_FACE_BYTES;
		const uint64_t gpu = vb_bytes + (uint64_t)geo.textureData.length + (uint64_t)geo.lightmapAtlas.length;
		const uint64_t load = (uint64_t)vert_count * 3 * sizeof( float )
			+ (uint64_t)geo.faceDrawCmds.length + (uint64_t)geo.faceVertexBase.length + (uint64_t)geo.faceRemap.length
			+ (uint64_t)geo.textureDir.length + (uint64_t)geo.batches.length;
		total_bytes = tables + ( gpu > load ? gpu : load );
	}
	if( total_bytes > NV2A_MAP_BUDGET_BYTES )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map exceeds NV2A_MAP_BUDGET_BYTES (%u > %u bytes) - not drawing its real geometry\n",
			(unsigned int)total_bytes, (unsigned int)NV2A_MAP_BUDGET_BYTES );
		goto done;
	}

	if( !nv2a_read_map_lumps( f, header, &geo, &lumps ))
	{
		gEngfuncs.Con_Printf( "ref_nv2a: could not read %s's renderer data - not drawing its real geometry\n", archive_name );
		goto done;
	}

	ok = nv2a_build_map_tables( header, &geo, &lumps );

done:
	nv2a_free_map_lumps( &lumps );
	gEngfuncs.fsapi->Close( f );
	if( !ok )
		nv2a_free_map_resources();
	return ok;
}

/* Validates the renderer-only lumps nv2a_read_map_lumps() read and builds
   every CPU-side table from them: the texture directory and batch table,
   the per-face culling table with its bounds, the face remap and the PVS
   table. The GPU buffers' locations are recorded for R_NewMap. */
static qboolean nv2a_build_map_tables( const mpak_header_t *header, const nv2a_mpak_geometry_t *geo_in, const nv2a_map_lumps_t *lumps )
{
	const nv2a_mpak_geometry_t geo = *geo_in;
	const byte *indices = (const byte *)nv2a_map_index_dwords; /* u16 each, read with memcpy: the storage is uint32_t */
	const uint32_t vert_count = header->vCount, index_count = header->iCount;
	const uint32_t dword_count = ( index_count + 1 ) / 2;
	const uint64_t vb_bytes = (uint64_t)vert_count * sizeof( nv2a_lit_vertex_t );
	uint32_t texture_count, batch_count;
	uint32_t i;

	nv2a_map_chunk_count = 0; /* X9-U: this map's chunk table starts empty, whatever came before */

	/* Every raw index must reference a real slot in the vertex array
	   - malformed data must fail cleanly here, not be trusted into a GPU
	   attribute-array read out of bounds later. (Every baked vertex is
	   used 1:1 - the shared index buffer already references vertices by
	   their final position.) X9-S: a single-chunk map's indices are
	   absolute; on a multi-chunk map they are chunk-relative and the
	   per-face check below adds each face's base. */
	for( i = 0; i < index_count; i++ )
	{
		uint16_t idx;

		memcpy( &idx, indices + (size_t)i * 2, sizeof( idx ));
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
	   larger than NV2A's real 4096 texture edge (log2 <= 12 - the same cap
	   the old atlas check used). dataLength must be the exact byte count of
	   that size's full DXT1 mip chain, and the running sum of every entry's
	   own dataLength rounded up to 128 bytes must land EXACTLY on the
	   texture-data lump's own real, bounds-checked length after the last
	   entry - refusing the archive beats sampling garbage: a wrong log2 or
	   a short running sum would not crash, it would silently bind the wrong
	   bytes as a texture, or read past nv2a_map_texture_data into whatever
	   memory follows it. This whole rule - the ONE this module used to keep
	   its own private copy of - is now Mpak_ValidateTextureDirectory()
	   (filesystem/mpak_texture_dir.h), shared byte-for-byte with the PC
	   tests and the baker's verify pass (fork-plan.md's X9 item 2
	   follow-up). `data` is passed NULL on purpose: this keeps the
	   console's accept/reject set exactly what it was before this change
	   (NV2A never samples padding bytes, so checking their CONTENT here
	   would be a new, untested hardware-facing check for no behavioural
	   gain - the padding-content check still runs, just in the PC tests and
	   PakManager::verifyFile, which is what the follow-up actually asked
	   for). */
	{
		uint32_t bad = 0;
		mpak_texdir_status_t status = Mpak_ValidateTextureDirectory(
			lumps->texture_dir, (uint64_t)geo.textureDir.length, texture_count,
			NULL, (uint64_t)geo.textureData.length, &bad );

		if( status != MPAK_TEXDIR_OK )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: texture directory entry %u failed validation (%s) - not drawing this map's real geometry\n",
				(unsigned int)bad, Mpak_TextureDirStatusString( status ));
			return false;
		}
	}

	/* physOffset (this entry's own byte offset into nv2a_map_texture_data)
	   is the running sum of Mpak_TextureAlignedLength(dataLength) over
	   every PRIOR entry - mpak_texture_info_t itself stores no offset
	   field, matching the archive's own "byte layout order IS the format"
	   convention one level down (filesystem/mpak_layout.h's own v9 note).
	   Safe to re-read every field here with no further bounds-checking:
	   the validation pass above already confirmed geo.textureDir covers
	   exactly texture_count entries of real, in-range data. */
	{
		uint64_t running = 0;

		for( i = 0; i < texture_count; i++ )
		{
			const byte *base = lumps->texture_dir + (size_t)i * MPAK_TEXTURE_INFO_SIZE;
			uint32_t log2w, log2h, dataLength;

			memcpy( &log2w,      base + MPAK_TEXTURE_INFO_OFS_LOG2WIDTH,  sizeof( log2w ));
			memcpy( &log2h,      base + MPAK_TEXTURE_INFO_OFS_LOG2HEIGHT, sizeof( log2h ));
			memcpy( &dataLength, base + MPAK_TEXTURE_INFO_OFS_DATALENGTH, sizeof( dataLength ));

			nv2a_map_textures[i].physOffset = (uint32_t)running;
			nv2a_map_textures[i].log2Width  = log2w;
			nv2a_map_textures[i].log2Height = log2h;
			Mpak_Dxt1ChainLength( log2w, log2h, &nv2a_map_textures[i].mipLevels );

			running += Mpak_TextureAlignedLength( dataLength );
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
			const byte *base = lumps->batches + (size_t)i * sizeof( mpak_texture_batch_t );
			uint32_t textureIndex, startFace, faceCount, startIndex, indexCount;

			memcpy( &textureIndex, base + 0,  sizeof( textureIndex ));
			memcpy( &startFace,    base + 4,  sizeof( startFace ));
			memcpy( &faceCount,    base + 8,  sizeof( faceCount ));
			memcpy( &startIndex,   base + 12, sizeof( startIndex ));
			memcpy( &indexCount,   base + 16, sizeof( indexCount ));

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

		/* One layer: this renderer binds one lightmap page, and every
		   layer would otherwise still be uploaded (nv2a-xbox-specialist,
		   X9-T review). */
		if( header->lightmapWidth == 0 || header->lightmapHeight == 0 || header->lightmapLayers != 1
			|| header->lightmapFormat != 0 /* 0 = RGBA8, the only format anything bakes today */
			|| expected_lm != (uint64_t)geo.lightmapAtlas.length )
			return false;

		/* The baker normalizes lu by its fixed page width, LM_PAGE_SIZE =
		   1024 (tools/asset-studio/src/formats/BspParser.cpp), and - since
		   X9-R - lv by the atlas's own final height, which it trims to the
		   rows it uses (LightmapAtlasPacker::atlasHeight, a multiple of 4 up
		   to 1024; up to 4096 since X9-T). The remap loop below undoes both with the header
		   fields (the same values stage 1 pushes to IMAGE_RECT), so the
		   inverse is exact when the width is the baker's 1024 and the
		   height is whatever the header says. Archives from before X9-R
		   (height always 1024, lv / 1024) satisfy the same rule. A width
		   other than 1024 still fails closed: lu's divisor is a baker
		   constant, not a header field (codex review: a square-but-not-1024
		   page would otherwise sample from the wrong place). */
		if( header->lightmapWidth != NV2A_LM_PAGE_SIZE
			|| header->lightmapHeight < 4 || header->lightmapHeight > NV2A_LM_MAX_HEIGHT )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: lightmap page %ux%u is not %u wide by 4..%u high - the baker's lu is normalized by a fixed %u-luxel width, not drawing its real geometry\n",
				(unsigned int)header->lightmapWidth, (unsigned int)header->lightmapHeight,
				NV2A_LM_PAGE_SIZE, NV2A_LM_MAX_HEIGHT, NV2A_LM_PAGE_SIZE );
			return false;
		}
	}

	/* X9-T: the vertex buffer, the texture data and the lightmap - the
	   three contiguous GPU buffers - are no longer built here. The engine
	   holds this whole archive (`buffer`) until Mod_ProcessRenderData
	   returns, so copying out of it here stacked the GPU copies on top of
	   it: pool_party had 4.0 MiB free at this point and its 3 MB lightmap
	   allocation failed. nv2a_upload_map_gpu_data() builds them from the
	   file at R_NewMap instead, after the engine has freed the archive -
	   see nv2a_pending_upload's own comment. Only where they live is
	   recorded here. */
	nv2a_pending_upload.vertices_offset = geo.vertices.offset;
	nv2a_pending_upload.vertex_count    = vert_count;
	nv2a_pending_upload.vb_bytes        = vb_bytes;
	nv2a_pending_upload.lightmap_width  = header->lightmapWidth;
	nv2a_pending_upload.lightmap_height = header->lightmapHeight;
	nv2a_map_vert_count = vert_count;

	/* X9-K: ONE contiguous buffer for every texture's own DXT1 bytes,
	   back-to-back in directory order - replaces the single shared-atlas
	   allocation this function used through X9-J. Copied verbatim, same
	   "already in the right hardware layout, no un-swizzle needed"
	   reasoning the old atlas copy cited (this file's own header
	   comment). nv2a_map_textures[].physOffset (computed in the
	   validation pass above) is each texture's own byte offset into THIS
	   buffer. */
	/* X9-T: filled by nv2a_upload_map_gpu_data() at R_NewMap. */
	nv2a_pending_upload.texture_offset  = geo.textureData.offset;
	nv2a_pending_upload.texture_bytes   = geo.textureData.length;
	nv2a_pending_upload.lightmap_offset = geo.lightmapAtlas.offset;
	nv2a_pending_upload.lightmap_bytes  = geo.lightmapAtlas.length;
	nv2a_map_texture_count = texture_count;
	nv2a_map_batch_count   = batch_count;
	nv2a_map_lightmap_width  = header->lightmapWidth;
	nv2a_map_lightmap_height = header->lightmapHeight;
	nv2a_map_lightmap_pitch  = nv2a_map_lightmap_width * 4; /* RGBA8, LU_IMAGE - no block rounding */

	/* The raw indices packed two to a dword for NV20_TCL_PRIMITIVE_3D_
	   INDEX_DATA (low half first - see nv2a_map_index_dwords's own
	   declaration comment) are already in nv2a_map_index_dwords: MPAK v12
	   reads them straight in (nv2a_read_map_lumps), since that packing is
	   the archive's own u16 array on a little-endian CPU. */

	/* X9-E: the camera is no longer computed here. nv2a_map_viewproj is
	   now written every real frame by GL_RenderFrame's own real
	   vieworigin/viewangles/fov - see nv2a_view_valid's own declaration
	   comment for why a map loaded before the first real frame still
	   draws safely (no world drawn that frame) instead of ever
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
		nv2a_map_face_t *faces = malloc( (size_t)face_count * sizeof( nv2a_map_face_t ));
		/* X9-X step 8: optional - a failure only leaves the atlas static. */
		nv2a_lm_block_t *lm_blocks = faces ? malloc( (size_t)face_count * sizeof( nv2a_lm_block_t )) : NULL;

		if( !faces )
		{
			gEngfuncs.Con_Printf( "ref_nv2a: face-culling table allocation failed (%u bytes) - drawing this map unculled\n",
				(unsigned int)( (uint64_t)face_count * sizeof( nv2a_map_face_t )));
		}
		else
		{
			uint32_t expect_cursor = 0;
			qboolean tiling_ok = true;
			/* X9-K sky-skip fix: MpakFaceInfo is one record per face, in the
			   SAME (sorted) order FaceDrawCmds already uses - fails open
			   (every face's .sky left false, drawn same as before this fix)
			   on a stale archive whose faceInfo lump doesn't have exactly
			   face_count records, never blocking the load over an
			   optional, cosmetic-only check. */
			qboolean faceinfo_ok = geo.faceInfo.length == (uint64_t)face_count * sizeof( mpak_face_info_t );
			/* X9-S: a multi-chunk map ships one base per face; a single-
			   chunk map ships none (every base 0). Any other length breaks
			   the per-face table, and a multi-chunk map cannot fall back to
			   the unculled whole-buffer draw (it has no single base), so
			   that case refuses the map below. */
			qboolean bases_ok = geo.faceVertexBase.length == 0
				|| geo.faceVertexBase.length == (uint64_t)face_count * sizeof( uint32_t );
			if( !bases_ok )
				tiling_ok = false;

			for( i = 0; i < face_count; i++ )
			{
				uint32_t start_index, face_indices, k;

				memcpy( &start_index,  lumps->face_draw_cmds + (size_t)i * 8,     sizeof( start_index ));
				memcpy( &face_indices, lumps->face_draw_cmds + (size_t)i * 8 + 4, sizeof( face_indices ));

				/* MPAK v12: faceInfo is read NV2A_FACEINFO_WINDOW records at
				   a time; a short read fails open like a bad length. */
				if( faceinfo_ok && ( i % NV2A_FACEINFO_WINDOW ) == 0 )
				{
					uint32_t n = face_count - i < NV2A_FACEINFO_WINDOW ? face_count - i : NV2A_FACEINFO_WINDOW;

					faceinfo_ok = nv2a_read_at( lumps->file, geo.faceInfo.offset + (fs_offset_t)i * sizeof( mpak_face_info_t ),
						lumps->face_info_window, (fs_offset_t)n * sizeof( mpak_face_info_t ));
				}
				if( faceinfo_ok )
				{
					faces[i].sky = ( lumps->face_info_window[i % NV2A_FACEINFO_WINDOW].flags & MPAK_SURF_DRAWSKY ) != 0;
					faces[i].turb = ( lumps->face_info_window[i % NV2A_FACEINFO_WINDOW].flags & MPAK_SURF_DRAWTURB ) != 0;
				}
				else
				{
					faces[i].sky = false;
					faces[i].turb = false;
				}

				/* X9-X step 8: the face's atlas block. A face with a real
				   lightmap has non-negative extents, a block on page 0 (the
				   only page this renderer binds) away from the reserved
				   corner at (0,0) (BspParser.cpp: lmAlloc defaults there for
				   a face with none), and at most LM_MAX_LUXELS per axis.
				   r_lightmap.c checks the rest against the engine's own
				   surface before it ever writes one. */
				if( lm_blocks )
				{
					const mpak_face_info_t *fi = &lumps->face_info_window[i % NV2A_FACEINFO_WINDOW];
					nv2a_lm_block_t *blk = &lm_blocks[i];

					blk->x = NV2A_LM_BLOCK_NONE;
					blk->y = 0;
					blk->smax = blk->tmax = 0;
					if( faceinfo_ok && fi->lightmapPage == 0 && ( fi->lightmapBlockX | fi->lightmapBlockY ) != 0
						&& fi->lightmapBlockX < NV2A_LM_BLOCK_NONE && fi->lightmapBlockY < NV2A_LM_BLOCK_NONE
						&& fi->extents[0] >= 0 && fi->extents[1] >= 0
						&& fi->extents[0] / 16 + 1 <= NV2A_LM_MAX_LUXELS_PER_AXIS && fi->extents[1] / 16 + 1 <= NV2A_LM_MAX_LUXELS_PER_AXIS )
					{
						blk->x = (uint16_t)fi->lightmapBlockX;
						blk->y = (uint16_t)fi->lightmapBlockY;
						blk->smax = (uint8_t)( fi->extents[0] / 16 + 1 );
						blk->tmax = (uint8_t)( fi->extents[1] / 16 + 1 );
					}
				}

				/* X9-U: a face's index count is stored in 16 bits. */
				if( face_indices == 0 || ( face_indices % 3 ) != 0 || face_indices > 0xFFFFu || start_index != expect_cursor
					|| (uint64_t)start_index + face_indices > index_count )
				{
					tiling_ok = false;
					break;
				}

				{
					uint32_t vertex_base = 0, c;
					vec3_t fmins, fmaxs;

					if( geo.faceVertexBase.length != 0 )
						memcpy( &vertex_base, lumps->face_vertex_base + (size_t)i * sizeof( uint32_t ), sizeof( uint32_t ));
					/* X9-U: the face stores its chunk's number, not its base. */
					for( c = 0; c < nv2a_map_chunk_count && nv2a_map_chunk_base[c] != vertex_base; c++ )
						;
					if( c == nv2a_map_chunk_count )
					{
						if( c == NV2A_MAP_MAX_CHUNKS )
						{
							tiling_ok = false;
							break;
						}
						nv2a_map_chunk_base[nv2a_map_chunk_count++] = vertex_base;
					}
					faces[i].chunk = (uint8_t)c;

					ClearBounds( fmins, fmaxs );
					for( k = start_index; k < start_index + face_indices; k++ )
					{
						uint16_t vi;

						memcpy( &vi, indices + (size_t)k * 2, sizeof( vi ));
						/* X9-S: indices are chunk-relative; the absolute vertex
						   must still be inside the vertex array. */
						if( (uint64_t)vertex_base + vi >= vert_count )
						{
							tiling_ok = false;
							break;
						}
						/* X9-T: positions from the archive (the vertex buffer
						   is built later, at R_NewMap); v12: streamed into
						   lumps->positions by nv2a_read_map_lumps. */
						AddPointToBounds( &lumps->positions[( (size_t)vertex_base + vi ) * 3], fmins, fmaxs );
					}
					if( !tiling_ok )
						break;
					/* X9-U: whole units, rounded outward, so the stored box
					   always contains the face. A face reaching past int16
					   (the engine allows +-32768, BOGUS_RANGE) cannot be
					   stored that way, so it refuses the table rather than
					   clamping the box inward (nv2a-xbox-specialist, X9-U
					   review). No retail map comes close. */
					for( c = 0; c < 3; c++ )
					{
						if( floorf( fmins[c] ) < -32768.0f || ceilf( fmaxs[c] ) > 32767.0f )
							tiling_ok = false;
						faces[i].mins[c] = (int16_t)bound( -32768.0f, floorf( fmins[c] ), 32767.0f );
						faces[i].maxs[c] = (int16_t)bound( -32768.0f, ceilf( fmaxs[c] ), 32767.0f );
					}
					if( !tiling_ok )
						break;
				}

				faces[i].index_begin = start_index;
				faces[i].index_count = (uint16_t)face_indices;
				faces[i].texture     = NV2A_FACE_NO_TEXTURE; /* filled from the batch table below; stays invalid if no valid batch covers it */

				expect_cursor = start_index + face_indices;
			}

			if( tiling_ok && expect_cursor == index_count )
			{
				nv2a_map_faces = faces;
				nv2a_map_face_lm = lm_blocks; /* X9-X step 8 */
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
							faces[f2].texture = (uint16_t)bt->textureIndex;
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
					uint32_t *remap = malloc( (size_t)remap_count * sizeof( uint32_t ));
					uint32_t *orig  = remap ? malloc( (size_t)face_count * sizeof( uint32_t )) : NULL;

					if( !remap || !orig )
					{
						gEngfuncs.Con_Printf( "ref_nv2a: face-remap table allocation failed - PVS culling disabled for this map\n" );
						if( remap ) free( remap );
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

							memcpy( &sortedIdx, lumps->face_remap + (size_t)j * sizeof( uint32_t ), sizeof( sortedIdx ));
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
							free( remap );
							free( orig );
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
				nv2a_map_face_visible = malloc( (size_t)face_count );
				if( nv2a_map_face_visible )
					memset( nv2a_map_face_visible, 1, (size_t)face_count );
				else
					gEngfuncs.Con_Printf( "ref_nv2a: PVS visibility table allocation failed (%u bytes) - frustum culling only\n",
						(unsigned int)face_count );

				/* R3 (#146): the visible-face list and the per-map face tables,
				   filled by nv2a_mark_leaves. Without them the walk visits
				   every face, as it did before - slower, never different. */
				if( nv2a_map_face_visible && nv2a_map_face_remap && nv2a_map_face_orig && face_count <= 0x10000u )
				{
					nv2a_pvs_faces = malloc( (size_t)face_count * sizeof( uint16_t ));
					nv2a_map_face_plane = malloc( (size_t)face_count * sizeof( uint16_t ));
					nv2a_map_face_listable = malloc( (size_t)face_count );
					if( !nv2a_pvs_faces || !nv2a_map_face_plane || !nv2a_map_face_listable )
					{
						gEngfuncs.Con_Printf( "ref_nv2a: visible-face list allocation failed - the world pass walks every face\n" );
						if( nv2a_pvs_faces ) { free( nv2a_pvs_faces ); nv2a_pvs_faces = NULL; }
						if( nv2a_map_face_plane ) { free( nv2a_map_face_plane ); nv2a_map_face_plane = NULL; }
						if( nv2a_map_face_listable ) { free( nv2a_map_face_listable ); nv2a_map_face_listable = NULL; }
					}
				}
			}
			else
			{
				gEngfuncs.Con_Printf( "ref_nv2a: FaceDrawCmds tiling did not validate - drawing this map unculled\n" );
				free( faces );
				if( lm_blocks )
					free( lm_blocks );
			}
		}
	}

	/* X9-S: a multi-chunk map (MPAK v11 faceVertexBase lump present) can
	   only be drawn through the per-face table - the unculled whole-buffer
	   fallback has no single base vertex to draw with. Without the table,
	   refuse the map rather than draw every chunk against base 0. */
	if( geo.faceVertexBase.length != 0 && !nv2a_map_faces )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: multi-chunk map has no usable per-face table - not drawing its real geometry\n" );
		return false;
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
		/* X9-X step 5 / gl_context.c:106-111,131-136: the OTHER unload this
		   callback owns, unconditional on nv2a_map_owner - every studio
		   model's own skins, freed here besides the map-owner check above
		   (which only ever applies to the world). */
		if( mod->type == mod_studio )
			Mod_StudioUnloadTextures( mod->cache.data );
		return true;
	}

	if( nv2a_mpak_header_valid( buffer, buffersize, &header ))
	{
		/* MPAK v12: `buffer` is only the archive's classic prefix, served
		   as "maps/<name>.bsp" (filesystem/mpak.c); the renderer's own
		   sections are read from the whole archive, "maps/<name>.mpak". */
		char archive_name[sizeof( nv2a_pending_upload.name )];
		const char *dot = strrchr( mod->name, '.' );
		size_t stem = dot ? (size_t)( dot - mod->name ) : strlen( mod->name );

		nv2a_free_map_resources();
		if( stem + sizeof( ".mpak" ) > sizeof( archive_name ) || buffersize < MPAK_SECTOR_SIZE )
			return true;
		memcpy( archive_name, mod->name, stem );
		memcpy( archive_name + stem, ".mpak", sizeof( ".mpak" ));

		/* X9-U: nothing is built here - the tables and the GPU buffers
		   both come from R_NewMap, once the engine has freed `buffer`
		   (nv2a_pending_upload). Only what that needs is kept. */
		nv2a_map_owner = mod;
		memcpy( &nv2a_pending_upload.header, header, sizeof( *header ));
		memcpy( nv2a_pending_upload.first_sector, buffer, MPAK_SECTOR_SIZE );
		memcpy( nv2a_pending_upload.name, archive_name, sizeof( archive_name ));
		nv2a_pending_upload.pending = true;
		nv2a_pending_upload.retried = false;
		nv2a_pending_upload.tables_built = false;
	}

	return true; /* the model load itself always succeeds regardless; drawing
	                real geometry is opportunistic - a model that isn't a real
	                nv2a-profile map archive just leaves whatever (if anything)
	                the world already set up untouched */
}

/* X9-X step 5: Mod_StudioLoadTextures/Mod_StudioUnloadTextures are now
   real, implemented in r_studio.c and declared by r_studio.h. */

/* X9-X step 7: CL_DrawParticles/CL_DrawTracers/CL_DrawBeams are real,
   implemented in r_effects.c and declared by r_effects.h. */

/* D5: whole port of ref/soft's own GL_RefGetParm switch (r_context.c:124-199),
   corrected where D5 says to - PARM_TEX_FILTERING answered locally
   instead of the reference's own "ref_soft doesn't filter" 0.
   PARM_GET_STUDIO_HDR now calls R_StudioGetHeader() (X9-X step 5,
   gl_context.c:218), r_studio.c's own m_pStudioHeader accessor. */
static intptr_t RefGetParm( int parm, int arg )
{
	switch( parm )
	{
	case PARM_TEX_WIDTH:      return nv2a_tex_get((unsigned int)arg )->width;
	case PARM_TEX_HEIGHT:     return nv2a_tex_get((unsigned int)arg )->height;
	case PARM_TEX_SRC_WIDTH:  return nv2a_tex_get((unsigned int)arg )->srcWidth;
	case PARM_TEX_SRC_HEIGHT: return nv2a_tex_get((unsigned int)arg )->srcHeight;
	case PARM_TEX_MIPCOUNT:   return 1; /* D2: no mip chain this phase */
	case PARM_TEX_DEPTH:      return 1;
	case PARM_TEX_SKYBOX:     return R_SkyboxTexture( arg ); /* gl_context.c:182 */
	case PARM_TEX_SKYTEXNUM:  return 0;
	case PARM_TEX_LIGHTMAP:   return 0;
	case PARM_TEX_TARGET:     return 0;
	case PARM_TEX_TEXNUM:     return 0;
	case PARM_TEX_GLFORMAT:   return 0;
	case PARM_TEX_ENCODE:     return 0;
	case PARM_TEX_FLAGS:      return nv2a_tex_get((unsigned int)arg )->flags;
	case PARM_TEX_MEMORY:     return (intptr_t)nv2a_tex_memory();
	case PARM_ACTIVE_TMU:     return 0;
	case PARM_LIGHTSTYLEVALUE:
		arg = bound( 0, arg, MAX_LIGHTSTYLES - 1 );
		return g_lightstylevalue[arg];
	case PARM_GL_CONTEXT_TYPE: return CONTEXT_TYPE_SOFTWARE;
	case PARM_SKY_SPHERE:      return 0;
	case PARM_GLES_WRAPPER:    return 0;
	case PARM_STENCIL_ACTIVE:  return 0;
	case PARM_MAX_IMAGE_UNITS: return 0;
	case PARM_REBUILD_GAMMA:   return 0;
	case PARM_GET_STUDIO_HDR:  return (intptr_t)R_StudioGetHeader();
	case PARM_TEX_FILTERING:
		if( arg < 0 )
			return !nv2a_r_texturenearest.value;
		return nv2a_texture_filtering_enabled( nv2a_tex_get((unsigned int)arg ));
	default:
		return ENGINE_GET_PARM_( parm, arg );
	}
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
/* Frame-time work counters (nv2a_ft.h): the leaves in the PVS at its
   last recompute, and where this frame's pushbuffer writes began. */
static uint32_t nv2a_ft_pvs_leaves;
static uint32_t *nv2a_ft_pb_frame_start;
static uint32_t nv2a_ft_models_frame_start;
uint32_t nv2a_ft_imm3d_counter = XBOX_FT_W_VERTS_TRI;

/* R3 (#146): nv2a_map_face_plane and nv2a_map_face_listable for `world`,
   once per map - see their declarations. Runs from nv2a_mark_leaves'
   once-per-map check, after world->numsurfaces has been matched against
   nv2a_map_face_count. */
static void nv2a_fill_face_planes( const model_t *world )
{
	uint32_t i;

	nv2a_map_face_plane_world = NULL;
	if( !nv2a_map_face_plane || !nv2a_map_face_listable || !nv2a_map_face_orig || !nv2a_map_faces )
		return;

	for( i = 0; i < nv2a_map_face_count; i++ )
	{
		uint32_t orig = nv2a_map_face_orig[i];
		uint16_t ref = NV2A_FACE_PLANE_NONE;

		if( orig < (uint32_t)world->numsurfaces )
		{
			const msurface_t *surf = &world->surfaces[orig];

			if( surf->plane >= world->planes && surf->plane < world->planes + world->numplanes
				&& (uint32_t)( surf->plane - world->planes ) <= NV2A_FACE_PLANE_INDEX_MAX )
			{
				if( !VectorIsNull( surf->plane->normal ))
					ref = (uint16_t)(( surf->plane - world->planes ) | ( FBitSet( surf->flags, NV2A_SURF_PLANEBACK ) ? NV2A_FACE_PLANE_BACK : 0 ));
			}
			else
			{
				ref = NV2A_FACE_PLANE_SURF;
			}
		}
		nv2a_map_face_plane[i] = ref;
		nv2a_map_face_listable[i] = !nv2a_map_faces[i].sky && nv2a_map_face_is_world( world, i );
	}
	nv2a_map_face_plane_world = world;
}

/* R3 (#146): nv2a_pvs_faces from nv2a_map_face_visible - see its
   declaration. Called by nv2a_mark_leaves right after every rewrite of
   that table. */
static void nv2a_build_pvs_faces( const model_t *world )
{
	uint32_t b, i, n = 0;

	nv2a_pvs_faces_world = NULL;
	if( !nv2a_pvs_faces || !nv2a_map_faces || !nv2a_map_face_visible || nv2a_map_face_count > 0x10000u
		|| nv2a_map_face_plane_world != world )
		return;

	for( b = 0; b < nv2a_map_batch_count; b++ )
	{
		const nv2a_map_batch_t *batch = &nv2a_map_batches[b];

		/* A batch outside the face table is drawn whole, without the
		   per-face walk; its slice stays empty. */
		if( (uint64_t)batch->startFace + batch->faceCount <= (uint64_t)nv2a_map_face_count )
		{
			uint32_t end_face = batch->startFace + batch->faceCount;

			for( i = batch->startFace; i < end_face; i++ )
			{
				if( !nv2a_map_face_visible[i] || !nv2a_map_face_listable[i] )
					continue;
				if( n >= nv2a_map_face_count )
					return; /* overlapping batches - the load's validation rules them out; no list */
				nv2a_pvs_faces[n++] = (uint16_t)i;
			}
		}
		nv2a_pvs_batch_end[b] = n;
	}
	nv2a_pvs_faces_world = world;
}

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
			free( nv2a_map_face_visible );
			nv2a_map_face_visible = NULL;
			nv2a_pvs_faces_world = NULL; /* R3 (#146): no table, no list */
			return;
		}
		nv2a_fill_face_planes( world ); /* R3 (#146) */
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
	nv2a_visbytes_valid = true;

	memset( nv2a_map_face_visible, 0, (size_t)nv2a_map_face_count );
	nv2a_ft_pvs_leaves = 0;

	for( i = 0; i < (uint32_t)world->numleafs; i++ )
	{
		const mleaf_t *vleaf;
		int j;

		if( !NV2A_CHECKVISBIT( nv2a_visbytes, i ))
			continue;
		nv2a_ft_pvs_leaves++;

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

	nv2a_build_pvs_faces( world ); /* R3 (#146): the walk's list, from the table just written */
}

/* X9-X step 8: r_lightmap.c's per-face visibility question - drawn by the
   world pass this frame? The same PVS and frustum tests
   nv2a_draw_map_geometry applies (before its backface test, which only
   drops faces seen from behind this frame). */
static qboolean nv2a_lm_face_in_view( uint32_t sorted )
{
	if( sorted >= nv2a_map_face_count || !nv2a_map_faces )
		return false;
	if( nv2a_map_faces[sorted].sky )
		return false;
	if( nv2a_map_face_visible && !nv2a_map_face_visible[sorted] )
		return false;
	return !nv2a_frustum_cull_face( &nv2a_map_faces[sorted] );
}

/*
========================================================================
X9-X item 14 step 12: decals, the GPU side (r_decals.c is the port of
gl_decals.c that decides what to draw)

A decal polygon is drawn with the world's own vertex program (vs_lit:
position, stage-0 and stage-1 coordinates) from a per-frame vertex buffer,
with the decal texture on stage 0, the face's lightmap on stage 1 (the
baked atlas, or an unlit entity's neutral page, whichever the pass left
bound) and ps_decal: decal x lightmap x 2 x the entity colour, alpha =
decal alpha x the entity alpha, into V0 so the fog blend still applies.
That is ref/gl's picture in one pass: DrawSingleDecal under GL_MODULATE
with R_SetRenderMode's colour (gl_decals.c:826, gl_rsurf.c:1717), and
R_BlendLightmaps' multiply over it (gl_rsurf.c:3711-3713).

State, as DrawDecalsBatch/DrawSurfaceDecals push it (gl_decals.c:856-875,
989-1000): SRC_ALPHA/ONE_MINUS_SRC_ALPHA blending (ONE for a premultiplied
texture), no depth writes except under kRenderTransColor (whose
R_SetRenderMode leaves them on), no alpha test, and gl_polyoffset as a
depth bias. Face culling is off: r_decals.c only hands over faces that
face the eye, except a kRenderTransTexture entity's back faces, which
ref/gl draws with GL_Cull( GL_NONE ).

The depth bias (divergences.md #139). With W-buffering (CONTROL0
Z_PERSPECTIVE, nv2a_enter_lit_state) the depth the NV2A tests is the
interpolated w, and NV097_SET_POLYGON_OFFSET_BIAS is added to it in the
same units (xemu hw/xbox/nv2a/pgraph/glsl/psh.c: zvalue += depthOffset
before the division by the Z24 range): here w is the eye distance in world
units, one Z24 step each at the vertex program's scale (vs_lit writes the
raw eye depth, nv2a_matrix_perspective's out[2][3] = 1): the bias assumes
that unscaled w, and must scale with it if w is ever scaled. The bias is
-gl_polyoffset, GL's `units` term: 2 world units nearer at any distance,
so a decal can show through something drawn before it that stands less
than 2 units in front of its face (a lip seen at a grazing angle). The slope term (GL's factor -1) is left
at 0: xemu marks the W-buffer slope as partially implemented and it is
unchecked on hardware, while a decal is coplanar with its face, so the
constant alone separates them. Unverified on hardware.
========================================================================
*/

/* The decal vertex buffer, rewritten each frame from the start (reset in
   R_BeginFrame, after its pb_busy() wait: no draw of the previous frame
   still reads it). 2048 vertices is 16 per pool entry of the Xbox's 128
   (XASH_LOW_MEMORY 1, protocol.h:213); a polygon that does not fit is
   dropped for the frame and counted. */
/* NV2A_DECAL_FRAME_VERTS, nv2a_decal_vb and nv2a_decal_vb_used are
   declared with the 2D ring, above R_Init. */

/* What nv2a_decal_gpu_begin has pushed, for the per-polygon binds. */
static int      nv2a_decal_bound_texnum = -1;
static int      nv2a_decal_bound_premult = -1;
static float    nv2a_decal_scale_s = 1.0f, nv2a_decal_scale_t = 1.0f;
static qboolean nv2a_decal_active;

int nv2a_decal_entity_surface_front( const msurface_t *surf, const float modelorg[3] )
{
	float dist;

	/* R_CullSurface's CULL_BACKSIDE (gl_cull.c:83-110) under GL_FRONT
	   culling; r_nocull returns CULL_VISIBLE before it (gl_cull.c:76). */
	if( nv2a_r_nocull.value || VectorIsNull( surf->plane->normal ))
		return true;

	dist = PlaneDiff( modelorg, surf->plane );
	if( FBitSet( surf->flags, SURF_PLANEBACK ))
		dist = -dist;

	return dist > BACKFACE_EPSILON;
}

int nv2a_decal_world_surface_drawn( const msurface_t *surf )
{
	const model_t *world = gp_cl->models[1];
	uint32_t orig, sorted;

	if( !surf || world != nv2a_map_owner || !world || !world->surfaces || !nv2a_map_face_remap )
		return false;
	if( surf < world->surfaces || surf >= world->surfaces + world->numsurfaces )
		return false;

	orig = (uint32_t)( surf - world->surfaces );
	if( orig >= nv2a_map_face_remap_count )
		return false;
	sorted = nv2a_map_face_remap[orig];

	/* PVS and frustum, as the world pass (R_RecursiveWorldNode's visframe
	   and R_CullSurface's frustum test). */
	if( !nv2a_lm_face_in_view( sorted ))
		return false;

	return nv2a_decal_entity_surface_front( surf, nv2a_eye_origin );
}

/* R_SetRenderMode's colour for the entity (gl_rsurf.c:1717), packed as the
   combiner factor's A8R8G8B8 (xemu pgraph_argb_pack32_to_rgba_float). */
static uint32_t nv2a_decal_tint( const cl_entity_t *e, float blend )
{
	float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
	uint32_t ur, ug, ub, ua;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		r = e->curstate.rendercolor.r / 255.0f;
		g = e->curstate.rendercolor.g / 255.0f;
		b = e->curstate.rendercolor.b / 255.0f;
		a = e->curstate.renderamt / 255.0f;
		break;
	case kRenderTransAdd:
		r = g = b = blend;
		break;
	case kRenderTransAlpha:
		break; /* the non-quake branch, gl_rsurf.c:1742-1751 */
	default:
		a = blend;
		break;
	}

	ur = (uint32_t)( bound( 0.0f, r, 1.0f ) * 255.0f + 0.5f );
	ug = (uint32_t)( bound( 0.0f, g, 1.0f ) * 255.0f + 0.5f );
	ub = (uint32_t)( bound( 0.0f, b, 1.0f ) * 255.0f + 0.5f );
	ua = (uint32_t)( bound( 0.0f, a, 1.0f ) * 255.0f + 0.5f );
	return ( ua << 24 ) | ( ur << 16 ) | ( ug << 8 ) | ub;
}

int nv2a_decal_gpu_begin( const cl_entity_t *e, float blend )
{
	uint32_t *p;
	uint32_t tint;
	float offset;

	if( !nv2a_decal_vb || !nv2a_frame_open || !e )
		return false;

	/* Whatever the TriAPI still holds draws first, under its own state
	   (the world's water, r_warp.c). */
	nv2a_2d_flush();

	/* The world's decals follow the TriAPI's water, which took the GPU,
	   and re-enter the lit state (the world's matrix, the atlas). A brush
	   entity's follow its own faces, still in the lit state with the
	   entity's matrix and lightmap page - re-entering would put the
	   world's back (lessons.md: a draw that depends on state it never
	   sets), so without it the entity's decals are not drawn. */
	if( !nv2a_lit_state_current )
	{
		if( e != nv2a_get_entity_by_index( 0 ))
			return false;
		nv2a_enter_lit_state();
	}

	/* ps_decal in place of ps_lit (vs_lit stays). The upload resets the
	   final combiner, so the fog goes back on after it. */
	p = pb_begin();
	#include "ps_decal.inl"
	pb_end( p );
	nv2a_ps_selected = NV2A_PS_DECAL;
	nv2a_resident_program = NV2A_RESIDENT_NONE;
	nv2a_sgpu_entered = false;
	nv2a_fog_pushed_valid = false;
	nv2a_push_fog( &nv2a_fog_current );

	tint = nv2a_decal_tint( e, blend );
	offset = R_DecalPolyOffset();

	p = pb_begin();
	/* SAME_FACTOR_ALL (ps_decal.inl): stage 0's factor serves both
	   stages; stage 1's is written too, so the constant does not depend
	   on that bit. */
	p = pb_push1( p, NV097_SET_COMBINER_FACTOR0, tint );
	p = pb_push1( p, NV097_SET_COMBINER_FACTOR0 + 4, tint );
	p = pb_push1( p, NV097_SET_BLEND_ENABLE, 1 );
	p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
	p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA );
	p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA );
	p = pb_push1( p, NV097_SET_DEPTH_MASK, e->curstate.rendermode == kRenderTransColor ? 1 : 0 );
	p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, 0 );
	p = pb_push1( p, NV097_SET_CULL_FACE_ENABLE, 0 );
	if( offset != 0.0f )
	{
		const float factor = 0.0f, bias = -offset;
		uint32_t word;

		p = pb_push1( p, NV097_SET_POLY_OFFSET_FILL_ENABLE, 1 );
		memcpy( &word, &factor, sizeof( word ));
		p = pb_push1( p, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, word );
		memcpy( &word, &bias, sizeof( word ));
		p = pb_push1( p, NV097_SET_POLYGON_OFFSET_BIAS, word );
	}
	pb_end( p );

	/* The attribute arrays on the decal buffer; each polygon draws from
	   its own start index. nv2a_bound_vertex_base no longer describes
	   what they point at. */
	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_lit_vertex_t ), &nv2a_decal_vb[0].pos[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_decal_vb[0].uv0[0] );
	nv2a_set_attrib_pointer( 10, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_decal_vb[0].uv1[0] );
	p = pb_begin();
	p = pb_push1( p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0 );
	pb_end( p );
	nv2a_bound_vertex_base = 0xFFFFFFFFu;

	nv2a_decal_bound_texnum = -1;
	nv2a_decal_bound_premult = -1;
	nv2a_decal_active = true;
	return true;
}

/* Stage 0 on engine texture `texnum`, as GL_Bind and the 2D flush's
   stage-0 block do it (r_image.c's table, the substitutions GL_Bind makes
   for a bad or storage-less slot), plus DrawSingleDecal's blend for a
   premultiplied texture (gl_decals.c:834-836). */
static qboolean nv2a_decal_bind_texture( int texnum )
{
	uint32_t phys, pitch, width, height, color_format;
	unsigned int prev, resolved;
	qboolean bilinear;
	int premult;
	uint32_t base_size = 0;
	uint32_t *p;

	if( texnum == nv2a_decal_bound_texnum )
		return true;

	prev = nv2a_tex_current();
	GL_Bind( 0, (unsigned int)texnum );
	resolved = nv2a_tex_current();
	GL_Bind( 0, prev ); /* the TriAPI keeps its own binding */

	if( !nv2a_tex_gpu_params( resolved, &phys, &pitch, &width, &height, &bilinear, &color_format ))
		return false;
	nv2a_tex_mark_used( resolved );
	nv2a_tex_coord_scale( nv2a_tex_get( resolved ), &nv2a_decal_scale_s, &nv2a_decal_scale_t );

	if( NV2A_TEX_COLOR_IS_COMPRESSED( color_format ))
	{
		base_size = MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, (uint32_t)nv2a_2d_log2_pot( width ))
			| MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, (uint32_t)nv2a_2d_log2_pot( height ));
	}

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, phys );
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, color_format )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 )
		| base_size );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( width << 16 ) | height );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00030303 ); /* TF_DECAL is TF_CLAMP (ref_api.h:84) */
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	/* never a zero convolution-kernel field (divergences.md #102) */
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, bilinear ? 0x02022000 : 0x01012000 );

	premult = FBitSet( nv2a_tex_get( resolved )->flags, TF_PREMULTIPLIED ) ? 1 : 0;
	if( premult != nv2a_decal_bound_premult )
	{
		p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, premult ? NV097_SET_BLEND_FUNC_SFACTOR_V_ONE : NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA );
		nv2a_decal_bound_premult = premult;
	}
	pb_end( p );

	nv2a_decal_bound_texnum = texnum;
	return true;
}

/* Where the face's lightmap sits for the decal's stage-1 coordinates, in
   atlas texels: the block's origin, to which each vertex adds its luxel
   position (r_decals.c's R_DecalVertsLight) - the baker's own formula
   (BspParser.cpp: blockX + (s - texturemins) / 16 + 0.5, Xbox bakes have
   no border). A face without a usable block (no lightmap, or one
   r_lightmap.c found disagreeing with the engine) samples what its own
   baked vertices sample: the coordinates of its first one, which for a
   face without a lightmap are the bake's constant fallback texel. */
static void nv2a_decal_lightmap_origin( const msurface_t *surf, float *bx, float *by, qboolean *fixed )
{
	const model_t *world = gp_cl->models[1];
	uint32_t orig, sorted;
	const nv2a_map_face_t *face;
	const nv2a_lit_vertex_t *vb;
	uint32_t idx;

	*bx = *by = 0.0f;
	*fixed = true;

	if( !surf || world != nv2a_map_owner || !world || !world->surfaces || !nv2a_map_face_remap || !nv2a_map_faces )
		return;
	if( surf < world->surfaces || surf >= world->surfaces + world->numsurfaces )
		return;
	orig = (uint32_t)( surf - world->surfaces );
	if( orig >= nv2a_map_face_remap_count )
		return;
	sorted = nv2a_map_face_remap[orig];
	if( sorted >= nv2a_map_face_count )
		return;

	if( nv2a_map_face_lm && nv2a_map_face_lm[sorted].x != NV2A_LM_BLOCK_NONE )
	{
		*bx = (float)nv2a_map_face_lm[sorted].x;
		*by = (float)nv2a_map_face_lm[sorted].y;
		*fixed = false;
		return;
	}

	face = &nv2a_map_faces[sorted];
	if( !nv2a_map_vb || !nv2a_map_index_dwords )
		return;
	idx = ( (const uint16_t *)nv2a_map_index_dwords )[face->index_begin];
	vb = (const nv2a_lit_vertex_t *)nv2a_map_vb + nv2a_face_vertex_base( face ) + idx;
	*bx = vb->uv1[0];
	*by = vb->uv1[1];
}

void nv2a_decal_gpu_draw( int texture, const float *v, int numverts, const msurface_t *surf )
{
	float bx, by;
	qboolean fixed;
	nv2a_lit_vertex_t *out;
	uint32_t *p;
	int i;

	if( !nv2a_decal_active || numverts < 3 || numverts > 256 )
		return;

	if( nv2a_decal_vb_used + (uint32_t)numverts > NV2A_DECAL_FRAME_VERTS )
	{
		nv2a_decal_stats.dropped++;
		return;
	}

	if( !nv2a_decal_bind_texture( texture ))
		return;

	nv2a_decal_lightmap_origin( surf, &bx, &by, &fixed );

	out = &nv2a_decal_vb[nv2a_decal_vb_used];
	for( i = 0; i < numverts; i++, v += VERTEXSIZE, out++ )
	{
		out->pos[0] = v[0];
		out->pos[1] = v[1];
		out->pos[2] = v[2];
		out->uv0[0] = v[3] * nv2a_decal_scale_s;
		out->uv0[1] = v[4] * nv2a_decal_scale_t;
		out->uv1[0] = fixed ? bx : bx + v[5];
		out->uv1[1] = fixed ? by : by + v[6];
	}

	/* GL_POLYGON: a convex polygon, the same triangles as a fan. The
	   vertex fetch cache is broken first: these vertices were written
	   after the GPU started on the previous polygon, whose last cache
	   line they may share (28-byte stride) - as nv2a_bind_vertex_base and
	   the 2D ring do whenever the array contents change. */
	p = pb_begin();
	p = pb_push1( p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0 );
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLE_FAN );
	p = pb_push1( p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK( NV097_DRAW_ARRAYS_COUNT, (uint32_t)numverts - 1 )
		| MASK( NV097_DRAW_ARRAYS_START_INDEX, nv2a_decal_vb_used ));
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
	pb_end( p );
	NV2A_FT_ADD( XBOX_FT_W_DRAWS, 1 );
	NV2A_FT_ADD( XBOX_FT_W_PRIMS, 1 );
	NV2A_FT_ADD( XBOX_FT_W_VERTS_DECAL, numverts );

	nv2a_decal_vb_used += (uint32_t)numverts;
}

void nv2a_decal_gpu_end( void )
{
	uint32_t *p;

	if( !nv2a_decal_active )
		return;

	p = pb_begin();
	p = pb_push1( p, NV097_SET_POLY_OFFSET_FILL_ENABLE, 0 );
	p = pb_push1( p, NV097_SET_CULL_FACE_ENABLE, 1 );
	p = pb_push1( p, NV097_SET_DEPTH_MASK, 1 );
	p = pb_push1( p, NV097_SET_BLEND_ENABLE, 0 );
	pb_end( p );

	/* The program, the arrays and stage 0 are the decals' now: the next
	   lit draw re-enters the lit state (ps_lit, arrays, constants) and the
	   TriAPI its own. */
	nv2a_lit_state_current = false;
	nv2a_2d_dirty = true;
	nv2a_decal_active = false;
}

/* X9-X step 8: hands r_lightmap.c this frame's map, view and the brush
   entities that will be drawn with their lightmap (R_HasLightmap,
   nv2a_entity_has_lightmap), then lets it rebuild what changed. */
static void nv2a_update_dynamic_lightmaps( void )
{
	static cl_entity_t *brush[NV2A_MAX_ENTITIES * 2];
	model_t *world = gp_cl->models[1];
	nv2a_lm_view_t view;
	int i, n = 0;

	if( !world || world != nv2a_map_owner || !nv2a_map_lightmap || !nv2a_map_face_lm
		|| !nv2a_map_faces || !nv2a_map_face_remap )
		return;

	for( i = 0; i < nv2a_num_solid_entities; i++ )
	{
		cl_entity_t *e = nv2a_solid_entities[i];

		if( e->model->type == mod_brush && nv2a_entity_has_lightmap( world, e ) && nv2a_brush_entity_visible( world, e, NULL, NULL ))
			brush[n++] = e;
	}
	for( i = 0; i < nv2a_num_trans_entities; i++ )
	{
		cl_entity_t *e = nv2a_trans_entities[i];

		if( e->model->type == mod_brush && nv2a_entity_has_lightmap( world, e ) && nv2a_brush_entity_visible( world, e, NULL, NULL ))
			brush[n++] = e;
	}

	view.world = world;
	view.atlas = (uint32_t *)nv2a_map_lightmap;
	view.atlas_width = nv2a_map_lightmap_width;
	view.atlas_height = nv2a_map_lightmap_height;
	view.atlas_stride_words = nv2a_map_lightmap_pitch / 4;
	view.blocks = nv2a_map_face_lm;
	view.remap = nv2a_map_face_remap;
	view.remap_count = nv2a_map_face_remap_count;
	view.face_count = nv2a_map_face_count;
	view.framecount = (int)nv2a_current_frame_number();
	view.face_in_view = nv2a_lm_face_in_view;

	R_LightmapUpdate( &view, brush, n );
}

/* X9-X item 14 step 10: the skybox (r_sky.c, divergences.md #137). The
   half of R_DrawWorld (gl_rsurf.c:3695-3719) that ref/gl spreads over its
   world walk: R_ClearSkyBox, every visible SURF_DRAWSKY world face into
   R_AddSkyBoxSurface (R_RecursiveWorldNode's skychain, gl_rsurf.c:3518,
   R_DrawTextureChains, :1532), then R_DrawSkyBox when any face came
   through (:3718, skipped in the dev overview as there).

   Drawn BEFORE the world with depth off (#137), so it cannot ride on
   nv2a_draw_map_geometry's own walk, which draws as it goes; it walks the
   sky batches itself instead. A sky face's texture is the map's sky
   texture, which the engine flags SURF_DRAWSKY by name (Mod_LoadSurfaces),
   and a batch is one texture, so a batch whose first face is sky is all
   sky - the rest are skipped at one byte each. Each face gets the world
   pass's own tests, in the world pass's order: world face (not a brush
   entity's - ref/gl never draws their sky either, R_RenderBrushPoly,
   gl_rsurf.c:1483), frustum, PVS, then the back-face test of
   R_CullSurface (gl_cull.c:83-110). Without a per-face table there is no
   face-to-surface mapping, and the world pass draws the sky batch as a
   plain wall, as before this step. */
static void nv2a_draw_sky( void )
{
	const model_t *world = gp_cl->models[1];
	uint32_t b, i;

	if( world != nv2a_map_owner )
		world = NULL;

	R_ClearSkyBox( nv2a_eye_origin );

	if( world == NULL || nv2a_map_faces == NULL || nv2a_map_face_orig == NULL )
		return;

	/* FWORLD_SKYSPHERE without a custom skybox: ref/gl draws Quake clouds
	   and no box (gl_rsurf.c:1526-1541). Not ported (#137); a Half-Life map
	   always has a custom skybox (R_SetupSky falls back to "desert"). */
	if( ENGINE_GET_PARM( PARM_SKY_SPHERE ))
		return;

	for( b = 0; b < nv2a_map_batch_count; b++ )
	{
		const nv2a_map_batch_t *batch = &nv2a_map_batches[b];
		uint32_t end_face;

		if( (uint64_t)batch->startFace + batch->faceCount > (uint64_t)nv2a_map_face_count || batch->faceCount == 0 )
			continue;
		if( !nv2a_map_faces[batch->startFace].sky )
			continue;

		end_face = batch->startFace + batch->faceCount;
		for( i = batch->startFace; i < end_face; i++ )
		{
			const nv2a_map_face_t *face = &nv2a_map_faces[i];
			uint32_t orig = nv2a_map_face_orig[i];
			const msurface_t *surf;

			if( !face->sky || !nv2a_map_face_is_world( world, i ))
				continue;
			if( orig >= (uint32_t)world->numsurfaces )
				continue;
			if( nv2a_frustum_cull_face( face ))
				continue;
			if( nv2a_map_face_visible != NULL && !nv2a_r_novis.value && !nv2a_map_face_visible[i] )
				continue;

			surf = &world->surfaces[orig];

			/* R_CullSurface's back-face test, as nv2a_draw_map_geometry
			   applies it (gl_cull.c:76: r_nocull returns first). */
			if( !nv2a_r_nocullback.value && !nv2a_r_nocull.value && !VectorIsNull( surf->plane->normal ))
			{
				float dist = PlaneDiff( nv2a_eye_origin, surf->plane );

				if( FBitSet( surf->flags, NV2A_SURF_PLANEBACK ))
					dist = -dist;
				if( dist <= BACKFACE_EPSILON )
					continue;
			}

			R_AddSkyBoxSurface( world, surf );
		}
	}

	if( R_SkyHasSurfaces() && !ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
	{
		/* A full TriAPI re-entry, whatever was pushed last: this is the
		   frame's first 3D TriAPI batch, and the GPU may hold the 2D
		   pass's or the world's state (lessons.md: a draw that depends
		   on state it never sets breaks by what else is on screen). */
		nv2a_2d_dirty = true;
		R_WarpSkyFogBegin(); /* X9-X item 14 step 11: gl_warp.c:334-338 */
		R_DrawSkyBox();
		/* Submitted now, before the world pushes its own state directly. */
		nv2a_2d_flush();
		R_WarpSkyFogEnd();
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
	double rvp_frametime; /* tr.frametime, gl_rmain.c:949-952 */
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
	XBOX_FT_SPAN( XBOX_FT_S_PVS, nv2a_mark_leaves( rvp->vieworigin ));

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
	/* X9-X step 5 Part C: kept without the viewport, for the view model's
	   own z-ranged re-combination (nv2a_2d_flush). */
	memcpy( nv2a_view_proj_combined, combined, sizeof( combined ));

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

	/* X9-X step 4 Part E: the SAME camera as nv2a_map_viewproj above, in
	   ref/common's own convention - see nv2a_gl_worldviewproj's own
	   declaration comment for why this is a second, independent build
	   rather than a transposed reuse. Port of ref/soft's own
	   R_SetupModelviewMatrix/R_SetupProjectionMatrix (r_main.c:379-438),
	   but with the SAME zNear=1/zFar=8192 the TriAPI draw path actually
	   renders with (nv2a_matrix_perspective( proj, xscale, yscale, 1.0f,
	   8192.0f ) above, this same function), NOT ref/gl's own
	   R_GetFarClip-derived zNear=4/zFar=max(256,farclip) (r_main.c:
	   379-384; byte-for-byte identical in gl_rmain.c:332-337,369-425).
	   codex review, real finding (divergences.md #119 point 2): the two
	   z-ranges used to disagree, and CFrustum::CalculateFrustum
	   (particleman/CFrustum.cpp:38-104) genuinely reads GetMatrix's
	   reported projection to extract real FRONT/BACK culling planes for
	   CBaseParticle::CheckVisibility (particleman/CBaseParticle.cpp:
	   106-146) - the earlier claim that "nothing reads the mismatch" was
	   false. Reporting the world's own actual clip volume instead fixes
	   the culling planes without touching the shared depth buffer this
	   was built for; only the FRONT/BACK planes move, since LEFT/RIGHT/
	   TOP/BOTTOM depend only on fov_x/fov_y, not on zNear/zFar. */
	{
		matrix4x4 worldview, projection, wvp;
		float zNear, zFar, yMax, xMax;

		zNear = 1.0f;
		zFar = 8192.0f;

		yMax = zNear * tanf((float)( DEG2RAD( rvp->fov_y ) * 0.5 ));
		xMax = zNear * tanf((float)( DEG2RAD( rvp->fov_x ) * 0.5 ));

		Matrix4x4_CreateProjection( projection, xMax, -xMax, yMax, -yMax, zNear, zFar );

		Matrix4x4_CreateModelview( worldview );
		Matrix4x4_ConcatRotate( worldview, -rvp->viewangles[2], 1, 0, 0 );
		Matrix4x4_ConcatRotate( worldview, -rvp->viewangles[0], 0, 1, 0 );
		Matrix4x4_ConcatRotate( worldview, -rvp->viewangles[1], 0, 0, 1 );
		Matrix4x4_ConcatTranslate( worldview, -rvp->vieworigin[0], -rvp->vieworigin[1], -rvp->vieworigin[2] );

		Matrix4x4_Concat( wvp, projection, worldview );

		Matrix4x4_Copy( nv2a_gl_worldview, worldview );
		Matrix4x4_Copy( nv2a_gl_projection, projection );
		Matrix4x4_Copy( nv2a_gl_worldviewproj, wvp );
	}

	nv2a_view_valid = true;

	/* X9-X step 5: the studio path's own per-frame context (r_studio.c
	   keeps this private instead of a shared RI/tr - r_studio.h's own
	   comment). R_RunViewmodelEvents runs here, before the world,
	   matching gl_rmain.c:1103 - it only computes the view model's own
	   bones/attachments and processes its client events (elights, model
	   sounds), no GPU state touched, so the ordering relative to the
	   flush/shadow-reset just below does not matter. */
	nv2a_studio_set_frame_context( rvp->flags, rvp->vieworigin, forward, right, up );
	/* X9-X step 6: same for the sprite path (r_sprite.h) - the RI.rvp
	   fields and RI.v* vectors gl_sprite.c reads. */
	R_SpriteSetFrameContext( rvp->flags, rvp->vieworigin, rvp->viewangles, rvp->viewport, forward, right, up );
	/* X9-X step 7: and for the effects (r_effects.h), with tr.frametime
	   as R_RenderScene computes it (gl_rmain.c:949-952). */
	rvp_frametime = !FBitSet( rvp->flags, RF_DRAW_CUBEMAP ) ? gp_cl->time - gp_cl->oldtime : 0.0;
	R_EfxSetFrameContext( rvp->flags, rvp->vieworigin, rvp->viewport, forward, right, up, rvp_frametime );
	R_RunViewmodelEvents();

	/* X9-X step 4 Part F: flush whatever the 2D pass had pending (a HUD
	   draw from a previous frame's tail, or - defensively - anything from
	   earlier this same frame) and reset the shadow to R_SetupGL's own
	   tail state (gl_rmain.c:568-572) before the world draw, which never
	   goes through nv2a_2d_desired/nv2a_2d_append() at all (it pushes its
	   own GPU state directly, nv2a_set_3d_state()) - the NEXT 2D or
	   TriAPI batch must not inherit a stale shadow left over from before
	   the world's own draw ran. */
	nv2a_2d_flush();
	nv2a_2d_reset_shadow_for_3d();

	/* X9-X item 14 step 11: R_SetupFrame's glState.isFogEnabled sample
	   (gl_rmain.c:517), then R_DrawFog (:965) - the fog R_CheckFog chose
	   last frame - before anything 3D draws. */
	R_WarpSetupFrame( rvp->vieworigin );
	R_WarpDrawFog();

	/* X9-X: the world is drawn here, where the engine asks for it
	   (V_RenderView), as ref/gl's R_RenderFrame does (gl_rmain.c:1076) -
	   not in R_EndFrame, which runs after the engine's 2D (HUD, console,
	   menu: V_PostRender, cl_view.c:529-573) and would draw over it. */
	if( nv2a_frame_open && nv2a_map_ready )
	{
		xbox_ft_span_t world_span;

		/* X9-X step 8: lightstyles and dynamic lights, written into the
		   atlas BEFORE this frame's first draw that samples it (the world,
		   then brush entities below). Safe against the GPU: R_EndFrame
		   and R_BeginFrame both spun on pb_busy(), so every draw of the
		   previous frame has retired, and only 2D (no atlas) has been
		   pushed since R_BeginFrame. M2 span k_lm (#141): the rebuild
		   and its brush-entity gather. */
		XBOX_FT_SPAN( XBOX_FT_S_LM, nv2a_update_dynamic_lightmaps());
		/* X9-X item 14 step 9: this frame's animation time (and, on a new
		   map's first frame, its tables), for the world and the brush
		   entities drawn after it. */
		nv2a_texanim_begin_frame( gp_cl->models[1] == nv2a_map_owner ? gp_cl->models[1] : NULL );
		/* M2 span k_world (#141): sky, world and world decals. */
		xbox_ft_span_begin( &world_span );
		/* X9-X item 14 step 10: the skybox, before the world (#137). */
		XBOX_FT_SPAN( XBOX_FT_S_SKY, nv2a_draw_sky());
		nv2a_draw_map_geometry(); /* ends with the world's opaque water (r_warp.c) */
		/* X9-X item 14 step 12: R_DrawWorld's DrawDecalsBatch, after the
		   texture chains (gl_rsurf.c:3707-3711). */
		XBOX_FT_SPAN( XBOX_FT_S_WORLD_DECALS, R_DecalDrawWorld());
		xbox_ft_span_end( &world_span, XBOX_FT_S_WORLD );
		nv2a_world_drawn_this_frame = true;
	}

	/* X9-X step 4 Part F: only once nv2a_frame_open (R_BeginFrame already
	   ran, R_EndFrame has not) - the menu preview call this function also
	   receives (this function's own header comment) returns before this
	   point (no RF_DRAW_WORLD), so it never reaches here. */
	if( nv2a_frame_open )
	{
		/* X9-X step 5: the same world model_t nv2a_draw_map_geometry()
		   itself computes (nv2a_map_owner-gated) - the entity passes below
		   need it too, for their own brush-entity draws. */
		const model_t *world = gp_cl->models[1];

		if( world != nv2a_map_owner )
			world = NULL;

		/* The world draw above pushed its own program, constants, array
		   formats, alpha func and stage 1 directly (nv2a_draw_map_geometry,
		   not through nv2a_2d_desired/append at all) - the 2D/TriAPI pass
		   must fully re-enter the GPU state before its own next flush. */
		nv2a_2d_dirty = true;

		if( nv2a_world_drawn_this_frame )
			gEngfuncs.R_DrawWorldHull(); /* port of R_DrawWorld's own tail, gl_rsurf.c:3728 */

		/* X9-X item 14 step 11: R_CheckFog, right after R_DrawWorld
		   (gl_rmain.c:971). */
		R_WarpCheckFog( rvp->flags, nv2a_viewleaf );

		/* X9-X step 5, gl_rmain.c:797-935's own order: solid entities
		   (brush direct, studio through R_DrawStudioModel), then solid
		   beams, then pfnDrawNormalTriangles, then translucent entities
		   sorted, then pfnDrawTransparentTriangles, then the translucent
		   effects, then the view model. */
		nv2a_draw_solid_entities( world );

		/* X9-X step 7: solid beams (gl_rmain.c:859-862) - the engine's
		   CL_DrawEFX (cl_efx.c:2079) frees dead beams and calls back
		   CL_DrawBeams (r_effects.c). */
		if( !FBitSet( rvp->flags, RF_ONLY_CLIENTDRAW ))
			XBOX_FT_SPAN( XBOX_FT_S_EFX, gEngfuncs.CL_DrawEFX( rvp_frametime, false ));

		XBOX_FT_SPAN( XBOX_FT_S_TRI_HOOKS, gEngfuncs.pfnDrawNormalTriangles());
		nv2a_draw_trans_entities( world );
		XBOX_FT_SPAN( XBOX_FT_S_TRI_HOOKS, gEngfuncs.pfnDrawTransparentTriangles());

		/* X9-X step 7: translucent beams, particles and tracers
		   (gl_rmain.c:919-924), after the client's own translucent
		   triangles, with R_AllowFog( false ) around them
		   (gl_rmain.c:921-923; X9-X item 14 step 11). */
		if( !FBitSet( rvp->flags, RF_ONLY_CLIENTDRAW ))
		{
			R_WarpAllowFog( false );
			XBOX_FT_SPAN( XBOX_FT_S_EFX, gEngfuncs.CL_DrawEFX( rvp_frametime, true ));
			R_WarpAllowFog( true );
		}

		nv2a_2d_desired.blend_enable = false; // pglDisable( GL_BLEND ), gl_rmain.c:928 "Trinity Render issues"

		R_DrawViewModel();

		/* X9-X item 14 step 11: R_DrawWaterSurfaces, after
		   R_DrawEntitiesOnList (gl_rmain.c:977) - the translucent world
		   water, when sv_wateralpha is below 1. */
		XBOX_FT_SPAN( XBOX_FT_S_WARP_TRANS, R_WarpDrawWaterSurfaces( rvp->flags ));

		/* Ends the frame with nothing queued - the SDK's own tri hooks
		   above may have batched TriAPI primitives that would otherwise
		   sit pending until the engine's own 2D pass (V_PostRender)
		   happens to trigger a flush. */
		nv2a_2d_flush();
	}
}

static void GL_OrthoBounds( const float *mins, const float *maxs )
{
	;
}

static qboolean R_SpeedsMessage( char *out, size_t size )
{
	return false;
}

/* X9-X step 7: ref/gl's Mod_GetCurrentVis (gl_rsurf.c:112) - RI.visbytes,
   the PVS row R_MarkLeaves filled (here nv2a_mark_leaves). The
   drawFuncs->Mod_GetCurrentVis branch is for tr.fCustomRendering, which
   this renderer never sets (no R_RenderFrame override). The engine reads
   this too: CL_BeamVisible (cl_efx.c:860) and TriBoxInPVS
   (cl_game.c:3246). NULL (all visible) until a real row exists. */
static byte *Mod_GetCurrentVis( void )
{
	return nv2a_visbytes_valid ? nv2a_visbytes : NULL;
}

byte *nv2a_mod_get_current_vis( void )
{
	return Mod_GetCurrentVis();
}

/*
========================================================================
X9-X Part F, generalised in step 4 Part C into one immediate-mode pass
with two spaces: 2D (R_DrawStretchPic/FillRGBA/the console/HUD/menu) and
3D (the TriAPI, D below). One packed vertex per space (2D: 20 bytes,
position/texel-unit texcoord/byte colour; 3D: 24 bytes, adding z) batched
through the SAME 256 KiB vertex ring D1 allocates in R_Init (both strides
are multiples of 4), flushed whenever the desired GPU-state shadow, the
resolved texture-stage key, the space, the primitive op, or an explicit
"dirty" flag changes from what was last actually pushed - see
nv2a_2d_flush()'s own comment for the GPU-state re-entry sequence and
citations.
========================================================================
*/

typedef struct
{
	float x, y;
	float u, v;
	byte  rgba[4];
} __attribute__(( packed )) nv2a_2d_vertex_t;

/* X9-X step 4 Part C: the TriAPI's own 3D vertex - IDENTICAL layout to
   nv2a_tri_util.h's own nv2a_tri_vert_t (the assembler's staged-vertex
   type), so a completed primitive from that host-tested assembler is
   already this wire format with no repacking. Kept as a distinct name
   here only because this file also needs sizeof() for the ring's stride
   math, same as nv2a_2d_vertex_t above. */
typedef nv2a_tri_vert_t nv2a_tri_vertex_t;

/* Which of the two immediate-mode spaces a batch belongs to - see this
   block's own header comment. */
typedef enum
{
	NV2A_IMM_SPACE_2D,
	NV2A_IMM_SPACE_3D
} nv2a_imm_space_t;

/* The resolved GPU texture-stage parameters of the pending batch's own
   texture - NOT its texnum (C1's own note): a same-frame slot reuse
   (GL_UpdateTexture on a still-pending texnum) must not silently
   redirect an already-queued batch to different pixels. */
typedef struct
{
	uint32_t offset, pitch, width, height;
	qboolean bilinear;
	uint32_t color_format; /* NV2A_TEX_COLOR_* - studio skins are 16 bpp, everything else A8B8G8R8 */
	/* X9-X step 11: the rest of the stage-0 words, so a map directory
	   texture (nv2a_tri_bind_map_texture) keeps the world's own WRAP
	   addressing, mip chain and filter - every texture-table texture
	   still gets the clamp/one-level/plain-filter values it always had. */
	uint32_t address, mip_levels, control0, filter;
} nv2a_2d_batch_key_t;

/* X9-X step 11: the map directory texture bound for the TriAPI instead of
   GL_Bind's (r_studio_bridge.h), or NV2A_TRI_NO_MAP_TEXTURE. */
static uint32_t nv2a_tri_map_texture = NV2A_TRI_NO_MAP_TEXTURE;

void nv2a_tri_bind_map_texture( uint32_t dir )
{
	nv2a_tri_map_texture = dir;
}

/* X9-X step 4 Part C: nv2a_2d_desired is now the single GL-like current
   state GL_SetRenderMode/VGUI_SetupDrawing/FillRGBA/TriRenderMode all
   write (ref/gl shares one GL state machine across all four too) -
   depth_mask/cull_enable added for the TriAPI's own CullFace/
   TriRenderMode. The SAME struct is also used for nv2a_2d_pushed/
   nv2a_2d_batch_state, where depth_mask/cull_enable instead hold the
   EFFECTIVE (post 2D-forcing) values - see the append path's own
   comment.

   X9-X step 5: the struct itself (with two new fields, alpha_ref and
   viewmodel_depthrange) now lives in r_studio_bridge.h, included above,
   so r_studio.c's own per-mesh state changes write the SAME shadow this
   file's flush pushes - see that header's own comment. */

static qboolean nv2a_in2d;
static byte     nv2a_2d_color[4] = { 255, 255, 255, 255 }; /* current colour, shared with TriAPI */

nv2a_2d_shadow_t nv2a_2d_desired = {
	false, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA, false,
	0, /* alpha_ref: DEFAULT_ALPHATEST 0, same as the flush's own re-entry push previously hardcoded */
	true, true, /* depth_mask/cull_enable: R_SetupGL's own 3D defaults (gl_rmain.c:568-572) until R_Set2DMode(true) first forces them off */
	false, /* viewmodel_depthrange: off until R_DrawViewModel (r_studio.c) turns it on */
	NV2A_IMM_SPACE_2D, NV097_SET_BEGIN_END_OP_QUADS
};

/* X9-X step 4 Part F: called by GL_RenderFrame right after the world
   draw (nv2a_draw_map_geometry pushes its own GPU state directly, never
   through nv2a_2d_desired) - R_SetupGL's own tail state (gl_rmain.c:
   568-572): cull on, blend off, alpha test off, colour 255x4. depth_mask
   is deliberately untouched (not part of R_SetupGL's own reset). */
static void nv2a_2d_reset_shadow_for_3d( void )
{
	nv2a_2d_desired.blend_enable = false;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.cull_enable = true;
	nv2a_2d_color[0] = nv2a_2d_color[1] = nv2a_2d_color[2] = nv2a_2d_color[3] = 255;
	/* X9-X step 6: R_Clear's pglDepthFunc( GL_LEQUAL ) (gl_rmain.c:322),
	   which ref/gl runs at the top of every scene. */
	nv2a_2d_desired.depth_func_equal = false;
}

static nv2a_2d_shadow_t nv2a_2d_pushed;
/* The render state the pending batch was appended under. A flush is
   triggered by the NEXT quad's different state, by which time
   nv2a_2d_desired already holds that next state - so the flush pushes
   this snapshot, not nv2a_2d_desired. Found live in xemu (X9-X): the
   engine brackets each HUD draw as GL_SetRenderMode(mode), draw,
   GL_SetRenderMode(kRenderNormal) (cl_game.c pfnSPR_DrawAdditive), and
   additive HUD sprites came out opaque on black boxes when flushed under
   the following SPR_Draw's alpha-test mode. */
static nv2a_2d_shadow_t nv2a_2d_batch_state;
static qboolean nv2a_2d_pushed_valid;

static nv2a_2d_batch_key_t nv2a_2d_batch_key;
static qboolean nv2a_2d_dirty = true; /* forces a full GPU-state re-entry the first time the 2D pass ever flushes */
/* nv2a_resident_program and nv2a_2d_ring are declared near
   nv2a_init_lit_shader() above - both are needed by code defined well
   before this point in the file (R_Init/R_Shutdown,
   nv2a_init_lit_shader() itself). */

static uint32_t nv2a_2d_ring_write_offset;
static uint32_t nv2a_2d_ring_batch_base_offset;
static uint32_t nv2a_2d_ring_pending_verts;

/* Tracks the ring base offset the vertex attribute-array pointers were
   last actually pointed at - INDEPENDENT of nv2a_2d_dirty, which only
   covers the once-per-re-entry GPU state (shader/matrix/array-format/
   alpha-test). A texture- or blend-only flush still starts a brand-new
   batch at a brand-new ring offset (nv2a_2d_append_quad()'s own
   comment), so the pointers must be checked against THIS on every
   flush, not just the rare full re-entry - see nv2a_2d_flush(). */
static uint32_t nv2a_2d_pushed_base_offset;
static qboolean nv2a_2d_pushed_base_valid;

/* D4/D6 accessors r_image.c's own texture manager needs from here. */
qboolean nv2a_frame_is_open( void ) { return nv2a_frame_open; }
uint32_t nv2a_current_frame_number( void ) { return debug_frame_count; }
qboolean nv2a_global_texture_nearest( void ) { return nv2a_r_texturenearest.value != 0.0f; }

/* X9-X: the same cgc/vp20compiler/fp20compiler pipeline vs_lit.inl
   already went through - see vs_2d.vs.cg/ps_2d.ps.cg's own provenance
   comments and docs/r3d/design/x9-x-textures-and-2d.md's shader-facts
   section for the scratch compile that established the exact register
   layout these two headers carry. Otherwise identical in shape to
   nv2a_init_lit_shader() above. */
static void nv2a_init_2d_shader( void )
{
	nv2a_sgpu_entered = false;

	nv2a_select_vertex_program( NV2A_VP_2D );
	nv2a_select_pixel_program( NV2A_PS_2D );
}

/* X9-X step 4 Part B: the TriAPI's own vertex program (vs_tri.vs.cg/
   vs_tri.inl - same cgc/vp20compiler pipeline as vs_2d, reproduced
   byte-for-byte before being committed). The pixel program is ps_2d.inl
   REUSED verbatim (tex0 x col0 modulate) - ref/gl's own TriRenderMode
   forces GL_MODULATE (gl_triapi.c:42) and ps_2d already is one, so no
   third fp20 program is needed. Otherwise identical in shape to
   nv2a_init_2d_shader() above. */
static void nv2a_init_tri_shader( void )
{
	nv2a_sgpu_entered = false;

	nv2a_select_vertex_program( NV2A_VP_TRI );
	nv2a_select_pixel_program( NV2A_PS_2D );
}

static qboolean nv2a_2d_resolve_batch_key( nv2a_2d_batch_key_t *key )
{
	uint32_t phys, pitch, width, height;
	qboolean bilinear;
	uint32_t color_format;

	/* X9-X step 11: a map directory texture, with exactly the words
	   nv2a_bind_map_texture_stage0 gives the world's own faces. */
	if( nv2a_tri_map_texture != NV2A_TRI_NO_MAP_TEXTURE )
	{
		const nv2a_map_texture_t *tex;
		qboolean nearest = nv2a_r_texturenearest.value != 0.0f;

		if( nv2a_tri_map_texture >= nv2a_map_texture_count || !nv2a_map_texture_data )
			return false;
		tex = &nv2a_map_textures[nv2a_tri_map_texture];
		key->offset = ( (uint32_t)nv2a_map_texture_data + (uint32_t)tex->physOffset ) & 0x03ffffff;
		key->width = 1u << tex->log2Width;
		key->height = 1u << tex->log2Height;
		key->pitch = key->width * 2; /* DXT1, as nv2a_bind_map_texture_stage0 */
		key->bilinear = !nearest;
		key->color_format = NV2A_TEX_COLOR_L_DXT1;
		key->address = 0x00010101;
		key->mip_levels = tex->mipLevels;
		key->control0 = 0x4003ffc0 | nv2a_anisotropy_bits();
		key->filter = ( nearest ? 0x01032000 : 0x02062000 ) | nv2a_lod_bias_bits();
		return true;
	}

	if( !nv2a_tex_gpu_params( nv2a_tex_current(), &phys, &pitch, &width, &height, &bilinear, &color_format ))
		return false; /* GL_Bind already substitutes the default whenever a slot has no storage - defensive only */

	/* Every primitive queued against a texture counts as a use THIS
	   frame, not only the GL_Bind that selected it (which may have been
	   frames ago): the studio skin cache must never evict, and hand to
	   the next upload, bytes a draw queued this frame still reads. */
	nv2a_tex_mark_used( nv2a_tex_current());

	key->offset = phys;
	key->pitch = pitch;
	key->width = width;
	key->height = height;
	key->bilinear = bilinear;
	key->color_format = color_format;
	key->address = 0x00030303;
	key->mip_levels = 1;
	key->control0 = 0x4003ffc0;
	/* D5/C4: NEVER a zero convolution-kernel field (divergences.md #102) - 0x01012000/0x02022000, same values nv2a_draw_map_geometry already proved. */
	key->filter = bilinear ? 0x02022000 : 0x01012000;
	return true;
}

/* Stage 0 for a resolved batch key (nv2a_2d_resolve_batch_key): the
   TriAPI flush's own block, shared since divergences.md #143 with the GPU
   studio path, which binds the same skins. */
/* The texture stage 0 words for a batch key (NV2A_STAGE0_DWORDS of them),
   appended at p; nv2a_push_stage0 pushes them as a block of their own. */
#define NV2A_STAGE0_DWORDS 14u
static uint32_t *nv2a_emit_stage0( uint32_t *p, const nv2a_2d_batch_key_t *key )
{
	uint32_t base_size = 0;

	/* divergences.md #131: a compressed texture (a baked DXT1 menu
	   tile) is sized ONLY by the FORMAT word's BASE_SIZE_U/V log2
	   fields - IMAGE_RECT and pitch size linear textures only, and
	   left at 0 a DXT texture is 1x1 (divergences.md #100, the
	   world's own DXT1 push above). nv2a_tex_upload_compressed only
	   accepts power-of-two sizes, so the log2 is exact. Its texture
	   coordinates are normalized (nv2a_tex_coord_scale). */
	if( NV2A_TEX_COLOR_IS_COMPRESSED( key->color_format ))
	{
		base_size = MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, (uint32_t)nv2a_2d_log2_pot( key->width ))
			| MASK( NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, (uint32_t)nv2a_2d_log2_pot( key->height ));
	}

	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, key->offset );
	NV2A_FT_ADD( XBOX_FT_W_TEX_BINDS, 1 );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, key->color_format )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, key->mip_levels )
		| base_size );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, key->pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( key->width << 16 ) | key->height );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, key->address );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, key->control0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, key->filter ); /* nv2a_2d_resolve_batch_key */
	return p;
}

static void nv2a_push_stage0( const nv2a_2d_batch_key_t *key )
{
	uint32_t *p = pb_begin();

	p = nv2a_emit_stage0( p, key );
	pb_end( p );
}

/* The number of vertices ONE output primitive of a given hardware
   BEGIN_END op needs - the reverse of nv2a_tri_verts_per_output_prim()
   (which maps a TOPOLOGY, not an op, and lives in the pbkit-free
   nv2a_tri_util.c). Every op nv2a_2d_batch_state.op is ever set to is one
   of these four (2D append_quad's own QUADS, or Begin()'s own
   nv2a_tri_topology_op() mapping) - no default case needed, but one kept
   defensively rather than reading past this switch. */
static uint32_t nv2a_2d_op_verts_per_prim( uint32_t op )
{
	switch( op )
	{
	case NV097_SET_BEGIN_END_OP_POINTS:    return 1;
	case NV097_SET_BEGIN_END_OP_LINES:     return 2;
	case NV097_SET_BEGIN_END_OP_TRIANGLES: return 3;
	case NV097_SET_BEGIN_END_OP_QUADS:     return 4;
	default:                               return 4;
	}
}

typedef struct { uint32_t op; } nv2a_2d_draw_ctx_t;

/* One DRAW_ARRAYS chunk, fully self-contained (its own BEGIN_END pair) -
   mirrors nv2a_draw_index_range()'s own precedent above (multiple small
   primitives, not one spanning several pushbuffer kicks), and trivially
   respects the 128-dword-per-block limit
   (deps/nxdk/lib/pbkit/pbkit_pushbuffer.h's own comment) either way. The
   op is now the CALLER's own (X9-X step 4 Part C: 2D always draws QUADS,
   the TriAPI draws whatever Begin()'s primitive maps to) rather than a
   hardcoded QUADS. */
static void nv2a_2d_emit_draw_arrays( uint32_t start, uint32_t count, void *ctx )
{
	uint32_t op = ((nv2a_2d_draw_ctx_t *)ctx)->op;
	uint32_t *p = pb_begin();

	p = pb_push1( p, NV097_SET_BEGIN_END, op );
	p = pb_push1( p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK( NV097_DRAW_ARRAYS_COUNT, count - 1 )
		| MASK( NV097_DRAW_ARRAYS_START_INDEX, start ));
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
	pb_end( p );
	NV2A_FT_ADD( XBOX_FT_W_PRIMS, 1 );
}

/* Which space nv2a_2d_flush() last actually re-entered the GPU state for -
   INDEPENDENT of nv2a_2d_dirty (X9-X step 4 Part C): the common re-entry
   below must also run whenever the batch's own space differs from this,
   since the two spaces need different shaders, depth-test enable and
   transform constants even when nothing else went "dirty". */
static nv2a_imm_space_t nv2a_2d_last_reentry_space = NV2A_IMM_SPACE_2D;

/* X9-X step 5 Part C: same reasoning as nv2a_2d_last_reentry_space above,
   for the view-model depth-range flag - a change must force the common
   re-entry block below too, since that is the only place the constant-96
   matrix is (re)pushed. */
static qboolean nv2a_2d_last_reentry_viewmodel;

/* Tracks which space the vertex-attribute pointers were last pushed for -
   same reasoning as nv2a_2d_pushed_base_offset below: a same-offset,
   different-space batch (unlikely but not impossible once the ring
   wraps) must still re-push the pointers with the OTHER stride. */
static nv2a_imm_space_t nv2a_2d_pushed_base_space = NV2A_IMM_SPACE_2D;

/* Draws (and clears) the pending batch. */
static void nv2a_2d_flush( void )
{
	if( nv2a_2d_ring_pending_verts == 0 )
		return;

	if( nv2a_2d_dirty || nv2a_2d_last_reentry_space != nv2a_2d_batch_state.space
		|| ( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_3D && nv2a_2d_last_reentry_viewmodel != nv2a_2d_batch_state.viewmodel_depthrange ))
	{
		uint32_t *p;
		int i;
		nv2a_resident_program_t want = ( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_2D ) ? NV2A_RESIDENT_2D : NV2A_RESIDENT_TRI;

		if( nv2a_resident_program != want )
		{
			if( want == NV2A_RESIDENT_2D )
				nv2a_init_2d_shader();
			else
				nv2a_init_tri_shader();
			nv2a_resident_program = want;
		}

		/* Common to both spaces (X9-X step 4 Part C): stage 1 off, alpha
		   func GREATER ref 0 (ref/gl's own DEFAULT_ALPHATEST 0.0f -
		   gl_local.h:71 - the world path leaves GEQUAL 0x80 behind), every
		   array format reset. DEPTH_MASK/CULL_FACE_ENABLE moved OUT of
		   this block (were 2D-only, forced 0 unconditionally) into the
		   per-flush shadow diff below, which now pushes them from the
		   snapshot's own EFFECTIVE values for either space. */
		p = pb_begin();
		p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0 + 0x40, 0 );
		p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER );
		/* X9-X step 5: was a hardcoded 0 (DEFAULT_ALPHATEST) - now the
		   batch's own alpha_ref, so a studio MASKED mesh's 0x7F
		   (NV2A_STUDIO_ALPHA_REF_MASKED) survives a re-entry. Every other
		   caller still leaves this at its own default of 0. */
		p = pb_push1( p, NV097_SET_ALPHA_REF, nv2a_2d_batch_state.alpha_ref );
		pb_end( p );

		p = pb_begin();
		pb_push( p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
		for( i = 0; i < 16; i++ )
			*( p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
		pb_end( p );

		if( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_2D )
		{
			float m[4][4];
			static const float vs_c4[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

			p = pb_begin();
			p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, 0 );
			pb_end( p );

			/* C5: rebuilt every re-entry from the engine's own reported 2D
			   size against the real back buffer - NOT
			   nv2a_matrix_viewport()'s NDC-to-screen construction, a
			   different matrix for a different purpose. */
			nv2a_2d_build_matrix( m, (float)nv2a_width, (float)nv2a_height,
				(float)gpGlobals->width, (float)gpGlobals->height, nv2a_r_2d_pixeloffset.value );

			p = pb_begin();
			p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
			pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 20 );
			memcpy( p, m, 16 * 4 );
			p += 16;
			memcpy( p, vs_c4, sizeof( vs_c4 ));
			p += 4;
			pb_end( p );
		}
		else
		{
			static const float vs_c4[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

			/* The TriAPI gets the SAME matrix and the SAME shader divide
			   as the world (X9-X step 4 Part C): DEPTH_TEST_ENABLE 1, the
			   world's own nv2a_map_viewproj at TRANSFORM_CONSTANT_LOAD 96,
			   exactly as the world's own constant push does (this file's
			   world constant block). The depth FUNC is left at pbkit's
			   own default LEQUAL (pbkit.c:3161) - nothing here reprograms
			   it - so the TriAPI's depth values are directly comparable
			   with the world's own depth buffer.

			   X9-X step 5 / design note section 2: when the batch is the
			   view model's own (viewmodel_depthrange), push a SECOND
			   matrix instead - the same view*proj this frame's own
			   GL_RenderFrame built, but re-combined with a viewport whose
			   z range is [0, 0.3*65536] rather than the world's own
			   [0, 65536] - the exact glDepthRange(gldepthmin, gldepthmin +
			   0.3*(gldepthmax-gldepthmin)) remap for this renderer's own
			   baked-in-the-matrix z range (gl_studio.c:3348/3363). NV097_
			   SET_CLIP_MIN/MAX are deliberately never written: the new
			   range is a sub-range of the world's own, which already
			   renders on hardware (item 12's frame times). */
			p = pb_begin();
			p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, 1 );
			pb_end( p );

			p = pb_begin();
			p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
			pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 20 );
			if( nv2a_2d_batch_state.viewmodel_depthrange )
			{
				float viewmodel_viewport[4][4];
				float viewmodel_viewproj[4][4];
				float z_min, z_max;

				nv2a_studio_viewmodel_zrange( &z_min, &z_max );
				nv2a_matrix_viewport( viewmodel_viewport, 0, 0, (float)nv2a_width, (float)nv2a_height, z_min, z_max );
				nv2a_matrix_multiply( viewmodel_viewproj, nv2a_view_proj_combined, viewmodel_viewport );
				memcpy( p, viewmodel_viewproj, 16 * 4 );
			}
			else
			{
				memcpy( p, nv2a_map_viewproj, 16 * 4 );
			}
			p += 16;
			memcpy( p, vs_c4, sizeof( vs_c4 ));
			p += 4;
			pb_end( p );
		}

		nv2a_lit_state_current = false; /* program, stage 1 and arrays are the TriAPI's now - a later brush entity re-enters */
		nv2a_2d_pushed_base_valid = false; /* force the pointer/cache re-push below too, whatever base offset it last pointed at */
		nv2a_2d_pushed_valid = false; /* force the shadow push below to run too, whatever it was last set to */
		nv2a_2d_last_reentry_space = nv2a_2d_batch_state.space;
		nv2a_2d_last_reentry_viewmodel = nv2a_2d_batch_state.viewmodel_depthrange;
		nv2a_2d_dirty = false;
	}

	/* QA round 1, finding 1: the array pointers must track the CURRENT
	   batch's base offset (and now space - X9-X step 4 Part C) on EVERY
	   flush, not only a full re-entry - nv2a_2d_append() starts a new
	   batch (and a new ring_batch_base_offset) on a texture-, blend- or
	   space-only flush too, without necessarily changing nv2a_2d_dirty or
	   nv2a_2d_last_reentry_space. Left inside the re-entry block above,
	   that flush would leave the pointers parked at the PREVIOUS batch's
	   offset/format, and DRAW_ARRAYS's START_INDEX (relative to whatever
	   address the pointers hold) would silently redraw the previous
	   batch's vertices, at the WRONG stride, under the new texture/blend/
	   space state. nv2a_bind_vertex_base() above is the same fix for the
	   world's own 3D path: re-push the pointers AND invalidate the vertex
	   fetch cache whenever the bound address (or here, format) moves, not
	   only on some coarser event. */
	if( !nv2a_2d_pushed_base_valid
		|| nv2a_2d_pushed_base_offset != nv2a_2d_ring_batch_base_offset
		|| nv2a_2d_pushed_base_space != nv2a_2d_batch_state.space )
	{
		byte *ring_batch = (byte *)nv2a_2d_ring + nv2a_2d_ring_batch_base_offset;
		uint32_t *p;

		if( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_2D )
		{
			nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_2d_vertex_t ), ring_batch );
			nv2a_set_attrib_pointer( 3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL, 4, sizeof( nv2a_2d_vertex_t ), ring_batch + 16 );
			nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_2d_vertex_t ), ring_batch + 8 );
		}
		else
		{
			/* X9-X step 4 Part C's own vertex format table: 0 = F, size 3,
			   +0 (position); 9 = F, size 2, +12 (texcoord); 3 = UB_OGL,
			   size 4, +20 (colour). */
			nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_tri_vertex_t ), ring_batch );
			nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_tri_vertex_t ), ring_batch + 12 );
			nv2a_set_attrib_pointer( 3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL, 4, sizeof( nv2a_tri_vertex_t ), ring_batch + 20 );
		}

		p = pb_begin();
		p = pb_push1( p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0 );
		pb_end( p );

		nv2a_2d_pushed_base_offset = nv2a_2d_ring_batch_base_offset;
		nv2a_2d_pushed_base_space = nv2a_2d_batch_state.space;
		nv2a_2d_pushed_base_valid = true;
	}

	if( !nv2a_2d_pushed_valid
		|| nv2a_2d_pushed.blend_enable != nv2a_2d_batch_state.blend_enable
		|| nv2a_2d_pushed.sfactor != nv2a_2d_batch_state.sfactor
		|| nv2a_2d_pushed.dfactor != nv2a_2d_batch_state.dfactor
		|| nv2a_2d_pushed.alpha_test_enable != nv2a_2d_batch_state.alpha_test_enable
		|| nv2a_2d_pushed.depth_mask != nv2a_2d_batch_state.depth_mask
		|| nv2a_2d_pushed.cull_enable != nv2a_2d_batch_state.cull_enable
		|| nv2a_2d_pushed.depth_test_disable != nv2a_2d_batch_state.depth_test_disable
		|| nv2a_2d_pushed.depth_func_equal != nv2a_2d_batch_state.depth_func_equal )
	{
		uint32_t *p = pb_begin();
		p = pb_push1( p, NV097_SET_BLEND_ENABLE, nv2a_2d_batch_state.blend_enable ? 1 : 0 );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, nv2a_2d_batch_state.sfactor );
		p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, nv2a_2d_batch_state.dfactor );
		p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
		p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, nv2a_2d_batch_state.alpha_test_enable ? 1 : 0 );
		p = pb_push1( p, NV097_SET_DEPTH_MASK, nv2a_2d_batch_state.depth_mask ? 1 : 0 );
		p = pb_push1( p, NV097_SET_CULL_FACE_ENABLE, nv2a_2d_batch_state.cull_enable ? 1 : 0 );
		/* X9-X step 6: the depth test and function, 3D only - the 2D
		   re-entry above already turned the test off, and a space change
		   always goes through that re-entry. */
		if( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_3D )
		{
			p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, nv2a_2d_batch_state.depth_test_disable ? 0 : 1 );
			p = pb_push1( p, NV097_SET_DEPTH_FUNC, nv2a_2d_batch_state.depth_func_equal ? NV097_SET_DEPTH_FUNC_V_EQUAL : NV097_SET_DEPTH_FUNC_V_LEQUAL );
		}
		pb_end( p );

		nv2a_2d_pushed = nv2a_2d_batch_state;
		nv2a_2d_pushed_valid = true;
	}

	/* Stage-0 texture block, as the removed bring-up test triangle pushed
	   it - LU_IMAGE_A8B8G8R8, not
	   A8R8G8B8: PF_RGBA_32's own memory byte order (R,G,B,A per pixel)
	   IS that format's bit layout read as a little-endian 32-bit word,
	   so nv2a_tex_upload's plain memcpy needs no channel swap. The colour
	   format now comes from the batch key: studio skins are uploaded at
	   16 bpp (LU_IMAGE_R5G6B5 / A1R5G5B5, r_image.c nv2a_tex_upload_skin),
	   and the key is resolved when the batch was APPENDED, so it is the
	   format of the pixels this batch was queued against. */
	nv2a_push_stage0( &nv2a_2d_batch_key );

	/* X9-X step 11: the batch's fog (3D only - 2D never fogs), after any
	   re-entry above, whose program upload resets the final combiner. */
	nv2a_push_fog( nv2a_2d_batch_state.space == NV2A_IMM_SPACE_3D ? &nv2a_2d_batch_state.fog : NULL );

	{
		nv2a_2d_draw_ctx_t ctx;
		ctx.op = nv2a_2d_batch_state.op;
		nv2a_tri_chunk( nv2a_2d_ring_pending_verts, nv2a_2d_op_verts_per_prim( ctx.op ), nv2a_2d_emit_draw_arrays, &ctx );
		NV2A_FT_ADD( XBOX_FT_W_DRAWS, 1 );
	}

	/* X9-X step 6: a glow sprite's depth test off and the sprite
	   lightmap pass's GL_EQUAL never outlive their own batch. The world
	   and brush-entity path (nv2a_enter_lit_state/nv2a_set_3d_state)
	   pushes its own state directly and assumes the depth function is
	   pbkit's LEQUAL - it never writes DEPTH_FUNC - and a brush entity
	   that follows without a TriAPI re-entry in between does not re-push
	   the depth test either, so restore both here, right after the draw,
	   instead of relying on the next flush's diff (lessons.md: a draw
	   that depends on state it never sets breaks by what else is on
	   screen). */
	if( nv2a_2d_batch_state.depth_test_disable || nv2a_2d_batch_state.depth_func_equal )
	{
		uint32_t *p = pb_begin();
		p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL );
		pb_end( p );
		nv2a_2d_pushed.depth_test_disable = false;
		nv2a_2d_pushed.depth_func_equal = false;
	}

	nv2a_2d_ring_pending_verts = 0;
}

/* X9-X step 11 (r_studio_bridge.h). */
void nv2a_tri_flush( void )
{
	nv2a_2d_flush();
}

/*
========================================================================
divergences.md #143: the GPU studio path's draws. R_StudioDrawPoints
(r_studio.c) hands a submodel here instead of skinning and lighting it
on the CPU and submitting it through the TriAPI: the static vertices
(r_studio_gpu.c) are skinned and lit by vs_studio (vs_studio.vp has the
arithmetic and the constant layout), coloured by ps_2d exactly as the
TriAPI's vertices are (texture x colour), and drawn as indexed triangle
lists in the TriAPI assembler's own order. The render state each mesh
draws under is the same nv2a_2d_desired the TriAPI would have snapshot
for it at append time, pushed immediately; the pending TriAPI batch is
flushed first, so draw order is unchanged.

What this path leaves behind for the others: its program (they select
theirs again: nv2a_2d_dirty, nv2a_lit_state_current), its array pointers (the
TriAPI's re-entry repoints, nv2a_2d_pushed_base_valid), its render and
stage-0 state (nv2a_2d_pushed_valid; the world's lit-state entry pushes
its own), and the depth test/function restored exactly as the TriAPI
flush restores them. It writes hardware constants 0..95 and 101..189,
which no other path reads; c[0..4] (96..100) are never touched.

Invariants this path relies on (nv2a-xbox-specialist review): no other
path writes hardware constants 101..127 (c[5..31]: the fixed constants
and the gamma table, reloaded only on a serial change or once a frame),
128..183 and 0..95 (the palette, reloaded when its contents change) or
184..189 (c[88..93], the local lights, likewise). Hardware
0..95 is also where the NV2A keeps its fixed-function transform state:
NV097_SET_COMPOSITE_MATRIX, MODEL_VIEW/INVERSE_MODEL_VIEW, texture
matrices and texgen, EYE_POSITION, FOG_PLANE and VIEWPORT_OFFSET/SCALE
write there. ref/nv2a pushes none of them (pbkit pushes EYE_POSITION and
FOG_PLANE once, at init); a new path that does must reload the palette
after it, or the low palette bank breaks.
========================================================================
*/

static const nv2a_sgpu_vertex_t *nv2a_sgpu_bound_vb;
static uint32_t nv2a_sgpu_lut_serial_pushed;
static qboolean nv2a_sgpu_lut_valid;

/* Shadows of the constants this path last wrote (divergences.md #143,
   constant traffic). The constants survive every other path (the
   section comment above), so a value already in place is not pushed
   again: c[13..15] once a frame, c[5..9] when the view-projection or the
   entity's light terms change (not for every submodel of an entity),
   c[10..12] when a mesh's differ from the previous mesh's, and each
   palette bank when its bones' matrices or light vectors differ from
   what it holds. All are invalidated at the start of each frame
   (R_BeginFrame), as the gamma table already was, so no frame depends on
   a value written by the one before. */
static qboolean nv2a_sgpu_fixed_valid;
static float    nv2a_sgpu_submodel_shadow[5][4];
static qboolean nv2a_sgpu_submodel_valid;
static float    nv2a_sgpu_elight_shadow[NV2A_SGPU_ELIGHT_REGS][4];
static qboolean nv2a_sgpu_elight_valid;
static float    nv2a_sgpu_mesh_shadow[NV2A_SGPU_MESH_REGS][4];
static qboolean nv2a_sgpu_mesh_valid;
static float    nv2a_sgpu_pal_rows_shadow[NV2A_SGPU_MAX_PALETTE * 3][4];
static float    nv2a_sgpu_pal_lights_shadow[NV2A_SGPU_MAX_PALETTE][4];
static uint32_t nv2a_sgpu_pal_valid_count; /* palette entries the shadow holds, from 0 */

/* vs_studio (vs_studio.inl, the vp20compiler output of vs_studio.vp,
   resident: nv2a_vp_table) with the TriAPI's ps_2d combiner. */
static void nv2a_init_studio_shader( void )
{
	nv2a_select_vertex_program( NV2A_VP_STUDIO );
	nv2a_select_pixel_program( NV2A_PS_2D );
}

/* An open pbkit block that several pushes share, so a mesh's constants,
   render state and texture stage cost one pb_end (a write-combining
   flush and an MMIO spin: pbkit.c pb_start) instead of three. Closed
   and reopened before it would pass 128 dwords. `load_next` is the
   constant slot the hardware's load pointer is at after the last
   SET_TRANSFORM_CONSTANT in this block (it advances per vec4), so
   contiguous ranges need no second SET_TRANSFORM_CONSTANT_LOAD. */
typedef struct
{
	uint32_t *start;
	uint32_t *p;
	uint32_t load_next; /* 0xFFFFFFFF: unknown */
} nv2a_pb_block_t;

#define NV2A_PB_BLOCK_DWORDS 128u

static void nv2a_blk_open( nv2a_pb_block_t *b )
{
	b->start = b->p = pb_begin();
	b->load_next = 0xFFFFFFFFu;
}

static void nv2a_blk_close( nv2a_pb_block_t *b )
{
	/* pb_begin only reads the put pointer (pbkit.c:1786, DBG off), so an
	   empty block needs no pb_end and costs nothing. */
	if( b->p != b->start )
		pb_end( b->p );
	b->start = b->p;
}

static void nv2a_blk_room( nv2a_pb_block_t *b, uint32_t dwords )
{
	if((uint32_t)( b->p - b->start ) + dwords > NV2A_PB_BLOCK_DWORDS )
	{
		nv2a_blk_close( b );
		nv2a_blk_open( b );
	}
}

/* `count` vec4 constants from hardware slot `slot` on, 8 a
   SET_TRANSFORM_CONSTANT header (the method's 32-dword window). */
static void nv2a_blk_consts( nv2a_pb_block_t *b, uint32_t slot, const float (*v)[4], uint32_t count )
{
	while( count > 0 )
	{
		uint32_t n = count > 8 ? 8 : count;

		nv2a_blk_room( b, 3 + n * 4 );
		if( b->load_next != slot )
			b->p = pb_push1( b->p, NV097_SET_TRANSFORM_CONSTANT_LOAD, slot );
		pb_push( b->p++, NV097_SET_TRANSFORM_CONSTANT, n * 4 );
		memcpy( b->p, v, n * 16 );
		b->p += n * 4;
		slot += n;
		v += n;
		count -= n;
		b->load_next = slot;
	}
}

static void nv2a_sgpu_push_consts( uint32_t slot, const float (*v)[4], uint32_t count )
{
	nv2a_pb_block_t b;

	nv2a_blk_open( &b );
	nv2a_blk_consts( &b, slot, v, count );
	nv2a_blk_close( &b );
}

/* The studio path's own GPU state, when another path took the GPU since
   it last drew: program and combiner (both resident or tracked, so
   usually only a PROGRAM_START), stage 1 off, alpha function, every
   array format reset (then 0/1/2/9 pointed at the submodel's vertices by
   the caller), and c[13..15] once a frame. The constants above c[4]
   survive other paths, so nothing else is reloaded here. */
static void nv2a_sgpu_enter( void )
{
	nv2a_pb_block_t b;
	int i;

	nv2a_init_studio_shader();
	nv2a_resident_program = NV2A_RESIDENT_STUDIO;

	nv2a_blk_open( &b );
	b.p = pb_push1( b.p, NV097_SET_TEXTURE_CONTROL0 + 0x40, 0 );
	b.p = pb_push1( b.p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER );
	pb_push( b.p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
	for( i = 0; i < 16; i++ )
		*( b.p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;

	if( !nv2a_sgpu_fixed_valid )
	{
		float fixed[NV2A_SGPU_FIXED_REGS][4];

		nv2a_sgpu_fixed_consts( fixed );
		nv2a_blk_consts( &b, NV2A_SGPU_HW_C0 + NV2A_SGPU_C_FIXED, (const float (*)[4])fixed, NV2A_SGPU_FIXED_REGS );
		nv2a_sgpu_fixed_valid = true;
	}
	nv2a_blk_close( &b );

	nv2a_sgpu_bound_vb = NULL;
	nv2a_sgpu_entered = true;
}

/* The frame's start (R_BeginFrame): every shadow above, and the gamma
   table, are reloaded on first use. */
static void nv2a_sgpu_invalidate_consts( void )
{
	nv2a_sgpu_lut_valid = false;
	nv2a_sgpu_fixed_valid = false;
	nv2a_sgpu_submodel_valid = false;
	nv2a_sgpu_elight_valid = false;
	nv2a_sgpu_mesh_valid = false;
	nv2a_sgpu_pal_valid_count = 0;
}

qboolean nv2a_sgpu_begin_submodel( const nv2a_sgpu_geometry_t *geo, const float light[4],
	const float lut[NV2A_SGPU_LUT_REGS][4], uint32_t lut_serial,
	const float elights[NV2A_SGPU_ELIGHT_REGS][4] )
{
	float block[5][4];

	/* Begin()'s own gate: nothing is drawn, on either path, without it. */
	if( !nv2a_ready || !nv2a_frame_open || !nv2a_view_valid || !geo )
		return false;

	if( !nv2a_sgpu_lut_valid || nv2a_sgpu_lut_serial_pushed != lut_serial )
	{
		nv2a_sgpu_push_consts( NV2A_SGPU_HW_C0 + NV2A_SGPU_C_LUT, lut, NV2A_SGPU_LUT_REGS );
		nv2a_sgpu_lut_serial_pushed = lut_serial;
		nv2a_sgpu_lut_valid = true;
	}

	/* c[5..8]: the TriAPI re-entry's own choice of matrix (nv2a_2d_flush)
	   for the current view-model depth range; c[9]: the light terms. */
	if( nv2a_2d_desired.viewmodel_depthrange )
	{
		float viewmodel_viewport[4][4];
		float z_min, z_max;

		nv2a_studio_viewmodel_zrange( &z_min, &z_max );
		nv2a_matrix_viewport( viewmodel_viewport, 0, 0, (float)nv2a_width, (float)nv2a_height, z_min, z_max );
		nv2a_matrix_multiply( (float (*)[4])block, nv2a_view_proj_combined, viewmodel_viewport );
	}
	else
	{
		memcpy( block, nv2a_map_viewproj, 16 * 4 );
	}
	memcpy( block[4], light, 4 * sizeof( float ));

	if( !nv2a_sgpu_submodel_valid || memcmp( block, nv2a_sgpu_submodel_shadow, sizeof( block )))
	{
		nv2a_sgpu_push_consts( NV2A_SGPU_HW_C0 + NV2A_SGPU_C_MVP, (const float (*)[4])block, 5 );
		memcpy( nv2a_sgpu_submodel_shadow, block, sizeof( block ));
		nv2a_sgpu_submodel_valid = true;
	}

	/* c[88..93]: the entity's local lights, all-zero signs when it has
	   none, so an entity without lights after one without lights pushes
	   nothing. */
	if( !nv2a_sgpu_elight_valid || memcmp( elights, nv2a_sgpu_elight_shadow, sizeof( nv2a_sgpu_elight_shadow )))
	{
		nv2a_sgpu_push_consts( NV2A_SGPU_HW_C0 + NV2A_SGPU_C_ELIGHT, elights, NV2A_SGPU_ELIGHT_REGS );
		memcpy( nv2a_sgpu_elight_shadow, elights, sizeof( nv2a_sgpu_elight_shadow ));
		nv2a_sgpu_elight_valid = true;
	}
	return true;
}

/* Pushes the entries [first, first + n) of one palette bank's matrix rows
   and light vectors when they differ from what the bank holds. */
static void nv2a_sgpu_palette_range( nv2a_pb_block_t *b, const float (*rows)[4], const float (*lights)[4], uint32_t first, uint32_t n )
{
	qboolean held = nv2a_sgpu_pal_valid_count >= first + n;

	if( n == 0 )
		return;
	if( !held || memcmp( nv2a_sgpu_pal_rows_shadow[first * 3], rows[first * 3], n * 3 * sizeof( float[4] )))
	{
		nv2a_blk_consts( b, nv2a_sgpu_matrix_slot( first ), &rows[first * 3], n * 3 );
		memcpy( nv2a_sgpu_pal_rows_shadow[first * 3], rows[first * 3], n * 3 * sizeof( float[4] ));
	}
	if( !held || memcmp( nv2a_sgpu_pal_lights_shadow[first], lights[first], n * sizeof( float[4] )))
	{
		nv2a_blk_consts( b, nv2a_sgpu_light_slot( first ), &lights[first], n );
		memcpy( nv2a_sgpu_pal_lights_shadow[first], lights[first], n * sizeof( float[4] ));
	}
}

void nv2a_sgpu_load_palette( const nv2a_sgpu_palette_t *pal, const float (*bonestransform)[3][4], const float (*blightvec)[3] )
{
	static float rows[NV2A_SGPU_MAX_PALETTE * 3][4];
	static float lights[NV2A_SGPU_MAX_PALETTE][4];
	nv2a_pb_block_t b;
	uint32_t e, low, n = pal->count;

	if( n > NV2A_SGPU_MAX_PALETTE )
		n = NV2A_SGPU_MAX_PALETTE;
	for( e = 0; e < n; e++ )
	{
		memcpy( rows[e * 3], bonestransform[pal->bone[e]], sizeof( float ) * 12 );
		lights[e][0] = blightvec[pal->bone[e]][0];
		lights[e][1] = blightvec[pal->bone[e]][1];
		lights[e][2] = blightvec[pal->bone[e]][2];
		lights[e][3] = 0.0f;
	}

	low = n < NV2A_SGPU_LOW_BONES ? n : NV2A_SGPU_LOW_BONES;
	nv2a_blk_open( &b );
	nv2a_sgpu_palette_range( &b, (const float (*)[4])rows, (const float (*)[4])lights, 0, low );
	nv2a_sgpu_palette_range( &b, (const float (*)[4])rows, (const float (*)[4])lights, low, n - low );
	nv2a_blk_close( &b );

	/* Entries 0..n-1 now match the hardware; any past them that an
	   earlier, larger palette loaded were not written, so they still do. */
	if( n > nv2a_sgpu_pal_valid_count )
		nv2a_sgpu_pal_valid_count = n;
}

void nv2a_sgpu_draw_mesh( const nv2a_sgpu_geometry_t *geo, int mesh, float mesh_consts[NV2A_SGPU_MESH_REGS][4] )
{
	const nv2a_sgpu_mesh_t *m = &geo->meshes[mesh];
	nv2a_2d_batch_key_t key;
	nv2a_pb_block_t b;
	uint32_t i, begin, end;
	uint32_t *p;

	/* Whatever the TriAPI holds draws first, under its own state - the
	   order the CPU path's appends would have kept. */
	nv2a_2d_flush();

	/* The same texture-stage resolution an append makes (and its
	   nv2a_tex_mark_used, which keeps the skin cache from evicting a skin
	   this frame still reads). */
	if( !nv2a_2d_resolve_batch_key( &key ))
		return;

	if( !nv2a_sgpu_entered )
		nv2a_sgpu_enter();
	nv2a_lit_state_current = false;
	nv2a_2d_dirty = true;
	nv2a_2d_pushed_valid = false;
	nv2a_2d_pushed_base_valid = false;

	if( nv2a_sgpu_bound_vb != geo->vb )
	{
		const byte *vb = (const byte *)geo->vb;

		nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_sgpu_vertex_t ), vb + 0 );
		nv2a_set_attrib_pointer( 2, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_sgpu_vertex_t ), vb + 12 );
		nv2a_set_attrib_pointer( 1, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_sgpu_vertex_t ), vb + 24 );
		nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_sgpu_vertex_t ), vb + 32 );
		p = pb_begin();
		p = pb_push1( p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0 );
		pb_end( p );
		nv2a_sgpu_bound_vb = geo->vb;
	}

	/* The texture coordinates' scale, as Begin captures it for Vertex3f. */
	nv2a_tex_coord_scale( nv2a_tex_get( nv2a_tex_current()), &mesh_consts[2][2], &mesh_consts[2][3] );

	/* One block: the mesh's constants when they changed, then
	   nv2a_2d_flush's 3D shadow push, from the desired state directly
	   (this draw is not deferred, so the desired state IS the state it
	   was "appended" under), with the alpha function the TriAPI
	   re-entry sets, then the texture stage. */
	nv2a_blk_open( &b );
	if( !nv2a_sgpu_mesh_valid || memcmp( mesh_consts, nv2a_sgpu_mesh_shadow, sizeof( nv2a_sgpu_mesh_shadow )))
	{
		nv2a_blk_consts( &b, NV2A_SGPU_HW_C0 + NV2A_SGPU_C_MESH, (const float (*)[4])mesh_consts, NV2A_SGPU_MESH_REGS );
		memcpy( nv2a_sgpu_mesh_shadow, mesh_consts, sizeof( nv2a_sgpu_mesh_shadow ));
		nv2a_sgpu_mesh_valid = true;
	}
	nv2a_blk_room( &b, 22 + NV2A_STAGE0_DWORDS );
	p = b.p;
	p = pb_push1( p, NV097_SET_BLEND_ENABLE, nv2a_2d_desired.blend_enable ? 1 : 0 );
	p = pb_push1( p, NV097_SET_BLEND_FUNC_SFACTOR, nv2a_2d_desired.sfactor );
	p = pb_push1( p, NV097_SET_BLEND_FUNC_DFACTOR, nv2a_2d_desired.dfactor );
	p = pb_push1( p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD );
	p = pb_push1( p, NV097_SET_ALPHA_TEST_ENABLE, nv2a_2d_desired.alpha_test_enable ? 1 : 0 );
	p = pb_push1( p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER );
	p = pb_push1( p, NV097_SET_ALPHA_REF, nv2a_2d_desired.alpha_ref );
	p = pb_push1( p, NV097_SET_DEPTH_MASK, nv2a_2d_desired.depth_mask ? 1 : 0 );
	p = pb_push1( p, NV097_SET_CULL_FACE_ENABLE, nv2a_2d_desired.cull_enable ? 1 : 0 );
	p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, nv2a_2d_desired.depth_test_disable ? 0 : 1 );
	p = pb_push1( p, NV097_SET_DEPTH_FUNC, nv2a_2d_desired.depth_func_equal ? NV097_SET_DEPTH_FUNC_V_EQUAL : NV097_SET_DEPTH_FUNC_V_LEQUAL );
	b.p = nv2a_emit_stage0( p, &key );
	nv2a_blk_close( &b );

	nv2a_push_fog( &nv2a_fog_current );

	/* The mesh's triangles: whole dwords of two u16 indices, 120 a block
	   (80 triangles, so no block splits one) - nv2a_draw_index_range's
	   shape. */
	NV2A_FT_ADD( XBOX_FT_W_DRAWS, 1 );
	begin = m->first_index / 2u;
	end = begin + m->index_count / 2u;
	for( i = begin; i < end; )
	{
		uint32_t batch = end - i;

		if( batch > 120 )
			batch = 120;
		p = pb_begin();
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
		pb_push( p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, batch );
		memcpy( p, &geo->index_dwords[i], batch * sizeof( uint32_t ));
		p += batch;
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
		pb_end( p );
		NV2A_FT_ADD( XBOX_FT_W_PRIMS, 1 );
		i += batch;
	}

	/* nv2a_2d_flush's own tail: the world and brush-entity path assume
	   the depth test on with LEQUAL and never write either. */
	if( nv2a_2d_desired.depth_test_disable || nv2a_2d_desired.depth_func_equal )
	{
		p = pb_begin();
		p = pb_push1( p, NV097_SET_DEPTH_TEST_ENABLE, 1 );
		p = pb_push1( p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL );
		pb_end( p );
	}
}

/* X9-X step 11 (r_studio_bridge.h): r_warp.c's water pieces, whole
   primitives straight into the 3D batch - what Begin/Vertex3f/End would
   produce for TRI_QUADS or a fan, without the assembler's per-vertex
   staging, which dominated the cost of a lake's 10,000 vertices a frame.
   Same gate as Begin (renderer, frame and camera ready); the colour is
   the current Color4ub/Color4f, as Vertex3f takes it. */
static void nv2a_2d_append( nv2a_imm_space_t space, uint32_t op, const void *verts, uint32_t n );

void nv2a_tri_append_3d( qboolean quads, nv2a_tri_vert_t *v, uint32_t n )
{
	uint32_t i;

	if( !nv2a_ready || !nv2a_frame_open || nv2a_in2d || !nv2a_view_valid || n == 0 )
		return;

	for( i = 0; i < n; i++ )
		memcpy( v[i].rgba, nv2a_2d_color, 4 );

	nv2a_2d_append( NV2A_IMM_SPACE_3D, quads ? NV097_SET_BEGIN_END_OP_QUADS : NV097_SET_BEGIN_END_OP_TRIANGLES, v, n );
}

/* X9-X item 14 step 13 (divergences.md #141): every place this renderer
   blocks on the GPU or the display goes through one of these three, so
   the frame-time recorder (engine/platform/xbox/xbox_frametime.c) can
   split a frame into CPU work and waiting. With the recorder off each is
   the plain wait plus one load and branch. The GPU wait is the time the
   GPU still had queued work when the CPU needed it idle - its excess over
   the CPU, not its busy time (pbkit exposes no GPU-side timestamp; the
   recorder's header comment has the detail). */
static void nv2a_wait_gpu_idle( void )
{
	uint64_t t0;

	if( !xbox_ft_waits.active )
	{
		while( pb_busy())
			;
		return;
	}

	t0 = xbox_ft_rdtsc();
	while( pb_busy())
		;
	xbox_ft_waits.gpu += xbox_ft_rdtsc() - t0;
}

static void nv2a_wait_vblank( void )
{
	uint64_t t0;

	if( !xbox_ft_waits.active )
	{
		pb_wait_for_vbl();
		return;
	}

	t0 = xbox_ft_rdtsc();
	pb_wait_for_vbl();
	xbox_ft_waits.vblank += xbox_ft_rdtsc() - t0;
}

/* pb_finished() returns 1 while all three back buffers are still queued
   for display (pbkit.c:2010-2015): a wait on scan-out, not on drawing. */
static void nv2a_wait_swap( void )
{
	uint64_t t0;

	if( !xbox_ft_waits.active )
	{
		while( pb_finished())
			;
		return;
	}

	t0 = xbox_ft_rdtsc();
	while( pb_finished())
		;
	xbox_ft_waits.swap += xbox_ft_rdtsc() - t0;
}

void nv2a_2d_flush_and_wait( void )
{
	nv2a_2d_flush();
	nv2a_wait_gpu_idle();
}

/* X9-X step 4 Part C: the one append path both spaces share - refactored
   out of the old nv2a_2d_append_quad() (kept below as a thin wrapper for
   R_DrawStretchPic/FillRGBA, unchanged). Flushes first if the desired
   state, the resolved texture key, the space, the op, or dirty changed
   since the last flush, or if the ring has no room for all `n` vertices
   (in which case it also spins pb_busy and resets the cursor - "the
   pointers get re-pushed with BREAK_VERTEX_BUFFER_CACHE" next flush, via
   the dirty flag). Drops the draw if the renderer or the frame isn't
   ready (C1's own rule). `verts` must already be in the wire format for
   `space` (nv2a_2d_vertex_t for 2D, nv2a_tri_vertex_t for 3D) - the one
   primitive it carries is appended atomically: room is checked for all
   `n` vertices together, never split across a flush. */
static void nv2a_2d_append( nv2a_imm_space_t space, uint32_t op, const void *verts, uint32_t n )
{
	nv2a_2d_batch_key_t key;
	uint32_t vertex_size = ( space == NV2A_IMM_SPACE_2D ) ? sizeof( nv2a_2d_vertex_t ) : sizeof( nv2a_tri_vertex_t );
	uint32_t bytes = vertex_size * n;

	if( !nv2a_ready || !nv2a_frame_open )
		return;

	if( !nv2a_2d_resolve_batch_key( &key ))
		return;

	if( nv2a_2d_ring_pending_verts > 0
		&& ( nv2a_2d_dirty
			|| memcmp( &key, &nv2a_2d_batch_key, sizeof( key )) != 0
			|| nv2a_2d_batch_state.space != space
			|| nv2a_2d_batch_state.op != op
			|| nv2a_2d_batch_state.blend_enable != nv2a_2d_desired.blend_enable
			|| nv2a_2d_batch_state.sfactor != nv2a_2d_desired.sfactor
			|| nv2a_2d_batch_state.dfactor != nv2a_2d_desired.dfactor
			|| nv2a_2d_batch_state.alpha_test_enable != nv2a_2d_desired.alpha_test_enable
			|| nv2a_2d_batch_state.alpha_ref != nv2a_2d_desired.alpha_ref
			|| nv2a_2d_batch_state.depth_mask != ( space == NV2A_IMM_SPACE_2D ? false : nv2a_2d_desired.depth_mask )
			|| nv2a_2d_batch_state.cull_enable != ( space == NV2A_IMM_SPACE_2D ? false : nv2a_2d_desired.cull_enable )
			|| ( space == NV2A_IMM_SPACE_3D && nv2a_2d_batch_state.viewmodel_depthrange != nv2a_2d_desired.viewmodel_depthrange )
			|| nv2a_2d_batch_state.depth_test_disable != ( space == NV2A_IMM_SPACE_2D ? false : nv2a_2d_desired.depth_test_disable )
			|| nv2a_2d_batch_state.depth_func_equal != ( space == NV2A_IMM_SPACE_2D ? false : nv2a_2d_desired.depth_func_equal )
			|| ( space == NV2A_IMM_SPACE_3D && memcmp( &nv2a_2d_batch_state.fog, &nv2a_fog_current, sizeof( nv2a_fog_current )) != 0 )))
	{
		nv2a_2d_flush();
	}

	nv2a_2d_batch_key = key;

	if( nv2a_2d_ring_write_offset + bytes > NV2A_2D_VERTEX_RING_BYTES )
	{
		nv2a_2d_flush();
		nv2a_wait_gpu_idle();
		nv2a_2d_ring_write_offset = 0;
		nv2a_2d_dirty = true;
	}

	if( nv2a_2d_ring_pending_verts == 0 )
	{
		nv2a_2d_ring_batch_base_offset = nv2a_2d_ring_write_offset;
		nv2a_2d_batch_state = nv2a_2d_desired;
		nv2a_2d_batch_state.space = space;
		nv2a_2d_batch_state.op = op;
		/* X9-X step 11: the GL fog state now, not nv2a_2d_desired's copy
		   (r_studio_bridge.h); 2D never fogs. */
		if( space == NV2A_IMM_SPACE_3D )
			nv2a_2d_batch_state.fog = nv2a_fog_current;
		else
			memset( &nv2a_2d_batch_state.fog, 0, sizeof( nv2a_2d_batch_state.fog ));
		if( space == NV2A_IMM_SPACE_2D )
		{
			/* Effective, not desired: 2D always draws with both forced
			   off, matching GL's effective behaviour with the depth test
			   off and R_Set2DMode(true)'s GL_Cull(NONE). viewmodel_depthrange
			   is 3D-only (X9-X step 5); forced off here so it never affects
			   the 2D dirty-check above. */
			nv2a_2d_batch_state.depth_mask = false;
			nv2a_2d_batch_state.cull_enable = false;
			nv2a_2d_batch_state.viewmodel_depthrange = false;
			/* X9-X step 6: 3D-only too - the 2D re-entry turns the depth
			   test off itself, and 2D never compares depth. */
			nv2a_2d_batch_state.depth_test_disable = false;
			nv2a_2d_batch_state.depth_func_equal = false;
		}
	}

	memcpy( (byte *)nv2a_2d_ring + nv2a_2d_ring_write_offset, verts, bytes );
	nv2a_2d_ring_write_offset += bytes;
	nv2a_2d_ring_pending_verts += n;
	NV2A_FT_ADD( space == NV2A_IMM_SPACE_2D ? XBOX_FT_W_VERTS_2D : nv2a_ft_imm3d_counter, n );
}

/* C4: appends one quad (4 vertices, texel-unit s/t already resolved by
   the caller) to the pending 2D batch - unchanged behaviour, now on top
   of the generic nv2a_2d_append() above. */
static void nv2a_2d_append_quad( float x, float y, float w, float h, float s1, float t1, float s2, float t2 )
{
	nv2a_2d_vertex_t verts[4];
	int i;

	verts[0].x = x;     verts[0].y = y;     verts[0].u = s1; verts[0].v = t1;
	verts[1].x = x + w; verts[1].y = y;     verts[1].u = s2; verts[1].v = t1;
	verts[2].x = x + w; verts[2].y = y + h; verts[2].u = s2; verts[2].v = t2;
	verts[3].x = x;     verts[3].y = y + h; verts[3].u = s1; verts[3].v = t2;
	for( i = 0; i < 4; i++ )
	{
		verts[i].rgba[0] = nv2a_2d_color[0];
		verts[i].rgba[1] = nv2a_2d_color[1];
		verts[i].rgba[2] = nv2a_2d_color[2];
		verts[i].rgba[3] = nv2a_2d_color[3];
	}

	nv2a_2d_append( NV2A_IMM_SPACE_2D, NV097_SET_BEGIN_END_OP_QUADS, verts, 4 );
}

static void nv2a_draw_no_world_backdrop( void );

/* C1. */
static void R_Set2DMode( qboolean enable )
{
	if( enable )
	{
		if( !nv2a_in2d )
		{
			nv2a_in2d = true;
			nv2a_2d_color[0] = nv2a_2d_color[1] = nv2a_2d_color[2] = nv2a_2d_color[3] = 255;
			nv2a_2d_desired.alpha_test_enable = true;
			/* X9-X step 4 Part F: gl_draw.c's own R_Set2DMode(true) - GL_Cull(NONE), pglDepthMask(GL_FALSE). */
			nv2a_2d_desired.depth_mask = false;
			nv2a_2d_desired.cull_enable = false;
			nv2a_2d_dirty = true;
		}

		/* The first R_Set2DMode(true) after R_BeginFrame's own is
		   V_PostRender's (cl_view.c:530), ahead of every HUD, console and
		   menu draw - with no world this frame (V_RenderView skipped it
		   under the menu, cl_view.c:394, or the map is not ready) the
		   backdrop goes in here, under all of them. Checked outside the
		   in2d early-out: with no world V_RenderView never left 2D mode. */
		if( nv2a_no_world_backdrop_pending )
			nv2a_draw_no_world_backdrop();
	}
	else
	{
		nv2a_2d_flush();
		nv2a_in2d = false;
		/* X9-X step 4 Part F: gl_draw.c's own R_Set2DMode(false) - pglEnable(GL_DEPTH_TEST)/pglDepthMask(GL_TRUE), GL_Cull(GL_FRONT). Matches nv2a_set_3d_state()'s own values, which every 3D draw path still pushes for itself. */
		nv2a_2d_desired.depth_mask = true;
		nv2a_2d_desired.cull_enable = true;
		nv2a_2d_desired.depth_test_disable = false; /* X9-X step 6: the same function's pglEnable( GL_DEPTH_TEST ) */
		nv2a_2d_dirty = true; /* every 3D path sets its own state via nv2a_set_3d_state(); push nothing here */
	}
}

/* Resonance3D (docs/r3d/divergences.md #129): what a frame with no world
   shows. R_BeginFrame already cleared it to black; this adds the engine
   version at the bottom right, the same text, colour and position the
   main menu shows (Con_DrawVersion's key_menu branch, console.c:2064-2100),
   so a paused game or a map that is not drawn yet reads as the engine
   running rather than a hang. It replaces the bring-up test triangle,
   which R_EndFrame drew AFTER the 2D pass, over the in-game menu. ref/gl
   draws nothing here: its frame is simply whatever the clear left.

   Skipped once anything 2D was already appended this frame (the connecting
   plaque draws before V_PostRender, cl_scrn.c:684), so HUD, console,
   menu and plaque are always on top of it. */
static void nv2a_draw_no_world_backdrop( void )
{
	char version[128], engine_version[32], os[32], arch[32];
	nv2a_2d_shadow_t saved_desired;
	byte saved_color[4];
	rgba_t color = { 240, 180, 24, 255 }; /* g_color_table[7]'s default, console.c:84 */
	int build, r, g, b, len = 0, height = 0;

	nv2a_no_world_backdrop_pending = false;

	if( nv2a_world_drawn_this_frame || nv2a_2d_ring_write_offset != 0 )
		return;

	if( !gEngfuncs.pfnGetCvarFloat( "scr_drawversion" ))
		return; /* Con_DrawVersion's own gate */

	/* host_ver is the engine's own "%i " XASH_VERSION " %s %s %s" of
	   Q_buildnum, Q_buildos, Q_buildarch and the commit (host.c:1238) -
	   the same values Con_DrawVersion formats. */
	if( sscanf( gEngfuncs.pfnGetCvarString( "host_ver" ), "%d %31s %31s %31s", &build, engine_version, os, arch ) != 4 )
		return;
	snprintf( version, sizeof( version ), "v%i/%s (%s-%s build %i)", PROTOCOL_VERSION, engine_version, os, arch, build );

	/* g_color_table[7] follows con_color (Con_SetColor, console.c:197). */
	switch( sscanf( gEngfuncs.pfnGetCvarString( "con_color" ), "%i %i %i", &r, &g, &b ))
	{
	case 1:
		color[0] = color[1] = color[2] = (byte)bound( 0, r, 255 );
		break;
	case 3:
		color[0] = (byte)bound( 0, r, 255 );
		color[1] = (byte)bound( 0, g, 255 );
		color[2] = (byte)bound( 0, b, 255 );
		break;
	}

	/* CL_DrawString sets its own render mode and colour through this
	   renderer (GL_SetRenderMode/Color4ub); put back what the engine's own
	   next 2D draw expects R_Set2DMode(true) to have left. Batches already
	   appended keep their own snapshot (nv2a_2d_batch_state). */
	saved_desired = nv2a_2d_desired;
	memcpy( saved_color, nv2a_2d_color, sizeof( saved_color ));

	gEngfuncs.Con_DrawStringLen( version, &len, &height );
	gEngfuncs.Con_DrawString( gpGlobals->width - len * 1.05f, gpGlobals->height - height * 1.05f, version, color );

	nv2a_2d_desired = saved_desired;
	memcpy( nv2a_2d_color, saved_color, sizeof( saved_color ));
}

/* C2: port of GL_SetRenderMode (ref/gl/gl_backend.c:424-461) - sets the
   desired shadow only, applied lazily at the next flush. */
static void GL_SetRenderMode( int renderMode )
{
	/* X9-X item 14 step 11: R_AllowFog per mode (gl_backend.c:429-460) -
	   every mode but glow and additive allows it. */
	R_WarpAllowFog( renderMode != kRenderGlow && renderMode != kRenderTransAdd );

	switch( renderMode )
	{
	default:
	case kRenderNormal:
		nv2a_2d_desired.blend_enable = false;
		nv2a_2d_desired.alpha_test_enable = false;
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
		nv2a_2d_desired.alpha_test_enable = false;
		break;
	case kRenderTransAlpha:
		nv2a_2d_desired.blend_enable = false;
		nv2a_2d_desired.alpha_test_enable = true;
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE;
		nv2a_2d_desired.alpha_test_enable = false;
		break;
	case kRenderScreenFadeModulate:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR;
		nv2a_2d_desired.alpha_test_enable = false;
		break;
	}
}

void Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	nv2a_2d_color[0] = r;
	nv2a_2d_color[1] = g;
	nv2a_2d_color[2] = b;
	nv2a_2d_color[3] = a;
}

/* C3. */
void Color4f( float r, float g, float b, float a )
{
	Color4ub(
		(byte)( bound( 0.0f, r, 1.0f ) * 255 + 0.5f ),
		(byte)( bound( 0.0f, g, 1.0f ) * 255 + 0.5f ),
		(byte)( bound( 0.0f, b, 1.0f ) * 255 + 0.5f ),
		(byte)( bound( 0.0f, a, 1.0f ) * 255 + 0.5f ));
}

/*
========================================================================
X9-X step 4 Part D: the TriAPI entry points - Begin/Vertex3f(v)/End stage
into nv2a_tri_util.c's own host-tested assembler (Part A), which emits
complete list primitives through nv2a_2d_append() (Part C) in either
space; TriRenderMode/CullFace write the same nv2a_2d_desired shadow
GL_SetRenderMode/VGUI_SetupDrawing/FillRGBA already share.
========================================================================
*/

/* Latched state, persisting across Begin/End like GL's own current
   state - TexCoord2f (normalized, initially 0,0) and nv2a_2d_color
   (declared with the 2D pass above, C3). */
static float nv2a_tri_texcoord[2];

/* D: TriRenderMode (gl_triapi.c:40-72), ported onto nv2a_2d_desired -
   with its R_AllowFog calls since X9-X item 14 step 11 (r_warp.c); neither
   alpha test nor the depth TEST are ever touched here (only depth_mask).
   No default case, same as the reference. */
void TriRenderMode( int mode )
{
	/* X9-X item 14 step 11: R_AllowFog per mode (gl_triapi.c:47-66). */
	if( mode == kRenderGlow || mode == kRenderTransAdd )
		R_WarpAllowFog( false );
	else if( mode == kRenderNormal || mode == kRenderTransAlpha || mode == kRenderTransColor || mode == kRenderTransTexture )
		R_WarpAllowFog( true );

	switch( mode )
	{
	case kRenderNormal:
		nv2a_2d_desired.blend_enable = false;
		nv2a_2d_desired.depth_mask = true;
		break;
	case kRenderTransAlpha:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
		nv2a_2d_desired.depth_mask = false;
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
		/* depth_mask UNCHANGED, matching the reference exactly. */
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE;
		nv2a_2d_desired.depth_mask = false;
		break;
	}
}

static qboolean       nv2a_tri_open;    /* inside a Begin/End pair */
static qboolean       nv2a_tri_dropped; /* this Begin was ignored - Vertex3f/End must no-op quietly, not warn again */
static nv2a_imm_space_t nv2a_tri_space;
static uint32_t       nv2a_tri_op;      /* the hardware BEGIN_END op this Begin's primitive maps to */
static float          nv2a_tri_tex_w = 1.0f, nv2a_tri_tex_h = 1.0f; /* the bound texture's own size, captured at Begin (D6: texel units) */
static nv2a_tri_asm_t nv2a_tri_asm;

/* nv2a_tri_util.h's own topology -> this file's own hardware BEGIN_END op
   (pbkit constants nv2a_tri_util.c cannot name itself, D's own note). */
static uint32_t nv2a_tri_topology_op( nv2a_tri_topology_t topo )
{
	switch( topo )
	{
	case NV2A_TRI_LIST_POINTS:    return NV097_SET_BEGIN_END_OP_POINTS;
	case NV2A_TRI_LIST_LINES:     return NV097_SET_BEGIN_END_OP_LINES;
	case NV2A_TRI_LIST_QUADS:
	case NV2A_TRI_QUAD_STRIP:     return NV097_SET_BEGIN_END_OP_QUADS;
	case NV2A_TRI_LIST_TRIANGLES:
	case NV2A_TRI_FAN:
	case NV2A_TRI_STRIP:
	default:                      return NV097_SET_BEGIN_END_OP_TRIANGLES;
	}
}

/* X9-X step 7 (divergences.md #133): GL_REPEAT for the TriAPI's 3D
   primitives, on only while r_effects.c's R_BeamDraw draws a beam - the
   one TriAPI caller whose texture coordinates run over many copies of
   its texture (R_DrawSegs, gl_beams.c:167-173). This renderer's linear
   textures clamp (TEXTURE_ADDRESS in nv2a_2d_flush), so each primitive is
   cut at the texture's cell boundaries instead (nv2a_efx_util.c,
   host-tested), in the units its vertices already carry (texels for a
   linear texture, 1 for a compressed one: nv2a_tri_tex_w/h, captured at
   Begin). */
static qboolean nv2a_tri_repeat;

void nv2a_tri_set_texture_repeat( qboolean on )
{
	nv2a_tri_repeat = on;
}

/* nv2a_efx_wrap_primitive's emit callback: a primitive that fit one cell
   keeps its Begin's own op; a cut one is always triangles, which starts a
   new batch when the Begin was QUADS (nv2a_2d_append flushes on an op
   change) - only a TRI_QUADS beam (TE_BEAMFOLLOW, whose V stays in [0,1])
   could ever mix the two. */
static void nv2a_tri_emit_repeated( const nv2a_tri_vert_t *v, uint32_t n, int split, void *ctx );

/* The assembler's own emit callback (nv2a_tri_util.h's nv2a_tri_emit_fn) -
   converts to the 20-byte 2D wire format (dropping z) in 2D space, or
   appends the 24-byte 3D format as-is (nv2a_tri_vert_t IS
   nv2a_tri_vertex_t, Part C's own note) in 3D space. */
static void nv2a_tri_emit_primitive( const nv2a_tri_vert_t *v, uint32_t n, void *ctx )
{
	(void)ctx;

	if( nv2a_tri_repeat && nv2a_tri_space == NV2A_IMM_SPACE_3D )
	{
		nv2a_efx_wrap_primitive( v, n, nv2a_tri_tex_w, nv2a_tri_tex_h, nv2a_tri_emit_repeated, NULL );
		return;
	}

	if( nv2a_tri_space == NV2A_IMM_SPACE_2D )
	{
		nv2a_2d_vertex_t verts2d[4];
		uint32_t i;

		for( i = 0; i < n; i++ )
		{
			verts2d[i].x = v[i].x;
			verts2d[i].y = v[i].y;
			verts2d[i].u = v[i].u;
			verts2d[i].v = v[i].v;
			memcpy( verts2d[i].rgba, v[i].rgba, 4 );
		}
		nv2a_2d_append( NV2A_IMM_SPACE_2D, nv2a_tri_op, verts2d, n );
	}
	else
	{
		nv2a_2d_append( NV2A_IMM_SPACE_3D, nv2a_tri_op, v, n );
	}
}

static void nv2a_tri_emit_repeated( const nv2a_tri_vert_t *v, uint32_t n, int split, void *ctx )
{
	(void)ctx;
	nv2a_2d_append( NV2A_IMM_SPACE_3D, split ? NV097_SET_BEGIN_END_OP_TRIANGLES : nv2a_tri_op, v, n );
}

/* D3: TriTexCoord2f (gl_triapi.c:180) - latches, does not emit. */
void TexCoord2f( float u, float v )
{
	nv2a_tri_texcoord[0] = u;
	nv2a_tri_texcoord[1] = v;
}

/* D: TriBegin (gl_triapi.c:81-113) - maps TRI_* to A's topology (any
   unknown mode falls to POLYGON/FAN, same as the reference's own
   default), resolves the drawing space from R_Set2DMode's own flag
   (cl_view.c:401 calls R_Set2DMode(false) before GL_RenderFrame, so it is
   reliable here), and captures the bound texture's own size for D6's
   texel-unit scaling. Drops the whole primitive rather than staging
   anything when the renderer/frame isn't ready, or when 3D and the
   camera has no valid matrix yet (nv2a_view_valid) - Vertex3f/End then
   quietly no-op via nv2a_tri_dropped, same as a GL context with nothing
   bound would draw nothing rather than crash. A nested Begin is ignored,
   as GL itself does (INVALID_OPERATION), logged once here. */
void Begin( int primitiveCode )
{
	nv2a_tri_topology_t topo;
	const nv2a_texture_t *tex;

	if( nv2a_tri_open )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "ref_nv2a: TriBegin called while already inside Begin/End - ignored\n" );
		return;
	}

	switch( primitiveCode )
	{
	case TRI_TRIANGLES:      topo = NV2A_TRI_LIST_TRIANGLES; break;
	case TRI_TRIANGLE_FAN:   topo = NV2A_TRI_FAN;             break;
	case TRI_QUADS:          topo = NV2A_TRI_LIST_QUADS;      break;
	case TRI_LINES:          topo = NV2A_TRI_LIST_LINES;      break;
	case TRI_TRIANGLE_STRIP: topo = NV2A_TRI_STRIP;           break;
	case TRI_QUAD_STRIP:     topo = NV2A_TRI_QUAD_STRIP;      break;
	case TRI_POINTS:         topo = NV2A_TRI_LIST_POINTS;     break;
	case TRI_POLYGON:
	default:                 topo = NV2A_TRI_FAN;             break;
	}

	nv2a_tri_open = true;
	nv2a_tri_space = nv2a_in2d ? NV2A_IMM_SPACE_2D : NV2A_IMM_SPACE_3D;
	nv2a_tri_dropped = !nv2a_ready || !nv2a_frame_open || ( nv2a_tri_space == NV2A_IMM_SPACE_3D && !nv2a_view_valid );

	if( nv2a_tri_dropped )
		return;

	nv2a_tri_op = nv2a_tri_topology_op( topo );

	if( nv2a_tri_map_texture != NV2A_TRI_NO_MAP_TEXTURE )
	{
		nv2a_tri_tex_w = nv2a_tri_tex_h = 1.0f; /* X9-X step 11: a DXT1 directory texture - normalized */
	}
	else
	{
		tex = nv2a_tex_get( nv2a_tex_current());
		nv2a_tex_coord_scale( tex, &nv2a_tri_tex_w, &nv2a_tri_tex_h ); /* texels for linear, 1 for DXT (divergences.md #131) */
	}

	nv2a_tri_begin( &nv2a_tri_asm, topo, nv2a_tri_emit_primitive, NULL );
}

/* D: TriVertex3f/3fv (gl_triapi.c:200-214) - outside Begin/End, or on a
   dropped primitive, these are ignored, same as GL with nothing bound.
   Texel units (D6/divergences #100): u/v scaled by the bound texture's
   own width/height captured at Begin. In 2D space z is ignored (dropped
   at nv2a_tri_emit_primitive) and x,y are 2D screen coordinates, the
   same as R_DrawStretchPic's. */
void Vertex3f( float x, float y, float z )
{
	nv2a_tri_vert_t v;

	if( !nv2a_tri_open || nv2a_tri_dropped )
		return;

	v.x = x;
	v.y = y;
	v.z = z;
	v.u = nv2a_tri_texcoord[0] * nv2a_tri_tex_w;
	v.v = nv2a_tri_texcoord[1] * nv2a_tri_tex_h;
	v.rgba[0] = nv2a_2d_color[0];
	v.rgba[1] = nv2a_2d_color[1];
	v.rgba[2] = nv2a_2d_color[2];
	v.rgba[3] = nv2a_2d_color[3];

	nv2a_tri_push( &nv2a_tri_asm, &v );
}

void Vertex3fv( const float *worldPnt )
{
	if( !worldPnt )
		return;
	Vertex3f( worldPnt[0], worldPnt[1], worldPnt[2] );
}

/* D: TriEnd (gl_triapi.c:117-123) - finishes the assembler (drops any
   incomplete trailing primitive, A's own rule) without flushing, so
   consecutive same-state Begin/Ends keep batching together. */
void End( void )
{
	if( !nv2a_tri_open )
		return;
	if( !nv2a_tri_dropped )
		nv2a_tri_end( &nv2a_tri_asm );
	nv2a_tri_open = false;
}

/* D: TriCullFace (gl_triapi.c:340-355) - TRI_FRONT means CULL_FACE_ENABLE
   1 with pbkit's own CW-front/cull-back (pbkit.c:3196-3198); anything
   else means 0. NV097_SET_CULL_FACE/FRONT_FACE are never written - both
   spaces share the world's own CW-front/cull-back convention already
   set up by pbkit, matching ref/gl's world draw (GL_Cull(GL_FRONT) =
   TriCullFace(TRI_FRONT), gl_rmain.c:568 / gl_triapi.c:340-355). */
void CullFace( TRICULLSTYLE mode )
{
	nv2a_2d_desired.cull_enable = ( mode == TRI_FRONT );
}

/* C2: port of VGUI_SetupDrawing (ref/gl/gl_context.c:360-375). */
static void VGUI_SetupDrawing( qboolean rect )
{
	nv2a_2d_desired.blend_enable = true;
	nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
	nv2a_2d_desired.dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
	nv2a_2d_desired.alpha_test_enable = !rect;
}

/* C3: port of FillRGBA (ref/gl/gl_context.c:49-68), corrected per D2/D6's
   own design note - binds the engine's own *white slot (GL_CreateTexture
   records it, D1) rather than disabling texturing outright, which this
   shader-only renderer has no equivalent of. */
static void FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	GL_Bind( 0, nv2a_tex_white());

	nv2a_2d_desired.blend_enable = true;
	nv2a_2d_desired.sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
	nv2a_2d_desired.dfactor = ( rendermode == kRenderTransAdd )
		? NV097_SET_BLEND_FUNC_DFACTOR_V_ONE
		: NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;

	Color4ub( r, g, b, a );

	nv2a_2d_append_quad( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f );

	nv2a_2d_desired.blend_enable = false; /* alpha test is left untouched, matching the reference */
}

/* C4/C6: port of R_DrawStretchPic (ref/gl/gl_draw.c:36-53). Texel
   coordinates (linear textures address in texels, divergences.md #100);
   a slot without TF_CLAMP whose s/t fall outside [0,1] is split into
   per-integer-cell quads on the CPU first (C6) - this renderer has no
   hardware wrap-addressing mode for a linear/unswizzled texture. */
static void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	const nv2a_texture_t *tex;
	float ss, st;

	GL_Bind( 0, (unsigned int)texnum );
	tex = nv2a_tex_get( nv2a_tex_current());
	nv2a_tex_coord_scale( tex, &ss, &st ); /* texels for linear, 1 for DXT (divergences.md #131) */

	if( !FBitSet( tex->flags, TF_CLAMP )
		&& ( s1 < 0.0f || s1 > 1.0f || s2 < 0.0f || s2 > 1.0f || t1 < 0.0f || t1 > 1.0f || t2 < 0.0f || t2 > 1.0f ))
	{
		static nv2a_2d_tile_t tiles[256];
		uint32_t n, i;
		int clamped = 0;

		n = nv2a_2d_tile_split( x, y, w, h, s1, t1, s2, t2, tiles, ARRAYSIZE( tiles ), &clamped );
		if( clamped )
			gEngfuncs.Con_DPrintf( S_ERROR "ref_nv2a: %s: wrap span too large, drawing clamped\n", tex->name );

		for( i = 0; i < n; i++ )
		{
			nv2a_2d_append_quad( tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h,
				tiles[i].s1 * ss, tiles[i].t1 * st,
				tiles[i].s2 * ss, tiles[i].t2 * st );
		}
		return;
	}

	nv2a_2d_append_quad( x, y, w, h, s1 * ss, t1 * st, s2 * ss, t2 * st );
}

/* X9-X: the frame, in the engine's own order (cl_view.c):
   R_BeginFrame (V_PreRender) opens and clears the back buffer;
   GL_RenderFrame (V_RenderView) draws the world; the engine's 2D calls
   (V_PostRender: HUD, console, menu) draw on top; R_EndFrame presents.
   ref/gl splits it the same way (gl_rmain.c:1033, :1076, :1116). The
   engine calls R_BeginFrame and R_EndFrame as a pair - neither runs
   while the loading plaque disables the screen (V_PreRender returns
   early) - but R_EndFrame still refuses to present a frame that was
   never opened. */
static void nv2a_try_pending_upload( void );

static void R_BeginFrame( qboolean clearScene )
{
	if( !nv2a_ready )
		return;

	/* X9-T: the one retry of a failed R_NewMap upload - by the first frame
	   after it, the client has freed the previous map's unused models. */
	if( nv2a_pending_upload.pending && nv2a_pending_upload.retried )
		nv2a_try_pending_upload();

	debug_frame_count++;

	/* X9-E, codex review: re-armed per frame so GL_RenderFrame keeps only
	   this frame's first real world pass. */
	nv2a_view_captured_this_frame = false;
	nv2a_world_drawn_this_frame = false;

	nv2a_wait_vblank();
	pb_reset();
	/* pbkit's pb_begin() (pbkit.c:1786) only returns the write pointer
	   when DBG is off, as it is (pbkit.c:14): no block is opened. Taken
	   right after the reset to the head, the whole frame's pushbuffer
	   is the span from here to R_EndFrame's own reading. */
	nv2a_ft_pb_frame_start = pb_begin();
	nv2a_ft_models_frame_start = nv2a_studio_models_drawn();
	pb_target_back_buffer();

	pb_erase_depth_stencil_buffer( 0, 0, nv2a_width, nv2a_height );
	pb_fill( 0, 0, nv2a_width, nv2a_height, 0x00000000 );

	nv2a_wait_gpu_idle();

	nv2a_frame_open = true;
	nv2a_lit_state_current = false; /* only the world pass (or a brush entity) establishes it within a frame */
	nv2a_sgpu_entered = false; /* divergences.md #143: re-entered by the first GPU studio mesh */
	nv2a_sgpu_invalidate_consts(); /* ... and its gamma segments and other constants reloaded, once a frame */

	/* X9-X: releases whatever GL_FreeTexture/GL_UpdateTexture queued last
	   frame (D4 - safe now, the pb_busy wait above proves the GPU is
	   idle), then resets the 2D vertex ring and re-enters 2D mode, as
	   ref/gl's own R_BeginFrame does (gl_rmain.c:1045). */
	nv2a_tex_begin_frame();
	nv2a_2d_ring_write_offset = 0;
	nv2a_2d_ring_pending_verts = 0;
	/* X9-X item 14 step 12: the decal buffer too, past the same wait. */
	nv2a_decal_vb_used = 0;
	nv2a_decal_stats.surfaces = nv2a_decal_stats.decals = nv2a_decal_stats.verts = nv2a_decal_stats.dropped = 0;
	R_Set2DMode( true );
	nv2a_no_world_backdrop_pending = true; /* set after the call above, so only the engine's own next R_Set2DMode(true) draws it */
}

static void R_EndFrame( void )
{
	if( !nv2a_ready || !nv2a_frame_open )
		return;

	/* A frame V_PostRender never reached (SCR_UpdateScreen skips it on
	   some changelevel frames, cl_scrn.c:703) still owes the no-world
	   backdrop; R_Set2DMode(true) draws it. Before nv2a_frame_open drops -
	   nv2a_2d_append refuses a closed frame. */
	if( nv2a_no_world_backdrop_pending )
		R_Set2DMode( true );

	nv2a_frame_open = false;

	/* X9-X: flushes the 2D pass's own pending batch, as ref/gl's own
	   R_EndFrame does (gl_rmain.c:1125). */
	R_Set2DMode( false );

	/* The frame's work counters that are states rather than events
	   (nv2a_ft.h). */
	if( nv2a_ft_pb_frame_start )
		NV2A_FT_SET( XBOX_FT_W_PB_DWORDS, pb_begin() - nv2a_ft_pb_frame_start );
	NV2A_FT_SET( XBOX_FT_W_WORLD_FACES, nv2a_world_drawn_this_frame ? nv2a_cull_faces_visible : 0 );
	NV2A_FT_SET( XBOX_FT_W_PVS_LEAVES, nv2a_world_drawn_this_frame ? nv2a_ft_pvs_leaves : 0 );
	NV2A_FT_SET( XBOX_FT_W_STUDIO_MODELS, nv2a_studio_models_drawn() - nv2a_ft_models_frame_start );

	nv2a_wait_gpu_idle();
	nv2a_wait_swap();
}

/* X9-T: the engine has freed the map archive by the time it calls this
   (Mod_LoadModel frees it right after Mod_ProcessRenderData), which is
   what makes room for the map's GPU buffers. */
/* Builds the pending map's GPU buffers (nv2a_pending_upload). A failure
   keeps the tables for one retry, unless it was the retry. */
static void nv2a_try_pending_upload( void )
{
	/* X9-X step 5 follow-up: the world's buffers get the memory before
	   studio skins do - they re-upload lazily on their next bind. Both
	   callers run between frames (R_NewMap during the load, R_BeginFrame
	   before it opens the frame), after R_EndFrame's pb_busy spin. */
	nv2a_tex_flush_skin_cache();
	/* divergences.md #143: the studio GPU geometry likewise - rebuilt on
	   each submodel's next draw. Same safety argument as the skins. */
	if( !nv2a_frame_open )
		nv2a_sgpu_cache_flush();

	/* X9-U: the tables first. R_NewMap runs before the client frees the
	   previous map's unused models (Mod_FreeUnused, cl_parse.c), so a
	   table build that runs out of memory gets the same one retry from
	   R_EndFrame as the GPU buffers (nv2a-xbox-specialist, X9-U review); a
	   malformed archive just fails twice. nv2a_load_map_geometry frees the
	   map's resources on failure, pending included, so what the retry
	   needs is kept here and put back. */
	if( !nv2a_pending_upload.tables_built )
	{
		static mpak_header_t header;
		static byte first_sector[MPAK_SECTOR_SIZE];
		char name[sizeof( nv2a_pending_upload.name )];
		const model_t *owner = nv2a_map_owner;
		const qboolean retried = nv2a_pending_upload.retried;

		memcpy( name, nv2a_pending_upload.name, sizeof( name ));
		memcpy( &header, &nv2a_pending_upload.header, sizeof( header ));
		memcpy( first_sector, nv2a_pending_upload.first_sector, sizeof( first_sector ));
		if( !nv2a_load_map_geometry( &header, first_sector, name ))
		{
			if( retried )
			{
				gEngfuncs.Con_Printf( "ref_nv2a: %s's tables still could not be built - not drawing its real geometry\n", name );
				return;
			}
			nv2a_map_owner = owner;
			memcpy( &nv2a_pending_upload.header, &header, sizeof( header ));
			memcpy( nv2a_pending_upload.first_sector, first_sector, sizeof( first_sector ));
			memcpy( nv2a_pending_upload.name, name, sizeof( name ));
			nv2a_pending_upload.pending = true;
			nv2a_pending_upload.tables_built = false;
			nv2a_pending_upload.retried = true; /* R_EndFrame tries once more */
			return;
		}
		nv2a_pending_upload.tables_built = true;
	}

	if( nv2a_upload_map_gpu_data())
	{
		nv2a_pending_upload.pending = false;
		nv2a_map_ready = true;
	}
	else if( nv2a_pending_upload.retried )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: %s's GPU buffers still do not fit - not drawing its real geometry\n", nv2a_pending_upload.name );
		nv2a_free_map_resources();
	}
	else
		nv2a_pending_upload.retried = true; /* R_EndFrame tries once more */
}

static void R_NewMap( void )
{
	if( nv2a_pending_upload.pending )
		nv2a_try_pending_upload();

	/* X9-X item 14 step 11: the water textures' fog parameters. */
	R_WarpNewMap( gp_cl->models[1] );

	/* X9-X item 14 step 12: R_ClearDecals, gl_context.c:403 - clear all
	   level decals - and GL_BuildLightmaps' reset of every surface's decal
	   chain (gl_context.c:429, gl_rsurf.c:3932-3944). */
	R_ClearDecals();
	R_DecalClearSurfaceChains();

	/* X9-X step 5, gl_context.c:405: forget every cached per-client player
	   model on a map change - the client index<->model_t mapping is only
	   valid for the map that built it. */
	R_StudioResetPlayerModels();
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
	.R_BeginFrame         = R_BeginFrame,
	.R_RenderScene        = R_SimpleStub,
	.R_EndFrame           = R_EndFrame,
	.R_PushScene          = R_SimpleStub,
	.R_PopScene           = R_SimpleStub,
	.GL_BackendStartFrame = R_SimpleStub,
	.GL_BackendEndFrame   = R_SimpleStub,

	.R_ClearScreen    = R_SimpleStub,
	.R_AllowFog       = R_WarpAllowFog, /* X9-X item 14 step 11: gl_rmain.c:42 */
	.GL_SetRenderMode = GL_SetRenderMode,

	.R_AddEntity      = R_AddEntity,
	.R_ProcessEntData = R_ProcessEntData,

	.R_ShowTextures = R_SimpleStub,

	.R_GetTextureOriginalBuffer = R_GetTextureOriginalBuffer,
	.GL_LoadTextureFromBuffer   = GL_LoadTextureFromBuffer,
	.GL_ProcessTexture          = GL_ProcessTexture,
	.R_SetupSky                 = R_SetupSky,

	.R_Set2DMode      = R_Set2DMode,
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

	.R_DecalShoot      = R_DecalShoot,      /* X9-X item 14 step 12: r_decals.c, gl_context.c:519-522 */
	.R_DecalRemoveAll  = R_DecalRemoveAll,
	.R_CreateDecalList = R_CreateDecalList,
	.R_ClearAllDecals  = R_ClearAllDecals,

	.R_StudioEstimateFrame = R_StudioEstimateFrame,
	.R_StudioLerpMovement  = R_StudioLerpMovement,
	.R_StudioFillAPI          = R_StudioFillAPI,
	.R_StudioSetDrawInterface = R_StudioSetDrawInterface,

	.R_SetSkyCloudsTextures     = R_SetSkyCloudsTextures,
	.GL_SubdivideSurface = GL_SubdivideSurface,
	.CL_RunLightStyles   = R_LightmapRunLightStyles, /* X9-X step 8: CL_RunLightStyles, plus "the style table is filled" */

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
	.GL_FreeTexture  = GL_FreeTexture,
	.R_OverrideTextureSourceSize = R_OverrideTextureSourceSize,

	.GL_UpdateTexture = GL_UpdateTexture,

	.GL_Bind = GL_Bind,

	.GL_RenderFrame    = GL_RenderFrame,
	.GL_OrthoBounds    = GL_OrthoBounds,
	.R_SpeedsMessage   = R_SpeedsMessage,
	.Mod_GetCurrentVis = Mod_GetCurrentVis,
	.R_NewMap          = R_NewMap,
	.R_ClearScene      = R_ClearScene,

	.TriRenderMode = TriRenderMode,
	.Begin         = Begin,
	.End           = End,
	.Color4f       = Color4f,
	.Color4ub      = Color4ub,
	.Vertex3fv     = Vertex3fv,
	.Vertex3f      = Vertex3f,
	.CullFace      = CullFace,

	.R_FillRenderAPI = R_FillRenderAPI,
	.R_FillTriAPI    = R_FillTriAPI,

	.VGUI_SetupDrawing   = VGUI_SetupDrawing,
};
