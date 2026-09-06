/*
sys/stat.h - stat() shim for nxdk

Part of compat/xbox; see that directory's README for why this exists.

nxdk's xboxrt/libc_extensions/stat.h declares stat() and fstat() inside an
`#if 0`, and defines no `struct stat` at all, so neither is usable. This
header supplies both.

Only the fields filesystem/ reads are populated: st_mode, st_size and st_mtime.
The rest exist so that code compiles and are left zero - do not start trusting
st_ino or st_dev, which have no meaning on FATX.
*/
#ifndef R3D_COMPAT_SYS_STAT_H
#define R3D_COMPAT_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat
{
	dev_t   st_dev;
	ino_t   st_ino;
	mode_t  st_mode;
	nlink_t st_nlink;
	uid_t   st_uid;
	gid_t   st_gid;
	dev_t   st_rdev;
	off_t   st_size;
	time_t  st_atime;
	time_t  st_mtime;
	time_t  st_ctime;
};

/* Only the two bits FATX can actually distinguish are meaningful. The others
 * are defined so S_IS* macros and mode arithmetic compile. */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_IFLNK  0120000

#define S_ISDIR( m )  ((( m ) & S_IFMT ) == S_IFDIR )
#define S_ISREG( m )  ((( m ) & S_IFMT ) == S_IFREG )
#define S_ISCHR( m )  ((( m ) & S_IFMT ) == S_IFCHR )
#define S_ISFIFO( m ) ((( m ) & S_IFMT ) == S_IFIFO )
#define S_ISLNK( m )  ((( m ) & S_IFMT ) == S_IFLNK )

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700

/* FATX has no group or other permissions. Defined so that mode arithmetic
 * in upstream code compiles; they are never enforced. */
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007

#ifdef __cplusplus
extern "C" {
#endif

int stat( const char *path, struct stat *st );
int fstat( int fd, struct stat *st );

/* POSIX mkdir takes a mode; FATX has no permissions, so it is ignored. */
int mkdir( const char *path, mode_t mode );

#ifdef __cplusplus
}
#endif

#endif /* R3D_COMPAT_SYS_STAT_H */
