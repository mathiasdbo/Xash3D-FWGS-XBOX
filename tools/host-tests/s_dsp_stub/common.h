/*
s_dsp_stub/common.h - the engine surface engine/client/sound/s_dsp.c compiles against, for its host test
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
tools/host-tests/s_dsp_host_test.c includes the real s_dsp.c; this
directory comes first on its include path and stands in for common.h,
client.h and sound.h (the other two are empty). The cvar calls keep the
engine's meaning where s_dsp.c depends on it: registering parses the
default string into the value (engine/common/cvar.c), and setting a
different value raises FCVAR_CHANGED (Cvar_Changed, cvar.c). Values of the
flags themselves do not matter to the test.
*/

#ifndef S_DSP_STUB_COMMON_H
#define S_DSP_STUB_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

typedef int qboolean;
typedef void *poolhandle_t;
#define false 0
#define true 1

#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( 0[p] ))
#define Q_min( a, b ) (((a) < (b)) ? (a) : (b))
#define Q_max( a, b ) (((a) > (b)) ? (a) : (b))
#define bound( min, num, max ) (( num ) >= ( min ) ? (( num ) < ( max ) ? ( num ) : ( max )) : ( min ))
#define SetBits( iBitVector, bits ) (( iBitVector ) = ( iBitVector ) | ( bits ))
#define ClearBits( iBitVector, bits ) (( iBitVector ) = ( iBitVector ) & ~( bits ))
#define FBitSet( iBitVector, bit ) (( iBitVector ) & ( bit ))

/* common/cvardef.h, the parts s_dsp.c uses */
#define FCVAR_ARCHIVE (1 << 0)
#define FCVAR_CHANGED (1 << 30)
typedef struct convar_s
{
	char *name;
	char *string;
	uint32_t flags;
	float value;
	struct convar_s *next;
	char *desc;
	char *def_string;
} convar_t;
#define CVAR_DEFINE( cv, cvname, cvstr, cvflags, cvdesc ) \
	convar_t cv = { (char *)cvname, (char *)cvstr, cvflags, 0.0f, NULL, (char *)cvdesc, NULL }
#define CVAR_DEFINE_AUTO( cv, cvstr, cvflags, cvdesc ) CVAR_DEFINE( cv, #cv, cvstr, cvflags, cvdesc )

void Cvar_RegisterVariable( convar_t *var );
void Cvar_DirectSetValue( convar_t *var, float value );
void Cvar_DirectSet( convar_t *var, const char *value );
void Cmd_AddRestrictedCommand( const char *name, void ( *fn )( void ), const char *desc );
void Cmd_RemoveCommand( const char *name );
int Cmd_Argc( void );
const char *Cmd_Argv( int i );
float Q_atof( const char *s );
void Con_Printf( const char *fmt, ... );
double Platform_DoubleTime( void );
int COM_RandomLong( int lo, int hi );
void *Mem_Calloc( poolhandle_t pool, size_t size );
void Mem_Free( void *p );
extern poolhandle_t sndpool;

#endif /* S_DSP_STUB_COMMON_H */
