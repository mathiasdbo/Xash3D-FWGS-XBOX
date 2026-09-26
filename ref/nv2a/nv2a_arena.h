/*
nv2a_arena.h - X9-X Part C: a first-fit sub-allocator over a caller-owned
contiguous span of bytes (offsets only - this module never reads or
writes the span itself, so it is safe to use over write-combined GPU
memory). ref/nv2a's own texture arena (D1, r_context.c/r_image.c) is its
first and only caller today.

Deliberately free of pbkit, xboxkrnl and engine includes so it builds and
is unit-tested on the host - see tools/host-tests/nv2a_arena_host_test.c
for the exact gcc command line and every case this is checked against.
xbox-ref-isolate.sh globs every ref/nv2a source file, so this one is
compiled into the Xbox module exactly like every other one, no build-list
change needed.
*/
#ifndef NV2A_ARENA_H
#define NV2A_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every offset this allocator hands out, and every size it rounds
   requests up to, is a multiple of this many bytes - matching the
   128-byte alignment the GPU's own texture OFFSET field expects
   (nv2a_bind_map_texture_stage0's own `phys & 0x03ffffff` pattern
   needs no extra masking when fed one of these offsets, since 128 is
   already a multiple of the hardware's own minimum alignment). */
#define NV2A_ARENA_GRANULE 128u

#define NV2A_ARENA_FAILED ((size_t)-1)

typedef struct nv2a_arena_s nv2a_arena_t;

/* `size`: the total span, in bytes, this arena sub-allocates offsets
   within. Returns NULL only if the small, fixed bookkeeping allocation
   itself fails (a real, if unlikely, host/console heap exhaustion) -
   the caller's own contiguous-memory allocation for the span is a
   separate, already-required failure path. */
nv2a_arena_t *nv2a_arena_init( size_t size );
void nv2a_arena_destroy( nv2a_arena_t *arena );

/* First-fit: returns the lowest-offset free block's start that is at
   least `size` bytes (rounded up to NV2A_ARENA_GRANULE), or
   NV2A_ARENA_FAILED if none is large enough. Always fails for size 0 -
   no caller here has a real zero-size case; one that does must special
   case it itself rather than silently receive offset 0. */
size_t nv2a_arena_alloc( nv2a_arena_t *arena, size_t size );

/* Frees a previously returned offset, coalescing with either
   neighbouring free block. Returns false, changing nothing, for an
   offset this arena did not itself hand out (already freed, or never
   allocated) - the caller decides whether that is a bug worth
   asserting on. */
int nv2a_arena_free( nv2a_arena_t *arena, size_t offset );

size_t nv2a_arena_used( const nv2a_arena_t *arena );
size_t nv2a_arena_free_bytes( const nv2a_arena_t *arena );
size_t nv2a_arena_largest_free( const nv2a_arena_t *arena );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_ARENA_H */
