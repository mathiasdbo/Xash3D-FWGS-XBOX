/*
printf_host_test.c - host test for compat/xbox/xbox_printf.c

Compares the Xbox printf family against the host C library's own snprintf
on a table of formats, so the formatter is validated before it is trusted
on the console (divergences.md #54). Not part of any Xbox build; run it on
the host from the repository root, in the MSYS2 MinGW-64 shell:

  gcc -O1 -Wall -Wno-format -fstack-protector-all \
      -Dvsnprintf=xb_vsnprintf -Dsnprintf=xb_snprintf -Dvsprintf=xb_vsprintf \
      -Dsprintf=xb_sprintf -D_vsnprintf=xb__vsnprintf -D_snprintf=xb__snprintf \
      -c compat/xbox/xbox_printf.c -o /tmp/xp.o
  gcc -O1 -D__USE_MINGW_ANSI_STDIO=1 -fstack-protector-all \
      tools/host-tests/printf_host_test.c /tmp/xp.o -o /tmp/printf_test && /tmp/printf_test

(-D__USE_MINGW_ANSI_STDIO=1 makes MinGW's printf the C99 reference - two-digit
exponents - rather than msvcrt's.) Exit status is the failure count.

Coverage, stated exactly: the build above runs with stack canaries only. Add
-fsanitize=address,undefined on a toolchain that ships the runtimes - neither
MinGW gcc nor MinGW clang 21 here does (no libclang_rt.asan_dynamic), and
MSYS2's CLANG64 environment, which does, is not installed - and record the
result in divergences.md #54, where the sanitizer pass is listed as outstanding.
Canaries catch an overrun past a frame's guard, not one between objects inside
the same frame.

Two kinds of check: chk() requires byte-equal output and return value on
ordinary formats; chkp() requires the leading digits to agree on extreme
precisions, where the formatter is best-effort past ~17 significant digits
and clamps precision at 350 - those cases exercise the buffer limits.
*/
/* Host test: compat/xbox/xbox_printf.c compiled with its public names renamed
   (xb_*) against the host libc's own snprintf, on a table of formats. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

int xb_vsnprintf( char *buf, size_t cap, const char *fmt, va_list ap );
int xb_snprintf( char *buf, size_t cap, const char *fmt, ... );

static int fails, total;

static void chk( const char *fmt, ... )
{
	char a[512], b[512];
	va_list ap, ap2;
	int ra, rb;

	va_start( ap, fmt );
	va_copy( ap2, ap );
	ra = vsnprintf( a, sizeof( a ), fmt, ap );
	rb = xb_vsnprintf( b, sizeof( b ), fmt, ap2 );
	va_end( ap2 );
	va_end( ap );
	total++;
	if( strcmp( a, b ) != 0 || ra != rb )
	{
		fails++;
		printf( "FAIL  fmt=%-16s libc=[%s](%d) xbox=[%s](%d)\n", fmt, a, ra, b, rb );
	}
}

/* prefix comparison: the first n characters must agree and neither may be empty */
static void chkp( int n, const char *fmt, ... )
{
	char a[1024], b[1024];
	va_list ap, ap2;
	va_start( ap, fmt ); va_copy( ap2, ap );
	vsnprintf( a, sizeof( a ), fmt, ap ); xb_vsnprintf( b, sizeof( b ), fmt, ap2 );
	va_end( ap2 ); va_end( ap );
	total++;
	if( strncmp( a, b, (size_t)n ) != 0 || !a[0] || !b[0] )
	{
		fails++;
		printf( "FAIL  fmt=%-16s prefix %d: libc=[%.40s...] xbox=[%.40s...]\n", fmt, n, a, b );
	}
}

int main( void )
{
	/* the one that mattered */
	chk( "%g %g %g", 0.0, 180.0, 0.0 );
	chk( "%g %g %g", 0.0, 270.0, 0.0 );
	chk( "%g %g %g", -12.5, 45.0, 359.999 );
	/* %g corners */
	chk( "%g", 0.0 ); chk( "%g", 1.0 ); chk( "%g", 100000.0 ); chk( "%g", 1000000.0 ); chk( "%g", 1234567.0 );
	chk( "%g", 0.0001 ); chk( "%g", 0.00001 ); chk( "%g", 123.456 ); chk( "%g", 0.5 ); chk( "%g", -0.0 );
	chk( "%g", 99999.95 ); chk( "%g", 9.9999999 ); chk( "%.3g", 3.14159 ); chk( "%.10g", 3.14159 ); chk( "%#g", 1.0 ); chk( "%G", 1e-10 );
	/* %f / %e */
	chk( "%f", 180.0 ); chk( "%f", -3.5 ); chk( "%.2f", 9.999 ); chk( "%.0f", 2.5 ); chk( "%.0f", 3.5 ); chk( "%#.0f", 3.0 );
	chk( "%10.3f|", 3.14159 ); chk( "%-10.3f|", 3.14159 ); chk( "%010.3f|", -3.14159 ); chk( "%+f", 1.0 ); chk( "% f", 1.0 );
	chk( "%f", 1e20 ); chk( "%f", 123456789012345678.0 ); chk( "%.12f", 0.1 );
	chk( "%e", 180.0 ); chk( "%e", 0.0 ); chk( "%.2e", 9.999 ); chk( "%E", 123456.0 ); chk( "%e", 1e-100 ); chk( "%e", 1e100 );
	chk( "%f %s %d", 1.5, "x", 7 ); /* argument alignment after a float */
	/* integers */
	chk( "%d", 0 ); chk( "%d", -2147483647 - 1 ); chk( "%u", 4294967295u ); chk( "%x", 0xdeadbeef ); chk( "%X", 255 ); chk( "%o", 8 );
	chk( "%#x", 255 ); chk( "%#o", 8 ); chk( "%#x", 0 ); chk( "%5d|", 42 ); chk( "%-5d|", 42 ); chk( "%05d|", 42 ); chk( "%+d", 42 ); chk( "% d", 42 );
	chk( "%.3d", 7 ); chk( "%.0d", 0 ); chk( "%5.3d|", 7 ); chk( "%ld", 123456789L ); chk( "%lld", 1234567890123LL ); chk( "%llu", 18446744073709551615ull );
	chk( "%zu", (size_t)42 ); chk( "%hd", (short)-5 ); chk( "%hhd", (signed char)-5 ); chk( "%hu", 65535 );
	/* strings, chars, misc */
	chk( "%s", "hello" ); chk( "%10s|", "hi" ); chk( "%-10s|", "hi" ); chk( "%.2s", "hello" ); chk( "%c", 'A' ); chk( "%3c|", 'A' ); chk( "%%" );
	chk( "%*d|", 6, 42 ); chk( "%-*d|", 6, 42 ); chk( "%.*f", 2, 3.14159 );
	chk( "%s=%g, %d%%", "sensitivity", 3.0, 100 );
	/* extreme but valid precisions (review: the first version overflowed a probe buffer on %.64g).
	   Beyond ~17 significant digits the formatter is best-effort, not the exact binary expansion,
	   so these check the leading digits and that nothing overflows (run under sanitizers). */
	chkp( 15, "%.64g", 123.456 ); chkp( 15, "%.350g", 1.0 / 3.0 ); chkp( 15, "%.350f", 1.7976931348623157e308 ); chkp( 15, "%.350e", 1.7976931348623157e308 );
	chkp( 15, "%.100f", 1e-300 ); chkp( 15, "%.64g", 1.7976931348623157e308 ); chk( "%.0g", 123456.0 ); chkp( 15, "%.350f", 2.5 );
	chkp( 15, "%f", 1.7976931348623157e308 ); chkp( 8, "%e", 4.9e-324 ); chkp( 8, "%g", 4.9e-324 ); chkp( 15, "%-400.350f|", -1.5 ); chkp( 15, "%0400.350e|", 1.5 );
	/* truncation semantics */
	{
		char b[6]; int r = xb_snprintf( b, sizeof( b ), "%s", "abcdefgh" ); total++;
		if( r != 8 || strcmp( b, "abcde" ) != 0 ) { fails++; printf( "FAIL truncation: r=%d b=[%s]\n", r, b ); }
		r = xb_snprintf( NULL, 0, "%g", 180.0 ); total++;
		if( r != 3 ) { fails++; printf( "FAIL count-only: r=%d\n", r ); }
	}
	printf( "%d checks, %d failures\n", total, fails );
	return fails != 0;
}
