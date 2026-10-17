/*
vox_stub/common.h - the engine surface the sound files compile against, for xbox_vox_stream_host_test
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
tools/host-tests/xbox_vox_stream_host_test_tu.c includes the real
s_vox.c, s_mix.c, s_mouth.c, s_load.c, snd_wav.c, snd_main.c, a part of
s_main.c (extract_c_functions.py) and, in the Xbox build,
xbox_vox_stream.c. This directory comes first on its include path and
stands in for engine/common/common.h (and client.h, and the overlapped
read layer). The real public/ and common/ headers give the types -
sound_api.h's channel_t and sfx_t, the real layouts - and wavdata_t is
copied from engine/common/common.h with its flags. The engine services
(memory, files, cvars, the console) are the test's
(xbox_vox_stream_host_test.c).

The two builds of the same sources link into one binary: every function
the sources define, and the per-build state the test compares (snd, the
client entities), is renamed by VOX_NAME to up_* or xb_*.
*/

#ifndef VOX_STUB_COMMON_H
#define VOX_STUB_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "xash3d_types.h"
#include "crtlib.h"
#include "crclib.h"
#include "cvardef.h"
#include "com_strings.h"
#include "const.h"

/* build.h undefines XASH_XBOX for a host build: the test picks it */
#undef XASH_XBOX
#define XASH_XBOX VOX_TEST_XBOX

/* ---- the two builds' names (not in the test's own file) ---- */
#ifndef VOX_TEST_MAIN
#if VOX_TEST_XBOX
#define VOX_NAME( x ) xb_##x
#else
#define VOX_NAME( x ) up_##x
#endif
#define VOX_Init                 VOX_NAME( VOX_Init )
#define VOX_Shutdown             VOX_NAME( VOX_Shutdown )
#define VOX_SetChanVol           VOX_NAME( VOX_SetChanVol )
#define VOX_LoadSound            VOX_NAME( VOX_LoadSound )
#define VOX_R3D_SentenceCount    VOX_NAME( VOX_R3D_SentenceCount )
#define VOX_ModifyPitch          VOX_NAME( VOX_ModifyPitch )
#define VOX_LoadWord             VOX_NAME( VOX_LoadWord )
#define VOX_FreeWord             VOX_NAME( VOX_FreeWord )
#define S_ClearBuffers           VOX_NAME( S_ClearBuffers )
#define S_PaintChannels          VOX_NAME( S_PaintChannels )
#define SND_MoveMouth8           VOX_NAME( SND_MoveMouth8 )
#define SND_MoveMouth16          VOX_NAME( SND_MoveMouth16 )
#define SND_ForceInitMouth       VOX_NAME( SND_ForceInitMouth )
#define SND_ForceCloseMouth      VOX_NAME( SND_ForceCloseMouth )
#define SND_MoveMouthRaw         VOX_NAME( SND_MoveMouthRaw )
#define S_SoundList_f            VOX_NAME( S_SoundList_f )
#define S_LoadSound              VOX_NAME( S_LoadSound )
#define S_FindName               VOX_NAME( S_FindName )
#define S_FreeSound              VOX_NAME( S_FreeSound )
#define S_BeginRegistration      VOX_NAME( S_BeginRegistration )
#define S_EndRegistration        VOX_NAME( S_EndRegistration )
#define S_RegisterSound          VOX_NAME( S_RegisterSound )
#define S_GetSfxByHandle         VOX_NAME( S_GetSfxByHandle )
#define S_InitSounds             VOX_NAME( S_InitSounds )
#define S_FreeSounds             VOX_NAME( S_FreeSounds )
#define S_RetrieveAudioSamples   VOX_NAME( S_RetrieveAudioSamples )
#define S_FreeChannel            VOX_NAME( S_FreeChannel )
#define SND_PickDynamicChannel   VOX_NAME( SND_PickDynamicChannel )
#define SND_PickStaticChannel    VOX_NAME( SND_PickStaticChannel )
#define S_StartSound             VOX_NAME( S_StartSound )
#define S_RestoreSound           VOX_NAME( S_RestoreSound )
#define S_GetCurrentDynamicSounds VOX_NAME( S_GetCurrentDynamicSounds )
#define S_GetCurrentStaticSounds VOX_NAME( S_GetCurrentStaticSounds )
#define S_StopAllSounds          VOX_NAME( S_StopAllSounds )
#define Sound_LoadWAV            VOX_NAME( Sound_LoadWAV )
#define Stream_OpenWAV           VOX_NAME( Stream_OpenWAV )
#define Stream_ReadWAV           VOX_NAME( Stream_ReadWAV )
#define Stream_SetPosWAV         VOX_NAME( Stream_SetPosWAV )
#define Stream_GetPosWAV         VOX_NAME( Stream_GetPosWAV )
#define Stream_FreeWAV           VOX_NAME( Stream_FreeWAV )
#define FS_LoadSound             VOX_NAME( FS_LoadSound )
#define FS_FreeSound             VOX_NAME( FS_FreeSound )
#define FS_OpenStream            VOX_NAME( FS_OpenStream )
#define FS_ReadStream            VOX_NAME( FS_ReadStream )
#define FS_GetStreamPos          VOX_NAME( FS_GetStreamPos )
#define FS_SetStreamPos          VOX_NAME( FS_SetStreamPos )
#define FS_FreeStream            VOX_NAME( FS_FreeStream )
#define CL_GetEntityByIndex      VOX_NAME( CL_GetEntityByIndex )
#define S_R3D_MixLoops           VOX_NAME( S_R3D_MixLoops ) /* xbox_s_mix_pitch.h's, in both builds */
#if !VOX_TEST_XBOX
#define S_R3D_VoxMixSource       VOX_NAME( S_R3D_VoxMixSource ) /* the upstream build's stand-in (the test TU) */
#endif
#define snd                      VOX_NAME( snd )

#define sound                    VOX_NAME( sound )
#endif /* VOX_TEST_MAIN */

/* ---- engine/common/common.h (and com_model.h, protocol.h), the parts the sound files use ---- */

#define NUM_AMBIENTS 4
#define SND_STOP         ( 1 << 5 )
#define SND_CHANGE_VOL   ( 1 << 6 )
#define SND_CHANGE_PITCH ( 1 << 7 )
#define SND_SPAWNING     ( 1 << 8 )
#define SND_LOCALSOUND   ( 1 << 9 )
#define SND_STOP_LOOPING ( 1 << 10 )

typedef struct soundlist_e
{
	char     name[MAX_QPATH];
	short    entnum;
	vec3_t   origin;
	float    volume;
	float    attenuation;
	qboolean looping;
	byte     channel;
	byte     pitch;
	byte     wordIndex;
	double   samplePos;
	double   forcedEnd;
} soundlist_t;

static inline qboolean Host_IsQuakeCompatible( void ) { return false; }

/* common/cl_entity.h's (sound.h's mouth functions take one) */
typedef struct mouth_s
{
	byte mouthopen;
	byte sndcount;
	int  sndavg;
} mouth_t;

typedef enum
{
	WF_UNKNOWN = 0,
	WF_PCMDATA,
	WF_MPGDATA,
	WF_VORBISDATA,
	WF_OPUSDATA,
	WF_TOTALCOUNT,
} sndformat_t;

typedef enum sndFlags_e
{
	SOUND_LOOPED   = BIT( 0 ),
	SOUND_STREAM   = BIT( 1 ),
	SOUND_RESAMPLE = BIT( 12 ),
} sndFlags_t;

typedef struct wavdata_s
{
	size_t  size;
	uint    loop_start;
	uint    samples;
	uint    type;
	uint    flags;
	word    rate;
	byte    width;
	byte    channels;
	byte    buffer[];
} wavdata_t;

typedef struct stream_s stream_t;
typedef struct file_s file_t;
typedef struct search_s
{
	int numfilenames;
	char **filenames;
	char *filenamesbuffer;
} search_t;

typedef enum { HOST_INIT = 0, HOST_FRAME, HOST_NOFOCUS, HOST_SLEEP } host_status_t;
#define BUGCOMP_SPATIALIZE_SOUND_WITH_ATTN_NONE BIT( 2 )
typedef struct host_parm_s
{
	host_status_t status;
	double realtime;
	poolhandle_t mempool, soundpool;
	uint32_t bugcomp;
} host_parm_t;
extern host_parm_t host;

typedef struct gameinfo_s
{
	char ambientsound[4][64];
} gameinfo_t;
extern gameinfo_t *GI;

/* memory: the test's, which knows which blocks a queued read writes into */
void *vox_test_alloc( size_t size, int zero );
void *vox_test_realloc( void *p, size_t size );
void vox_test_free( void *p );
#define Mem_Malloc( pool, size ) vox_test_alloc(( size ), 0 )
#define Mem_Calloc( pool, size ) vox_test_alloc(( size ), 1 )
#define Mem_Realloc( pool, ptr, size ) vox_test_realloc(( ptr ), ( size ))
#define Mem_Free( mem ) vox_test_free( mem )
#define Mem_Free2( ptr ) do { vox_test_free( *( ptr )); *( ptr ) = NULL; } while( 0 )

/* files */
byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly );
const char *FS_GetDiskPath( const char *name, qboolean gamedironly );
file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly );
fs_offset_t FS_Read( file_t *file, void *buffer, size_t buffersize );
fs_offset_t FS_Write( file_t *file, const void *data, size_t datasize );
int FS_Seek( file_t *file, fs_offset_t offset, int whence );
fs_offset_t FS_Tell( file_t *file );
qboolean FS_Eof( file_t *file );
int FS_Close( file_t *file );
qboolean FS_FileExists( const char *filename, int gamedironly );
search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly );

/* soundlib */
wavdata_t *FS_LoadSound( const char *filename, const byte *buffer, size_t size );
void FS_FreeSound( wavdata_t *pack );
stream_t *FS_OpenStream( const char *filename );
int FS_ReadStream( stream_t *stream, int bytes, void *buffer );
int FS_GetStreamPos( stream_t *stream );
int FS_SetStreamPos( stream_t *stream, int newpos );
void FS_FreeStream( stream_t *stream );
qboolean Sound_Process( wavdata_t **wav, int rate, int width, int channels, uint flags );

/* cvars, commands, console */
typedef void ( *xcommand_t )( void );
void Cvar_RegisterVariable( convar_t *var );
void Cvar_DirectSet( convar_t *var, const char *value );
void Cvar_DirectSetValue( convar_t *var, float value );
int Cmd_AddCommand( const char *cmd_name, xcommand_t function, const char *cmd_desc );
int Cmd_Argc( void );
const char *Cmd_Argv( int arg );
void Con_Printf( const char *fmt, ... );
void Con_DPrintf( const char *fmt, ... );
void Con_Reportf( const char *fmt, ... );
int COM_RandomLong( int lMin, int lMax );

#endif /* VOX_STUB_COMMON_H */
