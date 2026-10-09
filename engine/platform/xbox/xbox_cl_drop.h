/*
xbox_cl_drop.h - where CL_ParseClientData's dropped-frame loop can start
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
Resonance3D (docs/r3d/divergences.md #161). CL_ParseClientData marks the
frames between the last packet it parsed and this one as dropped
(engine/client/parse/cl_parse.c, :1001-1012 in the reference):

	for( j = cl.last_incoming_sequence + 1; j < i; j++ )
		if( cl.frames[j & CL_UPDATE_MASK].receivedtime >= 0.0 )
		{
			cl.frames[j & CL_UPDATE_MASK].receivedtime = -1.0f;
			cl.frames[j & CL_UPDATE_MASK].latency = 0;
		}

Since upstream's d61983a5 the server starts each client's netchan at a
random outgoing sequence in [1, 2^27) (Netchan_Setup, net_chan.c:249-250
in the reference), and CL_ClearState zeroes cl.last_incoming_sequence at
every map load, so the first svc_clientdata of each map runs this loop up
to 134 million times: 70-900 ms on the console (route-pr193), up to 1.7 s
in xemu, a different amount on every load. Measured, not guessed: the
xemu PC sampler put 1,162 of 1,211 CL_ParseClientData samples on the
loop's `incl %esi` (cl_parse.obj +0x40).

The body touches only slot j & mask, the same way for every j, and does
nothing to a slot a second time (receivedtime is -1 after the first). So
any CL_UPDATE_BACKUP consecutive values of j - one per slot, the mask
being CL_UPDATE_BACKUP - 1 - leave every slot exactly as the whole range
does. Starting at i - backup when the gap is longer than that gives
upstream's frames, bit for bit; tools/host-tests/xbox_cl_drop_host_test.c
runs both loops over random states and gaps, the largest the netchan can
produce included.

Pure and header-only, like xbox_bitrun.h.
*/

#ifndef XBOX_CL_DROP_H
#define XBOX_CL_DROP_H

#include <stdint.h>

/* The first j the loop above needs to visit: last_incoming + 1, or
   incoming - backup when more than `backup` sequences lie between them
   (backup: CL_UPDATE_BACKUP, a power of two, the mask + 1). On a live
   connection incoming is a netchan sequence below 2^30, but in demo
   playback it is whatever the file says (cl_demo.c), so nothing here may
   overflow for any int pair (Codex review): both candidates are computed
   in 64 bits, and the one returned is representable -
     - incoming - backup is returned only when it exceeds last_incoming + 1,
       which is at least INT_MIN + 1, and it is below incoming;
     - last_incoming + 1 is returned only when it is at most incoming;
     - otherwise (no sequence lies between them, last_incoming INT_MAX
       included) incoming itself, so `j < incoming` runs no pass - as
       upstream's loop, from last_incoming + 1 >= incoming, runs none.
   The loop's j++ never overflows: j < incoming <= INT_MAX. (The caller's
   own guard is Xbox_CL_FramesDropped below.) */
static inline int Xbox_CL_FirstDropToMark( int last_incoming, int incoming, int backup )
{
	const int64_t first = (int64_t)last_incoming + 1;
	const int64_t clamped = (int64_t)incoming - backup;

	if( clamped > first )
		return (int)clamped;
	if( first <= incoming )
		return (int)first;
	return incoming;
}

/* The caller's guard, upstream's `i > cl.last_incoming_sequence + 1`
   (cl_parse.c), computed in 64 bits: upstream's int sum overflows when
   last_incoming is INT_MAX, which demo playback can produce (Codex
   review). Same truth value as upstream's for every pair where that sum
   fits an int; for last_incoming INT_MAX it is false, as no sequence can
   lie between them. */
static inline int Xbox_CL_FramesDropped( int last_incoming, int incoming )
{
	return (int64_t)incoming > (int64_t)last_incoming + 1;
}

#endif /* XBOX_CL_DROP_H */
