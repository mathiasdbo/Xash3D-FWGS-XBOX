/*
s_dsp_host_test.c - the room DSP's delay times at the 44.1 and 48 kHz mix rates (divergence #173)
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
Upstream's s_dsp.c sizes its delays as seconds * SOUND_11k << room_hires
(2): samples at 44.1 kHz, the rate written into the constant. Divergence
#173 mixes at 48 kHz on the Xbox, and SX_R3D_SetRate scales the base rate
with SOUND_DMA_SPEED. This builds the real s_dsp.c (included below, with
XASH_XBOX 1) against a stub engine (tools/host-tests/s_dsp_stub) and
checks, for every room preset of both tables, at both rates:
- each delay line's length in seconds is the preset's, to the line's own
  granularity (4 samples: the << 2), and at 44.1 kHz it is upstream's
  value to the sample;
- the reverb's modulation periods and the amplitude modulation's
  (sxmod1/2) keep their length in seconds;
- an impulse through room 2 (a mono delay of 20 ms and a stereo delay of
  10 ms) comes back after 20 ms and 10 ms at both rates;
- a change of rate after the delays were set up reallocates them at the
  new length.

  gcc -O2 -Wall -std=gnu99 -DXASH_XBOX=1 -Itools/host-tests/s_dsp_stub -Iengine/client/sound \
      tools/host-tests/s_dsp_host_test.c -lm -o s_dsp_test && ./s_dsp_test
*/

#include <stdio.h>
#include <stdarg.h>
#include "s_dsp.c"

int s_dsp_test_dma_speed = SOUND_44k;
poolhandle_t sndpool;
client_t cl;

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

/* the engine's services, with the meaning s_dsp.c depends on */
void Cvar_RegisterVariable( convar_t *var ) { var->value = (float)atof( var->string ); }
void Cvar_DirectSetValue( convar_t *var, float value )
{
	if( var->value != value )
		SetBits( var->flags, FCVAR_CHANGED );
	var->value = value;
}
void Cvar_DirectSet( convar_t *var, const char *value ) { Cvar_DirectSetValue( var, (float)atof( value )); }
void Cmd_AddRestrictedCommand( const char *name, void ( *fn )( void ), const char *desc ) { (void)name; (void)fn; (void)desc; }
void Cmd_RemoveCommand( const char *name ) { (void)name; }
int Cmd_Argc( void ) { return 0; }
const char *Cmd_Argv( int i ) { (void)i; return ""; }
float Q_atof( const char *s ) { return (float)atof( s ); }
void Con_Printf( const char *fmt, ... ) { (void)fmt; }
double Platform_DoubleTime( void ) { return 0.0; }
static unsigned rng = 1;
int COM_RandomLong( int lo, int hi )
{
	rng = rng * 1664525u + 1013904223u;
	return lo + (int)(( rng >> 8 ) % (unsigned)( hi - lo + 1 ));
}
void *Mem_Calloc( poolhandle_t pool, size_t size ) { (void)pool; return calloc( 1, size ); }
void Mem_Free( void *p ) { free( p ); }

/* a fresh DSP at `rate`, preset `room` of table `table` applied */
static void start( int rate, int table, int room )
{
	portable_samplepair_t z[4];

	SX_Free();
	s_dsp_test_dma_speed = rate;
	SX_Init();
	Cvar_DirectSetValue( &dsp_coeff_table, (float)table );
	SetBits( dsp_coeff_table.flags, FCVAR_CHANGED );
	Cvar_DirectSetValue( &room_type, (float)room );
	room_typeprev = -1;
	memset( z, 0, sizeof( z ));
	SX_RoomFX( z, 4 );
}

/* upstream's length for `seconds` (float, as the preset's) at 44.1 kHz:
   (int)( delay * idsp_dma_speed ) << sxhires, a float product */
static int upstream_samples( float seconds )
{
	return (int)( seconds * SOUND_11k ) << 2;
}

static void check_line( const dly_t *d, float seconds, int rate, const char *what, int room )
{
	const double got = (double)d->delaysamples / rate;

	if( !d->lpdelayline )
	{
		CHECK( seconds == 0.0f );
		return;
	}
	/* within the line's 4-sample granularity of the preset's time */
	if( !( fabs( got - seconds ) < 4.0 / rate ))
		printf( "  room %d %s at %d Hz: %d samples = %.6f s for %.6f s\n", room, what, rate, d->delaysamples, got, (double)seconds );
	CHECK( fabs( got - seconds ) < 4.0 / rate );
	if( rate == SOUND_44k )
		CHECK( d->delaysamples == upstream_samples( seconds ));
	/* the line holds its longest delay at this rate */
	CHECK( d->cdelaysamplesmax >= (size_t)d->delaysamples + 1 );
}

static void test_presets( void )
{
	static const int rates[] = { SOUND_44k, SOUND_48k };
	int r, room, table, lines = 0;

	for( table = 0; table < 2; table++ )
		for( r = 0; r < 2; r++ )
			for( room = 1; room < (int)ARRAYSIZE( rgsxpre ); room++ )
			{
				const sx_preset_t *p = table ? &rgsxpre_hlalpha052[room] : &rgsxpre[room];
				const int rate = rates[r];

				start( rate, table, room );
				CHECK( idsp_dma_speed == ( rate == SOUND_44k ? 11025 : 12000 ));
				check_line( &monodly, Q_min( p->room_delay, MAX_MONO_DELAY ), rate, "mono delay", room );
				check_line( &stereodly, Q_min( p->room_left, MAX_STEREO_DELAY ), rate, "stereo delay", room );
				check_line( &reverbdly[0], Q_min( p->room_size, MAX_REVERB_DELAY ), rate, "reverb 1", room );
				check_line( &reverbdly[1], Q_min( p->room_size * 0.71f, MAX_REVERB_DELAY ), rate, "reverb 2", room );
				if( reverbdly[0].lpdelayline )
				{
					/* the reverbs' modulation periods: 500 and 700 base
					   samples at 11025, in seconds */
					CHECK( fabs( (double)reverbdly[0].mod / rate - 500.0 * 4 / 44100 ) < 4.0 / rate );
					CHECK( fabs( (double)reverbdly[1].mod / rate - 700.0 * 4 / 44100 ) < 4.0 / rate );
					if( rate == SOUND_44k )
						CHECK( reverbdly[0].mod == 2000 && reverbdly[1].mod == 2800 );
				}
				lines += !!monodly.lpdelayline + !!stereodly.lpdelayline + !!reverbdly[0].lpdelayline + !!reverbdly[1].lpdelayline;
			}
	CHECK( lines > 100 );

	/* the amplitude modulation's periods, in samples */
	start( SOUND_44k, 0, 26 );
	CHECK( sxmod1 == 350 && sxmod2 == 450 );
	start( SOUND_48k, 0, 26 );
	CHECK( sxmod1 == 380 && sxmod2 == 489 );
	CHECK( fabs( 380.0 / 48000 - 350.0 / 44100 ) < 1.0 / 48000 && fabs( 489.0 / 48000 - 450.0 / 44100 ) < 1.0 / 48000 );
	printf( "s_dsp: %d delay lines checked across 2 tables x 2 rates x %d rooms\n", lines, (int)ARRAYSIZE( rgsxpre ) - 1 );
}

/* An impulse through room 2: the first echo on the right channel is the
   mono delay's, the first sound on the left after the impulse the stereo
   delay's (it replaces the left channel with its delayed self). */
static void test_impulse( int rate, int *mono_at, int *stereo_at )
{
	static portable_samplepair_t buf[48000 / 4];
	const int n = rate / 4;
	int k;

	start( rate, 0, 2 );
	memset( buf, 0, sizeof( buf ));
	buf[0].left = buf[0].right = 10000;
	for( k = 0; k < n; k += 1024 )
		SX_RoomFX( buf + k, n - k < 1024 ? n - k : 1024 );
	*mono_at = *stereo_at = -1;
	for( k = 1; k < n; k++ )
	{
		if( *mono_at < 0 && buf[k].right )
			*mono_at = k;
		if( *stereo_at < 0 && buf[k].left )
			*stereo_at = k;
	}
}

static void test_impulses( void )
{
	int m44, s44, m48, s48;

	test_impulse( SOUND_44k, &m44, &s44 );
	test_impulse( SOUND_48k, &m48, &s48 );
	printf( "s_dsp: room 2 impulse: mono delay %d samples at 44.1 kHz (%.4f ms), %d at 48 kHz (%.4f ms); stereo delay %d (%.4f ms), %d (%.4f ms)\n",
		m44, m44 * 1000.0 / 44100, m48, m48 * 1000.0 / 48000, s44, s44 * 1000.0 / 44100, s48, s48 * 1000.0 / 48000 );
	CHECK( m44 == upstream_samples( 0.02f ) && s44 == upstream_samples( 0.01f ));
	CHECK( fabs( m48 / 48000.0 - 0.02 ) < 4.0 / 48000 && fabs( s48 / 48000.0 - 0.01 ) < 4.0 / 48000 );
	CHECK( fabs( m48 / 48000.0 - m44 / 44100.0 ) < 4.0 / 44100 && fabs( s48 / 48000.0 - s44 / 44100.0 ) < 4.0 / 44100 );
}

/* a rate change with the delays already set up: SX_R3D_SetRate frees them
   and the next SX_RoomFX sets them up again at the new length */
static void test_switch( void )
{
	portable_samplepair_t z[4];

	start( SOUND_44k, 0, 2 );
	CHECK( monodly.delaysamples == upstream_samples( 0.02f ) && monodly.cdelaysamplesmax == (size_t)upstream_samples( MAX_MONO_DELAY ) + 1 );
	s_dsp_test_dma_speed = SOUND_48k;
	SX_R3D_SetRate();
	CHECK( !monodly.lpdelayline && !stereodly.lpdelayline );
	memset( z, 0, sizeof( z ));
	SX_RoomFX( z, 4 );
	CHECK( monodly.lpdelayline && monodly.delaysamples == ( (int)( 0.02f * 12000 ) << 2 ));
	CHECK( monodly.cdelaysamplesmax == (size_t)(( (int)( MAX_MONO_DELAY * 12000 ) << 2 ) + 1 ));
	CHECK( stereodly.lpdelayline && stereodly.delaysamples == ( (int)( 0.01f * 12000 ) << 2 ));
	/* and back */
	s_dsp_test_dma_speed = SOUND_44k;
	SX_R3D_SetRate();
	SX_RoomFX( z, 4 );
	CHECK( monodly.delaysamples == upstream_samples( 0.02f ) && monodly.cdelaysamplesmax == (size_t)upstream_samples( MAX_MONO_DELAY ) + 1 );
	CHECK( idsp_dma_speed == 11025 && sxmod1 == 350 && sxmod2 == 450 );

	/* the reverb too (room 5, a 50 ms reverb): SX_ReloadRoomFX does not
	   flag its size, so SX_R3D_SetRate must */
	start( SOUND_44k, 0, 5 );
	CHECK( reverbdly[0].lpdelayline && reverbdly[0].delaysamples == upstream_samples( 0.05f ));
	s_dsp_test_dma_speed = SOUND_48k;
	SX_R3D_SetRate();
	SX_RoomFX( z, 4 );
	CHECK( reverbdly[0].lpdelayline && reverbdly[0].delaysamples == ( (int)( 0.05f * 12000 ) << 2 ));
	CHECK( reverbdly[1].lpdelayline && reverbdly[1].delaysamples == ( (int)( 0.05f * 0.71f * 12000 ) << 2 ));
	s_dsp_test_dma_speed = SOUND_44k;
}

int main( void )
{
	setvbuf( stdout, NULL, _IONBF, 0 );
	test_presets();
	test_impulses();
	test_switch();
	SX_Free();
	printf( "s_dsp: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
