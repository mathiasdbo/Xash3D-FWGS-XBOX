/* Host test for engine/platform/xbox/xbox_bmodel_origin.c
   (docs/r3d/divergences.md #163): every submodel's origin from one parse of
   the entity lump (Xbox_BmodelOrigins_Build + _Find) against the
   reference Mod_FindModelOrigin (engine/common/mod_bmodel.c, :1334-1384 in
   the reference), copied below with Host_Error turned into a longjmp.

   For each lump, every submodel 1 .. count-1 is asked with a null origin
   (0 and -0) and with non-null ones (the BSP's own dmodel_t origin, which
   the reference leaves alone), and the two vectors must be bit-identical.
   When the build accepts a lump, the reference must not Host_Error for any
   submodel; when it refuses one, the reference must Host_Error for a name
   no entity carries (it parses to the end), so the fallback is only taken
   on lumps the reference itself rejects somewhere.

   Lumps:
   1. hand-written cases: no entities, an empty string, a submodel named
      twice, two "model" keys in one entity, "origin" before and after
      "model", several "origin" keys, short and long origin strings, a name
      in another case ("MODEL", "Origin"), "*01", "*+1", "* 1", "*1x", "*0",
      a submodel out of range, comments, quoted braces, a key longer than
      the reference's keyname buffer, and each Host_Error the reference has
      (a token other than '{' where an entity starts, EOF inside an entity,
      '}' as a value), placed before and after the entities that name
      submodels;
   2. random lumps built from those pieces;
   3. optionally, real maps: each argument is a BSP or MPAK file (an MPAK
      starts with the BSP header); its entity lump and model count are read
      and checked the same way.

   Built and run by tools/host-tests/run.sh:
     gcc -O2 -Wall -std=c99 -Ipublic -Icommon -I3rdparty/library_suffix/include \
       -Iengine/platform/xbox tools/host-tests/xbox_bmodel_origin_host_test.c \
       engine/platform/xbox/xbox_bmodel_origin.c public/crtlib.c \
       -o xbox_bmodel_origin_test */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crtlib.h"
#include "xbox_bmodel_origin.h"

static int failures;
static long checks;
static jmp_buf host_error_jump;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { failures++; \
	if( failures <= 20 ) { printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); }}} while( 0 )

#define Host_Error( ... ) longjmp( host_error_jump, 1 )
#define VectorIsNull( v ) (( v )[0] == 0.0f && ( v )[1] == 0.0f && ( v )[2] == 0.0f )
#define VectorClear( x ) (( x )[0] = ( x )[1] = ( x )[2] = 0 )

/* The reference, mod_bmodel.c:1334-1384, verbatim but for the macros above. */
static void Mod_FindModelOrigin( const char *entities, const char *modelname, vec3_t origin )
{
	if( !entities || COM_StringEmptyOrNULL( modelname ))
		return;

	if( !origin || !VectorIsNull( origin ))
		return;

	char	*pfile = (char *)entities;
	string	keyname;
	char	token[2048];
	qboolean	model_found;
	qboolean	origin_found;

	while(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) != NULL )
	{
		if( token[0] != '{' )
			Host_Error( "%s: found %s when expecting {\n", __func__, token );

		model_found = origin_found = false;
		VectorClear( origin );

		while( 1 )
		{
			// parse key
			if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
				Host_Error( "%s: EOF without closing brace\n", __func__ );
			if( token[0] == '}' ) break; // end of desc

			Q_strncpy( keyname, token, sizeof( keyname ));

			// parse value
			if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
				Host_Error( "%s: EOF without closing brace\n", __func__ );

			if( token[0] == '}' )
				Host_Error( "%s: closing brace without data\n", __func__ );

			if( !Q_stricmp( keyname, "model" ) && !Q_stricmp( modelname, token ))
				model_found = true;

			if( !Q_stricmp( keyname, "origin" ))
			{
				Q_atov( origin, token, 3 );
				origin_found = true;
			}
		}

		if( model_found ) break;
	}
	(void)origin_found;
}

/* The reference's answer, or 1 if it Host_Errors. volatile: read after longjmp. */
static int reference( const char *ents, const char *name, vec3_t inout )
{
	volatile int errored = 0;
	vec3_t v;

	memcpy( v, inout, sizeof( v ));
	if( setjmp( host_error_jump ) == 0 )
		Mod_FindModelOrigin( ents, name, v );
	else
		errored = 1;
	memcpy( inout, v, sizeof( v ));
	return errored;
}

static long lumps_accepted, lumps_refused, answers_named, answers_unnamed;

static void check_lump( const char *ents, int count, const char *what )
{
	vec3_t *values = malloc( sizeof( vec3_t ) * ( count > 0 ? count : 1 ));
	int *state = malloc( sizeof( int ) * ( count > 0 ? count : 1 ));
	xbox_bmodel_origins_t idx;
	const qboolean ok = Xbox_BmodelOrigins_Build( &idx, ents, count, values, state );

	if( ok )
	{
		lumps_accepted++;
		for( int i = 1; i < count; i++ )
		{
			char name[16];
			static const vec3_t starts[4] = { { 0.0f, 0.0f, 0.0f }, { -0.0f, 0.0f, -0.0f }, { 16.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 16.0f }};

			Q_snprintf( name, sizeof( name ), "*%i", i );
			for( int s = 0; s < 4; s++ )
			{
				vec3_t ref, got;

				memcpy( ref, starts[s], sizeof( ref ));
				memcpy( got, starts[s], sizeof( got ));
				const int errored = reference( ents, name, ref );
				Xbox_BmodelOrigins_Find( &idx, i, got );

				CHECK( !errored, "%s: accepted, but the reference errors for %s", what, name );
				CHECK( !memcmp( ref, got, sizeof( ref )), "%s: %s start %d: reference (%g %g %g) index (%g %g %g)",
					what, name, s, ref[0], ref[1], ref[2], got[0], got[1], got[2] );
			}
			if( state[i] == -2 ) answers_named++; else answers_unnamed++; /* -2: ORIGIN_RESOLVED */
		}
	}
	else
	{
		vec3_t v = { 0.0f, 0.0f, 0.0f };

		lumps_refused++;
		CHECK( ents == NULL || reference( ents, "*99999999", v ),
			"%s: refused, but the reference parses it to the end without an error", what );
	}

	free( values );
	free( state );
}

/* 1. Hand-written lumps. */
static const struct { const char *ents; int count; } fixed_cases[] =
{
	{ "", 4 },
	{ "   \n\t ", 4 },
	{ "{ \"classname\" \"worldspawn\" }", 4 },
	{ "{ \"classname\" \"worldspawn\" \"origin\" \"1 2 3\" }", 4 },
	{ "{ \"classname\" \"worldspawn\" }\n{ \"model\" \"*1\" \"origin\" \"10 20 30\" }\n{ \"model\" \"*2\" }", 4 },
	{ "{ \"classname\" \"worldspawn\" }\n{ \"origin\" \"5 6 7\" \"model\" \"*1\" }\n{ \"model\" \"*1\" \"origin\" \"8 8 8\" }", 4 },
	{ "{ \"model\" \"*1\" \"model\" \"*3\" \"origin\" \"-1 -2 -3\" }\n{ \"model\" \"*3\" \"origin\" \"4 4 4\" }", 5 },
	{ "{ \"origin\" \"1 1 1\" \"origin\" \"2\" \"model\" \"*2\" }\n{ \"origin\" \"9 9 9\" }", 4 },
	{ "{ \"origin\" \"1.5 2.25\" \"model\" \"*1\" }{ \"origin\" \"1 2 3 4 5\" \"model\" \"*2\" }{ \"origin\" \"\" \"model\" \"*3\" }", 5 },
	{ "{ \"MODEL\" \"*1\" \"Origin\" \"3 3 3\" }\n{ \"Model\" \"*2\" \"ORIGIN\" \"4 4 4\" }", 4 },
	{ "{ \"model\" \"*01\" \"origin\" \"1 1 1\" }{ \"model\" \"*+1\" \"origin\" \"2 2 2\" }{ \"model\" \"* 1\" }{ \"model\" \"*1x\" }{ \"model\" \"*0\" \"origin\" \"5 5 5\" }{ \"model\" \"*9\" \"origin\" \"6 6 6\" }{ \"model\" \"*\" }{ \"origin\" \"7 7 7\" }", 4 },
	{ "{ \"model\" \"models/a.mdl\" \"origin\" \"1 2 3\" }{ \"model\" \"*2\" }", 4 },
	{ "// a comment\n{ \"model\" \"*1\" // trailing\n \"origin\" \"1 2 3\" }", 4 },
	{ "{ \"message\" \"{ not a brace }\" \"model\" \"*1\" \"origin\" \"4 5 6\" }", 4 },
	{ "{ model *1 origin 7 }{ \"model\" \"*2\" origin ( 1 2 3 ) }", 4 },
	{ "{ \"model\" \"*1\" }{ \"model\" \"*2\" \"origin\" \"1 2 3\" }{ \"origin\" \"5 5 5\" }", 3 },
	{ "{ \"model\" \"*1\" } garbage", 4 },
	{ "garbage { \"model\" \"*1\" }", 4 },
	{ "{ \"model\" \"*1\" }{ \"model\" \"*2\" ", 4 },
	{ "{ \"model\" \"*1\" }{ \"model\" ", 4 },
	{ "{ \"model\" }", 4 },
	{ "{ \"model\" \"*1\" }{ \"target\" }", 4 },
	{ "{ \"model\" \"*1\" \"origin\" \"1 2 3\" }{ \"model\" \"*2\" }{ \"broken\" \"}\" }", 4 },
	{ "{ \"model\" \"*1\" \"origin\" \"1 2 3\" }{ \"model\" \"*2\" }{ \"broken\" } }", 4 },
	{ "{ \"model\" \"*1\" \"origin\" \"1 2 3 }", 4 },
	{ "{ \"model\" \"*1\" }", 1 },
	{ "{ \"model\" \"*1\" }", 0 },
};

/* 2. Random lumps from the same pieces. */
static uint32_t rng = 0x12345678u;
static uint32_t rnd( void )
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

static void append( char **buf, size_t *len, size_t *cap, const char *s )
{
	size_t n = strlen( s );
	if( *len + n + 1 > *cap )
	{
		*cap = ( *len + n + 1 ) * 2;
		*buf = realloc( *buf, *cap );
	}
	memcpy( *buf + *len, s, n + 1 );
	*len += n;
}

static void random_lump( char **buf, size_t *len, size_t *cap, int count, int malformed )
{
	static const char *keys[] = { "\"model\"", "\"origin\"", "\"MODEL\"", "\"Origin\"", "\"classname\"", "\"targetname\"", "\"angles\"", "model", "origin" };
	static const char *origins[] = { "\"1 2 3\"", "\"-16 0 32.5\"", "\"7\"", "\"1 2\"", "\"\"", "\"0 0 0\"", "\"1e3 -2e-2 3\"", "\"  4 5 6\"", "\"x y z\"" };
	static const char *others[] = { "\"func_door\"", "\"{ brace }\"", "\"*\"", "\"models/b.mdl\"", "\"a // b\"", "\"( 1 2 )\"", "word" };
	const int ents = (int)( rnd() % 40 );
	const int bad_at = malformed ? (int)( rnd() % ( ents + 1 )) : -1;
	char tmp[64];

	*len = 0;
	append( buf, len, cap, "" );
	for( int e = 0; e < ents; e++ )
	{
		if( e == bad_at )
		{
			static const char *bad[] = { "oops ", "{ \"k\" \"}\" } ", "{ \"k\" } ", "{ \"k\" \"v\" ", "{ \"k\" " };
			append( buf, len, cap, bad[rnd() % 5] );
			if( rnd() % 2 ) return; // the bad piece last: EOF there
		}
		append( buf, len, cap, rnd() % 7 ? "{\n" : "{ " );
		const int pairs = (int)( rnd() % 6 );
		for( int p = 0; p < pairs; p++ )
		{
			const char *k = keys[rnd() % ( sizeof( keys ) / sizeof( keys[0] ))];
			append( buf, len, cap, k );
			append( buf, len, cap, " " );
			if( strstr( k, "odel" ) || strstr( k, "ODEL" ))
			{
				const uint32_t r = rnd() % 10;
				if( r < 7 ) snprintf( tmp, sizeof( tmp ), "\"*%d\"", (int)( rnd() % ( count + 2 )));
				else if( r == 7 ) snprintf( tmp, sizeof( tmp ), "\"*0%d\"", (int)( rnd() % 9 ));
				else if( r == 8 ) snprintf( tmp, sizeof( tmp ), "*%d", (int)( rnd() % ( count + 1 )));
				else snprintf( tmp, sizeof( tmp ), "\"models/m%d.mdl\"", (int)( rnd() % 3 ));
				append( buf, len, cap, tmp );
			}
			else if( strstr( k, "rigin" ) || strstr( k, "RIGIN" ))
				append( buf, len, cap, origins[rnd() % ( sizeof( origins ) / sizeof( origins[0] ))] );
			else
				append( buf, len, cap, others[rnd() % ( sizeof( others ) / sizeof( others[0] ))] );
			append( buf, len, cap, rnd() % 3 ? "\n" : " " );
		}
		append( buf, len, cap, "}\n" );
	}
	if( bad_at == ents )
		append( buf, len, cap, "{ \"k\" \"v\" " );
}

/* 3. Real maps: the BSP header (version, 15 lumps of offset/length) leads an MPAK too. */
static void check_file( const char *path )
{
	FILE *f = fopen( path, "rb" );
	int32_t head[1 + 15 * 2];
	char *ents;

	if( !f || fread( head, sizeof( head ), 1, f ) != 1 )
	{
		printf( "FAIL: cannot read %s\n", path );
		failures++;
		if( f ) fclose( f );
		return;
	}
	ents = calloc( 1, (size_t)head[2] + 1 );
	fseek( f, head[1], SEEK_SET );
	if( fread( ents, 1, (size_t)head[2], f ) != (size_t)head[2] )
	{
		printf( "FAIL: short entity lump in %s\n", path );
		failures++;
	}
	else
		check_lump( ents, head[1 + 14 * 2 + 1] / 64, path ); // LUMP_MODELS: dmodel_t is 64 bytes
	fclose( f );
	free( ents );
}

int main( int argc, char **argv )
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	long random_lumps = 0;

	for( size_t c = 0; c < sizeof( fixed_cases ) / sizeof( fixed_cases[0] ); c++ )
	{
		char what[32];
		snprintf( what, sizeof( what ), "fixed case %d", (int)c );
		check_lump( fixed_cases[c].ents, fixed_cases[c].count, what );
	}
	check_lump( NULL, 4, "NULL lump" );

	for( int r = 0; r < 20000; r++ )
	{
		const int count = (int)( rnd() % 12 );
		random_lump( &buf, &len, &cap, count, r % 4 == 3 );
		check_lump( buf, count, "random lump" );
		random_lumps++;
		if( failures > 20 ) break;
	}

	for( int a = 1; a < argc; a++ )
		check_file( argv[a] );

	free( buf );
	printf( "xbox_bmodel_origin: %ld checks, %d failures (%ld lumps accepted, %ld refused; %ld answers from a naming entity, %ld from the end of the lump; %ld random lumps, %d files)\n",
		checks, failures, lumps_accepted, lumps_refused, answers_named, answers_unnamed, random_lumps, argc - 1 );
	return failures ? 1 : 0;
}
