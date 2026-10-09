/*
nv2a_studio_util.c - see nv2a_studio_util.h for what this is and why it is
pure (host-tested in tools/host-tests/nv2a_arena_host_test.c).
*/
#include "nv2a_studio_util.h"

nv2a_studio_mesh_state_t nv2a_studio_mesh_state( int face_flags, int entity_opaque, nv2a_studio_mesh_state_t current )
{
	nv2a_studio_mesh_state_t out = current;

	if( face_flags & NV2A_STUDIO_NF_MASKED )
	{
		/* gl_studio.c:2090-2096: pglEnable(GL_ALPHA_TEST); pglAlphaFunc(GL_GREATER,0.5f); pglDepthMask(GL_TRUE). */
		out.alpha_test_enable = 1;
		out.alpha_ref = NV2A_STUDIO_ALPHA_REF_MASKED;
		out.depth_mask = 1;
	}
	else if( face_flags & NV2A_STUDIO_NF_ADDITIVE )
	{
		if( entity_opaque )
		{
			/* gl_studio.c:2100-2106: pglBlendFunc(GL_ONE,GL_ONE); pglDepthMask(GL_FALSE); pglEnable(GL_BLEND). */
			out.blend_enable = 1;
			out.sfactor = NV2A_STUDIO_SFACTOR_ONE;
			out.dfactor = NV2A_STUDIO_DFACTOR_ONE;
			out.depth_mask = 0;
		}
		else
		{
			/* gl_studio.c:2108: pglBlendFunc(GL_SRC_ALPHA,GL_ONE) - the entity's own
			   render-mode blend already has depth_mask/blend_enable set, only the
			   factors change. */
			out.blend_enable = 1;
			out.sfactor = NV2A_STUDIO_SFACTOR_SRC_ALPHA;
			out.dfactor = NV2A_STUDIO_DFACTOR_ONE;
		}
	}

	return out;
}

void nv2a_studio_viewmodel_zrange( float *z_min, float *z_max )
{
	*z_min = 0.0f;
	*z_max = 0.3f * 65536.0f;
}

float nv2a_depth_w_scale( int enabled )
{
	return enabled ? NV2A_WBUFFER_SCALE : 1.0f;
}

float nv2a_viewmodel_w_scale( int enabled )
{
	return enabled ? NV2A_WBUFFER_SCALE / NV2A_VIEWMODEL_W_DIVISOR : 1.0f;
}

void nv2a_matrix_scale_all( float m[4][4], float s )
{
	int i, j;

	for( i = 0; i < 4; i++ )
	{
		for( j = 0; j < 4; j++ )
			m[i][j] *= s;
	}
}

int32_t nv2a_wbuffer_depth( float w, float scale )
{
	const float z = w * scale;

	if( !( z >= 0.0f ) || z > 16777215.0f )
		return -1;
	return (int32_t)z; /* floor: z is not negative here */
}
