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
	const uint32_t lenFaceVertexBase = nv2a ? 2 * 4u : 0u; // v11: one base per face (both faces in chunk 0 here, as a multi-chunk map would ship)
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
	const uint32_t texDataLength = 24u; // v10: 4x4 DXT1 mip chain = 3 levels (4x4, 2x2, 1x1) x one 8-byte block
	const uint32_t lenTextureData = texturedXbox ? 128u : 0u; // v10: the chain zero-padded to 128 bytes
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
	fs_offset_t faceVertexBaseOffset;
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
	sector.desc.lenFaceVertexBase = lenFaceVertexBase;

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
	faceVertexBaseOffset = cursor;
	cursor += RoundUpSector( lenFaceVertexBase );

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
			// v10: one real 4x4 DXT1 texture directory entry + its 24-byte
			// mip chain padded to 128 + one texture-draw batch covering both
			// real faces.
			mpak_texture_info_t texInfo;
			mpak_texture_batch_t batch;

			texInfo.width = 4;
			texInfo.height = 4;
			texInfo.log2Width = 2;
			texInfo.log2Height = 2;
			texInfo.dataLength = texDataLength;
			memcpy( buf + textureDirOffset, &texInfo, sizeof( texInfo ));
			// v10 texture-directory rule (Mpak_ValidateTextureDirectory,
			// filesystem/mpak_texture_dir.h): padding bytes past a real
			// texture's own dataLength must be zero. Fill only the REAL
			// texDataLength (24) bytes with the sentinel 0x22 - the
			// remaining 104 bytes to the 128-byte boundary stay whatever
			// calloc() already zeroed them to, real zero padding, not
			// filler this new check would reject.
			memset( buf + textureDataOffset, 0x22, texDataLength );

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
		memset( buf + faceVertexBaseOffset, 0x77, lenFaceVertexBase );
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
					readInfo.dataLength != 24u || header.lenTextureData != 128u || // v10: 4x4 chain, padded
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

		// v11: the per-face vertex-chunk bases are the last lump.
		if( layout.faceVertexBase.length != header.lenFaceVertexBase ||
			buf[layout.faceVertexBase.offset] != 0x77 )
		{
			printf( "TestValidArchive(%s): faceVertexBase lump resolved to the wrong bytes\n", Label( targetXbox, nv2a ));
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

// ------------------------------------------------------------------------
// mpak_texture_dir.h: the v10 texture-directory rule (chain length,
// 128-byte padding, per-entry/whole-lump acceptance) - shared with
// ref/nv2a/r_context.c and tools/asset-studio's PakManager::verifyFile.
// Every expected value below is a hand-computed literal (worked out
// independently, the same discipline the rest of this file already
// follows), never a value read back from the function under test.
// ------------------------------------------------------------------------

// Writes one raw mpak_texture_info_t entry at dst, from explicit fields -
// never from Mpak_Dxt1ChainLength or any other function under test.
static void WriteTexInfo( byte *dst, uint32_t width, uint32_t height, uint32_t log2w, uint32_t log2h, uint32_t dataLength )
{
	mpak_texture_info_t info;
	info.width = width;
	info.height = height;
	info.log2Width = log2w;
	info.log2Height = log2h;
	info.dataLength = dataLength;
	memcpy( dst, &info, sizeof( info ));
}

// (a) Mpak_Dxt1ChainLength: hand-computed length/levels for a spread of
// square and non-square sizes, including the largest (4096x4096) and most
// aspect-skewed (4096x4, 4x64) cases this format allows.
static qboolean TestDxt1ChainLength( void )
{
	static const struct { uint32_t log2w, log2h; uint64_t length; uint32_t levels; } cases[] = {
		{ 2, 2, 24,       3 },  // 4x4
		{ 4, 4, 184,      5 },  // 16x16
		{ 6, 2, 264,      7 },  // 64x4
		{ 2, 6, 264,      7 },  // 4x64
		{ 8, 4, 2824,     9 },  // 256x16
		{ 12, 12, 11184824, 13 }, // 4096x4096
		{ 12, 2, 16392,   13 }, // 4096x4
	};
	size_t i;
	qboolean ok = true;

	for( i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
	{
		uint32_t levels = 0;
		uint64_t length = Mpak_Dxt1ChainLength( cases[i].log2w, cases[i].log2h, &levels );

		if( length != cases[i].length || levels != cases[i].levels )
		{
			printf( "TestDxt1ChainLength(log2 %u,%u): got length %llu levels %u, expected %llu %u\n",
				(unsigned int)cases[i].log2w, (unsigned int)cases[i].log2h,
				(unsigned long long)length, (unsigned int)levels,
				(unsigned long long)cases[i].length, (unsigned int)cases[i].levels );
			ok = false;
		}
	}
	return ok;
}

// (b) A real BuildMinimalArchive(true, true) texture dir/data, at the real
// offsets Mpak_ComputeLayout resolves - the same bytes TestValidArchive
// already proved correct - must validate OK with the padding-content check
// ON (unlike ref/nv2a, which always passes data = NULL).
static qboolean TestValidatesRealArchiveTextureDirectory( void )
{
	fs_offset_t fileSize;
	byte *buf = BuildMinimalArchive( true, true, &fileSize, NULL );
	mpak_header_t header;
	mpak_layout_t layout;
	qboolean ok = true;
	uint32_t bad = 0xDEADBEEFu;
	mpak_texdir_status_t status;

	memcpy( &header, buf + MPAK_DESCRIPTOR_OFFSET, sizeof( header ));
	if( !Mpak_ComputeLayout( &header, fileSize, &layout ))
	{
		printf( "TestValidatesRealArchiveTextureDirectory: ComputeLayout rejected a valid archive\n" );
		ok = false;
		goto done;
	}

	status = Mpak_ValidateTextureDirectory( buf + layout.textureDir.offset, (uint64_t)layout.textureDir.length,
		header.textureCount, buf + layout.textureData.offset, (uint64_t)layout.textureData.length, &bad );
	if( status != MPAK_TEXDIR_OK )
	{
		printf( "TestValidatesRealArchiveTextureDirectory: rejected a real archive (%s, entry %u)\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

done:
	free( buf );
	return ok;
}

// (c) Three real entries (4x4, 64x4, 256x16) laid out back to back, each
// zero-padded to 128 bytes: 128 + 384 + 2944 = 3456. The whole buffer is
// calloc'd (real zero), so the "real" data bytes double as valid zero
// padding content too - only the boundary math matters here.
static qboolean TestValidatesThreeTextureLump( void )
{
	byte dir[3 * MPAK_TEXTURE_INFO_SIZE];
	byte *data;
	const uint64_t dataLength = 128 + 384 + 2944;
	mpak_texdir_status_t status;
	uint32_t bad = 0;
	qboolean ok;

	WriteTexInfo( dir + 0 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );
	WriteTexInfo( dir + 1 * MPAK_TEXTURE_INFO_SIZE, 64, 4, 6, 2, 264 );
	WriteTexInfo( dir + 2 * MPAK_TEXTURE_INFO_SIZE, 256, 16, 8, 4, 2824 );

	data = (byte *)calloc( 1, (size_t)dataLength );
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 3, data, dataLength, &bad );
	ok = ( status == MPAK_TEXDIR_OK );
	if( !ok )
		printf( "TestValidatesThreeTextureLump: rejected (%s, entry %u)\n", Mpak_TextureDirStatusString( status ), (unsigned int)bad );

	free( data );
	return ok;
}

// (d) textureCount 0, dirLength 0, dataLength 0 - rejecting a texture-less
// map is the CALLER's policy (ref/nv2a's own NV2A_MAP_MAX_TEXTURES == 0
// check), not this function's.
static qboolean TestValidatesZeroTextureCount( void )
{
	mpak_texdir_status_t status = Mpak_ValidateTextureDirectory( NULL, 0, 0, NULL, 0, NULL );
	qboolean ok = ( status == MPAK_TEXDIR_OK );
	if( !ok )
		printf( "TestValidatesZeroTextureCount: rejected (%s)\n", Mpak_TextureDirStatusString( status ));
	return ok;
}

// (e) v9-style single-level sizes must be rejected now that dataLength has
// to be the FULL chain length.
static qboolean TestRejectsV9StyleChainLength( void )
{
	byte dir1[MPAK_TEXTURE_INFO_SIZE];
	byte dir2[2 * MPAK_TEXTURE_INFO_SIZE];
	mpak_texdir_status_t status;
	uint32_t bad;
	qboolean ok = true;

	// 16x16, dataLength 128 (v9's single 4x4-block-wide level, not the
	// real 184-byte 5-level chain).
	WriteTexInfo( dir1, 16, 16, 4, 4, 128 );
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir1, sizeof( dir1 ), 1, NULL, 128, &bad );
	if( status != MPAK_TEXDIR_BAD_CHAIN_LENGTH || bad != 0 )
	{
		printf( "TestRejectsV9StyleChainLength(single): got %s at %u, expected BAD_CHAIN_LENGTH at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	// A valid 4x4 (entry 0) followed by a 256x16 whose dataLength is the
	// v9 single-level size (2048), not the real 2824-byte chain.
	WriteTexInfo( dir2 + 0 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );
	WriteTexInfo( dir2 + 1 * MPAK_TEXTURE_INFO_SIZE, 256, 16, 8, 4, 2048 );
	bad = 0xFFFFFFFFu;
	// Lump length is never reached (the chain-length check on entry 1
	// fails first) - value is unused, filled in only to be a real,
	// answerable parameter.
	status = Mpak_ValidateTextureDirectory( dir2, sizeof( dir2 ), 2, NULL, 128 + 2944, &bad );
	if( status != MPAK_TEXDIR_BAD_CHAIN_LENGTH || bad != 1 )
	{
		printf( "TestRejectsV9StyleChainLength(pair): got %s at %u, expected BAD_CHAIN_LENGTH at 1\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	return ok;
}

// (f) v9-style unpadded sum: two valid 4x4 entries (24 + 24 = 48) over a
// 48-byte lump - correct for v9 (no padding), wrong for v10 (each must be
// aligned to 128 first).
static qboolean TestRejectsUnpaddedSum( void )
{
	byte dir[2 * MPAK_TEXTURE_INFO_SIZE];
	mpak_texdir_status_t status;
	uint32_t bad = 0xFFFFFFFFu;
	qboolean ok;

	WriteTexInfo( dir + 0 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );
	WriteTexInfo( dir + 1 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );

	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 2, NULL, 48, &bad );
	ok = ( status == MPAK_TEXDIR_DATA_LENGTH_MISMATCH );
	if( !ok )
		printf( "TestRejectsUnpaddedSum: got %s, expected DATA_LENGTH_MISMATCH\n", Mpak_TextureDirStatusString( status ));
	return ok;
}

// (g) Non-zero padding content - the check ref/nv2a opts OUT of (it always
// passes data = NULL) but the PC tests and the baker's verify pass don't.
static qboolean TestRejectsNonzeroPadding( void )
{
	byte dir[MPAK_TEXTURE_INFO_SIZE];
	byte data[128];
	mpak_texdir_status_t status;
	uint32_t bad;
	qboolean ok = true;

	WriteTexInfo( dir, 4, 4, 2, 2, 24 );

	// First padding byte (offset 24, right after the real 24 bytes).
	memset( data, 0, sizeof( data ));
	data[24] = 1;
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, data, sizeof( data ), &bad );
	if( status != MPAK_TEXDIR_NONZERO_PADDING || bad != 0 )
	{
		printf( "TestRejectsNonzeroPadding(first): got %s at %u, expected NONZERO_PADDING at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}
	// The same buffer with data = NULL (ref/nv2a's own opt-out) must
	// accept it - documents that the console's accept/reject set is
	// unaffected by padding content.
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, NULL, sizeof( data ), NULL );
	if( status != MPAK_TEXDIR_OK )
	{
		printf( "TestRejectsNonzeroPadding(first, data=NULL): got %s, expected OK\n", Mpak_TextureDirStatusString( status ));
		ok = false;
	}

	// Last byte of the padded extent (offset 127).
	memset( data, 0, sizeof( data ));
	data[127] = 1;
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, data, sizeof( data ), &bad );
	if( status != MPAK_TEXDIR_NONZERO_PADDING || bad != 0 )
	{
		printf( "TestRejectsNonzeroPadding(last): got %s at %u, expected NONZERO_PADDING at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, NULL, sizeof( data ), NULL );
	if( status != MPAK_TEXDIR_OK )
	{
		printf( "TestRejectsNonzeroPadding(last, data=NULL): got %s, expected OK\n", Mpak_TextureDirStatusString( status ));
		ok = false;
	}

	return ok;
}

// (h) log2 outside [2,12] - one texture each, otherwise real/self-consistent.
static qboolean TestRejectsBadLog2Range( void )
{
	byte dir[MPAK_TEXTURE_INFO_SIZE];
	mpak_texdir_status_t status;
	uint32_t bad;
	qboolean ok = true;

	// log2Width 1 (below MPAK_TEXTURE_LOG2_MIN) - width itself matches (2 == 1<<1).
	WriteTexInfo( dir, 2, 4, 1, 2, 0 );
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, NULL, 0, &bad );
	if( status != MPAK_TEXDIR_BAD_DIMENSIONS || bad != 0 )
	{
		printf( "TestRejectsBadLog2Range(low): got %s at %u, expected BAD_DIMENSIONS at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	// log2Width 13 (above MPAK_TEXTURE_LOG2_MAX) - width matches (8192 == 1<<13).
	WriteTexInfo( dir, 8192, 4, 13, 2, 0 );
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, NULL, 0, &bad );
	if( status != MPAK_TEXDIR_BAD_DIMENSIONS || bad != 0 )
	{
		printf( "TestRejectsBadLog2Range(high): got %s at %u, expected BAD_DIMENSIONS at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	return ok;
}

// (i) width disagrees with its own paired log2 field (16 != 1<<5 == 32).
static qboolean TestRejectsWidthLog2Mismatch( void )
{
	byte dir[MPAK_TEXTURE_INFO_SIZE];
	mpak_texdir_status_t status;
	uint32_t bad = 0xFFFFFFFFu;
	qboolean ok;

	WriteTexInfo( dir, 16, 16, 5, 4, 0 );
	status = Mpak_ValidateTextureDirectory( dir, sizeof( dir ), 1, NULL, 0, &bad );
	ok = ( status == MPAK_TEXDIR_BAD_DIMENSIONS && bad == 0 );
	if( !ok )
		printf( "TestRejectsWidthLog2Mismatch: got %s at %u, expected BAD_DIMENSIONS at 0\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
	return ok;
}

// (j) The whole-lump running-sum check, both directions, including a
// buffer allocated at EXACTLY the (too-short) lump size so an overread
// while validating the second entry would be a real out-of-bounds access
// (catchable by ASan/valgrind), not just a wrong comparison.
static qboolean TestRejectsLumpLengthMismatch( void )
{
	byte dir1[MPAK_TEXTURE_INFO_SIZE];
	byte dir2[2 * MPAK_TEXTURE_INFO_SIZE];
	byte *data;
	mpak_texdir_status_t status;
	uint32_t bad;
	qboolean ok = true;

	// One alignment unit (128) too long for a single real 4x4 texture.
	WriteTexInfo( dir1, 4, 4, 2, 2, 24 );
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir1, sizeof( dir1 ), 1, NULL, 256, &bad );
	if( status != MPAK_TEXDIR_DATA_LENGTH_MISMATCH || bad != 1 ) // bad index == textureCount
	{
		printf( "TestRejectsLumpLengthMismatch(long): got %s at %u, expected DATA_LENGTH_MISMATCH at 1\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	// Two valid 4x4 entries (128 + 128 = 256 real bytes needed), but the
	// lump is only 200 bytes - too short to hold the second entry's own
	// padded extent. data is allocated at exactly 200 bytes: the early-out
	// in Mpak_ValidateTextureDirectory must reject entry 1 BEFORE reading
	// any of its bytes, since [128, 256) is past the end of this buffer.
	WriteTexInfo( dir2 + 0 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );
	WriteTexInfo( dir2 + 1 * MPAK_TEXTURE_INFO_SIZE, 4, 4, 2, 2, 24 );
	data = (byte *)calloc( 1, 200 );
	bad = 0xFFFFFFFFu;
	status = Mpak_ValidateTextureDirectory( dir2, sizeof( dir2 ), 2, data, 200, &bad );
	if( status != MPAK_TEXDIR_DATA_LENGTH_MISMATCH || bad != 1 )
	{
		printf( "TestRejectsLumpLengthMismatch(short): got %s at %u, expected DATA_LENGTH_MISMATCH at 1\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}
	free( data );

	return ok;
}

// (k) dirLength is not textureCount * MPAK_TEXTURE_INFO_SIZE.
static qboolean TestRejectsBadDirectoryLength( void )
{
	byte dir[MPAK_TEXTURE_INFO_SIZE + 4]; // room for either mutation below
	mpak_texdir_status_t status;
	uint32_t bad;
	qboolean ok = true;

	WriteTexInfo( dir, 4, 4, 2, 2, 24 );

	bad = 0;
	status = Mpak_ValidateTextureDirectory( dir, MPAK_TEXTURE_INFO_SIZE - 1, 1, NULL, 0, &bad );
	if( status != MPAK_TEXDIR_BAD_DIRECTORY_LENGTH || bad != 0xFFFFFFFFu )
	{
		printf( "TestRejectsBadDirectoryLength(19): got %s at %u, expected BAD_DIRECTORY_LENGTH at UINT32_MAX\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	bad = 0;
	status = Mpak_ValidateTextureDirectory( dir, MPAK_TEXTURE_INFO_SIZE + 1, 1, NULL, 0, &bad );
	if( status != MPAK_TEXDIR_BAD_DIRECTORY_LENGTH || bad != 0xFFFFFFFFu )
	{
		printf( "TestRejectsBadDirectoryLength(21): got %s at %u, expected BAD_DIRECTORY_LENGTH at UINT32_MAX\n",
			Mpak_TextureDirStatusString( status ), (unsigned int)bad );
		ok = false;
	}

	return ok;
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

	// mpak_texture_dir.h - the v10 texture-directory rule.
	ok &= TestDxt1ChainLength();
	ok &= TestValidatesRealArchiveTextureDirectory();
	ok &= TestValidatesThreeTextureLump();
	ok &= TestValidatesZeroTextureCount();
	ok &= TestRejectsV9StyleChainLength();
	ok &= TestRejectsUnpaddedSum();
	ok &= TestRejectsNonzeroPadding();
	ok &= TestRejectsBadLog2Range();
	ok &= TestRejectsWidthLog2Mismatch();
	ok &= TestRejectsLumpLengthMismatch();
	ok &= TestRejectsBadDirectoryLength();

	if( !ok )
		return EXIT_FAILURE;

	printf( "success\n" );
	return EXIT_SUCCESS;
}
