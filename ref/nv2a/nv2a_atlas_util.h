/*
nv2a_atlas_util.h - the menu's cached glyph atlases uploaded straight from
their TGA (docs/r3d/divergences.md #157)
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

/*
The font cache (3rdparty/mainui font/AtlasTGA.h, #152) hands PIC_Load
each glyph atlas as one of two small TGAs: type 1, 8 bits a texel, with
the 32-bit colour map entry i = (255, 255, 255, i); or type 3, 16 bits a
texel, grey then alpha. The general path decodes either to RGBA32
(Image_LoadTGA, engine/common/imagelib/img_tga.c), and nv2a_tex_upload
classifies every texel and packs it back to LU_IMAGE_A8 or A8Y8 (#131).
For exactly these two shapes the result is known from the TGA bytes:

- type 1 with that colour map decodes every texel to (255, 255, 255, i)
  (img_tga.c:169-180 in the reference), so it classifies as white-alpha
  and its A8 bytes are the index bytes;
- type 3, 16 bits, decodes to (g, g, g, a) (:196-208): white-alpha (A8:
  the a bytes) when every g is 255, else grey-alpha, whose A8Y8 bytes are
  g then a - the TGA's own byte pairs.

IMAGE_HAS_ALPHA is set when some alpha is not 255 (:179, :204), which is
all nv2a_tex_process_image reads from the decode for these flags. Rows
are stored bottom first (attribute bit 5 clear: the else branch at
:143-147), so row y of the texture is stored row height - 1 - y.

Pure arithmetic, no engine headers, so tools/host-tests can build it
alone (nv2a_atlas_util_host_test.cpp checks it against a transcription of
the general path).
*/
#ifndef NV2A_ATLAS_UTIL_H
#define NV2A_ATLAS_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_ATLAS_TGA_HEADER_BYTES   18u
#define NV2A_ATLAS_TGA_COLORMAP_BYTES ( 256u * 4u )

typedef struct
{
	const uint8_t *rows;  /* the first stored row: the image's bottom row */
	uint32_t width, height;
	uint32_t src_bpp;     /* 1 (type 1) or 2 (type 3) bytes a stored texel */
	uint32_t dst_bpp;     /* 1: LU_IMAGE_A8; 2: LU_IMAGE_A8Y8 */
	int      has_alpha;   /* some alpha is not 255: Image_LoadTGA's IMAGE_HAS_ALPHA */
} nv2a_atlas_tga_t;

/* Fills `out` and returns 1 when buf[0..size) is exactly one of the two
   shapes above with both sides in 1..max_side and every texel present;
   returns 0 (out untouched) for anything else, which then takes the
   general path. Accepted: id length 0; attribute bit 5 clear; type 1 with
   colour map type 1, index 0, 256 entries of 32 bits, 8-bit texels and
   the identity map; type 3 with colour map type 0 and 16-bit texels. */
int nv2a_atlas_tga_probe( const uint8_t *buf, size_t size, uint32_t max_side, nv2a_atlas_tga_t *out );

/* Writes the texture: row y at dst + y * pitch, width * dst_bpp bytes;
   the bytes between that and `pitch` are not written (nor are they by
   nv2a_tex_upload). Never reads `dst` (write-combined memory). */
void nv2a_atlas_tga_copy( const nv2a_atlas_tga_t *a, uint8_t *dst, uint32_t pitch );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_ATLAS_UTIL_H */
