/*
xbox_dir_cache.c - listings of the disc's directories, scanned once
Resonance3D (docs/r3d/divergences.md #166); see xbox_dir_cache.h
*/
#include <string.h>
#include "xbox_dir_cache.h"

r3d_dir_cache_stats_t r3d_dir_cache_stats;

static struct
{
	struct
	{
		uint16_t key, key_len;   /* the pattern, in chars[] */
		uint16_t data;           /* the entries, in chars[] */
		uint16_t bytes;
		int      count;
	} dir[R3D_DIR_CACHE_DIRS];
	int    dirs;
	size_t used;
	char   chars[R3D_DIR_CACHE_BYTES];
} r3d_dc;

void r3d_dir_cache_flush( void )
{
	if( r3d_dc.dirs )
		r3d_dir_cache_stats.flushes++;
	r3d_dc.dirs = 0;
	r3d_dc.used = 0;
}

int r3d_dir_cache_cacheable( const char *pattern )
{
	const size_t n = strlen( pattern );
	size_t start;

	/* "D:\...\*": the disc, scanned with the pattern opendir() builds */
	if( n < 4 || pattern[0] != 'D' || pattern[1] != ':' || pattern[2] != '\\'
		|| pattern[n - 1] != '*' || pattern[n - 2] != '\\' )
		return 0;

	/* every component between "D:\" and "\*" plain ("D:\*", the root, has none) */
	start = 3;
	for( size_t i = 3; i <= n - 2 && n > 4; i++ )
	{
		if( i == n - 2 || pattern[i] == '\\' )
		{
			const size_t c = i - start;

			if( c == 0 || ( c == 1 && pattern[start] == '.' )
				|| ( c == 2 && pattern[start] == '.' && pattern[start + 1] == '.' ))
				return 0;
			start = i + 1;
		}
		else if( pattern[i] == '/' || pattern[i] == '*' || pattern[i] == '?' )
			return 0;
	}
	return 1;
}

int r3d_dir_cache_find( const char *pattern, r3d_dir_listing_t *out )
{
	const size_t n = strlen( pattern );

	for( int i = 0; i < r3d_dc.dirs; i++ )
	{
		if( r3d_dc.dir[i].key_len == n && !memcmp( r3d_dc.chars + r3d_dc.dir[i].key, pattern, n ))
		{
			out->data = r3d_dc.chars + r3d_dc.dir[i].data;
			out->bytes = r3d_dc.dir[i].bytes;
			out->count = r3d_dc.dir[i].count;
			r3d_dir_cache_stats.replayed++;
			return 1;
		}
	}
	return 0;
}

void r3d_dir_cache_store( const char *pattern, const char *data, size_t bytes, int count )
{
	const size_t n = strlen( pattern );
	r3d_dir_listing_t have;

	if( !r3d_dir_cache_cacheable( pattern ) || n + bytes > R3D_DIR_CACHE_BYTES || count < 0 )
		return;

	if( r3d_dir_cache_find( pattern, &have ))
	{
		r3d_dir_cache_stats.replayed--; /* a lookup, not a replay */
		return;
	}

	/* full: start again (forgetting is always safe) */
	if( r3d_dc.dirs == R3D_DIR_CACHE_DIRS || r3d_dc.used + n + bytes > R3D_DIR_CACHE_BYTES )
		r3d_dir_cache_flush();

	memcpy( r3d_dc.chars + r3d_dc.used, pattern, n );
	r3d_dc.dir[r3d_dc.dirs].key = (uint16_t)r3d_dc.used;
	r3d_dc.dir[r3d_dc.dirs].key_len = (uint16_t)n;
	r3d_dc.used += n;
	memcpy( r3d_dc.chars + r3d_dc.used, data, bytes );
	r3d_dc.dir[r3d_dc.dirs].data = (uint16_t)r3d_dc.used;
	r3d_dc.dir[r3d_dc.dirs].bytes = (uint16_t)bytes;
	r3d_dc.dir[r3d_dc.dirs].count = count;
	r3d_dc.used += bytes;
	r3d_dc.dirs++;
	r3d_dir_cache_stats.stored++;
}
