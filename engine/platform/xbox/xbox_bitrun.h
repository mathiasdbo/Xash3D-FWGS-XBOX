/*
xbox_bitrun.h - read a run of zero bits from a bit buffer at once
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #155). A delta's change bits are
almost all zeros: a typical entity delta sets one or two of
entity_state_t's 56. net_encode.c's Delta_ReadFieldRuns reads them with
this a run at a time instead of one MSG_ReadOneBit call each.

Xbox_BitReadZeroRun is the same as calling upstream's MSG_ReadOneBit
(engine/common/net_buffer.c, :563 in the reference) while it returns 0,
at most `max_bits` times, and stopping in front of the first 1 bit:

  - a bit is pData[i >> 3] >> ( i & 7 ) & 1, as MSG_ReadOneBit reads it,
    and only bytes holding bits below nDataBits are read;
  - MSG_ReadOneBit advances one bit only while the bit is inside the
    buffer and bOverflow is clear; past the end it sets bOverflow and
    returns 0 without moving. The run never covers such a call: it stops
    at the end of the buffer and does nothing once bOverflow is set, so
    the caller's next MSG_ReadOneBit makes it exactly as upstream would.

So after it returns n, the cursor is where n zero-returning
MSG_ReadOneBit calls would have left it, bOverflow is untouched, and the
next bit is either a 1 (when n < max_bits and the buffer had room) or
for MSG_ReadOneBit to read. Pure: tools/host-tests/xbox_bitrun_host_test.c
checks it against a model of MSG_ReadOneBit.
*/

#ifndef XBOX_BITRUN_H
#define XBOX_BITRUN_H

#include <stdint.h>

#define XBOX_BITRUN_MAX 24 /* bits looked at per call: with the bit offset, at most 31 - four bytes */

/* The zero bits at *cur_bit, up to max_bits (and XBOX_BITRUN_MAX), the
   buffer's end and the first 1: *cur_bit moves past them; returns how
   many. 0 when `overflow` is set (MSG_ReadOneBit reads nothing then). */
static inline int Xbox_BitReadZeroRun( const uint8_t *data, int *cur_bit, int num_bits, int overflow, int max_bits )
{
	const uint8_t *p;
	uint32_t v;
	int n, shift, zeros;

	if( overflow )
		return 0;

	n = max_bits < XBOX_BITRUN_MAX ? max_bits : XBOX_BITRUN_MAX;
	if( n > num_bits - *cur_bit )
		n = num_bits - *cur_bit;
	if( n <= 0 )
		return 0;

	shift = *cur_bit & 7;
	p = data + ( *cur_bit >> 3 );
	v = p[0];
	if( shift + n > 8 )
		v |= (uint32_t)p[1] << 8;
	if( shift + n > 16 )
		v |= (uint32_t)p[2] << 16;
	if( shift + n > 24 )
		v |= (uint32_t)p[3] << 24;
	v = ( v >> shift ) & (( 1u << n ) - 1u );

	zeros = v ? __builtin_ctz( v ) : n;
	*cur_bit += zeros;
	return zeros;
}

#endif /* XBOX_BITRUN_H */
