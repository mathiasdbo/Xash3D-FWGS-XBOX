/*
xbox_s_mix_pitch.h - the mixer's pitched (resampling) loops without x87 float-to-int conversions
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
Resonance3D (docs/r3d/divergences.md #145 (1)). Included by
engine/client/sound/s_mix.c only, in place of upstream's four pitched-loop
instantiations, so it defines the same four functions with the same
signatures and S_MixAudio's dispatch is upstream's.

Upstream's loops (S_MakeMixMonoPitch / S_MakeMixStereoPitch in s_mix.c)
advance a double `offset_frac` and truncate it, `(uint)offset_frac`, on
every output sample. The Pentium III has no SSE2, so clang emits that as
fnstcw/fldcw/fistp/fldcw and a 64-bit reload of two 32-bit stores:
several times the cost of the sample's integer work. These produce
upstream's exact source-index sequence, so the paint buffer is the same:

- S_MixFixedStep: when offset_frac and rate_scale are multiples of 2^-32
  (rate_scale < 2^20), every step of upstream's double arithmetic is
  exact (a sum of two such values below 2^21 fits a 53-bit significand,
  and so do the truncation and the subtraction), so a 32.32 fixed-point
  counter takes the same steps. The engine's pitch is a float
  (VOX_ModifyPitch, the sys_timescale multiplier), so at an 11, 22 or
  44 kHz source rate the rate is that float times 1/4, 1/2 or 1 and the
  channel's position stays on the grid: nearly every sound in the game.
  A step of exactly 1/2, 1/4, 1/8 or 1/16 (11 and 22 kHz at PITCH_NORM,
  the commonest case) takes S_MIX_RUNS below: the same index sequence
  with one multiply per source sample instead of one per output.
- otherwise (8/16/32/48 kHz files, time-compressed VOX words) upstream's
  double add, then for a sum below 2 one subtraction of 1.0 when it is
  at least 1: x - 1 is exact for 1 <= x < 2, so this is x - floor(x) and
  floor(x), what upstream computes. A sum of 2 or more takes upstream's
  own truncation. Under the Xbox kernel the x87 control word is 0x027F
  (53-bit precision, round to nearest; read in xemu, which boots the real
  kernel), so a value in a register rounds as one in memory; the only
  rounding in this loop is the add, which upstream performs identically.

tools/host-tests/s_mix_host_test.c compares these with upstream's loops,
built from the same s_mix.c, on the same calls.
*/

static qboolean S_MixFixedStep( double offset_frac, double rate_scale, uint32_t *frac, uint32_t *step_int, uint32_t *step_frac )
{
	const double f = offset_frac * 4294967296.0; // a power-of-two scale: exact
	const double r = rate_scale * 4294967296.0;

	if( !( f >= 0.0 && f < 4294967296.0 && r >= 0.0 && r < 4503599627370496.0 )) // r < 2^52
		return false;

	const int64_t fi = (int64_t)f;
	const int64_t ri = (int64_t)r;

	// not on the 2^-32 grid: upstream's arithmetic may round, so at
	// upstream's 44.1 kHz the double loop below keeps its exact result.
	// Mixing at 48 kHz (divergence #173) no sound is on the grid - 11025
	// over 48000 is 147/640 - and every channel would take the double
	// loop and none the runs; there the position and the step are rounded
	// down to it instead (the casts above truncate non-negative values).
	// Each output then reads the sample its exact position names, except
	// where that position lies on an integer or less than the call's
	// accumulated rounding (num_samples * 2^-32 of a sample at most) past
	// one, where it reads the one before; it never reads past the last
	// sample the exact positions reach, so S_MixChannelToBuffer's
	// out_count stays a bound (tools/host-tests/s_mix_host_test.c counts
	// the outputs that differ from upstream's double loop).
	if(( (double)fi != f || (double)ri != r ) && SOUND_DMA_SPEED == SOUND_44k )
		return false;

	*frac = (uint32_t)fi;
	*step_int = (uint32_t)( ri >> 32 );
	*step_frac = (uint32_t)ri;
	return true;
}

/* The run path of the fixed-point loop: a step of exactly 1/2, 1/4, 1/8 or
   1/16 (step_int 0, step_frac a power of two from 2^28) feeds each source
   sample to a run of outputs. The counter carries after the first
   ceil( ( 2^32 - frac ) / step_frac ) outputs, then after every
   2^32 / step_frac, so the index sequence is the general loop's with one
   multiply per source sample instead of one per output. Runs of 2 and 4
   (22 and 11 kHz at PITCH_NORM) are unrolled. Expands in the enclosing
   function, on its locals; LO/RO are the left/right sample offsets and
   ST the source step (mono 0, 0, 1; stereo 0, 1, 2). Returns from the
   function when it mixed the call. */
#define S_MIX_RUN_ADD( i ) \
	do { pbuf[i].left += l; pbuf[i].right += r; } while( 0 )
#define S_MIX_RUN_LR( x, LO, RO ) \
	const int l = ( S_SAMPLE ## x( data[sample_idx + ( LO )] ) * lvol ) >> ( x - 8 ); \
	const int r = ( S_SAMPLE ## x( data[sample_idx + ( RO )] ) * rvol ) >> ( x - 8 )
#define S_MIX_RUNS( x, LO, RO, ST ) \
	if( step_int == 0 && step_frac >= 0x10000000u && ( step_frac & ( step_frac - 1 )) == 0 ) \
	{ \
		const uint shift = (uint)__builtin_ctz( step_frac ); \
		const int reps = (int)( 1u << ( 32 - shift )); \
		const int head = (int)(((( (uint64_t)1 ) << 32 ) - frac + step_frac - 1 ) >> shift ); \
		int i = 0; \
		if( head < reps ) /* the first source sample's run is short */ \
		{ \
			S_MIX_RUN_LR( x, LO, RO ); \
			const int n = head < num_samples ? head : num_samples; \
			for( ; i < n; i++ ) \
				S_MIX_RUN_ADD( i ); \
			sample_idx += ( ST ); \
		} \
		if( reps == 2 ) \
		{ \
			for( ; i + 2 <= num_samples; i += 2, sample_idx += ( ST )) \
			{ \
				S_MIX_RUN_LR( x, LO, RO ); \
				S_MIX_RUN_ADD( i ); \
				S_MIX_RUN_ADD( i + 1 ); \
			} \
		} \
		else if( reps == 4 ) \
		{ \
			for( ; i + 4 <= num_samples; i += 4, sample_idx += ( ST )) \
			{ \
				S_MIX_RUN_LR( x, LO, RO ); \
				S_MIX_RUN_ADD( i ); \
				S_MIX_RUN_ADD( i + 1 ); \
				S_MIX_RUN_ADD( i + 2 ); \
				S_MIX_RUN_ADD( i + 3 ); \
			} \
		} \
		else \
		{ \
			for( ; i + reps <= num_samples; sample_idx += ( ST )) \
			{ \
				S_MIX_RUN_LR( x, LO, RO ); \
				for( const int e = i + reps; i < e; i++ ) \
					S_MIX_RUN_ADD( i ); \
			} \
		} \
		if( i < num_samples ) /* the last source sample's run, cut by the buffer's end */ \
		{ \
			S_MIX_RUN_LR( x, LO, RO ); \
			for( ; i < num_samples; i++ ) \
				S_MIX_RUN_ADD( i ); \
		} \
		return; \
	}

#define S_MakeMixMonoPitchXbox( x ) \
	static void S_MixMonoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int lvol = volume[0], rvol = volume[1]; \
		uint sample_idx = 0; \
		uint32_t frac, step_int, step_frac; \
		if( S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac )) \
		{ \
			S_MIX_RUNS( x, 0, 0, 1 ); \
			for( int i = 0; i < num_samples; i++ ) \
			{ \
				const int s = S_SAMPLE ## x( data[sample_idx] ); \
				pbuf[i].left  += ( s * lvol ) >> ( x - 8 ); \
				pbuf[i].right += ( s * rvol ) >> ( x - 8 ); \
				frac += step_frac; \
				sample_idx += step_int + ( frac < step_frac ); /* the carry out of the fraction */ \
			} \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( S_SAMPLE ## x( data[sample_idx] ) * lvol ) >> ( x - 8 ); \
			pbuf[i].right += ( S_SAMPLE ## x( data[sample_idx] ) * rvol ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			if( offset_frac >= 2.0 ) \
			{ \
				sample_idx += (uint)offset_frac; \
				offset_frac -= (uint)offset_frac; \
			} \
			else if( offset_frac >= 1.0 ) \
			{ \
				offset_frac -= 1.0; \
				sample_idx++; \
			} \
		} \
	} \

#define S_MakeMixStereoPitchXbox( x ) \
	static void S_MixStereoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int lvol = volume[0], rvol = volume[1]; \
		uint sample_idx = 0; \
		uint32_t frac, step_int, step_frac; \
		if( S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac )) \
		{ \
			S_MIX_RUNS( x, 0, 1, 2 ); \
			for( int i = 0; i < num_samples; i++ ) \
			{ \
				pbuf[i].left  += ( S_SAMPLE ## x( data[sample_idx+0] ) * lvol ) >> ( x - 8 ); \
				pbuf[i].right += ( S_SAMPLE ## x( data[sample_idx+1] ) * rvol ) >> ( x - 8 ); \
				frac += step_frac; \
				sample_idx += ( step_int + ( frac < step_frac )) << 1; \
			} \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( S_SAMPLE ## x( data[sample_idx+0] ) * lvol ) >> ( x - 8 ); \
			pbuf[i].right += ( S_SAMPLE ## x( data[sample_idx+1] ) * rvol ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			if( offset_frac >= 2.0 ) \
			{ \
				sample_idx += (uint)offset_frac << 1; \
				offset_frac -= (uint)offset_frac; \
			} \
			else if( offset_frac >= 1.0 ) \
			{ \
				offset_frac -= 1.0; \
				sample_idx += 2; \
			} \
		} \
	} \

/* docs/r3d/design/x9-audio-apu-voices.md section 3: the cache keeps 8-bit
   sounds unsigned, so the two unpitched 8-bit loops are upstream's
   (S_MakeMixMono / S_MakeMixStereo) with S_SAMPLE8's read; s_mix.c
   instantiates only their 16-bit forms on the Xbox. Every loop here reads
   through S_SAMPLE8 / S_SAMPLE16 for the same reason. */
static void S_MixMono8( portable_samplepair_t *pbuf, const int *volume, const void *buf, int num_samples )
{
	const int8_t *data = buf;
	for( int i = 0; i < num_samples; i++ )
	{
		pbuf[i].left  += S_SAMPLE8( data[i] ) * volume[0];
		pbuf[i].right += S_SAMPLE8( data[i] ) * volume[1];
	}
}

static void S_MixStereo8( portable_samplepair_t *pbuf, const int *volume, const void *buf, int num_samples )
{
	const int8_t *data = buf;
	for( int i = 0; i < num_samples; i++ )
	{
		pbuf[i].left  += S_SAMPLE8( data[i * 2 + 0] ) * volume[0];
		pbuf[i].right += S_SAMPLE8( data[i * 2 + 1] ) * volume[1];
	}
}

S_MakeMixMonoPitchXbox( 8 )
S_MakeMixMonoPitchXbox( 16 )
S_MakeMixStereoPitchXbox( 8 )
S_MakeMixStereoPitchXbox( 16 )

/*
Divergence #174: upstream's linear-interpolation loops (S_MakeMixMonoLerp /
S_MakeMixStereoLerp in s_mix.c, taken when s_lerping is set) on the same
32.32 counter as the pitched loops above, so they read the same source
indices: `(int)( a * ( 1.0 - f ) + b * f )` in doubles is two x87
conversions an output on the Pentium III, and the weight here is the
counter's top fraction bits instead.

- 16-bit: a + ( ( b - a ) * ( frac >> 17 )) >> 15. |b - a| <= 65535 and
  the weight is below 2^15, so the product fits an int.
- 8-bit: interpolated with 8 more bits, a * 256 + ( ( b - a ) * ( frac
  >> 16 )) >> 8, and mixed with `>> 8` where upstream mixes the 8-bit value
  unshifted: the same scale, without truncating the interpolated sample
  back to 8 bits first.

Both round toward minus infinity where upstream's cast truncates toward
zero; the interpolated sample differs from upstream's by at most 3
(16-bit: the 15-bit weight's error plus the rounding) or one 8-bit step
(tools/host-tests/s_mix_host_test.c). Off the fixed
point grid (only at upstream's 44.1 kHz, or a step of 2^20 samples or
more: S_MixFixedStep) these run upstream's double loop unchanged.
*/
#define S_LERP_WEIGHT16( frac ) ((int)(( frac ) >> 17 ))
#define S_LERP_WEIGHT8( frac )  ((int)(( frac ) >> 16 ))
#define S_LERP16( a, b, frac ) (( a ) + ((( (b) - (a) ) * S_LERP_WEIGHT16( frac )) >> 15 ))
#define S_LERP8( a, b, frac )  ((( a ) * 256 ) + ((( (b) - (a) ) * S_LERP_WEIGHT8( frac )) >> 8 ))

#define S_MakeMixMonoLerpXbox( x ) \
	static void S_MixMonoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int lvol = volume[0], rvol = volume[1]; \
		uint sample_idx = 0; \
		uint32_t frac, step_int, step_frac; \
		if( S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac )) \
		{ \
			for( int i = 0; i < num_samples; i++ ) \
			{ \
				const int s = S_LERP ## x( S_SAMPLE ## x( data[sample_idx] ), S_SAMPLE ## x( data[sample_idx + 1] ), frac ); \
				pbuf[i].left  += ( s * lvol ) >> 8; \
				pbuf[i].right += ( s * rvol ) >> 8; \
				frac += step_frac; \
				sample_idx += step_int + ( frac < step_frac ); \
			} \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int s = (int)( S_SAMPLE ## x( data[sample_idx] ) * ( 1.0 - offset_frac ) + S_SAMPLE ## x( data[sample_idx + 1] ) * offset_frac ); \
			pbuf[i].left  += ( s * lvol ) >> ( x - 8 ); \
			pbuf[i].right += ( s * rvol ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

#define S_MakeMixStereoLerpXbox( x ) \
	static void S_MixStereoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int lvol = volume[0], rvol = volume[1]; \
		uint sample_idx = 0; \
		uint32_t frac, step_int, step_frac; \
		if( S_MixFixedStep( offset_frac, rate_scale, &frac, &step_int, &step_frac )) \
		{ \
			for( int i = 0; i < num_samples; i++ ) \
			{ \
				const int sl = S_LERP ## x( S_SAMPLE ## x( data[sample_idx + 0] ), S_SAMPLE ## x( data[sample_idx + 2] ), frac ); \
				const int sr = S_LERP ## x( S_SAMPLE ## x( data[sample_idx + 1] ), S_SAMPLE ## x( data[sample_idx + 3] ), frac ); \
				pbuf[i].left  += ( sl * lvol ) >> 8; \
				pbuf[i].right += ( sr * rvol ) >> 8; \
				frac += step_frac; \
				sample_idx += ( step_int + ( frac < step_frac )) << 1; \
			} \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int sl = (int)( S_SAMPLE ## x( data[sample_idx + 0] ) * ( 1.0 - offset_frac ) + S_SAMPLE ## x( data[sample_idx + 2] ) * offset_frac ); \
			int sr = (int)( S_SAMPLE ## x( data[sample_idx + 1] ) * ( 1.0 - offset_frac ) + S_SAMPLE ## x( data[sample_idx + 3] ) * offset_frac ); \
			pbuf[i].left  += ( sl * lvol ) >> ( x - 8 ); \
			pbuf[i].right += ( sr * rvol ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac << 1; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

S_MakeMixMonoLerpXbox( 8 )
S_MakeMixMonoLerpXbox( 16 )
S_MakeMixStereoLerpXbox( 8 )
S_MakeMixStereoLerpXbox( 16 )

/* r3d_mixbench (engine/platform/xbox/xbox_s_mix_bench.c, #174) times the
   loops above as the XBE compiled them: mono 8, mono 16, stereo 8,
   stereo 16. */
typedef void ( *s_r3d_mix_loop_t )( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples );
void S_R3D_MixLoops( s_r3d_mix_loop_t pitch[4], s_r3d_mix_loop_t lerp[4] );
void S_R3D_MixLoops( s_r3d_mix_loop_t pitch[4], s_r3d_mix_loop_t lerp[4] )
{
	pitch[0] = S_MixMonoPitch8;
	pitch[1] = S_MixMonoPitch16;
	pitch[2] = S_MixStereoPitch8;
	pitch[3] = S_MixStereoPitch16;
	lerp[0] = S_MixMonoLerp8;
	lerp[1] = S_MixMonoLerp16;
	lerp[2] = S_MixStereoLerp8;
	lerp[3] = S_MixStereoLerp16;
}
