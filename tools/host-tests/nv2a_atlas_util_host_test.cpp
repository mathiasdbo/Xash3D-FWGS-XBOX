/*
nv2a_atlas_util_host_test.cpp - host test for ref/nv2a/nv2a_atlas_util.c

Resonance3D (docs/r3d/divergences.md #157): ref/nv2a uploads the font
cache's glyph atlases (3rdparty/mainui font/AtlasTGA.h, #152) straight
from their TGA bytes instead of decoding them to RGBA32 and packing them
back to 8 or 16 bits. The claim is that the texture is the same: format,
size, pitch, every texel byte, the bytes left unwritten, and the
TF_HAS_ALPHA flag. This test runs the general path - a transcription of
Image_LoadTGA (engine/common/imagelib/img_tga.c, identical in
xash3d-fwgs), then nv2a_tex_set_dimensions' halving and nv2a_tex_upload's
classify and pack (the real nv2a_2d_classify_rgba/pack_row_* from
nv2a_2d_util.c) - and the fast path (nv2a_atlas_tga_probe + _copy) on:
every form AtlasTGA_Write produces, from random atlases and edge cases
(fully transparent, fully opaque, one late texel deciding the class or
the flag, odd widths, 1 and 1024 sides); hand-made TGAs just outside the
accepted shapes (which must be refused); and random header and colour-map
mutations of valid files (refused, or identical). Wherever the fast path
accepts, both results must be byte for byte the same.

Build and run from the repository root (tools/host-tests/run.sh):

  gcc -O2 -fsanitize=undefined -fsanitize=float-cast-overflow \
    -fsanitize-undefined-trap-on-error -Wall -std=c99 -Iref/nv2a \
    -c ref/nv2a/nv2a_atlas_util.c -o .sweep/host-tests/nv2a_atlas_util.o &&
  gcc -O2 -fsanitize=undefined -fsanitize=float-cast-overflow \
    -fsanitize-undefined-trap-on-error -Wall -std=c99 -Iref/nv2a \
    -c ref/nv2a/nv2a_2d_util.c -o .sweep/host-tests/nv2a_2d_util_atlas.o &&
  gcc -O2 -fsanitize=undefined -fsanitize=float-cast-overflow \
    -fsanitize-undefined-trap-on-error -Wall -std=c99 -Iref/nv2a \
    -c ref/nv2a/nv2a_tri_util.c -o .sweep/host-tests/nv2a_tri_util_atlas.o &&
  g++ -O2 -fsanitize=undefined -fsanitize=float-cast-overflow \
    -fsanitize-undefined-trap-on-error -Wall -std=c++11 -Iref/nv2a \
    -I3rdparty/mainui/font tools/host-tests/nv2a_atlas_util_host_test.cpp \
    .sweep/host-tests/nv2a_atlas_util.o .sweep/host-tests/nv2a_2d_util_atlas.o \
    .sweep/host-tests/nv2a_tri_util_atlas.o \
    -o .sweep/host-tests/nv2a_atlas_util_test && .sweep/host-tests/nv2a_atlas_util_test
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "AtlasTGA.h"
#include "nv2a_atlas_util.h"
#include "nv2a_2d_util.h"

static int failures;
static long compared, refused;

#define CHECK( cond, ... ) \
	do { if( !( cond )) { failures++; if( failures < 40 ) { printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } } while( 0 )

#define IMAGE_HAS_ALPHA 1
#define IMAGE_HAS_COLOR 2
#define SENTINEL        0xA5

enum { FMT_A8 = 0x1F, FMT_A8Y8 = 0x20, FMT_RGBA = 0x12 }; // nv2a_texture.h NV2A_TEX_COLOR_LU_*

struct decoded_t
{
	bool ok;       // Image_LoadTGA returned true
	bool overread; // it would have read past the buffer (undefined; never on a fast-path input)
	int width, height, flags;
	std::vector<uint8_t> rgba; // top row first
};

/* Image_LoadTGA, img_tga.c:35-237 (line numbers of the reference clone),
   with IL_DONTFLIP_TGA and IL_LOAD_PLAYER_DECAL unset (the fast path's
   callers: r_image.c nv2a_tex_load_atlas_tga). Every branch is kept,
   including the ones no atlas takes: unknown image types that "succeed"
   with whatever the texel variables hold, the colour map index test,
   RLE packets. */
static decoded_t ImageLoadTGA( const uint8_t *buffer, size_t filesize )
{
	decoded_t d;
	d.ok = d.overread = false;
	d.width = d.height = d.flags = 0;

	if( filesize < 18 )
		return d;

	const uint8_t *buf_p = buffer;
	const uint8_t *end = buffer + filesize;
	const int id_length = *buf_p++;
	buf_p++; // colormap_type: read, never tested
	const int image_type = *buf_p++;
	const int colormap_index = buf_p[0] + buf_p[1] * 256; buf_p += 2;
	const int colormap_length = buf_p[0] + buf_p[1] * 256; buf_p += 2;
	const int colormap_size = *buf_p; buf_p += 1;
	buf_p += 4; // x_origin, y_origin
	const int width = buf_p[0] + buf_p[1] * 256; buf_p += 2;
	const int height = buf_p[0] + buf_p[1] * 256; buf_p += 2;
	const int pixel_size = *buf_p++;
	const int attributes = *buf_p++;
	buf_p += id_length;

	d.width = width;
	d.height = height;
	if( width > 8192 || height > 8192 || width <= 0 || height <= 0 ) // Image_ValidSize, img_utils.c
		return d;

	uint8_t palette[256][4];
	memset( palette, 0, sizeof( palette ));

#define NEED( n ) do { if( (size_t)( end - buf_p ) < (size_t)( n )) { d.overread = true; return d; } } while( 0 )
	if( image_type == 1 || image_type == 9 )
	{
		if( pixel_size != 8 || colormap_length != 256 || colormap_index )
			return d;
		if( colormap_size == 24 )
		{
			NEED( 256 * 3 );
			for( int i = 0; i < colormap_length; i++ )
			{
				palette[i][2] = *buf_p++;
				palette[i][1] = *buf_p++;
				palette[i][0] = *buf_p++;
				palette[i][3] = 255;
			}
		}
		else if( colormap_size == 32 )
		{
			NEED( 256 * 4 );
			for( int i = 0; i < colormap_length; i++ )
			{
				palette[i][2] = *buf_p++;
				palette[i][1] = *buf_p++;
				palette[i][0] = *buf_p++;
				palette[i][3] = *buf_p++;
			}
		}
		else
			return d;
	}
	else if( image_type == 2 || image_type == 10 )
	{
		if( pixel_size != 32 && pixel_size != 24 )
			return d;
	}
	else if( image_type == 3 || image_type == 11 )
	{
		if( pixel_size != 8 && pixel_size != 16 )
			return d;
	}

	const int columns = width, rows = height;
	d.rgba.assign( (size_t)columns * rows * 4, 0 );
	uint8_t *base = d.rgba.data();
	uint8_t *pixbuf;
	long row_inc;
	if( attributes & 0x20 )
	{
		pixbuf = base;
		row_inc = 0;
	}
	else
	{
		pixbuf = base + (size_t)( rows - 1 ) * columns * 4;
		row_inc = -(long)columns * 4 * 2;
	}

	const bool compressed = image_type == 9 || image_type == 10 || image_type == 11;
	uint8_t red = 0, green = 0, blue = 0, alpha = 0;
	int row = 0, col = 0;
	while( row < rows )
	{
		int pixelcount = 0x10000;
		int readpixelcount = 0x10000;

		if( compressed )
		{
			NEED( 1 );
			pixelcount = *buf_p++;
			if( pixelcount & 0x80 )
				readpixelcount = 1;
			pixelcount = 1 + ( pixelcount & 0x7f );
		}

		while( pixelcount-- && row < rows )
		{
			if( readpixelcount-- > 0 )
			{
				switch( image_type )
				{
				case 1:
				case 9:
					NEED( 1 );
					blue = *buf_p++;
					if( blue < colormap_length )
					{
						red = palette[blue][0];
						green = palette[blue][1];
						alpha = palette[blue][3];
						blue = palette[blue][2];
						if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
					}
					break;
				case 2:
				case 10:
					NEED( pixel_size / 8 );
					blue = *buf_p++;
					green = *buf_p++;
					red = *buf_p++;
					alpha = 255;
					if( pixel_size == 32 )
					{
						alpha = *buf_p++;
						if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
					}
					break;
				case 3:
				case 11:
					NEED( pixel_size / 8 );
					blue = green = red = *buf_p++;
					if( pixel_size == 16 )
					{
						alpha = *buf_p++;
						if( alpha != 255 ) d.flags |= IMAGE_HAS_ALPHA;
					}
					else
						alpha = 255;
					break;
				}
			}

			if( red != green || green != blue )
				d.flags |= IMAGE_HAS_COLOR;

			*pixbuf++ = red;
			*pixbuf++ = green;
			*pixbuf++ = blue;
			*pixbuf++ = alpha;
			if( ++col == columns )
			{
				row++;
				col = 0;
				pixbuf += row_inc;
			}
		}
	}
#undef NEED
	d.ok = true;
	return d;
}

struct texture_t
{
	bool made;      // a texture was created
	bool resampled; // the general path would have resampled (never a fast-path input)
	uint32_t format, width, height, src_width, src_height, pitch;
	bool tf_has_alpha;
	std::vector<uint8_t> mem; // pitch * height, SENTINEL where nothing was written
};

/* GL_LoadTexture's general path for a '#'-named .tga buffer and flags
   without TF_KEEP_SOURCE/TF_MAKELUMA/TF_NOFLIP_TGA (r_image.c): FS_LoadImage
   (the decode above), nv2a_tex_process_image (IMAGE_HAS_ALPHA ->
   TF_HAS_ALPHA; Image_Process with no flags is a no-op), nv2a_tex_upload
   (nv2a_tex_set_dimensions' halving past 1024, PF_RGBA_32 needs no
   conversion, classify on every texel, rows packed at a 64-byte pitch). */
static texture_t GeneralPath( const uint8_t *buf, size_t size )
{
	texture_t t;
	t.made = t.resampled = t.tf_has_alpha = false;
	t.format = t.width = t.height = t.src_width = t.src_height = t.pitch = 0;

	const decoded_t d = ImageLoadTGA( buf, size );
	if( !d.ok )
		return t;

	t.made = true;
	t.tf_has_alpha = ( d.flags & IMAGE_HAS_ALPHA ) != 0;
	t.src_width = d.width;
	t.src_height = d.height;
	int w = d.width, h = d.height;
	while( w > 1024 || h > 1024 )
	{
		w >>= 1;
		h >>= 1;
	}
	t.width = w < 1 ? 1 : w;
	t.height = h < 1 ? 1 : h;
	if( t.width != (uint32_t)d.width || t.height != (uint32_t)d.height )
	{
		t.resampled = true;
		return t;
	}

	uint32_t bpp;
	switch( nv2a_2d_classify_rgba( d.rgba.data(), t.width * t.height ))
	{
	case NV2A_TEXELS_WHITE_ALPHA: bpp = 1; t.format = FMT_A8; break;
	case NV2A_TEXELS_GREY_ALPHA:  bpp = 2; t.format = FMT_A8Y8; break;
	default:                      bpp = 4; t.format = FMT_RGBA; break;
	}
	t.pitch = ( t.width * bpp + 63 ) & ~63u;
	t.mem.assign( (size_t)t.pitch * t.height, SENTINEL );
	for( uint32_t y = 0; y < t.height; y++ )
	{
		const uint8_t *row = &d.rgba[(size_t)y * t.width * 4];
		uint8_t *dst = &t.mem[(size_t)y * t.pitch];
		if( bpp == 1 )
			nv2a_2d_pack_row_a8( dst, row, t.width );
		else if( bpp == 2 )
			nv2a_2d_pack_row_a8y8( dst, row, t.width );
		else
			memcpy( dst, row, (size_t)t.width * 4 );
	}
	return t;
}

/* r_image.c nv2a_tex_load_atlas_tga: probe, then the same fields and the
   copy into storage of pitch * height. */
static bool FastPath( const uint8_t *buf, size_t size, texture_t &t )
{
	nv2a_atlas_tga_t a;
	memset( &a, 0x5A, sizeof( a ));
	if( !nv2a_atlas_tga_probe( buf, size, 1024, &a ))
		return false;

	t.made = true;
	t.resampled = false;
	t.tf_has_alpha = a.has_alpha != 0;
	t.width = t.src_width = a.width;
	t.height = t.src_height = a.height;
	t.format = a.dst_bpp == 1 ? FMT_A8 : FMT_A8Y8;
	t.pitch = ( a.width * a.dst_bpp + 63 ) & ~63u;
	t.mem.assign( (size_t)t.pitch * t.height + 64, SENTINEL ); // 64 guard bytes past the storage
	nv2a_atlas_tga_copy( &a, t.mem.data(), t.pitch );
	for( size_t i = (size_t)t.pitch * t.height; i < t.mem.size(); i++ )
		CHECK( t.mem[i] == SENTINEL, "copy wrote past pitch * height" );
	t.mem.resize( (size_t)t.pitch * t.height );
	return true;
}

/* The fast path either refuses or matches the general path exactly. */
static void Compare( const char *what, const std::vector<uint8_t> &tga, bool expect_fast )
{
	texture_t fast;
	const bool took = FastPath( tga.data(), tga.size(), fast );

	CHECK( took == expect_fast, "%s: fast path %s, expected %s", what, took ? "taken" : "refused", expect_fast ? "taken" : "refused" );
	if( !took )
	{
		refused++;
		return;
	}

	const texture_t gen = GeneralPath( tga.data(), tga.size());
	compared++;
	CHECK( gen.made && !gen.resampled, "%s: fast path took a file the general path does not upload as is", what );
	if( !gen.made || gen.resampled )
		return;
	CHECK( fast.format == gen.format, "%s: format %#x, general %#x", what, fast.format, gen.format );
	CHECK( fast.width == gen.width && fast.height == gen.height && fast.src_width == gen.src_width && fast.src_height == gen.src_height,
		"%s: size", what );
	CHECK( fast.pitch == gen.pitch, "%s: pitch %u, general %u", what, fast.pitch, gen.pitch );
	CHECK( fast.tf_has_alpha == gen.tf_has_alpha, "%s: TF_HAS_ALPHA %d, general %d", what, fast.tf_has_alpha, gen.tf_has_alpha );
	CHECK( fast.mem == gen.mem, "%s: texture bytes differ", what );
}

static uint32_t rng = 0x12345678u;
static uint32_t Rand( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static uint8_t Rand8( void ) { return (uint8_t)( Rand() >> 11 ); }

enum kind_e
{
	K_WHITE,             // glyph-like coverage, some fully transparent texels
	K_WHITE_TRANSPARENT, // every alpha 0
	K_WHITE_OPAQUE,      // every alpha 255: no TF_HAS_ALPHA
	K_WHITE_LAST_ALPHA,  // opaque but the last texel
	K_GREY,              // the outlined console font
	K_GREY_OPAQUE,       // grey, every alpha 255
	K_GREY_LAST,         // white but the last texel grey: grey-alpha decided at the end
	K_COLOR,             // one coloured texel: type 2, refused
	K_COUNT
};

static std::vector<uint8_t> MakeAtlas( kind_e kind, int w, int h )
{
	const size_t n = (size_t)w * h;
	std::vector<uint8_t> a( n * 4 );
	for( size_t i = 0; i < n; i++ )
	{
		uint8_t *t = &a[i * 4];
		uint8_t g = Rand8();
		t[0] = t[1] = t[2] = 255;
		switch( kind )
		{
		case K_WHITE: t[3] = ( Rand() % 3 ) ? 0 : Rand8(); break;
		case K_WHITE_TRANSPARENT: t[3] = 0; break;
		case K_WHITE_OPAQUE: t[3] = 255; break;
		case K_WHITE_LAST_ALPHA: t[3] = i + 1 == n ? 254 : 255; break;
		case K_GREY: t[0] = t[1] = t[2] = ( Rand() % 4 ) ? g : 255; t[3] = ( Rand() % 3 ) ? 0 : Rand8(); break;
		case K_GREY_OPAQUE: t[0] = t[1] = t[2] = g; t[3] = 255; break;
		case K_GREY_LAST: t[0] = t[1] = t[2] = i + 1 == n ? 7 : 255; t[3] = Rand8(); break;
		case K_COLOR: t[3] = Rand8(); if( i == n / 2 ) t[1] = 3; break;
		default: break;
		}
		if( i == 0 && ( kind == K_GREY || kind == K_GREY_OPAQUE ))
			t[0] = t[1] = t[2] = 128; // grey for certain, even at 1x1
	}
	return a;
}

static std::vector<uint8_t> WriteAtlas( const std::vector<uint8_t> &rgba, int w, int h, atlasTgaForm_e *formOut )
{
	const atlasTgaForm_e form = AtlasTGA_Classify( rgba.data(), (size_t)w * h );
	std::vector<uint8_t> tga( AtlasTGA_Size( form, w, h ));
	AtlasTGA_Write( tga.data(), form, rgba.data(), w, h );
	if( formOut )
		*formOut = form;
	return tga;
}

/* Every form AtlasTGA_Write produces, at many sizes. */
static void TestAtlasForms( void )
{
	static const int sides[][2] = {
		{ 1, 1 }, { 1, 7 }, { 7, 1 }, { 3, 5 }, { 63, 2 }, { 64, 2 }, { 65, 3 }, { 127, 9 }, { 33, 33 },
		{ 256, 64 }, { 512, 256 }, { 1024, 1 }, { 1, 1024 }, { 1024, 64 }, { 300, 1024 },
	};
	char what[96];

	for( int k = 0; k < K_COUNT; k++ )
	{
		for( size_t s = 0; s < sizeof( sides ) / sizeof( sides[0] ); s++ )
		{
			const int w = sides[s][0], h = sides[s][1];
			atlasTgaForm_e form;
			const std::vector<uint8_t> tga = WriteAtlas( MakeAtlas( (kind_e)k, w, h ), w, h, &form );
			snprintf( what, sizeof( what ), "kind %d %dx%d form %d", k, w, h, form );
			Compare( what, tga, form != ATLAS_TGA_RGBA );
			if( k == K_COLOR )
				CHECK( form == ATLAS_TGA_RGBA, "%s: a coloured atlas must be type 2", what );
			if( k == K_GREY || k == K_GREY_OPAQUE || k == K_GREY_LAST )
				CHECK( form == ATLAS_TGA_GREY_ALPHA, "%s: expected grey-alpha", what );
			if( k <= K_WHITE_LAST_ALPHA )
				CHECK( form == ATLAS_TGA_WHITE_ALPHA, "%s: expected white-alpha", what );
		}
	}

	// Random atlases of every kind and random sizes.
	for( int i = 0; i < 600; i++ )
	{
		const kind_e k = (kind_e)( Rand() % K_COUNT );
		const int w = 1 + Rand() % 200, h = 1 + Rand() % 120;
		atlasTgaForm_e form;
		const std::vector<uint8_t> tga = WriteAtlas( MakeAtlas( k, w, h ), w, h, &form );
		snprintf( what, sizeof( what ), "random %d kind %d %dx%d", i, k, w, h );
		Compare( what, tga, form != ATLAS_TGA_RGBA );
	}
}

static void Put16( uint8_t *p, unsigned v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); }

/* A type 3 file by hand (AtlasTGA_Write would make every-grey-255 type 1). */
static std::vector<uint8_t> Type3( int w, int h, int pixel_size, bool all_white, bool opaque )
{
	const int bpp = pixel_size / 8;
	std::vector<uint8_t> t( 18 + (size_t)w * h * bpp, 0 );
	t[2] = 3;
	Put16( &t[12], w );
	Put16( &t[14], h );
	t[16] = (uint8_t)pixel_size;
	t[17] = 8;
	for( size_t i = 0; i < (size_t)w * h; i++ )
	{
		t[18 + i * bpp] = all_white ? 255 : Rand8();
		if( bpp == 2 )
			t[18 + i * bpp + 1] = opaque ? 255 : Rand8();
	}
	return t;
}

static void TestEdges( void )
{
	// Type 3 with every grey 255: the general path classifies it white,
	// A8 from the alpha bytes - the fast path's 2 -> 1 byte branch.
	Compare( "type 3 all white", Type3( 37, 11, 16, true, false ), true );
	Compare( "type 3 all white opaque", Type3( 64, 4, 16, true, true ), true );
	Compare( "type 3 grey opaque", Type3( 5, 5, 16, false, true ), true );
	Compare( "type 3 8-bit", Type3( 9, 9, 8, false, true ), false );

	std::vector<uint8_t> base = WriteAtlas( MakeAtlas( K_WHITE, 70, 13 ), 70, 13, NULL );
	std::vector<uint8_t> t;

	t = base; t.push_back( 0 ); t.push_back( 1 );
	Compare( "trailing bytes", t, true ); // Image_LoadTGA ignores them too
	t = base; t.pop_back();
	Compare( "one byte short", t, false );
	t = base; t[17] |= 0x20;
	Compare( "top-down (bit 5)", t, false );
	t = base; t[17] = 0;
	Compare( "attributes 0", t, true ); // only bit 5 is read
	t = base; t[0] = 3; t.insert( t.begin() + 18, 3, 'x' );
	Compare( "image id", t, false );
	t = base; t[1] = 0;
	Compare( "type 1, colour map type 0", t, false );
	t = base; t[2] = 9;
	Compare( "RLE type 9", t, false );
	t = base; t[7] = 24;
	Compare( "24-bit colour map", t, false );
	t = base; t[18 + 4 * 200 + 3] = 199;
	Compare( "colour map entry 200 alpha 199", t, false );
	t = base; t[18 + 4 * 255 + 0] = 254;
	Compare( "colour map entry 255 not white", t, false );
	t = base; Put16( &t[12], 0 );
	Compare( "width 0", t, false );

	// Over 1024: the general path halves and resamples; refused.
	t = WriteAtlas( MakeAtlas( K_WHITE, 1025, 2 ), 1025, 2, NULL );
	Compare( "1025 wide", t, false );
	t = WriteAtlas( MakeAtlas( K_GREY, 2, 2048 ), 2, 2048, NULL );
	Compare( "2048 high", t, false );

	// A type 3 file whose grey is decided by its last texel, and whose
	// alpha flag is decided by its last texel.
	t = Type3( 31, 3, 16, true, true );
	t[t.size() - 2] = 254;
	Compare( "type 3 last grey", t, true );
	t = Type3( 31, 3, 16, true, true );
	t[t.size() - 1] = 0;
	Compare( "type 3 last alpha", t, true );
}

/* Random single-byte mutations of the header and colour map of valid
   files: whatever the fast path still accepts must decode identically. */
static void TestMutations( void )
{
	char what[64];

	for( int i = 0; i < 20000; i++ )
	{
		const kind_e k = ( i & 1 ) ? K_WHITE : K_GREY;
		const int w = 1 + Rand() % 40, h = 1 + Rand() % 20;
		std::vector<uint8_t> t = WriteAtlas( MakeAtlas( k, w, h ), w, h, NULL );
		const size_t span = k == K_WHITE ? 18 + 1024 : 18;
		const size_t at = Rand() % span;
		t[at] = ( Rand() & 1 ) ? Rand8() : (uint8_t)( t[at] ^ ( 1u << ( Rand() % 8 )));
		texture_t fast;
		if( !FastPath( t.data(), t.size(), fast ))
		{
			refused++;
			continue;
		}
		snprintf( what, sizeof( what ), "mutation %d byte %u", i, (unsigned)at );
		Compare( what, t, true );
	}
}

int main( void )
{
	TestAtlasForms();
	TestEdges();
	TestMutations();

	printf( "nv2a_atlas_util: %ld textures compared byte for byte, %ld refused, %d failures\n", compared, refused, failures );
	return failures ? 1 : 0;
}
