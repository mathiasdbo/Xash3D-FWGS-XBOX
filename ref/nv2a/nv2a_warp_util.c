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

/* EmitWaterPolys, gl_rsurf.c:961-967. */
float nv2a_warp_wave_z( const float v[3], double time, float waveHeight )
{
	float nv;

	if( !waveHeight )
		return v[2];

	nv = nv2a_turbsin[(int)( time * 160.0f + v[1] + v[0] ) & 255] + 8.0f;
	nv = ( nv2a_turbsin[(int)( v[0] * 5.0f + time * 171.0f - v[1] ) & 255] + 8.0f ) * 0.8f + nv;
	return nv * waveHeight + v[2];
}

/* EmitWaterPolys, gl_rsurf.c:969-984. */
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

/* Mod_LooksLikeWaterTexture, mod_bmodel.c:2579 (REF_DEFAULT_TEXTURE is
   "*default", ref_api.h:216), outside Quake compatibility. */
static int nv2a_warp_lower( int c )
{
	return ( c >= 'A' && c <= 'Z' ) ? c - 'A' + 'a' : c;
}

int nv2a_warp_looks_like_water( const char *name )
{
	static const char laser[] = "laser";
	int i;

	if(( name[0] == '*' && strcmp( name, "*default" )) || name[0] == '!' )
		return 1;

	if( !strncmp( name, "water", 5 ))
		return 1;

	for( i = 0; i < 5; i++ ) /* Q_strnicmp( name, "laser", 5 ) */
	{
		if( nv2a_warp_lower( (unsigned char)name[i] ) != laser[i] )
			return 0;
	}
	return 1;
}

uint32_t nv2a_warp_gpu_bytes( uint32_t verts )
{
	if( verts > NV2A_WARP_GPU_MAX_VERTS )
		verts = NV2A_WARP_GPU_MAX_VERTS;
	return verts * NV2A_WARP_GPU_VERTEX_BYTES;
}

void nv2a_warp_gpu_table( float out[NV2A_WARP_GPU_TABLE_REGS][4] )
{
	int k;

	for( k = 0; k < (int)NV2A_WARP_GPU_TABLE_REGS; k++ )
	{
		out[k][0] = nv2a_turbsin[k];
		out[k][1] = out[k][2] = out[k][3] = 0.0f;
	}
}

static void nv2a_warp_set4( float r[4], float x, float y, float z, float w )
{
	r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

void nv2a_warp_gpu_params( const nv2a_warp_frame_t *frame, float waveHeight, const uint8_t rgba[4],
	float out[NV2A_WARP_GPU_PARAM_REGS][4] )
{
	/* The same scale nv2a_warp_texcoord_frame multiplies by. */
	const float scale = (float)( 0.125 * NV2A_WARP_TURBSCALE );

	nv2a_warp_set4( out[0], frame->frac160, frame->frac171, frame->fracts, scale );
	nv2a_warp_set4( out[1], 1.0f, 5.0f, 1.0f, -1.0f );
	/* -base: exact while the base is under 2^24 (time * 171 for 27 hours);
	   past that the comparison it feeds only matters for a vertex as far
	   below zero, which no map reaches. */
	nv2a_warp_set4( out[2], -(float)frame->base160, -(float)frame->base171, -(float)frame->basets, -(float)frame->basets );
	/* base & 255, the low byte the reference's ( int ) & 255 keeps; over
	   256 it is exact. */
	nv2a_warp_set4( out[3], (float)( frame->base160 & 255 ) / 256.0f, (float)( frame->base171 & 255 ) / 256.0f,
		(float)( frame->basets & 255 ) / 256.0f, (float)( frame->basets & 255 ) / 256.0f );
	nv2a_warp_set4( out[4], 1.0f / 256.0f, 256.0f, 128.0f, -128.0f );
	nv2a_warp_set4( out[5], 8.0f, 0.8f, waveHeight, 1.0f / NV2A_WARP_SUBDIVIDE_SIZE );
	/* The table address bias: A0 = k + bias reads hardware constant
	   96 + A0 = NV2A_WARP_GPU_TABLE_HW + k; the 0.25 keeps an ARL that
	   rounded instead of flooring on the same register. */
	nv2a_warp_set4( out[6], -2.0f, 1.0f, (float)NV2A_WARP_GPU_TABLE_HW - 96.0f + 0.25f, 0.0f );
	nv2a_warp_set4( out[7], rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f );
}

/* EXP's .y (NV_vertex_program: s - floor( s )). */
static float nv2a_vp_frac( float x )
{
	return x - floorf( x );
}

void nv2a_warp_gpu_emulate( const float params[NV2A_WARP_GPU_PARAM_REGS][4],
	const float table[NV2A_WARP_GPU_TABLE_REGS][4], const nv2a_warp_gpu_vertex_t *v,
	float *out_z, float out_st[2] )
{
	const float *c5 = params[0], *c6 = params[1], *c7 = params[2], *c8 = params[3];
	const float *c9 = params[4], *c10 = params[5], *c11 = params[6];
	float r1[4], r2[4], r4[4], r5, r6, r7[4], r8[4], r9, r10[4], r11[2];
	int i;

	/* MUL R2.xy, v[0].yxzw, c[6]; MUL R2.zw, v[8].xxyx, c[5].w */
	r2[0] = v->xyz[1] * c6[0];
	r2[1] = v->xyz[0] * c6[1];
	r2[2] = v->ot * c5[3];
	r2[3] = v->os * c5[3];
	/* ADD R1.xy, R2, c[5]; ADD R1.zw, R2, c[5].z */
	r1[0] = r2[0] + c5[0];
	r1[1] = r2[1] + c5[1];
	r1[2] = r2[2] + c5[2];
	r1[3] = r2[3] + c5[2];
	/* MAD R1.xy, v[0], c[6].zwzw, R1 (products by 1 and -1: exact) */
	r1[0] = v->xyz[0] * c6[2] + r1[0];
	r1[1] = v->xyz[1] * c6[3] + r1[1];

	for( i = 0; i < 4; i++ )
	{
		/* EXP R3.y, R1.i; ADD R4.i, R1.i, -R3.y */
		r4[i] = r1[i] - nv2a_vp_frac( r1[i] );
		/* SLT R5, R4, R1; SLT R6, R4, c[7]; MAD R4, R5, R6, R4 */
		r5 = r4[i] < r1[i] ? 1.0f : 0.0f;
		r6 = r4[i] < c7[i] ? 1.0f : 0.0f;
		r4[i] = r5 * r6 + r4[i];
		/* MUL R4, R4, c[9].x; ADD R4, R4, c[8] */
		r4[i] = r4[i] * c9[0];
		r4[i] = r4[i] + c8[i];
		/* EXP R3.y, R4.i; MUL R7.i, R3.y, c[9].y */
		r7[i] = nv2a_vp_frac( r4[i] ) * c9[1];
		/* SGE R8, R7, c[9].z; MAD R7, R8, c[9].w, R7 */
		r8[i] = r7[i] >= c9[2] ? 1.0f : 0.0f;
		r7[i] = r8[i] * c9[3] + r7[i];
		/* ADD R9, c[9].z, -R7; MIN R7, R7, R9 */
		r9 = c9[2] - r7[i];
		r7[i] = r7[i] < r9 ? r7[i] : r9;
		/* MAD R8, R8, c[11].x, c[11].y; ADD R7, R7, c[11].z */
		r8[i] = r8[i] * c11[0] + c11[1];
		r7[i] = r7[i] + c11[2];
		/* ARL A0.x, R7.i; MOV R10.i, c[A0.x + 0].x - hardware constant
		   96 + A0, table register 96 + A0 - NV2A_WARP_GPU_TABLE_HW */
		r10[i] = table[(int)floorf( r7[i] ) + 96 - (int)NV2A_WARP_GPU_TABLE_HW][0];
		/* MUL R10, R10, R8 */
		r10[i] = r10[i] * r8[i];
	}

	/* ADD R11.xy, R10, c[10].x; MUL R11.y, R11.y, c[10].y; ADD R11.x, R11.y, R11.x */
	r11[0] = r10[0] + c10[0];
	r11[1] = r10[1] + c10[0];
	r11[1] = r11[1] * c10[1];
	r11[0] = r11[1] + r11[0];
	/* MUL R11.x, R11.x, c[10].z; ADD R0.z, R11.x, v[0].z */
	r11[0] = r11[0] * c10[2];
	*out_z = r11[0] + v->xyz[2];

	/* ADD R10.zw, R10, v[8].xxxy; MUL o[TEX0].xy, R10.zwzw, c[10].w */
	out_st[0] = ( r10[2] + v->os ) * c10[3];
	out_st[1] = ( r10[3] + v->ot ) * c10[3];
}
