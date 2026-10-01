/*
r_warp.c - X9-X item 14 step 11: water warp and fog for ref/nv2a. A port
of ref/gl's water path (GL_SubdivideSurface, SubdividePolygon_r,
EmitWaterPolys, R_RenderBrushPoly's SURF_DRAWTURB branch,
R_DrawWaterSurfaces - gl_rsurf.c) and of its fog (R_CheckFog,
R_RecursiveFindWaterTexture, R_DrawFog, R_AllowFog - gl_rmain.c; TriFog,
TriFogParams - gl_triapi.c; the skybox's fog - gl_warp.c), root clone
c7768c48, drawn through the TriAPI internals in r_context.c
(Begin/TexCoord2f/Vertex3f/End and the nv2a_2d_desired shadow) the way
r_sky.c and r_effects.c draw.

How it fits together, as in the reference:
- As the map loads, the engine hands every SURF_DRAWTURB face to
  GL_SubdivideSurface (Mod_LoadSurfaces, mod_bmodel.c:3406 in the root clone), which cuts
  it along the 64-unit grid (SubdividePolygon_r, nv2a_warp_util.c) and
  keeps the pieces on the face (msurface_t::polys, in the model's pool).
- Each frame the world pass (r_context.c) hands every visible world water
  face here instead of drawing its baked, unwarped triangles; after the
  world they are drawn, each vertex moved by EmitWaterPolys' two-sine
  wave (the entity's scale is the wave height) and its texture coordinate
  pushed around by the r_turbsin table and cl.time. With sv_wateralpha
  below 1 they wait for R_DrawWaterSurfaces and draw blended after the
  entities. Brush entities (func_water, whose `waveheight` the game puts
  in scale) queue their water faces the same way and draw them under the
  entity's render mode and transform.
- The texture is the face's own baked DXT1 image from the map directory,
  bound with the world's WRAP addressing and mip chain: the coordinates
  run far past [0,1], which only a directory texture can repeat
  (nv2a_tri_bind_map_texture, r_context.c).
- Fog: R_CheckFog, after the world, finds the water the eye is in (the
  water entity's texture, or a SURF_DRAWTURB face reached from the view
  leaf) and takes its fog colour and density; R_DrawFog turns it on for
  the next frame. The GL fog state lives in nv2a_fog_current, which every
  draw applies (r_context.c: NV097_SET_FOG_*, the vertex programs' FOG
  output and the final combiner's fog blend).

Where it departs from ref/gl (docs/r3d/divergences.md #138):
- Storage. The pieces are kept as one block per face (positions only;
  the texture coordinates are two dot products, recomputed when drawn)
  rather than one glpoly2_t per piece with seven floats a vertex: the
  reference layout costs 1.91 MiB on disposal, which has 2.7 MiB free
  (0.62 MiB in this one).
  msurface_t::polys points at that block (nothing in the engine reads the
  field; only the renderer that filled it). A map whose water would need
  more than NV2A_WARP_MAX_MAP_BYTES keeps the rest of its water unwarped
  (the baked faces), with a warning.
- The fog colour. The reference takes it from the palette of wherever the
  texture's pixels came from (entry 3 the colour, entry 4's red the
  density, img_wad.c:584-592): the BSP's own embedded miptex with the
  default r_wadtextures 0, else the map's "wad" list (Mod_LoadTextureData,
  mod_bmodel.c:2822-2867). This renderer never loads either - the Xbox
  bake keeps no pixels or palette and drops the "wad" key - so the palette
  is read here once per map, by name, from the gamedir's mounted WADs.
  For retail content this is the same numbers: all 18 distinct '!' textures
  the retail maps use are in the disc's WADs, and the 12 embedded copies
  carry the same entries 3 and 4 as their WAD namesakes. A custom map's
  embedded water texture with its own palette would get the WAD's fog.
- No r_ripple (software ripple texture; the cvar defaults to 0,
  gl_opengl.c:38), no lightmapped water (FWORLD_HAS_LITWATER or
  gl_litwater_force, neither true for Half-Life, gl_rsurf.c:79), no
  multi-pass fog-colour correction (GL_SetupFogColorForSurfaces: this
  renderer draws lightmaps in one pass, so the true colour is right), no
  Quake global fog (ENGINE_QUAKE_COMPATIBLE), no Spirit direct-GL fog
  (R_CheckGLFog, HACKS_RELATED_HLMODS).
*/
#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "bspfile.h"
#include "com_strings.h" /* S_WARN */
#include "triangleapi.h"
#include "pmove.h"       /* movevars_s (gp_movevars->wateralpha) */
#include "enginefeatures.h"
#include "ref_params.h"   /* RF_DRAW_WORLD, RF_ONLY_CLIENTDRAW */
#include "crtlib.h"       /* Q_snprintf, Q_strnicmp - not transitive here */
#include "nv2a_texture.h"
#include "nv2a_warp_util.h"
#include "nv2a_tri_util.h"
#include "r_studio_bridge.h"
#include "r_warp.h"
#include "nv2a_ft.h" /* frame-time work counters (divergences.md #141) */

/* A cap on one map's water pieces: the largest retail map needs 0.64 MiB
   in this layout, 652348 bytes (disposal: 928 faces, 12410 pieces,
   49836 vertices). */
#define NV2A_WARP_MAX_MAP_BYTES ( 1024u * 1024u )

/* One face's pieces, in the order SubdividePolygon_r emits them: `counts`
   holds each piece's vertex count, `xyz` all the vertices, piece after
   piece. The reference chains its glpoly2_t pieces by prepending
   (gl_rsurf.c:274-276), so it draws them last-emitted first; so does
   R_WarpEmitSurface. */
typedef struct
{
	uint16_t numpolys;
	uint16_t numverts;
	uint8_t  *counts;
	float    *xyz;
} nv2a_warp_polys_t;

nv2a_warp_stats_t nv2a_warp_stats;

static CVAR_DEFINE_AUTO( gl_fog, "1", FCVAR_GLCONFIG, "allow for rendering fog using built-in OpenGL fog implementation" );
static qboolean nv2a_warp_cvars_registered;

/*
========================================================================
GL_SubdivideSurface
========================================================================
*/

/* The per-map budget's accounting. The engine loads every world into the
   same model_t slot (Mod_LoadWorld, engine/common/model.c:506), so the
   pointer alone cannot tell two maps apart: R_WarpNewMap, which runs after
   every map load, ends the count, and within one load the engine walks the
   surfaces in order (Mod_LoadSurfaces), so a surface at or before the last
   one seen, or a different surface array, also starts a new count. */
static const model_t    *nv2a_warp_loading_model;
static const msurface_t *nv2a_warp_loading_surfaces;
static const msurface_t *nv2a_warp_loading_last;
static size_t           nv2a_warp_map_bytes;
static qboolean         nv2a_warp_warned_budget;

static void R_WarpResetLoadBudget( void )
{
	nv2a_warp_loading_model = NULL;
	nv2a_warp_loading_surfaces = NULL;
	nv2a_warp_loading_last = NULL;
	nv2a_warp_map_bytes = 0;
	nv2a_warp_warned_budget = false;
}

typedef struct
{
	int              polys;
	int              verts;
	nv2a_warp_polys_t *out;   /* NULL on the counting pass */
	int              quads;   /* every piece has 4 points (SURF_DRAWTURB_QUADS) */
} nv2a_warp_build_t;

static void nv2a_warp_collect( void *ctx, int numverts, const float *verts )
{
	nv2a_warp_build_t *b = (nv2a_warp_build_t *)ctx;

	if( numverts != 4 )
		b->quads = false; /* gl_rsurf.c:269-270 */

	if( b->out )
	{
		b->out->counts[b->polys] = (uint8_t)numverts;
		memcpy( b->out->xyz + (size_t)b->verts * 3, verts, (size_t)numverts * 3 * sizeof( float ));
	}
	b->polys++;
	b->verts += numverts;
}

/* R_GetEdgePosition, gl_rsurf.c:163 (the same walk r_sky.c ports). */
static void nv2a_warp_edge_position( const model_t *mod, const msurface_t *fa, int i, vec3_t vec )
{
	const int lindex = mod->surfedges[fa->firstedge + i];

	if( FBitSet( mod->flags, MODEL_QBSP2 ))
	{
		const medge32_t *pedges = mod->edges32;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
	else
	{
		const medge16_t *pedges = mod->edges16;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
}

static int nv2a_warp_fill_edges( const model_t *mod, const msurface_t *fa, float verts[NV2A_WARP_SUBDIVIDE_SIZE][3] )
{
	int i;

	for( i = 0; i < fa->numedges; i++ )
		nv2a_warp_edge_position( mod, fa, i, verts[i] );
	return fa->numedges;
}

/* GL_SubdivideSurface, gl_rsurf.c:361: the edge loop, cut by
   SubdividePolygon_r - run twice, to count and then to fill one block. */
void R_WarpSubdivideSurface( model_t *mod, msurface_t *fa )
{
	float verts[NV2A_WARP_SUBDIVIDE_SIZE][3];
	nv2a_warp_build_t build;
	nv2a_warp_polys_t *warp;
	size_t counts_bytes, bytes;
	byte *block;

	if( !mod || !fa || !mod->surfedges || !mod->vertexes )
		return;

	if( mod != nv2a_warp_loading_model || mod->surfaces != nv2a_warp_loading_surfaces || fa <= nv2a_warp_loading_last )
	{
		R_WarpResetLoadBudget();
		nv2a_warp_loading_model = mod;
		nv2a_warp_loading_surfaces = mod->surfaces;
	}
	nv2a_warp_loading_last = fa;

	/* The reference Host_Errors on either (gl_rsurf.c:212); one face is
	   only cosmetic, so it stays unwarped (the baked face) instead. */
	if( fa->numedges < 3 || fa->numedges > NV2A_WARP_MAX_VERTS )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: water face with %d edges not subdivided\n", fa->numedges );
		return;
	}

	memset( &build, 0, sizeof( build ));
	build.quads = true;
	nv2a_warp_fill_edges( mod, fa, verts );
	if( !nv2a_warp_subdivide( fa->numedges, verts[0], nv2a_warp_collect, &build ) || build.polys == 0 || build.polys > 0xFFFF || build.verts > 0xFFFF )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_nv2a: water face not subdivided (a piece over %d points, or too many pieces)\n", NV2A_WARP_MAX_VERTS );
		return;
	}

	counts_bytes = ( (size_t)build.polys + 3 ) & ~(size_t)3;
	bytes = sizeof( nv2a_warp_polys_t ) + counts_bytes + (size_t)build.verts * 3 * sizeof( float );
	if( nv2a_warp_map_bytes + bytes > NV2A_WARP_MAX_MAP_BYTES )
	{
		if( !nv2a_warp_warned_budget )
		{
			gEngfuncs.Con_Printf( S_WARN "ref_nv2a: water over %u bytes, the rest of this map's water is not warped\n", NV2A_WARP_MAX_MAP_BYTES );
			nv2a_warp_warned_budget = true;
		}
		return;
	}
	nv2a_warp_map_bytes += bytes;

	block = Mem_Calloc( mod->mempool, bytes );
	warp = (nv2a_warp_polys_t *)block;
	warp->numpolys = (uint16_t)build.polys;
	warp->numverts = (uint16_t)build.verts;
	warp->counts = block + sizeof( nv2a_warp_polys_t );
	warp->xyz = (float *)( block + sizeof( nv2a_warp_polys_t ) + counts_bytes );

	/* The fill pass: the same input, so the same pieces in the same order. */
	memset( &build, 0, sizeof( build ));
	build.quads = true;
	build.out = warp;
	nv2a_warp_fill_edges( mod, fa, verts );
	nv2a_warp_subdivide( fa->numedges, verts[0], nv2a_warp_collect, &build );

	/* gl_rsurf.c:369-370: predicted for the GL context, cleared by any
	   piece that is not a quad. */
	if( build.quads )
		SetBits( fa->flags, SURF_DRAWTURB_QUADS );
	else
		ClearBits( fa->flags, SURF_DRAWTURB_QUADS );

	/* divergences.md #138: this renderer's own block, not a glpoly2_t
	   chain - nothing in the engine reads msurface_t::polys. */
	fa->polys = (glpoly2_t *)warp;
}

/*
========================================================================
EmitWaterPolys
========================================================================
*/

static vec3_t nv2a_warp_vieworigin; /* RI.rvp.vieworigin */

/* The entity's object transform, as r_context.c's nv2a_draw_brush_entity
   builds it (Matrix4x4_CreateFromEntity's axes): world = origin + x *
   forward + y * -right + z * up, or just + origin when not rotated. */
typedef struct
{
	qboolean identity;
	qboolean rotated;
	vec3_t   origin, forward, left, up;
} nv2a_warp_xform_t;

static void nv2a_warp_transform( const nv2a_warp_xform_t *x, float px, float py, float pz, float out[3] )
{
	if( x->identity )
	{
		out[0] = px; out[1] = py; out[2] = pz;
	}
	else if( !x->rotated )
	{
		out[0] = px + x->origin[0];
		out[1] = py + x->origin[1];
		out[2] = pz + x->origin[2];
	}
	else
	{
		int i;

		for( i = 0; i < 3; i++ )
			out[i] = x->origin[i] + px * x->forward[i] + py * x->left[i] + pz * x->up[i];
	}
}

/* EmitWaterPolys( warp, reverse, ripples ), gl_rsurf.c:926, for the
   current entity's scale and transform. r_ripple is not ported (#138), so
   ripples is always false. Each piece goes to the TriAPI's batch whole:
   a quad as a quad (GL_QUADS when every piece is one, gl_rsurf.c:940), any
   other piece as the triangle fan GL_POLYGON is. The wave and texture
   coordinate use this frame's hoisted time terms (nv2a_warp_frame_t). */
static nv2a_warp_frame_t nv2a_warp_frame;

static void R_WarpEmitSurface( const msurface_t *warp, qboolean reverse, float scale, const nv2a_warp_xform_t *xform )
{
	const nv2a_warp_polys_t *polys = (const nv2a_warp_polys_t *)warp->polys;
	const mtexinfo_t *info = warp->texinfo;
	const qboolean useQuads = FBitSet( warp->flags, SURF_DRAWTURB_QUADS );
	nv2a_tri_vert_t out[NV2A_WARP_MAX_VERTS];
	nv2a_tri_vert_t tri[3 * ( NV2A_WARP_MAX_VERTS - 2 )];
	float waveHeight;
	int p, end;

	if( !polys || !info ) return;

	/* set the current waveheight - from the first point of the chain's
	   head, the last piece emitted (header). */
	end = polys->numverts - polys->counts[polys->numpolys - 1];
	if( polys->xyz[end * 3 + 2] >= nv2a_warp_vieworigin[2] )
		waveHeight = -scale;
	else waveHeight = scale;

	nv2a_warp_stats.faces++;
	end = polys->numverts;
	for( p = polys->numpolys - 1; p >= 0; p-- )
	{
		const int n = polys->counts[p];
		const int first = end - n;
		int i;

		for( i = 0; i < n; i++ )
		{
			const float *v = polys->xyz + (size_t)( reverse ? end - 1 - i : first + i ) * 3;
			float st[2];
			float nv = nv2a_warp_wave_z_frame( &nv2a_warp_frame, v, waveHeight );

			/* R_TextureCoord for SURF_DRAWTURB (gl_rsurf.c:147-160): no
			   offset, no division by the texture size. */
			nv2a_warp_texcoord_frame( &nv2a_warp_frame, DotProduct( v, info->vecs[0] ), DotProduct( v, info->vecs[1] ), st );
			nv2a_warp_transform( xform, v[0], v[1], nv, &out[i].x );
			out[i].u = st[0];
			out[i].v = st[1];
		}

		if( useQuads && n == 4 )
		{
			nv2a_tri_append_3d( true, out, 4 );
		}
		else
		{
			/* GL_POLYGON as a fan, in GL's own order (0, i, i + 1). */
			int k = 0;

			for( i = 1; i + 1 < n; i++ )
			{
				tri[k++] = out[0];
				tri[k++] = out[i];
				tri[k++] = out[i + 1];
			}
			nv2a_tri_append_3d( false, tri, (uint32_t)k );
		}

		nv2a_warp_stats.polys++;
		nv2a_warp_stats.verts += n;
		NV2A_FT_ADD( XBOX_FT_W_WATER_VERTS, n );
		end = first;
	}
}

/* TriRenderMode sets the blend state these paths need, but also calls
   R_AllowFog, which ref/gl's R_SetRenderMode (gl_rsurf.c:1717) and
   R_DrawWaterSurfaces (:1630) never do: keep the GL fog enable as it was. */
static qboolean nv2a_gl_fog_enabled_now( void );
static void nv2a_gl_fog_enable( qboolean on );

static void R_WarpBlendMode( int mode )
{
	qboolean fog = nv2a_gl_fog_enabled_now();

	TriRenderMode( mode );
	if( nv2a_gl_fog_enabled_now() != fog )
		nv2a_gl_fog_enable( fog );
}

/*
========================================================================
The world's water (R_DrawTextureChains / R_DrawWaterSurfaces)
========================================================================
*/

/* One chain through msurface_t::texturechain, the field the reference
   chains the same faces through (gl_rsurf.c:3526). Faces arrive grouped
   by texture (the world pass walks the baked batches, one texture each),
   so consecutive faces share a bind. */
static msurface_t *nv2a_warp_world_chain;

void R_WarpAddWorldSurface( msurface_t *surf )
{
	if( !surf || !surf->polys )
		return;
	surf->texturechain = nv2a_warp_world_chain;
	nv2a_warp_world_chain = surf;
}

/* Binds the face's directory texture for the next primitives; false when
   it has none (the face is then skipped: no table, nothing to sample). */
static qboolean R_WarpBindSurface( const msurface_t *surf, uint32_t *bound )
{
	uint32_t dir = nv2a_surface_map_texture( surf );

	if( dir == NV2A_TRI_NO_MAP_TEXTURE )
		return false;
	if( dir != *bound )
	{
		nv2a_tri_bind_map_texture( dir );
		*bound = dir;
	}
	return true;
}

static void R_WarpDrawChain( msurface_t *chain, float scale )
{
	nv2a_warp_xform_t identity;
	uint32_t bound = NV2A_TRI_NO_MAP_TEXTURE;
	msurface_t *s;

	memset( &identity, 0, sizeof( identity ));
	identity.identity = true;

	for( s = chain; s != NULL; s = s->texturechain )
	{
		if( !R_WarpBindSurface( s, &bound ))
			continue;
		R_WarpEmitSurface( s, false, scale, &identity );
	}

	nv2a_tri_bind_map_texture( NV2A_TRI_NO_MAP_TEXTURE );
}

static float R_WarpWorldScale( void )
{
	const cl_entity_t *world_ent = nv2a_get_entity_by_index( 0 );

	return world_ent ? world_ent->curstate.scale : 0.0f;
}

/* R_DrawTextureChains' water (gl_rsurf.c:1515-1566): opaque, in the
   world's own state - colour 255 (:1516), blend off, depth writes on,
   the world's GL_FRONT culling (R_SetupGL). Translucent water
   (wateralpha < 1, :1555-1559) waits for R_WarpDrawWaterSurfaces. */
void R_WarpDrawWorld( void )
{
	nv2a_2d_shadow_t saved;

	if( !nv2a_warp_world_chain )
		return;

	if( gp_movevars->wateralpha < 1.0f )
		return; /* draw translucent water later */

	saved = nv2a_2d_desired;
	nv2a_2d_desired.blend_enable = false;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.depth_mask = true;
	nv2a_2d_desired.cull_enable = true;
	nv2a_2d_desired.depth_test_disable = false;
	nv2a_2d_desired.depth_func_equal = false;
	Color4ub( 255, 255, 255, 255 );

	R_WarpDrawChain( nv2a_warp_world_chain, R_WarpWorldScale());
	nv2a_warp_world_chain = NULL;

	nv2a_2d_desired = saved;
}

/* R_DrawWaterSurfaces, gl_rsurf.c:1630. */
void R_WarpDrawWaterSurfaces( int rvp_flags )
{
	nv2a_2d_shadow_t saved;

	if( !FBitSet( rvp_flags, RF_DRAW_WORLD ) || FBitSet( rvp_flags, RF_ONLY_CLIENTDRAW ))
	{
		nv2a_warp_world_chain = NULL;
		return;
	}

	// non-transparent water is already drawed
	if( !nv2a_warp_world_chain )
		return;

	saved = nv2a_2d_desired;
	/* pglEnable( GL_BLEND ); pglDepthMask( GL_FALSE ); pglDisable(
	   GL_ALPHA_TEST ); SRC_ALPHA, ONE_MINUS_SRC_ALPHA; MODULATE with
	   ( 1, 1, 1, wateralpha ) (gl_rsurf.c:1646-1651). */
	R_WarpBlendMode( kRenderTransTexture );
	nv2a_2d_desired.depth_mask = false;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.cull_enable = true;
	nv2a_2d_desired.depth_test_disable = false;
	nv2a_2d_desired.depth_func_equal = false;
	Color4f( 1.0f, 1.0f, 1.0f, gp_movevars->wateralpha );

	R_WarpDrawChain( nv2a_warp_world_chain, R_WarpWorldScale());
	nv2a_warp_world_chain = NULL;

	nv2a_2d_desired = saved;
	Color4ub( 255, 255, 255, 255 ); /* gl_rsurf.c:1685 */
}

/*
========================================================================
Brush entities (R_DrawBrushModel -> R_RenderBrushPoly)
========================================================================
*/

#define NV2A_WARP_MAX_ENTITY_FACES 1024

static struct
{
	msurface_t *surf;
	qboolean    reverse;
} nv2a_warp_entity_faces[NV2A_WARP_MAX_ENTITY_FACES];
static int      nv2a_warp_entity_count;
static qboolean nv2a_warp_warned_entity;

void R_WarpAddBrushSurface( msurface_t *surf, qboolean reverse )
{
	if( !surf || !surf->polys )
		return;
	if( nv2a_warp_entity_count >= NV2A_WARP_MAX_ENTITY_FACES )
	{
		if( !nv2a_warp_warned_entity )
		{
			gEngfuncs.Con_Printf( S_WARN "ref_nv2a: more than %d water faces in one brush entity, the rest not drawn\n", NV2A_WARP_MAX_ENTITY_FACES );
			nv2a_warp_warned_entity = true;
		}
		return;
	}
	nv2a_warp_entity_faces[nv2a_warp_entity_count].surf = surf;
	nv2a_warp_entity_faces[nv2a_warp_entity_count].reverse = reverse;
	nv2a_warp_entity_count++;
}

void R_WarpDrawBrushEntity( cl_entity_t *e, float blend, qboolean rotated )
{
	nv2a_2d_shadow_t saved;
	nv2a_warp_xform_t xform;
	uint32_t bound = NV2A_TRI_NO_MAP_TEXTURE;
	qboolean flat_color;
	int i;

	if( nv2a_warp_entity_count == 0 )
		return;

	memset( &xform, 0, sizeof( xform ));
	xform.rotated = rotated;
	VectorCopy( e->origin, xform.origin );
	if( rotated )
	{
		vec3_t right;

		AngleVectors( e->angles, xform.forward, right, xform.up );
		VectorNegate( right, xform.left );
	}

	saved = nv2a_2d_desired;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.depth_mask = true;
	nv2a_2d_desired.cull_enable = true;
	nv2a_2d_desired.depth_test_disable = false;
	nv2a_2d_desired.depth_func_equal = false;
	flat_color = false;

	/* R_SetRenderMode, gl_rsurf.c:1717, with tr.blend = blend. */
	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
		R_WarpBlendMode( kRenderNormal );
		Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
		break;
	case kRenderTransColor:
		/* GL_TEXTURE_2D off: the entity's colour alone. */
		R_WarpBlendMode( kRenderTransColor );
		Color4ub( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, e->curstate.renderamt );
		flat_color = true;
		break;
	case kRenderTransAdd:
		/* GL_ONE, GL_ONE with colour ( blend, blend, blend, 1 ): the
		   TriAPI's additive SRC_ALPHA, ONE is the same sum at alpha 1.
		   R_AllowFog( false ) around it (gl_rsurf.c:1884-1888). */
		R_WarpBlendMode( kRenderTransAdd );
		Color4f( blend, blend, blend, 1.0f );
		R_WarpAllowFog( false );
		break;
	case kRenderTransAlpha:
		/* Alpha test GREATER 0.25 (gl_rsurf.c:1752), no blend. */
		R_WarpBlendMode( kRenderNormal );
		nv2a_2d_desired.alpha_test_enable = true;
		nv2a_2d_desired.alpha_ref = 63; /* 0.25 * 255, GREATER */
		Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
		break;
	default:
		R_WarpBlendMode( kRenderTransTexture );
		nv2a_2d_desired.depth_mask = false;
		Color4f( 1.0f, 1.0f, 1.0f, blend );
		break;
	}

	if( flat_color )
		GL_Bind( 0, nv2a_tex_white());

	for( i = 0; i < nv2a_warp_entity_count; i++ )
	{
		msurface_t *s = nv2a_warp_entity_faces[i].surf;

		if( !flat_color && !R_WarpBindSurface( s, &bound ))
			continue;
		R_WarpEmitSurface( s, nv2a_warp_entity_faces[i].reverse, e->curstate.scale, &xform );
	}
	nv2a_warp_entity_count = 0;

	nv2a_tri_bind_map_texture( NV2A_TRI_NO_MAP_TEXTURE );
	/* Submitted under this entity's state before the caller's own brush
	   state push (r_context.c re-enters after any TriAPI flush). */
	nv2a_tri_flush();

	if( e->curstate.rendermode == kRenderTransAdd )
		R_WarpAllowFog( true );

	nv2a_2d_desired = saved;
	Color4ub( 255, 255, 255, 255 );
}

/*
========================================================================
Fog
========================================================================
*/

/* The water textures' fog parameters (gl_texture_t::fogParams), per
   world texture: r, g, b, density. */
#define NV2A_WARP_MAX_FOG_TEXTURES 512
static byte          nv2a_warp_fog_params[NV2A_WARP_MAX_FOG_TEXTURES][4];
static const model_t *nv2a_warp_fog_world;

/* RI's fog fields (gl_local.h:140-150) and the two cached values
   R_CheckFog keeps between frames. */
static struct
{
	qboolean fogEnabled, fogCustom, fogSkybox;
	vec4_t   fogColor;
	float    fogDensity, fogStart, fogEnd;
	int      cached_waterlevel;
	int      cached_contents;
} nv2a_ri_fog;

/* The GL fog state (glEnable( GL_FOG ) and glFog*) and glState's
   isFogEnabled sample. */
static struct
{
	qboolean enabled;
	int      mode;
	float    density, start, end;
	vec3_t   color;
} nv2a_gl_fog;
static qboolean nv2a_gl_is_fog_enabled;

nv2a_fog_hw_t nv2a_fog_current;

static void R_WarpSyncFog( void )
{
	nv2a_fog_current.enable = nv2a_gl_fog.enabled;
	nv2a_fog_current.mode = nv2a_gl_fog.mode;
	nv2a_fog_current.density = nv2a_gl_fog.density;
	nv2a_fog_current.start = nv2a_gl_fog.start;
	nv2a_fog_current.end = nv2a_gl_fog.end;
	nv2a_fog_current.color = nv2a_fog_pack_color( nv2a_gl_fog.color );
}

static void nv2a_gl_fog_enable( qboolean on )
{
	nv2a_gl_fog.enabled = on;
	R_WarpSyncFog();
}

static qboolean nv2a_gl_fog_enabled_now( void )
{
	return nv2a_gl_fog.enabled;
}

static void R_WarpRegisterCvars( void )
{
	if( nv2a_warp_cvars_registered )
		return;
	gEngfuncs.Cvar_RegisterVariable( &gl_fog );
	nv2a_warp_cvars_registered = true;
}

/* img_wad.c:582-592: a Half-Life water texture ('!' or "water" in its
   name) carries its fog colour in palette entry 3 and its density in
   entry 4's red. Loaded through the engine's image loader, which decodes
   a WAD miptex with its palette (Image_LoadMIP), so the numbers are the
   reference's own. */
static void R_WarpLoadFogParams( const model_t *world )
{
	int i;

	memset( nv2a_warp_fog_params, 0, sizeof( nv2a_warp_fog_params ));
	nv2a_warp_fog_world = world;

	if( !world || !world->textures )
		return;

	for( i = 0; i < world->numtextures && i < NV2A_WARP_MAX_FOG_TEXTURES; i++ )
	{
		const texture_t *t = world->textures[i];
		char name[80];
		rgbdata_t *pic;

		if( !t || !( t->name[0] == '!' || !Q_strnicmp( t->name, "water", 5 )))
			continue;

		Q_snprintf( name, sizeof( name ), "%s.mip", t->name );
		pic = gEngfuncs.FS_LoadImage( name, NULL, 0 );
		if( !pic )
		{
			gEngfuncs.Con_DPrintf( "ref_nv2a: no WAD palette for water texture %s, no fog\n", t->name );
			continue;
		}
		memcpy( nv2a_warp_fog_params[i], pic->fogParams, 4 );
		gEngfuncs.FS_FreeImage( pic );
		gEngfuncs.Con_DPrintf( "ref_nv2a: water %s fog %d %d %d density %d\n", t->name,
			nv2a_warp_fog_params[i][0], nv2a_warp_fog_params[i][1], nv2a_warp_fog_params[i][2], nv2a_warp_fog_params[i][3] );
	}
}

static const byte *R_WarpFogParamsFor( const texture_t *tex )
{
	const model_t *world = nv2a_warp_fog_world;
	int i;

	if( !world || !tex || world != gp_cl->models[1] )
		return NULL;
	for( i = 0; i < world->numtextures && i < NV2A_WARP_MAX_FOG_TEXTURES; i++ )
	{
		if( world->textures[i] == tex )
			return nv2a_warp_fog_params[i];
	}
	return NULL;
}

void R_WarpNewMap( const model_t *world )
{
	R_WarpRegisterCvars();
	nv2a_warp_world_chain = NULL;
	nv2a_warp_entity_count = 0;
	R_WarpLoadFogParams( world );

	/* R_NewMap, gl_context.c:411-413: glState.isFogEnabled = false and
	   GL_FOG off; RI's fog state carries over, as there. */
	nv2a_gl_is_fog_enabled = false;
	nv2a_gl_fog_enable( false );

	/* The map is loaded: the next GL_SubdivideSurface is another map's. */
	R_WarpResetLoadBudget();
}

void R_WarpFreeMap( void )
{
	nv2a_warp_world_chain = NULL;
	nv2a_warp_entity_count = 0;
	nv2a_warp_fog_world = NULL;
	nv2a_gl_is_fog_enabled = false;
	nv2a_gl_fog_enable( false );
}

void R_WarpShutdown( void )
{
	R_WarpFreeMap();
	R_WarpResetLoadBudget();
	memset( &nv2a_gl_fog, 0, sizeof( nv2a_gl_fog ));
	memset( &nv2a_fog_current, 0, sizeof( nv2a_fog_current ));
}

/* R_SetupFrame, gl_rmain.c:517 - glState.isFogEnabled is sampled before
   R_DrawFog runs, so it holds the state the previous frame left. */
void R_WarpSetupFrame( const float *vieworigin )
{
	R_WarpRegisterCvars();
	VectorCopy( vieworigin, nv2a_warp_vieworigin );
	nv2a_gl_is_fog_enabled = nv2a_gl_fog.enabled;
	memset( &nv2a_warp_stats, 0, sizeof( nv2a_warp_stats ));
	nv2a_warp_world_chain = NULL;
	nv2a_warp_frame_init( &nv2a_warp_frame, gp_cl->time );
}

/* R_AllowFog, gl_rmain.c:42. */
void R_WarpAllowFog( qboolean allowed )
{
	if( allowed )
	{
		if( nv2a_gl_is_fog_enabled && gl_fog.value )
			nv2a_gl_fog_enable( true );
	}
	else
	{
		if( nv2a_gl_is_fog_enabled )
			nv2a_gl_fog_enable( false );
	}
}

/* R_DrawFog, gl_rmain.c:780: GL_EXP at the water's density
   (ENGINE_QUAKE_COMPATIBLE's GL_EXP2 is not ported, #138). */
void R_WarpDrawFog( void )
{
	if( !nv2a_ri_fog.fogEnabled || !gl_fog.value )
		return;

	nv2a_gl_fog.mode = NV2A_FOG_MODE_EXP;
	nv2a_gl_fog.density = nv2a_ri_fog.fogDensity;
	VectorCopy( nv2a_ri_fog.fogColor, nv2a_gl_fog.color );
	nv2a_gl_fog_enable( true );
}

/* R_RecursiveFindWaterTexture, gl_rmain.c:586, returning the face's
   texture_t (whose fog parameters R_WarpFogParamsFor has) instead of its
   gl_texture_t. */
static const texture_t *R_RecursiveFindWaterTexture( const model_t *world, const mnode_t *node, const mnode_t *ignore, qboolean down )
{
	const texture_t *tex = NULL;
	mnode_t *child;

	// ignore solid nodes
	if( node->contents == CONTENTS_SOLID )
		return NULL;

	if( node->contents < 0 )
	{
		const mleaf_t *pleaf = (const mleaf_t *)node;
		msurface_t **mark;
		int i, c;

		// ignore non-liquid leaves
		if( node->contents != CONTENTS_WATER && node->contents != CONTENTS_LAVA && node->contents != CONTENTS_SLIME )
			return NULL;

		// find texture
		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		for( i = 0; i < c; i++, mark++ )
		{
			if( (*mark)->flags & SURF_DRAWTURB && (*mark)->texinfo && (*mark)->texinfo->texture )
				return (*mark)->texinfo->texture;
		}

		// texture not found
		return NULL;
	}

	// this is a regular node
	// traverse children
	child = node_child( node, 0, world );

	if( child && ( child != ignore ))
	{
		tex = R_RecursiveFindWaterTexture( world, child, node, true );
		if( tex ) return tex;
	}

	child = node_child( node, 1, world );

	if( child && ( child != ignore ))
	{
		tex = R_RecursiveFindWaterTexture( world, child, node, true );
		if( tex ) return tex;
	}

	// for down recursion, return immediately
	if( down ) return NULL;

	// texture not found, step up if any
	if( node->parent )
		return R_RecursiveFindWaterTexture( world, node->parent, node, false );

	// top-level node, bail out
	return NULL;
}

/* R_CheckFog, gl_rmain.c:655. */
void R_WarpCheckFog( int rvp_flags, mleaf_t *viewleaf )
{
	const model_t *world = gp_cl->models[1];
	cl_entity_t *ent;
	int cnt, waterlevel;

	// quake global fog: not ported (#138)
	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
	{
		nv2a_ri_fog.fogEnabled = false;
		return;
	}

	nv2a_ri_fog.fogEnabled = false;
	waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );

	if( FBitSet( rvp_flags, RF_ONLY_CLIENTDRAW ) || waterlevel < 3 || !FBitSet( rvp_flags, RF_DRAW_WORLD ) || !viewleaf || !world )
	{
		if( nv2a_ri_fog.cached_waterlevel == 3 )
		{
			// in some cases waterlevel jumps from 3 to 1. Catch it
			nv2a_ri_fog.cached_waterlevel = waterlevel;
			nv2a_ri_fog.cached_contents = CONTENTS_EMPTY;
			if( !nv2a_ri_fog.fogCustom )
			{
				nv2a_gl_is_fog_enabled = false;
				nv2a_gl_fog_enable( false );
			}
		}
		return;
	}

	ent = gEngfuncs.CL_GetWaterEntity( nv2a_warp_vieworigin );
	if( ent && ent->model && ent->model->type == mod_brush && ent->curstate.skin < 0 )
		cnt = ent->curstate.skin;
	else cnt = viewleaf->contents;

	nv2a_ri_fog.cached_waterlevel = waterlevel;

	if( !IsLiquidContents( nv2a_ri_fog.cached_contents ) && IsLiquidContents( cnt ))
	{
		const texture_t *tex = NULL;
		const byte *fog;

		// check for water texture
		if( ent && ent->model && ent->model->type == mod_brush )
		{
			int i, count = ent->model->nummodelsurfaces;
			const msurface_t *surf = &ent->model->surfaces[ent->model->firstmodelsurface];

			for( i = 0; i < count; i++, surf++ )
			{
				if( surf->flags & SURF_DRAWTURB && surf->texinfo && surf->texinfo->texture )
				{
					tex = surf->texinfo->texture;
					nv2a_ri_fog.cached_contents = ent->curstate.skin;
					break;
				}
			}
		}
		else
		{
			tex = R_RecursiveFindWaterTexture( world, viewleaf->parent, NULL, false );
			if( tex ) nv2a_ri_fog.cached_contents = viewleaf->contents;
		}

		if( !tex ) return; // no valid fogs

		fog = R_WarpFogParamsFor( tex );
		if( !fog ) return;

		// copy fog params
		nv2a_ri_fog.fogColor[0] = fog[0] / 255.0f;
		nv2a_ri_fog.fogColor[1] = fog[1] / 255.0f;
		nv2a_ri_fog.fogColor[2] = fog[2] / 255.0f;
		nv2a_ri_fog.fogDensity = fog[3] * 0.000025f;
		nv2a_ri_fog.fogStart = nv2a_ri_fog.fogEnd = 0.0f;
		nv2a_ri_fog.fogColor[3] = 1.0f;
		nv2a_ri_fog.fogCustom = false;
		nv2a_ri_fog.fogEnabled = true;
		nv2a_ri_fog.fogSkybox = true;
	}
	else
	{
		nv2a_ri_fog.fogCustom = false;
		nv2a_ri_fog.fogEnabled = true;
		nv2a_ri_fog.fogSkybox = true;
	}
}

/* R_DrawSkyBox, gl_warp.c:334-338: "don't fogging skybox (this fix old
   Half-Life bug)" unless RI.fogSkybox, and half the density. */
void R_WarpSkyFogBegin( void )
{
	if( !nv2a_ri_fog.fogSkybox )
		R_WarpAllowFog( false );

	if( nv2a_ri_fog.fogEnabled )
	{
		nv2a_gl_fog.density = nv2a_ri_fog.fogDensity * 0.5f;
		R_WarpSyncFog();
	}
}

/* gl_warp.c:361-365. */
void R_WarpSkyFogEnd( void )
{
	if( !nv2a_ri_fog.fogSkybox )
		R_WarpAllowFog( true );

	if( nv2a_ri_fog.fogEnabled )
	{
		nv2a_gl_fog.density = nv2a_ri_fog.fogDensity;
		R_WarpSyncFog();
	}
}

/* TriFog, gl_triapi.c:266. */
void R_WarpTriFog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
	R_WarpRegisterCvars();

	// overrided by internal fog
	if( nv2a_ri_fog.fogEnabled || !gl_fog.value ) return;
	nv2a_ri_fog.fogCustom = bOn;

	// check for invalid parms
	if( flEnd <= flStart )
	{
		nv2a_gl_is_fog_enabled = nv2a_ri_fog.fogCustom = false;
		nv2a_gl_fog_enable( false );
		return;
	}

	// copy fog params
	nv2a_ri_fog.fogColor[0] = flFogColor[0] / 255.0f;
	nv2a_ri_fog.fogColor[1] = flFogColor[1] / 255.0f;
	nv2a_ri_fog.fogColor[2] = flFogColor[2] / 255.0f;
	nv2a_ri_fog.fogColor[3] = 1.0f;

	nv2a_ri_fog.fogStart = flStart;
	nv2a_ri_fog.fogEnd = flEnd;

	if( nv2a_ri_fog.fogDensity > 0.0f )
	{
		nv2a_gl_fog.mode = NV2A_FOG_MODE_EXP2;
		nv2a_gl_fog.density = nv2a_ri_fog.fogDensity;
	}
	else
	{
		nv2a_gl_fog.mode = NV2A_FOG_MODE_LINEAR;
		nv2a_ri_fog.fogSkybox = true;
	}

	VectorCopy( nv2a_ri_fog.fogColor, nv2a_gl_fog.color );
	nv2a_gl_fog.start = nv2a_ri_fog.fogStart;
	nv2a_gl_fog.end = nv2a_ri_fog.fogEnd;

	/* pglEnable/pglDisable( GL_FOG ) by fogCustom (gl_triapi.c:279-282) */
	nv2a_gl_fog_enable( nv2a_ri_fog.fogCustom );
}

/* TriFogParams, gl_triapi.c:328. */
void R_WarpTriFogParams( float flDensity, int iFogSkybox )
{
	nv2a_ri_fog.fogDensity = flDensity;
	nv2a_ri_fog.fogSkybox = iFogSkybox;
}
