/*
mpak_texture_dir.h - the v10 per-texture DXT1 directory rule: chain length,
128-byte padding, and the per-entry/whole-lump checks that accept or reject
one - shared, byte-identical, by every consumer that has to apply it:
ref/nv2a/r_context.c (Xbox map load), filesystem/tests/mpak_layout.c (PC
tests) and tools/asset-studio's PakManager::verifyFile (the baker's own
verify pass).

Why header-only, not a function inside mpak_layout.c: ref/ and filesystem/
are separate, isolated static-lib units - ref/<module> is renamed and
linked next to the engine by tools/xbox-ref-isolate.sh, and per
ref/nv2a/r_context.c's own X9-B comment (r_context.c:948-981), a ref module
may only #include a filesystem/ header for its type/constant definitions
(they cost nothing, they carry no linkage) - it can never link against
filesystem/mpak_layout.c's real function bodies. A real function in
mpak_layout.c would therefore still leave ref/nv2a with its own untested
hand mirror, exactly the drift this file exists to close (see
docs/r3d/divergences.md #105).
Putting the rule here instead - included, not linked, by all three
consumers - means every one of them runs the literal same comparisons.

Every function below is `static inline` and this file includes nothing but
<stdint.h>/<stddef.h>/<string.h>, so it compiles unchanged as C (ref/nv2a,
filesystem/tests) and as C++ (tools/asset-studio) - no xash3d_types.h,
build.h or port.h dependency, and no implicit void* conversion or
designated initialiser that C++ would reject.

Editing this header alone does not make tools/xbox-link-check.sh
recompile anything: its stale-object check compares each .obj only with
its own .c (and tools/xbox-cc.sh). After changing this file, re-run
tools/xbox-sweep.sh (a full recompile) or recompile ref/nv2a/r_context.c
and filesystem/mpak_layout.c with tools/xbox-cc.sh by hand.

MUST change in lockstep with:
  - tools/asset-studio/src/formats/MpakHeader.h's MpakTextureInfo (the
    on-disk entry shape MPAK_TEXTURE_INFO_SIZE and the field offsets below
    describe);
  - tools/asset-studio/src/formats/PakManager.cpp's buildDxt1MipChain (the
    writer this file's Mpak_Dxt1ChainLength predicts the output size of).

Part of Resonance3D; see docs/r3d/fork-plan.md's X9 section.

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

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// mpak_texture_info_t / MpakTextureInfo on-disk shape (filesystem/
// mpak_layout.h's mpak_texture_info_t, MpakHeader.h's MpakTextureInfo): four uint32_t dimension/
// log2 fields, then dataLength, all at fixed 4-byte-aligned offsets.
#define MPAK_TEXTURE_INFO_SIZE   20u
#define MPAK_TEXTURE_INFO_OFS_WIDTH       0u
#define MPAK_TEXTURE_INFO_OFS_HEIGHT      4u
#define MPAK_TEXTURE_INFO_OFS_LOG2WIDTH   8u
#define MPAK_TEXTURE_INFO_OFS_LOG2HEIGHT  12u
#define MPAK_TEXTURE_INFO_OFS_DATALENGTH  16u

// A real 4x4 DXT1 block is the smallest representable level - one block,
// 8 bytes (nv2a_dxt1_chain_length's own base case; TextureOpt::compressToDXT
// walks the image in whole 4x4 blocks - TextureOpt.cpp's compressToDXT).
#define MPAK_TEXTURE_LOG2_MIN  2u
// NV2A's real maximum texture edge - BASE_SIZE_U/BASE_SIZE_V
// (deps/nxdk/lib/pbkit/nv_regs.h:581-582) are 4-bit log2 fields, and this
// format's own writer/reader already share this same 4096 ceiling
// (PakManager.cpp's nearestPowerOfTwo, r_context.c's per-entry validation).
#define MPAK_TEXTURE_LOG2_MAX  12u
// Xbox D3D's own texture base alignment - every texture's data is
// zero-padded to this boundary inside the textureData lump (MpakHeader.h's
// v9 -> v10 note).
#define MPAK_TEXTURE_DATA_ALIGN 128u

// Byte length of one POT texture's full DXT1 mip chain, base level down to
// 1x1 (max(log2w, log2h) + 1 levels - NV2A's MIPMAP_LEVELS counts every
// level, and a chain ending early leaves the smallest LODs undefined). A
// level narrower than 4 texels on a side still occupies one whole 4x4
// block per axis (ceil(w/4) * ceil(h/4) * 8 bytes) - the same size NV2A
// addresses for it. Mirrors the baker's buildDxt1MipChain
// (tools/asset-studio/src/formats/PakManager.cpp) byte for byte;
// if that function's level count or per-level byte size ever changes, this
// must change with it. *outLevels (if non-NULL) receives the level count.
//
// log2w/log2h > 31 would make `1u << (log2w - l)` undefined behaviour for
// some level - guarded here (returns 0/0) even though every real caller
// already range-checks against MPAK_TEXTURE_LOG2_MAX first, so this
// function is never itself the thing relying on that upstream check.
static inline uint64_t Mpak_Dxt1ChainLength( uint32_t log2w, uint32_t log2h, uint32_t *outLevels )
{
	uint32_t levels, l;
	uint64_t total;

	if( log2w > 31u || log2h > 31u )
	{
		if( outLevels )
			*outLevels = 0;
		return 0;
	}

	levels = ( log2w > log2h ? log2w : log2h ) + 1;
	total = 0;

	for( l = 0; l < levels; l++ )
	{
		uint32_t w = ( l < log2w ) ? ( 1u << ( log2w - l )) : 1u;
		uint32_t h = ( l < log2h ) ? ( 1u << ( log2h - l )) : 1u;
		total += (uint64_t)(( w + 3 ) / 4 ) * (( h + 3 ) / 4 ) * 8;
	}
	if( outLevels )
		*outLevels = levels;
	return total;
}

// Rounds a texture's real DXT1 chain length up to the next
// MPAK_TEXTURE_DATA_ALIGN (128) boundary - the padded extent that texture
// actually occupies inside the textureData lump, and the amount the
// running directory-entry cursor advances by.
static inline uint64_t Mpak_TextureAlignedLength( uint64_t dataLength )
{
	return ( dataLength + ( MPAK_TEXTURE_DATA_ALIGN - 1u )) & ~(uint64_t)( MPAK_TEXTURE_DATA_ALIGN - 1u );
}

typedef enum
{
	MPAK_TEXDIR_OK = 0,
	MPAK_TEXDIR_BAD_DIRECTORY_LENGTH,   // dirLength != textureCount * MPAK_TEXTURE_INFO_SIZE
	MPAK_TEXDIR_BAD_DIMENSIONS,         // log2 out of [2,12], or width/height != 1 << log2
	MPAK_TEXDIR_BAD_CHAIN_LENGTH,       // dataLength != Mpak_Dxt1ChainLength(log2w, log2h)
	MPAK_TEXDIR_DATA_LENGTH_MISMATCH,   // running, 128-aligned sum != the textureData lump's real length
	MPAK_TEXDIR_NONZERO_PADDING         // a padding byte (past dataLength, before the next 128 boundary) is not 0
} mpak_texdir_status_t;

static inline const char *Mpak_TextureDirStatusString( mpak_texdir_status_t status )
{
	switch( status )
	{
	case MPAK_TEXDIR_OK: return "ok";
	case MPAK_TEXDIR_BAD_DIRECTORY_LENGTH: return "directory length is not textureCount * sizeof(mpak_texture_info_t)";
	case MPAK_TEXDIR_BAD_DIMENSIONS: return "log2 out of [2,12] or width/height mismatched its own log2 field";
	case MPAK_TEXDIR_BAD_CHAIN_LENGTH: return "dataLength is not this size's full DXT1 mip chain length";
	case MPAK_TEXDIR_DATA_LENGTH_MISMATCH: return "summed, 128-aligned dataLength does not match the texture-data lump's real length";
	case MPAK_TEXDIR_NONZERO_PADDING: return "a padding byte past a texture's real dataLength is not zero";
	default: return "unknown mpak_texdir_status_t";
	}
}

// Validates one v10 texture directory against its matching texture-data
// lump, entry by entry, in directory order - the single rule every
// consumer (ref/nv2a load, the PC tests, the baker's verify pass) applies.
//
//   dir         - directory bytes, dirLength long (textureCount *
//                 MPAK_TEXTURE_INFO_SIZE entries, mpak_texture_info_t/
//                 MpakTextureInfo layout)
//   dirLength   - the textureDir lump's own real, bounds-checked length
//   textureCount- header->textureCount / desc.textureCount
//   data        - texture-data lump bytes, dataLength long; pass NULL to
//                 skip the padding-CONTENT check (step 6) - ref/nv2a does
//                 this on purpose (see this header's own file comment: NV2A
//                 never samples padding bytes, so checking their content at
//                 Xbox load time would change the console's accept/reject
//                 set for no behavioural reason). The PC tests and the
//                 baker's verify pass both pass a real buffer so padding
//                 corruption is still caught somewhere.
//   dataLength  - the textureData lump's own real, bounds-checked length
//   outBadIndex - if non-NULL, receives the offending entry index on
//                 failure: UINT32_MAX for a directory-length failure (step
//                 1, no entry could even be addressed) or textureCount for
//                 the final running-sum failure (step 8, every entry
//                 individually validated but the lump as a whole didn't
//                 balance).
//
// textureCount == 0 with dirLength == 0 and dataLength == 0 returns OK -
// rejecting a map with zero textures is the caller's own policy, not this
// function's (ref/nv2a already fails closed on textureCount == 0 before
// ever calling this, via NV2A_MAP_MAX_TEXTURES's own == 0 check).
static inline mpak_texdir_status_t Mpak_ValidateTextureDirectory(
	const uint8_t *dir, uint64_t dirLength, uint32_t textureCount,
	const uint8_t *data, uint64_t dataLength, uint32_t *outBadIndex )
{
	uint64_t running = 0;
	uint32_t i;

	if( dirLength != (uint64_t)textureCount * MPAK_TEXTURE_INFO_SIZE )
	{
		if( outBadIndex )
			*outBadIndex = 0xFFFFFFFFu;
		return MPAK_TEXDIR_BAD_DIRECTORY_LENGTH;
	}

	for( i = 0; i < textureCount; i++ )
	{
		uint64_t base = (uint64_t)i * MPAK_TEXTURE_INFO_SIZE;
		uint32_t width, height, log2w, log2h, entryLength;
		uint64_t aligned;

		memcpy( &width,       dir + base + MPAK_TEXTURE_INFO_OFS_WIDTH,      sizeof( width ));
		memcpy( &height,      dir + base + MPAK_TEXTURE_INFO_OFS_HEIGHT,     sizeof( height ));
		memcpy( &log2w,       dir + base + MPAK_TEXTURE_INFO_OFS_LOG2WIDTH,  sizeof( log2w ));
		memcpy( &log2h,       dir + base + MPAK_TEXTURE_INFO_OFS_LOG2HEIGHT, sizeof( log2h ));
		memcpy( &entryLength, dir + base + MPAK_TEXTURE_INFO_OFS_DATALENGTH, sizeof( entryLength ));

		if( log2w < MPAK_TEXTURE_LOG2_MIN || log2w > MPAK_TEXTURE_LOG2_MAX ||
			log2h < MPAK_TEXTURE_LOG2_MIN || log2h > MPAK_TEXTURE_LOG2_MAX ||
			width != ( 1u << log2w ) || height != ( 1u << log2h ))
		{
			if( outBadIndex )
				*outBadIndex = i;
			return MPAK_TEXDIR_BAD_DIMENSIONS;
		}

		if( (uint64_t)entryLength != Mpak_Dxt1ChainLength( log2w, log2h, NULL ))
		{
			if( outBadIndex )
				*outBadIndex = i;
			return MPAK_TEXDIR_BAD_CHAIN_LENGTH;
		}

		aligned = Mpak_TextureAlignedLength( entryLength );

		// Early-out BEFORE reading any padding bytes: the accept/reject set
		// is identical to the end-of-loop running != dataLength check
		// (step 8) would give, and this makes the padding read below
		// bounds-safe (running + aligned <= dataLength is now guaranteed
		// for the rest of this entry).
		if( running + aligned > dataLength )
		{
			if( outBadIndex )
				*outBadIndex = i;
			return MPAK_TEXDIR_DATA_LENGTH_MISMATCH;
		}

		if( data != NULL )
		{
			uint64_t p;
			for( p = running + entryLength; p < running + aligned; p++ )
			{
				if( data[p] != 0 )
				{
					if( outBadIndex )
						*outBadIndex = i;
					return MPAK_TEXDIR_NONZERO_PADDING;
				}
			}
		}

		running += aligned;
	}

	if( running != dataLength )
	{
		if( outBadIndex )
			*outBadIndex = textureCount;
		return MPAK_TEXDIR_DATA_LENGTH_MISMATCH;
	}

	return MPAK_TEXDIR_OK;
}

#ifdef __cplusplus
}
#endif
