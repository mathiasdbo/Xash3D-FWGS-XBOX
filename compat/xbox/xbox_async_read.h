/*
xbox_async_read.h - one file read while the caller works
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
Resonance3D (docs/r3d/divergences.md #172). Every read() of the POSIX
layer (xbox_posix_io.c) is synchronous: the kernel returns when the bytes
are in the caller's buffer, and on the console the thread waits out the
disk. The baked music refilled its read-ahead that way, and each 9 KiB
refill cost 0.5-1.0 ms of the frame it landed in (k_snd_music,
session 8c7db857 on the console, 2026-10-10).

This is the other shape: a handle opened for overlapped, unbuffered
reads (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nxdk's CreateFileA,
deps/nxdk/lib/winapi/fileio.c:69 and :68), one read in flight at a time,
issued with an explicit offset and an event (NtReadFile, as nxdk's
overlapped ReadFile does at fileio.c:115-141, called directly so the
returned status decides whether the read is queued) and collected later
(GetOverlappedResult, deps/nxdk/lib/winapi/ioapi.c:10-37).
The kernel queues the read to the disk and returns STATUS_PENDING, and the
caller's thread goes on while the drive transfers. Same thread, no lock:
nothing here is shared with the descriptor table.

An unbuffered read must start on a sector boundary of the file, ask for
whole sectors and land in a buffer aligned to one: callers use 4096-byte
units (the hard disk's sectors are 512 bytes, the DVD's 2048). The buffer
and `ov` belong to the kernel until the read is finished: finish it (or
close, which waits) before freeing either.

Every kernel call goes through R3D_IO_CALL (xbox_io_stats.h), in the
categories tools/test_posix_io_counters.py checks: the open is an open,
the read a read, the event and the completion meta (GetOverlappedResult
makes no kernel call when the read has already completed, but is counted
all the same), the closes closes.
*/

#ifndef XBOX_ASYNC_READ_H
#define XBOX_ASYNC_READ_H

#include <stdint.h>
#include <windows.h>

typedef struct r3d_async_read_s
{
	HANDLE handle;      /* INVALID_HANDLE_VALUE when not open */
	HANDLE event;       /* manual reset, signalled when a read completes; r3d_async_start
	                       clears it (NtClearEvent) before each read rather than relying on
	                       NtReadFile to, which is unverified on the Xbox kernel */
	OVERLAPPED ov;
	uint32_t asked;
	int state;          /* R3D_ASYNC_IDLE / _RUNNING / _DONE */
	int done_bytes;     /* _DONE: what the read delivered, or -1 */
} r3d_async_read_t;

#define R3D_ASYNC_IDLE     0
#define R3D_ASYNC_RUNNING  1   /* the kernel has it */
#define R3D_ASYNC_DONE     2   /* finished on the spot; finish hands out done_bytes */

#define R3D_ASYNC_PENDING  ( -2 )

/* Opens `path` (resolved as open() resolves it) for overlapped,
   unbuffered reads. Returns 0, or -1 with errno set and nothing left
   open. */
int r3d_async_open( r3d_async_read_t *a, const char *path );

/* Issues a read of `bytes` at `offset` into `dst`. Returns 1 when the
   kernel is still on it, 0 when it finished on the spot, -1 when it was
   refused (a read already in flight, a bad parameter, an error). */
int r3d_async_start( r3d_async_read_t *a, uint32_t offset, void *dst, uint32_t bytes );

/* The bytes the read delivered (or -1 for an error), waiting for it when
   `wait` is set; R3D_ASYNC_PENDING when `wait` is not set and the read is
   still running (that answer makes no kernel call). *waited is set when
   this call had to block on the disk. -1 when no read was issued. */
int r3d_async_finish( r3d_async_read_t *a, int wait, int *waited );

/* Waits for a read in flight, then closes. Idempotent. */
void r3d_async_close( r3d_async_read_t *a );

#endif /* XBOX_ASYNC_READ_H */
