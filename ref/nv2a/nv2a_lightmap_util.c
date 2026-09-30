/*
nv2a_lightmap_util.c - X9-X step 8: host-testable lightmap arithmetic for
ref/nv2a (see nv2a_lightmap_util.h for what is ported and where it
deliberately differs). No engine, pbkit or xboxkrnl dependency.
*/
#include "nv2a_lightmap_util.h"

int nv2a_lm_normalized_scale( int stylevalue )
{
	if( stylevalue <= 0 )
		return 0;

	/* 32-bit on purpose (a 64-bit divide is a libcall on i386, the
	   constant 32-bit one a multiply): clamped where value * 256 would
	   overflow - far past r_fullbright's 256 * 256 (ref_light.c:41), and
	   any texel is saturated long before. */
	if( stylevalue > NV2A_LM_MAX_STYLE_VALUE )
		stylevalue = NV2A_LM_MAX_STYLE_VALUE;

	return ( stylevalue * 256 + NV2A_LM_NORMAL_STYLE_VALUE / 2 ) / NV2A_LM_NORMAL_STYLE_VALUE;
}

/* R_CheckLightMap, gl_rsurf.c:1410-1414. */
int nv2a_lm_styles_changed( const uint8_t styles[NV2A_LM_STYLE_SLOTS], const int cached[NV2A_LM_STYLE_SLOTS], const int *values )
{
	int maps;

	for( maps = 0; maps < NV2A_LM_STYLE_SLOTS && styles[maps] != NV2A_LM_NO_STYLE; maps++ )
	{
		if( values[styles[maps]] != cached[maps] )
			return 1;
	}

	return 0;
}

/* R_UpdateSurfaceCachedLight, ref_light.c:692-696. */
void nv2a_lm_update_cached( const uint8_t styles[NV2A_LM_STYLE_SLOTS], int cached[NV2A_LM_STYLE_SLOTS], const int *values )
{
	int i;

	for( i = 0; i < NV2A_LM_STYLE_SLOTS && styles[i] != NV2A_LM_NO_STYLE; i++ )
		cached[i] = values[styles[i]];
}

void nv2a_lm_baked_cached( const uint8_t styles[NV2A_LM_STYLE_SLOTS], int cached[NV2A_LM_STYLE_SLOTS] )
{
	int i;

	for( i = 0; i < NV2A_LM_STYLE_SLOTS && styles[i] != NV2A_LM_NO_STYLE; i++ )
		cached[i] = ( i == 0 ) ? NV2A_LM_NORMAL_STYLE_VALUE : 0;
}

/* R_BuildLightMap, gl_rsurf.c:741-759: "add all the lightmaps". The
   memset + add of the reference is folded into the first style's pass
   (assign, not add) - same sums. Written for the Xbox objects, which are
   built at clang's default -O0 (tools/xbox-cc.sh passes no -O): one RGB
   triple per iteration and no calls in the loop (pdclib's memset is a
   byte loop, and -fno-builtin keeps it a call). */
void nv2a_lm_accumulate_styles( uint32_t *bl, const uint8_t *samples, int size,
	const uint8_t styles[NV2A_LM_STYLE_SLOTS], const int *normalized_scale )
{
	uint32_t *out;
	const uint8_t *lm;
	int map, i;

	for( map = 0; map < NV2A_LM_STYLE_SLOTS && samples; map++ )
	{
		uint32_t scale;

		if( styles[map] >= NV2A_LM_NO_STYLE )
			break;

		scale = (uint32_t)normalized_scale[styles[map]];
		lm = samples + (size_t)map * (size_t)size * 3;
		out = bl;

		if( map == 0 )
		{
			for( i = 0; i < size; i++, out += 3, lm += 3 )
			{
				out[0] = lm[0] * scale;
				out[1] = lm[1] * scale;
				out[2] = lm[2] * scale;
			}
		}
		else
		{
			for( i = 0; i < size; i++, out += 3, lm += 3 )
			{
				out[0] += lm[0] * scale;
				out[1] += lm[1] * scale;
				out[2] += lm[2] * scale;
			}
		}
	}

	if( map == 0 )
	{
		/* no samples or no style: nothing added, but still zeroed */
		for( i = 0, out = bl; i < size * 3; i++ )
			out[i] = 0;
	}
}

/* R_AddDynamicLights, gl_rsurf.c:607-660, from "rad is now the highest
   intensity on the plane" on. The integer/float mix is upstream's own. */
int nv2a_lm_add_dlight( uint32_t *bl, int smax, int tmax, float sample_size, int sample_frac,
	float sl, float tl, float rad, float minlight, uint8_t r, uint8_t g, uint8_t b )
{
	float half;
	int s0, s1, t0, t1, s, t;

	if( rad < minlight )
		return 0;

	minlight = rad - minlight;

	// dist >= max( sd, td ), so a luxel can only pass the test below when both
	// sd and td are under minlight
	half = ( minlight + 1.0f ) / ( sample_size * sample_frac );
	s0 = (int)( sl / sample_size - half );
	if( s0 < 0 ) s0 = 0;
	s1 = (int)( sl / sample_size + half );
	if( s1 > smax - 1 ) s1 = smax - 1;
	t0 = (int)( tl / sample_size - half );
	if( t0 < 0 ) t0 = 0;
	t1 = (int)( tl / sample_size + half );
	if( t1 > tmax - 1 ) t1 = tmax - 1;

	for( t = t0; t <= t1; t++ )
	{
		int td = (int)(( tl - sample_size * t ) * sample_frac );

		if( td < 0 )
			td = -td;

		for( s = s0; s <= s1; s++ )
		{
			int sd = (int)(( sl - sample_size * s ) * sample_frac );
			float dist;

			if( sd < 0 )
				sd = -sd;

			if( sd > td )
				dist = (float)( sd + ( td >> 1 ));
			else
				dist = (float)( td + ( sd >> 1 ));

			if( dist < minlight )
			{
				uint32_t *p = &bl[( s + ( t * smax )) * 3];
				const int amount = (int)(( rad - dist ) * 256 );

				p[0] += ( amount * r ) / 256;
				p[1] += ( amount * g ) / 256;
				p[2] += ( amount * b ) / 256;
			}
		}
	}

	return 1;
}

/* nv2a_lm_texel, as a macro: an inline function is still a call at -O0. */
#define NV2A_LM_TEXEL( v ) ((( v ) >> 8 ) > 255u ? 255u : (( v ) >> 8 ))

/* R_BuildLightMap, gl_rsurf.c:765-791: "Put into texture format". */
void nv2a_lm_pack_block( uint32_t *dst, uint32_t stride_words, const uint32_t *bl, int smax, int tmax )
{
	int s, t;

	for( t = 0; t < tmax; t++, dst += stride_words )
	{
		for( s = 0; s < smax; s++, bl += 3 )
		{
			const uint32_t r = NV2A_LM_TEXEL( bl[0] );
			const uint32_t g = NV2A_LM_TEXEL( bl[1] );
			const uint32_t b = NV2A_LM_TEXEL( bl[2] );

			dst[s] = r | ( g << 8 ) | ( b << 16 ) | 0xFF000000u;
		}
	}
}
