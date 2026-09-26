/*
nv2a_2d_util.c - see nv2a_2d_util.h for the contract of each function.
*/
#include "nv2a_2d_util.h"
#include <math.h>
#include <string.h>

void nv2a_2d_chunk_quads( uint32_t vertex_count, nv2a_2d_chunk_emit_fn emit, void *ctx )
{
	uint32_t start = 0;

	if( !emit )
		return;

	while( start < vertex_count )
	{
		uint32_t remaining = vertex_count - start;
		uint32_t count = remaining > NV2A_2D_MAX_DRAW_VERTS ? NV2A_2D_MAX_DRAW_VERTS : remaining;

		/* NV2A_2D_MAX_DRAW_VERTS (256) is itself a multiple of 4, so only
		   a REMAINDER chunk can end up not being one - round it down; a
		   vertex_count that wasn't a whole number of quads to begin with
		   is a bug upstream of this function (a partial quad was
		   appended), not something to paper over by emitting one. */
		count -= count % 4;
		if( count == 0 )
			break;

		emit( start, count, ctx );
		start += count;
	}
}

/* Internal cap on how many boundary crossings one axis scan will ever
   walk, independent of the caller's own max_tiles - just large enough
   that a legitimate few-hundred-cell span (still rejected below as
   "too many to draw untiled") never reads past this fixed buffer. */
#define NV2A_2D_TILE_AXIS_MAX 300

static int nv2a_2d_axis_bounds( float lo, float hi, float bounds[NV2A_2D_TILE_AXIS_MAX] )
{
	int n = 0;
	float cur;

	bounds[n++] = lo;

	if( hi > lo )
	{
		cur = floorf( lo ) + 1.0f;
		while( cur < hi && n < NV2A_2D_TILE_AXIS_MAX - 1 )
		{
			bounds[n++] = cur;
			cur += 1.0f;
		}
	}

	bounds[n++] = hi;
	return n - 1; /* cell count */
}

uint32_t nv2a_2d_tile_split( float x, float y, float w, float h,
	float s1, float t1, float s2, float t2,
	nv2a_2d_tile_t *out, uint32_t max_tiles, int *clamped )
{
	float sb[NV2A_2D_TILE_AXIS_MAX], tb[NV2A_2D_TILE_AXIS_MAX];
	int sCells, tCells, i, j, count;
	float sSpan, tSpan;

	if( clamped )
		*clamped = 0;

	if( !out || max_tiles == 0 )
		return 0;

	sCells = nv2a_2d_axis_bounds( s1, s2, sb );
	tCells = nv2a_2d_axis_bounds( t1, t2, tb );

	if( sCells < 1 ) sCells = 1;
	if( tCells < 1 ) tCells = 1;

	if( (uint32_t)sCells * (uint32_t)tCells > max_tiles
		|| sCells >= NV2A_2D_TILE_AXIS_MAX - 1 || tCells >= NV2A_2D_TILE_AXIS_MAX - 1 )
	{
		/* Too many cells to enumerate - draw clamped: one quad, the
		   whole rect, texture coordinates bound into [0,1] rather than
		   tiled. The caller logs the DPrintf-once; this only signals it. */
		if( clamped )
			*clamped = 1;

		out[0].x = x; out[0].y = y; out[0].w = w; out[0].h = h;
		out[0].s1 = s1 < 0.0f ? 0.0f : ( s1 > 1.0f ? 1.0f : s1 );
		out[0].t1 = t1 < 0.0f ? 0.0f : ( t1 > 1.0f ? 1.0f : t1 );
		out[0].s2 = s2 < 0.0f ? 0.0f : ( s2 > 1.0f ? 1.0f : s2 );
		out[0].t2 = t2 < 0.0f ? 0.0f : ( t2 > 1.0f ? 1.0f : t2 );
		return 1;
	}

	sSpan = s2 - s1;
	tSpan = t2 - t1;
	count = 0;

	for( j = 0; j < tCells; j++ )
	{
		float tLo = tb[j], tHi = tb[j + 1];
		float fracY0 = tSpan != 0.0f ? ( tLo - t1 ) / tSpan : 0.0f;
		float fracY1 = tSpan != 0.0f ? ( tHi - t1 ) / tSpan : 1.0f;
		float cellY = y + fracY0 * h;
		float cellH = ( fracY1 - fracY0 ) * h;
		/* Local (remapped-into-[0,1]) t for this cell - floorf(tLo) is
		   the cell's own integer origin, exact even when tLo already IS
		   that integer (a cell boundary), which is the intended case. */
		float localT1 = tLo - floorf( tLo );
		float localT2 = localT1 + ( tHi - tLo );

		for( i = 0; i < sCells; i++ )
		{
			float sLo = sb[i], sHi = sb[i + 1];
			float fracX0 = sSpan != 0.0f ? ( sLo - s1 ) / sSpan : 0.0f;
			float fracX1 = sSpan != 0.0f ? ( sHi - s1 ) / sSpan : 1.0f;
			float localS1 = sLo - floorf( sLo );
			float localS2 = localS1 + ( sHi - sLo );
			nv2a_2d_tile_t *o = &out[count++];

			o->x = x + fracX0 * w;
			o->w = ( fracX1 - fracX0 ) * w;
			o->y = cellY;
			o->h = cellH;
			o->s1 = localS1;
			o->s2 = localS2;
			o->t1 = localT1;
			o->t2 = localT2;
		}
	}

	return (uint32_t)count;
}

void nv2a_2d_build_matrix( float out[4][4], float nv2a_width, float nv2a_height,
	float engine_width, float engine_height, float pixel_offset )
{
	memset( out, 0, 4 * 4 * sizeof( float ));

	if( engine_width == 0.0f || engine_height == 0.0f )
		return; /* skipping the draw itself is the caller's job; just avoid a division by zero here regardless */

	out[0][0] = nv2a_width / engine_width;
	out[1][1] = nv2a_height / engine_height; /* no y-flip: the engine's own 2D space is already y-down */
	out[3][0] = pixel_offset;
	out[3][1] = pixel_offset;
	out[3][2] = 32768.0f;
	out[3][3] = 1.0f;
}
