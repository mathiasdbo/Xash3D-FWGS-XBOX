/*
xbox_audio_hal_util.c - the AC97 buffer accounting and the audio stall
watchdog, pure parts

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: 2026 Resonance3D contributors

MIT for the same reason as xbox_audio_hal_util.h.
*/

/* Resonance3D (docs/r3d/divergences.md #150). Contract of every function:
   xbox_audio_hal_util.h. No engine, kernel or hardware dependency - this
   file is compiled for the host by tools/host-tests/run.sh. */

#include "xbox_audio_hal_util.h"

uint32_t xbox_aci_finished( uint32_t submitted, uint32_t retired, uint32_t civ, uint32_t lvi, uint32_t evidence )
{
	const uint32_t mask = XBOX_ACI_DESCRIPTORS - 1;
	const uint32_t inflight = submitted - retired;
	uint32_t ahead, n;

	// nothing queued, or more than the ring can disambiguate
	if( inflight == 0 || inflight >= XBOX_ACI_DESCRIPTORS )
		return 0;

	// descriptors from the oldest unfinished buffer up to the one playing:
	// all of them are done
	ahead = ( civ - retired ) & mask;

	// CIV on a buffer already retired: a replay of the last buffer, or a
	// CIV that has not caught up yet - nothing new has finished
	if( ahead > inflight )
		return 0;

	n = ahead;

	// the buffer at CIV is the last one handed over: done only on evidence
	if( n + 1 == inflight && ( civ & mask ) == ( lvi & mask ))
	{
		if(( evidence & XBOX_ACI_HALTED_EMPTY ) || ( evidence & XBOX_ACI_FORCE ))
			n++;
		else if(( evidence & XBOX_ACI_LAST_DONE ) && ahead == 0 )
			n++; // LVBCI for this very buffer: CIV has not moved since
	}

	return n;
}

uint32_t xbox_aci_both_finished( uint32_t retired_analog, uint32_t retired_digital, uint32_t reported )
{
	const uint32_t a = retired_analog - reported;
	const uint32_t d = retired_digital - reported;

	return a < d ? a : d;
}

uint32_t xbox_aci_account( xbox_aci_books_t *books, uint32_t submitted, const uint32_t civ[2], const uint32_t lvi[2], const uint32_t evidence[2], int force )
{
	uint32_t n;
	int i;

	for( i = 0; i < 2; i++ )
	{
		if( force >= XBOX_ACI_KICK_ALL )
			books->retired[i] = submitted;
		else books->retired[i] += xbox_aci_finished( submitted, books->retired[i], civ[i], lvi[i],
			evidence[i] | ( force >= XBOX_ACI_KICK_LAST ? XBOX_ACI_FORCE : 0u ));
	}

	n = xbox_aci_both_finished( books->retired[0], books->retired[1], books->reported );
	books->reported += n;

	if( n == 0 && force >= XBOX_ACI_KICK_ALL )
	{
		// a lost report: wake the waiter once, outside the books
		books->rescues++;
		return 1;
	}

	return n;
}

uint32_t xbox_aci_watchdog_step( xbox_aci_watchdog_t *wd, double now, uint32_t count, int active, double timeout )
{
	if( !wd->armed || !active || count != wd->last_count )
	{
		wd->armed = 1;
		wd->last_count = count;
		wd->last_change = now;
		wd->stalls = 0;
		return 0;
	}

	if( now - wd->last_change < timeout )
		return 0;

	// stalled for a whole timeout: report it and re-arm, so a stall that
	// recovery did not cure fires again one timeout later
	wd->last_change = now;
	wd->stalls++;
	return wd->stalls;
}
