/*
mpak_layout.h - computes the real on-disk byte layout of an MPAK archive

Part of Resonance3D; see docs/r3d/fork-plan.md's X2 section.

MpakHeader6 (tools/asset-studio/src/formats/MpakHeader.h) has no offset
table - "byte layout order IS the format" - and, verified directly
against the real writer (tools/asset-studio/src/formats/PakManager.cpp's
save()), that layout is NOT simply the header's own field declaration
order: every section is independently padded up to the next 2048-byte
boundary (PakManager.cpp's writePadded lambda), the vertex/index geometry
block sits between the header and the first BSP lump with no length field
of its own (its size is vCount/iCount times a fixed per-record size the
header never states), and the texture block is target-dependent (Xbox
writes one DXT atlas; PC writes three separate DXT arrays) - none of
which "walk the header's fields in order" would get right.

This file is the single place that layout math lives, verified against a
real baked archive (filesystem/tests/mpak_layout.c) rather than derived
from the header comment alone. It does NOT parse any lump's CONTENTS -
teaching the engine to build runtime mnode_t/mleaf_t/etc. structures from
these offsets is real, separate, larger work (a rewrite inside
mod_bmodel.c, whose dbspmodel_t/Mod_LoadPlanes-and-friends are file-local
static functions this file cannot call into) - not started yet.

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
#pragma once

#include "build.h"
#include <stdint.h>
#include "port.h"
#include "xash3d_types.h" // fs_offset_t, qboolean - common/xash3d_types.h, on the include
                          // path as "xash3d_types.h" (sdk_includes exports "../common" as
                          // a root, not a "common/" prefix - found while wiring this up)

#ifdef __cplusplus
extern "C" {
#endif

#define MPAK_LAYOUT_MAGIC   "MPAK"
#define MPAK_LAYOUT_VERSION 8u
#define MPAK_SECTOR_SIZE    2048u
#define MPAK_TARGET_XBOX_FLAG 1u  // MpakHeader6::flags bit 0 - 1 = Xbox, 0 = PC
#define MPAK_FLAG_NV2A_BLOCK  2u  // flags bit 1 - the baked v6 block is populated (X9 profile)

// v7: a map archive IS a BSP30 file. Offset 0 holds a real dheader_t
// (common/bspfile.h) whose lump table points at the sector-aligned raw
// lumps, because the engine validates a map FROM DISK before any loader
// runs (SV_ValidateMap -> Mod_TestBmodelLumps, sv_game.c:823-853,
// mod_bmodel.c:4323-4398; docs/r3d/mpak-loading-design.md, F7) and reads
// exactly that struct. The mpak_header_t descriptor below lives at this
// fixed offset inside the same 2048-byte sector; bytes 124..127 are zero
// so the engine's dextrahdr_t probe at sizeof(dheader_t) never sees
// IDEXTRAHEADER. Mirrors MPAK_DESCRIPTOR_OFFSET in
// tools/asset-studio/src/formats/MpakHeader.h.
#define MPAK_DESCRIPTOR_OFFSET 128u

// Mirrors formats::MpakHeader6 (tools/asset-studio/src/formats/MpakHeader.h)
// byte-for-byte, field for field, in the SAME order - must be updated in
// lockstep with any change there. All-uint32_t (plus the 4-byte magic),
// so this needs no packing pragma to match the C++ struct's own natural
// (unpacked) layout.
typedef struct
{
	char     magic[4];
	uint32_t version;
	uint32_t headerSize;
	uint32_t bakerBuildId;
	uint32_t flags;

	uint32_t vCount;
	uint32_t iCount;

	uint32_t lenVisibility;
	uint32_t lenNodes;
	uint32_t lenLeaves;
	uint32_t lenMarksurfaces;
	uint32_t lenFaceDrawCmds;
	uint32_t lenPlanes;
	uint32_t lenClipnodes;
	uint32_t lenModels;

	uint32_t atlasWidth;
	uint32_t atlasHeight;
	uint32_t lenAtlas;

	uint32_t count64;
	uint32_t lenArray64;
	uint32_t count256;
	uint32_t lenArray256;
	uint32_t count512;
	uint32_t lenArray512;

	uint32_t lenAssetDict;  // always 0 - X2 scope narrowing, MpakHeader.h's own note
	uint32_t lenAssetData;  // always 0 - X2 scope narrowing, MpakHeader.h's own note

	uint32_t lenEntities;

	uint32_t lightmapWidth;
	uint32_t lightmapHeight;
	uint32_t lightmapLayers;
	uint32_t lightmapFormat; // 0 = RGBA8
	uint32_t lenLightmapAtlas;

	uint32_t lenFaceInfo;
	uint32_t lenTexinfo;

	// v7 classic core - see MpakHeader.h's v6 -> v7 note. Raw source lumps
	// the engine's own loaders consume unchanged, plus TEXTURES rebuilt as
	// embedded miptex (names, dimensions, 8-bit mips, palette).
	uint32_t lenVertexes;
	uint32_t lenEdges;
	uint32_t lenSurfedges;
	uint32_t lenFaces;
	uint32_t lenLighting;
	uint32_t lenMiptex;

	// v8 - see MpakHeader.h's v7 -> v8 note (docs/r3d/fork-plan.md's XB
	// "Lightstyles" census). nv2a profile only; 0 in the core profile and
	// whenever a real bake finds no face using a second lightstyle.
	uint32_t lenExtraLightBlocks;
} mpak_header_t;

// Mirrors formats::Vertex (tools/asset-studio/src/formats/PakManager.h)
// byte-for-byte - only its SIZE matters to this file (skipping the
// geometry block correctly), not its fields; mirrored in full anyway,
// matching MpakHeader.h's own MpakFaceInfo precedent, so whatever reads
// vertex data later doesn't have to duplicate this struct a second time.
typedef struct
{
	float    x, y, z;
	float    u, v;
	float    lu, lv;
	uint32_t lightmap_layer;
	float    rect[4];
	uint32_t texture_layer;
	uint32_t array_index;
	uint32_t _pad;
	uint32_t _pad2;
} mpak_vertex_t;

// One lump's real (unpadded) extent within the archive. offset is
// meaningless when length is 0 (an empty lump contributes no bytes at
// all, not even a placeholder sector - verified against PakManager.cpp's
// per-section `if (lenX > 0) writePadded(...)` guards).
typedef struct
{
	fs_offset_t offset;
	fs_offset_t length;
} mpak_lump_t;

typedef struct
{
	mpak_lump_t vertices;     // mpak_vertex_t[header.vCount]
	mpak_lump_t indices;      // uint16_t[header.iCount]
	mpak_lump_t visibility;
	mpak_lump_t nodes;
	mpak_lump_t leaves;
	mpak_lump_t marksurfaces;
	mpak_lump_t faceDrawCmds; // FaceDrawCmd[2 x uint32_t] each, PakManager.h
	mpak_lump_t planes;
	mpak_lump_t clipnodes;
	mpak_lump_t models;
	mpak_lump_t atlas;        // Xbox target only
	mpak_lump_t array64;      // PC target only
	mpak_lump_t array256;     // PC target only
	mpak_lump_t array512;     // PC target only
	mpak_lump_t assetDict;    // always empty - X2 scope narrowing
	mpak_lump_t assetData;    // always empty - X2 scope narrowing
	mpak_lump_t entities;
	mpak_lump_t lightmapAtlas;
	mpak_lump_t faceInfo;     // MpakFaceInfo[], 44 bytes each (MpakHeader.h's own static_assert)
	mpak_lump_t texinfo;      // dtexinfo_t[], common/bspfile.h
	// v7 classic core, in write order
	mpak_lump_t vertexes;     // dvertex_t[]
	mpak_lump_t edges;        // dedge_t[]
	mpak_lump_t surfedges;    // dsurfedge_t[]
	mpak_lump_t faces;        // dface_t[]
	mpak_lump_t lighting;     // color24 samples, every lightstyle
	mpak_lump_t miptex;       // dmiptexlump_t + miptex entries
	// v8 - one entry per face per extra (non-style-0) lightstyle it uses,
	// nv2a profile only. 20 bytes each (MpakHeader.h's MpakExtraLightBlock,
	// own static_assert); see its doc comment there for the field shapes -
	// mirrored to that struct only where SIZE matters (mpak_vertex_t's own
	// precedent above), not here, since nothing yet walks this lump's
	// records (this file's own header comment: content parsing is
	// separate, later work).
	mpak_lump_t extraLightBlocks;
} mpak_layout_t;

// Validates magic/version and returns true if header looks like a real,
// readable MPAK archive. Does not touch anything past sizeof(mpak_header_t).
qboolean Mpak_ValidateHeader( const mpak_header_t *header );

// Computes every lump's real offset/length from a validated header and the
// archive's total file size, bounds-checking every computed lump against
// fileSize and, for fixed-record lumps, that its length is an exact
// multiple of that record's real C struct size (a truncated/corrupted
// archive produces a fractional count here, not a silent off-by-some-bytes
// read later). Returns false (logging why, via Con_Reportf) on the first
// problem found, leaving *out partially written - callers must not use a
// failed result.
qboolean Mpak_ComputeLayout( const mpak_header_t *header, fs_offset_t fileSize, mpak_layout_t *out );

// Checks that the real dheader_t at the start of the archive's first
// sector describes exactly the bytes a successful Mpak_ComputeLayout
// derived from the descriptor: version HLBSP_VERSION, every one of the
// fifteen classic lumps at the computed {offset, length} (a lump with no
// bytes must be {0, 0} - Mod_LoadLump treats fileofs 0 as "absent"), and
// the four bytes after the dheader_t zero. The writer fills both tables
// from one cursor, so a mismatch is writer/reader drift and is rejected
// the way a header-size mismatch is. firstSector must hold at least
// MPAK_DESCRIPTOR_OFFSET bytes.
qboolean Mpak_ValidateBspHeader( const void *firstSector, const mpak_layout_t *layout );

#ifdef __cplusplus
}
#endif
