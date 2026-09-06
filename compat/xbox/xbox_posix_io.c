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
r3d_native_path

GoldSrc paths are '/'-separated; FATX wants '\'. Converts in place into a
caller-supplied buffer and returns it, or NULL if the path does not fit.

Deliberately does no other normalisation: no "..", no case folding, no drive
mapping. Callers pass paths that filesystem/ has already assembled, and
inventing a second normalisation layer here would make divergences between the
two very hard to find.
====================
*/
static const char *r3d_native_path( const char *path, char *out, size_t out_size )
{
	size_t i;

	if( !path || !out || out_size == 0 )
		return NULL;

	for( i = 0; i < out_size - 1 && path[i]; i++ )
		out[i] = ( path[i] == '/' ) ? '\\' : path[i];

	if( path[i] )   /* did not fit */
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
	DWORD  attr;
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION info;

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

	/* GetFileAttributesA, not FindFirstFileA. The find API treats the last
	 * path component as a wildcard PATTERN rather than a name, and nxdk's
	 * implementation of that handling is its own hazard
	 * (deps/nxdk/lib/winapi/findfile.c:51-72). It also costs an NtOpenFile on
	 * the parent plus a directory enumeration per probe, on the hot path of
	 * every file lookup - this is a plain attribute query. */
	attr = GetFileAttributesA( native );

	if( attr == INVALID_FILE_ATTRIBUTES )
	{
		errno = r3d_errno_from_win32( GetLastError());
		return -1;
	}

	memset( st, 0, sizeof( *st ));
	st->st_nlink = 1;

	if( attr & FILE_ATTRIBUTE_DIRECTORY )
	{
		/* A directory has no size or meaningful timestamps here, and opening
		 * one to ask would need FILE_FLAG_BACKUP_SEMANTICS. Callers only test
		 * S_ISDIR on this path (FS_SysFolderExists, sys.c:294-306). */
		st->st_mode = S_IFDIR | S_IRWXU;
		return 0;
	}

	st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;

	/* Size and mtime need the file itself. If it cannot be opened - in use,
	 * say - report what is known rather than failing a call that already
	 * proved the file exists. */
	h = CreateFileA( native, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );

	if( h == INVALID_HANDLE_VALUE )
		return 0;

	if( GetFileInformationByHandle( h, &info ))
	{
		st->st_size  = ( off_t )info.nFileSizeLow;
		st->st_mtime = r3d_filetime_to_time_t( &info.ftLastWriteTime );
		st->st_atime = st->st_mtime;
		st->st_ctime = st->st_mtime;
	}

	CloseHandle( h );
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

The Xbox has no per-process current directory. nxdk resolves every path
against a drive letter and exposes no SetCurrentDirectoryA, so these fail
rather than pretending to succeed - a silently wrong working directory would
surface much later as files that cannot be found.

filesystem_engine.c's root-directory detection has to be handed an absolute
path instead. Tracked as X0.5 in docs/r3d/fork-plan.md.
====================
*/
int chdir( const char *path )
{
	( void )path;
	errno = ENOSYS;
	return -1;
}

char *getcwd( char *buf, size_t size )
{
	( void )buf;
	( void )size;
	errno = ENOSYS;
	return NULL;
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
