/*
vox_stub/xbox_async_read.h - a scripted stand-in for compat/xbox/xbox_async_read.h
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

/* The same calls and return conventions as the console's
   (compat/xbox/xbox_posix_io.c, r3d_async_*), over stdio, with the timing
   the test scripts (xbox_vox_stream_host_test.c, "the disk"): a queued
   read's buffer is filled with garbage at start and with the file's bytes
   only when it lands, so code that reads it early, or frees it while
   queued, is caught. */

#ifndef VOX_STUB_ASYNC_READ_H
#define VOX_STUB_ASYNC_READ_H

#include <stdint.h>
#include <stdio.h>

typedef struct r3d_async_read_s
{
	FILE *f;
	int id;             /* the test's handle number */
	int state;          /* R3D_ASYNC_IDLE / _RUNNING / _DONE */
	int done_bytes;
	int polls;          /* RUNNING: polls left before it lands */
	uint32_t offset, asked;
	void *dst;
	char path[260];     /* for the test's scripted failures */
} r3d_async_read_t;

#define R3D_ASYNC_IDLE     0
#define R3D_ASYNC_RUNNING  1
#define R3D_ASYNC_DONE     2

#define R3D_ASYNC_PENDING  ( -2 )

int r3d_async_open( r3d_async_read_t *a, const char *path );
int r3d_async_start( r3d_async_read_t *a, uint32_t offset, void *dst, uint32_t bytes );
int r3d_async_finish( r3d_async_read_t *a, int wait, int *waited );
void r3d_async_close( r3d_async_read_t *a );
long long r3d_async_size( r3d_async_read_t *a );

#endif /* VOX_STUB_ASYNC_READ_H */
