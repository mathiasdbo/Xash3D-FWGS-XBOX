/*
r_lightmap.c - X9-X step 8: animated lightstyles and dynamic lights for
ref/nv2a's world and brush entities.

One mechanism, as docs/r3d/fork-plan.md item 14 step 8 puts it: a face's
lightmap is rebuilt from msurface_t::samples (every style is in the
classic prefix's LUMP_LIGHTING, which the engine keeps: Mod_LoadLighting
copies it into the model's mempool, this tree's
engine/common/mod_bmodel.c:3968-4044; :3883 in the root clone) times
g_lightstylevalue, plus the dynamic lights that reach it, and written
into the lightmap atlas the baker made (R_CheckLightMap, gl_rsurf.c:1400).

Ported from the root clone:
- R_CheckLightMap (gl_rsurf.c:1400): rebuild when a style's value differs
  from cached_light, or when a dynamic light reaches the face this frame.
- R_BuildLightMap (gl_rsurf.c:725) and R_AddDynamicLights
  (gl_rsurf.c:569): nv2a_lightmap_util.c holds their arithmetic; this file
  feeds them the engine's surface data.
- R_MarkLights / R_PushDlights (ref/common/ref_light.c:94, :170), copied
  so the world pass also learns WHICH faces a light reached - the ref/common
  version only stamps msurface_t::dlightframe, and finding those stamps
  again would mean walking every face of the map each frame.
- R_PushDlightsForBmodel (ref_light.c:188) is called as is, for brush
  entities, whose faces are few enough to walk.

Where it differs (divergences.md #134):
- In place. ref/gl sends a face with a flickering style (1-31 but 20) or
  a dynamic light to a separate lightmap texture rebuilt every frame
  (gl_lms.dynamic_surfaces, LM_UploadDynamicBlock) and draws it in its own
  pass; only switchable styles (0, 20, 32+) are written into the static
  atlas (gl_rsurf.c:1419-1449). ref/nv2a has one lightmap page and one
  world pass, so every rebuild goes into the atlas, and a block that took
  a dynamic light is rebuilt again, without it, once the light no longer
  reaches it (lm_held below) - upstream's static block was never touched.
  Because the block keeps what it was built with, a flickering style is
  rebuilt only when its value changes, not every frame.
- Visibility. ref/gl rebuilds the faces it draws. Here the world faces
  are the ones r_context.c's own world pass draws (PVS and frustum, before
  its backface test), and a brush entity's faces when the entity is drawn
  lit (R_HasLightmap). A face that is not drawn keeps a stale block until
  it is; a stale DYNAMIC light is the one exception, restored whether
  drawn or not, so no light is ever left behind in the atlas.
- Style 0. A face whose only style is 0 is not in the per-frame list
  (checking every face of the map each frame is the cost this file
  exists to avoid); when style 0's value changes (r_fullbright,
  ref_light.c:38-43), every block of the map is rebuilt once instead.
- The texel scale and no LightToTexGamma: nv2a_lightmap_util.h.
- No litwater branch (gl_rsurf.c:776-782). Without FWORLD_HAS_LITWATER it
  is the identity (Mod_LightmappedWaterScale 1, minlight 0,
  gl_rsurf.c:89-110), and a TEX_SPECIAL face is SURF_DRAWTILED
  (mod_bmodel.c:3373-3374 in the root clone), which has no block here.
- A per-frame budget (NV2A_LM_FRAME_LUXELS): past it, styled and dynamic
  faces wait for a later frame. ref/gl has none; its dynamic lightmap
  is rebuilt whole every frame.
- A block still holding a dynamic light is restored when its face is next
  drawn, not at once, unless the list of such blocks is half full - the
  update runs before any draw, so the stale light is never seen.

When: GL_RenderFrame, before the world draw. R_EndFrame spins on
pb_busy() and R_BeginFrame on pb_busy() again after its clears, so the
GPU has finished every draw of the previous frame; nothing pushed since
samples the atlas (2D only; its first flush of a frame turns stage 1
off), and this runs before this frame's first world or brush draw. The
atlas is write-combined memory (r_context.c nv2a_upload_map_gpu_data):
the words are written whole, in address order, and never read back. What
makes them visible to the GPU is pbkit itself - every pb_end() runs
pb_cache_flush() (sfence, then the NV_PFB_WC_CACHE flush,
deps/nxdk/lib/pbkit/pbkit.c:264-271, 1477-1478) before it moves the put
pointer, and the first pb_end() after this is the world's own state setup.
Not known: whether the NV2A's texture cache can keep stale atlas texels
across the frame boundary on real hardware. No texture-cache invalidate
method is defined in nxdk or the xemu sources, and xemu cannot show it
(it re-hashes a texture whose pages the CPU dirtied); r_image.c's
GL_UpdateTexture has the same exposure. Real hardware decides.

The Xbox objects are built at clang's default -O0 (tools/xbox-cc.sh
passes no -O), which is why the inner loops (nv2a_lightmap_util.c) avoid
calls and memset.
*/
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR */
#include "bspfile.h"     /* TEX_WORLD_LUXELS, TEX_EXTRA_LIGHTMAP, SURF_DRAWTILED */
#include "nv2a_lightmap_util.h"
#include "r_lightmap.h"
#include "nv2a_ft.h" /* frame-time work counters (divergences.md #141) */

/* engine/common/mod_local.h:24-25 (not visible to ref modules; ref/gl
   keeps its own copy of the first, gl_local.h:54). */
#define NV2A_LM_SAMPLE_SIZE      16
#define NV2A_LM_SAMPLE_EXTRASIZE 8

/* How many faces the per-frame lists hold. An explosion's light
   (radius ~300) reaches a few hundred faces; past this the extra faces
   simply get no dynamic light that frame (counted in list_overflows). */
#define NV2A_LM_MAX_LISTED 2048

/* Luxels rebuilt per frame, at most, for styles and dynamic lights. An
   explosion's light reaching ~300 faces of ~40 luxels fits (~12k); a
   switch turning ~400 faces takes a frame or two. At -O0 a luxel costs
   on the order of 100 P6 cycles (review estimate, not measured on the
   console), so this bounds the update to about 2-3 ms of a 733 MHz frame.
   Restoring a drawn block that holds a stale light, and the whole-map
   rebuild when style 0 changes, are never deferred. */
#define NV2A_LM_FRAME_LUXELS 16384

nv2a_lm_stats_t nv2a_lm_stats;

/* r_blocklights (gl_rsurf.c), sized for the baker's largest block. */
static uint32_t nv2a_lm_blocklights[NV2A_LM_MAX_LUXELS * 3];

/* g_lightstylevalue rescaled (nv2a_lm_normalized_scale), recomputed for
   a style only when its value changed (nv2a_lm_nscale_of; both zero at
   start, and nv2a_lm_normalized_scale( 0 ) is 0). */
static int nv2a_lm_nscale[MAX_LIGHTSTYLES];
static int nv2a_lm_nscale_of[MAX_LIGHTSTYLES];

/* Per-map state, built on the first update for a world. */
static const model_t *lm_world;   /* the world the state below was built for */
static uint8_t  *lm_held;         /* [numsurfaces], ORIGINAL order: 1 = the block holds a dynamic light */
static uint16_t *lm_styled;       /* ORIGINAL indices of world faces with a style other than 0 */
static uint32_t  lm_styled_count;
static int       lm_style0;       /* the style-0 value every block not in lm_styled was last built with */
static uint32_t  lm_styled_cursor; /* where the styled walk starts, so a budget cut never starves the tail */
static int       lm_budget;       /* luxels left this frame */
static qboolean  lm_styles_run;   /* CL_RunLightStyles has filled g_lightstylevalue for this map */

static uint16_t  lm_held_list[NV2A_LM_MAX_LISTED]; /* the faces lm_held marks */
static uint32_t  lm_held_count;
static uint16_t  lm_dlit[NV2A_LM_MAX_LISTED];      /* world faces a light reached this frame */
static uint32_t  lm_dlit_count;

/* This frame's inputs, for the helpers below. */
static const nv2a_lm_view_t *lm_view;

static inline uint32_t nv2a_lm_rdtsc( void )
{
	uint32_t lo, hi;

	__asm__ __volatile__( "rdtsc" : "=a"( lo ), "=d"( hi ));
	return lo;
}

void R_LightmapRunLightStyles( lightstyle_t *ls )
{
	CL_RunLightStyles( ls );
	lm_styles_run = true;
}

static void nv2a_lm_free_tables( void )
{
	if( lm_held ) { free( lm_held ); lm_held = NULL; }
	if( lm_styled ) { free( lm_styled ); lm_styled = NULL; }
	lm_styled_count = 0;
	lm_held_count = 0;
	lm_dlit_count = 0;
	lm_styled_cursor = 0;
	lm_world = NULL;
	memset( &nv2a_lm_stats, 0, sizeof( nv2a_lm_stats ));
}

void R_LightmapFreeMap( void )
{
	nv2a_lm_free_tables();
	lm_styles_run = false; /* the next map's first CL_RunLightStyles re-arms it */
}

static qboolean nv2a_lm_face_is_world( const model_t *world, uint32_t orig )
{
	return orig >= (uint32_t)world->firstmodelsurface
		&& orig < (uint32_t)world->firstmodelsurface + (uint32_t)world->nummodelsurfaces;
}

/* Checks every baked block against the surface the engine loaded for the
   same face and resets cached_light to what the baked block holds. A
   block whose size or place disagrees is dropped - rewriting it could
   only spill into a neighbour's luxels. */
static qboolean nv2a_lm_init_map( const nv2a_lm_view_t *view )
{
	model_t *world = view->world;
	uint32_t orig, styled = 0, rejected = 0;

	nv2a_lm_free_tables();

	if( world->numsurfaces <= 0 || (uint32_t)world->numsurfaces != view->remap_count || world->numsurfaces > 0xFFFF )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: %d surfaces vs a %u-face remap - no animated or dynamic lights on this map\n",
			world->numsurfaces, (unsigned int)view->remap_count );
		lm_world = world; /* do not retry every frame */
		return false;
	}

	for( orig = 0; orig < (uint32_t)world->numsurfaces; orig++ )
	{
		msurface_t *surf = &world->surfaces[orig];
		const uint32_t sorted = view->remap[orig];
		nv2a_lm_block_t *blk;
		int sample_size, smax, tmax;

		if( sorted >= view->face_count )
			continue;

		blk = &view->blocks[sorted];
		if( blk->x == NV2A_LM_BLOCK_NONE )
			continue;

		if( !surf->samples || !surf->info || surf->styles[0] == NV2A_LM_NO_STYLE || FBitSet( surf->flags, SURF_DRAWTILED ))
		{
			blk->x = NV2A_LM_BLOCK_NONE;
			rejected++;
			continue;
		}

		sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
		smax = ( surf->info->lightextents[0] / sample_size ) + 1;
		tmax = ( surf->info->lightextents[1] / sample_size ) + 1;

		if( smax != blk->smax || tmax != blk->tmax || smax > NV2A_LM_MAX_LUXELS_PER_AXIS || tmax > NV2A_LM_MAX_LUXELS_PER_AXIS
			|| (uint32_t)blk->x + (uint32_t)smax > view->atlas_width || (uint32_t)blk->y + (uint32_t)tmax > view->atlas_height )
		{
			blk->x = NV2A_LM_BLOCK_NONE;
			rejected++;
			continue;
		}

		nv2a_lm_baked_cached( surf->styles, surf->cached_light );

		if( nv2a_lm_face_is_world( world, orig ))
		{
			int i;

			for( i = 0; i < NV2A_LM_STYLE_SLOTS && surf->styles[i] != NV2A_LM_NO_STYLE; i++ )
			{
				if( surf->styles[i] != 0 )
				{
					styled++;
					break;
				}
			}
		}
	}

	lm_held = calloc( (size_t)world->numsurfaces, 1 );
	lm_styled = styled ? malloc( (size_t)styled * sizeof( *lm_styled )) : NULL;
	if( !lm_held || ( styled && !lm_styled ))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_nv2a: no memory for the lightmap tables - no animated or dynamic lights on this map\n" );
		nv2a_lm_free_tables();
		lm_world = world;
		return false;
	}

	for( orig = 0; orig < (uint32_t)world->numsurfaces && lm_styled_count < styled; orig++ )
	{
		const msurface_t *surf = &world->surfaces[orig];
		const uint32_t sorted = view->remap[orig];
		int i;

		if( sorted >= view->face_count || view->blocks[sorted].x == NV2A_LM_BLOCK_NONE || !nv2a_lm_face_is_world( world, orig ))
			continue;

		for( i = 0; i < NV2A_LM_STYLE_SLOTS && surf->styles[i] != NV2A_LM_NO_STYLE; i++ )
		{
			if( surf->styles[i] != 0 )
			{
				lm_styled[lm_styled_count++] = (uint16_t)orig;
				break;
			}
		}
	}

	lm_world = world;
	lm_style0 = NV2A_LM_NORMAL_STYLE_VALUE; /* the baked blocks */
	nv2a_lm_stats.styled_faces = lm_styled_count;
	nv2a_lm_stats.rejected_blocks = rejected;

	if( rejected )
		gEngfuncs.Con_Reportf( "ref_nv2a: %u lightmap block(s) do not match their surface and stay static\n", (unsigned int)rejected );

	return true;
}

/* R_AddDynamicLights, gl_rsurf.c:569-660, up to the falloff
   (nv2a_lm_add_dlight). `object` is RI.objectMatrix for a brush entity,
   NULL for the world (tr.modelviewIdentity). */
static void nv2a_lm_add_dynamic_lights( const msurface_t *surf, float sample_size, int smax, int tmax, const matrix4x4 *object )
{
	const mextrasurf_t *info = surf->info;
	const mtexinfo_t *tex = surf->texinfo;
	int sample_frac = 1.0;
	int lnum;

	// no dlighted surfaces here
	if( !surf->dlightbits )
		return;

	if( FBitSet( tex->flags, TEX_WORLD_LUXELS ))
	{
		if( tex->faceinfo )
			sample_frac = tex->faceinfo->texture_step;
		else if( FBitSet( tex->flags, TEX_EXTRA_LIGHTMAP ))
			sample_frac = NV2A_LM_SAMPLE_EXTRASIZE;
		else sample_frac = NV2A_LM_SAMPLE_SIZE;
	}

	for( lnum = 0; lnum < MAX_DLIGHTS; lnum++ )
	{
		vec3_t impact, origin_l;
		const dlight_t *dl;
		float rad, dist, sl, tl;

		if( !FBitSet( surf->dlightbits, BIT( lnum )))
			continue; // not lit by this light

		dl = &gp_dlights[lnum];

		// transform light origin to local bmodel space
		if( object )
			Matrix4x4_VectorITransform( *object, dl->origin, origin_l );
		else VectorCopy( dl->origin, origin_l );

		rad = dl->radius;
		dist = PlaneDiff( origin_l, surf->plane );
		rad -= fabs( dist );

		if( surf->plane->type < 3 )
		{
			VectorCopy( origin_l, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( origin_l, -dist, surf->plane->normal, impact );

		sl = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		tl = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];

		nv2a_lm_add_dlight( nv2a_lm_blocklights, smax, tmax, sample_size, sample_frac,
			sl, tl, rad, dl->minlight, dl->color.r, dl->color.g, dl->color.b );
	}
}

/* R_BuildLightMap (gl_rsurf.c:725) into the face's atlas block, then
   R_UpdateSurfaceCachedLight (ref_light.c:692) as R_CheckLightMap does
   after it (gl_rsurf.c:1438). `dynamic` adds the lights that reached the
   face this frame. The caller has checked the block (nv2a_lm_block_of). */
static void nv2a_lm_build( msurface_t *surf, const nv2a_lm_block_t *blk, qboolean dynamic, const matrix4x4 *object )
{
	const int smax = blk->smax;
	const int tmax = blk->tmax;
	const int size = smax * tmax;
	uint32_t *dst;

	nv2a_lm_accumulate_styles( nv2a_lm_blocklights, (const uint8_t *)surf->samples, size, surf->styles, nv2a_lm_nscale );

	// add all the dynamic lights
	if( dynamic && surf->dlightframe == lm_view->framecount )
		nv2a_lm_add_dynamic_lights( surf, (float)gEngfuncs.Mod_SampleSizeForFace( surf ), smax, tmax, object );

	dst = lm_view->atlas + (size_t)blk->y * lm_view->atlas_stride_words + blk->x;
	nv2a_lm_pack_block( dst, lm_view->atlas_stride_words, nv2a_lm_blocklights, smax, tmax );

	nv2a_lm_update_cached( surf->styles, surf->cached_light, g_lightstylevalue );

	nv2a_lm_stats.faces_built++;
	nv2a_lm_stats.luxels_built += (uint32_t)size;
	NV2A_FT_ADD( XBOX_FT_W_LM_FACES, 1 );
	NV2A_FT_ADD( XBOX_FT_W_LM_TEXELS, size );
	lm_budget -= size;
}

/* True when this frame's luxel budget cannot take the block. */
static qboolean nv2a_lm_over_budget( const nv2a_lm_block_t *blk )
{
	if( lm_budget >= (int)blk->smax * (int)blk->tmax )
		return false;

	nv2a_lm_stats.budget_skips++;
	return true;
}

/* The face's block, or NULL when it has none. */
static const nv2a_lm_block_t *nv2a_lm_block_of( uint32_t orig, uint32_t *sorted_out )
{
	uint32_t sorted;

	if( orig >= lm_view->remap_count )
		return NULL;

	sorted = lm_view->remap[orig];
	if( sorted >= lm_view->face_count || lm_view->blocks[sorted].x == NV2A_LM_BLOCK_NONE )
		return NULL;

	if( sorted_out )
		*sorted_out = sorted;
	return &lm_view->blocks[sorted];
}

/* Builds a face with its dynamic lights and remembers that its block now
   holds them. With no room left to remember it, the face is built
   without them instead - never a light nobody would take back out. */
static void nv2a_lm_build_dlit( msurface_t *surf, uint32_t orig, const nv2a_lm_block_t *blk, const matrix4x4 *object )
{
	if( !lm_held[orig] )
	{
		if( lm_held_count >= NV2A_LM_MAX_LISTED )
		{
			nv2a_lm_stats.list_overflows++;
			nv2a_lm_build( surf, blk, false, NULL );
			return;
		}
		lm_held[orig] = 1;
		lm_held_list[lm_held_count++] = (uint16_t)orig;
	}

	nv2a_lm_build( surf, blk, true, object );
}

/* R_MarkLights, ref/common/ref_light.c:94-164, for the world: also
   records each face the first time a light reaches it this frame. */
static void nv2a_lm_mark_lights( const dlight_t *light, int bit, const mnode_t *node, model_t *model, int dlightframecount )
{
	const float virtual_radius = light->radius * Q_max( 1.0f, r_dlight_virtual_radius.value );
	const float maxdist = light->radius * light->radius;
	float dist, dist_sq;
	int firstsurface, numsurfaces, i;
start:
	if( !node || node->contents < 0 )
		return;

	dist = PlaneDiff( light->origin, node->plane );

	if( dist > virtual_radius )
	{
		node = node_child( node, 0, model );
		goto start;
	}

	if( dist < -virtual_radius )
	{
		node = node_child( node, 1, model );
		goto start;
	}

	dist_sq = dist * dist;

	// mark the polygons
	firstsurface = node_firstsurface( node, model );
	numsurfaces = node_numsurfaces( node, model );

	for( i = 0; i < numsurfaces && dist_sq < maxdist; i++ )
	{
		vec3_t impact;
		float s, t, l;
		msurface_t *surf = &model->surfaces[firstsurface + i];
		const mextrasurf_t *info = surf->info;

		if( surf->plane->type < 3 )
		{
			VectorCopy( light->origin, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( light->origin, -dist, surf->plane->normal, impact );

		// a1ba: the fix was taken from JoeQuake, which traces back to FitzQuake,
		// which attributes it to LadyHavoc (Darkplaces author)
		// clamp center of light to corner and check brightness
		l = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		s = l + 0.5;
		s = bound( 0, s, info->lightextents[0] );
		s = l - s;

		l = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
		t = l + 0.5;
		t = bound( 0, t, info->lightextents[1] );
		t = l - t;

		if( s * s + t * t + dist_sq >= maxdist )
			continue;

		if( surf->dlightframe != dlightframecount )
		{
			surf->dlightbits = bit;
			surf->dlightframe = dlightframecount;

			/* ref/nv2a: the list the world's dynamic pass walks. */
			if( lm_dlit_count < NV2A_LM_MAX_LISTED )
				lm_dlit[lm_dlit_count++] = (uint16_t)( firstsurface + i );
			else
				nv2a_lm_stats.list_overflows++;
		}
		else surf->dlightbits |= bit;
	}

	nv2a_lm_mark_lights( light, bit, node_child( node, 0, model ), model, dlightframecount );
	node = node_child( node, 1, model );
	goto start;
}

/* R_PushDlights, ref_light.c:170-186, through the recording copy above.
   Returns how many lights are alive. */
static int nv2a_lm_push_dlights( model_t *model, int framecount )
{
	int i, live = 0;

	for( i = 0; i < MAX_DLIGHTS; i++ )
	{
		const dlight_t *l = &gp_dlights[i];

		if( l->die < gp_cl->time || !l->radius )
			continue;

		live++;
		nv2a_lm_mark_lights( l, 1 << i, model->nodes, model, framecount );
	}

	return live;
}

/* Style 0 moved away from what the blocks outside lm_styled were built
   with: rebuild every block of the map once, without dynamic lights
   (this frame's dynamic pass puts them back where they still reach). */
static void nv2a_lm_rebuild_all( model_t *world )
{
	uint32_t orig;

	for( orig = 0; orig < (uint32_t)world->numsurfaces; orig++ )
	{
		const nv2a_lm_block_t *blk = nv2a_lm_block_of( orig, NULL );

		if( blk )
			nv2a_lm_build( &world->surfaces[orig], blk, false, NULL );
		lm_held[orig] = 0;
	}

	lm_held_count = 0;
	lm_style0 = g_lightstylevalue[0];
	nv2a_lm_stats.full_rebuilds++;
}

/* RI.objectMatrix for a brush entity: R_RotateForEntity /
   R_TranslateForEntity (gl_rmain.c:448-492) with scale 1 (a brush model's
   curstate.scale is ignored there). */
static void nv2a_lm_entity_matrix( const cl_entity_t *e, matrix4x4 out )
{
	if( !VectorIsNull( e->angles ))
		Matrix4x4_CreateFromEntity( out, e->angles, e->origin, 1.0f );
	else
		Matrix4x4_CreateFromEntity( out, vec3_origin, e->origin, 1.0f );
}

void R_LightmapUpdate( const nv2a_lm_view_t *view, cl_entity_t **brush, int num_brush )
{
	model_t *world;
	const qboolean dynamic = r_dynamic->value != 0.0f;
	uint32_t start, k, cycles;
	int i;

	if( !view || !view->world || !view->atlas || !view->blocks || !view->remap || !view->face_in_view )
		return;

	world = view->world;
	lm_view = view;

	if( world != lm_world )
	{
		if( !nv2a_lm_init_map( view ))
			return;
	}
	if( !lm_held )
		return; /* this map's init failed */

	if( !lm_styles_run )
		return; /* g_lightstylevalue is not this map's yet (R_LightmapRunLightStyles) */

	start = nv2a_lm_rdtsc();
	nv2a_lm_stats.faces_built = 0;
	nv2a_lm_stats.luxels_built = 0;
	lm_budget = NV2A_LM_FRAME_LUXELS;

	for( i = 0; i < MAX_LIGHTSTYLES; i++ )
	{
		if( g_lightstylevalue[i] != nv2a_lm_nscale_of[i] )
		{
			nv2a_lm_nscale_of[i] = g_lightstylevalue[i];
			nv2a_lm_nscale[i] = nv2a_lm_normalized_scale( g_lightstylevalue[i] );
		}
	}

	/* R_CheckLightMap: with r_dynamic off nothing is rebuilt
	   (gl_rsurf.c:1404-1405) - but a block still holding a dynamic light
	   is put back below, as ref/gl never had one in its atlas. */
	if( dynamic && g_lightstylevalue[0] != lm_style0 )
		nv2a_lm_rebuild_all( world );

	/* Which faces the lights reach: R_PushDlights for the world
	   (gl_rmain.c:957), R_PushDlightsForBmodel per brush entity
	   (gl_rsurf.c:1879). */
	lm_dlit_count = 0;
	if( dynamic && nv2a_lm_push_dlights( world, view->framecount ) > 0 )
	{
		for( i = 0; i < num_brush; i++ )
		{
			matrix4x4 object;

			nv2a_lm_entity_matrix( brush[i], object );
			R_PushDlightsForBmodel( brush[i]->model, view->framecount, object );
		}
	}
	nv2a_lm_stats.dlit_faces = lm_dlit_count;

	/* A block holding a light that no longer reaches it goes back to its
	   styles alone - a world face when it is next drawn (this runs before
	   any draw, so the stale light is never seen), a brush-entity face at
	   once, and every one of them once the list is half full, so it can
	   always take new lights. Never deferred for the budget. */
	{
		const qboolean restore_all = lm_held_count > NV2A_LM_MAX_LISTED / 2;

		for( k = 0; k < lm_held_count; )
		{
			const uint32_t orig = lm_held_list[k];
			msurface_t *surf = &world->surfaces[orig];
			uint32_t sorted = 0;
			const nv2a_lm_block_t *blk = nv2a_lm_block_of( orig, &sorted );

			if( dynamic && surf->dlightframe == view->framecount )
			{
				k++;
				continue;
			}

			if( blk && !restore_all && nv2a_lm_face_is_world( world, orig ) && !view->face_in_view( sorted ))
			{
				k++;
				continue;
			}

			if( blk )
				nv2a_lm_build( surf, blk, false, NULL );
			lm_held[orig] = 0;
			lm_held_list[k] = lm_held_list[--lm_held_count];
		}
	}

	if( !dynamic )
		goto done;

	/* World faces a light reaches, when drawn ("dlighted surfaces are
	   always dynamic", gl_rsurf.c:1407) - before the styled faces, so a
	   budget cut falls on a switch's slow sweep, not on a muzzle flash. A
	   face cut here keeps what its block holds this frame. */
	for( k = 0; k < lm_dlit_count; k++ )
	{
		const uint32_t orig = lm_dlit[k];
		const nv2a_lm_block_t *blk;
		uint32_t sorted;

		if( !nv2a_lm_face_is_world( world, orig ))
			continue;

		blk = nv2a_lm_block_of( orig, &sorted );
		if( blk && view->face_in_view( sorted ) && !nv2a_lm_over_budget( blk ))
			nv2a_lm_build_dlit( &world->surfaces[orig], orig, blk, NULL );
	}

	/* World faces whose styles changed, when drawn (R_CheckLightMap's
	   style loop, gl_rsurf.c:1410-1450). A face a light also reaches was
	   built with the current styles by the pass above. The walk starts
	   where the last budget cut stopped; a face cut keeps its old
	   cached_light and is rebuilt on a later frame. */
	for( k = 0; k < lm_styled_count; k++ )
	{
		const uint32_t at = ( lm_styled_cursor + k ) % lm_styled_count;
		const uint32_t orig = lm_styled[at];
		msurface_t *surf = &world->surfaces[orig];
		const nv2a_lm_block_t *blk;
		uint32_t sorted;

		if( surf->dlightframe == view->framecount )
			continue;
		if( !nv2a_lm_styles_changed( surf->styles, surf->cached_light, g_lightstylevalue ))
			continue;

		blk = nv2a_lm_block_of( orig, &sorted );
		if( !blk || !view->face_in_view( sorted ))
			continue;

		if( nv2a_lm_over_budget( blk ))
		{
			lm_styled_cursor = at;
			break;
		}
		nv2a_lm_build( surf, blk, false, NULL );
	}

	/* Brush entities drawn lit this frame: all their faces go through
	   R_CheckLightMap when drawn (R_DrawBrushModel -> R_RenderBrushPoly /
	   R_DrawVBO, gl_rsurf.c:1462, :3413). */
	for( i = 0; i < num_brush; i++ )
	{
		const model_t *clmodel = brush[i]->model;
		matrix4x4 object;
		int j;

		nv2a_lm_entity_matrix( brush[i], object );

		for( j = 0; j < clmodel->nummodelsurfaces; j++ )
		{
			const uint32_t orig = (uint32_t)( clmodel->firstmodelsurface + j );
			msurface_t *surf;
			const nv2a_lm_block_t *blk;

			if( orig >= (uint32_t)world->numsurfaces )
				break;

			blk = nv2a_lm_block_of( orig, NULL );
			if( !blk )
				continue;

			surf = &world->surfaces[orig];
			if( surf->dlightframe == view->framecount )
			{
				if( !nv2a_lm_over_budget( blk ))
					nv2a_lm_build_dlit( surf, orig, blk, (const matrix4x4 *)&object );
			}
			else if( nv2a_lm_styles_changed( surf->styles, surf->cached_light, g_lightstylevalue ) && !nv2a_lm_over_budget( blk ))
				nv2a_lm_build( surf, blk, false, NULL );
		}
	}

done:
	/* Belt and braces: pb_end's pb_cache_flush already drains the
	   write-combining buffers before any draw (file header). */
	if( nv2a_lm_stats.faces_built )
		__asm__ __volatile__( "sfence" ::: "memory" );

	cycles = nv2a_lm_rdtsc() - start;
	nv2a_lm_stats.cycles = cycles;
	if( cycles > nv2a_lm_stats.max_cycles )
		nv2a_lm_stats.max_cycles = cycles;
	nv2a_lm_stats.total_faces_built += nv2a_lm_stats.faces_built;
	nv2a_lm_stats.held_faces = lm_held_count;
	nv2a_lm_stats.frames++;
}
