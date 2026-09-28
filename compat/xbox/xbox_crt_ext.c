/*
xbox_crt_ext.c - standard C library functions nxdk's pdclib declares but has
not implemented

Part of compat/xbox; see that directory's README.

Different in kind from xbox_win32_ext.c: that file covers the Win32 API
surface nxdk's own headers declare (windows.h and friends) but do not back
with real code. This file is for the plain C standard library - pdclib
declares these in <stdlib.h> and every other platform's libc has a working
one, but pdclib's own comment right above the declaration says so itself:
"TODO: atof(), strtof(), strtod(), strtold()"
(deps/nxdk/lib/pdclib/include/stdlib.h:29). Not an Xbox-specific gap the way
GetModuleFileName is - a gap in the vendored C library this platform happens
to ship, worth its own file so it does not get lost among the Win32 shims.
*/

#include <stdlib.h>
#include <time.h>

/*
====================
atof

Found in review, X1, 2026-09-09: linking the menu module surfaced this as
the one genuinely new undefined symbol beyond the already-parked/no-entry-
point set (3rdparty/mainui's CFGScript.cpp, ConnectionProgress.cpp,
ServerInfo.cpp all call it for ordinary small-magnitude config/cvar values -
not scientific-notation-heavy or precision-critical numeric work).

pdclib provides no strtod either (same TODO line), so this cannot be the
usual one-line `return strtod( nptr, NULL );` real libc implementations
often use - it is a real, self-contained parser. C99 7.20.1.2 semantics:
skip leading whitespace, an optional sign, decimal digits, an optional
fractional part, and an optional exponent; return 0.0 on no valid
conversion, silently ignoring anything unparsed after a valid prefix
(unlike strtod, atof has no way to report how far it got, so there is
nothing to report).
====================
*/
double atof( const char *nptr )
{
	double result = 0.0;
	double sign = 1.0;
	int has_digits = 0;

	if( !nptr )
		return 0.0;

	while( *nptr == ' ' || *nptr == '\t' || *nptr == '\n'
		|| *nptr == '\v' || *nptr == '\f' || *nptr == '\r' )
		nptr++;

	if( *nptr == '+' )
		nptr++;
	else if( *nptr == '-' )
	{
		sign = -1.0;
		nptr++;
	}

	while( *nptr >= '0' && *nptr <= '9' )
	{
		result = result * 10.0 + ( double )( *nptr - '0' );
		nptr++;
		has_digits = 1;
	}

	if( *nptr == '.' )
	{
		double frac_scale = 1.0;

		nptr++;
		while( *nptr >= '0' && *nptr <= '9' )
		{
			frac_scale *= 10.0;
			result += ( double )( *nptr - '0' ) / frac_scale;
			nptr++;
			has_digits = 1;
		}
	}

	if( !has_digits )
		return 0.0;

	if( *nptr == 'e' || *nptr == 'E' )
	{
		const char *look = nptr + 1;
		int exp_sign = 1;
		int exp_value = 0;
		int exp_has_digits = 0;

		if( *look == '+' )
			look++;
		else if( *look == '-' )
		{
			exp_sign = -1;
			look++;
		}

		/* Found in review, 2026-09-09: an unclamped exp_value invited two
		 * real problems from a string this platform cannot treat as
		 * trusted input forever - server-derived values eventually reach
		 * this through ServerInfo.cpp's Info_ValueForKey once networking
		 * is unparked, and config/script strings reach it today. A string
		 * like "1e2147483647" would have driven the scaling loop below
		 * through billions of iterations (a CPU-hang on a 733 MHz CPU,
		 * not a bounded parse), and accumulating exp_value with no bound
		 * signed-overflows before that loop is even reached - undefined
		 * behaviour, not just slow.
		 *
		 * ATOF_MAX_EXPONENT caps accumulation once it is already far past
		 * where the result matters: double's dynamic range is roughly
		 * 1e-324 to 1e308, so any exponent magnitude at or beyond 1000
		 * means "definitely overflows to +-infinity or underflows to 0"
		 * regardless of the exact value - which is exactly what clamping
		 * to this bound and letting the loop below run its (now trivial,
		 * at most 1000 iterations) course produces anyway. No accumulated
		 * value here ever exceeds 10009, nowhere near overflowing a
		 * signed int. */
#define ATOF_MAX_EXPONENT 1000
		while( *look >= '0' && *look <= '9' )
		{
			if( exp_value < ATOF_MAX_EXPONENT )
				exp_value = exp_value * 10 + ( *look - '0' );
			else
				exp_value = ATOF_MAX_EXPONENT;
			look++;
			exp_has_digits = 1;
		}

		if( exp_has_digits )
		{
			double exp_scale = 1.0;
			int i;

			for( i = 0; i < exp_value; i++ )
				exp_scale *= 10.0;

			result = ( exp_sign > 0 ) ? result * exp_scale : result / exp_scale;
		}
#undef ATOF_MAX_EXPONENT
	}

	return sign * result;
}

/*
====================
localtime

Found live, 2026-09-28: opening the main menu's Save/Load screen bugchecks
the Xbox kernel with STATUS_ACCESS_VIOLATION inside strftime, faulting on a
NULL `timeptr`. SV_SaveGameComment (engine/server/sv_save.c:2505) calls
`localtime( &fileTime )` and passes the result straight to strftime two
lines later (sv_save.c:2513) with no NULL check - correct, because every
platform's libc is expected to hand back a real `struct tm *`. pdclib's own
localtime() does not: `functions/time/localtime.c:13` is
`struct tm * localtime( const time_t * timer ) { return NULL; }`, one more
declared-but-absent function in the same family as the already-documented
`atof`/`strtod` gaps above.

The Xbox kernel's own time source (`time.c`'s `KeQuerySystemTime`, used by
pdclib's xbox-specific time()) exposes no timezone offset or TZ database at
all - there is no console-side "set your timezone" concept the way there is
on PC. So unlike a real libc, where localtime() and gmtime() differ by a
timezone/DST offset, on this platform they cannot differ: local time IS UTC
here. gmtime() itself is fully implemented (functions/time/gmtime.c) and
already used correctly elsewhere in this codebase; this just forwards to it
rather than duplicating its date-math.
====================
*/
struct tm *localtime( const time_t *timer )
{
	return gmtime( timer );
}
