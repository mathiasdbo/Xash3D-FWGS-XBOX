/*
nv2a_studio_gpu_host_test.c - host test for ref/nv2a/nv2a_studio_gpu_util.c,
the pure half of the GPU studio path (docs/r3d/divergences.md #143).
Run from the repository root through tools/host-tests/run.sh, or:

  gcc -O2 -Wall -std=c99 -Iref/nv2a tools/host-tests/nv2a_studio_gpu_host_test.c \
      ref/nv2a/nv2a_studio_gpu_util.c ref/nv2a/nv2a_tri_util.c -lm -o /tmp/t && /tmp/t [model.mdl ...]

Checks:
  - the palette slots: 38 bones, 4 registers each, all inside the 192
    hardware constants, none on c[0..31] (hardware 96..127), no overlap,
    and the local lights' c[88..93] clear of all of them;
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
    often they differ;
  - the local lights: the gamma fit against the engine's linear and
    screen tables, and the program's R_LightLambert (one and two elights,
    flipped or not) against the CPU path's colour bytes: how far apart,
    and how often;
  - chrome (divergences.md #170): vs_chrome.vp's arithmetic against
    R_StudioSetupChrome's (gl_studio.c:1120-1153) texture coordinates on
    random bones, normals and chrome origins, eight mutants of the
    transcription that must each be caught, the zero-vector case, the
    constant slots, and the .inl files' instruction counts and FLD_FINAL.
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
		uint32_t free_regs = 192u - NV2A_SGPU_C_END, s;

		CHECK( 4u * NV2A_SGPU_MAX_PALETTE + NV2A_SGPU_ELIGHT_REGS <= free_regs, "the palette and the local lights fit the free registers" );
		ok = 1;
		for( s = 0; s < NV2A_SGPU_ELIGHT_REGS; s++ )
		{
			uint32_t hw = NV2A_SGPU_HW_C0 + NV2A_SGPU_C_ELIGHT + s;

			if( hw >= 192 || used[hw] )
				ok = 0;
		}
		CHECK( ok && NV2A_SGPU_C_ELIGHT == 88u, "local lights at c[88..93], hardware 184..189, clear of the palette" );
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


/* ------------------------------------------------------------------ */
/* Local lights                                                        */
/* ------------------------------------------------------------------ */

/* engine/client/gamma.c:77-78: the linear and screen tables. */
static void build_powtables( double gamma, uint32_t linear[1024], uint32_t screen[1024] )
{
	int i;

	for( i = 0; i < 1024; i++ )
	{
		linear[i] = pow( i / 1023.0, gamma ) * 1023.0;
		screen[i] = pow( i / 1023.0, 1.0 / gamma ) * 1023.0;
	}
}

/* A random rotation (unit quaternion) and translation: bone matrices are
   Matrix3x4_FromOriginQuat products, orthonormal up to rounding. */
static void random_bone( float rows[3][4] )
{
	float q[4], len;
	int i;

	for( i = 0; i < 4; i++ )
		q[i] = frand() * 2.0f - 1.0f;
	len = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
	for( i = 0; i < 4; i++ )
		q[i] /= len;
	rows[0][0] = 1.0f - 2.0f * ( q[1] * q[1] + q[2] * q[2] );
	rows[1][0] = 2.0f * ( q[0] * q[1] + q[3] * q[2] );
	rows[2][0] = 2.0f * ( q[0] * q[2] - q[3] * q[1] );
	rows[0][1] = 2.0f * ( q[0] * q[1] - q[3] * q[2] );
	rows[1][1] = 1.0f - 2.0f * ( q[0] * q[0] + q[2] * q[2] );
	rows[2][1] = 2.0f * ( q[1] * q[2] + q[3] * q[0] );
	rows[0][2] = 2.0f * ( q[0] * q[2] + q[3] * q[1] );
	rows[1][2] = 2.0f * ( q[1] * q[2] - q[3] * q[0] );
	rows[2][2] = 1.0f - 2.0f * ( q[0] * q[0] + q[1] * q[1] );
	rows[0][3] = frand() * 2000.0f - 1000.0f;
	rows[1][3] = frand() * 2000.0f - 1000.0f;
	rows[2][3] = frand() * 400.0f - 200.0f;
}

static void test_fit_gamma( void )
{
	uint32_t lin[1024], scr[1024];
	float g = 0.0f;
	double gammas[3] = { 1.8, 2.5, 3.0 };
	int i, ok = 1;

	for( i = 0; i < 3; i++ )
	{
		build_powtables( gammas[i], lin, scr );
		if( !nv2a_sgpu_fit_gamma( lin, scr, &g ) || fabs( g - gammas[i] ) > 0.01 )
			ok = 0;
	}
	CHECK( ok, "gamma fit: recovers 1.8, 2.5 and 3.0 from the engine's tables" );
	CHECK( nv2a_sgpu_fit_gamma( NULL, NULL, &g ) && g == 1.0f, "gamma fit: no tables (linear gamma space) is gamma 1" );
	build_powtables( 2.5, lin, scr );
	scr[300] += 5;
	CHECK( !nv2a_sgpu_fit_gamma( lin, scr, &g ), "gamma fit: a screen table that is not the inverse power is refused" );
	build_powtables( 2.5, lin, scr );
	build_lightgamma( 2.5f, 0.0f, 2.5f, lin );
	CHECK( !nv2a_sgpu_fit_gamma( lin, scr, &g ), "gamma fit: a table that is not a power (the light table) is refused" );
}

/* One configuration: `count` elights at the muzzle flash's colour and
   radius (r_studio.c R_StudioEntityLight / the EF_MUZZLEFLASH elight,
   255 192 64, radius 24) or random ones, around random vertices. */
static void test_elight_case( double gamma, int identity, int count, int flip, int muzzle, int min_rgb, int gated, const char *name,
	int max_diff, double max_mismatch_pct )
{
	uint32_t lin[1024], scr[1024];
	const uint32_t *L = NULL, *S = NULL;
	float g = 1.0f, k[NV2A_SGPU_ELIGHT_REGS][4], greg[4];
	long n, lit = 0, mismatched = 0, samples = 0;
	long hist[4] = { 0, 0, 0, 0 };
	int worst = 0;
	char msg[256];

	if( !identity )
	{
		build_powtables( gamma, lin, scr );
		L = lin;
		S = scr;
	}
	if( !nv2a_sgpu_fit_gamma( L, S, &g ))
	{
		CHECK( 0, "elight case: gamma fit failed" );
		return;
	}
	nv2a_sgpu_elight_gamma_reg( g, greg );

	for( n = 0; n < 200000; n++ )
	{
		nv2a_sgpu_elights_t el;
		float rows[3][4], pos[3], nrm[3], color[3], world[3], len;
		uint8_t ref[3], vp[3];
		int i, c;

		random_bone( rows );
		for( c = 0; c < 3; c++ )
			pos[c] = frand() * 80.0f - 40.0f;
		for( c = 0; c < 3; c++ )
			nrm[c] = frand() * 2.0f - 1.0f;
		len = sqrtf( nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2] );
		if( len < 1e-3f )
			continue;
		for( c = 0; c < 3; c++ )
			nrm[c] /= len;
		for( c = 0; c < 3; c++ )
			color[c] = frand() * frand(); /* lightcolor * lv: dark more often than not */
		for( c = 0; c < 3; c++ )
			world[c] = pos[0] * rows[c][0] + pos[1] * rows[c][1] + pos[2] * rows[c][2] + rows[c][3];

		memset( &el, 0, sizeof( el ));
		el.count = count;
		el.flip = flip;
		for( i = 0; i < count; i++ )
		{
			float radius = muzzle ? 24.0f : 10.0f + frand() * 290.0f;
			int rgb[3] = { 255, 192, 64 };

			if( !muzzle )
				for( c = 0; c < 3; c++ )
					rgb[c] = min_rgb + (int)( frand() * ( 256 - min_rgb ));
			for( c = 0; c < 3; c++ )
			{
				el.origin[i][c] = world[c] + ( frand() * 2.0f - 1.0f ) * radius * 1.5f;
				el.color[i][c] = (float)( L ? L[rgb[c] << 2] : (uint32_t)( rgb[c] << 2 ));
			}
			el.r2[i] = radius * radius;
		}

		if( gated && !nv2a_sgpu_elights_fit( &el ))
			continue;
		nv2a_sgpu_elight_consts( &el, greg, k );
		nv2a_sgpu_ref_elight_color( L, S, &el, (const float (*)[4])rows, pos, nrm, color, ref );
		nv2a_sgpu_vp_elight_color( (const float (*)[4])k, (const float (*)[4])rows, pos, nrm, color, vp );

		{
			uint8_t plain[3];
			int changed = 0;

			for( c = 0; c < 3; c++ )
			{
				plain[c] = (uint8_t)( color[c] * 255.0f );
				if( ref[c] != plain[c] )
					changed = 1;
			}
			lit += changed;
		}
		for( c = 0; c < 3; c++ )
		{
			int d = abs( (int)ref[c] - (int)vp[c] );

			if( d > worst )
				worst = d;
			if( d )
				mismatched++;
			hist[d > 3 ? 3 : d]++;
			samples++;
		}
	}

	printf( "  elights %s: max diff %d, %.3f%% of colour bytes differ (1: %ld, 2: %ld, 3+: %ld of %ld), %ld of 200000 vertices lit\n",
		name, worst, 100.0 * mismatched / samples, hist[1], hist[2], hist[3], samples, lit );
	snprintf( msg, sizeof( msg ), "elights %s: colour bytes at most %d apart (got %d)", name, max_diff, worst );
	CHECK( worst <= max_diff, msg );
	snprintf( msg, sizeof( msg ), "elights %s: at most %.2f%% of colour bytes differ", name, max_mismatch_pct );
	CHECK( 100.0 * mismatched / samples <= max_mismatch_pct, msg );
}

static void test_elights( void )
{
	test_fit_gamma();
	CHECK( !nv2a_sgpu_elights_fit( &(nv2a_sgpu_elights_t){ .count = 3 } ), "three lights stay on the CPU" );
	test_elight_case( 2.5, 0, 0, 0, 1, 0, 1, "none (gamma 2.5)", 0, 0.0 );
	test_elight_case( 2.5, 0, 1, 0, 1, 0, 1, "one muzzle flash (gamma 2.5)", 2, 10.0 );
	test_elight_case( 2.5, 0, 2, 0, 1, 0, 1, "two muzzle flashes (gamma 2.5)", 2, 13.0 );
	test_elight_case( 2.5, 0, 2, 1, 1, 0, 1, "two muzzle flashes, flipped view model (gamma 2.5)", 2, 13.0 );
	test_elight_case( 1.8, 0, 2, 0, 1, 0, 1, "two muzzle flashes (gamma 1.8)", 2, 13.0 );
	test_elight_case( 3.0, 0, 2, 0, 1, 0, 1, "two muzzle flashes (gamma 3.0)", 2, 13.0 );
	test_elight_case( 2.5, 0, 2, 0, 0, 0, 1, "two random lights the gate passes (gamma 2.5)", 2, 13.0 );
	test_elight_case( 3.0, 0, 2, 0, 0, 0, 1, "two random lights the gate passes (gamma 3.0)", 2, 13.0 );
	test_elight_case( 1.0, 1, 2, 0, 0, 0, 1, "two random lights the gate passes (linear gamma space)", 1, 6.0 );
	/* Why the gate: without it a dark light can leave a lit vertex's sum
	   near 0, where ScreenGammaTable is steepest. Reported, not gated. */
	test_elight_case( 2.5, 0, 2, 0, 0, 0, 0, "two random lights, ungated (gamma 2.5)", 255, 100.0 );
}

/* ------------------------------------------------------------------ */
/* Chrome (divergences.md #170)                                        */
/* ------------------------------------------------------------------ */

static void random_unit( float v[3] )
{
	float len;

	do
	{
		v[0] = frand() * 2.0f - 1.0f;
		v[1] = frand() * 2.0f - 1.0f;
		v[2] = frand() * 2.0f - 1.0f;
		len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	} while( len < 0.1f || len > 1.0f );
	v[0] /= len; v[1] /= len; v[2] /= len;
}

static float dp3( const float a[3], const float b[3] )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The test's own copy of vs_chrome.vp's arithmetic, with one deliberate
   error per `mutant` (0: none). Mutant 0 must equal
   nv2a_sgpu_vp_chrome_st bit for bit, so the mutants are mutations of
   the transcription the test checks, and each must be caught. */
enum
{
	MUT_NONE,
	MUT_NO_PLUS_ONE,     /* ( n ) * 32 instead of ( n + 1 ) * 32 */
	MUT_CROSS_ORDER,     /* cross( vright, tmp ): up negated */
	MUT_NO_ORIGIN,       /* tmp from the bone origin alone */
	MUT_NO_UP_NORMALIZE, /* up not normalised (tmp unnormalised would be an equivalent mutant: both crosses are normalised after it, so its length cancels) */
	MUT_MODEL_NORMAL,    /* the model-space normal, not R n */
	MUT_SWAP_ST,         /* s from up, t from right */
	MUT_SCALE_64,        /* * 64 instead of * 32 */
	MUT_NO_FLOOR,        /* no MAX before RSQ */
	MUT_COUNT
};

static void vp_chrome_mut( const float rows[3][4], const float normal[3], const float k[NV2A_SGPU_CHROME_REGS][4],
	float ss, float ts, float out[2], int mutant )
{
	float rn[3], t[3], up[3], rt[3], l, a, b;
	const float *co = k[0], *vr = k[1];
	float floor_ = mutant == MUT_NO_FLOOR ? 0.0f : co[3];
	int i;

	for( i = 0; i < 3; i++ )
		rn[i] = mutant == MUT_MODEL_NORMAL ? normal[i] : dp3( normal, rows[i] );
	for( i = 0; i < 3; i++ )
		t[i] = mutant == MUT_NO_ORIGIN ? rows[i][3] : rows[i][3] - co[i];
	l = dp3( t, t ); l = l > floor_ ? l : floor_;
	l = 1.0f / sqrtf( l );
	t[0] *= l; t[1] *= l; t[2] *= l;
	if( mutant == MUT_CROSS_ORDER )
	{
		up[0] = vr[2] * t[1]; up[1] = vr[0] * t[2]; up[2] = vr[1] * t[0];
		up[0] = vr[1] * t[2] - up[0]; up[1] = vr[2] * t[0] - up[1]; up[2] = vr[0] * t[1] - up[2];
	}
	else
	{
		up[0] = t[2] * vr[1]; up[1] = t[0] * vr[2]; up[2] = t[1] * vr[0];
		up[0] = t[1] * vr[2] - up[0]; up[1] = t[2] * vr[0] - up[1]; up[2] = t[0] * vr[1] - up[2];
	}
	l = dp3( up, up ); l = l > floor_ ? l : floor_;
	l = 1.0f / sqrtf( l );
	if( mutant != MUT_NO_UP_NORMALIZE )
	{
		up[0] *= l; up[1] *= l; up[2] *= l;
	}
	rt[0] = up[2] * t[1]; rt[1] = up[0] * t[2]; rt[2] = up[1] * t[0];
	rt[0] = up[1] * t[2] - rt[0]; rt[1] = up[2] * t[0] - rt[1]; rt[2] = up[0] * t[1] - rt[2];
	l = dp3( rt, rt ); l = l > floor_ ? l : floor_;
	l = 1.0f / sqrtf( l );
	rt[0] *= l; rt[1] *= l; rt[2] *= l;
	a = dp3( rn, rt );
	b = dp3( rn, up );
	if( mutant == MUT_SWAP_ST )
	{
		float c = a; a = b; b = c;
	}
	if( mutant != MUT_NO_PLUS_ONE )
	{
		a += 1.0f; b += 1.0f;
	}
	a *= mutant == MUT_SCALE_64 ? 64.0f : 32.0f;
	b *= mutant == MUT_SCALE_64 ? 64.0f : 32.0f;
	out[0] = a * ss;
	out[1] = b * ts;
}

/* The CPU path for one vertex: the bone's vectors (R_StudioSetupChrome's
   cached part) then its texture coordinates. */
static void ref_chrome( const float rows[3][4], const float origin[3], const float vright[3], const float normal[3],
	float ss, float ts, float out[2] )
{
	float right[3], up[3];

	nv2a_sgpu_ref_chrome_bone( rows, origin, vright, right, up );
	nv2a_sgpu_ref_chrome_st( right, up, normal, ss, ts, out );
}

/* Counts the vertex-program instructions in a vp20compiler .inl and
   whether FLD_FINAL (dword 3, bit 0: vp20compiler main.c:164) is set on
   the last one only (r_context.c's resident layout depends on it). */
static int inl_words( const char *path, uint32_t *w, int max )
{
	FILE *f = fopen( path, "rb" );
	char line[512];
	int n = 0;

	if( !f )
		return -1;
	while( fgets( line, sizeof( line ), f ))
	{
		char *p = line;

		if( line[0] == '/' )
			continue;
		while(( p = strstr( p, "0x" )) != NULL && n < max )
		{
			w[n++] = (uint32_t)strtoul( p, &p, 16 );
		}
	}
	fclose( f );
	return n;
}

static void test_chrome_inl( void )
{
	static uint32_t w[4 * 256];
	int n, i, finals;

	n = inl_words( "ref/nv2a/vs_chrome.inl", w, 4 * 256 );
	if( n < 0 )
	{
		printf( "note: ref/nv2a/vs_chrome.inl not found (run from the repository root), skipped\n" );
		return;
	}
	finals = 0;
	for( i = 0; i < n / 4; i++ )
		finals += ( w[i * 4 + 3] & 1u ) ? 1 : 0;
	CHECK( n == 29 * 4 && finals == 1 && ( w[n - 1] & 1u ), "vs_chrome.inl: the compiler's stub and the 28-instruction tail, FLD_FINAL on the last only" );

	n = inl_words( "ref/nv2a/vs_studio.inl", w, 4 * 256 );
	finals = 0;
	for( i = 0; i < n / 4; i++ )
		finals += ( w[i * 4 + 3] & 1u ) ? 1 : 0;
	CHECK( n == 106 * 4 && finals == 1 && ( w[n - 1] & 1u ), "vs_studio.inl: 106 instructions, FLD_FINAL on the last only" );
	/* The chrome program is vs_studio's first 104 slots and the tail. */
	CHECK( 104 + 28 <= 136, "vs_studio's prefix and the chrome tail fit the 136 program slots" );
}

static void test_chrome( void )
{
	static const float sizes[][2] = { { 64, 64 }, { 128, 64 }, { 32, 32 }, { 256, 128 } };
	const long N = 200000;
	long n, over[MUT_COUNT], harness_diff = 0;
	double maxerr[MUT_COUNT];
	int m;
	char msg[256];

	test_chrome_inl();

	for( m = 0; m < MUT_COUNT; m++ )
	{
		over[m] = 0;
		maxerr[m] = 0.0;
	}

	for( n = 0; n < N; n++ )
	{
		float rows[3][4], origin[3], vright[3], normal[3], dir[3], k[NV2A_SGPU_CHROME_REGS][4], ref[2], vp[2];
		const float *sz = sizes[n % 4];
		float ss = 1.0f / sz[0], ts = 1.0f / sz[1], dist;
		int c;

		random_bone( rows );
		random_unit( normal );
		random_unit( vright );
		/* The chrome origin is the view origin: from a view model's bones
		   (a few units) to a far NPC's (thousands). */
		random_unit( dir );
		dist = n % 3 == 0 ? 1.0f + frand() * 30.0f : 30.0f + frand() * 3000.0f;
		for( c = 0; c < 3; c++ )
			origin[c] = rows[c][3] + dir[c] * dist;

		nv2a_sgpu_chrome_consts( origin, vright, k );
		ref_chrome( (const float (*)[4])rows, origin, vright, normal, ss, ts, ref );
		nv2a_sgpu_vp_chrome_st( (const float (*)[4])rows, normal, (const float (*)[4])k, ss, ts, vp );

		if( n < 1000 )
		{
			float mine[2];

			vp_chrome_mut( (const float (*)[4])rows, normal, (const float (*)[4])k, ss, ts, mine, MUT_NONE );
			if( memcmp( mine, vp, sizeof( vp )))
				harness_diff++;
		}

		for( m = 0; m < MUT_COUNT; m++ )
		{
			float out[2];
			double e;

			if( m == MUT_NONE )
			{
				out[0] = vp[0];
				out[1] = vp[1];
			}
			else
				vp_chrome_mut( (const float (*)[4])rows, normal, (const float (*)[4])k, ss, ts, out, m );
			/* in texels of the bound skin: the coordinate times its size */
			e = fabs((double)out[0] - ref[0] ) * sz[0];
			if( fabs((double)out[1] - ref[1] ) * sz[1] > e )
				e = fabs((double)out[1] - ref[1] ) * sz[1];
			if( out[0] != out[0] || out[1] != out[1] )
				e = 1e30;
			if( e > maxerr[m] )
				maxerr[m] = e;
			if( e > 1.0 / 256.0 )
				over[m]++;
		}
	}

	CHECK( harness_diff == 0, "chrome: the mutant harness without a mutation is nv2a_sgpu_vp_chrome_st, bit for bit" );

	/* The program against the reference: within 1/256 of a texel
	   everywhere (the sub-texel precision a bilinear fetch resolves), and
	   in practice far closer. */
	snprintf( msg, sizeof( msg ), "chrome: vs_chrome vs R_StudioSetupChrome on %ld random bones/normals/origins: max %.2e texel (limit 1/256)",
		N, maxerr[MUT_NONE] );
	CHECK( over[MUT_NONE] == 0, msg );
	printf( "  %s\n", msg );

	/* Every mutant is caught: wrong on a large share of the samples. */
	for( m = 1; m < MUT_COUNT; m++ )
	{
		if( m == MUT_NO_FLOOR )
			continue; /* only differs at a zero vector: below */
		snprintf( msg, sizeof( msg ), "chrome mutant %d caught (%.1f%% of samples off by more than 1/256 texel, max %.3g)",
			m, 100.0 * over[m] / N, maxerr[m] );
		CHECK( over[m] > N / 4, msg );
		printf( "  %s\n", msg );
	}

	/* A bone exactly at the chrome origin: the reference's VectorNormalize
	   leaves tmp at zero, so up = right = 0 and the coordinates are
	   ( 32, 32 ) * scale. The floored RSQ gives exactly that; without the
	   floor, NaN. */
	{
		float rows[3][4], vright[3], normal[3], k[NV2A_SGPU_CHROME_REGS][4], origin[3], ref[2], vp[2], mut[2];
		int c, ok = 1, caught = 1, t;

		for( t = 0; t < 1000; t++ )
		{
			random_bone( rows );
			random_unit( normal );
			random_unit( vright );
			for( c = 0; c < 3; c++ )
				origin[c] = rows[c][3];
			nv2a_sgpu_chrome_consts( origin, vright, k );
			ref_chrome( (const float (*)[4])rows, origin, vright, normal, 1.0f / 64.0f, 1.0f / 64.0f, ref );
			nv2a_sgpu_vp_chrome_st( (const float (*)[4])rows, normal, (const float (*)[4])k, 1.0f / 64.0f, 1.0f / 64.0f, vp );
			vp_chrome_mut( (const float (*)[4])rows, normal, (const float (*)[4])k, 1.0f / 64.0f, 1.0f / 64.0f, mut, MUT_NO_FLOOR );
			if( ref[0] != 0.5f || ref[1] != 0.5f )
				ok = 0;
			if( vp[0] != ref[0] || vp[1] != ref[1] )
				ok = 0;
			if( mut[0] == mut[0] && mut[1] == mut[1] )
				caught = 0;
		}
		CHECK( ok, "chrome: a bone at the chrome origin gets the reference's (32, 32) * scale exactly" );
		CHECK( caught, "chrome mutant: without the floor, a bone at the chrome origin is NaN" );
	}

	/* c[94..95] and their slots: the last two hardware constants, clear of
	   the palette and the local lights. */
	{
		float k[NV2A_SGPU_CHROME_REGS][4];
		const float o[3] = { 1, 2, 3 }, r[3] = { 4, 5, 6 };

		nv2a_sgpu_chrome_consts( o, r, k );
		CHECK( k[0][0] == 1 && k[0][2] == 3 && k[0][3] == NV2A_SGPU_CHROME_TINY && k[1][1] == 5 && k[1][3] == 0,
			"chrome consts: ( origin, floor ), ( vright, 0 )" );
		CHECK( NV2A_SGPU_C_CHROME == 94u && NV2A_SGPU_HW_C0 + NV2A_SGPU_C_CHROME + NV2A_SGPU_CHROME_REGS == 192u
			&& NV2A_SGPU_C_ELIGHT + NV2A_SGPU_ELIGHT_REGS == NV2A_SGPU_C_CHROME,
			"chrome consts at c[94..95], hardware 190..191, right after the local lights" );
	}
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
	test_elights();
	test_chrome();

	for( i = 1; i < argc; i++ )
		test_model( argv[i] );

	printf( "nv2a_studio_gpu: %d/%d checks passed\n", total - fails, total );
	return fails ? 1 : 0;
}
