/*
io.h - MSVC CRT aliases for nxdk

Part of compat/xbox; see that directory's README for why this exists.

filesystem/io.c includes <io.h> under XASH_WIN32 and calls _commit(), which is
the MSVC CRT's fsync. XASH_WIN32 is true for us, because XASH_XBOX is a
sub-platform of it (3rdparty/library_suffix patch, divergence #5), so this
header has to exist even though nothing on Xbox is really MSVC.

These are aliases for the POSIX entry points in xbox_posix_io.c, not separate
implementations.
*/
#ifndef R3D_COMPAT_IO_H
#define R3D_COMPAT_IO_H

#include <sys/types.h>

/* NOT via <unistd.h>. nxdk-cc appends "$@" after its own -I flags, so its
 * include directories are searched first and <unistd.h> resolves to nxdk's
 * empty licence stub rather than compat/xbox/unistd.h. Declaring here works
 * because <io.h> has no counterpart in nxdk and so does reach us.
 *
 * The waf integration builds its own command line and can put compat/xbox
 * first, at which point this duplication can go. */
ssize_t read( int fd, void *buf, size_t count );
ssize_t write( int fd, const void *buf, size_t count );
off_t   lseek( int fd, off_t offset, int whence );
int     close( int fd );
int     fsync( int fd );
int     unlink( const char *path );
int     rmdir( const char *path );
int     access( const char *path, int mode );
int     dup( int fd );
int     open( const char *path, int flags, ... );

#ifdef __cplusplus
extern "C" {
#endif

int _commit( int fd );

/* Real functions, not macros. common/port.h:47-48 already does
 *   #define open  _open
 *   #define read  _read
 * under XASH_WIN32, which is true for us. A reciprocal
 *   #define _open open
 * here would collide with it, and which side won depended on include
 * order - so these are thin wrappers in xbox_posix_io.c instead. Zero
 * edits to upstream that way. */
int     _open( const char *path, int flags, ... );
ssize_t _read( int fd, void *buf, size_t count );
ssize_t _write( int fd, const void *buf, size_t count );
off_t   _lseek( int fd, off_t offset, int whence );
int     _close( int fd );
int     _unlink( const char *path );
int     _mkdir( const char *path );
int     _rmdir( const char *path );
int     _access( const char *path, int mode );
int     _dup( int fd );

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_IO_H */
