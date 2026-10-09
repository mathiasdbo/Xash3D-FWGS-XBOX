/*
xbox_dir_cache.h - listings of the disc's directories, scanned once
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #166). An image asked for without an
extension (a decal, a sky side, a texture by name) is looked up with
FS_Search( "<name>.*" ) (engine/common/imagelib/img_main.c,
USE_FS_SEARCH_FOR_LOOKUPS, :330-377 in the reference), and FS_Search lists
every plain directory of the search path whole (filesystem/dir.c
FS_Search_DIR -> listdirectory), again for each cube-map side suffix it
tries. On the disc that is D:\valve\ (about 200 entries) and D:\, one
FindFirstFileA/FindNextFileA kernel call per entry: in xemu c1a0c's 104
static decals made 5,192 of those calls and its world load 1,889.

The disc's directories never change while the game runs: the game writes
only under E: (fs_writepath; FS_Open in a writing mode opens nothing
else). So the file layer keeps the listing of a D: directory it scanned to
the end, and a later opendir() of the same directory, spelled the same,
replays it - the same names, in the same order, with the same types -
without a kernel call. Only a complete scan is kept (readdir reached the
end), only paths on D: free of ".", ".." and empty components, and the
whole cache is emptied by every call of the file layer that could create,
remove or rename anything anywhere (open for anything but reading, a
successful mkdir, rename, remove, unlink, rmdir), so even a D: that is a
folder of the hard disk aliased by a path the game writes stays exact.

Pure, so tools/host-tests/xbox_dir_cache_host_test.c can test it.
*/

#ifndef XBOX_DIR_CACHE_H
#define XBOX_DIR_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define R3D_DIR_CACHE_DIRS  16     /* directories kept */
#define R3D_DIR_CACHE_BYTES 16384  /* their names and entries, packed */

typedef struct r3d_dir_cache_stats_s
{
	uint32_t replayed; /* opendir() calls answered from the cache */
	uint32_t stored;   /* listings kept */
	uint32_t flushes;  /* times the cache was emptied */
} r3d_dir_cache_stats_t;

extern r3d_dir_cache_stats_t r3d_dir_cache_stats;

/* A listing as stored: entries packed as [type byte][name]['\0'] ... */
typedef struct r3d_dir_listing_s
{
	const char *data;
	size_t      bytes;
	int         count;
} r3d_dir_listing_t;

/* 1 if a directory scanned as `pattern` (the FindFirstFileA pattern,
   "<dir>\*") may be kept: on D:, every component plain. */
int r3d_dir_cache_cacheable( const char *pattern );

/* The kept listing of `pattern`, copied into *out (pointers into the
   cache: copy them before any other call of this module). 1 if found;
   counts a replay. */
int r3d_dir_cache_find( const char *pattern, r3d_dir_listing_t *out );

/* Keep a complete listing of `pattern`. Does nothing if it is not
   cacheable or does not fit. */
void r3d_dir_cache_store( const char *pattern, const char *data, size_t bytes, int count );

/* Forget everything: something may have been created, removed or renamed. */
void r3d_dir_cache_flush( void );

#endif /* XBOX_DIR_CACHE_H */
