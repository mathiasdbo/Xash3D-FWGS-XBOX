/*
xbox_audio_hal_util.h - the AC97 buffer accounting and the audio stall
watchdog, pure parts

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: 2026 Resonance3D contributors

MIT, like the nxdk driver it fixes (xbox_audio_hal.c), so the accounting
can go upstream to nxdk unchanged (docs/r3d/divergences.md #150, Exit).
The licence text: deps/nxdk/LICENSES/MIT.txt.
*/

/* Resonance3D (docs/r3d/divergences.md #150). No engine, kernel or
   hardware dependency: tools/host-tests/run.sh compiles this for the host.
   The kernel-side user is xbox_audio_hal.c (the replacement for nxdk's
   lib/hal/audio.c); the engine-side user is xbox_audio_watchdog.c. */

#ifndef XBOX_AUDIO_HAL_UTIL_H
#define XBOX_AUDIO_HAL_UTIL_H

#include <stdint.h>

/* An AC97 bus master's buffer descriptor list has 32 entries; CIV, LVI
   and the descriptor index wrap at 32. */
#define XBOX_ACI_DESCRIPTORS 32u

/* Evidence that the buffer at CIV, when it is the last one handed over,
   has been played out (xbox_aci_finished's `evidence`). */
#define XBOX_ACI_HALTED_EMPTY 1u /* SR.DCH set, PICB 0, CR.RPBM set: running, but stopped at the end of that buffer */
#define XBOX_ACI_LAST_DONE    2u /* SR.LVBCI was raised since the previous evaluation */
#define XBOX_ACI_FORCE        4u /* the watchdog: nothing has moved for a second, it cannot still be playing */

/* Recovery levels (xbox_aci_account's `force`; XAudioR3D_Kick's level). */
#define XBOX_ACI_KICK_NONE 0
#define XBOX_ACI_KICK_LAST 1 /* the last buffer handed over counts as played if CIV is on it */
#define XBOX_ACI_KICK_ALL  2 /* every buffer in flight counts as played; one rescue report if none was left */

/*
How many more buffers one DMA engine (PCM out or S/PDIF) has finished,
from the engine's own registers rather than from counting interrupts.

  submitted - buffers handed to the engine so far (monotonic, wraps)
  retired   - buffers this function has already reported finished
  civ, lvi  - the engine's Current Index Value and Last Valid Index
  evidence  - XBOX_ACI_* bits

Every buffer before CIV has been played. The buffer AT CIV has been
played only when it is the last one handed over and the engine says so:
either it halted with nothing left in it (the AC97/ICH behaviour: DCH
set, PICB 0), or it raised LVBCI while CIV had not moved since the last
evaluation (a controller that keeps replaying the last buffer instead of
halting), or the watchdog forces it.

A CIV that is "behind" the oldest unfinished buffer - the engine still on
a buffer this function already retired, which is what a controller
replaying its last buffer looks like - retires nothing. So does more than
31 buffers in flight, which the ring cannot tell apart from that.

Never returns more than submitted - retired.
*/
uint32_t xbox_aci_finished( uint32_t submitted, uint32_t retired, uint32_t civ, uint32_t lvi, uint32_t evidence );

/* The buffers both engines have finished that have not been reported yet:
   a buffer can be refilled only when PCM out and S/PDIF have both read it
   (they play the same buffers). All three counts are monotonic and may
   wrap. */
uint32_t xbox_aci_both_finished( uint32_t retired_analog, uint32_t retired_digital, uint32_t reported );

/* The driver's books: what each engine (0 PCM out, 1 S/PDIF) has
   finished, and what has been reported to the callback. */
typedef struct xbox_aci_books_s
{
	uint32_t retired[2];
	uint32_t reported;
	uint32_t rescues;     /* reports made by KICK_ALL with nothing finished to report */
} xbox_aci_books_t;

/*
One accounting run (the DPC's): updates the books from both engines'
registers and evidence and returns how many callbacks to make - one per
buffer both engines have newly finished.

force: XBOX_ACI_KICK_*. KICK_LAST adds XBOX_ACI_FORCE to both engines'
evidence. KICK_ALL writes off everything in flight on both engines, and
if that leaves nothing to report it returns 1 anyway (a rescue, counted
in books->rescues, not in reported): after seconds without a single
submission the audio thread can only be waiting on a report that was
lost on its way (its semaphore post), and one more wakes it. A rescue
never makes `reported` overtake `retired`.
*/
uint32_t xbox_aci_account( xbox_aci_books_t *books, uint32_t submitted, const uint32_t civ[2], const uint32_t lvi[2], const uint32_t evidence[2], int force );

/*
The stall watchdog's decision. The caller samples a progress count once a
frame with the current time; when the count has not moved for `timeout`
seconds while `active`, the step returns the number of consecutive stalls
detected so far (1 for the first, 2 for the second timeout without
progress, ...) and re-arms for another `timeout`. Any progress, or
inactivity, resets it. Returns 0 when there is nothing to do.
*/
typedef struct xbox_aci_watchdog_s
{
	int      armed;        /* 0 until the first sample */
	uint32_t last_count;   /* the progress count at last_change */
	double   last_change;  /* when the count last moved (or the last stall fired) */
	uint32_t stalls;       /* consecutive stalls without progress */
} xbox_aci_watchdog_t;

uint32_t xbox_aci_watchdog_step( xbox_aci_watchdog_t *wd, double now, uint32_t count, int active, double timeout );

#endif /* XBOX_AUDIO_HAL_UTIL_H */
