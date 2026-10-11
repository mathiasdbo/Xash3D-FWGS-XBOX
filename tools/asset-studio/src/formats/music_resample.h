#pragma once
// music_resample - the music bake's sample-rate converter
// (docs/r3d/divergences.md #173, X12 step 2).
//
// The console mixes at 48 kHz (r3d_snd_rate). A 44.1 kHz track played
// there goes through upstream's raw-channel copy, S_RawSamplesStereo
// (engine/client/sound/s_main.c), which point-samples: each 48 kHz
// output takes the nearest earlier 44.1 kHz input, so the track's
// images fold back into the audible band. The baker resamples each track
// once, offline, to the mix rate instead, and the engine then copies it
// at a step of exactly 1.
//
// THE FILTER. A polyphase windowed-sinc interpolator for any rational
// ratio out/in = L/M (44100 -> 48000: L = 160, M = 147): output frame n
// is the input band-limited and evaluated at input time n * M / L, so
// output frame 0 is input frame 0 (no delay) and a source position p is
// output time p * L / M: the first output frame at or after it is
// ceil(p * L / M), the engine's Xbox_MusicXwav_ToFileFrames. Each of the
// L phases holds 2 * kHalfTaps
// taps of a Kaiser-windowed sinc (beta kKaiserBeta) whose -6 dB cutoff is
// half-way between kPassbandHz and the input's Nyquist frequency, and
// each phase is normalised to unit DC gain (no ripple at L / M of the
// output rate). Measured by tests/music_resample_test.cpp, on doubles,
// 44100 -> 48000: passband (<= 20 kHz) gain within 0.0005 dB; every
// image and alias of a tone anywhere in 0-22.05 kHz at least 110 dB
// under it; a tone in the band reconstructed to within -110 dB of the
// ideal sine. The int16 output is rounded to nearest; a sample past full
// scale (the band-limited waveform can overshoot a clipped source) is
// clamped and counted.
//
// Computed in double, one output sample at a time from its own inputs,
// so the result does not depend on the number of threads.

#include <cstdint>
#include <vector>

namespace formats {

// r3dm's resampler_id for this filter (0: none, the source's rate).
constexpr uint16_t kMusicResamplerId = 1;

struct MusicResampleStats {
    uint32_t clipped = 0;   // int16 samples clamped at full scale
    double peak = 0.0;      // the largest |sample| before clamping, in int16 units
};

// The output length for `frames` input frames: ceil(frames * out / in),
// the frames whose input time n * in / out is before the input's end.
// It is the engine's Xbox_MusicXwav_ToFileFrames of the source length.
uint32_t musicResampledFrames(uint32_t frames, uint32_t inRate, uint32_t outRate);

// Whether the converter takes this pair: 8000-48000 Hz, outRate above
// inRate (the bake only ever raises a track to the mix rate), and a
// reduced ratio with at most 1024 phases.
bool musicResampleSupported(uint32_t inRate, uint32_t outRate);

// One channel, in doubles (the tests' entry point): `in` has `frames`
// samples; returns musicResampledFrames(...) samples.
std::vector<double> resampleMusicChannel(const double* in, uint32_t frames, uint32_t inRate, uint32_t outRate,
                                         int threads);

// Interleaved int16, `channels` 1 or 2. Throws std::invalid_argument for
// an unsupported pair.
std::vector<int16_t> resampleMusic(const int16_t* in, uint32_t frames, uint32_t channels, uint32_t inRate,
                                   uint32_t outRate, int threads, MusicResampleStats* stats);

} // namespace formats
