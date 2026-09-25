/*
mpak_layout.c - see mpak_layout.h for what this computes and why

Part of Resonance3D; see docs/r3d/fork-plan.md's X2 section.

Copyright (C) Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "build.h"
#include <stddef.h>
#include <string.h>
#include "mpak_layout.h"
// Con_Reportf is filesystem_internal.h's own macro forwarding through
// g_engfuncs._Con_Reportf, not a plain linkable function - found while
// writing this file: a hand-written `void Con_Reportf(...)` declaration
// compiles fine but has nothing to link against. Pulling in the real
// header is what every other file in this directory (pak.c, mpak.c) does
// for the same reason.
#include "filesystem_internal.h"
#include "common/com_strings.h" // engine/common/com_strings.h - a DIFFERENT "common/" than the one below
#include "bspfile.h" // common/bspfile.h - dplane_t/dnode_t/dleaf_t/dclipnode_t/dmodel_t/dtexinfo_t,
                    // real sizes, not guessed. Referenced without a "common/" prefix on purpose -
                    // see mpak_layout.h's own include comment for why

// MpakFaceInfo (tools/asset-studio/src/formats/MpakHeader.h) is
// #pragma pack(push,1) and its own file already static_asserts
// sizeof(MpakFaceInfo) == 44 - cited, not re-derived, since this file has
// no reason to duplicate that struct just to learn its size.
#define MPAK_FACEINFO_SIZE 44u

// MpakExtraLightBlock (same header) - #pragma pack(push,1), own file
// static_asserts sizeof == 20. Same "cited, not re-derived" reasoning.
#define MPAK_EXTRALIGHTBLOCK_SIZE 20u

// Multiplies a running product by another dimension, rejecting as soon as
// the true (unbounded) product would exceed what any real uint32_t
// lenLightmapAtlas could ever match - checked incrementally so three
// arbitrary uint32_t dimensions can never conspire to overflow even
// uint64_t and wrap back around to a small, falsely-matching value.
// Found in review: width=height=2^31, layers=1, bytesPerPixel=4 makes
// the naive uint64_t product exactly 2^64, which wraps to 0 - an
// "impossible" multi-gigapixel lightmap would have passed as matching a
// declared lenLightmapAtlas of 0.
static qboolean Mpak_CheckedMulU32( uint64_t *product, uint32_t factor )
{
	if( factor != 0 && *product > (uint64_t)UINT32_MAX / factor )
		return false;
	*product *= factor;
	return true;
}

qboolean Mpak_ValidateHeader( const mpak_header_t *header )
{
	if( memcmp( header->magic, MPAK_LAYOUT_MAGIC, sizeof( header->magic )) != 0 )
	{
		Con_Reportf( S_ERROR "Mpak_ValidateHeader: bad magic\n" );
		return false;
	}

	if( header->version != MPAK_LAYOUT_VERSION )
	{
		Con_Reportf( S_ERROR "Mpak_ValidateHeader: version %u, this build only reads version %u\n",
			header->version, MPAK_LAYOUT_VERSION );
		return false;
	}

	// A mismatch here means the C mirror above and the real C++
	// MpakHeader6 (tools/asset-studio/src/formats/MpakHeader.h) have
	// drifted out of byte-for-byte sync - every offset this file computes
	// past the header would be built from the wrong field, silently, so
	// this is a hard failure, not a warning.
	if( header->headerSize != sizeof( mpak_header_t ))
	{
		Con_Reportf( S_ERROR "Mpak_ValidateHeader: header reports %u bytes, this build's mpak_header_t "
			"is %u bytes - the C reader and the C++ writer have drifted out of sync\n",
			header->headerSize, (uint32_t)sizeof( mpak_header_t ));
		return false;
	}

	if( header->headerSize > MPAK_SECTOR_SIZE )
	{
		Con_Reportf( S_ERROR "Mpak_ValidateHeader: header reports %u bytes, larger than the "
			"%u-byte sector every archive pads it to\n", header->headerSize, MPAK_SECTOR_SIZE );
		return false;
	}

	return true;
}

// Places one lump at the current cursor, bounds-checks it against the real
// file size, and advances the cursor by its sector-rounded extent (0 if
// the lump itself is empty - PakManager.cpp's save() writes literally
// nothing, not even a placeholder sector, for a zero-length section).
// recordSize is 0 for raw byte blobs (visibility, entities, texture
// blobs); for anything else, a length that isn't an exact multiple is a
// truncated/corrupted archive, caught here rather than producing a
// fractional record count later.
//
// length is uint64_t, not fs_offset_t, on purpose - found in review:
// fs_offset_t is off_t (common/xash3d_types.h:48), and Xbox's off_t is a
// 32-bit signed long (compat/xbox/sys/types.h:18). Every length here
// ultimately comes from a uint32_t header field an untrusted/corrupted
// archive controls; narrowing lenVisibility = 0xFFFFFFFF straight to a
// 32-bit signed fs_offset_t makes it -1, which the old `length <= 0`
// check then treated as "empty, skip" instead of rejecting - a corrupted
// 4 GB lump silently vanishing instead of failing loudly. Taking a
// uint64_t here can represent any uint32_t header field faithfully
// (widening, never narrowing) regardless of the platform's fs_offset_t
// width, so the bounds check below is what actually decides whether a
// value is acceptable - not the parameter's own type.
static qboolean Mpak_PlaceLump( const char *name, uint64_t length, uint32_t recordSize,
	fs_offset_t fileSize, fs_offset_t *cursor, mpak_lump_t *out )
{
	if( recordSize > 0 && ( length % recordSize ) != 0 )
	{
		Con_Reportf( S_ERROR "Mpak_ComputeLayout: %s length %llu is not a multiple of its %u-byte "
			"record size - truncated or corrupted archive\n", name, (unsigned long long)length, recordSize );
		return false;
	}

	out->offset = *cursor;
	out->length = 0;

	if( length == 0 )
		return true;

	// Found in a second review pass: the first fix here bounds-checked
	// only the UNPADDED length against fileSize, then advanced *cursor by
	// the sector-ROUNDED extent separately - a length that fits exactly
	// (e.g. fileSize == INT32_MAX, length == INT32_MAX - cursor, both
	// individually in range) can still round up to one byte past what a
	// 32-bit signed fs_offset_t cursor can hold, overflowing it there
	// instead. Checking the PADDED extent up front - in uint64_t, before
	// *cursor is ever touched - closes that gap: nothing is written to
	// *cursor or out->length until the full sector-rounded advance is
	// already known to fit inside the remaining space.
	//
	// This uint64_t arithmetic cannot itself overflow: every real caller
	// in this file passes a length bounded well under 2^40 (the widest is
	// vCount * sizeof(mpak_vertex_t), at most (2^32-1)*64 =~ 2^38), so
	// length + (MPAK_SECTOR_SIZE-1) has enormous headroom below UINT64_MAX.
	{
		uint64_t padded = (( length + ( MPAK_SECTOR_SIZE - 1 )) / MPAK_SECTOR_SIZE ) * MPAK_SECTOR_SIZE;

		// fileSize (a real, already-measured file size - never attacker
		// content) and *cursor (always <= fileSize, maintained by every
		// prior successful placement) are both valid non-negative
		// fs_offset_t values, so fileSize - *cursor cannot underflow.
		if( padded > (uint64_t)( fileSize - *cursor ))
		{
			Con_Reportf( S_ERROR "Mpak_ComputeLayout: %s (offset %lld, length %llu, padded %llu) runs "
				"past the archive's real size (%lld) - truncated or corrupted archive\n",
				name, (long long)*cursor, (unsigned long long)length, (unsigned long long)padded, (long long)fileSize );
			return false;
		}

		out->length = (fs_offset_t)length; // safe: padded >= length, and padded already fits in the remaining space
		*cursor += (fs_offset_t)padded;
	}

	return true;
}

qboolean Mpak_ComputeLayout( const mpak_header_t *header, fs_offset_t fileSize, mpak_layout_t *out )
{
	fs_offset_t cursor;

	memset( out, 0, sizeof( *out ));

	if( fileSize < MPAK_SECTOR_SIZE )
	{
		Con_Reportf( S_ERROR "Mpak_ComputeLayout: archive is %lld bytes, smaller than one header "
			"sector (%u)\n", (long long)fileSize, MPAK_SECTOR_SIZE );
		return false;
	}

	// The header itself always occupies exactly one full sector -
	// PakManager.cpp's save() writes it via the same writePadded() every
	// other section uses, unconditionally (no `if (len > 0)` guard, since
	// the header is never empty).
	cursor = MPAK_SECTOR_SIZE;

	// v12 (MpakHeader.h's v11 -> v12 note): every classic BSP lump comes
	// first, then every renderer-only section. filesystem/mpak.c serves
	// the engine only the prefix that ends at classicEnd, which is why the
	// grouping - not just the order within each group - is the format.
	// Raw BSP-format lumps, byte-identical copies of the source .bsp's own
	// LUMP_VISIBILITY/NODES/LEAVES/MARKSURFACES/PLANES/CLIPNODES/MODELS
	// (verified against tools/asset-studio/src/formats/BspParser.cpp's
	// getLumpData() calls feeding PakManager::setXData()) - real GoldSrc
	// struct sizes from common/bspfile.h, not guessed.
	if( !Mpak_PlaceLump( "visibility", header->lenVisibility, 0, fileSize, &cursor, &out->visibility ))
		return false;
	if( !Mpak_PlaceLump( "nodes", header->lenNodes, sizeof( dnode_t ), fileSize, &cursor, &out->nodes ))
		return false;
	if( !Mpak_PlaceLump( "leaves", header->lenLeaves, sizeof( dleaf_t ), fileSize, &cursor, &out->leaves ))
		return false;
	if( !Mpak_PlaceLump( "marksurfaces", header->lenMarksurfaces, sizeof( dmarkface_t ),
			fileSize, &cursor, &out->marksurfaces ))
		return false;
	if( !Mpak_PlaceLump( "planes", header->lenPlanes, sizeof( dplane_t ), fileSize, &cursor, &out->planes ))
		return false;
	if( !Mpak_PlaceLump( "clipnodes", header->lenClipnodes, sizeof( dclipnode_t ),
			fileSize, &cursor, &out->clipnodes ))
		return false;
	if( !Mpak_PlaceLump( "models", header->lenModels, sizeof( dmodel_t ), fileSize, &cursor, &out->models ))
		return false;

	if( !Mpak_PlaceLump( "entities", header->lenEntities, 0, fileSize, &cursor, &out->entities ))
		return false;

	if( !Mpak_PlaceLump( "texinfo", header->lenTexinfo, sizeof( dtexinfo_t ), fileSize, &cursor, &out->texinfo ))
		return false;

	// v7 classic core (MpakHeader.h's v6 -> v7 note), in this order. Real
	// GoldSrc record sizes from common/bspfile.h; LIGHTING is a sample blob
	// and the miptex lump is self-describing, so those two get no
	// divisibility check.
	if( !Mpak_PlaceLump( "vertexes", header->lenVertexes, sizeof( dvertex_t ), fileSize, &cursor, &out->vertexes ))
		return false;
	if( !Mpak_PlaceLump( "edges", header->lenEdges, sizeof( dedge_t ), fileSize, &cursor, &out->edges ))
		return false;
	if( !Mpak_PlaceLump( "surfedges", header->lenSurfedges, sizeof( dsurfedge_t ), fileSize, &cursor, &out->surfedges ))
		return false;
	if( !Mpak_PlaceLump( "faces", header->lenFaces, sizeof( dface_t ), fileSize, &cursor, &out->faces ))
		return false;
	if( !Mpak_PlaceLump( "lighting", header->lenLighting, 0, fileSize, &cursor, &out->lighting ))
		return false;
	if( !Mpak_PlaceLump( "miptex", header->lenMiptex, 0, fileSize, &cursor, &out->miptex ))
		return false;

	// Everything the engine's own map loader reads lies before this
	// offset; the first sector's dheader_t points nowhere past it.
	out->classicEnd = cursor;

	// Renderer-only sections from here on. Geometry: no length field of
	// its own in the header at all (only counts).
	// Widened to uint64_t BEFORE multiplying, not after - found in the same
	// review pass as Mpak_PlaceLump's own length-narrowing fix above: the
	// previous `(fs_offset_t)header->vCount * sizeof(...)` cast vCount to a
	// possibly-32-bit signed type first, so the multiplication itself could
	// already have wrapped before Mpak_PlaceLump ever saw the result.
	if( !Mpak_PlaceLump( "vertices", (uint64_t)header->vCount * sizeof( mpak_vertex_t ),
			0, fileSize, &cursor, &out->vertices ))
		return false;
	if( !Mpak_PlaceLump( "indices", (uint64_t)header->iCount * sizeof( uint16_t ),
			0, fileSize, &cursor, &out->indices ))
		return false;

	// FaceDrawCmd (tools/asset-studio/src/formats/PakManager.h): two
	// uint32_t, 8 bytes - not a real BSP lump; v12 moved it out of the
	// classic lumps, after the indices.
	if( !Mpak_PlaceLump( "faceDrawCmds", header->lenFaceDrawCmds, 2 * sizeof( uint32_t ),
			fileSize, &cursor, &out->faceDrawCmds ))
		return false;

	// Textures: v9 replaces the single shared atlas (v6-v8) with a
	// per-texture DXT1 directory + draw-batch table, Xbox target only - a
	// PC-target archive still has ONLY the three DXT arrays. Write order
	// matches PakManager.cpp save() and ref/nv2a/r_context.c's independent
	// nv2a_compute_mpak_layout mirror exactly (MpakHeader.h's v8 -> v9
	// note): texture directory, texture data, batches (Xbox only), then
	// the face remap - which, unlike the first three, is written for BOTH
	// targets, since BspParser's face sort runs in the shared nv2a face
	// loop regardless of target (PakManager.cpp save()'s own comment).
	// mpak_texture_info_t/mpak_texture_batch_t are fixed-size records, so
	// - like faceInfo/planes/etc. above - a declared length that isn't an
	// exact multiple of their real size is a truncated/corrupted archive,
	// caught by Mpak_PlaceLump's own recordSize check rather than producing
	// a fractional directory/batch count later. This function does NOT
	// validate the texture directory's own CONTENT (chain lengths, 128-byte
	// padding) - it only places the lump and checks its length is a whole
	// number of entries. That rule lives in mpak_texture_dir.h's
	// Mpak_ValidateTextureDirectory(), which ref/nv2a's real map loader
	// calls after Mpak_ComputeLayout places this lump (no runtime path in
	// this directory reads directory content, only placement).
	if( header->flags & MPAK_TARGET_XBOX_FLAG )
	{
		if( !Mpak_PlaceLump( "textureDir", header->lenTextureDir, sizeof( mpak_texture_info_t ),
				fileSize, &cursor, &out->textureDir ))
			return false;
		if( !Mpak_PlaceLump( "textureData", header->lenTextureData, 0, fileSize, &cursor, &out->textureData ))
			return false;
		if( !Mpak_PlaceLump( "batches", header->lenBatches, sizeof( mpak_texture_batch_t ),
				fileSize, &cursor, &out->batches ))
			return false;
	}

	if( !Mpak_PlaceLump( "faceRemap", header->lenFaceRemap, sizeof( uint32_t ), fileSize, &cursor, &out->faceRemap ))
		return false;

	if( !( header->flags & MPAK_TARGET_XBOX_FLAG ))
	{
		if( !Mpak_PlaceLump( "array64", header->lenArray64, 0, fileSize, &cursor, &out->array64 ))
			return false;
		if( !Mpak_PlaceLump( "array256", header->lenArray256, 0, fileSize, &cursor, &out->array256 ))
			return false;
		if( !Mpak_PlaceLump( "array512", header->lenArray512, 0, fileSize, &cursor, &out->array512 ))
			return false;
	}

	// Always empty under X2's narrowed scope (MpakHeader.h's own note),
	// but still real bytes to skip past if a stale pre-narrowing archive
	// somehow declares them.
	if( !Mpak_PlaceLump( "assetDict", header->lenAssetDict, 0, fileSize, &cursor, &out->assetDict ))
		return false;
	if( !Mpak_PlaceLump( "assetData", header->lenAssetData, 0, fileSize, &cursor, &out->assetData ))
		return false;

	// Cross-checked against the header's own declared dimensions, not
	// just bounds-checked - a real correctness signal beyond "fits in the
	// file", since lightmapWidth/Height/Layers/Format are already known.
	if( header->lightmapLayers > 0 )
	{
		uint32_t bytesPerPixel = ( header->lightmapFormat == 0 ) ? 4 : 0; // 0 = RGBA8; other formats not defined yet
		uint64_t expected = 1;
		qboolean fits;

		if( bytesPerPixel == 0 )
		{
			Con_Reportf( S_ERROR "Mpak_ComputeLayout: unknown lightmapFormat %u\n", header->lightmapFormat );
			return false;
		}

		// Checked multiplication, not a plain uint64_t product - see
		// Mpak_CheckedMulU32's own comment for why a plain product isn't
		// enough here.
		fits = Mpak_CheckedMulU32( &expected, header->lightmapWidth ) &&
			Mpak_CheckedMulU32( &expected, header->lightmapHeight ) &&
			Mpak_CheckedMulU32( &expected, header->lightmapLayers ) &&
			Mpak_CheckedMulU32( &expected, bytesPerPixel );

		if( !fits )
		{
			Con_Reportf( S_ERROR "Mpak_ComputeLayout: lightmap dimensions %ux%ux%u are too large to "
				"represent as any real byte count - corrupted archive\n",
				header->lightmapWidth, header->lightmapHeight, header->lightmapLayers );
			return false;
		}

		if( expected != (uint64_t)header->lenLightmapAtlas )
		{
			Con_Reportf( S_ERROR "Mpak_ComputeLayout: lightmap atlas is %u bytes, expected %llu from "
				"%ux%ux%u at %u bytes/pixel\n", header->lenLightmapAtlas, (unsigned long long)expected,
				header->lightmapWidth, header->lightmapHeight, header->lightmapLayers, bytesPerPixel );
			return false;
		}
	}
	if( !Mpak_PlaceLump( "lightmapAtlas", header->lenLightmapAtlas, 0, fileSize, &cursor, &out->lightmapAtlas ))
		return false;

	if( !Mpak_PlaceLump( "faceInfo", header->lenFaceInfo, MPAK_FACEINFO_SIZE, fileSize, &cursor, &out->faceInfo ))
		return false;
	// v8 (MpakHeader.h's v7 -> v8 note).
	if( !Mpak_PlaceLump( "extraLightBlocks", header->lenExtraLightBlocks, MPAK_EXTRALIGHTBLOCK_SIZE,
			fileSize, &cursor, &out->extraLightBlocks ))
		return false;

	// v11 (MpakHeader.h's v10 -> v11 note), appended last: one uint32_t
	// vertex-chunk base per sorted face, present only on multi-chunk maps.
	if( !Mpak_PlaceLump( "faceVertexBase", header->lenFaceVertexBase, sizeof( uint32_t ),
			fileSize, &cursor, &out->faceVertexBase ))
		return false;

	return true;
}

qboolean Mpak_ValidateBspHeader( const void *firstSector, const mpak_layout_t *layout )
{
	// dheader_t lump index -> the layout member holding the same bytes.
	// LUMP_* values come from common/bspfile.h, the same table
	// mod_bmodel.c's srclumps[] is built from.
	static const struct
	{
		int         lump;
		size_t      member;
		const char *name;
	} map[] =
	{
		{ LUMP_ENTITIES,     offsetof( mpak_layout_t, entities ),     "entities" },
		{ LUMP_PLANES,       offsetof( mpak_layout_t, planes ),       "planes" },
		{ LUMP_TEXTURES,     offsetof( mpak_layout_t, miptex ),       "textures" },
		{ LUMP_VERTEXES,     offsetof( mpak_layout_t, vertexes ),     "vertexes" },
		{ LUMP_VISIBILITY,   offsetof( mpak_layout_t, visibility ),   "visibility" },
		{ LUMP_NODES,        offsetof( mpak_layout_t, nodes ),        "nodes" },
		{ LUMP_TEXINFO,      offsetof( mpak_layout_t, texinfo ),      "texinfo" },
		{ LUMP_FACES,        offsetof( mpak_layout_t, faces ),        "faces" },
		{ LUMP_LIGHTING,     offsetof( mpak_layout_t, lighting ),     "lighting" },
		{ LUMP_CLIPNODES,    offsetof( mpak_layout_t, clipnodes ),    "clipnodes" },
		{ LUMP_LEAFS,        offsetof( mpak_layout_t, leaves ),       "leafs" },
		{ LUMP_MARKSURFACES, offsetof( mpak_layout_t, marksurfaces ), "marksurfaces" },
		{ LUMP_EDGES,        offsetof( mpak_layout_t, edges ),        "edges" },
		{ LUMP_SURFEDGES,    offsetof( mpak_layout_t, surfedges ),    "surfedges" },
		{ LUMP_MODELS,       offsetof( mpak_layout_t, models ),       "models" },
	};
	const dheader_t *bsp = (const dheader_t *)firstSector;
	uint32_t extraProbe;

	if( bsp->version != HLBSP_VERSION )
	{
		Con_Reportf( S_ERROR "Mpak_ValidateBspHeader: dheader_t version %i, expected %i\n", bsp->version, HLBSP_VERSION );
		return false;
	}

	// Mod_LoadBmodelLumps/Mod_TestBmodelLumps probe a dextrahdr_t right
	// after the dheader_t (mod_bmodel.c:4165, :4336) - its id word must not
	// be IDEXTRAHEADER, and the writer guarantees it is zero.
	memcpy( &extraProbe, (const byte *)firstSector + sizeof( dheader_t ), sizeof( extraProbe ));
	if( extraProbe != 0 )
	{
		Con_Reportf( S_ERROR "Mpak_ValidateBspHeader: bytes after the dheader_t are not zero (0x%08x)\n", extraProbe );
		return false;
	}

	for( size_t i = 0; i < sizeof( map ) / sizeof( map[0] ); i++ )
	{
		const mpak_lump_t *l = (const mpak_lump_t *)((const byte *)layout + map[i].member );
		const dlump_t *d = &bsp->lumps[map[i].lump];
		fs_offset_t expectedOfs = l->length > 0 ? l->offset : 0;

		if( d->fileofs != expectedOfs || d->filelen != l->length )
		{
			Con_Reportf( S_ERROR "Mpak_ValidateBspHeader: %s lump is {%i, %i} in the dheader_t but "
				"{%lld, %lld} by the descriptor - writer and reader have drifted\n",
				map[i].name, d->fileofs, d->filelen, (long long)expectedOfs, (long long)l->length );
			return false;
		}
	}

	return true;
}
