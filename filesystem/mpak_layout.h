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
writes a per-texture DXT1 directory + data blob + draw-batch table, one
entry per unique texture the map actually uses; PC writes three separate
DXT texture arrays) - none of which "walk the header's fields in order"
would get right.

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
#include "mpak_texture_dir.h" // MPAK_TEXTURE_INFO_SIZE and the v10 texture-directory
                              // validation rule itself - see that file's own header
                              // comment for why this lives there, header-only, rather
                              // than as a function in this file's own .c

#ifdef __cplusplus
extern "C" {
#endif

#define MPAK_LAYOUT_MAGIC   "MPAK"
#define MPAK_LAYOUT_VERSION 10u
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

	// --- v9: per-texture DXT1 directory + texture-draw batches, replacing
	// the single shared atlas (atlasWidth/atlasHeight/lenAtlas, v6-v8) -
	// see filesystem/mpak_layout.h's own file header / ref/nv2a/r_context.c's
	// nv2a_draw_map_geometry() for why: a shared atlas needs a per-vertex
	// atlas-rect remap that breaks on any face tiling more than one texture
	// repeat, and subdividing tiled faces to fix it overflows the u16 vertex
	// index on large outdoor maps. Replaced by independent, natively
	// hardware-WRAP-addressed per-texture DXT1 images. Xbox target only -
	// PC keeps array64/lenArray64/etc. below, completely unaffected (its
	// own Texture2DArray path never used the atlas or its rect[] remap to
	// begin with).
	uint32_t textureCount;   // unique per-texture DXT1 images actually used by this map
	uint32_t lenTextureDir;  // textureCount * sizeof(mpak_texture_info_t)
	uint32_t lenTextureData; // total bytes of all per-texture DXT1 mip chains in directory order, each zero-padded to 128 bytes (v10)
	uint32_t batchCount;     // texture-draw batches. Architecturally == textureCount (texture ID is the
	                         // PRIMARY sort key, so every used texture's faces land in exactly one
	                         // contiguous run) - kept as an explicit field for robustness/clarity rather
	                         // than inferred, matching this format's general preference for explicit lengths.
	uint32_t lenBatches;     // batchCount * sizeof(mpak_texture_batch_t)
	uint32_t lenFaceRemap;   // (original BSP face count) * sizeof(uint32_t) - the
	                         // originalToSortedFaceIndex[] table (baker sorts faces by texture ID for
	                         // batching; this is its inverse permutation, indexed by the ORIGINAL
	                         // LUMP_FACES position). The original face count is self-describing from
	                         // this length (lenFaceRemap / sizeof(uint32_t)) - no separate header field.
	                         // Mandatory: ref/nv2a's own PVS marking (nv2a_mark_leaves) computes a face
	                         // index from the engine's OWN classic-lump-derived msurface_t array (ORIGINAL
	                         // order, independent of this sort) and must translate it through this table
	                         // before indexing anything built from the now-SORTED mpak face order.

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
	uint32_t lightmapHeight;  // one page: trimmed to the rows used (4..1024, multiple of 4, X9-R); several pages: 1024. lv is normalized by it
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
//
// v9: `rect[4]` (the atlas-placement remap) is REMOVED - dead even on the
// PC path, which always wrote it as the constant identity {0,0,1,1}, and
// meaningless now that Xbox no longer shares one atlas either (see
// mpak_header_t's own v9 note above). 48 bytes, was 64.
typedef struct
{
	float    x, y, z;
	float    u, v;
	float    lu, lv;
	uint32_t lightmap_layer;
	uint32_t texture_layer;
	uint32_t array_index;
	uint32_t _pad;
	uint32_t _pad2;
} mpak_vertex_t;

// Mirrors formats::MpakFaceInfo (tools/asset-studio/src/formats/MpakHeader.h)
// byte-for-byte, 44 bytes - all members already fall on a 4-byte boundary
// (styles[4] is exactly 4 bytes), so this needs no packing pragma either.
// One entry per baked face, in the archive's own (X9-K: SORTED, not
// original BSP) face order - the same order FaceDrawCmds/nv2a_map_faces
// use. X9-K: ref/nv2a's own nv2a_load_map_geometry() reads only `flags`
// (specifically MPAK_SURF_DRAWSKY) from this lump - the rest of this
// mirror exists so a later reader doesn't have to re-derive the layout.
typedef struct
{
	uint32_t flags;
	int32_t  planenum;
	int32_t  texinfo;
	uint8_t  styles[4];
	int32_t  lightmapmins[2];
	int32_t  extents[2];
	uint32_t lightmapPage;
	uint32_t lightmapBlockX;
	uint32_t lightmapBlockY;
} mpak_face_info_t;

// GoldSrc BSP30 SURF_* flags this format bakes into MpakFaceInfo::flags -
// mirrors formats::SURF_DRAWSKY (MpakHeader.h). Only the one bit ref/nv2a
// actually consumes is mirrored here; add more as real consumers need them.
#define MPAK_SURF_DRAWSKY 0x0004u // texture name starts with "sky" - never has real geometry to show

// v9: mirrors formats::MpakTextureInfo (tools/asset-studio/src/formats/
// MpakHeader.h) byte-for-byte - one directory entry per unique per-texture
// DXT1 image this map actually uses (Xbox target only). No stored byte
// offset field: this entry's own physical offset into the texture-data
// blob (mpak_layout_t::textureData) is the running sum of dataLength over
// every PRIOR directory entry (v10: of dataLength rounded up to 128
// bytes) - matching this format's own "byte layout order IS the format"
// convention (this file's own header comment) one
// level down, inside a single lump instead of across the whole archive.
// All-uint32_t, so - like mpak_header_t/mpak_vertex_t above - this needs
// no packing pragma to match the C++ struct's own natural layout.
typedef struct
{
	uint32_t width;      // POT, rounded UP (never down) independently per axis from the source's native size
	uint32_t height;     // POT, rounded up (never down) independently per axis
	uint32_t log2Width;  // log2(width) - NV097_SET_TEXTURE_FORMAT's BASE_SIZE_U field
	uint32_t log2Height; // log2(height) - BASE_SIZE_V field
	uint32_t dataLength; // v10: this texture's whole DXT1 mip chain in bytes, base level down to 1x1
	                     // (levels under 4 texels still take whole 4x4 blocks); its data is then
	                     // zero-padded to 128 bytes, so offsets sum align128(dataLength) - the whole
	                     // rule (chain-length math, padding, per-entry/whole-lump acceptance) lives
	                     // in mpak_texture_dir.h's Mpak_ValidateTextureDirectory(), not here
} mpak_texture_info_t;

// Ties this struct's byte layout to MPAK_TEXTURE_INFO_SIZE, the offset
// table mpak_texture_dir.h's Mpak_ValidateTextureDirectory() reads by hand
// (it cannot just take sizeof(mpak_texture_info_t) - it also has to agree
// with formats::MpakTextureInfo, a separately-compiled C++ struct with no
// shared header of its own) - a drift here would otherwise silently
// misalign every field read past width/height.
STATIC_ASSERT( sizeof( mpak_texture_info_t ) == MPAK_TEXTURE_INFO_SIZE, "mpak_texture_info_t must match MPAK_TEXTURE_INFO_SIZE (mpak_texture_dir.h)" );

// v9: mirrors formats::MpakTextureBatch byte-for-byte - one texture-draw
// batch per texture that at least one face actually uses, naming the
// CONTIGUOUS run of SORTED-order faces (and the matching contiguous
// shared-index-buffer range) that use it. Texture ID is the archive's own
// primary sort key for faces, so every batch's own face/index range is
// contiguous by construction - never interleaved with another batch's -
// but batchCount is NOT guaranteed == textureCount: the texture directory
// can contain entries (unreferenced miptexs) no face ever uses, which get
// no batch at all.
typedef struct
{
	uint32_t textureIndex; // index into the texture directory (mpak_layout_t::textureDir)
	uint32_t startFace;    // first face, in SORTED/mpak order, belonging to this batch
	uint32_t faceCount;    // number of faces (sorted order) in this batch
	uint32_t startIndex;   // first index in the shared index buffer for this batch
	uint32_t indexCount;   // total indices for this batch
} mpak_texture_batch_t;

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
	mpak_lump_t textureDir;   // v9, Xbox target only - mpak_texture_info_t[header.textureCount]
	mpak_lump_t textureData;  // v9, Xbox target only - per-texture DXT1 data in directory order (v10: mip chains, each padded to 128)
	mpak_lump_t batches;      // v9, Xbox target only - mpak_texture_batch_t[header.batchCount]
	mpak_lump_t faceRemap;    // v9, Xbox target only - uint32_t[original BSP face count], originalToSortedFaceIndex[]
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
