// Proves the filesystem plumbing added for X2 (docs/r3d/fork-plan.md,
// filesystem/mpak.c): an .mpak archive dropped into a scanned game
// directory is discovered via g_archives[] (searchpath.c), and asking the
// VFS for the map name it represents ("maps/<basename>.bsp") returns the
// archive's bytes back verbatim. Does NOT exercise MpakHeader6's lump
// layout - filesystem/mpak.c is deliberately a whole-file passthrough,
// not a BSP-lump parser (see its own header comment for why). Also
// proves a loose "maps/<name>.bsp" still wins over a same-named .mpak
// when both are present - the ONLY currently-safe ordering, since
// nothing can parse an .mpak's bytes yet (see
// TestLooseMapStillWinsOverMpak's own comment for the regression this
// guards against).
#include "filesystem_test_common.h"

// A real MpakHeader6 (tools/asset-studio/src/formats/MpakHeader.h) is
// 2048-byte padded; this test only needs a buffer whose first 8 bytes are
// a valid probe header ("MPAK" + version 6, little-endian) - the rest is
// arbitrary payload standing in for the real lump data, since mpak.c never
// looks past those 8 bytes.
static qboolean WriteMpakFile( const char *path, uint32_t version, const byte *payload, size_t payloadSize )
{
	file_t *f = g_fs.Open( path, "wb", true );
	uint32_t versionLE;

	if( !f )
	{
		printf( "WriteMpakFile: Open(%s) failed\n", path );
		return false;
	}

	versionLE = version; // this test only runs little-endian hosts

	if( g_fs.Write( f, "MPAK", 4 ) != 4 ||
		g_fs.Write( f, &versionLE, sizeof( versionLE )) != sizeof( versionLE ) ||
		( payloadSize > 0 && g_fs.Write( f, payload, payloadSize ) != (fs_offset_t)payloadSize ))
	{
		printf( "WriteMpakFile: short write\n" );
		g_fs.Close( f );
		return false;
	}

	g_fs.Close( f );
	return true;
}

static qboolean TestDiscoveryAndWholeFilePassthrough( void )
{
	byte payload[512];
	byte fullExpected[8 + sizeof( payload )];
	byte *data;
	fs_offset_t len;
	qboolean ok;

	for( size_t i = 0; i < sizeof( payload ); i++ )
		payload[i] = (byte)( ( i * 2654435761u ) >> 24 );

	// A writepath has to exist before g_fs.Open("wb", ...) below can
	// create anything (same ordering filecopy.c/caseinsensitive.c use).
	g_fs.AddGameDirectory( "./", FS_GAMEDIR_PATH );

	if( !WriteMpakFile( "c1a3.mpak", 6, payload, sizeof( payload )))
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

	memcpy( fullExpected, "MPAK", 4 );
	memcpy( fullExpected + 4, &( uint32_t ){ 6 }, sizeof( uint32_t ));
	memcpy( fullExpected + 8, payload, sizeof( payload ));

	ok = true;
	if( len != (fs_offset_t)sizeof( fullExpected ))
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: size mismatch (%lld != %lld)\n",
			(long long)len, (long long)sizeof( fullExpected ));
		ok = false;
	}
	else if( memcmp( data, fullExpected, sizeof( fullExpected )) != 0 )
	{
		printf( "TestDiscoveryAndWholeFilePassthrough: content mismatch\n" );
		ok = false;
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

static qboolean WriteBadMagicFile( const char *path, const byte *payload, size_t payloadSize )
{
	file_t *f = g_fs.Open( path, "wb", true );
	uint32_t version = 6;

	if( !f )
	{
		printf( "WriteBadMagicFile: Open(%s) failed\n", path );
		return false;
	}

	if( g_fs.Write( f, "XXXX", 4 ) != 4 ||
		g_fs.Write( f, &version, sizeof( version )) != sizeof( version ) ||
		g_fs.Write( f, payload, payloadSize ) != (fs_offset_t)payloadSize )
	{
		printf( "WriteBadMagicFile: short write\n" );
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

	if( !WriteBadMagicFile( "badmagic.mpak", payload, sizeof( payload )))
		return false;

	if( !WriteMpakFile( "badversion.mpak", 99, payload, sizeof( payload )))
		return false;

	// Re-scan to discover both newly-written files.
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

	g_fs.Delete( "badmagic.mpak" );
	g_fs.Delete( "badversion.mpak" );
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
	if( !WriteMpakFile( "precedence_test/crossfire.mpak", 6, mpakPayload, sizeof( mpakPayload )))
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
