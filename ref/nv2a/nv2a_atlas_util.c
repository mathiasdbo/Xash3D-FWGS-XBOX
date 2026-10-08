/*
nv2a_atlas_util.c - see nv2a_atlas_util.h (docs/r3d/divergences.md #157)
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include <string.h>
#include "nv2a_atlas_util.h"

static uint32_t nv2a_atlas_le16( const uint8_t *p )
{
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 );
}

/* The colour map AtlasTGA_Write emits: entry i is B, G, R = 255, A = i. */
static int nv2a_atlas_identity_colormap( const uint8_t *map )
{
	uint32_t i;

	for( i = 0; i < 256; i++, map += 4 )
	{
		if( map[0] != 255 || map[1] != 255 || map[2] != 255 || map[3] != i )
			return 0;
	}
	return 1;
}

int nv2a_atlas_tga_probe( const uint8_t *buf, size_t size, uint32_t max_side, nv2a_atlas_tga_t *out )
{
	uint32_t width, height, src_bpp;
	size_t header, texels, i;
	const uint8_t *rows;
	int has_alpha = 0, all_white = 1;

	if( !buf || size < NV2A_ATLAS_TGA_HEADER_BYTES )
		return 0;

	width = nv2a_atlas_le16( &buf[12] );
	height = nv2a_atlas_le16( &buf[14] );

	/* id length 0 (the texels follow the header and colour map); bit 5
	   clear (bottom row first, the only order handled here) */
	if( buf[0] != 0 || ( buf[17] & 0x20 ) || width == 0 || height == 0 || width > max_side || height > max_side )
		return 0;

	if( buf[2] == 1 && buf[1] == 1 && nv2a_atlas_le16( &buf[3] ) == 0 && nv2a_atlas_le16( &buf[5] ) == 256
		&& buf[7] == 32 && buf[16] == 8 )
	{
		src_bpp = 1;
		header = NV2A_ATLAS_TGA_HEADER_BYTES + NV2A_ATLAS_TGA_COLORMAP_BYTES;
	}
	else if( buf[2] == 3 && buf[1] == 0 && buf[16] == 16 )
	{
		src_bpp = 2;
		header = NV2A_ATLAS_TGA_HEADER_BYTES;
	}
	else
		return 0;

	/* width, height <= 65535: no overflow in a 32-bit size_t for a
	   max_side up to 16384 (the caller passes 1024) */
	texels = (size_t)width * height;
	if( size < header || size - header < texels * src_bpp )
		return 0;

	if( src_bpp == 1 && !nv2a_atlas_identity_colormap( &buf[NV2A_ATLAS_TGA_HEADER_BYTES] ))
		return 0;

	rows = buf + header;

	/* Image_LoadTGA's IMAGE_HAS_ALPHA, and for type 3 the classifier's
	   white test (every R = G = B = 255). Both stop at the first texel
	   that settles them: a glyph atlas starts with transparent padding. */
	if( src_bpp == 1 )
	{
		for( i = 0; i < texels; i++ )
		{
			if( rows[i] != 255 )
			{
				has_alpha = 1;
				break;
			}
		}
	}
	else
	{
		for( i = 0; i < texels && ( all_white || !has_alpha ); i++ )
		{
			if( rows[i * 2] != 255 )
				all_white = 0;
			if( rows[i * 2 + 1] != 255 )
				has_alpha = 1;
		}
	}

	out->rows = rows;
	out->width = width;
	out->height = height;
	out->src_bpp = src_bpp;
	out->dst_bpp = ( src_bpp == 1 || all_white ) ? 1 : 2;
	out->has_alpha = has_alpha;
	return 1;
}

void nv2a_atlas_tga_copy( const nv2a_atlas_tga_t *a, uint8_t *dst, uint32_t pitch )
{
	const size_t src_row = (size_t)a->width * a->src_bpp;
	uint32_t y, x;

	for( y = 0; y < a->height; y++ )
	{
		const uint8_t *src = a->rows + (size_t)( a->height - 1 - y ) * src_row;
		uint8_t *out = dst + (size_t)y * pitch;

		if( a->src_bpp == a->dst_bpp )
			memcpy( out, src, src_row ); /* A8 from type 1, A8Y8 from type 3 */
		else
		{
			/* type 3 with every grey 255: A8, the alpha of each pair.
			   AtlasTGA_Write never makes one (it writes such an atlas as
			   type 1); kept so any type 3 file matches the general path. */
			for( x = 0; x < a->width; x++ )
				out[x] = src[x * 2 + 1];
		}
	}
}
