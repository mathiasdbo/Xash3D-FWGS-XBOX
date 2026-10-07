/*
nv2a_shot_util.h - the savegame thumbnail's 2x2 pixel average, packed
Resonance3D (docs/r3d/divergences.md #153; the half-size read is #139)

VID_ScreenShot (r_context.c) reads a VID_MINISHOT at half size, each
pixel the rounded average of a 2x2 block of the back buffer's B,G,R,X
pixels, alpha forced to 255. Upstream's per-channel loop computes, for
k = B, G, R, ( sum of the four bytes + 2 ) >> 2. This does the same sums
two channels at a time: B and R sit in bytes 0 and 2, so masking a pixel
with 0x00FF00FF leaves each in a 16-bit lane whose four-pixel sum (at
most 1020) plus 2 cannot carry into the next lane; shifting the packed
sum right by two and masking again leaves ( sum + 2 ) >> 2 in bytes 0
and 2 - the bits a lane shifts into its neighbour are the ones the mask
clears. G (byte 1) is the low lane of the same sum over the pixels
shifted down a byte. Bit-exact with the per-channel loop
(tools/host-tests/nv2a_shot_util_host_test.c checks it).
*/
#ifndef NV2A_SHOT_UTIL_H
#define NV2A_SHOT_UTIL_H

#include <stdint.h>

static inline uint32_t nv2a_shot_avg4( uint32_t a, uint32_t b, uint32_t c, uint32_t d )
{
	const uint32_t m = 0x00FF00FFu;
	const uint32_t br = (( a & m ) + ( b & m ) + ( c & m ) + ( d & m ) + 0x00020002u ) >> 2 & m;
	const uint32_t g = (((( a >> 8 ) & m ) + (( b >> 8 ) & m ) + (( c >> 8 ) & m ) + (( d >> 8 ) & m ) + 0x00020002u ) >> 2 ) & 0xFFu;

	return br | ( g << 8 ) | 0xFF000000u;
}

#endif /* NV2A_SHOT_UTIL_H */
