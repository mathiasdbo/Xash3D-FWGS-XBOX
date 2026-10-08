/* Host test for engine/platform/xbox/xbox_bitrun.h (docs/r3d/divergences.md
   #155): Xbox_BitReadZeroRun against a model of upstream's MSG_ReadOneBit,
   and a delta field walk built on it (the shape of net_encode.c's
   Delta_ReadFieldRuns) against upstream's one-MSG_ReadOneBit-per-field
   loop, value reads through a model of MSG_ReadUBitLong.

   The two models are upstream's code (engine/common/net_buffer.c in the
   reference: MSG_ReadOneBit :563-572, MSG_ReadUBitLong :574-616, the
   masks :184-190) over a struct with the sizebuf_t fields they use.

   1. The run, from every cursor of buffers of 1..12 bytes (bit counts
      that do and do not end on a byte), with and without the overflow
      flag, for every max_bits 0..40: the bits it skips are exactly the
      ones MSG_ReadOneBit would return 0 for without overflowing, and it
      stops only at max_bits, at its 24-bit window, at the buffer's end,
      on the overflow flag, or in front of a 1.
   2. The field walk, on random layouts (1..72 fields, values 1..32 bits)
      and messages written with random change-bit densities, read from
      buffers cut short at random points (so reads run off the end and
      overflow): the same fields copied and read, the same values, the
      same final cursor and overflow flag.

   Built and run by tools/host-tests/run.sh. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_bitrun.h"

static int failures;
static long checks;

#define CHECK( cond, ... ) do { checks++; if( !( cond )) { if( failures++ < 20 ) { fprintf( stderr, "%s:%d: ", __FILE__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); fputc( '\n', stderr ); } } } while( 0 )

typedef struct
{
	uint8_t *pData;
	int bOverflow;
	int iCurBit;
	int nDataBits;
} bits_t;

/* --- upstream's reader, modelled ------------------------------------- */

static const uint32_t ExtraMasks[32] =
{
	0x00000000, 0x00000001, 0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f, 0x0000007f,
	0x000000ff, 0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff, 0x00001fff, 0x00003fff, 0x00007fff,
	0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff, 0x000fffff, 0x001fffff, 0x003fffff, 0x007fffff,
	0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff, 0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff,
};

static int MSG_Overflow( bits_t *sb, int nBits )
{
	if( sb->iCurBit + nBits > sb->nDataBits )
		sb->bOverflow = 1;
	return sb->bOverflow;
}

static int MSG_ReadOneBit( bits_t *sb )
{
	if( !MSG_Overflow( sb, 1 ))
	{
		int value = sb->pData[sb->iCurBit >> 3] & ( 1 << ( sb->iCurBit & 7 ));
		sb->iCurBit++;
		return !!value;
	}
	return 0;
}

static uint32_t rd_le32( const uint8_t *p )
{
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

static uint32_t MSG_ReadUBitLong( bits_t *sb, int numbits )
{
	if( numbits == 8 )
	{
		int leftBits = sb->nDataBits - sb->iCurBit;

		if( leftBits >= 0 && leftBits < 8 )
			return 0; // end of message
	}

	if( MSG_Overflow( sb, numbits ))
	{
		sb->iCurBit = sb->nDataBits;
		return 0;
	}

	int idword1 = sb->iCurBit >> 5;
	uint32_t dword1 = rd_le32( sb->pData + 4 * idword1 );
	dword1 >>= ( sb->iCurBit & 31 );

	sb->iCurBit += numbits;
	uint32_t ret = dword1;

	if(( sb->iCurBit - 1 ) >> 5 == idword1 )
	{
		if( numbits != 32 )
			ret &= ExtraMasks[numbits];
	}
	else
	{
		int nExtraBits = sb->iCurBit & 31;
		uint32_t dword2 = rd_le32( sb->pData + 4 * ( idword1 + 1 )) & ExtraMasks[nExtraBits];

		ret |= ( dword2 << ( numbits - nExtraBits ));
	}
	return ret;
}

/* --- 1. the run ------------------------------------------------------ */

static uint32_t rng = 0x12345678u;

static uint32_t rnd( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

#define PAD 8 /* upstream's dword reads may look past the last byte */

static void test_run( void )
{
	uint8_t data[12 + PAD];
	int bytes, trial;

	for( bytes = 1; bytes <= 12; bytes++ )
	{
		for( trial = 0; trial < 300; trial++ )
		{
			/* sparse to dense: one in 2..64 bits set */
			const uint32_t one_in = 2u + rnd() % 63u;
			const int num_bits = bytes * 8 - (int)( rnd() % 8u );
			int i, cur, ov, max_bits;

			memset( data, 0, sizeof( data ));
			for( i = 0; i < bytes * 8; i++ )
				if( rnd() % one_in == 0 )
					data[i >> 3] |= (uint8_t)( 1u << ( i & 7 ));
			/* bits past num_bits set, so a run that read them would stop early */
			for( i = num_bits; i < bytes * 8; i++ )
				data[i >> 3] |= (uint8_t)( 1u << ( i & 7 ));

			for( cur = 0; cur <= num_bits; cur++ )
			for( ov = 0; ov <= 1; ov++ )
			for( max_bits = 0; max_bits <= 40; max_bits++ )
			{
				bits_t ref = { data, ov, cur, num_bits };
				int c = cur, n, k;

				n = Xbox_BitReadZeroRun( data, &c, num_bits, ov, max_bits );
				CHECK( n >= 0 && n <= max_bits && n <= XBOX_BITRUN_MAX, "n %d max %d", n, max_bits );
				CHECK( c == cur + n, "cursor %d after %d from %d", c, n, cur );

				/* n calls of upstream's MSG_ReadOneBit: each reads a 0
				   without overflowing, and they end where the run did */
				for( k = 0; k < n; k++ )
				{
					CHECK( MSG_ReadOneBit( &ref ) == 0 && !ref.bOverflow, "bit %d of the run from %d", k, cur );
				}
				CHECK( ref.iCurBit == c, "upstream cursor %d, run %d", ref.iCurBit, c );

				/* it stopped for one of its reasons */
				if( n < max_bits && n < XBOX_BITRUN_MAX )
				{
					const int at_end = c >= num_bits;
					CHECK( ov || at_end || MSG_ReadOneBit( &ref ) == 1, "stopped at %d (from %d, n %d) on a 0", c, cur, n );
				}
				if( ov )
					CHECK( n == 0, "a run with the overflow flag set" );
			}
		}
	}
}

/* --- 2. the field walk ----------------------------------------------- */

#define MAX_FIELDS 72

typedef struct
{
	int kind[MAX_FIELDS];   /* 0 copied, 1 read */
	uint32_t value[MAX_FIELDS];
	int cur, overflow;
} walk_t;

/* upstream: MSG_ReadDeltaEntity's loop of Delta_ReadField */
static void walk_upstream( bits_t *sb, const int *width, int n, walk_t *w )
{
	int i;

	for( i = 0; i < n; i++ )
	{
		if( !MSG_ReadOneBit( sb ))
		{
			w->kind[i] = 0;
			w->value[i] = 0;
			continue;
		}
		w->kind[i] = 1;
		w->value[i] = MSG_ReadUBitLong( sb, width[i] );
	}
	w->cur = sb->iCurBit;
	w->overflow = sb->bOverflow;
}

/* net_encode.c's Delta_ReadFieldRuns, field reads as above */
static void walk_runs( bits_t *sb, const int *width, int n, walk_t *w )
{
	int i = 0;

	while( i < n )
	{
		int zeros = Xbox_BitReadZeroRun( sb->pData, &sb->iCurBit, sb->nDataBits, sb->bOverflow, n - i );

		if( zeros > 0 )
		{
			for( ; zeros > 0; zeros--, i++ )
			{
				w->kind[i] = 0;
				w->value[i] = 0;
			}
			continue;
		}

		if( !MSG_ReadOneBit( sb ))
		{
			w->kind[i] = 0;
			w->value[i] = 0;
		}
		else
		{
			w->kind[i] = 1;
			w->value[i] = MSG_ReadUBitLong( sb, width[i] );
		}
		i++;
	}
	w->cur = sb->iCurBit;
	w->overflow = sb->bOverflow;
}

static void put_bits( uint8_t *buf, int *pos, uint32_t v, int nbits )
{
	int b;

	for( b = 0; b < nbits; b++, ( *pos )++ )
		if( v >> b & 1u )
			buf[*pos >> 3] |= (uint8_t)( 1u << ( *pos & 7 ));
}

static void test_walk( void )
{
	static uint8_t buf[512 + PAD];
	int trial, deltas_with_change = 0, overflowed = 0;

	for( trial = 0; trial < 200000; trial++ )
	{
		int width[MAX_FIELDS];
		const int n = 1 + (int)( rnd() % MAX_FIELDS );
		const uint32_t one_in = 1u + rnd() % 40u;
		const int start = (int)( rnd() % 64u );
		int i, pos = start, num_bits, changed = 0;
		walk_t a, b;
		bits_t sa, sb;

		memset( buf, 0, sizeof( buf ));
		/* bits before the message, as the entity number and header */
		for( i = 0; i < start; i++ )
			if( rnd() & 1u )
				buf[i >> 3] |= (uint8_t)( 1u << ( i & 7 ));
		for( i = 0; i < n; i++ )
		{
			width[i] = 1 + (int)( rnd() % 32u );
			if( rnd() % one_in == 0 )
			{
				put_bits( buf, &pos, 1, 1 );
				put_bits( buf, &pos, rnd(), width[i] );
				changed = 1;
			}
			else pos++;
		}
		/* trailing garbage: the next entity's bits */
		for( i = pos; i < pos + 40; i++ )
			if( rnd() & 1u )
				buf[i >> 3] |= (uint8_t)( 1u << ( i & 7 ));

		/* the buffer ends after the message (byte-padded), or anywhere
		   inside it */
		if( rnd() % 4u )
			num_bits = ( pos + 7 + (int)( rnd() % 64u )) & ~7;
		else num_bits = start + (int)( rnd() % (uint32_t)( pos - start + 1 ));

		sa.pData = sb.pData = buf;
		sa.iCurBit = sb.iCurBit = start;
		sa.nDataBits = sb.nDataBits = num_bits;
		sa.bOverflow = sb.bOverflow = ( rnd() % 64u ) == 0;

		walk_upstream( &sa, width, n, &a );
		walk_runs( &sb, width, n, &b );

		CHECK( a.cur == b.cur && a.overflow == b.overflow, "trial %d: cursor %d/%d overflow %d/%d", trial, a.cur, b.cur, a.overflow, b.overflow );
		for( i = 0; i < n; i++ )
			CHECK( a.kind[i] == b.kind[i] && a.value[i] == b.value[i], "trial %d field %d: %d %u / %d %u", trial, i, a.kind[i], a.value[i], b.kind[i], b.value[i] );
		deltas_with_change += changed;
		overflowed += a.overflow;
	}
	/* the cases the walk is meant to cover were drawn */
	CHECK( deltas_with_change > 100000 && overflowed > 10000, "coverage: %d with a change, %d overflowed", deltas_with_change, overflowed );
	printf( "xbox_bitrun: walk: %d deltas with a change, %d overflowed\n", deltas_with_change, overflowed );
}

int main( void )
{
	test_run();
	test_walk();
	printf( "xbox_bitrun: %ld checks, %d failures\n", checks, failures );
	return failures != 0;
}
