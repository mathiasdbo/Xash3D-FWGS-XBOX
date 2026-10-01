/*
xbox_string_fast.c - memcpy, memmove, memset, memcmp and strlen that replace
nxdk pdclib's at link time

Part of compat/xbox; see that directory's README. docs/r3d/divergences.md
#142.

Why: nxdk's prebuilt libraries are compiled with no -O flag at all
(`make NXDK_ONLY=y` passes no CFLAGS, docs/r3d/fork-plan.md:10183-10186), so
pdclib's string functions are -O0 byte loops: memcpy is
`while ( n-- ) *dest++ = *src++;` (deps/nxdk/lib/pdclib/functions/string/
memcpy.c:11-19), which at -O0 keeps n, src and dst in stack slots and runs
about 16 instructions per byte (docs/r3d/reports/perf-research/
xbox-cpu-bound-o2.md, item P1). Every memcpy in the XBE resolved to it
(link map, `libpdclib:memcpy.obj`): the 72-byte TriAPI triangle copy in
ref/nv2a, struct copies the compiler lowers to a call, every buffer copy.

How it wins the link: each of these is a separate member of libpdclib.lib
(memcpy.obj, memset.obj, ...), and this object is a direct linker input
ahead of the archive, so lld never extracts pdclib's members - the same
mechanism compat/xbox/xbox_printf.c uses (divergence #54). The link check
asserts this object's presence and order, and the link map shows where
each symbol came from.

How it is built: like the rest of the engine, -O2 -march=pentium3
-ffreestanding -fno-builtin. -fno-builtin matters here: it stops the
optimiser recognising the byte loops below as "a memcpy" and replacing them
with a call to memcpy - this function.

Choice of method, for a 733 MHz Coppermine Pentium III (P6 core, 32-byte
lines, one load and one store per clock):
  - Agner Fog's instruction tables (Pentium Pro/II/III) list REP MOVS at
    "ca. 6n" uops and REP STOS at "ca. 5n", fast only under conditions
    (large count, aligned operands), and his assembly manual (16.9, 17.8)
    says REP MOVS has a large setup overhead and is not for small blocks,
    while aligned moves in registers are efficient on every processor. A
    dword load plus a dword store is 3 uops.
  - So: below R3D_STR_REP_MIN bytes, an unrolled dword loop (unaligned
    dword loads are legal on x86 and cost only when they cross a cache
    line); from R3D_STR_REP_MIN up, REP MOVSD / REP STOSD after aligning
    the destination to 8, which is where the P6's fast-string mode can
    engage and which avoids the cache-bank conflicts Agner notes for
    operands not aligned by 8.
  - No MMX: a movq loop moves 8 bytes per store but needs EMMS and is a
    second code path to prove; whether it beats REP MOVSD on this core for
    the sizes the frame copies is a hardware measurement (the
    `r3d_membench` command, engine/platform/xbox/xbox_membench.c, gives
    cycles per byte). The thresholds are the tuning knobs for that.

Semantics are exactly C99's (7.21.2.1, 7.21.2.2, 7.21.6.1, 7.21.4.1,
7.21.6.3): memcpy and memset return their first argument, memmove copies as
if through a temporary buffer for any overlap, memcmp compares as unsigned
char. tools/host-tests/string_fast_host_test.c checks every size 0..300 at
every alignment pair against byte-loop references, and every overlap
distance for memmove; the same body runs on the host (x86-64: the inline
assembly uses the same string instructions with 64-bit registers).
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined( R3D_STRING_FAST_HOST_TEST )
/* tools/host-tests/string_fast_host_test.c: the same bodies under their
   own names, beside the host C library's (which is then the reference). */
#define memcpy  r3d_memcpy
#define memmove r3d_memmove
#define memset  r3d_memset
#define memcmp  r3d_memcmp
#define strlen  r3d_strlen
#else
/* compat/xbox/xbox_prelude.h maps the C spellings to __builtin_* so the
   compiler can inline small constant-size copies; the definitions below
   must be the real names. */
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#endif

/* From this many bytes, the string instructions; below it, the register
   loop. A tuning knob - see the header comment. */
#ifndef R3D_STR_REP_MIN
#define R3D_STR_REP_MIN 256u
#endif

/* A dword at any address, and allowed to alias anything: x86 loads and
   stores it directly, and the optimiser may not assume it is aligned or
   that it does not overlap a char access. */
typedef uint32_t __attribute__(( may_alias, aligned( 1 ))) r3d_u32u_t;

#define R3D_LD32( p )    ( *(const r3d_u32u_t *)( p ))
#define R3D_ST32( p, v ) ( *(r3d_u32u_t *)( p ) = ( v ))

/* REP MOVSD / REP STOSD (AT&T spelling movsl/stosl). The direction flag is
   clear on entry to any function under both the MS and SysV ABIs, so these
   run forward. "memory": they read and write through pointers the compiler
   cannot see in the operands. */
static inline void R3D_RepMovsd( unsigned char **d, const unsigned char **s, size_t dwords )
{
	__asm__ __volatile__( "rep movsl" : "+D"( *d ), "+S"( *s ), "+c"( dwords ) : : "memory" );
}

static inline void R3D_RepStosd( unsigned char **d, uint32_t v, size_t dwords )
{
	__asm__ __volatile__( "rep stosl" : "+D"( *d ), "+c"( dwords ) : "a"( v ) : "memory" );
}

/* The forward copy shared by memcpy and the non-overlapping / forward-safe
   memmove. Every dword is loaded before the store that follows it, and
   REP MOVSD is architecturally a sequence of single moves, so this is
   also correct for an overlapping copy with d < s (each store lands only
   on source bytes already read). */
static inline void R3D_CopyForward( unsigned char *d, const unsigned char *s, size_t n )
{
	if( n >= R3D_STR_REP_MIN )
	{
		/* Align the destination to 8: at most 7 bytes. */
		size_t head = (size_t)( -(uintptr_t)d & 7u );

		n -= head;
		while( head-- )
			*d++ = *s++;
		R3D_RepMovsd( &d, &s, n >> 2 );
		n &= 3u;
	}
	else
	{
		while( n >= 16u )
		{
			uint32_t a = R3D_LD32( s ), b = R3D_LD32( s + 4 );

			R3D_ST32( d, a );
			R3D_ST32( d + 4, b );
			a = R3D_LD32( s + 8 );
			b = R3D_LD32( s + 12 );
			R3D_ST32( d + 8, a );
			R3D_ST32( d + 12, b );
			d += 16;
			s += 16;
			n -= 16u;
		}
		while( n >= 4u )
		{
			R3D_ST32( d, R3D_LD32( s ));
			d += 4;
			s += 4;
			n -= 4u;
		}
	}

	while( n-- )
		*d++ = *s++;
}

/* The backward copy for memmove with s < d < s + n: from the end down, so
   each store lands only on source bytes already read (the bytes it covers
   are at least one above the dword just loaded). Overlapping backward
   moves are rare (console scrollback, a few buffers), so this stays a
   plain register loop - no STD/REP/CLD, whose flag handling is slow on
   the P6 and easy to get wrong. */
static inline void R3D_CopyBackward( unsigned char *d, const unsigned char *s, size_t n )
{
	d += n;
	s += n;

	while( n & 3u )
	{
		*--d = *--s;
		n--;
	}
	while( n >= 16u )
	{
		uint32_t a, b;

		d -= 16;
		s -= 16;
		n -= 16u;
		a = R3D_LD32( s + 12 );
		b = R3D_LD32( s + 8 );
		R3D_ST32( d + 12, a );
		R3D_ST32( d + 8, b );
		a = R3D_LD32( s + 4 );
		b = R3D_LD32( s );
		R3D_ST32( d + 4, a );
		R3D_ST32( d, b );
	}
	while( n )
	{
		d -= 4;
		s -= 4;
		n -= 4u;
		R3D_ST32( d, R3D_LD32( s ));
	}
}

void *memcpy( void *restrict s1, const void *restrict s2, size_t n )
{
	R3D_CopyForward( (unsigned char *)s1, (const unsigned char *)s2, n );
	return s1;
}

void *memmove( void *s1, const void *s2, size_t n )
{
	unsigned char *d = (unsigned char *)s1;
	const unsigned char *s = (const unsigned char *)s2;

	/* Compared as integers: relational operators on pointers into
	   different objects are undefined in C. The subtraction is modulo
	   2^N, so "d - s < n" is exactly "s <= d < s + n" - the one case a
	   forward copy would overwrite source bytes before reading them. */
	if( (uintptr_t)d - (uintptr_t)s >= (uintptr_t)n )
		R3D_CopyForward( d, s, n );
	else if( d != s )
		R3D_CopyBackward( d, s, n );
	return s1;
}

void *memset( void *s, int c, size_t n )
{
	unsigned char *d = (unsigned char *)s;
	uint32_t v = (unsigned char)c;

	v |= v << 8;
	v |= v << 16;

	if( n >= R3D_STR_REP_MIN )
	{
		size_t head = (size_t)( -(uintptr_t)d & 7u );

		n -= head;
		while( head-- )
			*d++ = (unsigned char)v;
		R3D_RepStosd( &d, v, n >> 2 );
		n &= 3u;
	}
	else
	{
		while( n >= 16u )
		{
			R3D_ST32( d, v );
			R3D_ST32( d + 4, v );
			R3D_ST32( d + 8, v );
			R3D_ST32( d + 12, v );
			d += 16;
			n -= 16u;
		}
		while( n >= 4u )
		{
			R3D_ST32( d, v );
			d += 4;
			n -= 4u;
		}
	}

	while( n-- )
		*d++ = (unsigned char)v;
	return s;
}

int memcmp( const void *s1, const void *s2, size_t n )
{
	const unsigned char *a = (const unsigned char *)s1;
	const unsigned char *b = (const unsigned char *)s2;

	/* Skip the equal prefix a dword at a time; the first differing dword
	   is then resolved byte by byte, which keeps the result's sign the
	   byte order's (memory order, not the little-endian value's). */
	while( n >= 4u && R3D_LD32( a ) == R3D_LD32( b ))
	{
		a += 4;
		b += 4;
		n -= 4u;
	}
	while( n-- )
	{
		if( *a != *b )
			return (int)*a - (int)*b;
		a++;
		b++;
	}
	return 0;
}

size_t strlen( const char *s )
{
	const char *p = s;

	/* Byte at a time: a word-at-a-time scan would read past the
	   terminator, which is harmless on x86 within a page but is exactly
	   the kind of cleverness a sanitizer or a guard page punishes. At -O2
	   this is two instructions per byte, against pdclib's -O0 stack-slot
	   loop. */
	while( *p )
		p++;
	return (size_t)( p - s );
}
