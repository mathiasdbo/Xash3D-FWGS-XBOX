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

/* ------------------------------------------------------------------ */
/* Local lights (elights)                                              */
/* ------------------------------------------------------------------ */

void nv2a_sgpu_elight_gamma_reg( float gamma, float out[4] )
{
	out[0] = gamma;
	out[1] = 1.0f / gamma;
	out[2] = (float)( pow( 255.75, (double)gamma ) / 1023.0 );
	out[3] = 1023.0f;
}

void nv2a_sgpu_elight_consts( const nv2a_sgpu_elights_t *el, const float gamma_reg[4], float out[NV2A_SGPU_ELIGHT_REGS][4] )
{
	int i, n = el->count;

	if( n < 0 )
		n = 0;
	if( n > (int)NV2A_SGPU_MAX_ELIGHTS )
		n = (int)NV2A_SGPU_MAX_ELIGHTS;

	memset( out, 0, sizeof( float ) * 4 * NV2A_SGPU_ELIGHT_REGS );
	for( i = 0; i < n; i++ )
	{
		out[i * 2][0] = el->origin[i][0];
		out[i * 2][1] = el->origin[i][1];
		out[i * 2][2] = el->origin[i][2];
		out[i * 2 + 1][0] = el->color[i][0];
		out[i * 2 + 1][1] = el->color[i][1];
		out[i * 2 + 1][2] = el->color[i][2];
		out[i * 2 + 1][3] = el->r2[i];
		/* R_LightLambert's r = -dot( normal, light ), or +dot for a
		   flipped view model; 0 turns the light off (r is then never
		   above 0). */
		out[5][i] = el->flip ? -1.0f : 1.0f;
	}
	memcpy( out[4], gamma_reg, sizeof( float ) * 4 );
	out[5][2] = NV2A_SGPU_ELIGHT_TINY;
}

int nv2a_sgpu_elights_fit( const nv2a_sgpu_elights_t *el )
{
	int i, c;

	if( el->count < 0 || el->count > (int)NV2A_SGPU_MAX_ELIGHTS )
		return 0;
	for( i = 0; i < el->count; i++ )
	{
		for( c = 0; c < 3; c++ )
		{
			if( !( el->color[i][c] >= NV2A_SGPU_ELIGHT_MIN_COLOR ))
				return 0;
		}
	}
	return 1;
}

static int sgpu_power_table_ok( const uint32_t *table, double g )
{
	int i;

	for( i = 0; i < 1024; i++ )
	{
		double want = floor( pow( i / 1023.0, g ) * 1023.0 );

		if( fabs( (double)table[i] - want ) > 1.0 )
			return 0;
	}
	return 1;
}

int nv2a_sgpu_fit_gamma( const uint32_t *linear, const uint32_t *screen, float *gamma )
{
	double g;

	if( !linear || !screen )
	{
		*gamma = 1.0f;
		return linear == screen; /* both NULL: the identity */
	}

	/* linear[i] = (int)( pow( i / 1023, g ) * 1023 ): a midpoint
	   entry pins g to about 1e-3, then every entry of both tables is
	   checked against it. */
	if( linear[767] == 0 || linear[767] >= 1023 )
		return 0;
	g = log(( linear[767] + 0.5 ) / 1023.0 ) / log( 767.0 / 1023.0 );
	if( !( g > 0.1 && g < 10.0 ))
		return 0;
	if( !sgpu_power_table_ok( linear, g ) || !sgpu_power_table_ok( screen, 1.0 / g ))
		return 0;
	*gamma = (float)g;
	return 1;
}

static uint32_t sgpu_gamma_table( const uint32_t *table, uint32_t b )
{
	if( b >= 1024 )
		return 0;
	return table ? table[b] : b;
}

void nv2a_sgpu_ref_elight_color( const uint32_t *linear, const uint32_t *screen, const nv2a_sgpu_elights_t *el,
	const float rows[3][4], const float localpos[3], const float normal[3], const float color[3], uint8_t out[3] )
{
	float light[NV2A_SGPU_MAX_ELIGHTS][4];
	float finalLight[3] = { 0.0f, 0.0f, 0.0f };
	int i, c;

	/* R_LightStrength: Matrix3x4_VectorITransform( lighttransform[bone],
	   el->origin, lightbonepos ) (public/matrixlib.c:35), then
	   localpos - lightbonepos. */
	for( i = 0; i < el->count; i++ )
	{
		float dir[3], lbp[3];

		dir[0] = el->origin[i][0] - rows[0][3];
		dir[1] = el->origin[i][1] - rows[1][3];
		dir[2] = el->origin[i][2] - rows[2][3];
		lbp[0] = dir[0] * rows[0][0] + dir[1] * rows[1][0] + dir[2] * rows[2][0];
		lbp[1] = dir[0] * rows[0][1] + dir[1] * rows[1][1] + dir[2] * rows[2][1];
		lbp[2] = dir[0] * rows[0][2] + dir[1] * rows[1][2] + dir[2] * rows[2][2];
		light[i][0] = localpos[0] - lbp[0];
		light[i][1] = localpos[1] - lbp[1];
		light[i][2] = localpos[2] - lbp[2];
		light[i][3] = 0.0f;
	}

	if( el->count == 0 )
	{
		for( c = 0; c < 3; c++ )
			out[c] = (uint8_t)( color[c] * 255.0f );
		return;
	}

	for( i = 0; i < el->count; i++ )
	{
		float r = normal[0] * light[i][0] + normal[1] * light[i][1] + normal[2] * light[i][2];

		if( !el->flip )
			r = -r;

		if( r > 0.0f )
		{
			float temp;

			if( light[i][3] == 0.0f )
			{
				float r2 = light[i][0] * light[i][0] + light[i][1] * light[i][1] + light[i][2] * light[i][2];

				if( r2 > 0.0f )
					light[i][3] = el->r2[i] / ( r2 * sqrt( r2 ));
				else
					light[i][3] = 0.0001f;
			}

			temp = r * light[i][3];
			for( c = 0; c < 3; c++ )
				finalLight[c] += el->color[i][c] + temp;
		}
	}

	if( finalLight[0] != 0.0f || finalLight[1] != 0.0f || finalLight[2] != 0.0f )
	{
		for( c = 0; c < 3; c++ )
		{
			float v = finalLight[c] + sgpu_gamma_table( linear, (uint32_t)( color[c] * 1023.0f ));

			if( v > 1023.0f )
				out[c] = 255;
			else
				out[c] = (uint8_t)( sgpu_gamma_table( screen, (uint32_t)v ) >> 2 );
		}
	}
	else
	{
		for( c = 0; c < 3; c++ )
			out[c] = (uint8_t)( color[c] * 255.0f );
	}
}

/* LIT's z: ( x > 0 ) ? y^w : 0 with x, y floored at 0 and w clamped to
   +-( 128 - 1/256 ) (NV_vertex_program; xemu's _LIT evaluates the power
   as exp2( w * log2( y ))). */
static float sgpu_lit_z( float x, float y, float w )
{
	const float epsilon = 1.0f / 256.0f;

	if( w < -( 128.0f - epsilon ))
		w = -( 128.0f - epsilon );
	if( w > 128.0f - epsilon )
		w = 128.0f - epsilon;
	if( y < 0.0f )
		y = 0.0f;
	return x > 0.0f ? exp2f( w * log2f( y )) : 0.0f;
}

void nv2a_sgpu_vp_elight_color( const float k[NV2A_SGPU_ELIGHT_REGS][4], const float rows[3][4],
	const float localpos[3], const float normal[3], const float color[3], uint8_t out[3] )
{
	volatile float world[3], nw[3], d[2][3], r[2], l2[2], rs[2], t[2], m[2], fl[3], e;
	volatile float base[3], p[3], cc[3], sg[3];
	int i, c;

	/* position block: R1 = bonestransform * v (DP4 with w = 1) */
	for( c = 0; c < 3; c++ )
		world[c] = localpos[0] * rows[c][0] + localpos[1] * rows[c][1] + localpos[2] * rows[c][2] + rows[c][3];

	/* local lights. R_LightStrength's light vector is v - R^T ( o - t ),
	   in the vertex bone's frame; for an orthonormal R, -L = R^T ( o - W )
	   with W = R v + t, so r = dot( n, -L ) = dot( R n, o - W ) and
	   |L|^2 = |o - W|^2. */
	for( c = 0; c < 3; c++ )
		nw[c] = normal[0] * rows[c][0] + normal[1] * rows[c][1] + normal[2] * rows[c][2];
	for( i = 0; i < 2; i++ )
	{
		for( c = 0; c < 3; c++ )
			d[i][c] = k[i * 2][c] - world[c];
		r[i] = nw[0] * d[i][0] + nw[1] * d[i][1] + nw[2] * d[i][2];
		l2[i] = d[i][0] * d[i][0] + d[i][1] * d[i][1] + d[i][2] * d[i][2];
	}
	for( i = 0; i < 2; i++ )
	{
		r[i] = r[i] * k[5][i];                         /* MUL: flip sign, or 0 for an unused light */
		l2[i] = l2[i] > k[5][2] ? l2[i] : k[5][2];     /* MAX tiny */
		rs[i] = 1.0f / sqrtf( l2[i] );                 /* RSQ */
		t[i] = rs[i] * rs[i];
		t[i] = t[i] * rs[i];
		t[i] = t[i] * r[i];
		m[i] = 0.0f < r[i] ? 1.0f : 0.0f;              /* SLT */
	}
	for( c = 0; c < 3; c++ )
	{
		volatile float a = t[0] * k[1][3] + k[1][c];   /* MAD: lc0 + temp0 */
		volatile float b;

		a = a * m[0];
		b = t[1] * k[3][3] + k[3][c];
		fl[c] = b * m[1] + a;
	}
	e = m[0] > m[1] ? m[0] : m[1];                     /* MAX: a light reached the vertex */

	/* the base colour bytes, as without local lights */
	for( c = 0; c < 3; c++ )
	{
		volatile float b = color[c] * 255.0f;
		base[c] = b - ( b - floorf( b ));
	}

	/* LinearGammaTable( color * 1023 ) as 1023 * color^gamma, plus the
	   lights, truncated as the table's (uint) index truncates it, then
	   ScreenGammaTable(...) >> 2 as floor( 255.75 * ( c / 1023 )^( 1 / gamma )),
	   evaluated as LIT( t * c, 1 / gamma ) with t = 255.75^gamma / 1023.
	   The LIT chain carries each result into the next LIT's x (its gate,
	   +tiny so a 0 result keeps the next channel open), which comes back
	   out in y. */
	p[0] = sgpu_lit_z( color[0], color[0], k[4][0] );
	p[1] = sgpu_lit_z( p[0] + k[5][2], color[1], k[4][0] );
	p[2] = sgpu_lit_z( p[1] + k[5][2], color[2], k[4][0] );
	p[0] = p[0] + k[5][2];
	p[1] = p[1] + k[5][2];
	for( c = 0; c < 3; c++ )
	{
		volatile float v = p[c] * k[4][3] + fl[c];     /* MAD: * 1023 + the lights */
		cc[c] = v - ( v - floorf( v ));                /* EXP .y, ADD */
		cc[c] = cc[c] * k[4][2];                       /* * t */
	}
	sg[0] = sgpu_lit_z( cc[0], cc[0], k[4][1] );
	sg[1] = sgpu_lit_z( sg[0] + k[5][2], cc[1], k[4][1] );
	sg[2] = sgpu_lit_z( sg[1] + k[5][2], cc[2], k[4][1] );
	sg[0] = sg[0] + k[5][2];
	sg[1] = sg[1] + k[5][2];
	for( c = 0; c < 3; c++ )
	{
		volatile float v = sg[c];
		volatile float b = v - ( v - floorf( v ));
		volatile float diff = b - base[c];
		volatile float o = diff * e + base[c];

		/* o[COL0] is clamped to [0, 1] on output (NV_vertex_program;
		   xemu glsl/vsh.c clamps oD0): a byte above 255 is 255. */
		out[c] = o > 255.0f ? 255 : (uint8_t)o;
	}
}
