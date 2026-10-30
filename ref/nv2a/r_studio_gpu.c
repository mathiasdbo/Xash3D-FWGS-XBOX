/*
r_studio_gpu.c - the static geometry of ref/nv2a's GPU studio path
(docs/r3d/divergences.md #143; r_studio_gpu.h): for each submodel drawn,
its unique vertices (in GPU memory), its indexed triangle lists and its
bone palettes (on the heap), built from the .mdl already in memory the
first time the submodel is drawn and kept under a fixed GPU budget with
LRU eviction.

The bookkeeping is nv2a_skin_cache.c's (host-tested): the same chunked,
on-demand backing, LRU and minimum-age rules the studio skins use,
instantiated a second time with submodel entries as its ids. The
geometry itself is nv2a_studio_gpu_util.c's (host-tested against the
TriAPI assembler on every retail model). This file only resolves the
studiohdr_t into the builder's arrays and owns the memory.

Lifetime: everything is dropped before each map's GPU upload
(nv2a_sgpu_cache_flush, next to the skin cache's), and an entry whose
model is freed is forgotten at once (nv2a_sgpu_cache_forget, from
Mod_StudioUnloadTextures) so a new model allocated at the same address
can never hit it; its storage then ages out like any other.

XASH_LOW_MEMORY (lessons.md): Mod_MaybeTruncateStudioTextureData
(engine/common/mod_studio.c) cuts a studio model at texturedataindex -
the texture pixels, which follow every bone, mesh, vertex and tricmd
lump in a compiled .mdl - so everything this file reads is still there.
The builder bounds-checks every tricmd index against numverts/numnorms
regardless (nv2a_sgpu_scan).
*/
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xash3d_types.h"
#include "const.h"
#include "com_strings.h"
#include "ref_common.h"
#include "xash3d_mathlib.h" /* HalfToFloat */
#include "studio.h"
#include "nv2a_texture.h"   /* NV2A_TEXTURE_MAXRAM */
#include "nv2a_skin_cache.h"
#include "nv2a_studio_gpu_util.h"
#include "r_studio_gpu.h"

#define NV2A_SGPU_MAX_CHUNKS ( NV2A_STUDIO_GPU_CACHE_BYTES / NV2A_STUDIO_GPU_CHUNK_BYTES )
#define NV2A_SGPU_NO_ENTRY   0xFFFFu
#define NV2A_SGPU_BUCKETS    256u

typedef struct
{
	const studiohdr_t    *hdr;      /* key, NULL when the slot is free or forgotten */
	const mstudiomodel_t *sub;      /* key */
	nv2a_sgpu_geometry_t  geo;
	void                 *heap;     /* indices + meshes + palettes, one block */
	uint32_t              heap_bytes;
	uint32_t              gpu_bytes;
	uint32_t              retry_frame; /* failed entries: not retried before this frame */
	uint16_t              next;     /* bucket chain */
	uint8_t               used;     /* slot holds a key (resident or failed) */
	uint8_t               resident; /* has cache storage */
} nv2a_sgpu_entry_t;

nv2a_sgpu_cache_stats_t nv2a_sgpu_cache_stats;

static nv2a_skin_cache_t *nv2a_sgpu_cache;
static void              *nv2a_sgpu_chunk_mem[NV2A_SGPU_MAX_CHUNKS];
static nv2a_sgpu_entry_t  nv2a_sgpu_entries[NV2A_STUDIO_GPU_MAX_ENTRIES];
static uint16_t           nv2a_sgpu_buckets[NV2A_SGPU_BUCKETS];
static uint32_t           nv2a_sgpu_build_frame;
static uint32_t           nv2a_sgpu_builds_this_frame;
static uint32_t           nv2a_sgpu_build_verts_this_frame; /* #211 */

static uint32_t nv2a_sgpu_bucket( const studiohdr_t *hdr, const mstudiomodel_t *sub )
{
	uint32_t h = (uint32_t)(uintptr_t)hdr * 2654435761u ^ (uint32_t)(uintptr_t)sub * 40503u;
	return ( h >> 8 ) & ( NV2A_SGPU_BUCKETS - 1u );
}

static void nv2a_sgpu_unlink( uint16_t id )
{
	nv2a_sgpu_entry_t *e = &nv2a_sgpu_entries[id];
	uint16_t *link = &nv2a_sgpu_buckets[nv2a_sgpu_bucket( e->hdr, e->sub )];

	while( *link != NV2A_SGPU_NO_ENTRY )
	{
		if( *link == id )
		{
			*link = e->next;
			break;
		}
		link = &nv2a_sgpu_entries[*link].next;
	}
	e->next = NV2A_SGPU_NO_ENTRY;
}

/* Drops a slot's heap block and key (not its cache storage). */
static void nv2a_sgpu_release_slot( uint16_t id )
{
	nv2a_sgpu_entry_t *e = &nv2a_sgpu_entries[id];

	if( e->hdr )
		nv2a_sgpu_unlink( id );
	if( e->heap )
	{
		free( e->heap );
		nv2a_sgpu_cache_stats.heap_used -= e->heap_bytes;
	}
	memset( e, 0, sizeof( *e ));
	e->next = NV2A_SGPU_NO_ENTRY;
}

static int nv2a_sgpu_grow( void *ctx, uint32_t index )
{
	(void)ctx;
	if( index >= NV2A_SGPU_MAX_CHUNKS )
		return 0;
	nv2a_sgpu_chunk_mem[index] = MmAllocateContiguousMemoryEx( NV2A_STUDIO_GPU_CHUNK_BYTES, 0, NV2A_TEXTURE_MAXRAM, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( nv2a_sgpu_chunk_mem[index] )
		nv2a_sgpu_cache_stats.gpu_backed += NV2A_STUDIO_GPU_CHUNK_BYTES;
	return nv2a_sgpu_chunk_mem[index] != NULL;
}

static void nv2a_sgpu_release( void *ctx, uint32_t index )
{
	(void)ctx;
	if( index < NV2A_SGPU_MAX_CHUNKS && nv2a_sgpu_chunk_mem[index] )
	{
		MmFreeContiguousMemory( nv2a_sgpu_chunk_mem[index] );
		nv2a_sgpu_chunk_mem[index] = NULL;
		nv2a_sgpu_cache_stats.gpu_backed -= NV2A_STUDIO_GPU_CHUNK_BYTES;
	}
}

/* nv2a_skin_cache's eviction callback: only entries at least
   NV2A_STUDIO_GPU_EVICT_MIN_AGE frames old reach it, and R_BeginFrame
   spins pb_busy before a frame opens, so the GPU is not reading them. */
static void nv2a_sgpu_evicted( void *ctx, uint32_t id )
{
	(void)ctx;
	if( id >= NV2A_STUDIO_GPU_MAX_ENTRIES )
		return;
	if( nv2a_sgpu_entries[id].resident )
	{
		nv2a_sgpu_cache_stats.entries--;
		nv2a_sgpu_cache_stats.gpu_used -= nv2a_sgpu_entries[id].gpu_bytes;
		nv2a_sgpu_cache_stats.evictions++;
	}
	nv2a_sgpu_release_slot( (uint16_t)id );
}

void nv2a_sgpu_cache_init( void )
{
	uint32_t i;

	memset( &nv2a_sgpu_cache_stats, 0, sizeof( nv2a_sgpu_cache_stats ));
	memset( nv2a_sgpu_chunk_mem, 0, sizeof( nv2a_sgpu_chunk_mem ));
	memset( nv2a_sgpu_entries, 0, sizeof( nv2a_sgpu_entries ));
	for( i = 0; i < NV2A_STUDIO_GPU_MAX_ENTRIES; i++ )
		nv2a_sgpu_entries[i].next = NV2A_SGPU_NO_ENTRY;
	for( i = 0; i < NV2A_SGPU_BUCKETS; i++ )
		nv2a_sgpu_buckets[i] = NV2A_SGPU_NO_ENTRY;

	/* No chunk is backed here (nv2a_sgpu_grow runs on the first build),
	   so nothing is taken before the map's own GPU upload. A NULL cache
	   only means every studio model draws on the CPU. */
	nv2a_sgpu_cache = nv2a_skin_cache_init( NV2A_STUDIO_GPU_CACHE_BYTES, NV2A_STUDIO_GPU_CHUNK_BYTES, NV2A_STUDIO_GPU_MAX_ENTRIES,
		NV2A_STUDIO_GPU_BUILDS_PER_FRAME, NV2A_STUDIO_GPU_CACHE_BYTES, NV2A_STUDIO_GPU_EVICT_MIN_AGE,
		nv2a_sgpu_grow, nv2a_sgpu_release, NULL );
	if( !nv2a_sgpu_cache )
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: could not set up the studio GPU cache, studio models draw on the CPU\n" );
}

static void nv2a_sgpu_drop_unbacked( void )
{
	uint32_t i;

	/* failed and forgotten slots hold heap or a key but no storage */
	for( i = 0; i < NV2A_STUDIO_GPU_MAX_ENTRIES; i++ )
	{
		if( nv2a_sgpu_entries[i].used && !nv2a_sgpu_entries[i].resident )
			nv2a_sgpu_release_slot( (uint16_t)i );
	}
}

void nv2a_sgpu_cache_flush( void )
{
	if( !nv2a_sgpu_cache )
		return;
	nv2a_skin_cache_flush( nv2a_sgpu_cache, nv2a_sgpu_evicted, NULL );
	nv2a_sgpu_drop_unbacked();
	nv2a_sgpu_cache_stats.builds = 0;
	nv2a_sgpu_cache_stats.failures = 0;
}

size_t nv2a_sgpu_contiguous( void )
{
	size_t i, bytes = 0;

	for( i = 0; i < NV2A_SGPU_MAX_CHUNKS; i++ )
		bytes += ( nv2a_sgpu_chunk_mem[i] ? MmQueryAllocationSize( nv2a_sgpu_chunk_mem[i] ) : 0 );
	return bytes;
}

void nv2a_sgpu_cache_shutdown( void )
{
	if( !nv2a_sgpu_cache )
		return;
	nv2a_skin_cache_flush( nv2a_sgpu_cache, nv2a_sgpu_evicted, NULL );
	nv2a_sgpu_drop_unbacked();
	nv2a_skin_cache_destroy( nv2a_sgpu_cache );
	nv2a_sgpu_cache = NULL;
}

void nv2a_sgpu_cache_forget( const void *hdr )
{
	uint32_t i;

	if( !hdr )
		return;
	for( i = 0; i < NV2A_STUDIO_GPU_MAX_ENTRIES; i++ )
	{
		nv2a_sgpu_entry_t *e = &nv2a_sgpu_entries[i];

		if( !e->used || (const void *)e->hdr != hdr )
			continue;
		if( e->resident )
		{
			/* storage stays until evicted (the GPU may read it this
			   frame); only the key goes, so nothing can hit it */
			nv2a_sgpu_unlink( (uint16_t)i );
			e->hdr = NULL;
			e->sub = NULL;
		}
		else
		{
			nv2a_sgpu_release_slot( (uint16_t)i );
		}
	}
}

static float nv2a_sgpu_half_to_float( uint16_t h )
{
	return HalfToFloat( h );
}

static int nv2a_sgpu_free_slot( void )
{
	uint32_t i;

	for( i = 0; i < NV2A_STUDIO_GPU_MAX_ENTRIES; i++ )
	{
		if( !nv2a_sgpu_entries[i].used )
			return (int)i;
	}
	return -1;
}

/* Records a submodel that could not be built, so it is not rescanned
   every frame. */
static void nv2a_sgpu_mark_failed( int id, const studiohdr_t *hdr, const mstudiomodel_t *sub, uint32_t retry_frame )
{
	nv2a_sgpu_entry_t *e = &nv2a_sgpu_entries[id];
	uint32_t b = nv2a_sgpu_bucket( hdr, sub );

	nv2a_sgpu_cache_stats.failures++;
	e->used = 1;
	e->resident = 0;
	e->hdr = hdr;
	e->sub = sub;
	e->retry_frame = retry_frame;
	e->next = nv2a_sgpu_buckets[b];
	nv2a_sgpu_buckets[b] = (uint16_t)id;
}

const nv2a_sgpu_geometry_t *nv2a_sgpu_cache_get( const studiohdr_t *hdr, const mstudiomodel_t *sub,
	const mstudiotexture_t *ptexture, int numtextures, const short *pskinref, int numskinref, uint32_t frame )
{
	const int16_t *tricmds[MAXSTUDIOMESHES];
	uint8_t uv_float[MAXSTUDIOMESHES];
	int mesh_numnorms[MAXSTUDIOMESHES];
	nv2a_sgpu_source_t src;
	nv2a_sgpu_sizes_t sizes;
	const mstudiomesh_t *pmesh;
	uint32_t max_mesh_verts, b, heap_bytes, gpu_bytes;
	uint16_t id;
	int slot, m;
	void *scratch, *heap;
	size_t offset;
	nv2a_skin_status_t status;
	nv2a_sgpu_entry_t *e;

	if( !nv2a_sgpu_cache || !hdr || !sub )
		return NULL;

	b = nv2a_sgpu_bucket( hdr, sub );
	for( id = nv2a_sgpu_buckets[b]; id != NV2A_SGPU_NO_ENTRY; id = nv2a_sgpu_entries[id].next )
	{
		e = &nv2a_sgpu_entries[id];
		if( e->hdr != hdr || e->sub != sub )
			continue;
		if( e->resident )
		{
			nv2a_skin_cache_touch( nv2a_sgpu_cache, id, frame );
			return &e->geo;
		}
		if( frame < e->retry_frame )
			return NULL;
		nv2a_sgpu_release_slot( id ); /* retry now */
		break;
	}

	/* A build walks every tricmd: capped per frame. */
	if( nv2a_sgpu_build_frame != frame )
	{
		nv2a_sgpu_build_frame = frame;
		nv2a_sgpu_builds_this_frame = 0;
		nv2a_sgpu_build_verts_this_frame = 0;
	}
	if( nv2a_sgpu_builds_this_frame > 0
		&& ( nv2a_sgpu_builds_this_frame >= NV2A_STUDIO_GPU_BUILDS_PER_FRAME
			|| nv2a_sgpu_build_verts_this_frame + (uint32_t)( sub->numverts > 0 ? sub->numverts : 0 ) > NV2A_STUDIO_GPU_BUILD_VERTS_PER_FRAME ))
		return NULL;

	slot = nv2a_sgpu_free_slot();
	if( slot < 0 || sub->nummesh <= 0 || sub->nummesh > MAXSTUDIOMESHES )
		return NULL;
	nv2a_sgpu_builds_this_frame++;
	nv2a_sgpu_build_verts_this_frame += (uint32_t)( sub->numverts > 0 ? sub->numverts : 0 );

	pmesh = (const mstudiomesh_t *)((const byte *)hdr + sub->meshindex );
	for( m = 0; m < sub->nummesh; m++ )
	{
		int skinref = pmesh[m].skinref, tex = -1;

		tricmds[m] = (const int16_t *)((const byte *)hdr + pmesh[m].triindex );
		mesh_numnorms[m] = pmesh[m].numnorms;
		if( pskinref && skinref >= 0 && skinref < numskinref )
			tex = pskinref[skinref];
		uv_float[m] = ( ptexture && tex >= 0 && tex < numtextures && FBitSet( ptexture[tex].flags, STUDIO_NF_UV_COORDS )) ? 1 : 0;
	}

	memset( &src, 0, sizeof( src ));
	src.numverts = sub->numverts;
	src.vertbone = (const uint8_t *)hdr + sub->vertinfoindex;
	src.verts = (const float *)((const byte *)hdr + sub->vertindex );
	src.numnorms = sub->numnorms;
	src.normbone = (const uint8_t *)hdr + sub->norminfoindex;
	src.norms = (const float *)((const byte *)hdr + sub->normindex );
	src.nummesh = sub->nummesh;
	src.tricmds = tricmds;
	src.uv_float = uv_float;
	src.mesh_numnorms = mesh_numnorms;
	src.numbones = hdr->numbones;
	src.half_to_float = nv2a_sgpu_half_to_float;

	if( !nv2a_sgpu_scan( &src, &max_mesh_verts ))
	{
		nv2a_sgpu_mark_failed( slot, hdr, sub, UINT32_MAX ); /* malformed: never */
		return NULL;
	}
	scratch = malloc( nv2a_sgpu_scratch_bytes( max_mesh_verts ));
	if( !scratch )
		return NULL;
	if( !nv2a_sgpu_measure( &src, scratch, &sizes ) || sizes.vertices == 0 )
	{
		free( scratch );
		nv2a_sgpu_mark_failed( slot, hdr, sub, UINT32_MAX );
		return NULL;
	}

	gpu_bytes = sizes.vertices * (uint32_t)sizeof( nv2a_sgpu_vertex_t );
	heap_bytes = (( sizes.indices * 2u + 3u ) & ~3u ) + (uint32_t)( sub->nummesh * sizeof( nv2a_sgpu_mesh_t ))
		+ sizes.palettes * (uint32_t)sizeof( nv2a_sgpu_palette_t );
	if( nv2a_sgpu_cache_stats.heap_used + heap_bytes > NV2A_STUDIO_GPU_HEAP_BYTES )
	{
		free( scratch );
		nv2a_sgpu_mark_failed( slot, hdr, sub, frame + NV2A_STUDIO_GPU_RETRY_FRAMES );
		return NULL;
	}

	status = nv2a_skin_cache_acquire( nv2a_sgpu_cache, (uint32_t)slot, gpu_bytes, frame, &offset, nv2a_sgpu_evicted, NULL );
	if( status != NV2A_SKIN_OK )
	{
		free( scratch );
		if( status != NV2A_SKIN_FRAME_CAP )
			nv2a_sgpu_mark_failed( slot, hdr, sub, status == NV2A_SKIN_TOO_BIG ? UINT32_MAX : frame + NV2A_STUDIO_GPU_RETRY_FRAMES );
		return NULL;
	}

	heap = malloc( heap_bytes );
	if( !heap )
	{
		nv2a_skin_cache_abandon( nv2a_sgpu_cache, (uint32_t)slot );
		free( scratch );
		return NULL;
	}

	e = &nv2a_sgpu_entries[slot];
	memset( e, 0, sizeof( *e ));
	e->heap = heap;
	e->heap_bytes = heap_bytes;
	e->gpu_bytes = gpu_bytes;
	e->geo.vb = (const nv2a_sgpu_vertex_t *)((byte *)nv2a_sgpu_chunk_mem[offset / NV2A_STUDIO_GPU_CHUNK_BYTES] + offset % NV2A_STUDIO_GPU_CHUNK_BYTES );
	e->geo.index_dwords = (const uint32_t *)heap;
	e->geo.meshes = (const nv2a_sgpu_mesh_t *)((byte *)heap + (( sizes.indices * 2u + 3u ) & ~3u ));
	e->geo.palettes = (const nv2a_sgpu_palette_t *)( e->geo.meshes + sub->nummesh );
	e->geo.nummesh = sub->nummesh;

	/* The vertices go straight into the write-combined chunk: the builder
	   writes each one once, in increasing order, and never reads them. */
	if( !nv2a_sgpu_build( &src, scratch, (nv2a_sgpu_vertex_t *)e->geo.vb, (uint16_t *)heap,
		(nv2a_sgpu_mesh_t *)e->geo.meshes, (nv2a_sgpu_palette_t *)e->geo.palettes, &sizes ))
	{
		free( scratch );
		free( heap );
		memset( e, 0, sizeof( *e ));
		e->next = NV2A_SGPU_NO_ENTRY;
		nv2a_skin_cache_abandon( nv2a_sgpu_cache, (uint32_t)slot );
		nv2a_sgpu_mark_failed( slot, hdr, sub, UINT32_MAX );
		return NULL;
	}
	free( scratch );

	e->hdr = hdr;
	e->sub = sub;
	e->used = 1;
	e->resident = 1;
	e->next = nv2a_sgpu_buckets[b];
	nv2a_sgpu_buckets[b] = (uint16_t)slot;

	nv2a_sgpu_cache_stats.entries++;
	nv2a_sgpu_cache_stats.gpu_used += gpu_bytes;
	nv2a_sgpu_cache_stats.heap_used += heap_bytes;
	nv2a_sgpu_cache_stats.builds++;
	return &e->geo;
}
