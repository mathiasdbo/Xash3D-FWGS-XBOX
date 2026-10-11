/*
music_adpcm_encode.h - the Xbox ADPCM encoder for baked music
Resonance3D (docs/r3d/divergences.md #171)

The engine's decoder (engine/platform/xbox/xbox_music_adpcm.c) is the only
reconstruction this encoder trusts: every candidate is scored by running
the engine's own table step, so what the baker measures is what the
console plays. C, so the host test can link it next to the decoder.
*/
#ifndef MUSIC_ADPCM_ENCODE_H
#define MUSIC_ADPCM_ENCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoder identity written to the r3dm chunk's encoder_id. Bump when the
   encoder's output changes. */
#define MUSIC_ADPCM_ENCODER_ID 1u

/* Encodes one block. `pcm` holds 65 interleaved frames of `channels`
   channels: the block's 64 and the next block's first (zeros past the
   end of the track), which the unplayed 64th code is aimed at so a
   65-frame decoder would still hear a plausible sample. Writes
   36 * channels bytes. `index_hint` (one per channel) carries the chosen
   step index from block to block; it only orders the search, every index
   is tried. Returns the block's squared error over its 64 played frames. */
uint64_t Music_AdpcmEncodeBlock( const int16_t *pcm, int channels, int *index_hint, uint8_t *out );

#ifdef __cplusplus
}
#endif

#endif
