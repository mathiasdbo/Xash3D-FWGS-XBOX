/*
xbox-e-wipe - one-shot fix for docs/r3d/divergences.md #88/#89/#90/#91's
own recurring live-verification blocker.

`filesystem/searchpath.c`'s `FS_Init` searches `fs_rootdir` (E:, writable)
before `fs_rodir` (D:, the disc) for every file lookup - a real, correctly-
designed property of this fork's own filesystem (built for save-file
support), not a bug. But `engine/common/filesystem_engine.c`'s own
`FS_DetermineRootDirectory` derives `fs_rootdir` as
`E:\UDATA\<TitleID>\valve`, and every XBE this fork's own packaging tools
(`cxbe`) produce carries the exact same hardcoded certificate TitleID,
0xFFFF0002 (`deps/nxdk/tools/cxbe/Xbe.cpp`'s own `dwTitleId = 0xFFFF0002`,
its own "TODO: generate in the form CX-9999" - a real, separate, not-yet-
fixed nxdk tooling gap, not something to fix here) - regardless of which
git branch, renderer, or test map produced an earlier boot on the same
shared xemu test HDD, they all write into this exact same directory. A
`video.cfg` left there by any earlier boot permanently shadows a fresh
disc's own copy, silently keeping `r_refdll` off whatever the disc's own
config asked for - the confirmed root cause of every one of X9-A through
X9-D's own still-unverified live-rendering claims.

(codex review, round 2, real finding): the diagnosed blocker is
specifically `video.cfg` - this tool does NOT recursively wipe the whole
`E:\UDATA\FFFF0002\valve\` tree (an earlier version did, and round 1's
own review already narrowed that FROM the whole `FFFF0002` TitleID
directory TO this one gamedir leaf, but even the leaf still holds real,
legitimate Resonance3D user data this tool has no business touching:
`config.cfg` (archived cvars/bindings, `engine/common/con_utils.c`'s own
`Host_WriteConfig`) and per-save `.sav` files (real game saves,
`engine/server/sv_save.c`)). Deleting exactly the two real, known
filenames `Host_FinalizeConfig`'s own atomic-write sequence
(`engine/common/con_utils.c:1372-1384`) can ever leave behind for
`video.cfg` specifically - the final file itself, and `video.cfg.new`
(the staging file a write leaves behind if a boot is interrupted mid-
write, before the rename that makes it live) - fixes the diagnosed
problem with nothing broader at risk. `video.cfg.bak` (the OTHER file
that same sequence produces) is deliberately left alone: it is never
read by anything, only ever written, so a stale copy cannot shadow
anything and deleting it would only lose a backup for no benefit.

No real, licensed Xbox title (a real retail Half-Life disc on the same
shared HDD included) can ever carry TitleID 0xFFFF0002 - only a real,
Microsoft-issued certificate gets a real title its own unique ID. But
0xFFFF0002 is NOT provably exclusive to THIS fork either: it is nxdk's
own stock, unmodified `cxbe` converter's hardcoded default
(`deps/nxdk/tools/cxbe/Xbe.cpp`), so any OTHER nxdk homebrew project
built with that same standard tool - one of `deps/nxdk/samples` run by
hand, for instance - would carry the exact same TitleID and could in
principle have its own real state under the same `E:\UDATA\FFFF0002\`
directory; a coincidental second nxdk project ALSO choosing to name its
own save directory "valve" is vanishingly unlikely, but the two named
files this tool deletes are ALSO exactly the two names this fork's own
build can ever produce there, so there is no broader exposure either
way. Every path this tool touches is a fixed string literal, never
built from a variable or an argument, so there is no path-construction
bug that could ever point it anywhere else.

Verification-only tool, not shipped with the game and not part of any
engine build - a standalone XBE in the same spirit as tools/x4-triangle,
run once under xemu before a live X9 verification attempt. Progress and
outcome are all exposed via a handful of plain globals meant to be read
with `tools/xemu-debug.py mem` - this project's own debug tooling has
found `screenshot`/`mem` the only reliable commands in this environment,
so this tool exposes everything a text/graphical readout would need
through globals a linker map can locate instead.
*/
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <windows.h>

#define WIPE_DIR         "E:\\UDATA\\FFFF0002\\valve"
#define WIPE_VIDEO_CFG   WIPE_DIR "\\video.cfg"
#define WIPE_VIDEO_CFG_NEW WIPE_DIR "\\video.cfg.new"

enum
{
	WIPE_STARTED       = 1,
	WIPE_MOUNT_FAILED  = 2,
	WIPE_MOUNTED       = 3,
	WIPE_DELETE_FAILED = 5,
	WIPE_DONE          = 6,
};

volatile unsigned int g_wipe_status        = 0;
volatile unsigned int g_wipe_files_deleted = 0;
volatile unsigned int g_wipe_last_error    = 0;

/* Deletes exactly one named file, tolerating "it doesn't exist" (nothing
   to do) but recording and reporting any other failure - a real I/O or
   access error must never be indistinguishable from "already clean".
   Returns FALSE only on a real, unexpected failure. */
static BOOL DeleteIfPresent( const char *path )
{
	DWORD err;

	if( DeleteFileA( path ))
	{
		g_wipe_files_deleted++;
		return TRUE;
	}

	err = GetLastError();
	if( err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND )
		return TRUE; /* already clean - not a failure */

	g_wipe_last_error = err;
	return FALSE;
}

int main( void )
{
	XVideoSetMode( 640, 480, 32, REFRESH_DEFAULT );

	g_wipe_status = WIPE_STARTED;

	if( !nxIsDriveMounted( 'E' ) && !nxMountDrive( 'E', "\\Device\\Harddisk0\\Partition1\\" ))
	{
		g_wipe_last_error = GetLastError();
		g_wipe_status = WIPE_MOUNT_FAILED;
		debugPrint( "e-wipe: couldn't mount E: (error %u)\n", g_wipe_last_error );
		while( 1 ) Sleep( 2000 );
	}
	g_wipe_status = WIPE_MOUNTED;

	if( !DeleteIfPresent( WIPE_VIDEO_CFG ) || !DeleteIfPresent( WIPE_VIDEO_CFG_NEW ))
	{
		g_wipe_status = WIPE_DELETE_FAILED;
		debugPrint( "e-wipe: delete failed, error %u (files deleted so far=%u)\n",
			g_wipe_last_error, g_wipe_files_deleted );
		while( 1 ) Sleep( 2000 );
	}

	g_wipe_status = WIPE_DONE;
	debugPrint( "e-wipe: done - deleted %u file(s)\n", g_wipe_files_deleted );
	while( 1 ) Sleep( 2000 );

	return 0;
}
