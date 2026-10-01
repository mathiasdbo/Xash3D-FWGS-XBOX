/*
xbox_membench.c - `r3d_membench`: the C library's memory functions, checked
and timed on the console
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #142). Whatever memcpy, memmove,
memset, memcmp and strlen the XBE links - nxdk pdclib's -O0 byte loops
before #142, compat/xbox/xbox_string_fast.c after - this command first
checks them against byte-loop references on the console itself (every
size 0..300 at every alignment pair, every memmove overlap distance up to
+-40), then times them with the time-stamp counter at the sizes the frame
uses: 72 bytes (one TriAPI triangle, ref/nv2a nv2a_2d_append), 1 KiB and
64 KiB.

The host test (tools/host-tests/string_fast_host_test.c) proves the C;
this proves the i386 object the XBE actually contains, which no host here
can run. The timings are what tunes R3D_STR_REP_MIN, and on the console
they are Pentium III cycles; in xemu they are the emulator's and only
their ratios between two builds mean anything.

Allocates its buffers for the duration of the command only. Its output also
goes to membench.txt (MB_Print below).
*/

#include "common.h"
#include "xbox_frametime.h"
#include <xboxkrnl/xboxkrnl.h>

#define MB_SMALL_MAX  300   /* every size 0..this, at every alignment pair */
#define MB_ALIGNS     8
#define MB_OVERLAP    40    /* memmove: destination - source in -this..this */
#define MB_PAD        64
#define MB_BIG        65536

/* Every line goes to the console and is appended to membench.txt in the
   engine's write directory (E:\UDATA\<title id>\valve\valve\ on the
   console), so a run can be read back without a screen: in xemu, with
   xemu stopped, through tools/xemu_hdd.py. */
static file_t *mb_file;

static void MB_Print( const char *fmt, ... )
{
	char line[256];
	va_list args;

	va_start( args, fmt );
	Q_vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );
	Con_Printf( "%s", line );
	if( mb_file )
		FS_Write( mb_file, line, Q_strlen( line ));
}

/* References. noinline and plain loops: the build's -fno-builtin keeps
   the compiler from turning them back into calls to what they check. */
static __attribute__(( noinline )) void MB_RefCopy( byte *d, const byte *s, size_t n )
{
	size_t i;

	if( d < s )
	{
		for( i = 0; i < n; i++ )
			d[i] = s[i];
	}
	else
	{
		for( i = n; i > 0; i-- )
			d[i - 1] = s[i - 1];
	}
}

static void MB_Pattern( byte *p, size_t n, uint32_t seed )
{
	size_t i;

	for( i = 0; i < n; i++ )
	{
		seed = seed * 1103515245u + 12345u;
		p[i] = (byte)( seed >> 16 );
	}
}

static int MB_Same( const byte *a, const byte *b, size_t n )
{
	size_t i;

	for( i = 0; i < n; i++ )
	{
		if( a[i] != b[i] )
			return 0;
	}
	return 1;
}

static int MB_Sign( int v )
{
	return ( v > 0 ) - ( v < 0 );
}

/* Returns the number of failed cases; prints the first few. */
static int MB_Check( byte *buf )
{
	const size_t span = MB_SMALL_MAX + 2 * MB_PAD;
	byte *a = buf, *b = buf + span, *src = buf + 2 * span;
	int fail = 0;
	size_t n, da, sa, i;
	int dist;

	MB_Pattern( src, span, 1u );

	/* memcpy and memset: a buffer copied with the function under test and
	   one with the reference must be identical, guard bytes included, and
	   the return value must be the destination. */
	for( n = 0; n <= MB_SMALL_MAX; n++ )
	{
		for( da = 0; da < MB_ALIGNS; da++ )
		{
			for( sa = 0; sa < MB_ALIGNS; sa++ )
			{
				MB_Pattern( a, span, 7u );
				MB_RefCopy( b, a, span );
				if( memcpy( a + MB_PAD + da, src + MB_PAD + sa, n ) != a + MB_PAD + da )
					fail++;
				MB_RefCopy( b + MB_PAD + da, src + MB_PAD + sa, n );
				if( !MB_Same( a, b, span ) && fail++ < 4 )
					MB_Print( S_ERROR "membench: memcpy n=%u da=%u sa=%u\n", (uint)n, (uint)da, (uint)sa );
			}

			MB_Pattern( a, span, 9u );
			MB_RefCopy( b, a, span );
			if( memset( a + MB_PAD + da, 0xA5 ^ (int)n, n ) != a + MB_PAD + da )
				fail++;
			for( i = 0; i < n; i++ )
				b[MB_PAD + da + i] = (byte)( 0xA5 ^ n );
			if( !MB_Same( a, b, span ) && fail++ < 4 )
				MB_Print( S_ERROR "membench: memset n=%u da=%u\n", (uint)n, (uint)da );
		}

		/* memmove at every overlap distance, both directions. */
		for( dist = -MB_OVERLAP; dist <= MB_OVERLAP; dist++ )
		{
			MB_Pattern( a, span, 11u + (uint32_t)n );
			MB_RefCopy( b, a, span );
			if( memmove( a + MB_PAD + dist, a + MB_PAD, n ) != a + MB_PAD + dist )
				fail++;
			MB_RefCopy( b + MB_PAD + dist, b + MB_PAD, n );
			if( !MB_Same( a, b, span ) && fail++ < 4 )
				MB_Print( S_ERROR "membench: memmove n=%u dist=%d\n", (uint)n, dist );
		}

		/* memcmp: equal, then one byte changed at each position, both
		   ways round, at a few alignments; the sign is unsigned-char
		   order of the first differing byte. */
		for( da = 0; da < 4; da++ )
		{
			byte *x = a + MB_PAD + da, *y = b + MB_PAD + ( 3 - da );

			MB_Pattern( x, n, 13u );
			MB_Pattern( y, n, 13u );
			if( memcmp( x, y, n ) != 0 && fail++ < 4 )
				MB_Print( S_ERROR "membench: memcmp equal n=%u\n", (uint)n );
			for( i = 0; i < n; i += 1 + n / 16 )
			{
				byte keep = y[i];

				y[i] = (byte)( x[i] + 0x80 );
				if( MB_Sign( memcmp( x, y, n )) != MB_Sign( (int)x[i] - (int)y[i] ) && fail++ < 4 )
					MB_Print( S_ERROR "membench: memcmp n=%u at %u\n", (uint)n, (uint)i );
				y[i] = keep;
			}
		}

		/* strlen at every alignment. */
		for( da = 0; da < MB_ALIGNS; da++ )
		{
			char *s = (char *)a + MB_PAD + da;

			for( i = 0; i < n; i++ )
				s[i] = (char)( 1 + ( i % 250 ));
			s[n] = 0;
			if( strlen( s ) != n && fail++ < 4 )
				MB_Print( S_ERROR "membench: strlen n=%u da=%u\n", (uint)n, (uint)da );
		}
	}
	return fail;
}

typedef struct
{
	const char *name;
	size_t size;
	uint32_t iters;
} mb_case_t;

static void MemBench_f( void )
{
	static const mb_case_t cases[] =
	{
		{ "72 B", 72, 20000 },
		{ "1 KiB", 1024, 2000 },
		{ "64 KiB", MB_BIG, 40 },
	};
	const size_t check_bytes = 3 * ( MB_SMALL_MAX + 2 * MB_PAD );
	const size_t bench_bytes = 2 * ( MB_BIG + MB_PAD );
	byte *buf = Z_Malloc( Q_max( check_bytes, bench_bytes ));
	volatile size_t vsize; /* keeps every call a call with a run-time size */
	uint64_t t0, t1, q0, q1, tsc_hz;
	uint64_t ticks[3][3];
	int fail, c, f;
	uint32_t i;

	mb_file = FS_Open( "membench.txt", "a", true );
	fail = MB_Check( buf );
	MB_Print( "membench: correctness %s (%d failed case(s)): sizes 0..%d, %d x %d alignments, memmove distances -%d..%d\n",
		fail ? "FAILED" : "ok", fail, MB_SMALL_MAX, MB_ALIGNS, MB_ALIGNS, MB_OVERLAP, MB_OVERLAP );

	/* Timing: destination and source dword-aligned, as the engine's
	   buffers are; memmove forward without overlap (its common case). */
	MB_Pattern( buf, bench_bytes, 3u );
	q0 = KeQueryPerformanceCounter();
	t0 = xbox_ft_rdtsc();
	for( c = 0; c < 3; c++ )
	{
		byte *d = buf, *s = buf + MB_BIG + MB_PAD;

		for( f = 0; f < 3; f++ )
		{
			uint64_t a = xbox_ft_rdtsc();

			for( i = 0; i < cases[c].iters; i++ )
			{
				vsize = cases[c].size;
				if( f == 0 )
					memcpy( d, s, vsize );
				else if( f == 1 )
					memmove( d, s, vsize );
				else
					memset( d, (int)i, vsize );
			}
			ticks[c][f] = xbox_ft_rdtsc() - a;
		}
	}
	t1 = xbox_ft_rdtsc();
	q1 = KeQueryPerformanceCounter();
	Z_Free( buf );

	/* The TSC rate over this run, against the kernel's counter. */
	tsc_hz = ( q1 > q0 ) ? (uint64_t)(( (double)( t1 - t0 ) * (double)KeQueryPerformanceFrequency()) / (double)( q1 - q0 )) : 733333333u;
	MB_Print( "membench: TSC %.1f MHz over the run; ticks per call / per byte:\n", (double)tsc_hz / 1e6 );
	for( c = 0; c < 3; c++ )
	{
		double calls = (double)cases[c].iters, bytes = calls * (double)cases[c].size;

		MB_Print( "membench: %-6s memcpy %9.1f %6.3f  memmove %9.1f %6.3f  memset %9.1f %6.3f\n", cases[c].name,
			(double)ticks[c][0] / calls, (double)ticks[c][0] / bytes,
			(double)ticks[c][1] / calls, (double)ticks[c][1] / bytes,
			(double)ticks[c][2] / calls, (double)ticks[c][2] / bytes );
	}
	if( mb_file )
	{
		FS_Close( mb_file );
		mb_file = NULL;
	}
}

void Xbox_MemBench_Init( void )
{
	Cmd_AddCommand( "r3d_membench", MemBench_f, "check and time memcpy/memmove/memset/memcmp/strlen (docs/r3d/divergences.md #142)" );
}
