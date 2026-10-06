/*
xbox_audio_watchdog.c - recovers the audio pipeline if it ever stalls
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
Resonance3D (docs/r3d/divergences.md #150). The safety net behind
xbox_audio_hal.c: a layout- and timing-sensitive stall of the audio
pipeline (SDL's audio thread waiting forever on a buffer the driver never
reports free) was silent for a whole session, with nothing in the log.

The progress signal is the driver's own submission count: SDL's Xbox
driver hands over one buffer per PlayDevice, which its audio thread calls
once per loop whether or not the device is paused (it plays silence
then), so the count stops exactly when that thread is stuck - in
WaitDevice, on a report that never came. "Active" is the driver having a
callback registered (SDL's device open); a new XAudioInit generation
(a reopened device) starts the clock again.

If the count has not moved for a second while active, this logs the
driver's counters and both DMA engines' registers - which also tell how
the controller behaves on an underrun - and asks the driver to account
again from the registers (XAudioR3D_Kick, XBOX_ACI_KICK_LAST): that wakes
a thread blocked on a lost completion and restarts a halted engine. A
stall that survives three such kicks gets XBOX_ACI_KICK_ALL, which writes
off every buffer in flight and wakes the thread once even if nothing was
left to report (a lost semaphore post). Nothing here can block:
SDL_CloseAudioDevice would wait for an audio thread that may be blocked
for good.

Called once a frame from Xbox_FrameTime_EndFrame (#141), recorder on or
off. Cost when nothing is wrong: three loads, two compares and a
subtraction.
*/

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include "xbox_audio_hal.h"
#include "xbox_audio_hal_util.h"

#define XBOX_AUDIO_STALL_SECONDS 1.0
#define XBOX_AUDIO_STALL_STRONG  4 /* consecutive stalls before KICK_ALL */
#define XBOX_AUDIO_STALL_LOGGED  8 /* stalls logged in full, then every 64th */

static xbox_aci_watchdog_t audio_wd;
static uint32_t audio_wd_generation;
static uint32_t audio_stalls_total;

void Xbox_AudioWatchdog( void )
{
	xbox_audio_hal_status_t st;
	uint32_t submitted, generation, stalls;
	int active, level;

	active = XAudioR3D_Progress( &submitted, &generation );

	// a reopened device: start over (an inactive sample resets the step)
	if( generation != audio_wd_generation )
	{
		audio_wd_generation = generation;
		active = 0;
	}

	stalls = xbox_aci_watchdog_step( &audio_wd, host.realtime, submitted, active, XBOX_AUDIO_STALL_SECONDS );
	if( !stalls )
		return;

	audio_stalls_total++;
	level = stalls >= XBOX_AUDIO_STALL_STRONG ? XBOX_ACI_KICK_ALL : XBOX_ACI_KICK_LAST;

	// the state as the stall left it, before the kick changes it
	if( audio_stalls_total <= XBOX_AUDIO_STALL_LOGGED || !( audio_stalls_total & 63 ))
	{
		XAudioR3D_GetStatus( &st );
		Con_Printf( S_WARN "audio stall %u (%u in a row, kick %d): buffers queued %u freed %u (pcm %u spdif %u) rescues %u, "
			"irq %u dpc %u lvbci %u kicks %u restarts %u; pcm civ %u lvi %u sr %04x picb %u cr %02x; spdif civ %u lvi %u sr %04x picb %u cr %02x\n",
			audio_stalls_total, stalls, level, st.submitted, st.delivered, st.retired[0], st.retired[1], st.rescues,
			st.interrupts, st.dpcs, st.drains, st.kicks, st.restarts,
			st.civ[0], st.lvi[0], st.sr[0], st.picb[0], st.cr[0],
			st.civ[1], st.lvi[1], st.sr[1], st.picb[1], st.cr[1] );
	}

	XAudioR3D_Kick( level );
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
