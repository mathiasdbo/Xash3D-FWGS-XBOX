/*
filesystem.h - engine FS
Copyright (C) 2003-2006 Mathieu Olivier
Copyright (C) 2000-2007 DarkPlaces contributors
Copyright (C) 2007 Uncle Mike
Copyright (C) 2015-2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef FILESYSTEM_INTERNAL_H
#define FILESYSTEM_INTERNAL_H

// build.h explicitly, ahead of everything else: found in review, 2026-09-07
// (X1, static-linking dedup) - this file needs XASH_XBOX defined before the
// rename block right below can gate on it, and nothing upstream of here
// guarantees a caller already pulled build.h in first. build.h has its own
// #pragma once and #ifndef guard, so including it again here is free.
#include "build.h"

// Ten symbols renamed here, scoped to this header (included by every
// filesystem/*.c and nowhere in engine/ - confirmed by grep, 2026-09-07),
// so this touches nothing outside filesystem/. Found in review: linking
// engine/filesystem/public/common/compat's Xbox object files together
// (`deps/nxdk/bin/nxdk-link -errorlimit:0`, no libraries, no entry point -
// just checking for duplicate/undefined symbols, matching
// x0-probe-results.md's own X1 methodology) produces exactly ten duplicate
// symbol errors, all one shape: filesystem/*.c's REAL implementation
// collides with a same-named symbol engine/ already owns. Reproduced this
// session, not assumed from the earlier probe - the first re-run this
// session count nine MORE (SNDDMA_*/VoiceCapture_* between
// platform/sdl1/s_sdl1.c and platform/sdl2/s_sdl2.c) that turned out to be
// this session's own measurement artifact, not a real collision: a real
// Xbox build only ever selects one SDL sound backend, never both, and
// tools/xbox-sweep.sh compiles every platform/*.c file individually
// regardless of which one a real build would choose - the identical
// "N of M compile has never meant N of M would link" gap that script's own
// header already names. Excluding platform/sdl1, sdl3, win32, posix, dos,
// psp, nswitch, psvita, android, linux, stub (none of which a real Xbox
// build selects) and filesystem/tests+public/tests (unit-test mains, not
// engine code) brought the count back to exactly the ten below, matching
// x0-probe-results.md precisely.
//
// `FS_Open`/`FS_Close`/`FS_LoadFile`/`FS_LoadDirectFile`/`FS_LoadGameInfo`/
// `FS_FindLibrary`/`FS_Search`: this module's REAL implementations collide
// with engine/common/filesystem_engine.c's thin wrapper functions of the
// identical name, which normally call through a dynamically-loaded
// fs_api_t vtable (COM_GetProcAddress on a separate filesystem.dll/.so) -
// an architecture that cannot work on Xbox at all regardless of this
// collision, since Xbox has no dynamic library loading (confirmed:
// filesystem_engine.c has no XASH_STATIC_LIBS branch, so even a
// XASH_STATIC_LIBS build still tries to dlopen a separate module today).
// `FI`: this module's real `fs_globals_t FI` collides with
// filesystem_engine.c's `fs_globals_t *FI` (a pointer, not a struct - the
// struct-vs-pointer mismatch x0-probe-results.md already named). `Mem_Alloc`/
// `Mem_Free`: filesystem's own minimal allocator (this module has to be
// self-contained enough to run before the engine's own pools exist)
// collides with engine/common/zone.c's real pool allocator of the same
// name.
//
// Renaming filesystem's side, not engine's: the alternative is renaming
// zone.c's Mem_Alloc/Mem_Free and filesystem_engine.c's six FS_* wrappers
// plus FI - touching engine/, which upstream changes far more often than
// this header, at every one of dozens of call sites across the whole
// engine tree instead of once here. Scoped as a rename, not a file
// exclusion: filesystem's own 83 internal call sites (grepped, 2026-09-07)
// need their own real allocator and their own real FS_Open regardless of
// what engine/ calls them - only the SYMBOL NAME collides, not the
// behaviour, so #define is the whole fix. Untouched by this rename:
// engine code's own calls to FS_Open/Mem_Alloc/etc, which still resolve to
// filesystem_engine.c's wrapper and zone.c's allocator exactly as before -
// this header is never included outside filesystem/ (confirmed by grep),
// so nothing outside this directory can even see these macros.
//
// Whether the wrapper's own dynamic-loading approach is fixable for Xbox
// at all - and whether engine code could call filesystem's real,
// now-renamed functions directly instead, skipping the vtable indirection
// entirely - is a separate, bigger question this rename does not answer.
// It only proves the LINK can succeed with duplicate names out of the way;
// whether FI/g_fsapi ever get populated correctly at Xbox runtime is X2's
// job (nothing calls Host_Main on Xbox yet either way - see
// docs/r3d/divergences.md and CHANGELOG.md's "Still open" for the
// standing note on that).
// _Mem_Alloc/_Mem_Free are NOT in this blanket #define block, unlike the
// eight above: found in review, 2026-09-07 - fs_interface_t (fscallback.h)
// has struct MEMBER fields also literally named _Mem_Alloc/_Mem_Free (the
// engine's own allocator, handed to filesystem at init time so it can
// route allocations through the engine's real pools instead of its own).
// A blind #define rewrites every token match, member accesses included -
// `g_engfuncs._Mem_Alloc`/`engfuncs->_Mem_Alloc` would have been renamed
// to a member name that does not exist on the struct, a real compile
// error caught immediately by trying to build filesystem/dll.c and
// filesystem.c against this exact block, not assumed safe. Renamed by
// hand instead, at the two declarations and two definitions below and
// nowhere else - see FSLib__Mem_Alloc/FSLib__Mem_Free further down.
#if XASH_XBOX
	#define FI              FSLib_FI
	#define FS_Open         FSLib_FS_Open
	#define FS_Close        FSLib_FS_Close
	#define FS_LoadFile     FSLib_FS_LoadFile
	#define FS_LoadDirectFile FSLib_FS_LoadDirectFile
	#define FS_LoadGameInfo FSLib_FS_LoadGameInfo
	#define FS_FindLibrary  FSLib_FS_FindLibrary
	#define FS_Search       FSLib_FS_Search
#endif

#include <stdlib.h>
#include "xash3d_types.h"
#include "filesystem.h"
#include "miniz.h"

#if XASH_ANDROID
#include <android/asset_manager.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct searchpath_s searchpath_t;
typedef struct dir_s dir_t;
typedef struct zip_s zip_t;
typedef struct pack_s pack_t;
typedef struct wfile_s wfile_t;
typedef struct mpak_s mpak_t;
typedef struct android_assets_s android_assets_t;

#define FILE_BUFF_SIZE (2048)
#define FILE_DEFLATED BIT( 0 )

typedef struct ztoolkit_s
{
	z_stream zstream;
	size_t   comp_length;
	size_t   in_ind, in_len;
	size_t   in_position;
	byte     input[FILE_BUFF_SIZE];
} ztoolkit_t;

struct file_s
{
	int          handle;      // file descriptor
	int          ungetc;      // single stored character from ungetc, cleared to EOF when read
	time_t       filetime;    // pak, wad or real filetime
	searchpath_t *searchpath;
	fs_offset_t  real_length; // uncompressed file size (for files opened in "read" mode)
	fs_offset_t  position;    // current position in the file
	fs_offset_t  offset;      // offset into the package (0 if external file)
	uint32_t     flags;
	ztoolkit_t   *ztk; // if not NULL, all read functions must go through decompression

	// contents buffer
	fs_offset_t buff_ind; // buffer current index
	fs_offset_t buff_len; // buffer current length
	byte		buff[FILE_BUFF_SIZE]; // intermediate buffer

#ifdef XASH_REDUCE_FD
	const char *backup_path;
	fs_offset_t backup_position;
	uint backup_options;
#endif
};

typedef enum searchpathtype_e
{
	SEARCHPATH_PLAIN = 0,
	SEARCHPATH_PAK,
	SEARCHPATH_WAD,
	SEARCHPATH_ZIP,
	SEARCHPATH_PK3DIR, // it's actually a plain directory but it must behave like a ZIP archive,
	SEARCHPATH_ANDROID_ASSETS,
	SEARCHPATH_MPAK // one archive = one map's baked geometry/textures/lightmaps; see mpak.c's header comment
} searchpathtype_t;

typedef struct stringlist_s
{
	// maxstrings changes as needed, causing reallocation of strings[] array
	int		maxstrings;
	int		numstrings;
	char		**strings;
} stringlist_t;

typedef struct searchpath_s
{
	string           filename;
	searchpathtype_t type;
	int              flags;

	union
	{
		dir_t   *dir;
		pack_t  *pack;
		wfile_t *wad;
		zip_t   *zip;
		mpak_t  *mpak;
		android_assets_t *assets;
	};

	struct searchpath_s *next;

	void    (*pfnPrintInfo)( struct searchpath_s *search, char *dst, size_t size );
	void    (*pfnClose)( struct searchpath_s *search );
	file_t *(*pfnOpenFile)( struct searchpath_s *search, const char *filename, const char *mode, int pack_ind );
	int     (*pfnFileTime)( struct searchpath_s *search, const char *filename );
	int     (*pfnFindFile)( struct searchpath_s *search, const char *path, char *fixedname, size_t len );
	void    (*pfnSearch)( struct searchpath_s *search, stringlist_t *list, const char *pattern, int caseinsensitive );
	byte   *(*pfnLoadFile)( struct searchpath_s *search, const char *path, int pack_ind, fs_offset_t *filesize, void *( *pfnAlloc )( size_t ), void ( *pfnFree )( void * ));
} searchpath_t;

typedef searchpath_t *(*FS_ADDARCHIVE_FULLPATH)( const char *path, int flags );

extern char fs_rootdir[MAX_SYSPATH], fs_basedir[MAX_SYSPATH], fs_rodir[MAX_SYSPATH];
extern fs_globals_t FI;
extern searchpath_t *fs_writepath, *fs_searchpaths;
extern poolhandle_t fs_mempool;
extern fs_interface_t g_engfuncs;
extern const fs_api_t g_api;

#define GI FI.GameInfo

// FSLIB_MEM_ALLOC/FSLIB_MEM_FREE, not a blind #define _Mem_Alloc/_Mem_Free
// rename like the eight symbols above: struct fs_interface_t (fscallback.h)
// has member fields also literally named _Mem_Alloc/_Mem_Free (the engine's
// own allocator, handed to filesystem at init so it can route through the
// engine's real pools) - a blanket rename would have rewritten
// g_engfuncs._Mem_Alloc/engfuncs->_Mem_Alloc to a member name the struct
// does not have, caught by actually compiling dll.c and filesystem.c
// against that version, not assumed safe. These two indirection macros
// name only the FREE FUNCTION this module defines below (filesystem.c),
// never a struct member, so they are safe to rename this way; every other
// platform keeps the original names, unaffected.
#if XASH_XBOX
	#define FSLIB_MEM_ALLOC FSLib__Mem_Alloc
	#define FSLIB_MEM_FREE  FSLib__Mem_Free
#else
	#define FSLIB_MEM_ALLOC _Mem_Alloc
	#define FSLIB_MEM_FREE  _Mem_Free
#endif

#define Mem_Malloc( pool, size ) FSLIB_MEM_ALLOC( pool, size, false, __FILE__, __LINE__ )
#define Mem_Calloc( pool, size ) FSLIB_MEM_ALLOC( pool, size, true, __FILE__, __LINE__ )
#define Mem_Realloc( pool, ptr, size ) g_engfuncs._Mem_Realloc( pool, ptr, size, true, __FILE__, __LINE__ )
#define Mem_Free( mem ) FSLIB_MEM_FREE( mem, __FILE__, __LINE__ )
#define Mem_AllocPool( name ) g_engfuncs._Mem_AllocPool( name, 0, __FILE__, __LINE__ )
#define Mem_AllocPoolExt( name, flags ) g_engfuncs._Mem_AllocPool( name, flags, __FILE__, __LINE__ )
#define Mem_FreePool( pool ) g_engfuncs._Mem_FreePool( pool, __FILE__, __LINE__ )

#define Con_Printf  (*g_engfuncs._Con_Printf)
#define Con_DPrintf (*g_engfuncs._Con_DPrintf)
#define Con_Reportf (*g_engfuncs._Con_Reportf)
#define Sys_Error   (*g_engfuncs._Sys_Error)
#define Sys_GetNativeObject (*g_engfuncs._Sys_GetNativeObject)

//
// filesystem.c
//
void FS_InitMemory( void );
void FSLIB_MEM_FREE( void *data, const char *filename, int fileline );
void *FSLIB_MEM_ALLOC( poolhandle_t poolptr, size_t size, qboolean clear, const char *filename, int fileline )
	ALLOC_CHECK( 2 ) MALLOC_LIKE( FSLIB_MEM_FREE, 1 ) WARN_UNUSED_RESULT;
void FS_EnsureOpenFile( file_t *file );
void FS_BackupFileName( file_t *file, const char *path, uint options );

//
// searchpath.c
//
searchpath_t *FS_MountArchive_Fullpath( const char *file, int flags );
void FS_AddGameDirectory( const char *dir, uint flags );
void FS_ClearSearchPath( void );
int FS_CheckNastyPath( const char *path );
void FS_AddGameHierarchy( const char *dir, uint flags );
void FS_Rescan( uint32_t flags, const char *language );
const char *FS_Gamedir( void );
void FS_LoadGameInfo( uint32_t flags, const char *language );
qboolean FS_InitStdio( qboolean caseinsensitive, const char *rootdir, const char *basedir, const char *gamedir, const char *rodir );
void FS_AllowDirectPaths( qboolean enable );
void FS_ShutdownStdio( void );
void FS_Path_f( void );
void FS_FindFile_f( const char *filename );
searchpath_t *FS_FindFile( const char *name, int *index, char *fixedname, size_t len, uint32_t flags );
qboolean FS_FindLibrary( const char *dllname, qboolean directpath, fs_dllinfo_t *dllInfo );
qboolean FS_FullPathToRelativePath( char *dst, const char *src, size_t size );
search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly ) MALLOC_LIKE( FSLIB_MEM_FREE, 1 ) WARN_UNUSED_RESULT;
qboolean FS_IsArchiveExtensionSupported( const char *ext, uint flags );
searchpath_t *FS_GetArchiveByName( const char *name, searchpath_t *prev );
int FS_FindFileInArchive( searchpath_t *sp, const char *path, char *truepath, size_t len );
file_t *FS_OpenFileFromArchive( searchpath_t *sp, const char *path, const char *mode, int pack_ind );

//
// gameinfo.c
//
void FS_MakeGameInfo( void );
qboolean FS_ParseGameInfo( const char *gamedir, gameinfo_t *GameInfo, qboolean rodir );

//
// sys.c
//
void stringlistinit( stringlist_t *list );
void stringlistfreecontents( stringlist_t *list );
void stringlistappend( stringlist_t *list, const char *text );
void stringlistsort( stringlist_t *list );
void listdirectory( stringlist_t *list, const char *path, qboolean dirs_only );
void FS_CreatePath( char *path );
int FS_SysFileTime( const char *filename );
file_t *FS_SysOpen( const char *filepath, const char *mode );
file_t *FS_OpenHandle( searchpath_t *search, int handle, fs_offset_t offset, fs_offset_t len );
qboolean FS_SysFileExists( const char *path );
qboolean FS_SysFolderExists( const char *path );
qboolean FS_SysFileOrFolderExists( const char *path );
int FS_SetCurrentDirectory( const char *path );

//
// io.c
//
file_t *FS_OpenReadFile( const char *filename, const char *mode, qboolean gamedironly );
int FS_Close( file_t *file );
file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly ) MALLOC_LIKE( FS_Close, 1 ) WARN_UNUSED_RESULT;
int FS_Flush( file_t *file );
fs_offset_t FS_Write( file_t *file, const void *data, size_t datasize );
fs_offset_t FS_Read( file_t *file, void *buffer, size_t buffersize );
int FS_Print( file_t *file, const char *msg );
int FS_Printf( file_t *file, const char *format, ... ) FORMAT_CHECK( 2 );
int FS_VPrintf( file_t *file, const char *format, va_list ap );
int FS_Getc( file_t *file );
int FS_UnGetc( file_t *file, char c );
int FS_Gets( file_t *file, char *string, size_t bufsize );
int FS_Seek( file_t *file, fs_offset_t offset, int whence );
fs_offset_t FS_Tell( const file_t *file );
qboolean FS_Eof( const file_t *file );
byte *FS_LoadFileFromArchive( searchpath_t *sp, const char *path, int pack_ind, fs_offset_t *filesizeptr, const qboolean sys_malloc );
byte *FS_LoadFileMalloc( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly ) MALLOC_LIKE( free, 1 ) WARN_UNUSED_RESULT;
byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly ) MALLOC_LIKE( FSLIB_MEM_FREE, 1 ) WARN_UNUSED_RESULT;
qboolean CRC32_File( dword *crcvalue, const char *filename );
qboolean MD5_HashFile( byte digest[16], const char *pszFileName, uint seed[4] );
byte *FS_LoadDirectFile( const char *path, fs_offset_t *filesizeptr ) MALLOC_LIKE( FSLIB_MEM_FREE, 1 ) WARN_UNUSED_RESULT;
qboolean FS_WriteFile( const char *filename, const void *data, fs_offset_t len );
int FS_FileExists( const char *filename, int gamedironly );
const char *FS_GetDiskPath( const char *name, qboolean gamedironly );
qboolean FS_GetFullDiskPath( char *buffer, size_t size, const char *name, qboolean gamedironly );
fs_offset_t FS_FileSize( const char *filename, qboolean gamedironly );
fs_offset_t FS_FileLength( const file_t *f );
int FS_FileTime( const char *filename, qboolean gamedironly );
qboolean FS_Rename( const char *oldname, const char *newname );
qboolean FS_Delete( const char *path );
qboolean FS_FileCopy( file_t *pOutput, file_t *pInput, int fileSize );

//
// pak.c
//
qboolean FS_CheckForQuakePak( const char *pakfile, const char *files[], size_t num_files );
searchpath_t *FS_AddPak_Fullpath( const char *pakfile, int flags );

//
// wad.c
//
searchpath_t *FS_AddWad_Fullpath( const char *wadfile, int flags );

//
// zip.c
//
searchpath_t *FS_AddZip_Fullpath( const char *zipfile, int flags );

//
// mpak.c
//
searchpath_t *FS_AddMpak_Fullpath( const char *mpakfile, int flags );

//
// dir.c
//
searchpath_t *FS_AddDir_Fullpath( const char *path, int flags );
qboolean FS_FixFileCase( dir_t *dir, const char *path, char *dst, const size_t len, qboolean createpath );
void FS_InitDirectorySearchpath( searchpath_t *search, const char *path, int flags );

//
// android.c
//
void FS_InitAndroid( void );
searchpath_t *FS_AddAndroidAssets_Fullpath( const char *path, int flags );

#ifdef __cplusplus
}
#endif

#endif // FILESYSTEM_INTERNAL_H
