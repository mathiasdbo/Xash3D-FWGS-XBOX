/*
r_sky.c - X9-X item 14 step 10: the skybox for ref/nv2a. A port of the
skybox half of ref/gl/gl_warp.c (root clone c7768c48) and of R_SetupSky
(gl_context.c:328), drawn through the TriAPI internals in r_context.c
(Begin/TexCoord2f/Vertex3fv/End, the nv2a_2d_desired shadow) the way
r_sprite.c and r_effects.c draw.

How it fits together, as in the reference:
- The engine finds gfx/env/<skyname>{rt,bk,lf,ft,up,dn}.{dds,tga,bmp}
  (CheckSkybox, ref_common.c:71 - .dds first), loads each side through
  GL_LoadTexture( TF_CLAMP|TF_SKY ) and hands the six texnums to
  R_SetupSky (ref_common.c:103-150). This file keeps them, and frees them
  on the next R_SetupSky( NULL ) (R_UnloadSkybox). Before this step
  ref/nv2a's R_SetupSky was empty: the sides were uploaded on every map
  and never drawn nor freed, so a map with a different skyname stranded
  the previous set (1.5 MiB at RGBA32).
- Each frame the world walk hands every visible SURF_DRAWSKY world face
  to R_AddSkyBoxSurface, which clips it (relative to the eye) against the
  six planes that split space into the cube's sides and grows each side's
  s/t bounds (ClipSkyPolygon/DrawSkyPolygon, nv2a_sky_util.c).
- R_DrawSkyBox draws, for every side that received something, one quad
  over those bounds on a box centred on the eye (MakeSkyVec).

Where it departs from ref/gl (docs/r3d/divergences.md #137):
- When and how deep. ref/gl draws the box after the world, depth-tested,
  at half its far clip (R_DrawWorld, gl_rsurf.c:3718; farClip = zmax *
  1.73, gl_rmain.c:332), inside a projection whose zFar is that same
  farClip (gl_rmain.c:385). ref/nv2a's projection has a fixed zFar of 8192
  (r_context.c), so on maps whose MaxRange puts farClip/2 past what fits
  (c2a5 12000, the Xen maps 8000) the reference's box would be clipped,
  and a box pulled in to fit would paint over world geometry between it
  and 8192. So r_context.c draws the box BEFORE the world, with the depth
  test and depth writes off, and everything drawn afterwards occludes it:
  the box behaves as if infinitely far. The size of the box then only
  decides near/far clipping, not the picture, and is capped to fit
  (NV2A_SKY_MAX_FARCLIP). The difference from ref/gl: geometry (or a
  model) farther than the reference's farClip/2 in a sky direction shows
  in front of the sky here, where ref/gl's box would cover it.
- No sky sphere. The Quake-style sky (FWORLD_SKYSPHERE without a custom
  skybox: R_DrawClouds, the polygon pass at the top of
  R_AddSkyBoxSurface) is not ported. The engine sets the flag when the
  world's "sky" texture is 2:1 and loads as indexed (Mod_InitSkyClouds,
  mod_bmodel.c:2873 -> :2852), and the engine's PARM_SKY_SPHERE only
  answers true without a custom skybox - a Half-Life map always has one
  (R_SetupSky falls back to "desert"). In that case nothing is drawn here;
  ref/gl draws clouds and no box.
- No fog (#119), no skybox texnum slots (SKYBOX_BASE_NUM, gl_image.c:1224:
  a GL texture-name trick for mods, nothing to reserve here).
*/
#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR */
#include "triangleapi.h"
#include "pmove.h"       /* movevars_s (gp_movevars->zmax, R_GetFarClip) */
#include "nv2a_texture.h"
#include "nv2a_sky_util.h"
#include "r_studio_bridge.h"
#include "r_sky.h"

STATIC_ASSERT( NV2A_SKY_SIDES == SKYBOX_MAX_SIDES, "nv2a_sky_util.h and ref_api.h disagree on the side count" );

/* r_context.c's projection zFar is 8192 (nv2a_matrix_perspective( ...,
   1.0f, 8192.0f )). A box of half-size h reaches h * sqrt(3) from the eye
   at its corners; 9400 >> 1 = 4700 puts them at 8141, inside it. */
#define NV2A_SKY_MAX_FARCLIP 9400

static int               nv2a_skybox_textures[SKYBOX_MAX_SIDES]; /* tr.skyboxTextures, gl_local.h:189 */
static nv2a_sky_bounds_t nv2a_sky_bounds;                        /* RI.skyMins/skyMaxs */
static vec3_t            nv2a_sky_cullorigin;                    /* RI.cullorigin */
static qboolean          nv2a_sky_has_surfaces;                  /* skychain != NULL */
static qboolean          nv2a_sky_warned_verts;

/* (h * sqrt(3))^2 < zFar^2, in integers: 3 * 4700^2 = 66270000 < 8192^2 = 67108864. */
STATIC_ASSERT( 3LL * ( NV2A_SKY_MAX_FARCLIP >> 1 ) * ( NV2A_SKY_MAX_FARCLIP >> 1 ) < 8192LL * 8192LL, "the sky box's corners must stay inside the projection's zFar" );

void R_UnloadSkybox( void )
{
	int i;

	/* release old skybox */
	for( i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( !nv2a_skybox_textures[i] )
			continue;
		GL_FreeTexture( (unsigned int)nv2a_skybox_textures[i] );
	}

	memset( nv2a_skybox_textures, 0, sizeof( nv2a_skybox_textures ));
	/* ref/gl also clears FWORLD_CUSTOM_SKYBOX here (gl_warp.c:324); its
	   only reader is PARM_SKY_SPHERE (cl_render.c:147), whose answer only
	   differs with FWORLD_SKYSPHERE set. The engine sets the bit again once
	   a new set loads (ref_common.c:148); if a load fails and even the
	   "desert" fallback is missing, ref/gl then leaves it clear and draws
	   the sphere where this port keeps it set and draws the gray stub
	   (divergences.md #137). */
}

void R_SetupSky( int *skyboxTextures )
{
	int i;

	R_UnloadSkybox();

	if( !skyboxTextures )
		return;

	for( i = 0; i < SKYBOX_MAX_SIDES; i++ )
		nv2a_skybox_textures[i] = skyboxTextures[i];
}

void R_SkyShutdown( void )
{
	memset( nv2a_skybox_textures, 0, sizeof( nv2a_skybox_textures ));
	nv2a_sky_has_surfaces = false;
}

int R_SkyboxTexture( int side )
{
	/* gl_context.c:182 Asserts the range; answer 0 instead of reading past
	   the table. */
	if( side < 0 || side >= SKYBOX_MAX_SIDES )
		return 0;
	return nv2a_skybox_textures[side];
}

void R_ClearSkyBox( const vec3_t cullorigin )
{
	nv2a_sky_clear( &nv2a_sky_bounds );
	VectorCopy( cullorigin, nv2a_sky_cullorigin );
	nv2a_sky_has_surfaces = false;
}

qboolean R_SkyHasSurfaces( void )
{
	return nv2a_sky_has_surfaces;
}

/* R_GetEdgePosition, gl_rsurf.c:163 - the vertex GL_BuildPolygonFromSurface
   gives a face's poly (gl_rsurf.c:417). A Half-Life face's single poly is
   exactly its edge loop; ref/gl only subdivides SURF_DRAWTURB faces. */
static void nv2a_sky_edge_position( const model_t *mod, const msurface_t *fa, int i, vec3_t vec )
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

void R_AddSkyBoxSurface( const model_t *mod, const msurface_t *fa )
{
	/* One more than a polygon's points: ClipSkyPolygon writes a copy of
	   the first point past the last (gl_warp.c:179). */
	static float verts[NV2A_SKY_MAX_CLIP_VERTS + 1][3];
	int i;

	if( !mod || !fa || !mod->surfedges || !mod->vertexes )
		return;

	if( fa->numedges > NV2A_SKY_MAX_CLIP_VERTS || fa->numedges < 3 )
	{
		/* ref/gl calls Host_Error here (gl_warp.c:138); a face is only
		   cosmetic, so it is skipped and reported once. */
		if( !nv2a_sky_warned_verts )
		{
			gEngfuncs.Con_Printf( S_WARN "ref_nv2a: sky face with %d edges not clipped (limit %d)\n",
				fa->numedges, NV2A_SKY_MAX_CLIP_VERTS );
			nv2a_sky_warned_verts = true;
		}
		return;
	}

	/* calculate vertex values for sky box */
	for( i = 0; i < fa->numedges; i++ )
	{
		vec3_t pos;

		nv2a_sky_edge_position( mod, fa, i, pos );
		VectorSubtract( pos, nv2a_sky_cullorigin, verts[i] );
	}

	if( nv2a_sky_clip_polygon( &nv2a_sky_bounds, fa->numedges, verts[0] ))
		nv2a_sky_has_surfaces = true;
}

/* MakeSkyVec, gl_warp.c:221, emitted as TriTexCoord2f + TriVertex3fv. */
static void nv2a_sky_emit_vec( float s, float t, int axis, int farclip )
{
	float pos[3], st[2];

	nv2a_sky_make_vec( s, t, axis, farclip, nv2a_sky_cullorigin, pos, st );
	TexCoord2f( st[0], st[1] );
	Vertex3fv( pos );
}

void R_DrawSkyBox( void )
{
	nv2a_2d_shadow_t saved = nv2a_2d_desired;
	int farclip, i;

	/* RI.farClip = R_GetFarClip() (gl_rmain.c:332: the world is being
	   drawn, so zmax * 1.73), truncated to an int as MakeSkyVec does, and
	   capped to the NV2A projection (header, #137). */
	farclip = (int)( gp_movevars->zmax * 1.73f );
	if( farclip > NV2A_SKY_MAX_FARCLIP )
		farclip = NV2A_SKY_MAX_FARCLIP;

	/* gl_warp.c:340-342: blend and alpha test off, GL_REPLACE (the vertex
	   colour is white, so the TriAPI's modulate is the same). #137: depth
	   test and writes off, so the world drawn next covers it. Cull off:
	   the box is only ever seen from inside, whatever its winding. */
	nv2a_2d_desired.blend_enable = false;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.depth_mask = false;
	nv2a_2d_desired.depth_test_disable = true;
	nv2a_2d_desired.depth_func_equal = false;
	nv2a_2d_desired.cull_enable = false;
	Color4ub( 255, 255, 255, 255 );

	for( i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		int texnum;

		if( nv2a_sky_side_empty( &nv2a_sky_bounds, i ))
			continue;

		texnum = nv2a_skybox_textures[nv2a_sky_tex_order[i]];
		if( texnum )
			GL_Bind( 0, (unsigned int)texnum );
		else
			GL_Bind( 0, nv2a_tex_gray()); /* stub */

		Begin( TRI_QUADS );
		nv2a_sky_emit_vec( nv2a_sky_bounds.mins[0][i], nv2a_sky_bounds.mins[1][i], i, farclip );
		nv2a_sky_emit_vec( nv2a_sky_bounds.mins[0][i], nv2a_sky_bounds.maxs[1][i], i, farclip );
		nv2a_sky_emit_vec( nv2a_sky_bounds.maxs[0][i], nv2a_sky_bounds.maxs[1][i], i, farclip );
		nv2a_sky_emit_vec( nv2a_sky_bounds.maxs[0][i], nv2a_sky_bounds.mins[1][i], i, farclip );
		End();
	}

	/* The queued quads were appended under the state above (the batch
	   snapshot the flush pushes); restore the shadow so nothing drawn
	   after inherits the sky's depth-off state. */
	nv2a_2d_desired = saved;
}
