/*
mainui_atlas_tga_host_test.cpp - host test for 3rdparty/mainui/font/AtlasTGA.h

Resonance3D (docs/r3d/divergences.md #152): the font cache stores a glyph
atlas as an 8-, 16- or 32-bit TGA instead of upstream's 32-bit BMP. The
claim that makes this safe is that the engine decodes the TGA to exactly
the RGBA32 texels and image flags it decoded from the BMP. This test
encodes atlases of every class, decodes each TGA the way
engine/common/imagelib/img_tga.c Image_LoadTGA does and the 32-bit BMP
the way img_bmp.c Image_LoadBMP does (both transcribed below, for the
paths these files take), and compares texels and flags; it also checks
the cache reader's header and size validation.

Build and run from the repository root (tools/host-tests/run.sh):

  g++ -O2 -fsanitize=undefined -fsanitize=float-cast-overflow \
    -fsanitize-undefined-trap-on-error -Wall -std=c++11 \
    -I3rdparty/mainui/font tools/host-tests/mainui_atlas_tga_host_test.cpp \
    -o .sweep/host-tests/mainui_atlas_tga_test && .sweep/host-tests/mainui_atlas_tga_test
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "AtlasTGA.h"

#define IMAGE_HAS_ALPHA 1
#define IMAGE_HAS_COLOR 2

static int failures;

#define CHECK( cond, ... ) \
	do { if( !( cond )) { failures++; printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } while( 0 )

struct decoded_t
{
	std::vector<uint8_t> rgba; // top row first, as imagelib's buffer
	int flags;
	bool ok;
};

/* Image_LoadTGA (img_tga.c), uncompressed types 1, 2 and 3, attribute
   bit 5 clear and IL_DONTFLIP_TGA unset: the first stored row is the
   image's last. Reference lines (engine/common/imagelib/img_tga.c,
   identical to xash3d-fwgs): type checks and colour map :68-129, row
   order :138-147, the texel loop :150-231, alpha flags :179/:193/:204,
   colour flag :212-213. PIC_Load's '#' name and buffer take the buffer
   path and pick the loader by extension (img_main.c:416, :423, :470). */
static decoded_t DecodeTGA( const uint8_t *buf, size_t size )
{
	decoded_t d;
	d.flags = 0;
	d.ok = false;
	if( size < 18 )
		return d;

	const uint8_t *p = buf;
	const int id_length = p[0], image_type = p[2];
	const int colormap_length = p[5] | ( p[6] << 8 ), colormap_size = p[7];
	const int columns = p[12] | ( p[13] << 8 ), rows = p[14] | ( p[15] << 8 );
	const int pixel_size = p[16];
	uint8_t palette[256][4];
	p += 18 + id_length;

	if( image_type == 1 )
	{
		if( pixel_size != 8 || colormap_length != 256 || colormap_size != 32 )
			return d;
		for( int i = 0; i < 256; i++ )
		{
			palette[i][2] = *p++;
			palette[i][1] = *p++;
			palette[i][0] = *p++;
			palette[i][3] = *p++;
		}
	}

	d.rgba.resize( (size_t)columns * rows * 4 );
	for( int row = rows - 1; row >= 0; row-- )
	{
		uint8_t *out = &d.rgba[(size_t)row * columns * 4];
		for( int col = 0; col < columns; col++ )
		{
			uint8_t red = 0, green = 0, blue = 0, alpha = 0;
			switch( image_type )
			{
			case 1:
				blue = *p++;
				red = palette[blue][0];
				green = palette[blue][1];
				alpha = palette[blue][3];
				blue = palette[blue][2];
				if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
				break;
			case 2:
				blue = *p++;
				green = *p++;
				red = *p++;
				alpha = 255;
				if( pixel_size == 32 )
				{
					alpha = *p++;
					if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
				}
				break;
			case 3:
				blue = green = red = *p++;
				if( pixel_size == 16 )
				{
					alpha = *p++;
					if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
				}
				else alpha = 255;
				break;
			default:
				return d;
			}
			if( red != green || green != blue )
				d.flags |= IMAGE_HAS_COLOR;
			*out++ = red;
			*out++ = green;
			*out++ = blue;
			*out++ = alpha;
		}
	}
	d.ok = (size_t)( p - buf ) == size;
	return d;
}

/* The 32-bit BMP upstream's cache held (CBMP: 54-byte header, rows bottom
   first, B G R A) as Image_LoadBMP (img_bmp.c) decodes it: the first
   stored row is the image's last (the row loop, :223), `case 32`
   (:319-329), colour flag :336-337. */
static decoded_t DecodeBMP32( const uint8_t *bottomUpRGBA, int columns, int rows )
{
	decoded_t d;
	d.flags = 0;
	d.ok = true;
	d.rgba.resize( (size_t)columns * rows * 4 );
	const uint8_t *p = bottomUpRGBA;
	for( int row = rows - 1; row >= 0; row-- )
	{
		uint8_t *out = &d.rgba[(size_t)row * columns * 4];
		for( int col = 0; col < columns; col++, p += 4 )
		{
			// stored B G R A; the CBMP canvas holds R G B A, which
			// mainui wrote straight into the BMP's pixel bytes - so the
			// BMP loader read R as blue and B as red. Only R = G = B atlases
			// reach the 8- and 16-bit forms, where that swap is invisible;
			// for the 32-bit form the TGA writer keeps the same order.
			const uint8_t blue = p[0], green = p[1], red = p[2], alpha = p[3];
			*out++ = red;
			*out++ = green;
			*out++ = blue;
			*out++ = alpha;
			if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
			if( red != green || green != blue ) d.flags |= IMAGE_HAS_COLOR;
		}
	}
	return d;
}

static uint32_t rng = 12345;
static uint8_t Rand8( void )
{
	rng = rng * 1103515245u + 12345u;
	return (uint8_t)( rng >> 16 );
}

enum kind_e { KIND_WHITE, KIND_GREY, KIND_COLOR, KIND_OPAQUE_WHITE };

static std::vector<uint8_t> MakeAtlas( kind_e kind, int w, int h )
{
	std::vector<uint8_t> a( (size_t)w * h * 4 );
	for( size_t i = 0; i < (size_t)w * h; i++ )
	{
		uint8_t *t = &a[i * 4];
		const uint8_t g = Rand8();
		switch( kind )
		{
		case KIND_WHITE: t[0] = t[1] = t[2] = 255; t[3] = ( i % 7 ) ? Rand8() : 0; break;
		case KIND_OPAQUE_WHITE: t[0] = t[1] = t[2] = 255; t[3] = 255; break;
		case KIND_GREY: t[0] = t[1] = t[2] = ( i % 5 == 1 ) ? 255 : ( g == 255 ? 254 : g ); t[3] = ( i % 3 ) ? Rand8() : 0; break;
		case KIND_COLOR: t[0] = g; t[1] = Rand8(); t[2] = Rand8(); t[3] = Rand8(); break;
		}
	}
	return a;
}

static void TestRoundTrip( kind_e kind, int w, int h, atlasTgaForm_e expect )
{
	std::vector<uint8_t> atlas = MakeAtlas( kind, w, h );
	const atlasTgaForm_e form = AtlasTGA_Classify( atlas.data(), (size_t)w * h );
	CHECK( form == expect, "kind %d %dx%d: form %d, expected %d", kind, w, h, form, expect );

	const size_t size = AtlasTGA_Size( form, w, h );
	std::vector<uint8_t> tga( size + 16, 0xCD );
	AtlasTGA_Write( tga.data(), form, atlas.data(), w, h );
	CHECK( tga[size] == 0xCD, "kind %d: wrote past AtlasTGA_Size", kind );
	CHECK( AtlasTGA_CheckedSize( tga.data(), size ) == size, "kind %d: CheckedSize of its own output", kind );
	CHECK( AtlasTGA_CheckedSize( tga.data(), size + 16 ) == size, "kind %d: CheckedSize with trailing bytes", kind );
	CHECK( AtlasTGA_CheckedSize( tga.data(), size - 1 ) == 0, "kind %d: a short file passed", kind );

	// the canvas mainui hands both writers: R G B A bytes, bottom row first
	const decoded_t viaTga = DecodeTGA( tga.data(), size );
	const decoded_t viaBmp = DecodeBMP32( atlas.data(), w, h );
	CHECK( viaTga.ok, "kind %d: TGA did not decode to its exact size", kind );
	CHECK( viaTga.flags == viaBmp.flags, "kind %d: flags %d via TGA, %d via BMP", kind, viaTga.flags, viaBmp.flags );
	CHECK( viaTga.rgba == viaBmp.rgba, "kind %d %dx%d: texels differ between the TGA and the BMP", kind, w, h );
}

static void TestRejects( void )
{
	const int w = 8, h = 4;
	std::vector<uint8_t> atlas = MakeAtlas( KIND_WHITE, w, h );
	const size_t size = AtlasTGA_Size( ATLAS_TGA_WHITE_ALPHA, w, h );
	std::vector<uint8_t> good( size );
	AtlasTGA_Write( good.data(), ATLAS_TGA_WHITE_ALPHA, atlas.data(), w, h );

	struct { int offset; uint8_t value; const char *what; } bad[] =
	{
		{ 0, 1, "an image id" },
		{ 1, 0, "no colour map on type 1" },
		{ 2, 9, "RLE type 9" },
		{ 2, 10, "RLE type 10" },
		{ 3, 1, "colour map index 1" },
		{ 5, 255, "colour map of 255 entries" },
		{ 7, 24, "24-bit colour map" },
		{ 12, 0, "zero width" },
		{ 16, 16, "16-bit texels on type 1" },
		{ 17, 0x28, "top-left origin" },
	};
	for( size_t i = 0; i < sizeof( bad ) / sizeof( bad[0] ); i++ )
	{
		std::vector<uint8_t> t = good;
		t[bad[i].offset] = bad[i].value;
		if( bad[i].offset == 12 )
			t[13] = 0;
		CHECK( AtlasTGA_CheckedSize( t.data(), t.size() ) == 0, "accepted %s", bad[i].what );
	}

	CHECK( AtlasTGA_CheckedSize( good.data(), 17 ) == 0, "accepted a truncated header" );

	// a header whose size would wrap a 32-bit size_t (65535 x 65535 x 4)
	// must be refused by the side cap, not by luck
	{
		std::vector<uint8_t> t = good;
		t[2] = ATLAS_TGA_RGBA; t[1] = 0; t[5] = t[6] = t[7] = 0; t[16] = 32;
		t[12] = t[13] = t[14] = t[15] = 0xFF;
		CHECK( AtlasTGA_CheckedSize( t.data(), (size_t)-1 ) == 0, "accepted a 65535x65535 header" );
		t[12] = 0x01; t[13] = 0x20; t[14] = 4; t[15] = 0; // 8193 wide
		CHECK( AtlasTGA_CheckedSize( t.data(), (size_t)-1 ) == 0, "accepted a side over ATLAS_TGA_MAX_SIDE" );
	}

	// a white-alpha atlas with one grey texel is grey, one coloured texel RGBA
	atlas[5 * 4 + 0] = atlas[5 * 4 + 1] = atlas[5 * 4 + 2] = 254;
	CHECK( AtlasTGA_Classify( atlas.data(), w * h ) == ATLAS_TGA_GREY_ALPHA, "one grey texel kept the atlas white" );
	atlas[6 * 4 + 1] = 0;
	CHECK( AtlasTGA_Classify( atlas.data(), w * h ) == ATLAS_TGA_RGBA, "one coloured texel kept the atlas grey" );
}

int main( void )
{
	static const int sizes[][2] = { { 1, 1 }, { 4, 1 }, { 256, 256 }, { 256, 512 }, { 512, 512 }, { 12, 7 } };

	for( size_t i = 0; i < sizeof( sizes ) / sizeof( sizes[0] ); i++ )
	{
		const int w = sizes[i][0], h = sizes[i][1];
		TestRoundTrip( KIND_WHITE, w, h, ATLAS_TGA_WHITE_ALPHA );
		TestRoundTrip( KIND_OPAQUE_WHITE, w, h, ATLAS_TGA_WHITE_ALPHA );
		TestRoundTrip( KIND_GREY, w, h, ATLAS_TGA_GREY_ALPHA );
		TestRoundTrip( KIND_COLOR, w, h, ATLAS_TGA_RGBA );
	}
	TestRejects();

	if( failures )
	{
		printf( "mainui_atlas_tga: %d failure(s)\n", failures );
		return 1;
	}
	printf( "mainui_atlas_tga: all passed\n" );
	return 0;
}
