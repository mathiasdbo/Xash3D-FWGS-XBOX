/*
fcntl.h - POSIX open() shim for nxdk

Part of compat/xbox; see that directory's README for why this exists.

The O_* values are this shim's own — nothing on the Xbox side interprets them
but xbox_posix_io.c, so they only have to be internally consistent and
distinct as bit flags. filesystem/filesystem.c:26 defines O_BINARY to 0 when
it is absent, so it is given a real (harmless) bit here to keep the two paths
from diverging.
*/
#ifndef R3D_COMPAT_FCNTL_H
#define R3D_COMPAT_FCNTL_H

#include <sys/types.h>

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003

#define O_CREAT    0x0100
#define O_TRUNC    0x0200
#define O_EXCL     0x0400
#define O_APPEND   0x0800

/* No text-mode translation happens on this platform: every open is binary.
 * The flag exists so code that ORs it in still compiles and compares equal. */
#define O_BINARY   0x8000
#define O_TEXT     0x0000

#ifdef __cplusplus
extern "C" {
#endif

int open( const char *path, int flags, ... );

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_FCNTL_H */
