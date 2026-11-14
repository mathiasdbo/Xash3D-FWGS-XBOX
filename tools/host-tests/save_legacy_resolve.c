/*
save_legacy_resolve.c - the engine's own `ofs:N` reader, for tools/save_fixture_gate.py (XS.4)

  gcc -O2 -std=c99 -Iengine/common tools/host-tests/save_legacy_resolve.c -o save_legacy_resolve
  echo ofs:4294950224 | save_legacy_resolve      -> OK BreakTouch@CBreakable

Reads one pointer string per line on stdin; prints OK <name>, BYFIELD, UNKNOWN or BAD for each, in order.
*/
#include <stdio.h>
#include <string.h>
#include "../../engine/common/save_legacy.c"

int main( void )
{
	char line[256];

	while( fgets( line, sizeof( line ), stdin ))
	{
		const char *name = NULL;
		size_t n = strlen( line );

		while( n && ( line[n - 1] == '\n' || line[n - 1] == '\r' ))
			line[--n] = 0;

		switch( SaveLegacy_Resolve( line, &name ))
		{
		case SAVE_LEGACY_OK:      printf( "OK %s\n", name ); break;
		case SAVE_LEGACY_BYFIELD: printf( "BYFIELD\n" ); break;
		case SAVE_LEGACY_UNKNOWN: printf( "UNKNOWN\n" ); break;
		default:                  printf( "BAD\n" ); break;
		}
	}

	return 0;
}
