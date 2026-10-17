/*
s_load.c - sounds managment
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "sound.h"
#if XASH_XBOX
#include "platform/xbox/xbox_frametime.h" // first-use load counters, docs/r3d/divergences.md #141
#include "platform/xbox/xbox_vox_stream.h" // placeholders of streamed sounds, #177
#endif

// during registration it is possible to have more sounds
// than could actually be referenced during gameplay,
// because we don't want to free anything until we are
// sure we won't need it.
#define MAX_SFX      8192
#define MAX_SFX_HASH (MAX_SFX/4)

static int      s_numSfx = 0;
static sfx_t    s_knownSfx[MAX_SFX];
static sfx_t    *s_sfxHashList[MAX_SFX_HASH];
static qboolean s_registering = false;

#define SENTENCE_INDEX -99999 // unique sentence index
static string   s_sentenceImmediateName;	// keep dummy sentence name

/*
=================
S_SoundList_f
=================
*/
void S_SoundList_f( void )
{
	sfx_t	*sfx;
	int	i, totalSfx = 0;
	int	totalSize = 0;

	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] )
			continue;

		wavdata_t *sc = sfx->cache;
		if( sc )
		{
			totalSize += sc->size;

			if( FBitSet( sc->flags, SOUND_LOOPED ))
				Con_Printf( "L" );
			else
				Con_Printf( " " );

			if( sfx->name[0] == '*' || !Q_strncmp( sfx->name, DEFAULT_SOUNDPATH, sizeof( DEFAULT_SOUNDPATH ) - 1 ))
				Con_Printf( " (%2db) %s : %s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			else Con_Printf( " (%2db) %s : " DEFAULT_SOUNDPATH "%s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			totalSfx++;
		}
	}

	Con_Printf( "-------------------------------------------\n" );
	Con_Printf( "%i total sounds\n", totalSfx );
	Con_Printf( "%s total memory\n", Q_memprint( totalSize ));
	Con_Printf( "\n" );
}

/*
=================
S_CreateDefaultSound
=================
*/
static wavdata_t *S_CreateDefaultSound( void )
{
	uint samples = SOUND_DMA_SPEED;
	uint channels = 1;
	uint width = 2;
	size_t size = samples * width * channels;

	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( wavdata_t ) + size );

	sc->width = width;
	sc->channels = channels;
	sc->rate = SOUND_DMA_SPEED;
	sc->samples = samples;
	sc->size = size;

	return sc;
}

/*
=================
S_LoadSound
=================
*/
wavdata_t *S_LoadSound( sfx_t *sfx )
{
	wavdata_t	*sc = NULL;

	if( !sfx ) return NULL;

	// see if still in memory
	if( sfx->cache )
#if XASH_XBOX
		if( !S_R3D_VoxIsHeader( sfx->cache )) // #177: a streamed sound's placeholder loads whole below
#endif
		return sfx->cache;

	if( COM_StringEmptyOrNULL( sfx->name ))
		return NULL;

#if XASH_XBOX
	// the frame-time recorder's snd_loads/snd_load_kb/snd_load_kc (#141)
	xbox_ft_span_t ft_load;
	xbox_ft_load_t ft_stage; // the precache's sounds, summed (#162)
	xbox_ft_span_begin( &ft_load );
	Xbox_FT_LoadBegin( &ft_stage );
#endif

	// load it from disk
	if( Q_stricmp( sfx->name, "*default" ))
	{
		// load it from disk
		if( s_warn_late_precache.value > 0 && cls.state == ca_active )
			Con_Printf( S_WARN "%s: late precache of %s\n", __func__, sfx->name );

		if( sfx->name[0] == '*' )
			sc = FS_LoadSound( sfx->name + 1, NULL, 0 );
#if XASH_XBOX
		// #177: a small file a stream's open already read is decoded from those bytes
		else if( !( sc = S_R3D_VoxPreloaded( sfx )))
			sc = FS_LoadSound( sfx->name, NULL, 0 );
#else
		else
			sc = FS_LoadSound( sfx->name, NULL, 0 );
#endif
	}

	if( !sc ) sc = S_CreateDefaultSound();

	if( sc->rate < SOUND_11k ) // some bad sounds
		Sound_Process( &sc, SOUND_11k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_11k && sc->rate < SOUND_22k ) // some bad sounds
		Sound_Process( &sc, SOUND_22k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_22k && sc->rate != SOUND_44k ) // some bad sounds
		Sound_Process( &sc, SOUND_44k, sc->width, sc->channels, SOUND_RESAMPLE );

#if XASH_XBOX
	S_R3D_VoxDetach( sfx ); // #177: the placeholder this load replaces
#endif
	sfx->cache = sc;

#if XASH_XBOX
	{
		const uint32_t kc = xbox_ft_load_end( &ft_load, XBOX_FT_W_SND_LOADS, XBOX_FT_W_SND_LOAD_KB, XBOX_FT_W_SND_LOAD_KC, sc->size );

		// named in the recording when it happens in play: the
		// cls.state test of the s_warn_late_precache warning above (without
		// its cvar or its "*default" exclusion)
		if( cls.state == ca_active )
			Xbox_FT_SoundNote( &ft_stage, sfx->name, s_r3d_load_tag, kc, (uint32_t)sc->size ); // #177: why, and its file calls
		else
			Xbox_FT_LoadAccum( &ft_stage, XBOX_FT_LACC_SOUND );
		s_r3d_load_tag = R3D_VOX_TAG_OTHER;
	}
#endif

	return sfx->cache;
}

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName( const char *pname, qboolean *pfInCache )
{
	sfx_t	*sfx;
	uint	i;
	string	name;

	if( COM_StringEmptyOrNULL( pname ) || !snd.initialized )
		return NULL;

	if( Q_strlen( pname ) >= sizeof( sfx->name ))
		return NULL;

	Q_strncpy( name, pname, sizeof( name ));
	COM_FixSlashes( name );

	// see if already loaded
	uint hash = COM_HashKey( name, MAX_SFX_HASH );
	for( sfx = s_sfxHashList[hash]; sfx; sfx = sfx->hashNext )
	{
		if( !Q_strcmp( sfx->name, name ))
		{
			if( pfInCache )
			{
				// indicate whether or not sound is currently in the cache.
				*pfInCache = ( sfx->cache != NULL ) ? true : false;
			}
			// prolonge registration
			sfx->servercount = cl.servercount;
			return sfx;
		}
	}

	// find a free sfx slot spot
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++)
		if( !sfx->name[0] ) break; // free spot

	if( i == s_numSfx )
	{
		if( s_numSfx == MAX_SFX )
			return NULL;
		s_numSfx++;
	}

	sfx = &s_knownSfx[i];
	memset( sfx, 0, sizeof( *sfx ));
	if( pfInCache ) *pfInCache = false;
	Q_strncpy( sfx->name, name, sizeof( sfx->name ));
	sfx->servercount = cl.servercount;
	sfx->hashValue = COM_HashKey( sfx->name, MAX_SFX_HASH );

	// link it in
	sfx->hashNext = s_sfxHashList[sfx->hashValue];
	s_sfxHashList[sfx->hashValue] = sfx;

	return sfx;
}

/*
==================
S_FreeSound
==================
*/
void S_FreeSound( sfx_t *sfx )
{
	if( !sfx || !sfx->name[0] )
		return;

	// de-link it from the hash tree
	sfx_t **prev = &s_sfxHashList[sfx->hashValue];
	while( 1 )
	{
		sfx_t *hashSfx = *prev;
		if( !hashSfx )
			break;

		if( hashSfx == sfx )
		{
			*prev = hashSfx->hashNext;
			break;
		}
		prev = &hashSfx->hashNext;
	}

#if XASH_XBOX
	S_R3D_VoxForget( sfx ); // #177: a placeholder (sfx->cache goes NULL) and the facts kept for sfx
#endif
	if( clgame.soundFuncs.pfnS_FreeSound )
	{
		clgame.soundFuncs.pfnS_FreeSound( sfx, sfx - s_knownSfx );
		return;
	}

	if( sfx->cache )
		FS_FreeSound( sfx->cache );
	memset( sfx, 0, sizeof( *sfx ));
}

/*
=====================
S_BeginRegistration

=====================
*/
void S_BeginRegistration( void )
{
	snd.have_ambient_sfx = false;

	// check for automatic ambient sounds
	for( int i = 0; i < NUM_AMBIENTS; i++ )
	{
		if( !GI->ambientsound[i][0] )
			continue;	// empty slot

		snd.ambient_sfx[i] = S_RegisterSound( GI->ambientsound[i] );
		if( snd.ambient_sfx[i] )
			snd.have_ambient_sfx = true; // allow auto-ambients
	}

	s_registering = true;
}

/*
=====================
S_EndRegistration

=====================
*/
void S_EndRegistration( void )
{
	sfx_t	*sfx;
	int	i;

	if( !s_registering || !snd.initialized )
		return;

	// free any sounds not from this registration sequence
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] || !Q_stricmp( sfx->name, "*default" ))
			continue; // don't release default sound

		if( sfx->servercount != cl.servercount )
			S_FreeSound( sfx ); // don't need this sound
	}

	// load everything in
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] )
			continue;
#if XASH_XBOX
		S_R3D_VoxRegister( sfx ); // #177: precached speech as a placeholder, the rest S_LoadSound
#else
		S_LoadSound( sfx );
#endif
	}
	s_registering = false;
}

/*
==================
S_RegisterSound

==================
*/
sound_t S_RegisterSound( const char *name )
{
	sfx_t	*sfx;

	if( COM_StringEmptyOrNULL( name ) || !snd.initialized )
		return -1;

	if( S_TestSoundChar( name, '!' ))
	{
		Q_strncpy( s_sentenceImmediateName, name, sizeof( s_sentenceImmediateName ));
		return SENTENCE_INDEX;
	}

	// some stupid mappers used leading '/' or '\' in path to models or sounds
	if( name[0] == '/' || name[0] == '\\' ) name++;
	if( name[0] == '/' || name[0] == '\\' ) name++;

	sfx = S_FindName( name, NULL );
	if( !sfx ) return -1;

	sfx->servercount = cl.servercount;
#if XASH_XBOX
	if( !s_registering ) S_R3D_VoxRegister( sfx ); // #177
#else
	if( !s_registering ) S_LoadSound( sfx );
#endif

	return sfx - s_knownSfx;
}

sfx_t *S_GetSfxByHandle( sound_t handle )
{
	if( !snd.initialized )
		return NULL;

	// create new sfx
	if( handle == SENTENCE_INDEX )
		return S_FindName( s_sentenceImmediateName, NULL );

	if( handle < 0 || handle >= s_numSfx )
		return NULL;

	return &s_knownSfx[handle];
}

/*
=================
S_InitSounds
=================
*/
void S_InitSounds( void )
{
	// create unused 0-entry
	Q_strncpy( s_knownSfx->name, "*default", sizeof( s_knownSfx->name ));
	s_knownSfx->hashValue = COM_HashKey( s_knownSfx->name, MAX_SFX_HASH );
	s_knownSfx->hashNext = s_sfxHashList[s_knownSfx->hashValue];
	s_sfxHashList[s_knownSfx->hashValue] = s_knownSfx;
	s_knownSfx->cache = S_CreateDefaultSound();
	s_numSfx = 1;
}

/*
=================
S_FreeSounds
=================
*/
void S_FreeSounds( void )
{
	sfx_t	*sfx;
	int	i;

	if( !snd.initialized )
		return;

	// stop all sounds
	S_StopAllSounds( true );

	// free all sounds
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
		S_FreeSound( sfx );


	memset( s_knownSfx, 0, sizeof( s_knownSfx ));
	memset( s_sfxHashList, 0, sizeof( s_sfxHashList ));

	s_numSfx = 0;
}
