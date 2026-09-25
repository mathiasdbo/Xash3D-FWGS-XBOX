// Proves the filesystem plumbing added for X2 (docs/r3d/fork-plan.md,
// filesystem/mpak.c): an .mpak archive dropped into a scanned game
// directory is discovered via g_archives[] (searchpath.c), and asking the
// VFS for the map name it represents ("maps/<basename>.bsp") returns the
// archive's classic-lump prefix, and "maps/<basename>.mpak" the whole
// archive, byte for byte (MPAK v12 - filesystem/mpak.c's header comment).
// Also proves an archive whose dheader_t disagrees with its descriptor is
// not mounted at all. Also
// proves a loose "maps/<name>.bsp" still wins over a same-named .mpak
// when both are present - the ONLY currently-safe ordering, since
// nothing can parse an .mpak's bytes yet (see
// TestLooseMapStillWinsOverMpak's own comment for the regression this
// guards against).
#include "filesystem_test_common.h"
#include "mpak_layout.h" // MPAK_DESCRIPTOR_OFFSET, MPAK_LAYOUT_VERSION
#include "bspfile.h"     // HLBSP_VERSION

// A v7 map archive's first sector holds a real BSP30 dheader_t at 0 and
// the MpakHeader6 descriptor at MPAK_DESCRIPTOR_OFFSET
// (tools/asset-studio/src/formats/MpakHeader.h's first-sector note). mpak.c
// probes both, and since v12 walks the whole descriptor's layout, so a
// mountable test archive has to be a real (tiny) one: its payload is the
// entities lump - the only classic lump - followed by a renderer-only
// faceRemap section. WriteFirstSector alone still writes the broken
// sectors TestRejectsBadArchive needs.
#define TEST_FIRST_SECTOR 2048u
#define TEST_REMAP_BYTES  512u  // the renderer-only section: 128 uint32_t face-remap entries

static qboolean WriteFirstSector( file_t *f, uint32_t bspVersion, const char magic[4], uint32_t version )
{
	byte sector[TEST_FIRST_SECTOR];

	memset( sector, 0, sizeof( sector ));
	memcpy( sector, &bspVersion, sizeof( bspVersion )); // dheader_t.version; lumps stay {0, 0}
	memcpy( sector + MPAK_DESCRIPTOR_OFFSET, magic, 4 );
	memcpy( sector + MPAK_DESCRIPTOR_OFFSET + 4, &version, sizeof( version )); // this test only runs little-endian hosts

	return g_fs.Write( f, sector, sizeof( sector )) == (fs_offset_t)sizeof( sector );
}

// Writes a valid v12 map archive: payload is the entities lump (at most
// one sector), then TEST_REMAP_BYTES of 0xA5 as the renderer-only
// faceRemap section, every section padded to a whole sector the way
// PakManager.cpp's writePadded does. entitiesOfs overrides the dheader_t's
// entities offset (0 = the real one), to write an archive whose dheader_t
// and descriptor disagree.
static qboolean WriteMpakFile( const char *path, uint32_t version, const byte *payload, size_t payloadSize, int32_t entitiesOfs )
{
	byte sector[TEST_FIRST_SECTOR];
	byte section[TEST_FIRST_SECTOR];
	mpak_header_t desc;
	dheader_t *bsp = (dheader_t *)sector;
	file_t *f;
	qboolean ok;

	if( payloadSize == 0 || payloadSize > sizeof( section ))
		return false;

	memset( sector, 0, sizeof( sector ));
	bsp->version = HLBSP_VERSION;
	bsp->lumps[LUMP_ENTITIES].fileofs = entitiesOfs ? entitiesOfs : (int32_t)TEST_FIRST_SECTOR;
	bsp->lumps[LUMP_ENTITIES].filelen = (int32_t)payloadSize;

	memset( &desc, 0, sizeof( desc ));
	memcpy( desc.magic, "MPAK", 4 );
	desc.version = version; // this test only runs little-endian hosts
	desc.headerSize = sizeof( mpak_header_t );
	desc.lenEntities = (uint32_t)payloadSize;
	desc.lenFaceRemap = TEST_REMAP_BYTES;
	memcpy( sector + MPAK_DESCRIPTOR_OFFSET, &desc, sizeof( desc ));

	f = g_fs.Open( path, "wb", true );
	if( !f )
	{
		printf( "WriteMpakFile: Open(%s) failed\n", path );
		return false;
	}

	ok = g_fs.Write( f, sector, sizeof( sector )) == (fs_offset_t)sizeof( sector );
	memset( section, 0, sizeof( section ));
	memcpy( section, payload, payloadSize );
	ok = ok && g_fs.Write( f, section, sizeof( section )) == (fs_offset_t)sizeof( section );
	memset( section, 0, sizeof( section ));
	memset( section, 0xA5, TEST_REMAP_BYTES );
	ok = ok && g_fs.Write( f, section, sizeof( section )) == (fs_offset_t)sizeof( section );
	g_fs.Close( f );

	if( !ok )
		printf( "WriteMpakFile: short write\n" );
	return ok;
}

static qboolean TestDiscoveryAndWholeFilePassthrough( void )
{
	byte payload[512];
	byte *data, *archive;
	fs_offset_t len, archiveLen;
	qboolean ok;

	for( size_t i = 0; i < sizeof( payload ); i++ )
		payload[i] = (byte)( ( i * 2654435761u ) >> 24 );

	// A writepath has to exist before g_fs.Open("wb", ...) below can
	// create anything (same ordering filecopy.c/caseinsensitive.c use).
	g_fs.AddGameDirectory( "./", FS_GAMEDIR_PATH );

	if( !WriteMpakFile( "c1a3.mpak", MPAK_LAYOUT_VERSION, payload, sizeof( payload ), 0 ))
		return false;

	// Re-scan: this second call is what actually exercises the new "mpak"
	// entry in searchpath.c's g_archives[] table (FS_AddGameDirectory
	// lists the directory and mounts any *.mpak it finds), discovering
	// the file c1a3.mpak just wrote - not a direct FS_AddMpak_Fullpath call.
	g_fs.AddGameDirectory( "./", FS_GAMEDIR_PATH );

	if( !g_fs.FileExists( "maps/c1a3.bsp", true ))
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: maps/c1a3.bsp not found via mpak archive\n" );
		return false;
	}

	data = g_fs.LoadFile( "maps/c1a3.bsp", &len, true );
	if( !data )
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: LoadFile(maps/c1a3.bsp) failed\n" );
		return false;
	}

	// v12: the .bsp name is the classic prefix - the first sector and the
	// entities lump's sector - and stops before the renderer section.
	ok = true;
	if( len != (fs_offset_t)( 2 * TEST_FIRST_SECTOR ))
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: .bsp view is %lld bytes, expected the %u-byte classic prefix\n",
			(long long)len, 2 * TEST_FIRST_SECTOR );
		ok = false;
	}
	else if( memcmp( data + TEST_FIRST_SECTOR, payload, sizeof( payload )) != 0
		|| memcmp( data + MPAK_DESCRIPTOR_OFFSET, "MPAK", 4 ) != 0 )
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: .bsp view content mismatch\n" );
		ok = false;
	}

	// The .mpak name is the whole archive, renderer section included.
	archive = g_fs.LoadFile( "maps/c1a3.mpak", &archiveLen, true );
	if( !archive )
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: LoadFile(maps/c1a3.mpak) failed\n" );
		ok = false;
	}
	else
	{
		if( archiveLen != (fs_offset_t)( 3 * TEST_FIRST_SECTOR )
			|| memcmp( archive, data, (size_t)( 2 * TEST_FIRST_SECTOR )) != 0
			|| archive[2 * TEST_FIRST_SECTOR] != 0xA5 || archive[2 * TEST_FIRST_SECTOR + TEST_REMAP_BYTES - 1] != 0xA5 )
		{
			printf( "TestDiscoveryAndWholeFilePassthrough: .mpak view is not the whole archive (len %lld)\n",
				(long long)archiveLen );
			ok = false;
		}
		free( archive );
	}

	free( data );

	// A name this archive does not answer to must not resolve through it.
	if( ok && g_fs.FileExists( "maps/nonexistent.bsp", true ))
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: unrelated map name incorrectly resolved\n" );
		ok = false;
	}

	// Not deleted: mpak.c's FS_OpenFile_MPAK keeps its own handle to
	// c1a3.mpak open for the archive's lifetime (same as every other
	// real-archive searchpath_t - packs are unmounted at shutdown, not
	// mid-session), and Windows refuses to delete a file that's still
	// open without FILE_SHARE_DELETE. The process exit below reclaims it.
	return ok;
}

// A first sector that is wrong in exactly one way: bad descriptor magic,
// or no BSP30 dheader_t at all (what a UiPacker menu archive - descriptor
// at 0 - or any non-map file renamed .mpak looks like).
static qboolean WriteBrokenSectorFile( const char *path, uint32_t bspVersion, const char magic[4], const byte *payload, size_t payloadSize )
{
	file_t *f = g_fs.Open( path, "wb", true );

	if( !f )
	{
		printf( "WriteBrokenSectorFile: Open(%s) failed\n", path );
		return false;
	}

	if( !WriteFirstSector( f, bspVersion, magic, MPAK_LAYOUT_VERSION ) ||
		g_fs.Write( f, payload, payloadSize ) != (fs_offset_t)payloadSize )
	{
		printf( "WriteBrokenSectorFile: short write\n" );
		g_fs.Close( f );
		return false;
	}

	g_fs.Close( f );
	return true;
}

// A wrong magic or an unsupported version must be rejected at discovery
// time (FS_LoadMpak returning NULL), not silently accepted - proves the
// header validation in filesystem/mpak.c actually runs during a real
// directory scan, not just when called directly.
static qboolean TestRejectsBadArchive( void )
{
	byte payload[16] = { 0 };
	qboolean ok = true;

	// AddGameDirectory establishes the writepath these g_fs.Open("wb", ...)
	// calls need - already done once in TestDiscoveryAndWholeFilePassthrough,
	// but repeated here so this test stays correct if ever run standalone.
	g_fs.AddGameDirectory( "./", FS_GAMEDIR_PATH );

	if( !WriteBrokenSectorFile( "badmagic.mpak", HLBSP_VERSION, "XXXX", payload, sizeof( payload )))
		return false;

	if( !WriteMpakFile( "badversion.mpak", 99, payload, sizeof( payload ), 0 ))
		return false;

	// v12: the right version, but the dheader_t puts the entities lump one
	// sector later than the descriptor's walk does - writer/reader drift.
	if( !WriteMpakFile( "badlayout.mpak", MPAK_LAYOUT_VERSION, payload, sizeof( payload ), 2 * TEST_FIRST_SECTOR ))
		return false;

	// v7: a descriptor with no BSP30 dheader_t in front of it is not a map
	// archive - a plain file renamed .mpak, or UiPacker's menu archives.
	if( !WriteBrokenSectorFile( "nobsp.mpak", 0, "MPAK", payload, sizeof( payload )))
		return false;

	// Re-scan to discover the newly-written files.
	g_fs.AddGameDirectory( "./", FS_GAMEDIR_PATH );

	if( g_fs.FileExists( "maps/badmagic.bsp", true ))
	{
		printf( "TestRejectsBadArchive: bad-magic archive was incorrectly mounted\n" );
		ok = false;
	}

	if( g_fs.FileExists( "maps/badversion.bsp", true ))
	{
		printf( "TestRejectsBadArchive: bad-version archive was incorrectly mounted\n" );
		ok = false;
	}

	if( g_fs.FileExists( "maps/badlayout.bsp", true ) || g_fs.FileExists( "maps/badlayout.mpak", true ))
	{
		printf( "TestRejectsBadArchive: archive with a drifted dheader_t was incorrectly mounted\n" );
		ok = false;
	}

	if( g_fs.FileExists( "maps/nobsp.bsp", true ))
	{
		printf( "TestRejectsBadArchive: archive with no dheader_t was incorrectly mounted\n" );
		ok = false;
	}

	g_fs.Delete( "badmagic.mpak" );
	g_fs.Delete( "badversion.mpak" );
	g_fs.Delete( "badlayout.mpak" );
	g_fs.Delete( "nobsp.mpak" );
	return ok;
}

// Found in review, twice. Round 1: an earlier version of this file (and
// of searchpath.c's g_archives[] comment) claimed an .mpak wins over a
// same-named loose map because of where its entry sat in g_archives[] -
// wrong, since FS_AddGameDirectory always mounts the whole archive group
// before the plain directory, so the plain directory always won
// regardless of array order. Round 2: making .mpak actually win (via a
// mount_after_plain_dir second pass) turned out to be actively harmful
// right now, not just premature - filesystem/mpak.c serves raw
// MPAK-magic bytes, and Mod_LoadModel's format-dispatch switch (model.c)
// does not recognize that magic yet, so a map that would otherwise load
// fine from its loose .bsp would instead hard-fail the moment its baked
// .mpak sibling was merely present alongside it. mount_after_plain_dir
// was removed entirely; .mpak is an ordinary g_archives[] entry now, so
// a loose .bsp wins if both are present - this test now proves THAT
// (the currently-correct, currently-safe behavior), not the reverse.
// Precedence flips back to favor .mpak once a real Mod_LoadModel/
// brush-model MPAK parser exists to make winning safe.
//
// Both files must exist BEFORE the one AddGameDirectory call that
// discovers them, in a directory never scanned before in this process -
// FS_AddArchive_Fullpath treats a directory already mounted at a given
// path as a no-op (matched by type+filename), so a later re-scan never
// re-prepends it; only freshly-discovered archives from that later call
// get prepended, which would make them appear to "win" regardless of the
// real mount-order logic being tested. A fresh "precedence_test/"
// subdirectory, scanned exactly once with both files already in place,
// is what actually exercises it.
static qboolean TestLooseMapStillWinsOverMpak( void )
{
	byte mpakPayload[64];
	byte looseBsp[32];
	byte *data;
	fs_offset_t len;
	file_t *f;
	qboolean ok;

	for( size_t i = 0; i < sizeof( mpakPayload ); i++ )
		mpakPayload[i] = (byte)( 0x11 + i );
	// Deliberately a different byte pattern (and a different length) than
	// mpakPayload, so a pass that returns the wrong file's bytes cannot
	// pass by coincidence.
	for( size_t i = 0; i < sizeof( looseBsp ); i++ )
		looseBsp[i] = (byte)( 0xEE - i );

	// Writing relative to precedence_test/ only needs the writepath
	// established by main()'s earlier AddGameDirectory("./", ...) calls -
	// it does not scan or mount precedence_test/ itself.
	if( !WriteMpakFile( "precedence_test/crossfire.mpak", MPAK_LAYOUT_VERSION, mpakPayload, sizeof( mpakPayload ), 0 ))
		return false;

	f = g_fs.Open( "precedence_test/maps/crossfire.bsp", "wb", true );
	if( !f )
	{
		printf( "TestLooseMapStillWinsOverMpak: Open(precedence_test/maps/crossfire.bsp) failed\n" );
		return false;
	}
	if( g_fs.Write( f, looseBsp, sizeof( looseBsp )) != (fs_offset_t)sizeof( looseBsp ))
	{
		printf( "TestLooseMapStillWinsOverMpak: short write\n" );
		g_fs.Close( f );
		return false;
	}
	g_fs.Close( f );

	// The one and only scan of this directory: both files already exist,
	// so this is where mount order is actually decided.
	g_fs.AddGameDirectory( "precedence_test/", FS_GAMEDIR_PATH );

	data = g_fs.LoadFile( "maps/crossfire.bsp", &len, true );
	if( !data )
	{
		printf( "TestLooseMapStillWinsOverMpak: LoadFile(maps/crossfire.bsp) failed\n" );
		return false;
	}

	// The loose file must win: nothing can parse crossfire.mpak's bytes
	// yet (filesystem/mpak.c is a whole-file passthrough, not a BSP-lump
	// parser), so resolving to it instead would silently break a
	// previously-loadable map the instant its baked .mpak sibling exists.
	ok = true;
	if( len != (fs_offset_t)sizeof( looseBsp ) || memcmp( data, looseBsp, sizeof( looseBsp )) != 0 )
	{
		printf( "TestLooseMapStillWinsOverMpak: resolved to crossfire.mpak instead of the loose "
			"maps/crossfire.bsp (len %lld, expected %lld)\n", (long long)len, (long long)sizeof( looseBsp ));
		ok = false;
	}

	free( data );
	g_fs.Delete( "precedence_test/maps/crossfire.bsp" );
	// crossfire.mpak stays open (mounted), same reasoning as c1a3.mpak above.
	return ok;
}

int main( void )
{
	if( !LoadFilesystem() )
		return EXIT_FAILURE;

	if( !TestDiscoveryAndWholeFilePassthrough())
		return EXIT_FAILURE;

	if( !TestRejectsBadArchive())
		return EXIT_FAILURE;

	if( !TestLooseMapStillWinsOverMpak())
		return EXIT_FAILURE;

	FreeLibrary( g_hModule );

	printf( "success\n" );

	return EXIT_SUCCESS;
}
