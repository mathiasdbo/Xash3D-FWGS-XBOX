/*
sys/utime.h - file timestamp shim for nxdk

Part of compat/xbox; see that directory's README.

Only public/miniz.c needs this, to preserve archive entry timestamps on
extraction. FATX does store a write time, so the operation is meaningful and
utime() is implemented in xbox_posix_io.c rather than stubbed.
*/
#ifndef R3D_COMPAT_SYS_UTIME_H
#define R3D_COMPAT_SYS_UTIME_H

#include <sys/types.h>
#include <time.h>

struct utimbuf
{
	time_t actime;    /* access time - FATX does not track this; ignored */
	time_t modtime;   /* modification time */
};

/* MSVC's spelling, which is what miniz reaches for under _WIN32. */
#define _utimbuf utimbuf

#ifdef __cplusplus
extern "C" {
#endif

int utime( const char *path, const struct utimbuf *times );
int _utime( const char *path, const struct utimbuf *times );

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_SYS_UTIME_H */
