/*
xbox_posix_io.c - POSIX file I/O over Win32, for nxdk

Part of compat/xbox; see that directory's README for the measurements that
motivate it. nxdk provides no low-level POSIX file I/O, and filesystem/ is
built on raw file descriptors, so this bridges the two.

Everything here sits on the Win32 subset nxdk does implement. Nothing in this
file is clever; where a POSIX concept has no Xbox counterpart the function
fails honestly rather than pretending.

Not thread-safe. The descriptor table is a plain array with no lock. The
engine does use SDL threads (engine/common/net_ws.c:299-307 maps its mutex and
thread primitives onto SDL when XASH_SDL == 2), but filesystem/ is called from
the main thread. If that stops being true, this table needs a mutex before
anything else here changes.
*/

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>   /* SEEK_SET / SEEK_CUR / SEEK_END */
#include <stdint.h>  /* intptr_t, for _get_osfhandle */

/* Console, so a small table is plenty. filesystem/ keeps few files open at a
 * time, and XASH_REDUCE_FD exists upstream precisely for platforms that cannot
 * afford many. 0-2 are reserved so a descriptor is never confused with a
 * standard stream. */
#define R3D_MAX_FDS   64
#define R3D_FIRST_FD  3

static HANDLE r3d_fds[R3D_MAX_FDS];

/* A borrowed slot holds a handle this layer did not open and must not close.
 * Only fileno() creates one: pdclib owns that handle and closes it in fclose,
 * so calling CloseHandle here as well would close a live file out from under
 * the FILE* - and the descriptor is then reused, so the second close would hit
 * an unrelated object.
 *
 * r3d_borrowed_file records WHICH stream a borrowed slot came from, and is the
 * other half of fileno's lookup key. Keying on the handle alone would be a
 * cache over a value the kernel recycles: pdclib closes the handle in fclose
 * without telling us, the slot survives with a dangling value, and the next
 * CreateFileA can be handed that exact number - at which point a fileno() for
 * an unrelated stream would match the stale slot. Requiring the FILE* to match
 * too closes that: the slot is returned only when it still describes this
 * stream's current handle, which is the invariant write() actually needs. */
static char   r3d_borrowed[R3D_MAX_FDS];
static FILE  *r3d_borrowed_file[R3D_MAX_FDS];

#define FD_VALID( fd ) \
	(( fd ) >= R3D_FIRST_FD && ( fd ) < R3D_MAX_FDS && r3d_fds[( fd )] != NULL )

/*
====================
r3d_cwd

X0.5 (docs/r3d/fork-plan.md), route 1 of the two the plan named: an
emulated process-wide current directory, not a real one - nxdk has no
SetCurrentDirectoryA and every path API resolves against
ObDosDevicesDirectory() with no CWD in the stack at all
(FS_AddGameHierarchy's "./" and "valve/" would otherwise NtOpenFile a DOS
device named "." or "valve", which do not exist - STATUS_OBJECT_NAME_
NOT_FOUND). The doc's own argument against this route was about chdir()
AS AN API pretending to succeed at something real; this is different -
on a single-process console nothing can observe an emulated CWD versus
a real one, so it is not a lie, it is the only implementation a real CWD
*could* have here.

Defaults to "D:\" - not a guess: tools/xbox-dedicated-link-check.sh
already links `-include:_automount_d_drive`, a CRT-init-array hook that
mounts D: to the running XBE's own directory before main() runs, and
engine/common/filesystem_engine.c's own XASH_XBOX branch (X2 W5,
divergence #29) already hands FS_SetCurrentDirectory this exact value as
its first real call - the default here is that same fact stated once
more, defensively, for any path resolved before that call ever happens.
====================
*/
static char r3d_cwd[MAX_PATH] = "D:\\";

/*
====================
r3d_path_is_absolute

The only absolute-path shape this platform has: a drive letter and a
colon (FATX has no leading-slash-is-root convention the way POSIX does -
there is no root without a drive). Every genuinely absolute path this
codebase hands compat/xbox is built exactly this way (host.default_
gamedir, fs_rootdir, "D:\" itself); everything else - GoldSrc's own
"./", "valve/", "valve/maps/c1a3.bsp" - is relative and needs r3d_cwd.
====================
*/
static int r3d_path_is_absolute( const char *path )
{
	return path && path[0] && path[1] == ':';
}

/*
====================
r3d_native_path

GoldSrc paths are '/'-separated; FATX wants '\'. Converts into a
caller-supplied buffer and returns it, or NULL if the result does not
fit. An absolute path (r3d_path_is_absolute) converts unchanged, exactly
as before this function grew CWD-awareness; a relative one resolves
against r3d_cwd first, with one '\' joining the two - not simple
concatenation, since neither r3d_cwd nor the incoming path can be
assumed to already carry the separator at their shared edge.

Still deliberately does no OTHER normalisation: no "..", no case
folding, no collapsing a doubled separator at the join. Callers pass
paths that filesystem/ has already assembled, and inventing a second
normalisation layer here would make divergences between the two very
hard to find - X0.5's own resolution adds exactly one new rule (relative
resolves against r3d_cwd), not a general path-canonicalisation pass.
====================
*/
static const char *r3d_native_path( const char *path, char *out, size_t out_size )
{
	size_t i = 0, j;

	if( !path || !out || out_size == 0 )
		return NULL;

	if( !r3d_path_is_absolute( path ))
	{
		for( ; i < out_size - 1 && r3d_cwd[i]; i++ )
			out[i] = r3d_cwd[i];

		if( r3d_cwd[i] )   /* cwd alone did not fit */
			return NULL;

		if( i > 0 && out[i - 1] != '\\' && path[0] && path[0] != '/' )
		{
			if( i >= out_size - 1 )
				return NULL;
			out[i++] = '\\';
		}
	}

	for( j = 0; i < out_size - 1 && path[j]; i++, j++ )
		out[i] = ( path[j] == '/' ) ? '\\' : path[j];

	if( path[j] )   /* did not fit */
		return NULL;

	out[i] = '\0';
	return out;
}

/*
====================
r3d_errno_from_win32

Callers test errno, so collapsing every failure to one value defeats them.
FS_SysOpen in particular suppresses its diagnostic when errno is ENOENT, so
mapping a sharing violation or a full volume to ENOENT made every open failure
on this platform silent - which is how the first version of this file behaved.
====================
*/
static int r3d_errno_from_win32( DWORD err )
{
	switch( err )
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:      return ENOENT;
	case ERROR_ACCESS_DENIED:      return EACCES;
	case ERROR_SHARING_VIOLATION:  return EBUSY;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:     return EEXIST;
	case ERROR_DISK_FULL:          return ENOSPC;
	case ERROR_WRITE_PROTECT:      return EROFS;
	case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
	case ERROR_INVALID_NAME:
	case ERROR_INVALID_PARAMETER:  return EINVAL;
	case ERROR_DIR_NOT_EMPTY:      return ENOTEMPTY;
	default:                       return EIO;
	}
}

/*
====================
r3d_take_fd / r3d_alloc_fd

Two variants, differing only in who owns the handle when the table is full.
====================
*/
static int r3d_take_fd( HANDLE h )
{
	int fd;

	for( fd = R3D_FIRST_FD; fd < R3D_MAX_FDS; fd++ )
	{
		if( r3d_fds[fd] == NULL )
		{
			r3d_fds[fd] = h;
			return fd;
		}
	}

	errno = EMFILE;
	return -1;
}

/* Owning variant: on exhaustion the handle has no other owner, so it is closed
 * rather than leaked. fileno() must NOT use this - the handle it registers
 * belongs to a live FILE. */
static int r3d_alloc_fd( HANDLE h )
{
	int fd = r3d_take_fd( h );

	if( fd < 0 )
		CloseHandle( h );

	return fd;
}

/*
====================
r3d_filetime_to_time_t

FILETIME counts 100ns intervals from 1601-01-01; time_t counts seconds from
1970-01-01. 11644473600 is the gap in seconds.
====================
*/
static time_t r3d_filetime_to_time_t( const FILETIME *ft )
{
	unsigned long long t;

	t   = (( unsigned long long )ft->dwHighDateTime << 32 ) | ft->dwLowDateTime;
	t  /= 10000000ULL;          /* to seconds */

	if( t < 11644473600ULL )
		return 0;

	return ( time_t )( t - 11644473600ULL );
}

/*
====================
open
====================
*/
int open( const char *path, int flags, ... )
{
	char   native[MAX_PATH];
	DWORD  access, creation;
	HANDLE h;

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	switch( flags & O_ACCMODE )
	{
	case O_WRONLY: access = GENERIC_WRITE; break;
	case O_RDWR:   access = GENERIC_READ | GENERIC_WRITE; break;
	default:       access = GENERIC_READ; break;
	}

	if( flags & O_CREAT )
	{
		if( flags & O_EXCL )        creation = CREATE_NEW;
		else if( flags & O_TRUNC )  creation = CREATE_ALWAYS;
		else                        creation = OPEN_ALWAYS;
	}
	else
	{
		creation = ( flags & O_TRUNC ) ? TRUNCATE_EXISTING : OPEN_EXISTING;
	}

	h = CreateFileA( native, access, FILE_SHARE_READ, NULL, creation,
		FILE_ATTRIBUTE_NORMAL, NULL );

	if( h == INVALID_HANDLE_VALUE )
	{
		errno = r3d_errno_from_win32( GetLastError());
		return -1;
	}

	if( flags & O_APPEND )
		SetFilePointer( h, 0, NULL, FILE_END );

	return r3d_alloc_fd( h );
}

/*
====================
read
====================
*/
ssize_t read( int fd, void *buf, size_t count )
{
	DWORD got;

	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	if( !ReadFile( r3d_fds[fd], buf, ( DWORD )count, &got, NULL ))
	{
		errno = EIO;
		return -1;
	}

	return ( ssize_t )got;
}

/*
====================
write
====================
*/
ssize_t write( int fd, const void *buf, size_t count )
{
	DWORD put;

	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	if( !WriteFile( r3d_fds[fd], buf, ( DWORD )count, &put, NULL ))
	{
		errno = EIO;
		return -1;
	}

	return ( ssize_t )put;
}

/*
====================
lseek
====================
*/
off_t lseek( int fd, off_t offset, int whence )
{
	DWORD method, pos;

	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	switch( whence )
	{
	case SEEK_SET: method = FILE_BEGIN; break;
	case SEEK_CUR: method = FILE_CURRENT; break;
	case SEEK_END: method = FILE_END; break;
	default:       errno = EINVAL; return -1;
	}

	pos = SetFilePointer( r3d_fds[fd], ( LONG )offset, NULL, method );

	if( pos == INVALID_SET_FILE_POINTER )
	{
		errno = EINVAL;
		return -1;
	}

	return ( off_t )pos;
}

/*
====================
close
====================
*/
int close( int fd )
{
	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	/* A borrowed handle belongs to pdclib's FILE. Release the slot, leave the
	 * handle alone; fclose will close it. */
	if( !r3d_borrowed[fd] )
		CloseHandle( r3d_fds[fd] );

	r3d_borrowed[fd] = 0;
	r3d_borrowed_file[fd] = NULL;
	r3d_fds[fd] = NULL;
	return 0;
}

/*
====================
fsync

FlushFileBuffers is not exposed by nxdk's winapi, so this goes straight to the
kernel call it would have wrapped.
====================
*/
int fsync( int fd )
{
	IO_STATUS_BLOCK iosb;

	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	if( !NT_SUCCESS( NtFlushBuffersFile( r3d_fds[fd], &iosb )))
	{
		errno = EIO;
		return -1;
	}

	return 0;
}

int _commit( int fd )
{
	return fsync( fd );
}

/*
====================
ftruncate
====================
*/
int ftruncate( int fd, off_t length )
{
	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	if( SetFilePointer( r3d_fds[fd], ( LONG )length, NULL, FILE_BEGIN )
		== INVALID_SET_FILE_POINTER )
	{
		errno = EINVAL;
		return -1;
	}

	return SetEndOfFile( r3d_fds[fd] ) ? 0 : ( errno = EIO, -1 );
}

/*
====================
stat
====================
*/
int stat( const char *path, struct stat *st )
{
	char   native[MAX_PATH];
	size_t len;
	WIN32_FILE_ATTRIBUTE_DATA fad;

	if( !st || !r3d_native_path( path, native, sizeof( native )))
	{
		errno = EINVAL;
		return -1;
	}

	/* POSIX stat("dir/") succeeds, and filesystem/ relies on it: every
	 * searchpath FS_AddGameHierarchy builds carries a trailing separator
	 * (searchpath.c:364-409), and COM_PathSlashFix guarantees one. Strip it,
	 * but never past a drive root ("D:\") which must keep its separator. */
	len = strlen( native );

	while( len > 1 && native[len - 1] == '\\' && native[len - 2] != ':' )
		native[--len] = '\0';

	/* GetFileAttributesExA answers all of it in ONE kernel call:
	 * NtQueryFullAttributesFile returns attributes, size and all three
	 * timestamps together (deps/nxdk/lib/winapi/filemanip.c:37-68).
	 *
	 * This function used to open the file to get size and mtime -
	 * GetFileAttributesA, then CreateFileA + GetFileInformationByHandle +
	 * CloseHandle, five kernel transitions where one does. That is the hot path
	 * and not by a little: FS_FindFile_DIR (filesystem/dir.c:396) probes every
	 * plain searchpath on every file lookup, and loading a map does that once
	 * per searchpath per referenced file, on a 733 MHz CPU behind an IDE bus.
	 * The old comment right above rejected FindFirstFileA for costing "an
	 * NtOpenFile on the parent plus a directory enumeration per probe" and then
	 * did something more expensive than what it rejected.
	 *
	 * Not FindFirstFileA, still: it treats the last path component as a
	 * wildcard PATTERN rather than a name, and nxdk's handling of that is its
	 * own hazard (deps/nxdk/lib/winapi/findfile.c:51-72). */
	if( !GetFileAttributesExA( native, GetFileExInfoStandard, &fad ))
	{
		errno = r3d_errno_from_win32( GetLastError());
		return -1;
	}

	memset( st, 0, sizeof( *st ));
	st->st_nlink = 1;

	if( fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
	{
		/* Callers only test S_ISDIR on this path (FS_SysFolderExists,
		 * sys.c:294-306); a size for a directory would be meaningless. */
		st->st_mode = S_IFDIR | S_IRWXU;
		return 0;
	}

	st->st_mode  = S_IFREG | S_IRUSR | S_IWUSR;
	st->st_size  = ( off_t )fad.nFileSizeLow;
	st->st_mtime = r3d_filetime_to_time_t( &fad.ftLastWriteTime );
	st->st_atime = st->st_mtime;
	st->st_ctime = st->st_mtime;

	return 0;
}

/*
====================
fstat
====================
*/
int fstat( int fd, struct stat *st )
{
	BY_HANDLE_FILE_INFORMATION info;

	if( !FD_VALID( fd ) || !st ) { errno = EBADF; return -1; }

	if( !GetFileInformationByHandle( r3d_fds[fd], &info ))
	{
		errno = EIO;
		return -1;
	}

	memset( st, 0, sizeof( *st ));
	st->st_mode  = ( info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		? ( S_IFDIR | S_IRWXU ) : ( S_IFREG | S_IRUSR | S_IWUSR );
	st->st_size  = ( off_t )info.nFileSizeLow;
	st->st_mtime = r3d_filetime_to_time_t( &info.ftLastWriteTime );
	st->st_atime = st->st_mtime;
	st->st_ctime = st->st_mtime;
	st->st_nlink = 1;

	return 0;
}

/*
====================
mkdir
====================
*/
int mkdir( const char *path, mode_t mode )
{
	char native[MAX_PATH];

	( void )mode;   /* FATX has no permissions */

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	if( !CreateDirectoryA( native, NULL ))
	{
		errno = r3d_errno_from_win32( GetLastError());
		return -1;
	}

	return 0;
}

/*
====================
remove / rename

filesystem/io.c's FS_Delete and FS_Rename call these, and pdclib's versions
hand the path straight to DeleteFileA and MoveFileA with no '/' to '\'
conversion - so they would silently fail on every path the engine builds.
Defined here so ours are linked instead.
====================
*/
int remove( const char *path )
{
	char  native[MAX_PATH];
	DWORD attr;

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	/* POSIX remove() removes directories too. */
	attr = GetFileAttributesA( native );

	if( attr != INVALID_FILE_ATTRIBUTES && ( attr & FILE_ATTRIBUTE_DIRECTORY ))
		return RemoveDirectoryA( native )
			? 0 : ( errno = r3d_errno_from_win32( GetLastError()), -1 );

	return DeleteFileA( native )
		? 0 : ( errno = r3d_errno_from_win32( GetLastError()), -1 );
}

int rename( const char *from, const char *to )
{
	char nfrom[MAX_PATH], nto[MAX_PATH];

	if( !r3d_native_path( from, nfrom, sizeof( nfrom ))
		|| !r3d_native_path( to, nto, sizeof( nto )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	return MoveFileA( nfrom, nto )
		? 0 : ( errno = r3d_errno_from_win32( GetLastError()), -1 );
}

/*
====================
unlink
====================
*/
int unlink( const char *path )
{
	char native[MAX_PATH];

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	return DeleteFileA( native )
		? 0 : ( errno = r3d_errno_from_win32( GetLastError()), -1 );
}

/*
====================
rmdir
====================
*/
int rmdir( const char *path )
{
	char native[MAX_PATH];

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	return RemoveDirectoryA( native )
		? 0 : ( errno = r3d_errno_from_win32( GetLastError()), -1 );
}

/*
====================
access

FATX carries no permission bits, so any existing path answers yes to every
mode. Reporting write access on a read-only medium would be a lie, but nxdk
gives no way to tell, and callers here only test F_OK and R_OK.
====================
*/
int access( const char *path, int mode )
{
	char  native[MAX_PATH];
	DWORD attr;

	( void )mode;

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	attr = GetFileAttributesA( native );
	return ( attr == INVALID_FILE_ATTRIBUTES )
		? ( errno = r3d_errno_from_win32( GetLastError()), -1 ) : 0;
}

/*
====================
chdir / getcwd

X0.5, route 1 (docs/r3d/fork-plan.md) - see r3d_cwd's own comment above
for why an emulated CWD is the right shape for this platform, not a lie
this file used to refuse to tell. Real POSIX chdir semantics: an
absolute argument replaces r3d_cwd outright; a relative one resolves
against the CURRENT r3d_cwd first (through r3d_native_path, which
already knows how), exactly like changing directory relative to where
you already are. Existence is checked the same way FS_SysOpen would
check any other directory - GetFileAttributesA, FILE_ATTRIBUTE_
DIRECTORY - so chdir("valve") to a name that is not really a directory
here fails instead of silently making every subsequent relative path
wrong.

filesystem/sys.c's FS_SetCurrentDirectory routes to this function on
Xbox now (widened alongside XASH_POSIX, not a new branch - this file's
own contract already matches: 0 on success, -1 with errno set).

Previously an honest, permanent ENOSYS ("the Xbox has no per-process
current directory... fail rather than pretending to succeed" - a real,
deliberate design note when it was written, now superseded by the case
made above, kept here in the history rather than pretended away).
====================
*/
int chdir( const char *path )
{
	char resolved[MAX_PATH];
	DWORD attr;

	if( !r3d_native_path( path, resolved, sizeof( resolved )))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	attr = GetFileAttributesA( resolved );
	if( attr == INVALID_FILE_ATTRIBUTES )
	{
		errno = r3d_errno_from_win32( GetLastError());
		return -1;
	}
	if( !( attr & FILE_ATTRIBUTE_DIRECTORY ))
	{
		errno = ENOTDIR;
		return -1;
	}

	if( strlen( resolved ) >= sizeof( r3d_cwd ))
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy( r3d_cwd, resolved, strlen( resolved ) + 1 );

	return 0;
}

char *getcwd( char *buf, size_t size )
{
	size_t len = strlen( r3d_cwd );

	if( !buf || size == 0 )
	{
		errno = EINVAL;
		return NULL;
	}
	if( len >= size )
	{
		errno = ERANGE;
		return NULL;
	}

	memcpy( buf, r3d_cwd, len + 1 );
	return buf;
}

/*
====================
dup

On the live path, contrary to what an earlier version of this comment claimed.

The mistake is worth recording: the comment said the only caller sat behind
XASH_REDUCE_FD, which this build does not enable. That inverted the guard.
filesystem/sys.c:71-73 defines HAVE_DUP for every platform except PSVita and
NSwitch, so Xbox gets it, and it is the XASH_REDUCE_FD branch (sys.c:446-450)
that avoids dup() - and XASH_REDUCE_FD is set only for DOS4GW and PSP
(common/defaults.h:44,49).

So FS_OpenHandle (sys.c:453) calls this on every open of a file inside a PAK,
which is where retail Half-Life keeps all of its content. Returning -1 here
meant nothing in pak0.pak could be read at all.

nxdk exposes no DuplicateHandle, but the kernel call underneath it is
available.
====================
*/
/* nxdk declares NtDuplicateObject (xboxkrnl.h:2975) but defines none of the
 * option constants. This is the standard NT value. */
#ifndef DUPLICATE_SAME_ACCESS
#define DUPLICATE_SAME_ACCESS 0x00000002
#endif

int dup( int fd )
{
	HANDLE dst;

	if( !FD_VALID( fd )) { errno = EBADF; return -1; }

	if( !NT_SUCCESS( NtDuplicateObject( r3d_fds[fd], &dst, DUPLICATE_SAME_ACCESS )))
	{
		errno = EMFILE;
		return -1;
	}

	return r3d_alloc_fd( dst );
}

/*
====================
Directory iteration

Win32's find API hands back the first entry from FindFirstFileA, while POSIX
opendir returns only a handle. The gap is closed with one entry of lookahead.
====================
*/
struct DIR
{
	HANDLE           find;
	WIN32_FIND_DATAA data;
	int              pending;   /* data holds an entry not yet returned */
	struct dirent    ent;
};

DIR *opendir( const char *path )
{
	char  native[MAX_PATH];
	size_t len;
	DIR  *dir;

	if( !r3d_native_path( path, native, sizeof( native )))
	{
		errno = ENAMETOOLONG;
		return NULL;
	}

	len = strlen( native );

	if( len + 3 >= sizeof( native ))
	{
		errno = ENAMETOOLONG;
		return NULL;
	}

	if( len > 0 && native[len - 1] != '\\' )
		native[len++] = '\\';

	native[len++] = '*';
	native[len]   = '\0';

	dir = ( DIR * )calloc( 1, sizeof( DIR ));

	if( !dir )
	{
		errno = ENOMEM;
		return NULL;
	}

	dir->find = FindFirstFileA( native, &dir->data );

	if( dir->find == INVALID_HANDLE_VALUE )
	{
		free( dir );
		errno = ENOENT;
		return NULL;
	}

	dir->pending = 1;
	return dir;
}

struct dirent *readdir( DIR *dir )
{
	if( !dir ) { errno = EBADF; return NULL; }

	if( !dir->pending && !FindNextFileA( dir->find, &dir->data ))
		return NULL;

	dir->pending = 0;

	dir->ent.d_ino  = 0;
	dir->ent.d_type = ( dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		? DT_DIR : DT_REG;

	strncpy( dir->ent.d_name, dir->data.cFileName, NAME_MAX );
	dir->ent.d_name[NAME_MAX] = '\0';

	return &dir->ent;
}

int closedir( DIR *dir )
{
	if( !dir ) { errno = EBADF; return -1; }

	FindClose( dir->find );
	free( dir );
	return 0;
}

/*
====================
MSVC CRT aliases

common/port.h maps open->_open and read->_read under XASH_WIN32, so these
names have to exist. They are the same functions under another spelling.
====================
*/
int _open( const char *path, int flags, ... )    { return open( path, flags ); }
ssize_t _read( int fd, void *buf, size_t n )     { return read( fd, buf, n ); }
ssize_t _write( int fd, const void *buf, size_t n ) { return write( fd, buf, n ); }
off_t _lseek( int fd, off_t off, int whence )    { return lseek( fd, off, whence ); }
int _close( int fd )                             { return close( fd ); }
int _unlink( const char *path )                  { return unlink( path ); }
int _mkdir( const char *path )                   { return mkdir( path, 0777 ); }
int _rmdir( const char *path )                   { return rmdir( path ); }
int _access( const char *path, int mode )        { return access( path, mode ); }
int _dup( int fd )                               { return dup( fd ); }


/*
====================
fileno

pdclib keeps the OS handle in its FILE, at `handle`
(deps/nxdk/lib/pdclib/include/pdclib/_PDCLIB_int.h:297), but exposes no fileno.

An earlier version of this returned that handle raw, with a comment claiming
the only caller "passes it straight to isatty-style checks and never to read()
or close()". Both halves were false. There is no isatty anywhere in engine/,
and engine/common/sys_con.c:126 stores the result in s_ld.logfileno and then
passes it to write() at :251 and :257 - resolved through compat/xbox/io.h to
the write() above, which treats its argument as an index into r3d_fds.

An NT handle is not a descriptor index. Small handles land inside [3, 64) and
would have written console output into whatever unrelated file occupied that
slot; larger ones fail FD_VALID and the log body vanishes silently, because
both call sites discard the error. Only the header and footer, which go through
pdclib's FILE, would have appeared.

It was latent rather than live - nxdk's crt0 passes argc = 0
(deps/nxdk/lib/pdclib/platform/xbox/crt0.c:42-43), so Sys_CheckParm can never
match -log - but a console port wants argument injection, and PSVita already
has exactly that at engine/common/launcher.c:39-43. The trap would have sprung
the day someone added the Xbox branch.

So this registers the handle properly and returns a real descriptor. The slot
is marked BORROWED: pdclib owns that handle and will close it in fclose, so
close() must release the slot without touching the handle. See r3d_borrowed.

Known bound, not a bug to fix here: the caller does not balance this. Sys_CloseLog
(sys_con.c:166) calls fclose and drops the FILE*, but never close()s the
descriptor, so the slot stays occupied with a handle pdclib has closed. It is
bounded by the number of log opens - one, in practice - and it is unreachable,
because sys_con.c:338 gates every write on s_ld.logfile being non-NULL. The
stale value cannot be mistaken for a live one either: fileno's lookup key
includes the FILE*, so a recycled handle number alone will not match it.

This is the second time in this file that a confidently justified comment
described a caller that does not exist. See docs/r3d/lessons.md.
====================
*/
int fileno( FILE *stream )
{
	HANDLE h;
	int    fd;

	if( !stream ) { errno = EBADF; return -1; }

	h = ( HANDLE )stream->handle;

	/* Already registered for THIS stream, and still describing its current
	 * handle? Then hand back the same descriptor rather than consuming a slot
	 * per call. Both halves of the key are needed - see r3d_borrowed_file. */
	for( fd = R3D_FIRST_FD; fd < R3D_MAX_FDS; fd++ )
	{
		if( r3d_borrowed[fd] && r3d_borrowed_file[fd] == stream && r3d_fds[fd] == h )
			return fd;
	}

	/* r3d_take_fd, not r3d_alloc_fd: on exhaustion the owning variant closes
	 * the handle, and this one belongs to pdclib's FILE. */
	fd = r3d_take_fd( h );

	if( fd >= 0 )
	{
		r3d_borrowed[fd] = 1;
		r3d_borrowed_file[fd] = stream;
	}

	return fd;
}

/*
====================
_r3d_release_stream_fd

Releases the borrowed r3d_fds slot fileno() registered for this FILE*, if
one exists, WITHOUT touching the OS handle or the FILE itself - the real
close still has to happen separately (fclose()).

Added 2026-09-08 for 3rdparty/opusfile's stream.c: op_fseek's SEEK_END
path calls fileno() once per seekable open (opusfile seeks to end during
op_open_seekable2_impl, opusfile.c:1399), and a borrowed slot is released
ONLY by this file's own close(fd) - never by fclose() alone, since fclose
is pdclib's own function with no idea this table exists. opusfile's
default close callback is plain fclose. Found in review: without this,
every seekable opusfile open permanently consumed one of R3D_MAX_FDS - 3
= 61 slots for the rest of the process's life, and after enough of them
fileno()/_filelengthi64 would start failing even though the underlying
file was genuinely closed - not yet exercised at runtime (nothing calls
opusfile from the engine yet), but a real bug regardless of when it fires.
Callers needing this: an NXDK-specific close callback that runs this
before fclose(), matching the SEEK_END path's own fileno() call.
====================
*/
void _r3d_release_stream_fd( FILE *stream )
{
	int fd;

	for( fd = R3D_FIRST_FD; fd < R3D_MAX_FDS; fd++ )
	{
		if( r3d_borrowed[fd] && r3d_borrowed_file[fd] == stream )
		{
			close( fd );
			return;
		}
	}
}


/*
====================
_fseeki64 / _ftelli64

MSVC's 64-bit stdio offsets. 3rdparty/vorbis reaches for them under _WIN32,
which is true here.

**These are 32-bit underneath.** pdclib's fseek and ftell take and return long,
which is 32 bits on this target, so files above 2 GB cannot be addressed. That
is not a limitation worth engineering around: the Xbox's media is a DVD or a
hard disc partition far below that, and FATX caps a single file at 4 GB anyway.
Recorded so nobody later assumes the 64 in the name is real.
====================
*/
int _fseeki64( FILE *stream, long long offset, int origin )
{
	if( offset > 0x7FFFFFFFLL || offset < -0x80000000LL )
	{
		errno = EINVAL;
		return -1;
	}

	return fseek( stream, ( long )offset, origin );
}

long long _ftelli64( FILE *stream )
{
	long pos = ftell( stream );
	return ( pos < 0 ) ? -1LL : ( long long )pos;
}

/*
====================
_fileno / _get_osfhandle / _filelengthi64

MSVC CRT names, added 2026-09-08 for 3rdparty/opusfile's stream.c (see
patches/opusfile-xbox-stream.patch - only the two calls this platform
still needs went through this file's own fileno()/r3d_fds, the rest of
that file's _WIN32 branch was patched out instead of shimmed, since nxdk
has no GetFileType or wide-char CRT to shim against at all).

_fileno is the same function as fileno() above under its MSVC spelling -
common/port.h's own "MSVC CRT aliases" pattern, applied here for
third-party code that reaches for the underscore name directly instead of
through a macro this project controls.

_get_osfhandle looks up the real Win32 HANDLE fileno() registered for a
descriptor. This is the one place outside fileno()/close() that reads
r3d_fds directly rather than going through read()/write()/lseek() - a
raw HANDLE is exactly what a caller doing its own GetFileSizeEx needs, and
duplicating that lookup through an existing wrapper would cost a needless
indirection for no safety benefit; FD_VALID still gates it.

_filelengthi64 is real MSVC CRT surface for "how big is the file behind
this descriptor" - opusfile's op_fseek uses it for SEEK_END, since fgetpos/
fsetpos (used for SEEK_CUR/SEEK_SET) have no equivalent for "seek to the
end" on their own. GetFileSizeEx is genuinely implemented by nxdk's winapi
(deps/nxdk/lib/winapi/fileio.c, via FILE_NETWORK_OPEN_INFORMATION) - this
is not a fabricated file length. 32-bit LARGE_INTEGER-to-long long is safe
here for the same reason _fseeki64 above already only guards a 32-bit
range: FATX caps a file at 4 GB, far under LARGE_INTEGER's real range.
====================
*/
int _fileno( FILE *stream )
{
	return fileno( stream );
}

intptr_t _get_osfhandle( int fd )
{
	if( !FD_VALID( fd ))
	{
		errno = EBADF;
		return -1;
	}

	return ( intptr_t )r3d_fds[fd];
}

long long _filelengthi64( int fd )
{
	LARGE_INTEGER size;

	if( !FD_VALID( fd ))
	{
		errno = EBADF;
		return -1;
	}

	if( !GetFileSizeEx( r3d_fds[fd], &size ))
	{
		errno = EIO;
		return -1;
	}

	return ( long long )size.QuadPart;
}
