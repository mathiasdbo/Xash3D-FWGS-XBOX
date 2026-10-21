/*
xbox_snd_meter.h - a level meter on the final mix (docs/r3d/divergences.md #189)
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
#ifndef XBOX_SND_METER_H
#define XBOX_SND_METER_H

void Xbox_SndMeter_Init( void );
void Xbox_SndMeter_Frame( void ); /* once a frame: hooks OpenAL's output, prints a finished window */
qboolean Xbox_SndMeter_SDL( void ); /* the SDL mix's samples are to be measured */
void Xbox_SndMeter_Block( const short *samples, int frames ); /* interleaved 16-bit stereo */

#endif
