/*
save_legacy_host_test.c - host test for engine/common/save_legacy.c (XS.3: what an `ofs:N` in an old save means)

  gcc -O2 -Wall -std=c99 -Iengine/common tools/host-tests/save_legacy_host_test.c -o /tmp/save_legacy_test && /tmp/save_legacy_test
*/
#include <stdio.h>
#include <string.h>
#include "../../engine/common/save_legacy.c" // the table is static: include the unit to reach it

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )

static int resolve( const char *s, const char **name )
{
	return SaveLegacy_Resolve( s, name );
}

int main( void )
{
	const char *name;
	int off, i, n = (int)( sizeof( legacy_rows ) / sizeof( legacy_rows[0] ));

	// the table is what binary search needs: strictly increasing, one row per offset
	CHECK( n > 200 );
	for( i = 1; i < n; i++ )
		CHECK( legacy_rows[i - 1].offset < legacy_rows[i].offset );

	// parsing: a decimal, and a negative offset is its unsigned 32-bit wrap
	CHECK( SaveLegacy_ParseOffset( "ofs:0", &off ) == SAVE_LEGACY_OK && off == 0 );
	CHECK( SaveLegacy_ParseOffset( "ofs:41696", &off ) == SAVE_LEGACY_OK && off == 41696 );
	CHECK( SaveLegacy_ParseOffset( "ofs:4294950224", &off ) == SAVE_LEGACY_OK && off == -17072 );
	CHECK( SaveLegacy_ParseOffset( "ofs:-17072", &off ) == SAVE_LEGACY_OK && off == -17072 );
	CHECK( SaveLegacy_ParseOffset( "ofs:4294967295", &off ) == SAVE_LEGACY_OK && off == -1 );
	CHECK( SaveLegacy_ParseOffset( "ofs:2147483647", &off ) == SAVE_LEGACY_OK && off == 2147483647 );
	CHECK( SaveLegacy_ParseOffset( "ofs:2147483648", &off ) == SAVE_LEGACY_OK && off == -2147483647 - 1 );

	// parsing refuses what is not a plain number, and what does not fit 32 bits
	CHECK( SaveLegacy_ParseOffset( "ofs:", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs:-", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs:asdf", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs:12x", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs: 12", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs:4294967296", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "ofs:99999999999999999999999", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "Think@CFoo", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( "", &off ) == SAVE_LEGACY_BADSTRING );
	CHECK( SaveLegacy_ParseOffset( NULL, &off ) == SAVE_LEGACY_BADSTRING );

	// known offsets: the one in the plan's notes, and the first rows
	CHECK( resolve( "ofs:4294950224", &name ) == SAVE_LEGACY_OK && name && !strcmp( name, "BreakTouch@CBreakable" ));
	CHECK( resolve( "ofs:-210384", &name ) == SAVE_LEGACY_OK && name && !strcmp( name, "SUB_CallUseToggle@CBaseEntity" ));

	// the folded offset is never guessed: the field has to say
	CHECK( resolve( "ofs:4294756928", &name ) == SAVE_LEGACY_BYFIELD && name == NULL );

	// an offset no public build wrote is refused, not mapped to an address
	CHECK( resolve( "ofs:1", &name ) == SAVE_LEGACY_UNKNOWN && name == NULL );
	CHECK( resolve( "ofs:4294967295", &name ) == SAVE_LEGACY_UNKNOWN && name == NULL );
	CHECK( resolve( "ofs:2147483648", &name ) == SAVE_LEGACY_UNKNOWN && name == NULL );
	CHECK( resolve( "ofs:garbage", &name ) == SAVE_LEGACY_BADSTRING && name == NULL );

	// every row resolves through the text form a save uses, as the wrapped decimal
	for( i = 0; i < n; i++ )
	{
		char s[32];
		int r;

		snprintf( s, sizeof( s ), "ofs:%u", (unsigned)legacy_rows[i].offset );
		r = resolve( s, &name );
		CHECK( legacy_rows[i].name ? ( r == SAVE_LEGACY_OK && name == legacy_rows[i].name ) : ( r == SAVE_LEGACY_BYFIELD && !name ));
		CHECK( !legacy_rows[i].name || strchr( legacy_rows[i].name, '@' ));
	}

	if( failures )
	{
		printf( "save_legacy_host_test: %d FAILED\n", failures );
		return 1;
	}

	printf( "save_legacy_host_test: ok (%d offsets)\n", n );
	return 0;
}
