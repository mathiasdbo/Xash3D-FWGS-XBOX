/*
nv2a_studio_gpu_util.c - see nv2a_studio_gpu_util.h. Pure C99: no pbkit,
no engine, no GPU memory (tools/host-tests/nv2a_studio_gpu_host_test.c).
*/
#include "nv2a_studio_gpu_util.h"

#include <math.h>
#include <string.h>

static uint32_t sgpu_plan_bones( const nv2a_sgpu_source_t *src, nv2a_sgpu_palette_t *palettes, uint16_t *mesh_palette );

uint32_t nv2a_sgpu_matrix_slot( uint32_t p )
{
	if( p < NV2A_SGPU_LOW_BONES )
		return 3u * p;
	return ( NV2A_SGPU_HW_C0 + NV2A_SGPU_C_END ) + 3u * ( p - NV2A_SGPU_LOW_BONES );
}

uint32_t nv2a_sgpu_light_slot( uint32_t p )
{
	if( p < NV2A_SGPU_LOW_BONES )
		return 3u * NV2A_SGPU_LOW_BONES + p;
	return ( NV2A_SGPU_HW_C0 + NV2A_SGPU_C_END ) + 3u * ( NV2A_SGPU_MAX_PALETTE - NV2A_SGPU_LOW_BONES ) + ( p - NV2A_SGPU_LOW_BONES );
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* The scratch layout: an open-addressed table of (local vertex + 1), the
   unique tuples of the mesh being built, and the current strip's local
   vertex numbers. */
static uint32_t sgpu_table_size( uint32_t max_mesh_verts )
{
	uint32_t n = 16;

	while( n < max_mesh_verts * 2u )
		n <<= 1;
	return n;
}

size_t nv2a_sgpu_scratch_bytes( uint32_t max_mesh_verts )
{
	return (size_t)sgpu_table_size( max_mesh_verts ) * sizeof( uint32_t )
		+ (size_t)max_mesh_verts * sizeof( uint64_t )
		+ (size_t)max_mesh_verts * sizeof( uint32_t );
}

typedef struct
{
	uint32_t *table;
	uint32_t  table_mask;
	uint64_t *tuples;
	uint32_t *strip;
	uint32_t  count; /* unique tuples in the current mesh */
} sgpu_scratch_t;

static void sgpu_scratch_init( sgpu_scratch_t *s, void *mem, uint32_t max_mesh_verts )
{
	uint32_t size = sgpu_table_size( max_mesh_verts );

	s->table = (uint32_t *)mem;
	s->table_mask = size - 1u;
	s->tuples = (uint64_t *)( s->table + size );
	s->strip = (uint32_t *)( s->tuples + max_mesh_verts );
	s->count = 0;
}

static void sgpu_scratch_reset( sgpu_scratch_t *s )
{
	memset( s->table, 0, ( s->table_mask + 1u ) * sizeof( uint32_t ));
	s->count = 0;
}

static uint64_t sgpu_key( const int16_t *v )
{
	return (uint64_t)(uint16_t)v[0] | ( (uint64_t)(uint16_t)v[1] << 16 ) | ( (uint64_t)(uint16_t)v[2] << 32 ) | ( (uint64_t)(uint16_t)v[3] << 48 );
}

/* The mesh-local number of tricmd vertex v; *fresh is set when it is new. */
static uint32_t sgpu_intern( sgpu_scratch_t *s, const int16_t *v, int *fresh )
{
	uint64_t key = sgpu_key( v );
	uint32_t h = (uint32_t)( key ^ ( key >> 29 ) ^ ( key >> 41 )) * 2654435761u;
	uint32_t slot = h & s->table_mask;

	for( ;; )
	{
		uint32_t e = s->table[slot];

		if( e == 0 )
		{
			s->tuples[s->count] = key;
			s->table[slot] = ++s->count;
			*fresh = 1;
			return s->count - 1u;
		}
		if( s->tuples[e - 1u] == key )
		{
			*fresh = 0;
			return e - 1u;
		}
		slot = ( slot + 1u ) & s->table_mask;
	}
}

/* One mesh's tricmd vertex count, or -1 when an index is out of range. */
static long sgpu_mesh_verts( const nv2a_sgpu_source_t *src, const int16_t *cmd )
{
	long total = 0;
	int n;

	while(( n = *cmd++ ) != 0 )
	{
		if( n < 0 )
			n = -n;
		total += n;
		for( ; n > 0; n--, cmd += 4 )
		{
			if( cmd[0] < 0 || cmd[0] >= src->numverts || cmd[1] < 0 || cmd[1] >= src->numnorms )
				return -1;
			if( src->numbones > 0 && ( src->vertbone[cmd[0]] >= src->numbones || src->normbone[cmd[1]] >= src->numbones ))
				return -1;
		}
	}
	return total;
}

int nv2a_sgpu_scan( const nv2a_sgpu_source_t *src, uint32_t *max_mesh_verts )
{
	uint32_t best = 0;
	int m;

	if( !src || src->nummesh < 0 || src->numverts < 0 || src->numnorms < 0 )
		return 0;
	for( m = 0; m < src->nummesh; m++ )
	{
		long n = sgpu_mesh_verts( src, src->tricmds[m] );

		if( n < 0 || n > 65535 )
			return 0;
		if( (uint32_t)n > best )
			best = (uint32_t)n;
	}
	*max_mesh_verts = best;
	return 1;
}

/* The bones one mesh references (vertex and normal bones), as a bitmap. */
static void sgpu_mesh_bones( const nv2a_sgpu_source_t *src, const int16_t *cmd, uint32_t bits[8] )
{
	int n;

	memset( bits, 0, 8 * sizeof( uint32_t ));
	while(( n = *cmd++ ) != 0 )
	{
		if( n < 0 )
			n = -n;
		for( ; n > 0; n--, cmd += 4 )
		{
			uint32_t vb = src->vertbone[cmd[0]], nb = src->normbone[cmd[1]];
			bits[vb >> 5] |= 1u << ( vb & 31u );
			bits[nb >> 5] |= 1u << ( nb & 31u );
		}
	}
}

static uint32_t sgpu_popcount( const uint32_t bits[8] )
{
	uint32_t c = 0;
	int i;

	for( i = 0; i < 8; i++ )
	{
		uint32_t x = bits[i];
		while( x )
		{
			x &= x - 1u;
			c++;
		}
	}
	return c;
}

static void sgpu_palette_from_bits( const uint32_t bits[8], nv2a_sgpu_palette_t *pal )
{
	uint32_t b;

	pal->count = 0;
	for( b = 0; b < 256u; b++ )
	{
		if( bits[b >> 5] & ( 1u << ( b & 31u )))
			pal->bone[pal->count++] = (uint8_t)b;
	}
}

/* Walks one mesh's tricmds: interns every vertex (emitting each new one
   through `verts` when non-NULL) and writes the triangle list with the
   TriAPI assembler's order (nv2a_tri_util.c: strip (v[i-2], v[i-1], v[i])
   on even i-2 and (v[i-1], v[i-2], v[i]) on odd, fan (v0, v[i-1], v[i])).
   Returns the triangle count. */
static uint32_t sgpu_walk_mesh( const nv2a_sgpu_source_t *src, int mesh, sgpu_scratch_t *s, uint32_t vertex_base,
	nv2a_sgpu_vertex_t *verts, const uint8_t *bone_to_entry, uint16_t *indices )
{
	const int16_t *cmd = src->tricmds[mesh];
	int uv_float = src->uv_float ? src->uv_float[mesh] : 0;
	uint32_t tris = 0;
	int n;

	sgpu_scratch_reset( s );
	while(( n = *cmd++ ) != 0 )
	{
		int fan = n < 0;
		int count, i;

		if( fan )
			n = -n;
		count = n;
		for( i = 0; i < count; i++, cmd += 4 )
		{
			int fresh;
			uint32_t local = sgpu_intern( s, cmd, &fresh );

			s->strip[i] = vertex_base + local;
			if( fresh && verts )
			{
				nv2a_sgpu_vertex_t *v = &verts[vertex_base + local];
				const float *p = &src->verts[cmd[0] * 3];
				const float *nrm = &src->norms[cmd[1] * 3];
				uint32_t mp = bone_to_entry[src->vertbone[cmd[0]]], lp = bone_to_entry[src->normbone[cmd[1]]];

				v->pos[0] = p[0]; v->pos[1] = p[1]; v->pos[2] = p[2];
				v->norm[0] = nrm[0]; v->norm[1] = nrm[1]; v->norm[2] = nrm[2];
				v->addr[0] = (float)((int32_t)nv2a_sgpu_matrix_slot( mp ) - (int32_t)NV2A_SGPU_HW_C0 );
				v->addr[1] = (float)((int32_t)nv2a_sgpu_light_slot( lp ) - (int32_t)NV2A_SGPU_HW_C0 );
				if( uv_float && src->half_to_float )
				{
					v->st[0] = src->half_to_float( (uint16_t)cmd[2] );
					v->st[1] = src->half_to_float( (uint16_t)cmd[3] );
				}
				else
				{
					v->st[0] = (float)cmd[2];
					v->st[1] = (float)cmd[3];
				}
			}
		}

		for( i = 2; i < count; i++ )
		{
			uint32_t a, b, c = s->strip[i];

			if( fan )
			{
				a = s->strip[0];
				b = s->strip[i - 1];
			}
			else if((( i - 2 ) & 1 ) == 0 )
			{
				a = s->strip[i - 2];
				b = s->strip[i - 1];
			}
			else
			{
				a = s->strip[i - 1];
				b = s->strip[i - 2];
			}
			if( indices )
			{
				indices[tris * 3 + 0] = (uint16_t)a;
				indices[tris * 3 + 1] = (uint16_t)b;
				indices[tris * 3 + 2] = (uint16_t)c;
			}
			tris++;
		}
	}
	return tris;
}

/* Whether every normal mesh m's tricmds use lies in its own range of
   the submodel's normals (see nv2a_sgpu_source_t.mesh_numnorms). */
static int sgpu_mesh_norms_own( const nv2a_sgpu_source_t *src, int m )
{
	const int16_t *cmd = src->tricmds[m];
	int first = 0, k, n;

	if( !src->mesh_numnorms )
		return 1;
	for( k = 0; k < m; k++ )
		first += src->mesh_numnorms[k];
	while(( n = *cmd++ ) != 0 )
	{
		if( n < 0 )
			n = -n;
		for( ; n > 0; n--, cmd += 4 )
		{
			if( cmd[1] < first || cmd[1] >= first + src->mesh_numnorms[m] )
				return 0;
		}
	}
	return 1;
}

/* The palette plan: one palette for the submodel when all of its meshes'
   bones fit, else one per mesh. Fills `palettes` when non-NULL; returns
   the palette count and, per mesh, which palette (or NO_PALETTE). A mesh
   whose normals are not its own is NO_PALETTE either way. */
static uint32_t sgpu_plan_palettes( const nv2a_sgpu_source_t *src, nv2a_sgpu_palette_t *palettes, uint16_t *mesh_palette )
{
	uint32_t count = sgpu_plan_bones( src, palettes, mesh_palette );
	int m;

	for( m = 0; m < src->nummesh; m++ )
	{
		if( mesh_palette[m] != NV2A_SGPU_NO_PALETTE && !sgpu_mesh_norms_own( src, m ))
			mesh_palette[m] = NV2A_SGPU_NO_PALETTE;
	}
	return count;
}

static uint32_t sgpu_plan_bones( const nv2a_sgpu_source_t *src, nv2a_sgpu_palette_t *palettes, uint16_t *mesh_palette )
{
	uint32_t all[8], bits[8];
	int m, i;

	memset( all, 0, sizeof( all ));
	for( m = 0; m < src->nummesh; m++ )
	{
		sgpu_mesh_bones( src, src->tricmds[m], bits );
		for( i = 0; i < 8; i++ )
			all[i] |= bits[i];
	}

	if( sgpu_popcount( all ) <= NV2A_SGPU_MAX_PALETTE )
	{
		if( palettes )
			sgpu_palette_from_bits( all, &palettes[0] );
		for( m = 0; m < src->nummesh; m++ )
			mesh_palette[m] = 0;
		return 1;
	}

	for( m = 0; m < src->nummesh; m++ )
	{
		sgpu_mesh_bones( src, src->tricmds[m], bits );
		if( sgpu_popcount( bits ) <= NV2A_SGPU_MAX_PALETTE )
		{
			if( palettes )
				sgpu_palette_from_bits( bits, &palettes[m] );
			mesh_palette[m] = (uint16_t)m;
		}
		else
		{
			if( palettes )
				palettes[m].count = 0;
			mesh_palette[m] = NV2A_SGPU_NO_PALETTE;
		}
	}
	return (uint32_t)src->nummesh;
}

int nv2a_sgpu_measure( const nv2a_sgpu_source_t *src, void *scratch, nv2a_sgpu_sizes_t *sizes )
{
	sgpu_scratch_t s;
	uint32_t max_mesh_verts;
	uint16_t mesh_palette[256];
	int m;

	if( !nv2a_sgpu_scan( src, &max_mesh_verts ) || src->nummesh > 256 )
		return 0;

	sgpu_scratch_init( &s, scratch, max_mesh_verts );
	memset( sizes, 0, sizeof( *sizes ));
	sizes->max_mesh_verts = max_mesh_verts;
	sizes->palettes = sgpu_plan_palettes( src, NULL, mesh_palette );

	for( m = 0; m < src->nummesh; m++ )
	{
		uint32_t tris = sgpu_walk_mesh( src, m, &s, 0, NULL, NULL, NULL );
		uint32_t n = tris * 3u;

		if( n & 1u )
			n += 3u;
		sizes->vertices += s.count;
		sizes->indices += n;
		if( sizes->vertices > 65536u )
			return 0;
	}
	return 1;
}

int nv2a_sgpu_build( const nv2a_sgpu_source_t *src, void *scratch, nv2a_sgpu_vertex_t *verts, uint16_t *indices,
	nv2a_sgpu_mesh_t *meshes, nv2a_sgpu_palette_t *palettes, const nv2a_sgpu_sizes_t *sizes )
{
	sgpu_scratch_t s;
	uint16_t mesh_palette[256];
	uint8_t bone_to_entry[256];
	uint32_t vbase = 0, ibase = 0;
	int m;

	if( src->nummesh > 256 )
		return 0;

	sgpu_scratch_init( &s, scratch, sizes->max_mesh_verts );
	if( sgpu_plan_palettes( src, palettes, mesh_palette ) != sizes->palettes )
		return 0;

	for( m = 0; m < src->nummesh; m++ )
	{
		uint32_t tris, n, i;
		const nv2a_sgpu_palette_t *pal = NULL;

		memset( bone_to_entry, 0, sizeof( bone_to_entry ));
		if( mesh_palette[m] != NV2A_SGPU_NO_PALETTE )
		{
			pal = &palettes[mesh_palette[m]];
			for( i = 0; i < pal->count; i++ )
				bone_to_entry[pal->bone[i]] = (uint8_t)i;
		}

		tris = sgpu_walk_mesh( src, m, &s, vbase, verts, bone_to_entry, indices + ibase );
		n = tris * 3u;
		if( n & 1u )
		{
			/* One degenerate triangle keeps every mesh's range on a dword
			   boundary (two u16 indices per NV097_ARRAY_ELEMENT16 word);
			   it repeats a vertex, so it covers no pixel. */
			uint16_t last = n ? indices[ibase + n - 1u] : (uint16_t)vbase;
			indices[ibase + n + 0] = last;
			indices[ibase + n + 1] = last;
			indices[ibase + n + 2] = last;
			n += 3u;
		}

		meshes[m].first_index = ibase;
		meshes[m].index_count = n;
		meshes[m].tris = tris;
		meshes[m].palette = mesh_palette[m];
		meshes[m].uv_float = (uint16_t)( src->uv_float ? src->uv_float[m] : 0 );

		vbase += s.count;
		ibase += n;
	}

	return vbase == sizes->vertices && ibase == sizes->indices;
}

/* ------------------------------------------------------------------ */
/* Gamma table                                                         */
/* ------------------------------------------------------------------ */

#define SGPU_TABLE_MAX_INDEX 1020u /* (uint)( 255.0f * 4 ) */

static uint32_t sgpu_table( const uint32_t *table, uint32_t i )
{
	return table ? table[i] : i;
}

float nv2a_sgpu_lut_eval( const float lut[NV2A_SGPU_LUT_REGS][4], uint32_t i )
{
	/* vs_studio.vp's table block, in its order. */
	volatile float idx = (float)i;
	volatile float u = idx * ( 1.0f / 64.0f );
	volatile float fu = u - floorf( u );           /* EXP .y */
	volatile float pair = u - fu + 0.25f;          /* ARL floors it back */
	volatile float off64 = fu * 64.0f;
	volatile float hi = off64 >= 32.0f ? 1.0f : 0.0f; /* SGE */
	volatile float off = hi * -32.0f + off64;      /* MAD, exact: hi is 0 or 1 */
	volatile float lo = -hi + 1.0f;
	const float *r = lut[(int)pair];
	volatile float b0 = r[0] * lo, s0 = r[1] * lo, b1 = r[2] * hi, s1 = r[3] * hi;
	volatile float base = b0 + b1, slope = s0 + s1;
	volatile float t = slope * off;

	return base + t;
}

float nv2a_sgpu_build_lut( const uint32_t *table, float out[NV2A_SGPU_LUT_REGS][4] )
{
	uint32_t k, i;
	float worst = 0.0f;

	for( k = 0; k < 2u * NV2A_SGPU_LUT_REGS; k++ )
	{
		uint32_t start = k * NV2A_SGPU_LUT_SEGMENT;
		uint32_t end = start + NV2A_SGPU_LUT_SEGMENT;
		float base, slope;

		if( end > SGPU_TABLE_MAX_INDEX )
			end = SGPU_TABLE_MAX_INDEX;
		base = (float)sgpu_table( table, start ) / 1023.0f;
		slope = end > start ? ((float)sgpu_table( table, end ) / 1023.0f - base ) / (float)( end - start ) : 0.0f;
		out[k >> 1][( k & 1u ) * 2u + 0u] = base;
		out[k >> 1][( k & 1u ) * 2u + 1u] = slope;
	}

	for( i = 0; i <= SGPU_TABLE_MAX_INDEX; i++ )
	{
		float want = (float)sgpu_table( table, i ) / 1023.0f;
		float err = fabsf( nv2a_sgpu_lut_eval( (const float (*)[4])out, i ) - want ) * 255.0f;

		if( err > worst )
			worst = err;
	}
	return worst;
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

void nv2a_sgpu_fixed_consts( float out[NV2A_SGPU_FIXED_REGS][4] )
{
	float r = (float)NV2A_SGPU_SHADE_LAMBERT;

	out[0][0] = r - 1.0f;
	out[0][1] = 1.0f / r;
	out[0][2] = 4.0f;
	out[0][3] = 1.0f / 64.0f;
	out[1][0] = 1.0f;
	out[1][1] = 255.0f;
	out[1][2] = 1.0f / 255.0f;
	out[1][3] = 32.0f;
	out[2][0] = 64.0f;
	out[2][1] = 0.25f;
	out[2][2] = 0.0f;
	out[2][3] = 0.0f;
}

void nv2a_sgpu_light_consts( float ambient, float shadelight, float out[4], float *flat_illum )
{
	volatile float illum = ambient;

	illum += shadelight;
	out[0] = illum;
	out[1] = shadelight;
	out[2] = 0.0f;
	out[3] = 1.0f;

	illum = ambient;
	illum += shadelight * 0.8f;
	*flat_illum = illum;
}

void nv2a_sgpu_mesh_consts( int face_flags, float flat_illum, const float lightcolor[3], uint8_t alpha,
	float s_scale, float t_scale, float tex_w, float tex_h, float out[NV2A_SGPU_MESH_REGS][4] )
{
	int flat = ( face_flags & NV2A_SGPU_NF_FLATSHADE ) != 0;
	int fullbright = ( face_flags & NV2A_SGPU_NF_FULLBRIGHT ) != 0;

	out[0][0] = flat ? 0.0f : 1.0f;
	out[0][1] = flat ? flat_illum : 0.0f;
	out[0][2] = fullbright ? 0.0f : 1.0f;
	out[0][3] = fullbright ? 1.0f : 0.0f;
	out[1][0] = lightcolor[0];
	out[1][1] = lightcolor[1];
	out[1][2] = lightcolor[2];
	out[1][3] = (float)alpha / 255.0f;
	out[2][0] = s_scale;
	out[2][1] = t_scale;
	out[2][2] = tex_w;
	out[2][3] = tex_h;
}

/* ------------------------------------------------------------------ */
/* The CPU path and the program, for the host test                     */
/* ------------------------------------------------------------------ */

void nv2a_sgpu_ref_color( const uint32_t *table, float ambient, float shadelight, const float lightcolor[3],
	int face_flags, const float normal[3], const float blightvec[3], uint8_t out[3] )
{
	volatile float illum, lv;
	int c;

	if( face_flags & NV2A_SGPU_NF_FULLBRIGHT )
	{
		lv = 1.0f;
	}
	else
	{
		illum = ambient;
		if( face_flags & NV2A_SGPU_NF_FLATSHADE )
		{
			illum += shadelight * 0.8f;
		}
		else
		{
			volatile float r, lightcos;

			lightcos = normal[0] * blightvec[0] + normal[1] * blightvec[1] + normal[2] * blightvec[2];
			if( lightcos > 1.0f )
				lightcos = 1.0f;
			illum += shadelight;
			r = (float)NV2A_SGPU_SHADE_LAMBERT;
			lightcos = ( lightcos + ( r - 1.0f )) / r;
			if( lightcos > 0.0f )
				illum -= shadelight * lightcos;
			if( illum < 0.0f )
				illum = 0.0f;
		}
		if( illum > 255.0f )
			illum = 255.0f;
		lv = (float)sgpu_table( table, (uint32_t)( illum * 4 )) / 1023.0f;
	}

	for( c = 0; c < 3; c++ )
	{
		volatile float v = lightcolor[c] * lv;
		volatile float b = v * 255.0f;
		out[c] = (uint8_t)b;
	}
}

void nv2a_sgpu_vp_color( const float lut[NV2A_SGPU_LUT_REGS][4], float ambient, float shadelight, const float lightcolor[3],
	int face_flags, const float normal[3], const float blightvec[3], uint8_t out[3] )
{
	float fixed[NV2A_SGPU_FIXED_REGS][4], c9[4], mesh[NV2A_SGPU_MESH_REGS][4], flat;
	volatile float x, lv;
	int c;

	nv2a_sgpu_fixed_consts( fixed );
	nv2a_sgpu_light_consts( ambient, shadelight, c9, &flat );
	nv2a_sgpu_mesh_consts( face_flags, flat, lightcolor, 255, 1.0f, 1.0f, 1.0f, 1.0f, mesh );

	/* DP3 is taken as the same three products summed left to right; the
	   hardware's order is not documented (the residual difference is
	   measured, not assumed away). */
	x = normal[0] * blightvec[0] + normal[1] * blightvec[1] + normal[2] * blightvec[2];
	x = x < fixed[1][0] ? x : fixed[1][0];   /* MIN 1 */
	x = x + fixed[0][0];                     /* + ( r - 1 ) */
	x = x * fixed[0][1];                     /* * 1/r */
	x = x > c9[2] ? x : c9[2];               /* MAX 0 */
	x = x * c9[1];                           /* * shadelight */
	x = -x + c9[0];                          /* ambient + shadelight - */
	x = x > c9[2] ? x : c9[2];               /* MAX 0 */
	x = x * mesh[0][0];                      /* lambert selector */
	x = x + mesh[0][1];                      /* flat illum */
	x = x < fixed[1][1] ? x : fixed[1][1];   /* MIN 255 */
	x = x * fixed[0][2];                     /* * 4 */
	x = x - ( x - floorf( x ));              /* EXP .y, then subtract */

	lv = nv2a_sgpu_lut_eval( lut, (uint32_t)x );
	lv = lv * mesh[0][2];
	lv = lv + mesh[0][3];

	for( c = 0; c < 3; c++ )
	{
		volatile float v = mesh[1][c] * lv;
		volatile float b = v * fixed[1][1];
		b = b - ( b - floorf( b ));
		out[c] = (uint8_t)b;
	}
}
