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

#include <string.h>
#include "xash3d_types.h"
#include "const.h"
#include "cvardef.h"
#include "ref_common.h"

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

static qboolean R_Init( void )
{
	qboolean video_ok;

	debug_marker = 1;

	video_ok = gEngfuncs.R_Init_Video( REF_SOFTWARE );
	debug_marker = video_ok ? 3 : 2;

	if( !video_ok )
	{
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

static qboolean Mod_ProcessRenderData( model_t *mod, qboolean create, const byte *buffer, size_t buffersize )
{
	return true;
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
