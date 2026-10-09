/*
sv_save.c - save\restore implementation
Copyright (C) 2008 Uncle Mike

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
#include "server.h"
#include "library.h"
#include "const.h"
#include "render_api.h"	// decallist_t
#include "sound.h"		// S_GetDynamicSounds
#include "ref_common.h" // decals
#if XASH_XBOX && !XASH_DEDICATED
#include "platform/xbox/xbox_frametime.h" // the frame-time recorder's save spans, docs/r3d/divergences.md #141
#else
#define XBOX_FT_SPAN( id, stmt ) stmt
#define XBOX_FT_SPAN_BEGIN( var )
#define XBOX_FT_SPAN_END( var, id )
#define XBOX_FT_LOAD_BEGIN( var )
#define XBOX_FT_LOAD_END( var, stage )
#define XBOX_FT_LOAD_ACCUM( var, acc )
#endif

#if XASH_XBOX
/*
Resonance3D (docs/r3d/divergences.md #151): the save path's file writes,
staged. Upstream writes each file of a save with one FS_Write per field
(`sv_save.c:1582-1592`, `:1746-1754`, `:1260-1268`, `:1023-1028` in the
reference) and copies every <map>.HL? into the .sav through FS_FileCopy
(`:654-656`), which allocates a 1 MiB buffer per file (`io.c:1100`).
FS_Write is unbuffered (`io.c:181`): on the Xbox each call is a
synchronous NtWriteFile into the FATX driver, and in xemu each one cost
about a millisecond of CPU however few bytes it carried - an autosave
late in a chapter made ~130 of them. The kernel was also about three
times slower reading into, and twice as slow writing from, a buffer
that does not start on a page. Here the bytes of each file are gathered,
in the order upstream writes them, in the free tail of the active save
buffer (svgame.globals->pSaveData, 4 MiB, of which a save uses a few
hundred KiB), from its first page boundary, and handed to FS_Write in as
few calls as fit - one per file in practice - so every file gets the
same bytes. Nothing is allocated. SaveFinish and SaveClear flush first,
so staged bytes never outlive their buffer; a file is complete once
SaveClose returns, as upstream's FS_Close.
*/
#define SAVE_STAGE_MIN ( 64 * 1024 ) // a smaller free tail is not worth staging into: write through
#define SAVE_STAGE_ALIGN 4096        // the stage and every file read into it start on a page

typedef struct save_stage_s
{
	file_t *file; // the file the staged bytes belong to; NULL when nothing is staged
	byte   *buf;  // the free tail of the save buffer, past pCurrentData
	size_t  len;
	size_t  cap;
} save_stage_t;

static save_stage_t save_stage;

// Set when a save file write or a read into the stage fell short since
// SaveGameSlot started; SaveGameSlot then removes the incomplete .sav
// and fails instead of reporting a save it did not finish. Upstream
// ignored FS_Write's count per field; a staged flush carries a whole file,
// so a short write must not be dropped silently.
static qboolean save_io_failed;

static void SaveIO_Fail( const char *func, size_t lost )
{
	if( !save_io_failed )
		Con_Printf( S_ERROR "%s: a save file write fell short (%u bytes not written)\n", func, (uint)lost );
	save_io_failed = true;
}

// Writes the whole stage, continuing after a short write; a write that
// makes no progress fails the save.
static void SaveStage_Flush( void )
{
	const byte *p = save_stage.buf;
	size_t left = save_stage.len;

	while( save_stage.file && left > 0 )
	{
		fs_offset_t written = FS_Write( save_stage.file, p, left );

		if( written <= 0 )
		{
			SaveIO_Fail( __func__, left );
			break;
		}
		p += written;
		left -= (size_t)written;
	}
	save_stage.len = 0;
}

static void SaveStage_End( void )
{
	SaveStage_Flush();
	save_stage.file = NULL;
}

// Stages `file`'s writes from here on, ending any other file's; false
// when there is no save buffer or too little of it left (write through).
static qboolean SaveStage_Begin( file_t *file )
{
	SAVERESTOREDATA *pSaveData = svgame.globals->pSaveData;
	byte *tail, *end;

	if( save_stage.file == file )
		return true;

	SaveStage_End();

	if( !file || !pSaveData )
		return false;

	// The game DLL's overflow ("Save/Restore overflow!", CSave::BufferData)
	// sets size to bufferSize without writing: the files then take the
	// whole buffer, the stage included. Write through, as upstream; the
	// unwritten bytes are stale rather than zero (SaveInitUncleared), in a
	// save that is broken either way.
	if( pSaveData->size >= pSaveData->bufferSize )
		return false;

	// everything written so far lies before pCurrentData (the data, the
	// ETABLE, then the token table StoreHashTable appended)
	tail = (byte *)pSaveData->pCurrentData;
	end = (byte *)( pSaveData + 1 ) + pSaveData->bufferSize;
	if( tail < (byte *)( pSaveData + 1 ) || tail > end )
		return false;
	tail += ( SAVE_STAGE_ALIGN - (uintptr_t)tail % SAVE_STAGE_ALIGN ) % SAVE_STAGE_ALIGN;
	if( tail > end || (size_t)( end - tail ) < SAVE_STAGE_MIN )
		return false;

	save_stage.file = file;
	save_stage.buf = tail;
	save_stage.len = 0;
	// whole pages, so a file bigger than the stage is flushed at page
	// offsets of the file too
	save_stage.cap = (size_t)( end - tail ) / SAVE_STAGE_ALIGN * SAVE_STAGE_ALIGN;
	return true;
}

static fs_offset_t SaveWrite( file_t *file, const void *data, size_t datasize )
{
	const byte *src = data;
	size_t left = datasize;

	if( !SaveStage_Begin( file ))
	{
		fs_offset_t written = FS_Write( file, data, datasize );

		if( written != (fs_offset_t)datasize )
			SaveIO_Fail( __func__, datasize - ( written > 0 ? (size_t)written : 0 ));
		return written;
	}

	while( left > 0 )
	{
		size_t n;

		if( save_stage.len == save_stage.cap )
			SaveStage_Flush();

		n = Q_min( left, save_stage.cap - save_stage.len );
		memcpy( save_stage.buf + save_stage.len, src, n );
		save_stage.len += n;
		src += n;
		left -= n;
	}

	return datasize;
}

// FS_FileCopy's copy loop (io.c:1102-1118) reading into the stage: the
// same bytes, without the 1 MiB buffer. On a short read it stops as
// FS_FileCopy does, dropping the chunk it was reading; the chunks are the
// stage's, not 1 MiB, so a truncated input file leaves a different (and
// equally corrupt) tail in the .sav.
static qboolean SaveCopy( file_t *pOutput, file_t *pInput, int fileSize )
{
	if( !SaveStage_Begin( pOutput ))
	{
		if( FS_FileCopy( pOutput, pInput, fileSize ))
			return true;
		SaveIO_Fail( __func__, (size_t)fileSize );
		return false;
	}

	while( fileSize > 0 )
	{
		byte *dst = save_stage.buf + save_stage.len;
		byte *end = save_stage.buf + save_stage.cap;
		// read on the next page boundary (a third of the time of an
		// unaligned read, in xemu), then move the bytes down into place
		byte *rd = dst + ( SAVE_STAGE_ALIGN - (uintptr_t)dst % SAVE_STAGE_ALIGN ) % SAVE_STAGE_ALIGN;
		int size, readSize;

		if( rd >= end )
		{
			SaveStage_Flush(); // the stage starts on a page: the next read lands in place
			continue;
		}

		size = (int)Q_min( (size_t)fileSize, (size_t)( end - rd ));
		readSize = FS_Read( pInput, rd, size );
		if( readSize < size )
		{
			Con_Reportf( S_ERROR "%s: unexpected end of input file (%d < %d)\n", __func__, readSize, size );
			SaveIO_Fail( __func__, (size_t)fileSize );
			return false;
		}

		if( rd != dst )
			memmove( dst, rd, size );
		save_stage.len += size;
		fileSize -= size;
	}

	return true;
}

static int SaveClose( file_t *file )
{
	if( file && save_stage.file == file )
		SaveStage_End();

	return FS_Close( file );
}
#else // !XASH_XBOX
#define SaveWrite FS_Write
#define SaveCopy  FS_FileCopy
#define SaveClose FS_Close
#endif // !XASH_XBOX

/*
==============================================================================
SAVE FILE

half-life implementation of saverestore system
==============================================================================
*/
#define SAVEFILE_HEADER		(('V'<<24)+('L'<<16)+('A'<<8)+'V')	// little-endian "VALV"
#define SAVEGAME_HEADER		(('V'<<24)+('A'<<16)+('S'<<8)+'J')	// little-endian "JSAV"
#define SAVEGAME_VERSION		0x0071				// Version 0.71 GoldSrc compatible
#define CLIENT_SAVEGAME_VERSION	0x0067				// Version 0.67

#define SAVE_HEAPSIZE		0x400000				// reserve 4Mb for now
#define SAVE_HASHSTRINGS		0xFFF				// 4095 unique strings

// savedata headers
typedef struct
{
	char	mapName[32];
	char	comment[80];
	int	mapCount;
} GAME_HEADER;

typedef struct
{
	int	skillLevel;
	int	entityCount;
	int	connectionCount;
	int	lightStyleCount;
	float	time;
	char	mapName[32];
	char	skyName[32];
	int	skyColor_r;
	int	skyColor_g;
	int	skyColor_b;
	float	skyVec_x;
	float	skyVec_y;
	float	skyVec_z;
} SAVE_HEADER;

typedef struct
{
	int	decalCount;	// render decals count
	int	entityCount;	// static entity count
	int	soundCount;	// sounds count
	int	tempEntsCount;	// not used
	char	introTrack[64];
	char	mainTrack[64];
	int	trackPosition;
	short	viewentity;	// Xash3D added
	float	wateralpha;
	float	wateramp;		// world waves
} SAVE_CLIENT;

typedef struct
{
	int	index;
	char	style[256];
	float	time;
} SAVE_LIGHTSTYLE;

#if XASH_WIN32
static void (__cdecl *pfnSaveGameComment)( char *buffer, int max_length ) = NULL;
#else // XASH_WIN32
static void (*pfnSaveGameComment)( char *buffer, int max_length ) = NULL;
#endif // XASH_WIN32

static TYPEDESCRIPTION gGameHeader[] =
{
	DEFINE_ARRAY( GAME_HEADER, mapName, FIELD_CHARACTER, 32 ),
	DEFINE_ARRAY( GAME_HEADER, comment, FIELD_CHARACTER, 80 ),
	DEFINE_FIELD( GAME_HEADER, mapCount, FIELD_INTEGER ),
};

static TYPEDESCRIPTION gSaveHeader[] =
{
	DEFINE_FIELD( SAVE_HEADER, skillLevel, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, entityCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, connectionCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, lightStyleCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, time, FIELD_TIME ),
	DEFINE_ARRAY( SAVE_HEADER, mapName, FIELD_CHARACTER, 32 ),
	DEFINE_ARRAY( SAVE_HEADER, skyName, FIELD_CHARACTER, 32 ),
	DEFINE_FIELD( SAVE_HEADER, skyColor_r, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, skyColor_g, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, skyColor_b, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_HEADER, skyVec_x, FIELD_FLOAT ),
	DEFINE_FIELD( SAVE_HEADER, skyVec_y, FIELD_FLOAT ),
	DEFINE_FIELD( SAVE_HEADER, skyVec_z, FIELD_FLOAT ),
};

static TYPEDESCRIPTION gAdjacency[] =
{
	DEFINE_ARRAY( LEVELLIST, mapName, FIELD_CHARACTER, 32 ),
	DEFINE_ARRAY( LEVELLIST, landmarkName, FIELD_CHARACTER, 32 ),
	DEFINE_FIELD( LEVELLIST, pentLandmark, FIELD_EDICT ),
	DEFINE_FIELD( LEVELLIST, vecLandmarkOrigin, FIELD_VECTOR ),
};

static TYPEDESCRIPTION gLightStyle[] =
{
	DEFINE_FIELD( SAVE_LIGHTSTYLE, index, FIELD_INTEGER ),
	DEFINE_ARRAY( SAVE_LIGHTSTYLE, style, FIELD_CHARACTER, 256 ),
	DEFINE_FIELD( SAVE_LIGHTSTYLE, time, FIELD_FLOAT ),
};

static TYPEDESCRIPTION gEntityTable[] =
{
	DEFINE_FIELD( ENTITYTABLE, id, FIELD_INTEGER ),
	DEFINE_FIELD( ENTITYTABLE, location, FIELD_INTEGER ),
	DEFINE_FIELD( ENTITYTABLE, size, FIELD_INTEGER ),
	DEFINE_FIELD( ENTITYTABLE, flags, FIELD_INTEGER ),
	DEFINE_FIELD( ENTITYTABLE, classname, FIELD_STRING ),
};

static TYPEDESCRIPTION gSaveClient[] =
{
	DEFINE_FIELD( SAVE_CLIENT, decalCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_CLIENT, entityCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_CLIENT, soundCount, FIELD_INTEGER ),
	DEFINE_FIELD( SAVE_CLIENT, tempEntsCount, FIELD_INTEGER ),
	DEFINE_ARRAY( SAVE_CLIENT, introTrack, FIELD_CHARACTER, 64 ),
	DEFINE_ARRAY( SAVE_CLIENT, mainTrack, FIELD_CHARACTER, 64 ),
	DEFINE_FIELD( SAVE_CLIENT, trackPosition, FIELD_INTEGER ),
	// mods based on HLU SDK disallow usage of FIELD_SHORT
	DEFINE_ARRAY( SAVE_CLIENT, viewentity, FIELD_CHARACTER, sizeof( short )),
	DEFINE_FIELD( SAVE_CLIENT, wateralpha, FIELD_FLOAT ),
	DEFINE_FIELD( SAVE_CLIENT, wateramp, FIELD_FLOAT ),
};

static TYPEDESCRIPTION gDecalEntry[] =
{
	DEFINE_FIELD( decallist_t, position, FIELD_VECTOR ),
	DEFINE_ARRAY( decallist_t, name, FIELD_CHARACTER, 64 ),
	// mods based on HLU SDK disallow usage of FIELD_SHORT
	DEFINE_ARRAY( decallist_t, entityIndex, FIELD_CHARACTER, sizeof( short )),
	DEFINE_FIELD( decallist_t, depth, FIELD_CHARACTER ),
	DEFINE_FIELD( decallist_t, flags, FIELD_CHARACTER ),
	DEFINE_FIELD( decallist_t, scale, FIELD_FLOAT ),
	DEFINE_FIELD( decallist_t, impactPlaneNormal, FIELD_VECTOR ),
	DEFINE_ARRAY( decallist_t, studio_state, FIELD_CHARACTER, sizeof( modelstate_t )),
};

// Can use any FIELD type here because only Xash3D games will spawn static entities
static TYPEDESCRIPTION gStaticEntry[] =
{
	DEFINE_FIELD( entity_state_t, messagenum, FIELD_MODELNAME ), // HACKHACK: store model into messagenum
	DEFINE_FIELD( entity_state_t, origin, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, angles, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, sequence, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, frame, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, colormap, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, skin, FIELD_SHORT ),
	DEFINE_FIELD( entity_state_t, body, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, scale, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, effects, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, framerate, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, mins, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, maxs, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, startpos, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, rendermode, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, renderamt, FIELD_FLOAT ),
	DEFINE_ARRAY( entity_state_t, rendercolor, FIELD_CHARACTER, sizeof( color24 )),
	DEFINE_FIELD( entity_state_t, renderfx, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, controller, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, blending, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, solid, FIELD_SHORT ),
	DEFINE_FIELD( entity_state_t, animtime, FIELD_TIME ),
	DEFINE_FIELD( entity_state_t, movetype, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, vuser1, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, vuser2, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, vuser3, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, vuser4, FIELD_VECTOR ),
	DEFINE_FIELD( entity_state_t, iuser1, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, iuser2, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, iuser3, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, iuser4, FIELD_INTEGER ),
	DEFINE_FIELD( entity_state_t, fuser1, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, fuser2, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, fuser3, FIELD_FLOAT ),
	DEFINE_FIELD( entity_state_t, fuser4, FIELD_FLOAT ),
};

static TYPEDESCRIPTION gSoundEntry[] =
{
	DEFINE_ARRAY( soundlist_t, name, FIELD_CHARACTER, 64 ),
	// mods based on HLU SDK disallow usage of FIELD_SHORT
	DEFINE_ARRAY( soundlist_t, entnum, FIELD_CHARACTER, sizeof( short )),
	DEFINE_FIELD( soundlist_t, origin, FIELD_VECTOR ),
	DEFINE_FIELD( soundlist_t, volume, FIELD_FLOAT ),
	DEFINE_FIELD( soundlist_t, attenuation, FIELD_FLOAT ),
	DEFINE_FIELD( soundlist_t, looping, FIELD_BOOLEAN ),
	DEFINE_FIELD( soundlist_t, channel, FIELD_CHARACTER ),
	DEFINE_FIELD( soundlist_t, pitch, FIELD_CHARACTER ),
	DEFINE_FIELD( soundlist_t, wordIndex, FIELD_CHARACTER ),
	DEFINE_ARRAY( soundlist_t, samplePos, FIELD_CHARACTER, sizeof( double )),
	DEFINE_ARRAY( soundlist_t, forcedEnd, FIELD_CHARACTER, sizeof( double )),
};

static TYPEDESCRIPTION gTempEntvars[] =
{
	DEFINE_ENTITY_FIELD( classname, FIELD_STRING ),
	DEFINE_ENTITY_GLOBAL_FIELD( globalname, FIELD_STRING ),
};

#if XASH_XBOX
// XM.1 item 10: the Pause screen's difficulty annotation was meant to
// carry a chapter title too - dropped in the plan as "no such concept
// exists" until Codex review found this table and pointed at it, which
// is why the cvar lives here rather than in vid_common.c alongside
// vid_refresh/vid_aspect: it depends on gTitleComments[], a few lines
// below, which is file-static and stays that way rather than getting
// exported. Same read-only-cvar shape as that precedent regardless -
// zero new ABI surface, Xbox-only definition and registration (Codex
// review round 2 on the Display PR: an Xbox-only value has no business
// reserving a cvar name on every platform).
CVAR_DEFINE( sv_leveltitle, "sv_leveltitle", "", FCVAR_READ_ONLY, "current level's resolved title, Xbox only" );
#endif

static const struct
{
	const char *mapname;
	const char *titlename;
} gTitleComments[] =
{
	// default Half-Life map titles
	// ordering is important
	// strings hw.so| grep T0A0TITLE -B 50 -A 150
	{ "T0A0", "#T0A0TITLE" },
	{ "C0A0", "#C0A0TITLE" },
	{ "C1A0", "#C0A1TITLE" },
	{ "C1A1", "#C1A1TITLE" },
	{ "C1A2", "#C1A2TITLE" },
	{ "C1A3", "#C1A3TITLE" },
	{ "C1A4", "#C1A4TITLE" },
	{ "C2A1", "#C2A1TITLE" },
	{ "C2A2", "#C2A2TITLE" },
	{ "C2A3", "#C2A3TITLE" },
	{ "C2A4D", "#C2A4TITLE2" },
	{ "C2A4E", "#C2A4TITLE2" },
	{ "C2A4F", "#C2A4TITLE2" },
	{ "C2A4G", "#C2A4TITLE2" },
	{ "C2A4", "#C2A4TITLE1" },
	{ "C2A5", "#C2A5TITLE" },
	{ "C3A1", "#C3A1TITLE" },
	{ "C3A2", "#C3A2TITLE" },
	{ "C4A1A", "#C4A1ATITLE" },
	{ "C4A1B", "#C4A1ATITLE" },
	{ "C4A1C", "#C4A1ATITLE" },
	{ "C4A1D", "#C4A1ATITLE" },
	{ "C4A1E", "#C4A1ATITLE" },
	{ "C4A1", "#C4A1TITLE" },
	{ "C4A2", "#C4A2TITLE" },
	{ "C4A3", "#C4A3TITLE" },
	{ "C5A1", "#C5TITLE" },
	{ "OFBOOT", "#OF_BOOT0TITLE" },
	{ "OF0A", "#OF1A1TITLE" },
	{ "OF1A1", "#OF1A3TITLE" },
	{ "OF1A2", "#OF1A3TITLE" },
	{ "OF1A3", "#OF1A3TITLE" },
	{ "OF1A4", "#OF1A3TITLE" },
	{ "OF1A", "#OF1A5TITLE" },
	{ "OF2A1", "#OF2A1TITLE" },
	{ "OF2A2", "#OF2A1TITLE" },
	{ "OF2A3", "#OF2A1TITLE" },
	{ "OF2A", "#OF2A4TITLE" },
	{ "OF3A1", "#OF3A1TITLE" },
	{ "OF3A2", "#OF3A1TITLE" },
	{ "OF3A", "#OF3A3TITLE" },
	{ "OF4A1", "#OF4A1TITLE" },
	{ "OF4A2", "#OF4A1TITLE" },
	{ "OF4A3", "#OF4A1TITLE" },
	{ "OF4A", "#OF4A4TITLE" },
	{ "OF5A", "#OF5A1TITLE" },
	{ "OF6A1", "#OF6A1TITLE" },
	{ "OF6A2", "#OF6A1TITLE" },
	{ "OF6A3", "#OF6A1TITLE" },
	{ "OF6A4b", "#OF6A4TITLE" },
	{ "OF6A4", "#OF6A4TITLE" },
	{ "OF6A5", "#OF6A4TITLE" },
	{ "OF6A", "#OF6A4TITLE" },
	{ "OF7A", "#OF7A0TITLE" },
	{ "ba_tram", "#BA_TRAMTITLE" },
	{ "ba_security", "#BA_SECURITYTITLE" },
	{ "ba_main", "#BA_SECURITYTITLE" },
	{ "ba_elevator", "#BA_SECURITYTITLE" },
	{ "ba_canal", "#BA_CANALSTITLE" },
	{ "ba_yard", "#BA_YARDTITLE" },
	{ "ba_xen", "#BA_XENTITLE" },
	{ "ba_hazard", "#BA_HAZARD" },
	{ "ba_power", "#BA_POWERTITLE" },
	{ "ba_teleport1", "#BA_POWERTITLE" },
	{ "ba_teleport", "#BA_TELEPORTTITLE" },
	{ "ba_outro", "#BA_OUTRO" },
};

/*
=============
SV_ResolveLevelTitle

Pulled out of SaveBuildComment's own "else" branch below (Codex review
of Pause.cpp, round 1: a claim that classic Half-Life has no chapter-
title concept at all was checked against deps/hlsdk/cl_dll/ only and
missed this - the real, retail mapname-to-title-token table, already
used to build save-file comments). Read-only, no side effects, and
deliberately excludes SaveBuildComment's OWN first branch
(pfnSaveGameComment - a gamedll callback that BUILDS a full comment
string, not something to invoke just to peek at a title): mapname
prefix match against gTitleComments[] first (the complete, canonical
table for retail HL/OpFor/BShift, populated regardless of whether a
given map happens to set its own worldspawn "chaptertitle" - most
mid-chapter maps don't), falling back to worldspawn's own message
field, then the raw mapname. The returned string may be a "#TOKEN"-
style localization key (matching L()'s own '#'-stripping convention,
LoadGame.cpp:286's own already-existing use of exactly this) or plain
text - both are safe to hand to L() unchanged.
=============
*/
static const char *SV_ResolveLevelTitle( void )
{
	size_t i;
	const char *mapname = SV_GetString( svgame.globals->mapname );

	for( i = 0; i < ARRAYSIZE( gTitleComments ); i++ )
	{
		// compare if strings are equal at beginning
		size_t len = strlen( gTitleComments[i].mapname );
		if( !Q_strnicmp( mapname, gTitleComments[i].mapname, len ))
			return gTitleComments[i].titlename;
	}

	if( svgame.edicts->v.message != 0 )
	{
		// trying to extract message from the world
		return SV_GetString( svgame.edicts->v.message );
	}

	// or use mapname
	return SV_GetString( svgame.globals->mapname );
}

#if XASH_XBOX
/*
=============
SV_UpdateLevelTitleCvar

Called from SV_ActivateServer (sv_init.c), right after
pfnServerActivate - worldspawn's own Spawn() already ran by then (entity
spawning happens earlier in SV_SpawnServer), so svgame.globals->mapname
and svgame.edicts->v.message are both valid. Xbox only: this cvar has
exactly one reader, Pause.cpp's own difficulty/title annotation.
=============
*/
void SV_UpdateLevelTitleCvar( void )
{
	Cvar_FullSet( "sv_leveltitle", SV_ResolveLevelTitle(), sv_leveltitle.flags );
}
#endif // XASH_XBOX

/*
=============
SaveBuildComment

build commentary for each savegame
typically it writes world message and level time
=============
*/
static void SaveBuildComment( char *text, int maxlength )
{
	string      comment;
	const char *pName = NULL;

	text[0] = '\0'; // clear

	if( pfnSaveGameComment != NULL )
	{
		// get save comment from gamedll
		pfnSaveGameComment( comment, MAX_STRING );
		pName = comment;
	}
	else
	{
		pName = SV_ResolveLevelTitle();
	}

	Q_snprintf( text, maxlength, "%-64.64s %02d:%02d", pName, (int)(sv.time / 60.0 ), (int)fmod( sv.time, 60.0 ));
}

/*
=============
DirectoryCount

counting all the files with HL1-HL3 extension
in save folder
=============
*/
static int DirectoryCount( const char *pPath )
{
	int	count;
	search_t	*t;

	t = FS_Search( pPath, true, true );	// lookup only in gamedir
	if( !t ) return 0; // empty

	count = t->numfilenames;
	Mem_Free( t );

	return count;
}

/*
=============
InitEntityTable

reserve space for ETABLE's
=============
*/
static void InitEntityTable( SAVERESTOREDATA *pSaveData, int entityCount )
{
	pSaveData->pTable = Mem_Calloc( host.mempool, sizeof( ENTITYTABLE ) * entityCount );
	pSaveData->tableCount = entityCount;

	// setup entitytable
	for( int i = 0; i < entityCount; i++ )
	{
		ENTITYTABLE *pTable = &pSaveData->pTable[i];
		pTable->pent = SV_EdictNum( i );
		pTable->id = i;
	}
}

/*
=============
EntryInTable

check level in transition list
=============
*/
static int EntryInTable( SAVERESTOREDATA *pSaveData, const char *pMapName, int index )
{
	for( int i = index + 1; i < pSaveData->connectionCount; i++ )
	{
		if ( !Q_stricmp( pSaveData->levelList[i].mapName, pMapName ))
			return i;
	}

	return -1;
}

/*
=============
EdictFromTable

get edict from table
=============
*/
static edict_t *EdictFromTable( SAVERESTOREDATA *pSaveData, int entityIndex )
{
	if( pSaveData && pSaveData->pTable )
	{
		entityIndex = bound( 0, entityIndex, pSaveData->tableCount - 1 );
		return pSaveData->pTable[entityIndex].pent;
	}

	return NULL;
}

/*
=============
LandmarkOrigin

find global offset for a given landmark
=============
*/
static void LandmarkOrigin( SAVERESTOREDATA *pSaveData, vec3_t output, const char *pLandmarkName )
{
	for( int i = 0; i < pSaveData->connectionCount; i++ )
	{
		if( !Q_strcmp( pSaveData->levelList[i].landmarkName, pLandmarkName ))
		{
			VectorCopy( pSaveData->levelList[i].vecLandmarkOrigin, output );
			return;
		}
	}

	VectorClear( output );
}

/*
=============
EntityInSolid

some moved edicts on a next level cause stuck
outside of world. Find them and remove
=============
*/
static int EntityInSolid( edict_t *pent )
{
	edict_t	*aiment = pent->v.aiment;
	vec3_t	point;

	// if you're attached to a client, always go through
	if( pent->v.movetype == MOVETYPE_FOLLOW && SV_IsValidEdict( aiment ) && FBitSet( aiment->v.flags, FL_CLIENT ))
		return 0;

	VectorAverage( pent->v.absmin, pent->v.absmax, point );
	svs.groupmask = pent->v.groupinfo;

	return (SV_PointContents( point ) == CONTENTS_SOLID);
}

/*
=============
ClearSaveDir

remove all the temp files HL1-HL3
(it will be extracted again from another .sav file)
=============
*/
static void ClearSaveDir( void )
{
	search_t	*t;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: the pending .sav copies these files
#endif

	// just delete all HL? files
	t = FS_Search( DEFAULT_SAVE_DIRECTORY "*.HL?", true, true );
	if( !t ) return; // already empty

	for( int i = 0; i < t->numfilenames; i++ )
		FS_Delete( t->filenames[i] );

	Mem_Free( t );
}

/*
=============
IsValidSave

savegame is allowed?
=============
*/
static int IsValidSave( void )
{
	if( !svs.initialized || sv.state != ss_active )
	{
		Con_Printf( "Not playing a local game.\n" );
		return 0;
	}

	// ignore autosave during background
	if( sv.background || UI_CreditsActive( ))
		return 0;

	if( svgame.physFuncs.SV_AllowSaveGame != NULL )
	{
		if( !svgame.physFuncs.SV_AllowSaveGame( ))
		{
			Con_Printf( "Savegame is not allowed.\n" );
			return 0;
		}
	}

	if( !CL_Active( ))
	{
		Con_Printf( "Can't save if not active.\n" );
		return 0;
	}

	if( CL_IsIntermission( ))
	{
		Con_Printf( "Can't save during intermission.\n" );
		return 0;
	}

	if( svs.maxclients != 1 )
	{
		Con_Printf( "Can't save multiplayer games.\n" );
		return 0;
	}

	if( svs.clients && svs.clients[0].state == cs_spawned )
	{
		edict_t	*pl = svs.clients[0].edict;

		if( !pl )
		{
			Con_Printf( "Can't savegame without a player!\n" );
			return 0;
		}

		if( pl->v.deadflag || pl->v.health <= 0.0f )
		{
			Con_Printf( "Can't savegame with a dead player\n" );
			return 0;
		}

		// Passed all checks, it's ok to save
		return 1;
	}

	Con_Printf( "Can't savegame without a client!\n" );

	return 0;
}

/*
=============
AgeSaveList

scroll the name list down
=============
*/
static void AgeSaveList( const char *pName, int count )
{
	char	newName[MAX_OSPATH], oldName[MAX_OSPATH];
	char	newShot[MAX_OSPATH], oldShot[MAX_OSPATH];

	// delete last quick/autosave (e.g. quick05.sav)
	Q_snprintf( newName, sizeof( newName ), DEFAULT_SAVE_DIRECTORY "%s%02d.sav", pName, count );
	Q_snprintf( newShot, sizeof( newShot ), DEFAULT_SAVE_DIRECTORY "%s%02d.bmp", pName, count );

	// only delete from game directory, basedir is read-only
	FS_Delete( newName );
	FS_Delete( newShot );

#if !XASH_DEDICATED
	// unloading the shot footprint
	GL_FreeImage( newShot );
#endif // XASH_DEDICATED

	while( count > 0 )
	{
		if( count == 1 )
		{
			// quick.sav
			Q_snprintf( oldName, sizeof( oldName ), DEFAULT_SAVE_DIRECTORY "%s.sav", pName );
			Q_snprintf( oldShot, sizeof( oldShot ), DEFAULT_SAVE_DIRECTORY "%s.bmp", pName );
		}
		else
		{
			// quick04.sav, etc.
			Q_snprintf( oldName, sizeof( oldName ), DEFAULT_SAVE_DIRECTORY "%s%02d.sav", pName, count - 1 );
			Q_snprintf( oldShot, sizeof( oldShot ), DEFAULT_SAVE_DIRECTORY "%s%02d.bmp", pName, count - 1 );
		}

		Q_snprintf( newName, sizeof( newName ), DEFAULT_SAVE_DIRECTORY "%s%02d.sav", pName, count );
		Q_snprintf( newShot, sizeof( newShot ), DEFAULT_SAVE_DIRECTORY "%s%02d.bmp", pName, count );

#if !XASH_DEDICATED
		// unloading the oldshot footprint too
		GL_FreeImage( oldShot );
#endif // XASH_DEDICATED

		// scroll the name list down (e.g. rename quick04.sav to quick05.sav)
		FS_Rename( oldName, newName );
		FS_Rename( oldShot, newShot );
		count--;
	}
}

/*
=============
DirectoryCopy

put the HL1-HL3 files into .sav file
=============
*/
static void DirectoryCopy( const char *pPath, file_t *pFile )
{
	search_t	*t;

	t = FS_Search( pPath, true, true );
	if( !t ) return; // nothing to copy ?

	for( int i = 0; i < t->numfilenames; i++ )
	{
		char	szName[MAX_OSPATH];
		file_t	*pCopy = FS_Open( t->filenames[i], "rb", true );
		int	fileSize = FS_FileLength( pCopy );

		memset( szName, 0, sizeof( szName )); // clearing the string to prevent garbage in output file
		Q_strncpy( szName, COM_FileWithoutPath( t->filenames[i] ), sizeof( szName ));
		SaveWrite( pFile, szName, MAX_OSPATH );
		SaveWrite( pFile, &fileSize, sizeof( int ));
		SaveCopy( pFile, pCopy, fileSize );
		FS_Close( pCopy );
	}
	Mem_Free( t );
}

/*
=============
DirectoryExtract

extract the HL1-HL3 files from the .sav file
=============
*/
static qboolean DirectoryExtract( file_t *pFile, int fileCount )
{
	for( int i = 0; i < fileCount; i++ )
	{
		char	szName[MAX_OSPATH];
		char	fileName[MAX_OSPATH];
		int	fileSize;
		file_t	*pCopy;

		// filename can only be as long as a map name + extension
		FS_Read( pFile, szName, MAX_OSPATH );
		FS_Read( pFile, &fileSize, sizeof( int ));
		Q_snprintf( fileName, sizeof( fileName ), DEFAULT_SAVE_DIRECTORY "%s", szName );
		COM_FixSlashes( fileName );

		pCopy = FS_Open( fileName, "wb", true );
		if( !pCopy )
		{
			Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, fileName );
			return false;
		}

		FS_FileCopy( pCopy, pFile, fileSize );
		FS_Close( pCopy );
	}

	return true;
}

/*
=============
SaveInit

initialize global save-restore buffer
=============
*/
static SAVERESTOREDATA *SaveInit( int size, int tokenCount )
{
	SAVERESTOREDATA	*pSaveData;

	pSaveData = Mem_Calloc( host.mempool, sizeof( SAVERESTOREDATA ) + size );
	pSaveData->pTokens = (char **)Mem_Calloc( host.mempool, tokenCount * sizeof( char* ));
	pSaveData->tokenCount = tokenCount;

	pSaveData->pBaseData = (char *)(pSaveData + 1); // skip the save structure);
	pSaveData->pCurrentData = pSaveData->pBaseData; // reset the pointer
	pSaveData->bufferSize = size;

	pSaveData->time = svgame.globals->time;	// Use DLL time

	// shared with dlls
	svgame.globals->pSaveData = pSaveData;

	return pSaveData;
}

#if XASH_XBOX
/*
=============
SaveInitUncleared

Resonance3D (docs/r3d/divergences.md #151): SaveInit for the two buffers
a save writes into (SaveGameState, SaveGameSlot). Upstream clears the
whole 4 MiB (Mem_Calloc, `sv_save.c:709` in the reference); a save
writes a few hundred KiB from the start and the files take only the
bytes written (`size`, then the token table StoreHashTable appends), so
the clearing of the rest - two 4 MiB passes per autosave, every page
touched - changes nothing a file receives. Upstream already relies on
that: SaveClientState reuses the HL1 buffer through SaveClear without
clearing it. The structure and the token table are cleared as before.
The load path keeps SaveInit: it parses the token table it reads in.
=============
*/
static SAVERESTOREDATA *SaveInitUncleared( int size, int tokenCount )
{
	SAVERESTOREDATA	*pSaveData;

	pSaveData = Mem_Malloc( host.mempool, sizeof( SAVERESTOREDATA ) + size );
	memset( pSaveData, 0, sizeof( SAVERESTOREDATA ));
	pSaveData->pTokens = (char **)Mem_Calloc( host.mempool, tokenCount * sizeof( char* ));
	pSaveData->tokenCount = tokenCount;

	pSaveData->pBaseData = (char *)(pSaveData + 1); // skip the save structure);
	pSaveData->pCurrentData = pSaveData->pBaseData; // reset the pointer
	pSaveData->bufferSize = size;

	pSaveData->time = svgame.globals->time;	// Use DLL time

	// shared with dlls
	svgame.globals->pSaveData = pSaveData;

	return pSaveData;
}
#else
#define SaveInitUncleared SaveInit
#endif

/*
=============
SaveClear

clearing buffer for reuse
=============
*/
static void SaveClear( SAVERESTOREDATA *pSaveData )
{
#if XASH_XBOX
	SaveStage_End(); // #151: staged bytes may live in this buffer
#endif

	memset( pSaveData->pTokens, 0, pSaveData->tokenCount * sizeof( char* ));

	pSaveData->pBaseData = (char *)(pSaveData + 1); // skip the save structure);
	pSaveData->pCurrentData = pSaveData->pBaseData; // reset the pointer
	pSaveData->time = svgame.globals->time;	// Use DLL time
	pSaveData->tokenSize = 0;	// reset the hashtable
	pSaveData->size = 0;	// reset the pointer

	// shared with dlls
	svgame.globals->pSaveData = pSaveData;
}

/*
=============
SaveFinish

release global save-restore buffer
=============
*/
static void SaveFinish( SAVERESTOREDATA *pSaveData )
{
	if( !pSaveData ) return;

#if XASH_XBOX
	SaveStage_End(); // #151: write out what is staged in this buffer before it is freed
#endif

	if( pSaveData->pTokens )
	{
		Mem_Free( pSaveData->pTokens );
		pSaveData->pTokens = NULL;
		pSaveData->tokenCount = 0;
	}

	if( pSaveData->pTable )
	{
		Mem_Free( pSaveData->pTable );
		pSaveData->pTable = NULL;
		pSaveData->tableCount = 0;
	}

	svgame.globals->pSaveData = NULL;
	Mem_Free( pSaveData );
}

/*
=============
StoreHashTable

write the stringtable into file
=============
*/
static char *StoreHashTable( SAVERESTOREDATA *pSaveData )
{
	char	*pTokenData = pSaveData->pCurrentData;

	// Write entity string token table
	if( pSaveData->pTokens )
	{
		for( int i = 0; i < pSaveData->tokenCount; i++ )
		{
			const char *pszToken = pSaveData->pTokens[i] ? pSaveData->pTokens[i] : "";

			// just copy the token byte-by-byte
			while( *pszToken )
				*pSaveData->pCurrentData++ = *pszToken++;
			*pSaveData->pCurrentData++ = 0; // Write the term
		}
	}

	pSaveData->tokenSize = pSaveData->pCurrentData - pTokenData;

	return pTokenData;
}

/*
=============
BuildHashTable

build the stringtable from buffer
=============
*/
static void BuildHashTable( SAVERESTOREDATA *pSaveData, file_t *pFile )
{
	char	*pszTokenList = pSaveData->pBaseData;

	// Parse the symbol table
	if( pSaveData->tokenSize > 0 )
	{
		FS_Read( pFile, pszTokenList, pSaveData->tokenSize );

		// make sure the token strings pointed to by the pToken hashtable.
		for( int i = 0; i < pSaveData->tokenCount; i++ )
		{
			pSaveData->pTokens[i] = *pszTokenList ? pszTokenList : NULL;
			while( *pszTokenList++ );	// Find next token (after next null)
		}
	}

	// rebase the data pointer
	pSaveData->pBaseData = pszTokenList;	// pszTokenList now points after token data
	pSaveData->pCurrentData = pSaveData->pBaseData;
}

/*
=============
GetClientDataSize

g-cont: this routine is redundant
i'm write it just for more readable code
=============
*/
static int GetClientDataSize( const char *level )
{
	int	tokenCount, tokenSize;
	int	size, id, version;
	char	name[MAX_QPATH];
	file_t	*pFile;

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL2", level );

	if(( pFile = FS_Open( name, "rb", true )) == NULL )
		return 0;

	FS_Read( pFile, &id, sizeof( id ));
	if( id != SAVEGAME_HEADER )
	{
		FS_Close( pFile );
		return 0;
	}

	FS_Read( pFile, &version, sizeof( version ));
	if( version != CLIENT_SAVEGAME_VERSION )
	{
		FS_Close( pFile );
		return 0;
	}

	FS_Read( pFile, &size, sizeof( int ));
	FS_Read( pFile, &tokenCount, sizeof( int ));
	FS_Read( pFile, &tokenSize, sizeof( int ));
	FS_Close( pFile );

	return ( size + tokenSize );
}

/*
=============
LoadSaveData

fill the save resore buffer
parse hash strings
=============
*/
static SAVERESTOREDATA *LoadSaveData( const char *level )
{
	int		tokenSize, tableCount;
	int		size, tokenCount;
	char		name[MAX_OSPATH];
	int		id, version;
	int		clientSize;
	SAVERESTOREDATA	*pSaveData;
	int		totalSize;
	file_t		*pFile;

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL1", level );
	Con_Printf( "Loading game from %s...\n", name );

	if(( pFile = FS_Open( name, "rb", true )) == NULL )
	{
		Con_Printf( S_ERROR "Couldn't open save data file %s.\n", name );
		return NULL;
	}

	// Read the header
	FS_Read( pFile, &id, sizeof( int ));
	FS_Read( pFile, &version, sizeof( int ));

	// is this a valid save?
	if( id != SAVEFILE_HEADER || version != SAVEGAME_VERSION )
	{
		FS_Close( pFile );
		return NULL;
	}

	// Read the sections info and the data
	FS_Read( pFile, &size, sizeof( int ));		// total size of all data to initialize read buffer
	FS_Read( pFile, &tableCount, sizeof( int ));	// entities count to right initialize entity table
	FS_Read( pFile, &tokenCount, sizeof( int ));	// num hash tokens to prepare token table
	FS_Read( pFile, &tokenSize, sizeof( int ));	// total size of hash tokens

	// determine highest size of seve-restore buffer
	// because it's used twice: for HL1 and HL2 restore
	clientSize = GetClientDataSize( level );
	totalSize = Q_max( clientSize, ( size + tokenSize ));

	// init the read buffer
	pSaveData = SaveInit( totalSize, tokenCount );

	Q_strncpy( pSaveData->szCurrentMapName, level, sizeof( pSaveData->szCurrentMapName ));
	pSaveData->tableCount = tableCount;		// count ETABLE entries
	pSaveData->tokenCount = tokenCount;
	pSaveData->tokenSize = tokenSize;

	// Parse the symbol table
	BuildHashTable( pSaveData, pFile );

	// Set up the restore basis
	pSaveData->fUseLandmark = true;
	pSaveData->time = 0.0f;

	// now reading all the rest of data
	FS_Read( pFile, pSaveData->pBaseData, size );
	FS_Close( pFile ); // data is sucessfully moved into SaveRestore buffer (ETABLE will be init later)

	return pSaveData;
}

/*
=============
ParseSaveTables

reading global data, setup ETABLE's
=============
*/
static void ParseSaveTables( SAVERESTOREDATA *pSaveData, SAVE_HEADER *pHeader, int updateGlobals )
{
	SAVE_LIGHTSTYLE	light;
	int		i;

	// Re-base the savedata since we re-ordered the entity/table / restore fields
	InitEntityTable( pSaveData, pSaveData->tableCount );

	for( i = 0; i < pSaveData->tableCount; i++ )
	{
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "ETABLE", &pSaveData->pTable[i], gEntityTable, ARRAYSIZE( gEntityTable ));
		pSaveData->pTable[i].pent = NULL;
	}

	pSaveData->pBaseData = pSaveData->pCurrentData;
	pSaveData->size = 0;

	// process SAVE_HEADER
	svgame.dllFuncs.pfnSaveReadFields( pSaveData, "Save Header", pHeader, gSaveHeader, ARRAYSIZE( gSaveHeader ));

	pSaveData->connectionCount = pHeader->connectionCount;
	VectorClear( pSaveData->vecLandmarkOffset );
	pSaveData->time = pHeader->time;
	pSaveData->fUseLandmark = true;

	// read adjacency list
	for( i = 0; i < pSaveData->connectionCount; i++ )
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "ADJACENCY", &pSaveData->levelList[i], gAdjacency, ARRAYSIZE( gAdjacency ));

	if( updateGlobals )
		memset( sv.lightstyles, 0, sizeof( sv.lightstyles ));

	for( i = 0; i < pHeader->lightStyleCount; i++ )
	{
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "LIGHTSTYLE", &light, gLightStyle, ARRAYSIZE( gLightStyle ));
		if( updateGlobals ) SV_SetLightStyle( light.index, light.style, light.time );
	}
}

/*
=============
EntityPatchWrite

write out the list of entities that are no longer in the save file for this level
(they've been moved to another level)
=============
*/
static qboolean EntityPatchWrite( SAVERESTOREDATA *pSaveData, const char *level )
{
	char	name[MAX_QPATH];
	int	i, size = 0;
	file_t	*pFile;

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL3", level );

	if(( pFile = FS_Open( name, "wb", true )) == NULL )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, name );
		return false;
	}

	for( i = 0; i < pSaveData->tableCount; i++ )
	{
		if( FBitSet( pSaveData->pTable[i].flags, FENTTABLE_REMOVED ))
			size++;
	}

	// patch count
	SaveWrite( pFile, &size, sizeof( int ));

	for( i = 0; i < pSaveData->tableCount; i++ )
	{
		if( FBitSet( pSaveData->pTable[i].flags, FENTTABLE_REMOVED ))
			SaveWrite( pFile, &i, sizeof( int ));
	}

	SaveClose( pFile );

	return true;
}

/*
=============
EntityPatchRead

read the list of entities that are no longer in the save file for this level
(they've been moved to another level)
=============
*/
static void EntityPatchRead( SAVERESTOREDATA *pSaveData, const char *level )
{
	char	name[MAX_QPATH];
	int	size;
	file_t	*pFile;

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL3", level );

	if(( pFile = FS_Open( name, "rb", true )) == NULL )
		return;

	// patch count
	FS_Read( pFile, &size, sizeof( int ));

	for( int i = 0; i < size; i++ )
	{
		int	entityId;

		FS_Read( pFile, &entityId, sizeof( int ));
		pSaveData->pTable[entityId].flags = FENTTABLE_REMOVED;
	}

	FS_Close( pFile );
}

/*
=============
RestoreDecal

restore decal\move across transition
=============
*/
static void RestoreDecal( SAVERESTOREDATA *pSaveData, decallist_t *entry, qboolean adjacent )
{
	int	decalIndex, entityIndex = 0;
	int	flags = entry->flags;
	int	modelIndex = 0;
	edict_t	*pEdict;

	// never move permanent decals
	if( adjacent && FBitSet( flags, FDECAL_PERMANENT ))
		return;

	// restore entity and model index
	pEdict = EdictFromTable( pSaveData, entry->entityIndex );

	if( SV_RestoreCustomDecal( entry, pEdict, adjacent ))
		return; // decal was sucessfully restored at the game-side

	// studio decals are handled at game-side
	if( FBitSet( flags, FDECAL_STUDIO ))
		return;

	if( SV_IsValidEdict( pEdict ))
		modelIndex = pEdict->v.modelindex;

	if( SV_IsValidEdict( pEdict ))
		entityIndex = NUM_FOR_EDICT( pEdict );

	decalIndex = pfnDecalIndex( entry->name );

	// this can happens if brush entity from previous level was turned into world geometry
	if( adjacent && entry->entityIndex != 0 && !SV_IsValidEdict( pEdict ))
	{
		trace_t	tr;

		Con_Printf( S_ERROR "RestoreDecal: couldn't restore entity index %i\n", entry->entityIndex );

		vec3_t testspot = Vec3( entry->position );
		VectorMA( testspot, 5.0f, entry->impactPlaneNormal, testspot );

		vec3_t testend = Vec3( entry->position );
		VectorMA( testend, -5.0f, entry->impactPlaneNormal, testend );

		tr = SV_Move( testspot, vec3_origin, vec3_origin, testend, MOVE_NOMONSTERS, NULL, false );

		// NOTE: this code may does wrong result on moving brushes e.g. func_tracktrain
		if( tr.fraction != 1.0f && !tr.allsolid )
		{
			// check impact plane normal
			float	dot = DotProduct( entry->impactPlaneNormal, tr.plane.normal );

			if( dot >= 0.95f )
			{
				entityIndex = pfnIndexOfEdict( tr.ent );
				if( entityIndex > 0 ) modelIndex = tr.ent->v.modelindex;
				SV_CreateDecal( &sv.signon, tr.endpos, decalIndex, entityIndex, modelIndex, flags, entry->scale );
			}
		}
	}
	else
	{
		// global entity is exist on new level so we can apply decal in local space
		SV_CreateDecal( &sv.signon, entry->position, decalIndex, entityIndex, modelIndex, flags, entry->scale );
	}
}

/*
=============
RestoreSound

continue playing sound from saved position
=============
*/
static void RestoreSound( SAVERESTOREDATA *pSaveData, soundlist_t *snd )
{
	edict_t	*ent = EdictFromTable( pSaveData, snd->entnum );
	int	flags = SND_RESTORE_POSITION;

	// this can happens if serialized map contain 4096 static decals...
	if( MSG_GetNumBytesLeft( &sv.signon ) < 36 )
		return;

	if( !snd->looping )
		SetBits( flags, SND_STOP_LOOPING );

	if( SV_BuildSoundMsg( &sv.signon, ent, snd->channel, snd->name, snd->volume * 255, snd->attenuation, flags, snd->pitch, snd->origin ))
	{
		// write extradata for svc_restoresound
		MSG_WriteByte( &sv.signon, snd->wordIndex );
		MSG_WriteBytes( &sv.signon, &snd->samplePos, sizeof( snd->samplePos ));
		MSG_WriteBytes( &sv.signon, &snd->forcedEnd, sizeof( snd->forcedEnd ));
	}
}

/*
=============
SaveClientState

write out the list of premanent decals for this level
=============
*/
static qboolean SaveClientState( SAVERESTOREDATA *pSaveData, const char *level, int changelevel )
{
	soundlist_t	soundInfo[MAX_CHANNELS];
	sv_client_t	*cl = svs.clients;
	char		name[MAX_QPATH];
	int		i;
	char		*pTokenData;
	decallist_t	*decalList = NULL;
	SAVE_CLIENT	header = { 0 };
	file_t		*pFile;

	// clearing the saving buffer to reuse
	SaveClear( pSaveData );

	header.entityCount = sv.num_static_entities;

	// initialize client header
#if !XASH_DEDICATED
	if( !Host_IsDedicated( ))
	{
		// g-cont. add space for studiodecals if present
		decalList = (decallist_t *)Mem_Calloc( host.mempool, sizeof( decallist_t ) * MAX_RENDER_DECALS * 2 );

		header.decalCount = ref.dllFuncs.R_CreateDecalList( decalList );

		if( !changelevel ) // sounds won't going across transition
		{
			header.soundCount = S_GetCurrentDynamicSounds( soundInfo, MAX_CHANNELS );

			// music not reqiured to save position: it's just continue playing on a next level
			S_StreamGetCurrentState(
				header.introTrack, sizeof( header.introTrack ),
				header.mainTrack, sizeof( header.mainTrack ),
				&header.trackPosition );
		}
	}
#endif // XASH_DEDICATED

	// save viewentity to allow camera works after save\restore
	if( SV_IsValidEdict( cl->pViewEntity ) && cl->pViewEntity != cl->edict )
		header.viewentity = NUM_FOR_EDICT( cl->pViewEntity );

	header.wateralpha = sv_wateralpha.value;
	header.wateramp = sv_wateramp.value;

	// Store the client header
	svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "ClientHeader", &header, gSaveClient, ARRAYSIZE( gSaveClient ));

	// store decals
	for( i = 0; decalList != NULL && i < header.decalCount; i++ )
	{
		// NOTE: apply landmark offset only for brush entities without origin brushes
		if( pSaveData->fUseLandmark && FBitSet( decalList[i].flags, FDECAL_USE_LANDMARK ))
			VectorSubtract( decalList[i].position, pSaveData->vecLandmarkOffset, decalList[i].position );

		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "DECALLIST", &decalList[i], gDecalEntry, ARRAYSIZE( gDecalEntry ));
	}

	if( decalList )
		Mem_Free( decalList );

	// write client entities
	for( i = 0; i < header.entityCount; i++ )
		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "STATICENTITY", &svs.static_entities[i], gStaticEntry, ARRAYSIZE( gStaticEntry ));

	// write sounds
	for( i = 0; i < header.soundCount; i++ )
		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "SOUNDLIST", &soundInfo[i], gSoundEntry, ARRAYSIZE( gSoundEntry ));

	// Write entity string token table
	pTokenData = StoreHashTable( pSaveData );

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL2", level );

	// output to disk
	if(( pFile = FS_Open( name, "wb", true )) == NULL )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, name );
		return false;
	}

	int version = CLIENT_SAVEGAME_VERSION;
	int id = SAVEGAME_HEADER;

	SaveWrite( pFile, &id, sizeof( id ));
	SaveWrite( pFile, &version, sizeof( version ));
	SaveWrite( pFile, &pSaveData->size, sizeof( int )); // does not include token table

	// write out the tokens first so we can load them before we load the entities
	SaveWrite( pFile, &pSaveData->tokenCount, sizeof( int ));
	SaveWrite( pFile, &pSaveData->tokenSize, sizeof( int ));
	SaveWrite( pFile, pTokenData, pSaveData->tokenSize );
	SaveWrite( pFile, pSaveData->pBaseData, pSaveData->size ); // header and globals
	SaveClose( pFile );

	return true;
}

/*
=============
LoadClientState

read the list of decals and reapply them again
=============
*/
static void LoadClientState( SAVERESTOREDATA *pSaveData, const char *level, qboolean changelevel, qboolean adjacent )
{
	int		tokenCount, tokenSize;
	int		i, size, id, version;
	sv_client_t	*cl = svs.clients;
	char		name[MAX_QPATH];
	soundlist_t	soundEntry;
	decallist_t	decalEntry;
	SAVE_CLIENT	header;
	file_t		*pFile;

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL2", level );

	if(( pFile = FS_Open( name, "rb", true )) == NULL )
		return; // something bad is happens

	FS_Read( pFile, &id, sizeof( id ));
	if( id != SAVEGAME_HEADER )
	{
		FS_Close( pFile );
		return;
	}

	FS_Read( pFile, &version, sizeof( version ));
	if( version != CLIENT_SAVEGAME_VERSION )
	{
		FS_Close( pFile );
		return;
	}

	FS_Read( pFile, &size, sizeof( int ));
	FS_Read( pFile, &tokenCount, sizeof( int ));
	FS_Read( pFile, &tokenSize, sizeof( int ));

	// sanity check
	ASSERT( pSaveData->bufferSize >= ( size + tokenSize ));

	// clearing the restore buffer to reuse
	SaveClear( pSaveData );
	pSaveData->tokenCount = tokenCount;
	pSaveData->tokenSize = tokenSize;

	// Parse the symbol table
	BuildHashTable( pSaveData, pFile );

	FS_Read( pFile, pSaveData->pBaseData, size );
	FS_Close( pFile );

	// Read the client header
	svgame.dllFuncs.pfnSaveReadFields( pSaveData, "ClientHeader", &header, gSaveClient, ARRAYSIZE( gSaveClient ));

	// restore decals
	for( i = 0; i < header.decalCount; i++ )
	{
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "DECALLIST", &decalEntry, gDecalEntry, ARRAYSIZE( gDecalEntry ));

		// NOTE: apply landmark offset only for brush entities without origin brushes
		if( pSaveData->fUseLandmark && FBitSet( decalEntry.flags, FDECAL_USE_LANDMARK ))
			VectorAdd( decalEntry.position, pSaveData->vecLandmarkOffset, decalEntry.position );
		RestoreDecal( pSaveData, &decalEntry, adjacent );
	}

	// clear old entities
	if( !adjacent )
	{
		memset( svs.static_entities, 0, sizeof( entity_state_t ) * MAX_STATIC_ENTITIES );
		sv.num_static_entities = 0;
	}

	// restore client entities
	for( i = 0; i < header.entityCount; i++ )
	{
		id = sv.num_static_entities;
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "STATICENTITY", &svs.static_entities[id], gStaticEntry, ARRAYSIZE( gStaticEntry ));
		if( adjacent ) continue; // static entities won't loading from adjacent levels

		if( SV_CreateStaticEntity( &sv.signon, id ))
			sv.num_static_entities++;
	}

	// restore sounds
	for( i = 0; i < header.soundCount; i++ )
	{
		svgame.dllFuncs.pfnSaveReadFields( pSaveData, "SOUNDLIST", &soundEntry, gSoundEntry, ARRAYSIZE( gSoundEntry ));
		if( adjacent ) continue; // sounds don't going across the levels

		RestoreSound( pSaveData, &soundEntry );
	}

	if( !adjacent )
	{
		// restore camera view here
		edict_t	*pent = pSaveData->pTable[bound( 0, (word)header.viewentity, pSaveData->tableCount )].pent;

		if( !COM_StringEmpty( header.introTrack ))
		{
			// NOTE: music is automatically goes across transition, never restore it on changelevel
			MSG_BeginServerCmd( &sv.signon, svc_stufftext );
			MSG_WriteStringf( &sv.signon, "music \"%s\" \"%s\" %i\n", header.introTrack, header.mainTrack, header.trackPosition );
		}

		// don't go camera across the levels
		if( header.viewentity > svs.maxclients && !changelevel )
			cl->pViewEntity = pent;

		// restore some client cvars
		Cvar_SetValue( "sv_wateralpha", header.wateralpha );
		Cvar_SetValue( "sv_wateramp", header.wateramp );
	}
}

/*
=============
CreateEntitiesInRestoreList

alloc private data for restored entities
=============
*/
static void CreateEntitiesInRestoreList( SAVERESTOREDATA *pSaveData, int levelMask, qboolean create_world )
{
	int		i, active;
	ENTITYTABLE	*pTable;
	edict_t		*pent;

	// create entity list
	if( svgame.physFuncs.pfnCreateEntitiesInRestoreList != NULL )
	{
		svgame.physFuncs.pfnCreateEntitiesInRestoreList( pSaveData, levelMask, create_world );
	}
	else
	{
		for( i = 0; i < pSaveData->tableCount; i++ )
		{
			pTable = &pSaveData->pTable[i];
			pent = NULL;

			if( pTable->classname && pTable->size && ( !FBitSet( pTable->flags, FENTTABLE_REMOVED ) || !create_world ))
			{
				if( !create_world )
					active = FBitSet( pTable->flags, levelMask ) ? 1 : 0;
				else active = 1;

				if( pTable->id == 0 && create_world ) // worldspawn
				{
					pent = SV_EdictNum( 0 );
					SV_InitEdict( pent );
					pent = SV_CreateNamedEntity( pent, pTable->classname );
				}
				else if(( pTable->id > 0 ) && ( pTable->id < svs.maxclients + 1 ))
				{
					edict_t	*ed = SV_EdictNum( pTable->id );

					if( !FBitSet( pTable->flags, FENTTABLE_PLAYER ))
						Con_Printf( S_ERROR "ENTITY IS NOT A PLAYER: %d\n", i );

					// create the player
					if( active && SV_IsValidEdict( ed ))
						pent = SV_CreateNamedEntity( ed, pTable->classname );
				}
				else if( active )
				{
					pent = SV_CreateNamedEntity( NULL, pTable->classname );
				}
			}

			pTable->pent = pent;
		}
	}
}

/*
=============
SaveGameState

save current game state
=============
*/
static SAVERESTOREDATA *SaveGameState( int changelevel )
{
	char		name[MAX_QPATH];
	int		i, id, version;
	char		*pTableData;
	char		*pTokenData;
	SAVERESTOREDATA	*pSaveData;
	int		tableSize;
	int		dataSize;
	ENTITYTABLE	*pTable;
	SAVE_HEADER	header;
	SAVE_LIGHTSTYLE	light;
	file_t		*pFile;

	if( !svgame.dllFuncs.pfnParmsChangeLevel )
		return NULL;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: this rewrites <map>.HL1-3, which a pending .sav copies
#endif

	XBOX_FT_SPAN( XBOX_FT_S_SAVE_INIT, pSaveData = SaveInitUncleared( SAVE_HEAPSIZE, SAVE_HASHSTRINGS ));
	XBOX_FT_SPAN_BEGIN( ft_ents );

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.HL1", sv.name );
	COM_FixSlashes( name );

	// initialize entity table to count moved entities
	InitEntityTable( pSaveData, svgame.numEntities );

	// Build the adjacent map list
	svgame.dllFuncs.pfnParmsChangeLevel();

	// Write the global data
	header.skillLevel = (int)skill.value;	// this is created from an int even though it's a float
	header.entityCount = pSaveData->tableCount;
	header.connectionCount = pSaveData->connectionCount;
	header.time = svgame.globals->time;	// use DLL time
	Q_strncpy( header.mapName, sv.name, sizeof( header.mapName ));
	Q_strncpy( header.skyName, sv_skyname.string, sizeof( header.skyName ));
	header.skyColor_r = sv_skycolor_r.value;
	header.skyColor_g = sv_skycolor_g.value;
	header.skyColor_b = sv_skycolor_b.value;
	header.skyVec_x = sv_skyvec_x.value;
	header.skyVec_y = sv_skyvec_y.value;
	header.skyVec_z = sv_skyvec_z.value;
	header.lightStyleCount = 0;

	// counting the lightstyles
	for( i = 0; i < MAX_LIGHTSTYLES; i++ )
	{
		if( sv.lightstyles[i].pattern[0] )
			header.lightStyleCount++;
	}

	// Write the main header
	pSaveData->time = 0.0f; // prohibits rebase of header.time (keep compatibility with old saves)
	svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "Save Header", &header, gSaveHeader, ARRAYSIZE( gSaveHeader ));
	pSaveData->time = header.time;

	// Write the adjacency list
	for( i = 0; i < pSaveData->connectionCount; i++ )
		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "ADJACENCY", &pSaveData->levelList[i], gAdjacency, ARRAYSIZE( gAdjacency ));

	// Write the lightstyles
	for( i = 0; i < MAX_LIGHTSTYLES; i++ )
	{
		if( !sv.lightstyles[i].pattern[0] )
			continue;

		Q_strncpy( light.style, sv.lightstyles[i].pattern, sizeof( light.style ));
		light.time = sv.lightstyles[i].time;
		light.index = i;

		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "LIGHTSTYLE", &light, gLightStyle, ARRAYSIZE( gLightStyle ));
	}

	// build the table of entities
	// this is used to turn pointers into savable indices
	// build up ID numbers for each entity, for use in pointer conversions
	// if an entity requires a certain edict number upon restore, save that as well
	for( i = 0; i < svgame.numEntities; i++ )
	{
		pTable = &pSaveData->pTable[i];
		pTable->location = pSaveData->size;
		pSaveData->currentIndex = i;
		pTable->size = 0;

		if( !SV_IsValidEdict( pTable->pent ))
			continue;

		svgame.dllFuncs.pfnSave( pTable->pent, pSaveData );

		if( FBitSet( pTable->pent->v.flags, FL_CLIENT ))
			SetBits( pTable->flags, FENTTABLE_PLAYER );
	}

	// total data what includes:
	// 1. save header
	// 2. adjacency list
	// 3. lightstyles
	// 4. all the entity data
	dataSize = pSaveData->size;

	// Write entity table
	pTableData = pSaveData->pCurrentData;

	for( i = 0; i < pSaveData->tableCount; i++ )
		svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "ETABLE", &pSaveData->pTable[i], gEntityTable, ARRAYSIZE( gEntityTable ));

	tableSize = pSaveData->size - dataSize;

	// Write entity string token table
	pTokenData = StoreHashTable( pSaveData );
	XBOX_FT_SPAN_END( ft_ents, XBOX_FT_S_SAVE_ENTS );
	XBOX_FT_SPAN_BEGIN( ft_hl1 );

	// output to disk
	if(( pFile = FS_Open( name, "wb", true )) == NULL )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, name );
		SaveFinish( pSaveData );
		return NULL;
	}

	// Write the header -- THIS SHOULD NEVER CHANGE STRUCTURE, USE SAVE_HEADER FOR NEW HEADER INFORMATION
	// THIS IS ONLY HERE TO IDENTIFY THE FILE AND GET IT'S SIZE.
	version = SAVEGAME_VERSION;
	id = SAVEFILE_HEADER;

	// write the header
	SaveWrite( pFile, &id, sizeof( id ));
	SaveWrite( pFile, &version, sizeof( version ));

	// Write out the tokens and table FIRST so they are loaded in the right order, then write out the rest of the data in the file.
	SaveWrite( pFile, &pSaveData->size, sizeof( int ));	// total size of all data to initialize read buffer
	SaveWrite( pFile, &pSaveData->tableCount, sizeof( int ));	// entities count to right initialize entity table
	SaveWrite( pFile, &pSaveData->tokenCount, sizeof( int ));	// num hash tokens to prepare token table
	SaveWrite( pFile, &pSaveData->tokenSize, sizeof( int ));	// total size of hash tokens
	SaveWrite( pFile, pTokenData, pSaveData->tokenSize );	// write tokens into the file
	SaveWrite( pFile, pTableData, tableSize );		// dump ETABLE structures
	SaveWrite( pFile, pSaveData->pBaseData, dataSize );	// and finally store all the other data
	SaveClose( pFile );

	if( !EntityPatchWrite( pSaveData, sv.name ))
	{
		SaveFinish( pSaveData );
		return NULL;
	}
	XBOX_FT_SPAN_END( ft_hl1, XBOX_FT_S_SAVE_HL1 );

	qboolean client_saved;
	XBOX_FT_SPAN( XBOX_FT_S_SAVE_CLIENT, client_saved = SaveClientState( pSaveData, sv.name, changelevel ));
	if( !client_saved )
	{
		SaveFinish( pSaveData );
		return NULL;
	}

	return pSaveData;
}

/*
=============
LoadGameState

load current game state
=============
*/
static int LoadGameState( char const *level, qboolean changelevel )
{
	SAVERESTOREDATA	*pSaveData;
	SAVE_HEADER	header;

	pSaveData = LoadSaveData( level );
	if( !pSaveData ) return 0; // couldn't load the file

	// must set mapname before calling into DLL
	Q_strncpy( sv.name, level, sizeof( sv.name ));
	svgame.globals->mapname = SV_MakeString( sv.name );

	ParseSaveTables( pSaveData, &header, true );
	EntityPatchRead( pSaveData, level );

	// pause until all clients connect
	sv.loadgame = sv.paused = true;

	Cvar_SetValue( "skill", header.skillLevel );
	Cvar_Set( "sv_skyname", header.skyName );

	// restore sky parms
	Cvar_SetValue( "sv_skycolor_r", header.skyColor_r );
	Cvar_SetValue( "sv_skycolor_g", header.skyColor_g );
	Cvar_SetValue( "sv_skycolor_b", header.skyColor_b );
	Cvar_SetValue( "sv_skyvec_x", header.skyVec_x );
	Cvar_SetValue( "sv_skyvec_y", header.skyVec_y );
	Cvar_SetValue( "sv_skyvec_z", header.skyVec_z );

	// create entity list
	CreateEntitiesInRestoreList( pSaveData, 0, true );

	// now spawn entities
	for( int i = 0; i < pSaveData->tableCount; i++ )
	{
		ENTITYTABLE	*pTable = &pSaveData->pTable[i];
		edict_t		*pent;

		pSaveData->pCurrentData = pSaveData->pBaseData + pTable->location;
		pSaveData->size = pTable->location;
		pSaveData->currentIndex = i;
		pent = pTable->pent;

		if( pent != NULL )
		{
			if( svgame.dllFuncs.pfnRestore( pent, pSaveData, 0 ) < 0 )
			{
				SetBits( pent->v.flags, FL_KILLME );
				pTable->pent = NULL;
			}
			else
			{
				// force the entity to be relinked
//				SV_LinkEdict( pent, false );
			}
		}
	}

	LoadClientState( pSaveData, level, changelevel, false );

	SaveFinish( pSaveData );

	// restore server time
	sv.time = header.time;

	return 1;
}

#if XASH_XBOX
/*
Resonance3D (docs/r3d/divergences.md #153): the .sav written after the
save's frame. Upstream's SaveGameSlot writes the whole .sav in the frame
the game is saved (`sv_save.c:1732-1758` in the reference): the header,
tokens and globals, then every <map>.HL? of the chapter (DirectoryCopy,
`:639-660`) - the largest part of an autosave stall on the console, and
it grows with the chapter. Everything that depends on game state is
still taken in that frame (the game DLL's serialisation, the .HL1-3 of
the current map, the game header and globals, the aged names); only
copying the .HL? files into the .sav, and the .sav's write, move to the
following frames, sv_save_async KiB a frame (xbox_save_stream.c makes
the bytes). The .sav is written as <name>.tmp, renamed <name>.new when
complete and then over <name>.sav, so no listing or load sees a partial
one and a power cut keeps either the old save or the complete new one
(SaveAsync_Sweep). Everything that reads or changes the save folder
calls SV_SaveAsync_Flush first (the list is in #153), so the .HL? files
cannot change under the copy and nobody sees the folder without the
save.
*/
#include "platform/xbox/xbox_save_stream.h"

CVAR_DEFINE_AUTO( sv_save_async, "32", 0, "Xbox: KiB of a savegame written per frame after the save's frame (0: the whole save in its frame)" );

#define SAVE_ASYNC_MIN_KB 4
#define SAVE_ASYNC_MAX_KB 256
#define SAVE_ASYNC_MAX_OPENS 4 // .HL? files a frame may open: each open, read and close costs ~0.5-2 ms in xemu
#define SAVE_ASYNC_MAX_HEAD ( 64 * 1024 ) // larger globals and tokens (a mod's): write in the frame, as upstream
#define SAVE_ASYNC_FLUSH_BYTES ( 1024 * 1024 ) // a flush's chunk: every write call has a fixed cost
#define SAVE_ASYNC_SLOW_FRAME ( 1.0 / 30.0 ) // after a slower frame (a load's first frames) the pump waits...
#define SAVE_ASYNC_MAX_WAITS 60              // ...but not more frames than this in a row

STATIC_ASSERT( XBOX_SAVE_STREAM_NAME_BYTES == MAX_OSPATH, "the .sav's per-file name field is DirectoryCopy's szName[MAX_OSPATH]" );

#if !XASH_DEDICATED
// the frame-time recorder (#141): KiB written per frame, flush time
#define SAVE_ASYNC_FT_KB( n ) ( xbox_ft_work.c[XBOX_FT_W_SAVE_PUMP_KB] += (uint32_t)((( n ) + 1023u ) / 1024u ))
#define SAVE_ASYNC_FT_FLUSH_END( var ) xbox_ft_time_add( &( var ), XBOX_FT_W_SAVE_FLUSH_KC )
#else
#define SAVE_ASYNC_FT_KB( n )
#define SAVE_ASYNC_FT_FLUSH_END( var )
#endif

typedef struct save_async_s
{
	xbox_save_stream_t stream;
	file_t   *out;      // the open <name>.tmp; NULL: no save pending
	search_t *files;    // the *.HL? list taken in the save's frame
	byte     *head;     // the .sav's leading bytes (header, tokens, globals)
	byte     *chunk;    // the chunk's allocation, made by the first pump step (after the 4 MiB save buffer is gone)
	int       frame;    // host.framecount of the save: the pump starts on the next frame
	int       waits;    // frames in a row the pump let pass after a slow frame
	qboolean  written;  // every byte is in the .tmp: the next step renames
	qboolean  failed;
	qboolean  busy;     // in SV_SaveAsync_Flush
	uint      bytes;    // written so far
	uint      steps;    // chunks written
	char      name[MAX_QPATH];
	char      tmpname[MAX_QPATH];
	char      newname[MAX_QPATH];
} save_async_t;

static save_async_t save_async;

static void *SaveAsync_Open( void *ctx, uint32_t index, char *name, size_t name_size, int32_t *length )
{
	const char *path = save_async.files->filenames[index];
	file_t *f = FS_Open( path, "rb", true ); // DirectoryCopy's FS_Open, FS_FileLength, Q_strncpy

	*length = (int32_t)FS_FileLength( f );
	Q_strncpy( name, COM_FileWithoutPath( path ), name_size );
	return f;
}

static int32_t SaveAsync_Read( void *ctx, void *handle, void *dst, int32_t size )
{
	return (int32_t)FS_Read( handle, dst, size );
}

static void SaveAsync_Close( void *ctx, void *handle )
{
	FS_Close( handle );
}

// Takes what the .sav needs from the save's frame and opens <name>.tmp.
// False (nothing pending, nothing changed) when the deferred write is off
// or does not apply: the caller then writes the .sav in the frame, as
// upstream.
static qboolean SaveAsync_Begin( const char *name, const char *pSaveName, SAVERESTOREDATA *pSaveData, const char *pTokenData, const char *hlPath )
{
	const xbox_save_stream_io_t io = { SaveAsync_Open, SaveAsync_Read, SaveAsync_Close, NULL };
	int version = SAVEGAME_VERSION;
	int id = SAVEGAME_HEADER;
	size_t head;
	byte *p;

	// with a .HL? write already short, the in-frame path reports the
	// failure (#151); after the game DLL's buffer overflow (#151,
	// SaveStage_Begin) the "globals" are the whole 4 MiB; a pending save
	// was flushed by SV_SaveGame
	if( sv_save_async.value <= 0.0f || save_async.out || save_io_failed || pSaveData->size >= pSaveData->bufferSize )
		return false;

	// SaveGameSlot's seven writes before DirectoryCopy, in their order
	head = sizeof( int ) * 5 + pSaveData->tokenSize + pSaveData->size;
	if( head > SAVE_ASYNC_MAX_HEAD )
		return false;

	Q_snprintf( save_async.tmpname, sizeof( save_async.tmpname ), DEFAULT_SAVE_DIRECTORY "%s.tmp", pSaveName );
	Q_snprintf( save_async.newname, sizeof( save_async.newname ), DEFAULT_SAVE_DIRECTORY "%s.new", pSaveName );
	COM_FixSlashes( save_async.tmpname );
	COM_FixSlashes( save_async.newname );
	if(( save_async.out = FS_Open( save_async.tmpname, "wb", true )) == NULL )
		return false;
	Q_strncpy( save_async.name, name, sizeof( save_async.name ));

	save_async.head = p = Mem_Malloc( host.mempool, head );
	memcpy( p, &id, sizeof( int )); p += sizeof( int );
	memcpy( p, &version, sizeof( int )); p += sizeof( int );
	memcpy( p, &pSaveData->size, sizeof( int )); p += sizeof( int ); // does not include token table
	memcpy( p, &pSaveData->tokenCount, sizeof( int )); p += sizeof( int );
	memcpy( p, &pSaveData->tokenSize, sizeof( int )); p += sizeof( int );
	memcpy( p, pTokenData, pSaveData->tokenSize ); p += pSaveData->tokenSize;
	memcpy( p, pSaveData->pBaseData, pSaveData->size ); // header and globals

	// DirectoryCopy's FS_Search, now: the files cannot change before the
	// copy ends (every writer flushes first)
	save_async.files = FS_Search( hlPath, true, true );

	// the chunk is set by the first step (SaveAsync_SetChunk)
	xbox_save_stream_init( &save_async.stream, &io, save_async.head, (uint32_t)head,
		save_async.files ? (uint32_t)save_async.files->numfilenames : 0u,
		NULL, 0, SAVE_STAGE_ALIGN );
	save_async.chunk = NULL;
	save_async.frame = host.framecount;
	save_async.waits = 0;
	save_async.written = false;
	save_async.failed = false;
	save_async.busy = false; // an error that left a flush by longjmp
	save_async.bytes = 0;
	save_async.steps = 0;
	return true;
}

// The stream's chunk: `bytes` from `alloc`, from its first page boundary.
static void SaveAsync_SetChunk( byte *alloc, size_t bytes )
{
	save_async.stream.chunk = alloc + ( SAVE_STAGE_ALIGN - (uintptr_t)alloc % SAVE_STAGE_ALIGN ) % SAVE_STAGE_ALIGN;
	save_async.stream.cap = (uint32_t)bytes;
}

// The save is complete or abandoned: close and free, then put the .sav in
// place or delete the .tmp. Everything is detached first, so a nested
// call (an error inside this one) finds nothing pending.
static void SaveAsync_End( void )
{
	file_t *out = save_async.out;
	search_t *files = save_async.files;
	byte *head = save_async.head, *chunk = save_async.chunk;

	save_async.out = NULL;
	save_async.files = NULL;
	save_async.head = save_async.chunk = NULL;
	xbox_save_stream_close( &save_async.stream );
	FS_Close( out );
	if( files ) Mem_Free( files );
	if( head ) Mem_Free( head );
	if( chunk ) Mem_Free( chunk );

	if( save_async.failed )
	{
		// #151: never leave a .sav whose bytes did not all reach the disk
		Con_Printf( S_ERROR "%s: %s is incomplete, removing it\n", __func__, save_async.name );
		FS_Delete( save_async.tmpname );
		return;
	}

	// .tmp -> .new marks it complete; nxdk's rename (MoveFileA) does not
	// replace an existing file (ReplaceIfExists FALSE), so the old .sav
	// goes before .new takes its name. Until then the old one stayed whole
	// (upstream truncated it in the save's frame). A cut in between leaves
	// a .new that SaveAsync_Sweep puts in place.
	FS_Delete( save_async.newname );
	if( !FS_Rename( save_async.tmpname, save_async.newname ))
	{
		Con_Printf( S_ERROR "%s: can't rename %s to %s\n", __func__, save_async.tmpname, save_async.newname );
		return;
	}
	FS_Delete( save_async.name );
	if( !FS_Rename( save_async.newname, save_async.name ))
		Con_Printf( S_ERROR "%s: can't rename %s to %s\n", __func__, save_async.newname, save_async.name );
	else
		Con_Reportf( "%s: %s written, %u bytes in %u step(s)\n", __func__, save_async.name, save_async.bytes, save_async.steps );
}

// Produces and writes one chunk (`max_opens` .HL? files opened at most,
// 0: no limit). Once everything is written the next call ends the save
// (the renames get a frame of their own), or this one if `now`.
static void SaveAsync_Step( uint max_opens, qboolean now )
{
	uint32_t len;
	const byte *p;

	if( !save_async.written )
	{
		if( !save_async.stream.chunk )
		{
			const size_t kb = bound( SAVE_ASYNC_MIN_KB, (int)sv_save_async.value, SAVE_ASYNC_MAX_KB );
			const size_t bytes = kb * 1024 / SAVE_STAGE_ALIGN * SAVE_STAGE_ALIGN;

			save_async.chunk = Mem_Malloc( host.mempool, bytes + SAVE_STAGE_ALIGN );
			SaveAsync_SetChunk( save_async.chunk, bytes );
		}

		len = xbox_save_stream_fill( &save_async.stream, max_opens );
		p = save_async.stream.chunk;
		if( save_async.stream.failed )
		{
			Con_Printf( S_ERROR "%s: unexpected end of a .HL? file\n", __func__ );
			save_async.failed = true;
		}

		// SaveStage_Flush's loop: keep writing after a short write
		while( !save_async.failed && len > 0 )
		{
			fs_offset_t written = FS_Write( save_async.out, p, len );

			if( written <= 0 )
			{
				Con_Printf( S_ERROR "%s: a save file write fell short (%u bytes not written)\n", __func__, (uint)len );
				save_async.failed = true;
				break;
			}
			p += written;
			len -= (uint32_t)written;
			save_async.bytes += (uint)written;
		}
		SAVE_ASYNC_FT_KB( (uint32_t)( p - save_async.stream.chunk ));
		xbox_save_stream_consumed( &save_async.stream );
		save_async.steps++;
		save_async.written = save_async.stream.done;

		if( !save_async.failed && !( save_async.written && now ))
			return;
	}

	SaveAsync_End();
}

/*
=============
SV_SaveAsync_Frame

Host_Frame, every frame: one chunk of a pending save, from the frame
after the save on. After a frame slower than 30 fps (a load's first
frames, where autosaves sit) it waits, up to SAVE_ASYNC_MAX_WAITS frames.
=============
*/
void SV_SaveAsync_Frame( void )
{
	if( !save_async.out || save_async.busy || host.framecount == save_async.frame )
		return;

	if( host.realframetime > SAVE_ASYNC_SLOW_FRAME && save_async.waits < SAVE_ASYNC_MAX_WAITS )
	{
		save_async.waits++;
		return;
	}
	save_async.waits = 0;

	XBOX_FT_SPAN_BEGIN( ft_pump );
	SaveAsync_Step( SAVE_ASYNC_MAX_OPENS, false );
	XBOX_FT_SPAN_END( ft_pump, XBOX_FT_S_SAVE_PUMP );
}

/*
=============
SV_SaveAsync_Flush

Completes a pending save now. Called before anything that reads or
changes the save folder (#153 lists them). Each write call has a fixed
cost (xemu: 922 KiB in 39 ms with one 1 MiB chunk, 102-122 ms in 64 KiB
ones), so it writes from a 1 MiB buffer when one can be had - plain
malloc, which can fail, freed before the caller goes on (the level change
and load after most flushes allocate the 4 MiB save buffer next) -
and from the pump's chunk otherwise. Entered again from inside itself
(an error while flushing reaching SV_Shutdown) it gives the save up.
=============
*/
void SV_SaveAsync_Flush( void )
{
	byte *big;

	if( !save_async.out )
		return;

	if( save_async.busy )
	{
		save_async.failed = true;
		SaveAsync_End();
		return;
	}

	save_async.busy = true;
	XBOX_FT_SPAN_BEGIN( ft_flush );
	big = save_async.written ? NULL : malloc( SAVE_ASYNC_FLUSH_BYTES + SAVE_STAGE_ALIGN );
	if( big )
		SaveAsync_SetChunk( big, SAVE_ASYNC_FLUSH_BYTES ); // the stream's chunk is empty between steps
	while( save_async.out )
		SaveAsync_Step( 0, true );
	free( big );
	SAVE_ASYNC_FT_FLUSH_END( ft_flush );
	save_async.busy = false;
}

/*
=============
SaveAsync_Sweep

Once a boot, with no save pending: a <name>.tmp is a deferred .sav that
a power cut or crash stopped short (the .sav it was replacing is still
there): deleted. A <name>.new is a complete one stopped between its two
renames: put in place when <name>.sav is gone, deleted otherwise.
Nothing else lists these names (every listing searches *.sav).
=============
*/
static void SaveAsync_Sweep( void )
{
	static qboolean swept;
	search_t *t;

	if( swept || save_async.out )
		return;
	swept = true;

	if(( t = FS_Search( DEFAULT_SAVE_DIRECTORY "*.tmp", true, true )) != NULL )
	{
		for( int i = 0; i < t->numfilenames; i++ )
			FS_Delete( t->filenames[i] );
		Mem_Free( t );
	}

	if(( t = FS_Search( DEFAULT_SAVE_DIRECTORY "*.new", true, true )) != NULL )
	{
		for( int i = 0; i < t->numfilenames; i++ )
		{
			char sav[MAX_QPATH];

			Q_strncpy( sav, t->filenames[i], sizeof( sav ));
			COM_ReplaceExtension( sav, ".sav", sizeof( sav ));
			if( FS_FileExists( sav, true ) || !FS_Rename( t->filenames[i], sav ))
				FS_Delete( t->filenames[i] );
		}
		Mem_Free( t );
	}
}
#endif // XASH_XBOX

/*
=============
SaveGameSlot

do a save game
=============
*/
static qboolean SaveGameSlot( const char *pSaveName, const char *pSaveComment )
{
	char		hlPath[MAX_QPATH];
	char		name[MAX_QPATH];
	char		*pTokenData;
	SAVERESTOREDATA	*pSaveData;
	GAME_HEADER	gameHeader;
	file_t		*pFile;

#if XASH_XBOX
	save_io_failed = false; // #151: every file of this save, the .HL? ones included, is checked from here
#endif
	pSaveData = SaveGameState( false );
	if( !pSaveData )
		return false;

	XBOX_FT_SPAN_BEGIN( ft_init );
	SaveFinish( pSaveData );
	pSaveData = SaveInitUncleared( SAVE_HEAPSIZE, SAVE_HASHSTRINGS ); // re-init the buffer
	XBOX_FT_SPAN_END( ft_init, XBOX_FT_S_SAVE_INIT );
	XBOX_FT_SPAN_BEGIN( ft_globals );

	Q_strncpy( hlPath, DEFAULT_SAVE_DIRECTORY "*.HL?", sizeof( hlPath ) );
	Q_strncpy( gameHeader.mapName, sv.name, sizeof( gameHeader.mapName )); // get the name of level where a player
	Q_strncpy( gameHeader.comment, pSaveComment, sizeof( gameHeader.comment ));
	gameHeader.mapCount = DirectoryCount( hlPath ); // counting all the adjacency maps

	// Store the game header
	svgame.dllFuncs.pfnSaveWriteFields( pSaveData, "GameHeader", &gameHeader, gGameHeader, ARRAYSIZE( gGameHeader ));

	// Write the game globals
	svgame.dllFuncs.pfnSaveGlobalState( pSaveData );

	// Write entity string token table
	pTokenData = StoreHashTable( pSaveData );
	XBOX_FT_SPAN_END( ft_globals, XBOX_FT_S_SAVE_GLOBALS );

	Q_snprintf( name, sizeof( name ), DEFAULT_SAVE_DIRECTORY "%s.sav", pSaveName );
	COM_FixSlashes( name );

	XBOX_FT_SPAN_BEGIN( ft_age );
	// output to disk
	if( !Q_stricmp( pSaveName, "quick" ))
		AgeSaveList( pSaveName, GI->quicksave_aged_count );
	else if( !Q_stricmp( pSaveName, "autosave" ))
		AgeSaveList( pSaveName, GI->autosave_aged_count );
	XBOX_FT_SPAN_END( ft_age, XBOX_FT_S_SAVE_AGE );
	XBOX_FT_SPAN_BEGIN( ft_sav );

#if XASH_XBOX
	// #153: copying the .HL? files into the .sav and writing it go to the
	// following frames; what they need from this frame is taken here
	if( SaveAsync_Begin( name, pSaveName, pSaveData, pTokenData, hlPath ))
	{
		// pending the preview image for savegame
		Cbuf_AddTextf( "saveshot \"%s\"\n", pSaveName );
		Con_Printf( "Saving game to %s...\n", name );
		SaveFinish( pSaveData );
		XBOX_FT_SPAN_END( ft_sav, XBOX_FT_S_SAVE_SAV );
		return true;
	}
#endif

	// output to disk
	if(( pFile = FS_Open( name, "wb", true )) == NULL )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, name );
		SaveFinish( pSaveData );
		return false;
	}

	// pending the preview image for savegame
	Cbuf_AddTextf( "saveshot \"%s\"\n", pSaveName );
	Con_Printf( "Saving game to %s...\n", name );

	int version = SAVEGAME_VERSION;
	int id = SAVEGAME_HEADER;

	SaveWrite( pFile, &id, sizeof( id ));
	SaveWrite( pFile, &version, sizeof( version ));
	SaveWrite( pFile, &pSaveData->size, sizeof( int )); // does not include token table

	// write out the tokens first so we can load them before we load the entities
	SaveWrite( pFile, &pSaveData->tokenCount, sizeof( int ));
	SaveWrite( pFile, &pSaveData->tokenSize, sizeof( int ));
	SaveWrite( pFile, pTokenData, pSaveData->tokenSize );
	SaveWrite( pFile, pSaveData->pBaseData, pSaveData->size ); // header and globals

	DirectoryCopy( hlPath, pFile );
	SaveFinish( pSaveData );
	SaveClose( pFile );
	XBOX_FT_SPAN_END( ft_sav, XBOX_FT_S_SAVE_SAV );

#if XASH_XBOX
	// #151: never leave a .sav whose bytes did not all reach the disk
	if( save_io_failed )
	{
		Con_Printf( S_ERROR "%s: %s is incomplete, removing it\n", __func__, name );
		FS_Delete( name );
		return false;
	}
#endif

	return true;
}

/*
=============
SaveReadHeader

read header of .sav file
=============
*/
static int SaveReadHeader( file_t *pFile, GAME_HEADER *pHeader )
{
	int		tokenCount, tokenSize;
	int		size, id, version;
	SAVERESTOREDATA	*pSaveData;

	FS_Read( pFile, &id, sizeof( id ));
	if( id != SAVEGAME_HEADER )
	{
		FS_Close( pFile );
		return 0;
	}

	FS_Read( pFile, &version, sizeof( version ));
	if( version != SAVEGAME_VERSION )
	{
		FS_Close( pFile );
		return 0;
	}

	FS_Read( pFile, &size, sizeof( int ));
	FS_Read( pFile, &tokenCount, sizeof( int ));
	FS_Read( pFile, &tokenSize, sizeof( int ));

	pSaveData = SaveInit( size + tokenSize, tokenCount );
	pSaveData->tokenCount = tokenCount;
	pSaveData->tokenSize = tokenSize;

	// Parse the symbol table
	BuildHashTable( pSaveData, pFile );

	// Set up the restore basis
	pSaveData->fUseLandmark = false;
	pSaveData->time = 0.0f;

	FS_Read( pFile, pSaveData->pBaseData, size );

	svgame.dllFuncs.pfnSaveReadFields( pSaveData, "GameHeader", pHeader, gGameHeader, ARRAYSIZE( gGameHeader ));

	svgame.dllFuncs.pfnRestoreGlobalState( pSaveData );

	SaveFinish( pSaveData );

	return 1;
}

/*
=============
CreateEntityTransitionList

moving edicts to another level
=============
*/
static int CreateEntityTransitionList( SAVERESTOREDATA *pSaveData, int levelMask )
{
	int		movedCount;

	movedCount = 0;

	// create entity list
	CreateEntitiesInRestoreList( pSaveData, levelMask, false );

	// now spawn entities
	for( int i = 0; i < pSaveData->tableCount; i++ )
	{
		ENTITYTABLE	*pTable = &pSaveData->pTable[i];
		edict_t		*pent;

		pSaveData->pCurrentData = pSaveData->pBaseData + pTable->location;
		pSaveData->size = pTable->location;
		pSaveData->currentIndex = i;
		pent = pTable->pent;

		if( SV_IsValidEdict( pent ) && FBitSet( pTable->flags, levelMask )) // screen out the player if he's not to be spawned
		{
			if( FBitSet( pTable->flags, FENTTABLE_GLOBAL ))
			{
				entvars_t	tmpVars;
				edict_t	*pNewEnt;

				// NOTE: we need to update table pointer so decals on the global entities with brush models can be
				// correctly moved. found the classname and the globalname for our globalentity
				svgame.dllFuncs.pfnSaveReadFields( pSaveData, "ENTVARS", &tmpVars, gTempEntvars, ARRAYSIZE( gTempEntvars ));

				// reset the save pointers, so dll can read this too
				pSaveData->pCurrentData = pSaveData->pBaseData + pTable->location;
				pSaveData->size = pTable->location;

				// IMPORTANT: we should find the already spawned or local restored global entity
				pNewEnt = SV_FindGlobalEntity( tmpVars.classname, tmpVars.globalname );

				Con_DPrintf( "Merging changes for global: %s\n", SV_GetString( pTable->classname ));

				// -------------------------------------------------------------------------
				// Pass the "global" flag to the DLL to indicate this entity should only override
				// a matching entity, not be spawned
				if( svgame.dllFuncs.pfnRestore( pent, pSaveData, 1 ) > 0 )
				{
					movedCount++;
				}
				else
				{
					if( SV_IsValidEdict( pNewEnt )) // update the table so decals can find parent entity
						pTable->pent = pNewEnt;
					SetBits( pent->v.flags, FL_KILLME );
				}
			}
			else
			{
				Con_Reportf( "Transferring %s (%d)\n", SV_GetString( pTable->classname ), NUM_FOR_EDICT( pent ));

				if( svgame.dllFuncs.pfnRestore( pent, pSaveData, 0 ) < 0 )
				{
					SetBits( pent->v.flags, FL_KILLME );
				}
				else
				{
					if( !FBitSet( pTable->flags, FENTTABLE_PLAYER ) && EntityInSolid( pent ))
					{
						// this can happen during normal processing - PVS is just a guess,
						// some map areas won't exist in the new map
						Con_Reportf( "Suppressing %s\n", SV_GetString( pTable->classname ));
						SetBits( pent->v.flags, FL_KILLME );
					}
					else
					{
						pTable->flags = FENTTABLE_REMOVED;
						movedCount++;
					}
				}
			}

			// remove any entities that were removed using UTIL_Remove()
			// as a result of the above calls to UTIL_RemoveImmediate()
			SV_FreeOldEntities ();
		}
	}

	return movedCount;
}

/*
=============
LoadAdjacentEnts

loading edicts from adjacency levels
=============
*/
static void LoadAdjacentEnts( const char *pOldLevel, const char *pLandmarkName )
{
	SAVE_HEADER	header;
	SAVERESTOREDATA	currentLevelData, *pSaveData;
	int		i, test, flags, index, movedCount = 0;
	qboolean		foundprevious = false;
	vec3_t		landmarkOrigin;

	memset( &currentLevelData, 0, sizeof( SAVERESTOREDATA ));
	svgame.globals->pSaveData = &currentLevelData;
	sv.loadgame = sv.paused = true;

	// build the adjacent map list
	svgame.dllFuncs.pfnParmsChangeLevel();

	for( i = 0; i < currentLevelData.connectionCount; i++ )
	{
		// make sure the previous level is in the connection list so we can
		// bring over the player.
		if( !Q_stricmp( currentLevelData.levelList[i].mapName, pOldLevel ))
			foundprevious = true;

		for( test = 0; test < i; test++ )
		{
			// only do maps once
			if( !Q_stricmp( currentLevelData.levelList[i].mapName, currentLevelData.levelList[test].mapName ))
				break;
		}

		// map was already in the list
		if( test < i ) continue;

		pSaveData = LoadSaveData( currentLevelData.levelList[i].mapName );

		if( pSaveData )
		{
			ParseSaveTables( pSaveData, &header, false );
			EntityPatchRead( pSaveData, currentLevelData.levelList[i].mapName );

			pSaveData->time = sv.time; // - header.time;
			pSaveData->fUseLandmark = true;
			flags = movedCount = 0;
			index = -1;

			// calculate landmark offset
			LandmarkOrigin( &currentLevelData, landmarkOrigin, pLandmarkName );
			LandmarkOrigin( pSaveData, pSaveData->vecLandmarkOffset, pLandmarkName );
			VectorSubtract( landmarkOrigin, pSaveData->vecLandmarkOffset, pSaveData->vecLandmarkOffset );

			if( !Q_stricmp( currentLevelData.levelList[i].mapName, pOldLevel ))
				SetBits( flags, FENTTABLE_PLAYER );

			while( 1 )
			{
				index = EntryInTable( pSaveData, sv.name, index );
				if( index < 0 ) break;
				SetBits( flags, BIT( index ));
			}

			if( flags ) movedCount = CreateEntityTransitionList( pSaveData, flags );

			// if ents were moved, rewrite entity table to save file
			if( movedCount )
			{
				if( !EntityPatchWrite( pSaveData, currentLevelData.levelList[i].mapName ))
				{
					SaveFinish( pSaveData );

					Host_Error( "Level transition ERROR\nCan't write entity table for %s while transitioning to %s from %s\n",
						currentLevelData.levelList[i].mapName, pOldLevel, sv.name );
				}
			}

			// move the decals from another level
			LoadClientState( pSaveData, currentLevelData.levelList[i].mapName, true, true );

			SaveFinish( pSaveData );
		}
	}

	svgame.globals->pSaveData = NULL;

	if( !foundprevious )
		Host_Error( "Level transition ERROR\nCan't find connection to %s from %s\n", pOldLevel, sv.name );
}

/*
=============
SV_LoadGameState

loading entities from the savegame
=============
*/
int SV_LoadGameState( char const *level )
{
	return LoadGameState( level, false );
}

/*
=============
SV_ClearGameState

clear current game state
=============
*/
void SV_ClearGameState( void )
{
	ClearSaveDir();

	if( svgame.dllFuncs.pfnResetGlobalState != NULL )
		svgame.dllFuncs.pfnResetGlobalState();
}

/*
=============
SV_ChangeLevel
=============
*/
void SV_ChangeLevel( qboolean loadfromsavedgame, const char *mapname, const char *start, qboolean background )
{
	char		level[MAX_QPATH];
	char		oldlevel[MAX_QPATH];
	char		_startspot[MAX_QPATH];
	char		*startspot = NULL;
	SAVERESTOREDATA	*pSaveData = NULL;
	XBOX_FT_LOAD_BEGIN( ft_all ); // #162: the whole change; its stages below and in sv_init.c

#if XASH_XBOX
	{
		XBOX_FT_LOAD_BEGIN( ft_flush );
		SV_SaveAsync_Flush(); // #153: a map change, smooth or not, with nothing pending across it
		XBOX_FT_LOAD_END( ft_flush, "sv_saveflush" );
	}
#endif

	if( sv.state != ss_active )
	{
		Con_Printf( S_ERROR "server not running\n");
		return;
	}

	if( start )
	{
		Q_strncpy( _startspot, start, sizeof( _startspot ));
		startspot = _startspot;
	}

	Q_strncpy( level, mapname, sizeof( level ));
	Q_strncpy( oldlevel, sv.name, sizeof( oldlevel ));

	if( loadfromsavedgame )
	{
		// smooth transition in-progress
		svgame.globals->changelevel = true;

		// save the current level's state
		XBOX_FT_LOAD_BEGIN( ft_save ); // #162
		pSaveData = SaveGameState( true );
		XBOX_FT_LOAD_END( ft_save, "sv_lvlsave" );

		if( !pSaveData )
		{
			// make user notice the error
			// do not use Host_Error, so the game progress won't be lost
			Sys_Warn( "Can't write save file for performaing change level; check permissions" );
			svgame.globals->changelevel = false;
			return;
		}
	}

	XBOX_FT_LOAD_BEGIN( ft_deact ); // #162
	SV_InactivateClients ();
	SV_FinalMessage( "", true );
	SV_DeactivateServer ();
	XBOX_FT_LOAD_END( ft_deact, "sv_deactivate" );

	if( !SV_SpawnServer( level, startspot, background ))
		return;	// ???

	if( loadfromsavedgame )
	{
		XBOX_FT_LOAD_BEGIN( ft_restore ); // #162

		// finish saving gamestate
		SaveFinish( pSaveData );

		if( !LoadGameState( level, true ))
			SV_SpawnEntities( level );
		LoadAdjacentEnts( oldlevel, startspot );

		if( sv_newunit.value )
			ClearSaveDir();
		XBOX_FT_LOAD_END( ft_restore, "sv_lvlrestore" );
		SV_ActivateServer( false );
	}
	else
	{
		// classic quake changelevel
		svgame.dllFuncs.pfnResetGlobalState();
		XBOX_FT_LOAD_BEGIN( ft_ents ); // #162
		SV_SpawnEntities( level );
		XBOX_FT_LOAD_END( ft_ents, "sv_ents" );
		SV_ActivateServer( true );
	}

	XBOX_FT_LOAD_END( ft_all, "sv_changelevel" );
}

/*
=============
SV_LoadGame
=============
*/
qboolean SV_LoadGame( const char *pPath )
{
	qboolean		validload = false;
	GAME_HEADER	gameHeader;
	file_t		*pFile;
	uint		flags;

	if( Host_IsDedicated() )
		return false;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: a load reads a .sav and replaces the .HL? files
#endif

	if( UI_CreditsActive( ))
		return false;

	if( COM_StringEmptyOrNULL( pPath ))
		return false;

	// silently ignore if missed
	if( !FS_FileExists( pPath, true ))
		return false;

	// initialize game if needs
	if( !SV_InitGame( false ))
		return false;

	svs.initialized = true;
	pFile = FS_Open( pPath, "rb", true );

	if( pFile )
	{
		SV_ClearGameState();

		if( SaveReadHeader( pFile, &gameHeader ))
			validload = DirectoryExtract( pFile, gameHeader.mapCount );

		FS_Close( pFile );

		if( validload )
		{
			// now check for map problems
			flags = SV_MapIsValid( gameHeader.mapName, NULL );

			if( FBitSet( flags, MAP_INVALID_VERSION ))
			{
				Con_Printf( S_ERROR "map %s is invalid or not supported\n", gameHeader.mapName );
				validload = false;
			}

			if( !FBitSet( flags, MAP_IS_EXIST ))
			{
				Con_Printf( S_ERROR "map %s doesn't exist\n", gameHeader.mapName );
				validload = false;
			}
		}
	}

	if( !validload )
	{
		Con_Printf( S_ERROR "Couldn't load %s\n", pPath );
		return false;
	}

	Con_Printf( "Loading game from %s...\n", pPath );
	Cvar_FullSet( "maxplayers", "1", FCVAR_LATCH );
	Cvar_SetValue( "deathmatch", 0 );
	Cvar_SetValue( "coop", 0 );
	COM_LoadGame( gameHeader.mapName );

	return true;
}

/*
==================
SV_SaveGame
==================
*/
qboolean SV_SaveGame( const char *pName )
{
	char   comment[80];
	string savename;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: one save at a time; "new" looks for a free name
#endif

	if( COM_StringEmptyOrNULL( pName ))
		return false;

	// can we save at this point?
	if( !IsValidSave( ))
		return false;

	if( !Q_stricmp( pName, "new" ))
	{
		int n;

		// scan for a free filename
		for( n = 0; n < 1000; n++ )
		{
			Q_snprintf( savename, sizeof( savename ), "save%03d", n );

			if( !FS_FileExists( va( DEFAULT_SAVE_DIRECTORY "%s.sav", savename ), true ))
				break;
		}

		if( n == 1000 )
		{
			Con_Printf( S_ERROR "no free slots for savegame\n" );
			return false;
		}
	}
	else Q_strncpy( savename, pName, sizeof( savename ));

#if !XASH_DEDICATED
	// unload previous image from memory (it's will be overwritten)
	GL_FreeImage( va( DEFAULT_SAVE_DIRECTORY "%s.bmp", savename ) );
#endif // XASH_DEDICATED

	SaveBuildComment( comment, sizeof( comment ));

	qboolean saved;
	XBOX_FT_SPAN( XBOX_FT_S_SAVE, saved = SaveGameSlot( savename, comment ));
	return saved;
}

static int SV_CompareFileTime( int ft1, int ft2 )
{
	if( ft1 < ft2 )
	{
		return -1;
	}
	else if( ft1 > ft2 )
	{
		return 1;
	}
	return 0;
}

/*
==================
SV_GetLatestSave

used for reload game after player death
==================
*/
const char *SV_GetLatestSave( void )
{
	static char	savename[MAX_QPATH];
	int		newest = 0;
	int		found = 0;
	search_t		*t;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: the newest save may be the pending one
#endif

	if(( t = FS_Search( DEFAULT_SAVE_DIRECTORY "*.sav" , true, true )) == NULL )
		return NULL;

	for( int i = 0; i < t->numfilenames; i++ )
	{
		int	ft = FS_FileTime( t->filenames[i], true );

		// found a match?
		if( ft > 0 )
		{
			// should we use the matched?
			if( !found || SV_CompareFileTime( newest, ft ) < 0 )
			{
				Q_strncpy( savename, t->filenames[i], sizeof( savename ));
				newest = ft;
				found = 1;
			}
		}
	}

	Mem_Free( t ); // release search

	if( found )
		return savename;
	return NULL;
}

/*
==================
SV_GetSaveComment

check savegame for valid
==================
*/
int GAME_EXPORT SV_GetSaveComment( const char *savename, char *comment )
{
	int	i, tag, size, nNumberOfFields, nFieldSize, tokenSize, tokenCount;
	char	*pData, *pSaveData, *pFieldName, **pTokenList;
	string	mapName, description;
	file_t	*f;

#if XASH_XBOX
	SV_SaveAsync_Flush(); // #153: the menu's save list reads each .sav's header
#endif

	if(( f = FS_Open( savename, "rb", true )) == NULL )
	{
		// just not exist - clear comment
		comment[0] = '\0';
		return 0;
	}

	FS_Read( f, &tag, sizeof( int ));
	if( tag != SAVEGAME_HEADER )
	{
		// invalid header
		Q_strncpy( comment, "<corrupted>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	FS_Read( f, &tag, sizeof( int ));

	if( tag == 0x0065 )
	{
		Q_strncpy( comment, "<old version "XASH_ENGINE_NAME" unsupported>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	if( tag < SAVEGAME_VERSION )
	{
		Q_strncpy( comment, "<old version>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	if( tag > SAVEGAME_VERSION )
	{
		// old xash version ?
		Q_strncpy( comment, "<invalid version>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	mapName[0] = '\0';
	comment[0] = '\0';

	FS_Read( f, &size, sizeof( int ));
	FS_Read( f, &tokenCount, sizeof( int ));	// These two ints are the token list
	FS_Read( f, &tokenSize, sizeof( int ));
	size += tokenSize;

	// sanity check.
	if( tokenCount < 0 || tokenCount > SAVE_HASHSTRINGS )
	{
		Q_strncpy( comment, "<corrupted hashtable>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	if( tokenSize < 0 || tokenSize > SAVE_HEAPSIZE )
	{
		Q_strncpy( comment, "<corrupted hashtable>", MAX_STRING );
		FS_Close( f );
		return 0;
	}

	pSaveData = (char *)Mem_Malloc( host.mempool, size );
	FS_Read( f, pSaveData, size );
	pData = pSaveData;

	// allocate a table for the strings, and parse the table
	if( tokenSize > 0 )
	{
		pTokenList = Mem_Calloc( host.mempool, tokenCount * sizeof( char* ));

		// make sure the token strings pointed to by the pToken hashtable.
		for( i = 0; i < tokenCount; i++ )
		{
			pTokenList[i] = *pData ? pData : NULL;	// point to each string in the pToken table
			while( *pData++ );			// find next token (after next null)
		}
	}
	else pTokenList = NULL;

	// short, short (size, index of field name)
	nFieldSize = *(short *)pData;
	pData += sizeof( short );
	pFieldName = pTokenList[*(short *)pData];

	if( Q_stricmp( pFieldName, "GameHeader" ))
	{
		Q_strncpy( comment, "<missing GameHeader>", MAX_STRING );
		if( pTokenList ) Mem_Free( pTokenList );
		if( pSaveData ) Mem_Free( pSaveData );
		FS_Close( f );
		return 0;
	}

	// int (fieldcount)
	pData += sizeof( short );
	nNumberOfFields = (int)*pData;
	pData += nFieldSize;

	// each field is a short (size), short (index of name), binary string of "size" bytes (data)
	for( i = 0; i < nNumberOfFields; i++ )
	{
		size_t size;
		// Data order is:
		// Size
		// szName
		// Actual Data
		nFieldSize = *(short *)pData;
		pData += sizeof( short );

		pFieldName = pTokenList[*(short *)pData];
		pData += sizeof( short );

		size = Q_min( nFieldSize, MAX_STRING );

		if( !Q_stricmp( pFieldName, "comment" ))
		{
			Q_strncpy( description, pData, size );
		}
		else if( !Q_stricmp( pFieldName, "mapName" ))
		{
			Q_strncpy( mapName, pData, size );
		}

		// move to start of next field.
		pData += nFieldSize;
	}

	// delete the string table we allocated
	if( pTokenList ) Mem_Free( pTokenList );
	if( pSaveData ) Mem_Free( pSaveData );
	FS_Close( f );

	// at least mapname should be filled
	if( !COM_StringEmpty( mapName ))
	{
		time_t		fileTime;
		const struct tm	*file_tm;
		string		timestring;
		uint		flags;

		// now check for map problems
		flags = SV_MapIsValid( mapName, NULL );

		if( FBitSet( flags, MAP_INVALID_VERSION ))
		{
			Q_snprintf( comment, MAX_STRING, "<map %s has invalid format>", mapName );
			return 0;
		}

		if( !FBitSet( flags, MAP_IS_EXIST ))
		{
			Q_snprintf( comment, MAX_STRING, "<map %s is missed>", mapName );
			return 0;
		}

		fileTime = FS_FileTime( savename, true );
		file_tm = localtime( &fileTime );

		// split comment to sections
		if( Q_strstr( savename, "quick" ))
			Q_snprintf( comment, CS_SIZE, "[quick]%s", description );
		else if( Q_strstr( savename, "autosave" ))
			Q_snprintf( comment, CS_SIZE, "[autosave]%s", description );
		else Q_strncpy( comment, description, CS_SIZE );
		strftime( timestring, sizeof ( timestring ), "%b%d %Y", file_tm );
		Q_strncpy( comment + CS_SIZE, timestring, CS_TIME );
		strftime( timestring, sizeof( timestring ), "%H:%M", file_tm );
		Q_strncpy( comment + CS_SIZE + CS_TIME, timestring, CS_TIME );
		Q_strncpy( comment + CS_SIZE + (CS_TIME * 2), description + CS_SIZE, CS_SIZE );

		return 1;
	}

	Q_strncpy( comment, "<unknown version>", MAX_STRING );

	return 0;
}

void SV_InitSaveRestore( void )
{
	pfnSaveGameComment = COM_GetProcAddress( svgame.hInstance, "SV_SaveGameComment" );

#if XASH_XBOX
	SaveAsync_Sweep(); // #153: a deferred .sav a power cut stopped
#endif
}
