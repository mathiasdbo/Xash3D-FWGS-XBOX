/*
nv2a_arena.c - see nv2a_arena.h for the contract. A classic first-fit
allocator: one singly-linked free list, kept sorted by offset so a free
only ever has to look at its immediate list neighbours to coalesce, plus
one unsorted list of live allocations (so free() can look an offset up
without the caller having to remember a size). All bookkeeping nodes are
malloc'd heap memory - never the arena's own backing span.
*/
#include "nv2a_arena.h"
#include <stdlib.h>

typedef struct nv2a_arena_block_s
{
	size_t offset;
	size_t size;
	struct nv2a_arena_block_s *next;
} nv2a_arena_block_t;

struct nv2a_arena_s
{
	size_t total_size;
	nv2a_arena_block_t *free_list;  /* sorted ascending by offset */
	nv2a_arena_block_t *alloc_list; /* unsorted, one node per live allocation */
};

nv2a_arena_t *nv2a_arena_init( size_t size )
{
	nv2a_arena_t *arena = (nv2a_arena_t *)malloc( sizeof( nv2a_arena_t ));
	nv2a_arena_block_t *node;

	if( !arena )
		return NULL;

	node = (nv2a_arena_block_t *)malloc( sizeof( nv2a_arena_block_t ));
	if( !node )
	{
		free( arena );
		return NULL;
	}

	node->offset = 0;
	/* A span that isn't itself granule-sized just leaves its remainder
	   bytes forever unreachable - conservative, and no real caller here
	   ever sizes its span to anything but a round number of KiB/MiB. */
	node->size = size - ( size % NV2A_ARENA_GRANULE );
	node->next = NULL;

	arena->total_size = size;
	arena->free_list = node;
	arena->alloc_list = NULL;

	return arena;
}

void nv2a_arena_destroy( nv2a_arena_t *arena )
{
	nv2a_arena_block_t *node, *next;

	if( !arena )
		return;

	for( node = arena->free_list; node; node = next )
	{
		next = node->next;
		free( node );
	}

	for( node = arena->alloc_list; node; node = next )
	{
		next = node->next;
		free( node );
	}

	free( arena );
}

size_t nv2a_arena_alloc( nv2a_arena_t *arena, size_t size )
{
	nv2a_arena_block_t *node, *prev = NULL;
	nv2a_arena_block_t *alloc_node;
	size_t rounded, result;

	if( !arena || size == 0 )
		return NV2A_ARENA_FAILED;

	rounded = ( size + NV2A_ARENA_GRANULE - 1 ) & ~(size_t)( NV2A_ARENA_GRANULE - 1 );

	for( node = arena->free_list; node; prev = node, node = node->next )
	{
		if( node->size >= rounded )
			break;
	}

	if( !node )
		return NV2A_ARENA_FAILED; /* no free block big enough anywhere */

	alloc_node = (nv2a_arena_block_t *)malloc( sizeof( nv2a_arena_block_t ));
	if( !alloc_node )
		return NV2A_ARENA_FAILED; /* room existed; bookkeeping itself is out of heap - fail clean, arena unchanged */

	result = node->offset;
	alloc_node->offset = result;
	alloc_node->size = rounded;
	alloc_node->next = arena->alloc_list;
	arena->alloc_list = alloc_node;

	if( node->size == rounded )
	{
		/* Exact fit: the free node is fully consumed. */
		if( prev )
			prev->next = node->next;
		else
			arena->free_list = node->next;
		free( node );
	}
	else
	{
		node->offset += rounded;
		node->size -= rounded;
	}

	return result;
}

int nv2a_arena_free( nv2a_arena_t *arena, size_t offset )
{
	nv2a_arena_block_t *node, *prev = NULL;
	nv2a_arena_block_t *fprev, *fcur;
	nv2a_arena_block_t *free_node;

	if( !arena )
		return 0;

	for( node = arena->alloc_list; node; prev = node, node = node->next )
	{
		if( node->offset == offset )
			break;
	}

	if( !node )
		return 0; /* not a live allocation of this arena */

	if( prev )
		prev->next = node->next;
	else
		arena->alloc_list = node->next;

	/* Splice into the free list, kept sorted by offset. */
	fprev = NULL;
	fcur = arena->free_list;
	while( fcur && fcur->offset < node->offset )
	{
		fprev = fcur;
		fcur = fcur->next;
	}

	free_node = node; /* reuse the allocation node's storage as the free node */
	free_node->next = fcur;
	if( fprev )
		fprev->next = free_node;
	else
		arena->free_list = free_node;

	/* Coalesce right first - fcur may be absorbed and freed here, before
	   the left-neighbour check below ever looks at free_node->next. */
	if( free_node->next && free_node->offset + free_node->size == free_node->next->offset )
	{
		nv2a_arena_block_t *right = free_node->next;
		free_node->size += right->size;
		free_node->next = right->next;
		free( right );
	}

	/* Then coalesce left - fprev may absorb free_node (now possibly
	   already merged with its own right neighbour) and free it. */
	if( fprev && fprev->offset + fprev->size == free_node->offset )
	{
		fprev->size += free_node->size;
		fprev->next = free_node->next;
		free( free_node );
	}

	return 1;
}

size_t nv2a_arena_used( const nv2a_arena_t *arena )
{
	size_t used = 0;
	const nv2a_arena_block_t *node;

	if( !arena )
		return 0;

	for( node = arena->alloc_list; node; node = node->next )
		used += node->size;

	return used;
}

size_t nv2a_arena_free_bytes( const nv2a_arena_t *arena )
{
	size_t total = 0;
	const nv2a_arena_block_t *node;

	if( !arena )
		return 0;

	for( node = arena->free_list; node; node = node->next )
		total += node->size;

	return total;
}

size_t nv2a_arena_largest_free( const nv2a_arena_t *arena )
{
	size_t largest = 0;
	const nv2a_arena_block_t *node;

	if( !arena )
		return 0;

	for( node = arena->free_list; node; node = node->next )
		if( node->size > largest )
			largest = node->size;

	return largest;
}
