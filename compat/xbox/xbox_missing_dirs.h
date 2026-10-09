/*
xbox_missing_dirs.h - directories the file layer has seen do not exist
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
Resonance3D (docs/r3d/divergences.md #164). Every file lookup walks the
whole search path (filesystem/searchpath.c FS_FindFile), and on this
platform two of the plain directories in it do not exist (valve/custom/
and valve_downloads/ on the hard disk), and the writable game directory
(E:\UDATA\<title>\valve\) holds none of the game's models, sounds,
sprites or events. So each lookup of a file on the disc costs, before the
disc is even asked: a FindFirstFileA on each missing directory
(filesystem/dir.c FS_MaybeUpdateDirEntries rescans a searchpath whose
root it found empty, every time) and a GetFileAttributesExA under a
directory of E: that does not exist. The xemu trace of one changelevel
(c1a0e -> c1a0c) counted 1,906 directory scans and about 900 such misses.

This table remembers directories that do not exist, and stat() and
opendir() answer a path strictly under one of them - which the kernel
would refuse with ERROR_PATH_NOT_FOUND - without a kernel call. A
directory is recorded only when its own GetFileAttributesExA failed with
ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND, which the file layer asks
in two places, once each:

- after a lookup of P failed with ERROR_PATH_NOT_FOUND (a directory above
  P's last component is missing), of P's parent;
- after a directory scan of D failed with ERROR_FILE_NOT_FOUND or
  ERROR_PATH_NOT_FOUND ("missing" or "empty"), of D.

So an entry never rests on how a driver reads "path not found" for a
missing last component: it is a directory the kernel said is not there.

An entry only means "this path does not exist", so only something coming
into existence there can make it wrong: a directory created
(CreateDirectoryA), or a directory or file renamed into place
(MoveFileA). Creating a file needs its parent directory to exist (the
entries are directories, so no file can appear under one), and deleting
or writing can only remove or change things that exist. The file layer
empties the table after every successful mkdir, before every rename
whatever its outcome, and after the root directories
engine/common/filesystem_engine.c creates at start-up. A failed mkdir
(FS_CreatePath calls it for every directory of every path it writes,
existing ones included) changes nothing and keeps the table. Nothing else
in the image creates directories or renames: every
CreateDirectoryA/MoveFileA/RemoveDirectoryA/DeleteFileA import is in
those two objects (llvm-nm over every object of the hl link), pdclib's
fopen creates files only, and utime (xbox_win32_ext.c) changes a time.
File I/O runs on the main thread only (the one other thread, net_ws.c's
DNS resolver, touches no file).

Nothing outside the game can change the disks while it runs (one title
at a time; xemu holds the disk image), so the kernel's answer for a
recorded directory cannot change behind the table's back.

Paths are compared as spelled (case included), and a path with a "." or
".." component or an empty one ("\\") is never recorded or answered, so
a table entry can only cover paths the kernel resolves through it.

Pure, so tools/host-tests/xbox_missing_dirs_host_test.c can run it
against a model of the kernel's answers.
*/

#ifndef XBOX_MISSING_DIRS_H
#define XBOX_MISSING_DIRS_H

#include <stddef.h>
#include <stdint.h>

#define R3D_MISSING_DIRS_MAX   64    /* directories remembered */
#define R3D_MISSING_DIRS_CHARS 4096  /* their paths, packed */

typedef struct r3d_missing_dirs_stats_s
{
	uint32_t answered; /* stat()/opendir() calls answered from the table */
	uint32_t recorded; /* directories recorded */
	uint32_t flushes;  /* times the table was emptied */
} r3d_missing_dirs_stats_t;

extern r3d_missing_dirs_stats_t r3d_missing_dirs_stats;

/* Forget everything: a directory may have come into existence. */
void r3d_missing_dirs_flush( void );

/* 1 if `native` (a full native path, '\' separators) lies strictly under
   a recorded directory: the kernel would fail it with
   ERROR_PATH_NOT_FOUND. Counts an answer. */
int r3d_missing_dirs_covers( const char *native );

/* The directory `native` does not exist (its own lookup failed with
   ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND): record it. */
void r3d_missing_dirs_note_dir( const char *native );

#endif /* XBOX_MISSING_DIRS_H */
