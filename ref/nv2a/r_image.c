/*
r_image.c - X9-X Part D: ref/nv2a's texture manager. CPU-side table,
upload path and every GL_*Texture* / RefGetParm-adjacent entry point the
engine calls through gReffuncs (r_context.c). See nv2a_texture.h for the
scope statement (flat upload, no mips, no world textures - Part A keeps
those off the GPU entirely; 8/16 bpp and DXT storage for 2D pics since
divergences.md #131).

Ported from, in order of how closely each piece is followed:
  - ref/soft/r_image.c: the table itself, GL_AllocTexture, GL_CheckTexName,
    GL_TextureForName, GL_SetTextureDimensions, R_InitImages/R_ShutdownImages,
    R_TextureList_f (r_image.c:110-134, 438-509, 517-560, 895-980).
  - ref/gl/gl_image.c: GL_ProcessImage, GL_TextureFilteringEnabled,
    GL_LoadTexture, GL_LoadTextureFromBuffer, GL_FreeTexture
    (gl_image.c:72-90, 1107-1170, 1372-1411, 1568-1604, 1734-1741).
  - ref/gl/gl_draw.c: GL_UpdateTexture (gl_draw.c:60-116).
  - ref/gl/gl_context.c: GL_TextureName/GL_TextureData/
    R_GetTextureOriginalBuffer/R_OverrideTextureSourceSize
    (gl_context.c:31-39, 279-286, 377-383).
  - ref/gl/gl_backend.c: GL_Bind (gl_backend.c:180-185).
*/
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xash3d_types.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR - not pulled in transitively here, unlike ref/soft/ref/gl's own r_local.h/gl_local.h (X9-X finding) */
#include "crtlib.h" /* Q_strncpy, Q_stricmp, Q_memprint - also not transitive here */
#include "crclib.h" /* COM_HashKey, as ref/soft/r_image.c uses */
#include "ref_common.h"

#include "studio.h" /* mstudiotexture_t, the header Image_LoadMDL reads (studio skin upload) */
#include "enginefeatures.h" /* ENGINE_LINEAR_GAMMA_SPACE, the TextureToGamma switch */

#include "nv2a_texture.h"
#include "nv2a_arena.h"
#include "nv2a_skin_cache.h"
#include "nv2a_studio_util.h" /* NV2A_STUDIO_NF_MASKED, pinned to studio.h in r_studio.c */
#include "nv2a_2d_util.h" /* texel classification and DXT sizes (divergences.md #131) */
#include "nv2a_ft.h" /* the frame-time recorder's k_skin_read span (divergences.md #141) */

/* ------------------------------------------------------------------ */
/* Table storage.                                                      */
/* ------------------------------------------------------------------ */

#define NV2A_TEXTURES_HASH_SIZE ( NV2A_MAX_TEXTURES >> 2 )

/* Calloc'd in nv2a_tex_init(), not static - the #114 discipline
   (ref/soft/r_image.c:20-28's own XASH_XBOX table, same reasoning: keep
   this off .bss for a console that also links ref/soft beside it). */
static nv2a_texture_t *nv2a_textures;
static nv2a_texture_t *nv2a_textures_hash[NV2A_TEXTURES_HASH_SIZE];
static uint32_t nv2a_textures_count;

static nv2a_arena_t *nv2a_texture_arena;
static void         *nv2a_texture_arena_mem; /* the real contiguous span the arena sub-allocates offsets within */
static uint32_t      nv2a_fallback_count;    /* slots whose storage did not fit the arena */

static unsigned int nv2a_bound_texture_tmu0;
static unsigned int nv2a_texnum_default, nv2a_texnum_particle, nv2a_texnum_white, nv2a_texnum_gray, nv2a_texnum_black;

/* D4: storage queued for release at the next safe point (R_BeginFrame,
   after its own pb_busy wait, or R_Shutdown after pb_kill) rather than
   freed while the GPU might still be reading it. Sized generously for
   "however many textures could plausibly be freed inside one frame" -
   see nv2a_tex_queue_free's own comment for what happens past this. */
#define NV2A_TEX_MAX_DEFERRED_FREES 64

typedef struct
{
	qboolean has_arena_storage;
	size_t   gpu_offset;
	void    *fallback;
	qboolean skin_storage; /* gpu_offset is into the studio skin cache span instead */
} nv2a_deferred_free_t;

static nv2a_deferred_free_t nv2a_deferred_frees[NV2A_TEX_MAX_DEFERRED_FREES];
static uint32_t nv2a_deferred_free_count;

/* Studio skin cache (nv2a_texture.h's NV2A_STUDIO_SKIN_CACHE_BYTES): the
   bookkeeping (nv2a_skin_cache.c) plus the real contiguous chunks it
   hands out offsets within, each backed on demand (nv2a_skin_grow) and
   given back when a flush empties it (nv2a_skin_release). NULL cache ==
   the bookkeeping could not be allocated; every studio skin then draws
   untextured (white) rather than failing the renderer. */
#define NV2A_SKIN_MAX_CHUNKS ( NV2A_STUDIO_SKIN_CACHE_BYTES / NV2A_STUDIO_SKIN_CHUNK_BYTES )
STATIC_ASSERT( NV2A_SKIN_MAX_CHUNKS >= 1, "the skin cache budget must hold at least one chunk" );
static nv2a_skin_cache_t *nv2a_skin_cache;
static void              *nv2a_skin_chunk_mem[NV2A_SKIN_MAX_CHUNKS];

/* Rate-limited "cache full" reporting: at most one line per this many
   frames, carrying how many binds drew untextured in between. */
#define NV2A_SKIN_LOG_INTERVAL_FRAMES 300u
static qboolean nv2a_skin_logged_once;
static uint32_t nv2a_skin_log_frame;
static uint32_t nv2a_skin_missed_binds;

/* The source of one lazily uploaded studio skin (nv2a_texture_t::skin).
   The pixels are NOT kept in RAM: with XASH_LOW_MEMORY (tools/xbox-cc.sh)
   the engine truncates every studio model's texture data right after
   Mod_StudioLoadTextures (engine/common/mod_studio.c:1105-1116
   Mod_MaybeTruncateStudioTextureData), and a T.mdl's pixels live in a
   buffer freed at :1175. So an upload re-reads them from the model file. */
typedef struct nv2a_skin_source_s
{
	char     path[MAX_QPATH];
	uint32_t file_length;
	uint32_t pixel_offset;  /* width*height indices, then the 768-byte palette */
	int      studio_flags;  /* mstudiotexture_t::flags at registration (STUDIO_NF_*) */
	word     width, height; /* mstudiotexture_t::width/height - the SOURCE size */
	qboolean failed;        /* permanent: file gone/changed or undecodable - never retried */
} nv2a_skin_source_t;

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static nv2a_texture_t *nv2a_tex_get_mutable( unsigned int texnum )
{
	if( texnum >= NV2A_MAX_TEXTURES )
		return &nv2a_textures[0];
	return &nv2a_textures[texnum];
}

const nv2a_texture_t *nv2a_tex_get( unsigned int texnum )
{
	return nv2a_tex_get_mutable( texnum );
}

unsigned int nv2a_tex_default( void ) { return nv2a_texnum_default; }
unsigned int nv2a_tex_white( void )   { return nv2a_texnum_white; }
unsigned int nv2a_tex_particle( void ) { return nv2a_texnum_particle; }
unsigned int nv2a_tex_gray( void )     { return nv2a_texnum_gray; }

static qboolean nv2a_tex_check_name( const char *name )
{
	int len;

	if( COM_StringEmptyOrNULL( name ))
		return false;

	len = Q_strlen( name );
	if( len >= (int)sizeof( nv2a_textures->name ))
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: too long name %s (%d)\n", __func__, name, len );
		return false;
	}

	return true;
}

static nv2a_texture_t *nv2a_tex_for_name( const char *name )
{
	uint hash = COM_HashKey( name, NV2A_TEXTURES_HASH_SIZE );
	nv2a_texture_t *tex;

	for( tex = nv2a_textures_hash[hash]; tex; tex = tex->nextHash )
		if( !Q_stricmp( tex->name, name ))
			return tex;

	return NULL;
}

static void nv2a_tex_unlink_hash( nv2a_texture_t *tex )
{
	nv2a_texture_t **prev = &nv2a_textures_hash[tex->hashValue];

	while( *prev )
	{
		if( *prev == tex )
		{
			*prev = tex->nextHash;
			return;
		}
		prev = &(*prev)->nextHash;
	}
}

static nv2a_texture_t *nv2a_tex_alloc_slot( const char *name, texFlags_t flags )
{
	uint32_t i;
	nv2a_texture_t *tex = NULL;

	for( i = 0; i < nv2a_textures_count; i++ )
	{
		if( !nv2a_textures[i].name[0] )
		{
			tex = &nv2a_textures[i];
			break;
		}
	}

	if( !tex )
	{
		if( nv2a_textures_count == NV2A_MAX_TEXTURES )
		{
			gEngfuncs.Host_Error( "%s: MAX_TEXTURES limit exceeds\n", __func__ );
			return &nv2a_textures[0]; /* Host_Error longjmps out; never reached */
		}
		tex = &nv2a_textures[nv2a_textures_count];
		nv2a_textures_count++;
	}

	Q_strncpy( tex->name, name, sizeof( tex->name ));
	tex->flags = flags;

	tex->hashValue = COM_HashKey( name, NV2A_TEXTURES_HASH_SIZE );
	tex->nextHash = nv2a_textures_hash[tex->hashValue];
	nv2a_textures_hash[tex->hashValue] = tex;

	return tex;
}

/* D3: unlike ref/gl and ref/soft (gl_image.c:1401-1405, r_image.c:620-625,
   both a plain memset() that truncates the hash chain at this slot), an
   upload failure here unlinks first - a named divergence, cheap to do
   properly since GL_FreeTexture already needs the same unlink. */
static void nv2a_tex_drop_skin( nv2a_texture_t *tex );

static void nv2a_tex_fail_slot( nv2a_texture_t *tex )
{
	nv2a_tex_drop_skin( tex );
	nv2a_tex_unlink_hash( tex );

	if( tex->original )
	{
		gEngfuncs.FS_FreeImage( tex->original );
		tex->original = NULL;
	}

	memset( tex, 0, sizeof( *tex ));
}

/* ------------------------------------------------------------------ */
/* GPU storage: the shared arena, with a per-texture fallback when it   */
/* has no room (D2).                                                    */
/* ------------------------------------------------------------------ */

static qboolean nv2a_tex_alloc_storage( nv2a_texture_t *tex, size_t bytes )
{
	size_t offset;

	tex->has_arena_storage = false;
	tex->fallback = NULL;
	tex->gpu_offset = 0;
	tex->gpu_size = 0;

	if( bytes == 0 )
		return true; /* GL_CreateTexture(NULL): dimensions only, r_image.c:248-249 */

	offset = nv2a_arena_alloc( nv2a_texture_arena, bytes );
	if( offset != NV2A_ARENA_FAILED )
	{
		tex->has_arena_storage = true;
		tex->gpu_offset = offset;
		tex->gpu_size = bytes;
		return true;
	}

	tex->fallback = MmAllocateContiguousMemoryEx( bytes, 0, NV2A_TEXTURE_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !tex->fallback )
		return false;

	gEngfuncs.Con_Printf( "ref_nv2a: %s (%u bytes) did not fit the texture arena, using a standalone allocation\n",
		tex->name, (unsigned int)bytes );
	nv2a_fallback_count++;
	tex->gpu_size = bytes;
	return true;
}

static void nv2a_tex_free_storage_now( qboolean has_arena_storage, size_t gpu_offset, void *fallback )
{
	if( has_arena_storage )
		nv2a_arena_free( nv2a_texture_arena, gpu_offset );
	else if( fallback )
	{
		MmFreeContiguousMemory( fallback );
		if( nv2a_fallback_count )
			nv2a_fallback_count--;
	}
}

static void nv2a_tex_release_entry_now( const nv2a_deferred_free_t *e )
{
	if( e->skin_storage )
		nv2a_skin_cache_free_offset( nv2a_skin_cache, e->gpu_offset );
	else
		nv2a_tex_free_storage_now( e->has_arena_storage, e->gpu_offset, e->fallback );
}

static void nv2a_tex_queue_entry( const nv2a_deferred_free_t *entry )
{
	if( nv2a_deferred_free_count >= NV2A_TEX_MAX_DEFERRED_FREES )
	{
		/* QA round 2, finding 3: a CPU-side array filling up says nothing
		   about whether the GPU has actually finished reading any of the
		   storage referenced by draws already submitted this frame -
		   releasing anything immediately here, unsynchronized, is
		   exactly the corruption/fault this queue exists to prevent
		   (reachable in one call: a sprite or frame-group unload can
		   queue more than 64 frees at once, engine/client/cl_sprite.c:
		   324-351). Flush whatever 2D batch is pending and spin pb_busy -
		   the same wait GL_UpdateTexture's own same-frame branch already
		   uses above - which proves every draw submitted up to this
		   point has actually retired on the GPU. Only THEN is it safe to
		   drain the entire queue (not just make room for this one entry:
		   the other 64 are exactly as GPU-referenced as this one, and
		   leaving them queued past a proven-idle point buys nothing).
		   nv2a_tex_begin_frame() already does precisely that drain; it
		   is otherwise only ever called from R_BeginFrame/R_Shutdown,
		   but its own precondition - GPU idle - now holds here too. */
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: texture free queue full, flushing and waiting before draining\n" );
		nv2a_2d_flush_and_wait();
		nv2a_tex_begin_frame();
		nv2a_tex_release_entry_now( entry );
		return;
	}

	nv2a_deferred_frees[nv2a_deferred_free_count] = *entry;
	nv2a_deferred_free_count++;
}

static void nv2a_tex_queue_free( qboolean has_arena_storage, size_t gpu_offset, void *fallback )
{
	nv2a_deferred_free_t entry;

	if( !has_arena_storage && !fallback )
		return; /* no storage to release */

	entry.has_arena_storage = has_arena_storage;
	entry.gpu_offset = gpu_offset;
	entry.fallback = fallback;
	entry.skin_storage = false;
	nv2a_tex_queue_entry( &entry );
}

/* Same D4 discipline for a skin cache offset: GL_FreeTexture of a skin
   drawn earlier THIS frame must not hand its bytes to the next upload
   while the GPU may still be reading them. */
static void nv2a_tex_queue_free_skin( size_t skin_offset )
{
	nv2a_deferred_free_t entry;

	entry.has_arena_storage = false;
	entry.gpu_offset = skin_offset;
	entry.fallback = NULL;
	entry.skin_storage = true;
	nv2a_tex_queue_entry( &entry );
}

void nv2a_tex_begin_frame( void )
{
	uint32_t i;

	for( i = 0; i < nv2a_deferred_free_count; i++ )
		nv2a_tex_release_entry_now( &nv2a_deferred_frees[i] );

	nv2a_deferred_free_count = 0;
}

/* ------------------------------------------------------------------ */
/* Pixel processing (D2).                                              */
/* ------------------------------------------------------------------ */

/* Port of ref/gl's GL_ProcessImage (gl_image.c:1107-1170). */
static void nv2a_tex_process_image( nv2a_texture_t *tex, rgbdata_t *pic )
{
	uint img_flags = 0;

	if( FBitSet( tex->flags, TF_FORCE_COLOR ))
		SetBits( pic->flags, IMAGE_HAS_COLOR );
	if( FBitSet( pic->flags, IMAGE_HAS_ALPHA ))
		SetBits( tex->flags, TF_HAS_ALPHA );
	if( FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		SetBits( tex->flags, TF_PREMULTIPLIED );

	if( ImageCompressed( pic->type ))
	{
		/* Out of scope this phase (see nv2a_texture.h) - reachable only
		   if a caller ever hands this a compressed rgbdata_t; matches
		   the reference's own "disable mipmapping, drop KEEP_SOURCE"
		   shape, but nv2a_tex_upload refuses compressed input outright
		   before storage is ever touched. */
		SetBits( tex->flags, TF_NOMIPMAP );
		ClearBits( tex->flags, TF_KEEP_SOURCE );
		return;
	}

	if( FBitSet( pic->flags, IMAGE_HAS_LUMA ))
		SetBits( tex->flags, TF_HAS_LUMA );
	if( FBitSet( pic->flags, IMAGE_QUAKEPAL ))
		SetBits( tex->flags, TF_QUAKEPAL );

	if( FBitSet( tex->flags, TF_MAKELUMA ))
	{
		img_flags |= IMAGE_MAKE_LUMA;
		ClearBits( tex->flags, TF_MAKELUMA );
	}

	if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
		tex->original = gEngfuncs.FS_CopyImage( pic ); /* BEFORE expansion - console font reads this 8-bit original */

	if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
		img_flags |= IMAGE_FORCE_RGBA;

	if( pic->buffer )
		gEngfuncs.Image_Process( &pic, 0, 0, img_flags, 0.0f );

	if( FBitSet( pic->flags, IMAGE_PLAYERDECAL ) && pic->buffer && pic->type == PF_RGBA_32
		&& FBitSet( pic->flags, IMAGE_HAS_ALPHA ) && !FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
	{
		int cnt = (int)( pic->width * pic->height );
		byte *p = pic->buffer;
		int i;

		for( i = 0; i < cnt; i++, p += 4 )
		{
			int a = p[3];
			p[0] = (byte)(( p[0] * a + 127 ) / 255 );
			p[1] = (byte)(( p[1] * a + 127 ) / 255 );
			p[2] = (byte)(( p[2] * a + 127 ) / 255 );
		}
		SetBits( pic->flags, IMAGE_PREMULTIPLIED );
		SetBits( tex->flags, TF_PREMULTIPLIED );
	}

	if( FBitSet( tex->flags, TF_LUMINANCE ))
		ClearBits( pic->flags, IMAGE_HAS_COLOR );
}

/* D2's own trap: Image_Process must never see a PF_LUMINANCE buffer -
   Image_DecompressInternal sets type=RGBA_32 with size 0 when
   Image_Decompress fails (img_utils.c:1260-1277), which is exactly what
   happens if a LUMINANCE buffer is run through it. Expanded here
   instead: L,L,L,255 per source byte, matching a GL_LUMINANCE upload.
   `pic` must be an rgbdata_t THIS module owns outright (either the
   whole way through, as in the load path, or a fresh FS_CopyImage()
   result, as in GL_UpdateTexture) - freeing its old buffer and
   replacing it is exactly what Image_Process itself would have done. */
static void nv2a_tex_expand_luminance_inplace( rgbdata_t *pic )
{
	size_t count = (size_t)pic->width * pic->height;
	byte *out = (byte *)Mem_Malloc( r_temppool, count * 4 );
	size_t i;

	for( i = 0; i < count; i++ )
	{
		byte l = pic->buffer[i];
		out[i * 4 + 0] = l;
		out[i * 4 + 1] = l;
		out[i * 4 + 2] = l;
		out[i * 4 + 3] = 255;
	}

	Mem_Free( pic->buffer );
	pic->buffer = out;
	pic->type = PF_RGBA_32;
	pic->size = count * 4;
	SetBits( pic->flags, IMAGE_HAS_COLOR );
}

/* D2: guarantees an RGBA32-shaped buffer (or a graceful, logged
   failure) after nv2a_tex_process_image has already resolved indexed
   input. ref/gl uploads several other formats natively; this manager
   always needs one RGBA32-shaped buffer, in the exact byte order
   (R,G,B,A per pixel) the LU_IMAGE_A8B8G8R8 texture format expects
   verbatim - see r_context.c's own 2D flush comment for why no byte
   swap is needed. */
static qboolean nv2a_tex_convert_to_rgba32( nv2a_texture_t *tex, rgbdata_t *pic )
{
	if( !pic->buffer )
		return true;

	switch( pic->type )
	{
	case PF_RGBA_32:
		return true;
	case PF_BGRA_32:
	case PF_RGB_24:
	case PF_BGR_24:
		return gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );
	case PF_LUMINANCE:
		nv2a_tex_expand_luminance_inplace( pic );
		return true;
	default:
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: %s: unsupported pixel format %d, not uploaded\n", tex->name, pic->type );
		return false;
	}
}

/* Port of ref/soft's GL_SetTextureDimensions (r_image.c:110-134). */
static void nv2a_tex_set_dimensions( nv2a_texture_t *tex, int width, int height )
{
	const int maxTextureSize = 1024;

	tex->srcWidth = (word)width;
	tex->srcHeight = (word)height;

	while( width > maxTextureSize || height > maxTextureSize )
	{
		width >>= 1;
		height >>= 1;
	}

	tex->width = (word)Q_max( 1, width );
	tex->height = (word)Q_max( 1, height );
}

/* divergences.md #131: a block-compressed pic (a DDS - img_dds.c hands
   PF_DXT1/3/5 through unchanged once R_Init has set IL_DDS_HARDWARE, as
   ref/gl does when the GPU has S3TC, gl_opengl.c:1108-1109) is copied
   to the GPU as is: the NV2A samples DXT natively (nv_regs.h:547-549),
   as the world's own DXT1 pages already prove. ref/gl uploads every mip
   level the file has (GL_UploadTexture); this 2D manager has no mip
   chain anywhere (nv2a_texture.h), so only level 0 goes up. Sized by
   BASE_SIZE, which holds log2 - so both sides must be powers of two;
   the baker only writes POT tiles (UiPacker bakeBackgroundArt), and
   anything else is refused and logged rather than resampled (a DXT
   image cannot go through Image_Process). */
static qboolean nv2a_tex_upload_compressed( nv2a_texture_t *tex, rgbdata_t *pic )
{
	uint32_t format, block_bytes, bytes;
	void *dstBase;

	switch( pic->type )
	{
	case PF_DXT1: format = NV2A_TEX_COLOR_L_DXT1;  block_bytes = 8;  break;
	case PF_DXT3: format = NV2A_TEX_COLOR_L_DXT23; block_bytes = 16; break;
	case PF_DXT5: format = NV2A_TEX_COLOR_L_DXT45; block_bytes = 16; break;
	default:
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: %s: compressed format %d has no NV2A equivalent, not uploaded\n", tex->name, pic->type );
		return false;
	}

	if( nv2a_2d_log2_pot( pic->width ) < 0 || nv2a_2d_log2_pot( pic->height ) < 0 )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: %s: compressed %dx%d is not power-of-two sized, not uploaded\n",
			tex->name, pic->width, pic->height );
		return false;
	}

	bytes = nv2a_2d_dxt_level0_bytes( pic->width, pic->height, block_bytes );
	if( !pic->buffer || pic->size < bytes )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: %s: compressed data too short (%u < %u), not uploaded\n",
			tex->name, (unsigned int)pic->size, bytes );
		return false;
	}

	/* No 1024 halving here (nv2a_tex_set_dimensions): a DXT image cannot
	   be resampled, and 4096 is the NV2A's own limit, checked above. */
	tex->srcWidth = tex->width = pic->width;
	tex->srcHeight = tex->height = pic->height;

	if( !nv2a_tex_alloc_storage( tex, bytes ))
		return false;
	tex->pitch = (( pic->width + 3 ) / 4 ) * block_bytes; /* one row of blocks; ignored by the GPU for DXT, kept for texturelist */
	tex->color_format = format;

	dstBase = tex->has_arena_storage ? (byte *)nv2a_texture_arena_mem + tex->gpu_offset : tex->fallback;
	memcpy( dstBase, pic->buffer, bytes );
	return true;
}

/* D2: shared by GL_LoadTexture, GL_LoadTextureFromBuffer and
   GL_CreateTexture. `pic` must already have been through
   nv2a_tex_process_image. No mip chain, no cube faces, no arrays or 3D
   layers (all out of scope this phase - see nv2a_texture.h).

   divergences.md #131: an image whose every texel has R == G == B is
   stored at 8 bpp (LU_IMAGE_A8, all RGB 255) or 16 bpp (LU_IMAGE_A8Y8)
   instead of 32 - the menu's font atlases are white glyphs with coverage
   in alpha (mainui StbFont.cpp GetCharRGBA), 2.9 MiB of RGBA32 measured
   with the main menu up. The class is decided on every texel
   (nv2a_2d_classify_rgba), so what the GPU samples is unchanged. */
static qboolean nv2a_tex_upload( nv2a_texture_t *tex, rgbdata_t *pic )
{
	uint32_t pitch, y, bpp, format;
	nv2a_texel_class_t texels;
	byte *src, *dst;
	void *dstBase;

	if( pic->buffer && ImageCompressed( pic->type ))
		return nv2a_tex_upload_compressed( tex, pic );

	nv2a_tex_set_dimensions( tex, pic->width, pic->height );

	if( !pic->buffer )
		return nv2a_tex_alloc_storage( tex, 0 ); /* NULL buffer (cl_video.c:283): dimensions only */

	if( !nv2a_tex_convert_to_rgba32( tex, pic ))
		return false;

	if( tex->width != pic->width || tex->height != pic->height )
	{
		if( !gEngfuncs.Image_Process( &pic, tex->width, tex->height, IMAGE_RESAMPLE, 0.0f ))
			return false;
	}

	texels = nv2a_2d_classify_rgba( pic->buffer, (uint32_t)tex->width * tex->height );
	switch( texels )
	{
	case NV2A_TEXELS_WHITE_ALPHA: bpp = 1; format = NV2A_TEX_COLOR_LU_A8;       break;
	case NV2A_TEXELS_GREY_ALPHA:  bpp = 2; format = NV2A_TEX_COLOR_LU_A8Y8;     break;
	default:                      bpp = 4; format = NV2A_TEX_COLOR_LU_A8B8G8R8; break;
	}

	pitch = ALIGN((uint32_t)tex->width * bpp, 64 ); /* LU_IMAGE rows: 64-byte pitch alignment at every depth */
	if( !nv2a_tex_alloc_storage( tex, (size_t)pitch * tex->height ))
		return false;
	tex->pitch = pitch;
	tex->color_format = format;

	if( tex->gpu_size == 0 )
		return true;

	src = pic->buffer;
	dstBase = tex->has_arena_storage ? (byte *)nv2a_texture_arena_mem + tex->gpu_offset : tex->fallback;
	dst = (byte *)dstBase;

	for( y = 0; y < (uint32_t)tex->height; y++ )
	{
		const byte *row = src + (size_t)y * tex->width * 4;

		if( bpp == 1 )
			nv2a_2d_pack_row_a8( dst + (size_t)y * pitch, row, tex->width );
		else if( bpp == 2 )
			nv2a_2d_pack_row_a8y8( dst + (size_t)y * pitch, row, tex->width );
		else
			memcpy( dst + (size_t)y * pitch, row, (size_t)tex->width * 4 );
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* Studio skins: lazy, 16 bpp, cached under a fixed budget             */
/* (divergences.md, lazy studio skins; nv2a_texture.h's constants).    */
/* ------------------------------------------------------------------ */

static unsigned int nv2a_tex_index( const nv2a_texture_t *tex )
{
	return (unsigned int)( tex - nv2a_textures );
}

/* A skin cache offset (chunk index * chunk bytes + offset within it,
   nv2a_skin_cache.h) to its address in that chunk's memory. */
static byte *nv2a_skin_addr( size_t offset )
{
	return (byte *)nv2a_skin_chunk_mem[offset / NV2A_STUDIO_SKIN_CHUNK_BYTES] + offset % NV2A_STUDIO_SKIN_CHUNK_BYTES;
}

/* nv2a_skin_cache's chunk backing: one physically contiguous,
   write-combined allocation per chunk, same flags and ceiling as every
   other texture allocation here. */
static int nv2a_skin_grow( void *ctx, uint32_t index )
{
	(void)ctx;
	if( index >= NV2A_SKIN_MAX_CHUNKS )
		return 0;
	nv2a_skin_chunk_mem[index] = MmAllocateContiguousMemoryEx( NV2A_STUDIO_SKIN_CHUNK_BYTES, 0, NV2A_TEXTURE_MAXRAM, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE );
	return nv2a_skin_chunk_mem[index] != NULL;
}

static void nv2a_skin_release( void *ctx, uint32_t index )
{
	(void)ctx;
	if( index < NV2A_SKIN_MAX_CHUNKS && nv2a_skin_chunk_mem[index] )
	{
		MmFreeContiguousMemory( nv2a_skin_chunk_mem[index] );
		nv2a_skin_chunk_mem[index] = NULL;
	}
}

static byte *nv2a_tex_skin_base( const nv2a_texture_t *tex )
{
	return nv2a_skin_addr( tex->gpu_offset );
}

static void nv2a_tex_clear_skin_storage( nv2a_texture_t *tex )
{
	tex->has_skin_storage = false;
	tex->gpu_offset = 0;
	tex->gpu_size = 0;
	tex->pitch = 0;
	tex->color_format = 0;
}

/* nv2a_skin_cache_acquire's eviction callback: the victim's slot goes
   back to "not uploaded" (name, size and source kept) before the cache
   releases its bytes. Only skins at least NV2A_STUDIO_SKIN_EVICT_MIN_AGE
   frames old are ever passed here, so the GPU is not reading them. */
static void nv2a_tex_skin_evicted( void *ctx, uint32_t id )
{
	(void)ctx;
	if( id >= NV2A_MAX_TEXTURES )
		return;
	nv2a_tex_clear_skin_storage( &nv2a_textures[id] );

	/* Still the bound texture (nothing re-bound it for two frames): a
	   draw appended without a new GL_Bind would otherwise resolve a slot
	   with no storage and be dropped (nv2a_2d_resolve_batch_key) - bind
	   the same white placeholder a not-yet-uploaded skin draws with
	   instead; the next GL_Bind of this skin re-uploads it. */
	if( nv2a_bound_texture_tmu0 == id )
		nv2a_bound_texture_tmu0 = nv2a_texnum_white;
}

/* Turns a studio skin slot back into a plain one with no storage: its
   cache entry is detached and the bytes released at the next safe point
   (D4 - the skin may have been drawn this frame), and its source record
   freed. Used by GL_FreeTexture, and by the update paths should the
   engine ever write new pixels into a skin's texnum (it does not today:
   cl_remap.c updates its own per-entity copies, never the model's slot),
   so they then behave exactly as for any other texture. */
static void nv2a_tex_drop_skin( nv2a_texture_t *tex )
{
	size_t offset;

	if( !tex->skin )
		return;

	if( tex->has_skin_storage && nv2a_skin_cache_detach( nv2a_skin_cache, nv2a_tex_index( tex ), &offset ))
	{
		if( nv2a_frame_is_open())
			nv2a_tex_queue_free_skin( offset );
		else
			nv2a_skin_cache_free_offset( nv2a_skin_cache, offset );
	}
	nv2a_tex_clear_skin_storage( tex );

	free( tex->skin );
	tex->skin = NULL;
}

static void nv2a_tex_skin_report_miss( void )
{
	uint32_t frame = nv2a_current_frame_number();

	nv2a_skin_missed_binds++;
	if( nv2a_skin_logged_once && (uint32_t)( frame - nv2a_skin_log_frame ) < NV2A_SKIN_LOG_INTERVAL_FRAMES )
		return;

	gEngfuncs.Con_DPrintf( "ref_nv2a: studio skin cache full (%u of %u KiB, %u evictions), %u skin bind(s) drew untextured\n",
		(unsigned int)( nv2a_skin_cache_backed( nv2a_skin_cache ) >> 10 ),
		(unsigned int)( nv2a_skin_cache_capacity( nv2a_skin_cache ) >> 10 ),
		nv2a_skin_cache_eviction_count( nv2a_skin_cache ), nv2a_skin_missed_binds );
	nv2a_skin_logged_once = true;
	nv2a_skin_log_frame = frame;
	nv2a_skin_missed_binds = 0;
}

/* Reads the skin's raw 8-bit data (width*height indices, then the
   768-byte palette) back from its model file into `buf`, which must hold
   src->width * src->height + 768 bytes. False on any failure, including
   a file whose length is no longer the one registered. */
static qboolean nv2a_tex_skin_read_raw( const nv2a_skin_source_t *src, byte *buf )
{
	size_t data_bytes = (size_t)src->width * src->height + 768;
	file_t *f;
	qboolean ok;

	f = gEngfuncs.fsapi->Open( src->path, "rb", false );
	if( !f )
		return false;

	ok = gEngfuncs.fsapi->FileLength( f ) == (fs_offset_t)src->file_length
		&& gEngfuncs.fsapi->Seek( f, (fs_offset_t)src->pixel_offset, SEEK_SET ) == 0
		&& gEngfuncs.fsapi->Read( f, buf, data_bytes ) == (fs_offset_t)data_bytes;
	gEngfuncs.fsapi->Close( f );
	return ok;
}

/* The rare path: a skin larger than 1024 on a side, which
   nv2a_tex_set_dimensions shrank, so it needs the engine's resampler.
   Decodes through the engine's own studio image loader - the same
   FS_LoadImage(texname, mstudiotexture_t + pixels + palette) call
   R_StudioLoadTexture made at precache (gl_studio.c R_StudioLoadTexture;
   Image_LoadMDL reads only flags/width/height from the header,
   img_wad.c:195-200, and the pixels through Image_SetMDLPointer) - then
   nv2a_tex_upload's own RGBA32 + resample steps, and packs to `format`.
   Unlike the direct path this allocates from the engine's image pool,
   whose exhaustion is a Sys_Error (zone.c); no retail skin takes it.
   `raw` is the nv2a_tex_skin_read_raw buffer, preceded by room for the
   header. */
static qboolean nv2a_tex_skin_decode_resampled( nv2a_texture_t *tex, byte *raw_with_header, uint32_t format, byte *dst, uint32_t pitch )
{
	const nv2a_skin_source_t *src = tex->skin;
	size_t total = sizeof( mstudiotexture_t ) + (size_t)src->width * src->height + 768;
	mstudiotexture_t *header = (mstudiotexture_t *)raw_with_header;
	rgbdata_t *pic;
	uint32_t y;

	memset( header, 0, sizeof( *header ));
	header->flags = src->studio_flags;
	header->width = src->width;
	header->height = src->height;

	/* GL_LoadTexture's own sequence: force flags, then load. No
	   TF_KEEP_SOURCE skin ever gets here (R_StudioLoadTexture keeps those
	   on the eager path), so no IL_KEEP_8BIT. */
	gEngfuncs.Image_SetForceFlags( 0 );
	gEngfuncs.Image_SetMDLPointer( raw_with_header + sizeof( mstudiotexture_t ));
	pic = gEngfuncs.FS_LoadImage( tex->name, raw_with_header, total );
	if( !pic )
		return false;

	nv2a_tex_process_image( tex, pic );
	if( !nv2a_tex_convert_to_rgba32( tex, pic )
		|| (( pic->width != tex->width || pic->height != tex->height )
			&& !gEngfuncs.Image_Process( &pic, tex->width, tex->height, IMAGE_RESAMPLE, 0.0f ))
		|| !pic->buffer || pic->type != PF_RGBA_32 )
	{
		gEngfuncs.FS_FreeImage( pic );
		return false;
	}

	for( y = 0; y < (uint32_t)tex->height; y++ )
	{
		const uint8_t *row = pic->buffer + (size_t)y * tex->width * 4;
		uint16_t *out = (uint16_t *)( dst + (size_t)y * pitch );

		if( format == NV2A_TEX_COLOR_LU_A1R5G5B5 )
			nv2a_pack_a1r5g5b5( row, out, tex->width );
		else
			nv2a_pack_r5g6b5( row, out, tex->width );
	}

	gEngfuncs.FS_FreeImage( pic );
	return true;
}

/* Uploads one studio skin into the cache at 16 bpp. On anything but
   success the slot is left with no storage; `permanent` says whether it
   is worth trying again on a later bind.

   The normal path decodes the 8-bit data itself, through a 256-entry
   16 bpp table (nv2a_skin_build_lut) that reproduces Image_LoadMDL's
   palette exactly: LUMP_TEXGAMMA through the engine's own texture gamma
   table for an ordinary skin, LUMP_MASKED (raw colours, index 255
   transparent) for STUDIO_NF_MASKED (img_wad.c:212-221,
   img_utils.c:284-310). The only allocation is one malloc of the raw
   bytes - never the engine's image pool, whose exhaustion is a Sys_Error
   (zone.c) that a mid-game upload on a tight map must not risk (NV2A
   specialist review) - and there is no RGBA intermediate. What
   nv2a_tex_process_image would do to such a skin at precache is
   flag bookkeeping only (TF_HAS_ALPHA, set at registration). */
static qboolean nv2a_tex_upload_skin( nv2a_texture_t *tex, qboolean *permanent )
{
	nv2a_skin_source_t *src = tex->skin;
	unsigned int texnum = nv2a_tex_index( tex );
	qboolean masked = FBitSet( src->studio_flags, NV2A_STUDIO_NF_MASKED ) ? true : false;
	qboolean resampled = ( tex->width != src->width || tex->height != src->height );
	size_t header_room = resampled ? sizeof( mstudiotexture_t ) : 0;
	uint32_t pitch, y, format;
	nv2a_skin_status_t status;
	size_t offset;
	byte *buf, *dst;
	qboolean ok;

	*permanent = false;

	pitch = ALIGN((uint32_t)tex->width * 2, 64 ); /* same 64-byte LU_IMAGE row alignment as nv2a_tex_upload's RGBA32 rows */

	/* A1R5G5B5 keeps a masked skin's one alpha bit (Image_LoadMDL sets
	   IMAGE_HAS_ALPHA|IMAGE_ONEBIT_ALPHA only for STUDIO_NF_MASKED,
	   img_wad.c:216-221); every other skin is opaque, and R5G6B5 gives
	   green the spare bit instead. */
	format = masked ? NV2A_TEX_COLOR_LU_A1R5G5B5 : NV2A_TEX_COLOR_LU_R5G6B5;

	/* The raw bytes first, so a malloc failure costs no eviction.
	   malloc, not a zone pool: failure here is survivable (retry later). */
	buf = (byte *)malloc( header_room + (size_t)src->width * src->height + 768 );
	if( !buf )
	{
		nv2a_tex_skin_report_miss();
		return false;
	}

	status = nv2a_skin_cache_acquire( nv2a_skin_cache, texnum, (size_t)pitch * tex->height,
		nv2a_current_frame_number(), &offset, nv2a_tex_skin_evicted, NULL );

	switch( status )
	{
	case NV2A_SKIN_OK:
		break;
	case NV2A_SKIN_FRAME_CAP:
		free( buf );
		return false; /* expected at a scene change; the next frame gets it */
	case NV2A_SKIN_NO_ROOM:
		free( buf );
		nv2a_tex_skin_report_miss();
		return false;
	default: /* TOO_BIG / BAD_ID: can never succeed */
		free( buf );
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: studio skin %s (%ux%u) is larger than one %u KiB skin cache chunk, drawn untextured\n",
			tex->name, tex->width, tex->height, (unsigned int)( nv2a_skin_cache_chunk_bytes( nv2a_skin_cache ) >> 10 ));
		*permanent = true;
		return false;
	}

	/* The destination is a freshly acquired range: either never used, or
	   an evicted skin's that has not been drawn for at least
	   NV2A_STUDIO_SKIN_EVICT_MIN_AGE frames - no GPU hazard in writing it
	   now, mid-frame. Sequential 16-bit stores, no read-back: the right
	   pattern for write-combined memory. */
	dst = nv2a_skin_addr( offset );

	ok = nv2a_tex_skin_read_raw( src, buf + header_room );
	if( ok && resampled )
	{
		ok = nv2a_tex_skin_decode_resampled( tex, buf, format, dst, pitch );
	}
	else if( ok )
	{
		const byte *indices = buf;
		const byte *palette = buf + (size_t)src->width * src->height;
		const byte *texgamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE )
			? NULL : (const byte *)ENGINE_GET_PARM( PARM_GET_TEXGAMMATABLE_PTR ); /* TextureToGamma, gamma.c:175-181 / gl_local.h:655-658 */
		uint16_t lut[256];

		nv2a_skin_build_lut( palette, texgamma, masked, lut );
		for( y = 0; y < (uint32_t)tex->height; y++ )
			nv2a_skin_expand_indexed( indices + (size_t)y * src->width, lut, (uint16_t *)( dst + (size_t)y * pitch ), tex->width );
	}
	free( buf );

	if( !ok )
	{
		/* The victims evicted for this range stay evicted: they re-upload
		   on their next bind like any evicted skin (bounded, rare). */
		nv2a_skin_cache_abandon( nv2a_skin_cache, texnum );
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: could not re-read studio skin %s from %s, drawn untextured\n",
			tex->name, src->path );
		*permanent = true;
		return false;
	}

	tex->has_skin_storage = true;
	tex->has_arena_storage = false;
	tex->fallback = NULL;
	tex->gpu_offset = offset;
	tex->gpu_size = (size_t)pitch * tex->height;
	tex->pitch = pitch;
	tex->color_format = format;

	SetBits( tex->flags, TF_IMG_UPLOADED );
	return true;
}

/* GL_Bind's studio skin step: returns the texnum to actually bind -
   `texnum` itself once its skin is resident (marked used this frame),
   otherwise the white texture as the placeholder for this bind. White,
   not the default checkerboard: the mesh still gets its lighting, so a
   skin arriving a frame or two late reads as a brief flat shade rather
   than a flash of debug pattern. */
static unsigned int nv2a_tex_resolve_skin( unsigned int texnum )
{
	nv2a_texture_t *tex = &nv2a_textures[texnum];
	qboolean permanent;
	qboolean uploaded;

	if( tex->has_skin_storage )
	{
		nv2a_skin_cache_touch( nv2a_skin_cache, texnum, nv2a_current_frame_number());
		return texnum;
	}

	if( !nv2a_skin_cache || tex->skin->failed )
		return nv2a_texnum_white;

	/* M2 span k_skin_read (#141): the lazy read off the disc and the
	   decode into the skin cache. */
	XBOX_FT_SPAN( XBOX_FT_S_SKIN_READ, uploaded = nv2a_tex_upload_skin( tex, &permanent ));
	if( uploaded )
		return texnum;

	if( permanent )
		tex->skin->failed = true;
	return nv2a_texnum_white;
}

int nv2a_tex_register_studio_skin( const char *texname, texFlags_t flags, const char *path, uint32_t file_length,
	uint32_t pixel_offset, int studio_flags, int width, int height )
{
	nv2a_skin_source_t *src;
	nv2a_texture_t *tex;
	uint64_t data_end;

	if( !nv2a_tex_check_name( texname ) || COM_StringEmptyOrNULL( path ))
		return 0;

	if(( tex = nv2a_tex_for_name( texname )))
		return (int)nv2a_tex_index( tex ); /* GL_LoadTexture's own early-out for an already loaded name */

	/* Everything the upload will read must be inside the file as it is
	   now; Image_LoadMDL's own size check (img_wad.c:207-209) is the
	   same w*h+768. */
	data_end = (uint64_t)pixel_offset + (uint64_t)width * (uint64_t)height + 768u;
	if( width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF
		|| data_end > file_length || Q_strlen( path ) >= (int)sizeof( src->path ))
		return 0;

	src = (nv2a_skin_source_t *)calloc( 1, sizeof( *src ));
	if( !src )
		return 0;

	Q_strncpy( src->path, path, sizeof( src->path ));
	src->file_length = file_length;
	src->pixel_offset = pixel_offset;
	src->studio_flags = studio_flags;
	src->width = (word)width;
	src->height = (word)height;

	tex = nv2a_tex_alloc_slot( texname, flags );
	tex->skin = src;
	nv2a_tex_set_dimensions( tex, width, height );

	/* What nv2a_tex_process_image would set from Image_LoadMDL's
	   IMAGE_HAS_ALPHA (img_wad.c:216-221), so the slot's flags already
	   read the same before the first upload as after it. */
	if( FBitSet( studio_flags, NV2A_STUDIO_NF_MASKED ))
		SetBits( tex->flags, TF_HAS_ALPHA );

	return (int)nv2a_tex_index( tex );
}

void nv2a_tex_mark_used( unsigned int texnum )
{
	nv2a_texture_t *tex;

	if( texnum >= NV2A_MAX_TEXTURES || !nv2a_textures )
		return;

	tex = &nv2a_textures[texnum];
	tex->last_used_frame = nv2a_current_frame_number();
	if( tex->has_skin_storage )
		nv2a_skin_cache_touch( nv2a_skin_cache, texnum, tex->last_used_frame );
}

void nv2a_tex_flush_skin_cache( void )
{
	/* Only between frames: R_EndFrame's pb_busy spin (r_context.c) has
	   then retired every draw that could read a skin, which is what makes
	   draining the D4 queue and releasing chunks safe here. Inside a frame
	   this is a no-op rather than a hazard. */
	if( !nv2a_skin_cache || nv2a_frame_is_open())
		return;

	nv2a_tex_begin_frame(); /* skin bytes waiting in the D4 queue keep their chunk backed */
	nv2a_skin_cache_flush( nv2a_skin_cache, nv2a_tex_skin_evicted, NULL );
}

/* ------------------------------------------------------------------ */
/* Entry points (D3).                                                  */
/* ------------------------------------------------------------------ */

int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	nv2a_texture_t *tex;
	uint picFlags = 0;
	rgbdata_t *pic;

	if( !nv2a_tex_check_name( name ))
		return 0;

	if(( tex = nv2a_tex_for_name( name )))
		return (int)( tex - nv2a_textures );

	if( FBitSet( flags, TF_NOFLIP_TGA ))
		SetBits( picFlags, IL_DONTFLIP_TGA );
	if( FBitSet( flags, TF_KEEP_SOURCE ) && !FBitSet( flags, TF_EXPAND_SOURCE ))
		SetBits( picFlags, IL_KEEP_8BIT );

	gEngfuncs.Image_SetForceFlags( picFlags );

	pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic )
		return 0;

	tex = nv2a_tex_alloc_slot( name, (texFlags_t)flags );
	nv2a_tex_process_image( tex, pic );

	if( !nv2a_tex_upload( tex, pic ))
	{
		nv2a_tex_fail_slot( tex );
		gEngfuncs.FS_FreeImage( pic );
		return 0;
	}

	/* QA round 2, finding 2: mirrors ref/gl's own GL_UploadTexture, which
	   sets this flag right after its own upload succeeds (gl_image.c:
	   1095, "done"). Without it, nv2a_tex_process_image's TF_KEEP_SOURCE
	   guard below never engages, so every later GL_LoadTextureFromBuffer
	   update on this same slot re-copies `tex->original` and leaks the
	   previous copy - unreachable from this function (it never revisits
	   an already-loaded name, see the early-out above), but this is the
	   one place a fresh slot's first successful upload completes. */
	SetBits( tex->flags, TF_IMG_UPLOADED );

	gEngfuncs.FS_FreeImage( pic );

	return (int)( tex - nv2a_textures );
}

int GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	nv2a_texture_t *tex;
	qboolean old_has_arena;
	size_t old_offset;
	void *old_fallback;

	if( !nv2a_tex_check_name( name ))
		return 0;

	tex = nv2a_tex_for_name( name );
	if( tex && !update )
		return (int)( tex - nv2a_textures );

	if( !pic )
		return 0;

	if( update )
	{
		if( !tex )
			gEngfuncs.Host_Error( "%s: couldn't find texture %s for update\n", __func__, name );
		nv2a_tex_drop_skin( tex ); /* a studio skin written to becomes a plain texture (nv2a_tex_drop_skin) */
		SetBits( tex->flags, flags );
	}
	else
		tex = nv2a_tex_alloc_slot( name, flags );

	/* QA round 1, finding 2: capture the slot's OWN storage before the
	   upload below touches it. nv2a_tex_upload() calls
	   nv2a_tex_alloc_storage(), which unconditionally overwrites
	   has_arena_storage/gpu_offset/gpu_size/fallback for the NEW
	   allocation - on an update (e.g. a player colour remap reusing the
	   same texnum, cl_remap's own caller) that would silently orphan the
	   OLD allocation with no path back to it: never freed via
	   nv2a_arena_free(), never MmFreeContiguousMemory()'d, permanently
	   consuming arena bytes (or a standalone allocation) on every single
	   update, worse still on an upload failure further below (which
	   memset()s the whole slot, losing even the pointer). A freshly
	   allocated slot's fields are all zero, so releasing them below is a
	   no-op in the non-update path (both helpers early-out when neither
	   is set). Mirrors GL_UpdateTexture's own size-change branch. */
	old_has_arena = tex->has_arena_storage;
	old_offset = tex->gpu_offset;
	old_fallback = tex->fallback;

	nv2a_tex_process_image( tex, pic );

	if( !nv2a_tex_upload( tex, pic ))
	{
		nv2a_tex_fail_slot( tex );

		if( nv2a_frame_is_open())
			nv2a_tex_queue_free( old_has_arena, old_offset, old_fallback );
		else
			nv2a_tex_free_storage_now( old_has_arena, old_offset, old_fallback );

		return 0;
	}

	/* QA round 2, finding 2: the real bug this closes. Every repeat call
	   here IS reachable in practice (update == true is exactly the
	   player-colour-remap path re-using the same texnum) and, before this
	   flag was ever actually set anywhere, re-entered the TF_KEEP_SOURCE
	   branch in nv2a_tex_process_image() on EVERY update, replacing
	   tex->original with a fresh FS_CopyImage() and leaking the previous
	   one - same class of bug as QA round 1's storage leak, in the
	   `original` field instead of GPU storage. Setting it here (mirrors
	   ref/gl's GL_UploadTexture, gl_image.c:1095) makes that guard
	   actually guard: the first successful upload takes the copy, every
	   update after retains it untouched - matching the reference's own
	   "only on first upload" rule (gl_image.c:1144-1145) exactly, rather
	   than freeing and re-copying it. */
	SetBits( tex->flags, TF_IMG_UPLOADED );

	if( nv2a_frame_is_open())
		nv2a_tex_queue_free( old_has_arena, old_offset, old_fallback );
	else
		nv2a_tex_free_storage_now( old_has_arena, old_offset, old_fallback );

	return (int)( tex - nv2a_textures );
}

int GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	rgbdata_t empty;
	int texnum;

	memset( &empty, 0, sizeof( empty ));
	empty.width = (word)width;
	empty.height = (word)height;
	empty.type = PF_RGBA_32;
	empty.size = (size_t)width * height * 4;
	empty.buffer = (byte *)buffer;

	ClearBits( flags, TF_TEXTURE_3D );

	if( !FBitSet( flags, TF_LUMINANCE ) && !FBitSet( flags, TF_ALPHACONTRAST ))
		SetBits( empty.flags, IMAGE_HAS_COLOR );
	if( FBitSet( flags, TF_HAS_ALPHA ))
		SetBits( empty.flags, IMAGE_HAS_ALPHA );

	if( FBitSet( flags, TF_CUBEMAP ))
		return 0; /* out of scope this phase */

	texnum = GL_LoadTextureFromBuffer( name, &empty, flags, false );

	/* D1: record the built-ins, as ref/soft's own GL_CreateTexture does
	   (r_image.c:731-740) - the engine creates these five right after
	   R_Init returns (ref_common.c:494-544,582-590). */
	if( !Q_strcmp( name, REF_DEFAULT_TEXTURE ))
		nv2a_texnum_default = (unsigned int)texnum;
	else if( !Q_strcmp( name, REF_PARTICLE_TEXTURE ))
		nv2a_texnum_particle = (unsigned int)texnum;
	else if( !Q_strcmp( name, REF_WHITE_TEXTURE ))
		nv2a_texnum_white = (unsigned int)texnum;
	else if( !Q_strcmp( name, REF_GRAY_TEXTURE ))
		nv2a_texnum_gray = (unsigned int)texnum;
	else if( !Q_strcmp( name, REF_BLACK_TEXTURE ))
		nv2a_texnum_black = (unsigned int)texnum;

	return texnum;
}

int GL_FindTexture( const char *name )
{
	nv2a_texture_t *tex;

	if( !nv2a_tex_check_name( name ))
		return 0;

	tex = nv2a_tex_for_name( name );
	return tex ? (int)( tex - nv2a_textures ) : 0;
}

void GL_FreeTexture( unsigned int texnum )
{
	nv2a_texture_t *tex;

	if( texnum == 0 || texnum >= NV2A_MAX_TEXTURES )
		return;

	tex = &nv2a_textures[texnum];
	if( !tex->name[0] )
		return; /* already freed */

	nv2a_tex_unlink_hash( tex );

	if( tex->original )
	{
		gEngfuncs.FS_FreeImage( tex->original );
		tex->original = NULL;
	}

	/* Studio skin: detach from the cache (bytes deferred like below) and
	   free the source record - nothing of it outlives the slot. */
	nv2a_tex_drop_skin( tex );

	/* D4: metadata clears immediately either way; only the GPU storage
	   itself may need to wait for a safe point. */
	if( nv2a_frame_is_open())
		nv2a_tex_queue_free( tex->has_arena_storage, tex->gpu_offset, tex->fallback );
	else
		nv2a_tex_free_storage_now( tex->has_arena_storage, tex->gpu_offset, tex->fallback );

	memset( tex, 0, sizeof( *tex ));
}

const char *GL_TextureName( unsigned int texnum )
{
	return nv2a_tex_get( texnum )->name;
}

const byte *GL_TextureData( unsigned int texnum )
{
	const nv2a_texture_t *tex = nv2a_tex_get( texnum );
	return tex->original ? tex->original->buffer : NULL;
}

const byte *R_GetTextureOriginalBuffer( unsigned int idx )
{
	const nv2a_texture_t *tex = nv2a_tex_get( idx );

	if( !tex->original || !tex->original->buffer )
		return NULL;

	return tex->original->buffer;
}

void R_OverrideTextureSourceSize( unsigned int texnum, unsigned int srcWidth, unsigned int srcHeight )
{
	nv2a_texture_t *tex = nv2a_tex_get_mutable( texnum );

	tex->srcWidth = (word)srcWidth;
	tex->srcHeight = (word)srcHeight;
}

void GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	nv2a_texture_t *tex;
	rgbdata_t wrapper, *owned;
	qboolean resample;

	switch( fmt )
	{
	case PF_RGBA_32:
	case PF_BGRA_32:
	case PF_RGB_24:
	case PF_BGR_24:
	case PF_LUMINANCE:
		break;
	default:
		gEngfuncs.Con_DPrintf( S_ERROR "%s: unsupported pixel format %i\n", __func__, fmt );
		return;
	}

	if( texnum <= 0 || texnum >= NV2A_MAX_TEXTURES )
		return;

	tex = &nv2a_textures[texnum];
	if( !tex->name[0] )
		return;

	nv2a_tex_drop_skin( tex ); /* a studio skin written to becomes a plain texture (nv2a_tex_drop_skin) */

	memset( &wrapper, 0, sizeof( wrapper ));
	wrapper.width = (word)cols;
	wrapper.height = (word)rows;
	wrapper.type = fmt;
	wrapper.size = gEngfuncs.Image_CalcImageSize( fmt, cols, rows, 1 );
	wrapper.buffer = (byte *)buffer;

	/* A REAL, owned copy first (D4) - the caller's own `buffer` is
	   neither ours to free nor guaranteed to survive Image_Process's
	   own reallocation. */
	owned = gEngfuncs.FS_CopyImage( &wrapper );
	if( !owned )
		return;

	resample = ( cols != width || rows != height );

	if( fmt == PF_LUMINANCE )
		nv2a_tex_expand_luminance_inplace( owned );
	else if( fmt != PF_RGBA_32 )
		gEngfuncs.Image_Process( &owned, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

	if( resample )
		gEngfuncs.Image_Process( &owned, width, height, IMAGE_RESAMPLE, 0.0f );

	cols = owned->width;
	rows = owned->height;

	/* QA round 2, finding 1: a storage-less slot (gpu_size == 0, e.g. the
	   dimensions-only placeholder GL_CreateTexture(..., NULL, ...) makes
	   for engine/client/cl_video.c's own 64x64 *cintexture) can still
	   match "cols == tex->width && rows == tex->height" - it has real
	   width/height, just no allocation - and would take the in-place
	   branch below straight into a NULL `fallback` (has_arena_storage is
	   false too), an unconditional NULL write. Route it through the
	   size-change branch instead: alloc_storage(0) never ran for it, so
	   there is truly no old storage to defer-free (both release helpers
	   already no-op when neither has_arena_storage nor fallback is set),
	   and that branch allocates real storage before copying into it. */
	/* divergences.md #131: in place only onto 32 bpp storage - a slot the
	   upload stored at 8/16 bpp or compressed has a different row size, so
	   it takes the reallocating branch below, which stores RGBA32. */
	if( cols == tex->width && rows == tex->height && tex->gpu_size != 0
		&& ( tex->color_format == 0 || tex->color_format == NV2A_TEX_COLOR_LU_A8B8G8R8 ))
	{
		void *dstBase;
		byte *dst, *src;
		uint32_t y;

		/* Same size: write in place - unless the GPU may still be mid-
		   batch reading this exact texture from earlier THIS frame. */
		if( tex->last_used_frame == nv2a_current_frame_number())
			nv2a_2d_flush_and_wait();

		dstBase = tex->has_arena_storage ? (byte *)nv2a_texture_arena_mem + tex->gpu_offset : tex->fallback;
		dst = (byte *)dstBase;
		src = owned->buffer;

		for( y = 0; y < (uint32_t)tex->height; y++ )
			memcpy( dst + (size_t)y * tex->pitch, src + (size_t)y * tex->width * 4, (size_t)tex->width * 4 );
	}
	else
	{
		/* Size change: new storage, defer-free the old (D4). */
		qboolean old_has_arena = tex->has_arena_storage;
		size_t old_offset = tex->gpu_offset;
		void *old_fallback = tex->fallback;
		uint32_t pitch;
		void *dstBase;
		byte *dst, *src;
		uint32_t y;
		/* Restored whole on an allocation failure below, so the slot keeps
		   describing the storage it still owns (size, pitch and format
		   together - a DXT format beside a new NPOT size would otherwise
		   survive in a slot the 2D flush can still resolve). */
		const nv2a_texture_t before = *tex;

		tex->srcWidth = (word)cols;
		tex->srcHeight = (word)rows;
		tex->width = (word)cols;
		tex->height = (word)rows;
		pitch = ALIGN((uint32_t)tex->width * 4, 64 );

		if( !nv2a_tex_alloc_storage( tex, (size_t)pitch * tex->height ))
		{
			gEngfuncs.Con_Printf( S_ERROR "%s: %s: out of texture memory on update\n", __func__, tex->name );
			*tex = before;
			gEngfuncs.FS_FreeImage( owned );
			return;
		}
		tex->pitch = pitch;
		tex->color_format = NV2A_TEX_COLOR_LU_A8B8G8R8;

		dstBase = tex->has_arena_storage ? (byte *)nv2a_texture_arena_mem + tex->gpu_offset : tex->fallback;
		dst = (byte *)dstBase;
		src = owned->buffer;

		for( y = 0; y < (uint32_t)tex->height; y++ )
			memcpy( dst + (size_t)y * pitch, src + (size_t)y * tex->width * 4, (size_t)tex->width * 4 );

		if( nv2a_frame_is_open())
			nv2a_tex_queue_free( old_has_arena, old_offset, old_fallback );
		else
			nv2a_tex_free_storage_now( old_has_arena, old_offset, old_fallback );
	}

	gEngfuncs.FS_FreeImage( owned );
}

void GL_Bind( int tmu, unsigned int texnum )
{
	nv2a_texture_t *tex;

	if( texnum == 0 || texnum >= NV2A_MAX_TEXTURES )
	{
		if( texnum != 0 )
			gEngfuncs.Con_DPrintf( S_ERROR "%s: invalid texturenum %d\n", __func__, texnum );
		texnum = nv2a_texnum_default;
	}

	/* Studio skin: uploaded on first bind, placeholder (white) while it
	   is not resident - see nv2a_tex_resolve_skin. */
	if( nv2a_textures[texnum].skin )
		texnum = nv2a_tex_resolve_skin( texnum );

	tex = &nv2a_textures[texnum];

	if( tex->gpu_size == 0 )
	{
		/* A slot with no GPU storage also binds the default - a named
		   divergence (D6): OpenGL tolerates binding an "incomplete"
		   texture object; NV2A's texture stage registers need a real
		   offset/pitch/size to push, and there is nothing sensible to
		   substitute here instead. */
		texnum = nv2a_texnum_default;
		tex = &nv2a_textures[texnum];
	}

	if( tmu == 0 )
	{
		nv2a_bound_texture_tmu0 = texnum;
		tex->last_used_frame = nv2a_current_frame_number();
	}
}

unsigned int nv2a_tex_current( void )
{
	return nv2a_bound_texture_tmu0;
}

qboolean nv2a_tex_gpu_params( unsigned int texnum, uint32_t *phys_offset, uint32_t *pitch,
	uint32_t *width, uint32_t *height, qboolean *bilinear, uint32_t *color_format )
{
	const nv2a_texture_t *tex = nv2a_tex_get( texnum );

	if( tex->gpu_size == 0 )
		return false;

	/* A compressed texture is sized only by log2 BASE_SIZE fields (the 2D
	   flush): nv2a_tex_upload_compressed only stores power-of-two ones, so
	   this never fires - but a -1 must never reach that register field. */
	if( NV2A_TEX_COLOR_IS_COMPRESSED( tex->color_format )
		&& ( nv2a_2d_log2_pot( tex->width ) < 0 || nv2a_2d_log2_pot( tex->height ) < 0 ))
		return false;

	if( phys_offset )
	{
		uint32_t base = tex->has_skin_storage
			? (uint32_t)(uintptr_t)nv2a_tex_skin_base( tex )
			: tex->has_arena_storage
				? (uint32_t)((byte *)nv2a_texture_arena_mem + tex->gpu_offset)
				: (uint32_t)(uintptr_t)tex->fallback;
		*phys_offset = base & 0x03ffffff;
	}
	if( pitch )     *pitch = tex->pitch;
	if( width )     *width = tex->width;
	if( height )    *height = tex->height;
	if( bilinear )  *bilinear = nv2a_texture_filtering_enabled( tex );
	if( color_format ) *color_format = tex->color_format ? tex->color_format : NV2A_TEX_COLOR_LU_A8B8G8R8;

	return true;
}

void nv2a_tex_coord_scale( const nv2a_texture_t *tex, float *scale_s, float *scale_t )
{
	if( !tex || NV2A_TEX_COLOR_IS_COMPRESSED( tex->color_format ))
	{
		*scale_s = 1.0f;
		*scale_t = 1.0f;
		return;
	}

	*scale_s = (float)tex->width;
	*scale_t = (float)tex->height;
}

/* ------------------------------------------------------------------ */
/* D5 helper.                                                          */
/* ------------------------------------------------------------------ */

/* Port of GL_TextureFilteringEnabled (ref/gl/gl_image.c:72-90). No
   TF_DEPTHMAP branch (this manager never uploads a depth map) and no
   TF_NOMIPMAP/numMips branch (D2: no mip chain exists this phase, so
   that branch of the reference is unconditionally taken here). */
qboolean nv2a_texture_filtering_enabled( const nv2a_texture_t *tex )
{
	if( FBitSet( tex->flags, TF_NEAREST ))
		return false;

	if( FBitSet( tex->flags, TF_ATLAS_PAGE ))
		return true; /* the nv2a lightmap stage is always bilinear, divergences.md #102 */

	if( FBitSet( tex->flags, TF_ALLOW_NEAREST ))
		return !nv2a_global_texture_nearest();

	return true;
}

/* ------------------------------------------------------------------ */
/* D7.                                                                 */
/* ------------------------------------------------------------------ */

size_t nv2a_tex_memory( void )
{
	size_t total = 0;
	uint32_t i;

	for( i = 0; i < nv2a_textures_count; i++ )
		total += nv2a_textures[i].gpu_size;

	return total;
}

void nv2a_tex_arena_stats( size_t *used, size_t *free_bytes, size_t *largest_free, uint32_t *fallback_count )
{
	if( used )           *used = nv2a_arena_used( nv2a_texture_arena );
	if( free_bytes )     *free_bytes = nv2a_arena_free_bytes( nv2a_texture_arena );
	if( largest_free )   *largest_free = nv2a_arena_largest_free( nv2a_texture_arena );
	if( fallback_count ) *fallback_count = nv2a_fallback_count;
}

void R_TextureList_f( void )
{
	uint32_t i, count = 0;
	size_t bytes = 0, used, free_bytes, largest_free;
	uint32_t fallback = 0;

	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( " -id-   -w-  -h-     -size- -wrap- -name--------\n" );

	for( i = 0; i < nv2a_textures_count; i++ )
	{
		const nv2a_texture_t *tex = &nv2a_textures[i];

		if( !tex->name[0] )
			continue;

		bytes += tex->gpu_size;
		count++;

		gEngfuncs.Con_Printf( "%4i: ", i );
		gEngfuncs.Con_Printf( "%4i %4i ", tex->width, tex->height );
		gEngfuncs.Con_Printf( "%12s ", Q_memprint((float)tex->gpu_size ));
		gEngfuncs.Con_Printf( FBitSet( tex->flags, TF_CLAMP ) ? "clamp  " : "repeat " );
		gEngfuncs.Con_Printf( "  %s\n", tex->name );
	}

	gEngfuncs.Con_Printf( "---------------------------------------------------------\n" );
	gEngfuncs.Con_Printf( "%i total textures, %s total memory used\n", count, Q_memprint((float)bytes ));

	nv2a_tex_arena_stats( &used, &free_bytes, &largest_free, &fallback );
	gEngfuncs.Con_Printf( "arena: %s used, %s free, %s largest free block, %u standalone fallback(s)\n",
		Q_memprint((float)used ), Q_memprint((float)free_bytes ), Q_memprint((float)largest_free ), fallback );
	gEngfuncs.Con_Printf( "studio skin cache: %s used in %s backed, budget %s, %u skin(s) resident at 16 bpp, %u eviction(s)\n",
		Q_memprint((float)nv2a_skin_cache_used( nv2a_skin_cache )), Q_memprint((float)nv2a_skin_cache_backed( nv2a_skin_cache )),
		Q_memprint((float)nv2a_skin_cache_capacity( nv2a_skin_cache )),
		nv2a_skin_cache_resident_count( nv2a_skin_cache ), nv2a_skin_cache_eviction_count( nv2a_skin_cache ));
	gEngfuncs.Con_Printf( "\n" );
}

/* ------------------------------------------------------------------ */
/* Lifetime (D1).                                                      */
/* ------------------------------------------------------------------ */

qboolean nv2a_tex_init( void )
{
	nv2a_textures = (nv2a_texture_t *)calloc( NV2A_MAX_TEXTURES, sizeof( nv2a_texture_t ));
	if( !nv2a_textures )
		return false;

	memset( nv2a_textures_hash, 0, sizeof( nv2a_textures_hash ));
	nv2a_textures_count = 0;
	nv2a_fallback_count = 0;
	nv2a_deferred_free_count = 0;
	nv2a_bound_texture_tmu0 = 0;
	nv2a_texnum_default = nv2a_texnum_particle = nv2a_texnum_white = nv2a_texnum_gray = nv2a_texnum_black = 0;

	/* D1: the arena, allocated once, before any map. */
	nv2a_texture_arena_mem = MmAllocateContiguousMemoryEx( NV2A_TEXTURE_ARENA_BYTES, 0, NV2A_TEXTURE_MAXRAM, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_texture_arena_mem )
	{
		free( nv2a_textures );
		nv2a_textures = NULL;
		return false;
	}

	nv2a_texture_arena = nv2a_arena_init( NV2A_TEXTURE_ARENA_BYTES );
	if( !nv2a_texture_arena )
	{
		MmFreeContiguousMemory( nv2a_texture_arena_mem );
		nv2a_texture_arena_mem = NULL;
		free( nv2a_textures );
		nv2a_textures = NULL;
		return false;
	}

	/* The studio skin cache's bookkeeping only - no chunk is backed until
	   a skin is first bound (nv2a_skin_grow), so nothing is taken before
	   the map's own GPU upload. Not fatal if it fails: studio skins then
	   draw untextured, everything else is unaffected. */
	nv2a_skin_logged_once = false;
	nv2a_skin_missed_binds = 0;
	memset( nv2a_skin_chunk_mem, 0, sizeof( nv2a_skin_chunk_mem ));
	nv2a_skin_cache = nv2a_skin_cache_init( NV2A_STUDIO_SKIN_CACHE_BYTES, NV2A_STUDIO_SKIN_CHUNK_BYTES, NV2A_MAX_TEXTURES,
		NV2A_STUDIO_SKIN_UPLOADS_PER_FRAME, NV2A_STUDIO_SKIN_BYTES_PER_FRAME, NV2A_STUDIO_SKIN_EVICT_MIN_AGE,
		nv2a_skin_grow, nv2a_skin_release, NULL );
	if( !nv2a_skin_cache )
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: could not set up the studio skin cache, studio models draw untextured\n" );

	/* Slot 0: a hashed "*unused*" entry (ref/soft's R_InitImages, r_image.c:941-959). */
	Q_strncpy( nv2a_textures[0].name, "*unused*", sizeof( nv2a_textures[0].name ));
	nv2a_textures[0].hashValue = COM_HashKey( nv2a_textures[0].name, NV2A_TEXTURES_HASH_SIZE );
	nv2a_textures[0].nextHash = nv2a_textures_hash[nv2a_textures[0].hashValue];
	nv2a_textures_hash[nv2a_textures[0].hashValue] = &nv2a_textures[0];
	nv2a_textures_count = 1;

	gEngfuncs.Cmd_AddCommand( "texturelist", R_TextureList_f, "display loaded textures list" );

	return true;
}

void nv2a_tex_shutdown( void )
{
	uint32_t i;

	gEngfuncs.Cmd_RemoveCommand( "texturelist" );

	if( nv2a_textures )
	{
		for( i = 0; i < nv2a_textures_count; i++ )
			GL_FreeTexture( i ); /* handles hash-unlink/original/storage; index 0 is a guarded no-op */
	}

	/* D7: R_Shutdown calls this after pb_kill - the GPU is idle, so
	   releasing whatever the loop above just queued (it never queues
	   anything here in practice, since no frame is open during
	   shutdown, but this stays a real no-op rather than an assumption)
	   before tearing down the arena itself is safe. */
	nv2a_tex_begin_frame();

	/* Every skin slot was just dropped by GL_FreeTexture above and the
	   drain released its bytes, so nothing points into a chunk now;
	   destroy hands every backed chunk back through nv2a_skin_release. */
	if( nv2a_skin_cache )
	{
		nv2a_skin_cache_destroy( nv2a_skin_cache );
		nv2a_skin_cache = NULL;
	}

	if( nv2a_texture_arena )
	{
		nv2a_arena_destroy( nv2a_texture_arena );
		nv2a_texture_arena = NULL;
	}
	if( nv2a_texture_arena_mem )
	{
		MmFreeContiguousMemory( nv2a_texture_arena_mem );
		nv2a_texture_arena_mem = NULL;
	}
	if( nv2a_textures )
	{
		free( nv2a_textures );
		nv2a_textures = NULL;
	}
	nv2a_textures_count = 0;
}
