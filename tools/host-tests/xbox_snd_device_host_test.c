/*
xbox_snd_device_host_test.c - the sound subsystem across a rate switch that loses the device (divergence #173)
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
A model of engine/client/sound/s_main.c's S_Init, S_R3D_ApplyRate and
S_Shutdown on the Xbox, with the decisions of
engine/platform/xbox/xbox_snd_device.h, the guards of the steps they call
(S_FreeSounds and S_StopAllSounds return on !snd.initialized,
SNDDMA_Shutdown clears it) and an SNDDMA_Init whose outcomes the test
chooses. Every path - both opens failing, the old rate coming back, the
new rate opening, sound off from boot - must end with each part of the
subsystem (commands, the client's hooks, the sounds, the DSP, sndpool)
torn down exactly once, the device closed exactly once per open, and
every sound call afterwards a no-op on a subsystem that is gone. The same
model without the reopen's teardown (upstream's S_Shutdown order) leaks,
so the test sees the bug it guards against.

  gcc -O2 -Wall -std=c99 -Iengine/platform/xbox tools/host-tests/xbox_snd_device_host_test.c -o snd_device_test && ./snd_device_test
*/

#include <stdio.h>
#include <string.h>
#include "xbox_snd_device.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

typedef struct
{
	/* the engine's state */
	int initialized, device_lost, device_open, pool, commands, hooks, sounds, dsp;
	/* what happened */
	int opens, closes, pool_frees, command_removals, hook_shutdowns, sound_frees, dsp_frees;
	int calls_after, touched_after;
	/* the test's SNDDMA_Init outcomes, in order: 1 opens, 0 fails */
	const int *init_results;
	int init_next;
	int fix; /* 1: the #173 teardown; 0: upstream's order only */
} model_t;

static int SNDDMA_Init_m( model_t *m )
{
	if( !m->init_results[m->init_next++] )
		return 0;
	m->device_open = 1;
	m->initialized = 1;
	m->opens++;
	return 1;
}

static void SNDDMA_Shutdown_m( model_t *m )
{
	m->initialized = 0; // s_sdl2.c: snd.initialized = false first
	if( m->device_open )
	{
		m->device_open = 0;
		m->closes++;
	}
}

static void S_Init_m( model_t *m )
{
	m->commands = 1;
	m->pool = 1;
	if( !SNDDMA_Init_m( m ))
	{
		m->pool = 0; // upstream: Mem_FreePool, commands stay registered
		return;
	}
	m->hooks = 1;
	m->sounds = 1;
	m->dsp = 1;
}

static void S_Shutdown_m( model_t *m )
{
	if( !Xbox_SndShutdown_Runs( m->initialized, m->device_lost ))
		return;
	if( m->commands ) { m->commands = 0; m->command_removals++; }
	if( m->hooks ) { m->hooks = 0; m->hook_shutdowns++; }
	if( m->initialized && m->sounds ) { m->sounds = 0; m->sound_frees++; } // S_FreeSounds' guard
	if( m->dsp ) { m->dsp = 0; m->dsp_frees++; }
	if( Xbox_SndShutdown_ClosesDevice( m->device_lost ))
		SNDDMA_Shutdown_m( m );
	if( m->pool ) { m->pool = 0; m->pool_frees++; }
	if( m->device_lost )
	{
		m->initialized = 0;
		m->device_lost = 0;
	}
}

/* S_R3D_ApplyRate's device part: close, the new rate, else the old one,
   else the teardown */
static void ApplyRate_m( model_t *m )
{
	if( !m->initialized )
		return; // SND_UpdateSound returns first
	SNDDMA_Shutdown_m( m );
	if( SNDDMA_Init_m( m ))
		return;
	if( SNDDMA_Init_m( m ))
		return;
	if( m->fix )
	{
		m->device_lost = 1;
		m->initialized = 1;
		S_Shutdown_m( m );
	}
}

/* any later sound entry point: SND_UpdateSound, S_StartSound,
   S_RegisterSound... all return on !snd.initialized */
static void sound_call_m( model_t *m )
{
	m->calls_after++;
	if( !m->initialized )
		return;
	if( !m->pool || !m->device_open )
		m->touched_after++; // would use a freed pool or a closed device
}

static void run( const int *results, int switches, int fix, model_t *m )
{
	int k;

	memset( m, 0, sizeof( *m ));
	m->init_results = results;
	m->fix = fix;
	S_Init_m( m );
	for( k = 0; k < switches; k++ )
	{
		ApplyRate_m( m );
		sound_call_m( m );
		sound_call_m( m );
	}
	S_Shutdown_m( m ); // the engine's exit
	S_Shutdown_m( m ); // and a second one, as a Host_Error path may
	sound_call_m( m );
}

/* every part torn down once, the device closed once per open */
static void check_clean( const model_t *m, int booted )
{
	CHECK( !m->initialized && !m->device_open && !m->pool && !m->device_lost );
	CHECK( m->closes == m->opens );
	CHECK( m->touched_after == 0 );
	if( booted )
	{
		CHECK( m->commands == 0 && m->command_removals == 1 );
		CHECK( m->hooks == 0 && m->hook_shutdowns == 1 );
		CHECK( m->sounds == 0 && m->sound_frees == 1 );
		CHECK( m->dsp == 0 && m->dsp_frees == 1 );
		CHECK( m->pool_frees == 1 );
	}
}

int main( void )
{
	static const int both_fail[] = { 1, 0, 0 };
	static const int old_back[] = { 1, 0, 1, 1, 1, 1 };
	static const int new_ok[] = { 1, 1, 1, 1, 1, 1, 1 };
	static const int fail_later[] = { 1, 1, 1, 0, 0, 1, 1 };
	static const int no_sound[] = { 0 };
	model_t m;

	/* both opens fail on the first switch: torn down on the spot, once */
	run( both_fail, 1, 1, &m );
	check_clean( &m, 1 );
	CHECK( m.opens == 1 && m.closes == 1 );

	/* ... and further switch attempts afterwards are no-ops */
	run( both_fail, 5, 1, &m );
	check_clean( &m, 1 );

	/* the new rate fails, the old one comes back: nothing torn down until exit */
	run( old_back, 2, 1, &m );
	check_clean( &m, 1 );
	CHECK( m.opens == 3 );

	/* every switch opens */
	run( new_ok, 4, 1, &m );
	check_clean( &m, 1 );
	CHECK( m.opens == 5 );

	/* two good switches, then both opens fail */
	run( fail_later, 4, 1, &m );
	check_clean( &m, 1 );
	CHECK( m.opens == 3 );

	/* sound off from boot (upstream's failed S_Init): nothing to free twice */
	run( no_sound, 2, 1, &m );
	CHECK( !m.initialized && !m.device_open && m.pool_frees == 0 && m.touched_after == 0 && m.opens == 0 );

	/* upstream's order alone leaks the subsystem when both opens fail */
	run( both_fail, 1, 0, &m );
	CHECK( m.pool && m.sounds && m.dsp && m.hooks && m.pool_frees == 0 );

	printf( "snd_device: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
