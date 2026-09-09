// Proves mpak_layout.c's offset math against real, hand-computed byte
// layouts - not against itself (mpak_layout.c's own sector-rounding logic
// is inline/static, unreachable from here, so every "expected" offset
// below is arithmetic this file works out independently, the same way
// filesystem/tests/mpak.c's own byte-exact checks are independent of what
// mpak.c itself computed).
//
// Standalone, unlike this directory's other tests: mpak_layout.c is meant
// to be called directly from engine C code (mod_bmodel.c, eventually -
// see mpak_layout.h's own header comment), not through the dynamic
// fs_api_t vtable filesystem_test_common.h's LoadFilesystem() loads, so
// this links mpak_layout.c straight into the test binary instead. That
// needs a real g_engfuncs definition (mpak_layout.c's Con_Reportf calls
// through it on every failure path this test deliberately exercises) -
// provided below, zero-initialized except _Con_Reportf, since nothing
// else in mpak_layout.c touches any other field.
#include "build.h"
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "filesystem_internal.h"
#include "mpak_layout.h"
#include "bspfile.h"

static void TestReportf( const char *fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	vprintf( fmt, args );
	va_end( args );
}

fs_interface_t g_engfuncs = { ._Con_Reportf = TestReportf };

#define SECTOR 2048u

static fs_offset_t RoundUpSector( fs_offset_t len )
{
	return (( len + ( SECTOR - 1 )) / SECTOR ) * SECTOR;
}

// Builds a minimal, real (not degenerate) archive: 2 planes, a real
// entity-lump string, a 4x4x1 RGBA8 lightmap layer, 1 texinfo entry - one
// of every "must divide evenly" and "must match declared dimensions"
// check mpak_layout.c performs, so a bug in any single check has
// something real to catch it against. targetXbox selects which of the
// two mutually exclusive texture branches gets exercised.
static byte *BuildMinimalArchive( qboolean targetXbox, fs_offset_t *outSize )
{
	static const char entityText[] = "{\n\"classname\" \"worldspawn\"\n}\n";
	const uint32_t lenEntities = (uint32_t)sizeof( entityText ); // includes the NUL, real GoldSrc convention
	const uint32_t lenPlanes = 2 * (uint32_t)sizeof( dplane_t );
	const uint32_t lenTexinfo = 1 * (uint32_t)sizeof( dtexinfo_t );
	const uint32_t lightmapW = 4, lightmapH = 4, lightmapLayers = 1;
	const uint32_t lenLightmap = lightmapW * lightmapH * lightmapLayers * 4;
	const uint32_t lenAtlas = targetXbox ? 128u : 0u;   // arbitrary, only needs to be > 0
	const uint32_t lenArray256 = targetXbox ? 0u : 96u; // arbitrary, only needs to be > 0; exercises a non-first PC array

	mpak_header_t header;
	fs_offset_t cursor;
	fs_offset_t entitiesOffset, planesOffset, lightmapOffset, texinfoOffset, textureOffset;
	byte *buf;

	memset( &header, 0, sizeof( header ));
	memcpy( header.magic, MPAK_LAYOUT_MAGIC, sizeof( header.magic ));
	header.version = MPAK_LAYOUT_VERSION;
	header.headerSize = (uint32_t)sizeof( mpak_header_t );
	header.flags = targetXbox ? MPAK_TARGET_XBOX_FLAG : 0;
	header.lenPlanes = lenPlanes;
	header.lenEntities = lenEntities;
	header.lightmapWidth = lightmapW;
	header.lightmapHeight = lightmapH;
	header.lightmapLayers = lightmapLayers;
	header.lightmapFormat = 0; // RGBA8
	header.lenLightmapAtlas = lenLightmap;
	header.lenTexinfo = lenTexinfo;
	if( targetXbox )
		header.lenAtlas = lenAtlas;
	else
		header.lenArray256 = lenArray256;

	// Independently walk the same section order mpak_layout.c documents
	// (its own header comment, verified against PakManager.cpp's save()),
	// computing each offset by hand rather than reusing anything from
	// mpak_layout.c itself.
	cursor = SECTOR; // header always occupies exactly one sector
	cursor += RoundUpSector( 0 ); // vertices (vCount=0)
	cursor += RoundUpSector( 0 ); // indices (iCount=0)
	cursor += RoundUpSector( 0 ); // visibility
	cursor += RoundUpSector( 0 ); // nodes
	cursor += RoundUpSector( 0 ); // leaves
	cursor += RoundUpSector( 0 ); // marksurfaces
	cursor += RoundUpSector( 0 ); // faceDrawCmds
	planesOffset = cursor;
	cursor += RoundUpSector( lenPlanes );
	cursor += RoundUpSector( 0 ); // clipnodes
	cursor += RoundUpSector( 0 ); // models
	textureOffset = cursor;
	if( targetXbox )
	{
		cursor += RoundUpSector( lenAtlas );
	}
	else
	{
		cursor += RoundUpSector( 0 ); // array64
		cursor += RoundUpSector( lenArray256 );
		cursor += RoundUpSector( 0 ); // array512
	}
	cursor += RoundUpSector( 0 ); // assetDict
	cursor += RoundUpSector( 0 ); // assetData
	entitiesOffset = cursor;
	cursor += RoundUpSector( lenEntities );
	lightmapOffset = cursor;
	cursor += RoundUpSector( lenLightmap );
	cursor += RoundUpSector( 0 ); // faceInfo
	texinfoOffset = cursor;
	cursor += RoundUpSector( lenTexinfo );

	buf = (byte *)calloc( 1, (size_t)cursor );
	memcpy( buf, &header, sizeof( header ));
	memcpy( buf + planesOffset, "AAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBB", lenPlanes ); // 2x20 real bytes, content unchecked
	memcpy( buf + entitiesOffset, entityText, lenEntities );
	memset( buf + lightmapOffset, 0xAA, lenLightmap );
	memset( buf + texinfoOffset, 0x11, lenTexinfo );
	if( targetXbox )
		memset( buf + textureOffset, 0x22, lenAtlas );
	else
		memset( buf + textureOffset + RoundUpSector( 0 ), 0x33, lenArray256 ); // skip the empty array64 slot

	*outSize = cursor;
	return buf;
}

static qboolean TestValidArchive( qboolean targetXbox )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( targetXbox, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean ok = true;

	memcpy( &header, buf, sizeof( header ));

	if( !Mpak_ValidateHeader( &header ))
	{
		printf( "TestValidArchive(%s): ValidateHeader rejected a valid header\n", targetXbox ? "xbox" : "pc" );
		ok = false;
		goto done;
	}

	if( !Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		printf( "TestValidArchive(%s): ComputeLayout rejected a valid archive\n", targetXbox ? "xbox" : "pc" );
		ok = false;
		goto done;
	}

	if( layout.planes.length != header.lenPlanes ||
		memcmp( buf + layout.planes.offset, "AAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBB", header.lenPlanes ) != 0 )
	{
		printf( "TestValidArchive(%s): planes lump resolved to the wrong bytes\n", targetXbox ? "xbox" : "pc" );
		ok = false;
	}

	if( layout.entities.length != header.lenEntities ||
		strcmp( (const char *)( buf + layout.entities.offset ), "{\n\"classname\" \"worldspawn\"\n}\n" ) != 0 )
	{
		printf( "TestValidArchive(%s): entities lump did not resolve to real entity text\n", targetXbox ? "xbox" : "pc" );
		ok = false;
	}

	if( layout.texinfo.length != header.lenTexinfo )
	{
		printf( "TestValidArchive(%s): texinfo lump length mismatch\n", targetXbox ? "xbox" : "pc" );
		ok = false;
	}

	if( targetXbox )
	{
		if( layout.atlas.length != header.lenAtlas || layout.array64.length != 0 ||
			layout.array256.length != 0 || layout.array512.length != 0 )
		{
			printf( "TestValidArchive(xbox): texture branch populated the wrong fields\n" );
			ok = false;
		}
	}
	else
	{
		if( layout.array256.length != header.lenArray256 || layout.atlas.length != 0 )
		{
			printf( "TestValidArchive(pc): texture branch populated the wrong fields\n" );
			ok = false;
		}
	}

done:
	free( buf );
	return ok;
}

// Every corruption case below must be REJECTED - each mutates one real,
// independently-verified-valid archive from BuildMinimalArchive() (PC
// target) in exactly one way.
static qboolean TestRejectsBadMagic( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	memcpy( header.magic, "XXXX", 4 );
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsBadMagic: accepted\n" );
	return rejected;
}

static qboolean TestRejectsBadVersion( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.version = 99;
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsBadVersion: accepted\n" );
	return rejected;
}

static qboolean TestRejectsHeaderSizeDrift( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.headerSize = (uint32_t)sizeof( header ) + 4; // simulate a drifted C++ struct
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsHeaderSizeDrift: accepted\n" );
	return rejected;
}

static qboolean TestRejectsTruncatedFile( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	// Chop off the last sector (the texinfo lump) - a real truncated download/copy.
	rejected = !Mpak_ComputeLayout( &header, fileSize - SECTOR, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsTruncatedFile: accepted\n" );
	return rejected;
}

static qboolean TestRejectsNonDivisiblePlanesLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.lenPlanes += 1; // no longer a multiple of sizeof(dplane_t)
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsNonDivisiblePlanesLength: accepted\n" );
	return rejected;
}

static qboolean TestRejectsLightmapSizeMismatch( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.lightmapWidth += 1; // width*height*layers*4 no longer matches lenLightmapAtlas
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsLightmapSizeMismatch: accepted\n" );
	return rejected;
}

// Found in review: fs_offset_t is off_t (common/xash3d_types.h:48), which
// is a 32-bit signed long on this build too (sizeof(fs_offset_t) == 4,
// confirmed - not Xbox-only). A raw-blob lump (recordSize == 0, no
// divisibility check to catch it another way) with a header-declared
// length of 0xFFFFFFFF used to narrow straight to fs_offset_t, becoming
// -1, which the old `length <= 0` check then treated as "empty, skip"
// instead of rejecting - a corrupted ~4 GB lump silently vanishing
// instead of failing loudly. Mpak_PlaceLump now takes a uint64_t length
// specifically so this case has nowhere to hide.
static qboolean TestRejectsUint32MaxRawLumpLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.lenVisibility = 0xFFFFFFFFu; // recordSize == 0 for visibility - only the bounds check can catch this
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsUint32MaxRawLumpLength: accepted\n" );
	return rejected;
}

// Companion case for the geometry block specifically: vCount chosen so
// vCount * sizeof(mpak_vertex_t) is exactly 2^32 (67108864 * 64) - a
// value that wraps to precisely 0 in 32-bit arithmetic, not some other
// nonzero garbage. The old `(fs_offset_t)header->vCount * sizeof(...)`
// cast vCount to a 32-bit signed type BEFORE multiplying, so this
// wraparound happened before Mpak_PlaceLump ever saw a length at all -
// a claimed 67 million vertices reads back as "zero vertices, valid",
// silently dropping the entire geometry block rather than rejecting a
// claim the file can't possibly back up.
static qboolean TestRejectsOverflowingVertexCount( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, &fileSize );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf, sizeof( header ));
	header.vCount = 67108864u; // * sizeof(mpak_vertex_t)=64 == 2^32 exactly
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsOverflowingVertexCount: accepted\n" );
	return rejected;
}

// Found in review (round 2): round 1's fix bounds-checked only the
// UNPADDED length against fileSize; the sector-rounded advance could
// still overflow a 32-bit signed fs_offset_t cursor even when the raw
// length fit exactly. fileSize == INT32_MAX and length == INT32_MAX -
// cursor (2048) both individually fit (cursor + length == fileSize,
// exactly), but roundUp(length, 2048) lands one byte past INT32_MAX.
// Header built directly rather than via BuildMinimalArchive - this is
// about the header's own internal arithmetic, not about matching real
// file bytes, and a real ~2 GB buffer isn't needed to exercise it.
static qboolean TestRejectsPaddedExtentOverflow( void )
{
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memset( &header, 0, sizeof( header ));
	memcpy( header.magic, MPAK_LAYOUT_MAGIC, sizeof( header.magic ));
	header.version = MPAK_LAYOUT_VERSION;
	header.headerSize = (uint32_t)sizeof( header );
	header.lenVisibility = 2147481599u; // fits unpadded at fileSize == INT32_MAX; its padded extent does not

	rejected = !Mpak_ComputeLayout( &header, 2147483647, &layout );

	if( !rejected ) printf( "TestRejectsPaddedExtentOverflow: accepted\n" );
	return rejected;
}

// Found in review (round 2): the lightmap cross-check's plain uint64_t
// product can itself overflow and wrap - width=height=2^31, layers=1,
// bytesPerPixel=4 makes the naive product exactly 2^64, wrapping to 0,
// which would incorrectly match a declared lenLightmapAtlas of 0.
static qboolean TestRejectsLightmapDimensionOverflow( void )
{
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memset( &header, 0, sizeof( header ));
	memcpy( header.magic, MPAK_LAYOUT_MAGIC, sizeof( header.magic ));
	header.version = MPAK_LAYOUT_VERSION;
	header.headerSize = (uint32_t)sizeof( header );
	header.lightmapWidth = 2147483648u; // 2^31
	header.lightmapHeight = 2147483648u;
	header.lightmapLayers = 1;
	header.lightmapFormat = 0; // RGBA8
	header.lenLightmapAtlas = 0; // what the naive (overflowed) product would wrongly match

	rejected = !Mpak_ComputeLayout( &header, MPAK_SECTOR_SIZE, &layout );

	if( !rejected ) printf( "TestRejectsLightmapDimensionOverflow: accepted\n" );
	return rejected;
}

int main( void )
{
	int ok = 1;

	ok &= TestValidArchive( false );
	ok &= TestValidArchive( true );
	ok &= TestRejectsBadMagic();
	ok &= TestRejectsBadVersion();
	ok &= TestRejectsHeaderSizeDrift();
	ok &= TestRejectsTruncatedFile();
	ok &= TestRejectsNonDivisiblePlanesLength();
	ok &= TestRejectsLightmapSizeMismatch();
	ok &= TestRejectsUint32MaxRawLumpLength();
	ok &= TestRejectsOverflowingVertexCount();
	ok &= TestRejectsPaddedExtentOverflow();
	ok &= TestRejectsLightmapDimensionOverflow();

	if( !ok )
		return EXIT_FAILURE;

	printf( "success\n" );
	return EXIT_SUCCESS;
}
