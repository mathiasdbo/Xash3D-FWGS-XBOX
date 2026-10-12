/*
xbox_s_mix_bench.c - `r3d_mixbench`: the mixer's point and interpolating loops, timed on the console
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
Resonance3D (docs/r3d/divergences.md #174). s_lerping's interpolation
costs more per output than the point loop; how much on a Pentium III is a
property of the i386 code in the XBE, which no host here can run
(tools/host-tests/s_mix_bench.c times a host build). xemu does not run the
audio callback, so a route there mixes almost nothing and cannot show it
either. This command calls the XBE's own loops (S_R3D_MixLoops,
xbox_s_mix_pitch.h) on the calls a channel makes - PAINTBUFFER_SIZE
outputs of an 11 or 22 kHz sound at the current mix rate - and prints
time-stamp-counter ticks per output: point, interpolated, their ratio,
and the point loop at upstream's 44.1 kHz ratios (#145's run path).

On the console the ticks are Pentium III cycles; in xemu they are the
emulator's and only the ratios mean anything. Output also goes to
mixbench.txt in the write directory (E:\UDATA\<title id>\valve\valve\),
read back in xemu through tools/xemu_hdd.py with xemu stopped.
*/

#include "common.h"
#include "sound.h"
#include "xbox_frametime.h"

typedef void ( *s_r3d_mix_loop_t )( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples );
void S_R3D_MixLoops( s_r3d_mix_loop_t pitch[4], s_r3d_mix_loop_t lerp[4] );

#define MXB_SOURCE  32768 /* 16-bit samples: enough for PAINTBUFFER_SIZE stereo outputs at any rate below */
#define MXB_CALLS   200
#define MXB_ROUNDS  3

static file_t *mxb_file;

static void MXB_Print( const char *fmt, ... )
{
	char line[256];
	va_list args;

	va_start( args, fmt );
	Q_vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );
	Con_Printf( "%s", line );
	if( mxb_file )
		FS_Write( mxb_file, line, Q_strlen( line ));
}

/* ticks per output, the best of MXB_ROUNDS rounds of MXB_CALLS calls */
static double MXB_Time( s_r3d_mix_loop_t fn, portable_samplepair_t *out, const void *src, double rate )
{
	static const int vol[2] = { 200, 180 };
	double best = 1e30;
	int round, c;

	for( round = 0; round < MXB_ROUNDS; round++ )
	{
		const uint64_t t0 = xbox_ft_rdtsc();

		for( c = 0; c < MXB_CALLS; c++ )
			fn( out, vol, src, ( c & 7 ) * 0.125, rate, PAINTBUFFER_SIZE ); // the fraction a channel carries
		{
			const double t = (double)( xbox_ft_rdtsc() - t0 ) / ( (double)MXB_CALLS * PAINTBUFFER_SIZE );

			if( t < best )
				best = t;
		}
	}
	return best;
}

static void MixBench_f( void )
{
	static const struct { int src_rate, stereo, width; const char *name; } cases[] =
	{
		{ 11025, 0, 1, "11 kHz  8-bit mono  " },
		{ 22050, 0, 1, "22 kHz  8-bit mono  " },
		{ 11025, 0, 2, "11 kHz 16-bit mono  " },
		{ 22050, 1, 2, "22 kHz 16-bit stereo" },
	};
	s_r3d_mix_loop_t pitch[4], lerp[4];
	int16_t *src = Z_Malloc( MXB_SOURCE * sizeof( int16_t ));
	portable_samplepair_t *out = Z_Malloc(( PAINTBUFFER_SIZE + 1 ) * sizeof( portable_samplepair_t ));
	uint32_t seed = 12345u;
	int i, c;

	for( i = 0; i < MXB_SOURCE; i++ )
	{
		seed = seed * 1664525u + 1013904223u;
		src[i] = (int16_t)( seed >> 16 );
	}
	S_R3D_MixLoops( pitch, lerp );
	mxb_file = FS_Open( "mixbench.txt", "a", true );
	MXB_Print( "mixbench: ticks per output, %d calls of %d outputs, best of %d; mix rate %d Hz\n",
		MXB_CALLS, PAINTBUFFER_SIZE, MXB_ROUNDS, SOUND_DMA_SPEED );
	MXB_Print( "mixbench: source                 point   lerp  lerp/point   point at 44.1k ratio\n" );
	for( c = 0; c < (int)( sizeof( cases ) / sizeof( cases[0] )); c++ )
	{
		const int k = cases[c].stereo * 2 + ( cases[c].width - 1 );
		const double rate = (double)cases[c].src_rate / SOUND_DMA_SPEED;
		const double p = MXB_Time( pitch[k], out, src, rate );
		const double l = MXB_Time( lerp[k], out, src, rate );
		const double r44 = MXB_Time( pitch[k], out, src, (double)cases[c].src_rate / SOUND_44k );

		MXB_Print( "mixbench: %s %7.2f %7.2f %8.2fx %10.2f\n", cases[c].name, p, l, l / p, r44 );
	}
	if( mxb_file )
	{
		FS_Close( mxb_file );
		mxb_file = NULL;
	}
	Z_Free( out );
	Z_Free( src );
}

void Xbox_MixBench_Init( void )
{
	Cmd_AddCommand( "r3d_mixbench", MixBench_f, "time the mixer's point and interpolating loops (docs/r3d/divergences.md #174)" );
}
