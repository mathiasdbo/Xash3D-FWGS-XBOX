/* Host test for compat/xbox/xbox_string_fast.c (docs/r3d/divergences.md
   #142): memcpy, memmove, memset, memcmp and strlen against the host C
   library, which is the reference.

   The implementation is compiled with -DR3D_STRING_FAST_HOST_TEST, which
   names its functions r3d_memcpy... so both sets link into one program.
   It runs on x86-64 here (no 32-bit host toolchain in this tree): the
   same C, and the same `rep movsl`/`rep stosl` with 64-bit registers. The
   i386 object the XBE links is checked on the console itself by the
   `r3d_membench` command (engine/platform/xbox/xbox_membench.c).

   Every size 0..300 at every destination/source alignment 0..7, sizes
   either side of the string-instruction threshold and large blocks, every
   memmove overlap distance -40..40 plus large overlaps, memset fill
   values that are not bytes, memcmp's sign at every differing position,
   and guard bytes around every destination.

   Built and run by tools/host-tests/run.sh (-O2 with trapping UBSan, as
   the Xbox build is -O2):

     gcc $HOST_CFLAGS -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
         -DR3D_STRING_FAST_HOST_TEST -c compat/xbox/xbox_string_fast.c -o sf.o
     gcc $HOST_CFLAGS tools/host-tests/string_fast_host_test.c sf.o -o string_fast_test
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *r3d_memcpy( void *restrict s1, const void *restrict s2, size_t n );
void *r3d_memmove( void *s1, const void *s2, size_t n );
void *r3d_memset( void *s, int c, size_t n );
int r3d_memcmp( const void *s1, const void *s2, size_t n );
size_t r3d_strlen( const char *s );

static int failures, checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { if( failures++ < 20 ) { fprintf( stderr, "%s:%d: ", __FILE__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); fputc( '\n', stderr ); } } } while( 0 )

#define PAD   64
#define BIG   ( 70000 + 2 * PAD + 16 )

static unsigned char *A, *B, *S;

static void fill( unsigned char *p, size_t n, uint32_t seed )
{
	size_t i;

	for( i = 0; i < n; i++ )
	{
		seed = seed * 1103515245u + 12345u;
		p[i] = (unsigned char)( seed >> 16 );
	}
}

static int sign( int v )
{
	return ( v > 0 ) - ( v < 0 );
}

/* memcpy of n bytes at the given alignments: the whole buffer, guards
   included, must equal the C library's result. */
static void check_copy( size_t n, size_t da, size_t sa )
{
	size_t span = n + 2 * PAD + 16;
	void *r;

	fill( A, span, (uint32_t)( 7 + n ));
	memcpy( B, A, span );
	fill( S, span, (uint32_t)( 3 + n + da ));
	r = r3d_memcpy( A + PAD + da, S + PAD + sa, n );
	memcpy( B + PAD + da, S + PAD + sa, n );
	CHECK( r == A + PAD + da, "memcpy return n=%zu", n );
	CHECK( !memcmp( A, B, span ), "memcpy n=%zu da=%zu sa=%zu", n, da, sa );
}

static void check_set( size_t n, size_t da, int c )
{
	size_t span = n + 2 * PAD + 16;
	void *r;

	fill( A, span, (uint32_t)( 9 + n ));
	memcpy( B, A, span );
	r = r3d_memset( A + PAD + da, c, n );
	memset( B + PAD + da, c, n );
	CHECK( r == A + PAD + da, "memset return n=%zu", n );
	CHECK( !memcmp( A, B, span ), "memset n=%zu da=%zu c=%d", n, da, c );
}

/* memmove within one buffer, destination `dist` bytes from the source. */
static void check_move( size_t n, long dist, size_t sa )
{
	size_t span = n + 2 * PAD + 2 * 1024 + 16;
	unsigned char *src = A + PAD + 1024 + sa, *bsrc = B + PAD + 1024 + sa;
	void *r;

	fill( A, span, (uint32_t)( 11 + n + (size_t)dist ));
	memcpy( B, A, span );
	r = r3d_memmove( src + dist, src, n );
	memmove( bsrc + dist, bsrc, n );
	CHECK( r == src + dist, "memmove return n=%zu dist=%ld", n, dist );
	CHECK( !memcmp( A, B, span ), "memmove n=%zu dist=%ld sa=%zu", n, dist, sa );
}

static void check_cmp( size_t n, size_t xa, size_t ya )
{
	unsigned char *x = A + PAD + xa, *y = B + PAD + ya;
	size_t i;

	fill( x, n, (uint32_t)( 13 + n ));
	fill( y, n, (uint32_t)( 13 + n ));
	CHECK( r3d_memcmp( x, y, n ) == 0, "memcmp equal n=%zu", n );
	for( i = 0; i < n; i++ )
	{
		unsigned char keep = y[i];
		int k;

		for( k = 0; k < 2; k++ )
		{
			/* Both ways round, and with a difference in the top bit:
			   memcmp compares as unsigned char. */
			y[i] = (unsigned char)( x[i] + ( k ? 0x80 : 1 ));
			CHECK( sign( r3d_memcmp( x, y, n )) == sign( memcmp( x, y, n )), "memcmp n=%zu at %zu k=%d", n, i, k );
			CHECK( sign( r3d_memcmp( y, x, n )) == sign( memcmp( y, x, n )), "memcmp rev n=%zu at %zu k=%d", n, i, k );
		}
		y[i] = keep;
	}
	/* A difference in a later byte must not decide when an earlier one
	   differs the other way (the dword prefix scan must not compare
	   little-endian values). */
	if( n >= 4 )
	{
		fill( x, n, 5u );
		memcpy( y, x, n );
		x[0] = 1; y[0] = 2;
		x[3] = 9; y[3] = 0;
		CHECK( r3d_memcmp( x, y, n ) < 0, "memcmp byte order n=%zu", n );
	}
}

static void check_strlen( size_t n, size_t da )
{
	char *s = (char *)A + PAD + da;
	size_t i;

	for( i = 0; i < n; i++ )
		s[i] = (char)( 1 + ( i * 7 ) % 255 );
	s[n] = 0;
	s[n + 1] = 'x';
	CHECK( r3d_strlen( s ) == n, "strlen n=%zu da=%zu", n, da );
}

int main( void )
{
	static const size_t big_sizes[] = { 255, 256, 257, 259, 260, 263, 264, 300, 511, 512, 1000, 1024, 4099, 65536, 65541, 70000 };
	static const long big_dists[] = { -1000, -64, -8, -7, -4, -3, -1, 1, 3, 4, 7, 8, 64, 1000 };
	static const int fills[] = { 0, 0xFF, 0x80, 0x5A, 0x1A5, -1, -128 };
	size_t n, da, sa, i, j;
	long dist;

	A = malloc( BIG + 4096 );
	B = malloc( BIG + 4096 );
	S = malloc( BIG + 4096 );
	if( !A || !B || !S )
		return 2;

	for( n = 0; n <= 300; n++ )
	{
		for( da = 0; da < 8; da++ )
		{
			for( sa = 0; sa < 8; sa++ )
				check_copy( n, da, sa );
			for( j = 0; j < sizeof( fills ) / sizeof( fills[0] ); j++ )
				check_set( n, da, fills[j] );
			check_strlen( n, da );
		}
		for( dist = -40; dist <= 40; dist++ )
			for( sa = 0; sa < 4; sa++ )
				check_move( n, dist, sa );
		for( da = 0; da < 4; da++ )
			check_cmp( n, da, 3 - da );
	}

	for( i = 0; i < sizeof( big_sizes ) / sizeof( big_sizes[0] ); i++ )
	{
		n = big_sizes[i];
		for( da = 0; da < 8; da += 3 )
		{
			for( sa = 0; sa < 8; sa += 1 )
				check_copy( n, da, sa );
			check_set( n, da, 0x5A );
			check_set( n, da, 0x1A5 );
		}
		for( j = 0; j < sizeof( big_dists ) / sizeof( big_dists[0] ); j++ )
			for( sa = 0; sa < 4; sa++ )
				check_move( n, big_dists[j], sa );
		check_move( n, 0, 0 );
		check_strlen( n, 1 );
		if( n <= 1024 )
			check_cmp( n, 1, 2 );
	}

	free( A );
	free( B );
	free( S );
	if( failures )
	{
		fprintf( stderr, "string_fast: %d of %d checks failed\n", failures, checks );
		return 1;
	}
	printf( "string_fast: %d checks passed\n", checks );
	return 0;
}
