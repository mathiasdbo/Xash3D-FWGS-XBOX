/*
r_studio.c - X9-X step 5: studio model renderer for ref/nv2a
(docs/r3d/design/x9-x-studio-models.md). Whole-function port of
ref/gl/gl_studio.c (function names and gl_studio.c:<line> citations kept
per function), routed through the TriAPI internals in r_context.c
(Begin/TexCoord2f/Color4ub/Vertex3fv/End/CullFace/TriRenderMode) exactly
as ref/soft/r_studio.c already routes the same reference through its own
TriAPI - see that file for the precedent this is built on.

Not part of any host test (studio.h/r_studioint.h and the mstudio*_t
walk are engine types, same reasoning as r_context.c itself); the
per-mesh render-state mapping this file calls into is host-tested
separately (nv2a_studio_util.c, tools/host-tests).
*/
#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR - not pulled in transitively here (r_context.c's own finding) */
#include "crtlib.h" /* Q_strncpy/Q_stricmp/Q_strcmp/Q_snprintf - not transitive here either */
#include "r_studioint.h"
#include "triangleapi.h"
#include "studio.h"
#include "protocol.h"
#include "enginefeatures.h"
#include "ref_params.h" /* RF_DRAW_WORLD/RF_DRAW_CUBEMAP - only forward-declared via ref_api.h otherwise (r_context.c's own reason for this include) */
#include "nv2a_texture.h"
#include "nv2a_studio_util.h"
#include "r_studio_bridge.h"
#include "r_studio.h"
#include "r_sprite.h" /* X9-X step 6: TriSpriteTexture (gl_triapi.c:242) */
#include <stdlib.h> /* calloc/free, R_StudioAllocStatics */
#include <string.h>
#include <math.h>

/* ref/soft/r_local.h:62's own SHADE_LAMBERT - a private renderer constant,
   not an engine one, so copied rather than included from anywhere. */
#define SHADE_LAMBERT 1.4953241

/* R_AllowFog (ref/soft/r_local.h:387, ref/gl's own R_AllowFog): an
   internal helper both those renderers implement for real (GL fog
   enable/disable around additive studio meshes); nv2a has no fog
   (TriRenderMode's own comment) so this is a pure no-op, called bare -
   NOT gEngfuncs.R_AllowFog, a completely different thing (ref_interface_t's
   own slot, the ENGINE's callback INTO this renderer, r_context.c's own
   stub for it). */
static void R_AllowFog( qboolean allow )
{
	(void)allow;
}

/* nv2a_studio_util.h's own mirrors of studio.h's STUDIO_NF_MASKED/ADDITIVE
   (kept host-buildable there without studio.h) pinned against the real
   constants - this file has studio.h. */
STATIC_ASSERT( NV2A_STUDIO_NF_MASKED == STUDIO_NF_MASKED, "NV2A_STUDIO_NF_MASKED drifted from studio.h" );
STATIC_ASSERT( NV2A_STUDIO_NF_ADDITIVE == STUDIO_NF_ADDITIVE, "NV2A_STUDIO_NF_ADDITIVE drifted from studio.h" );

/* pfnGetModelCounters's own small stats block - ref/gl's r_stats, scoped
   to just the two studio counters this file touches. */
static struct
{
	int c_studio_models_drawn;
	int c_studio_polys;
} nv2a_studio_stats;

#define EVENT_CLIENT    5000    // less than this value it is a server-side studio event
#define MAX_LOCALLIGHTS 4

typedef struct
{
	char    name[MAX_OSPATH];
	char    modelname[MAX_OSPATH];
	model_t *model;
} player_model_t;

CVAR_DEFINE_AUTO( r_shadows, "0", 0, "draw ugly shadows" );

typedef struct sortedmesh_s
{
	mstudiomesh_t *mesh;
	int           flags;                    // face flags
} sortedmesh_t;

/* Design note section 5 / census: the largest real submodel
   (max(numverts,numnorms)) across the build-test valve/models tree is
   1198 (garg.mdl); upstream ref/soft's own XASH_LOW_MEMORY value of 1024
   would overflow on three retail submodels (garg/nihilanth/loader all
   exceed it). 2048 is the smallest power of two above the measured
   maximum, with a per-submodel skip-and-log guard below
   (R_StudioDrawPoints) for anything that would still overflow it -
   nothing in the census does, but a mod's own custom model might. */
#undef MAXSTUDIOVERTS
#define MAXSTUDIOVERTS 2048

typedef struct
{
	double         time;
	double         frametime;
	int            framecount;              // studio framecount
	qboolean       interpolate;
	int            rendermode;
	float          blend;                   // blend value

	// bones
	matrix3x4      rotationmatrix;
	matrix3x4      bonestransform[MAXSTUDIOBONES];
	matrix3x4      lighttransform[MAXSTUDIOBONES];

	// boneweighting stuff
	matrix3x4      worldtransform[MAXSTUDIOBONES];

	// cached bones
	matrix3x4      cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4      cached_lighttransform[MAXSTUDIOBONES];
	char           cached_bonenames[MAXSTUDIOBONES][32];
	int            cached_numbones;                 // number of bones in cache

	sortedmesh_t   meshes[MAXSTUDIOMESHES];         // sorted meshes
	vec3_t         verts[MAXSTUDIOVERTS];
	vec3_t         norms[MAXSTUDIOVERTS];

	// lighting state
	float          ambientlight;
	float          shadelight;
	vec3_t         lightvec;                    // averaging light direction
	vec3_t         lightspot;                   // shadow spot
	vec3_t         lightcolor;                  // averaging lightcolor
	vec3_t         blightvec[MAXSTUDIOBONES];   // bone light vecs
	vec3_t         lightvalues[MAXSTUDIOVERTS]; // precomputed lightvalues per each shared vertex of submodel

	// chrome stuff
	vec3_t         chrome_origin;
	vec2_t         chrome[MAXSTUDIOVERTS];      // texture coords for surface normals
	vec3_t         chromeright[MAXSTUDIOBONES]; // chrome vector "right" in bone reference frames
	vec3_t         chromeup[MAXSTUDIOBONES];    // chrome vector "up" in bone reference frames
	int            chromeage[MAXSTUDIOBONES];   // last time chrome vectors were updated

	// glowshell stuff
	int            normaltable[MAXSTUDIOVERTS];     // glowshell uses this

	// elights cache
	int            numlocallights;
	int            lightage[MAXSTUDIOBONES];
	dlight_t       *locallight[MAX_LOCALLIGHTS];
	uint           locallightcolor[MAX_LOCALLIGHTS][3];
	vec4_t         lightpos[MAXSTUDIOVERTS][MAX_LOCALLIGHTS];
	vec3_t         lightbonepos[MAXSTUDIOBONES][MAX_LOCALLIGHTS];
	float          locallightR2[MAX_LOCALLIGHTS];

	// playermodels
	player_model_t player_models[MAX_CLIENTS];
} studio_draw_state_t;

/* Design note section 5: pinned against
   tools/asset-studio/src/formats/Nv2aMapFit.h's own kXboxStudioStateBytes,
   the same way nv2a_texture.h pins the texture table's own budget. Update
   both together if this struct's size ever changes (a MAXSTUDIOVERTS
   change, a new field...). */
STATIC_ASSERT( sizeof( studio_draw_state_t ) <= 384u * 1024u, "studio_draw_state_t exceeds Nv2aMapFit.h's kXboxStudioStateBytes budget" );

// studio-related cvars
CVAR_DEFINE_AUTO( r_studio_sort_textures, "0", FCVAR_GLCONFIG, "change draw order for additive meshes" );
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
static cvar_t *cl_righthand = NULL;

static r_studio_interface_t *pStudioDraw;
/* Xbox-only renderer: always heap-allocated (R_Init/R_Shutdown), following
   ref/soft's own XASH_XBOX branch (divergences.md #114,
   ref/soft/r_studio.c:113-125) - kept as an #if, not unconditional, so
   this file would still build (with a plain .bss instance) if it were
   ever pulled into a non-Xbox build. */
#if XASH_XBOX
static studio_draw_state_t  *g_studio_ptr;              // global studio state, R_AllocStudioStatics (divergences.md #114)
#define g_studio ( *g_studio_ptr )

qboolean R_StudioAllocStatics( void )
{
	g_studio_ptr = calloc( 1, sizeof( *g_studio_ptr ));
	return g_studio_ptr != NULL;
}

void R_StudioFreeStatics( void )
{
	free( g_studio_ptr );
	g_studio_ptr = NULL;
}
#else
static studio_draw_state_t  g_studio;                   // global studio state

qboolean R_StudioAllocStatics( void ) { return true; }
void     R_StudioFreeStatics( void )  { ; }
#endif

// global variables
static qboolean m_fDoRemap;
static mstudiomodel_t *m_pSubModel;
static mstudiobodyparts_t *m_pBodyPart;
static player_info_t *m_pPlayerInfo;
static studiohdr_t *m_pStudioHeader;
static float m_flGaitMovement;
static int g_nTopColor, g_nBottomColor;	// remap colors
static int g_nFaceFlags, g_nForceFaceFlags;

/* X9-X step 5 Part A: the small local context replacing ref/gl's RI/tr
   globals (this file's own header comment / design note section 0). Set
   once per frame (nv2a_studio_set_frame_context, from GL_RenderFrame) or
   once per drawn entity (nv2a_studio_begin_entity/nv2a_studio_set_blend,
   from r_context.c's own entity-list walk) - see r_studio.h. */
typedef struct
{
	cl_entity_t *currententity;
	model_t     *currentmodel;
	cl_entity_t *viewent;      // PARM_GET_VIEWENT_PTR, set once (R_StudioInit)
	dlight_t    *elights;      // PARM_GET_ELIGHTS_PTR, set once (R_StudioInit)
	uint        *lightgammatable;
	uint        *screengammatable;
	uint        *lineargammatable;
	float        blend;
	qboolean     fFlipViewModel;
	int          rvp_flags;
	vec3_t       vieworigin;
	vec3_t       vforward, vright, vup;
} nv2a_studio_ctx_t;

static nv2a_studio_ctx_t nv2a_studio_ctx;

void nv2a_studio_set_frame_context( int rvp_flags, const vec3_t vieworigin,
	const vec3_t vforward, const vec3_t vright, const vec3_t vup )
{
	nv2a_studio_ctx.rvp_flags = rvp_flags;
	VectorCopy( vieworigin, nv2a_studio_ctx.vieworigin );
	VectorCopy( vforward, nv2a_studio_ctx.vforward );
	VectorCopy( vright, nv2a_studio_ctx.vright );
	VectorCopy( vup, nv2a_studio_ctx.vup );
}

void nv2a_studio_begin_entity( cl_entity_t *e )
{
	nv2a_studio_ctx.currententity = e;
	nv2a_studio_ctx.currentmodel = e ? e->model : NULL;
}

void nv2a_studio_set_blend( float blend )
{
	nv2a_studio_ctx.blend = blend;
}

/* ref/soft/r_local.h:531-556's own gamma inlines, ported onto this file's
   own context struct instead of tr.*gammatable - same engine parms
   (PARM_GET_LIGHTGAMMATABLE_PTR and siblings), same
   ENGINE_LINEAR_GAMMA_SPACE bypass. TextureToGamma is not needed (never
   called from the studio path). */
static inline uint LightToTexGamma( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;
	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? nv2a_studio_ctx.lightgammatable[b] : b;
}

static inline uint ScreenGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;
	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? nv2a_studio_ctx.screengammatable[b] : b;
}

static inline uint LinearGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;
	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? nv2a_studio_ctx.lineargammatable[b] : b;
}

/* ref/soft/r_local.h's own small helpers this file also needs - CL_FxBlend
   and R_CullModel are r_context.c's own nv2a_fx_blend/nv2a_frustum_cull_box
   (r_studio_bridge.h); everything below is pure gp_cl/gp_host arithmetic,
   with no dependency on r_context.c's own state. */
#define R_ModelOpaque( rm ) ( ( rm ) == kRenderNormal )
#define CL_IsViewEntityLocalPlayer() ( gp_cl->viewentity == ( gp_cl->playernum + 1 ))

static inline model_t *CL_ModelHandle( int index )
{
	if( unlikely( index < 0 || index >= gp_cl->nummodels ))
		return NULL;
	return gp_cl->models[index];
}

static inline cl_entity_t *CL_GetEntityByIndex( int index )
{
	return nv2a_get_entity_by_index( index );
}

/* R_CullModel (ref/gl/gl_cull.c:43), ported onto nv2a_frustum_cull_box -
   R_StudioComputeBBox is the only caller (its own citation). */
static qboolean R_CullModel( cl_entity_t *e, const vec3_t mins, const vec3_t maxs )
{
	(void)e;
	return nv2a_frustum_cull_box( mins, maxs );
}

/*
====================
R_StudioInit

Also pulls the engine parms ref/soft's own R_Init pulls at the same point
in start-up (r_main.c:1378-1381) - viewent/elights/gamma tables - since
this renderer keeps them on its own local context instead of a shared
tr. Called once, from r_context.c's R_Init, after R_Init_Video/pb_init
(same ordering ref/soft uses; PARM_GET_VIEWENT_PTR etc. are only valid
once the engine's own client state exists, which is true by then).
====================
*/
void R_StudioInit( void )
{
	Matrix3x4_LoadIdentity( g_studio.rotationmatrix );

	/* Unlike ref/soft (comments this out, "cvar disabled by Valve"): the
	   X9-X step 5 spec explicitly asks for r_shadows registered at 0 -
	   GL_StudioDrawShadow's own stencil path stays out of scope, but the
	   cvar itself should exist for parity with a real HL client config. */
	gEngfuncs.Cvar_RegisterVariable( &r_shadows );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_sort_textures );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_builtin_renderer );
	cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	nv2a_studio_ctx.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	nv2a_studio_ctx.elights = (dlight_t *)ENGINE_GET_PARM( PARM_GET_ELIGHTS_PTR );
	nv2a_studio_ctx.lightgammatable = (uint *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	nv2a_studio_ctx.screengammatable = (uint *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR );
	nv2a_studio_ctx.lineargammatable = (uint *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR );

	g_studio.interpolate = true;
	g_studio.framecount = 0;
	m_fDoRemap = false;
}
/*
================
R_StudioSetupTimings

init current time for a given model
================
*/
static void R_StudioSetupTimings( void )
{
	if( FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ))
	{
		// synchronize with server time
		g_studio.time = gp_cl->time;
		g_studio.frametime = gp_cl->time - gp_cl->oldtime;
	}
	else
	{
		// menu stuff
		g_studio.time = gp_host->realtime;
		g_studio.frametime = gp_host->frametime;
	}
}

/*
================
R_AllowFlipViewModel

should a flip the viewmodel if cl_righthand is set to 1
================
*/
static qboolean R_AllowFlipViewModel( cl_entity_t *e )
{
	if( cl_righthand && cl_righthand->value > 0 )
	{
		if( e == nv2a_studio_ctx.viewent )
			return true;
	}

	return false;
}

/*
================
R_StudioComputeBBox

Compute a full bounding box for current sequence
================
*/
static qboolean R_StudioComputeBBox( vec3_t bbox[8] )
{
	vec3_t      studio_mins, studio_maxs;
	vec3_t      mins, maxs, p1, p2;
	cl_entity_t *e = nv2a_studio_ctx.currententity;

	if( !m_pStudioHeader )
		return false;

	// check if we have valid mins\maxs
	if( !VectorIsNull( nv2a_studio_ctx.currentmodel->mins ) && !VectorIsNull( nv2a_studio_ctx.currentmodel->maxs ))
	{
		// clipping bounding box
		VectorCopy( nv2a_studio_ctx.currentmodel->mins, mins );
		VectorCopy( nv2a_studio_ctx.currentmodel->maxs, maxs );
	}
	else
	{
		ClearBounds( mins, maxs );
	}

	// check sequence range
	if( e->curstate.sequence < 0 || e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + e->curstate.sequence;

	// add sequence box to the model box
	AddPointToBounds( pseqdesc->bbmin, mins, maxs );
	AddPointToBounds( pseqdesc->bbmax, mins, maxs );
	ClearBounds( studio_mins, studio_maxs );

	// compute a full bounding box
	for( int i = 0; i < 8; i++ )
	{
		p1[0] = ( i & 1 ) ? mins[0] : maxs[0];
		p1[1] = ( i & 2 ) ? mins[1] : maxs[1];
		p1[2] = ( i & 4 ) ? mins[2] : maxs[2];

		Matrix3x4_VectorTransform( g_studio.rotationmatrix, p1, p2 );
		AddPointToBounds( p2, studio_mins, studio_maxs );
		if( bbox )
			VectorCopy( p2, bbox[i] );
	}

	if( !bbox && R_CullModel( e, studio_mins, studio_maxs ))
		return false;  // model culled
	return true;           // visible
}

static void R_StudioComputeSkinMatrix( mstudioboneweight_t *boneweights, matrix3x4 result )
{
	float flWeight0, flWeight1, flWeight2, flWeight3;
	int   numbones = 0;
	float flTotal;

	for( int i = 0; i < MAXSTUDIOBONEWEIGHTS; i++ )
	{
		if( boneweights->bone[i] != -1 )
			numbones++;
	}

	if( numbones == 4 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		vec4_t *boneMat3 = (vec4_t *)g_studio.worldtransform[boneweights->bone[3]];
		flWeight0 = boneweights->weight[0] / 255.0f;
		flWeight1 = boneweights->weight[1] / 255.0f;
		flWeight2 = boneweights->weight[2] / 255.0f;
		flWeight3 = boneweights->weight[3] / 255.0f;
		flTotal = flWeight0 + flWeight1 + flWeight2 + flWeight3;

		if( flTotal < 1.0f )
			flWeight0 += 1.0f - flTotal;                    // compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2 + boneMat3[0][0] * flWeight3;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2 + boneMat3[0][1] * flWeight3;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2 + boneMat3[0][2] * flWeight3;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2 + boneMat3[0][3] * flWeight3;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2 + boneMat3[1][0] * flWeight3;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2 + boneMat3[1][1] * flWeight3;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2 + boneMat3[1][2] * flWeight3;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2 + boneMat3[1][3] * flWeight3;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2 + boneMat3[2][0] * flWeight3;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2 + boneMat3[2][1] * flWeight3;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2 + boneMat3[2][2] * flWeight3;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2 + boneMat3[2][3] * flWeight3;
	}
	else if( numbones == 3 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		flWeight0 = boneweights->weight[0] / 255.0f;
		flWeight1 = boneweights->weight[1] / 255.0f;
		flWeight2 = boneweights->weight[2] / 255.0f;
		flTotal = flWeight0 + flWeight1 + flWeight2;

		if( flTotal < 1.0f )
			flWeight0 += 1.0f - flTotal;                    // compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2;
	}
	else if( numbones == 2 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		flWeight0 = boneweights->weight[0] / 255.0f;
		flWeight1 = boneweights->weight[1] / 255.0f;
		flTotal = flWeight0 + flWeight1;

		if( flTotal < 1.0f )
			flWeight0 += 1.0f - flTotal;                    // compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1;
	}
	else
	{
		Matrix3x4_Copy( result, g_studio.worldtransform[boneweights->bone[0]] );
	}
}

/*
===============
pfnGetCurrentEntity

===============
*/
static cl_entity_t *pfnGetCurrentEntity( void )
{
	return nv2a_studio_ctx.currententity;
}

/*
===============
pfnPlayerInfo

===============
*/
player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ))
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

/*
===============
pfnGetPlayerState

===============
*/
static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ))
		return &nv2a_studio_ctx.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

/*
===============
pfnGetEngineTimes

===============
*/
static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount )
		*framecount = nv2a_current_frame_number();
	if( current )
		*current = gp_cl->time;
	if( old )
		*old = gp_cl->oldtime;
}

/*
===============
pfnGetViewInfo

===============
*/
static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *forwardv )
{
	if( origin )
		VectorCopy( nv2a_studio_ctx.vieworigin, origin );
	if( forwardv )
		VectorCopy( nv2a_studio_ctx.vforward, forwardv );
	if( rightv )
		VectorCopy( nv2a_studio_ctx.vright, rightv );
	if( upv )
		VectorCopy( nv2a_studio_ctx.vup, upv );
}

/*
===============
pfnGetModelCounters

===============
*/
static void pfnGetModelCounters( int **s, int **a )
{
	*s = &g_studio.framecount;
	*a = &nv2a_studio_stats.c_studio_models_drawn;
}

/*
===============
pfnStudioGetBoneTransform

===============
*/
static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)g_studio.bonestransform;
}

/*
===============
pfnStudioGetLightTransform

===============
*/
static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)g_studio.lighttransform;
}

/*
===============
pfnStudioGetRotationMatrix

===============
*/
static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)g_studio.rotationmatrix;
}

/*
====================
StudioPlayerBlend

====================
*/
static void R_StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	// calc up/down pointing
	*pBlend = ( *pPitch * 3.0f );

	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f ) // catch qc error
			*pBlend = 127;
		else
			*pBlend = 255 * ( *pBlend - pseqdesc->blendstart[0] ) / ( pseqdesc->blendend[0] - pseqdesc->blendstart[0] );
		*pPitch = 0.0f;
	}
}

/*
====================
R_StudioLerpMovement

====================
*/
void GAME_EXPORT R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float f = 1.0f;

	// don't do it if the goalstarttime hasn't updated in a while.
	// NOTE: Because we need to interpolate multiplayer characters, the interpolation time limit
	// was increased to 1.0 s., which is 2x the max lag we are accounting for.
	if( g_studio.interpolate && ( time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ))
		f = ( time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

	// Con_Printf( "%4.2f %.2f %.2f\n", f, e->curstate.animtime, g_studio.time );
	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t q, q1, q2;

		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else
		VectorCopy( e->curstate.angles, angles );
}

/*
====================
StudioSetUpTransform

====================
*/
static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	// interpolate monsters position (moved into UpdateEntityFields by user request)
	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
	{
		R_StudioLerpMovement( e, g_studio.time, origin, angles );
	}

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		angles[PITCH] = -angles[PITCH]; // stupid quake bug

	// don't rotate clients, only aim
	if( e->player )
		angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( g_studio.rotationmatrix, angles, origin, 1.0f );

	if( nv2a_studio_ctx.fFlipViewModel )
	{
		g_studio.rotationmatrix[0][1] = -g_studio.rotationmatrix[0][1];
		g_studio.rotationmatrix[1][1] = -g_studio.rotationmatrix[1][1];
		g_studio.rotationmatrix[2][1] = -g_studio.rotationmatrix[2][1];
	}
}

/*
====================
StudioEstimateFrame

====================
*/
float GAME_EXPORT R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	double dfdt, f;

	if( g_studio.interpolate )
	{
		if( time < e->curstate.animtime )
			dfdt = 0.0;
		else
			dfdt = ( time - e->curstate.animtime ) * e->curstate.framerate * pseqdesc->fps;
	}
	else
		dfdt = 0;

	if( pseqdesc->numframes <= 1 )
		f = 0.0;
	else
		f = ( e->curstate.frame * ( pseqdesc->numframes - 1 )) / 256.0;

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)( f / ( pseqdesc->numframes - 1 )) * ( pseqdesc->numframes - 1 );
		if( f < 0 )
			f += ( pseqdesc->numframes - 1 );
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;
		if( f < 0.0 )
			f = 0.0;
	}
	return f;
}

/*
====================
StudioEstimateInterpolant

====================
*/
static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float dadt = 1.0f;

	if( g_studio.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = ( g_studio.time - e->curstate.animtime ) / 0.1f;
		if( dadt > 2.0f )
			dadt = 2.0f;
	}

	return dadt;
}

/*
====================
StudioFxTransform

====================
*/
static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 )
				axis = 2;         // choose between x & z
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			float offset;
			int   axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 )
				axis = 2;         // choose between x & z
			offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
	{
		float scale;

		scale = 1.0f + ( g_studio.time - ent->curstate.animtime ) * 10.0f;
		if( scale > 2.0f )
			scale = 2.0f;                    // don't blow up more than 200%

		transform[0][1] *= scale;
		transform[1][1] *= scale;
		transform[2][1] *= scale;
	}
	break;
	}
}

/*
====================
StudioCalcBoneAdj

====================
*/
static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	mstudiobonecontroller_t *pbonecontroller;
	float value = 0.0f;
	int   i, j;

	pbonecontroller = (mstudiobonecontroller_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex );

	for( j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
	{
		i = pbonecontroller[j].index;

		if( i == STUDIO_MOUTH )
		{
			// mouth hardcoded at controller 4
			value = (float)mouthopen / 64.0f;
			value = bound( 0.0f, value, 1.0f );
			value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		else if( i < 4 )
		{
			// check for 360% wrapping
			if( FBitSet( pbonecontroller[j].type, STUDIO_RLOOP ))
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = ( pcontroller1[i] + 128 ) % 256;
					int b = ( pcontroller2[i] + 128 ) % 256;
					value = (( a * dadt ) + ( b * ( 1.0f - dadt )) - 128 ) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
				else
				{
					value = (( pcontroller1[i] * dadt + ( pcontroller2[i] ) * ( 1.0f - dadt ))) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = ( pcontroller1[i] * dadt + pcontroller2[i] * ( 1.0f - dadt )) / 255.0f;
				value = bound( 0.0f, value, 1.0f );
				value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
		}

		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

/*
====================
StudioCalcRotations

====================
*/
static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	int           i, frame;
	float         adj[MAXSTUDIOCONTROLLERS];
	float         s, dadt;
	mstudiobone_t *pbone;

	// bah, fix this bug with changing sequences too fast
	if( f > pseqdesc->numframes - 1 )
	{
		f = 0.0f;
	}
	else if( f < -0.01f )
	{
		// BUG ( somewhere else ) but this code should validate this data.
		// This could cause a crash if the frame # is negative, so we'll go ahead
		// and clamp it here
		f = -0.01f;
	}

	frame = (int)f;

	dadt = R_StudioEstimateInterpolant( e );
	s = ( f - frame );

	// add in programtic controllers
	pbone = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	for( i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X )
		pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y )
		pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z )
		pos[pseqdesc->motionbone][2] = 0.0f;
}

/*
====================
StudioMergeBones

====================
*/
static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	int i, j;
	mstudiobone_t    *pbones;
	mstudioseqdesc_t *pseqdesc;
	mstudioanim_t    *panim;
	matrix3x4        bonematrix;
	static vec4_t    q[MAXSTUDIOBONES];
	static float     pos[MAXSTUDIOBONES][3];
	float f;

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + e->curstate.sequence;

	f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		for( j = 0; j < g_studio.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, g_studio.cached_bonenames[j] ))
			{
				Matrix3x4_Copy( g_studio.bonestransform[i], g_studio.cached_bonestransform[j] );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= g_studio.cached_numbones )
		{
			Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );
			if( pbones[i].parent == -1 )
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

				// apply client-side effects to the transformation matrix
				R_StudioFxTransform( e, g_studio.bonestransform[i] );
			}
			else
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
				Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
			}
		}
	}
}

/*
====================
StudioSetupBones

====================
*/
static void R_StudioSetupBones( cl_entity_t *e )
{
	float            f;
	mstudiobone_t    *pbones;
	mstudioseqdesc_t *pseqdesc;
	mstudioanim_t    *panim;
	matrix3x4        bonematrix;
	static vec3_t    pos[MAXSTUDIOBONES];
	static vec4_t    q[MAXSTUDIOBONES];
	static vec3_t    pos2[MAXSTUDIOBONES];
	static vec4_t    q2[MAXSTUDIOBONES];
	static vec3_t    pos3[MAXSTUDIOBONES];
	static vec4_t    q3[MAXSTUDIOBONES];
	static vec3_t    pos4[MAXSTUDIOBONES];
	static vec4_t    q4[MAXSTUDIOBONES];
	int i;

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + e->curstate.sequence;

	f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, nv2a_studio_ctx.currentmodel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends > 1 )
	{
		float s;
		float dadt;

		panim += m_pStudioHeader->numbones;
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );

		dadt = R_StudioEstimateInterpolant( e );
		s = ( e->curstate.blending[0] * dadt + e->latched.prevblending[0] * ( 1.0f - dadt )) / 255.0f;

		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q2, pos2, s );

		if( pseqdesc->numblends == 4 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, f );

			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, f );

			s = ( e->curstate.blending[0] * dadt + e->latched.prevblending[0] * ( 1.0f - dadt )) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

			s = ( e->curstate.blending[1] * dadt + e->latched.prevblending[1] * ( 1.0f - dadt )) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q3, pos3, s );
		}
	}

	if( g_studio.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > g_studio.time ) && ( e->latched.prevsequence < m_pStudioHeader->numseq ))
	{
		// blend from last sequence
		static vec3_t pos1b[MAXSTUDIOBONES];
		static vec4_t q1b[MAXSTUDIOBONES];
		float         s;

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + e->latched.prevsequence;
		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, nv2a_studio_ctx.currentmodel, pseqdesc );

		// clip prevframe
		R_StudioCalcRotations( e, pos1b, q1b, pseqdesc, panim, e->latched.prevframe );

		if( pseqdesc->numblends > 1 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, e->latched.prevframe );

			s = ( e->latched.prevseqblending[0] ) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q2, pos2, s );

			if( pseqdesc->numblends == 4 )
			{
				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, e->latched.prevframe );

				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, e->latched.prevframe );

				s = ( e->latched.prevseqblending[0] ) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

				s = ( e->latched.prevseqblending[1] ) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q3, pos3, s );
			}
		}

		s = 1.0f - ( g_studio.time - e->latched.sequencetime ) / 0.2f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q1b, pos1b, s );
	}
	else
	{
		// store prevframe otherwise
		e->latched.prevframe = f;
	}

	pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	// calc gait animation
	if( m_pPlayerInfo && m_pPlayerInfo->gaitsequence != 0 )
	{
		qboolean copy_bones = true;

		if( m_pPlayerInfo->gaitsequence >= m_pStudioHeader->numseq )
			m_pPlayerInfo->gaitsequence = 0;

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pPlayerInfo->gaitsequence;

		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, nv2a_studio_ctx.currentmodel, pseqdesc );
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, m_pPlayerInfo->gaitframe );

		for( i = 0; i < m_pStudioHeader->numbones; i++ )
		{
			if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
				copy_bones = false;
			else if( !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
				copy_bones = true;

			if( !copy_bones )
				continue;

			VectorCopy( pos2[i], pos[i] );
			Vector4Copy( q2[i], q[i] );
		}
	}

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );

		if( pbones[i].parent == -1 )
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
			Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

			// apply client-side effects to the transformation matrix
			R_StudioFxTransform( e, g_studio.bonestransform[i] );
		}
		else
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
			Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
		}
	}
}

/*
====================
StudioSaveBones

====================
*/
static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	g_studio.cached_numbones = m_pStudioHeader->numbones;

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, 32 );
	}
}

/*
====================
StudioBuildNormalTable

NOTE: m_pSubModel must be set
====================
*/
static void R_StudioBuildNormalTable( void )
{
	cl_entity_t   *e = nv2a_studio_ctx.currententity;
	mstudiomesh_t *pmesh;
	int           i, j;

	Assert( m_pSubModel != NULL );

	// reset chrome cache
	for( i = 0; i < m_pStudioHeader->numbones; i++ )
		g_studio.chromeage[i] = 0;

	for( i = 0; i < m_pSubModel->numverts; i++ )
		g_studio.normaltable[i] = -1;

	for( j = 0; j < m_pSubModel->nummesh; j++ )
	{
		short *ptricmds;

		pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex ) + j;
		ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex );

		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
				i = -i;

			for( ; i > 0; i--, ptricmds += 4 )
			{
				if( g_studio.normaltable[ptricmds[0]] < 0 )
					g_studio.normaltable[ptricmds[0]] = ptricmds[1];
			}
		}
	}

	g_studio.chrome_origin[0] = cos( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[1] = sin( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[2] = cos( r_glowshellfreq->value * g_studio.time * 0.33f ) * 4000.0f;

	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
		Color4ub( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, 255 );
	else
		Color4ub( 255, 255, 255, 255 );
}

/*
====================
StudioGenerateNormals

NOTE: m_pSubModel must be set
g_studio.verts must be computed
====================
*/
static void R_StudioGenerateNormals( void )
{
	int           v0, v1, v2;
	vec3_t        e0, e1, norm;
	mstudiomesh_t *pmesh;
	int           i, j;

	Assert( m_pSubModel != NULL );

	for( i = 0; i < m_pSubModel->numverts; i++ )
		VectorClear( g_studio.norms[i] );

	for( j = 0; j < m_pSubModel->nummesh; j++ )
	{
		short *ptricmds;

		pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex ) + j;
		ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex );

		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				i = -i;

				if( i > 2 )
				{
					v0 = ptricmds[0]; ptricmds += 4;
					v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						v1 = v2;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
			else
			{
				if( i > 2 )
				{
					qboolean odd = false;

					v0 = ptricmds[0]; ptricmds += 4;
					v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						if( odd )
							v1 = v2;
						else
							v0 = v2;

						odd = !odd;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
		}
	}

	for( i = 0; i < m_pSubModel->numverts; i++ )
		VectorNormalize( g_studio.norms[i] );
}

/*
====================
StudioSetupChrome

====================
*/
static void R_StudioSetupChrome( float *pchrome, int bone, vec3_t normal )
{
	float n;

	if( g_studio.chromeage[bone] != g_studio.framecount )
	{
		// calculate vectors from the viewer to the bone. This roughly adjusts for position
		vec3_t chromeupvec;    // g_studio.chrome t vector in world reference frame
		vec3_t chromerightvec; // g_studio.chrome s vector in world reference frame
		vec3_t tmp;            // vector pointing at bone in world reference frame

		VectorNegate( g_studio.chrome_origin, tmp );
		tmp[0] += g_studio.lighttransform[bone][0][3];
		tmp[1] += g_studio.lighttransform[bone][1][3];
		tmp[2] += g_studio.lighttransform[bone][2][3];

		VectorNormalize( tmp );
		CrossProduct( tmp, nv2a_studio_ctx.vright, chromeupvec );
		VectorNormalize( chromeupvec );
		CrossProduct( chromeupvec, tmp, chromerightvec );
		VectorNormalize( chromerightvec );

		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromeupvec, g_studio.chromeup[bone] );
		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromerightvec, g_studio.chromeright[bone] );

		g_studio.chromeage[bone] = g_studio.framecount;
	}

	// calc s coord
	n = DotProduct( normal, g_studio.chromeright[bone] );
	pchrome[0] = ( n + 1.0f ) * 32.0f;

	// calc t coord
	n = DotProduct( normal, g_studio.chromeup[bone] );
	pchrome[1] = ( n + 1.0f ) * 32.0f;
}

/*
====================
StudioCalcAttachments

====================
*/
static void R_StudioCalcAttachments( void )
{
	vec3_t forward, bonepos;
	vec3_t localOrg, localAng;

	// calculate attachment points
	mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex );

	for( int i = 0; i < Q_min( MAXSTUDIOATTACHMENTS, m_pStudioHeader->numattachments ); i++ )
	{
		Matrix3x4_VectorTransform( g_studio.lighttransform[pAtt[i].bone], pAtt[i].org, nv2a_studio_ctx.currententity->attachment[i] );
		VectorSubtract( nv2a_studio_ctx.currententity->attachment[i], nv2a_studio_ctx.currententity->origin, localOrg );
		Matrix3x4_OriginFromMatrix( g_studio.lighttransform[pAtt[i].bone], bonepos );
		VectorSubtract( localOrg, bonepos, forward ); // make forward
		VectorNormalizeFast( forward );
		VectorAngles( forward, localAng );
	}
}

/*
===============
pfnStudioSetupModel

===============
*/
static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	if( bodypart > m_pStudioHeader->numbodyparts )
		bodypart = 0;

	m_pBodyPart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex ) + bodypart;

	int index = nv2a_studio_ctx.currententity->curstate.body / m_pBodyPart->base;
	index = index % m_pBodyPart->nummodels;

	m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex ) + index;

	if( ppbodypart )
		*ppbodypart = m_pBodyPart;
	if( ppsubmodel )
		*ppsubmodel = m_pSubModel;
}

/*
===============
R_StudioCheckBBox

===============
*/
static int R_StudioCheckBBox( void )
{
	if( !nv2a_studio_ctx.currententity || !nv2a_studio_ctx.currentmodel )
		return false;

	return R_StudioComputeBBox( NULL );
}

/*
===============
pfnStudioEntityLight

===============
*/
static void R_StudioEntityLight( alight_t *lightinfo )
{
	int         lnum, i, j, k;
	float       minstrength, dist2, f, r2;
	float       lstrength[MAX_LOCALLIGHTS];
	cl_entity_t *ent = nv2a_studio_ctx.currententity;
	vec3_t      mid, origin;
	dlight_t    *el;

	g_studio.numlocallights = 0;

	if( !ent || !r_dynamic->value )
		return;

	for( i = 0; i < MAX_LOCALLIGHTS; i++ )
		lstrength[i] = 0;

	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );
	dist2 = 1000000.0f;
	k = 0;

	for( lnum = 0; lnum < MAX_ELIGHTS; lnum++ )
	{
		el = &nv2a_studio_ctx.elights[lnum];

		if( el->die < g_studio.time || el->radius <= 0.0f )
			continue;

		if(( el->key & 0xFFF ) == ent->index )
		{
			int att = ( el->key >> 12 ) & 0xF;

			if( att )
				VectorCopy( ent->attachment[att], el->origin );
			else
				VectorCopy( ent->origin, el->origin );
		}

		VectorSubtract( origin, el->origin, mid );

		f = DotProduct( mid, mid );
		r2 = el->radius * el->radius;

		if( f > r2 )
			minstrength = r2 / f;
		else
			minstrength = 1.0f;

		if( minstrength > 0.05f )
		{
			if( g_studio.numlocallights >= MAX_LOCALLIGHTS )
			{
				for( j = 0, k = -1; j < g_studio.numlocallights; j++ )
				{
					if( lstrength[j] < dist2 && lstrength[j] < minstrength )
					{
						dist2 = lstrength[j];
						k = j;
					}
				}
			}
			else
				k = g_studio.numlocallights;

			if( k != -1 )
			{
				g_studio.locallightcolor[k][0] = LinearGammaTable( el->color.r << 2 );
				g_studio.locallightcolor[k][1] = LinearGammaTable( el->color.g << 2 );
				g_studio.locallightcolor[k][2] = LinearGammaTable( el->color.b << 2 );
				g_studio.locallightR2[k] = r2;
				g_studio.locallight[k] = el;
				lstrength[k] = minstrength;

				if( k >= g_studio.numlocallights )
					g_studio.numlocallights = k + 1;
			}
		}
	}
}

/*
===============
R_StudioSetupLighting

===============
*/
static void R_StudioSetupLighting( alight_t *plight )
{
	float scale = 1.0f;

	if( !m_pStudioHeader || !plight )
		return;

	if( nv2a_studio_ctx.currententity != NULL )
		scale = nv2a_studio_ctx.currententity->curstate.scale;

	g_studio.ambientlight = plight->ambientlight;
	g_studio.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, g_studio.lightvec );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_VectorIRotate( g_studio.lighttransform[i], plight->plightvec, g_studio.blightvec[i] );
		if( scale > 1.0f )
			VectorNormalize( g_studio.blightvec[i] );            // in case model may be scaled
	}

	VectorCopy( plight->color, g_studio.lightcolor );
}

/*
===============
R_StudioLighting

===============
*/
static void R_StudioLighting( float *lv, int bone, int flags, vec3_t normal )
{
	float illum;

	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
	{
		*lv = 1.0f;
		return;
	}

	illum = g_studio.ambientlight;

	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
	{
		illum += g_studio.shadelight * 0.8f;
	}
	else
	{
		float r, lightcos;

		if( bone != -1 )
			lightcos = DotProduct( normal, g_studio.blightvec[bone] );
		else
			lightcos = DotProduct( normal, g_studio.lightvec ); // -1 colinear, 1 opposite
		if( lightcos > 1.0f )
			lightcos = 1.0f;

		illum += g_studio.shadelight;

		r = SHADE_LAMBERT;

		// do modified hemispherical lighting
		if( r <= 1.0f )
		{
			r += 1.0f;
			lightcos = (( r - 1.0f ) - lightcos ) / r;
			if( lightcos > 0.0f )
				illum += g_studio.shadelight * lightcos;
		}
		else
		{
			lightcos = ( lightcos + ( r - 1.0f )) / r;
			if( lightcos > 0.0f )
				illum -= g_studio.shadelight * lightcos;
		}

		illum = Q_max( illum, 0.0f );
	}

	illum = Q_min( illum, 255.0f );

	*lv = LightToTexGamma( illum * 4 ) / 1023.0f;
}

/*
====================
R_LightLambert

====================
*/
static void R_LightLambert( vec4_t light[MAX_LOCALLIGHTS], const vec3_t normal, const vec3_t color, byte *out )
{
	if( !g_studio.numlocallights )
	{
		VectorScale( color, 255.0f, out );
		return;
	}

	vec3_t finalLight = { 0, 0, 0 };

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		float r;

		r = DotProduct( normal, light[i] );
		if( likely( !nv2a_studio_ctx.fFlipViewModel ))
			r = -r;

		if( r > 0.0f )
		{
			vec3_t localLight;
			float  temp;

			if( light[i][3] == 0.0f )
			{
				float r2 = DotProduct( light[i], light[i] );

				if( r2 > 0.0f )
					light[i][3] = g_studio.locallightR2[i] / ( r2 * sqrt( r2 ));
				else
					light[i][3] = 0.0001f;
			}

			temp = r * light[i][3];

			VectorAddScalar( g_studio.locallightcolor[i], temp, localLight );
			VectorAdd( finalLight, localLight, finalLight );
		}
	}

	if( !VectorIsNull( finalLight ))
	{
		for( int i = 0; i < 3; i++ )
		{
			float c = finalLight[i] + LinearGammaTable( color[i] * 1023.0f );

			if( c > 1023.0f )
				out[i] = 255;
			else
				out[i] = ScreenGammaTable( c ) >> 2;
		}
	}
	else
	{
		VectorScale( color, 255.0f, out );
	}
}

static void R_StudioSetColorBegin( short *ptricmds, vec3_t *pstudionorms )
{
	float  *lv = (float *)g_studio.lightvalues[ptricmds[1]];
	rgba_t color;

	color[3] = nv2a_studio_ctx.blend * 255;

	R_LightLambert( g_studio.lightpos[ptricmds[0]], pstudionorms[ptricmds[1]], lv, color );
	Color4ub( color[0], color[1], color[2], color[3] );
}


/*
====================
R_LightStrength

====================
*/
static void R_LightStrength( int bone, vec3_t localpos, vec4_t light[MAX_LOCALLIGHTS] )
{
	int i;

	if( g_studio.lightage[bone] != g_studio.framecount )
	{
		for( i = 0; i < g_studio.numlocallights; i++ )
		{
			dlight_t *el = g_studio.locallight[i];
			Matrix3x4_VectorITransform( g_studio.lighttransform[bone], el->origin, g_studio.lightbonepos[bone][i] );
		}

		g_studio.lightage[bone] = g_studio.framecount;
	}

	for( i = 0; i < g_studio.numlocallights; i++ )
	{
		VectorSubtract( localpos, g_studio.lightbonepos[bone][i], light[i] );
		light[i][3] = 0.0f;
	}
}

/*
===============
R_StudioSetupSkin

===============
*/
static void R_StudioSetupSkin( studiohdr_t *ptexturehdr, int index )
{
	mstudiotexture_t *ptexture = NULL;

	/* gl_studio.c:1483: the chrome sprite R_StudioRenderModel bound
	   (TriSpriteTexture, X9-X step 6) stays bound. */
	if( FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME ))
		return;

	if( ptexturehdr == NULL )
		return;

	// NOTE: user may ignore to call StudioRemapColors and remap_info will be unavailable
	if( m_fDoRemap )
		ptexture = gEngfuncs.CL_GetRemapInfoForEntity( nv2a_studio_ctx.currententity )->ptexture;
	if( !ptexture )
		ptexture = (mstudiotexture_t *)((byte *)ptexturehdr + ptexturehdr->textureindex );        // fallback

	if( r_lightmap->value && !r_fullbright->value )
		GL_Bind( XASH_TEXTURE0, nv2a_tex_white() );
	else
		GL_Bind( XASH_TEXTURE0, ptexture[index].index );
}

/*
===============
R_StudioGetTexture

Doesn't changes studio global state at all
===============
*/
mstudiotexture_t * GAME_EXPORT R_StudioGetTexture( cl_entity_t *e )
{
	mstudiotexture_t *ptexture;
	studiohdr_t      *phdr, *thdr;

	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, e->model )) == NULL )
		return NULL;

	thdr = m_pStudioHeader;
	if( !thdr )
		return NULL;

	if( m_fDoRemap )
		ptexture = gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture;
	else
		ptexture = (mstudiotexture_t *)((byte *)thdr + thdr->textureindex );

	return ptexture;
}

static void R_StudioSetRenderamt( int iRenderamt )
{
	if( !nv2a_studio_ctx.currententity )
		return;

	nv2a_studio_ctx.currententity->curstate.renderamt = iRenderamt;
	nv2a_studio_ctx.blend = nv2a_fx_blend( nv2a_studio_ctx.currententity ) / 255.0f;
}

/*
===============
R_StudioSetCullState

sets true for enable backculling (for left-hand viewmodel)
===============
*/
static void R_StudioSetCullState( int iCull )
{
	// This function intentionally does nothing
}

/*
===============
R_StudioRenderShadow

just a prefab for render shadow
===============
*/
static void R_StudioRenderShadow( int iSprite, float *p1, float *p2, float *p3, float *p4 )
{
	if( !p1 || !p2 || !p3 || !p4 )
		return;

	/* gl_studio.c:1554 - TriSpriteTexture is r_sprite.c's since X9-X
	   step 6. */
	if( TriSpriteTexture( CL_ModelHandle( iSprite ), 0 ))
	{
		TriRenderMode( kRenderTransAlpha );
		Color4f( 0.0f, 0.0f, 0.0f, 1.0f );

		Begin( TRI_QUADS );
		TexCoord2f( 0.0f, 0.0f );
		Vertex3fv( p1 );
		TexCoord2f( 0.0f, 1.0f );
		Vertex3fv( p2 );
		TexCoord2f( 1.0f, 1.0f );
		Vertex3fv( p3 );
		TexCoord2f( 1.0f, 0.0f );
		Vertex3fv( p4 );
		End();

		TriRenderMode( kRenderNormal );
	}
}

/*
===============
R_StudioMeshCompare

Sorting opaque entities by model type
===============
*/
static int R_StudioMeshCompare( const sortedmesh_t *a, const sortedmesh_t *b )
{
	if( FBitSet( a->flags, STUDIO_NF_ADDITIVE ))
		return 1;

	if( FBitSet( a->flags, STUDIO_NF_MASKED ))
		return -1;

	return 0;
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawNormalMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t )
{
	int   i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			Begin( TRI_TRIANGLE_FAN );
			i = -i;
		}
		else
			Begin( TRI_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			R_StudioSetColorBegin( ptricmds, pstudionorms );

			TexCoord2f( ptricmds[2] * s, ptricmds[3] * t );
			Vertex3fv( g_studio.verts[ptricmds[0]] );
		}

		End();
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawFloatMesh( short *ptricmds, vec3_t *pstudionorms )
{
	int   i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			Begin( TRI_TRIANGLE_FAN );
			i = -i;
		}
		else
			Begin( TRI_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			R_StudioSetColorBegin( ptricmds, pstudionorms );
			TexCoord2f( HalfToFloat( ptricmds[2] ), HalfToFloat( ptricmds[3] ));
			Vertex3fv( g_studio.verts[ptricmds[0]] );
		}

		End();
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawChromeMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float scale )
{
	float    *lv, *av;
	int      i, idx;
	qboolean glowShell = ( scale > 0.0f ) ? true : false;
	vec3_t   vert;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			Begin( TRI_TRIANGLE_FAN );
			i = -i;
		}
		else
			Begin( TRI_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			if( glowShell )
			{
				color24 *clr = &nv2a_studio_ctx.currententity->curstate.rendercolor;

				idx = g_studio.normaltable[ptricmds[0]];
				av = g_studio.verts[ptricmds[0]];
				lv = g_studio.norms[ptricmds[0]];
				VectorMA( av, scale, lv, vert );
				Color4ub( clr->r, clr->g, clr->b, 255 );
				TexCoord2f( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
				Vertex3fv( vert );
			}
			else
			{
				idx = ptricmds[1];
				lv = (float *)g_studio.lightvalues[ptricmds[1]];
				R_StudioSetColorBegin( ptricmds, pstudionorms );
				TexCoord2f( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
				Vertex3fv( g_studio.verts[ptricmds[0]] );
			}
		}

		End();
	}
}


/*
===============
R_StudioDrawPoints

===============
*/
static void R_StudioDrawPoints( void )
{
	int              i, j, k, m_skinnum;
	float            shellscale = 0.0f;
	qboolean         need_sort = false;
	byte             *pvertbone;
	byte             *pnormbone;
	vec3_t           *pstudioverts;
	vec3_t           *pstudionorms;
	mstudiotexture_t *ptexture;
	mstudiomesh_t    *pmesh;
	short            *pskinref;
	float            lv_tmp;

	if( !m_pStudioHeader )
		return;

	/* Design note section 5 / census: skip (with a one-time log per model)
	   a submodel whose numverts or numnorms exceeds this port's own
	   MAXSTUDIOVERTS cap (2048) - nothing in the retail census does, but a
	   mod's own custom model might, and g_studio.verts[]/norms[] have no
	   other bounds check before this function writes them. */
	if( m_pSubModel->numverts > MAXSTUDIOVERTS || m_pSubModel->numnorms > MAXSTUDIOVERTS )
	{
		static const model_t *nv2a_studio_warned_model;

		if( nv2a_studio_warned_model != nv2a_studio_ctx.currentmodel )
		{
			gEngfuncs.Con_DPrintf( S_ERROR "ref_nv2a: %s exceeds MAXSTUDIOVERTS (%d verts/%d norms > %d) - not drawing this submodel\n",
				nv2a_studio_ctx.currentmodel ? nv2a_studio_ctx.currentmodel->name : "?",
				m_pSubModel->numverts, m_pSubModel->numnorms, MAXSTUDIOVERTS );
			nv2a_studio_warned_model = nv2a_studio_ctx.currentmodel;
		}
		return;
	}

	m_skinnum = nv2a_studio_ctx.currententity->curstate.skin;
	ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex );
	pvertbone = ((byte *)m_pStudioHeader + m_pSubModel->vertinfoindex );
	pnormbone = ((byte *)m_pStudioHeader + m_pSubModel->norminfoindex );

	pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex );
	pstudioverts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex );
	pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex );

	pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex );
	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += ( m_skinnum * m_pStudioHeader->numskinref );

	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) && m_pSubModel->blendvertinfoindex != 0 && m_pSubModel->blendnorminfoindex != 0 )
	{
		mstudioboneweight_t *pvertweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendvertinfoindex );
		mstudioboneweight_t *pnormweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendnorminfoindex );
		matrix3x4           skinMat;

		for( i = 0; i < m_pSubModel->numverts; i++ )
		{
			R_StudioComputeSkinMatrix( &pvertweight[i], skinMat );
			Matrix3x4_VectorTransform( skinMat, pstudioverts[i], g_studio.verts[i] );
			R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
		}

		for( i = 0; i < m_pSubModel->numnorms; i++ )
		{
			R_StudioComputeSkinMatrix( &pnormweight[i], skinMat );
			Matrix3x4_VectorRotate( skinMat, pstudionorms[i], g_studio.norms[i] );
		}
	}
	else
	{
		for( i = 0; i < m_pSubModel->numverts; i++ )
		{
			Matrix3x4_VectorTransform( g_studio.bonestransform[pvertbone[i]], pstudioverts[i], g_studio.verts[i] );
			R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
		}
	}

	// generate shared normals for properly scaling glowing shell
	if( nv2a_studio_ctx.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		float factor = ( 1.0f / 128.0f );
		shellscale = Q_max( factor, nv2a_studio_ctx.currententity->curstate.renderamt * factor );
		R_StudioBuildNormalTable();
		R_StudioGenerateNormals();
	}

	for( j = k = 0; j < m_pSubModel->nummesh; j++ )
	{
		g_nFaceFlags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;

		// fill in sortedmesh info
		g_studio.meshes[j].flags = g_nFaceFlags;
		g_studio.meshes[j].mesh = &pmesh[j];

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED | STUDIO_NF_ADDITIVE ))
			need_sort = true;

		if( nv2a_studio_ctx.currententity->curstate.rendermode == kRenderTransAdd )
		{
			for( i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorSet( g_studio.lightvalues[k], nv2a_studio_ctx.blend, nv2a_studio_ctx.blend, nv2a_studio_ctx.blend );
			}
		}
		else
		{
			for( i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
					R_StudioLighting( &lv_tmp, -1, g_nFaceFlags, g_studio.norms[k] );
				else
					R_StudioLighting( &lv_tmp, *pnormbone, g_nFaceFlags, (float *)pstudionorms );

				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorScale( g_studio.lightcolor, lv_tmp, g_studio.lightvalues[k] );
			}
		}
	}

	if( r_studio_sort_textures.value && need_sort )
	{
		// resort opaque and translucent meshes draw order
		qsort( g_studio.meshes, m_pSubModel->nummesh, sizeof( sortedmesh_t ), (void *)R_StudioMeshCompare );
	}

	// NOTE: rewind normals at start
	pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex );

	/* gl_studio.c:2055-2072 - the GoldSrc "faceCull != GL_NONE" bug: the
	   flipped view model (cl_righthand) only disables culling while the
	   world already has it on, rather than always forcing it explicitly -
	   see the reference comment (github.com/FWGS/xash3d-fwgs/issues/2517).
	   CullFace(TRI_FRONT) mirrors GL_Cull(GL_FRONT); CullFace(TRI_NONE)
	   mirrors GL_Cull(GL_NONE) - both real TRICULLSTYLE values
	   (triangleapi.h), not a stand-in. */
	if( nv2a_2d_desired.cull_enable )
	{
		if( R_AllowFlipViewModel( nv2a_studio_ctx.currententity ))
		{
			nv2a_studio_ctx.fFlipViewModel = true;
			CullFace( TRI_NONE );
		}
		else
		{
			nv2a_studio_ctx.fFlipViewModel = false;
			CullFace( TRI_FRONT );
		}
	}

	for( j = 0; j < m_pSubModel->nummesh; j++ )
	{
		float oldblend = nv2a_studio_ctx.blend;
		short *ptricmds;
		float s, t;

		pmesh = g_studio.meshes[j].mesh;
		ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex );

		g_nFaceFlags = ptexture[pskinref[pmesh->skinref]].flags | g_nForceFaceFlags;

		s = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].width;
		t = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].height;

		{
			/* Design note section 6 / gl_studio.c:2090-2123, restructured as
			   a symmetric apply/restore pair around every mesh instead of
			   the reference's own two separate if/else-if blocks -
			   nv2a_studio_mesh_state() (host-tested,
			   tools/host-tests/nv2a_arena_host_test.c) is a no-op for a
			   plain mesh, so calling apply/restore unconditionally is
			   behaviourally identical to only calling them for MASKED/
			   ADDITIVE meshes. */
			int entity_opaque = R_ModelOpaque( nv2a_studio_ctx.currententity->curstate.rendermode );

			nv2a_studio_apply_mesh_state( g_nFaceFlags, entity_opaque );

			if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
			{
				if( entity_opaque )
					nv2a_studio_ctx.blend = 1.0f;
			}
			else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && entity_opaque )
			{
				R_AllowFog( false );
			}

			R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh->skinref] );

			if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
				R_StudioDrawChromeMesh( ptricmds, pstudionorms, s, t, shellscale );
			else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ))
				R_StudioDrawFloatMesh( ptricmds, pstudionorms );
			else
				R_StudioDrawNormalMesh( ptricmds, pstudionorms, s, t );

			nv2a_studio_restore_mesh_state();

			if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && entity_opaque )
				R_AllowFog( true );
		}

		nv2a_studio_stats.c_studio_polys += pmesh->numtris;
		nv2a_studio_ctx.blend = oldblend;
	}
}

/*
===============
R_StudioDrawHulls

===============
*/
static void R_StudioDrawHulls( void )
{
}

/*
===============
R_StudioDrawAbsBBox

===============
*/
static void R_StudioDrawAbsBBox( void )
{
	vec3_t p[8], tmp;
	float  lv;
	int    i;

	// looks ugly, skip
	if( nv2a_studio_ctx.currententity == nv2a_studio_ctx.viewent )
		return;

	if( !R_StudioComputeBBox( p ))
		return;

	GL_Bind( XASH_TEXTURE0, nv2a_tex_white() );
	Color4f( 0.5f, 0.5f, 1.0f, 0.5f );
	TriRenderMode( kRenderTransAdd );

	Begin( TRI_QUADS );
	for( i = 0; i < 6; i++ )
	{
		VectorClear( tmp );
		tmp[i % 3] = ( i < 3 ) ? 1.0f : -1.0f;
		R_StudioLighting( &lv, -1, 0, tmp );

		Color4f( lv, lv, lv, 1.0f ); /* TriBrightness (gl_triapi.c), ported as a plain Color4f */
		Vertex3fv( p[boxpnt[i][0]] );
		Vertex3fv( p[boxpnt[i][1]] );
		Vertex3fv( p[boxpnt[i][2]] );
		Vertex3fv( p[boxpnt[i][3]] );
	}
	End();
	TriRenderMode( kRenderNormal );
}

/*
===============
R_StudioDrawBones

===============
*/
static void R_StudioDrawBones( void )
{
}

static void R_StudioDrawAttachments( void )
{
}

/*
===============
R_StudioSetRemapColors

===============
*/
static void R_StudioSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( nv2a_studio_ctx.currententity, nv2a_studio_ctx.currentmodel, newTop, newBottom ))
		m_fDoRemap = true;
}

void R_StudioResetPlayerModels( void )
{
	memset( g_studio.player_models, 0, sizeof( g_studio.player_models ));
}

/*
===============
R_StudioSetupPlayerModel

===============
*/
static model_t *R_StudioSetupPlayerModel( int index )
{
	player_info_t  *info = gEngfuncs.pfnPlayerInfo( index );
	player_model_t *state;

	if( index < 0 || index >= gp_cl->maxclients )
		return NULL;

	state = &g_studio.player_models[index];

	// g-cont: force for "dev-mode", non-local games and menu preview
	if(( gpGlobals->developer || !ENGINE_GET_PARM( PARM_SINGLEPLAYER_GAME ) || !FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD )) && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ))
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ));
			state->name[sizeof( state->name ) - 1] = 0;

			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ))
				state->model = gEngfuncs.Mod_ForName( state->modelname, false, true );
			else
				state->model = NULL;

			if( !state->model )
				state->model = nv2a_studio_ctx.currententity->model;
		}
	}
	else
	{
		if( state->model != nv2a_studio_ctx.currententity->model )
			state->model = nv2a_studio_ctx.currententity->model;
		state->name[0] = 0;
	}

	return state->model;
}

/*
================
R_GetEntityRenderMode

check for texture flags
================
*/
int R_GetEntityRenderMode( cl_entity_t *ent )
{
	int              i, opaque, trans;
	mstudiotexture_t *ptexture;
	cl_entity_t      *oldent;
	model_t          *model = NULL;
	studiohdr_t      *phdr;

	oldent = nv2a_studio_ctx.currententity;
	nv2a_studio_ctx.currententity = ent;

	if( ent->player ) // check it for real playermodel
		model = R_StudioSetupPlayerModel( ent->curstate.number - 1 );

	if( !model )
		model = ent->model;

	nv2a_studio_ctx.currententity = oldent;

	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, model )) == NULL )
	{
		if( R_ModelOpaque( ent->curstate.rendermode ))
		{
			// forcing to choose right sorting type
			if(( model && model->type == mod_brush ) && FBitSet( model->flags, MODEL_TRANSPARENT ))
				return kRenderTransAlpha;
		}
		return ent->curstate.rendermode;
	}
	ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex );

	for( opaque = trans = i = 0; i < phdr->numtextures; i++, ptexture++ )
	{
		// ignore chrome & additive it's just a specular-like effect
		if( FBitSet( ptexture->flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture->flags, STUDIO_NF_CHROME ))
			trans++;
		else
			opaque++;
	}

	// if model is more additive than opaque
	if( trans > opaque )
		return kRenderTransAdd;
	return ent->curstate.rendermode;
}

/*
===============
R_StudioClientEvents

===============
*/
static void R_StudioClientEvents( void )
{
	mstudioseqdesc_t *pseqdesc;
	mstudioevent_t   *pevent;
	cl_entity_t      *e = nv2a_studio_ctx.currententity;
	int   i, sequence;
	float end, start;

	if( g_studio.frametime == 0.0 )
		return; // gamepaused

	// fill attachments with interpolated origin
	if( m_pStudioHeader->numattachments <= 0 )
	{
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[0] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[1] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[2] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[3] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
	{
		dlight_t *el = gEngfuncs.CL_AllocElight( 0 );

		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
		VectorCopy( e->attachment[0], el->origin );
		el->die = gp_cl->time + 0.05f;
		el->color.r = 255;
		el->color.g = 192;
		el->color.b = 64;
		el->decay = 320;
		el->radius = 24;
	}

	sequence = bound( 0, e->curstate.sequence, m_pStudioHeader->numseq - 1 );
	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + sequence;

	// no events for this animation
	if( pseqdesc->numevents == 0 )
		return;

	end = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );
	start = end - e->curstate.framerate * gp_host->frametime * pseqdesc->fps;
	pevent = (mstudioevent_t *)((byte *)m_pStudioHeader + pseqdesc->eventindex );

	if( e->latched.sequencetime == e->curstate.animtime )
	{
		if( !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
			start = -0.01f;
	}

	for( i = 0; i < pseqdesc->numevents; i++ )
	{
		// ignore all non-client-side events
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if((float)pevent[i].frame > start && pevent[i].frame <= end )
			gEngfuncs.pfnStudioEvent( &pevent[i], e );
	}
}

/*
===============
R_StudioGetForceFaceFlags

===============
*/
static int R_StudioGetForceFaceFlags( void )
{
	return g_nForceFaceFlags;
}

/*
===============
R_StudioSetForceFaceFlags

===============
*/
static void R_StudioSetForceFaceFlags( int flags )
{
	g_nForceFaceFlags = flags;
}

/*
===============
pfnStudioSetHeader

===============
*/
static void R_StudioSetHeader( studiohdr_t *pheader )
{
	m_pStudioHeader = pheader;
	m_fDoRemap = false;
}

studiohdr_t *R_StudioGetHeader( void )
{
	return m_pStudioHeader;
}

/*
===============
R_StudioSetRenderModel

===============
*/
static void R_StudioSetRenderModel( model_t *model )
{
	nv2a_studio_ctx.currentmodel = model;
}

/*
===============
R_StudioSetupRenderer

===============
*/
static void R_StudioSetupRenderer( int rendermode )
{
	studiohdr_t *phdr = m_pStudioHeader;
	int         i;

	if( rendermode > kRenderTransAdd )
		rendermode = 0;
	g_studio.rendermode = bound( 0, rendermode, kRenderTransAdd );

	/* gl_studio.c:2552-2553. */
	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( false );

	nv2a_2d_desired.alpha_test_enable = false; /* gl_studio.c:2557, pglDisable(GL_ALPHA_TEST) */

	// a point to setup local to world transform for boneweighted models
	if( phdr && FBitSet( phdr->flags, STUDIO_HAS_BONEINFO ))
	{
		// NOTE: extended boneinfo goes immediately after bones
		mstudioboneinfo_t *boneinfo = (mstudioboneinfo_t *)((byte *)phdr + phdr->boneindex + phdr->numbones * sizeof( mstudiobone_t ));

		for( i = 0; i < phdr->numbones; i++ )
			Matrix3x4_ConcatTransforms( g_studio.worldtransform[i], g_studio.bonestransform[i], boneinfo[i].poseToBone );
	}
}

/*
===============
R_StudioRestoreRenderer

===============
*/
static void R_StudioRestoreRenderer( void )
{
	/* gl_studio.c:2576-2580. */
	if( g_studio.rendermode != kRenderNormal )
		nv2a_2d_desired.blend_enable = false; /* pglDisable(GL_BLEND) */

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( true );

	m_fDoRemap = false;
}

/*
===============
R_StudioSetChromeOrigin

===============
*/
static void R_StudioSetChromeOrigin( void )
{
	VectorCopy( nv2a_studio_ctx.vieworigin, g_studio.chrome_origin );
}

/*
===============
GL_StudioSetRenderMode

set rendermode for studiomodel
===============
*/
static void GL_StudioSetRenderMode( int rendermode )
{
	/* gl_studio.c:2667-2688, whole function - a distinct GL state setter
	   from the TriAPI TriRenderMode and the 2D GL_SetRenderMode (each maps
	   a DIFFERENT set of reference values), so ported directly onto the
	   batch-state shadow instead of reusing either. */
	switch( rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		nv2a_2d_desired.blend_enable = true;
		nv2a_2d_desired.sfactor = NV2A_STUDIO_SFACTOR_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV2A_STUDIO_DFACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case kRenderTransAdd:
		Color4f( nv2a_studio_ctx.blend, nv2a_studio_ctx.blend, nv2a_studio_ctx.blend, 1.0f );
		nv2a_2d_desired.sfactor = NV2A_STUDIO_SFACTOR_ONE;
		nv2a_2d_desired.dfactor = NV2A_STUDIO_DFACTOR_ONE;
		nv2a_2d_desired.depth_mask = false;
		nv2a_2d_desired.blend_enable = true;
		break;
	default:
		nv2a_2d_desired.sfactor = NV2A_STUDIO_SFACTOR_SRC_ALPHA;
		nv2a_2d_desired.dfactor = NV2A_STUDIO_DFACTOR_ONE_MINUS_SRC_ALPHA;
		Color4f( 1.0f, 1.0f, 1.0f, nv2a_studio_ctx.blend );
		nv2a_2d_desired.depth_mask = true;
		nv2a_2d_desired.blend_enable = true;
		break;
	}
}

/*
===============
GL_StudioDrawShadow

g-cont: don't modify this code it's 100% matched with
original GoldSrc code and used in some mods to enable
studio shadows with some asm tricks
===============
*/
static void GL_StudioDrawShadow( void )
{
	/* gl_studio.c:2704-2723: only the unconditional pglDepthMask(GL_TRUE)
	   is ported (spec) - the r_shadows stencil-shadow path stays out of
	   scope (docs/r3d/divergences.md); r_shadows is still registered
	   (R_StudioInit), default 0, so a real HL client config round-trips. */
	nv2a_2d_desired.depth_mask = true;
}

/*
====================
StudioRenderFinal

====================
*/
static void R_StudioRenderFinal( void )
{
	int i, rendermode;

	rendermode = R_StudioGetForceFaceFlags() ? kRenderTransAdd : nv2a_studio_ctx.currententity->curstate.rendermode;
	R_StudioSetupRenderer( rendermode );

	if( r_drawentities->value == 2 )
	{
		R_StudioDrawBones();
	}
	else if( r_drawentities->value == 3 )
	{
		R_StudioDrawHulls();
	}
	else
	{
		for( i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			R_StudioSetupModel( i, (void **)&m_pBodyPart, (void **)&m_pSubModel );

			GL_StudioSetRenderMode( rendermode );
			R_StudioDrawPoints();
			GL_StudioDrawShadow();
		}
	}

	if( r_drawentities->value == 4 )
	{
		TriRenderMode( kRenderTransAdd );
		R_StudioDrawHulls( );
		TriRenderMode( kRenderNormal );
	}

	if( r_drawentities->value == 5 )
	{
		R_StudioDrawAbsBBox( );
	}

	if( r_drawentities->value == 6 )
	{
		R_StudioDrawAttachments();
	}

	R_StudioRestoreRenderer();
}

/*
====================
StudioRenderModel

====================
*/
static void R_StudioRenderModel( void )
{
	R_StudioSetChromeOrigin();
	R_StudioSetForceFaceFlags( 0 );

	if( nv2a_studio_ctx.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		nv2a_studio_ctx.currententity->curstate.renderfx = kRenderFxNone;

		R_StudioRenderFinal( );

		R_StudioSetForceFaceFlags( STUDIO_NF_CHROME );
		TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_CHROME_SPRITE ), 0 ); /* gl_studio.c:2830 */
		nv2a_studio_ctx.currententity->curstate.renderfx = kRenderFxGlowShell;

		R_StudioRenderFinal( );
	}
	else
	{
		R_StudioRenderFinal( );
	}
}

/*
====================
StudioEstimateGait

====================
*/
static void R_StudioEstimateGait( entity_state_t *pplayer )
{
	vec3_t est_velocity;
	float  dt;

	dt = bound( 0.0f, g_studio.frametime, 1.0f );

	if( dt == 0.0f || m_pPlayerInfo->renderframe == nv2a_current_frame_number() )
	{
		m_flGaitMovement = 0;
		return;
	}

	VectorSubtract( nv2a_studio_ctx.currententity->origin, m_pPlayerInfo->prevgaitorigin, est_velocity );
	VectorCopy( nv2a_studio_ctx.currententity->origin, m_pPlayerInfo->prevgaitorigin );
	m_flGaitMovement = VectorLength( est_velocity );

	if( dt <= 0.0f || m_flGaitMovement / dt < 5.0f )
	{
		m_flGaitMovement = 0.0f;
		est_velocity[0] = 0.0f;
		est_velocity[1] = 0.0f;
	}

	if( est_velocity[1] == 0.0f && est_velocity[0] == 0.0f )
	{
		float flYawDiff = nv2a_studio_ctx.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;

		flYawDiff = flYawDiff - (int)( flYawDiff / 360 ) * 360;
		if( flYawDiff > 180.0f )
			flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f )
			flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else
			flYawDiff *= dt;

		m_pPlayerInfo->gaityaw += flYawDiff;
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - (int)( m_pPlayerInfo->gaityaw / 360 ) * 360;

		m_flGaitMovement = 0.0f;
	}
	else
	{
		m_pPlayerInfo->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180 / M_PI_F );
		if( m_pPlayerInfo->gaityaw > 180.0f )
			m_pPlayerInfo->gaityaw = 180.0f;
		if( m_pPlayerInfo->gaityaw < -180.0f )
			m_pPlayerInfo->gaityaw = -180.0f;
	}

}

/*
====================
StudioProcessGait

====================
*/
static void R_StudioProcessGait( entity_state_t *pplayer )
{
	mstudioseqdesc_t *pseqdesc;
	int   iBlend;
	float dt, flYaw; // view direction relative to movement

	if( nv2a_studio_ctx.currententity->curstate.sequence >= m_pStudioHeader->numseq )
		nv2a_studio_ctx.currententity->curstate.sequence = 0;

	dt = bound( 0.0f, g_studio.frametime, 1.0f );

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + nv2a_studio_ctx.currententity->curstate.sequence;

	R_StudioPlayerBlend( pseqdesc, &iBlend, &nv2a_studio_ctx.currententity->angles[PITCH] );

	nv2a_studio_ctx.currententity->latched.prevangles[PITCH] = nv2a_studio_ctx.currententity->angles[PITCH];
	nv2a_studio_ctx.currententity->curstate.blending[0] = iBlend;
	nv2a_studio_ctx.currententity->latched.prevblending[0] = nv2a_studio_ctx.currententity->curstate.blending[0];
	nv2a_studio_ctx.currententity->latched.prevseqblending[0] = nv2a_studio_ctx.currententity->curstate.blending[0];
	R_StudioEstimateGait( pplayer );

	// calc side to side turning
	flYaw = nv2a_studio_ctx.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;
	flYaw = flYaw - (int)( flYaw / 360 ) * 360;
	if( flYaw < -180.0f )
		flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f )
		flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw + 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw + 180.0f;
	}

	// adjust torso
	nv2a_studio_ctx.currententity->curstate.controller[0] = (( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	nv2a_studio_ctx.currententity->curstate.controller[1] = (( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	nv2a_studio_ctx.currententity->curstate.controller[2] = (( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	nv2a_studio_ctx.currententity->curstate.controller[3] = (( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	nv2a_studio_ctx.currententity->latched.prevcontroller[0] = nv2a_studio_ctx.currententity->curstate.controller[0];
	nv2a_studio_ctx.currententity->latched.prevcontroller[1] = nv2a_studio_ctx.currententity->curstate.controller[1];
	nv2a_studio_ctx.currententity->latched.prevcontroller[2] = nv2a_studio_ctx.currententity->curstate.controller[2];
	nv2a_studio_ctx.currententity->latched.prevcontroller[3] = nv2a_studio_ctx.currententity->curstate.controller[3];

	nv2a_studio_ctx.currententity->angles[YAW] = m_pPlayerInfo->gaityaw;
	if( nv2a_studio_ctx.currententity->angles[YAW] < -0 )
		nv2a_studio_ctx.currententity->angles[YAW] += 360.0f;
	nv2a_studio_ctx.currententity->latched.prevangles[YAW] = nv2a_studio_ctx.currententity->angles[YAW];

	if( pplayer->gaitsequence >= m_pStudioHeader->numseq )
		pplayer->gaitsequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + pplayer->gaitsequence;

	// calc gait frame
	if( pseqdesc->linearmovement[0] > 0 )
		m_pPlayerInfo->gaitframe += ( m_flGaitMovement / pseqdesc->linearmovement[0] ) * pseqdesc->numframes;
	else
		m_pPlayerInfo->gaitframe += pseqdesc->fps * dt;

	// do modulo
	m_pPlayerInfo->gaitframe = m_pPlayerInfo->gaitframe - (int)( m_pPlayerInfo->gaitframe / pseqdesc->numframes ) * pseqdesc->numframes;
	if( m_pPlayerInfo->gaitframe < 0 )
		m_pPlayerInfo->gaitframe += pseqdesc->numframes;
}

/*
===============
R_StudioDrawPlayer

===============
*/
static int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	int      m_nPlayerIndex;
	alight_t lighting;
	vec3_t   dir;

	m_nPlayerIndex = pplayer->number - 1;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gp_cl->maxclients )
		return 0;

	nv2a_studio_ctx.currentmodel = R_StudioSetupPlayerModel( m_nPlayerIndex );
	if( nv2a_studio_ctx.currentmodel == NULL )
		return 0;

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, nv2a_studio_ctx.currentmodel ));

	if( pplayer->gaitsequence )
	{
		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		vec3_t orig_angles = Vec3( nv2a_studio_ctx.currententity->angles );

		R_StudioProcessGait( pplayer );

		m_pPlayerInfo->gaitsequence = pplayer->gaitsequence;
		m_pPlayerInfo = NULL;

		R_StudioSetUpTransform( nv2a_studio_ctx.currententity );
		VectorCopy( orig_angles, nv2a_studio_ctx.currententity->angles );
	}
	else
	{
		nv2a_studio_ctx.currententity->curstate.controller[0] = 127;
		nv2a_studio_ctx.currententity->curstate.controller[1] = 127;
		nv2a_studio_ctx.currententity->curstate.controller[2] = 127;
		nv2a_studio_ctx.currententity->curstate.controller[3] = 127;
		nv2a_studio_ctx.currententity->latched.prevcontroller[0] = nv2a_studio_ctx.currententity->curstate.controller[0];
		nv2a_studio_ctx.currententity->latched.prevcontroller[1] = nv2a_studio_ctx.currententity->curstate.controller[1];
		nv2a_studio_ctx.currententity->latched.prevcontroller[2] = nv2a_studio_ctx.currententity->curstate.controller[2];
		nv2a_studio_ctx.currententity->latched.prevcontroller[3] = nv2a_studio_ctx.currententity->curstate.controller[3];

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		m_pPlayerInfo->gaitsequence = 0;

		R_StudioSetUpTransform( nv2a_studio_ctx.currententity );
	}

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		nv2a_studio_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
	R_StudioSetupBones( nv2a_studio_ctx.currententity );
	R_StudioSaveBones( );

	m_pPlayerInfo->renderframe = nv2a_current_frame_number();
	m_pPlayerInfo = NULL;

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( nv2a_studio_ctx.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( nv2a_studio_ctx.currententity->index );
			memcpy( ent->attachment, nv2a_studio_ctx.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		// change body if it's a menu entity
		if( cl_himodels->value && ( nv2a_studio_ctx.currentmodel != nv2a_studio_ctx.currententity->model || !FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD )))
		{
			// show highest resolution multiplayer model
			nv2a_studio_ctx.currententity->curstate.body = 255;
		}

		if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && ( nv2a_studio_ctx.currentmodel == nv2a_studio_ctx.currententity->model ))
			nv2a_studio_ctx.currententity->curstate.body = 1; // force helmet

		lighting.plightvec = dir;
		R_EntityDynamicLight( nv2a_studio_ctx.currententity, &lighting, FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );

		// get remap colors
		g_nTopColor = m_pPlayerInfo->topcolor;
		g_nBottomColor = m_pPlayerInfo->bottomcolor;

		if( g_nTopColor < 0 )
			g_nTopColor = 0;
		if( g_nTopColor > 360 )
			g_nTopColor = 360;
		if( g_nBottomColor < 0 )
			g_nBottomColor = 0;
		if( g_nBottomColor > 360 )
			g_nBottomColor = 360;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel( );
		m_pPlayerInfo = NULL;

		if( pplayer->weaponmodel )
		{
			cl_entity_t saveent = *nv2a_studio_ctx.currententity;
			model_t     *pweaponmodel = CL_ModelHandle( pplayer->weaponmodel );

			m_pStudioHeader = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, pweaponmodel );

			R_StudioMergeBones( nv2a_studio_ctx.currententity, pweaponmodel );
			R_StudioSetupLighting( &lighting );
			R_StudioRenderModel( );
			R_StudioCalcAttachments( );

			*nv2a_studio_ctx.currententity = saveent;
		}
	}

	return 1;
}

/*
===============
R_StudioDrawModel

===============
*/
static int R_StudioDrawModel( int flags )
{
	alight_t lighting;
	vec3_t   dir;

	if( nv2a_studio_ctx.currententity->curstate.renderfx == kRenderFxDeadPlayer )
	{
		entity_state_t deadplayer;
		int result;

		if( nv2a_studio_ctx.currententity->curstate.renderamt <= 0
		    || nv2a_studio_ctx.currententity->curstate.renderamt > gp_cl->maxclients )
			return 0;

		// get copy of player
		deadplayer = *R_StudioGetPlayerState( nv2a_studio_ctx.currententity->curstate.renderamt - 1 );

		// clear weapon, movement state
		deadplayer.number = nv2a_studio_ctx.currententity->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;

		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( nv2a_studio_ctx.currententity->curstate.angles, deadplayer.angles );
		VectorCopy( nv2a_studio_ctx.currententity->curstate.origin, deadplayer.origin );

		g_studio.interpolate = false;
		result = R_StudioDrawPlayer( flags, &deadplayer ); // draw as though it were a player
		g_studio.interpolate = true;

		return result;
	}

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, nv2a_studio_ctx.currentmodel ));

	R_StudioSetUpTransform( nv2a_studio_ctx.currententity );

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		nv2a_studio_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	if( nv2a_studio_ctx.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( nv2a_studio_ctx.currententity, nv2a_studio_ctx.currentmodel );
	else
		R_StudioSetupBones( nv2a_studio_ctx.currententity );

	R_StudioSaveBones();

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( nv2a_studio_ctx.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( nv2a_studio_ctx.currententity->index );
			memcpy( ent->attachment, nv2a_studio_ctx.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		lighting.plightvec = dir;
		R_EntityDynamicLight( nv2a_studio_ctx.currententity, &lighting, FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		// get remap colors
		g_nTopColor = nv2a_studio_ctx.currententity->curstate.colormap & 0xFF;
		g_nBottomColor = ( nv2a_studio_ctx.currententity->curstate.colormap & 0xFF00 ) >> 8;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel();
	}

	return 1;
}

/*
=================
R_StudioDrawModelInternal
=================
*/
static void R_StudioDrawModelInternal( cl_entity_t *e, int flags )
{
	if( !FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, &e->curstate );
		else
			R_StudioDrawModel( flags );
	}
	else if( unlikely( r_studio_builtin_renderer.value ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else
			R_StudioDrawModel( flags );
	}
	else
	{
		// select the properly method
		if( e->player )
			pStudioDraw->StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else
			pStudioDraw->StudioDrawModel( flags );
	}
}

static cl_entity_t *R_FindParentEntity( cl_entity_t *e, cl_entity_t **entities, uint num_entities )
{
	uint i;

	for( i = 0; i < num_entities; i++ )
	{
		if( entities[i]->index == e->curstate.aiment )
			return entities[i];
	}

	return NULL;
}

/*
=================
R_DrawStudioModel
=================
*/
void R_DrawStudioModel( cl_entity_t *e )
{
	R_StudioSetupTimings();

	if( e->player )
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER | STUDIO_EVENTS );
	}
	else if( e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( !parent || !parent->model || parent->model->type != mod_studio )
			return;

		parent = R_FindParentEntity( e, nv2a_solid_entities, ((uint)nv2a_num_solid_entities) );

		if( !parent )
			parent = R_FindParentEntity( e, nv2a_trans_entities, ((uint)nv2a_num_trans_entities) );

		if( parent )
		{
			nv2a_studio_ctx.currententity = parent;
			R_StudioDrawModelInternal( nv2a_studio_ctx.currententity, 0 );
			VectorCopy( nv2a_studio_ctx.currententity->curstate.origin, e->curstate.origin );
			VectorCopy( nv2a_studio_ctx.currententity->origin, e->origin );
			nv2a_studio_ctx.currententity = e;

			R_StudioDrawModelInternal( e, STUDIO_RENDER | STUDIO_EVENTS );
		}
	}
	else
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER | STUDIO_EVENTS );
	}
}


/*
=================
R_RunViewmodelEvents
=================
*/
void R_RunViewmodelEvents( void )
{
	int    i;

	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died
	if( ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	nv2a_studio_ctx.currententity = nv2a_studio_ctx.viewent;

	if( !nv2a_studio_ctx.currententity->model || nv2a_studio_ctx.currententity->model->type != mod_studio )
		return;

	R_StudioSetupTimings();

	vec3_t simorg = Vec3( gp_cl->simorg );
	for( i = 0; i < 4; i++ )
		VectorCopy( simorg, nv2a_studio_ctx.currententity->attachment[i] );
	nv2a_studio_ctx.currentmodel = nv2a_studio_ctx.currententity->model;

	R_StudioDrawModelInternal( nv2a_studio_ctx.currententity, STUDIO_EVENTS );
}

/*
=================
R_DrawViewModel
=================
*/
void R_DrawViewModel( void )
{
	cl_entity_t *view = nv2a_studio_ctx.viewent;

	R_GatherPlayerLight( view );

	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died - gl_studio.c:3335
	// also gates on RF_DRAW_CUBEMAP, a pass this renderer never draws (no env-map
	// capture exists here), kept anyway so this matches the whole real function.
	if( FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	nv2a_studio_ctx.blend = nv2a_fx_blend( view ) / 255.0f;
	if( !R_ModelOpaque( view->curstate.rendermode ) && nv2a_studio_ctx.blend <= 0.0f )
		return; // invisible ?

	nv2a_studio_ctx.currententity = view;

	if( !nv2a_studio_ctx.currententity->model )
		return;

	/* gl_studio.c:3348: adjust the depth range to prevent view model from
	   poking into walls - design note section 2: the world's own viewport
	   matrix already bakes z in [0, 65536] (nv2a_matrix_viewport,
	   r_context.c), so glDepthRange's own remap becomes a batch-state flag
	   the flush (r_context.c) uses to push a second, z-ranged copy of that
	   matrix instead of a real hardware depth-range register. Note: the
	   real reference has NO fFlipViewModel/pglFrontFace toggle here - that
	   logic lives in R_StudioDrawPoints (gl_studio.c:2055-2072, ported
	   there, not here); an earlier port had invented one at this spot,
	   which this port does not carry forward. */
	nv2a_studio_set_viewmodel_depthrange( 1 );
	nv2a_studio_ctx.currentmodel = nv2a_studio_ctx.currententity->model;

	switch( nv2a_studio_ctx.currententity->model->type )
	{
	case mod_alias:
		// Alias (Quake mdl) models are out of scope (spec outOfScope list) - not drawn.
		break;
	case mod_studio:
		R_StudioSetupTimings();
		R_StudioDrawModelInternal( nv2a_studio_ctx.currententity, STUDIO_RENDER );
		break;
	default:
		break; // the view model is never mod_brush/mod_sprite/mod_bad in practice
	}

	// restore depth range - gl_studio.c:3363
	nv2a_studio_set_viewmodel_depthrange( 0 );
}

/*
====================
R_StudioLoadTexture

load model texture with unique name

ref/nv2a: `srcpath`/`srclen` name the file this header's texture offsets
point into (R_StudioSkinSourceFile below), or srcpath is NULL. With a
source, the skin is registered LAZILY (nv2a_tex_register_studio_skin):
a named slot with no GPU storage, uploaded at 16 bpp on first bind and
kept under the studio skin cache's budget - divergences.md, lazy studio
skins. ref/gl uploads it here, at RGBA, unconditionally
(gl_studio.c R_StudioLoadTexture). Everything else in this function -
the DM_Base/remap pixel copy, the texture name, the flags, the
duplicated gl_texturenum - is unchanged, so ptexture->index is still a
live, named texnum for cl_remap.c.
====================
*/
static void R_StudioLoadTexture( model_t *mod, studiohdr_t *phdr, mstudiotexture_t *ptexture, const char *srcpath, uint32_t srclen )
{
	size_t    size;
	int       flags = 0;
	int       pixel_offset;
	char      texname[128], name[128], mdlname[128];
	texture_t *tx = NULL;

	if( ptexture->flags & STUDIO_NF_NORMALMAP )
		flags |= ( TF_NORMALMAP );

	// store some textures for remapping
	if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ) || !Q_strnicmp( ptexture->name, "remap", 5 ))
	{
		int  i, size;
		char val[6];
		byte *pixels;

		i = mod->numtextures;
		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t * ));
		size = ptexture->width * ptexture->height + 768;
		tx = Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		// store ranges into anim_min, anim_max etc
		if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ))
		{
			Q_strncpy( tx->name, "DM_Base", sizeof( tx->name ));
			tx->anim_min = PLATE_HUE_START; // topcolor start
			tx->anim_max = PLATE_HUE_END;   // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			tx->anim_total = SUIT_HUE_END; // bottomcolor end
		}
		else
		{
			Q_strncpy( tx->name, "DM_User", sizeof( tx->name )); // custom remapped
			Q_strncpy( val, ptexture->name + 7, 4 );
			tx->anim_min = bound( 0, Q_atoi( val ), 255 ); // topcolor start
			Q_strncpy( val, ptexture->name + 11, 4 );
			tx->anim_max = bound( 0, Q_atoi( val ), 255 ); // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			Q_strncpy( val, ptexture->name + 15, 4 );
			tx->anim_total = bound( 0, Q_atoi( val ), 255 ); // bottomcolor end
		}

		tx->width = ptexture->width;
		tx->height = ptexture->height;

		// the pixels immediately follow the structures
		pixels = (byte *)phdr + ptexture->index;
		memcpy( tx + 1, pixels, size );

		ptexture->flags |= STUDIO_NF_COLORMAP; // yes, this is colormap image
		flags |= TF_FORCE_COLOR;

		mod->numtextures++; // done
	}

	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_FileBase( ptexture->name, name, sizeof( name ));
	COM_StripExtension( mdlname );

	if( FBitSet( ptexture->flags, STUDIO_NF_NOMIPS ))
		SetBits( flags, TF_NOMIPMAP );

	if( FBitSet( gp_host->features, ENGINE_LOAD_DELUXEDATA ) && FBitSet( ptexture->flags, STUDIO_NF_MASKED ))
		flags |= TF_KEEP_SOURCE; // Paranoia2 texture alpha-tracing

	// build the texname
	Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );

	/* The file offset of this skin's pixels, before index is overwritten
	   with the texnum below - the lazy slot re-reads them from here. */
	pixel_offset = ptexture->index;
	ptexture->index = 0;

	/* Lazy: never for TF_KEEP_SOURCE (GL_TextureData must have the kept
	   8-bit original from the start, which only the eager load makes). */
	if( srcpath && pixel_offset > 0 && !FBitSet( flags, TF_KEEP_SOURCE ))
	{
		ptexture->index = nv2a_tex_register_studio_skin( texname, (texFlags_t)flags, srcpath, srclen,
			(uint32_t)pixel_offset, ptexture->flags, ptexture->width, ptexture->height );
	}

	if( !ptexture->index )
	{
		// NOTE: replace index with pointer to start of imagebuffer, ImageLib expected it
		// ptexture->index = (int)((byte *)phdr) + ptexture->index;
		gEngfuncs.Image_SetMDLPointer((byte *)phdr + pixel_offset );
		size = sizeof( mstudiotexture_t ) + ptexture->width * ptexture->height + 768;
		ptexture->index = GL_LoadTexture( texname, (byte *)ptexture, size, flags );
	}

	if( !ptexture->index )
	{
		ptexture->index = nv2a_tex_default();
	}
	else if( tx )
	{
		// duplicate texnum for easy acess
		tx->gl_texturenum = ptexture->index;
	}
}

/*
=================
R_StudioSkinSourceFile

ref/nv2a only: which file `phdr`'s texture offsets index into, and its
size, for the lazy skin upload's re-read. Mod_LoadStudioModel
(engine/common/mod_studio.c:1125-1195) calls Mod_StudioLoadTextures in
exactly two shapes:
  - with mod->cache.data itself: a byte copy of the model file
    Mod_LoadModel read as COM_FixSlashes(mod->name) (model.c:329-332);
  - with the header of the separate textures file, before cache.data is
    set (mod_studio.c:1149-1158): Mod_StudioTexName(mod->name), i.e. the
    name with its extension replaced by "T.mdl" (mod_studio.c:947-956).
The file is opened once here only for its length, which the upload
checks again so a changed file is never read at stale offsets. False
(eager upload instead) if it cannot be opened.
=================
*/
static qboolean R_StudioSkinSourceFile( model_t *mod, studiohdr_t *phdr, char *path, size_t pathsize, uint32_t *length )
{
	file_t *f;
	fs_offset_t len;

	Q_strncpy( path, mod->name, pathsize );
	COM_FixSlashes( path );

	if( (void *)phdr != mod->cache.data )
	{
		COM_StripExtension( path );
		Q_strncat( path, "T.mdl", pathsize );
	}

	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	len = gEngfuncs.fsapi->FileLength( f );
	gEngfuncs.fsapi->Close( f );

	if( len <= 0 || (uint64_t)len > 0xFFFFFFFFu )
		return false;
	*length = (uint32_t)len;
	return true;
}

/*
=================
Mod_StudioLoadTextures
=================
*/
void GAME_EXPORT Mod_StudioLoadTextures( model_t *mod, void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;
	char srcpath[MAX_QPATH];
	uint32_t srclen = 0;
	qboolean lazy;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr ) + phdr->textureindex );
	if( phdr->textureindex > 0 )
	{
		lazy = R_StudioSkinSourceFile( mod, phdr, srcpath, sizeof( srcpath ), &srclen );
		for( int i = 0; i < phdr->numtextures; i++ )
			R_StudioLoadTexture( mod, phdr, &ptexture[i], lazy ? srcpath : NULL, srclen );
	}
}

/*
=================
Mod_StudioUnloadTextures
=================
*/
void Mod_StudioUnloadTextures( void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr ) + phdr->textureindex );

	// release all textures
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( ptexture[i].index == nv2a_tex_default() )
			continue;
		GL_FreeTexture( ptexture[i].index );
	}
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	R_EntityDynamicLight( ent, plight, FBitSet( nv2a_studio_ctx.rvp_flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );
}

qboolean R_StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw )
{
	cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin         = (void *)R_StudioSetupSkin;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = (void *)R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GL_StudioDrawShadow;
	api->GL_SetRenderMode        = GL_StudioSetRenderMode;
	api->StudioSetRenderamt      = R_StudioSetRenderamt;
	api->StudioSetCullState      = R_StudioSetCullState;
	api->StudioRenderShadow      = R_StudioRenderShadow;

	pDefaultDraw->version         = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

void R_StudioSetDrawInterface( r_studio_interface_t *pDraw )
{
	pStudioDraw = pDraw;
}
