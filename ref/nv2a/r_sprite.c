/*
r_sprite.c - X9-X step 6: sprite renderer for ref/nv2a. Whole-function port
of ref/gl/gl_sprite.c (function names and gl_sprite.c:<line> citations kept
per function), drawn through the TriAPI internals in r_context.c
(Begin/TexCoord2f/Color4f/Vertex3fv/End/CullFace) the way r_studio.c
ports gl_studio.c.

What is not here because it is not in gl_sprite.c either: loading. The
engine loads every sprite and uploads its frames through GL_LoadTexture
(Mod_LoadSpriteModel -> Mod_SpriteLoadTextures, engine/client/cl_sprite.c:
237, with the sprite's own palette installed per texFormat: #texgamma.pal
for SPR_NORMAL/SPR_ADDITIVE, #gradient.pal for SPR_INDEXALPHA, #masked.pal
for SPR_ALPHTEST), and picks the non-interpolated frame itself
(R_GetSpriteFrame, cl_sprite.c:32). Those frames land in r_image.c's
texture table like any other 2D texture, stored by the same A8/A8Y8/RGBA32
classifier (divergences.md #131).

The GL state R_DrawSpriteModel sets is computed by nv2a_sprite_util.c
(host-tested) and copied into nv2a_2d_desired, the shadow the TriAPI flush
pushes; two fields were added to that shadow for this file
(depth_test_disable for kRenderGlow, depth_func_equal for the sprite
'lightmap' pass - r_studio_bridge.h).
*/
#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_WARN */
#include "pm_local.h"    /* PM_GLASS_IGNORE / PM_STUDIO_IGNORE, as gl_sprite.c:17 */
#include "sprite.h"
#include "studio.h"      /* MAXSTUDIOATTACHMENTS */
#include "entity_types.h"
#include "triangleapi.h"
#include "ref_params.h"  /* RF_DRAW_CUBEMAP */
#include "nv2a_texture.h"
#include "nv2a_sprite_util.h"
#include "r_studio_bridge.h"
#include "r_sprite.h"

/* nv2a_sprite_util.h's mirrors of const.h, pinned here where const.h is
   available (the nv_regs.h mirrors are pinned in r_context.c). */
STATIC_ASSERT( NV2A_SPRITE_RENDER_NORMAL == kRenderNormal, "NV2A_SPRITE_RENDER_NORMAL drifted from const.h" );
STATIC_ASSERT( NV2A_SPRITE_RENDER_TRANS_COLOR == kRenderTransColor, "NV2A_SPRITE_RENDER_TRANS_COLOR drifted from const.h" );
STATIC_ASSERT( NV2A_SPRITE_RENDER_TRANS_TEXTURE == kRenderTransTexture, "NV2A_SPRITE_RENDER_TRANS_TEXTURE drifted from const.h" );
STATIC_ASSERT( NV2A_SPRITE_RENDER_GLOW == kRenderGlow, "NV2A_SPRITE_RENDER_GLOW drifted from const.h" );
STATIC_ASSERT( NV2A_SPRITE_RENDER_TRANS_ALPHA == kRenderTransAlpha, "NV2A_SPRITE_RENDER_TRANS_ALPHA drifted from const.h" );
STATIC_ASSERT( NV2A_SPRITE_RENDER_TRANS_ADD == kRenderTransAdd, "NV2A_SPRITE_RENDER_TRANS_ADD drifted from const.h" );

#define GLARE_FALLOFF	19000.0f

/* ref/gl registers r_traceglow in GL_InitCommands (gl_opengl.c:32,1149);
   this renderer registers it with the sprite code that reads it. */
static CVAR_DEFINE_AUTO( r_traceglow, "0", FCVAR_GLCONFIG, "cull flares behind models" );

/* ref/gl's r_stats sprite counters (gl_local.h:243,251). */
static struct
{
	uint c_sprite_polys;
	uint c_sprite_models_drawn;
} nv2a_sprite_stats;

/* The RI.rvp fields, RI.v* vectors and tr.blend gl_sprite.c reads - set
   by r_context.c (r_sprite.h). */
static struct
{
	int          rvp_flags;
	vec3_t       vieworigin;
	vec3_t       viewangles;
	int          viewport[4];
	vec3_t       vforward, vright, vup;
	float        blend;
	cl_entity_t *viewent; /* tr.viewent, PARM_GET_VIEWENT_PTR (gl_opengl.c's R_Init) */
} RI;

void R_SpriteInit( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_traceglow );
	RI.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
}

void R_SpriteSetFrameContext( int rvp_flags, const vec3_t vieworigin, const vec3_t viewangles,
	const int viewport[4], const vec3_t vforward, const vec3_t vright, const vec3_t vup )
{
	RI.rvp_flags = rvp_flags;
	VectorCopy( vieworigin, RI.vieworigin );
	VectorCopy( viewangles, RI.viewangles );
	RI.viewport[0] = viewport[0];
	RI.viewport[1] = viewport[1];
	RI.viewport[2] = viewport[2];
	RI.viewport[3] = viewport[3];
	VectorCopy( vforward, RI.vforward );
	VectorCopy( vright, RI.vright );
	VectorCopy( vup, RI.vup );
}

void R_SpriteSetBlend( float blend )
{
	RI.blend = blend;
}

/* R_AllowFog: this renderer has no fog (TriRenderMode's own comment,
   r_context.c; r_studio.c keeps the same no-op). */
static void R_AllowFog( qboolean allowed )
{
	(void)allowed;
}

/* The GL state gl_sprite.c changes, read from and written back to the
   TriAPI's shadow. */
static nv2a_sprite_state_t R_SpriteGetGLState( void )
{
	nv2a_sprite_state_t s;

	s.blend_enable      = nv2a_2d_desired.blend_enable;
	s.sfactor           = nv2a_2d_desired.sfactor;
	s.dfactor           = nv2a_2d_desired.dfactor;
	s.depth_mask        = nv2a_2d_desired.depth_mask;
	s.depth_test        = !nv2a_2d_desired.depth_test_disable;
	s.depth_func_equal  = nv2a_2d_desired.depth_func_equal;
	s.alpha_test_enable = nv2a_2d_desired.alpha_test_enable;
	s.alpha_ref         = nv2a_2d_desired.alpha_ref;
	return s;
}

static void R_SpriteSetGLState( nv2a_sprite_state_t s )
{
	nv2a_2d_desired.blend_enable       = s.blend_enable ? true : false;
	nv2a_2d_desired.sfactor            = s.sfactor;
	nv2a_2d_desired.dfactor            = s.dfactor;
	nv2a_2d_desired.depth_mask         = s.depth_mask ? true : false;
	nv2a_2d_desired.depth_test_disable = s.depth_test ? false : true;
	nv2a_2d_desired.depth_func_equal   = s.depth_func_equal ? true : false;
	nv2a_2d_desired.alpha_test_enable  = s.alpha_test_enable ? true : false;
	nv2a_2d_desired.alpha_ref          = s.alpha_ref;
}

/* CL_GetEntityByIndex (ref/gl's gl_local.h inline) - r_context.c's
   bounds-checked copy, as r_studio.c uses. */
static cl_entity_t *CL_GetEntityByIndex( int index )
{
	return nv2a_get_entity_by_index( index );
}

#define CL_IsViewEntityLocalPlayer() ( gp_cl->viewentity == ( gp_cl->playernum + 1 ))

/*
=============
R_CullModel

ref/gl/gl_cull.c:43, on r_context.c's frustum (nv2a_frustum_cull_box is
R_CullBox, gl_cull.c:33). A sprite is never the view entity in practice,
but the whole function is kept.
=============
*/
static qboolean R_CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax )
{
	if( e == RI.viewent )
	{
		if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
			return true;

		if( !FBitSet( RI.rvp_flags, RF_DRAW_CUBEMAP ) && !ENGINE_GET_PARM( PARM_THIRDPERSON ) && CL_IsViewEntityLocalPlayer())
			return false;

		return true;
	}

	return nv2a_frustum_cull_box( absmin, absmax );
}

/*
=============
TriWorldToScreen

ref/gl/gl_triapi.c:223 - R_WorldToScreen's NDC scaled into the view's
viewport. Not the engine's TriWorldToScreen (cl_game.c:3232), which
returns NDC unscaled.
=============
*/
static int TriWorldToScreen( const float *world, float *screen )
{
	int retval = nv2a_world_to_screen( world, screen );

	screen[0] =  0.5f * screen[0] * (float)RI.viewport[2];
	screen[1] = -0.5f * screen[1] * (float)RI.viewport[3];
	screen[0] += 0.5f * (float)RI.viewport[2];
	screen[1] += 0.5f * (float)RI.viewport[3];

	return retval;
}

/*
=============
TriSpriteTexture

ref/gl/gl_triapi.c:242. GL_Bind already substitutes the default texture
for a texnum out of range (r_image.c), the second check below.
=============
*/
int TriSpriteTexture( model_t *pSpriteModel, int frame )
{
	mspriteframe_t *pframe;
	int gl_texturenum;

	if( !pSpriteModel || pSpriteModel->type != mod_sprite || !pSpriteModel->cache.data )
		return 0;

	pframe = gEngfuncs.R_GetSpriteFrame( pSpriteModel, frame, 0.0f );
	if( !pframe )
		return 0;

	gl_texturenum = pframe->gl_texturenum;
	if( gl_texturenum == 0 )
		return 0;

	if( gl_texturenum <= 0 || gl_texturenum >= NV2A_MAX_TEXTURES )
		gl_texturenum = nv2a_tex_default();

	GL_Bind( XASH_TEXTURE0, gl_texturenum );

	return 1;
}

/*
================
R_GetSpriteFrameInterpolant

gl_sprite.c:32.
NOTE: we using prevblending[0] and [1] for holds interval
between frames where are we lerping
================
*/
static float R_GetSpriteFrameInterpolant( cl_entity_t *ent, mspriteframe_t **oldframe, mspriteframe_t **curframe )
{
	msprite_t *psprite = ent->model->cache.data;
	int frame = (int)ent->curstate.frame;
	float lerpFrac = 1.0f;

	// misc info
	int m_fDoInterp = ( ent->curstate.effects & EF_NOINTERP ) ? false : true;

	if( frame < 0 )
	{
		frame = 0;
	}
	else if( frame >= psprite->numframes )
	{
		gEngfuncs.Con_Reportf( S_WARN "%s: no such frame %d (%s)\n", __func__, frame, ent->model->name );
		frame = psprite->numframes - 1;
	}

	if( psprite->frames[frame].type == FRAME_SINGLE )
	{
		if( m_fDoInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_SINGLE )
			{
				// this can be happens when rendering switched between single and angled frames
				// or change model on replace delta-entity
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = ( gp_cl->time - ent->latched.sequencetime ) * 11.0f;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		if( ent->latched.prevblending[0] >= psprite->numframes )
		{
			// reset interpolation on change model
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			ent->latched.sequencetime = gp_cl->time;
			lerpFrac = 0.0f;
		}

		// get the interpolated frames
		if( oldframe ) *oldframe = psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( curframe ) *curframe = psprite->frames[frame].frameptr;
	}
	else if( psprite->frames[frame].type == FRAME_GROUP )
	{
		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		int i, j;

		// gl_sprite.c:102-128, nv2a_sprite_util.c (host-tested)
		lerpFrac = nv2a_sprite_group_frames( pspritegroup->intervals, pspritegroup->numframes, (float)gp_cl->time, m_fDoInterp, &j, &i );

		// get the interpolated frames
		if( oldframe ) *oldframe = pspritegroup->frames[j];
		if( curframe ) *curframe = pspritegroup->frames[i];
	}
	else if( psprite->frames[frame].type == FRAME_ANGLED )
	{
		// e.g. doom-style sprite monsters
		float yaw = ent->angles[YAW];
		int angleframe = nv2a_sprite_angle_frame( RI.viewangles[1], yaw ); // gl_sprite.c:138
		mspritegroup_t *pspritegroup;

		if( m_fDoInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_ANGLED )
			{
				// this can be happens when rendering switched between single and angled frames
				// or change model on replace delta-entity
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = ( gp_cl->time - ent->latched.sequencetime ) * ent->curstate.framerate;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		pspritegroup = (mspritegroup_t *)psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( oldframe ) *oldframe = pspritegroup->frames[angleframe];

		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		if( curframe ) *curframe = pspritegroup->frames[angleframe];
	}

	return lerpFrac;
}

/*
================
R_CullSpriteModel

gl_sprite.c:192. Cull sprite model by bbox
================
*/
static qboolean R_CullSpriteModel( cl_entity_t *e, vec3_t origin )
{
	vec3_t sprite_mins, sprite_maxs;
	float scale = 1.0f;

	if( !e->model->cache.data )
		return true;

	if( e->curstate.scale > 0.0f )
		scale = e->curstate.scale;

	// scale original bbox (no rotation for sprites)
	VectorScale( e->model->mins, scale, sprite_mins );
	VectorScale( e->model->maxs, scale, sprite_maxs );

	VectorAdd( sprite_mins, origin, sprite_mins );
	VectorAdd( sprite_maxs, origin, sprite_maxs );

	return R_CullModel( e, sprite_mins, sprite_maxs );
}

/*
================
R_SpriteGlowBlend

gl_sprite.c:220. Set sprite brightness factor
================
*/
static float R_SpriteGlowBlend( vec3_t origin, int rendermode, int renderfx, float *pscale )
{
	vec3_t glowDist;
	float dist, brightness;

	(void)rendermode;

	VectorSubtract( origin, RI.vieworigin, glowDist );
	dist = VectorLength( glowDist );

	if( !FBitSet( RI.rvp_flags, RF_DRAW_CUBEMAP ))
	{
		pmtrace_t *tr = gEngfuncs.EV_VisTraceLine( RI.vieworigin, origin, r_traceglow.value ? PM_GLASS_IGNORE : ( PM_GLASS_IGNORE | PM_STUDIO_IGNORE ));

		if(( 1.0f - tr->fraction ) * dist > 8.0f )
			return 0.0f;
	}

	if( renderfx == kRenderFxNoDissipation )
		return 1.0f;

	brightness = GLARE_FALLOFF / ( dist * dist );
	brightness = bound( 0.05f, brightness, 1.0f );
	*pscale *= dist * ( 1.0f / 200.0f );

	return brightness;
}

/*
================
R_SpriteOccluded

gl_sprite.c:251. Do occlusion test for glow-sprites
================
*/
static qboolean R_SpriteOccluded( cl_entity_t *e, vec3_t origin, float *pscale )
{
	if( e->curstate.rendermode == kRenderGlow )
	{
		float blend;
		vec3_t v;

		TriWorldToScreen( origin, v );

		if( v[0] < RI.viewport[0] || v[0] > RI.viewport[0] + RI.viewport[2] )
			return true; // do scissor
		if( v[1] < RI.viewport[1] || v[1] > RI.viewport[1] + RI.viewport[3] )
			return true; // do scissor

		blend = R_SpriteGlowBlend( origin, e->curstate.rendermode, e->curstate.renderfx, pscale );
		RI.blend *= blend;

		if( blend <= 0.01f )
			return true; // faded
	}
	else
	{
		if( R_CullSpriteModel( e, origin ))
			return true;
	}

	return false;
}

/*
=================
R_DrawSpriteQuad

gl_sprite.c:285. pglBegin( GL_QUADS ) is the TriAPI's TRI_QUADS; the
texture coordinates stay normalized, Vertex3f scales them to texels for
this renderer's linear textures (r_context.c, divergences.md #100).
=================
*/
static void R_DrawSpriteQuad( mspriteframe_t *frame, vec3_t org, vec3_t v_right, vec3_t v_up, float scale )
{
	vec3_t point;

	nv2a_sprite_stats.c_sprite_polys++;

	Begin( TRI_QUADS );
		TexCoord2f( 0.0f, 1.0f );
		VectorMA( org, frame->down * scale, v_up, point );
		VectorMA( point, frame->left * scale, v_right, point );
		Vertex3fv( point );
		TexCoord2f( 0.0f, 0.0f );
		VectorMA( org, frame->up * scale, v_up, point );
		VectorMA( point, frame->left * scale, v_right, point );
		Vertex3fv( point );
		TexCoord2f( 1.0f, 0.0f );
		VectorMA( org, frame->up * scale, v_up, point );
		VectorMA( point, frame->right * scale, v_right, point );
		Vertex3fv( point );
		TexCoord2f( 1.0f, 1.0f );
		VectorMA( org, frame->down * scale, v_up, point );
		VectorMA( point, frame->right * scale, v_right, point );
		Vertex3fv( point );
	End();
}

/* gl_sprite.c:311. */
static qboolean R_SpriteHasLightmap( cl_entity_t *e, int texFormat )
{
	if( !r_sprite_lighting->value )
		return false;

	if( texFormat != SPR_ALPHTEST )
		return false;

	if( FBitSet( e->curstate.effects, EF_FULLBRIGHT ))
		return false;

	if( e->curstate.renderamt <= 127 )
		return false;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
	case kRenderTransAlpha:
	case kRenderTransTexture:
		break;
	default:
		return false;
	}

	return true;
}

/*
=================
R_SpriteAllowLerping

gl_sprite.c:343.
=================
*/
static qboolean R_SpriteAllowLerping( cl_entity_t *e, msprite_t *psprite )
{
	if( !r_sprite_lerping->value )
		return false;

	if( psprite->numframes <= 1 )
		return false;

	if( psprite->texFormat != SPR_ADDITIVE )
		return false;

	if( e->curstate.rendermode == kRenderNormal || e->curstate.rendermode == kRenderTransAlpha )
		return false;

	return true;
}

/*
=================
R_DrawSpriteModel

gl_sprite.c:365. The GL calls are replaced by the shadow state they stand
for (nv2a_sprite_util.c); pglColor4f is the TriAPI's Color4f, GL_Cull is
CullFace, GL_Bind is r_image.c's.
=================
*/
void R_DrawSpriteModel( cl_entity_t *e )
{
	model_t *model;
	msprite_t *psprite;
	vec3_t color, color2 = { 0.0f };
	vec3_t v_right, v_up;
	vec3_t origin;
	mspriteframe_t *frame = NULL, *oldframe = NULL;
	float lerp = 1.0f;
	float scale;
	int type;

	if( FBitSet( RI.rvp_flags, RF_DRAW_CUBEMAP ))
		return;

	model = e->model;
	psprite = (msprite_t *)model->cache.data;
	VectorCopy( e->origin, origin ); // set render origin

	// do movewith
	if( e->curstate.aiment > 0 && e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( parent && parent->model )
		{
			if( parent->model->type == mod_studio && e->curstate.body > 0 )
			{
				int num = bound( 1, e->curstate.body, MAXSTUDIOATTACHMENTS );
				VectorCopy( parent->attachment[num - 1], origin );
			}
			else VectorCopy( parent->origin, origin );
		}
	}

	scale = e->curstate.scale;
	if( !scale ) scale = 1.0f;

	if( R_SpriteOccluded( e, origin, &scale ))
		return; // sprite culled

	nv2a_sprite_stats.c_sprite_models_drawn++;

	if( e->curstate.rendermode == kRenderGlow || e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( false );

	// select properly rendermode, all sprites can have color
	// (gl_sprite.c:403-430, and GL_GREATER 1/3 for a lit sprite, :455)
	R_SpriteSetGLState( nv2a_sprite_begin_state( e->curstate.rendermode, R_SpriteHasLightmap( e, psprite->texFormat ), R_SpriteGetGLState()));

	// NOTE: never pass sprites with rendercolor '0 0 0' it's a stupid Valve Hammer Editor bug
	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
	{
		color[0] = (float)e->curstate.rendercolor.r * ( 1.0f / 255.0f );
		color[1] = (float)e->curstate.rendercolor.g * ( 1.0f / 255.0f );
		color[2] = (float)e->curstate.rendercolor.b * ( 1.0f / 255.0f );
	}
	else
	{
		color[0] = 1.0f;
		color[1] = 1.0f;
		color[2] = 1.0f;
	}

	if( R_SpriteHasLightmap( e, psprite->texFormat ))
	{
		colorVec lightColor = R_LightPoint( origin );
		// FIXME: collect light from dlights?
		color2[0] = (float)lightColor.r * ( 1.0f / 255.0f );
		color2[1] = (float)lightColor.g * ( 1.0f / 255.0f );
		color2[2] = (float)lightColor.b * ( 1.0f / 255.0f );
		// NOTE: sprites with 'lightmap' looks ugly when alpha func is GL_GREATER 0.0
		// NOTE: make them easier to see with 0.3333, was 0.5 in original
		// (the alpha func itself is set by nv2a_sprite_begin_state above)
	}

	if( R_SpriteAllowLerping( e, psprite ))
		lerp = R_GetSpriteFrameInterpolant( e, &oldframe, &frame );
	else frame = oldframe = gEngfuncs.R_GetSpriteFrame( model, e->curstate.frame, e->angles[YAW] );

	type = psprite->type;

	// automatically roll parallel sprites if requested
	if( e->angles[ROLL] != 0.0f && type == SPR_FWD_PARALLEL )
		type = SPR_FWD_PARALLEL_ORIENTED;

	switch( type )
	{
	case SPR_ORIENTED:
	{
		vec3_t v_forward;
		AngleVectors( e->angles, v_forward, v_right, v_up );
		VectorScale( v_forward, 0.01f, v_forward ); // to avoid z-fighting
		VectorSubtract( origin, v_forward, origin );
		break;
	}
	case SPR_FACING_UPRIGHT:
		VectorSet( v_right, origin[1] - RI.vieworigin[1], -( origin[0] - RI.vieworigin[0] ), 0.0f );
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorNormalize( v_right );
		break;
	case SPR_FWD_PARALLEL_UPRIGHT:
	{
		float dot = RI.vforward[2];
		/* Returns with the render-mode state above still set, as the
		   reference does; it does not outlive the frame here either -
		   R_Set2DMode(false) re-enables the depth test and
		   nv2a_2d_reset_shadow_for_3d sets LEQUAL (r_context.c), as
		   gl_draw.c's R_Set2DMode and R_Clear do upstream. */
		if(( dot > 0.999848f ) || ( dot < -0.999848f )) // cos(1 degree) = 0.999848
			return; // invisible
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorSet( v_right, RI.vforward[1], -RI.vforward[0], 0.0f );
		VectorNormalize( v_right );
		break;
	}
	case SPR_FWD_PARALLEL_ORIENTED:
	{
		float angle = e->angles[ROLL] * ( M_PI2 / 360.0f );
		float sr, cr;
		int i;
		SinCos( angle, &sr, &cr );
		for( i = 0; i < 3; i++ )
		{
			v_right[i] = ( RI.vright[i] * cr + RI.vup[i] * sr );
			v_up[i] = RI.vright[i] * -sr + RI.vup[i] * cr;
		}
		break;
	}
	case SPR_FWD_PARALLEL: // normal sprite
	default:
		VectorCopy( RI.vright, v_right );
		VectorCopy( RI.vup, v_up );
		break;
	}

	if( psprite->facecull == SPR_CULL_NONE )
		CullFace( TRI_NONE );

	if( oldframe == frame )
	{
		// draw the single non-lerped frame
		Color4f( color[0], color[1], color[2], RI.blend );
		GL_Bind( XASH_TEXTURE0, frame->gl_texturenum );
		R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
	}
	else
	{
		// draw two combined lerped frames
		float ilerp;

		lerp = bound( 0.0f, lerp, 1.0f );
		ilerp = 1.0f - lerp;

		if( ilerp != 0.0f )
		{
			Color4f( color[0], color[1], color[2], RI.blend * ilerp );
			GL_Bind( XASH_TEXTURE0, oldframe->gl_texturenum );
			R_DrawSpriteQuad( oldframe, origin, v_right, v_up, scale );
		}

		if( lerp != 0.0f )
		{
			Color4f( color[0], color[1], color[2], RI.blend * lerp );
			GL_Bind( XASH_TEXTURE0, frame->gl_texturenum );
			R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
		}
	}

	// draw the sprite 'lightmap' :-)
	if( R_SpriteHasLightmap( e, psprite->texFormat ))
	{
		// gl_sprite.c:548-554
		R_SpriteSetGLState( nv2a_sprite_lightmap_state( r_lightmap->value != 0.0f, R_SpriteGetGLState()));

		Color4f( color2[0], color2[1], color2[2], RI.blend );
		GL_Bind( XASH_TEXTURE0, nv2a_tex_white());
		R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );

		// gl_sprite.c:559-561
		R_SpriteSetGLState( nv2a_sprite_lightmap_end_state( R_SpriteGetGLState()));
	}

	if( psprite->facecull == SPR_CULL_NONE )
		CullFace( TRI_FRONT );

	if( e->curstate.rendermode == kRenderGlow || e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( true );

	// gl_sprite.c:567-578 (GL_REPLACE is not a state this renderer's
	// TriAPI has: its only combiner is ps_2d's modulate)
	R_SpriteSetGLState( nv2a_sprite_end_state( e->curstate.rendermode, R_SpriteGetGLState()));
}
