/*
xbox_wad_search.c - the lumps an FS_Search of a WAD can match
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

/* docs/r3d/divergences.md #167; see xbox_wad_search.h for why the run is
   exact. */
#include <string.h>
#include "crtlib.h"
#include "xbox_wad_search.h"

int R3D_WadHasSeparatorNames( const dlumpinfo_t *lumps, int numlumps )
{
	int i;

	for( i = 0; i < numlumps; i++ )
	{
		/* the name as FS_Search_WAD copies it: up to its NUL, which
		   W_Open's Q_strnlwr always writes inside the 16 bytes */
		const char *name = lumps[i].name;
		size_t j;

		for( j = 0; j < sizeof( lumps[i].name ) && name[j]; j++ )
		{
			if( name[j] == '/' || name[j] == '\\' || name[j] == ':' )
				return 1;
		}
	}

	return 0;
}

void R3D_WadSearchRange( const dlumpinfo_t *lumps, int numlumps, const char *wadpattern, int separator_names,
	int *first, int *end )
{
	int lo, hi;

	*first = 0;
	*end = numlumps;

	if( numlumps <= 0 || separator_names || strpbrk( wadpattern, "*?" ))
		return;

	/* the first name not below the pattern ... */
	lo = 0;
	hi = numlumps;
	while( lo < hi )
	{
		const int mid = lo + ( hi - lo ) / 2;

		if( Q_stricmp( lumps[mid].name, wadpattern ) < 0 )
			lo = mid + 1;
		else hi = mid;
	}
	*first = lo;

	/* ... and the first one above it */
	hi = numlumps;
	while( lo < hi )
	{
		const int mid = lo + ( hi - lo ) / 2;

		if( Q_stricmp( lumps[mid].name, wadpattern ) <= 0 )
			lo = mid + 1;
		else hi = mid;
	}
	*end = lo;
}
