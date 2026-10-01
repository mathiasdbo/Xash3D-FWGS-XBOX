/*
r_decals.c - X9-X item 14 step 12: decals (bullet holes, blood, scorch
marks, sprayed logos) for ref/nv2a. A port of ref/gl's gl_decals.c (root
clone), function for function: the pool (gDecalPool, R_DecalAlloc,
R_DecalUnlink), placement (R_DecalShoot, R_DecalNode, R_DecalNodeSurfaces,
R_DecalSurface, R_DecalCreate, R_DecalIntersect), the clipped polygon
(R_DecalVertsClip, R_DecalCreatePoly, R_DecalSetupVerts), serialization
for savegames (R_CreateDecalList, R_DecalUnProject, DecalListAdd), removal
(R_DecalRemoveAll, R_EntityRemoveDecals, R_ClearAllDecals) and the draw
order (DrawDecalsBatch, DrawSurfaceDecals, DrawSingleDecal). The GPU side
of the draw is r_context.c's (nv2a_decal_gpu_*): the decal texture on
stage 0, the face's lightmap from the baked atlas on stage 1, the world's
vertex program, a combiner that multiplies in the entity's colour
(ps_decal), and the polygon offset gl_polyoffset asks for.

How it fits together, as in the reference:
- The engine calls R_DecalShoot with a decal texture (loaded from
  decals.wad on first use, CL_DecalIndex) and a point on a brush model.
  R_DecalNode walks the model's BSP to every face within the decal's
  radius; R_DecalSurface keeps the ones the decal's rectangle reaches, and
  R_DecalCreate clips the face's polygon to the decal (nv2a_decal_util.c)
  and links the decal to the face (msurface_t::pdecals).
- A decal on a brush entity is stored in the entity's model space, so it
  moves with the door or train it was shot on.
- Each frame the decals of the faces the world pass drew are drawn after
  the world (R_DecalDrawWorld), a brush entity's after its own faces
  (R_DecalDrawBrushEntity), under the entity's render mode.
- Saving a game asks R_CreateDecalList for every decal; loading it shoots
  them again (sv_save.c, SV_RestoreCustomDecal).

Where it departs from ref/gl (docs/r3d/divergences.md #139):
- The face polygon. ref/gl clips against msurface_t::polys, which
  GL_BuildPolygonFromSurface builds for every face at load (gl_rsurf.c:381:
  the face's edge loop, gl_keeptjunctions 1 keeping every vertex). This
  renderer draws faces from the bake and never builds them, so the loop is
  read from the model's edges when a decal is placed
  (nv2a_decal_surface_poly, R_GetEdgePosition's own walk) - the same points.
  msurface_t::polys stays the warp block r_warp.c keeps on water faces,
  which never take decals.
- No stencil: ref/gl draws decals on a '{' (SURF_TRANSPARENT) face only
  with a stencil buffer (gl_decals.c:656, 877-926), masked to the face's
  texture alpha. That path is not ported, so such faces take no decals,
  as in ref/gl on a context without stencil.
- R_ClearDecals frees each decal's polygon (the reference's memset leaves
  them in r_temppool until shutdown, a leak per map).
- A face with more edges than the clip buffers hold takes no decal (the
  reference overruns g_DecalClipVerts, nv2a_decal_util.h).
- Draw order: world decals are drawn surface by surface in pool order
  rather than texture-chain order (each surface's own list keeps its
  order, which is what decides overlaps), and a translucent brush entity's
  decals after all of its faces rather than after each face.
- Lighting: the decal is modulated by the face's lightmap in one pass
  (ps_decal) instead of by R_BlendLightmaps' second pass over it
  (gl_rsurf.c:3711-3713) - the same product below saturation, as for the
  world itself (ref/gl clamps once after its framebuffer multiply, here the
  decal and the face each clamp before the blend). This follows ref/gl's
  default path (gl_vbo 0, gl_opengl.c:34), not R_DrawStaticDecals.
- Only the map's own brush models take decals (their surfaces are the
  world's, nv2a_decal_surface_model): ref/gl also decals a brush model
  loaded from another .bsp, which this renderer does not draw at all.
- A face with no mextrasurf_t samples lightmap luxel (0.5, 0.5)
  (R_DecalVertsLight); the reference has no such case to handle.
- R_DecalSetupVerts hands out the stored mesh instead of copying it into
  g_DecalClipVerts (gl_decals.c:795-813): only the stencil path, not
  ported, writes the copy.
- Texture indices are bounded by this renderer's table (NV2A_MAX_TEXTURES,
  1024) where ref/gl uses MAX_TEXTURES (8192): the same check, its table.
- Depth: ref/gl draws a brush entity's faces with their own offset
  (gl_polyoffset_bmodels) and its decals with gl_polyoffset, sharing the
  units term; here the faces have none and the decals a constant bias of
  -gl_polyoffset without the slope term (r_context.c, the decal section).
*/
#include <string.h>
#include <stdlib.h>

#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR */
#include "crtlib.h"      /* COM_FileBase, Q_strcmp */
#include "nv2a_texture.h"
#include "nv2a_decal_util.h"
#include "r_studio_bridge.h"
#include "r_decals.h"
#include "nv2a_ft.h" /* frame-time work counters (divergences.md #141) */

#define DECAL_OVERLAP_DISTANCE	2
#define DECAL_DISTANCE		4	// too big values produce more clipped polygons
#define MAX_DECALCLIPVERT		NV2A_DECAL_MAX_CLIPVERT	// produced vertexes of fragmented decal (gl_decals.c:20)

// empirically determined constants for minimizing overalpping decals
#define MAX_OVERLAP_DECALS		6
#define DECAL_OVERLAP_DIST		8
#define MIN_DECAL_SCALE		0.01f
#define MAX_DECAL_SCALE		16.0f

STATIC_ASSERT( VERTEXSIZE == NV2A_DECAL_VERTEXSIZE, "nv2a_decal_util's vertex layout is com_model.h's VERTEXSIZE" );

#define WORLDMODEL ( gp_cl->models[1] )
#define CL_GetEntityByIndex( i ) nv2a_get_entity_by_index( i )

static inline model_t *CL_ModelHandle( int index )
{
	if( unlikely( index < 0 || index >= gp_cl->nummodels ))
		return NULL;
	return gp_cl->models[index];
}

// This structure contains the information used to create new decals
typedef struct
{
	vec3_t		m_Position;	// world coordinates of the decal center
	model_t		*m_pModel;	// the model the decal is going to be applied in
	int		m_iTexture;	// The decal material
	int		m_Size;		// Size of the decal (in world coords)
	int		m_Flags;
	int		m_Entity;		// Entity the decal is applied to.
	float		m_scale;
	int		m_decalWidth;
	int		m_decalHeight;
	vec3_t		m_Basis[3];
} decalinfo_t;

static float	g_DecalClipVerts[MAX_DECALCLIPVERT][VERTEXSIZE];
static float	g_DecalClipVerts2[MAX_DECALCLIPVERT][VERTEXSIZE];

decal_t	gDecalPool[MAX_RENDER_DECALS];
static int	gDecalCount;

nv2a_decal_stats_t nv2a_decal_stats;

/* gl_polyoffset, gl_opengl.c:15 - "polygon offset for decals". */
CVAR_DEFINE_AUTO( gl_polyoffset, "2", FCVAR_GLCONFIG, "polygon offset for decals" );
static qboolean nv2a_decal_cvars_registered;

/* tr.draw_decals / tr.num_draw_decals (gl_local.h:198): the surfaces whose
   decals DrawDecalsBatch draws. Every decal is on one surface, so the pool
   size bounds it (ref/gl: MAX_DECAL_SURFS, 4096). */
static msurface_t *nv2a_draw_decals[MAX_RENDER_DECALS];
static int         nv2a_num_draw_decals;

/* The surfaces a non-kRenderNormal brush entity's faces handed
   DrawSurfaceDecals, with their CULL_BACKSIDE flag (reverse). */
static msurface_t *nv2a_single_decals[MAX_RENDER_DECALS];
static qboolean    nv2a_single_reverse[MAX_RENDER_DECALS];
static int         nv2a_num_single_decals;

static qboolean nv2a_decal_warned_verts;

void R_ClearDecals( void )
{
	int i;

	/* Divergence: free the polygons the memset below would strand (the
	   reference leaks them into r_temppool until shutdown). psurface is
	   not followed: the surfaces may already be gone with the old map. */
	for( i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		if( gDecalPool[i].polys )
			Mem_Free( gDecalPool[i].polys );
	}

	memset( gDecalPool, 0, sizeof( gDecalPool ));
	gDecalCount = 0;
	nv2a_num_draw_decals = 0;
	nv2a_num_single_decals = 0;
}

void R_DecalClearSurfaceChains( void )
{
	int i;

	/* GL_BuildLightmaps' "clearing all decal chains" (gl_rsurf.c:3932-3944),
	   which R_NewMap runs right after R_ClearDecals (gl_context.c:403,429):
	   the engine keeps the same world model_t when the same map loads
	   again (a quickload, Mod_LoadWorld's "already loaded?",
	   model.c:502), so its surfaces would otherwise still point at pool
	   entries R_ClearDecals just zeroed. */
	for( i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = CL_ModelHandle( i + 1 );
		int j;

		if( m == NULL )
			continue;

		if( m->name[0] == '*' || m->type != mod_brush || !m->surfaces )
			continue;

		for( j = 0; j < m->numsurfaces; j++ )
		{
			// clearing all decal chains
			m->surfaces[j].pdecals = NULL;
		}
	}
}

float R_DecalPolyOffset( void )
{
	return gl_polyoffset.value;
}

void R_DecalInit( void )
{
	if( !nv2a_decal_cvars_registered )
	{
		gEngfuncs.Cvar_RegisterVariable( &gl_polyoffset );
		nv2a_decal_cvars_registered = true;
	}

	/* R_Init's R_ClearDecals (gl_opengl.c:1282) - the pool is static, so
	   nothing is left from an earlier R_Init to free. */
	memset( gDecalPool, 0, sizeof( gDecalPool ));
	gDecalCount = 0;
	nv2a_num_draw_decals = 0;
	nv2a_num_single_decals = 0;
}

// unlink pdecal from any surface it's attached to
static void R_DecalUnlink( decal_t *pdecal )
{
	if( pdecal->psurface )
	{
		if( pdecal->psurface->pdecals == pdecal )
		{
			pdecal->psurface->pdecals = pdecal->pnext;
		}
		else
		{
			decal_t *tmp = pdecal->psurface->pdecals;
			if( !tmp ) gEngfuncs.Host_Error( "%s: bad decal list\n", __func__ );

			while( tmp->pnext )
			{
				if( tmp->pnext == pdecal )
				{
					tmp->pnext = pdecal->pnext;
					break;
				}
				tmp = tmp->pnext;
			}
		}
	}

	if( pdecal->polys )
		Mem_Free( pdecal->polys );

	pdecal->psurface = NULL;
	pdecal->polys = NULL;
}

// Just reuse next decal in list
// A decal that spans multiple surfaces will use multiple decal_t pool entries,
// as each surface needs it's own.
static decal_t *R_DecalAlloc( decal_t *pdecal )
{
	int	limit = MAX_RENDER_DECALS;

	if( r_decals->value < limit )
		limit = r_decals->value;

	if( !limit ) return NULL;

	if( !pdecal )
	{
		int	count = 0;

		// check for the odd possiblity of infinte loop
		do
		{
			if( gDecalCount >= limit )
				gDecalCount = 0;

			pdecal = &gDecalPool[gDecalCount]; // reuse next decal
			gDecalCount++;
			count++;
		} while( FBitSet( pdecal->flags, FDECAL_PERMANENT ) && count < limit );
	}

	// if decal is already linked to a surface, unlink it.
	R_DecalUnlink( pdecal );

	return pdecal;
}

//-----------------------------------------------------------------------------
// find decal image and grab size from it
//-----------------------------------------------------------------------------
static void R_GetDecalDimensions( int texture, int *width, int *height )
{
	const nv2a_texture_t *glt;

	if( width ) *width = 1;	// to avoid divide by zero
	if( height ) *height = 1;

	/* R_GetTextureParms (gl_draw.c:23): the source size, which
	   R_OverrideTextureSourceSize can replace for a texture replacement. */
	glt = nv2a_tex_get( (unsigned int)texture );
	if( width ) *width = glt->srcWidth;
	if( height ) *height = glt->srcHeight;
}

//-----------------------------------------------------------------------------
// compute the decal basis based on surface normal
//-----------------------------------------------------------------------------
static void R_DecalComputeBasis( msurface_t *surf, int flags, vec3_t textureSpaceBasis[3] )
{
	vec3_t	surfaceNormal;

	// setup normal
	if( surf->flags & SURF_PLANEBACK )
		VectorNegate( surf->plane->normal, surfaceNormal );
	else VectorCopy( surf->plane->normal, surfaceNormal );

	VectorNormalize2( surfaceNormal, textureSpaceBasis[2] );
	/* gl_decals.c:154-175: an #if 0'd FDECAL_CUSTOM basis, not ported. */
	VectorNormalize2( surf->texinfo->vecs[0], textureSpaceBasis[0] );
	VectorNormalize2( surf->texinfo->vecs[1], textureSpaceBasis[1] );
}

static void R_SetupDecalTextureSpaceBasis( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	int	width, height;

	// Compute the non-scaled decal basis
	R_DecalComputeBasis( surf, pDecal->flags, textureSpaceBasis );
	R_GetDecalDimensions( texture, &width, &height );

	// world width of decal = ptexture->width / pDecal->scale
	// world height of decal = ptexture->height / pDecal->scale
	// scale is inverse, scales world space to decal u/v space [0,1]
	// OPTIMIZE: Get rid of these divides
	decalWorldScale[0] = (float)pDecal->scale / width;
	decalWorldScale[1] = (float)pDecal->scale / height;

	VectorScale( textureSpaceBasis[0], decalWorldScale[0], textureSpaceBasis[0] );
	VectorScale( textureSpaceBasis[1], decalWorldScale[1], textureSpaceBasis[1] );
}

/* R_GetEdgePosition (gl_rsurf.c:163) for every edge of the face: the
   polygon GL_BuildPolygonFromSurface builds (gl_rsurf.c:381, with
   gl_keeptjunctions 1, gl_opengl.c:13, so no vertex is dropped). Returns
   the vertex count, or 0 for a face the clip cannot take. */
static int nv2a_decal_surface_poly( const model_t *mod, const msurface_t *fa, float *verts )
{
	int i;

	if( !mod || !mod->surfedges || !mod->vertexes || fa->numedges < 3 )
		return 0;

	if( fa->numedges > NV2A_DECAL_MAX_INPUT_VERTS )
	{
		if( !nv2a_decal_warned_verts )
		{
			gEngfuncs.Con_Printf( S_WARN "ref_nv2a: a face with %d edges takes no decals (limit %d)\n",
				fa->numedges, NV2A_DECAL_MAX_INPUT_VERTS );
			nv2a_decal_warned_verts = true;
		}
		return 0;
	}

	for( i = 0; i < fa->numedges; i++, verts += VERTEXSIZE )
	{
		const int lindex = mod->surfedges[fa->firstedge + i];

		if( FBitSet( mod->flags, MODEL_QBSP2 ))
		{
			const medge32_t *pedges = mod->edges32;

			if( lindex > 0 )
				VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, verts );
			else
				VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, verts );
		}
		else
		{
			const medge16_t *pedges = mod->edges16;

			if( lindex > 0 )
				VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, verts );
			else
				VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, verts );
		}
	}

	return fa->numedges;
}

/* The model whose surface array holds `surf`: the world's, which every
   inline brush model of the map shares (Mod_SetupSubmodels). */
static const model_t *nv2a_decal_surface_model( const msurface_t *surf )
{
	const model_t *world = WORLDMODEL;

	if( world && world->surfaces && surf >= world->surfaces && surf < world->surfaces + world->numsurfaces )
		return world;
	return NULL;
}

// Build the initial list of vertices from the surface verts into the global array, 'verts'.
static int R_SetupDecalVertsForMSurface( decal_t *pDecal, msurface_t *surf, vec3_t textureSpaceBasis[3], float *verts )
{
	const int numverts = nv2a_decal_surface_poly( nv2a_decal_surface_model( surf ), surf, verts );
	int i;

	for( i = 0; i < numverts; i++, verts += VERTEXSIZE )
	{
		// VectorCopy( v, verts ); copy model space coordinates - nv2a_decal_surface_poly wrote them
		verts[3] = DotProduct( verts, textureSpaceBasis[0] ) - pDecal->dx + 0.5f;
		verts[4] = DotProduct( verts, textureSpaceBasis[1] ) - pDecal->dy + 0.5f;
		verts[5] = verts[6] = 0.0f;
	}

	return numverts;
}

// Figure out where the decal maps onto the surface.
static void R_SetupDecalClip( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	R_SetupDecalTextureSpaceBasis( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	// Generate texture coordinates for each vertex in decal s,t space
	// probably should pre-generate this, store it and use it for decal-decal collisions
	// as in R_DecalsIntersect()
	pDecal->dx = DotProduct( pDecal->position, textureSpaceBasis[0] );
	pDecal->dy = DotProduct( pDecal->position, textureSpaceBasis[1] );
}

//-----------------------------------------------------------------------------
// Generate clipped vertex list for decal pdecal projected onto polygon psurf
//-----------------------------------------------------------------------------
static float *R_DecalVertsClip( decal_t *pDecal, msurface_t *surf, int texture, int *pVertCount )
{
	float	decalWorldScale[2];
	vec3_t	textureSpaceBasis[3];
	int	numverts;

	// figure out where the decal maps onto the surface.
	R_SetupDecalClip( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	// build the initial list of vertices from the surface verts.
	numverts = R_SetupDecalVertsForMSurface( pDecal, surf, textureSpaceBasis, g_DecalClipVerts[0] );

	/* R_DoDecalSHClip (gl_decals.c:349), nv2a_decal_util.c */
	numverts = nv2a_decal_clip_polygon( g_DecalClipVerts, g_DecalClipVerts2, numverts );
	if( pVertCount )
		*pVertCount = numverts;

	return g_DecalClipVerts[0];
}

// Generate lighting coordinates at each vertex for decal vertices v[] on surface psurf
static void R_DecalVertsLight( float *v, msurface_t *surf, int vertCount )
{
	float sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	int j;

	if( !surf->info )
	{
		for( j = 0; j < vertCount; j++, v += VERTEXSIZE )
			v[5] = v[6] = 0.5f;
		return;
	}

	for( j = 0; j < vertCount; j++, v += VERTEXSIZE )
	{
		// lightmap texture coordinates - R_LightmapCoord (gl_rsurf.c:130)
		// within the face's block; r_context.c adds the block's origin
		nv2a_decal_lightmap_luxel( v, (const float (*)[4])surf->info->lmvecs, surf->info->lightmapmins, sample_size, &v[5] );
	}
}

// Check for intersecting decals on this surface
static decal_t *R_DecalIntersect( decalinfo_t *decalinfo, msurface_t *surf, int *pcount )
{
	decal_t *plast = NULL;
	float lastArea = 2;
	int texture;
	int mapSize[2];
	vec3_t decalExtents[2];
	decal_t *pDecal;

	*pcount = 0;

	// (Same as R_SetupDecalClip).
	texture = decalinfo->m_iTexture;

	// precalculate the extents of decalinfo's decal in world space.
	R_GetDecalDimensions( texture, &mapSize[0], &mapSize[1] );
	VectorScale( decalinfo->m_Basis[0], (( mapSize[0] / decalinfo->m_scale ) * 0.5f ), decalExtents[0] );
	VectorScale( decalinfo->m_Basis[1], (( mapSize[1] / decalinfo->m_scale ) * 0.5f ), decalExtents[1] );

	pDecal = surf->pdecals;

	while( pDecal )
	{
		texture = pDecal->texture;

		// Don't steal bigger decals and replace them with smaller decals
		// Don't steal permanent decals
		if( !FBitSet( pDecal->flags, FDECAL_PERMANENT ))
		{
			vec3_t	testBasis[3];
			vec3_t	testPosition[2];
			float	testWorldScale[2];
			vec2_t	vDecalMin, vDecalMax, vUnionMin, vUnionMax;

			R_SetupDecalTextureSpaceBasis( pDecal, surf, texture, testBasis, testWorldScale );

			VectorSubtract( decalinfo->m_Position, decalExtents[0], testPosition[0] );
			VectorSubtract( decalinfo->m_Position, decalExtents[1], testPosition[1] );

			// Here, we project the min and max extents of the decal that got passed in into
			// this decal's (pDecal's) [0,0,1,1] clip space, just like we would if we were
			// clipping a triangle into pDecal's clip space.
			vDecalMin[0] = DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f;
			vDecalMin[1] = DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f;

			VectorAdd( decalinfo->m_Position, decalExtents[0], testPosition[0] );
			VectorAdd( decalinfo->m_Position, decalExtents[1], testPosition[1] );

			vDecalMax[0] = DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f;
			vDecalMax[1] = DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f;

			// Now figure out the part of the projection that intersects pDecal's
			// clip box [0,0,1,1].
			vUnionMin[0] = Q_max( vDecalMin[0], 0 );
			vUnionMin[1] = Q_max( vDecalMin[1], 0 );
			vUnionMax[0] = Q_min( vDecalMax[0], 1 );
			vUnionMax[1] = Q_min( vDecalMax[1], 1 );

			if( vUnionMin[0] < 1 && vUnionMin[1] < 1 && vUnionMax[0] > 0 && vUnionMax[1] > 0 )
			{
				// Figure out how much of this intersects the (0,0) - (1,1) bbox.
				float	flArea = ( vUnionMax[0] - vUnionMin[1] ) * ( vUnionMax[1] - vUnionMin[1] ); /* sic, gl_decals.c:452 */

				if( flArea > 0.6f )
				{
					*pcount += 1;

					if( !plast || flArea <= lastArea )
					{
						plast = pDecal;
						lastArea =  flArea;
					}
				}
			}
		}
		pDecal = pDecal->pnext;
	}
	return plast;
}

// Build the vertex list for a decal on a surface and clip it to the surface.
// This is a template so it can work on world surfaces and dynamic displacement
// triangles the same way.
static float *R_DecalSetupVerts( decal_t *pDecal, msurface_t *surf, int texture, int *outCount )
{
	glpoly2_t *p = pDecal->polys;
	float *v;
	int count;

	if( p )
	{
		/* gl_decals.c:795-813 copies the mesh into g_DecalClipVerts; its
		   callers here only read it, so the mesh is handed out directly. */
		v = p->verts[0];
		count = p->numverts;
	}
	else
	{
		v = R_DecalVertsClip( pDecal, surf, texture, &count );
		R_DecalVertsLight( v, surf, count );
	}

	if( outCount )
		*outCount = count;

	return v;
}

/*
====================
R_DecalCreatePoly

creates mesh for decal on first rendering
====================
*/
static glpoly2_t *R_DecalCreatePoly( decalinfo_t *decalinfo, decal_t *pdecal, msurface_t *surf )
{
	int lnumverts, i;
	float *v;
	glpoly2_t *poly;

	if( pdecal->polys )	// already created?
		return pdecal->polys;

	v = R_DecalSetupVerts( pdecal, surf, pdecal->texture, &lnumverts );
	if( !lnumverts ) return NULL;	// probably this never happens

	// allocate glpoly
	// REFTODO: com_studiocache pool!
	poly = Mem_Calloc( r_temppool, sizeof( glpoly2_t ) + lnumverts * VERTEXSIZE * sizeof( float ));
	poly->next = pdecal->polys;
	poly->flags = surf->flags;
	pdecal->polys = poly;
	poly->numverts = lnumverts;

	for( i = 0; i < lnumverts; i++, v += VERTEXSIZE )
	{
		VectorCopy( v, poly->verts[i] );
		poly->verts[i][3] = v[3];
		poly->verts[i][4] = v[4];
		poly->verts[i][5] = v[5];
		poly->verts[i][6] = v[6];
	}

	return poly;
}

// Add the decal to the surface's list of decals.
static void R_AddDecalToSurface( decal_t *pdecal, msurface_t *surf, decalinfo_t *decalinfo )
{
	decal_t *pold;

	pdecal->pnext = NULL;
	pold = surf->pdecals;

	if( pold )
	{
		while( pold->pnext )
			pold = pold->pnext;
		pold->pnext = pdecal;
	}
	else
	{
		surf->pdecals = pdecal;
	}

	// tag surface
	pdecal->psurface = surf;

	// at this point decal are linked with surface
	// and will be culled, drawing and sorting
	// together with surface

	// alloc clipped poly for decal
	R_DecalCreatePoly( decalinfo, pdecal, surf );
	/* R_AddDecalVBO (gl_rsurf.c:2343): ref/gl's VBO copy - this renderer
	   streams the polygon when drawing (nv2a_decal_gpu_draw). */
}

static void R_DecalCreate( decalinfo_t *decalinfo, msurface_t *surf, float x, float y )
{
	int count, vertCount;
	decal_t *pold, *pdecal;

	if( !surf ) return;	// ???

	pold = R_DecalIntersect( decalinfo, surf, &count );
	if( count < MAX_OVERLAP_DECALS ) pold = NULL;

	pdecal = R_DecalAlloc( pold );
	if( !pdecal ) return; // r_decals == 0 ???

	pdecal->flags = decalinfo->m_Flags;

	VectorCopy( decalinfo->m_Position, pdecal->position );

	pdecal->dx = x;
	pdecal->dy = y;

	// set scaling
	pdecal->scale = decalinfo->m_scale;
	pdecal->entityIndex = decalinfo->m_Entity;
	pdecal->texture = decalinfo->m_iTexture;

	// check to see if the decal actually intersects the surface
	// if not, then remove the decal
	R_DecalVertsClip( pdecal, surf, decalinfo->m_iTexture, &vertCount );

	if( !vertCount )
	{
		R_DecalUnlink( pdecal );
		return;
	}

	// add to the surface's list
	R_AddDecalToSurface( pdecal, surf, decalinfo );
}

static void R_DecalSurface( msurface_t *surf, decalinfo_t *decalinfo )
{
	// get the texture associated with this surface
	mtexinfo_t *tex = surf->texinfo;
	connstate_t state = ENGINE_GET_PARM( PARM_CONNSTATE );
	vec4_t textureU, textureV;
	float s, t, w, h;

	// we in restore mode
	if( state == ca_connected || state == ca_validate )
	{
		// NOTE: we may have the decal on this surface that come from another level.
		// check duplicate with same position and texture
		decal_t *decal = surf->pdecals;
		while( decal != NULL )
		{
			if( VectorCompare( decal->position, decalinfo->m_Position ) && decal->texture == decalinfo->m_iTexture )
				return; // decal already exists, don't place it again
			decal = decal->pnext;
		}
	}

	Vector4Copy( tex->vecs[0], textureU );
	Vector4Copy( tex->vecs[1], textureV );

	// project decal center into the texture space of the surface
	s = DotProduct( decalinfo->m_Position, textureU ) + textureU[3] - surf->texturemins[0];
	t = DotProduct( decalinfo->m_Position, textureV ) + textureV[3] - surf->texturemins[1];

	// Determine the decal basis (measured in world space)
	// Note that the decal basis vectors 0 and 1 will always lie in the same
	// plane as the texture space basis vectorstextureVecsTexelsPerWorldUnits.
	R_DecalComputeBasis( surf, decalinfo->m_Flags, decalinfo->m_Basis );

	// Compute an effective width and height (axis aligned) in the parent texture space
	// How does this work? decalBasis[0] represents the u-direction (width)
	// of the decal measured in world space, decalBasis[1] represents the
	// v-direction (height) measured in world space.
	// textureVecsTexelsPerWorldUnits[0] represents the u direction of
	// the surface's texture space measured in world space (with the appropriate
	// scale factor folded in), and textureVecsTexelsPerWorldUnits[1]
	// represents the texture space v direction. We want to find the dimensions (w,h)
	// of a square measured in texture space, axis aligned to that coordinate system.
	// All we need to do is to find the components of the decal edge vectors
	// (decalWidth * decalBasis[0], decalHeight * decalBasis[1])
	// in texture coordinates:

	w = fabs( decalinfo->m_decalWidth  * DotProduct( textureU, decalinfo->m_Basis[0] )) +
	    fabs( decalinfo->m_decalHeight * DotProduct( textureU, decalinfo->m_Basis[1] ));

	h = fabs( decalinfo->m_decalWidth  * DotProduct( textureV, decalinfo->m_Basis[0] )) +
	    fabs( decalinfo->m_decalHeight * DotProduct( textureV, decalinfo->m_Basis[1] ));

	// move s,t to upper left corner
	s -= ( w * 0.5f );
	t -= ( h * 0.5f );

	// Is this rect within the surface? -- tex width & height are unsigned
	if( s <= -w || t <= -h || s > ( surf->extents[0] + w ) || t > ( surf->extents[1] + h ))
	{
		return; // nope
	}

	// stamp it
	R_DecalCreate( decalinfo, surf, s, t );
}

//-----------------------------------------------------------------------------
// iterate over all surfaces on a node, looking for surfaces to decal
//-----------------------------------------------------------------------------
static void R_DecalNodeSurfaces( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	// iterate over all surfaces in the node
	int firstsurface = node_firstsurface( node, model );
	int numsurfaces  = node_numsurfaces( node, model );
	msurface_t *surf = model->surfaces + firstsurface;
	int i;

	for( i = 0; i < numsurfaces; i++, surf++ )
	{
		// never apply decals on the water or sky surfaces
		if( surf->flags & ( SURF_DRAWTURB|SURF_DRAWSKY|SURF_CONVEYOR ))
			continue;

		/* glState.stencilEnabled is always false here (no stencil path,
		   this file's header). */
		if( surf->flags & SURF_TRANSPARENT )
			continue;

		R_DecalSurface( surf, decalinfo );
	}
}

//-----------------------------------------------------------------------------
// Recursive routine to find surface to apply a decal to.  World coordinates of
// the decal are passed in r_recalpos like the rest of the engine.  This should
// be called through R_DecalShoot()
//-----------------------------------------------------------------------------
static void R_DecalNode( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	mplane_t *splitplane;
	float dist;

	if( node->contents < 0 )
	{
		// hit a leaf
		return;
	}

	splitplane = node->plane;
	dist = DotProduct( decalinfo->m_Position, splitplane->normal ) - splitplane->dist;

	if( dist > decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
	}
	else if( dist < -decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
	else
	{
		if( dist < DECAL_DISTANCE && dist > -DECAL_DISTANCE )
			R_DecalNodeSurfaces( model, node, decalinfo );

		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
}

// Shoots a decal onto the surface of the BSP.  position is the center of the decal in world coords
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, float *pos, int flags, float scale )
{
	cl_entity_t *ent = NULL;
	model_t *model = NULL;
	decalinfo_t decalInfo;
	hull_t *hull;
	int width, height;

	/* MAX_TEXTURES in ref/gl: the size of the renderer's texture table,
	   here NV2A_MAX_TEXTURES (nv2a_texture.h). */
	if( textureIndex <= 0 || textureIndex >= NV2A_MAX_TEXTURES )
	{
		gEngfuncs.Con_Printf( S_ERROR "Decal has invalid texture!\n" );
		return;
	}

	if( entityIndex > 0 )
	{
		ent = CL_GetEntityByIndex( entityIndex );

		if( modelIndex > 0 ) model = CL_ModelHandle( modelIndex );
		else if( ent != NULL ) model = CL_ModelHandle( ent->curstate.modelindex );
		else return;
	}
	else if( modelIndex > 0 )
		model = CL_ModelHandle( modelIndex );
	else model = CL_ModelHandle( 1 );

	if( !model ) return;

	if( model->type != mod_brush )
	{
		gEngfuncs.Con_Reportf( S_ERROR "Decals must hit mod_brush!\n" );
		return;
	}

	decalInfo.m_pModel = model;
	hull = &model->hulls[0];	// always use #0 hull

	// NOTE: all the decals at 'first shoot' placed into local space of parent entity
	// and won't transform again on a next restore, levelchange etc
	if( ent && !FBitSet( flags, FDECAL_LOCAL_SPACE ))
	{
		vec3_t pos_l;

		// transform decal position in local bmodel space
		if( !VectorIsNull( ent->angles ))
		{
			matrix4x4 matrix;

			Matrix4x4_CreateFromEntity( matrix, ent->angles, ent->origin, 1.0f );
			Matrix4x4_VectorITransform( matrix, pos, pos_l );
		}
		else
		{
			VectorSubtract( pos, ent->origin, pos_l );
		}

		VectorCopy( pos_l, decalInfo.m_Position );
		// decal position moved into local space
		SetBits( flags, FDECAL_LOCAL_SPACE );
	}
	else
	{
		// already in local space
		VectorCopy( pos, decalInfo.m_Position );
	}

	// this decal must use landmark for correct transition
	// because their model exist only in world-space
	if( !FBitSet( model->flags, MODEL_HAS_ORIGIN ))
		SetBits( flags, FDECAL_USE_LANDMARK );

	// more state used by R_DecalNode()
	decalInfo.m_iTexture = textureIndex;
	decalInfo.m_Entity = entityIndex;
	decalInfo.m_Flags = flags;

	R_GetDecalDimensions( textureIndex, &width, &height );
	decalInfo.m_Size = width >> 1;
	if(( height >> 1 ) > decalInfo.m_Size )
		decalInfo.m_Size = height >> 1;

	decalInfo.m_scale = bound( MIN_DECAL_SCALE, scale, MAX_DECAL_SCALE );

	// compute the decal dimensions in world space
	decalInfo.m_decalWidth = width / decalInfo.m_scale;
	decalInfo.m_decalHeight = height / decalInfo.m_scale;

	R_DecalNode( model, &model->nodes[hull->firstclipnode], &decalInfo );
}

/*
=============================================================

  DRAWING - gl_decals.c:826-1021, the GPU side in r_context.c

=============================================================
*/

static void DrawSingleDecal( decal_t *pDecal, msurface_t *fa )
{
	int numVerts;
	float *v = R_DecalSetupVerts( pDecal, fa, pDecal->texture, &numVerts );
	if( !numVerts ) return;

	/* GL_Bind, the TF_PREMULTIPLIED blend and the GL_POLYGON
	   (gl_decals.c:832-846) */
	nv2a_decal_gpu_draw( pDecal->texture, v, numVerts, fa );
	nv2a_decal_stats.decals++;
	nv2a_decal_stats.verts += (unsigned int)numVerts;
	NV2A_FT_ADD( XBOX_FT_W_DECALS, 1 );
}

/* DrawSurfaceDecals( fa, single, reverse ), gl_decals.c:849, without the
   state push/pop of `single` (gl_decals.c:856-875, 953-978), which
   nv2a_decal_gpu_begin/end do once per entity instead of per surface (the
   state is the entity's, the same for each of its surfaces), and without
   the stencil path (this file's header). */
static void DrawSurfaceDecals( msurface_t *fa, qboolean reverse, const cl_entity_t *e )
{
	decal_t *p;

	if( !fa->pdecals ) return;

	nv2a_decal_stats.surfaces++;

	if( reverse && e->curstate.rendermode == kRenderTransTexture )
	{
		decal_t *list[MAX_RENDER_DECALS]; /* 1024 in the reference; no surface holds more than the pool */
		int count = 0, i;

		for( p = fa->pdecals; p && count < MAX_RENDER_DECALS; p = p->pnext )
			if( p->texture ) list[count++] = p;

		for( i = count - 1; i >= 0; i-- )
			DrawSingleDecal( list[i], fa );
	}
	else
	{
		for( p = fa->pdecals; p; p = p->pnext )
		{
			if( !p->texture ) continue;
			DrawSingleDecal( p, fa );
		}
	}
}

/* DrawDecalsBatch, gl_decals.c:981: the batched surfaces under `e`'s
   state, then the list is empty again. */
static void DrawDecalsBatch( const cl_entity_t *e, float blend )
{
	int i;

	if( !nv2a_num_draw_decals )
		return;

	if( nv2a_decal_gpu_begin( e, blend ))
	{
		for( i = 0; i < nv2a_num_draw_decals; i++ )
			DrawSurfaceDecals( nv2a_draw_decals[i], false, e );

		nv2a_decal_gpu_end();
	}

	nv2a_num_draw_decals = 0;
}

void R_DecalDrawWorld( void )
{
	const model_t *world = WORLDMODEL;
	const cl_entity_t *world_ent = CL_GetEntityByIndex( 0 );
	int i, in_use = 0;

	nv2a_num_draw_decals = 0;

	if( !world || !world_ent || ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
		return;

	/* R_RecursiveWorldNode -> R_RenderBrushPoly -> R_RenderDecalsForSurface
	   (gl_rsurf.c:1384) adds each drawn world surface with decals. Here the
	   pool is walked instead of the faces: a surface is added once, at its
	   list's head decal, if the world pass drew it (the header's draw
	   order note). */
	for( i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		decal_t *d = &gDecalPool[i];
		msurface_t *surf = d->psurface;

		if( !surf )
			continue;
		in_use++;

		if( surf->pdecals != d )
			continue;
		if( surf < world->surfaces + world->firstmodelsurface || surf >= world->surfaces + world->firstmodelsurface + world->nummodelsurfaces )
			continue; /* a brush entity's surface: R_DecalDrawBrushEntity */
		if( !nv2a_decal_world_surface_drawn( surf ))
			continue;

		nv2a_draw_decals[nv2a_num_draw_decals++] = surf;
	}

	nv2a_decal_stats.in_use = (unsigned int)in_use;

	DrawDecalsBatch( world_ent, 1.0f );
}

void R_DecalAddBrushSurface( const cl_entity_t *e, msurface_t *surf, int backside )
{
	if( !e || !surf || !surf->pdecals )
		return;

	/* R_RenderDecalsForSurface, gl_rsurf.c:1384: `e` is RI.currententity,
	   the entity nv2a_draw_brush_entity is drawing; its render mode
	   decides batch or sequential, as there. */

	if( e->curstate.rendermode == kRenderNormal )
	{
		// batch decals to draw later
		if( nv2a_num_draw_decals < MAX_RENDER_DECALS )
			nv2a_draw_decals[nv2a_num_draw_decals++] = surf;
	}
	else
	{
		// if rendermode != kRenderNormal draw decals sequentially
		if( nv2a_num_single_decals < MAX_RENDER_DECALS )
		{
			nv2a_single_decals[nv2a_num_single_decals] = surf;
			nv2a_single_reverse[nv2a_num_single_decals] = backside ? true : false;
			nv2a_num_single_decals++;
		}
	}
}

void R_DecalDrawBrushEntity( cl_entity_t *e, float blend )
{
	int i;

	if( !e )
	{
		nv2a_num_draw_decals = nv2a_num_single_decals = 0;
		return;
	}

	if( nv2a_num_single_decals )
	{
		if( nv2a_decal_gpu_begin( e, blend ))
		{
			for( i = 0; i < nv2a_num_single_decals; i++ )
				DrawSurfaceDecals( nv2a_single_decals[i], nv2a_single_reverse[i], e );
			nv2a_decal_gpu_end();
		}
		nv2a_num_single_decals = 0;
	}

	/* R_DrawBrushModel's DrawDecalsBatch, gl_rsurf.c:1914 */
	DrawDecalsBatch( e, blend );
}

/*
=============================================================

  DECALS SERIALIZATION

=============================================================
*/
static qboolean R_DecalUnProject( decal_t *pdecal, decallist_t *entry )
{
	if( !pdecal || !( pdecal->psurface ))
		return false;

	VectorCopy( pdecal->position, entry->position );
	entry->entityIndex = pdecal->entityIndex;

	// Grab surface plane equation
	if( pdecal->psurface->flags & SURF_PLANEBACK )
		VectorNegate( pdecal->psurface->plane->normal, entry->impactPlaneNormal );
	else VectorCopy( pdecal->psurface->plane->normal, entry->impactPlaneNormal );

	return true;
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *pList -
//			count -
// Output : static int
//-----------------------------------------------------------------------------
static int DecalListAdd( decallist_t *pList, int count )
{
	decallist_t *pdecal = pList + count;
	int i;

	for( i = 0; i < count; i++ )
	{
		if( !Q_strcmp( pdecal->name, pList[i].name ) &&  pdecal->entityIndex == pList[i].entityIndex )
		{
			vec3_t tmp;
			VectorSubtract( pdecal->position, pList[i].position, tmp );	// Merge

			if( VectorLength( tmp ) < DECAL_OVERLAP_DISTANCE )
				return count;
		}
	}

	// this is a new decal
	return count + 1;
}

static int DecalDepthCompare( const void *a, const void *b )
{
	const decallist_t *elem1 = (const decallist_t *)a;
	const decallist_t *elem2 = (const decallist_t *)b;

	if( elem1->depth > elem2->depth )
		return 1;
	if( elem1->depth < elem2->depth )
		return -1;

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: Called by CSaveRestore::SaveClientState
// Input  : *pList -
// Output : int
//-----------------------------------------------------------------------------
int R_CreateDecalList( decallist_t *pList )
{
	int total = 0;

	if( WORLDMODEL )
	{
		int i;

		for( i = 0; i < MAX_RENDER_DECALS; i++ )
		{
			decal_t *decal = &gDecalPool[i];
			decal_t *pdecals;
			int depth;

			// decal is in use and is not a custom decal
			if( decal->psurface == NULL || FBitSet( decal->flags, FDECAL_DONTSAVE ))
				 continue;

			// compute depth
			depth = 0;
			pdecals = decal->psurface->pdecals;

			while( pdecals && pdecals != decal )
			{
				depth++;
				pdecals = pdecals->pnext;
			}

			pList[total].depth = depth;
			pList[total].flags = decal->flags;
			pList[total].scale = decal->scale;

			R_DecalUnProject( decal, &pList[total] );
			COM_FileBase( GL_TextureName( decal->texture ), pList[total].name, sizeof( pList[total].name ));

			// check to see if the decal should be added
			total = DecalListAdd( pList, total );
		}

		if( gEngfuncs.drawFuncs->R_CreateStudioDecalList )
		{
			total += gEngfuncs.drawFuncs->R_CreateStudioDecalList( pList, total );
		}
	}

	// sort the decals lowest depth first, so they can be re-applied in order
	qsort( pList, total, sizeof( decallist_t ), DecalDepthCompare );

	return total;
}

/*
===============
R_DecalRemoveAll

remove all decals with specified texture
===============
*/
void R_DecalRemoveAll( int textureIndex )
{
	int i;

	if( textureIndex < 0 || textureIndex >= NV2A_MAX_TEXTURES )
		return; // out of bounds

	for( i = 0; i < gDecalCount; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];

		// don't remove permanent decals
		if( FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			continue;

		if( !textureIndex || ( pdecal->texture == textureIndex ))
			R_DecalUnlink( pdecal );
	}
}

/*
===============
R_EntityRemoveDecals

remove all decals from specified entity
===============
*/
void R_EntityRemoveDecals( model_t *mod )
{
	msurface_t *psurf;
	int i;

	if( !mod || mod->type != mod_brush )
		return;

	psurf = &mod->surfaces[mod->firstmodelsurface];
	for( i = 0; i < mod->nummodelsurfaces; i++, psurf++ )
	{
		decal_t *p;

		/* gl_decals.c:1177 reads p->pnext after R_DecalUnlink(p), which
		   leaves it intact, so the walk goes on: the same here. */
		for( p = psurf->pdecals; p; p = p->pnext )
			R_DecalUnlink( p );
	}
}

/*
===============
R_ClearAllDecals

remove all decals from anything
used for full decals restart
===============
*/
void R_ClearAllDecals( void )
{
	int i;

	// because gDecalCount may be zeroed after recach the decal limit
	for( i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];
		R_DecalUnlink( pdecal );
	}

	if( gEngfuncs.drawFuncs->R_ClearStudioDecals )
	{
		gEngfuncs.drawFuncs->R_ClearStudioDecals();
	}
}
