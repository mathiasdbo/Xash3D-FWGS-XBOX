/*
nv2a_tri_util.c - see nv2a_tri_util.h for the contract of each function.
*/
#include "nv2a_tri_util.h"
#include <string.h>

void nv2a_tri_begin( nv2a_tri_asm_t *a, nv2a_tri_topology_t topology, nv2a_tri_emit_fn emit, void *ctx )
{
	if( !a )
		return;

	memset( a, 0, sizeof( *a ));
	a->topology = topology;
	a->emit     = emit;
	a->ctx      = ctx;
}

void nv2a_tri_push( nv2a_tri_asm_t *a, const nv2a_tri_vert_t *v )
{
	nv2a_tri_vert_t out[4];

	if( !a || !v || !a->emit )
		return;

	switch( a->topology )
	{
	case NV2A_TRI_LIST_POINTS:
		a->emit( v, 1, a->ctx );
		break;

	case NV2A_TRI_LIST_LINES:
		if(( a->count & 1u ) == 0 )
		{
			a->buf[0] = *v;
		}
		else
		{
			out[0] = a->buf[0];
			out[1] = *v;
			a->emit( out, 2, a->ctx );
		}
		a->count++;
		break;

	case NV2A_TRI_LIST_TRIANGLES:
		a->buf[a->count % 3] = *v;
		if( a->count % 3 == 2 )
		{
			out[0] = a->buf[0]; out[1] = a->buf[1]; out[2] = a->buf[2];
			a->emit( out, 3, a->ctx );
		}
		a->count++;
		break;

	case NV2A_TRI_LIST_QUADS:
		a->buf[a->count % 4] = *v;
		if( a->count % 4 == 3 )
		{
			out[0] = a->buf[0]; out[1] = a->buf[1]; out[2] = a->buf[2]; out[3] = a->buf[3];
			a->emit( out, 4, a->ctx );
		}
		a->count++;
		break;

	case NV2A_TRI_FAN: /* also TRI_POLYGON - (v0, v[i-1], v[i]) for i >= 2 */
		if( a->count == 0 )
		{
			a->v0     = *v;
			a->buf[0] = *v; /* prev */
		}
		else if( a->count == 1 )
		{
			a->buf[0] = *v; /* prev, no triangle until the 3rd vertex */
		}
		else
		{
			out[0] = a->v0;
			out[1] = a->buf[0];
			out[2] = *v;
			a->emit( out, 3, a->ctx );
			a->buf[0] = *v;
		}
		a->count++;
		break;

	case NV2A_TRI_STRIP:
		/* buf[0]=v[i], buf[1]=v[i+1] for the triangle about to complete -
		   see the header comment for the even/odd winding swap. */
		if( a->count < 2 )
		{
			a->buf[a->count] = *v;
		}
		else
		{
			uint32_t i = a->count - 2;

			if(( i & 1u ) == 0 )
			{
				out[0] = a->buf[0]; out[1] = a->buf[1]; out[2] = *v;
			}
			else
			{
				out[0] = a->buf[1]; out[1] = a->buf[0]; out[2] = *v;
			}
			a->emit( out, 3, a->ctx );
			a->buf[0] = a->buf[1];
			a->buf[1] = *v;
		}
		a->count++;
		break;

	case NV2A_TRI_QUAD_STRIP:
		/* buf[0]=v[2k], buf[1]=v[2k+1] (the previous quad's trailing
		   edge), buf[2]=v[2k+2] staged until v[2k+3] arrives. */
		if( a->count < 2 )
		{
			a->buf[a->count] = *v;
		}
		else if(( a->count & 1u ) == 0 )
		{
			a->buf[2] = *v; /* v[2k+2], pending */
		}
		else
		{
			out[0] = a->buf[0]; out[1] = a->buf[1]; out[2] = *v; out[3] = a->buf[2];
			a->emit( out, 4, a->ctx );
			a->buf[0] = a->buf[2];
			a->buf[1] = *v;
		}
		a->count++;
		break;

	default:
		break;
	}
}

void nv2a_tri_end( nv2a_tri_asm_t *a )
{
	/* Incomplete trailing vertices are simply never emitted above - end()
	   only needs to stop the count so a stale Begin can't be resumed. */
	if( a )
		a->count = 0;
}

uint32_t nv2a_tri_verts_per_output_prim( nv2a_tri_topology_t topology )
{
	switch( topology )
	{
	case NV2A_TRI_LIST_POINTS:    return 1;
	case NV2A_TRI_LIST_LINES:     return 2;
	case NV2A_TRI_LIST_TRIANGLES:
	case NV2A_TRI_FAN:
	case NV2A_TRI_STRIP:          return 3;
	case NV2A_TRI_LIST_QUADS:
	case NV2A_TRI_QUAD_STRIP:     return 4;
	default:                      return 3; /* unreached: nv2a_tri_topology_t has no other member */
	}
}

void nv2a_tri_chunk( uint32_t vertex_count, uint32_t verts_per_prim, nv2a_tri_chunk_emit_fn emit, void *ctx )
{
	uint32_t start = 0;
	uint32_t max_chunk;

	if( !emit || verts_per_prim == 0 )
		return;

	/* Largest multiple of verts_per_prim that still fits in one
	   DRAW_ARRAYS (<= NV2A_2D_MAX_DRAW_VERTS) - e.g. 255 for triangles,
	   256 for quads, 256 for points/lines. */
	max_chunk = NV2A_2D_MAX_DRAW_VERTS - ( NV2A_2D_MAX_DRAW_VERTS % verts_per_prim );
	if( max_chunk == 0 )
		return;

	while( start < vertex_count )
	{
		uint32_t remaining = vertex_count - start;
		uint32_t count = remaining > max_chunk ? max_chunk : remaining;

		count -= count % verts_per_prim;
		if( count == 0 )
			break;

		emit( start, count, ctx );
		start += count;
	}
}
