/*
filesystem.c - game filesystem based on DP fs
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

#if XASH_SDL == 2
#include <SDL.h> // SDL_GetBasePath
#elif XASH_SDL == 3
#include <SDL3/SDL.h>
#endif

#include <errno.h>
#include "common.h"
#include "library.h"
#include "platform/platform.h"

#if XASH_WIN32
#include <direct.h>
#endif

#if XASH_XBOX
#include <windows.h>
#include <nxdk/mount.h>
#include "xbox_missing_dirs.h" // r3d_missing_dirs_flush, divergences.md #164
#endif

static CVAR_DEFINE_AUTO( fs_mount_hd, "0", FCVAR_PRIVILEGED, "mount high definition content folder" );
static CVAR_DEFINE_AUTO( fs_mount_lv, "0", FCVAR_PRIVILEGED, "mount low violence models content folder" );
static CVAR_DEFINE_AUTO( fs_mount_addon, "0", FCVAR_PRIVILEGED, "mount addon content folder" );
static CVAR_DEFINE_AUTO( fs_mount_l10n, "0", FCVAR_PRIVILEGED, "mount localization content folder" );
static CVAR_DEFINE_AUTO( ui_language, "english", FCVAR_PRIVILEGED, "selected game language" );

fs_api_t g_fsapi;
fs_globals_t *FI;

static pfnCreateInterface_t fs_pfnCreateInterface;
static HINSTANCE fs_hInstance;

search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly )
{
	return g_fsapi.Search( pattern, caseinsensitive, gamedironly );
}

int FS_Close( file_t *file )
{
	return g_fsapi.Close( file );
}

file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly )
{
	return g_fsapi.Open( filepath, mode, gamedironly );
}

byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	return g_fsapi.LoadFile( path, filesizeptr, gamedironly );
}

byte *FS_LoadDirectFile( const char *path, fs_offset_t *filesizeptr )
{
	return g_fsapi.LoadDirectFile( path, filesizeptr );
}

static void COM_StripDirectorySlash( char *pname )
{
	size_t len = Q_strlen( pname );
	if( len > 0 && pname[len - 1] == '/' )
		pname[len - 1] = 0;
}

void *FS_GetNativeObject( const char *obj )
{
	if( fs_pfnCreateInterface )
		return fs_pfnCreateInterface( obj, NULL );

	return NULL;
}

static uint32_t FS_MountFlags( void )
{
	uint32_t flags = 0;

	// FIXME: VFS shouldn't care about this, allow engine to mount gamedirs
	if( fs_mount_lv.value ) SetBits( flags, FS_MOUNT_LV );
	if( fs_mount_hd.value ) SetBits( flags, FS_MOUNT_HD );
	if( fs_mount_addon.value ) SetBits( flags, FS_MOUNT_ADDON );
	if( fs_mount_l10n.value ) SetBits( flags, FS_MOUNT_L10N );

	return flags;
}

void FS_Rescan_f( void )
{
	g_fsapi.Rescan( FS_MountFlags(), ui_language.string );
}

static void FS_LoadVFSConfig( const char *gamedir )
{
	string parm;

	if( Host_IsDedicated( ))
		return;

	Cbuf_AddTextf( "exec %s/vfs.cfg\n", gamedir );
	Cbuf_Execute();

	if( Sys_GetParmFromCmdLine( "-language", parm ))
	{
		Cvar_DirectSet( &ui_language, parm );
		Cvar_DirectSet( &fs_mount_l10n, "1" );
	}

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void FS_SaveVFSConfig( void )
{
	const qboolean force_save = !FS_FileExists( "vfs.cfg", true );

	if( !force_save && !FBitSet( fs_mount_hd.flags|fs_mount_lv.flags|fs_mount_l10n.flags|fs_mount_addon.flags|ui_language.flags, FCVAR_CHANGED ))
	{
		Con_Reportf( "%s: no need to save vfs.cfg\n", __func__ );
		return;
	}

	Con_Printf( "%s()\n", __func__ );

	file_t *f = FS_Open( "vfs.cfg.new", "w", true );
	if( !f )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, "vfs.cfg.new" );
		return;
	}

	FS_Printf( f, "%s \"%d\"\n", fs_mount_hd.name, (int)fs_mount_hd.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_lv.name, (int)fs_mount_lv.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_l10n.name, (int)fs_mount_l10n.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_addon.name, (int)fs_mount_addon.value );
	FS_Printf( f, "%s \"%s\"\n", ui_language.name, ui_language.string );

	Host_FinalizeConfig( f, "vfs.cfg" );

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void FS_LoadGameInfo( void )
{
	FS_LoadVFSConfig( g_fsapi.Gamedir( ));

	g_fsapi.LoadGameInfo( FS_MountFlags(), ui_language.string );
}

static void FS_ClearPaths_f( void )
{
	FS_ClearSearchPath();
}

static void FS_Path_f_( void )
{
	FS_Path_f();
}

static void FS_FindFile_f_( void )
{
	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "fs_find <filepath>\n" );
		return;
	}
	g_fsapi.FindFile_f( Cmd_Argv( 1 ));
}

static void FS_MakeGameInfo_f( void )
{
	g_fsapi.MakeGameInfo();
}

static const fs_interface_t fs_memfuncs =
{
	Con_Printf,
	Con_DPrintf,
	Con_Reportf,
	Sys_Error,

	_Mem_AllocPool,
	_Mem_FreePool,
	_Mem_Alloc,
	_Mem_Realloc,
	_Mem_Free,

	Sys_GetNativeObject,
};

static void FS_UnloadProgs( void )
{
	if( fs_hInstance )
	{
		COM_FreeLibrary( fs_hInstance );
		fs_hInstance = 0;
	}
}

#ifdef XASH_INTERNAL_GAMELIBS
#define FILESYSTEM_STDIO_DLL "filesystem_stdio"
#elif XASH_ANDROID
#define FILESYSTEM_STDIO_DLL "libfilesystem_stdio.so"
#else
#define FILESYSTEM_STDIO_DLL "filesystem_stdio." OS_LIB_EXT
#endif

static qboolean FS_LoadProgs( void )
{
	const char *name = FILESYSTEM_STDIO_DLL;

	fs_hInstance = COM_LoadLibrary( name, false, true );

	if( !fs_hInstance )
	{
		Sys_Error( "%s: can't load filesystem library %s: %s\n", __func__, name, COM_GetLibraryError() );
		return false;
	}

	FSAPI GetFSAPI;
	if( !( GetFSAPI = (FSAPI)COM_GetProcAddress( fs_hInstance, GET_FS_API )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find GetFSAPI entry point in %s\n", __func__, name );
		return false;
	}

	if( GetFSAPI( FS_API_VERSION, &g_fsapi, &FI, &fs_memfuncs ) != FS_API_VERSION )
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't initialize filesystem API: wrong version\n", __func__ );
		return false;
	}

	if( !( fs_pfnCreateInterface = (pfnCreateInterface_t)COM_GetProcAddress( fs_hInstance, "CreateInterface" )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find CreateInterface entry point in %s\n", __func__, name );
		return false;
	}

	Con_DPrintf( "%s: filesystem_stdio successfully loaded\n", __func__ );
	return true;
}

static qboolean FS_DetermineRootDirectory( char *out, size_t size )
{
	const char *path = getenv( "XASH3D_BASEDIR" );

	if( !COM_StringEmptyOrNULL( path ))
	{
		Q_strncpy( out, path, size );
		return true;
	}

#if XASH_XBOX
	// Found in review, X2 W5 - Codex, round 1: getenv() above always
	// returns NULL here (nxdk's pdclib getenv is a real, unconditional
	// stub, deps/nxdk/lib/pdclib/platform/xbox/functions/stdlib/getenv.c),
	// SDL_GetBasePath returns NULL (SDL_FILESYSTEM_DUMMY,
	// SDL_config_xbox.h), and compat/xbox's getcwd is an honest ENOSYS
	// stub (xbox_posix_io.c - "the Xbox has no per-process current
	// directory"), so every case below Sys_Errors before FS_Init ever
	// reaches content. The docs/CHANGELOG claim that getcwd already
	// resolved this (X0.5) was checked by seeing the function existed, not
	// by reading its body - wrong, corrected here and in those docs.
	//
	// D:\ (X0.5's own fix - tools/xbox-dedicated-link-check.sh links
	// `-include:_automount_d_drive`, deps/nxdk/lib/nxdk/automount_d.c, a
	// CRT-init-array hook that mounts D: to the directory containing the
	// running XBE before main() runs) solved "can the engine find its
	// assets at all" - X0.5's own real exit line. It never solved "can it
	// WRITE anything": D:\ is the disc (an XISO), permanently read-only,
	// and this function's return value becomes fs_rootdir - upstream's
	// own name for the WRITABLE root, split from fs_rodir (a separate,
	// optional read-only overlay, FS_DetermineReadOnlyRootDirectory
	// below) precisely for platforms like this one. Returning D:\ here
	// left fs_rootdir pointing at read-only media, which stayed invisible
	// through X7.4 step 2 - nothing tries to write until the first real
	// level transition's SaveGameState() (specialist review; see
	// docs/r3d/fork-plan.md's X7.4 step 3).
	//
	// The real fix: mount E:, the real Xbox convention for per-title user
	// data (confirmed against hal/fileio.c's own partition table -
	// getPartitionIndex maps 'E' to \Device\Harddisk0\Partition1\, the
	// same real hardware layout every retail title's own E:\UDATA\<title
	// ID>\ save path already uses), then use that real, hardware-standard
	// layout - EXCEPT the title ID itself. Codex review, round 1, caught
	// a real gap: cxbe (deps/nxdk/tools/cxbe/Xbe.cpp:275) hardcodes
	// dwTitleId = 0xFFFF0002 into every certificate this project's own
	// tools generate (its own comment: "TODO: generate in the form
	// CX-9999" - a real, separate, not-yet-done nxdk tooling gap, not
	// invented here), so reading it back gives every title THIS fork
	// ever builds the exact same value, not a real per-title namespace -
	// confirmed live below, not assumed. Namespaced under it instead,
	// with the one thing genuinely known at this point in the project's
	// real timeline: XASH_GAMEDIR, the exact same compile-time per-title
	// name launcher.c already relies on for the identical "which title
	// is this build" purpose (defaulted the same way, "valve", since
	// X7.6's own real per-title `-D` build wiring does not exist yet
	// either - see docs/r3d/fork-plan.md's X7.6). Whoever wires that up
	// needs to pass it to this translation unit too, not just
	// launcher.c's; a real, unique certificate TitleID per title
	// (cxbe's own real fix) would let this drop the extra directory
	// level entirely and is worth doing at that same point, not before -
	// no title but "valve" exists to test isolation against today.
	// CreateDirectoryA has no FILE_OPEN_IF-style "create if missing, else
	// succeed" mode (winapi/filemanip.c:274-299 always uses FILE_CREATE),
	// so every path level is created unconditionally and
	// GetLastError()'s ERROR_ALREADY_EXISTS is the one failure this
	// genuinely expects and ignores - a real, empty HDD partition on a
	// first boot needs UDATA\ and the title-ID level created too, not
	// just the per-game leaf.
#ifndef XASH_GAMEDIR
#define XASH_GAMEDIR "valve" // !!! Same default as engine/common/launcher.c - keep both in sync !!!
#endif
	{
		string xbox_rootdir;
		DWORD title_id;

		if( !nxIsDriveMounted( 'E' ) && !nxMountDrive( 'E', "\\Device\\Harddisk0\\Partition1\\" ))
		{
			Sys_Error( "couldn't mount E: (Partition1, the real Xbox per-title user-data drive) - error %lu", GetLastError( ));
			return false;
		}

		title_id = CURRENT_XBE_HEADER->CertificateHeader->TitleID;
		Q_snprintf( xbox_rootdir, sizeof( xbox_rootdir ), "E:\\UDATA\\%08X\\" XASH_GAMEDIR, title_id );

		if( !CreateDirectoryA( "E:\\UDATA", NULL ) && GetLastError() != ERROR_ALREADY_EXISTS )
		{
			Sys_Error( "couldn't create E:\\UDATA - error %lu", GetLastError( ));
			return false;
		}
		{
			string title_dir;
			Q_snprintf( title_dir, sizeof( title_dir ), "E:\\UDATA\\%08X", title_id );
			if( !CreateDirectoryA( title_dir, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS )
			{
				Sys_Error( "couldn't create %s - error %lu", title_dir, GetLastError( ));
				return false;
			}
		}
		if( !CreateDirectoryA( xbox_rootdir, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS )
		{
			Sys_Error( "couldn't create %s - error %lu", xbox_rootdir, GetLastError( ));
			return false;
		}
		// divergences.md #164: these directories may have been looked
		// under before they existed; the file layer forgets what it saw
		r3d_missing_dirs_flush();

		Q_strncpy( out, xbox_rootdir, size );
		return true;
	}
#elif XASH_IOS
	Q_strncpy( out, IOS_GetDocsDir(), size );
	return true;
#elif XASH_PSVITA
	if( PSVita_GetBasePath( out, size ))
		return true;
	Sys_Error( "couldn't find %s data directory", XASH_ENGINE_NAME );
	return false;
#elif ( XASH_SDL >= 2 ) && !XASH_NSWITCH // GetBasePath not impl'd in switch-sdl2
	path = SDL_GetBasePath();

#if XASH_APPLE
	if( path != NULL && Q_stristr( path, ".app" ))
	{
		SDL_free((void *)path );
		path = SDL_GetPrefPath( NULL, XASH_ENGINE_NAME );
	}
#endif

	if( path != NULL )
	{
		Q_strncpy( out, path, size );
		SDL_free((void *)path );
		return true;
	}

#if XASH_POSIX || XASH_WIN32
	if( getcwd( out, size ))
		return true;
	Sys_Error( "couldn't determine current directory: %s, getcwd: %s", SDL_GetError(), strerror( errno ));
#else // !( XASH_POSIX || XASH_WIN32 )
	Sys_Error( "couldn't determine current directory: %s", SDL_GetError( ));
#endif // !( XASH_POSIX || XASH_WIN32 )
	return false;
#else // generic case
	if( getcwd( out, size ))
		return true;

	Sys_Error( "couldn't determine current directory: %s", strerror( errno ));
	return false;
#endif // generic case
}

static qboolean FS_DetermineReadOnlyRootDirectory( char *out, size_t size )
{
	const char *env_rodir = getenv( "XASH3D_RODIR" );

	if( _Sys_GetParmFromCmdLine( "-rodir", out, size ))
		return true;

	if( !COM_StringEmptyOrNULL( env_rodir ))
	{
		Q_strncpy( out, env_rodir, size );
		return true;
	}

#if XASH_IOS
	Q_strncpy( out, IOS_GetExecDir(), size );
	return true;
#elif XASH_XBOX
	// -rodir can never arrive via argv here (matching this function's own
	// -rodir/env checks above, both real but unreachable on this
	// platform): nxdk's own crt0 hardcodes argc=0 into main() (deps/nxdk/
	// lib/pdclib/platform/xbox/crt0.c's main_wrapper: `main(0, &_argv)`),
	// and getenv() is a real, unconditional NULL stub (see
	// FS_DetermineRootDirectory's own comment on both). D:\ - the disc,
	// guaranteed mounted by X0.5's own automount hook - is exactly what
	// upstream's fs_rodir contract wants here: a separate, read-only
	// overlay of real game content, now that fs_rootdir above points at
	// the real writable E:\UDATA\<title ID>\ instead.
	Q_strncpy( out, "D:\\", size );
	return true;
#endif

	return false;
}

/*
================
FS_Init
================
*/
void FS_Init( void )
{
	string gamedir;
	char rodir[MAX_OSPATH], rootdir[MAX_OSPATH];
	rodir[0] = rootdir[0] = 0;

	if( !FS_DetermineRootDirectory( rootdir, sizeof( rootdir )) || COM_StringEmpty( rootdir ))
	{
		Sys_Error( "couldn't determine current directory (empty string)" );
		return;
	}
	COM_FixSlashes( rootdir );
	COM_StripDirectorySlash( rootdir );

	FS_DetermineReadOnlyRootDirectory( rodir, sizeof( rodir ));
	COM_FixSlashes( rodir );
	COM_StripDirectorySlash( rodir );

	if( !Sys_GetParmFromCmdLine( "-game", gamedir ))
	{
		char *env = getenv( "XASH3D_GAME" );
		if( env )
			Q_strncpy( gamedir, env, sizeof( gamedir ));
		else
			Q_strncpy( gamedir, host.default_gamedir, sizeof( gamedir )); // gamedir == basedir
	}

	FS_LoadProgs();

	// TODO: this function will cause engine to stop in case of fail
	// when it will have an option to return string error, restore Sys_Error
	// FIXME: why do we call this function before InitStdio?
	// because InitStdio immediately scans all available game directories
	// and this better be reworked at some point
	g_fsapi.SetCurrentDirectory( rootdir );

	if( !g_fsapi.InitStdio( true, rootdir, host.default_gamedir, gamedir, rodir ))
	{
		Sys_Error( "Can't init filesystem_stdio!\n" );
		return;
	}

	Cmd_AddRestrictedCommand( "fs_rescan", FS_Rescan_f, "rescan filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_path", FS_Path_f_, "show filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_find", FS_FindFile_f_, "find file across search pathes and show all occurences" );
	Cmd_AddRestrictedCommand( "fs_clearpaths", FS_ClearPaths_f, "clear filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_make_gameinfo", FS_MakeGameInfo_f, "create gameinfo.txt for current running game" );

	Cvar_RegisterVariable( &fs_mount_hd );
	Cvar_RegisterVariable( &fs_mount_lv );
	Cvar_RegisterVariable( &fs_mount_addon );
	Cvar_RegisterVariable( &fs_mount_l10n );
	Cvar_RegisterVariable( &ui_language );

	if( !Sys_GetParmFromCmdLine( "-dll", host.gamedll ))
		host.gamedll[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-clientlib", host.clientlib ))
		host.clientlib[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-menulib", host.menulib ))
		host.menulib[0] = 0;
}

/*
================
FS_Shutdown
================
*/
void FS_Shutdown( void )
{
	if( g_fsapi.ShutdownStdio )
		g_fsapi.ShutdownStdio();

	FS_UnloadProgs();
}
