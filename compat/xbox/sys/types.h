/*
sys/types.h - POSIX type shim for nxdk

Part of compat/xbox; see that directory's README for why this exists.
On the Xbox include path only.
*/
#ifndef R3D_COMPAT_SYS_TYPES_H
#define R3D_COMPAT_SYS_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* nxdk's xboxrt/libc_extensions/stat.h already typedefs mode_t; including it
 * here keeps the two definitions from colliding if both headers are pulled in.
 * Everything else in that header sits inside an #if 0. */
#include <stat.h>

typedef long          off_t;
typedef int           ssize_t;
typedef unsigned int  dev_t;
typedef unsigned int  ino_t;
typedef unsigned int  nlink_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef long          time_t_compat;

#endif /* R3D_COMPAT_SYS_TYPES_H */
