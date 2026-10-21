/*
xbox_snd_meter.c - a level meter on the final mix, for the SDL and OpenAL outputs
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
Resonance3D (docs/r3d/divergences.md #189). With "r3d_snd_meter 1" (a
userconfig.d cfg or the console) the final mix's level is measured where it
leaves for the codec: on SDL, the samples S_TransferPaintBuffer writes; on
OpenAL, every block the output callback hands over (alXboxOutputCallback,
which turns the zero-copy output into copy mode while it is set). Every two
seconds of output one line goes to the console and to sndmeter.txt: the
RMS over the two seconds, the loudest 100 ms in them and the peak, in dBFS
of 16-bit full scale. The same scene on both outputs gives their level
difference.

The sums run in the thread that hands the samples over (the mixer on SDL,
OpenAL's output thread); the result of each window is copied out and a
counter moved, which the main loop prints from (Xbox_SndMeter_Frame).
*/
#include "common.h"
#include "platform/platform.h"
#include "sound.h"
#include "platform/xbox/xbox_snd_meter.h"
#include "platform/xbox/xbox_snd_al.h"

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alext.h>
#include <math.h>

static CVAR_DEFINE_AUTO( r3d_snd_meter, "0", 0, "1: the final mix's level every 2 seconds, to the console and sndmeter.txt" );

typedef struct
{
	double   sum2, win2, win_max;
	int      peak;
	uint32_t frames, win_frames;
} meter_acc_t;

static meter_acc_t s_acc; /* the writer's */
static struct { double rms, loud; int peak; } s_out; /* the last whole window */
static volatile uint32_t s_seq;
static uint32_t s_seen;
static int s_rate = 48000;
static qboolean s_hooked; /* the OpenAL output callback is ours */
static qboolean s_file_started; /* sndmeter.txt written this boot: appended to from then on */

static double Meter_dB( double v )
{
	return v > 0.0 ? 20.0 * log10( v / 32768.0 ) : -120.0;
}

void Xbox_SndMeter_Block( const short *samples, int frames )
{
	meter_acc_t *a = &s_acc;
	const uint32_t win = (uint32_t)( s_rate / 10 );
	int i;

	// frame by frame, so each 100 ms window closes at its own boundary, not the block's
	for( i = 0; i < frames; i++ )
	{
		const int l = samples[i * 2], r = samples[i * 2 + 1];
		const double sq = (double)l * l + (double)r * r;

		a->sum2 += sq;
		a->win2 += sq;
		if( abs( l ) > a->peak )
			a->peak = abs( l );
		if( abs( r ) > a->peak )
			a->peak = abs( r );
		if( ++a->win_frames >= win )
		{
			const double rms = sqrt( a->win2 / ( 2.0 * a->win_frames ));

			if( rms > a->win_max )
				a->win_max = rms;
			a->win2 = 0.0;
			a->win_frames = 0;
		}
	}
	a->frames += (uint32_t)frames;
	if( a->frames >= (uint32_t)( 2 * s_rate ))
	{
		s_out.rms = sqrt( a->sum2 / ( 2.0 * a->frames ));
		s_out.loud = a->win_max;
		s_out.peak = a->peak;
		s_seq++;
		memset( a, 0, sizeof( *a ));
	}
}

static void AL_APIENTRY Meter_AL( ALshort *samples, ALsizei frames, ALvoid *user )
{
	(void)user;
	Xbox_SndMeter_Block( samples, frames );
}

qboolean Xbox_SndMeter_SDL( void )
{
	return r3d_snd_meter.value != 0.0f && !Xbox_SndAL_Active();
}

void Xbox_SndMeter_Init( void )
{
	Cvar_RegisterVariable( &r3d_snd_meter );
}

void Xbox_SndMeter_Frame( void )
{
	const qboolean al = Xbox_SndAL_Active();
	const qboolean on = r3d_snd_meter.value != 0.0f;
	file_t *f;

	if( !al )
		s_hooked = false; // a closed device dropped the callback with it
	if( on && al && !s_hooked )
	{
		memset( &s_acc, 0, sizeof( s_acc ));
		s_rate = 48000; // the OpenAL output's rate
		alXboxOutputCallback( Meter_AL, NULL );
		s_hooked = true;
	}
	else if( !on && s_hooked )
	{
		alXboxOutputCallback( NULL, NULL ); // back to zero-copy
		s_hooked = false;
	}
	if( on && !al && snd.format.speed > 0 )
		s_rate = snd.format.speed;

	if( s_seq == s_seen )
		return;
	s_seen = s_seq;
	Con_Printf( "snd meter (%s): rms %.1f dBFS, loudest 100 ms %.1f, peak %.1f\n", al ? "OpenAL" : "SDL",
		Meter_dB( s_out.rms ), Meter_dB( s_out.loud ), Meter_dB( (double)s_out.peak ));
	// a new file each boot, then appended to, as sndalprof.txt (xbox_snd_al.c)
	if(( f = FS_Open( "sndmeter.txt", s_file_started ? "a" : "w", true )))
	{
		FS_Printf( f, "%.1f %s rms %.1f loud %.1f peak %.1f", host.realtime, al ? "OpenAL" : "SDL",
			Meter_dB( s_out.rms ), Meter_dB( s_out.loud ), Meter_dB( (double)s_out.peak ));
		if( al )
		{
			// the timed starts and stops the output thread made exact, since
			// the open (sndalprof.txt has the same, but only at a map change)
			ALXBOXSTATS st;

			memset( &st, 0, sizeof( st ));
			st.size = sizeof( st );
			alXboxGetStats( &st );
			FS_Printf( f, " | held %u muted %u refused %u thread %u late %u skipped %u err %.2f..%.2f ms mean %.2f",
				st.timed_holds, st.timed_mutes, st.timed_hold_refused, st.timed_hold_done, st.timed_hold_overdue,
				st.timed_hold_skipped, st.timed_hold_err_min / 48.0, st.timed_hold_err_max / 48.0, st.timed_hold_err_mean / 48.0 );
		}
		FS_Printf( f, "\n" );
		FS_Close( f );
		s_file_started = true;
	}
}
