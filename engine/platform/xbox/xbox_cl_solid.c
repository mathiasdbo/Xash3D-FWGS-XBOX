/*
xbox_cl_solid.c - skip a CL_SetSolidEntities that would rebuild the same lists
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

/* Resonance3D (docs/r3d/divergences.md #156): see xbox_cl_solid.h. */

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "client.h"
#include "xbox_cl_solid.h"
#include "xbox_pm_cull.h" /* xbox_model_generation */

typedef struct xbox_cl_solid_key_s
{
	const playermove_t *pmove;
	const model_t *world;
	const entity_state_t *ring;
	uint32_t model_generation;
	int parsecount;
	int frame_valid;
	int first_entity;
	int num_entities;
	int num_client_entities;
	int playernum;
	int maxclients;
} xbox_cl_solid_key_t;

static struct
{
	qboolean valid;
	qboolean settled; /* a rebuild now would write what is there */
	xbox_cl_solid_key_t key;
	int numphysent, numvisent, nummoveent;
} built;

static void CL_SolidKey( xbox_cl_solid_key_t *k )
{
	const frame_t *frame = &cl.frames[cl.parsecountmod];

	memset( k, 0, sizeof( *k ));
	k->pmove = clgame.pmove;
	k->world = cl.worldmodel;
	k->ring = cls.packet_entities;
	k->model_generation = xbox_model_generation;
	k->parsecount = cl.parsecount;
	k->frame_valid = frame->valid;
	k->first_entity = frame->first_entity;
	k->num_entities = frame->num_entities;
	k->num_client_entities = cls.num_client_entities;
	k->playernum = cl.playernum;
	k->maxclients = cl.maxclients;
}

/* CL_CopyEntityToPhysEnt's choice for a state that is not SOLID_BBOX
   (cl_pmove.c:339-344 in the reference), made again with pe->solid as
   the rebuild left it, state->solid (:357) - what the next rebuild
   would choose. The rebuild put the state's model in exactly one of
   pe->model and pe->studiomodel (CL_AddLinksToPmove passes no state
   without a model); the world's slot (SOLID_BSP, pe->model) keeps its
   choice. A SOLID_BBOX state's choice reads only the state and the
   model, so it is already what a rebuild would make. */
static void CL_SolidSettle( physent_t *pe, int count )
{
	int i;

	for( i = 0; i < count; i++, pe++ )
	{
		model_t *mod;

		if( pe->solid == SOLID_BBOX )
			continue;

		mod = pe->model ? pe->model : pe->studiomodel;
		if( pe->solid != SOLID_BSP && ( mod != NULL ) && ( mod->type == mod_studio ))
		{
			pe->model = NULL;
			pe->studiomodel = mod;
		}
		else
		{
			pe->model = mod;
			pe->studiomodel = NULL;
		}
	}
}

int Xbox_CL_SolidReuse( void )
{
	playermove_t *pm = clgame.pmove;
	xbox_cl_solid_key_t k;

	if( !built.valid || !pm )
		return 0;
	if( pm->numphysent != built.numphysent || pm->numvisent != built.numvisent || pm->nummoveent != built.nummoveent )
		return 0;
	CL_SolidKey( &k );
	if( memcmp( &k, &built.key, sizeof( k )))
		return 0;

	if( !built.settled )
	{
		CL_SolidSettle( pm->physents, pm->numphysent );
		CL_SolidSettle( pm->visents, pm->numvisent );
		CL_SolidSettle( pm->moveents, pm->nummoveent );
		built.settled = true;
	}
	return 1;
}

void Xbox_CL_SolidBuilt( void )
{
	CL_SolidKey( &built.key );
	built.numphysent = clgame.pmove->numphysent;
	built.numvisent = clgame.pmove->numvisent;
	built.nummoveent = clgame.pmove->nummoveent;
	built.valid = true;
	built.settled = false;
}

void Xbox_CL_SolidInvalidate( void )
{
	built.valid = false;
}

void Xbox_CL_SolidPlayers( void )
{
	if( clgame.pmove && clgame.pmove->numphysent < built.numphysent )
		built.valid = false;
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
