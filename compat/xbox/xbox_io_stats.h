/*
xbox_io_stats.h - counters of the POSIX file layer's kernel calls
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
Resonance3D (docs/r3d/divergences.md #162). Every file the engine, the
filesystem module and the games read on the Xbox goes through
xbox_posix_io.c's open/read/lseek/stat/close, each a synchronous call
into the kernel's FATX or CDFS driver. Every Win32 or kernel call that
file makes goes through R3D_IO_CALL below, which counts it in one
category, with the time-stamp-counter ticks spent inside it, and read()
and write() add the bytes they move, so the
frame-time recorder's load notes (engine/platform/xbox/xbox_frametime.c,
XBOX_FT_NOTE_LOAD) can split a load into time waiting on the disk and
time on the CPU. "Inside the kernel" is both: the driver's own copy out
of its cache and the wait for the IDE transfer. xemu models no seek or
transfer latency, so there it is mostly the copy; on the console the
difference between the two is the disk.

Always on: an rdtsc and a few adds per kernel call, which costs
microseconds.
*/

#ifndef XBOX_IO_STATS_H
#define XBOX_IO_STATS_H

#include <stdint.h>

typedef struct r3d_io_stats_s
{
	/* One per kernel call, by category (xbox_posix_io.c names each call's): */
	uint32_t opens;       /* CreateFileA: open(), failed ones included */
	uint32_t open_fails;  /* ... of which failed */
	uint32_t reads;       /* ReadFile: read() */
	uint32_t writes;      /* WriteFile: write() */
	uint32_t seeks;       /* SetFilePointer: lseek(), open()'s O_APPEND, ftruncate()'s positioning */
	uint32_t probes;      /* GetFileAttributes(Ex)A: stat(), access(), chdir() and remove()'s
	                         directory test - a lookup's existence tests */
	uint32_t closes;      /* CloseHandle: close(), and open()/dup()'s full-table cleanup */
	uint32_t meta;        /* the rest: NtFlushBuffersFile (fsync), SetEndOfFile (ftruncate),
	                         GetFileInformationByHandle (fstat), CreateDirectoryA,
	                         RemoveDirectoryA, DeleteFileA, MoveFileA, NtDuplicateObject (dup),
	                         GetFileSizeEx, and the directory scans' FindFirstFileA,
	                         FindNextFileA and FindClose */
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t ticks;       /* TSC ticks inside all of the above */
} r3d_io_stats_t;

extern r3d_io_stats_t r3d_io_stats;

static inline uint64_t r3d_io_rdtsc( void )
{
	uint32_t lo, hi;

	__asm__ __volatile__( "rdtsc" : "=a"( lo ), "=d"( hi ));
	return ( (uint64_t)hi << 32 ) | lo;
}

/* Makes one Win32/kernel call, counted in r3d_io_stats.<field> (the
   categories above) with its ticks added to `ticks`; evaluates to the
   call's result. GetLastError is untouched (rdtsc sets no error code).
   tools/test_posix_io_counters.py checks that every such call in
   xbox_posix_io.c goes through this, in its category. */
#define R3D_IO_CALL( field, call ) \
	( __extension__ ({ \
		const uint64_t r3d_io_t0_ = r3d_io_rdtsc(); \
		__typeof__( call ) r3d_io_r_ = ( call ); \
		r3d_io_stats.ticks += r3d_io_rdtsc() - r3d_io_t0_; \
		r3d_io_stats.field++; \
		r3d_io_r_; \
	}))

#endif /* XBOX_IO_STATS_H */
