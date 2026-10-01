/*
nv2a_studio_gpu_host_test.c - host test for ref/nv2a/nv2a_studio_gpu_util.c,
the pure half of the GPU studio path (docs/r3d/divergences.md #143).
Run from the repository root through tools/host-tests/run.sh, or:

  gcc -O2 -Wall -std=c99 -Iref/nv2a tools/host-tests/nv2a_studio_gpu_host_test.c \
      ref/nv2a/nv2a_studio_gpu_util.c ref/nv2a/nv2a_tri_util.c -lm -o /tmp/t && /tmp/t [model.mdl ...]

Checks:
  - the palette slots: 40 bones, 4 registers each, all inside the 192
    hardware constants, none on c[0..31] (hardware 96..127), no overlap;
  - the geometry against the TriAPI's own assembler (nv2a_tri_util.c, the
    CPU path's triangle order and winding) on synthetic strips and fans
    and on every submodel of each .mdl named on the command line;
  - the palette plan: one palette per submodel when it fits, else per
    mesh, else the mesh stays on the CPU;
  - the gamma table segments against the engine's own table
    (engine/client/gamma.c BuildGammaTable, ported below) at the default
    cvars and at the extremes the menu allows;
  - the program's lighting arithmetic against R_StudioLighting's
    (r_studio.c:1593-1645) colour bytes: never more than 1 apart, and how
    often they differ.
Exit status is nonzero iff a check failed.
*/
#include "nv2a_studio_gpu_util.h"
#include "nv2a_tri_util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, total;

#define CHECK( cond, msg ) do { total++; if( !( cond )) { fails++; printf( "FAIL: %s\n", msg ); } } while( 0 )

/* ------------------------------------------------------------------ */

static void test_slots( void )
{
	unsigned char used[192];
	uint32_t p;
	int ok = 1;

	memset( used, 0, sizeof( used ));
	for( p = 0; p < NV2A_SGPU_MAX_PALETTE; p++ )
	{
		uint32_t m = nv2a_sgpu_matrix_slot( p ), l = nv2a_sgpu_light_slot( p ), s;
		uint32_t slots[4] = { m, m + 1, m + 2, l };

		for( s = 0; s < 4; s++ )
		{
			if( slots[s] >= 192 || ( slots[s] >= NV2A_SGPU_HW_C0 && slots[s] < NV2A_SGPU_HW_C0 + NV2A_SGPU_C_END ) || used[slots[s]] )
				ok = 0;
			else
				used[slots[s]] = 1;
		}
	}
	CHECK( ok, "palette slots: inside 0..191, off hardware 96..127, no overlap" );
	CHECK( NV2A_SGPU_C_LUT + NV2A_SGPU_LUT_REGS == NV2A_SGPU_C_END, "LUT ends where the palette's upper block starts" );
	{
		uint32_t free_regs = 192u - NV2A_SGPU_C_END;
		CHECK( free_regs == 4u * NV2A_SGPU_MAX_PALETTE, "every free register is a palette register" );
	}
	CHECK( (int)nv2a_sgpu_matrix_slot( 0 ) - 96 >= -96 && (int)nv2a_sgpu_light_slot( NV2A_SGPU_MAX_PALETTE - 1 ) - 96 <= 95,
		"A0 addresses stay in -96..95" );
}

/* ------------------------------------------------------------------ */
/* The oracle: the TriAPI assembler fed the same tricmds.              */
/* ------------------------------------------------------------------ */

typedef struct
{
	float  *tri;   /* 12 floats a triangle: (vi, ni, s, t) x 3 */
	uint32_t count, cap;
} oracle_t;

static void oracle_emit( const nv2a_tri_vert_t *v, uint32_t n, void *ctx )
{
	oracle_t *o = (oracle_t *)ctx;
	uint32_t i;

	if( n != 3 )
		return;
	if( o->count == o->cap )
	{
		o->cap = o->cap ? o->cap * 2 : 256;
		o->tri = (float *)realloc( o->tri, o->cap * 12 * sizeof( float ));
	}
	for( i = 0; i < 3; i++ )
	{
		o->tri[o->count * 12 + i * 4 + 0] = v[i].x;
		o->tri[o->count * 12 + i * 4 + 1] = v[i].y;
		o->tri[o->count * 12 + i * 4 + 2] = v[i].z;
		o->tri[o->count * 12 + i * 4 + 3] = v[i].u;
	}
	o->count++;
}

static void oracle_mesh( const int16_t *cmd, oracle_t *o )
{
	nv2a_tri_asm_t a;
	int n;

	o->count = 0;
	while(( n = *cmd++ ) != 0 )
	{
		nv2a_tri_begin( &a, n < 0 ? NV2A_TRI_FAN : NV2A_TRI_STRIP, oracle_emit, o );
		if( n < 0 )
			n = -n;
		for( ; n > 0; n--, cmd += 4 )
		{
			nv2a_tri_vert_t v;
			memset( &v, 0, sizeof( v ));
			v.x = cmd[0];
			v.y = cmd[1];
			v.z = cmd[2];
			v.u = cmd[3];
			nv2a_tri_push( &a, &v );
		}
		nv2a_tri_end( &a );
	}
}

/* Builds `src` (whose verts[i] = (i, 0, 0) and norms[j] = (j, 0, 0), so
   a built vertex names its own tuple) and compares every mesh with the
   oracle. Returns the number of triangle mismatches; -1 on a build
   failure. Fills *sizes_out and the meshes/palettes when non-NULL. */
static int check_source( const nv2a_sgpu_source_t *src, nv2a_sgpu_sizes_t *sizes_out, nv2a_sgpu_mesh_t *meshes_out, nv2a_sgpu_palette_t *pal_out, int identity_coords )
{
	nv2a_sgpu_sizes_t sizes;
	uint32_t maxv;
	void *scratch;
	nv2a_sgpu_vertex_t *verts;
	uint16_t *idx;
	nv2a_sgpu_mesh_t *meshes;
	nv2a_sgpu_palette_t *pals;
	oracle_t o = { 0 };
	int m, bad = 0;

	if( !nv2a_sgpu_scan( src, &maxv ))
		return -1;
	scratch = malloc( nv2a_sgpu_scratch_bytes( maxv ) + 16 );
	if( !nv2a_sgpu_measure( src, scratch, &sizes ))
	{
		free( scratch );
		return -1;
	}
	verts = (nv2a_sgpu_vertex_t *)calloc( sizes.vertices + 1, sizeof( *verts ));
	idx = (uint16_t *)calloc( sizes.indices + 2, sizeof( *idx ));
	meshes = (nv2a_sgpu_mesh_t *)calloc( src->nummesh + 1, sizeof( *meshes ));
	pals = (nv2a_sgpu_palette_t *)calloc( sizes.palettes + 1, sizeof( *pals ));
	if( !nv2a_sgpu_build( src, scratch, verts, idx, meshes, pals, &sizes ))
		bad = -1;

	for( m = 0; m < src->nummesh && bad >= 0; m++ )
	{
		uint32_t t, k;

		oracle_mesh( src->tricmds[m], &o );
		if( meshes[m].tris != o.count || ( meshes[m].first_index & 1 ) || ( meshes[m].index_count & 1 )
			|| meshes[m].index_count < meshes[m].tris * 3 || meshes[m].index_count > meshes[m].tris * 3 + 3 )
		{
			bad++;
			continue;
		}
		for( t = 0; t < o.count; t++ )
		{
			for( k = 0; k < 3; k++ )
			{
				const nv2a_sgpu_vertex_t *v = &verts[idx[meshes[m].first_index + t * 3 + k]];
				const float *w = &o.tri[t * 12 + k * 4];
				int vi = (int)w[0], ni = (int)w[1];
				float want_x = identity_coords ? w[0] : src->verts[vi * 3 + 0];
				float want_n = identity_coords ? w[1] : src->norms[ni * 3 + 0];

				if( v->pos[0] != want_x || v->norm[0] != want_n || v->st[0] != w[2] || v->st[1] != w[3] )
					bad++;
				/* the vertex's addresses name its own bones' palette slots */
				if( meshes[m].palette != NV2A_SGPU_NO_PALETTE )
				{
					const nv2a_sgpu_palette_t *p = &pals[meshes[m].palette];
					uint32_t e, found = 0;

					for( e = 0; e < p->count; e++ )
					{
						if( p->bone[e] == src->vertbone[vi] && v->addr[0] == (float)((int)nv2a_sgpu_matrix_slot( e ) - 96 ))
							found |= 1;
						if( p->bone[e] == src->normbone[ni] && v->addr[1] == (float)((int)nv2a_sgpu_light_slot( e ) - 96 ))
							found |= 2;
					}
					if( found != 3 )
						bad++;
				}
			}
		}
		/* the pad triangle, if any, is degenerate */
		if( meshes[m].index_count == meshes[m].tris * 3 + 3 )
		{
			const uint16_t *pad = &idx[meshes[m].first_index + meshes[m].tris * 3];
			if( pad[0] != pad[1] || pad[1] != pad[2] )
				bad++;
		}
	}

	if( sizes_out )
		*sizes_out = sizes;
	if( meshes_out )
		memcpy( meshes_out, meshes, src->nummesh * sizeof( *meshes ));
	if( pal_out )
		memcpy( pal_out, pals, sizes.palettes * sizeof( *pals ));
	free( o.tri );
	free( scratch );
	free( verts );
	free( idx );
	free( meshes );
	free( pals );
	return bad;
}

static float h2f_stub( uint16_t h )
{
	return (float)h * 0.5f;
}

static void test_geometry_synthetic( void )
{
	/* mesh 0: a strip of 5 and a fan of 4, sharing one tuple;
	   mesh 1: a strip of 4 (odd triangle count overall: 2 -> padded? no, 2 tris
	   = 6 indices, even) plus a strip of 3 = 3 tris -> 9 indices -> pad */
	static const int16_t mesh0[] = {
		5, 0,0, 1,1,  1,1, 2,2,  2,2, 3,3,  3,3, 4,4,  4,4, 5,5,
		-4, 0,0, 1,1,  4,4, 5,5,  5,0, 6,6,  6,1, 7,7,
		0 };
	static const int16_t mesh1[] = {
		4, 1,1, 1,1,  2,2, 2,2,  3,3, 3,3,  1,1, 1,1,
		3, 6,6, 8,8,  5,5, 9,9,  4,4, 1,1,
		0 };
	static const int16_t *cmds[2] = { mesh0, mesh1 };
	static const uint8_t uvf[2] = { 0, 1 };
	float verts[7 * 3], norms[7 * 3];
	uint8_t vb[7] = { 0, 1, 2, 3, 4, 5, 6 }, nb[7] = { 6, 5, 4, 3, 2, 1, 0 };
	nv2a_sgpu_source_t src;
	nv2a_sgpu_sizes_t sizes;
	nv2a_sgpu_mesh_t meshes[2];
	nv2a_sgpu_palette_t pals[2];
	int i, bad;

	for( i = 0; i < 7; i++ )
	{
		verts[i * 3] = (float)i; verts[i * 3 + 1] = verts[i * 3 + 2] = 0.0f;
		norms[i * 3] = (float)i; norms[i * 3 + 1] = norms[i * 3 + 2] = 0.0f;
	}
	memset( &src, 0, sizeof( src ));
	src.numverts = 7; src.vertbone = vb; src.verts = verts;
	src.numnorms = 7; src.normbone = nb; src.norms = norms;
	src.nummesh = 1; src.tricmds = cmds; src.uv_float = NULL;

	bad = check_source( &src, &sizes, meshes, pals, 1 );
	CHECK( bad == 0, "synthetic mesh 0: same triangles, order and winding as the TriAPI assembler" );
	CHECK( sizes.vertices == 7, "synthetic mesh 0: 7 unique (v, n, s, t) tuples" );
	CHECK( meshes[0].tris == 5 && meshes[0].index_count == 18, "synthetic mesh 0: 5 triangles, padded to 18 indices" );
	CHECK( sizes.palettes == 1 && pals[0].count == 7, "synthetic: one submodel palette of the 7 bones used" );

	/* the UV_COORDS mesh uses half_to_float; compare against it directly */
	src.nummesh = 2; src.uv_float = uvf; src.half_to_float = h2f_stub;
	{
		uint32_t maxv;
		void *scratch;
		nv2a_sgpu_vertex_t *v;
		uint16_t *idx;
		int ok = nv2a_sgpu_scan( &src, &maxv );

		scratch = malloc( nv2a_sgpu_scratch_bytes( maxv ));
		ok = ok && nv2a_sgpu_measure( &src, scratch, &sizes );
		v = (nv2a_sgpu_vertex_t *)calloc( sizes.vertices, sizeof( *v ));
		idx = (uint16_t *)calloc( sizes.indices, sizeof( *idx ));
		ok = ok && nv2a_sgpu_build( &src, scratch, v, idx, meshes, pals, &sizes );
		CHECK( ok, "synthetic two meshes: builds" );
		CHECK( meshes[1].first_index == 18 && meshes[1].tris == 3 && meshes[1].index_count == 12, "mesh 1 starts on an even index, 3 tris padded to 12" );
		CHECK( ok && v[idx[18]].st[0] == 0.5f && v[idx[18]].st[1] == 0.5f, "UV_COORDS mesh: coordinates through half_to_float" );
		CHECK( ok && v[idx[0]].st[0] == 1.0f, "plain mesh: raw s" );
		CHECK( ok && meshes[0].palette == 0 && meshes[1].palette == 0, "no normal-range check without mesh_numnorms" );

		/* mesh 0 owns normals 0..6, mesh 1 owns 7..13 but uses 1..6 */
		{
			static const int mnorms[2] = { 7, 7 };
			src.mesh_numnorms = mnorms;
			ok = nv2a_sgpu_measure( &src, scratch, &sizes ) && nv2a_sgpu_build( &src, scratch, v, idx, meshes, pals, &sizes );
			CHECK( ok && meshes[0].palette == 0 && meshes[1].palette == NV2A_SGPU_NO_PALETTE,
				"a mesh lighting another mesh's normals stays on the CPU" );
			src.mesh_numnorms = NULL;
		}
		free( scratch ); free( v ); free( idx );
	}

	/* malformed: index out of range */
	{
		static const int16_t badcmd[] = { 3, 0,0,0,0, 9,0,0,0, 1,1,0,0, 0 };
		static const int16_t *bc[1] = { badcmd };
		uint32_t maxv;
		src.nummesh = 1; src.tricmds = bc; src.uv_float = NULL;
		CHECK( !nv2a_sgpu_scan( &src, &maxv ), "an out-of-range vertex index fails the scan" );
		src.tricmds = cmds; src.nummesh = 2; src.numbones = 6; /* bone 6 is used */
		CHECK( !nv2a_sgpu_scan( &src, &maxv ), "a bone at or past numbones fails the scan" );
		src.numbones = 7;
		CHECK( nv2a_sgpu_scan( &src, &maxv ), "bones below numbones pass" );
		src.numbones = 0;
	}
}

/* Palette planning: 50 bones over two meshes of 25 each -> per-mesh
   palettes; a mesh of 45 -> stays on the CPU. */
static void test_palette_plan( void )
{
	enum { NV = 100 };
	static int16_t m0[1 + 4 * NV + 1], m1[1 + 4 * NV + 1], m2[1 + 4 * NV + 1];
	const int16_t *cmds[3] = { m0, m1, m2 };
	float verts[NV * 3], norms[NV * 3];
	uint8_t vb[NV], nb[NV];
	nv2a_sgpu_source_t src;
	nv2a_sgpu_sizes_t sizes;
	nv2a_sgpu_mesh_t meshes[3];
	nv2a_sgpu_palette_t pals[3];
	int i, bad;

	for( i = 0; i < NV; i++ )
	{
		verts[i * 3] = (float)i; verts[i * 3 + 1] = verts[i * 3 + 2] = 0.0f;
		norms[i * 3] = (float)i; norms[i * 3 + 1] = norms[i * 3 + 2] = 0.0f;
		vb[i] = (uint8_t)i;      /* vertex i -> bone i */
		nb[i] = (uint8_t)i;
	}
	/* m0: vertices 0..24 (bones 0..24), m1: 25..49, m2: 50..94 (45 bones) */
	m0[0] = 25; for( i = 0; i < 25; i++ ) { m0[1 + i * 4] = (int16_t)i; m0[2 + i * 4] = (int16_t)i; m0[3 + i * 4] = 0; m0[4 + i * 4] = 0; } m0[1 + 25 * 4] = 0;
	m1[0] = 25; for( i = 0; i < 25; i++ ) { m1[1 + i * 4] = (int16_t)( 25 + i ); m1[2 + i * 4] = (int16_t)( 25 + i ); m1[3 + i * 4] = 0; m1[4 + i * 4] = 0; } m1[1 + 25 * 4] = 0;
	m2[0] = 45; for( i = 0; i < 45; i++ ) { m2[1 + i * 4] = (int16_t)( 50 + i ); m2[2 + i * 4] = (int16_t)( 50 + i ); m2[3 + i * 4] = 0; m2[4 + i * 4] = 0; } m2[1 + 45 * 4] = 0;

	memset( &src, 0, sizeof( src ));
	src.numverts = NV; src.vertbone = vb; src.verts = verts;
	src.numnorms = NV; src.normbone = nb; src.norms = norms;
	src.tricmds = cmds;

	src.nummesh = 1;
	bad = check_source( &src, &sizes, meshes, pals, 1 );
	CHECK( bad == 0 && sizes.palettes == 1 && pals[0].count == 25, "25 bones: one submodel palette" );

	src.nummesh = 2;
	bad = check_source( &src, &sizes, meshes, pals, 1 );
	CHECK( bad == 0 && sizes.palettes == 2 && meshes[0].palette == 0 && meshes[1].palette == 1
		&& pals[0].count == 25 && pals[1].count == 25 && pals[1].bone[0] == 25, "50 bones over two meshes: a palette per mesh" );

	src.nummesh = 3;
	bad = check_source( &src, &sizes, meshes, pals, 1 );
	CHECK( bad == 0 && sizes.palettes == 3 && meshes[2].palette == NV2A_SGPU_NO_PALETTE && meshes[0].palette == 0,
		"a 45-bone mesh stays on the CPU, the others keep their palettes" );
}

/* ------------------------------------------------------------------ */
/* Real models                                                         */
/* ------------------------------------------------------------------ */

static int32_t rd32( const unsigned char *b, size_t o ) { int32_t v; memcpy( &v, b + o, 4 ); return v; }

static float h2f_real( uint16_t h )
{
	/* IEEE half to float, for the test only (the engine's HalfToFloat). */
	uint32_t s = ( h >> 15 ) & 1, e = ( h >> 10 ) & 31, m = h & 1023;
	float f = e == 0 ? ldexpf( (float)m, -24 ) : e == 31 ? INFINITY : ldexpf( (float)( m | 1024 ), (int)e - 25 );
	return s ? -f : f;
}

static void test_model( const char *path )
{
	FILE *f = fopen( path, "rb" );
	unsigned char *b;
	long len;
	int bp, submodels = 0, failed = 0, cpu_meshes = 0;
	uint32_t max_pal = 0, verts = 0, indices = 0;
	char msg[512];

	if( !f )
	{
		printf( "note: %s not found, skipped\n", path );
		return;
	}
	fseek( f, 0, SEEK_END ); len = ftell( f ); fseek( f, 0, SEEK_SET );
	b = (unsigned char *)malloc( len );
	if( fread( b, 1, len, f ) != (size_t)len ) { fclose( f ); free( b ); return; }
	fclose( f );
	if( len < 212 || memcmp( b, "IDST", 4 ) != 0 )
	{
		free( b ); /* a sequence-group file (IDSQ) or not a model: nothing to build */
		return;
	}

	for( bp = 0; bp < rd32( b, 204 ); bp++ )
	{
		size_t bpo = rd32( b, 208 ) + bp * 76;
		int nm = rd32( b, bpo + 64 ), mi = rd32( b, bpo + 72 ), m;

		for( m = 0; m < nm; m++ )
		{
			size_t mo = mi + m * 112;
			int nmesh = rd32( b, mo + 72 ), meshi = rd32( b, mo + 76 ), k, bad;
			const int16_t **cmds = (const int16_t **)malloc(( nmesh + 1 ) * sizeof( *cmds ));
			int *mnorms = (int *)malloc(( nmesh + 1 ) * sizeof( int ));
			nv2a_sgpu_source_t src;
			nv2a_sgpu_sizes_t sizes;
			nv2a_sgpu_mesh_t *meshes = (nv2a_sgpu_mesh_t *)calloc( nmesh + 1, sizeof( *meshes ));
			nv2a_sgpu_palette_t *pals = (nv2a_sgpu_palette_t *)calloc( nmesh + 1, sizeof( *pals ));

			for( k = 0; k < nmesh; k++ )
			{
				cmds[k] = (const int16_t *)( b + rd32( b, meshi + k * 20 + 4 ));
				mnorms[k] = rd32( b, meshi + k * 20 + 12 );
			}
			memset( &src, 0, sizeof( src ));
			src.numverts = rd32( b, mo + 80 );
			src.vertbone = b + rd32( b, mo + 84 );
			src.verts = (const float *)( b + rd32( b, mo + 88 ));
			src.numnorms = rd32( b, mo + 92 );
			src.normbone = b + rd32( b, mo + 96 );
			src.norms = (const float *)( b + rd32( b, mo + 100 ));
			src.nummesh = nmesh;
			src.tricmds = cmds;
			src.mesh_numnorms = mnorms;
			src.numbones = rd32( b, 140 );
			src.half_to_float = h2f_real;

			bad = check_source( &src, &sizes, meshes, pals, 0 );
			submodels++;
			if( bad != 0 )
				failed++;
			for( k = 0; k < nmesh; k++ )
				if( meshes[k].palette == NV2A_SGPU_NO_PALETTE )
					cpu_meshes++;
			for( k = 0; k < (int)sizes.palettes; k++ )
				if( pals[k].count > max_pal )
					max_pal = pals[k].count;
			verts += sizes.vertices;
			indices += sizes.indices;
			free( cmds ); free( mnorms ); free( meshes ); free( pals );
		}
	}
	snprintf( msg, sizeof( msg ), "%s: %d submodels match the TriAPI assembler", path, submodels );
	CHECK( failed == 0, msg );
	snprintf( msg, sizeof( msg ), "%s: every mesh fits a palette and lights only its own normals", path );
	CHECK( cpu_meshes == 0, msg );
	printf( "  %s: %d submodels, largest palette %u, %u vertices (%u bytes), %u indices (%u bytes)\n",
		path, submodels, max_pal, verts, verts * (uint32_t)sizeof( nv2a_sgpu_vertex_t ), indices, indices * 2u );
	free( b );
}

/* ------------------------------------------------------------------ */
/* Gamma table and lighting                                            */
/* ------------------------------------------------------------------ */

/* engine/client/gamma.c BuildGammaTable (:35-79), the light table only,
   with the engine's own float/double mix. */
static void build_lightgamma( float gamma, float brightness, float lightgamma, uint32_t out[1024] )
{
	float g1, g3;
	int i;

	g1 = gamma != 0.0 ? 1.0 / gamma : 0.4;
	if( brightness <= 0.0 )
		g3 = 0.125;
	else if( brightness <= 1.0 )
		g3 = 0.125 - brightness * brightness * 0.075;
	else
		g3 = 0.05;

	for( i = 0; i < 1024; i++ )
	{
		float f = pow( i / 1023.0, (double)lightgamma );
		double d;
		int inf;

		if( brightness > 1.0 )
			f *= brightness;
		if( f <= g3 )
			f = ( f / g3 ) * 0.125;
		else
			f = (( f - g3 ) / ( 1.0 - g3 )) * 0.875 + 0.125;
		d = pow( (double)f, (double)g1 );
		inf = d * 1023.0;
		out[i] = inf < 0 ? 0 : inf > 1023 ? 1023 : inf;
	}
}

/* xorshift: reproducible across hosts */
static uint32_t rng = 2463534242u;
static float frand( void )
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return (float)( rng >> 8 ) / 16777216.0f;
}

static void test_lighting( const uint32_t *table, const char *name, float max_lut_err, double max_mismatch )
{
	float lut[NV2A_SGPU_LUT_REGS][4];
	float err = nv2a_sgpu_build_lut( table, lut );
	long n, mismatched = 0, samples = 0;
	int worst = 0;
	char msg[256];

	snprintf( msg, sizeof( msg ), "%s: table segments within %.2f of a colour byte (got %.3f)", name, max_lut_err, err );
	CHECK( err <= max_lut_err, msg );

	for( n = 0; n < 200000; n++ )
	{
		static const int flag_set[4] = { 0, NV2A_SGPU_NF_FLATSHADE, NV2A_SGPU_NF_FULLBRIGHT, 0 };
		float normal[3], blight[3], color[3], len;
		float ambient = (float)(int)( frand() * 128.0f ); /* alight_t.ambientlight is an int */
		float shade = (float)(int)( frand() * 192.0f );
		int flags = flag_set[n & 3], c;
		uint8_t a[3], b[3];

		normal[0] = frand() * 2 - 1; normal[1] = frand() * 2 - 1; normal[2] = frand() * 2 - 1;
		len = sqrtf( normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] ) + 1e-6f;
		normal[0] /= len; normal[1] /= len; normal[2] /= len;
		blight[0] = frand() * 2 - 1; blight[1] = frand() * 2 - 1; blight[2] = frand() * 2 - 1;
		len = sqrtf( blight[0] * blight[0] + blight[1] * blight[1] + blight[2] * blight[2] ) + 1e-6f;
		blight[0] /= len; blight[1] /= len; blight[2] /= len;
		color[0] = 0.5f + frand() * 0.5f; color[1] = 0.5f + frand() * 0.5f; color[2] = 0.5f + frand() * 0.5f;
		if(( n & 7 ) == 0 )
			color[0] = color[1] = color[2] = 1.0f;

		nv2a_sgpu_ref_color( table, ambient, shade, color, flags, normal, blight, a );
		nv2a_sgpu_vp_color( (const float (*)[4])lut, ambient, shade, color, flags, normal, blight, b );
		for( c = 0; c < 3; c++ )
		{
			int d = abs( (int)a[c] - (int)b[c] );
			samples++;
			if( d )
				mismatched++;
			if( d > worst )
				worst = d;
		}
	}
	printf( "  %s: table error %.3f byte, %ld of %ld colour bytes differ (%.2f%%), worst %d\n",
		name, err, mismatched, samples, 100.0 * mismatched / samples, worst );
	snprintf( msg, sizeof( msg ), "%s: program colour within 1 of the CPU path's", name );
	CHECK( worst <= 1, msg );
	snprintf( msg, sizeof( msg ), "%s: at most %.1f%% of colour bytes differ", name, max_mismatch );
	CHECK( 100.0 * mismatched / samples <= max_mismatch, msg );
}

static void test_gamma_gate( void )
{
	uint32_t t[1024];
	float lut[NV2A_SGPU_LUT_REGS][4];

	build_lightgamma( 3.0f, 0.0f, 1.8f, t );
	CHECK( nv2a_sgpu_build_lut( t, lut ) > 0.5f, "gamma 3 / lightgamma 1.8: the segments are too coarse - the 0.5 gate keeps it on the CPU" );
	build_lightgamma( 2.5f, 1.0f, 2.5f, t );
	printf( "  brightness 1: table error %.3f byte\n", nv2a_sgpu_build_lut( t, lut ));
}

static void test_consts( void )
{
	float c9[4], flat, mesh[3][4], fixed[3][4];
	float lc[3] = { 1.0f, 0.5f, 0.25f };

	nv2a_sgpu_light_consts( 10.0f, 100.0f, c9, &flat );
	CHECK( c9[0] == 110.0f && c9[1] == 100.0f && c9[2] == 0.0f && c9[3] == 1.0f, "c[9] = ( ambient + shade, shade, 0, 1 )" );
	CHECK( flat == 10.0f + 100.0f * 0.8f, "flat illum = ambient + shade * 0.8f" );
	nv2a_sgpu_mesh_consts( NV2A_SGPU_NF_FLATSHADE, flat, lc, 128, 0.25f, 0.125f, 64.0f, 32.0f, mesh );
	CHECK( mesh[0][0] == 0.0f && mesh[0][1] == flat && mesh[0][2] == 1.0f && mesh[0][3] == 0.0f, "flat-shade selectors" );
	CHECK( mesh[1][3] == 128.0f / 255.0f && mesh[2][0] == 0.25f && mesh[2][3] == 32.0f, "alpha and texture scales" );
	nv2a_sgpu_mesh_consts( NV2A_SGPU_NF_FULLBRIGHT, flat, lc, 255, 1, 1, 1, 1, mesh );
	CHECK( mesh[0][0] == 1.0f && mesh[0][1] == 0.0f && mesh[0][2] == 0.0f && mesh[0][3] == 1.0f, "fullbright selectors" );
	nv2a_sgpu_fixed_consts( fixed );
	CHECK( fixed[0][0] == (float)NV2A_SGPU_SHADE_LAMBERT - 1.0f && fixed[0][3] == 1.0f / 64.0f && fixed[2][0] == 64.0f, "fixed constants" );
}

int main( int argc, char **argv )
{
	uint32_t table[1024];
	int i;

	test_slots();
	test_geometry_synthetic();
	test_palette_plan();
	test_consts();
	test_gamma_gate();

	test_lighting( NULL, "linear gamma space (identity table)", 0.01f, 1.0 );
	build_lightgamma( 2.5f, 0.0f, 2.5f, table );
	test_lighting( table, "default cvars (gamma 2.5, lightgamma 2.5, brightness 0)", 0.5f, 10.0 );

	for( i = 1; i < argc; i++ )
		test_model( argv[i] );

	printf( "nv2a_studio_gpu: %d/%d checks passed\n", total - fails, total );
	return fails ? 1 : 0;
}
