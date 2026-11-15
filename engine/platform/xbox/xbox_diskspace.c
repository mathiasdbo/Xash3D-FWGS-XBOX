/*
xbox_diskspace.c - how much room the hard drive has left for saved games
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
#include "xbox_diskspace.h"

#if XASH_XBOX && !XASH_DEDICATED
#include <windows.h>

static CVAR_DEFINE_AUTO( r3d_disk_low_mb, "16", FCVAR_FILTERABLE,
	"a save or a load is refused with a message when the hard drive is known to have less than this many MiB free (0: never)" );

uint64_t Xbox_DiskFreeBytes( void )
{
	ULARGE_INTEGER avail, total, freeb;

	if( !GetDiskFreeSpaceExA( "E:\\", &avail, &total, &freeb ))
		return XBOX_DISK_UNKNOWN;

	/* a volume that reports nothing at all is a failed query, not an empty drive */
	if( total.QuadPart == 0 )
		return XBOX_DISK_UNKNOWN;

	return (uint64_t)avail.QuadPart;
}

qboolean Xbox_DiskLow( uint64_t need_bytes )
{
	uint64_t free_bytes;

	if( need_bytes == 0 )
		return false;

	free_bytes = Xbox_DiskFreeBytes();
	return free_bytes != XBOX_DISK_UNKNOWN && free_bytes < need_bytes;
}

uint64_t Xbox_DiskNeededForSave( void )
{
	return (uint64_t)bound( 0, (int)r3d_disk_low_mb.value, 4096 ) * 1024ull * 1024ull;
}

void Xbox_DiskSpace_Init( void )
{
	Cvar_RegisterVariable( &r3d_disk_low_mb );
}
#endif // XASH_XBOX
