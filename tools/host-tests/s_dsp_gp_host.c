/*
s_dsp_gp_host.c - the reference room effect and the GP kernel's parameters, for tools/host-tests/gp_room_kernels_test.py
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
Reads a scenario on stdin: the number of 32-sample blocks, then per block
"room water table off active gain" and 64 room-mix and 64 dry samples
(left, right interleaved). Writes per block the 16 kernel parameters that
SX_R3D_GpParams gives for that block's controls, then the 64 samples the
CPU path makes (S_PaintChannels: SX_RoomFX on the room mix unless the menu
is up, the mix with the master volume, CLIP16) with the previous block's
controls: the kernel applies a frame's parameters from the next frame on
(gp_hldsp.asm). Built from s_dsp_gp_tu.c twice; the test's header has the
commands.
*/
#include <stdio.h>
#include <stdarg.h>
#include "common.h"
#include "sound.h"

int s_dsp_test_dma_speed = SOUND_48k;
poolhandle_t sndpool;

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
int COM_RandomLong( int lo, int hi ) { (void)hi; return lo; } /* only in dead code and dsp_profile */
void *Mem_Calloc( poolhandle_t pool, size_t size ) { (void)pool; return calloc( 1, size ); }
void Mem_Free( void *p ) { free( p ); }

void ref_SX_Init( void );
void ref_SX_RoomFX( portable_samplepair_t *paint, int num_samples );
void ref_control( float room, int water, float table, float off );
void drv_SX_Init( void );
void drv_SX_R3D_GpParams( int *params, qboolean active, int gain );
void drv_control( float room, int water, float table, float off );
extern qboolean drv_sx_r3d_gp;

typedef struct { float room, table, off; int water, active, gain; } control_t;

int main( void )
{
	control_t cur, prev;
	int blocks, m, i;

	if( scanf( "%d", &blocks ) != 1 )
		return 1;
	ref_SX_Init();
	drv_SX_Init();
	drv_sx_r3d_gp = true;

	for( m = 0; m < blocks; m++ )
	{
		portable_samplepair_t room[32];
		int dry[64], params[SXGP_PARAMS];

		if( scanf( "%f %d %f %f %d %d", &cur.room, &cur.water, &cur.table, &cur.off, &cur.active, &cur.gain ) != 6 )
			return 1;
		for( i = 0; i < 32; i++ )
			if( scanf( "%d %d", &room[i].left, &room[i].right ) != 2 )
				return 1;
		for( i = 0; i < 64; i++ )
			if( scanf( "%d", &dry[i] ) != 1 )
				return 1;
		if( m == 0 )
			prev = cur;

		drv_control( cur.room, cur.water, cur.table, cur.off );
		drv_SX_R3D_GpParams( params, cur.active, cur.gain );
		for( i = 0; i < SXGP_PARAMS; i++ )
			printf( "%d ", params[i] );
		printf( "\n" );

		ref_control( prev.room, prev.water, prev.table, prev.off );
		if( prev.active )
			ref_SX_RoomFX( room, 32 );
		for( i = 0; i < 32; i++ )
		{
			int l = dry[2 * i] + (( room[i].left * prev.gain ) >> 8 );
			int r = dry[2 * i + 1] + (( room[i].right * prev.gain ) >> 8 );

			printf( "%d %d ", CLIP16( l ), CLIP16( r ));
		}
		printf( "\n" );
		prev = cur;
	}
	return 0;
}
