/*
xbox_diskspace.h - how much room the hard drive has left for saved games
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
#ifndef XBOX_DISKSPACE_H
#define XBOX_DISKSPACE_H

#include "common.h"

/*
A full E: drive does not fail loudly: the engine's writes die part-way (a working file cut at 64 KiB, the next ones empty), a save
loads half restored (no weapons, no suit, enemies that do not think) and nothing says why. These say it.

Everything here answers "is it KNOWN to be low": a drive that cannot be asked, or a failed query, is never taken for a full one, so
this cannot refuse a save the drive could have taken.
*/

#define XBOX_DISK_UNKNOWN ((uint64_t)-1)

/* free bytes on E: (the drive that holds E:\UDATA\<title id>\<game>, filesystem_engine.c), or XBOX_DISK_UNKNOWN */
uint64_t Xbox_DiskFreeBytes( void );

/* true only when the free space is known and below `need_bytes` */
qboolean Xbox_DiskLow( uint64_t need_bytes );

/* r3d_disk_low_mb: the room a save or a load needs, MiB (the largest save seen is 2 MiB; it is written as .tmp, renamed
   .new, then .sav, and a load rewrites the per-map working files) */
uint64_t Xbox_DiskNeededForSave( void );

void Xbox_DiskSpace_Init( void ); /* registers r3d_disk_low_mb; called from Xbox_FrameTime_Init, before the configs run */

#endif // XBOX_DISKSPACE_H
