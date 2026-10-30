/*
nv2a_skin_cache.c - see nv2a_skin_cache.h for the contract. One entry per
id (a texture slot number), one first-fit nv2a_arena per backed chunk for
the offsets, and a linear scan for the least-recently-used victim. The
scan is O(max_ids) per eviction step, but eviction only ever runs inside
an upload, which the per-frame allowance already limits to a couple per
frame; an intrusive LRU list would buy nothing measurable there and add a
second structure to keep consistent.
*/
#include "nv2a_skin_cache.h"
#include "nv2a_arena.h"
#include <stdlib.h>

typedef struct
{
	size_t   offset; /* cache offset: chunk * chunk_bytes + offset within it */
	size_t   bytes;
	uint32_t last_used;
	int      resident;
} nv2a_skin_entry_t;

struct nv2a_skin_cache_s
{
	size_t             chunk_bytes;
	uint32_t           max_chunks;
	nv2a_arena_t     **chunks; /* max_chunks slots; NULL == not backed */
	nv2a_skin_grow_fn    grow;
	nv2a_skin_release_fn release;
	void                *backing_ctx;

	nv2a_skin_entry_t *entries;
	uint32_t           max_ids;
	uint32_t           resident_count;
	uint32_t           evictions;

	uint32_t max_uploads_per_frame;
	size_t   max_bytes_per_frame;
	uint32_t min_evict_age;

	/* The per-frame allowance, reset lazily the first time a new frame
	   number is seen - so no separate begin-frame call can be forgotten
	   or made twice (r_image.c's nv2a_tex_begin_frame also runs mid-frame
	   when its free queue fills). */
	int      frame_valid;
	uint32_t frame;
	uint32_t uploads_this_frame;
	size_t   bytes_this_frame;
};

nv2a_skin_cache_t *nv2a_skin_cache_init( size_t budget, size_t chunk_bytes, uint32_t max_ids,
	uint32_t max_uploads_per_frame, size_t max_bytes_per_frame, uint32_t min_evict_age,
	nv2a_skin_grow_fn grow, nv2a_skin_release_fn release, void *backing_ctx )
{
	nv2a_skin_cache_t *cache;
	uint32_t max_chunks;

	if( chunk_bytes == 0 || !grow || !release )
		return NULL;

	max_chunks = (uint32_t)( budget / chunk_bytes );
	if( max_chunks == 0 )
		max_chunks = 1;

	cache = (nv2a_skin_cache_t *)calloc( 1, sizeof( *cache ));
	if( !cache )
		return NULL;

	cache->chunks = (nv2a_arena_t **)calloc( max_chunks, sizeof( nv2a_arena_t * ));
	cache->entries = (nv2a_skin_entry_t *)calloc( max_ids ? max_ids : 1, sizeof( nv2a_skin_entry_t ));
	if( !cache->chunks || !cache->entries )
	{
		free( cache->chunks );
		free( cache->entries );
		free( cache );
		return NULL;
	}

	cache->chunk_bytes = chunk_bytes;
	cache->max_chunks = max_chunks;
	cache->grow = grow;
	cache->release = release;
	cache->backing_ctx = backing_ctx;
	cache->max_ids = max_ids;
	cache->max_uploads_per_frame = max_uploads_per_frame;
	cache->max_bytes_per_frame = max_bytes_per_frame;
	cache->min_evict_age = min_evict_age;
	return cache;
}

static void nv2a_skin_cache_release_chunk( nv2a_skin_cache_t *cache, uint32_t index )
{
	nv2a_arena_destroy( cache->chunks[index] );
	cache->chunks[index] = NULL;
	cache->release( cache->backing_ctx, index );
}

void nv2a_skin_cache_destroy( nv2a_skin_cache_t *cache )
{
	uint32_t i;

	if( !cache )
		return;
	for( i = 0; i < cache->max_chunks; i++ )
		if( cache->chunks[i] )
			nv2a_skin_cache_release_chunk( cache, i );
	free( cache->chunks );
	free( cache->entries );
	free( cache );
}

static void nv2a_skin_cache_sync_frame( nv2a_skin_cache_t *cache, uint32_t frame )
{
	if( cache->frame_valid && cache->frame == frame )
		return;
	cache->frame_valid = 1;
	cache->frame = frame;
	cache->uploads_this_frame = 0;
	cache->bytes_this_frame = 0;
}

/* Unsigned subtraction: correct across the uint32_t frame counter's wrap. */
static int nv2a_skin_cache_evictable( const nv2a_skin_cache_t *cache, const nv2a_skin_entry_t *e, uint32_t frame )
{
	return e->resident && (uint32_t)( frame - e->last_used ) >= cache->min_evict_age;
}

static void nv2a_skin_cache_arena_free( nv2a_skin_cache_t *cache, size_t offset )
{
	uint32_t index = (uint32_t)( offset / cache->chunk_bytes );

	if( index < cache->max_chunks && cache->chunks[index] )
		nv2a_arena_free( cache->chunks[index], offset % cache->chunk_bytes );
}

static void nv2a_skin_cache_release_entry( nv2a_skin_cache_t *cache, nv2a_skin_entry_t *e )
{
	nv2a_skin_cache_arena_free( cache, e->offset );
	e->resident = 0;
	e->offset = 0;
	e->bytes = 0;
	cache->resident_count--;
}

/* First fit across the backed chunks, lowest chunk first. */
static size_t nv2a_skin_cache_try_alloc( nv2a_skin_cache_t *cache, size_t bytes )
{
	uint32_t i;

	for( i = 0; i < cache->max_chunks; i++ )
	{
		size_t got;

		if( !cache->chunks[i] )
			continue;
		got = nv2a_arena_alloc( cache->chunks[i], bytes );
		if( got != NV2A_ARENA_FAILED )
			return (size_t)i * cache->chunk_bytes + got;
	}
	return NV2A_ARENA_FAILED;
}

/* Backs the lowest unbacked chunk; NV2A_ARENA_FAILED at the budget or
   when the owner could not allocate it. */
static size_t nv2a_skin_cache_grow_and_alloc( nv2a_skin_cache_t *cache, size_t bytes )
{
	uint32_t i;

	for( i = 0; i < cache->max_chunks; i++ )
	{
		size_t got;

		if( cache->chunks[i] )
			continue;

		cache->chunks[i] = nv2a_arena_init( cache->chunk_bytes );
		if( !cache->chunks[i] )
			return NV2A_ARENA_FAILED;
		if( !cache->grow( cache->backing_ctx, i ))
		{
			nv2a_arena_destroy( cache->chunks[i] );
			cache->chunks[i] = NULL;
			return NV2A_ARENA_FAILED;
		}

		got = nv2a_arena_alloc( cache->chunks[i], bytes );
		return got == NV2A_ARENA_FAILED ? NV2A_ARENA_FAILED : (size_t)i * cache->chunk_bytes + got;
	}
	return NV2A_ARENA_FAILED;
}

nv2a_skin_status_t nv2a_skin_cache_acquire( nv2a_skin_cache_t *cache, uint32_t id, size_t bytes, uint32_t frame,
	size_t *offset, nv2a_skin_evict_fn evict, void *ctx )
{
	size_t got, reclaimable;
	uint32_t i;

	if( !cache || id >= cache->max_ids || cache->entries[id].resident || bytes == 0 )
		return NV2A_SKIN_BAD_ID;

	if( bytes > cache->chunk_bytes )
		return NV2A_SKIN_TOO_BIG;

	nv2a_skin_cache_sync_frame( cache, frame );

	/* The allowance: the first upload of a frame always goes (see the
	   header), later ones only while both counts stay under the caps. */
	if( cache->uploads_this_frame > 0
		&& ( cache->uploads_this_frame >= cache->max_uploads_per_frame
			|| cache->bytes_this_frame + bytes > cache->max_bytes_per_frame ))
		return NV2A_SKIN_FRAME_CAP;

	got = nv2a_skin_cache_try_alloc( cache, bytes );
	if( got == NV2A_ARENA_FAILED )
		got = nv2a_skin_cache_grow_and_alloc( cache, bytes );

	if( got == NV2A_ARENA_FAILED )
	{
		/* Do not evict anything unless evicting everything eligible could
		   at least cover the byte count - otherwise the victims are lost
		   for nothing. Fragmentation can still defeat a covered count;
		   then the loop below evicts until it runs out of victims and
		   reports NO_ROOM, which is the worst case, not the common one. */
		reclaimable = 0;
		for( i = 0; i < cache->max_chunks; i++ )
			if( cache->chunks[i] )
				reclaimable += nv2a_arena_free_bytes( cache->chunks[i] );
		for( i = 0; i < cache->max_ids; i++ )
			if( nv2a_skin_cache_evictable( cache, &cache->entries[i], frame ))
				reclaimable += cache->entries[i].bytes;
		if( reclaimable < bytes )
			return NV2A_SKIN_NO_ROOM;

		while( got == NV2A_ARENA_FAILED )
		{
			uint32_t victim = cache->max_ids;
			uint32_t oldest_age = 0;

			for( i = 0; i < cache->max_ids; i++ )
			{
				const nv2a_skin_entry_t *e = &cache->entries[i];
				uint32_t age;

				if( !nv2a_skin_cache_evictable( cache, e, frame ))
					continue;
				age = frame - e->last_used;
				if( victim == cache->max_ids || age > oldest_age )
				{
					victim = i;
					oldest_age = age;
				}
			}

			if( victim == cache->max_ids )
				return NV2A_SKIN_NO_ROOM;

			if( evict )
				evict( ctx, victim );
			nv2a_skin_cache_release_entry( cache, &cache->entries[victim] );
			cache->evictions++;

			got = nv2a_skin_cache_try_alloc( cache, bytes );
		}
	}

	cache->entries[id].resident = 1;
	cache->entries[id].offset = got;
	cache->entries[id].bytes = bytes;
	cache->entries[id].last_used = frame;
	cache->resident_count++;
	cache->uploads_this_frame++;
	cache->bytes_this_frame += bytes;

	if( offset )
		*offset = got;
	return NV2A_SKIN_OK;
}

void nv2a_skin_cache_abandon( nv2a_skin_cache_t *cache, uint32_t id )
{
	if( !cache || id >= cache->max_ids || !cache->entries[id].resident )
		return;
	nv2a_skin_cache_release_entry( cache, &cache->entries[id] );
}

void nv2a_skin_cache_touch( nv2a_skin_cache_t *cache, uint32_t id, uint32_t frame )
{
	if( !cache || id >= cache->max_ids || !cache->entries[id].resident )
		return;
	cache->entries[id].last_used = frame;
}

int nv2a_skin_cache_is_resident( const nv2a_skin_cache_t *cache, uint32_t id )
{
	return cache && id < cache->max_ids && cache->entries[id].resident;
}

int nv2a_skin_cache_detach( nv2a_skin_cache_t *cache, uint32_t id, size_t *offset )
{
	nv2a_skin_entry_t *e;

	if( !cache || id >= cache->max_ids || !cache->entries[id].resident )
		return 0;

	e = &cache->entries[id];
	if( offset )
		*offset = e->offset;
	e->resident = 0;
	e->offset = 0;
	e->bytes = 0;
	cache->resident_count--;
	return 1;
}

void nv2a_skin_cache_free_offset( nv2a_skin_cache_t *cache, size_t offset )
{
	if( cache )
		nv2a_skin_cache_arena_free( cache, offset );
}

uint32_t nv2a_skin_cache_trim( nv2a_skin_cache_t *cache )
{
	uint32_t i, released = 0;

	if( !cache )
		return 0;
	for( i = 0; i < cache->max_chunks; i++ )
	{
		if( cache->chunks[i] && nv2a_arena_used( cache->chunks[i] ) == 0 )
		{
			nv2a_skin_cache_release_chunk( cache, i );
			released++;
		}
	}
	return released;
}

void nv2a_skin_cache_flush( nv2a_skin_cache_t *cache, nv2a_skin_evict_fn evict, void *ctx )
{
	uint32_t i;

	if( !cache )
		return;
	for( i = 0; i < cache->max_ids; i++ )
	{
		if( !cache->entries[i].resident )
			continue;
		if( evict )
			evict( ctx, i );
		nv2a_skin_cache_release_entry( cache, &cache->entries[i] );
		cache->evictions++;
	}
	nv2a_skin_cache_trim( cache );
}

size_t nv2a_skin_cache_capacity( const nv2a_skin_cache_t *cache )
{
	return cache ? (size_t)cache->max_chunks * cache->chunk_bytes : 0;
}

size_t nv2a_skin_cache_chunk_bytes( const nv2a_skin_cache_t *cache )
{
	return cache ? cache->chunk_bytes : 0;
}

size_t nv2a_skin_cache_backed( const nv2a_skin_cache_t *cache )
{
	size_t total = 0;
	uint32_t i;

	if( !cache )
		return 0;
	for( i = 0; i < cache->max_chunks; i++ )
		if( cache->chunks[i] )
			total += cache->chunk_bytes;
	return total;
}

size_t nv2a_skin_cache_used( const nv2a_skin_cache_t *cache )
{
	size_t total = 0;
	uint32_t i;

	if( !cache )
		return 0;
	for( i = 0; i < cache->max_chunks; i++ )
		if( cache->chunks[i] )
			total += nv2a_arena_used( cache->chunks[i] );
	return total;
}

void nv2a_skin_cache_reset_allowance( nv2a_skin_cache_t *cache )
{
	if( !cache )
		return;
	cache->uploads_this_frame = 0;
	cache->bytes_this_frame = 0;
}

uint32_t nv2a_skin_cache_resident_count( const nv2a_skin_cache_t *cache )
{
	return cache ? cache->resident_count : 0;
}

uint32_t nv2a_skin_cache_eviction_count( const nv2a_skin_cache_t *cache )
{
	return cache ? cache->evictions : 0;
}

/* Round-to-nearest channel narrowing: (v * max + 127) / 255 maps 0 -> 0
   and 255 -> max exactly, which a plain shift also does, but keeps
   mid-tones from all rounding down (a truncating >>3 darkens the whole
   skin by half a step on average). */
static uint16_t nv2a_narrow( uint8_t v, uint32_t max )
{
	return (uint16_t)(( v * max + 127u ) / 255u );
}

void nv2a_pack_r5g6b5( const uint8_t *rgba, uint16_t *out, size_t count )
{
	size_t i;

	for( i = 0; i < count; i++, rgba += 4 )
		out[i] = (uint16_t)(( nv2a_narrow( rgba[0], 31 ) << 11 ) | ( nv2a_narrow( rgba[1], 63 ) << 5 ) | nv2a_narrow( rgba[2], 31 ));
}

void nv2a_pack_a1r5g5b5( const uint8_t *rgba, uint16_t *out, size_t count )
{
	size_t i;

	for( i = 0; i < count; i++, rgba += 4 )
		out[i] = (uint16_t)(( rgba[3] >= 128 ? 0x8000u : 0u ) | ( nv2a_narrow( rgba[0], 31 ) << 10 )
			| ( nv2a_narrow( rgba[1], 31 ) << 5 ) | nv2a_narrow( rgba[2], 31 ));
}

void nv2a_skin_build_lut( const uint8_t *palette768, const uint8_t *texgamma, int masked, uint16_t lut[256] )
{
	uint8_t rgba[4];
	int i;

	for( i = 0; i < 256; i++ )
	{
		const uint8_t *p = palette768 + i * 3;

		if( masked )
		{
			/* LUMP_MASKED: raw colours, index 255 is 0 (img_utils.c:306-310) */
			rgba[0] = i == 255 ? 0 : p[0];
			rgba[1] = i == 255 ? 0 : p[1];
			rgba[2] = i == 255 ? 0 : p[2];
			rgba[3] = i == 255 ? 0 : 0xFF;
			nv2a_pack_a1r5g5b5( rgba, &lut[i], 1 );
		}
		else
		{
			/* LUMP_TEXGAMMA (img_utils.c:292-300) */
			rgba[0] = texgamma ? texgamma[p[0]] : p[0];
			rgba[1] = texgamma ? texgamma[p[1]] : p[1];
			rgba[2] = texgamma ? texgamma[p[2]] : p[2];
			rgba[3] = 0xFF;
			nv2a_pack_r5g6b5( rgba, &lut[i], 1 );
		}
	}
}

void nv2a_skin_expand_indexed( const uint8_t *indices, const uint16_t lut[256], uint16_t *out, size_t count )
{
	size_t i;

	for( i = 0; i < count; i++ )
		out[i] = lut[indices[i]];
}
