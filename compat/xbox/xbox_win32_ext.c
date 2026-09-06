/*
xbox_win32_ext.c - the Win32 surface nxdk does not implement

Part of compat/xbox; see that directory's README.

XASH_XBOX is a sub-platform of XASH_WIN32, so the engine's Win32 code paths are
live. nxdk implements most of what they need but not all, and the gaps are not
where you would guess: it has CreateFileA and FindFirstFileA but no
GetModuleFileName, no MultiByteToWideChar, no GetUserName.

Everything here is a real implementation over what nxdk does provide, except
where the concept does not exist on the console at all - those say so and fail
rather than returning a plausible lie.
*/

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <errno.h>
#include <sys/utime.h>
#include <time.h>

/*
====================
GetModuleFileNameA

There is no module list on the Xbox: one XBE is mapped and that is all. The
engine calls this to locate itself, so it answers with the launch path the
kernel recorded, which is what "where am I" means here.

XeImageFileName is a counted ANSI string in kernel object-manager form
("\\Device\\Harddisk0\\Partition2\\default.xbe"), not a drive-letter path.
Callers that only need a directory to hang a search path off are served; a
caller that tried to CreateFileA the result would need the mapping applied
first. Nothing in the engine does that today.
====================
*/
DWORD WINAPI GetModuleFileNameA( HMODULE module, LPSTR filename, DWORD size )
{
	const char *src;
	DWORD       len;

	( void )module;

	if( !filename || size == 0 )
		return 0;

	src = ( const char * )XeImageFileName->Buffer;
	len = ( DWORD )XeImageFileName->Length;

	if( len >= size )
		len = size - 1;

	memcpy( filename, src, len );
	filename[len] = '\0';

	return len;
}

DWORD WINAPI GetModuleFileNameW( HMODULE module, LPWSTR filename, DWORD size )
{
	char  narrow[MAX_PATH];
	DWORD len, i;

	len = GetModuleFileNameA( module, narrow, sizeof( narrow ));

	if( !filename || size == 0 )
		return 0;

	if( len >= size )
		len = size - 1;

	for( i = 0; i < len; i++ )
		filename[i] = ( WCHAR )( unsigned char )narrow[i];

	filename[len] = 0;
	return len;
}

/*
====================
MultiByteToWideChar / WideCharToMultiByte

nxdk has neither. Only the UTF-8 and ANSI code pages are honoured, which is all
the engine and miniz ask for.

The UTF-8 decoder here is deliberately strict about length but permissive about
content: a malformed sequence becomes U+FFFD rather than an error, because the
callers treat a failed conversion as a failed file operation and a mangled
filename is more useful to debug than a silent absence.

Surrogate pairs are handled, so characters outside the BMP survive.
====================
*/
static int r3d_utf8_decode( const unsigned char *s, int len, unsigned int *out )
{
	unsigned int cp;
	int          extra, i;

	if( len <= 0 ) return 0;

	if( s[0] < 0x80 )        { *out = s[0]; return 1; }
	else if(( s[0] & 0xE0 ) == 0xC0 ) { cp = s[0] & 0x1F; extra = 1; }
	else if(( s[0] & 0xF0 ) == 0xE0 ) { cp = s[0] & 0x0F; extra = 2; }
	else if(( s[0] & 0xF8 ) == 0xF0 ) { cp = s[0] & 0x07; extra = 3; }
	else { *out = 0xFFFD; return 1; }

	if( extra >= len ) { *out = 0xFFFD; return 1; }

	for( i = 1; i <= extra; i++ )
	{
		if(( s[i] & 0xC0 ) != 0x80 ) { *out = 0xFFFD; return 1; }
		cp = ( cp << 6 ) | ( s[i] & 0x3F );
	}

	*out = cp;
	return extra + 1;
}

int WINAPI MultiByteToWideChar( UINT page, DWORD flags, LPCSTR src, int src_len,
	LPWSTR dst, int dst_len )
{
	const unsigned char *in = ( const unsigned char * )src;
	int written = 0, consumed = 0, total;

	( void )flags;

	if( !src ) { SetLastError( ERROR_INVALID_PARAMETER ); return 0; }

	total = ( src_len < 0 ) ? ( int )strlen( src ) + 1 : src_len;

	while( consumed < total )
	{
		unsigned int cp;
		int          n;

		if( page == CP_UTF8 )
		{
			n = r3d_utf8_decode( in + consumed, total - consumed, &cp );
			if( n <= 0 ) break;
		}
		else
		{
			cp = in[consumed];   /* ANSI: one byte, one code point */
			n  = 1;
		}

		consumed += n;

		if( cp >= 0x10000 )
		{
			/* Surrogate pair. Both halves or neither. */
			if( dst )
			{
				if( written + 2 > dst_len )
				{
					SetLastError( ERROR_INSUFFICIENT_BUFFER );
					return 0;
				}
				cp -= 0x10000;
				dst[written]     = ( WCHAR )( 0xD800 + ( cp >> 10 ));
				dst[written + 1] = ( WCHAR )( 0xDC00 + ( cp & 0x3FF ));
			}
			written += 2;
		}
		else
		{
			if( dst )
			{
				if( written + 1 > dst_len )
				{
					SetLastError( ERROR_INSUFFICIENT_BUFFER );
					return 0;
				}
				dst[written] = ( WCHAR )cp;
			}
			written += 1;
		}
	}

	return written;   /* dst == NULL means "how big a buffer?" */
}

int WINAPI WideCharToMultiByte( UINT page, DWORD flags, LPCWSTR src, int src_len,
	LPSTR dst, int dst_len, LPCSTR def_char, LPBOOL used_def )
{
	int written = 0, i, total;

	( void )flags;
	( void )def_char;

	if( used_def ) *used_def = FALSE;
	if( !src ) { SetLastError( ERROR_INVALID_PARAMETER ); return 0; }

	total = ( src_len < 0 ) ? ( int )wcslen( src ) + 1 : src_len;

	for( i = 0; i < total; i++ )
	{
		unsigned int cp = src[i];

		/* Recombine a surrogate pair into one code point. */
		if( cp >= 0xD800 && cp <= 0xDBFF && i + 1 < total
			&& src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF )
		{
			cp = 0x10000 + (( cp - 0xD800 ) << 10 ) + ( src[i + 1] - 0xDC00 );
			i++;
		}

		if( page != CP_UTF8 )
		{
			if( dst )
			{
				if( written >= dst_len ) { SetLastError( ERROR_INSUFFICIENT_BUFFER ); return 0; }
				dst[written] = ( cp < 0x100 ) ? ( char )cp : '?';
				if( cp >= 0x100 && used_def ) *used_def = TRUE;
			}
			written++;
			continue;
		}

		if( cp < 0x80 )
		{
			if( dst )
			{
				if( written + 1 > dst_len ) { SetLastError( ERROR_INSUFFICIENT_BUFFER ); return 0; }
				dst[written] = ( char )cp;
			}
			written += 1;
		}
		else if( cp < 0x800 )
		{
			if( dst )
			{
				if( written + 2 > dst_len ) { SetLastError( ERROR_INSUFFICIENT_BUFFER ); return 0; }
				dst[written]     = ( char )( 0xC0 | ( cp >> 6 ));
				dst[written + 1] = ( char )( 0x80 | ( cp & 0x3F ));
			}
			written += 2;
		}
		else if( cp < 0x10000 )
		{
			if( dst )
			{
				if( written + 3 > dst_len ) { SetLastError( ERROR_INSUFFICIENT_BUFFER ); return 0; }
				dst[written]     = ( char )( 0xE0 | ( cp >> 12 ));
				dst[written + 1] = ( char )( 0x80 | (( cp >> 6 ) & 0x3F ));
				dst[written + 2] = ( char )( 0x80 | ( cp & 0x3F ));
			}
			written += 3;
		}
		else
		{
			if( dst )
			{
				if( written + 4 > dst_len ) { SetLastError( ERROR_INSUFFICIENT_BUFFER ); return 0; }
				dst[written]     = ( char )( 0xF0 | ( cp >> 18 ));
				dst[written + 1] = ( char )( 0x80 | (( cp >> 12 ) & 0x3F ));
				dst[written + 2] = ( char )( 0x80 | (( cp >> 6 ) & 0x3F ));
				dst[written + 3] = ( char )( 0x80 | ( cp & 0x3F ));
			}
			written += 4;
		}
	}

	return written;
}

/*
====================
utime

FATX stores a write time, so this is a real operation rather than a stub. The
access time is discarded: the filesystem does not track one.
====================
*/
int utime( const char *path, const struct utimbuf *times )
{
	char     native[MAX_PATH];
	HANDLE   h;
	FILETIME ft;
	unsigned long long t;
	size_t   i;
	BOOL     ok;

	for( i = 0; i + 1 < sizeof( native ) && path[i]; i++ )
		native[i] = ( path[i] == '/' ) ? '\\' : path[i];

	if( path[i] ) { errno = ENAMETOOLONG; return -1; }
	native[i] = '\0';

	h = CreateFileA( native, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );

	if( h == INVALID_HANDLE_VALUE ) { errno = ENOENT; return -1; }

	/* time_t seconds from 1970 back to FILETIME's 100ns units from 1601. */
	t = (( unsigned long long )( times ? times->modtime : 0 ) + 11644473600ULL )
		* 10000000ULL;

	ft.dwLowDateTime  = ( DWORD )( t & 0xFFFFFFFFULL );
	ft.dwHighDateTime = ( DWORD )( t >> 32 );

	ok = SetFileTime( h, NULL, NULL, &ft );
	CloseHandle( h );

	return ok ? 0 : ( errno = EIO, -1 );
}

int _utime( const char *path, const struct utimbuf *times )
{
	return utime( path, times );
}


/*
====================
GetUserNameA / GetUserNameW

The Xbox has no user accounts in the Win32 sense. The engine uses this only to
seed a default player name, so a fixed answer is correct rather than a
placeholder — there is no better one to discover.
====================
*/
static const char *R3D_USER_NAME = "Player";

BOOL WINAPI GetUserNameA( LPSTR buffer, LPDWORD size )
{
	DWORD need = ( DWORD )strlen( R3D_USER_NAME ) + 1;

	if( !size ) { SetLastError( ERROR_INVALID_PARAMETER ); return FALSE; }

	if( !buffer || *size < need )
	{
		*size = need;
		SetLastError( ERROR_INSUFFICIENT_BUFFER );
		return FALSE;
	}

	memcpy( buffer, R3D_USER_NAME, need );
	*size = need;
	return TRUE;
}

BOOL WINAPI GetUserNameW( LPWSTR buffer, LPDWORD size )
{
	DWORD need = ( DWORD )strlen( R3D_USER_NAME ) + 1, i;

	if( !size ) { SetLastError( ERROR_INVALID_PARAMETER ); return FALSE; }

	if( !buffer || *size < need )
	{
		*size = need;
		SetLastError( ERROR_INSUFFICIENT_BUFFER );
		return FALSE;
	}

	for( i = 0; i < need; i++ )
		buffer[i] = ( WCHAR )( unsigned char )R3D_USER_NAME[i];

	*size = need;
	return TRUE;
}

/*
====================
GetCursorPos / SetCursorPos

There is no pointer. Reporting failure rather than (0,0) matters: a caller
that believes the cursor is at the origin will act on it, where one that sees
the call fail can fall back to gamepad navigation. See docs/r3d/ui-architecture.md.
====================
*/
BOOL WINAPI GetCursorPos( LPPOINT point )
{
	/* Write a defined value even though this fails. Win32 leaves *point
	 * untouched on failure and a careful caller would check the return; the two
	 * in-tree callers - pfnGetWindowCenterX and pfnGetWindowCenterY, at
	 * engine/client/dll_int/cl_game.c:2040 and :2066 - do not. Both declare a
	 * bare POINT on the stack and return a field of it regardless. An
	 * uninitialised read is worse than a wrong-but-defined origin, and this
	 * costs two stores. */
	if( point )
	{
		point->x = 0;
		point->y = 0;
	}

	SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
	return FALSE;
}

BOOL WINAPI SetCursorPos( int x, int y )
{
	( void )x; ( void )y;
	SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
	return FALSE;
}

/*
====================
_wfullpath

Canonicalises a path to absolute form. Everything the engine passes here is
already absolute — nxdk resolves paths against a drive letter and there is no
working directory to be relative to — so this copies rather than resolving,
and says so instead of pretending to normalise "..".
====================
*/
WCHAR *_wfullpath( WCHAR *abs, const WCHAR *rel, size_t max )
{
	size_t i;

	if( !rel ) return NULL;

	if( !abs )
	{
		SetLastError( ERROR_INVALID_PARAMETER );
		return NULL;   /* the allocating form is not supported */
	}

	for( i = 0; i + 1 < max && rel[i]; i++ )
		abs[i] = rel[i];

	if( rel[i] ) return NULL;   /* did not fit */

	abs[i] = 0;
	return abs;
}


/*
====================
GetModuleHandleA / GetModuleHandleExA

Exactly one XBE is mapped, so "the module containing this address" and "the
module named X" have the same unambiguous answer. Returning a fixed non-NULL
token is correct rather than a placeholder: every caller here only passes the
result back to GetModuleFileName, which ignores it for the same reason.
====================
*/
#define R3D_ONLY_MODULE (( HMODULE )0x00010000 )   /* the XBE base address */

HMODULE WINAPI GetModuleHandleA( LPCSTR name )
{
	( void )name;
	return R3D_ONLY_MODULE;
}

BOOL WINAPI GetModuleHandleExA( DWORD flags, LPCSTR name, HMODULE *module )
{
	( void )flags; ( void )name;
	if( !module ) { SetLastError( ERROR_INVALID_PARAMETER ); return FALSE; }
	*module = R3D_ONLY_MODULE;
	return TRUE;
}

BOOL WINAPI GetModuleHandleExW( DWORD flags, LPCWSTR name, HMODULE *module )
{
	( void )flags; ( void )name;
	if( !module ) { SetLastError( ERROR_INVALID_PARAMETER ); return FALSE; }
	*module = R3D_ONLY_MODULE;
	return TRUE;
}


/*
====================
localtime_s / gmtime_s

MSVC's Annex K bounds-checked time functions. pdclib is plain C99 and has the
unchecked forms only.

Argument ORDER differs between MSVC and C11 Annex K, and they disagree about
the return value. These follow MSVC, because that is what public/miniz.c was
written against and MSVC is what _WIN32 implies to it.
====================
*/
errno_t localtime_s( struct tm *result, const time_t *timer )
{
	struct tm *r;

	if( !result || !timer ) return EINVAL;

	r = localtime( timer );
	if( !r ) return EINVAL;

	*result = *r;
	return 0;
}

errno_t gmtime_s( struct tm *result, const time_t *timer )
{
	struct tm *r;

	if( !result || !timer ) return EINVAL;

	r = gmtime( timer );
	if( !r ) return EINVAL;

	*result = *r;
	return 0;
}
