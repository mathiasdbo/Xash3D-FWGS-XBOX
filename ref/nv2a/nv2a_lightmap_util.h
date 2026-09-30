/*
nv2a_lightmap_util.h - X9-X step 8: the pure-arithmetic half of ref/nv2a's
animated lightstyles and dynamic lights (r_lightmap.c), factored out so it
builds and is unit-tested on the host without pbkit/xboxkrnl/engine
includes - the same split nv2a_efx_util.h and nv2a_sprite_util.h use.

What it ports, from ref/gl/gl_rsurf.c (root clone):

- R_BuildLightMap (gl_rsurf.c:725): every style's samples times its
  g_lightstylevalue, summed into r_blocklights, then written out as
  texels.
- R_AddDynamicLights (gl_rsurf.c:569): the per-luxel falloff of one
  dynamic light, from the point where the light's impact on the plane is
  known.
- R_CheckLightMap's (gl_rsurf.c:1400) "did any style change since this
  block was built" test against msurface_t::cached_light.

Where it deliberately differs from R_BuildLightMap (divergences.md #134):
the texel is not `LightToTexGamma( min( bl * lightscale >> 14, 1023 )) >> 2`
but `min( bl >> 8, 255 )` over styles rescaled so that the normal style
value (264, 'm') gives back the sample byte itself. That is the value
the baker already wrote into the atlas for style 0
(tools/asset-studio/src/formats/LightmapAtlasPacker.h writeBlock copies
the LUMP_LIGHTING bytes verbatim), and ps_lit doubles the lightmap
(the gl_overbright convention), so a face rebuilt here matches the baked
faces around it byte for byte instead of coming out 3% brighter (264/256)
with a gamma curve the rest of the world does not have. Dynamic lights
keep their reference units (bl >> 8 is ref/gl's own bl * 256 >> 14 >> 2).
*/
#ifndef NV2A_LIGHTMAP_UTIL_H
#define NV2A_LIGHTMAP_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAXLIGHTMAPS (common/com_model.h / bspfile.h): four style slots per
   face, the first 255 ending the list. */
#define NV2A_LM_STYLE_SLOTS 4
#define NV2A_LM_NO_STYLE    255

/* The baker's LM_MAX_LUXELS (LightmapAtlasPacker.h:24): no baked block is
   wider or taller than this, so a block of this size bounds the scratch
   r_blocklights needs. */
#define NV2A_LM_MAX_LUXELS_PER_AXIS 64
#define NV2A_LM_MAX_LUXELS ( NV2A_LM_MAX_LUXELS_PER_AXIS * NV2A_LM_MAX_LUXELS_PER_AXIS )

/* 'm' (normal light) * 22, CL_RunLightStyles (ref_light.c:60,68): the
   value the baked atlas's style-0 bytes stand for. */
#define NV2A_LM_NORMAL_STYLE_VALUE 264

/* The largest style value nv2a_lm_normalized_scale takes as is: value * 256
   + 132 still fits an int. */
#define NV2A_LM_MAX_STYLE_VALUE (( 0x7FFFFFFF - NV2A_LM_NORMAL_STYLE_VALUE / 2 ) / 256 )

/* A g_lightstylevalue rescaled so that NV2A_LM_NORMAL_STYLE_VALUE maps to
   256 (see the file header). Rounded to nearest; negative input is 0,
   input past NV2A_LM_MAX_STYLE_VALUE is clamped to it. */
int nv2a_lm_normalized_scale( int stylevalue );

/* R_CheckLightMap's per-slot test (gl_rsurf.c:1411-1414): non-zero when
   any used style slot's current value differs from the one the block was
   last built with. `values` is g_lightstylevalue (indexed by style). */
int nv2a_lm_styles_changed( const uint8_t styles[NV2A_LM_STYLE_SLOTS], const int cached[NV2A_LM_STYLE_SLOTS], const int *values );

/* R_UpdateSurfaceCachedLight (ref_light.c:692): what cached_light holds
   after a build with the current values. Slots past the last style are
   left alone, as upstream leaves them. */
void nv2a_lm_update_cached( const uint8_t styles[NV2A_LM_STYLE_SLOTS], int cached[NV2A_LM_STYLE_SLOTS], const int *values );

/* What cached_light must say for a block that still holds the baked
   bytes: style slot 0 at the normal value, every later slot at 0 - the
   baked Xbox atlas carries style slot 0 only (BspParser.cpp:597-598,
   lmExtraStyles is false for the Xbox target), which is exactly what a
   build with those values would write. */
void nv2a_lm_baked_cached( const uint8_t styles[NV2A_LM_STYLE_SLOTS], int cached[NV2A_LM_STYLE_SLOTS] );

/* R_BuildLightMap's first half (gl_rsurf.c:741-759): zeroes size*3
   accumulators, then adds every used style slot's samples (RGB bytes,
   `size` per slot, slots back to back as LUMP_LIGHTING stores them) times
   normalized_scale[style]. `samples` NULL adds nothing (the reference's
   `&& surf->samples` loop condition). */
void nv2a_lm_accumulate_styles( uint32_t *bl, const uint8_t *samples, int size,
	const uint8_t styles[NV2A_LM_STYLE_SLOTS], const int *normalized_scale );

/* R_AddDynamicLights' falloff (gl_rsurf.c:607-660) for one light, given
   what the caller computed from the light and the surface: `rad` is
   dl->radius minus the light's distance to the plane, `minlight` is
   dl->minlight, (sl, tl) the impact point in lightmap space already minus
   lightmapmins. Returns 0 when the light is too dim to reach the plane
   (the reference's `continue`). sample_frac stays an int, as upstream
   declares it (`int sample_frac = 1.0;`). */
int nv2a_lm_add_dlight( uint32_t *bl, int smax, int tmax, float sample_size, int sample_frac,
	float sl, float tl, float rad, float minlight, uint8_t r, uint8_t g, uint8_t b );

/* One texel of R_BuildLightMap's second half, with this file's scale:
   min( bl >> 8, 255 ). */
static inline uint32_t nv2a_lm_texel( uint32_t bl )
{
	bl >>= 8;
	return bl > 255u ? 255u : bl;
}

/* Writes the smax x tmax block as the atlas stores it (RGBA8, bytes
   R,G,B,A - one little-endian word R | G<<8 | B<<16 | 0xFF<<24, alpha 255
   as R_BuildLightMap's dst[3] = 255), `stride_words` words per row. Writes
   whole words in address order, which is what write-combined GPU memory
   wants. */
void nv2a_lm_pack_block( uint32_t *dst, uint32_t stride_words, const uint32_t *bl, int smax, int tmax );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_LIGHTMAP_UTIL_H */
