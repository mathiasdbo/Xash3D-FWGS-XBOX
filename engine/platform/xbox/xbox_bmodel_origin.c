/*
xbox_bmodel_origin.c - Mod_FindModelOrigin's answers for every submodel from one parse
Resonance3D (docs/r3d/divergences.md #163); see xbox_bmodel_origin.h
*/
#include <string.h>
#include "crtlib.h"
#include "xbox_bmodel_origin.h"

// state[] values; a pending submodel holds the next pending one, or PENDING_END
#define ORIGIN_UNSEEN    (-3)
#define ORIGIN_RESOLVED  (-2)
#define ORIGIN_PEND_END  (-1)

/*
The submodel a "model" value names, or -1. The reference matches
Q_stricmp( "*%i", token ) == 0 (Mod_SetupSubmodels builds the name with
Q_snprintf, mod_bmodel.c:2156 in the reference); case folding touches
neither '*' nor digits, so a token that matches is "*" and the decimal
digits of the number, byte for byte, and parsing them finds that number.
The candidate is then confirmed with the reference's own comparison, so
"*01" or "*+1" (which the reference never matches) are rejected here too.
*/
static int Xbox_BmodelOrigins_Submodel( const char *token, int count )
{
	char canon[16];
	int n = 0, digits = 0;

	if( token[0] != '*' )
		return -1;

	for( const char *p = token + 1; *p; p++ )
	{
		if( *p < '0' || *p > '9' || ++digits > 9 )
			return -1;
		n = n * 10 + ( *p - '0' );
	}

	if( digits == 0 || n < 1 || n >= count )
		return -1;

	Q_snprintf( canon, sizeof( canon ), "*%i", n );
	return Q_stricmp( canon, token ) ? -1 : n;
}

qboolean Xbox_BmodelOrigins_Build( xbox_bmodel_origins_t *idx, const char *entities,
	int count, vec3_t *origins, int *state )
{
	// the reference's own locals, sizes included (mod_bmodel.c:1342-1344)
	char   *pfile = (char *)entities;
	string keyname;
	char   token[2048];
	vec3_t cur;

	memset( idx, 0, sizeof( *idx ));
	idx->count = count;
	idx->origins = origins;
	idx->state = state;

	if( !entities )
		return false;

	for( int i = 0; i < count; i++ )
		state[i] = ORIGIN_UNSEEN;

	// the reference's loop (mod_bmodel.c:1348-1383), every Host_Error a failure
	while(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) != NULL )
	{
		int pending = ORIGIN_PEND_END;

		if( token[0] != '{' )
			return false;

		cur[0] = cur[1] = cur[2] = 0.0f; // VectorClear( origin )

		while( 1 )
		{
			// parse key
			if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
				return false;
			if( token[0] == '}' ) break; // end of desc

			Q_strncpy( keyname, token, sizeof( keyname ));

			// parse value
			if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
				return false;

			if( token[0] == '}' )
				return false;

			if( !Q_stricmp( keyname, "model" ))
			{
				int n = Xbox_BmodelOrigins_Submodel( token, count );

				// only the first entity naming a submodel answers for it
				if( n > 0 && state[n] == ORIGIN_UNSEEN )
				{
					state[n] = pending;
					pending = n;
				}
			}

			if( !Q_stricmp( keyname, "origin" ))
				Q_atov( cur, token, 3 );
		}

		// the reference breaks here for these submodels: the vector is final
		while( pending != ORIGIN_PEND_END )
		{
			int next = state[pending];

			memcpy( origins[pending], cur, sizeof( vec3_t ));
			state[pending] = ORIGIN_RESOLVED;
			pending = next;
		}

		memcpy( idx->last, cur, sizeof( vec3_t ));
		idx->entities++;
	}

	return true;
}

void Xbox_BmodelOrigins_Find( const xbox_bmodel_origins_t *idx, int submodel, vec3_t origin )
{
	// the reference's early outs (mod_bmodel.c:1336-1340); "*%i" is never empty
	if( !origin || !( origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f ))
		return;

	if( submodel > 0 && submodel < idx->count && idx->state[submodel] == ORIGIN_RESOLVED )
		memcpy( origin, idx->origins[submodel], sizeof( vec3_t ));
	else if( idx->entities > 0 )
		memcpy( origin, idx->last, sizeof( vec3_t )); // no entity names it: parsed to the end
}
