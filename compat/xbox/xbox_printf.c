/*
xbox_printf.c - the buffer-writing printf family, with floating point, for nxdk

Part of compat/xbox; see that directory's README and docs/r3d/divergences.md.

Why this file exists (found live, docs/r3d/fork-plan.md XG.1, 2026-09-15):
nxdk's pdclib parses the floating-point conversions and then does nothing
with them - deps/nxdk/lib/pdclib/functions/_PDCLIB/_PDCLIB_print.c:452-458 is
`case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': break;` - so
"%g" prints nothing, and because the double is never pulled off the va_list,
every argument after it is misaligned too. The engine's own "angle" ->
"angles" keyvalue conversion is `Q_snprintf( "%g %g %g", ... )`
(engine/server/sv_game.c:5023), which therefore produced an empty string;
the HL SDK parsed that as 0 0 0, and every entity placed with an "angle" key
faced yaw 0 - the player spawning backwards in c0a0 ("angle" "180"), the
guard at the door facing sideways ("angle" "270"). The pre-fix state rests
on the report, on the pdclib source, and on a screenshot of the old build
booting c0a0 (looking backward into the tram car); with this file in the
build, the svc_setangle the server writes at that spawn reads (0, 180, 0)
on the wire (tools/xemu-debug.py) and the view looks forward down the
track. SDL2's own formatter cannot stand in:
SDL_config_xbox.h sets HAVE_VSNPRINTF, so SDL_vsnprintf forwards to the same
pdclib.

What this is: a self-contained vsnprintf and the buffer-writing wrappers -
snprintf, vsprintf, sprintf - which the engine (through Q_vsnprintf), the
statically linked HL SDK, nxdk's own debugPrint and SDL_vsnprintf all end up
calling. _vsnprintf and _snprintf are defined too, as a safety net only:
nxdk maps those MSVC spellings to the plain names by macro
(xboxrt/libc_extensions/stdio_ext_.h:6-7), so no object in the link
references them today. Defined in an object that is linked ahead of
libpdclib.lib, so lld never pulls pdclib's own members for these names:
an archive member is only extracted for a symbol still undefined at that
point, and a definition from a direct-input object discards the lazy
entry. The one ordering that could break this - a referencing object,
then the archive, then this file - fails loudly as a duplicate symbol,
which tools/xbox-link-check.sh gates to zero; it cannot silently revert to
pdclib. The link check also asserts this object is present and precedes
libpdclib.lib, which closes the only silent path (the object missing). The stream family (printf, fprintf, vfprintf) is left to pdclib
on purpose: this port's console output goes through con_xbox.c, and nothing
that matters formats a float to a stream.

Conversions: d i u x X o c s p % with the flags - + space # 0, width and
precision including '*', and the length modifiers hh h l ll z t j; f F e E
g G with C99 semantics for precision, the # flag and %g's trailing-zero
removal and e/f selection; inf/nan. Two stated limits: a float precision
above 350 is clamped to 350 (every float body buffer here is sized for
DBL_MAX's 309 integer digits plus that, FMT_FLOAT_BUF), and beyond about 17
significant digits the digits are best-effort rather than the exact binary
expansion a full dtoa would print - nothing in this engine or the SDK
formats floats at that precision, and the host test pins the leading
digits for such formats and drives the buffer limits with stack canaries
on (no ASan/UBSan runtime exists in the host toolchain here; that pass is
recorded as outstanding in divergences.md #54). Not implemented: %a/%A (hex floats),
%n, wide %ls/%lc, positional arguments - none are used by the engine or the
SDK, and an unknown conversion is echoed literally so it is visible rather
than silently dropped, which is exactly the failure this file replaces.
Verified against the host C library's own snprintf on a table of formats -
tools/host-tests/printf_host_test.c, which says how to build and run it on
the host (kept out of compat/ so the Xbox sweep never compiles a second
main()) - before it was trusted on the console.

Return value follows C99: the length the full output would have had, so a
caller can detect truncation; the buffer is always NUL-terminated when its
size is non-zero.
*/

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct
{
	char   *buf;
	size_t  cap;   /* bytes available including the terminator; 0 = count only */
	size_t  len;   /* characters produced so far (may exceed cap) */
} out_t;

static void out_char( out_t *o, char c )
{
	if( o->cap && o->len + 1 < o->cap )
		o->buf[o->len] = c;
	o->len++;
}

static void out_padded( out_t *o, const char *sign, size_t signlen, const char *body, size_t bodylen,
	int width, int left, int zero )
{
	size_t total = signlen + bodylen;
	int pad = ( width > 0 && (size_t)width > total ) ? width - (int)total : 0;
	size_t i;

	if( !left && !zero )
		for( i = 0; i < (size_t)pad; i++ ) out_char( o, ' ' );
	for( i = 0; i < signlen; i++ ) out_char( o, sign[i] );
	if( !left && zero )
		for( i = 0; i < (size_t)pad; i++ ) out_char( o, '0' );
	for( i = 0; i < bodylen; i++ ) out_char( o, body[i] );
	if( left )
		for( i = 0; i < (size_t)pad; i++ ) out_char( o, ' ' );
}

/* unsigned integer -> digits, most significant first, into the END of tmp;
   returns the start pointer. precision handled by the caller. */
static char *utoa_rev( char *end, unsigned long long v, unsigned base, int upper )
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char *p = end;

	*--p = '\0';
	if( v == 0 )
		*--p = '0';
	while( v )
	{
		*--p = digits[v % base];
		v /= base;
	}
	return p;
}

/* ---- floating point ---------------------------------------------------- */

/* Emit the decimal digits of an integer-valued double (>= 0, < 1e18 fits
   in unsigned long long; larger values fall back to repeated division in
   floating point, exact enough for %f of large magnitudes at this port's
   precision needs). */
static size_t emit_integer_part( char *dst, double ip )
{
	char tmp[400];
	size_t n = 0;

	if( ip < 1e18 )
	{
		char *p = utoa_rev( tmp + sizeof( tmp ), (unsigned long long)ip, 10, 0 );
		n = strlen( p );
		memcpy( dst, p, n );
		return n;
	}

	/* very large: peel digits from the top by repeated division */
	{
		int ndig = 0;
		double t = ip;
		char rev[400];
		while( t >= 1.0 && ndig < (int)sizeof( rev ) - 1 )
		{
			double q = floor( t / 10.0 );
			double r = t - q * 10.0;
			/* Clamp before converting, not after: above 2^53 the remainder
			   is not confined to [0, 10) - it can be as large as one ulp of
			   t (1e284 at t = 1e300) - and converting an out-of-range double
			   to int is undefined. Found by the host test built at -O2 with
			   -fsanitize=float-cast-overflow when the Xbox build moved off
			   -O0 (tools/xbox-opt.sh). */
			int d = r <= 0.0 ? 0 : r >= 9.0 ? 9 : (int)r;
			rev[ndig++] = (char)( '0' + d );
			t = q;
		}
		while( ndig-- )
			dst[n++] = rev[ndig];
		return n;
	}
}

/* %f core: |v| with `prec` fractional digits into dst (no sign). Returns
   length. Digits are generated one past the precision and the decimal
   string is then rounded - half-up, half-to-even on an exact tie - so the
   carry propagates into the integer part ("9.999" %.2f -> "10.00") and the
   result does not suffer the double rounding that scaling by a power of
   ten first would introduce (found by the host test: 99999.95 %g came out
   as 100000, 3.14159 %.3f as 3.141). */
#define FMT_MAX_FRAC 350

/* Worst case a float body can be: DBL_MAX has 309 integer digits, plus '.',
   plus FMT_MAX_FRAC fraction digits (or, for %e, plus "e+308"). Every
   buffer a float body is rendered into is this size; found in review - the
   first version probed %g's exponent into 64 bytes. */
#define FMT_FLOAT_BUF ( 309 + 1 + FMT_MAX_FRAC + 8 )

static size_t fmt_fixed( char *dst, double v, int prec, int alt )
{
	double ip, fp;
	char ibuf[400], fbuf[FMT_MAX_FRAC + 2];
	size_t ilen, n = 0, i;
	int ndig, extra, round_up, carry;

	if( prec > FMT_MAX_FRAC )
		prec = FMT_MAX_FRAC;

	fp = modf( v, &ip );
	ilen = emit_integer_part( ibuf, ip );

	/* prec digits plus one guard digit (the loop below always fills them;
	   the memset only keeps -Wmaybe-uninitialized quiet) */
	memset( fbuf, '0', sizeof( fbuf ));
	ndig = prec + 1;
	for( i = 0; i < (size_t)ndig; i++ )
	{
		int d;
		fp *= 10.0;
		d = (int)fp;
		if( d < 0 ) d = 0;
		if( d > 9 ) d = 9;
		fbuf[i] = (char)( '0' + d );
		fp -= d;
	}

	extra = fbuf[prec] - '0';
	if( extra > 5 || ( extra == 5 && fp > 0.0 ))
		round_up = 1;
	else if( extra == 5 )
	{
		/* exact tie: round to even on the last kept digit */
		int last = prec > 0 ? fbuf[prec - 1] - '0' : ( ilen ? ibuf[ilen - 1] - '0' : 0 );
		round_up = last & 1;
	}
	else
		round_up = 0;

	carry = round_up;
	for( i = prec; carry && i > 0; i-- )
	{
		if( fbuf[i - 1] == '9' ) fbuf[i - 1] = '0';
		else { fbuf[i - 1]++; carry = 0; }
	}
	for( i = ilen; carry && i > 0; i-- )
	{
		if( ibuf[i - 1] == '9' ) ibuf[i - 1] = '0';
		else { ibuf[i - 1]++; carry = 0; }
	}
	if( carry )
	{
		memmove( ibuf + 1, ibuf, ilen );
		ibuf[0] = '1';
		ilen++;
	}

	memcpy( dst, ibuf, ilen );
	n = ilen;
	if( prec > 0 || alt )
		dst[n++] = '.';
	memcpy( dst + n, fbuf, (size_t)prec );
	n += (size_t)prec;
	return n;
}

/* %e core: d.ddddde+XX with `prec` fractional digits, no sign. The
   significant digits come straight from the value - the integer part
   exactly, the fraction by digit extraction - never from v / 10^e: that
   division has its own rounding error, which the host test caught turning
   99999.95 (%.5e) into 1.00000e+05 by manufacturing an exact-looking tie.
   Rounding is on the decimal string, one guard digit, half-to-even on a
   true tie, the same as fmt_fixed. */
static size_t fmt_exp( char *dst, double v, int prec, int alt, int upper, int *exp_out )
{
	char ibuf[400], dig[FMT_MAX_FRAC + 400];
	size_t ilen = 0, nd = 0, i, n = 0;
	int e = 0, want, extra, round_up, carry, rest_nonzero = 0;
	double ip, fp;

	if( prec > FMT_MAX_FRAC )
		prec = FMT_MAX_FRAC;
	want = prec + 1; /* significant digits kept, plus one guard */

	if( v == 0.0 )
	{
		memset( dig, '0', (size_t)want + 1 );
		nd = (size_t)want + 1;
	}
	else
	{
		fp = modf( v, &ip );
		if( ip >= 1.0 )
		{
			ilen = emit_integer_part( ibuf, ip );
			e = (int)ilen - 1;
			for( i = 0; i < ilen && nd < (size_t)want + 1; i++ )
				dig[nd++] = ibuf[i];
			for( ; i < ilen; i++ )
				if( ibuf[i] != '0' ) rest_nonzero = 1;
		}
		else
		{
			/* leading fraction zeros set a negative exponent */
			e = -1;
			for( i = 0; i < 400; i++ )
			{
				int d;
				fp *= 10.0;
				d = (int)fp;
				fp -= d;
				if( d != 0 ) { dig[nd++] = (char)( '0' + d ); break; }
				e--;
			}
		}
		while( nd < (size_t)want + 1 )
		{
			int d;
			fp *= 10.0;
			d = (int)fp;
			if( d < 0 ) d = 0;
			if( d > 9 ) d = 9;
			dig[nd++] = (char)( '0' + d );
			fp -= d;
		}
		if( fp > 0.0 ) rest_nonzero = 1;
	}

	extra = dig[want] - '0';
	if( extra > 5 || ( extra == 5 && rest_nonzero ))
		round_up = 1;
	else if( extra == 5 )
		round_up = ( dig[want - 1] - '0' ) & 1;
	else
		round_up = 0;

	carry = round_up;
	for( i = (size_t)want; carry && i > 0; i-- )
	{
		if( dig[i - 1] == '9' ) dig[i - 1] = '0';
		else { dig[i - 1]++; carry = 0; }
	}
	if( carry )
	{
		/* 9.99.. rounded up to 10.0..: shift and bump the exponent */
		memmove( dig + 1, dig, (size_t)want );
		dig[0] = '1';
		e++;
	}

	dst[n++] = dig[0];
	if( prec > 0 || alt )
		dst[n++] = '.';
	memcpy( dst + n, dig + 1, (size_t)prec );
	n += (size_t)prec;
	dst[n++] = upper ? 'E' : 'e';
	dst[n++] = e < 0 ? '-' : '+';
	{
		int ae = e < 0 ? -e : e;
		if( ae >= 100 )
		{
			dst[n++] = (char)( '0' + ae / 100 );
			ae %= 100;
		}
		dst[n++] = (char)( '0' + ae / 10 );
		dst[n++] = (char)( '0' + ae % 10 );
	}
	if( exp_out ) *exp_out = e;
	return n;
}

/* strip trailing zeros of a fixed/exp body's fraction (C99 %g without #) */
static size_t strip_trailing_zeros( char *s, size_t n )
{
	size_t epos = n, dot = (size_t)-1, i;

	for( i = 0; i < n; i++ )
	{
		if( s[i] == '.' ) dot = i;
		if( s[i] == 'e' || s[i] == 'E' ) { epos = i; break; }
	}
	if( dot == (size_t)-1 )
		return n;
	{
		size_t end = epos;           /* one past the last fraction char */
		size_t taillen = n - epos;   /* exponent part, if any */
		char tail[8];
		size_t cut = end;
		memcpy( tail, s + epos, taillen );
		while( cut > dot + 1 && s[cut - 1] == '0' ) cut--;
		if( cut == dot + 1 ) cut = dot; /* drop the dot too */
		memcpy( s + cut, tail, taillen );
		return cut + taillen;
	}
}

static size_t fmt_float( char *dst, double v, char conv, int prec, int alt )
{
	int upper = ( conv == 'F' || conv == 'E' || conv == 'G' );
	int c = conv | 0x20;

	if( isnan( v ))
	{
		memcpy( dst, upper ? "NAN" : "nan", 3 );
		return 3;
	}
	if( isinf( v ))
	{
		memcpy( dst, upper ? "INF" : "inf", 3 );
		return 3;
	}
	if( prec < 0 ) prec = 6;
	if( prec > FMT_MAX_FRAC ) prec = FMT_MAX_FRAC;

	if( c == 'f' )
		return fmt_fixed( dst, v, prec, alt );
	if( c == 'e' )
		return fmt_exp( dst, v, prec, alt, upper, NULL );

	/* %g: P significant digits; use %e if exp < -4 or exp >= P, else %f
	   with P-1-exp fraction digits; strip trailing zeros unless # */
	{
		int P = prec == 0 ? 1 : prec;
		int e = 0;
		size_t n;

		if( v != 0.0 )
		{
			/* exponent as %e with P-1 digits would round it */
			char tmp[FMT_FLOAT_BUF];
			fmt_exp( tmp, v, P - 1, 0, 0, &e );
		}
		if( e < -4 || e >= P )
			n = fmt_exp( dst, v, P - 1, alt, upper, NULL );
		else
			n = fmt_fixed( dst, v, P - 1 - e, alt );
		if( !alt )
			n = strip_trailing_zeros( dst, n );
		return n;
	}
}

/* ---- the formatter ----------------------------------------------------- */

int vsnprintf( char *buf, size_t cap, const char *fmt, va_list ap )
{
	out_t o;
	const char *f = fmt;

	o.buf = buf;
	o.cap = cap;
	o.len = 0;

	for( ; *f; f++ )
	{
		int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
		int width = -1, prec = -1;
		int lmod = 0; /* -2 hh, -1 h, 0 none, 1 l, 2 ll, 3 z/t, 4 j */
		char conv;

		if( *f != '%' )
		{
			out_char( &o, *f );
			continue;
		}
		f++;

		for( ;; f++ )
		{
			if( *f == '-' ) left = 1;
			else if( *f == '+' ) plus = 1;
			else if( *f == ' ' ) space = 1;
			else if( *f == '#' ) alt = 1;
			else if( *f == '0' ) zero = 1;
			else break;
		}
		if( *f == '*' )
		{
			width = va_arg( ap, int );
			if( width < 0 ) { left = 1; width = -width; }
			f++;
		}
		else while( *f >= '0' && *f <= '9' )
			width = ( width < 0 ? 0 : width * 10 ) + ( *f++ - '0' );
		if( *f == '.' )
		{
			f++;
			prec = 0;
			if( *f == '*' )
			{
				prec = va_arg( ap, int );
				if( prec < 0 ) prec = -1;
				f++;
			}
			else while( *f >= '0' && *f <= '9' )
				prec = prec * 10 + ( *f++ - '0' );
		}
		if( *f == 'h' ) { lmod = -1; f++; if( *f == 'h' ) { lmod = -2; f++; } }
		else if( *f == 'l' ) { lmod = 1; f++; if( *f == 'l' ) { lmod = 2; f++; } }
		else if( *f == 'z' || *f == 't' ) { lmod = 3; f++; }
		else if( *f == 'j' ) { lmod = 4; f++; }
		else if( *f == 'L' ) { f++; }

		conv = *f;
		if( !conv )
			break;

		switch( conv )
		{
		case '%':
			out_char( &o, '%' );
			break;
		case 'c':
		{
			char c = (char)va_arg( ap, int );
			out_padded( &o, "", 0, &c, 1, width, left, 0 );
			break;
		}
		case 's':
		{
			const char *s = va_arg( ap, const char * );
			size_t n;
			if( !s ) s = "(null)";
			n = strlen( s );
			if( prec >= 0 && (size_t)prec < n ) n = (size_t)prec;
			out_padded( &o, "", 0, s, n, width, left, 0 );
			break;
		}
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p':
		{
			unsigned long long uv;
			int neg = 0;
			char tmp[32], *body;
			char sign[3];
			size_t signlen = 0, bodylen;
			unsigned base = 10;
			int upper = 0;

			if( conv == 'p' )
			{
				uv = (unsigned long long)(uintptr_t)va_arg( ap, void * );
				base = 16; alt = 1; conv = 'x';
			}
			else if( conv == 'd' || conv == 'i' )
			{
				long long sv;
				switch( lmod )
				{
				case 2: sv = va_arg( ap, long long ); break;
				case 4: sv = va_arg( ap, intmax_t ); break;
				case 3: sv = (long long)va_arg( ap, ptrdiff_t ); break;
				case 1: sv = va_arg( ap, long ); break;
				case -1: sv = (short)va_arg( ap, int ); break;
				case -2: sv = (signed char)va_arg( ap, int ); break;
				default: sv = va_arg( ap, int ); break;
				}
				if( sv < 0 ) { neg = 1; uv = (unsigned long long)( -( sv + 1 )) + 1ull; }
				else uv = (unsigned long long)sv;
			}
			else
			{
				switch( lmod )
				{
				case 2: uv = va_arg( ap, unsigned long long ); break;
				case 4: uv = va_arg( ap, uintmax_t ); break;
				case 3: uv = va_arg( ap, size_t ); break;
				case 1: uv = va_arg( ap, unsigned long ); break;
				case -1: uv = (unsigned short)va_arg( ap, unsigned ); break;
				case -2: uv = (unsigned char)va_arg( ap, unsigned ); break;
				default: uv = va_arg( ap, unsigned ); break;
				}
				if( conv == 'x' ) base = 16;
				else if( conv == 'X' ) { base = 16; upper = 1; }
				else if( conv == 'o' ) base = 8;
			}

			body = utoa_rev( tmp + sizeof( tmp ), uv, base, upper );
			bodylen = strlen( body );
			if( prec >= 0 )
			{
				/* precision = minimum digits; zero flag is ignored */
				zero = 0;
				if( prec == 0 && uv == 0 ) bodylen = 0;
				while( bodylen < (size_t)prec && body > tmp ) { *--body = '0'; bodylen++; }
			}
			if( alt && base == 8 && ( bodylen == 0 || body[0] != '0' ) && body > tmp ) { *--body = '0'; bodylen++; }

			if( neg ) sign[signlen++] = '-';
			else if( plus && ( conv == 'd' || conv == 'i' )) sign[signlen++] = '+';
			else if( space && ( conv == 'd' || conv == 'i' )) sign[signlen++] = ' ';
			if( alt && base == 16 && uv != 0 ) { sign[signlen++] = '0'; sign[signlen++] = upper ? 'X' : 'x'; }

			out_padded( &o, sign, signlen, body, bodylen, width, left, zero );
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
		{
			double v = va_arg( ap, double );
			char body[FMT_FLOAT_BUF];
			char sign[2];
			size_t signlen = 0, bodylen;
			int neg = signbit( v ) && !isnan( v );

			if( neg ) v = -v;
			bodylen = fmt_float( body, v, conv, prec, alt );
			if( neg ) sign[signlen++] = '-';
			else if( plus ) sign[signlen++] = '+';
			else if( space ) sign[signlen++] = ' ';
			if( isnan( v ) || isinf( v )) zero = 0;
			out_padded( &o, sign, signlen, body, bodylen, width, left, zero );
			break;
		}
		default:
			/* unknown conversion: echo it, visibly */
			out_char( &o, '%' );
			out_char( &o, conv );
			break;
		}
	}

	if( cap )
		buf[o.len < cap ? o.len : cap - 1] = '\0';
	return (int)o.len;
}

int snprintf( char *buf, size_t cap, const char *fmt, ... )
{
	va_list ap;
	int n;
	va_start( ap, fmt );
	n = vsnprintf( buf, cap, fmt, ap );
	va_end( ap );
	return n;
}

int vsprintf( char *buf, const char *fmt, va_list ap )
{
	return vsnprintf( buf, (size_t)-1 / 2, fmt, ap );
}

int sprintf( char *buf, const char *fmt, ... )
{
	va_list ap;
	int n;
	va_start( ap, fmt );
	n = vsnprintf( buf, (size_t)-1 / 2, fmt, ap );
	va_end( ap );
	return n;
}

/* MSVC-style spellings the HL SDK and Win32-flavoured engine code use;
   these return -1 on truncation like the originals, and do not guarantee a
   terminator when truncated - matching _vsnprintf, which Q_vsnprintf
   already handles for the MSVC build. */
int _vsnprintf( char *buf, size_t cap, const char *fmt, va_list ap )
{
	int n = vsnprintf( buf, cap, fmt, ap );
	return ( cap && (size_t)n >= cap ) ? -1 : n;
}

int _snprintf( char *buf, size_t cap, const char *fmt, ... )
{
	va_list ap;
	int n;
	va_start( ap, fmt );
	n = _vsnprintf( buf, cap, fmt, ap );
	va_end( ap );
	return n;
}
