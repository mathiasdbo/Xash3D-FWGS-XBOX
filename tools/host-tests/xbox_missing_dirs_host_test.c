/* Host test for compat/xbox/xbox_missing_dirs.c (docs/r3d/divergences.md
   #164): stat() and opendir() answered from the table of missing
   directories must give what the kernel would have, through any sequence
   of lookups and changes to the file system.

   A model of the kernel's answers stands in for FATX: a set of
   directories and files, where a lookup fails with ERROR_PATH_NOT_FOUND
   when a component above the last is missing or is a file, and with
   ERROR_FILE_NOT_FOUND when only the last is missing; a directory scan
   (FindFirstFileA "D\*": NtOpenFile of D, then the first entry) fails
   like a lookup of D, with ERROR_DIRECTORY when D is a file, and with
   ERROR_FILE_NOT_FOUND when D is empty (deps/nxdk/lib/winapi/findfile.c).
   Half the sequences use a second driver for D: that reports a missing
   last component as ERROR_PATH_NOT_FOUND as well.

   The calls are wrapped exactly as compat/xbox/xbox_posix_io.c wraps them:
   - stat: answered (ERROR_PATH_NOT_FOUND) if the table covers the path;
     else the kernel, and after a PATH_NOT_FOUND one lookup of the parent,
     which notes it when that fails with FILE_NOT_FOUND/PATH_NOT_FOUND;
   - opendir: answered if the table covers "D\*"; else the scan, and on
     FILE_NOT_FOUND/PATH_NOT_FOUND one lookup of D, which notes D when it
     fails with either;
   - a successful mkdir empties the table after it, a rename before it;
     file creation and deletion of files and trees do not touch it (they
     cannot make a missing directory exist), and the sequences include
     them to show that.
   Each wrapped call's result must equal the kernel model's result for the
   same call at the same moment, error code included; an answered call
   must be one the kernel fails with ERROR_PATH_NOT_FOUND.

   Paths come from a small vocabulary, so lookups hit recorded directories
   often, with "." and ".." components, doubled and trailing separators,
   other case, and both drives. Also checked: the table's own edges (full,
   out of characters, a drive alone, an entry above another).

   Built and run by tools/host-tests/run.sh:
     gcc -O2 -Wall -std=c99 -Icompat/xbox tools/host-tests/xbox_missing_dirs_host_test.c \
       compat/xbox/xbox_missing_dirs.c -o xbox_missing_dirs_test */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_missing_dirs.h"

#define OK                0
#define E_FILE_NOT_FOUND  2  /* winerror.h */
#define E_PATH_NOT_FOUND  3
#define E_DIRECTORY       267
#define E_ALREADY_EXISTS  183

static int failures;
static long checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { failures++; \
	if( failures <= 20 ) { printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); }}} while( 0 )

/* ---- the kernel model ---- */

#define MAX_NODES 512
static struct { char path[128]; int dir; } nodes[MAX_NODES];
static int nnodes;

/* A second driver: D: answers ERROR_PATH_NOT_FOUND for a missing last
   component too (the asset-pipeline review's question about XDVDFS). The
   table must stay exact under it, which needs the parent confirmed by its
   own lookup before it is recorded. */
static int leaf_pnf_on_d;

/* FATX compares names case-insensitively; the model does too, so the
   table's exact-spelling comparison is checked against a kernel that
   would accept another case. */
static int find_node_ci( const char *p, size_t n )
{
	for( int i = 0; i < nnodes; i++ )
	{
		if( strlen( nodes[i].path ) != n )
			continue;
		size_t k = 0;
		for( ; k < n; k++ )
		{
			char a = nodes[i].path[k], b = p[k];
			if( a >= 'A' && a <= 'Z' ) a += 32;
			if( b >= 'A' && b <= 'Z' ) b += 32;
			if( a != b ) break;
		}
		if( k == n ) return i;
	}
	return -1;
}

/* NtQueryFullAttributesFile: the drive ("E:") always exists. Components
   are compared whole, so "." and ".." are names that never exist, and an
   empty component (a doubled separator) fails as a missing intermediate. */
static int k_lookup( const char *p, int *is_dir )
{
	const size_t n = strlen( p );
	size_t i = 0;

	while( i < n && p[i] != '\\' ) i++;    /* the drive */
	if( i == n ) { *is_dir = 1; return OK; }

	for( ;; )
	{
		size_t j = i + 1;
		while( j < n && p[j] != '\\' ) j++;
		const int last = ( j >= n );
		const int k = ( j > i + 1 ) ? find_node_ci( p, j ) : -1;

		if( last )
		{
			if( k < 0 ) return ( leaf_pnf_on_d && p[0] == 'D' ) ? E_PATH_NOT_FOUND : E_FILE_NOT_FOUND;
			*is_dir = nodes[k].dir;
			return OK;
		}
		if( k < 0 || !nodes[k].dir )
			return E_PATH_NOT_FOUND;
		i = j;
	}
}

static int k_stat( const char *p )
{
	int d;
	return k_lookup( p, &d );
}

/* The canonical spelling of an existing node (or the drive itself), for
   prefix tests; NULL when the path does not exist. */
static const char *k_canon( const char *p, char *buf, size_t size )
{
	int d;
	const size_t n = strlen( p );

	if( k_lookup( p, &d ) != OK )
		return NULL;
	if( !strchr( p, '\\' ))
	{
		snprintf( buf, size, "%s", p ); /* a drive */
		return buf;
	}
	return nodes[find_node_ci( p, n )].path;
}

/* FindFirstFileA( "D\*" ) */
static int k_scan( const char *pattern )
{
	char dirp[160], cbuf[160];
	const size_t n = strlen( pattern ) - 2; /* strip "\*" */
	const char *canon;
	int d, r;

	memcpy( dirp, pattern, n );
	dirp[n] = 0;
	if(( r = k_lookup( dirp, &d )) != OK )
		return r;
	if( !d )
		return E_DIRECTORY;
	canon = k_canon( dirp, cbuf, sizeof( cbuf ));
	const size_t cn = strlen( canon );
	for( int i = 0; i < nnodes; i++ )
	{
		const char *q = nodes[i].path;
		if( strlen( q ) > cn + 1 && !strncmp( q, canon, cn ) && q[cn] == '\\' && !strchr( q + cn + 1, '\\' ))
			return OK;
	}
	return E_FILE_NOT_FOUND;
}

static int k_create( const char *p, int dir )
{
	int d;
	const size_t n = strlen( p );
	const char *slash = strrchr( p, '\\' );
	char parent[160];

	if( !slash || n >= sizeof( nodes[0].path ) || nnodes == MAX_NODES ) return E_PATH_NOT_FOUND;
	memcpy( parent, p, slash - p );
	parent[slash - p] = 0;
	if( k_lookup( parent, &d ) != OK || !d ) return E_PATH_NOT_FOUND;
	if( slash[1] == 0 || !strcmp( slash + 1, "." ) || !strcmp( slash + 1, ".." )) return E_PATH_NOT_FOUND;
	if( find_node_ci( p, n ) >= 0 ) return E_ALREADY_EXISTS;
	/* stored under its parent's canonical spelling */
	{
		char cbuf[160];
		const char *cp = k_canon( parent, cbuf, sizeof( cbuf ));
		snprintf( nodes[nnodes].path, sizeof( nodes[0].path ), "%s%s", cp, slash );
	}
	nodes[nnodes].dir = dir;
	nnodes++;
	return OK;
}

static void k_delete_tree( const char *p )
{
	char canon[128];
	const int k = find_node_ci( p, strlen( p ));

	if( k < 0 )
		return;
	strcpy( canon, nodes[k].path );
	const size_t n = strlen( canon );
	for( int i = 0; i < nnodes; )
	{
		const char *q = nodes[i].path;
		if( !strncmp( q, canon, n ) && ( q[n] == 0 || q[n] == '\\' ))
			nodes[i] = nodes[--nnodes];
		else
			i++;
	}
}

static int k_rename( const char *from, const char *to )
{
	char old[128], dest[128];
	const int k = find_node_ci( from, strlen( from ));

	if( k < 0 ) return E_FILE_NOT_FOUND;
	strcpy( old, nodes[k].path );
	const size_t n = strlen( old );
	if( !strncmp( to, old, n ) && ( to[n] == 0 || to[n] == '\\' )) return E_PATH_NOT_FOUND; /* into itself */
	if( k_create( to, nodes[k].dir ) != OK ) return E_PATH_NOT_FOUND;
	strcpy( dest, nodes[nnodes - 1].path );
	/* move the subtree under the new name, then drop the old one */
	for( int i = 0; i < nnodes; i++ )
	{
		const char *q = nodes[i].path;
		if( strlen( q ) > n && !strncmp( q, old, n ) && q[n] == '\\' && nnodes < MAX_NODES )
		{
			char tmp[256];
			snprintf( tmp, sizeof( tmp ), "%s%s", dest, q + n );
			if( strlen( tmp ) < sizeof( nodes[0].path ))
			{
				strcpy( nodes[nnodes].path, tmp );
				nodes[nnodes].dir = nodes[i].dir;
				nnodes++;
			}
		}
	}
	k_delete_tree( old );
	return OK;
}

/* ---- the wrapped calls, as in xbox_posix_io.c ---- */

static int w_stat( const char *p, int *answered )
{
	*answered = 0;
	if( r3d_missing_dirs_covers( p ))
	{
		*answered = 1;
		return E_PATH_NOT_FOUND;
	}
	const int r = k_stat( p );
	if( r == E_PATH_NOT_FOUND )
	{
		/* the parent, once its own lookup says it does not exist */
		const char *slash = strrchr( p, '\\' );
		if( slash && slash - p > 2 )
		{
			char parent[200];
			memcpy( parent, p, slash - p );
			parent[slash - p] = 0;
			const int pr = k_stat( parent );
			if( pr == E_FILE_NOT_FOUND || pr == E_PATH_NOT_FOUND )
				r3d_missing_dirs_note_dir( parent );
		}
	}
	return r;
}

static int w_scan( const char *dirp, int *answered )
{
	char pattern[200];
	size_t len = strlen( dirp );

	memcpy( pattern, dirp, len );
	if( len > 0 && pattern[len - 1] != '\\' ) pattern[len++] = '\\';
	pattern[len++] = '*';
	pattern[len] = 0;

	*answered = 0;
	if( r3d_missing_dirs_covers( pattern ))
	{
		*answered = 1;
		return E_PATH_NOT_FOUND; /* opendir says ENOENT whatever the code */
	}
	const int r = k_scan( pattern );
	if( r == E_FILE_NOT_FOUND || r == E_PATH_NOT_FOUND )
	{
		pattern[len - 2] = 0;
		const int d = k_stat( pattern );
		if( d == E_FILE_NOT_FOUND || d == E_PATH_NOT_FOUND )
			r3d_missing_dirs_note_dir( pattern );
	}
	return r;
}

/* ---- the test ---- */

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd( void ) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static const char *const words[] = { "a", "b", "c", "A", "sound", "x.wav", ".", "..", "", "custom" };

static void random_path( char *out, size_t size, int for_scan )
{
	int n = 1 + (int)( rnd() % 4 );
	size_t len = (size_t)snprintf( out, size, "%s", rnd() % 4 ? "E:" : "D:" );

	for( int i = 0; i < n; i++ )
		len += (size_t)snprintf( out + len, size - len, "\\%s", words[rnd() % ( sizeof( words ) / sizeof( words[0] ))] );
	if( for_scan && rnd() % 2 )
		snprintf( out + len, size - len, "\\" );
}

static long answered_stats, answered_scans, kernel_pnf;

static void run_sequence( int ops )
{
	nnodes = 0;
	r3d_missing_dirs_flush();
	k_create( "E:\\a", 1 );
	k_create( "E:\\a\\b", 1 );
	k_create( "E:\\a\\b\\x.wav", 0 );
	k_create( "D:\\sound", 1 );
	k_create( "D:\\sound\\x.wav", 0 );

	for( int o = 0; o < ops; o++ )
	{
		char p[200], q[200];
		int answered;
		const uint32_t what = rnd() % 20;

		random_path( p, sizeof( p ), what >= 10 && what < 15 );
		if( what < 10 )
		{
			const int expect = k_stat( p );
			const int got = w_stat( p, &answered );
			CHECK( got == expect, "stat %s: wrapped %d, kernel %d", p, got, expect );
			if( answered ) { answered_stats++; CHECK( expect == E_PATH_NOT_FOUND, "stat %s answered, kernel %d", p, expect ); }
			if( expect == E_PATH_NOT_FOUND ) kernel_pnf++;
		}
		else if( what < 15 )
		{
			char pattern[210];
			size_t len = strlen( p );
			memcpy( pattern, p, len + 1 );
			if( len > 0 && pattern[len - 1] != '\\' ) { pattern[len++] = '\\'; }
			pattern[len++] = '*'; pattern[len] = 0;
			const int expect = k_scan( pattern );
			const int got = w_scan( p, &answered );
			/* opendir reports success or ENOENT; an answered scan must be one the kernel fails */
			CHECK(( got == OK ) == ( expect == OK ), "scan %s: wrapped %d, kernel %d", p, got, expect );
			if( answered ) { answered_scans++; CHECK( expect != OK, "scan %s answered, kernel %d", p, expect ); }
			if( !answered ) CHECK( got == expect, "scan %s: wrapped %d, kernel %d", p, got, expect );
		}
		else
		{
			/* a change: the table is emptied only after a successful mkdir
			   and before any rename, as the file layer does; creating,
			   writing and deleting files or directories keep it */
			switch( what )
			{
			case 15: case 16: if( k_create( p, 1 ) == OK ) r3d_missing_dirs_flush(); break; /* mkdir */
			case 17: k_create( p, 0 ); break;                   /* open( O_CREAT ) */
			case 18: r3d_missing_dirs_flush(); random_path( q, sizeof( q ), 0 ); k_rename( p, q ); break;
			default: k_delete_tree( p ); break;                 /* remove / unlink / rmdir */
			}
		}
	}
}

static void table_edges( void )
{
	char p[300];

	r3d_missing_dirs_flush();
	r3d_missing_dirs_note_dir( "E:" );
	r3d_missing_dirs_note_dir( "E:\\" );
	CHECK( !r3d_missing_dirs_covers( "E:\\a" ), "a drive alone was recorded" );

	/* a trailing separator is an empty component: another path, never recorded */
	r3d_missing_dirs_note_dir( "E:\\m\\" );
	CHECK( !r3d_missing_dirs_covers( "E:\\m\\x" ), "a path with a trailing separator was recorded as another" );
	r3d_missing_dirs_note_dir( "E:\\m" );
	CHECK( r3d_missing_dirs_covers( "E:\\m\\x" ), "the entry was not recorded" );
	CHECK( !r3d_missing_dirs_covers( "E:\\m" ), "the directory itself was answered" );
	CHECK( !r3d_missing_dirs_covers( "E:\\mm\\x" ), "a longer name was answered" );
	CHECK( !r3d_missing_dirs_covers( "E:\\M\\x" ), "another case was answered" );
	CHECK( !r3d_missing_dirs_covers( "E:\\m\\..\\x" ), "a .. component was answered" );
	CHECK( !r3d_missing_dirs_covers( "E:\\m\\.\\x" ), "a . component was answered" );
	CHECK( !r3d_missing_dirs_covers( "E:\\m\\\\x" ), "an empty component was answered" );

	r3d_missing_dirs_note_dir( "E:\\n\\.." );
	CHECK( !r3d_missing_dirs_covers( "E:\\n\\..\\z" ), "a .. directory was recorded" );

	/* full by count: the oldest are forgotten, the newest kept */
	r3d_missing_dirs_flush();
	for( int i = 0; i < R3D_MISSING_DIRS_MAX + 5; i++ )
	{
		snprintf( p, sizeof( p ), "E:\\d%d", i );
		r3d_missing_dirs_note_dir( p );
	}
	snprintf( p, sizeof( p ), "E:\\d%d\\x", R3D_MISSING_DIRS_MAX + 4 );
	CHECK( r3d_missing_dirs_covers( p ), "the newest entry was lost when the table filled" );

	/* full by characters */
	r3d_missing_dirs_flush();
	for( int i = 0; i < 40; i++ )
	{
		char name[260];
		memset( name, 'q', 200 );
		snprintf( name + 200, 60, "%d", i );
		snprintf( p, sizeof( p ), "E:\\%s", name );
		r3d_missing_dirs_note_dir( p );
		snprintf( p + strlen( p ), sizeof( p ) - strlen( p ), "\\x" );
		CHECK( r3d_missing_dirs_covers( p ), "entry %d lost when the characters ran out", i );
	}

	/* an entry under one already recorded adds nothing */
	r3d_missing_dirs_flush();
	r3d_missing_dirs_note_dir( "E:\\top" );
	const uint32_t before = r3d_missing_dirs_stats.recorded;
	r3d_missing_dirs_note_dir( "E:\\top\\sub" );
	r3d_missing_dirs_note_dir( "E:\\top\\sub\\x" );
	CHECK( r3d_missing_dirs_stats.recorded == before, "an entry under a recorded one was added" );
	r3d_missing_dirs_flush();
	CHECK( !r3d_missing_dirs_covers( "E:\\top\\x" ), "flush kept an entry" );
}

int main( void )
{
	table_edges();
	for( int s = 0; s < 3000 && failures <= 20; s++ )
	{
		leaf_pnf_on_d = s & 1;
		run_sequence( 200 );
	}

	CHECK( answered_stats > 1000 && answered_scans > 100, "the table answered too little to test (%ld stats, %ld scans)", answered_stats, answered_scans );
	printf( "xbox_missing_dirs: %ld checks, %d failures (%ld stats and %ld scans answered from the table; the kernel model failed %ld stats with PATH_NOT_FOUND)\n",
		checks, failures, answered_stats, answered_scans, kernel_pnf );
	return failures ? 1 : 0;
}
