/*
sys/utime.h - file timestamp shim for nxdk

Part of compat/xbox; see that directory's README.

NOTHING CALLS THIS IN THE CURRENT CONFIGURATION. An earlier version of this
comment said "only public/miniz.c needs this", which was wrong in the way that
matters: miniz's `utime` call at :3504 sits inside `#ifndef MINIZ_NO_STDIO`
(opened at :3480), and its three `#include <sys/utime.h>` lines at :3132, :3153
and :3169 are all in the `#else` arm of the `#ifdef MINIZ_NO_STDIO` at :3065.
tools/xbox-cc.sh:104 passes -DMINIZ_NO_STDIO=1, so none of them are reached.

It is kept rather than deleted because it is correct and cheap, and because the
day MINIZ_NO_STDIO comes off — the only thing that would make miniz's archive
*writing* path live — this is what it needs. FATX does store a write time, so
the operation is meaningful and utime() is implemented in xbox_win32_ext.c
rather than stubbed.

Do not read this as "archive extraction preserves timestamps on Xbox". It does
not, because nothing calls it. See docs/r3d/lessons.md on comments that assert
a caller.
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
