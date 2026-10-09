/*
xbox_wad_search.h - the lumps an FS_Search of a WAD can match
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
Resonance3D (docs/r3d/divergences.md #167). An image asked for without an
extension is looked up with FS_Search( "<name>.*" ) (img_main.c,
Image_ProbeLoad2), and FS_Search_WAD (filesystem/wad.c:475-558 in the
reference) runs matchpattern over every lump of every mounted WAD: 3,661
lumps over the nine retail WADs, for each of c1a0c's 22 decal textures,
43 ms of CPU in xemu out of the 91 ms the decals' textures cost.

A WAD's lump table is already sorted for W_FindLump's binary search: by
Q_stricmp of the name, then by type (W_AddFileToWad, wad.c:184-220). When
the pattern has no wildcard ('*' or '?', the only characters
matchpattern_with_separator treats specially, public/crtlib.c:889-944 in
the reference) and no
lump name of the WAD contains a separator ('/', '\\' or ':', which make
FS_Search_WAD also try the name's leading components, wad.c:543-555),
matchpattern( name, pattern, true ) is true exactly when the two strings
are equal with A-Z folded to a-z - the equality Q_stricmp tests - so the
lumps it can accept are one contiguous run of the sorted table. The
caller's loop then visits that run only, in the same index order, with
its own body unchanged (type filter, matchpattern, the list append), so
it appends the same strings in the same order. Otherwise the run is the
whole table, as before.

Only one direction is needed: every name matchpattern accepts must be
Q_stricmp-equal to the pattern. The table is sorted and searched with the
same Q_stricmp, so one that folded more than A-Z would only widen the
run, which the unchanged body filters; one that folded less would break
it.
*/
#ifndef XBOX_WAD_SEARCH_H
#define XBOX_WAD_SEARCH_H

#include "wadfile.h"

/* Non-zero when any of the `numlumps` names contains '/', '\\' or ':' -
   computed once, when the WAD is opened (its table never changes after
   W_Open). */
int R3D_WadHasSeparatorNames( const dlumpinfo_t *lumps, int numlumps );

/* The run [*first, *end) of `lumps` (sorted as W_AddFileToWad sorts
   them) that FS_Search_WAD's loop must visit for `wadpattern` (the
   pattern's file base, as that function computes it): the lumps whose
   name Q_stricmp's equal to it, or [0, numlumps) when the pattern has a
   wildcard or `separator_names` is set. */
void R3D_WadSearchRange( const dlumpinfo_t *lumps, int numlumps, const char *wadpattern, int separator_names,
	int *first, int *end );

#endif /* XBOX_WAD_SEARCH_H */
