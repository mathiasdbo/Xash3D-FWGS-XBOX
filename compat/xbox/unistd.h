/*
unistd.h - POSIX file-descriptor I/O shim for nxdk

Part of compat/xbox; see that directory's README for why this exists.

nxdk ships an xboxrt/libc_extensions/unistd.h that contains nothing but a
licence header. This one is on the include path ahead of it and supplies the
calls filesystem/ actually makes.
*/
#ifndef R3D_COMPAT_UNISTD_H
#define R3D_COMPAT_UNISTD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* common/port.h:47-48 rewrites open->_open and read->_read under
 * XASH_WIN32, which is true for us, so any file including <unistd.h> on
 * this platform needs the underscore spellings visible too. Implemented as
 * thin wrappers in xbox_posix_io.c. */
int     _open( const char *path, int flags, ... );
ssize_t _read( int fd, void *buf, size_t count );

ssize_t read( int fd, void *buf, size_t count );
ssize_t write( int fd, const void *buf, size_t count );
off_t   lseek( int fd, off_t offset, int whence );
int     close( int fd );
int     fsync( int fd );
int     ftruncate( int fd, off_t length );

/* pdclib defines these, but without '/' to '\' conversion, so ours must
 * be the ones that link. See xbox_posix_io.c. */
int     remove( const char *path );
int     rename( const char *from, const char *to );

int     unlink( const char *path );
int     rmdir( const char *path );
int     access( const char *path, int mode );

/* Always fails. The Xbox has no per-process current directory: nxdk resolves
 * every path against a drive letter, and there is no SetCurrentDirectoryA to
 * build this on. filesystem_engine.c's root-directory detection has to be
 * given an absolute path instead - tracked as X0.5 in docs/r3d/fork-plan.md. */
int     chdir( const char *path );
char   *getcwd( char *buf, size_t size );

/* Always fails. nxdk exposes no DuplicateHandle. The single caller is behind
 * XASH_REDUCE_FD, which this build does not enable; if that changes, reopen
 * the file by name rather than duplicating the descriptor. */
int     dup( int fd );

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_UNISTD_H */
