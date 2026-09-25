/*
mod_phs_baked.c - the world's PHS, read from its MPAK v13 archive
Part of Resonance3D (docs/r3d/divergences.md #117, roadmap X9 item 13).

A multiplayer server builds the world's PHS at load with Mod_CalcPHS
(mod_bmodel.c), which holds 2 x rowbytes x (visleafs + 1) of uncompressed
rows while it works - ~10.1 MiB on pool_party, ~7.6 MiB on disposal. The
baker builds the same result offline (tools/asset-studio/src/formats/
PhsBuilder.h) and stores it in the Xbox archive; an Xbox host reads it here
instead. Kept out of mod_bmodel.c so the upstream file carries only the
call site and the test command (upstream-fidelity review, X9-W).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "mod_local.h"

#if XASH_XBOX || XASH_ENGINE_TESTS
#include "mpak_layout.h" // mpak_header_t, MPAK_DESCRIPTOR_OFFSET, MPAK_SECTOR_SIZE

/*
===========
Mod_CheckCompressedRow

True when Mod_DecompressPVS can fill visbytes from `in` without reading
at or past `end`. The baked PHS comes from a file, so every row is walked
once before Mod_FatPVS is allowed to decompress it.
===========
*/
static qboolean Mod_CheckCompressedRow( const byte *in, const byte *end, size_t visbytes )
{
	size_t filled = 0;

	while( filled < visbytes )
	{
		if( in >= end )
			return false;
		if( *in )
		{
			in++;
			filled++;
		}
		else
		{
			if( in + 1 >= end )
				return false;
			filled += in[1];
			in += 2;
		}
	}
	return true;
}

/*
===========
Mod_ReadBakedPHS

The world's PHS as the baker built it, read from "maps/<name>.mpak"
instead of computed by Mod_CalcPHS. The result has Mod_CalcPHS's exact
shape and lifetime - a size_t offset per row (visleafs + 1) and the
compressed rows, both from mod->mempool - and tools/test-phs.sh proves
the bytes equal over every retail map. Nothing is trusted: the
descriptor, the row count, every offset and every row are checked, and
any failure returns false, so the caller falls back to Mod_CalcPHS.

`visdatasize` is the loaded world's LUMP_VISIBILITY length, checked
against the archive's, so a loose .bsp loaded in place of the archive's
own prefix cannot be paired with another map's PHS; 0 skips the check
(test_phs, where the world model no longer carries it).
===========
*/
qboolean Mod_ReadBakedPHS( const model_t *mod, size_t visdatasize, byte **out_phs, size_t **out_ofs, size_t *out_len )
{
	const size_t count = mod->numleafs + 1; // as Mod_CalcPHS
	char archive[MAX_QPATH];
	mpak_header_t header;
	uint32_t *ofs32 = NULL;
	size_t *ofs = NULL;
	byte *rows = NULL;
	fs_offset_t filesize, rowsAt;
	file_t *f;

	*out_phs = NULL;
	*out_ofs = NULL;
	*out_len = 0;

	if( !mod->visdata )
		return false; // Mod_CalcPHS would build nothing either

	Q_strncpy( archive, mod->name, sizeof( archive ));
	COM_ReplaceExtension( archive, ".mpak", sizeof( archive ));
	f = FS_Open( archive, "rb", false );
	if( !f )
		return false; // a loose map, not an archive: nothing baked

	filesize = FS_FileLength( f );
	if( FS_Seek( f, MPAK_DESCRIPTOR_OFFSET, SEEK_SET ) != 0 || FS_Read( f, &header, sizeof( header )) != sizeof( header ))
		goto fail;
	if( memcmp( header.magic, MPAK_LAYOUT_MAGIC, sizeof( header.magic )) || header.version != MPAK_LAYOUT_VERSION
		|| header.headerSize != sizeof( header ))
		goto fail;
	if( !header.lenPhsOffsets )
		goto fail; // baked without one (PhsBuilder.h names why at bake time)
	if( header.lenPhsOffsets != count * sizeof( uint32_t ) || !header.lenPhsData || ( header.phsOffset % MPAK_SECTOR_SIZE ))
		goto fail;
	if( visdatasize && header.lenVisibility != visdatasize )
		goto fail;

	rowsAt = (fs_offset_t)header.phsOffset + ALIGN( header.lenPhsOffsets, MPAK_SECTOR_SIZE );
	if( rowsAt + (fs_offset_t)header.lenPhsData > filesize )
		goto fail;

	// Where size_t is 32 bits (the Xbox) the file's offsets are the
	// engine's, read in place; elsewhere (64-bit test builds) they are
	// widened through a temporary (memory-budget review, X9-W).
	ofs = Mem_Malloc( mod->mempool, sizeof( *ofs ) * count );
	ofs32 = sizeof( *ofs ) == sizeof( *ofs32 ) ? (uint32_t *)ofs : Mem_Malloc( mod->mempool, header.lenPhsOffsets );
	rows = Mem_Malloc( mod->mempool, header.lenPhsData );
	if( FS_Seek( f, header.phsOffset, SEEK_SET ) != 0 || FS_Read( f, ofs32, header.lenPhsOffsets ) != header.lenPhsOffsets
		|| FS_Seek( f, rowsAt, SEEK_SET ) != 0 || FS_Read( f, rows, header.lenPhsData ) != header.lenPhsData )
		goto fail;

	for( size_t i = 0; i < count; i++ )
	{
		const uint32_t o = ofs32[i];
		if( o >= header.lenPhsData || !Mod_CheckCompressedRow( rows + o, rows + header.lenPhsData, world.visbytes ))
			goto fail;
		ofs[i] = o;
	}

	if( (void *)ofs32 != (void *)ofs )
		Mem_Free( ofs32 );
	FS_Close( f );
	*out_phs = rows;
	*out_ofs = ofs;
	*out_len = header.lenPhsData;
	return true;

fail:
	Con_Reportf( S_WARN "%s: %s has no usable baked PHS\n", __func__, archive );
	if( ofs32 && (void *)ofs32 != (void *)ofs ) Mem_Free( ofs32 );
	if( rows ) Mem_Free( rows );
	if( ofs ) Mem_Free( ofs );
	FS_Close( f );
	return false;
}
#endif // XASH_XBOX || XASH_ENGINE_TESTS
