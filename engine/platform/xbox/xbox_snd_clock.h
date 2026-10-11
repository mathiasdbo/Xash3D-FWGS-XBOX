/*
xbox_snd_clock.h - the mixer's DMA clock, with a rebase for a device reopened at run time
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
Resonance3D (docs/r3d/divergences.md #173). Upstream's S_GetSoundtime
(engine/client/sound/s_main.c) turns the device's position in its ring
(snd.samplepos, 16-bit sample units: two a stereo frame) into a running
frame count with two function-static variables, `buffers` (ring wraps
seen) and `oldsamplepos`. r3d_snd_rate reopens the device at run time,
and a reopened device starts again at position 0: upstream's arithmetic
would read that as one more wrap and move the clock up to the next
32768-frame boundary (0.7 s at 48 kHz). On the Xbox S_GetSoundtime keeps
that arithmetic here, line for line, on state the reopen can rebase.

The mixer writes frame t at ring index t % fullsamples
(S_TransferPaintBuffer) and the device plays index samplepos / 2, so the
clock must stay congruent with the device's position modulo the ring.
A rebase therefore keeps the logical time T and moves the device to
match: the new, silent device is set to play from index T % fullsamples,
and the clock's wrap count to T / fullsamples. Nothing jumps, nothing is
rounded to a ring boundary.

No engine dependency: tools/host-tests/xbox_snd_clock_host_test.c.
*/
#ifndef XBOX_SND_CLOCK_H
#define XBOX_SND_CLOCK_H

#include <stdint.h>

typedef struct xbox_snd_clock_s
{
	int buffers;      /* upstream's `buffers`: ring wraps counted */
	int oldsamplepos; /* upstream's `oldsamplepos` */
} xbox_snd_clock_t;

/* S_GetSoundtime's arithmetic (s_main.c:1509-1536 in the reference):
   the frames played so far. *chop is set where upstream chops the clock
   (a wrap with paintedtime past 0x40000000); the caller then does what
   upstream does there - paintedtime = fullsamples, S_StopAllSounds( true ). */
static inline int Xbox_SndClock_Time( xbox_snd_clock_t *c, int samplepos, int fullsamples, int paintedtime, int *chop )
{
	*chop = 0;
	if( samplepos < c->oldsamplepos )
	{
		c->buffers++; // buffer wrapped

		if( paintedtime > 0x40000000 )
		{
			// time to chop things off to avoid 32 bit limits
			c->buffers = 0;
			*chop = 1;
		}
	}

	c->oldsamplepos = samplepos;

	return c->buffers * fullsamples + samplepos / 2;
}

/* The time `t` frames at `old_rate` is, in frames at `new_rate`: the same
   instant in seconds, rounded down. */
static inline int Xbox_SndClock_Scale( int t, int old_rate, int new_rate )
{
	if( t <= 0 || old_rate <= 0 || old_rate == new_rate )
		return t;
	return (int)(( (int64_t)t * new_rate ) / old_rate );
}

/* Rebase after a reopen: the clock reads `t` (frames, >= 0) now. Returns
   the position, in samplepos units, the device must be set to (under its
   lock, before it plays anything the mixer will write). */
static inline int Xbox_SndClock_Rebase( xbox_snd_clock_t *c, int t, int fullsamples )
{
	const int samplepos = ( t % fullsamples ) * 2;

	c->buffers = t / fullsamples;
	c->oldsamplepos = samplepos;
	return samplepos;
}

#endif /* XBOX_SND_CLOCK_H */
