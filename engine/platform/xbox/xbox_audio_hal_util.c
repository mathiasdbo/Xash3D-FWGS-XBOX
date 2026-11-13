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

	// the S/PDIF engine of an emulator never plays: waiting for it stalls the audio thread for good
	if( !books->spdif_dead && books->retired[1] == 0 && civ[1] == 0 && books->retired[0] >= XBOX_ACI_SPDIF_DEAD_AFTER )
		books->spdif_dead = 1;
	if( books->spdif_dead )
		books->retired[1] = books->retired[0];

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

/* Waits, polling every poll_us for up to timeout_us, until (reg & mask)
   == want; returns 1 when it did. */
static int aci_wait( const xbox_aci_mmio_t *io, unsigned offset, uint8_t mask, uint8_t want, unsigned timeout_us, unsigned poll_us )
{
	unsigned waited = 0;

	for( ;; )
	{
		if(( io->read8( io->ctx, offset ) & mask ) == want )
			return 1;
		if( waited >= timeout_us )
			return 0;
		io->stall_us( io->ctx, poll_us );
		waited += poll_us;
	}
}

unsigned xbox_aci_reset_engines( const xbox_aci_mmio_t *io, unsigned timeout_us, unsigned poll_us )
{
	static const unsigned box[2] = { XBOX_ACI_BOX_PCM_OUT, XBOX_ACI_BOX_SPDIF };
	unsigned stuck = 0;
	int i;

	if( !poll_us )
		poll_us = 1;

	// pause both first, so they halt together
	for( i = 0; i < 2; i++ )
		io->write8( io->ctx, box[i] + XBOX_ACI_REG_CR, 0 );

	for( i = 0; i < 2; i++ )
	{
		if( !aci_wait( io, box[i] + XBOX_ACI_REG_SR, XBOX_ACI_SR_DCH, XBOX_ACI_SR_DCH, timeout_us, poll_us ))
		{
			stuck |= 1u << i; // never reset a running engine
			continue;
		}
		io->write8( io->ctx, box[i] + XBOX_ACI_REG_CR, XBOX_ACI_CR_RESET );
		if( !aci_wait( io, box[i] + XBOX_ACI_REG_CR, XBOX_ACI_CR_RR, 0, timeout_us, poll_us ))
			stuck |= 1u << i;
	}
	return stuck;
}

unsigned xbox_aci_prepare( const xbox_aci_mmio_t *io, void ( *clear_rings )( void *ctx ), void *ring_ctx,
	unsigned timeout_us, unsigned poll_us )
{
	const unsigned stuck = xbox_aci_reset_engines( io, timeout_us, poll_us );

	if( stuck )
		return stuck; // an engine may still be fetching: leave its ring alone

	clear_rings( ring_ctx );
	io->write8( io->ctx, XBOX_ACI_BOX_PCM_OUT + XBOX_ACI_REG_SR, XBOX_ACI_REG_SR_CLEAR );
	io->write8( io->ctx, XBOX_ACI_BOX_SPDIF + XBOX_ACI_REG_SR, XBOX_ACI_REG_SR_CLEAR );
	return 0;
}
