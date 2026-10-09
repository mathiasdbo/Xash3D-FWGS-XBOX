/* Host test for compat/xbox/xbox_dir_cache.c (docs/r3d/divergences.md
   #166): a directory listing replayed from the cache must be exactly what
   a scan of the directory would return at that moment - the same names,
   in the same order, with the same types - through any sequence of scans
   and changes to the file system.

   A model of the directories stands in for the kernel: each directory is
   an ordered list of (name, is_dir); a scan returns it in order, and a
   change (create, delete, rename) edits it. The calls are wrapped as
   compat/xbox/xbox_posix_io.c wraps them:
   - opendir: a kept listing of the same pattern is replayed (copied);
     otherwise the directory is scanned and, if cacheable, recorded entry
     by entry as readdir returns them;
   - closedir: a recording is kept only if readdir reached the end and no
     change came between opendir and closedir;
   - every change bumps the change count and empties the cache first.
   Scans are read to the end or stopped early, and changes happen while a
   scan is open, between scans and between a scan and its close. Also
   checked: which patterns are cacheable, a listing too big to keep, the
   cache running out of slots and of bytes.

   Built and run by tools/host-tests/run.sh:
     gcc -O2 -Wall -std=c99 -Icompat/xbox tools/host-tests/xbox_dir_cache_host_test.c \
       compat/xbox/xbox_dir_cache.c -o xbox_dir_cache_test */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_dir_cache.h"

static int failures;
static long checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { failures++; \
	if( failures <= 20 ) { printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); }}} while( 0 )

/* ---- the model ---- */

#define NDIRS 6
#define MAXE  400
static const char *const dir_patterns[NDIRS] = { "D:\\valve\\*", "D:\\*", "D:\\valve\\sound\\*", "E:\\save\\*", "D:\\a\\b\\*", "D:\\big\\*" };
static struct { char name[48]; int is_dir; } entries[NDIRS][MAXE];
static int nentries[NDIRS];

static uint32_t rng = 0x2545f491u;
static uint32_t rnd( void ) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void model_reset( void )
{
	for( int d = 0; d < NDIRS; d++ )
	{
		nentries[d] = d == 5 ? 380 : (int)( rnd() % 40 ) + 1; /* "big" does not fit the cache */
		for( int i = 0; i < nentries[d]; i++ )
		{
			snprintf( entries[d][i].name, sizeof( entries[d][i].name ), d == 5 ? "a_rather_long_file_name_%d.wav" : "f%u", d == 5 ? i : rnd() % 1000 );
			entries[d][i].is_dir = (int)( rnd() % 4 == 0 );
		}
	}
}

/* ---- the wrapped calls ---- */

static uint32_t fs_changes;

static void fs_changed( void )
{
	fs_changes++;
	r3d_dir_cache_flush();
}

typedef struct
{
	int d;                 /* the model directory scanned */
	int pos;               /* next entry of a real scan */
	char *replay;          /* a replayed listing */
	size_t replay_pos, replay_bytes;
	char rec[R3D_DIR_CACHE_BYTES];
	size_t rec_len;
	int rec_ok, complete;
	uint32_t changes;
} wdir_t;

static void w_opendir( wdir_t *w, int d )
{
	r3d_dir_listing_t kept;

	memset( w, 0, sizeof( *w ));
	w->d = d;
	if( r3d_dir_cache_find( dir_patterns[d], &kept ))
	{
		w->replay = malloc( kept.bytes ? kept.bytes : 1 );
		memcpy( w->replay, kept.data, kept.bytes );
		w->replay_bytes = kept.bytes;
		return;
	}
	if( r3d_dir_cache_cacheable( dir_patterns[d] ))
	{
		w->rec_ok = 1;
		w->changes = fs_changes;
	}
}

/* 1 and the entry, or 0 at the end */
static int w_readdir( wdir_t *w, const char **name, int *is_dir )
{
	if( w->replay )
	{
		if( w->replay_pos >= w->replay_bytes )
			return 0;
		*is_dir = w->replay[w->replay_pos];
		*name = w->replay + w->replay_pos + 1;
		w->replay_pos += strlen( *name ) + 2;
		return 1;
	}
	if( w->pos >= nentries[w->d] )
	{
		w->complete = 1;
		return 0;
	}
	*name = entries[w->d][w->pos].name;
	*is_dir = entries[w->d][w->pos].is_dir;
	w->pos++;
	if( w->rec_ok )
	{
		const size_t n = strlen( *name ) + 2;
		if( w->rec_len + n > sizeof( w->rec ))
			w->rec_ok = 0;
		else
		{
			w->rec[w->rec_len] = (char)*is_dir;
			memcpy( w->rec + w->rec_len + 1, *name, n - 1 );
			w->rec_len += n;
		}
	}
	return 1;
}

static void w_closedir( wdir_t *w )
{
	if( w->replay )
	{
		free( w->replay );
		return;
	}
	if( w->rec_ok && w->complete && w->changes == fs_changes )
	{
		int count = 0;
		for( size_t at = 0; at < w->rec_len; at += strlen( w->rec + at + 1 ) + 2 )
			count++;
		r3d_dir_cache_store( dir_patterns[w->d], w->rec, w->rec_len, count );
	}
}

static void change( void )
{
	const int d = (int)( rnd() % NDIRS );

	fs_changed();
	switch( rnd() % 3 )
	{
	case 0: /* create */
		if( nentries[d] < MAXE )
		{
			const int at = (int)( rnd() % ( nentries[d] + 1 ));
			memmove( &entries[d][at + 1], &entries[d][at], ( nentries[d] - at ) * sizeof( entries[d][0] ));
			snprintf( entries[d][at].name, sizeof( entries[d][at].name ), "new%u", rnd() % 1000 );
			entries[d][at].is_dir = (int)( rnd() % 2 );
			nentries[d]++;
		}
		break;
	case 1: /* delete */
		if( nentries[d] > 1 )
		{
			const int at = (int)( rnd() % nentries[d] );
			memmove( &entries[d][at], &entries[d][at + 1], ( nentries[d] - at - 1 ) * sizeof( entries[d][0] ));
			nentries[d]--;
		}
		break;
	default: /* rename */
		snprintf( entries[d][rnd() % nentries[d]].name, sizeof( entries[d][0].name ), "ren%u", rnd() % 1000 );
		break;
	}
}

static long replayed_scans, real_scans;

static void run_sequence( int ops )
{
	model_reset();
	r3d_dir_cache_flush();
	for( int o = 0; o < ops; o++ )
	{
		const uint32_t what = rnd() % 10;

		if( what < 7 )
		{
			wdir_t w;
			const int d = (int)( rnd() % NDIRS );
			const int stop_after = rnd() % 5 == 0 ? (int)( rnd() % 5 ) : 1 << 30;
			const int change_at = rnd() % 8 == 0 ? (int)( rnd() % 6 ) : -1;
			const char *name;
			int is_dir, i = 0;

			/* what a real scan returns now, if nothing changes during it */
			const int expect_n = nentries[d];
			char expect[MAXE][48];
			int expect_dir[MAXE];
			for( int k = 0; k < expect_n; k++ ) { strcpy( expect[k], entries[d][k].name ); expect_dir[k] = entries[d][k].is_dir; }

			w_opendir( &w, d );
			if( w.replay ) replayed_scans++; else real_scans++;
			const int replayed = w.replay != NULL;
			int changed_during = 0;
			while( i < stop_after && w_readdir( &w, &name, &is_dir ))
			{
				if( !changed_during )
				{
					CHECK( i < expect_n && !strcmp( name, expect[i] ) && is_dir == expect_dir[i],
						"%s entry %d: got %s/%d, the directory holds %s/%d (%s)", dir_patterns[d], i, name, is_dir,
						i < expect_n ? expect[i] : "(end)", i < expect_n ? expect_dir[i] : -1, replayed ? "replayed" : "scanned" );
				}
				i++;
				if( i == change_at ) { change(); changed_during = 1; }
			}
			if( !changed_during && i < stop_after )
				CHECK( i == expect_n, "%s: %d entries, the directory holds %d (%s)", dir_patterns[d], i, expect_n, replayed ? "replayed" : "scanned" );
			if( rnd() % 10 == 0 ) change(); /* between the end of a scan and its close */
			w_closedir( &w );
		}
		else
			change();
	}
}

static void patterns( void )
{
	static const struct { const char *p; int ok; } cases[] =
	{
		{ "D:\\*", 1 }, { "D:\\valve\\*", 1 }, { "D:\\valve\\sound\\*", 1 },
		{ "E:\\valve\\*", 0 }, { "d:\\valve\\*", 0 }, { "D:valve\\*", 0 }, { "D:\\valve", 0 },
		{ "D:\\valve\\*.wav", 0 }, { "D:\\valve\\\\*", 0 }, { "D:\\\\*", 0 }, { "D:\\.\\*", 0 },
		{ "D:\\valve\\..\\*", 0 }, { "D:\\va?ve\\*", 0 }, { "D:\\v*\\*", 0 }, { "D:\\valve/x\\*", 0 }, { "D:*", 0 },
	};
	for( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
		CHECK( r3d_dir_cache_cacheable( cases[i].p ) == cases[i].ok, "cacheable( %s ) should be %d", cases[i].p, cases[i].ok );

	/* out of slots and of bytes: the newest listing stays */
	char pattern[64], data[2000];
	r3d_dir_listing_t got;
	memset( data, 'x', sizeof( data ));
	r3d_dir_cache_flush();
	for( int i = 0; i < R3D_DIR_CACHE_DIRS * 3; i++ )
	{
		snprintf( pattern, sizeof( pattern ), "D:\\d%d\\*", i );
		data[0] = 0; data[sizeof( data ) - 1] = 0;
		r3d_dir_cache_store( pattern, data, sizeof( data ), 1 );
		CHECK( r3d_dir_cache_find( pattern, &got ) && got.bytes == sizeof( data ), "listing %d lost when the cache filled", i );
	}
	r3d_dir_cache_flush();
	CHECK( !r3d_dir_cache_find( pattern, &got ), "flush kept a listing" );
}

int main( void )
{
	patterns();
	for( int s = 0; s < 2000 && failures <= 20; s++ )
		run_sequence( 150 );
	CHECK( replayed_scans > 10000, "too few replays to test (%ld)", replayed_scans );
	printf( "xbox_dir_cache: %ld checks, %d failures (%ld scans replayed, %ld scanned)\n", checks, failures, replayed_scans, real_scans );
	return failures ? 1 : 0;
}
