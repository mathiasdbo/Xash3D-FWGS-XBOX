/*
nv2a_efx_util.c - X9-X step 7: see nv2a_efx_util.h. Pure C99, no engine
or pbkit includes, host-tested (tools/host-tests/nv2a_efx_util_host_test.c).
*/
#include "nv2a_efx_util.h"
#include <limits.h>
#include <math.h>
#include <string.h>

/* gl_beams.c:43-57, unchanged but for the injected random source. */
void nv2a_efx_frac_noise( float *noise, int divs, nv2a_efx_random_fn random_float )
{
	int div2 = divs >> 1;
	if( divs < 2 ) return;

	// noise is normalized to +/- scale
	noise[div2] = ( noise[0] + noise[divs] ) * 0.5f + divs * random_float( -0.125f, 0.125f );

	if( div2 > 1 )
	{
		nv2a_efx_frac_noise( &noise[div2], div2, random_float );
		nv2a_efx_frac_noise( noise, div2, random_float );
	}
}

/* gl_beams.c:59-68. M_PI_F, as xash3d_mathlib.h:46 defines it. */
void nv2a_efx_sine_noise( float *noise, int divs )
{
	float freq = 0;
	float step = (float)3.14159265358979323846 / (float)divs;
	int i;

	for( i = 0; i < divs; i++ )
	{
		noise[i] = sin( freq );
		freq += step;
	}
}

/* A clipped polygon: a triangle or quad gains at most one vertex per
   clipping plane, and a piece is clipped by four (two per axis). */
#define WRAP_POLY_MAX 12

typedef struct
{
	nv2a_tri_vert_t v[WRAP_POLY_MAX];
	uint32_t        n;
} wrap_poly_t;

static float wrap_coord( const nv2a_tri_vert_t *v, int axis )
{
	return axis ? v->v : v->u;
}

static void wrap_lerp( nv2a_tri_vert_t *out, const nv2a_tri_vert_t *a, const nv2a_tri_vert_t *b, float t )
{
	int i;

	out->x = a->x + ( b->x - a->x ) * t;
	out->y = a->y + ( b->y - a->y ) * t;
	out->z = a->z + ( b->z - a->z ) * t;
	out->u = a->u + ( b->u - a->u ) * t;
	out->v = a->v + ( b->v - a->v ) * t;

	for( i = 0; i < 4; i++ )
	{
		float c = (float)a->rgba[i] + ((float)b->rgba[i] - (float)a->rgba[i] ) * t;
		out->rgba[i] = (uint8_t)( c + 0.5f );
	}
}

/* Sutherland-Hodgman against one axis-aligned plane: keeps the side
   where sign * ( coord - bound ) >= 0. An intersection vertex is placed
   exactly on the bound, so the pieces of neighbouring cells share their
   edge bit for bit. */
static void wrap_clip( wrap_poly_t *out, const wrap_poly_t *in, int axis, float bound, float sign )
{
	uint32_t i;

	out->n = 0;

	for( i = 0; i < in->n; i++ )
	{
		const nv2a_tri_vert_t *a = &in->v[i];
		const nv2a_tri_vert_t *b = &in->v[( i + 1 ) % in->n];
		float da = sign * ( wrap_coord( a, axis ) - bound );
		float db = sign * ( wrap_coord( b, axis ) - bound );

		if( da >= 0.0f && out->n < WRAP_POLY_MAX )
			out->v[out->n++] = *a;

		if(( da >= 0.0f ) != ( db >= 0.0f ) && out->n < WRAP_POLY_MAX )
		{
			nv2a_tri_vert_t *m = &out->v[out->n++];

			wrap_lerp( m, a, b, da / ( da - db ));
			if( axis ) m->v = bound;
			else m->u = bound;
		}
	}
}

/* (int)floorf( q ) and (int)ceilf( q ) without the libm calls
   (x9-perf-efx, docs/r3d/divergences.md #159). nxdk's floorf and ceilf
   (pdclib, platform/xbox/functions/math) switch the x87 rounding mode
   with two fldcw around an frndint, and wrap_cell_range ran four of them
   per beam primitive: the largest single cost of a beam in xemu. The
   truncating conversion compiles to cvttss2si for the Xbox's SSE floats.
   The result equals the libm form for every q whose floor or ceiling
   fits an int, which the casts already required: below 2^24 (float)i is
   exact, and from 2^24 up every float is an integer, so i == q. The
   parameter is a float, so q is rounded to float before the compare, as
   the libm call's argument was. */
static int wrap_floor( float q )
{
	int i = (int)q;

	if( (float)i > q && i != INT_MIN ) /* out of range: cvttss2si's INT_MIN, not i - 1 */
		i--;
	return i;
}

static int wrap_ceil( float q )
{
	int i = (int)q;

	if( (float)i < q )
		i++;
	return i;
}

/* The cells [first, last] a coordinate range touches. A range ending
   exactly on a boundary does not reach into the next cell: [0, 1] is one
   cell, as GL_REPEAT samples a texture coordinate of exactly 1.0 at the
   texture's last texel edge, not the next copy's first. */
static void wrap_cell_range( float lo, float hi, float cell, int *first, int *last )
{
	*first = wrap_floor( lo / cell );
	*last = wrap_ceil( hi / cell ) - 1;
	if( *last < *first )
		*last = *first;
}

static void wrap_shift( nv2a_tri_vert_t *v, uint32_t n, float du, float dv )
{
	uint32_t i;

	for( i = 0; i < n; i++ )
	{
		v[i].u -= du;
		v[i].v -= dv;
	}
}

void nv2a_efx_wrap_primitive( const nv2a_tri_vert_t *v, uint32_t n, float cell_u, float cell_v,
	nv2a_efx_wrap_emit_fn emit, void *ctx )
{
	nv2a_tri_vert_t shifted[4];
	float umin, umax, vmin, vmax;
	int u0, u1, v0, v1, cu, cv;
	uint32_t i;

	if( n == 0 || n > 4 )
		return;

	if( !( cell_u > 0.0f ) || !( cell_v > 0.0f ))
	{
		emit( v, n, 0, ctx );
		return;
	}

	umin = umax = v[0].u;
	vmin = vmax = v[0].v;
	for( i = 1; i < n; i++ )
	{
		if( v[i].u < umin ) umin = v[i].u;
		if( v[i].u > umax ) umax = v[i].u;
		if( v[i].v < vmin ) vmin = v[i].v;
		if( v[i].v > vmax ) vmax = v[i].v;
	}

	wrap_cell_range( umin, umax, cell_u, &u0, &u1 );
	wrap_cell_range( vmin, vmax, cell_v, &v0, &v1 );

	/* One cell (the common case: a beam segment spans a fraction of its
	   texture), a point or line, or a span past the cap: shift whole. */
	if(( u0 == u1 && v0 == v1 ) || n < 3
		|| u1 - u0 >= NV2A_EFX_WRAP_MAX_CELLS || v1 - v0 >= NV2A_EFX_WRAP_MAX_CELLS )
	{
		memcpy( shifted, v, n * sizeof( *v ));
		wrap_shift( shifted, n, (float)u0 * cell_u, (float)v0 * cell_v );
		emit( shifted, n, 0, ctx );
		return;
	}

	for( cv = v0; cv <= v1; cv++ )
	{
		wrap_poly_t in, a, b;

		memcpy( in.v, v, n * sizeof( *v ));
		in.n = n;

		wrap_clip( &a, &in, 1, (float)cv * cell_v, 1.0f );
		wrap_clip( &b, &a, 1, (float)( cv + 1 ) * cell_v, -1.0f );
		if( b.n < 3 )
			continue;

		for( cu = u0; cu <= u1; cu++ )
		{
			wrap_poly_t c, d;
			uint32_t k;

			wrap_clip( &c, &b, 0, (float)cu * cell_u, 1.0f );
			wrap_clip( &d, &c, 0, (float)( cu + 1 ) * cell_u, -1.0f );
			if( d.n < 3 )
				continue;

			wrap_shift( d.v, d.n, (float)cu * cell_u, (float)cv * cell_v );

			/* A clipped convex polygon keeps the input's winding; fan it
			   from its first vertex. */
			for( k = 1; k + 1 < d.n; k++ )
			{
				nv2a_tri_vert_t tri[3];

				tri[0] = d.v[0];
				tri[1] = d.v[k];
				tri[2] = d.v[k + 1];
				emit( tri, 3, 1, ctx );
			}
		}
	}
}
