/*
xbox_audio_hal.c - the Original Xbox AC97 audio driver (replaces nxdk's
lib/hal/audio.c)

SPDX-License-Identifier: MIT

Based on nxdk's lib/hal/audio.c (nxdk 29638d0), MIT-licensed:
  SPDX-FileCopyrightText: 2003 Andy Green
  SPDX-FileCopyrightText: 2004 Craig Edwards
  SPDX-FileCopyrightText: 2005-2006 Richard Osborne
  SPDX-FileCopyrightText: 2006 Guillaume Lamonoca
  SPDX-FileCopyrightText: 2017-2020 Jannik Vogel
  SPDX-FileCopyrightText: 2020-2021 Stefan Schmidt
Changes: SPDX-FileCopyrightText: 2026 Resonance3D contributors
Licence text: deps/nxdk/LICENSES/MIT.txt.
*/

/*
Resonance3D (docs/r3d/divergences.md #150).

This file defines every symbol nxdk's lib/hal/audio.c exports
(XAudioInit, XAudioPlay, XAudioPause, XAudioProvideSamples,
XDumpAudioStatus and ac97Device) with the same <hal/audio.h> contract, so
the linker resolves them here and never extracts that member from
libnxdk_hal.lib. deps/nxdk stays untouched.

Why: nxdk's ISR counts buffer-completion interrupts and, after an
interrupt that carried LVBCI (the engine played out its last valid
buffer: an underrun), ignores the next completion (`analogDrained`, nxdk
commit 33e5ebb "Only cause 1 callback per XAudioProvideSamples"). That is
right for a controller that keeps replaying the last buffer; on one that
halts at the last valid buffer - the AC97/ICH behaviour, and QEMU's, so
xemu's - the ignored completion is a real one. With SDL's two buffers
(SDL_xboxaudio.c) the audio thread then waits on a semaphore post that
never comes, the hardware drains and halts, no interrupt ever fires again:
silence for the rest of the session, from one underrun. The audio thread
is created at normal priority after XAudioPlay has started the first
buffer, so whether the first underrun happens at startup depends on when
the scheduler first runs it - which is why it came and went with unrelated
code changes.

What changes:
- the accounting: the DPC works out, per DMA engine, how many buffers have
  finished from the engine's own CIV/LVI/SR/PICB (xbox_aci_finished,
  xbox_audio_hal_util.c, host-tested under both controller behaviours),
  not from interrupt counts, and calls the callback once per buffer both
  engines have finished. A coalesced DPC (KeInsertQueueDpc refuses a
  second insertion while one is queued) or a missed interrupt therefore
  loses nothing: the next run sees the registers.
- the ISR acknowledges exactly the status bits it read (write-1-to-clear)
  instead of 0xFF, so an LVBCI raised between its read and its write is
  not wiped out unseen.
- XAudioProvideSamples runs at DISPATCH_LEVEL, so the DPC never sees half
  a submission.
- XAudioInit disconnects the interrupt and dequeues the DPC before
  re-initialising (SDL's XBOXAUDIO_CloseDevice calls it again on a
  connected interrupt object).
- ac97Device is 512-byte aligned: the hardware is given the physical
  address of each 256-byte descriptor list's first entry and reads on
  from there, which is only right if the list does not cross a page.
- XAudioR3D_Kick/XAudioR3D_Progress/XAudioR3D_GetStatus (xbox_audio_hal.h)
  for the engine's stall watchdog (xbox_audio_watchdog.c).
- the ISR also queues the DPC on a FIFO error.
- XAudioR3D_ThreadMark/XAudioR3D_ThreadStats (#173): the audio thread's
  CPU time from SDL's callback to its buffer's submission, the buffers
  submitted and the PCM engine's underruns, for the frame-time recorder.

Unverified on the console (only the ICH specification and QEMU's model
back it): that the MCPX halts at the last valid buffer rather than
replaying it (the accounting is right either way), and that an LVI write
restarts a halted engine (both nxdk's driver and this one rely on it; a
recovery kick also tries pause-and-run if it does not). The watchdog's
log line records the registers that settle both.

The register layout (NABM boxes at +0x110 PCM out and +0x170 S/PDIF, each:
+0x04 CIV, +0x05 LVI, +0x06 SR, +0x08 PICB, +0x0B CR) is the AC97 bus
master's, as nxdk's file already uses it (0x114-0x11B, 0x174-0x17B).
*/

#include "build.h"

#if XASH_XBOX && !XASH_DEDICATED

#include <stdint.h>
#include <stdbool.h>
#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>
#include "xbox_audio_hal.h"
#include "xbox_audio_hal_util.h"

#define AUDIO_IRQ 6

#define ACI_BOX_PCM_OUT 0x110
#define ACI_BOX_SPDIF   0x170
#define ACI_CIV  0x04
#define ACI_LVI  0x05
#define ACI_SR   0x06
#define ACI_PICB 0x08
#define ACI_CR   0x0B

#define ACI_SR_DCH   0x01 /* DMA controller halted */
#define ACI_SR_LVBCI 0x04 /* last valid buffer completion interrupt */
#define ACI_SR_BCIS  0x08 /* buffer completion interrupt status */
#define ACI_SR_FIFOE 0x10 /* FIFO error (underrun) */

#define ACI_CR_RPBM 0x01  /* run/pause bus master */
#define ACI_CR_RUN  0x1d  /* XAudioPlay's value: run, interrupts on (FIFO error, LVB, completion) */
#define ACI_CR_STOP 0x1c  /* XAudioPause's value */

static const unsigned aci_box[2] = { ACI_BOX_PCM_OUT, ACI_BOX_SPDIF };

// global reference to the ac97 device (exported, <hal/audio.h>).
// 512-aligned: 2 x 32 descriptors of 8 bytes, never across a page.
AC97_DEVICE ac97Device __attribute__(( aligned( 512 )));

static KINTERRUPT aci_interrupt;
static KDPC aci_dpc;
static bool aci_initialized;   // aci_dpc and aci_interrupt initialised
static bool aci_connected;

// Buffers handed over; written at DISPATCH_LEVEL by XAudioProvideSamples,
// read by the DPC.
static volatile uint32_t aci_submitted;
// XAudioInit calls, so the watchdog can tell a reopened device.
static volatile uint32_t aci_generation;
// The DPC's own: buffers each engine has finished, and buffers reported.
static xbox_aci_books_t aci_books;
// Set by the ISR when an engine raised LVBCI, consumed by the DPC.
static volatile long aci_last_done[2];
// The watchdog's recovery level for the next DPC run (XBOX_ACI_KICK_*).
static volatile long aci_force;

static volatile uint32_t aci_stat_interrupts, aci_stat_dpcs, aci_stat_drains;
static volatile uint32_t aci_stat_kicks, aci_stat_restarts;

// The audio thread's time and the counters the recorder reads
// (XAudioR3D_ThreadStats, #173). The mark is the audio thread's alone;
// the totals are written by one context each and read whole (32-bit).
static uint64_t aci_thr_mark;
static bool aci_thr_pending;
static volatile uint32_t aci_thr_cycles, aci_thr_runs;
static volatile uint32_t aci_stat_buffers, aci_stat_drains_pcm;

static inline uint64_t aci_rdtsc( void )
{
	return __builtin_ia32_rdtsc();
}

// XAudioR3D_BeginClose's timer (#173): wakes the audio thread while SDL
// closes the device, until XAudioInit cancels it.
static KTIMER aci_close_timer;
static KDPC aci_close_dpc;
static bool aci_close_armed;

static inline volatile uint8_t *aci_regs( void )
{
	return (volatile uint8_t *)ac97Device.mmio;
}

// The accounting. Integer only: a DPC must not touch the FPU without
// saving its state.
static void NTAPI aci_dpc_routine( PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2 )
{
	volatile uint8_t *pb = aci_regs();
	const long force = __atomic_exchange_n( &aci_force, 0, __ATOMIC_SEQ_CST );
	const uint32_t submitted = aci_submitted;
	uint32_t civ[2], lvi[2], evidence[2], inflight[2], n;
	uint8_t sr[2], cr[2];
	int i;

	(void)Dpc; (void)DeferredContext; (void)SystemArgument1; (void)SystemArgument2;

	aci_stat_dpcs++;

	// before the books change: what each engine still had to play
	inflight[0] = submitted - aci_books.retired[0];
	inflight[1] = submitted - aci_books.retired[1];

	for( i = 0; i < 2; i++ )
	{
		const unsigned box = aci_box[i];
		uint16_t picb;

		// evidence first, then the registers it is evidence about
		evidence[i] = __atomic_exchange_n( &aci_last_done[i], 0, __ATOMIC_SEQ_CST ) ? XBOX_ACI_LAST_DONE : 0;

		civ[i] = pb[box + ACI_CIV] & ( XBOX_ACI_DESCRIPTORS - 1 );
		lvi[i] = pb[box + ACI_LVI] & ( XBOX_ACI_DESCRIPTORS - 1 );
		sr[i] = pb[box + ACI_SR];
		cr[i] = pb[box + ACI_CR];
		picb = *(volatile uint16_t *)( pb + box + ACI_PICB );

		// halted at the end of a buffer while set to run - not merely
		// paused (XAudioInit's state before XAudioPlay is DCH, PICB 0 too)
		if(( sr[i] & ACI_SR_DCH ) && picb == 0 && ( cr[i] & ACI_CR_RPBM ))
			evidence[i] |= XBOX_ACI_HALTED_EMPTY;
	}

	n = xbox_aci_account( &aci_books, submitted, civ, lvi, evidence, (int)force );

	if( force )
	{
		for( i = 0; i < 2; i++ )
		{
			const unsigned box = aci_box[i];

			// halted with buffers still queued: an LVI write restarts a
			// halted bus master (ICH, QEMU). Unverified on the MCPX, so if
			// it is still halted after that, pause and run it again.
			if(( sr[i] & ACI_SR_DCH ) && civ[i] != lvi[i] && ( cr[i] & ACI_CR_RPBM ) && inflight[i] != 0 )
			{
				pb[box + ACI_LVI] = (uint8_t)lvi[i];
				aci_stat_restarts++;
				if( pb[box + ACI_SR] & ACI_SR_DCH )
				{
					pb[box + ACI_CR] = ACI_CR_STOP;
					pb[box + ACI_CR] = ACI_CR_RUN;
					aci_stat_restarts++;
				}
			}
			// not running at all: what XAudioPlay writes
			if( !( cr[i] & ACI_CR_RPBM ))
			{
				pb[box + ACI_CR] = ACI_CR_RUN;
				aci_stat_restarts++;
			}
		}
	}

	if( ac97Device.callback )
	{
		while( n-- )
			ac97Device.callback( (void *)&ac97Device, ac97Device.callbackData );
	}
}

// Acknowledges what each engine raised and leaves the accounting to the
// DPC. PCM out and S/PDIF play the same buffers; both are watched, since a
// buffer is free only when both have read it.
static BOOLEAN NTAPI aci_isr( PKINTERRUPT Interrupt, PVOID ServiceContext )
{
	volatile uint8_t *pb = aci_regs();
	bool queue = false;
	int i;

	(void)Interrupt; (void)ServiceContext;

	aci_stat_interrupts++;

	for( i = 0; i < 2; i++ )
	{
		const unsigned box = aci_box[i];
		const uint8_t sr = pb[box + ACI_SR];

		if( !sr )
			continue;

		if( sr & ACI_SR_LVBCI )
		{
			aci_last_done[i] = 1;
			aci_stat_drains++;
			if( i == 0 )
				aci_stat_drains_pcm++; // the recorder's snd_underruns (#173)
		}
		// FIFO error too: the accounting is idempotent, so a run too many
		// costs nothing
		if( sr & ( ACI_SR_LVBCI | ACI_SR_BCIS | ACI_SR_FIFOE ))
			queue = true;

		// write-1-to-clear exactly what was read (nxdk writes 0xFF, which
		// also clears a source raised after the read, unseen)
		pb[box + ACI_SR] = sr;
	}

	// coalesces with a DPC already queued: harmless, the DPC reads the
	// registers rather than counting interrupts
	if( queue )
		KeInsertQueueDpc( &aci_dpc, NULL, NULL );

	return TRUE;
}

void XDumpAudioStatus( void )
{
	// nxdk's is a no-op too (its debugPrint is commented out)
}

// Initialises the audio subsystem. Callable again (SDL's CloseDevice does,
// with a NULL callback, to silence the hardware).
//
// sampleSizeInBits and numChannels are ignored, as in nxdk: 16 bits, 2
// channels.
void XAudioInit( int sampleSizeInBits, int numChannels, XAudioCallback callback, void *data )
{
	AC97_DEVICE *pac97device = &ac97Device;
	volatile unsigned char *pb;
	KIRQL irql;
	ULONG vector;
	unsigned int i;

	// a re-initialisation: no interrupt or DPC of the previous one may run
	// against the state reset below
	if( aci_connected )
	{
		KeDisconnectInterrupt( &aci_interrupt );
		aci_connected = false;
	}
	if( aci_initialized )
		KeRemoveQueueDpc( &aci_dpc );
	// a close's wake-up timer (XAudioR3D_BeginClose): SDL's thread has
	// exited by now (CloseDevice runs after SDL_WaitThread)
	if( aci_close_armed )
	{
		KeCancelTimer( &aci_close_timer );
		KeRemoveQueueDpc( &aci_close_dpc );
		aci_close_armed = false;
	}

	// Hack to prevent an assertion in MmGetPhysicalAddress by locking the
	// memory (nxdk's comment: a future API should allocate it with
	// MmAllocateContiguousMemory instead).
	MmLockUnlockBufferPages( (PVOID)pac97device, sizeof( AC97_DEVICE ), FALSE );

	pac97device->mmio = (unsigned int *)0xfec00000;
	pac97device->nextDescriptor = 0;
	pac97device->callback = callback;
	pac97device->callbackData = data;
	pac97device->sampleSizeInBits = sampleSizeInBits;
	pac97device->numChannels = numChannels;

	pb = (unsigned char *)pac97device->mmio;

	// initialise descriptors to all zero (no samples)
	for( i = 0; i < 32; i++ )
	{
		pac97device->pcmSpdifDescriptor[i].bufferStartAddress = 0;
		pac97device->pcmSpdifDescriptor[i].bufferLengthInSamples = 0;
		pac97device->pcmSpdifDescriptor[i].bufferControl = 0;
		pac97device->pcmOutDescriptor[i].bufferStartAddress = 0;
		pac97device->pcmOutDescriptor[i].bufferLengthInSamples = 0;
		pac97device->pcmOutDescriptor[i].bufferControl = 0;
	}

	// perform cold reset
	pac97device->mmio[0x12C >> 2] &= ~2;
	{
		LARGE_INTEGER interval;
		interval.QuadPart = -10;
		KeDelayExecutionThread( KernelMode, FALSE, &interval );
	}
	pac97device->mmio[0x12C >> 2] |= 2;

	// wait until the chip is finished resetting...
	while( !( pac97device->mmio[0x130 >> 2] & 0x100 ))
		;

	// reset bus master registers for analog output
	pb[0x11B] = ( 1 << 4 ) | ( 1 << 3 ) | ( 1 << 2 ) | ( 1 << 1 );
	while( pb[0x11B] & ( 1 << 1 ))
		;

	// reset bus master registers for digital output
	pb[0x17B] = ( 1 << 4 ) | ( 1 << 3 ) | ( 1 << 2 ) | ( 1 << 1 );
	while( pb[0x17B] & ( 1 << 1 ))
		;

	// clear all interrupts
	pb[0x116] = 0xFF;
	pb[0x176] = 0xFF;

	// tell the audio chip where it should look for the descriptors
	pac97device->mmio[0x100 >> 2] = 0; // no PCM input
	pac97device->mmio[0x110 >> 2] = MmGetPhysicalAddress( (void *)&pac97device->pcmOutDescriptor[0] );
	pac97device->mmio[0x170 >> 2] = MmGetPhysicalAddress( (void *)&pac97device->pcmSpdifDescriptor[0] );

	// default to being silent...
	XAudioPause();

	// reset buffer status
	aci_submitted = 0;
	aci_books.retired[0] = aci_books.retired[1] = 0;
	aci_books.reported = 0;
	aci_last_done[0] = aci_last_done[1] = 0;
	aci_force = 0;
	aci_generation++;
	// a callback whose buffer never reached the hardware (SDL closing the
	// device plays into its work buffer): no interval to close (#173)
	aci_thr_pending = false;

	// register our ISR
	vector = HalGetInterruptVector( AUDIO_IRQ, &irql );

	KeInitializeDpc( &aci_dpc, aci_dpc_routine, NULL );
	KeInitializeInterrupt( &aci_interrupt, aci_isr, NULL, vector, irql, LevelSensitive, FALSE );
	aci_initialized = true;

	aci_connected = KeConnectInterrupt( &aci_interrupt ) ? true : false;
}

// tell the chip it is OK to play...
void XAudioPlay( void )
{
	volatile unsigned char *pb = aci_regs();
	pb[0x11B] = ACI_CR_RUN; // PCM out - run, allow interrupts
	pb[0x17B] = ACI_CR_RUN; // S/PDIF - run, allow interrupts
}

// tell the chip it is paused.
void XAudioPause( void )
{
	volatile unsigned char *pb = aci_regs();
	pb[0x11B] = ACI_CR_STOP; // PCM out - pause, allow interrupts
	pb[0x17B] = ACI_CR_STOP; // S/PDIF - pause, allow interrupts
}

// Gives the audio chip one more buffer, on both engines (they share it).
// The callback reports when both have played it.
void XAudioProvideSamples( unsigned char *buffer, unsigned short bufferLength, int isFinal )
{
	AC97_DEVICE *pac97device = &ac97Device;
	volatile unsigned char *pb = aci_regs();
	unsigned short bufferControl = 0x8000; // interrupt on completion
	const unsigned int address = MmGetPhysicalAddress( (PVOID)buffer );
	const unsigned int wordCount = bufferLength / 2;
	unsigned int idx;
	KIRQL old_irql;

	// #173: closes the audio thread's interval opened by its callback
	if( aci_thr_pending )
	{
		aci_thr_cycles += (uint32_t)( aci_rdtsc() - aci_thr_mark );
		aci_thr_runs++;
		aci_thr_pending = false;
	}
	aci_stat_buffers++;

	if( isFinal )
		bufferControl |= 0x4000;

	// the DPC must see a submission whole: descriptor, LVI and count
	old_irql = KeRaiseIrqlToDpcLevel();

	idx = aci_submitted & ( XBOX_ACI_DESCRIPTORS - 1 );

	pac97device->pcmOutDescriptor[idx].bufferStartAddress    = address;
	pac97device->pcmOutDescriptor[idx].bufferLengthInSamples = wordCount;
	pac97device->pcmOutDescriptor[idx].bufferControl         = bufferControl;
	pac97device->pcmSpdifDescriptor[idx].bufferStartAddress    = address;
	pac97device->pcmSpdifDescriptor[idx].bufferLengthInSamples = wordCount;
	pac97device->pcmSpdifDescriptor[idx].bufferControl         = bufferControl;

	pb[0x115] = (unsigned char)idx; // set last valid index (restarts a halted engine)
	pb[0x175] = (unsigned char)idx;

	aci_submitted++;
	pac97device->nextDescriptor = (unsigned char)(( idx + 1 ) & ( XBOX_ACI_DESCRIPTORS - 1 ));

	KfLowerIrql( old_irql );
}

void XAudioR3D_Kick( int level )
{
	long cur;

	if( !aci_initialized )
		return;

	aci_stat_kicks++;

	// keep the stronger of two pending requests
	cur = __atomic_load_n( &aci_force, __ATOMIC_SEQ_CST );
	while( cur < level && !__atomic_compare_exchange_n( &aci_force, &cur, (long)level, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
		;

	// at PASSIVE_LEVEL the DPC runs before this returns
	KeInsertQueueDpc( &aci_dpc, NULL, NULL );
}

int XAudioR3D_Progress( uint32_t *submitted, uint32_t *generation )
{
	*submitted = aci_submitted;
	*generation = aci_generation;
	return aci_initialized && ac97Device.callback != NULL;
}

// Integer only, like aci_dpc_routine: reports one free buffer, which is
// one semaphore post for SDL's thread.
static void NTAPI aci_close_dpc_routine( PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2 )
{
	(void)Dpc; (void)DeferredContext; (void)SystemArgument1; (void)SystemArgument2;

	if( ac97Device.callback )
		ac97Device.callback( (void *)&ac97Device, ac97Device.callbackData );
}

void XAudioR3D_BeginClose( void )
{
	LARGE_INTEGER due;

	if( !aci_initialized || aci_close_armed )
		return;

	KeInitializeDpc( &aci_close_dpc, aci_close_dpc_routine, NULL );
	KeInitializeTimerEx( &aci_close_timer, NotificationTimer );
	due.QuadPart = -100000; // 10 ms from now, in 100 ns units; then every 10 ms
	aci_close_armed = true;
	KeSetTimerEx( &aci_close_timer, due, 10, &aci_close_dpc );
}

void XAudioR3D_ThreadMark( void )
{
	aci_thr_mark = aci_rdtsc();
	aci_thr_pending = true;
}

void XAudioR3D_ThreadStats( xbox_audio_thread_stats_t *out )
{
	out->cycles = aci_thr_cycles;
	out->runs = aci_thr_runs;
	out->buffers = aci_stat_buffers;
	out->underruns = aci_stat_drains_pcm;
}

void XAudioR3D_GetStatus( xbox_audio_hal_status_t *out )
{
	volatile uint8_t *pb = aci_regs();
	int i;

	out->generation = aci_generation;
	out->submitted = aci_submitted;
	out->delivered = aci_books.reported;
	out->retired[0] = aci_books.retired[0];
	out->retired[1] = aci_books.retired[1];
	out->rescues = aci_books.rescues;
	out->interrupts = aci_stat_interrupts;
	out->dpcs = aci_stat_dpcs;
	out->drains = aci_stat_drains;
	out->kicks = aci_stat_kicks;
	out->restarts = aci_stat_restarts;

	for( i = 0; i < 2; i++ )
	{
		const unsigned box = aci_box[i];
		out->civ[i] = pb ? pb[box + ACI_CIV] : 0;
		out->lvi[i] = pb ? pb[box + ACI_LVI] : 0;
		out->sr[i] = pb ? *(volatile uint16_t *)( pb + box + ACI_SR ) : 0;
		out->picb[i] = pb ? *(volatile uint16_t *)( pb + box + ACI_PICB ) : 0;
		out->cr[i] = pb ? pb[box + ACI_CR] : 0;
	}
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
