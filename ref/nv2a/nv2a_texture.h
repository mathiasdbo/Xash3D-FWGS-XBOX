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
	uint32_t *width, uint32_t *height, qboolean *bilinear );

/* The builtins GL_CreateTexture records (D1) - used by FillRGBA (r_context.c,
   C3: binds *white) and by GL_Bind's own fallback. */
unsigned int nv2a_tex_default( void );
unsigned int nv2a_tex_white( void );

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
