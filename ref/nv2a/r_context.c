/*
r_context.c -- ref/nv2a bring-up (X9): a minimal ref_interface_t whose only
real job is proving pbkit can drive the NV2A through the actual engine
ref-module path, not just tools/x4-triangle's standalone program.

Every stub function below is copied verbatim in shape from ref/null/
r_context.c (upstream's own "run full client in headless environments"
module, and the proven precedent for "the whole ref_interface_t table can
be satisfied with trivial stubs plus a real R_Init_Video call"). Only
R_Init, R_Shutdown and R_EndFrame do real work: R_Init brings up pbkit and
uploads the shader/texture/vertex data, R_EndFrame draws one fixed
textured triangle every frame and presents it. The init/per-frame sequence
- pb_init, the vp20 program upload loop, the fp20 pixel-shader block, the
texture-stage register values, pb_wait_for_vbl/reset/target_back_buffer/
erase_depth_stencil_buffer/pb_finished/pb_show_front_screen - is
tools/x4-triangle/main.c's own already-proven sequence, unmodified in
shape; see that file's own citations for where each piece originally came
from (deps/nxdk/samples/triangle and deps/nxdk/samples/mesh).

Deliberately out of scope for this bring-up (see docs/r3d/fork-plan.md's
X9 section and docs/r3d/divergences.md for the rest of the story): no
MPAK-derived geometry, no register combiner stages beyond the single one
X4 already proved (a plain textured triangle, no lighting), no VP20
skinning, no swizzled textures - X4's own open defect (SZ_A8R8G8B8 renders
as a flat fill of texel (0,0), root cause unresolved) stays untouched, so
this module uses ONLY the linear texture path X4 already proved correct
under xemu.
*/

#include <math.h>
#include <string.h>
#include "xash3d_types.h"
#include "const.h"
#include "cvardef.h"
#include "ref_common.h"
#include "mpak_layout.h" /* filesystem/mpak_layout.h - types/constants only, see
                            nv2a_compute_mpak_layout()'s own comment for why the
                            offset-COMPUTING function body still needs its own
                            copy here rather than calling filesystem's real
                            Mpak_ComputeLayout() directly. */

#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

/* gEngfuncs/gpGlobals and GetRefAPI itself are ref/common/ref_context.c's
   job (it does `*funcs = gReffuncs;` against the plain, non-static table
   below) - xbox-ref-isolate.sh compiles ref/common alongside every module,
   ref/soft included, and ref/soft/r_context.c follows this exact same
   shape (a gReffuncs table only, no GetRefAPI of its own). An earlier
   version of this file copied ref/null's fully self-contained style
   instead (its own GetRefAPI + static gEngfuncs/gpGlobals) - ref/null
   never links ref/common at all (its own wscript's `use` list omits it),
   so that shape doesn't apply here; it produced a real duplicate-symbol
   link error (_nv2a_GetRefAPI defined by both this file and ref/common/
   ref_context.c) the first time this module was linked next to ref/soft. */

/*
========================================================================
Bring-up render state: one fixed textured triangle, tools/x4-triangle's
own vertex layout, checkerboard texture and vp20/fp20 shader pair.
========================================================================
*/

typedef struct
{
	float pos[3];
	float uv[2];
} __attribute__(( packed )) nv2a_test_vertex_t;

/* X9-C: the real map-geometry vertex format, adding a second UV pair for
   the lightmap stage. Position + diffuse UV stay first and in the SAME
   layout as nv2a_test_vertex_t on purpose - attribute slots 0 (position)
   and 9 (diffuse UV) are set up identically for both vertex formats, only
   the new slot 10 (lightmap UV, ref/nv2a/vs_lit.vs.cg's own TEXCOORD1 -
   confirmed by a real vp20compiler run to map there, not assumed) is new. */
typedef struct
{
	float pos[3];
	float uv0[2];
	float uv1[2];
} __attribute__(( packed )) nv2a_lit_vertex_t;

/* Same triangle and checkerboard tools/x4-triangle/main.c already proved
   renders correctly under xemu (its own X4_TEXTURE_SWIZZLED=0 path). */
static const nv2a_test_vertex_t nv2a_test_verts[] =
{
	//  X     Y     Z       U     V
	{{ -1.0f, -1.0f, 1.0f }, { 0.0f, 1.0f }},
	{{  0.0f,  1.0f, 1.0f }, { 0.5f, 0.0f }},
	{{  1.0f, -1.0f, 1.0f }, { 1.0f, 1.0f }},
};

#define NV2A_TEST_TEX_SIZE_LOG2 3
#define NV2A_TEST_TEX_SIZE ( 1 << NV2A_TEST_TEX_SIZE_LOG2 ) /* 8x8 */
#define NV2A_TEST_MAXRAM 0x03FFAFFF

#define MASK( mask, val ) ( ( ( val ) << ( __builtin_ffs( mask ) - 1 ) ) & ( mask ) )

static qboolean  nv2a_ready;
static uint32_t *nv2a_vb;
static void     *nv2a_tex;
static float     nv2a_viewport[4][4];
static int       nv2a_width, nv2a_height;

/* X9-B/C drew one hard-coded real face from a real baked map, instead of
   the fixed test triangle, once a map's own .mpak data was loaded. X9-D
   scales this to the whole map's own real geometry - the baked index
   buffer is already one flat, contiguous list of every face's own
   triangles concatenated in bake order (confirmed reading
   tools/asset-studio/src/formats/PakManager.cpp's own write sequence), so
   drawing all of it needs no per-FaceDrawCmd lookup at all; there is no
   longer a fixed vertex/index cap sized for one test face either - both
   buffers below are allocated at their real, per-map size. */
static qboolean  nv2a_map_ready;
static const model_t *nv2a_map_owner; /* which model_t's own Mod_ProcessRenderData(create=true)
                                          populated the state below - Mod_ProcessRenderData is
                                          called for EVERY model, world and non-world alike
                                          (engine/common/model.c), not just the map; without
                                          this, the first studio/sprite model loaded right after
                                          the world would wipe it on its own unrelated unload
                                          (codex review, real finding). */
static uint32_t *nv2a_map_vb;                       /* GPU-visible, nv2a_test_vertex_t[nv2a_map_vert_count] */
static uint32_t  nv2a_map_vert_count;
/* pbkit's own indexed-draw method (deps/nxdk/samples/mesh/main.c's
   draw_indices()) pushes index values inline into the pushbuffer as
   dwords - there is no separate GPU-resident index buffer to allocate,
   unlike the vertex array and the atlas below. Each dword packs TWO
   16-bit indices (deps/nxdk/samples/mesh/verts.h's own data - e.g.
   0x00010002 is indices {2, 1}, low half first) - codex review found an
   earlier version submitted one raw 32-bit "index" per dword instead,
   which silently zeroed every odd-positioned index's own upper 16 bits
   at the GPU. nv2a_map_dword_count counts DWORDS, not logical indices -
   an odd logical index count pads its final dword by repeating the last
   real index (a harmless degenerate triangle, not corrupt data). */
static uint32_t *nv2a_map_index_dwords;             /* Ordinary engine-pooled CPU memory (r_temppool,
                                                        Mem_Malloc/Mem_Free) - unlike nv2a_map_vb this
                                                        is only ever CPU-read into the pushbuffer, never
                                                        touched by the GPU directly, so it has no reason
                                                        to cost scarce contiguous write-combined memory
                                                        (codex review, real finding). Packed pairs,
                                                        sized to the real map's own iCount, not a fixed
                                                        bring-up cap. */
static uint32_t  nv2a_map_dword_count;
static void     *nv2a_map_atlas;
static uint32_t  nv2a_map_atlas_width, nv2a_map_atlas_height, nv2a_map_atlas_pitch;
static void     *nv2a_map_lightmap;     /* GPU-visible, RGBA8, no format conversion needed -
                                            LU_IMAGE_A8R8G8B8 is the same proven-working
                                            uncompressed linear format X9-A's own checkerboard
                                            already used, just bound on texture stage 1 */
static uint32_t  nv2a_map_lightmap_width, nv2a_map_lightmap_height, nv2a_map_lightmap_pitch;
static float     nv2a_map_viewproj[4][4];

static void nv2a_matrix_viewport( float out[4][4], float x, float y, float width, float height, float z_min, float z_max )
{
	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = width / 2.0f;
	out[1][1] = height / -2.0f;
	out[2][2] = z_max - z_min;
	out[3][3] = 1.0f;
	out[3][0] = x + width / 2.0f;
	out[3][1] = y + height / 2.0f;
	out[3][2] = z_min;
}

/*
========================================================================
X9-B: real baked geometry, one real face.

tools/x4-triangle/vs.vs.cg's own shader (ref/nv2a/vs.inl) does exactly one
thing: `position = mul(float4(pos.xyz,1), m_viewport); position.xyz /=
position.w;` - a single row-vector matrix multiply plus the perspective
divide already baked in. nv2a_matrix_viewport() above is the flat,
w-always-1 NDC-to-screen half of that for the X9-A test triangle (whose
own vertices are hand-authored already inside [-1,1]). Real MPAK world
vertices need a real view + perspective transform PREPENDED before that
same NDC-to-screen matrix - no shader change needed, just a different
4x4 constant, so this is real (if minimal) camera math, not a hack.
========================================================================
*/

typedef struct { float x, y, z; } nv2a_vec3_t;

static nv2a_vec3_t nv2a_vec3_sub( nv2a_vec3_t a, nv2a_vec3_t b )
{
	nv2a_vec3_t r = { a.x - b.x, a.y - b.y, a.z - b.z };
	return r;
}

static nv2a_vec3_t nv2a_vec3_cross( nv2a_vec3_t a, nv2a_vec3_t b )
{
	nv2a_vec3_t r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	return r;
}

static float nv2a_vec3_dot( nv2a_vec3_t a, nv2a_vec3_t b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static nv2a_vec3_t nv2a_vec3_normalize( nv2a_vec3_t a )
{
	float len = sqrtf( nv2a_vec3_dot( a, a ));
	nv2a_vec3_t r = { 0 };
	if( len > 0.0001f )
	{
		r.x = a.x / len;
		r.y = a.y / len;
		r.z = a.z / len;
	}
	return r;
}

/* result = a * b (row-vector convention: v' = v * result = v * a * b). */
static void nv2a_matrix_multiply( float result[4][4], const float a[4][4], const float b[4][4] )
{
	int i, j, k;
	float tmp[4][4];

	for( i = 0; i < 4; i++ )
	{
		for( j = 0; j < 4; j++ )
		{
			float sum = 0.0f;
			for( k = 0; k < 4; k++ )
				sum += a[i][k] * b[k][j];
			tmp[i][j] = sum;
		}
	}
	memcpy( result, tmp, sizeof( tmp ));
}

/* Standard left-handed row-vector LookAt (matches D3DXMatrixLookAtLH) -
   eye/target/up in world space, up need not be exactly orthogonal to the
   view direction. */
static void nv2a_matrix_lookat( float out[4][4], nv2a_vec3_t eye, nv2a_vec3_t target, nv2a_vec3_t up )
{
	nv2a_vec3_t zaxis = nv2a_vec3_normalize( nv2a_vec3_sub( target, eye ));
	nv2a_vec3_t xaxis = nv2a_vec3_normalize( nv2a_vec3_cross( up, zaxis ));
	nv2a_vec3_t yaxis = nv2a_vec3_cross( zaxis, xaxis );

	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = xaxis.x; out[1][0] = xaxis.y; out[2][0] = xaxis.z;
	out[0][1] = yaxis.x; out[1][1] = yaxis.y; out[2][1] = yaxis.z;
	out[0][2] = zaxis.x; out[1][2] = zaxis.y; out[2][2] = zaxis.z;
	out[3][0] = -nv2a_vec3_dot( xaxis, eye );
	out[3][1] = -nv2a_vec3_dot( yaxis, eye );
	out[3][2] = -nv2a_vec3_dot( zaxis, eye );
	out[3][3] = 1.0f;
}

/* Standard left-handed row-vector perspective projection (matches
   D3DXMatrixPerspectiveFovLH): outputs NDC x,y in [-1,1] and z in [0,1]
   (near=0, far=1) in the SAME convention nv2a_matrix_viewport() above
   already expects as ITS OWN input - chaining lookat * perspective *
   nv2a_matrix_viewport reuses that function completely unchanged.
   fovYRadians/aspect/zNear/zFar are all compile-time constants for this
   bring-up (a fixed debug camera), so no runtime tan() is needed - the
   caller passes a precomputed yScale. */
static void nv2a_matrix_perspective( float out[4][4], float yScale, float aspect, float zNear, float zFar )
{
	memset( out, 0, 4 * 4 * sizeof( float ));
	out[0][0] = yScale / aspect;
	out[1][1] = yScale;
	out[2][2] = zFar / ( zFar - zNear );
	out[2][3] = 1.0f;
	out[3][2] = -zNear * zFar / ( zFar - zNear );
}

/*
========================================================================
X9-B: a private, narrowly-scoped mirror of filesystem/mpak_layout.c's own
Mpak_ComputeLayout() - NOT a call to it. ref/ and filesystem/ are separate
isolated static-lib units (tools/xbox-ref-isolate.sh), each only reachable
from the engine through its own function-pointer API (ref_api_t /
GetFSAPI), never directly from each other - so this module cannot link
against filesystem/mpak_layout.c's real function body, only #include its
header for the shared type/constant definitions (mpak_header_t,
mpak_vertex_t, MPAK_FLAG_NV2A_BLOCK, MPAK_SECTOR_SIZE, ...), which cost
nothing to reuse since they carry no linkage.

This mirror walks every section in the SAME order for the SAME reason the
real one does (no offset table - byte order IS the format), but only
CAPTURES the ones this slice actually reads (vertices, indices, atlas,
lightmapAtlas); every other section (FaceDrawCmd included - X9-D draws
the whole shared index buffer directly and no longer needs any per-face
lookup) still has to be stepped over correctly to reach the ones that
matter. Verified independently against a real baked archive
(crossfire.mpak, nv2a profile) by hand in Python before writing this - the
final cursor landed exactly on the real file size and the computed
offsets read back real, sane vertex/index data - see
docs/r3d/divergences.md's X9-B/X9-D entries. Deliberately simpler than the
real Mpak_ComputeLayout(): no fixed-record-size divisibility checks, no
overflow-hardened lightmap cross-validation - a corrupted archive fails a
bounds check here and this function returns false, not a crash, but the
diagnostics are coarser than the real implementation's. If mpak_layout.c's
own section order or record sizes ever change, this must change with it -
there is no compiler-enforced link between the two copies. */
typedef struct
{
	mpak_lump_t vertices;
	mpak_lump_t indices;
	mpak_lump_t atlas;
	mpak_lump_t lightmapAtlas;
} nv2a_mpak_geometry_t;

static qboolean nv2a_mpak_place( fs_offset_t fileSize, fs_offset_t *cursor, uint64_t length, mpak_lump_t *out )
{
	uint64_t padded;

	out->offset = *cursor;
	out->length = 0;

	if( length == 0 )
		return true;

	padded = (( length + ( MPAK_SECTOR_SIZE - 1 )) / MPAK_SECTOR_SIZE ) * MPAK_SECTOR_SIZE;
	if( *cursor < 0 || padded > (uint64_t)( fileSize - *cursor ))
		return false;

	out->length = (fs_offset_t)length;
	*cursor += (fs_offset_t)padded;
	return true;
}

static qboolean nv2a_compute_mpak_layout( const mpak_header_t *header, fs_offset_t fileSize, nv2a_mpak_geometry_t *out )
{
	fs_offset_t cursor = MPAK_SECTOR_SIZE; /* the header itself always occupies one full sector */
	mpak_lump_t skip;

	memset( out, 0, sizeof( *out ));

	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->vCount * sizeof( mpak_vertex_t ), &out->vertices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, (uint64_t)header->iCount * sizeof( uint16_t ), &out->indices )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenVisibility, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenNodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLeaves, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenMarksurfaces, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenFaceDrawCmds, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenPlanes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenClipnodes, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenModels, &skip )) return false;

	if( !( header->flags & MPAK_TARGET_XBOX_FLAG ))
		return false; /* PC-target archive - no atlas lump at all, nothing this slice can draw */

	if( !nv2a_mpak_place( fileSize, &cursor, header->lenAtlas, &out->atlas )) return false;

	/* X9-C: continue the walk far enough to reach lightmapAtlas - assetDict
	   and assetData are always empty under X2's own narrowed scope
	   (MpakHeader.h's own note) but still real cursor steps to skip if a
	   stale pre-narrowing archive ever declares them non-empty. */
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenAssetDict, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenAssetData, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenEntities, &skip )) return false;
	if( !nv2a_mpak_place( fileSize, &cursor, header->lenLightmapAtlas, &out->lightmapAtlas )) return false;

	return true;
}

/* tools/x4-triangle/main.c's own init_shader(), unmodified in shape - the
   vp20 program upload loop plus the fp20 pixel-shader block, both
   pre-compiled headers copied from that same directory (ref/nv2a/vs.inl,
   ref/nv2a/ps.inl - see those files for the cgc/vp20compiler/fp20compiler
   provenance). */
static void nv2a_init_shader( void )
{
	uint32_t *p;
	int i;

	uint32_t vs_program[] =
	{
		#include "vs.inl"
	};

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_START, 0 );
	p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
		| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0 );
	pb_end( p );

	for( i = 0; i < (int)( sizeof( vs_program ) / 16 ); i++ )
	{
		p = pb_begin();
		pb_push( p++, NV097_SET_TRANSFORM_PROGRAM, 4 );
		memcpy( p, &vs_program[i * 4], 4 * 4 );
		p += 4;
		pb_end( p );
	}

	p = pb_begin();
	#include "ps.inl"
	pb_end( p );
}

/* X9-C: the diffuse x lightmap x 2 program pair (ref/nv2a/vs_lit.vs.cg,
   ps_lit.ps.cg - both real, freshly-authored Cg sources compiled through
   the same cgc/vp20compiler/fp20compiler pipeline vs.inl/ps.inl already
   were, verified reproducible before being committed). The NV2A only
   holds one active vertex/pixel program at a time, so whichever draw path
   runs this frame re-uploads its OWN program right before drawing rather
   than both programs staying resident together - simple and correct for
   a bring-up slice with only two draw paths, at the cost of a small,
   irrelevant-for-correctness re-upload every frame. Otherwise identical
   in shape to nv2a_init_shader() above. */
static void nv2a_init_lit_shader( void )
{
	uint32_t *p;
	int i;

	uint32_t vs_program[] =
	{
		#include "vs_lit.inl"
	};

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_START, 0 );
	p = pb_push1( p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM )
		| MASK( NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV ));
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0 );
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0 );
	pb_end( p );

	for( i = 0; i < (int)( sizeof( vs_program ) / 16 ); i++ )
	{
		p = pb_begin();
		pb_push( p++, NV097_SET_TRANSFORM_PROGRAM, 4 );
		memcpy( p, &vs_program[i * 4], 4 * 4 );
		p += 4;
		pb_end( p );
	}

	p = pb_begin();
	#include "ps_lit.inl"
	pb_end( p );
}

/* tools/x4-triangle/main.c's own init_texture(), LINEAR PATH ONLY - no
   swizzled copy, since that path is X4's own open, unresolved defect
   (see this file's header comment). Returns false (leaving nv2a_tex NULL)
   on allocation failure - a 64 MiB unified-memory console can genuinely
   run out of contiguous pages, and this must fail R_Init cleanly rather
   than memcpy into a null pointer (found in codex review). */
static qboolean nv2a_init_texture( void )
{
	uint32_t linear[NV2A_TEST_TEX_SIZE * NV2A_TEST_TEX_SIZE];
	int x, y;

	for( y = 0; y < NV2A_TEST_TEX_SIZE; y++ )
	{
		for( x = 0; x < NV2A_TEST_TEX_SIZE; x++ )
		{
			int checker = ( x ^ y ) & 1;
			/* A8R8G8B8: opaque white / opaque HEV orange */
			linear[y * NV2A_TEST_TEX_SIZE + x] = checker ? 0xFFFFFFFFu : 0xFFFF8000u;
		}
	}

	nv2a_tex = MmAllocateContiguousMemoryEx( sizeof( linear ), 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_tex )
		return false;

	memcpy( nv2a_tex, linear, sizeof( linear ));
	return true;
}

static void nv2a_set_attrib_pointer( unsigned int index, unsigned int format, unsigned int size, unsigned int stride, const void *data )
{
	uint32_t *p = pb_begin();
	p = pb_push1( p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format )
		| MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size )
		| MASK( NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride ));
	p = pb_push1( p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4, (uint32_t)data & 0x03ffffff );
	pb_end( p );
}

static void nv2a_draw_test_triangle( void )
{
	uint32_t *p;
	uint32_t tex_phys;
	int i;

	/* X9-C: the NV2A only holds one resident vertex/pixel program at a
	   time, and nv2a_draw_map_geometry() may have uploaded its own
	   two-texcoord/two-texture program since this path last ran - re-load
	   this path's own single-texture program every time it draws, matching
	   the same on-demand-upload discipline the lit path uses. */
	nv2a_init_shader();

	/* Texture stage 0: 8x8 A8R8G8B8, clamped, bilinear, LINEAR unswizzled
	   (LU_IMAGE_A8R8G8B8) - see tools/x4-triangle/main.c's own citations
	   for where each register value below comes from. */
	p = pb_begin();
	tex_phys = (uint32_t)nv2a_tex & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, ( NV2A_TEST_TEX_SIZE * 4 ) << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( NV2A_TEST_TEX_SIZE << 16 ) | NV2A_TEST_TEX_SIZE );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, 0x04074000 );
	pb_end( p );

	/* Shader constants: the same fixed viewport transform every frame. */
	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 16 );
	memcpy( p, nv2a_viewport, 16 * 4 );
	p += 16;
	pb_end( p );

	p = pb_begin();
	pb_push( p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
	for( i = 0; i < 16; i++ )
		*( p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	pb_end( p );

	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_test_vertex_t ), &nv2a_vb[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_test_vertex_t ), &nv2a_vb[3] );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
	p = pb_push1( p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK( NV097_DRAW_ARRAYS_COUNT, ( ARRAYSIZE( nv2a_test_verts ) - 1 ))
		| MASK( NV097_DRAW_ARRAYS_START_INDEX, 0 ));
	p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
	pb_end( p );

	pb_print( "ref_nv2a bring-up\n" );
	pb_draw_text_screen();
}

/* X9-B: the real geometry path - same VP20 transform shader and the same
   single-texture-stage fp20 combiner X9-A already proved (no shader or
   combiner changes this slice), a real DXT1 atlas instead of the 8x8
   linear checkerboard, and an indexed draw over real baked vertices
   instead of X4's own plain sequential NV097_DRAW_ARRAYS. Batches indices
   in groups of <=120 (deps/nxdk/samples/mesh/main.c's own
   NV20_TCL_PRIMITIVE_3D_INDEX_DATA limit) - written as the real, general
   batching loop from the start rather than a one-batch shortcut, which is
   what let X9-D scale this same function to a whole map's own dword count
   with no changes here at all. */
static void nv2a_draw_map_geometry( void )
{
	uint32_t *p;
	uint32_t tex_phys;
	uint32_t i;

	/* This path's own two-texcoord/two-texture program, re-uploaded every
	   time it draws for the same reason nv2a_draw_test_triangle() now
	   re-uploads its own - see that function's own comment. */
	nv2a_init_lit_shader();

	/* Texture stage 0: the real DXT1 diffuse atlas, unchanged from X9-B. */
	p = pb_begin();
	tex_phys = (uint32_t)nv2a_map_atlas & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1, nv2a_map_atlas_pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT, ( nv2a_map_atlas_width << 16 ) | nv2a_map_atlas_height );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER, 0x04074000 );
	pb_end( p );

	/* X9-C: texture stage 1, the real RGBA8 lightmap - the SAME proven
	   uncompressed-linear path X9-A's own checkerboard used on stage 0,
	   just at stage 1's own register offset (+0x40 per stage, tools/
	   x4-triangle/main.c's own documented per-stage addressing), and a
	   DIFFERENT color format (codex review, TWO rounds, real finding both
	   times - the first fix was itself wrong): the baker writes each
	   texel bytewise as R,G,B,A in memory (tools/asset-studio/src/
	   formats/BspParser.cpp's writeBlock(), `page.rgba[destOffset+0..3] =
	   R,G,B,A`). X9-A's own checkerboard uses LU_IMAGE_A8R8G8B8 correctly
	   because it writes native uint32_t INTEGER literals (0xFFFFFFFFu
	   etc.) matching that format's own packed-integer convention
	   (verified against the checkerboard's own "opaque HEV orange" value,
	   0xFFFF8000 -> A=FF,R=FF,G=80,B=00, a real orange), not raw bytes.
	   For a raw LITTLE-ENDIAN byte buffer, the format name has to match
	   the INTEGER that byte order forms when read as a little-endian
	   word, not the byte order itself directly: [R,G,B,A] in memory reads
	   as the word 0xAABBGGRR (byte0=R is the LOW byte), which is
	   LU_IMAGE_A8B8G8R8's own name (A=MSB..R=LSB), NOT LU_IMAGE_R8G8B8A8
	   (round 1's own fix - wrong, corrected in round 2: that name would
	   match memory order [A,B,G,R], not this buffer's real [R,G,B,A]). */
	p = pb_begin();
	tex_phys = (uint32_t)nv2a_map_lightmap & 0x03ffffff;
	p = pb_push1( p, NV097_SET_TEXTURE_OFFSET + 0x40, tex_phys );
	p = pb_push1( p, NV097_SET_TEXTURE_FORMAT + 0x40,
		MASK( NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2 )
		| MASK( NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 )
		| MASK( NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1 ));
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL1 + 0x40, nv2a_map_lightmap_pitch << 16 );
	p = pb_push1( p, NV097_SET_TEXTURE_IMAGE_RECT + 0x40, ( nv2a_map_lightmap_width << 16 ) | nv2a_map_lightmap_height );
	p = pb_push1( p, NV097_SET_TEXTURE_ADDRESS + 0x40, 0x00030303 );
	p = pb_push1( p, NV097_SET_TEXTURE_CONTROL0 + 0x40, 0x4003ffc0 );
	p = pb_push1( p, NV097_SET_TEXTURE_FILTER + 0x40, 0x04074000 );
	pb_end( p );

	p = pb_begin();
	p = pb_push1( p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 );
	pb_push( p++, NV097_SET_TRANSFORM_CONSTANT, 16 );
	memcpy( p, nv2a_map_viewproj, 16 * 4 );
	p += 16;
	pb_end( p );

	p = pb_begin();
	pb_push( p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16 );
	for( i = 0; i < 16; i++ )
		*( p++ ) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	pb_end( p );

	nv2a_set_attrib_pointer( 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[0] );
	nv2a_set_attrib_pointer( 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[3] );
	/* Lightmap UV, attribute slot 10 - vs_lit.vs.cg's own TEXCOORD1,
	   confirmed by a real vp20compiler run (not assumed) to map there. */
	nv2a_set_attrib_pointer( 10, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof( nv2a_lit_vertex_t ), &nv2a_map_vb[5] );

	for( i = 0; i < nv2a_map_dword_count; )
	{
		uint32_t batch = nv2a_map_dword_count - i;
		if( batch > 120 )
			batch = 120;

		p = pb_begin();
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES );
		pb_push( p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, batch );
		memcpy( p, &nv2a_map_index_dwords[i], batch * sizeof( uint32_t ));
		p += batch;
		p = pb_push1( p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END );
		pb_end( p );

		i += batch;
	}

	pb_print( "ref_nv2a: real baked geometry\n" );
	pb_draw_text_screen();
}

/*
========================================================================
ref_interface_t
========================================================================
*/

static void R_SimpleStub( void )
{
	;
}

static void R_SimpleStubInt( int unused )
{
	;
}

static void R_SimpleStubUInt( unsigned int unused )
{
	;
}

static void R_SimpleStubBool( qboolean unused )
{
	;
}

static qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw )
{
	return false;
}

static void R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw )
{
	;
}

static void R_GetDetailScaleForTexture( int texture, float *xScale, float *yScale )
{
	if( xScale ) *xScale = 1.0f;
	if( yScale ) *yScale = 1.0f;
}

static void R_SetDetailScaleForTexture( int texture, float xScale, float yScale )
{
	;
}

static void R_FillRenderAPI( render_api_t *api )
{
	;
}

static void R_FillTriAPI( triangleapi_t *api )
{
	;
}

/* Bring-up diagnostic only - not part of the ref_interface_t contract.
   Read directly out of guest memory (mem <addr> 4) via the module's own
   isolated symbol name (_nv2a_debug_marker / _nv2a_debug_pb_status in the
   XBE's own .map) to see exactly how far R_Init got without needing a
   live pad or the cursor/mon/regs debug commands this project's own
   tooling has repeatedly found unreliable. Values: 1 = entered R_Init,
   2 = R_Init_Video returned false, 3 = R_Init_Video returned true,
   4 = pb_init failed (debug_pb_status holds its status code), 6 = texture
   allocation failed, 7 = vertex buffer allocation failed, 5 = fully
   initialised. */
uint32_t debug_marker;
int32_t  debug_pb_status;

static qboolean pb_initialized;

/* One cleanup path for every failure branch below, and for normal
   shutdown - matching ref/soft's own unconditional R_Free_Video discipline
   (r_main.c:1351-1398) rather than gating it on how far init got (a real
   gap codex review found: the first version skipped R_Free_Video entirely
   on an R_Init_Video failure, and skipped it in R_Shutdown unless nv2a_ready
   was already true). pb_kill() is still gated on pb_initialized specifically,
   since pbkit's own contract has no documented "safe to call without a
   matching pb_init" guarantee. */
static void nv2a_free_resources( void )
{
	if( nv2a_vb )  { MmFreeContiguousMemory( nv2a_vb );  nv2a_vb  = NULL; }
	if( nv2a_tex ) { MmFreeContiguousMemory( nv2a_tex ); nv2a_tex = NULL; }
}

/* X9-B's own map-geometry resources - separate from the above because they
   come and go with map loads/unloads (Mod_ProcessRenderData), not with
   R_Init/R_Shutdown, though R_Shutdown must still free them if a map is
   loaded when the renderer itself is torn down. */
static void nv2a_free_map_resources( void )
{
	nv2a_map_ready = false;
	nv2a_map_owner = NULL;
	nv2a_map_vert_count = 0;
	nv2a_map_dword_count = 0;
	if( nv2a_map_vb )            { MmFreeContiguousMemory( nv2a_map_vb );            nv2a_map_vb            = NULL; }
	if( nv2a_map_index_dwords )  { Mem_Free( nv2a_map_index_dwords );                nv2a_map_index_dwords  = NULL; }
	if( nv2a_map_atlas )         { MmFreeContiguousMemory( nv2a_map_atlas );         nv2a_map_atlas         = NULL; }
	if( nv2a_map_lightmap )      { MmFreeContiguousMemory( nv2a_map_lightmap );      nv2a_map_lightmap      = NULL; }
}

static qboolean R_Init( void )
{
	qboolean video_ok;

	debug_marker = 1;

	/* Codex review, X9-D: nv2a_map_index_dwords (CPU-only, see its own
	   declaration comment) belongs in ordinary engine-pooled memory, not
	   contiguous GPU-visible memory - r_temppool is the same real,
	   engine-backed allocator ref/soft and ref/gl already use for their
	   own CPU-side data (ref/soft/r_main.c, ref/gl/gl_opengl.c). */
	r_temppool = Mem_AllocPool( "ref_nv2a zone" );

	video_ok = gEngfuncs.R_Init_Video( REF_SOFTWARE );
	debug_marker = video_ok ? 3 : 2;

	if( !video_ok )
	{
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	/* A nonzero pb_init() is NOT reliably a clean no-op on this exact
	   nxdk version (codex review, real finding, deliberately not
	   "fixed" here): pb_init's own internal failure paths
	   (deps/nxdk/lib/pbkit/pbkit.c ~2297-2366) can return -2/-3 after
	   allocating some but not all of its own contiguous buffers without
	   freeing them, and -5 after installing the GPU interrupt handler
	   and writing GPU registers, all without calling pb_kill() itself.
	   Calling pb_kill() here regardless would be wrong (codex's own
	   point): pbkit has no documented contract for "safe to tear down
	   after a specific negative return", so guessing at extra cleanup
	   risks tearing down state pbkit itself never finished setting up.
	   The correct fix is in nxdk (pb_init made atomic on every failure
	   path), tracked as a real, deferred limitation - see
	   docs/r3d/divergences.md #88 - not attempted in this vendored
	   dependency for a bring-up milestone. */
	if(( debug_pb_status = pb_init()) != 0 )
	{
		debug_marker = 4;
		gEngfuncs.Con_Printf( "ref_nv2a: pb_init failed (%d)\n", debug_pb_status );
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}
	pb_initialized = true;

	pb_show_front_screen();

	nv2a_width  = pb_back_buffer_width();
	nv2a_height = pb_back_buffer_height();

	nv2a_init_shader();

	if( !nv2a_init_texture() )
	{
		debug_marker = 6;
		pb_kill();
		pb_initialized = false;
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}

	nv2a_vb = MmAllocateContiguousMemoryEx( sizeof( nv2a_test_verts ), 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_vb )
	{
		/* pb_kill() before freeing nv2a_tex, matching R_Shutdown's own
		   ordering - no frame has been submitted yet at this point in
		   R_Init, so there is no in-flight GPU reference to race here,
		   but keeping one consistent teardown order avoids relying on
		   that reasoning holding at every call site forever. */
		debug_marker = 7;
		pb_kill();
		pb_initialized = false;
		nv2a_free_resources();
		Mem_FreePool( &r_temppool );
		gEngfuncs.R_Free_Video();
		return false;
	}
	memcpy( nv2a_vb, nv2a_test_verts, sizeof( nv2a_test_verts ));

	nv2a_matrix_viewport( nv2a_viewport, 0, 0, (float)nv2a_width, (float)nv2a_height, 0, 65536.0f );

	debug_marker = 5;
	nv2a_ready = true;
	return true;
}

static void R_Shutdown( void )
{
	nv2a_ready = false;

	/* pb_kill() FIRST: it starts the pushbuffer queue and waits for the
	   GPU to go idle before returning (deps/nxdk/lib/pbkit/pbkit.c's own
	   teardown), and R_EndFrame's own pb_finished() swap is asynchronous -
	   freeing the vertex/texture pages first could let a still-in-flight
	   frame have the NV2A fetch from memory already handed back (found in
	   codex review). Only free them once the GPU is provably idle. */
	if( pb_initialized )
	{
		pb_kill();
		pb_initialized = false;
	}

	nv2a_free_resources();
	nv2a_free_map_resources();
	Mem_FreePool( &r_temppool );

	gEngfuncs.R_Free_Video();
}

static const char *R_GetConfigName( void )
{
	return "ref_nv2a";
}

static qboolean R_SetDisplayTransform( ref_screen_rotation_t rotate, int x, int y, float scale_x, float scale_y )
{
	return true;
}

static void GL_SetupAttributes( int safegl )
{
	;
}

static qboolean R_AddEntity( struct cl_entity_s *clent, int type )
{
	return true;
}

static void R_ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	;
}

static const byte *R_GetTextureOriginalBuffer( unsigned int idx )
{
	return NULL;
}

static int GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	return 0;
}

static void GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	;
}

static void R_SetupSky( int *skytextures )
{
	;
}

static void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	;
}

static void FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	;
}

static int WorldToScreen( const vec3_t world, vec3_t screen )
{
	VectorClear( screen );
	return 0;
}

static qboolean VID_ScreenShot( const char *filename, int shot_type )
{
	return false;
}

static qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	return false;
}

static void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	;
}

static int R_CreateDecalList( struct decallist_s *pList )
{
	return 0;
}

static float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	return 0.0f;
}

static void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	;
}

static void R_SetSkyCloudsTextures( int solidskyTexture, int alphaskyTexture )
{
	;
}

static void GL_SubdivideSurface( model_t *mod, msurface_t *fa )
{
	;
}

/* Cheap, side-effect-free pre-check: does this buffer even look like a
   real, nv2a-profile MPAK archive? Used by Mod_ProcessRenderData BEFORE
   touching any already-loaded map state, since that callback fires for
   EVERY model (studio, sprite, every brush submodel), not just the world
   (codex review, real finding) - a later, unrelated model's own buffer
   must fail this and leave existing map geometry alone, not silently
   free it first and find out afterward that there was nothing to
   replace it with. */
static qboolean nv2a_mpak_header_valid( const byte *buffer, size_t buffersize, const mpak_header_t **out_header )
{
	const mpak_header_t *header;

	if( buffersize < MPAK_DESCRIPTOR_OFFSET + sizeof( mpak_header_t ))
		return false;

	header = (const mpak_header_t *)( buffer + MPAK_DESCRIPTOR_OFFSET );

	if( memcmp( header->magic, MPAK_LAYOUT_MAGIC, sizeof( header->magic )) != 0 )
		return false;
	if( header->version != MPAK_LAYOUT_VERSION || header->headerSize != sizeof( mpak_header_t ))
		return false;
	if( !( header->flags & MPAK_FLAG_NV2A_BLOCK ))
		return false; /* a real archive, just not baked with the profile this slice reads */

	*out_header = header;
	return true;
}

/* Reads the WHOLE map's real geometry (every baked vertex, the whole
   shared index buffer, the whole DXT1 atlas, the whole lightmap atlas)
   out of a real, nv2a-profile-baked .mpak buffer and prepares it to be
   drawn in place of the X9-A test triangle: a real, per-map-sized
   remapped vertex buffer (position + atlas-space UV + lightmap UV), a
   real per-map-sized packed index-dword buffer, the real DXT1 atlas
   copied verbatim (already in the right hardware layout - no un-swizzle
   needed, see this file's own header comment and
   docs/r3d/divergences.md's X9-B entry), the real lightmap atlas copied
   verbatim, and a real look-at + perspective camera framing the map's own
   first baked triangle. Caller (Mod_ProcessRenderData) has already
   confirmed the header itself is a real nv2a-profile archive via
   nv2a_mpak_header_valid() - this function only has to validate
   everything PAST the header.

   UV remap is `atlas_uv = rect.xy + frac(raw_uv) * rect.zw` - a per-VERTEX
   remap, not per-pixel. This is exact for a face whose raw UV range fits
   in one texture repeat, and a known, accepted, documented approximation
   for a tiling face (the interpolated UV between two vertices that wrap
   to the SAME fractional value is flat, not repeating, across that edge) -
   fixing that for real needs a pixel-shader-side wrap, out of scope for
   this bring-up slice. Returns false (leaving the caller to keep drawing
   the X9-A triangle) for anything past the header that doesn't parse as
   expected, or that would exceed NV2A_MAP_BUDGET_BYTES - never crashes on
   a truncated, malformed, or just very large archive. */
/* A coarse guard against a malformed or just implausibly large archive
   attempting a doomed giant allocation - NOT a real accounting of the
   Xbox's own 64MiB total budget (codex review, real finding: the
   engine's own static state, pbkit's front/back/depth buffers, every
   other subsystem's own allocations, and allocator fragmentation are
   all real, all already resident by the time this function runs, and
   NONE of them are visible to this narrowly-scoped ref module - a
   proper admission-control budget belongs at BAKE time, in
   tools/asset-studio, where the real total per-map cost is known before
   a single byte reaches the console, not guessed at here from four
   numbers this one function happens to compute). The buffer this
   function is CALLED with is already real and already resident though
   (Mod_ProcessRenderData's own contract - engine/common/model.c's
   Mod_LoadModel FS_LoadFile's the whole archive before calling in), so
   it belongs in the sum this function CAN check. Cross-checked in
   Python against the real crossfire.mpak (docs/r3d/divergences.md #91):
   19,480 verts * 28B + 34,326 indices/2*4B + a real 2048x2048 DXT1 atlas
   + a real 1024x1024 RGBA8 lightmap page + the archive's own 10.5MB on
   disk totals ~16.6MB - comfortably under this cap, with real margin
   left to catch a corrupt or absurdly oversized archive before it can
   hurt a 64MiB console, without pretending this one function can see
   the platform's whole memory picture. */
#define NV2A_MAP_BUDGET_BYTES ( 24u * 1024u * 1024u )

static qboolean nv2a_load_map_geometry( const mpak_header_t *header, const byte *buffer, size_t buffersize )
{
	nv2a_mpak_geometry_t geo;
	uint32_t vert_count, index_count, dword_count;
	uint32_t i;
	uint64_t vb_bytes, idx_bytes, total_bytes;
	nv2a_vec3_t v0, v1, v2, centroid, normal, up, eye;
	float view[4][4], proj[4][4], ndc_to_screen[4][4], combined[4][4];
	uint64_t blocksW, blocksH;

	if( !nv2a_compute_mpak_layout( header, (fs_offset_t)buffersize, &geo ))
		return false;

	if( geo.atlas.length == 0 || geo.lightmapAtlas.length == 0 )
		return false;

	vert_count = header->vCount;
	index_count = header->iCount;
	if( vert_count < 3 || index_count == 0 )
		return false;
	if( (uint64_t)vert_count * sizeof( mpak_vertex_t ) != (uint64_t)geo.vertices.length )
		return false;
	if( (uint64_t)index_count * sizeof( uint16_t ) != (uint64_t)geo.indices.length )
		return false;

	dword_count = ( index_count + 1 ) / 2;
	vb_bytes = (uint64_t)vert_count * sizeof( nv2a_lit_vertex_t );
	idx_bytes = (uint64_t)dword_count * sizeof( uint32_t );
	/* buffersize (the source archive, already resident per
	   Mod_ProcessRenderData's own contract) is real, known cost this
	   function CAN account for - see NV2A_MAP_BUDGET_BYTES's own comment
	   for what this check does and does not cover. */
	total_bytes = vb_bytes + idx_bytes + (uint64_t)geo.atlas.length + (uint64_t)geo.lightmapAtlas.length + (uint64_t)buffersize;
	if( total_bytes > NV2A_MAP_BUDGET_BYTES )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map exceeds NV2A_MAP_BUDGET_BYTES (%u > %u bytes) - not drawing its real geometry\n",
			(unsigned int)total_bytes, (unsigned int)NV2A_MAP_BUDGET_BYTES );
		return false;
	}

	/* Every baked vertex is remapped 1:1 - the shared index buffer already
	   references vertices by their final position in this same array
	   (that's what makes shared indexing possible at all), so unlike the
	   old one-face slice there is no "which vertices does this face touch"
	   deduplication to do here. */
	for( i = 0; i < vert_count; i++ )
	{
		mpak_vertex_t v;

		memcpy( &v, buffer + geo.vertices.offset + (uint64_t)i * sizeof( mpak_vertex_t ), sizeof( v ));

		/* X9-D still only handles vertices on lightmap page 0 - multi-page
		   lightmap support stays explicitly deferred (same limit X9-C
		   already documented), so a vertex on any other page must fail the
		   whole load cleanly rather than silently sample the wrong page. */
		if( v.lightmap_layer != 0 )
			return false;
	}

	/* Every raw index must reference a real slot in the vertex array above
	   - malformed data must fail cleanly here, not be trusted into a GPU
	   attribute-array read out of bounds later. */
	for( i = 0; i < index_count; i++ )
	{
		uint16_t idx;

		memcpy( &idx, buffer + geo.indices.offset + (uint64_t)i * sizeof( uint16_t ), sizeof( idx ));
		if( idx >= vert_count )
			return false;
	}

	/* codex review, real finding: atlasWidth/Height came straight from the
	   header with nothing checking them against the atlas lump's own real
	   (bounds-checked) length - a malformed archive could declare a huge
	   width/height over a tiny actual allocation, and the draw path would
	   then program the GPU's own pitch/image-rect registers from those
	   unchecked dimensions, reading past nv2a_map_atlas into whatever
	   memory follows it. DXT1 is 8 bytes per 4x4 block - computed in
	   uint64_t (header fields are only uint32_t, so blocksW*blocksH*8
	   cannot overflow 64 bits) and required to match the atlas lump's own
	   already-bounds-checked length exactly, not just be "small enough". */
	blocksW = ( (uint64_t)header->atlasWidth + 3 ) / 4;
	blocksH = ( (uint64_t)header->atlasHeight + 3 ) / 4;
	if( header->atlasWidth == 0 || header->atlasHeight == 0 || blocksW * blocksH * 8 != (uint64_t)geo.atlas.length )
		return false;

	/* Same "don't trust dimensions independent of the lump's own real,
	   bounds-checked length" discipline the atlas check above already
	   established (codex review, X9-B round 2) - RGBA8 is a plain 4 bytes
	   per texel, no block rounding needed. */
	{
		uint64_t expected_lm = (uint64_t)header->lightmapWidth * header->lightmapHeight * header->lightmapLayers * 4;

		if( header->lightmapWidth == 0 || header->lightmapHeight == 0 || header->lightmapLayers == 0
			|| header->lightmapFormat != 0 /* 0 = RGBA8, the only format anything bakes today */
			|| expected_lm != (uint64_t)geo.lightmapAtlas.length )
			return false;
	}

	nv2a_map_vb = MmAllocateContiguousMemoryEx( (uint32_t)vb_bytes, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_vb )
	{
		/* codex review, real finding: a validated, real nv2a-profile
		   archive that fails an allocation here fell back to the test
		   triangle with no diagnostic at all - the NV2A_MAP_BUDGET_BYTES
		   check above only guards against a corrupt/absurd archive, not
		   real contiguous-memory fragmentation on an already-running
		   console, so this path is genuinely reachable and worth a real
		   message when it happens. */
		gEngfuncs.Con_Printf( "ref_nv2a: map vertex buffer allocation failed (%u bytes)\n", (unsigned int)vb_bytes );
		return false;
	}
	{
		nv2a_lit_vertex_t *dst = (nv2a_lit_vertex_t *)nv2a_map_vb;

		for( i = 0; i < vert_count; i++ )
		{
			mpak_vertex_t v;

			memcpy( &v, buffer + geo.vertices.offset + (uint64_t)i * sizeof( mpak_vertex_t ), sizeof( v ));

			dst[i].pos[0] = v.x;
			dst[i].pos[1] = v.y;
			dst[i].pos[2] = v.z;
			dst[i].uv0[0] = v.rect[0] + ( v.u - floorf( v.u )) * v.rect[2];
			dst[i].uv0[1] = v.rect[1] + ( v.v - floorf( v.v )) * v.rect[3];
			dst[i].uv1[0] = v.lu;
			dst[i].uv1[1] = v.lv;
		}

		v0.x = dst[0].pos[0]; v0.y = dst[0].pos[1]; v0.z = dst[0].pos[2];
		v1.x = dst[1].pos[0]; v1.y = dst[1].pos[1]; v1.z = dst[1].pos[2];
		v2.x = dst[2].pos[0]; v2.y = dst[2].pos[1]; v2.z = dst[2].pos[2];
	}
	nv2a_map_vert_count = vert_count;

	nv2a_map_atlas = MmAllocateContiguousMemoryEx( geo.atlas.length, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_atlas )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map atlas allocation failed (%u bytes)\n", (unsigned int)geo.atlas.length );
		nv2a_free_map_resources();
		return false;
	}
	memcpy( nv2a_map_atlas, buffer + geo.atlas.offset, geo.atlas.length );
	nv2a_map_atlas_width  = header->atlasWidth;
	nv2a_map_atlas_height = header->atlasHeight;
	nv2a_map_atlas_pitch  = (uint32_t)( blocksW * 8 ); /* same ceil(width/4)*8 the length check above already validated */

	nv2a_map_lightmap = MmAllocateContiguousMemoryEx( geo.lightmapAtlas.length, 0, NV2A_TEST_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE );
	if( !nv2a_map_lightmap )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map lightmap allocation failed (%u bytes)\n", (unsigned int)geo.lightmapAtlas.length );
		nv2a_free_map_resources();
		return false;
	}
	memcpy( nv2a_map_lightmap, buffer + geo.lightmapAtlas.offset, geo.lightmapAtlas.length );
	nv2a_map_lightmap_width  = header->lightmapWidth;
	nv2a_map_lightmap_height = header->lightmapHeight;
	nv2a_map_lightmap_pitch  = nv2a_map_lightmap_width * 4; /* RGBA8, LU_IMAGE - no block rounding */

	/* Pack pairs of raw indices into dwords for NV20_TCL_PRIMITIVE_3D_
	   INDEX_DATA (see nv2a_map_index_dwords's own declaration comment for
	   why - low half first, matching deps/nxdk/samples/mesh/verts.h's own
	   data). An odd index_count pads its final dword's high half by
	   repeating the last real index. Unlike X9-B/C there is no local-index
	   remap here - every raw index already names its final slot in
	   nv2a_map_vb directly, so this is a straight widen-and-pack walk over
	   the whole buffer. */
	nv2a_map_index_dwords = Mem_Malloc( r_temppool, (size_t)idx_bytes );
	if( !nv2a_map_index_dwords )
	{
		gEngfuncs.Con_Printf( "ref_nv2a: map index buffer allocation failed (%u bytes)\n", (unsigned int)idx_bytes );
		nv2a_free_map_resources();
		return false;
	}
	nv2a_map_dword_count = dword_count;
	for( i = 0; i < dword_count; i++ )
	{
		uint16_t lo16, hi16;
		uint32_t lo, hi;

		memcpy( &lo16, buffer + geo.indices.offset + (uint64_t)( i * 2 ) * sizeof( uint16_t ), sizeof( lo16 ));
		lo = lo16;
		if( i * 2 + 1 < index_count )
		{
			memcpy( &hi16, buffer + geo.indices.offset + (uint64_t)( i * 2 + 1 ) * sizeof( uint16_t ), sizeof( hi16 ));
			hi = hi16;
		}
		else
		{
			hi = lo;
		}
		nv2a_map_index_dwords[i] = lo | ( hi << 16 );
	}

	/* A fixed debug camera: look at the map's own first baked triangle from
	   along its own normal - not part of the real game's own view pipeline
	   (a much later X9 slice's job), just enough to frame real, actual
	   geometry for this bring-up's own proof; still an arbitrary rather
	   than a good vantage point, same honest limitation X9-B/C already
	   documented for their own one-face version of this. */
	centroid.x = ( v0.x + v1.x + v2.x ) / 3.0f;
	centroid.y = ( v0.y + v1.y + v2.y ) / 3.0f;
	centroid.z = ( v0.z + v1.z + v2.z ) / 3.0f;

	normal = nv2a_vec3_normalize( nv2a_vec3_cross(
		nv2a_vec3_sub( v1, v0 ),
		nv2a_vec3_sub( v2, v0 )));

	up.x = 0.0f; up.y = 0.0f; up.z = 1.0f; /* GoldSrc's own Z-up convention */
	if( fabsf( normal.z ) > 0.99f )
	{
		up.x = 1.0f; up.y = 0.0f; up.z = 0.0f;
	}

	eye.x = centroid.x + normal.x * 200.0f;
	eye.y = centroid.y + normal.y * 200.0f;
	eye.z = centroid.z + normal.z * 200.0f;

	nv2a_matrix_lookat( view, eye, centroid, up );
	/* yScale = 1/tan(fovY/2) with fovY = 90 degrees, so tan(45 deg) = 1 -
	   a literal 1.0, no runtime tan() needed for a fixed debug FOV. */
	nv2a_matrix_perspective( proj, 1.0f, (float)nv2a_width / (float)nv2a_height, 1.0f, 8192.0f );
	nv2a_matrix_viewport( ndc_to_screen, 0, 0, (float)nv2a_width, (float)nv2a_height, 0, 65536.0f );

	nv2a_matrix_multiply( combined, view, proj );
	nv2a_matrix_multiply( nv2a_map_viewproj, combined, ndc_to_screen );

	return true;
}

/* Called for EVERY model the engine loads or unloads - the world, every
   inline brush submodel, every studio model, every sprite
   (engine/common/model.c) - not just the map. Only ever touches the
   current map-geometry state for the ONE model that actually owns it:
   an unload is ignored unless it's the exact model that populated it
   (nv2a_map_owner), and a load only replaces it once the new buffer has
   already been confirmed to be a real nv2a-profile archive - a later,
   unrelated model's own load or unload leaves whatever the world already
   set up completely alone (codex review, real finding: the first version
   freed unconditionally on every call, so the first studio/sprite model
   loaded right after the world silently wiped it). */
static qboolean Mod_ProcessRenderData( model_t *mod, qboolean create, const byte *buffer, size_t buffersize )
{
	const mpak_header_t *header;

	if( !create )
	{
		if( mod == nv2a_map_owner )
			nv2a_free_map_resources();
		return true;
	}

	if( nv2a_mpak_header_valid( buffer, buffersize, &header ))
	{
		nv2a_free_map_resources();
		if( nv2a_load_map_geometry( header, buffer, buffersize ))
		{
			nv2a_map_ready = true;
			nv2a_map_owner = mod;
		}
	}

	return true; /* the model load itself always succeeds regardless; drawing
	                real geometry is opportunistic - a model that isn't a real
	                nv2a-profile map archive just leaves whatever (if anything)
	                the world already set up untouched */
}

static void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	;
}

static void CL_DrawParticles( double frametime, particle_t *particles, float partsize )
{
	;
}

static void CL_DrawTracers( double frametime, particle_t *tracers )
{
	;
}

static void CL_DrawBeams( int fTrans, BEAM *beams )
{
	;
}

static intptr_t RefGetParm( int parm, int arg )
{
	if( parm == PARM_GL_CONTEXT_TYPE )
		return CONTEXT_TYPE_SOFTWARE;

	return 0;
}

static int GL_FindTexture( const char *name )
{
	return 0;
}

static const char *GL_TextureName( unsigned int texnum )
{
	return NULL;
}

static const byte *GL_TextureData( unsigned int texnum )
{
	return NULL;
}

static int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	return 0;
}

static int GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	return 0;
}

static void R_OverrideTextureSourceSize( unsigned int textnum, unsigned int srcWidth, unsigned int srcHeight )
{
	;
}

static void GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	;
}

static void GL_Bind( int tmu, unsigned int texnum )
{
	;
}

static void GL_RenderFrame( const struct ref_viewpass_s *rvp )
{
	;
}

static void GL_OrthoBounds( const float *mins, const float *maxs )
{
	;
}

static qboolean R_SpeedsMessage( char *out, size_t size )
{
	return false;
}

static byte *Mod_GetCurrentVis( void )
{
	return NULL;
}

static void Color4f( float r, float g, float b, float a )
{
	;
}

static void Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	;
}

static void Vertex3fv( const float *worldPnt )
{
	;
}

static void Vertex3f( float x, float y, float z )
{
	;
}

static void CullFace( TRICULLSTYLE mode )
{
	;
}

static void VGUI_SetupDrawing( qboolean rect )
{
	;
}

/* The one real per-frame entry point: draws and presents the fixed test
   triangle every frame, at the same point ref/soft's own R_EndFrame does
   its real work (R_BlitScreen) - see this file's header comment for why
   R_BeginFrame/R_RenderScene stay no-ops for this bring-up. */
static void R_EndFrame( void )
{
	if( !nv2a_ready )
		return;

	pb_wait_for_vbl();
	pb_reset();
	pb_target_back_buffer();

	pb_erase_depth_stencil_buffer( 0, 0, nv2a_width, nv2a_height );
	pb_fill( 0, 0, nv2a_width, nv2a_height, 0x00000000 );
	pb_erase_text_screen();

	while( pb_busy())
		;

	if( nv2a_map_ready )
		nv2a_draw_map_geometry();
	else
		nv2a_draw_test_triangle();

	while( pb_busy())
		;

	while( pb_finished())
		;
}

const ref_interface_t gReffuncs =
{
	.R_Init                = R_Init,
	.R_Shutdown            = R_Shutdown,
	.R_GetConfigName       = R_GetConfigName,
	.R_SetDisplayTransform = R_SetDisplayTransform,

	.GL_SetupAttributes = GL_SetupAttributes,
	.GL_InitExtensions  = R_SimpleStub,
	.GL_ClearExtensions = R_SimpleStub,

	.R_GammaChanged       = R_SimpleStubBool,
	.R_BeginFrame         = R_SimpleStubBool,
	.R_RenderScene        = R_SimpleStub,
	.R_EndFrame           = R_EndFrame,
	.R_PushScene          = R_SimpleStub,
	.R_PopScene           = R_SimpleStub,
	.GL_BackendStartFrame = R_SimpleStub,
	.GL_BackendEndFrame   = R_SimpleStub,

	.R_ClearScreen    = R_SimpleStub,
	.R_AllowFog       = R_SimpleStubBool,
	.GL_SetRenderMode = R_SimpleStubInt,

	.R_AddEntity      = R_AddEntity,
	.R_ProcessEntData = R_ProcessEntData,

	.R_ShowTextures = R_SimpleStub,

	.R_GetTextureOriginalBuffer = R_GetTextureOriginalBuffer,
	.GL_LoadTextureFromBuffer   = GL_LoadTextureFromBuffer,
	.GL_ProcessTexture          = GL_ProcessTexture,
	.R_SetupSky                 = R_SetupSky,

	.R_Set2DMode      = R_SimpleStubBool,
	.R_DrawStretchPic = R_DrawStretchPic,
	.FillRGBA         = FillRGBA,
	.WorldToScreen    = WorldToScreen,

	.VID_ScreenShot  = VID_ScreenShot,
	.VID_CubemapShot = VID_CubemapShot,

	/* ref/common's own real implementations (ref_light.c, declared in
	   ref_common.h) - NOT local stubs. A local static stub with either
	   name here was a real, isolated duplicate-symbol link error: this
	   module compiles ref/common alongside itself (xbox-ref-isolate.sh),
	   and llvm-objcopy --redefine-syms renames both the local stub and
	   ref/common's real function to the same _nv2a_-prefixed name. */
	.R_LightPoint = R_LightPoint,

	.R_DecalShoot      = R_DecalShoot,
	.R_DecalRemoveAll  = R_SimpleStubInt,
	.R_CreateDecalList = R_CreateDecalList,
	.R_ClearAllDecals  = R_SimpleStub,

	.R_StudioEstimateFrame = R_StudioEstimateFrame,
	.R_StudioLerpMovement  = R_StudioLerpMovement,
	.R_StudioFillAPI          = R_StudioFillAPI,
	.R_StudioSetDrawInterface = R_StudioSetDrawInterface,

	.R_SetSkyCloudsTextures     = R_SetSkyCloudsTextures,
	.GL_SubdivideSurface = GL_SubdivideSurface,
	.CL_RunLightStyles   = CL_RunLightStyles,

	.Mod_ProcessRenderData  = Mod_ProcessRenderData,
	.Mod_StudioLoadTextures = Mod_StudioLoadTextures,

	.CL_DrawParticles = CL_DrawParticles,
	.CL_DrawTracers   = CL_DrawTracers,
	.CL_DrawBeams     = CL_DrawBeams,

	.RefGetParm = RefGetParm,

	.R_GetDetailScaleForTexture = R_GetDetailScaleForTexture,
	.R_SetDetailScaleForTexture = R_SetDetailScaleForTexture,

	.GL_CreateTexture = GL_CreateTexture,
	.GL_FindTexture  = GL_FindTexture,
	.GL_TextureName  = GL_TextureName,
	.GL_TextureData  = GL_TextureData,
	.GL_LoadTexture  = GL_LoadTexture,
	.GL_FreeTexture  = R_SimpleStubUInt,
	.R_OverrideTextureSourceSize = R_OverrideTextureSourceSize,

	.GL_UpdateTexture = GL_UpdateTexture,

	.GL_Bind = GL_Bind,

	.GL_RenderFrame    = GL_RenderFrame,
	.GL_OrthoBounds    = GL_OrthoBounds,
	.R_SpeedsMessage   = R_SpeedsMessage,
	.Mod_GetCurrentVis = Mod_GetCurrentVis,
	.R_NewMap          = R_SimpleStub,
	.R_ClearScene      = R_SimpleStub,

	.TriRenderMode = R_SimpleStubInt,
	.Begin         = R_SimpleStubInt,
	.End           = R_SimpleStub,
	.Color4f       = Color4f,
	.Color4ub      = Color4ub,
	.Vertex3fv     = Vertex3fv,
	.Vertex3f      = Vertex3f,
	.CullFace      = CullFace,

	.R_FillRenderAPI = R_FillRenderAPI,
	.R_FillTriAPI    = R_FillTriAPI,

	.VGUI_SetupDrawing   = VGUI_SetupDrawing,
};
