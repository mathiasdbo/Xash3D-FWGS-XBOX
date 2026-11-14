/*
save_legacy.h - what an `ofs:N` in a saved game written by v0.1.2, v0.1.3 or v0.2.0 means (XS.3)
Copyright (C) 2026 Resonance3D

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef SAVE_LEGACY_H
#define SAVE_LEGACY_H

// Old saves hold a think/touch/use/blocked/move-done pointer as `ofs:N`, N the function's offset from GameDLLInit in the
// link that wrote it. The three public builds share one layout, and tools/save-legacy-table.txt (XS.1) says which
// function each offset was. This file reads it. It has no engine dependencies so a host test can compile it alone.

#define SAVE_LEGACY_OK        0
#define SAVE_LEGACY_BADSTRING 1  // not `ofs:` followed by a decimal number that fits 32 bits
#define SAVE_LEGACY_UNKNOWN   2  // an offset no public build ever wrote
#define SAVE_LEGACY_BYFIELD   3  // one offset, two functions (CallMonsterThink / CallHitBottom): the game says which

// Parses `ofs:N`. N is a decimal, and a negative offset is its unsigned 32-bit wrap (`%zu` wrote 4294950224 for -17072),
// which is read as such instead of relying on atoi's overflow. Returns SAVE_LEGACY_OK or SAVE_LEGACY_BADSTRING.
int SaveLegacy_ParseOffset( const char *str, int *offset );

// The platform-neutral name (`Think@CFoo`) of the function an offset meant, in *name. SAVE_LEGACY_OK,
// SAVE_LEGACY_UNKNOWN, or SAVE_LEGACY_BYFIELD (and *name is NULL for the last two). Never an address.
int SaveLegacy_NameForOffset( int offset, const char **name );

// both in one: `ofs:N` -> name
int SaveLegacy_Resolve( const char *str, const char **name );

#endif // SAVE_LEGACY_H
