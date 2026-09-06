/*
dirent.h - directory iteration shim for nxdk

Part of compat/xbox; see that directory's README for why this exists.
nxdk has no dirent.h anywhere.

Backed by FindFirstFileA / FindNextFileA / FindClose. Note the semantic gap
that follows from that: the Win32 find API returns its first entry from
FindFirstFileA, whereas POSIX opendir returns a handle and readdir produces
every entry. The DIR here therefore carries one entry of lookahead.
*/
#ifndef R3D_COMPAT_DIRENT_H
#define R3D_COMPAT_DIRENT_H

#include <sys/types.h>

#define NAME_MAX 255

struct dirent
{
	ino_t          d_ino;         /* always 0 - FATX has no inodes */
	unsigned char  d_type;
	char           d_name[NAME_MAX + 1];
};

/* d_type values, matching the POSIX names filesystem/ tests against. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

typedef struct DIR DIR;

#ifdef __cplusplus
extern "C" {
#endif

DIR           *opendir( const char *path );
struct dirent *readdir( DIR *dir );
int            closedir( DIR *dir );

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_DIRENT_H */
