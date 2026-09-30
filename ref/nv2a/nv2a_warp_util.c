/*
nv2a_warp_util.c - see nv2a_warp_util.h. Ports of ref/gl/gl_rsurf.c
(root clone c7768c48) cited per function; the fog parameter formulas are
cited in the header.
*/
#include "nv2a_warp_util.h"
#include <math.h>
#include <string.h>

/* r_turbsin, gl_rsurf.c:24 - the engine's own table (engine/warpsin.h),
   included the same way. */
static const float nv2a_turbsin[] =
{
#include "warpsin.h"
};

/* TURBSCALE, gl_rsurf.c:21 ( 256.0f / ( M_PI2 )). M_PI2 is a double
   (xash3d_mathlib.h:45), so the quotient is a double, as here. */
#define NV2A_WARP_TURBSCALE ( 256.0f / ( 3.14159265358979323846 * 2.0 ))

float nv2a_warp_turbsin( int i )
{
	return nv2a_turbsin[i & 255];
}

/* BoundPoly, gl_rsurf.c:188. */
static void nv2a_warp_bound_poly( int numverts, const float *verts, float mins[3], float maxs[3] )
{
	int i, j;

	mins[0] = mins[1] = mins[2] = 999999.0f;  /* ClearBounds, xash3d_mathlib.h:403 */
	maxs[0] = maxs[1] = maxs[2] = -999999.0f;

	for( i = 0; i < numverts; i++ )
	{
		for( j = 0; j < 3; j++, verts++ )
		{
			if( *verts < mins[j] ) mins[j] = *verts;
			if( *verts > maxs[j] ) maxs[j] = *verts;
		}
	}
}

/* SubdividePolygon_r, gl_rsurf.c:204, line for line. The leaf (the
   glpoly2_t allocation, gl_rsurf.c:269-284) is the caller's `emit`. */
int nv2a_warp_subdivide( int numverts, float *verts, nv2a_warp_poly_fn emit, void *ctx )
{
	float front[NV2A_WARP_SUBDIVIDE_SIZE][3], back[NV2A_WARP_SUBDIVIDE_SIZE][3];
	float dist[NV2A_WARP_SUBDIVIDE_SIZE];
	float mins[3], maxs[3];
	float *v;
	int i, j;

	if( numverts > NV2A_WARP_MAX_VERTS )
		return 0; /* Host_Error in the reference (gl_rsurf.c:212) */

	nv2a_warp_bound_poly( numverts, verts, mins, maxs );

	for( i = 0; i < 3; i++ )
	{
		float m = ( mins[i] + maxs[i] ) * 0.5f;
		int f = 0, b = 0;

		m = NV2A_WARP_SUBDIVIDE_SIZE * floor( m / NV2A_WARP_SUBDIVIDE_SIZE + 0.5f );
		if( maxs[i] - m < 8 ) continue;
		if( m - mins[i] < 8 ) continue;

		// cut it
		v = verts + i;
		for( j = 0; j < numverts; j++, v += 3 )
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v -= i;
		memcpy( v, verts, 3 * sizeof( float ));

		v = verts;
		for( j = 0; j < numverts; j++, v += 3 )
		{
			if( dist[j] >= 0 )
			{
				memcpy( front[f], v, 3 * sizeof( float ));
				f++;
			}

			if( dist[j] <= 0 )
			{
				memcpy( back[b], v, 3 * sizeof( float ));
				b++;
			}

			if( dist[j] == 0 || dist[j + 1] == 0 )
				continue;

			if(( dist[j] > 0 ) != ( dist[j + 1] > 0 ))
			{
				// clip point
				float frac = dist[j] / ( dist[j] - dist[j + 1] );
				int k;

				for( k = 0; k < 3; k++ )
					front[f][k] = back[b][k] = v[k] + frac * ( v[3 + k] - v[k] );
				f++;
				b++;
			}
		}

		if( !nv2a_warp_subdivide( f, front[0], emit, ctx ))
			return 0;
		return nv2a_warp_subdivide( b, back[0], emit, ctx );
	}

	emit( ctx, numverts, verts );
	return 1;
}

/* EmitWaterPolys, gl_rsurf.c:959-964. */
float nv2a_warp_wave_z( const float v[3], double time, float waveHeight )
{
	float nv;

	if( !waveHeight )
		return v[2];

	nv = nv2a_turbsin[(int)( time * 160.0f + v[1] + v[0] ) & 255] + 8.0f;
	nv = ( nv2a_turbsin[(int)( v[0] * 5.0f + time * 171.0f - v[1] ) & 255] + 8.0f ) * 0.8f + nv;
	return nv * waveHeight + v[2];
}

/* EmitWaterPolys, gl_rsurf.c:966-984. */
void nv2a_warp_texcoord( float os, float ot, double time, int ripples, float out_st[2] )
{
	float s, t;

	if( !ripples )
	{
		s = os + nv2a_turbsin[(int)(( ot * 0.125f + time ) * NV2A_WARP_TURBSCALE ) & 255];
		t = ot + nv2a_turbsin[(int)(( os * 0.125f + time ) * NV2A_WARP_TURBSCALE ) & 255];
	}
	else
	{
		s = os;
		t = ot;
	}

	out_st[0] = s * ( 1.0f / NV2A_WARP_SUBDIVIDE_SIZE );
	out_st[1] = t * ( 1.0f / NV2A_WARP_SUBDIVIDE_SIZE );
}

static void nv2a_warp_split( double x, int *base, float *frac )
{
	double b = floor( x );

	*base = (int)b;
	*frac = (float)( x - b );
}

void nv2a_warp_frame_init( nv2a_warp_frame_t *frame, double time )
{
	nv2a_warp_split( time * 160.0f, &frame->base160, &frame->frac160 );
	nv2a_warp_split( time * 171.0f, &frame->base171, &frame->frac171 );
	nv2a_warp_split( time * NV2A_WARP_TURBSCALE, &frame->basets, &frame->fracts );
}

/* trunc( base + a ) = floor( base + a ) = base + floor( a ) when the sum is
   not negative; a negative sum with a fraction truncates to one more. */
int nv2a_warp_trunc_sum( int base, float a )
{
	int fl = (int)a; /* toward zero */
	int n;

	if( a < (float)fl )
		fl--; /* floor */
	n = base + fl;
	if( n < 0 && (float)fl != a )
		n++;
	return n;
}

float nv2a_warp_wave_z_frame( const nv2a_warp_frame_t *frame, const float v[3], float waveHeight )
{
	float nv;

	if( !waveHeight )
		return v[2];

	nv = nv2a_turbsin[nv2a_warp_trunc_sum( frame->base160, frame->frac160 + v[1] + v[0] ) & 255] + 8.0f;
	nv = ( nv2a_turbsin[nv2a_warp_trunc_sum( frame->base171, v[0] * 5.0f + frame->frac171 - v[1] ) & 255] + 8.0f ) * 0.8f + nv;
	return nv * waveHeight + v[2];
}

void nv2a_warp_texcoord_frame( const nv2a_warp_frame_t *frame, float os, float ot, float out_st[2] )
{
	/* ( o * 0.125f + time ) * TURBSCALE = o * ( 0.125 * TURBSCALE ) + time * TURBSCALE */
	const float scale = (float)( 0.125 * NV2A_WARP_TURBSCALE );
	float s = os + nv2a_turbsin[nv2a_warp_trunc_sum( frame->basets, ot * scale + frame->fracts ) & 255];
	float t = ot + nv2a_turbsin[nv2a_warp_trunc_sum( frame->basets, os * scale + frame->fracts ) & 255];

	out_st[0] = s * ( 1.0f / NV2A_WARP_SUBDIVIDE_SIZE );
	out_st[1] = t * ( 1.0f / NV2A_WARP_SUBDIVIDE_SIZE );
}

void nv2a_fog_exp_params( float density, float out_params[2] )
{
	out_params[0] = 1.5f;
	out_params[1] = -density / ( 2.0f * logf( 256.0f ));
}

void nv2a_fog_exp2_params( float density, float out_params[2] )
{
	out_params[0] = 1.5f;
	out_params[1] = -density / ( 2.0f * sqrtf( logf( 256.0f )));
}

void nv2a_fog_linear_params( float start, float end, float out_params[2] )
{
	out_params[1] = -1.0f / ( end - start );
	out_params[0] = 1.0f - end * out_params[1];
}

static uint32_t nv2a_fog_channel( float c )
{
	if( !( c > 0.0f ))
		return 0;
	if( c >= 1.0f )
		return 255;
	return (uint32_t)( c * 255.0f + 0.5f );
}

uint32_t nv2a_fog_pack_color( const float rgb[3] )
{
	return nv2a_fog_channel( rgb[0] ) | ( nv2a_fog_channel( rgb[1] ) << 8 ) | ( nv2a_fog_channel( rgb[2] ) << 16 );
}
