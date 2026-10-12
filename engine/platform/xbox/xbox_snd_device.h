/*
xbox_snd_device.h - the sound subsystem's teardown when a rate switch loses the device
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
Resonance3D (docs/r3d/divergences.md #173). r3d_snd_rate's reopen closes
the device (SNDDMA_Shutdown, which clears snd.initialized) and opens it
again. If neither the new rate nor the old one opens, the device is gone
but the subsystem - its commands, the client's sound hooks, the sounds,
the raw channels, the DSP, sndpool - is still up, and upstream's
S_Shutdown returns at once on !snd.initialized, so none of it would ever
be freed. The reopen then unwinds the subsystem on the spot through
S_Shutdown itself, with snd.initialized set again so its guarded steps
(S_FreeSounds, S_StopAllSounds) run; it skips the device's own shutdown
(already done) and clears snd.initialized at the end. Afterwards the state is the one
upstream's S_Init leaves when SNDDMA_Init fails at boot: snd.initialized
false and sndpool freed, which every sound entry point already handles;
a later S_Shutdown returns at once, so nothing is freed twice.

The two decisions S_Shutdown makes, as functions of that state, so
tools/host-tests/xbox_snd_device_host_test.c can drive the same rules
through a model of the reopen and the shutdown.
*/
#ifndef XBOX_SND_DEVICE_H
#define XBOX_SND_DEVICE_H

/* S_Shutdown tears the subsystem down: it is initialised, or a reopen
   lost its device with the subsystem still up */
static inline int Xbox_SndShutdown_Runs( int initialized, int device_lost )
{
	return initialized || device_lost;
}

/* ... and closes the device itself only when it still has one */
static inline int Xbox_SndShutdown_ClosesDevice( int device_lost )
{
	return !device_lost;
}

#endif /* XBOX_SND_DEVICE_H */
