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

/* The test hands both builds upstream's signed 8-bit samples. The Xbox
   build's cache keeps them unsigned (docs/r3d/design/x9-audio-apu-voices.md
   section 3), so its entries give its mixer the same bytes XORed with 0x80,
   as the Xbox loader leaves them: the outputs must then be upstream's. */
#if XASH_XBOX
static uint8_t s_mix_test_u8[1 << 17];

static const void *S_MixTestUnsigned( const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples )
{
	size_t bytes = ((size_t)( offset_frac + rate_scale * num_samples ) + 4 ) * (size_t)channels;

	if( width != 1 )
		return buf;
	if( bytes > sizeof( s_mix_test_u8 ))
		bytes = sizeof( s_mix_test_u8 );
	for( size_t i = 0; i < bytes; i++ )
		s_mix_test_u8[i] = ((const uint8_t *)buf )[i] ^ 0x80;
	return s_mix_test_u8;
}

static void S_MixTestFlipCache( channel_t *chan )
{
	wavdata_t *wav = chan->sfx->cache;

	if( wav->width == 1 )
	{
		for( size_t i = 0; i < (size_t)wav->samples * wav->channels; i++ )
			wav->buffer[i] ^= 0x80;
	}
}
#else
#define S_MixTestUnsigned( buf, channels, width, offset_frac, rate_scale, num_samples ) ( buf )
#define S_MixTestFlipCache( chan ) ((void)0)
#endif

void S_MIX_TEST_ENTRY( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples )
{
	buf = S_MixTestUnsigned( buf, channels, width, offset_frac, rate_scale, num_samples );
	S_MixAudio( pbuf, pvol, buf, channels, width, offset_frac, rate_scale, num_samples, false );
}

/* The same with s_lerping's interpolation (divergence #174). */
void S_MIX_TEST_ENTRY_LERP( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, double offset_frac, double rate_scale, int num_samples )
{
	buf = S_MixTestUnsigned( buf, channels, width, offset_frac, rate_scale, num_samples );
	S_MixAudio( pbuf, pvol, buf, channels, width, offset_frac, rate_scale, num_samples, true );
}

/* A whole channel through S_MixChannelToBuffer (divergence #173: a sound
   plays for the same time and at the same pitch at either mix rate). */
int S_MIX_TEST_CHANNEL( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch )
{
	int n;

	S_MixTestFlipCache( chan );
	n = S_MixChannelToBuffer( pbuf, chan, num_samples, out_rate, pitch, 0, 0 );
	S_MixTestFlipCache( chan );
	return n;
}

#if XASH_XBOX
/* Which of #145 (1)'s two loops a call takes: 1 the 32.32 fixed-point one. */
int s_mix_test_fixed_step( double offset_frac, double rate_scale )
{
	uint32_t frac, step_int, step_frac;

	return S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac );
}
#endif
