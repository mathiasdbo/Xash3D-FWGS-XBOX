/*
mpak.c - MPAK support for filesystem

Part of Resonance3D; see docs/r3d/fork-plan.md's X2 section.

An .mpak archive is NOT a general named-file container like a .pak/.wad -
X2's scope narrowing (tools/asset-studio/src/formats/MpakHeader.h) limits it
to exactly one map's baked BSP geometry, level (WAD-resolved) textures and
lightmap atlas. One archive holds one map's data, full stop; there is no
internal directory to search, because there is nothing to disambiguate.

That shape doesn't need pak.c's binary-searched dpackfile_t table - a
mpak_s just remembers which single virtual name its one archive answers
to and hands back the whole file when asked for it. The server precaches
worldmodels as "maps/<name>.bsp" regardless of what actually produced the
bytes (sv_init.c:1025); Mod_LoadModel dispatches purely on the buffer's
own magic number (model.c's switch on *(uint*)buf), not the file
extension, so an .mpak archive answering to a ".bsp"-suffixed name is not
a lie - it's the same indirection pak.c already relies on for every file
it serves under a name that isn't its own.

Scope note, revised for MPAK v7: a map archive IS a BSP30 file - a real
dheader_t leads its first sector and every classic lump is a raw,
sector-aligned copy the engine's unmodified loaders consume
(docs/r3d/mpak-loading-design.md). So what this file serves under
"maps/<name>.bsp" is accepted as-is by SV_ValidateMap's from-disk
validation, by Mod_LoadModel's version switch and by Mod_LoadBmodelLumps;
no engine code knows the archive exists. This file's job is only the
filesystem plumbing: discovering the archive in a game directory,
probing that it really is one (dheader_t at 0 AND the MPAK descriptor at
MPAK_DESCRIPTOR_OFFSET), and answering the one name it represents.

Copyright (C) 2015-2023 Xash3D FWGS contributors (searchpath_t pattern,
adapted from pak.c/wad.c in this same directory)
Copyright (C) Resonance3D contributors (this file)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "build.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if XASH_POSIX
#include <unistd.h>
#endif
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "port.h"
#include "filesystem_internal.h"
#include "crtlib.h"
#include "common/com_strings.h"
#include "bspfile.h"     // dheader_t, HLBSP_VERSION - common/bspfile.h (see mpak_layout.h's include note)
#include "mpak_layout.h" // MPAK_LAYOUT_MAGIC/VERSION, MPAK_DESCRIPTOR_OFFSET

/*
========================================================================
MPAK FILES

One archive, one map, one virtual name - see this file's header comment.
========================================================================
*/

// Mirrors formats::MpakHeader6's first two members byte-for-byte
// (tools/asset-studio/src/formats/MpakHeader.h) - this reader only reads
// enough of the descriptor to validate the archive before serving it
// whole; it does not need the rest of the lump layout (see the scope
// note above).
//
// v7: the descriptor no longer sits at offset 0. A map archive IS a BSP30
// file - a real dheader_t leads the first sector, so every engine path
// that validates a map from disk before any loader runs accepts it
// unchanged (mpak_layout.h's MPAK_DESCRIPTOR_OFFSET note;
// docs/r3d/mpak-loading-design.md, F7) - and the descriptor follows at
// MPAK_DESCRIPTOR_OFFSET. The probe checks both: a plain .bsp renamed
// .mpak has no descriptor, and UiPacker's menu archives (descriptor at
// 0, no dheader_t) are not maps and are correctly no longer mounted as
// "maps/menu.bsp".
typedef struct
{
	char     magic[4];
	uint32_t version;
} mpak_probe_header_t;

#define MPAK_MAGIC   MPAK_LAYOUT_MAGIC
#define MPAK_VERSION MPAK_LAYOUT_VERSION

struct mpak_s
{
	file_t      *handle;
	fs_offset_t filesize;         // whole-archive length, served verbatim as the map's own bytes
	string      virtualName;      // "maps/<archive's own basename>.bsp" - the one name this archive answers to
};

/*
=================
FS_LoadMpak

Takes an explicit (not game tree related) path to an mpak file. Validates
the header and derives the one virtual name this archive answers to; does
not read or interpret anything past mpak_probe_header_t.
=================
*/
static mpak_t *FS_LoadMpak( const char *mpakfile )
{
	mpak_probe_header_t header;
	mpak_t *mpak;
	string basename;

	file_t *handle = FS_SysOpen( mpakfile, "rb" );

	if( handle == NULL )
	{
		Con_Reportf( "%s couldn't open: %s\n", mpakfile, strerror( errno ));
		return NULL;
	}

	byte firstSector[MPAK_DESCRIPTOR_OFFSET + sizeof( mpak_probe_header_t )];
	const dheader_t *bsp = (const dheader_t *)firstSector;

	if( FS_Read( handle, firstSector, sizeof( firstSector )) != sizeof( firstSector ))
	{
		Con_Reportf( S_ERROR "%s is smaller than an MPAK first sector. Ignored.\n", mpakfile );
		FS_Close( handle );
		return NULL;
	}

	if( bsp->version != HLBSP_VERSION )
	{
		Con_Reportf( "%s does not start with a BSP30 header. Ignored.\n", mpakfile );
		FS_Close( handle );
		return NULL;
	}

	memcpy( &header, firstSector + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));

	if( memcmp( header.magic, MPAK_MAGIC, sizeof( header.magic )) != 0 )
	{
		Con_Reportf( "%s has no MPAK descriptor at offset %u. Ignored.\n", mpakfile, MPAK_DESCRIPTOR_OFFSET );
		FS_Close( handle );
		return NULL;
	}

	// Hard version check, same posture as every prior .mpak bump
	// (MpakHeader.h's own doc comment): a v6 reader has no
	// partial-compatibility burden with any other version - every
	// archive needs a full rebake regardless.
	if( LittleLong( header.version ) != MPAK_VERSION )
	{
		Con_Reportf( S_ERROR "%s is MPAK version %lu, this build only reads version %u. Ignored.\n",
			mpakfile, (unsigned long)LittleLong( header.version ), MPAK_VERSION );
		FS_Close( handle );
		return NULL;
	}

	mpak = (mpak_t *)Mem_Calloc( fs_mempool, sizeof( mpak_t ));
	mpak->handle = handle;
	mpak->filesize = FS_FileLength( handle );

	COM_FileBase( mpakfile, basename, sizeof( basename ));
	Q_snprintf( mpak->virtualName, sizeof( mpak->virtualName ), "maps/%s.bsp", basename );

	// pak.c's equivalent XASH_REDUCE_FD block passes its own file_t* to
	// close(2) rather than the underlying int descriptor - a type
	// mismatch that only compiles as dead code today because this
	// project's build never defines XASH_REDUCE_FD (grep-confirmed: no
	// waf option or #define sets it anywhere in this tree). Rather than
	// carry forward an unverifiable pattern, this handle just stays open
	// for the archive's lifetime, same as every other real-archive
	// codepath actually exercised in this build.
	return mpak;
}

/*
===========
FS_OpenFile_MPAK

The whole archive IS the file this searchpath answers to - open it at
offset 0, length filesize, same as any other file this filesystem serves.
===========
*/
static file_t *FS_OpenFile_MPAK( searchpath_t *search, const char *filename, const char *mode, int pack_ind )
{
	return FS_OpenHandle( search, search->mpak->handle->handle, 0, search->mpak->filesize );
}

/*
===========
FS_FindFile_MPAK

A single-entry archive: matches only its own virtual name.
===========
*/
static int FS_FindFile_MPAK( searchpath_t *search, const char *path, char *fixedname, size_t len )
{
	if( Q_stricmp( search->mpak->virtualName, path ))
		return -1;

	if( fixedname )
		Q_strncpy( fixedname, search->mpak->virtualName, len );

	// index 0 is the only valid "found" value for a one-entry archive;
	// pfnOpenFile below ignores it regardless, same as FS_OpenFile_MPAK's
	// pak_ind-ignoring signature already documents.
	return 0;
}

/*
===========
FS_Search_MPAK

Matches the one virtual name against a glob, same path-stripping loop as
FS_Search_PAK (pak.c) so directory-level patterns (e.g. "maps/*.bsp" from
con_utils.c's map-name autocompletion) still see this archive's map.
===========
*/
static void FS_Search_MPAK( searchpath_t *search, stringlist_t *list, const char *pattern, int caseinsensitive )
{
	string temp;

	Q_strncpy( temp, search->mpak->virtualName, sizeof( temp ));
	while( temp[0] )
	{
		const char *slash, *backslash, *colon, *separator;

		if( matchpattern( temp, pattern, true ))
		{
			int j;

			for( j = 0; j < list->numstrings; j++ )
			{
				if( !Q_strcmp( list->strings[j], temp ))
					break;
			}

			if( j == list->numstrings )
				stringlistappend( list, temp );
		}

		slash = Q_strrchr( temp, '/' );
		backslash = Q_strrchr( temp, '\\' );
		colon = Q_strrchr( temp, ':' );
		separator = temp;
		if( separator < slash )
			separator = slash;
		if( separator < backslash )
			separator = backslash;
		if( separator < colon )
			separator = colon;
		*((char *)separator) = 0;
	}
}

/*
===========
FS_FileTime_MPAK
===========
*/
static int FS_FileTime_MPAK( searchpath_t *search, const char *filename )
{
	return search->mpak->handle->filetime;
}

/*
===========
FS_PrintInfo_MPAK
===========
*/
static void FS_PrintInfo_MPAK( searchpath_t *search, char *dst, size_t size )
{
	Q_snprintf( dst, size, "%s (%s)", search->filename, search->mpak->virtualName );
}

/*
===========
FS_Close_MPAK
===========
*/
static void FS_Close_MPAK( searchpath_t *search )
{
	if( search->mpak->handle != NULL )
		FS_Close( search->mpak->handle );
	Mem_Free( search->mpak );
}

/*
================
FS_AddMpak_Fullpath

Adds the given .mpak to the search path. FS_LoadMpak already reported why
on failure, so this only needs to propagate NULL.
================
*/
searchpath_t *FS_AddMpak_Fullpath( const char *mpakfile, int flags )
{
	searchpath_t *search;
	mpak_t *mpak = FS_LoadMpak( mpakfile );

	if( !mpak )
		return NULL;

	search = (searchpath_t *)Mem_Calloc( fs_mempool, sizeof( searchpath_t ));
	Q_strncpy( search->filename, mpakfile, sizeof( search->filename ));
	search->mpak = mpak;
	search->type = SEARCHPATH_MPAK;
	search->flags = flags;

	search->pfnPrintInfo = FS_PrintInfo_MPAK;
	search->pfnClose = FS_Close_MPAK;
	search->pfnOpenFile = FS_OpenFile_MPAK;
	search->pfnFileTime = FS_FileTime_MPAK;
	search->pfnFindFile = FS_FindFile_MPAK;
	search->pfnSearch = FS_Search_MPAK;
	// pfnLoadFile deliberately left NULL, same as pak.c/wad.c: the generic
	// open+read fallback already reads the whole archive in one shot
	// (this "archive" IS one file), so a custom pfnAlloc/pfnFree path
	// would only add complexity for no benefit measured yet. fork-plan.md's
	// X2 text describing "loading per resource through pfnLoadFile's
	// custom pfnAlloc/pfnFree" predates the scope-narrowing pivot away
	// from per-resource storage and is stale on this specific point.

	Con_Reportf( "Adding MPAK: %s (%s)\n", mpakfile, mpak->virtualName );

	return search;
}
