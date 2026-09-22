// Proves mpak_layout.c's offset math against real, hand-computed byte
// layouts - not against itself (mpak_layout.c's own sector-rounding logic
// is inline/static, unreachable from here, so every "expected" offset
// below is arithmetic this file works out independently, the same way
// filesystem/tests/mpak.c's own byte-exact checks are independent of what
// mpak.c itself computed).
//
// Standalone, unlike this directory's other tests: mpak_layout.c is meant
// to be called directly from engine C code, not through the dynamic
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

// Everything a valid v7 first sector must hold: the real dheader_t at 0
// and the descriptor at MPAK_DESCRIPTOR_OFFSET. Filled by
// BuildMinimalArchive from the same hand-computed offsets it lays the
// lumps out with, so the test's own two tables agree by construction -
// exactly the property Mpak_ValidateBspHeader checks on real archives.
typedef struct
{
	dheader_t     bsp;
	mpak_header_t desc;
} first_sector_t;

// Builds a minimal, real (not degenerate) v7 archive: 2 planes, a real
// entity-lump string, 3 vertexes, 2 edges, 4 surfedges, 2 faces, a
// small lighting blob, a tiny miptex lump, 1 texinfo entry, and - in the
// nv2a profile only - a 4x4x1 RGBA8 lightmap layer plus a texture blob:
// one of every "must divide evenly", "must match declared dimensions"
// and "must agree with the dheader_t" check mpak_layout.c performs, so a
// bug in any single check has something real to catch it against.
static byte *BuildMinimalArchive( qboolean targetXbox, qboolean nv2a, fs_offset_t *outSize, first_sector_t *outSector )
{
	static const char entityText[] = "{\n\"classname\" \"worldspawn\"\n}\n";
	const uint32_t lenEntities = (uint32_t)sizeof( entityText ); // includes the NUL, real GoldSrc convention
	const uint32_t lenPlanes = 2 * (uint32_t)sizeof( dplane_t );
	const uint32_t lenTexinfo = 1 * (uint32_t)sizeof( dtexinfo_t );
	const uint32_t lenVertexes = 3 * (uint32_t)sizeof( dvertex_t );
	const uint32_t lenEdges = 2 * (uint32_t)sizeof( dedge_t );
	const uint32_t lenSurfedges = 4 * (uint32_t)sizeof( dsurfedge_t );
	const uint32_t lenFaces = 2 * (uint32_t)sizeof( dface_t );
	const uint32_t lenLighting = 48;
	const uint32_t lenMiptex = 4 + 4 + 40; // dmiptexlump_t{1} + one header-only miptex entry
	const uint32_t lenExtraLightBlocks = nv2a ? 2 * 20u : 0u; // 2 MpakExtraLightBlock records
	const uint32_t lightmapW = 4, lightmapH = 4, lightmapLayers = nv2a ? 1u : 0u;
	const uint32_t lenLightmap = nv2a ? lightmapW * lightmapH * 4 : 0u;
	// v9: one real per-texture DXT1 directory entry + its matching 4x4 (8-byte)
	// DXT1 blob + one texture-draw batch, Xbox target only - replaces the old
	// shared-atlas lump (lenAtlas). faceCount/numOriginalFaces below is 2,
	// matching lenFaces/sizeof(dface_t), so the batch's own faceCount and the
	// faceRemap table's own element count both refer to the same real face set.
	const uint32_t numOriginalFaces = 2u;
	const qboolean texturedXbox = ( nv2a && targetXbox );
	const uint32_t lenTextureDir = texturedXbox ? (uint32_t)sizeof( mpak_texture_info_t ) : 0u; // 1 entry
	const uint32_t lenTextureData = texturedXbox ? 8u : 0u; // 4x4 DXT1 = width*height/2
	const uint32_t lenBatches = texturedXbox ? (uint32_t)sizeof( mpak_texture_batch_t ) : 0u; // 1 batch
	// v9: BspParser's face sort (and this table) runs in the shared nv2a face
	// loop regardless of target - PakManager.cpp save()'s own comment - so
	// this is written for BOTH targets whenever the nv2a profile is active,
	// unlike the three fields above which are Xbox-only.
	const uint32_t lenFaceRemap = nv2a ? numOriginalFaces * (uint32_t)sizeof( uint32_t ) : 0u;
	const uint32_t lenArray256 = ( nv2a && !targetXbox ) ? 96u : 0u; // arbitrary; exercises a non-first PC array

	first_sector_t sector;
	fs_offset_t cursor;
	fs_offset_t entitiesOffset, planesOffset, lightmapOffset, texinfoOffset;
	fs_offset_t textureDirOffset, textureDataOffset, batchesOffset, faceRemapOffset, arrayOffset;
	fs_offset_t vertexesOffset, edgesOffset, surfedgesOffset, facesOffset, lightingOffset, miptexOffset;
	fs_offset_t extraLightBlocksOffset;
	byte *buf;

	memset( &sector, 0, sizeof( sector ));
	memcpy( sector.desc.magic, MPAK_LAYOUT_MAGIC, sizeof( sector.desc.magic ));
	sector.desc.version = MPAK_LAYOUT_VERSION;
	sector.desc.headerSize = (uint32_t)sizeof( mpak_header_t );
	sector.desc.flags = ( targetXbox ? MPAK_TARGET_XBOX_FLAG : 0 ) | ( nv2a ? MPAK_FLAG_NV2A_BLOCK : 0 );
	sector.desc.lenPlanes = lenPlanes;
	sector.desc.lenEntities = lenEntities;
	sector.desc.lightmapWidth = nv2a ? lightmapW : 0;
	sector.desc.lightmapHeight = nv2a ? lightmapH : 0;
	sector.desc.lightmapLayers = lightmapLayers;
	sector.desc.lightmapFormat = 0; // RGBA8
	sector.desc.lenLightmapAtlas = lenLightmap;
	sector.desc.lenTexinfo = lenTexinfo;
	sector.desc.textureCount = texturedXbox ? 1u : 0u;
	sector.desc.lenTextureDir = lenTextureDir;
	sector.desc.lenTextureData = lenTextureData;
	sector.desc.batchCount = texturedXbox ? 1u : 0u;
	sector.desc.lenBatches = lenBatches;
	sector.desc.lenFaceRemap = lenFaceRemap;
	sector.desc.lenArray256 = lenArray256;
	sector.desc.lenVertexes = lenVertexes;
	sector.desc.lenEdges = lenEdges;
	sector.desc.lenSurfedges = lenSurfedges;
	sector.desc.lenFaces = lenFaces;
	sector.desc.lenLighting = lenLighting;
	sector.desc.lenMiptex = lenMiptex;
	sector.desc.lenExtraLightBlocks = lenExtraLightBlocks;

	// Independently walk the same section order mpak_layout.c documents
	// (its own header comment, verified against PakManager.cpp's save()),
	// computing each offset by hand rather than reusing anything from
	// mpak_layout.c itself.
	cursor = SECTOR; // first sector: dheader_t + descriptor
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
	// v9: texture directory/data/batches (Xbox only), then the face remap
	// (both targets) - matches mpak_layout.c's own write-order walk, which
	// in turn matches PakManager.cpp save()'s real write order.
	textureDirOffset = textureDataOffset = batchesOffset = 0;
	if( targetXbox )
	{
		textureDirOffset = cursor;
		cursor += RoundUpSector( lenTextureDir );
		textureDataOffset = cursor;
		cursor += RoundUpSector( lenTextureData );
		batchesOffset = cursor;
		cursor += RoundUpSector( lenBatches );
	}
	faceRemapOffset = cursor;
	cursor += RoundUpSector( lenFaceRemap );
	arrayOffset = 0;
	if( !targetXbox )
	{
		cursor += RoundUpSector( 0 ); // array64
		arrayOffset = cursor;
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
	vertexesOffset = cursor;
	cursor += RoundUpSector( lenVertexes );
	edgesOffset = cursor;
	cursor += RoundUpSector( lenEdges );
	surfedgesOffset = cursor;
	cursor += RoundUpSector( lenSurfedges );
	facesOffset = cursor;
	cursor += RoundUpSector( lenFaces );
	lightingOffset = cursor;
	cursor += RoundUpSector( lenLighting );
	miptexOffset = cursor;
	cursor += RoundUpSector( lenMiptex );
	extraLightBlocksOffset = cursor;
	cursor += RoundUpSector( lenExtraLightBlocks );

	// The dheader_t: the same offsets, in bspfile.h's lump order.
	sector.bsp.version = HLBSP_VERSION;
#define LUMP( n, ofs, len ) do { sector.bsp.lumps[n].fileofs = (int)(ofs); sector.bsp.lumps[n].filelen = (int)(len); } while( 0 )
	LUMP( LUMP_ENTITIES, entitiesOffset, lenEntities );
	LUMP( LUMP_PLANES, planesOffset, lenPlanes );
	LUMP( LUMP_TEXTURES, miptexOffset, lenMiptex );
	LUMP( LUMP_VERTEXES, vertexesOffset, lenVertexes );
	LUMP( LUMP_TEXINFO, texinfoOffset, lenTexinfo );
	LUMP( LUMP_FACES, facesOffset, lenFaces );
	LUMP( LUMP_LIGHTING, lightingOffset, lenLighting );
	LUMP( LUMP_EDGES, edgesOffset, lenEdges );
	LUMP( LUMP_SURFEDGES, surfedgesOffset, lenSurfedges );
	// visibility/nodes/leafs/marksurfaces/clipnodes/models are empty here: {0, 0}
#undef LUMP

	buf = (byte *)calloc( 1, (size_t)cursor );
	memcpy( buf, &sector.bsp, sizeof( sector.bsp ));
	memcpy( buf + MPAK_DESCRIPTOR_OFFSET, &sector.desc, sizeof( sector.desc ));
	memcpy( buf + planesOffset, "AAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBB", lenPlanes ); // 2x20 real bytes, content unchecked
	memcpy( buf + entitiesOffset, entityText, lenEntities );
	memset( buf + texinfoOffset, 0x11, lenTexinfo );
	memset( buf + vertexesOffset, 0x44, lenVertexes );
	memset( buf + edgesOffset, 0x55, lenEdges );
	memset( buf + surfedgesOffset, 0x66, lenSurfedges );
	memset( buf + facesOffset, 0x77, lenFaces );
	memset( buf + lightingOffset, 0x88, lenLighting );
	memcpy( buf + miptexOffset, "\x01\x00\x00\x00\x08\x00\x00\x00" "minimal_test\x00\x00\x00\x00", 8 + 16 ); // nummiptex 1, dataofs 8, name
	if( nv2a )
	{
		memset( buf + lightmapOffset, 0xAA, lenLightmap );
		if( targetXbox )
		{
			// v9: one real 4x4 DXT1 texture directory entry + its matching
			// 8-byte blob + one texture-draw batch covering both real faces.
			mpak_texture_info_t texInfo;
			mpak_texture_batch_t batch;

			texInfo.width = 4;
			texInfo.height = 4;
			texInfo.log2Width = 2;
			texInfo.log2Height = 2;
			texInfo.dataLength = lenTextureData;
			memcpy( buf + textureDirOffset, &texInfo, sizeof( texInfo ));
			memset( buf + textureDataOffset, 0x22, lenTextureData );

			batch.textureIndex = 0;
			batch.startFace = 0;
			batch.faceCount = numOriginalFaces;
			batch.startIndex = 0;
			batch.indexCount = 6;
			memcpy( buf + batchesOffset, &batch, sizeof( batch ));
		}
		else
		{
			memset( buf + arrayOffset, 0x33, lenArray256 );
		}
		if( lenFaceRemap > 0 )
		{
			// v9: a real bijection over the two faces (the inverse permutation
			// a texture-ID sort would produce), not just nonzero filler -
			// written for BOTH targets, matching PakManager.cpp save()'s own
			// "the face sort is unconditional" comment.
			uint32_t remap[2] = { 1, 0 };
			memcpy( buf + faceRemapOffset, remap, sizeof( remap ));
		}
		memset( buf + extraLightBlocksOffset, 0x99, lenExtraLightBlocks );
	}

	*outSize = cursor;
	if( outSector )
		*outSector = sector;
	return buf;
}

static const char *Label( qboolean targetXbox, qboolean nv2a )
{
	if( nv2a )
		return targetXbox ? "xbox/nv2a" : "pc/nv2a";
	return targetXbox ? "xbox/core" : "pc/core";
}

static qboolean TestValidArchive( qboolean targetXbox, qboolean nv2a )
{
	fs_offset_t fileSize;
	first_sector_t sector;
	byte *buf = BuildMinimalArchive( targetXbox, nv2a, &fileSize, &sector );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean ok = true;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));

	if( !Mpak_ValidateHeader( &header ))
	{
		printf( "TestValidArchive(%s): ValidateHeader rejected a valid header\n", Label( targetXbox, nv2a ));
		ok = false;
		goto done;
	}

	if( !Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		printf( "TestValidArchive(%s): ComputeLayout rejected a valid archive\n", Label( targetXbox, nv2a ));
		ok = false;
		goto done;
	}

	if( !Mpak_ValidateBspHeader( buf, &layout ))
	{
		printf( "TestValidArchive(%s): ValidateBspHeader rejected a consistent dheader_t\n", Label( targetXbox, nv2a ));
		ok = false;
		goto done;
	}

	if( layout.planes.length != header.lenPlanes ||
		memcmp( buf + layout.planes.offset, "AAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBB", header.lenPlanes ) != 0 )
	{
		printf( "TestValidArchive(%s): planes lump resolved to the wrong bytes\n", Label( targetXbox, nv2a ));
		ok = false;
	}

	if( layout.entities.length != header.lenEntities ||
		strcmp( (const char *)( buf + layout.entities.offset ), "{\n\"classname\" \"worldspawn\"\n}\n" ) != 0 )
	{
		printf( "TestValidArchive(%s): entities lump did not resolve to real entity text\n", Label( targetXbox, nv2a ));
		ok = false;
	}

	if( layout.texinfo.length != header.lenTexinfo )
	{
		printf( "TestValidArchive(%s): texinfo lump length mismatch\n", Label( targetXbox, nv2a ));
		ok = false;
	}

	// The six v7 lumps, each resolved to the bytes this test wrote there.
	if( layout.vertexes.length != header.lenVertexes || buf[layout.vertexes.offset] != 0x44 ||
		layout.edges.length != header.lenEdges || buf[layout.edges.offset] != 0x55 ||
		layout.surfedges.length != header.lenSurfedges || buf[layout.surfedges.offset] != 0x66 ||
		layout.faces.length != header.lenFaces || buf[layout.faces.offset] != 0x77 ||
		layout.lighting.length != header.lenLighting || buf[layout.lighting.offset] != 0x88 ||
		layout.miptex.length != header.lenMiptex || memcmp( buf + layout.miptex.offset + 8, "minimal_test", 12 ) != 0 )
	{
		printf( "TestValidArchive(%s): a v7 classic-core lump resolved to the wrong bytes\n", Label( targetXbox, nv2a ));
		ok = false;
	}

	if( nv2a )
	{
		if( targetXbox )
		{
			if( layout.textureDir.length != header.lenTextureDir ||
				layout.textureData.length != header.lenTextureData ||
				layout.batches.length != header.lenBatches ||
				layout.array64.length != 0 || layout.array256.length != 0 || layout.array512.length != 0 )
			{
				printf( "TestValidArchive(xbox/nv2a): texture branch populated the wrong fields\n" );
				ok = false;
			}
			else
			{
				// v9: resolve the real directory entry/batch this test wrote,
				// not just their lengths.
				mpak_texture_info_t readInfo;
				mpak_texture_batch_t readBatch;

				memcpy( &readInfo, buf + layout.textureDir.offset, sizeof( readInfo ));
				memcpy( &readBatch, buf + layout.batches.offset, sizeof( readBatch ));
				if( readInfo.width != 4 || readInfo.height != 4 ||
					readInfo.log2Width != 2 || readInfo.log2Height != 2 ||
					readInfo.dataLength != header.lenTextureData ||
					buf[layout.textureData.offset] != 0x22 ||
					readBatch.faceCount != 2 || readBatch.textureIndex != 0 )
				{
					printf( "TestValidArchive(xbox/nv2a): texture directory/batch resolved to the wrong bytes\n" );
					ok = false;
				}
			}
		}
		else if( layout.array256.length != header.lenArray256 ||
			layout.textureDir.length != 0 || layout.textureData.length != 0 || layout.batches.length != 0 )
		{
			printf( "TestValidArchive(pc/nv2a): texture branch populated the wrong fields\n" );
			ok = false;
		}

		// v9: the face remap is written for BOTH targets in the nv2a profile
		// (PakManager.cpp save()'s own "the face sort is unconditional"
		// comment) - a real bijection, not just a nonzero length.
		if( layout.faceRemap.length != header.lenFaceRemap )
		{
			printf( "TestValidArchive(%s): faceRemap lump length mismatch\n", Label( targetXbox, nv2a ));
			ok = false;
		}
		else
		{
			uint32_t remap[2];
			memcpy( remap, buf + layout.faceRemap.offset, sizeof( remap ));
			if( remap[0] != 1 || remap[1] != 0 )
			{
				printf( "TestValidArchive(%s): faceRemap lump resolved to the wrong bytes\n", Label( targetXbox, nv2a ));
				ok = false;
			}
		}

		if( layout.extraLightBlocks.length != header.lenExtraLightBlocks ||
			buf[layout.extraLightBlocks.offset] != 0x99 )
		{
			printf( "TestValidArchive(%s): extraLightBlocks lump resolved to the wrong bytes\n", Label( targetXbox, nv2a ));
			ok = false;
		}
	}
	else if( layout.textureDir.length != 0 || layout.textureData.length != 0 || layout.batches.length != 0 ||
		layout.faceRemap.length != 0 || layout.array256.length != 0 || layout.lightmapAtlas.length != 0 ||
		layout.extraLightBlocks.length != 0 )
	{
		printf( "TestValidArchive(%s): core profile must carry no baked block\n", Label( targetXbox, nv2a ));
		ok = false;
	}

done:
	free( buf );
	return ok;
}

// Every corruption case below must be REJECTED - each mutates one real,
// independently-verified-valid archive from BuildMinimalArchive() (PC
// target, core profile) in exactly one way.
static qboolean TestRejectsBadMagic( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	memcpy( header.magic, "XXXX", 4 );
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsBadMagic: accepted\n" );
	return rejected;
}

static qboolean TestRejectsBadVersion( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.version = 99;
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsBadVersion: accepted\n" );
	return rejected;
}

static qboolean TestRejectsHeaderSizeDrift( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.headerSize = (uint32_t)sizeof( header ) + 4; // simulate a drifted C++ struct
	rejected = !Mpak_ValidateHeader( &header );

	free( buf );
	if( !rejected ) printf( "TestRejectsHeaderSizeDrift: accepted\n" );
	return rejected;
}

static qboolean TestRejectsTruncatedFile( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	// Chop off the last sector (the miptex lump) - a real truncated download/copy.
	rejected = !Mpak_ComputeLayout( &header, fileSize - SECTOR, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsTruncatedFile: accepted\n" );
	return rejected;
}

static qboolean TestRejectsNonDivisiblePlanesLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.lenPlanes += 1; // no longer a multiple of sizeof(dplane_t)
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsNonDivisiblePlanesLength: accepted\n" );
	return rejected;
}

static qboolean TestRejectsNonDivisibleFacesLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.lenFaces += 1; // no longer a multiple of sizeof(dface_t) - a v7 lump's own check
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsNonDivisibleFacesLength: accepted\n" );
	return rejected;
}

static qboolean TestRejectsNonDivisibleExtraLightBlocksLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, true, &fileSize, NULL ); // nv2a - lenExtraLightBlocks is 0 otherwise
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.lenExtraLightBlocks += 1; // no longer a multiple of MPAK_EXTRALIGHTBLOCK_SIZE (20)
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsNonDivisibleExtraLightBlocksLength: accepted\n" );
	return rejected;
}

static qboolean TestRejectsLightmapSizeMismatch( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, true, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
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
// instead of rejecting. Mpak_PlaceLump takes a uint64_t length
// specifically so this case has nowhere to hide.
static qboolean TestRejectsUint32MaxRawLumpLength( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.lenVisibility = 0xFFFFFFFFu; // recordSize == 0 for visibility - only the bounds check can catch this
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsUint32MaxRawLumpLength: accepted\n" );
	return rejected;
}

// Companion case for the geometry block: vCount chosen so
// vCount * sizeof(mpak_vertex_t) is exactly 2^32 (67108864 * 64), which
// wraps to precisely 0 in 32-bit arithmetic - a claimed 67 million
// vertices would read back as "zero vertices, valid".
static qboolean TestRejectsOverflowingVertexCount( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	header.vCount = 67108864u; // * sizeof(mpak_vertex_t)=64 == 2^32 exactly
	rejected = !Mpak_ComputeLayout( &header, fileSize, &layout );

	free( buf );
	if( !rejected ) printf( "TestRejectsOverflowingVertexCount: accepted\n" );
	return rejected;
}

// Found in review (round 2): the unpadded length can fit exactly while
// the sector-rounded advance overflows a 32-bit signed fs_offset_t
// cursor (fileSize == INT32_MAX, length == INT32_MAX - 2048).
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

// Found in review (round 2): width=height=2^31, layers=1, bytesPerPixel=4
// makes the naive uint64_t product exactly 2^64, wrapping to 0, which
// would incorrectly match a declared lenLightmapAtlas of 0.
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

// v7: the dheader_t at offset 0 and the descriptor describe the same
// bytes twice. Each of these corrupts one half and must be caught.
static qboolean TestRejectsBspVersion( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected = false;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	if( Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		((dheader_t *)buf )->version = 29; // Q1BSP_VERSION - not what this archive claims to be
		rejected = !Mpak_ValidateBspHeader( buf, &layout );
	}

	free( buf );
	if( !rejected ) printf( "TestRejectsBspVersion: accepted\n" );
	return rejected;
}

static qboolean TestRejectsDriftedLumpTable( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected = false;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	if( Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		((dheader_t *)buf )->lumps[LUMP_LIGHTING].fileofs += SECTOR; // one sector off: what a writer/reader order drift looks like
		rejected = !Mpak_ValidateBspHeader( buf, &layout );
	}

	free( buf );
	if( !rejected ) printf( "TestRejectsDriftedLumpTable: accepted\n" );
	return rejected;
}

static qboolean TestRejectsExtraHeaderProbeBytes( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( false, false, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean rejected = false;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	if( Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		memcpy( buf + sizeof( dheader_t ), "XASH", 4 ); // IDEXTRAHEADER-shaped garbage where the engine probes for it
		rejected = !Mpak_ValidateBspHeader( buf, &layout );
	}

	free( buf );
	if( !rejected ) printf( "TestRejectsExtraHeaderProbeBytes: accepted\n" );
	return rejected;
}

int main( void )
{
	int ok = 1;

	ok &= TestValidArchive( false, false );
	ok &= TestValidArchive( true, false );
	ok &= TestValidArchive( false, true );
	ok &= TestValidArchive( true, true );
	ok &= TestRejectsBadMagic();
	ok &= TestRejectsBadVersion();
	ok &= TestRejectsHeaderSizeDrift();
	ok &= TestRejectsTruncatedFile();
	ok &= TestRejectsNonDivisiblePlanesLength();
	ok &= TestRejectsNonDivisibleFacesLength();
	ok &= TestRejectsNonDivisibleExtraLightBlocksLength();
	ok &= TestRejectsLightmapSizeMismatch();
	ok &= TestRejectsUint32MaxRawLumpLength();
	ok &= TestRejectsOverflowingVertexCount();
	ok &= TestRejectsPaddedExtentOverflow();
	ok &= TestRejectsLightmapDimensionOverflow();
	ok &= TestRejectsBspVersion();
	ok &= TestRejectsDriftedLumpTable();
	ok &= TestRejectsExtraHeaderProbeBytes();

	if( !ok )
		return EXIT_FAILURE;

	printf( "success\n" );
	return EXIT_SUCCESS;
}
