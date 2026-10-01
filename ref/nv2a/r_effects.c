/*
r_effects.c - X9-X step 7: effects renderer for ref/nv2a. Whole-function port
of ref/gl/gl_beams.c (beams: every TE_BEAM* type, ET_BEAM server beams)
and ref/gl/gl_rpart.c (particles, tracers), function names and
gl_beams.c:<line> / gl_rpart.c:<line> citations kept per function, drawn
through the TriAPI internals in r_context.c (Begin/TexCoord2f/Color4f/
Vertex3fv/End/CullFace/TriRenderMode) the way r_sprite.c ports
gl_sprite.c.

What is not here because it is not in the renderer upstream either: the
effect lists. The engine owns and simulates them (cl_efx.c: allocation,
CL_ThinkParticle, R_FreeDeadParticles, CL_FreeDeadBeams) and hands them
to this file through CL_DrawEFX (cl_efx.c:2079), which r_context.c's
GL_RenderFrame calls at R_DrawEntitiesOnList's two points
(gl_rmain.c:861, :922). The particle texture is the engine's own
REF_PARTICLE_TEXTURE (ref_common.c:531), already created on main; beams
and tracers bind sprite frames the engine uploaded at precache.

Where it departs from ref/gl (docs/r3d/divergences.md #133):
- Texture repeat. Beam V coordinates scroll over many copies of the
  sprite (R_DrawSegs, gl_beams.c:167-173); ref/gl samples them with
  GL_REPEAT, ref/nv2a's linear textures clamp. R_BeamDraw turns on the
  TriAPI's repeat splitter (nv2a_tri_set_texture_repeat, r_context.c,
  nv2a_efx_util.c) around its draw, so each primitive is cut at the
  texture's cell boundaries instead. Bilinear filtering does not blend
  across a cut the way it blends across GL_REPEAT's seam.
- R_AllowFog is r_warp.c's (X9-X item 14 step 11); pglNormal3fv has no consumer (no
  lighting on the TriAPI); pglShadeModel is always Gouraud (pbkit.c:3170).
*/
#include "ref_common.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "com_strings.h" /* S_ERROR */
#include "r_efx.h"       /* common/r_efx.h: particle_t, pt_* (this renderer's own header is r_effects.h, not to shadow it) */
#include "beamdef.h"
#include "customentity.h"
#include "entity_types.h"
#include "triangleapi.h"
#include "pmove.h"       /* movevars_s (gp_movevars->gravity) */
#include "nv2a_texture.h"
#include "nv2a_efx_util.h"
#include "nv2a_sprite_util.h" /* NV2A_SPRITE_SFACTOR_ / DFACTOR_ mirrors of nv_regs.h, pinned in r_context.c */
#include "r_studio_bridge.h"
#include "r_warp.h" /* X9-X item 14 step 11: R_WarpAllowFog */
#include "r_sprite.h"    /* TriSpriteTexture (gl_triapi.c:242) */
#include "r_effects.h"
#include "nv2a_ft.h" /* frame-time work counters (divergences.md #141) */

#define NOISE_DIVISIONS	NV2A_EFX_NOISE_DIVISIONS // gl_beams.c:25

typedef struct
{
	vec3_t	pos;
	float	texcoord;	// Y texture coordinate
	float	width;
} beamseg_t;

/* ref/gl's r_stats efx counters (gl_local.h). */
static struct
{
	uint c_view_beams_count;
	uint c_particle_count;
} nv2a_efx_stats;

/* The RI.rvp fields, RI.v* vectors (RI.cull_v* are the same vectors on
   the one pass this renderer draws, gl_rmain.c's R_SetupFrustum) and
   tr.frametime / tr.palette the effects read - set by r_context.c. */
static struct
{
	int     rvp_flags;
	vec3_t  vieworigin;
	int     viewport[4];
	vec3_t  vforward, vright, vup;
	double  frametime;
	color24 *palette;
} RI;

/* ref/gl's ds.renderMode / ds.triRGBA (gl_triapi.c), which the renderer's
   own TriColor4f and TriBrightness read. Private to ref/gl as it is here:
   the client's TriAPI keeps its own copy (clgame.ds, cl_game.c:3174). */
static struct
{
	int   renderMode;
	float triRGBA[4];
} ds;

void R_EfxInit( void )
{
	RI.palette = (color24 *)ENGINE_GET_PARM( PARM_GET_PALETTE_PTR );
}

void R_EfxSetFrameContext( int rvp_flags, const vec3_t vieworigin, const int viewport[4],
	const vec3_t vforward, const vec3_t vright, const vec3_t vup, double frametime )
{
	RI.rvp_flags = rvp_flags;
	VectorCopy( vieworigin, RI.vieworigin );
	RI.viewport[0] = viewport[0];
	RI.viewport[1] = viewport[1];
	RI.viewport[2] = viewport[2];
	RI.viewport[3] = viewport[3];
	VectorCopy( vforward, RI.vforward );
	VectorCopy( vright, RI.vright );
	VectorCopy( vup, RI.vup );
	RI.frametime = frametime;
}

#define WORLDMODEL ( gp_cl->models[1] )

static inline model_t *CL_ModelHandle( int index )
{
	if( unlikely( index < 0 || index >= gp_cl->nummodels ))
		return NULL;
	return gp_cl->models[index];
}

#define CL_FxBlend( e ) nv2a_fx_blend( e )
#define R_CullBox( mins, maxs ) nv2a_frustum_cull_box( mins, maxs )
#define Mod_GetCurrentVis() nv2a_mod_get_current_vis()

/* R_AllowFog (gl_rmain.c:42), r_warp.c's port since X9-X item 14
   step 11. */
static void R_AllowFog( qboolean allowed )
{
	R_WarpAllowFog( allowed );
}

/* ---- the ref/gl TriAPI helpers these files call ---------------------- */

/* GL_Cull( GL_NONE / GL_FRONT ): TriCullFace's mapping (gl_triapi.c:340). */
#define GL_Cull_None()  CullFace( TRI_NONE )
#define GL_Cull_Front() CullFace( TRI_FRONT )

/* TriRenderMode (gl_triapi.c:40): the r_context.c one sets the GL state;
   ds.renderMode is this file's, for TriColor4f. */
static void R_TriRenderMode( int mode )
{
	ds.renderMode = mode;
	TriRenderMode( mode );
}

/* TriColor4ub, the renderer's own (gl_triapi.c:156): alpha is dropped
   from the GL colour, kept in ds.triRGBA. */
static void R_TriColor4ub( byte r, byte g, byte b, byte a )
{
	ds.triRGBA[0] = r * ( 1.0f / 255.0f );
	ds.triRGBA[1] = g * ( 1.0f / 255.0f );
	ds.triRGBA[2] = b * ( 1.0f / 255.0f );
	ds.triRGBA[3] = a * ( 1.0f / 255.0f );

	Color4f( ds.triRGBA[0], ds.triRGBA[1], ds.triRGBA[2], 1.0f );
}

/* TriColor4f (gl_triapi.c:171): premultiplied by alpha, GL alpha 1, except
   in kRenderTransAlpha. */
static void R_TriColor4f( float r, float g, float b, float a )
{
	if( ds.renderMode == kRenderTransAlpha )
		R_TriColor4ub( r * 255.9f, g * 255.9f, b * 255.9f, a * 255.0f );
	else Color4f( r * a, g * a, b * a, 1.0 );

	ds.triRGBA[0] = r;
	ds.triRGBA[1] = g;
	ds.triRGBA[2] = b;
	ds.triRGBA[3] = a;
}

/* TriBrightness (gl_triapi.c:362). */
static void TriBrightness( float brightness )
{
	float r = ds.triRGBA[0] * ds.triRGBA[3] * brightness;
	float g = ds.triRGBA[1] * ds.triRGBA[3] * brightness;
	float b = ds.triRGBA[2] * ds.triRGBA[3] * brightness;

	Color4f( r, g, b, 1.0f );
}

/* TriWorldToScreen (gl_triapi.c:223): R_WorldToScreen's NDC scaled into
   the view's viewport - the same port r_sprite.c carries. */
static int TriWorldToScreen( const float *world, float *screen )
{
	int retval = nv2a_world_to_screen( world, screen );

	screen[0] =  0.5f * screen[0] * (float)RI.viewport[2];
	screen[1] = -0.5f * screen[1] * (float)RI.viewport[3];
	screen[0] += 0.5f * (float)RI.viewport[2];
	screen[1] += 0.5f * (float)RI.viewport[3];

	return retval;
}

/* pglNormal3fv: nothing on the TriAPI consumes a normal (no lighting). */
#define pglNormal3fv( n ) ((void)( n ))

/*
==============================================================

FRACTAL NOISE

==============================================================
*/
static float	rgNoise[NOISE_DIVISIONS+1];	// global noise array

// gl_beams.c:43-68, host-tested in nv2a_efx_util.c
static void FracNoise( float *noise, int divs )
{
	nv2a_efx_frac_noise( noise, divs, gEngfuncs.COM_RandomFloat );
}

static void SineNoise( float *noise, int divs )
{
	nv2a_efx_sine_noise( noise, divs );
}

/*
==============================================================

BEAM MATHLIB

==============================================================
*/
// gl_beams.c:78
static void R_BeamComputePerpendicular( const vec3_t vecBeamDelta, vec3_t pPerp )
{
	// direction in worldspace of the center of the beam
	vec3_t	vecBeamCenter;

	VectorNormalize2( vecBeamDelta, vecBeamCenter );
	CrossProduct( RI.vforward, vecBeamCenter, pPerp );
	VectorNormalize( pPerp );
}

// gl_beams.c:88
static void R_BeamComputeNormal( const vec3_t vStartPos, const vec3_t vNextPos, vec3_t pNormal )
{
	// vTangentY = line vector for beam
	vec3_t	vTangentY, vDirToBeam;

	VectorSubtract( vStartPos, vNextPos, vTangentY );

	// vDirToBeam = vector from viewer origin to beam
	VectorSubtract( vStartPos, RI.vieworigin, vDirToBeam );

	// get a vector that is perpendicular to us and perpendicular to the beam.
	// this is used to fatten the beam.
	CrossProduct( vTangentY, vDirToBeam, pNormal );
	VectorNormalizeFast( pNormal );
}

/*
==============
R_BeamCull

gl_beams.c:112. Cull the beam by bbox
==============
*/
static qboolean R_BeamCull( const vec3_t start, const vec3_t end, qboolean pvsOnly )
{
	vec3_t mins, maxs;

	for( int i = 0; i < 3; i++ )
	{
		if( start[i] < end[i] )
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		// don't let it be zero sized
		if( mins[i] == maxs[i] )
			maxs[i] += 1.0f;
	}

	// check bbox
	if( gEngfuncs.Mod_BoxVisible( mins, maxs, Mod_GetCurrentVis( )))
	{
		if( pvsOnly || !R_CullBox( mins, maxs ))
		{
			// beam is visible
			return false;
		}
	}

	// beam is culled
	return true;
}

/*
==============================================================

BEAM DRAW METHODS

==============================================================
*/
/*
================
R_DrawSegs

gl_beams.c:157. general code for drawing beams
================
*/
static void R_DrawSegs( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments, int flags )
{
	if( segments < 2 ) return;

	float length = VectorLength( delta );
	float flMaxWidth = width * 0.5f;
	float div = 1.0f / ( segments - 1 );

	if( length * div < flMaxWidth * 1.414f )
	{
		// here, we have too many segments; we could get overlap... so lets have less segments
		segments = (int)( length / ( flMaxWidth * 1.414f )) + 1.0f;
		if( segments < 2 ) segments = 2;
	}

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	div = 1.0f / (segments - 1);
	length *= 0.01f;
	float vStep = length * div;	// Texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	float vLast = fmod( freq * speed, 1 );

	if( flags & FBEAM_SINENOISE )
	{
		if( segments < 16 )
		{
			segments = 16;
			div = 1.0f / ( segments - 1 );
		}
		scale *= 100.0f;
		length = segments * 0.1f;
	}
	else
	{
		scale *= length * 2.0f;
	}

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	int noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );
	float brightness = 1.0f;
	int noiseIndex = 0;

	if( FBitSet( flags, FBEAM_SHADEIN ))
		brightness = 0;

	// Choose two vectors that are perpendicular to the beam
	vec3_t perp1;
	R_BeamComputePerpendicular( delta, perp1 );

	int total_segs = segments;
	int segs_drawn = 0;
	beamseg_t curSeg;
	vec3_t vLastNormal;

	/* ref/gl leaves both uninitialised; neither is read before it is set
	   (curSeg on segs_drawn > 0, vLastNormal on segs_drawn > 1 or on the
	   last segment, which is always preceded by a first one) - zeroed
	   only to keep the compiler's flow analysis quiet. */
	memset( &curSeg, 0, sizeof( curSeg ));
	VectorClear( vLastNormal );

	// specify all the segments.
	for( int i = 0; i < segments; i++ )
	{
		beamseg_t nextSeg;
		vec3_t vPoint1, vPoint2;

		Assert( noiseIndex < ( NOISE_DIVISIONS << 16 ));

		float fraction = i * div;

		VectorMA( source, fraction, delta, nextSeg.pos );

		// distort using noise
		if( scale != 0 )
		{
			float factor = rgNoise[noiseIndex>>16] * scale;

			if( FBitSet( flags, FBEAM_SINENOISE ))
			{
				float	s, c;

				SinCos( fraction * M_PI_F * length + freq, &s, &c );
				VectorMA( nextSeg.pos, (factor * s), RI.vup, nextSeg.pos );

				// rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
				VectorMA( nextSeg.pos, (factor * c), RI.vright, nextSeg.pos );
			}
			else
			{
				VectorMA( nextSeg.pos, factor, perp1, nextSeg.pos );
			}
		}

		// specify the next segment.
		nextSeg.width = width * 2.0f;
		nextSeg.texcoord = vLast;

		if( segs_drawn > 0 )
		{
			// Get a vector that is perpendicular to us and perpendicular to the beam.
			// This is used to fatten the beam.
			vec3_t	vNormal, vAveNormal;

			R_BeamComputeNormal( curSeg.pos, nextSeg.pos, vNormal );

			if( segs_drawn > 1 )
			{
				// Average this with the previous normal
				VectorAdd( vNormal, vLastNormal, vAveNormal );
				VectorScale( vAveNormal, 0.5f, vAveNormal );
				VectorNormalizeFast( vAveNormal );
			}
			else
			{
				VectorCopy( vNormal, vAveNormal );
			}

			VectorCopy( vNormal, vLastNormal );

			// draw regular segment
			VectorMA( curSeg.pos, ( curSeg.width * 0.5f ), vAveNormal, vPoint1 );
			VectorMA( curSeg.pos, (-curSeg.width * 0.5f ), vAveNormal, vPoint2 );

			TexCoord2f( 0.0f, curSeg.texcoord );
			TriBrightness( brightness );
			pglNormal3fv( vAveNormal );
			Vertex3fv( vPoint1 );

			TexCoord2f( 1.0f, curSeg.texcoord );
			TriBrightness( brightness );
			pglNormal3fv( vAveNormal );
			Vertex3fv( vPoint2 );
		}

		curSeg = nextSeg;
		segs_drawn++;

		if( FBitSet( flags, FBEAM_SHADEIN ) && FBitSet( flags, FBEAM_SHADEOUT ))
		{
			if( fraction < 0.5f ) brightness = fraction;
			else brightness = ( 1.0f - fraction );
		}
		else if( FBitSet( flags, FBEAM_SHADEIN ))
		{
			brightness = fraction;
		}
		else if( FBitSet( flags, FBEAM_SHADEOUT ))
		{
			brightness = 1.0f - fraction;
		}

		if( segs_drawn == total_segs )
		{
			// draw the last segment
			VectorMA( curSeg.pos, ( curSeg.width * 0.5f ), vLastNormal, vPoint1 );
			VectorMA( curSeg.pos, (-curSeg.width * 0.5f ), vLastNormal, vPoint2 );

			// specify the points.
			TexCoord2f( 0.0f, curSeg.texcoord );
			TriBrightness( brightness );
			pglNormal3fv( vLastNormal );
			Vertex3fv( vPoint1 );

			TexCoord2f( 1.0f, curSeg.texcoord );
			TriBrightness( brightness );
			pglNormal3fv( vLastNormal );
			Vertex3fv( vPoint2 );
		}

		vLast += vStep; // Advance texture scroll (v axis only)
		noiseIndex += noiseStep;
	}
}

/*
================
R_DrawTorus

gl_beams.c:344. Draw beamtours
================
*/
static void R_DrawTorus( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f; // don't lose all of the noise/texture on short beams

	float div = 1.0f / (segments - 1);

	float vStep = length * div; // Texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	float vLast = fmod( freq * speed, 1 );
	scale = scale * length;

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	int noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );
	int noiseIndex = 0;

	vec3_t screenLast = { 0 };

	for( int i = 0; i < segments; i++ )
	{
		float fraction = i * div;
		float s, c;
		SinCos( fraction * M_PI2_F, &s, &c );

		vec3_t point;
		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2];

		// distort using noise
		if( scale != 0 )
		{
			if(( noiseIndex >> 16 ) < NOISE_DIVISIONS )
			{
				float factor = rgNoise[noiseIndex>>16] * scale;
				VectorMA( point, factor, RI.vup, point );

				// rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
				factor = rgNoise[noiseIndex>>16] * scale * cos( fraction * M_PI_F * 3 + freq );
				VectorMA( point, factor, RI.vright, point );
			}
		}

		// Transform point into screen space
		vec3_t screen;
		TriWorldToScreen( point, screen );

		if( i != 0 )
		{
			// build world-space normal to screen-space direction vector
			vec3_t tmp;
			VectorSubtract( screen, screenLast, tmp );

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize( tmp );
			vec3_t normal;
			VectorScale( RI.vup, -tmp[0], normal );	// Build point along noraml line (normal is -y, x)
			VectorMA( normal, tmp[1], RI.vright, normal );

			// Make a wide line
			vec3_t last1, last2;
			VectorMA( point, width, normal, last1 );
			VectorMA( point, -width, normal, last2 );

			vLast += vStep; // advance texture scroll (v axis only)
			TexCoord2f( 1, vLast );
			Vertex3fv( last2 );
			TexCoord2f( 0, vLast );
			Vertex3fv( last1 );
		}

		VectorCopy( screen, screenLast );
		noiseIndex += noiseStep;
	}
}

/*
================
R_DrawDisk

gl_beams.c:434. Draw beamdisk
================
*/
static void R_DrawDisk( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )		// UNDONE: Allow more segments?
		segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;	// don't lose all of the noise/texture on short beams

	float div = 1.0f / (segments - 1);
	float vStep = length * div;		// Texture length texels per space pixel

	// scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	float vLast = fmod( freq * speed, 1 );
	scale = scale * length;

	// clamp the beam width
	float w = fmod( freq, width * 0.1f ) * delta[2];

	// NOTE: we must force the degenerate triangles to be on the edge
	for( int i = 0; i < segments; i++ )
	{
		float s, c;
		float fraction = i * div;
		vec3_t point = Vec3( source );

		TriBrightness( 1.0f );
		TexCoord2f( 1.0f, vLast );
		Vertex3fv( point );

		SinCos( fraction * M_PI2_F, &s, &c );
		point[0] = s * w + source[0];
		point[1] = c * w + source[1];
		point[2] = source[2];

		TriBrightness( 1.0f );
		TexCoord2f( 0.0f, vLast );
		Vertex3fv( point );

		vLast += vStep;	// advance texture scroll (v axis only)
	}
}

/*
================
R_DrawCylinder

gl_beams.c:481. Draw beam cylinder
================
*/
static void R_DrawCylinder( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;	// don't lose all of the noise/texture on short beams

	float div = 1.0f / (segments - 1);
	float vStep = length * div;		// texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	float vLast = fmod( freq * speed, 1 );
	scale = scale * length;

	for ( int i = 0; i < segments; i++ )
	{
		float s, c;
		float fraction = i * div;
		SinCos( fraction * M_PI2_F, &s, &c );

		vec3_t point;
		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2] + width;

		TriBrightness( 0 );
		TexCoord2f( 1, vLast );
		Vertex3fv( point );

		point[0] = s * freq * ( delta[2] + width ) + source[0];
		point[1] = c * freq * ( delta[2] + width ) + source[1];
		point[2] = source[2] - width;

		TriBrightness( 1 );
		TexCoord2f( 0, vLast );
		Vertex3fv( point );

		vLast += vStep;	// Advance texture scroll (v axis only)
	}
}

/*
==============
R_DrawBeamFollow

gl_beams.c:530. drawi followed beam
==============
*/
static void R_DrawBeamFollow( BEAM *pbeam, float frametime )
{
	gEngfuncs.R_FreeDeadParticles( &pbeam->particles );

	particle_t *particles = pbeam->particles;
	particle_t *pnew = NULL;

	vec3_t delta;
	float div = 0;
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ))
	{
		if( particles )
		{
			VectorSubtract( particles->org, pbeam->source, delta );
			div = VectorLength( delta );

			if( div >= 32 )
			{
				pnew = gEngfuncs.CL_AllocParticleFast();
			}
		}
		else
		{
			pnew = gEngfuncs.CL_AllocParticleFast();
		}
	}

	if( pnew )
	{
		VectorCopy( pbeam->source, pnew->org );
		pnew->die = gp_cl->time + pbeam->amplitude;
		VectorClear( pnew->vel );

		pnew->next = particles;
		pbeam->particles = pnew;
		particles = pnew;
	}

	// nothing to draw
	if( !particles ) return;

	vec3_t screen, screenLast;
	if( !pnew && div != 0 )
	{
		VectorCopy( pbeam->source, delta );
		TriWorldToScreen( pbeam->source, screenLast );
		TriWorldToScreen( particles->org, screen );
	}
	else if( particles && particles->next )
	{
		VectorCopy( particles->org, delta );
		TriWorldToScreen( particles->org, screenLast );
		TriWorldToScreen( particles->next->org, screen );
		particles = particles->next;
	}
	else
	{
		return;
	}

	// UNDONE: This won't work, screen and screenLast must be extrapolated here to fix the
	// first beam segment for this trail

	// build world-space normal to screen-space direction vector
	vec3_t tmp;
	VectorSubtract( screen, screenLast, tmp );
	// we don't need Z, we're in screen space
	tmp[2] = 0;
	VectorNormalize( tmp );

	// Build point along noraml line (normal is -y, x)
	vec3_t normal;
	VectorScale( RI.vup, tmp[0], normal );	// Build point along normal line (normal is -y, x)
	VectorMA( normal, tmp[1], RI.vright, normal );

	// Make a wide line
	vec3_t last1, last2;
	VectorMA( delta, pbeam->width, normal, last1 );
	VectorMA( delta, -pbeam->width, normal, last2 );

	div = 1.0f / pbeam->amplitude;
	float fraction = ( pbeam->die - gp_cl->time ) * div;

	float vLast = 0.0f;
	float vStep = 1.0f;

	while( particles )
	{
		TriBrightness( fraction );
		TexCoord2f( 1, 1 );
		Vertex3fv( last2 );
		TriBrightness( fraction );
		TexCoord2f( 0, 1 );
		Vertex3fv( last1 );

		// Transform point into screen space
		TriWorldToScreen( particles->org, screen );
		// Build world-space normal to screen-space direction vector
		VectorSubtract( screen, screenLast, tmp );

		// we don't need Z, we're in screen space
		tmp[2] = 0;
		VectorNormalize( tmp );
		VectorScale( RI.vup, tmp[0], normal );	// Build point along noraml line (normal is -y, x)
		VectorMA( normal, tmp[1], RI.vright, normal );

		// Make a wide line
		VectorMA( particles->org, pbeam->width, normal, last1 );
		VectorMA( particles->org, -pbeam->width, normal, last2 );

		vLast += vStep;	// Advance texture scroll (v axis only)

		if( particles->next != NULL )
		{
			fraction = (particles->die - gp_cl->time) * div;
		}
		else
		{
			fraction = 0.0;
		}

		TriBrightness( fraction );
		TexCoord2f( 0, 0 );
		Vertex3fv( last1 );
		TriBrightness( fraction );
		TexCoord2f( 1, 0 );
		Vertex3fv( last2 );

		VectorCopy( screen, screenLast );

		particles = particles->next;
	}

	// drift popcorn trail if there is a velocity
	particles = pbeam->particles;

	while( particles )
	{
		VectorMA( particles->org, frametime, particles->vel, particles->org );
		particles = particles->next;
	}
}

/*
================
R_DrawRing

gl_beams.c:686. Draw beamring
================
*/
static void R_DrawRing( vec3_t source, vec3_t delta, float width, float amplitude, float freq, float speed, int segments )
{
	if( segments < 2 )
		return;

	vec3_t screenLast;
	VectorClear( screenLast );
	segments = segments * M_PI_F;

	if( segments > NOISE_DIVISIONS * 8 )
		segments = NOISE_DIVISIONS * 8;

	float length = VectorLength( delta ) * 0.01f * M_PI_F;
	if( length < 0.5f ) length = 0.5f;		// Don't lose all of the noise/texture on short beams

	float div = 1.0f / ( segments - 1 );

	float vStep = length * div / 8.0f;			// texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	float vLast = fmod( freq * speed, 1.0f );
	float scale = amplitude * length / 8.0f;

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	int noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f ) * 8;
	int noiseIndex = 0;

	VectorScale( delta, 0.5f, delta );
	vec3_t center;
	VectorAdd( source, delta, center );

	vec3_t xaxis = Vec3( delta );
	float radius = VectorLength( xaxis );

	// cull beamring
	// --------------------------------
	// Compute box center +/- radius
	vec3_t last1 = { radius, radius, scale };
	vec3_t tmp, screen;
	VectorAdd( center, last1, tmp );		// maxs
	VectorSubtract( center, last1, screen );	// mins

	if( !WORLDMODEL )
		return;

	// is that box in PVS && frustum?
	if( !gEngfuncs.Mod_BoxVisible( screen, tmp, Mod_GetCurrentVis( )) || R_CullBox( screen, tmp ))
	{
		return;
	}

	vec3_t yaxis = { xaxis[1], -xaxis[0], 0.0f };
	VectorNormalize( yaxis );
	VectorScale( yaxis, radius, yaxis );

	int j = segments / 8;

	for( int i = 0; i < segments + 1; i++ )
	{
		float fraction = i * div;
		float x, y;
		SinCos( fraction * M_PI2_F, &x, &y );

		vec3_t point;
		VectorMAMAM( x, xaxis, y, yaxis, 1.0f, center, point );

		// distort using noise
		float factor = rgNoise[(noiseIndex >> 16) & (NOISE_DIVISIONS - 1)] * scale;
		VectorMA( point, factor, RI.vup, point );

		// Rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
		factor = rgNoise[(noiseIndex >> 16) & (NOISE_DIVISIONS - 1)] * scale;
		factor *= cos( fraction * M_PI_F * 24 + freq );
		VectorMA( point, factor, RI.vright, point );

		// Transform point into screen space
		TriWorldToScreen( point, screen );

		if( i != 0 )
		{
			// build world-space normal to screen-space direction vector
			VectorSubtract( screen, screenLast, tmp );

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize( tmp );

			// Build point along normal line (normal is -y, x)
			vec3_t normal;
			VectorScale( RI.vup, tmp[0], normal );
			VectorMA( normal, tmp[1], RI.vright, normal );

			// Make a wide line
			vec3_t last2;
			VectorMA( point, width, normal, last1 );
			VectorMA( point, -width, normal, last2 );

			vLast += vStep;	// Advance texture scroll (v axis only)
			TexCoord2f( 1.0f, vLast );
			Vertex3fv( last2 );
			TexCoord2f( 0.0f, vLast );
			Vertex3fv( last1 );
		}

		VectorCopy( screen, screenLast );
		noiseIndex += noiseStep;
		j--;

		if( j == 0 && amplitude != 0 )
		{
			j = segments / 8;
			FracNoise( rgNoise, NOISE_DIVISIONS );
		}
	}
}

/*
==============
R_BeamComputePoint

gl_beams.c:809. compute attachment point for beam
==============
*/
static qboolean R_BeamComputePoint( int beamEnt, vec3_t pt )
{
	cl_entity_t *ent = gEngfuncs.R_BeamGetEntity( beamEnt );

	int attach;
	if( beamEnt < 0 )
		attach = BEAMENT_ATTACHMENT( -beamEnt );
	else attach = BEAMENT_ATTACHMENT( beamEnt );

	if( !ent )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: invalid entity %i\n", __func__, BEAMENT_ENTITY( beamEnt ));
		VectorClear( pt );
		return false;
	}

	// get attachment
	if( attach > 0 )
		VectorCopy( ent->attachment[attach - 1], pt );
	else if( ent->index == ( gp_cl->playernum + 1 ))
		VectorCopy( gp_cl->simorg, pt );
	else VectorCopy( ent->origin, pt );

	return true;
}

/*
==============
R_BeamRecomputeEndpoints

gl_beams.c:841. Recomputes beam endpoints..
==============
*/
static qboolean R_BeamRecomputeEndpoints( BEAM *pbeam )
{
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ))
	{
		cl_entity_t *start = gEngfuncs.R_BeamGetEntity( pbeam->startEntity );

		if( R_BeamComputePoint( pbeam->startEntity, pbeam->source ))
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = start->model;
			SetBits( pbeam->flags, FBEAM_STARTVISIBLE );
		}
		else if( !FBitSet( pbeam->flags, FBEAM_FOREVER ))
		{
			ClearBits( pbeam->flags, FBEAM_STARTENTITY );
		}
	}

	if( FBitSet( pbeam->flags, FBEAM_ENDENTITY ))
	{
		cl_entity_t *end = gEngfuncs.R_BeamGetEntity( pbeam->endEntity );

		if( R_BeamComputePoint( pbeam->endEntity, pbeam->target ))
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = end->model;
			SetBits( pbeam->flags, FBEAM_ENDVISIBLE );
		}
		else if( !FBitSet( pbeam->flags, FBEAM_FOREVER ))
		{
			ClearBits( pbeam->flags, FBEAM_ENDENTITY );
			pbeam->die = gp_cl->time;
			return false;
		}
		else
		{
			return false;
		}
	}

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ) && !FBitSet( pbeam->flags, FBEAM_STARTVISIBLE ))
		return false;
	return true;
}


/*
==============
R_BeamDraw

gl_beams.c:894. Update beam vars and draw it
==============
*/
static void R_BeamDraw( BEAM *pbeam, float frametime )
{
	model_t *model = CL_ModelHandle( pbeam->modelIndex );
	SetBits( pbeam->flags, FBEAM_ISACTIVE );

	if( !model || model->type != mod_sprite )
	{
		pbeam->flags &= ~FBEAM_ISACTIVE; // force to ignore
		pbeam->die = gp_cl->time;
		return;
	}

	// update frequency
	pbeam->freq += frametime;

	// generate fractal noise
	if( frametime != 0.0f )
	{
		rgNoise[0] = 0;
		rgNoise[NOISE_DIVISIONS] = 0;
	}

	if( pbeam->amplitude != 0 && frametime != 0.0f )
	{
		if( FBitSet( pbeam->flags, FBEAM_SINENOISE ))
			SineNoise( rgNoise, NOISE_DIVISIONS );
		else FracNoise( rgNoise, NOISE_DIVISIONS );
	}

	// update end points
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY|FBEAM_ENDENTITY ))
	{
		// makes sure attachment[0] + attachment[1] are valid
		if( !R_BeamRecomputeEndpoints( pbeam ))
		{
			ClearBits( pbeam->flags, FBEAM_ISACTIVE ); // force to ignore
			return;
		}

		// compute segments from the new endpoints
		vec3_t delta;
		VectorSubtract( pbeam->target, pbeam->source, delta );
		VectorClear( pbeam->delta );

		if( VectorLength( delta ) > 0.0000001f )
			VectorCopy( delta, pbeam->delta );

		if( pbeam->amplitude >= 0.50f )
			pbeam->segments = VectorLength( pbeam->delta ) * 0.25f + 3.0f; // one per 4 pixels
		else pbeam->segments = VectorLength( pbeam->delta ) * 0.075f + 3.0f; // one per 16 pixels
	}

	if( pbeam->type == TE_BEAMPOINTS && R_BeamCull( pbeam->source, pbeam->target, 0 ))
	{
		ClearBits( pbeam->flags, FBEAM_ISACTIVE );
		return;
	}

	// don't draw really short or inactive beams
	if( !FBitSet( pbeam->flags, FBEAM_ISACTIVE ) || VectorLength( pbeam->delta ) < 0.1f )
	{
		return;
	}

	if( pbeam->flags & ( FBEAM_FADEIN|FBEAM_FADEOUT ))
	{
		// update life cycle
		pbeam->t = pbeam->freq + ( pbeam->die - gp_cl->time );
		if( pbeam->t != 0.0f ) pbeam->t = 1.0f - pbeam->freq / pbeam->t;
	}

	if( pbeam->type == TE_BEAMHOSE )
	{
		vec3_t delta;
		VectorSubtract( pbeam->target, pbeam->source, delta );
		VectorNormalize( delta );

		float flDot = DotProduct( delta, RI.vforward );

		// abort if the player's looking along it away from the source
		if( flDot > 0 )
		{
			return;
		}
		else
		{
			float flFade = pow( flDot, 10 );

			// fade the beam if the player's not looking at the source
			vec3_t localDir;
			VectorSubtract( RI.vieworigin, pbeam->source, localDir );
			flDot = DotProduct( delta, localDir );
			vec3_t vecProjection, tmp;
			VectorScale( delta, flDot, vecProjection );
			VectorSubtract( localDir, vecProjection, tmp );
			float flDistance = VectorLength( tmp );

			if( flDistance > 30 )
			{
				flDistance = 1.0f - (( flDistance - 30.0f ) / 64.0f );
				if( flDistance <= 0 ) flFade = 0;
				else flFade *= pow( flDistance, 3 );
			}

			if( flFade < ( 1.0f / 255.0f ))
				return;

			// FIXME: needs to be testing
			pbeam->brightness *= flFade;
		}
	}

	R_TriRenderMode( FBitSet( pbeam->flags, FBEAM_SOLID ) ? kRenderNormal : kRenderTransAdd );

	if( !TriSpriteTexture( model, (int)(pbeam->frame + pbeam->frameRate * gp_cl->time) % pbeam->frameCount ))
	{
		ClearBits( pbeam->flags, FBEAM_ISACTIVE );
		return;
	}

	if( pbeam->type == TE_BEAMFOLLOW )
	{
		// XASH SPECIFIC: get brightness from head entity
		cl_entity_t *pStart = gEngfuncs.R_BeamGetEntity( pbeam->startEntity );
		if( pStart && pStart->curstate.rendermode != kRenderNormal )
			pbeam->brightness = CL_FxBlend( pStart ) / 255.0f;
	}

	if( FBitSet( pbeam->flags, FBEAM_FADEIN ))
		R_TriColor4f( pbeam->r, pbeam->g, pbeam->b, pbeam->t * pbeam->brightness );
	else if( FBitSet( pbeam->flags, FBEAM_FADEOUT ))
		R_TriColor4f( pbeam->r, pbeam->g, pbeam->b, ( 1.0f - pbeam->t ) * pbeam->brightness );
	else R_TriColor4f( pbeam->r, pbeam->g, pbeam->b, pbeam->brightness );

	/* divergences.md #133: GL_REPEAT on the beam's sprite (see this
	   file's header) - the TriAPI cuts each primitive at the texture's
	   cell boundaries while this is on. */
	nv2a_tri_set_texture_repeat( true );

	switch( pbeam->type )
	{
	case TE_BEAMTORUS:
		GL_Cull_None();
		Begin( TRI_TRIANGLE_STRIP );
		R_DrawTorus( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		End();
		break;
	case TE_BEAMDISK:
		GL_Cull_None();
		Begin( TRI_TRIANGLE_STRIP );
		R_DrawDisk( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		End();
		break;
	case TE_BEAMCYLINDER:
		GL_Cull_None();
		Begin( TRI_TRIANGLE_STRIP );
		R_DrawCylinder( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		End();
		break;
	case TE_BEAMPOINTS:
	case TE_BEAMHOSE:
		Begin( TRI_TRIANGLE_STRIP );
		R_DrawSegs( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments, pbeam->flags );
		End();
		break;
	case TE_BEAMFOLLOW:
		Begin( TRI_QUADS );
		R_DrawBeamFollow( pbeam, frametime );
		End();
		break;
	case TE_BEAMRING:
		GL_Cull_None();
		Begin( TRI_TRIANGLE_STRIP );
		R_DrawRing( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		End();
		break;
	}

	nv2a_tri_set_texture_repeat( false );

	GL_Cull_Front();
	nv2a_efx_stats.c_view_beams_count++;
	NV2A_FT_ADD( XBOX_FT_W_BEAMS, 1 );
}

/*
==============
R_BeamSetAttributes

gl_beams.c:1087. set beam attributes
==============
*/
static void R_BeamSetAttributes( BEAM *pbeam, float r, float g, float b, float framerate, int startFrame )
{
	pbeam->frame = (float)startFrame;
	pbeam->frameRate = framerate;
	pbeam->r = r;
	pbeam->g = g;
	pbeam->b = b;
}

/*
==============
R_BeamSetup

gl_beams.c:1104. generic function. all beams must be
passed through this
==============
*/
static void R_BeamSetup( BEAM *pbeam, vec3_t start, vec3_t end, int modelIndex, float life, float width, float amplitude, float brightness, float speed )
{
	model_t	*sprite = CL_ModelHandle( modelIndex );

	if( !sprite ) return;

	pbeam->type = BEAM_POINTS;
	pbeam->modelIndex = modelIndex;
	pbeam->frame = 0;
	pbeam->frameRate = 0;
	pbeam->frameCount = sprite->numframes;

	VectorCopy( start, pbeam->source );
	VectorCopy( end, pbeam->target );
	VectorSubtract( end, start, pbeam->delta );

	pbeam->freq = speed * gp_cl->time;
	pbeam->die = life + gp_cl->time;
	pbeam->amplitude = amplitude;
	pbeam->brightness = brightness;
	pbeam->width = width;
	pbeam->speed = speed;

	if( amplitude >= 0.50f )
		pbeam->segments = VectorLength( pbeam->delta ) * 0.25f + 3.0f;	// one per 4 pixels
	else pbeam->segments = VectorLength( pbeam->delta ) * 0.075f + 3.0f;		// one per 16 pixels

	pbeam->pFollowModel = NULL;
	pbeam->flags = 0;
}

/*
==============
R_BeamDrawCustomEntity

gl_beams.c:1142. initialize beam from server entity
==============
*/
static void R_BeamDrawCustomEntity( cl_entity_t *ent )
{
	float amp = ent->curstate.body / 100.0f;
	float blend = CL_FxBlend( ent ) / 255.0f;

	float r = ent->curstate.rendercolor.r / 255.0f;
	float g = ent->curstate.rendercolor.g / 255.0f;
	float b = ent->curstate.rendercolor.b / 255.0f;

	BEAM beam;
	/* ref/gl leaves `beam` uninitialised; R_BeamSetup fills every field
	   R_BeamDraw reads except startEntity/endEntity/t/particles, which
	   the switch below or R_BeamDraw set before reading - and nothing at
	   all when the model index is bad (R_BeamSetup returns early), in
	   which case R_BeamDraw reads modelIndex. Zeroed so that case reads
	   index 0 (no model: R_BeamDraw's own "force to ignore") instead of
	   stack garbage. */
	memset( &beam, 0, sizeof( beam ));
	R_BeamSetup( &beam, ent->origin, ent->curstate.angles, ent->curstate.modelindex, 0, ent->curstate.scale, amp, blend, ent->curstate.animtime );
	R_BeamSetAttributes( &beam, r, g, b, ent->curstate.framerate, ent->curstate.frame );
	beam.pFollowModel = NULL;

	switch( ent->curstate.rendermode & 0x0F )
	{
	case BEAM_ENTPOINT:
		beam.type	= TE_BEAMPOINTS;
		if( ent->curstate.sequence )
		{
			SetBits( beam.flags, FBEAM_STARTENTITY );
			beam.startEntity = ent->curstate.sequence;
		}
		if( ent->curstate.skin )
		{
			SetBits( beam.flags, FBEAM_ENDENTITY );
			beam.endEntity = ent->curstate.skin;
		}
		break;
	case BEAM_ENTS:
		beam.type	= TE_BEAMPOINTS;
		SetBits( beam.flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY );
		beam.startEntity = ent->curstate.sequence;
		beam.endEntity = ent->curstate.skin;
		break;
	case BEAM_HOSE:
		beam.type	= TE_BEAMHOSE;
		break;
	case BEAM_POINTS:
		// already set up
		break;
	}

	int beamFlags = ( ent->curstate.rendermode & 0xF0 );

	if( FBitSet( beamFlags, BEAM_FSINE ))
		SetBits( beam.flags, FBEAM_SINENOISE );

	if( FBitSet( beamFlags, BEAM_FSOLID ))
		SetBits( beam.flags, FBEAM_SOLID );

	if( FBitSet( beamFlags, BEAM_FSHADEIN ))
		SetBits( beam.flags, FBEAM_SHADEIN );

	if( FBitSet( beamFlags, BEAM_FSHADEOUT ))
		SetBits( beam.flags, FBEAM_SHADEOUT );

	// draw it
	R_BeamDraw( &beam, RI.frametime );
}


/*
==============
CL_DrawBeams

gl_beams.c:1207. draw beam loop
==============
*/
void CL_DrawBeams( int fTrans, BEAM *active_beams )
{
	// pglShadeModel( GL_SMOOTH ): always Gouraud here (pbkit.c:3170)
	nv2a_2d_desired.depth_mask = fTrans ? false : true;

	// server beams don't allocate beam chains
	// all params are stored in cl_entity_t
	for( int i = 0; i < nv2a_num_beam_entities; i++ )
	{
		cl_entity_t *currentbeam = nv2a_beam_entities[i];
		int flags = currentbeam->curstate.rendermode & 0xF0;

		if( fTrans && FBitSet( flags, FBEAM_SOLID ))
			continue;

		if( !fTrans && !FBitSet( flags, FBEAM_SOLID ))
			continue;

		R_BeamDrawCustomEntity( currentbeam );
		nv2a_efx_stats.c_view_beams_count++;
		NV2A_FT_ADD( XBOX_FT_W_BEAMS, 1 );
	}

	// draw temporary entity beams
	for( BEAM *pBeam = active_beams; pBeam; pBeam = pBeam->next )
	{
		if( fTrans && FBitSet( pBeam->flags, FBEAM_SOLID ))
			continue;

		if( !fTrans && !FBitSet( pBeam->flags, FBEAM_SOLID ))
			continue;

		R_BeamDraw( pBeam, gp_cl->time - gp_cl->oldtime );
	}

	// pglShadeModel( GL_FLAT ): see above
	nv2a_2d_desired.depth_mask = true;
}

/*
==============================================================

PARTICLES AND TRACERS (gl_rpart.c)

==============================================================
*/
static float gTracerSize[11] = { 1.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static color24 gTracerColors[] =
{
{ 255, 255, 255 },		// White
{ 255, 0, 0 },		// Red
{ 0, 255, 0 },		// Green
{ 0, 0, 255 },		// Blue
{ 0, 0, 0 },		// Tracer default, filled in from cvars, etc.
{ 255, 167, 17 },		// Yellow-orange sparks
{ 255, 130, 90 },		// Yellowish streaks (garg)
{ 55, 60, 144 },		// Blue egon streak
{ 255, 130, 90 },		// More Yellowish streaks (garg)
{ 255, 140, 90 },		// More Yellowish streaks (garg)
{ 200, 130, 90 },		// More red streaks (garg)
{ 255, 120, 70 },		// Darker red streaks (garg)
};

/*
================
CL_DrawParticles

gl_rpart.c:47. update particle color, position, free expired and draw it
================
*/
void CL_DrawParticles( double frametime, particle_t *cl_active_particles, float partsize )
{
	if( !cl_active_particles )
		return;	// nothing to draw?

	nv2a_2d_desired.blend_enable = true;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.sfactor = NV2A_SPRITE_SFACTOR_SRC_ALPHA;
	nv2a_2d_desired.dfactor = NV2A_SPRITE_DFACTOR_ONE_MINUS_SRC_ALPHA;

	GL_Bind( XASH_TEXTURE0, nv2a_tex_particle( ));
	// pglTexEnvf( GL_MODULATE ): the TriAPI's only combiner
	nv2a_2d_desired.depth_mask = false;

	Begin( TRI_QUADS );

	for( particle_t *p = cl_active_particles; p; p = p->next )
	{
		if(( p->type != pt_blob ) || ( p->unused == 255 ))
		{
			float size = partsize; // get initial size of particle

			// scale up to keep particles from disappearing
			size += (p->org[0] - RI.vieworigin[0]) * RI.vforward[0];
			size += (p->org[1] - RI.vieworigin[1]) * RI.vforward[1];
			size += (p->org[2] - RI.vieworigin[2]) * RI.vforward[2];

			if( size < 20.0f ) size = partsize;
			else size = partsize + size * 0.002f;

			// scale the axes by radius
			vec3_t right, up;
			VectorScale( RI.vright, size, right );
			VectorScale( RI.vup, size, up );

			p->color = bound( 0, p->color, 255 );
			color24 color = RI.palette[p->color];

			int alpha = 255 * (p->die - gp_cl->time) * 16.0f;
			if( alpha > 255 || p->type == pt_static )
				alpha = 255;

			Color4ub( color.r, color.g, color.b, alpha );

			TexCoord2f( 0.0f, 1.0f );
			Vertex3f( p->org[0] - right[0] + up[0], p->org[1] - right[1] + up[1], p->org[2] - right[2] + up[2] );
			TexCoord2f( 0.0f, 0.0f );
			Vertex3f( p->org[0] + right[0] + up[0], p->org[1] + right[1] + up[1], p->org[2] + right[2] + up[2] );
			TexCoord2f( 1.0f, 0.0f );
			Vertex3f( p->org[0] + right[0] - up[0], p->org[1] + right[1] - up[1], p->org[2] + right[2] - up[2] );
			TexCoord2f( 1.0f, 1.0f );
			Vertex3f( p->org[0] - right[0] - up[0], p->org[1] - right[1] - up[1], p->org[2] - right[2] - up[2] );
			nv2a_efx_stats.c_particle_count++;
			NV2A_FT_ADD( XBOX_FT_W_PARTICLES, 1 );
		}

		gEngfuncs.CL_ThinkParticle( frametime, p );
	}

	End();
	nv2a_2d_desired.depth_mask = true;
}

/*
================
CL_CullTracer

gl_rpart.c:112. check tracer bbox
================
*/
static qboolean CL_CullTracer( particle_t *p, const vec3_t start, const vec3_t end )
{
	vec3_t mins, maxs;

	// compute the bounding box
	for( int i = 0; i < 3; i++ )
	{
		if( start[i] < end[i] )
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		// don't let it be zero sized
		if( mins[i] == maxs[i] )
		{
			maxs[i] += gTracerSize[p->type] * 2.0f;
		}
	}

	// check bbox
	return R_CullBox( mins, maxs );
}

/*
================
CL_DrawTracers

gl_rpart.c:150. update tracer color, position, free expired and draw it
================
*/
void CL_DrawTracers( double frametime, particle_t *cl_active_tracers )
{
	// update tracer color if this is changed
	if( FBitSet( tracerred->flags|tracergreen->flags|tracerblue->flags|traceralpha->flags, FCVAR_CHANGED ))
	{
		color24 *customColors = &gTracerColors[4];
		customColors->r = (byte)(tracerred->value * traceralpha->value * 255);
		customColors->g = (byte)(tracergreen->value * traceralpha->value * 255);
		customColors->b = (byte)(tracerblue->value * traceralpha->value * 255);
		ClearBits( tracerred->flags, FCVAR_CHANGED );
		ClearBits( tracergreen->flags, FCVAR_CHANGED );
		ClearBits( tracerblue->flags, FCVAR_CHANGED );
		ClearBits( traceralpha->flags, FCVAR_CHANGED );
	}

	if( !cl_active_tracers )
		return;	// nothing to draw?

	if( !TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_DOT_SPRITE ), 0 ))
		return;

	R_AllowFog( false );
	nv2a_2d_desired.blend_enable = true;
	nv2a_2d_desired.sfactor = NV2A_SPRITE_SFACTOR_SRC_ALPHA;
	nv2a_2d_desired.dfactor = NV2A_SPRITE_DFACTOR_ONE;
	nv2a_2d_desired.alpha_test_enable = false;
	nv2a_2d_desired.depth_mask = false;

	float gravity = frametime * gp_movevars->gravity;
	float scale = 1.0 - (frametime * 0.9);
	if( scale < 0.0f ) scale = 0.0f;

	Begin( TRI_QUADS );

	for( particle_t *p = cl_active_tracers; p; p = p->next )
	{
		float atten = (p->die - gp_cl->time);
		if( atten > 0.1f ) atten = 0.1f;

		vec3_t start, end, delta;
		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		if( !CL_CullTracer( p, start, end ))
		{
			vec3_t	verts[4], tmp2;
			vec3_t	tmp, normal;
			vec3_t	screen, screenLast;

			// Transform point into screen space
			TriWorldToScreen( start, screen );
			TriWorldToScreen( end, screenLast );

			// build world-space normal to screen-space direction vector
			VectorSubtract( screen, screenLast, tmp );

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize( tmp );

			// build point along noraml line (normal is -y, x)
			VectorScale( RI.vup, tmp[0] * gTracerSize[p->type], normal );
			VectorScale( RI.vright, -tmp[1] * gTracerSize[p->type], tmp2 );
			VectorSubtract( normal, tmp2, normal );

			// compute four vertexes
			VectorSubtract( start, normal, verts[0] );
			VectorAdd( start, normal, verts[1] );
			VectorAdd( verts[0], delta, verts[2] );
			VectorAdd( verts[1], delta, verts[3] );

			if( p->color < 0 || p->color >= sizeof( gTracerColors ) / sizeof( gTracerColors[0] ))
			{
				p->color = TRACER_COLORINDEX_DEFAULT;
			}

			color24 color = gTracerColors[p->color];
			Color4ub( color.r, color.g, color.b, p->unused );

				TexCoord2f( 0.0f, 0.8f );
				Vertex3fv( verts[2] );
				TexCoord2f( 1.0f, 0.8f );
				Vertex3fv( verts[3] );
				TexCoord2f( 1.0f, 0.0f );
				Vertex3fv( verts[1] );
				TexCoord2f( 0.0f, 0.0f );
				Vertex3fv( verts[0] );
		}

		// evaluate position
		VectorMA( p->org, frametime, p->vel, p->org );

		if( p->type == pt_grav )
		{
			p->vel[0] *= scale;
			p->vel[1] *= scale;
			p->vel[2] -= gravity;

			p->unused = 255 * (p->die - gp_cl->time) * 2;
			if( p->unused > 255 ) p->unused = 255;
		}
		else if( p->type == pt_slowgrav )
		{
			p->vel[2] = gravity * 0.05f;
		}
	}
	End();

	nv2a_2d_desired.depth_mask = true;
	R_AllowFog( true );
}
