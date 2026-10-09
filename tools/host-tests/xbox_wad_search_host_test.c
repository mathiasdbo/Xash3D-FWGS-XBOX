/* Host test for compat/xbox/xbox_wad_search.c (docs/r3d/divergences.md
   #167): FS_Search_WAD's loop, run over only the run of lumps
   R3D_WadSearchRange returns, must append exactly the strings, in exactly
   the order, that the reference loop over the whole table appends.

   The WAD side is copied from the reference (filesystem/wad.c in
   G:\Proyectos\Ferrum56\xash3d-fwgs): W_Open's name clean-up and sort
   (:366-383: Q_strnlwr, the last '*' turned into '!', the conchars type
   fix-up, W_AddFileToWad's binary insertion, :184-220) and FS_Search_WAD's
   loop body (:510-557) with W_ExtFromType (:119-131). matchpattern,
   Q_stricmp, Q_strnlwr, Q_snprintf and COM_DefaultExtension are the real
   ones from public/crtlib.c, as the Xbox build links them.

   Checked:
   - random WADs (names from a small pool so the same name recurs with the
     same and with other types, in other case, with '{', '!', '*', '?',
     '-', bytes from 0x80 up, and in some WADs '/', '\\' or ':'; never a
     16-byte name without its NUL, which overruns W_Open's own name[16]
     in Q_strnlwr upstream and none of the retail WADs has), several mounted in a random
     order and searched into one shared list (as FS_Search does), the list
     sometimes seeded with strings, including bare lump names the
     reference's de-duplication compares against;
   - patterns taken from the WAD in other case, absent ones, empty ones,
     longer than a lump name, with '*' or '?' (the whole table), and every
     lump type plus TYP_ANY;
   - the retail WADs, when a directory holding them is given (run.sh passes
     .sweep-xiso/hl/valve if present): every lump name in three spellings,
     with TYP_ANY and its own type, over all of them mounted in order.
   Also counted: how many lumps the narrowed loop visits per lookup.

   Built and run by tools/host-tests/run.sh:
     gcc -O2 -Wall -std=c99 -Ipublic -Icommon -I3rdparty/library_suffix/include -iquote compat/xbox \
       tools/host-tests/xbox_wad_search_host_test.c compat/xbox/xbox_wad_search.c public/crtlib.c \
       -o xbox_wad_search_test && ./xbox_wad_search_test [directory of .wad files] */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crtlib.h"
#include "wadfile.h"
#include "xbox_wad_search.h"

static int failures;
static long checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { failures++; \
	if( failures <= 20 ) { printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); }}} while( 0 )

/* ---- the reference's WAD side ---- */

typedef struct
{
	dlumpinfo_t *lumps;
	int numlumps;
	int separator_names; /* what W_Open records under divergence #167 */
	char folder[32];     /* FS_Search_WAD's wadfolder: "" or the WAD's base name */
} twad_t;

static const struct { char ext[4]; signed char type; } wad_types[] =
{
	{ "pal", TYP_PALETTE }, { "dds", TYP_DDSTEX }, { "lmp", TYP_GFXPIC },
	{ "fnt", TYP_QFONT }, { "mip", TYP_MIPTEX }, { "txt", TYP_SCRIPT },
};

/* W_ExtFromType, wad.c:119-131 */
static const char *W_ExtFromType( signed char lumptype )
{
	if( lumptype == TYP_NONE || lumptype == TYP_ANY )
		return "";

	for( int i = 0; i < (int)( sizeof( wad_types ) / sizeof( wad_types[0] )); i++ )
	{
		if( lumptype == wad_types[i].type )
			return wad_types[i].ext;
	}

	return "";
}

/* W_AddFileToWad, wad.c:184-220 (the duplicate warning left out) */
static void W_AddFileToWad( const char *name, twad_t *wad, dlumpinfo_t *newlump )
{
	dlumpinfo_t *plump;
	int left = 0;
	int right = wad->numlumps - 1;

	while( left <= right )
	{
		int middle = ( left + right ) / 2;
		int diff = Q_stricmp( wad->lumps[middle].name, name );

		if( !diff )
		{
			if( wad->lumps[middle].type < newlump->type )
				diff = 1;
			else if( wad->lumps[middle].type > newlump->type )
				diff = -1;
		}

		if( diff > 0 ) right = middle - 1;
		else left = middle + 1;
	}

	plump = &wad->lumps[left];
	memmove( plump + 1, plump, ( wad->numlumps - left ) * sizeof( *plump ));
	wad->numlumps++;

	*plump = *newlump;
	memcpy( plump->name, name, sizeof( plump->name ));
}

/* W_Open's sort loop, wad.c:366-383, over `count` source lumps */
static void build_wad( twad_t *wad, dlumpinfo_t *srclumps, int count )
{
	wad->lumps = (dlumpinfo_t *)calloc( count > 0 ? count : 1, sizeof( dlumpinfo_t ));
	wad->numlumps = 0;

	for( int i = 0; i < count; i++ )
	{
		char name[16];
		int k;

		Q_strnlwr( srclumps[i].name, name, sizeof( srclumps[i].name ));

		k = Q_strlen( Q_strrchr( name, '*' ));
		if( k ) name[strlen( name ) - k] = '!'; /* Q_strlen in the reference; name is never NULL */

		if( srclumps[i].type == 68 && !Q_stricmp( srclumps[i].name, "conchars" ))
			srclumps[i].type = TYP_GFXPIC;

		W_AddFileToWad( name, wad, &srclumps[i] );
	}

	wad->separator_names = R3D_WadHasSeparatorNames( wad->lumps, wad->numlumps );
}

/* ---- FS_Search's shared result list ---- */

typedef struct
{
	char **s;
	int n, cap;
} tlist_t;

static void list_append( tlist_t *l, const char *s )
{
	if( l->n == l->cap )
	{
		l->cap = l->cap ? l->cap * 2 : 16;
		l->s = (char **)realloc( l->s, l->cap * sizeof( *l->s ));
	}
	l->s[l->n] = (char *)malloc( strlen( s ) + 1 );
	strcpy( l->s[l->n++], s );
}

static void list_free( tlist_t *l )
{
	for( int i = 0; i < l->n; i++ )
		free( l->s[i] );
	free( l->s );
	memset( l, 0, sizeof( *l ));
}

static long visited_narrow, visited_full, lookups_narrowable;

/* FS_Search_WAD's loop, wad.c:510-557, over [first, end) */
static void search_wad( const twad_t *wad, tlist_t *list, const char *wadpattern, signed char type, int narrowed )
{
	char temp[MAX_STRING], temp2[MAX_STRING], buf[MAX_VA_STRING];
	int first = 0, end = wad->numlumps;

	if( narrowed )
		R3D_WadSearchRange( wad->lumps, wad->numlumps, wadpattern, wad->separator_names, &first, &end );

	if( narrowed )
		visited_narrow += end - first;
	else visited_full += end - first;

	for( int i = first; i < end; i++ )
	{
		if( type != TYP_ANY && wad->lumps[i].type != type )
			continue;

		Q_strncpy( temp, wad->lumps[i].name, sizeof( temp ));

		while( temp[0] )
		{
			const char *slash, *backslash, *colon, *separator;

			if( matchpattern( temp, wadpattern, true ))
			{
				int j;

				for( j = 0; j < list->n; j++ )
				{
					if( !Q_strcmp( list->s[j], temp ))
						break;
				}

				if( j == list->n )
				{
					Q_snprintf( temp2, sizeof( temp2 ), "%s/%s", wad->folder, temp );
					Q_snprintf( buf, sizeof( buf ), ".%s", W_ExtFromType( wad->lumps[i].type ));
					COM_DefaultExtension( temp2, buf, sizeof( temp2 ));
					list_append( list, temp2 );
				}
			}

			slash = Q_strrchr( temp, '/' );
			backslash = Q_strrchr( temp, '\\' );
			colon = Q_strrchr( temp, ':' );
			separator = temp;
			if( separator < slash )
				separator = slash;
			if( separator < backslash )
				separator = backslash;
			if( separator < colon )
				separator = colon;
			*((char *)separator) = 0;
		}
	}
}

/* One FS_Search over `count` mounted WADs in `order`, both ways, from the
   same seeded list; the lists must be equal string for string. */
static void compare_search( twad_t *wads, const int *order, int count, const char *wadpattern, signed char type,
	const tlist_t *seed, const char *what )
{
	tlist_t ref = { 0 }, ours = { 0 };

	for( int i = 0; i < seed->n; i++ )
	{
		list_append( &ref, seed->s[i] );
		list_append( &ours, seed->s[i] );
	}

	for( int i = 0; i < count; i++ )
	{
		search_wad( &wads[order[i]], &ref, wadpattern, type, 0 );
		search_wad( &wads[order[i]], &ours, wadpattern, type, 1 );
	}

	CHECK( ref.n == ours.n, "%s: pattern \"%s\" type %d: %d strings, narrowed %d", what, wadpattern, type, ref.n, ours.n );
	for( int i = 0; i < ref.n && i < ours.n; i++ )
		CHECK( !strcmp( ref.s[i], ours.s[i] ), "%s: pattern \"%s\" type %d: [%d] \"%s\" vs \"%s\"", what, wadpattern, type,
			i, ref.s[i], ours.s[i] );

	if( !strpbrk( wadpattern, "*?" ))
		lookups_narrowable++;

	list_free( &ref );
	list_free( &ours );
}

/* ---- random WADs ---- */

static uint32_t rng_state = 0x167u;

static uint32_t rnd( void )
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static int rnd_n( int n )
{
	return (int)( rnd() % (uint32_t)n );
}

static const signed char lump_types[] = { TYP_MIPTEX, TYP_MIPTEX, TYP_MIPTEX, TYP_GFXPIC, TYP_PALETTE, TYP_QFONT,
	TYP_SCRIPT, TYP_DDSTEX, TYP_LABEL, TYP_NONE, TYP_COLORMAP2, 68 };
static const signed char search_types[] = { TYP_ANY, TYP_ANY, TYP_ANY, TYP_MIPTEX, TYP_GFXPIC, TYP_PALETTE, TYP_QFONT,
	TYP_SCRIPT, TYP_DDSTEX, TYP_LABEL, TYP_NONE };

#define POOL 48
static char pool[POOL][16];

static void random_name( char *out, int maxlen, int with_separators )
{
	/* bytes from 0x80 up too (a translated mod's WAD): neither Q_tolower
	   nor matchpattern folds them */
	static const char plain[] = "abcxyzABCXYZ{{!!_-0189\xc9\xe9\x80\xff";
	static const char special[] = "*?/\\:";
	int len = rnd_n( maxlen + 1 );

	for( int i = 0; i < len; i++ )
	{
		if( rnd_n( 12 ) == 0 )
		{
			char c = special[rnd_n( 5 )];

			if( !with_separators && ( c == '/' || c == '\\' || c == ':' ))
				c = '*';
			out[i] = c;
		}
		else out[i] = plain[rnd_n( (int)sizeof( plain ) - 1 )];
	}
	out[len] = 0;
}

/* the same name in random case */
static void recase( char *s )
{
	for( ; *s; s++ )
	{
		if( rnd_n( 2 ))
		{
			if( *s >= 'a' && *s <= 'z' ) *s += 'A' - 'a';
			else if( *s >= 'A' && *s <= 'Z' ) *s += 'a' - 'A';
		}
	}
}

static void random_pattern( char *out, size_t size, const twad_t *wads, int count )
{
	const int kind = rnd_n( 10 );

	if( kind < 5 && count )
	{
		/* a name one of the WADs holds, in random case */
		const twad_t *w = &wads[rnd_n( count )];

		if( w->numlumps )
		{
			Q_strncpy( out, w->lumps[rnd_n( w->numlumps )].name, size );
			recase( out );
			return;
		}
	}
	if( kind < 7 )
	{
		Q_strncpy( out, pool[rnd_n( POOL )], size );
		recase( out );
		return;
	}
	if( kind == 7 )
	{
		/* longer than any lump name can be */
		Q_snprintf( out, size, "%s%s", pool[rnd_n( POOL )], "abcdefghijklmnopq" );
		return;
	}
	if( kind == 8 )
	{
		out[0] = 0;
		return;
	}
	{
		char tmp[16];

		random_name( tmp, 15, 0 );
		Q_strncpy( out, tmp, size );
	}
}

static void random_round( void )
{
	twad_t wads[5];
	int order[5];
	const int count = 1 + rnd_n( 5 );
	tlist_t seed = { 0 };

	for( int w = 0; w < count; w++ )
	{
		const int n = rnd_n( 80 );
		const int seps = rnd_n( 4 ) == 0;
		dlumpinfo_t *src = (dlumpinfo_t *)calloc( n > 0 ? n : 1, sizeof( *src ));

		for( int i = 0; i < n; i++ )
		{
			if( rnd_n( 4 ))
			{
				Q_strncpy( src[i].name, pool[rnd_n( POOL )], sizeof( src[i].name ));
				if( rnd_n( 3 ) == 0 )
					recase( src[i].name );
			}
			else random_name( src[i].name, 15, seps );
			if( seps && rnd_n( 6 ) == 0 )
			{
				/* a pool name behind a folder, so its prefix can match */
				Q_snprintf( src[i].name, sizeof( src[i].name ), "%.5s%c%.8s", pool[rnd_n( POOL )], "/\\:"[rnd_n( 3 )],
					pool[rnd_n( POOL )] );
			}
			if( rnd_n( 30 ) == 0 )
				Q_strncpy( src[i].name, "CONCHARS", sizeof( src[i].name ));
			src[i].type = lump_types[rnd_n( (int)sizeof( lump_types ))];
		}
		build_wad( &wads[w], src, n );
		free( src );
		Q_snprintf( wads[w].folder, sizeof( wads[w].folder ), "%s", rnd_n( 2 ) ? "" : "decals" );
		order[w] = w;
	}

	/* a random mount order */
	for( int i = count - 1; i > 0; i-- )
	{
		const int j = rnd_n( i + 1 );
		const int t = order[i];

		order[i] = order[j];
		order[j] = t;
	}

	if( rnd_n( 3 ) == 0 )
	{
		/* strings already in the list: other paths, and bare lump names,
		   which the reference's de-duplication compares against */
		const int n = rnd_n( 6 );

		for( int i = 0; i < n; i++ )
		{
			const twad_t *w = &wads[rnd_n( count )];

			if( rnd_n( 2 ) && w->numlumps )
				list_append( &seed, w->lumps[rnd_n( w->numlumps )].name );
			else list_append( &seed, pool[rnd_n( POOL )] );
		}
	}

	for( int q = 0; q < 40; q++ )
	{
		char pattern[64];

		random_pattern( pattern, sizeof( pattern ), wads, count );
		compare_search( wads, order, count, pattern, search_types[rnd_n( (int)sizeof( search_types ))], &seed, "random" );
	}

	list_free( &seed );
	for( int w = 0; w < count; w++ )
		free( wads[w].lumps );
}

/* ---- hand-written cases ---- */

static void fixed_cases( void )
{
	static const char *const names[] =
	{
		"{shot1", "{SHOT1", "{shot1", "{shot2", "{shot10", "{shot", "c1a0_w1", "C1A0_W1", "a*b*c", "q?x",
		"", "conchars", "aaa", "aab", "{", "z",
	};
	static const signed char types[] =
	{
		TYP_MIPTEX, TYP_MIPTEX, TYP_GFXPIC, TYP_MIPTEX, TYP_MIPTEX, TYP_MIPTEX, TYP_MIPTEX, TYP_GFXPIC, TYP_MIPTEX,
		TYP_MIPTEX, TYP_MIPTEX, 68, TYP_PALETTE, TYP_MIPTEX, TYP_MIPTEX, TYP_QFONT,
	};
	static const char *const patterns[] =
	{
		"{shot1", "{Shot1", "{SHOT1", "{shot", "{shot10", "{shot1x", "c1a0_w1", "a!b*c", "a*b!c", "a!b!c", "q?x",
		"", "conchars", "{*", "*", "?", "aa?", "zz", "{", "Z", "aaa", "AAB", "aaaa",
	};
	const int nn = (int)( sizeof( names ) / sizeof( names[0] ));
	dlumpinfo_t src[16];
	twad_t wads[2];
	int order[2] = { 0, 1 };
	tlist_t seed = { 0 };

	for( int i = 0; i < nn; i++ )
	{
		memset( &src[i], 0, sizeof( src[i] ));
		Q_strncpy( src[i].name, names[i], sizeof( src[i].name ));
		src[i].type = types[i];
	}
	build_wad( &wads[0], src, nn );
	Q_strncpy( wads[0].folder, "", sizeof( wads[0].folder ));
	CHECK( !wads[0].separator_names, "fixed WAD has no separator names" );

	/* the same names behind a folder in a second WAD */
	for( int i = 0; i < nn; i++ )
	{
		memset( &src[i], 0, sizeof( src[i] ));
		Q_snprintf( src[i].name, sizeof( src[i].name ), "%s%s", i % 3 ? "d/" : "", names[i] );
		src[i].type = types[i];
	}
	build_wad( &wads[1], src, nn );
	Q_strncpy( wads[1].folder, "decals", sizeof( wads[1].folder ));
	CHECK( wads[1].separator_names, "folder WAD has separator names" );

	for( int p = 0; p < (int)( sizeof( patterns ) / sizeof( patterns[0] )); p++ )
	{
		for( int t = 0; t < (int)sizeof( search_types ); t++ )
		{
			compare_search( wads, order, 1, patterns[p], search_types[t], &seed, "fixed" );
			compare_search( wads, order, 2, patterns[p], search_types[t], &seed, "fixed+folder" );
		}
	}

	/* the narrowed run of "{shot1" is exactly its three spellings */
	{
		int first, end;

		R3D_WadSearchRange( wads[0].lumps, wads[0].numlumps, "{SHOT1", 0, &first, &end );
		CHECK( end - first == 3, "{SHOT1 run is %d lumps", end - first );
		R3D_WadSearchRange( wads[0].lumps, wads[0].numlumps, "{shot1", 1, &first, &end );
		CHECK( first == 0 && end == wads[0].numlumps, "separator names give the whole table" );
		R3D_WadSearchRange( wads[0].lumps, wads[0].numlumps, "{sh?t1", 0, &first, &end );
		CHECK( first == 0 && end == wads[0].numlumps, "a wildcard gives the whole table" );
		R3D_WadSearchRange( wads[0].lumps, 0, "{shot1", 0, &first, &end );
		CHECK( first == 0 && end == 0, "an empty table gives an empty run" );
	}

	free( wads[0].lumps );
	free( wads[1].lumps );
}

/* ---- the retail WADs ---- */

static int load_wad_file( const char *path, twad_t *wad )
{
	FILE *f = fopen( path, "rb" );
	dwadinfo_t header;
	dlumpinfo_t *src;

	if( !f )
		return 0;
	if( fread( &header, sizeof( header ), 1, f ) != 1 || header.numlumps <= 0 || header.numlumps > 65535
		|| fseek( f, header.infotableofs, SEEK_SET ))
	{
		fclose( f );
		return 0;
	}
	src = (dlumpinfo_t *)calloc( header.numlumps, sizeof( *src ));
	if( fread( src, sizeof( *src ), header.numlumps, f ) != (size_t)header.numlumps )
	{
		free( src );
		fclose( f );
		return 0;
	}
	fclose( f );
	for( int i = 0; i < header.numlumps; i++ )
		src[i].name[sizeof( src[i].name ) - 1] = 0; /* W_Open's Q_strnlwr keeps 15 characters */
	build_wad( wad, src, header.numlumps );
	free( src );
	wad->folder[0] = 0;
	return 1;
}

static void retail_wads( const char *dir )
{
	static const char *const files[] =
	{
		"cached.wad", "decals.wad", "fonts.wad", "gfx.wad", "halflife.wad", "liquids.wad", "spraypaint.wad",
		"tempdecal.wad", "xeno.wad",
	};
	twad_t wads[9];
	int order[9];
	int count = 0;
	long lumps = 0, seps = 0;
	const long before_n = visited_narrow, before_f = visited_full, before_l = lookups_narrowable;
	tlist_t seed = { 0 };

	for( int i = 0; i < 9; i++ )
	{
		char path[1024];

		snprintf( path, sizeof( path ), "%s/%s", dir, files[i] );
		if( load_wad_file( path, &wads[count] ))
		{
			lumps += wads[count].numlumps;
			seps += wads[count].separator_names;
			order[count] = count;
			count++;
		}
	}
	if( !count )
	{
		printf( "retail WADs: none found in %s, skipped\n", dir );
		return;
	}

	for( int w = 0; w < count; w++ )
	{
		for( int i = 0; i < wads[w].numlumps; i++ )
		{
			char pattern[32];

			Q_strncpy( pattern, wads[w].lumps[i].name, sizeof( pattern ));
			compare_search( wads, order, count, pattern, TYP_ANY, &seed, "retail" );
			compare_search( wads, order, count, pattern, wads[w].lumps[i].type, &seed, "retail" );
			for( char *c = pattern; *c; c++ )
				if( *c >= 'a' && *c <= 'z' ) *c += 'A' - 'a';
			compare_search( wads, order, count, pattern, TYP_ANY, &seed, "retail" );
			recase( pattern );
			compare_search( wads, order, count, pattern, TYP_ANY, &seed, "retail" );
		}
	}
	printf( "retail WADs: %d files, %ld lumps, %ld with separator names; %ld lookups visited %.2f lumps each "
		"instead of %.0f\n", count, lumps, seps, lookups_narrowable - before_l,
		(double)( visited_narrow - before_n ) / (double)( lookups_narrowable - before_l ),
		(double)( visited_full - before_f ) / (double)( lookups_narrowable - before_l ));

	for( int w = 0; w < count; w++ )
		free( wads[w].lumps );
}

int main( int argc, char **argv )
{
	for( int i = 0; i < POOL; i++ )
		random_name( pool[i], 1 + rnd_n( 8 ), 0 );

	fixed_cases();
	for( int r = 0; r < 4000; r++ )
		random_round();
	if( argc > 1 )
		retail_wads( argv[1] );

	printf( "xbox_wad_search: %ld checks, %d failures\n", checks, failures );
	return failures ? 1 : 0;
}
