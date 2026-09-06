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
r3d_alloc_fd
====================
*/
static int r3d_alloc_fd( HANDLE h )
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

	CloseHandle( h );
	errno = EMFILE;
	return -1;
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
		errno = ENOENT;
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

	CloseHandle( r3d_fds[fd] );
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
	char             native[MAX_PATH];
	WIN32_FIND_DATAA fd;
	HANDLE           find;

	if( !st || !r3d_native_path( path, native, sizeof( native )))
	{
		errno = EINVAL;
		return -1;
	}

	/* FindFirstFileA answers attributes, size and timestamps in one call,
	 * where GetFileAttributesA would only give the first. */
	find = FindFirstFileA( native, &fd );

	if( find == INVALID_HANDLE_VALUE )
	{
		errno = ENOENT;
		return -1;
	}

	FindClose( find );

	memset( st, 0, sizeof( *st ));
	st->st_mode  = ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		? ( S_IFDIR | S_IRWXU ) : ( S_IFREG | S_IRUSR | S_IWUSR );
	st->st_size  = ( off_t )fd.nFileSizeLow;
	st->st_mtime = r3d_filetime_to_time_t( &fd.ftLastWriteTime );
	st->st_atime = st->st_mtime;
	st->st_ctime = st->st_mtime;
	st->st_nlink = 1;

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
		errno = EEXIST;
		return -1;
	}

	return 0;
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

	return DeleteFileA( native ) ? 0 : ( errno = ENOENT, -1 );
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

	return RemoveDirectoryA( native ) ? 0 : ( errno = ENOENT, -1 );
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
	return ( attr == INVALID_FILE_ATTRIBUTES ) ? ( errno = ENOENT, -1 ) : 0;
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

nxdk exposes no DuplicateHandle. The only caller is behind XASH_REDUCE_FD,
which this build does not enable; if that changes, reopen by name instead.
====================
*/
int dup( int fd )
{
	( void )fd;
	errno = ENOSYS;
	return -1;
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
