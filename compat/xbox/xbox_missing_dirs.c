/*
xbox_missing_dirs.c - directories the file layer has seen do not exist
Resonance3D (docs/r3d/divergences.md #164); see xbox_missing_dirs.h
*/
#include <string.h>
#include "xbox_missing_dirs.h"

r3d_missing_dirs_stats_t r3d_missing_dirs_stats;

static struct
{
	uint16_t start[R3D_MISSING_DIRS_MAX];
	uint16_t len[R3D_MISSING_DIRS_MAX];
	int      count;
	size_t   used;
	char     chars[R3D_MISSING_DIRS_CHARS];
} r3d_md;

void r3d_missing_dirs_flush( void )
{
	if( r3d_md.count )
		r3d_missing_dirs_stats.flushes++;
	r3d_md.count = 0;
	r3d_md.used = 0;
}

/* Every component non-empty and neither "." nor "..": the path names
   what it spells. The drive ("E:") is the first component. */
static int r3d_md_plain( const char *p, size_t n )
{
	size_t start = 0;

	if( n == 0 )
		return 0;

	for( size_t i = 0; i <= n; i++ )
	{
		if( i == n || p[i] == '\\' )
		{
			const size_t c = i - start;

			if( c == 0 || ( c == 1 && p[start] == '.' ) || ( c == 2 && p[start] == '.' && p[start + 1] == '.' ))
				return 0;
			start = i + 1;
		}
		else if( p[i] == '/' )
			return 0; /* not a native path: never produced by r3d_native_path */
	}
	return 1;
}

int r3d_missing_dirs_covers( const char *native )
{
	const size_t n = strlen( native );

	if( !r3d_md.count || !r3d_md_plain( native, n ))
		return 0;

	for( int i = 0; i < r3d_md.count; i++ )
	{
		const size_t len = r3d_md.len[i];

		if( n > len && native[len] == '\\' && !memcmp( native, r3d_md.chars + r3d_md.start[i], len ))
		{
			r3d_missing_dirs_stats.answered++;
			return 1;
		}
	}
	return 0;
}

static void r3d_md_record( const char *dir, size_t len )
{
	/* "E:" alone is a drive, not a directory to remember */
	if( len < 3 || !r3d_md_plain( dir, len ))
		return;

	for( int i = 0; i < r3d_md.count; i++ )
	{
		const size_t have = r3d_md.len[i];

		/* already covered: the same directory, or one above it */
		if( len >= have && ( len == have || dir[have] == '\\' )
			&& !memcmp( dir, r3d_md.chars + r3d_md.start[i], have ))
			return;
	}

	/* full: start again (forgetting is always safe) */
	if( r3d_md.count == R3D_MISSING_DIRS_MAX || r3d_md.used + len > R3D_MISSING_DIRS_CHARS )
		r3d_missing_dirs_flush();

	if( len > R3D_MISSING_DIRS_CHARS )
		return;

	memcpy( r3d_md.chars + r3d_md.used, dir, len );
	r3d_md.start[r3d_md.count] = (uint16_t)r3d_md.used;
	r3d_md.len[r3d_md.count] = (uint16_t)len;
	r3d_md.used += len;
	r3d_md.count++;
	r3d_missing_dirs_stats.recorded++;
}

void r3d_missing_dirs_note_dir( const char *native )
{
	/* as looked up: a trailing separator is an empty component, which
	   r3d_md_plain refuses (stripping it would record another path) */
	r3d_md_record( native, strlen( native ));
}
