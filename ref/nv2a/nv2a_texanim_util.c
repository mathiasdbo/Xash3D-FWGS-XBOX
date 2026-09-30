/*
nv2a_texanim_util.c - X9-X item 14 step 9, see nv2a_texanim_util.h.
*/
#include "nv2a_texanim_util.h"
#include <string.h>

int nv2a_texanim_ticks( double time )
{
	/* (int)(gp_cl->time * speed), gl_rsurf.c:549. The client clock is
	   seconds since the map started; clamp only what an int cannot hold,
	   which the reference would leave undefined. */
	double t = time * NV2A_TEXANIM_SPEED;

	if( t >= 2147483647.0 )
		return 2147483647;
	if( t <= -2147483648.0 )
		return (int)-2147483647 - 1;
	return (int)t;
}

int nv2a_texanim_name_animated( const char *name )
{
	if( !name )
		return 0;
	if( name[0] != '-' && name[0] != '+' )
		return 0;
	return ( name[1] >= '0' && name[1] <= '9' ) || ( name[1] >= 'a' && name[1] <= 'j' );
}

/* The texture the walk starts from: base, or its alternate set when the
   entity's frame is non-zero (gl_rsurf.c:522-526). */
static uint32_t nv2a_texanim_start( const nv2a_texanim_t *tab, uint32_t count, uint32_t base, int alternate )
{
	if( alternate && tab[base].alternate < count )
		return tab[base].alternate;
	return base;
}

int nv2a_texanim_per_surface( const nv2a_texanim_t *tab, uint32_t count, uint32_t base, int alternate )
{
	uint32_t cur;

	if( !tab || base >= count )
		return 0;
	cur = nv2a_texanim_start( tab, count, base, alternate );
	return tab[cur].anim_total && tab[cur].random;
}

uint32_t nv2a_texanim_resolve( const nv2a_texanim_t *tab, uint32_t count, uint32_t base, int alternate, int ticks,
	int has_surface, int texturemins_s, int texturemins_t, const uint16_t rtable[NV2A_TEXANIM_MOD_FRAMES][NV2A_TEXANIM_MOD_FRAMES] )
{
	uint32_t cur;
	int reletive;
	int steps = 0;

	if( !tab || base >= count )
		return base;

	cur = nv2a_texanim_start( tab, count, base, alternate );

	if( !tab[cur].anim_total )
		return cur; /* gl_rsurf.c:528 */

	if( tab[cur].random )
	{
		uint32_t width = tab[cur].width, height = tab[cur].height;
		int tx, ty;

		/* R_TextureAnim: "already tiled", returns b (gl_rsurf.c:478-481). */
		if( !has_surface || !rtable )
			return base;
		/* The reference divides by base->width and base->height unguarded;
		   a zero-sized texture cannot be tiled, so it stays. */
		if( !width || !height )
			return base;
		/* gl_rsurf.c:533-534: texturemins is a short promoted to int and
		   base->width << 16 is unsigned, so the sum and the division are
		   unsigned - a negative texturemins wraps and the << 16 term
		   brings it back into range, as in the reference. */
		tx = (int)(( (uint32_t)texturemins_s + ( width << 16 )) / width ) % NV2A_TEXANIM_MOD_FRAMES;
		ty = (int)(( (uint32_t)texturemins_t + ( height << 16 )) / height ) % NV2A_TEXANIM_MOD_FRAMES;
		reletive = rtable[tx][ty] % tab[cur].anim_total; /* gl_rsurf.c:536 */
	}
	else
	{
		reletive = ticks % tab[cur].anim_total; /* gl_rsurf.c:549 */
	}

	/* gl_rsurf.c:552-559 */
	while( tab[cur].anim_min > reletive || tab[cur].anim_max <= reletive )
	{
		cur = tab[cur].anim_next;
		if( cur >= count || ++steps > NV2A_TEXANIM_MOD_FRAMES )
			return base;
	}
	return cur;
}

uint32_t nv2a_texanim_extra_frames( const char *const *names, const uint8_t *used, uint32_t count, uint16_t *out, uint32_t out_cap )
{
	uint32_t i, j, n = 0;

	if( !names || !used )
		return 0;

	for( j = 0; j < count; j++ )
	{
		if( used[j] || !nv2a_texanim_name_animated( names[j] ))
			continue;
		for( i = 0; i < count; i++ )
		{
			if( !used[i] || !nv2a_texanim_name_animated( names[i] ))
				continue;
			/* Q_strcmp( altTexture->name + 2, baseTexture->name + 2 ),
			   mod_bmodel.c:3095 */
			if( strcmp( names[i] + 2, names[j] + 2 ) == 0 )
			{
				if( n < out_cap && out )
					out[n] = (uint16_t)j;
				n++;
				break;
			}
		}
	}
	return n;
}
