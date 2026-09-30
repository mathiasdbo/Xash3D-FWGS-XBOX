/*
nv2a_texture.h - X9-X Part D: ref/nv2a's own texture manager, modelled on
ref/soft's table + entry points (ref/soft/r_image.c, ref/soft/r_local.h's
image_t) and ref/gl's pixel-processing pipeline (GL_ProcessImage). CPU-side
table and upload path live in r_image.c; the GPU register pushes that need
the currently-bound texture's resolved storage (offset/pitch/width/height/
filter) stay in r_context.c beside the existing push helpers and read the
slot through nv2a_tex_get() below.

Scope this phase (see docs/r3d/design/x9-x-textures-and-2d.md and the
X9-X spec's own outOfScope list): every texture is uploaded flat, at
PF_RGBA_32 (well, A8B8G8R8 byte order on the GPU - see nv2a_tex_upload's
own comment), with no mip chain, no swizzling and no compressed input.
World (.mip) textures never reach this manager at all in the Xbox nv2a
bake (X9-X Part A drops the worldspawn "wad" key, so the engine's own
Mod_LoadTextureData never calls GL_LoadTexture for one) - only 2D/HUD/
sprite/menu art does.

X9-X step 5 follow-up: studio model skins are the one exception - lazy,
16 bpp and cached under a fixed budget (NV2A_STUDIO_SKIN_CACHE_BYTES
below, nv2a_skin_cache.h). Player colour-remap copies
(engine/client/cl_remap.c CL_DuplicateTexture) still come through the
plain GL_LoadTexture path at RGBA32: they are per-entity copies whose
palette is only valid during that one call, so they cannot be re-read
later.
*/
#ifndef NV2A_TEXTURE_H
#define NV2A_TEXTURE_H

#include "xash3d_types.h"
#include "render_api.h" /* texFlags_t, TF_*, PARM_* */
#include "com_image.h"  /* rgbdata_t, pixformat_t, IMAGE_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ref/soft's own Xbox MAX_TEXTURES (ref/soft/r_local.h:48-51, set by
   tools/xbox-cc.sh:341) - kept identical here so GL_CheckTexName/
   GL_AllocTexture semantics match what the engine already expects from
   an Xbox ref module. */
#define NV2A_MAX_TEXTURES 1024

/* Same ceiling nv2a_init_texture()'s own NV2A_TEST_MAXRAM already uses
   for every other contiguous allocation in this module (r_context.c). */
#define NV2A_TEXTURE_MAXRAM 0x03FFAFFF

/* D1: the resident texture arena (3 MiB), the 2D vertex ring (256 KiB)
   and an upper bound on this table's own footprint - see
   tools/asset-studio/src/formats/Nv2aMapFit.h's own mirrored constants. */
#define NV2A_TEXTURE_ARENA_BYTES ( 3u * 1024u * 1024u )
#define NV2A_2D_VERTEX_RING_BYTES ( 256u * 1024u )

/* Studio skin cache (docs/r3d/divergences.md #126;
   docs/r3d/design/x9-x-studio-models.md "Memory"). ref/gl uploads every
   skin of every precached model at load (gl_studio.c R_StudioLoadTexture);
   measured in xemu with that behaviour at RGBA32, the skins alone took
   14.5 MiB on c1a0 and c4a3 failed to load. Here a skin is uploaded the
   first time it is bound, at 16 bpp, into a cache of at most this many
   bytes - the term the bake-time fit check charges
   (tools/asset-studio/src/formats/Nv2aMapFit.h,
   kXboxStudioSkinCacheBytes).

   Sized from xemu measurements on c1a0 (the texture table read live over
   the debugger): EVERY skin the map precaches (458, weapons, gibs and
   NPCs included) is 4.11 MiB at 16 bpp, the opening view draws 48 of
   them in 0.37 MiB, and the largest single skin is 132 KiB. 2 MiB holds
   half of every skin the map could ever show, over five times the
   opening view; evictions beyond that are LRU and cost a re-read, not a
   failure. Tune from `texturelist`'s eviction count, not by guessing. */
#define NV2A_STUDIO_SKIN_CACHE_BYTES ( 2u * 1024u * 1024u )

/* The cache is backed in chunks of this size, each one physically
   contiguous allocation made only when the chunks already backed have no
   room (nv2a_skin_cache.h) - NOT reserved at R_Init: a reservation there
   sat mostly unused (0.37 MiB used of 4 MiB on c1a0's opening view)
   while taking memory the map's own GPU upload needed first (disposal's
   world upload failed with it). A map load flushes the whole cache and
   gives its chunks back (nv2a_tex_flush_skin_cache) before that upload.
   512 KiB fits a 512x512 skin at 16 bpp (pitch 1024, the largest a chunk
   holds; larger ones draw untextured) with room to spare over the 132 KiB
   retail maximum measured above. */
#define NV2A_STUDIO_SKIN_CHUNK_BYTES ( 512u * 1024u )

/* A skin is evictable once it has gone this many frames without being
   bound or drawn. 1 is the correctness floor: R_BeginFrame spins pb_busy
   before it opens a frame (r_context.c), so nothing drawn in an earlier
   frame is still being read by the GPU, and a skin used THIS frame may
   still be. The second frame is thrash avoidance, not safety: a skin
   drawn last frame is almost certainly on screen again this frame, just
   not reached yet by this frame's studio pass. */
#define NV2A_STUDIO_SKIN_EVICT_MIN_AGE 2u

/* Per-frame upload allowance. An upload re-reads the skin's 8-bit data
   from the model file (an open, a seek and a read - a cold HDD seek is
   ~10-15 ms and a DVD one can pass 100 ms, NV2A specialist review; not
   yet measured on the console, and xemu does not model it) and expands
   it through a palette table into the cache. So a scene that suddenly
   shows many new skins (a map start, a view model switch) spreads them
   over a few frames (drawn untextured until then) instead of hitching
   one frame by all of them. The first upload of a frame always goes,
   whatever its size. Tune from console measurements. */
#define NV2A_STUDIO_SKIN_UPLOADS_PER_FRAME 2u
#define NV2A_STUDIO_SKIN_BYTES_PER_FRAME   ( 256u * 1024u )

/* NV097_SET_TEXTURE_FORMAT_COLOR_* values a texture slot can carry
   (deps/nxdk/lib/pbkit/nv_regs.h:550-551, :577) - mirrored here so
   r_image.c needs no pbkit include; r_context.c STATIC_ASSERTs each
   against nv_regs.h. Studio skins are 16 bpp (A1R5G5B5 when masked,
   R5G6B5 otherwise); everything else stays A8B8G8R8. */
#define NV2A_TEX_COLOR_LU_A1R5G5B5 0x10u
#define NV2A_TEX_COLOR_LU_R5G6B5   0x11u
#define NV2A_TEX_COLOR_LU_A8B8G8R8 0x3Fu

/* divergences.md #131 (menu GPU memory): the narrower storage classes
   nv2a_tex_upload picks when an image allows it without changing a
   sampled value (nv2a_2d_util.h nv2a_2d_classify_rgba), and the
   block-compressed formats a DDS pic arrives in (nv_regs.h:547-549,
   :562-563). Compressed textures are addressed like the world's DXT1
   pages: normalized coordinates, sized by the FORMAT word's
   BASE_SIZE_U/V (divergences.md #100) - never by IMAGE_RECT/pitch, which
   size only the LU_IMAGE formats above. */
#define NV2A_TEX_COLOR_LU_A8       0x1Fu
#define NV2A_TEX_COLOR_LU_A8Y8     0x20u
#define NV2A_TEX_COLOR_L_DXT1      0x0Cu /* L_DXT1_A1R5G5B5 */
#define NV2A_TEX_COLOR_L_DXT23     0x0Eu /* L_DXT23_A8R8G8B8 */
#define NV2A_TEX_COLOR_L_DXT45     0x0Fu /* L_DXT45_A8R8G8B8 */

#define NV2A_TEX_COLOR_IS_COMPRESSED( fmt ) \
	(( fmt ) == NV2A_TEX_COLOR_L_DXT1 || ( fmt ) == NV2A_TEX_COLOR_L_DXT23 || ( fmt ) == NV2A_TEX_COLOR_L_DXT45 )

typedef struct nv2a_texture_s
{
	char       name[256];  /* ref/soft image_t::name's own size (r_local.h:303) */
	texFlags_t flags;
	word       width, height;       /* upload size */
	word       srcWidth, srcHeight; /* original, pre-resample size */
	uint32_t   pitch;                /* GPU row pitch, bytes (0: no storage) */

	/* Storage: EITHER an offset into the shared arena (has_arena_storage
	   true) OR a standalone contiguous allocation (fallback != NULL) OR
	   neither (gpu_size 0 - GL_CreateTexture(NULL), dimensions only). */
	qboolean   has_arena_storage;
	size_t     gpu_offset;   /* valid iff has_arena_storage */
	void      *fallback;     /* valid iff storage exists and is NOT in the arena */
	size_t     gpu_size;     /* bytes actually resident; 0 if neither of the above */

	uint32_t   last_used_frame; /* D4/GL_UpdateTexture: same-frame write hazard */

	/* NV2A_TEX_COLOR_* of whatever storage exists (0 is treated as
	   A8B8G8R8, the format of every upload path but the studio skin one). */
	uint32_t   color_format;

	/* A THIRD storage kind: gpu_offset is into the studio skin cache span
	   (NV2A_STUDIO_SKIN_CACHE_BYTES), owned by the skin cache, never the
	   shared arena or a fallback. Only ever set on a slot with `skin`. */
	qboolean   has_skin_storage;

	/* Non-NULL: a lazily uploaded studio skin (nv2a_tex_register_studio_skin).
	   The slot keeps its name, size and flags from registration on - the
	   engine reads mstudiotexture_t::index as a live, named texnum
	   (engine/client/cl_remap.c CL_DuplicateTexture/CL_UpdateStudioTexture/
	   CL_CmpStudioTextures) - and only its GPU storage comes and goes:
	   uploaded on first GL_Bind, evicted by the cache, re-uploaded on the
	   next bind. has_skin_storage false == "not uploaded". */
	struct nv2a_skin_source_s *skin;

	rgbdata_t *original; /* kept source, TF_KEEP_SOURCE only (D2) */

	uint               hashValue;
	struct nv2a_texture_s *nextHash;
} nv2a_texture_t;

/* B: Nv2aMapFit.h's kXboxTextureTableBytes (tools/asset-studio) is a
   512 KiB upper bound on this table plus its name-hash table - checked
   here, the same way nv2a_map_face_t's own size is pinned in
   r_context.c, so the two can never silently drift apart. */
STATIC_ASSERT(( NV2A_MAX_TEXTURES * sizeof( nv2a_texture_t )) + (( NV2A_MAX_TEXTURES >> 2 ) * sizeof( void * )) <= 512u * 1024u,
	"nv2a_texture_t table + name hash table exceeds Nv2aMapFit.h's kXboxTextureTableBytes budget" );

/* R_Init/R_Shutdown/R_BeginFrame hooks (called from r_context.c). */
qboolean nv2a_tex_init( void );
void     nv2a_tex_shutdown( void );
void     nv2a_tex_begin_frame( void ); /* releases deferred frees - AFTER the pb_busy wait */

/* ref_interface_t entry points (D3), assigned directly into gReffuncs. */
int         GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags );
int         GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );
int         GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags );
int         GL_FindTexture( const char *name );
void        GL_FreeTexture( unsigned int texnum );
const char *GL_TextureName( unsigned int texnum );
const byte *GL_TextureData( unsigned int texnum );
const byte *R_GetTextureOriginalBuffer( unsigned int idx );
void        R_OverrideTextureSourceSize( unsigned int texnum, unsigned int srcWidth, unsigned int srcHeight );
void        GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt );
void        GL_Bind( int tmu, unsigned int texnum );
void        R_TextureList_f( void );

/* Bounds-checked slot accessor - index 0 (the hashed "*unused*" slot,
   also REF_DEFAULT_TEXTURE's home) for anything out of range. The
   reference only ever ASSERTs there (r_image.c:52); this module has no
   assert-and-continue story on Xbox, so it returns a real, harmless
   slot instead - a named divergence (D3). */
const nv2a_texture_t *nv2a_tex_get( unsigned int texnum );

/* D5: port of GL_TextureFilteringEnabled (ref/gl/gl_image.c:72-90), also
   used by the 2D flush (r_context.c) to pick the stage filter word. */
qboolean nv2a_texture_filtering_enabled( const nv2a_texture_t *tex );

/* Total bytes resident on the GPU right now (arena + fallback), for
   PARM_TEX_MEMORY and `texturelist`. */
size_t nv2a_tex_memory( void );

/* `texturelist`'s own extra reporting (D7): arena used/free/largest-free
   and how many slots fell back to a standalone allocation. */
void nv2a_tex_arena_stats( size_t *used, size_t *free_bytes, size_t *largest_free, uint32_t *fallback_count );

/* The texnum currently bound to tmu 0 (GL_Bind only ever tracks tmu 0 -
   D6) - what the 2D flush (r_context.c) pushes the stage-0 texture
   block for. 0 (the default texture) if nothing has bound yet. */
unsigned int nv2a_tex_current( void );

/* The 2D flush's own batch key (r_context.c): the resolved, GPU-ready
   parameters of `texnum`'s storage. Returns false (nothing written) for
   a slot with no storage - GL_Bind already substitutes the default
   texture in that case, so this is a defensive case, not a normal one. */
qboolean nv2a_tex_gpu_params( unsigned int texnum, uint32_t *phys_offset, uint32_t *pitch,
	uint32_t *width, uint32_t *height, qboolean *bilinear, uint32_t *color_format );

/* What a 0..1 texture coordinate is multiplied by before it reaches the
   GPU for `tex`: its width/height for a linear (LU_IMAGE) texture, which
   the NV2A samples in texel units (divergences.md #100), and 1 for a
   compressed one, which it samples in normalized units. Every 2D/TriAPI
   path that used to scale by tex->width/height goes through this. */
void nv2a_tex_coord_scale( const nv2a_texture_t *tex, float *scale_s, float *scale_t );

/* Marks `texnum` used this frame. The 2D/TriAPI append path calls it for
   every primitive it queues: a texture can stay bound across frames with
   no new GL_Bind, and the skin cache must never evict (and overwrite)
   storage that a draw queued THIS frame still points at. */
void nv2a_tex_mark_used( unsigned int texnum );

/* Registers a lazily uploaded studio skin (see nv2a_texture_t::skin):
   a named slot with no storage, whose 8-bit pixels (width*height
   indices, then the 768-byte palette) are read back from `path` at
   `pixel_offset` on first bind. `file_length` is `path`'s size at
   registration - a mismatch at upload time means the file changed, and
   the skin stays untextured rather than reading the wrong bytes. An
   existing slot of the same name is returned as is, like GL_LoadTexture.
   Returns 0 (caller falls back to the eager GL_LoadTexture path) if the
   arguments do not describe a readable skin or no slot is free. */
int nv2a_tex_register_studio_skin( const char *texname, texFlags_t flags, const char *path, uint32_t file_length,
	uint32_t pixel_offset, int studio_flags, int width, int height );

/* Evicts every studio skin and gives the cache's chunks back, so the
   next map's world GPU upload gets that memory first; the skins still
   wanted re-upload on their next bind. Called by r_context.c right before
   a map's GPU upload. A no-op inside a frame (the GPU may still be
   reading skins then). */
void nv2a_tex_flush_skin_cache( void );

/* The builtins GL_CreateTexture records (D1) - used by FillRGBA (r_context.c,
   C3: binds *white) and by GL_Bind's own fallback. */
unsigned int nv2a_tex_default( void );
unsigned int nv2a_tex_white( void );
/* X9-X step 7: ref/gl's tr.particleTexture (gl_image.c:1656), bound by
   CL_DrawParticles (r_efx.c). */
unsigned int nv2a_tex_particle( void );
/* X9-X item 14 step 10: ref/gl's tr.grayTexture (gl_image.c:1660), the
   stub R_DrawSkyBox binds for a side with no image (gl_warp.c:351). */
unsigned int nv2a_tex_gray( void );

/* Implemented in r_context.c, needed here for D4/D6's own frame-hazard
   and filter-selection logic - see each call site's own comment. */
qboolean nv2a_frame_is_open( void );          /* D4: is a frame currently open (R_BeginFrame..R_EndFrame)? */
uint32_t nv2a_current_frame_number( void );   /* D4: this frame's own counter, for the same-frame write hazard */
void     nv2a_2d_flush_and_wait( void );      /* D4: flush the pending 2D batch (if any) and spin pb_busy */
qboolean nv2a_global_texture_nearest( void ); /* D5: nv2a_r_texturenearest.value != 0 */

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEXTURE_H */
