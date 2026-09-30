/*
nv2a_decal_util.c - X9-X item 14 step 12: see nv2a_decal_util.h. Pure C,
no engine or pbkit includes; host-tested by
tools/host-tests/nv2a_decal_util_host_test.c.
*/
#include "nv2a_decal_util.h"
#include <string.h>

int nv2a_decal_clip_inside( const float *vert, int edge )
{
	switch( edge )
	{
	case NV2A_DECAL_LEFT_EDGE:
		if( vert[3] > 0.0f )
			return 1;
		return 0;
	case NV2A_DECAL_RIGHT_EDGE:
		if( vert[3] < 1.0f )
			return 1;
		return 0;
	case NV2A_DECAL_TOP_EDGE:
		if( vert[4] > 0.0f )
			return 1;
		return 0;
	case NV2A_DECAL_BOTTOM_EDGE:
		if( vert[4] < 1.0f )
			return 1;
		return 0;
	}
	return 0;
}

void nv2a_decal_clip_intersect( const float *one, const float *two, float *out, int edge )
{
	// t is the parameter of the line between one and two clipped to the edge
	// or the fraction of the clipped point between one & two
	// vert[0], vert[1], vert[2] is X, Y, Z
	// vert[3] is u
	// vert[4] is v
	// vert[5] is lightmap u
	// vert[6] is lightmap v
	float t;

	if( edge < NV2A_DECAL_TOP_EDGE )
	{
		if( edge == NV2A_DECAL_LEFT_EDGE )
		{
			// left
			t = (( one[3] - 0.0f ) / ( one[3] - two[3] ));
			out[3] = out[5] = 0.0f;
		}
		else
		{
			// right
			t = (( one[3] - 1.0f ) / ( one[3] - two[3] ));
			out[3] = out[5] = 1.0f;
		}

		out[4] = one[4] + ( two[4] - one[4] ) * t;
		out[6] = one[6] + ( two[6] - one[6] ) * t;
	}
	else
	{
		if( edge == NV2A_DECAL_TOP_EDGE )
		{
			// top
			t = (( one[4] - 0.0f ) / ( one[4] - two[4] ));
			out[4] = out[6] = 0.0f;
		}
		else
		{
			// bottom
			t = (( one[4] - 1.0f ) / ( one[4] - two[4] ));
			out[4] = out[6] = 1.0f;
		}

		out[3] = one[3] + ( two[3] - one[3] ) * t;
		out[5] = one[5] + ( two[4] - one[5] ) * t; /* sic, gl_decals.c:297 */
	}

	/* VectorLerp( one, t, two, out ), xash3d_mathlib.h */
	out[0] = one[0] + t * ( two[0] - one[0] );
	out[1] = one[1] + t * ( two[1] - one[1] );
	out[2] = one[2] + t * ( two[2] - one[2] );
}

int nv2a_decal_sh_clip( const float *vert, int vertCount, float *out, int edge )
{
	int outCount = 0;
	const float *s;
	int j;

	if( vertCount <= 0 )
		return 0; /* the reference forms &vert[-VERTEXSIZE] and never reads it: same result */

	s = &vert[( vertCount - 1 ) * NV2A_DECAL_VERTEXSIZE];

	for( j = 0; j < vertCount; j++ )
	{
		const float *p = &vert[j * NV2A_DECAL_VERTEXSIZE];

		if( nv2a_decal_clip_inside( p, edge ))
		{
			if( nv2a_decal_clip_inside( s, edge ))
			{
				// Add a vertex and advance out to next vertex
				memcpy( out, p, sizeof( float ) * NV2A_DECAL_VERTEXSIZE );
				out += NV2A_DECAL_VERTEXSIZE;
				outCount++;
			}
			else
			{
				nv2a_decal_clip_intersect( s, p, out, edge );
				out += NV2A_DECAL_VERTEXSIZE;
				outCount++;

				memcpy( out, p, sizeof( float ) * NV2A_DECAL_VERTEXSIZE );
				out += NV2A_DECAL_VERTEXSIZE;
				outCount++;
			}
		}
		else
		{
			if( nv2a_decal_clip_inside( s, edge ))
			{
				nv2a_decal_clip_intersect( p, s, out, edge );
				out += NV2A_DECAL_VERTEXSIZE;
				outCount++;
			}
		}

		s = p;
	}

	return outCount;
}

int nv2a_decal_clip_polygon( float verts[][NV2A_DECAL_VERTEXSIZE], float scratch[][NV2A_DECAL_VERTEXSIZE], int count )
{
	int outCount;

	if( count < 3 || count > NV2A_DECAL_MAX_INPUT_VERTS )
		return 0;

	/* R_DoDecalSHClip, gl_decals.c:354-357: in -> scratch -> in -> scratch -> in. */
	outCount = nv2a_decal_sh_clip( verts[0], count, scratch[0], NV2A_DECAL_LEFT_EDGE );
	outCount = nv2a_decal_sh_clip( scratch[0], outCount, verts[0], NV2A_DECAL_RIGHT_EDGE );
	outCount = nv2a_decal_sh_clip( verts[0], outCount, scratch[0], NV2A_DECAL_TOP_EDGE );
	outCount = nv2a_decal_sh_clip( scratch[0], outCount, verts[0], NV2A_DECAL_BOTTOM_EDGE );

	return outCount;
}

void nv2a_decal_lightmap_luxel( const float v[3], const float lmvecs[2][4], const short lightmapmins[2], float sample_size, float out[2] )
{
	int k;

	for( k = 0; k < 2; k++ )
	{
		float s = v[0] * lmvecs[k][0] + v[1] * lmvecs[k][1] + v[2] * lmvecs[k][2] + lmvecs[k][3] - lightmapmins[k];

		out[k] = s / sample_size + 0.5f;
	}
}
