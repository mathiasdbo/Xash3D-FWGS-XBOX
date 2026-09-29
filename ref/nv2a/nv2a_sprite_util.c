/*
nv2a_sprite_util.c - see nv2a_sprite_util.h for the contract of each
function. Each body cites the gl_sprite.c lines it transcribes.
*/
#include "nv2a_sprite_util.h"

nv2a_sprite_state_t nv2a_sprite_begin_state( int rendermode, int has_lightmap, nv2a_sprite_state_t cur )
{
	nv2a_sprite_state_t s = cur;

	// select properly rendermode (gl_sprite.c:403-426)
	switch( rendermode )
	{
	case NV2A_SPRITE_RENDER_TRANS_ALPHA:
		s.depth_mask = 0;
		// fallthrough
	case NV2A_SPRITE_RENDER_TRANS_COLOR:
	case NV2A_SPRITE_RENDER_TRANS_TEXTURE:
		s.blend_enable = 1;
		s.sfactor = NV2A_SPRITE_SFACTOR_SRC_ALPHA;
		s.dfactor = NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case NV2A_SPRITE_RENDER_GLOW:
		s.depth_test = 0;
		// fallthrough
	case NV2A_SPRITE_RENDER_TRANS_ADD:
		s.blend_enable = 1;
		s.sfactor = NV2A_SPRITE_SFACTOR_SRC_ALPHA;
		s.dfactor = NV2A_SPRITE_DFACTOR_ONE;
		s.depth_mask = 0;
		break;
	case NV2A_SPRITE_RENDER_NORMAL:
	default:
		s.blend_enable = 0;
		break;
	}

	// all sprites can have color (gl_sprite.c:429-430; GL_MODULATE is the
	// only combiner this renderer's TriAPI has, ps_2d)
	s.alpha_test_enable = 1;

	// gl_sprite.c:455
	if( has_lightmap )
		s.alpha_ref = NV2A_SPRITE_ALPHA_REF_LIGHTMAP;

	return s;
}

nv2a_sprite_state_t nv2a_sprite_lightmap_state( int r_lightmap, nv2a_sprite_state_t cur )
{
	nv2a_sprite_state_t s = cur;

	// gl_sprite.c:548-553
	s.blend_enable = r_lightmap ? 0 : 1;
	s.depth_func_equal = 1;
	s.alpha_test_enable = 0;
	s.sfactor = NV2A_SPRITE_SFACTOR_ZERO;
	s.dfactor = NV2A_SPRITE_DFACTOR_SRC_COLOR;
	return s;
}

nv2a_sprite_state_t nv2a_sprite_lightmap_end_state( nv2a_sprite_state_t cur )
{
	nv2a_sprite_state_t s = cur;

	// gl_sprite.c:559-561
	s.alpha_ref = NV2A_SPRITE_ALPHA_REF_DEFAULT;
	s.depth_func_equal = 0;
	s.blend_enable = 0;
	return s;
}

nv2a_sprite_state_t nv2a_sprite_end_state( int rendermode, nv2a_sprite_state_t cur )
{
	nv2a_sprite_state_t s = cur;

	// gl_sprite.c:567-568
	s.alpha_test_enable = 0;
	s.depth_mask = 1;

	// gl_sprite.c:573-578
	if( rendermode != NV2A_SPRITE_RENDER_NORMAL )
	{
		s.blend_enable = 0;
		s.depth_test = 1;
	}
	return s;
}

float nv2a_sprite_group_frames( const float *intervals, int numframes, float time, int do_interp, int *oldframe, int *curframe )
{
	float fullinterval = intervals[numframes - 1];
	float jinterval = numframes > 1 ? intervals[1] - intervals[0] : intervals[0];
	float jtime = 0.0f;
	float lerpFrac = 1.0f;
	float targettime;
	int i, j;

	/* gl_sprite.c:106 reads pintervals[1] unconditionally; a one-frame
	   group is legal in the file format (Mod_SpriteLoadGroup takes any
	   numframes), where that reads one float past the array. For one
	   frame the loop below never runs, so both indices are 0 and the
	   caller draws that single frame without looking at the lerp
	   (R_DrawSpriteModel's oldframe == frame branch, gl_sprite.c:517);
	   reading intervals[0] instead only avoids the out-of-bounds read. */

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by zero
	targettime = time - ((int)( time / fullinterval )) * fullinterval;

	// LadyHavoc: since I can't measure the time properly when it loops from numframes - 1 to 0,
	// i instead measure the time of the first frame, hoping it is consistent
	for( i = 0, j = numframes - 1; i < ( numframes - 1 ); i++ )
	{
		if( intervals[i] > targettime )
			break;
		j = i;
		jinterval = intervals[i] - jtime;
		jtime = intervals[i];
	}

	if( do_interp )
		lerpFrac = ( targettime - jtime ) / jinterval;
	else j = i; // no lerping

	*oldframe = j;
	*curframe = i;
	return lerpFrac;
}

/* xash3d_mathlib.h:92. */
#define NV2A_SPRITE_Q_RINT( x ) (( x ) < 0.0f ? ((int)(( x ) - 0.5f )) : ((int)(( x ) + 0.5f )))

int nv2a_sprite_angle_frame( float view_yaw, float ent_yaw )
{
	float a = ( view_yaw - ent_yaw + 45.0f ) / 360 * 8;
	return (int)( NV2A_SPRITE_Q_RINT( a ) - 4 ) & 7;
}
