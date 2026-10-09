/*
xbox_bmodel_origin.h - Mod_FindModelOrigin's answers for every submodel from one parse
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
Resonance3D (docs/r3d/divergences.md #163). Mod_SetupSubmodels calls
Mod_FindModelOrigin once per submodel "*1" .. "*N-1"
(engine/common/mod_bmodel.c, :2157 in the reference), and each call
re-parses the map's entity lump from the start until the entity whose
"model" key names that submodel (:1334-1384 in the reference): quadratic
in the size of the map. On c1a0c (126 models, a 78 KB lump) that is
4.5 MB of COM_ParseFile per load, c1a1f 6.6 MB; the xemu sampler put
about 45% of the brush-model load (sv_world) in that parse.

This file parses the lump once and records, for every submodel, the
value Mod_FindModelOrigin would leave in its `origin` argument:

- Each entity starts by clearing the vector, then applies every "origin"
  key in order with Q_atov; the reference stops after the closing brace of
  the first entity with a matching "model" key, so the answer for a
  submodel is the vector at the end of that entity.
- A submodel no entity names leaves the vector as the end of the LAST
  entity left it (the reference parses to the end of the lump).
- A non-null `origin` (the BSP's own dmodel_t origin), or a lump with no
  entities, leaves `origin` untouched.

The reference Host_Errors on a malformed lump (a token other than '{'
where an entity starts, EOF inside an entity, '}' as a value), but only
when a call reaches the bad spot. When the single parse meets any of
those, the build fails and the caller runs the reference function for
every submodel, so a malformed lump errors (or not) exactly as before.

The tokens are produced by the same COM_ParseFile calls into a buffer of
the same size as the reference's, and compared with the same Q_stricmp,
so the parse cannot differ; tools/host-tests/xbox_bmodel_origin_host_test.c
checks every answer against a copy of the reference function.
*/

#ifndef XBOX_BMODEL_ORIGIN_H
#define XBOX_BMODEL_ORIGIN_H

#include "xash3d_types.h"

typedef struct xbox_bmodel_origins_s
{
	int     count;      // the submodels "*1" .. "*count-1" are covered
	vec3_t *origins;    // [count], the answer for each submodel an entity names
	int    *state;      // [count], internal: unseen / pending / resolved
	int     entities;   // number of entities in the lump
	vec3_t  last;       // the vector at the end of the last entity
} xbox_bmodel_origins_t;

/* Parse `entities` once. `origins` and `state` are caller storage of
   `count` entries each. Returns false when `entities` is NULL or the lump
   is malformed anywhere: then the caller must call the reference
   Mod_FindModelOrigin for each submodel instead. */
qboolean Xbox_BmodelOrigins_Build( xbox_bmodel_origins_t *idx, const char *entities,
	int count, vec3_t *origins, int *state );

/* Mod_FindModelOrigin( entities, "*submodel", origin ) for a lump the
   build accepted: leaves `origin` exactly as the reference would. */
void Xbox_BmodelOrigins_Find( const xbox_bmodel_origins_t *idx, int submodel, vec3_t origin );

#endif // XBOX_BMODEL_ORIGIN_H
