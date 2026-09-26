/*
r_image.c - X9-X Part D: ref/nv2a's texture manager. CPU-side table,
upload path and every GL_*Texture* / RefGetParm-adjacent entry point the
engine calls through gReffuncs (r_context.c). See nv2a_texture.h for the
scope statement (flat RGBA32 upload, no mips, no compression, no world
textures - Part A keeps those off the GPU entirely).

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

#include "nv2a_texture.h"
#include "nv2a_arena.h"

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
} nv2a_deferred_free_t;

static nv2a_deferred_free_t nv2a_deferred_frees[NV2A_TEX_MAX_DEFERRED_FREES];
static uint32_t nv2a_deferred_free_count;

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
static void nv2a_tex_fail_slot( nv2a_texture_t *tex )
{
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

static void nv2a_tex_queue_free( qboolean has_arena_storage, size_t gpu_offset, void *fallback )
{
	if( !has_arena_storage && !fallback )
		return; /* no storage to release */

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
		nv2a_tex_free_storage_now( has_arena_storage, gpu_offset, fallback );
		return;
	}

	nv2a_deferred_frees[nv2a_deferred_free_count].has_arena_storage = has_arena_storage;
	nv2a_deferred_frees[nv2a_deferred_free_count].gpu_offset = gpu_offset;
	nv2a_deferred_frees[nv2a_deferred_free_count].fallback = fallback;
	nv2a_deferred_free_count++;
}

void nv2a_tex_begin_frame( void )
{
	uint32_t i;

	for( i = 0; i < nv2a_deferred_free_count; i++ )
		nv2a_tex_free_storage_now( nv2a_deferred_frees[i].has_arena_storage,
			nv2a_deferred_frees[i].gpu_offset, nv2a_deferred_frees[i].fallback );

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

/* D2: shared by GL_LoadTexture, GL_LoadTextureFromBuffer and
   GL_CreateTexture. `pic` must already have been through
   nv2a_tex_process_image. No mip chain, no cube faces, no arrays or 3D
   layers (all out of scope this phase - see nv2a_texture.h). */
static qboolean nv2a_tex_upload( nv2a_texture_t *tex, rgbdata_t *pic )
{
	uint32_t pitch, y;
	byte *src, *dst;
	void *dstBase;

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

	pitch = ALIGN((uint32_t)tex->width * 4, 64 );
	if( !nv2a_tex_alloc_storage( tex, (size_t)pitch * tex->height ))
		return false;
	tex->pitch = pitch;

	if( tex->gpu_size == 0 )
		return true;

	src = pic->buffer;
	dstBase = tex->has_arena_storage ? (byte *)nv2a_texture_arena_mem + tex->gpu_offset : tex->fallback;
	dst = (byte *)dstBase;

	for( y = 0; y < (uint32_t)tex->height; y++ )
		memcpy( dst + (size_t)y * pitch, src + (size_t)y * tex->width * 4, (size_t)tex->width * 4 );

	return true;
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
	if( cols == tex->width && rows == tex->height && tex->gpu_size != 0 )
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

		tex->srcWidth = (word)cols;
		tex->srcHeight = (word)rows;
		tex->width = (word)cols;
		tex->height = (word)rows;
		pitch = ALIGN((uint32_t)tex->width * 4, 64 );

		if( !nv2a_tex_alloc_storage( tex, (size_t)pitch * tex->height ))
		{
			gEngfuncs.Con_Printf( S_ERROR "%s: %s: out of texture memory on update\n", __func__, tex->name );
			tex->has_arena_storage = old_has_arena;
			tex->gpu_offset = old_offset;
			tex->fallback = old_fallback;
			gEngfuncs.FS_FreeImage( owned );
			return;
		}
		tex->pitch = pitch;

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
	uint32_t *width, uint32_t *height, qboolean *bilinear )
{
	const nv2a_texture_t *tex = nv2a_tex_get( texnum );

	if( tex->gpu_size == 0 )
		return false;

	if( phys_offset )
	{
		uint32_t base = tex->has_arena_storage
			? (uint32_t)((byte *)nv2a_texture_arena_mem + tex->gpu_offset)
			: (uint32_t)(uintptr_t)tex->fallback;
		*phys_offset = base & 0x03ffffff;
	}
	if( pitch )     *pitch = tex->pitch;
	if( width )     *width = tex->width;
	if( height )    *height = tex->height;
	if( bilinear )  *bilinear = nv2a_texture_filtering_enabled( tex );

	return true;
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
