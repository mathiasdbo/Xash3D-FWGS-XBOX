/*
xbox_title_meta.c - the dashboard's name and picture for this game
(docs/r3d/divergences.md #215)

The dashboard's memory screen lists E:\UDATA\<title id>\ by reading two files
in it: TitleMeta.xbx, a UTF-16 "TitleName=<name>" line, and TitleImage.xbx,
the 128x128 XPR picture. Without them the entry shows as an unnamed
folder. Retail titles ship both; nothing in an nxdk build writes them.

The title folder is created in filesystem_engine.c; this is called right
after, once the folder exists. It writes TitleMeta.xbx, and copies
TitleImage.xbx from the XBE's own folder (D:\titleimage.xbx, put there by the
packager) when that file is there. Files already in the folder are left
alone, so a player's own picture or a later name stays.
*/
#include "common.h"
#include "platform/platform.h"

#include <windows.h>

#ifndef XASH_GAMEDIR
#define XASH_GAMEDIR "valve" // same default as filesystem_engine.c and launcher.c
#endif

#define TITLE_IMAGE_MAX 16384 // a 128x128 DXT1 XPR is 10240 bytes

static const char *Xbox_TitleName( void )
{
	if( !Q_strcmp( XASH_GAMEDIR, "gearbox" ))
		return "Opposing Force";
	if( !Q_strcmp( XASH_GAMEDIR, "bshift" ))
		return "Blue Shift";
	return "Half-Life";
}

static qboolean Xbox_Exists( const char *path )
{
	return GetFileAttributesA( path ) != INVALID_FILE_ATTRIBUTES;
}

static void Xbox_WriteNew( const char *path, const void *data, DWORD size )
{
	DWORD  done = 0;
	HANDLE h = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );

	if( h == INVALID_HANDLE_VALUE )
		return;

	if( !WriteFile( h, data, size, &done, NULL ) || done != size )
	{
		CloseHandle( h );
		DeleteFileA( path ); // never leave half a file for the dashboard to read
		return;
	}
	CloseHandle( h );
}

/*
=============
Xbox_WriteTitleMeta

title_dir is E:\UDATA\<title id>, already created
=============
*/
void Xbox_WriteTitleMeta( const char *title_dir )
{
	string path;
	byte   meta[2 + 2 * 64];
	byte   image[TITLE_IMAGE_MAX];
	DWORD  size = 0, got = 0;
	const char *name = Xbox_TitleName();
	const char *src = "D:\\titleimage.xbx";
	char   line[64];
	int    i, n;
	HANDLE h;

	Q_snprintf( path, sizeof( path ), "%s\\TitleMeta.xbx", title_dir );
	if( !Xbox_Exists( path ))
	{
		n = Q_snprintf( line, sizeof( line ), "TitleName=%s\r\n", name );
		meta[0] = 0xFF; // UTF-16 little endian byte order mark
		meta[1] = 0xFE;
		for( i = 0; i < n; i++ )
		{
			meta[2 + 2 * i] = line[i];
			meta[3 + 2 * i] = 0;
		}
		Xbox_WriteNew( path, meta, 2 + 2 * n );
	}

	Q_snprintf( path, sizeof( path ), "%s\\TitleImage.xbx", title_dir );
	if( Xbox_Exists( path ))
		return;

	h = CreateFileA( src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
	if( h == INVALID_HANDLE_VALUE )
		return;

	size = GetFileSize( h, NULL );
	if( size >= 0x800 && size <= TITLE_IMAGE_MAX && ReadFile( h, image, size, &got, NULL ) && got == size
		&& !memcmp( image, "XPR0", 4 ))
		Xbox_WriteNew( path, image, size );
	CloseHandle( h );
}
