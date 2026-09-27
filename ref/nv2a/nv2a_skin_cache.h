/*
nv2a_skin_cache.h - X9-X step 5 follow-up: the bookkeeping half of
ref/nv2a's studio skin cache (docs/r3d/design/x9-x-studio-models.md,
"Memory"; docs/r3d/divergences.md #126). ref/gl uploads every skin of
every studio model at precache time (ref/gl/gl_studio.c
R_StudioLoadTexture -> GL_LoadTexture); on the Xbox that cost 14.5 MiB
on c1a0 and stopped c4a3 and disposal loading at all, so ref/nv2a instead
uploads a skin the first time it is bound, into a cache capped at a fixed
budget, evicting the least recently used when a new one does not fit.

The cache's memory is NOT reserved up front: it is backed in fixed-size
chunks, each one physically contiguous allocation obtained through a
callback the first time the chunks already backed have no room, up to
budget / chunk_bytes chunks, and handed back through another callback
when empty (nv2a_skin_cache_trim / nv2a_skin_cache_flush). Measured on
c1a0 (xemu): the opening view needs 0.37 MiB of skins and EVERY skin the
map precaches is 4.11 MiB at 16 bpp, so a reservation made at R_Init
would sit mostly unused while taking memory the world's own GPU upload
needs - the upload that failed on disposal.

Offsets this module hands out are "chunk index * chunk_bytes + offset
within the chunk"; the owner maps them to its chunk base pointers.

This file decides WHICH skin lives WHERE and which one to evict; it never
touches the memory, pbkit, xboxkrnl or the engine (r_image.c owns the
real contiguous allocations, the pixel conversion and the texture slots).
That split is what lets tools/host-tests/nv2a_skin_cache_host_test.c
check the LRU, age, growth and per-frame rules on the host - same pattern
as nv2a_arena.h, which each chunk sits on top of.

Also here: the 16 bpp packers and the palette table the upload uses, pure
arithmetic for the same reason.
*/
#ifndef NV2A_SKIN_CACHE_H
#define NV2A_SKIN_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nv2a_skin_cache_s nv2a_skin_cache_t;

/* Result of nv2a_skin_cache_acquire. Only NV2A_SKIN_OK hands out
   storage; every other value means "draw with a placeholder this time". */
typedef enum
{
	NV2A_SKIN_OK = 0,      /* resident at *offset, last-used = this frame */
	NV2A_SKIN_FRAME_CAP,   /* this frame's upload allowance is spent - retry next frame */
	NV2A_SKIN_NO_ROOM,     /* budget reached (or a chunk could not be backed) and nothing old enough to evict frees enough - retry later */
	NV2A_SKIN_TOO_BIG,     /* larger than one chunk - will never fit, do not retry */
	NV2A_SKIN_BAD_ID       /* id out of range, or already resident (caller bug) */
} nv2a_skin_status_t;

/* Called once per evicted skin, BEFORE its storage is released, so the
   owner (r_image.c) can drop every reference it holds to that offset. */
typedef void ( *nv2a_skin_evict_fn )( void *ctx, uint32_t id );

/* Chunk backing. grow: allocate chunk `index`'s memory (chunk_bytes,
   physically contiguous); return nonzero on success. release: give chunk
   `index`'s memory back; only ever called for a chunk with nothing in it,
   from nv2a_skin_cache_trim/flush/destroy - the caller of those decides
   when the GPU is idle enough for that. */
typedef int  ( *nv2a_skin_grow_fn )( void *ctx, uint32_t index );
typedef void ( *nv2a_skin_release_fn )( void *ctx, uint32_t index );

/* budget:          the most bytes of chunks ever backed at once; rounded
                    DOWN to whole chunks (at least one).
   chunk_bytes:     size of one chunk, and so of the largest skin.
   max_ids:         ids are 0..max_ids-1 (r_image.c: texture slot numbers).
   max_uploads / max_bytes_per_frame: the per-frame upload allowance. The
                    first upload of a frame is always allowed whatever its
                    size, so a skin bigger than max_bytes_per_frame still
                    uploads eventually instead of never.
   min_evict_age:   a skin is only evictable once frame - last_used >= this.
   grow / release / backing_ctx: see above.
   Returns NULL if a bookkeeping allocation fails. Nothing is backed yet. */
nv2a_skin_cache_t *nv2a_skin_cache_init( size_t budget, size_t chunk_bytes, uint32_t max_ids,
	uint32_t max_uploads_per_frame, size_t max_bytes_per_frame, uint32_t min_evict_age,
	nv2a_skin_grow_fn grow, nv2a_skin_release_fn release, void *backing_ctx );

/* Releases every backed chunk (through `release`) and the bookkeeping.
   The caller guarantees the GPU is idle and nothing still points in. */
void nv2a_skin_cache_destroy( nv2a_skin_cache_t *cache );

/* Reserves `bytes` for `id` in `frame`: first in a chunk already backed,
   then by backing a new chunk while under budget, and only then by
   evicting least-recently-used skins at least min_evict_age frames old
   (each reported through `evict` first). On NV2A_SKIN_OK, *offset is the
   cache offset (see above). Nothing is evicted when even evicting every
   eligible skin could not free enough bytes. */
nv2a_skin_status_t nv2a_skin_cache_acquire( nv2a_skin_cache_t *cache, uint32_t id, size_t bytes, uint32_t frame,
	size_t *offset, nv2a_skin_evict_fn evict, void *ctx );

/* Undoes an acquire whose upload then failed: the storage was never
   handed to the GPU, so it is released immediately, and the frame's
   upload allowance is NOT refunded (the attempt still cost the time). */
void nv2a_skin_cache_abandon( nv2a_skin_cache_t *cache, uint32_t id );

/* Marks `id` used in `frame` (no-op if it is not resident). */
void nv2a_skin_cache_touch( nv2a_skin_cache_t *cache, uint32_t id, uint32_t frame );

int nv2a_skin_cache_is_resident( const nv2a_skin_cache_t *cache, uint32_t id );

/* Forgets `id` WITHOUT releasing its storage (the GPU may still read it
   this frame): returns 1 and writes the offset the caller must later pass
   to nv2a_skin_cache_free_offset, or 0 if it was not resident. */
int nv2a_skin_cache_detach( nv2a_skin_cache_t *cache, uint32_t id, size_t *offset );
void nv2a_skin_cache_free_offset( nv2a_skin_cache_t *cache, size_t offset );

/* Hands every backed chunk that holds nothing (no resident skin, no
   detached offset still waiting for free_offset) back through `release`.
   Returns how many were released. GPU must be idle. */
uint32_t nv2a_skin_cache_trim( nv2a_skin_cache_t *cache );

/* Evicts EVERY resident skin (through `evict`, age ignored), then trims.
   For a map load: the new world's GPU upload gets the memory, and the
   skins still wanted re-upload on their next bind. GPU must be idle, and
   detached offsets not yet freed keep their chunk backed. */
void nv2a_skin_cache_flush( nv2a_skin_cache_t *cache, nv2a_skin_evict_fn evict, void *ctx );

size_t   nv2a_skin_cache_capacity( const nv2a_skin_cache_t *cache ); /* the budget, whole chunks */
size_t   nv2a_skin_cache_chunk_bytes( const nv2a_skin_cache_t *cache );
size_t   nv2a_skin_cache_backed( const nv2a_skin_cache_t *cache );   /* bytes of chunks backed right now */
size_t   nv2a_skin_cache_used( const nv2a_skin_cache_t *cache );     /* bytes allocated within them */
uint32_t nv2a_skin_cache_resident_count( const nv2a_skin_cache_t *cache );
uint32_t nv2a_skin_cache_eviction_count( const nv2a_skin_cache_t *cache );

/* RGBA32 (R,G,B,A bytes per texel) -> the 16-bit words
   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 / _A1R5G5B5 read
   (little-endian, red in the high bits - nv_regs.h:550-551, the same
   A..B = MSB..LSB naming LU_IMAGE_A8B8G8R8 follows). Channels round to
   nearest; A1 is set for alpha >= 128 (Image_LoadMDL's masked palette
   only ever produces 0 or 255, img_wad.c:216-221). */
void nv2a_pack_r5g6b5( const uint8_t *rgba, uint16_t *out, size_t count );
void nv2a_pack_a1r5g5b5( const uint8_t *rgba, uint16_t *out, size_t count );

/* The direct studio skin decode: a 256-entry 16 bpp lookup table built
   from a skin's 768-byte palette, reproducing exactly what the engine's
   Image_LoadMDL would expand to RGBA (img_wad.c:212-221 ->
   Image_SetPalette, img_utils.c:284-310), then packed as above:
     masked == 0: LUMP_TEXGAMMA - every entry through `texgamma`
                  (TextureToGamma, engine/client/gamma.c:175-181; NULL
                  means identity, i.e. ENGINE_LINEAR_GAMMA_SPACE), opaque,
                  as R5G6B5;
     masked != 0: LUMP_MASKED - entries 0..254 raw (no gamma), opaque;
                  entry 255 all zero (transparent black), as A1R5G5B5.
   Decoding through the table needs no RGBA intermediate and no engine
   image-pool allocation, both of which a mid-game upload cannot afford
   (the zone allocator Sys_Errors on exhaustion, zone.c). */
void nv2a_skin_build_lut( const uint8_t *palette768, const uint8_t *texgamma, int masked, uint16_t lut[256] );
void nv2a_skin_expand_indexed( const uint8_t *indices, const uint16_t lut[256], uint16_t *out, size_t count );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_SKIN_CACHE_H */
