/*
nv2a_sky_util.c - X9-X item 14 step 10, see nv2a_sky_util.h. A port of the
skybox arithmetic of ref/gl/gl_warp.c (root clone c7768c48), kept line for
line where the reference is plain C so a later divergence is findable.
*/
#include "nv2a_sky_util.h"
#include <math.h>
#include <string.h>

/* ON_EPSILON, common/mathlib.h - the reference's plane side test. */
#define NV2A_SKY_ON_EPSILON 0.1f

#define NV2A_SKY_SIDE_FRONT 0
#define NV2A_SKY_SIDE_BACK  1
#define NV2A_SKY_SIDE_ON    2

const int nv2a_sky_tex_order[NV2A_SKY_SIDES] = { 0, 2, 1, 3, 4, 5 }; /* gl_warp.c:23 */

/* gl_warp.c:25-33. */
static const float skyclip[NV2A_SKY_SIDES][3] =
{
{  1,  1,  0 },
{  1, -1,  0 },
{  0, -1,  1 },
{  0,  1,  1 },
{  1,  0,  1 },
{ -1,  0,  1 }
};

/* gl_warp.c:36-44. 1 = s, 2 = t, 3 = 2048 (the box half-size). */
static const int st_to_vec[NV2A_SKY_SIDES][3] =
{
{  3, -1,  2 },
{ -3,  1,  2 },
{  1,  3,  2 },
{ -1, -3,  2 },
{ -2, -1,  3 },  /* 0 degrees yaw, look straight up */
{  2, -1, -3 }   /* look straight down */
};

/* gl_warp.c:47-55. s = [0]/[2], t = [1]/[2]. */
static const int vec_to_st[NV2A_SKY_SIDES][3] =
{
{ -2,  3,  1 },
{  2,  3, -1 },
{  1,  3,  2 },
{ -1,  3, -2 },
{ -2, -1,  3 },
{ -2,  1, -3 }
};

void nv2a_sky_clear( nv2a_sky_bounds_t *bounds )
{
	int i;

	for( i = 0; i < NV2A_SKY_SIDES; i++ )
	{
		bounds->mins[0][i] = bounds->mins[1][i] = 9999999.0f;
		bounds->maxs[0][i] = bounds->maxs[1][i] = -9999999.0f;
	}
}

/* DrawSkyPolygon, gl_warp.c:78: pick the side the polygon's summed
   direction points at, and grow that side's bounds by every vertex's
   projection onto it. */
static void nv2a_sky_draw_polygon( nv2a_sky_bounds_t *bounds, int nump, const float *vecs )
{
	float v[3] = { 0.0f, 0.0f, 0.0f };
	float av[3];
	const float *vp;
	int i, axis;

	for( i = 0, vp = vecs; i < nump; i++, vp += 3 )
	{
		v[0] += vp[0];
		v[1] += vp[1];
		v[2] += vp[2];
	}

	av[0] = fabsf( v[0] );
	av[1] = fabsf( v[1] );
	av[2] = fabsf( v[2] );

	if( av[0] > av[1] && av[0] > av[2] )
		axis = ( v[0] < 0 ) ? 1 : 0;
	else if( av[1] > av[2] && av[1] > av[0] )
		axis = ( v[1] < 0 ) ? 3 : 2;
	else
		axis = ( v[2] < 0 ) ? 5 : 4;

	/* project new texture coords */
	for( i = 0; i < nump; i++, vecs += 3 )
	{
		int j = vec_to_st[axis][2];
		float dv = ( j > 0 ) ? vecs[j - 1] : -vecs[-j - 1];
		float s, t;

		if( dv == 0.0f )
			continue;

		j = vec_to_st[axis][0];
		s = ( j < 0 ) ? -vecs[-j - 1] / dv : vecs[j - 1] / dv;

		j = vec_to_st[axis][1];
		t = ( j < 0 ) ? -vecs[-j - 1] / dv : vecs[j - 1] / dv;

		if( s < bounds->mins[0][axis] ) bounds->mins[0][axis] = s;
		if( t < bounds->mins[1][axis] ) bounds->mins[1][axis] = t;
		if( s > bounds->maxs[0][axis] ) bounds->maxs[0][axis] = s;
		if( t > bounds->maxs[1][axis] ) bounds->maxs[1][axis] = t;
	}
}

/* The reference keeps these on the stack of each ClipSkyPolygon call
   (gl_warp.c:128-134): about 4 KiB a level, up to seven levels deep, so
   ~29 KiB of stack per sky face - more than this port wants to assume the
   Xbox render thread has spare. Each recursion level only ever writes the
   slot of its own stage, and a level's second half (newv[stage][1]) is
   read only after its first recursion returned - which touched stages
   above `stage` only - so one static set per stage is equivalent. 7 x
   (2 x 129 x 3 floats + 129 floats + 129 ints) = 29.3 KiB of BSS instead. */
static float nv2a_sky_newv[NV2A_SKY_SIDES + 1][2][NV2A_SKY_MAX_CLIP_VERTS + 1][3];
static float nv2a_sky_dists[NV2A_SKY_SIDES + 1][NV2A_SKY_MAX_CLIP_VERTS + 1];
static int   nv2a_sky_sides[NV2A_SKY_SIDES + 1][NV2A_SKY_MAX_CLIP_VERTS + 1];

/* ClipSkyPolygon, gl_warp.c:126, recursion and all. */
static void nv2a_sky_clip_stage( nv2a_sky_bounds_t *bounds, int nump, float *vecs, int stage )
{
	const float *norm;
	float *v, d, e;
	int front, back;
	float *dists;
	int *sides;
	int newc[2];
	int i, j;

loc1:
	if( stage == 6 )
	{
		/* fully clipped, so draw it */
		nv2a_sky_draw_polygon( bounds, nump, vecs );
		return;
	}

	dists = nv2a_sky_dists[stage];
	sides = nv2a_sky_sides[stage];

	front = back = 0;
	norm = skyclip[stage];
	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		d = v[0] * norm[0] + v[1] * norm[1] + v[2] * norm[2];
		if( d > NV2A_SKY_ON_EPSILON )
		{
			front = 1;
			sides[i] = NV2A_SKY_SIDE_FRONT;
		}
		else if( d < -NV2A_SKY_ON_EPSILON )
		{
			back = 1;
			sides[i] = NV2A_SKY_SIDE_BACK;
		}
		else
		{
			sides[i] = NV2A_SKY_SIDE_ON;
		}
		dists[i] = d;
	}

	if( !front || !back )
	{
		/* not clipped */
		stage++;
		goto loc1;
	}

	/* clip it */
	sides[i] = sides[0];
	dists[i] = dists[0];
	memcpy( vecs + i * 3, vecs, 3 * sizeof( float ));
	newc[0] = newc[1] = 0;

	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		switch( sides[i] )
		{
		case NV2A_SKY_SIDE_FRONT:
			memcpy( nv2a_sky_newv[stage][0][newc[0]], v, 3 * sizeof( float ));
			newc[0]++;
			break;
		case NV2A_SKY_SIDE_BACK:
			memcpy( nv2a_sky_newv[stage][1][newc[1]], v, 3 * sizeof( float ));
			newc[1]++;
			break;
		case NV2A_SKY_SIDE_ON:
			memcpy( nv2a_sky_newv[stage][0][newc[0]], v, 3 * sizeof( float ));
			newc[0]++;
			memcpy( nv2a_sky_newv[stage][1][newc[1]], v, 3 * sizeof( float ));
			newc[1]++;
			break;
		}

		if( sides[i] == NV2A_SKY_SIDE_ON || sides[i + 1] == NV2A_SKY_SIDE_ON || sides[i + 1] == sides[i] )
			continue;

		d = dists[i] / ( dists[i] - dists[i + 1] );
		for( j = 0; j < 3; j++ )
		{
			e = v[j] + d * ( v[j + 3] - v[j] );
			nv2a_sky_newv[stage][0][newc[0]][j] = e;
			nv2a_sky_newv[stage][1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	/* continue - newc can reach nump + 1 here, one below the buffers'
	   NV2A_SKY_MAX_CLIP_VERTS + 1, and a child writes one more past it
	   (the wrap copy above), which the reference's own buffers of the
	   same size also leave room for only up to its Host_Error bound. The
	   bound is re-checked per child so no write can pass the end. */
	if( newc[0] <= NV2A_SKY_MAX_CLIP_VERTS )
		nv2a_sky_clip_stage( bounds, newc[0], nv2a_sky_newv[stage][0][0], stage + 1 );
	if( newc[1] <= NV2A_SKY_MAX_CLIP_VERTS )
		nv2a_sky_clip_stage( bounds, newc[1], nv2a_sky_newv[stage][1][0], stage + 1 );
}

int nv2a_sky_clip_polygon( nv2a_sky_bounds_t *bounds, int nump, float *vecs )
{
	if( nump > NV2A_SKY_MAX_CLIP_VERTS )
		return 0;

	nv2a_sky_clip_stage( bounds, nump, vecs, 0 );
	return 1;
}

int nv2a_sky_side_empty( const nv2a_sky_bounds_t *bounds, int side )
{
	return bounds->mins[0][side] >= bounds->maxs[0][side] || bounds->mins[1][side] >= bounds->maxs[1][side];
}

void nv2a_sky_make_vec( float s, float t, int axis, int farclip, const float origin[3], float out_pos[3], float out_st[2] )
{
	float b[3];
	int j;

	b[0] = s * (float)( farclip >> 1 );
	b[1] = t * (float)( farclip >> 1 );
	b[2] = (float)( farclip >> 1 );

	for( j = 0; j < 3; j++ )
	{
		int k = st_to_vec[axis][j];
		out_pos[j] = ( k < 0 ) ? -b[-k - 1] : b[k - 1];
		out_pos[j] += origin[j];
	}

	/* avoid bilerp seam */
	s = ( s + 1.0f ) * 0.5f;
	t = ( t + 1.0f ) * 0.5f;

	/* GL_CLAMPTOEDGE_EXT branch (gl_warp.c:242-251) - see the header. */
	if( s < 0.0f ) s = 0.0f; else if( s > 1.0f ) s = 1.0f;
	if( t < 0.0f ) t = 0.0f; else if( t > 1.0f ) t = 1.0f;

	t = 1.0f - t;

	out_st[0] = s;
	out_st[1] = t;
}
