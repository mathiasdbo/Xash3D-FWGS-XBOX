/*
s_mix_host_test_tu.c - one build of engine/client/sound/s_mix.c for s_mix_host_test.c
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

/* tools/host-tests/run.sh compiles this twice: with XASH_XBOX 0 (upstream's
   mixer loops) and 1 (divergence #145's), each exporting its S_MixAudio
   under the name S_MIX_TEST_ENTRY gives it. */

#include "s_mix.c"

void S_MIX_TEST_ENTRY( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples )
{
	S_MixAudio( pbuf, pvol, buf, channels, width, offset_frac, rate_scale, num_samples, false );
}

#if XASH_XBOX
/* Which of #145 (1)'s two loops a call takes: 1 the 32.32 fixed-point one. */
int s_mix_test_fixed_step( double offset_frac, double rate_scale )
{
	uint32_t frac, step_int, step_frac;

	return S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac );
}
#endif
