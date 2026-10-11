// music_resample - see music_resample.h (docs/r3d/divergences.md #173).
#include "music_resample.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace formats {

namespace {

// 2 * kHalfTaps input samples under each output sample.
constexpr int kHalfTaps = 96;
// Kaiser's beta: about 117 dB of stopband for this length, with a
// transition band (Kaiser's estimate, (A - 7.95) / (2.285 * N) radians a
// sample, divided by 2 pi) of 0.0395 of the input rate - 1745 Hz at
// 44.1 kHz.
constexpr double kKaiserBeta = 12.0;
// The -6 dB cutoff as a fraction of the input rate: half-way between
// 20 kHz and 22.05 kHz at 44.1 kHz, so the transition band (about
// 20.15-21.9 kHz) sits inside that gap: everything at or below 20 kHz
// passes and everything from the input's Nyquist frequency up - where
// the images are - is stopped.
constexpr double kCutoff = 21025.0 / 44100.0;
constexpr uint32_t kMaxPhases = 1024;

double besselI0(double x) {
    // the power series, to convergence (x <= kKaiserBeta here)
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

struct Ratio {
    uint32_t L = 0, M = 0; // out / in, reduced
};

Ratio reduce(uint32_t inRate, uint32_t outRate) {
    const uint32_t g = std::gcd(inRate, outRate);
    return {outRate / g, inRate / g};
}

// The L phases' taps: phase p is the filter at input offsets k - p / L,
// k = -kHalfTaps + 1 .. kHalfTaps, normalised to sum to 1.
std::vector<double> buildTable(const Ratio& r) {
    const int taps = 2 * kHalfTaps;
    std::vector<double> t(static_cast<size_t>(r.L) * taps);
    const double i0b = besselI0(kKaiserBeta);
    const double pi = 3.14159265358979323846;
    for (uint32_t p = 0; p < r.L; ++p) {
        double* h = t.data() + static_cast<size_t>(p) * taps;
        const double d = static_cast<double>(p) / r.L;
        double sum = 0.0;
        for (int j = 0; j < taps; ++j) {
            const double x = static_cast<double>(j - kHalfTaps + 1) - d; // input offset from the output's time
            const double u = x / kHalfTaps;
            const double w = std::fabs(u) >= 1.0 ? 0.0 : besselI0(kKaiserBeta * std::sqrt(1.0 - u * u)) / i0b;
            const double a = 2.0 * kCutoff * x;
            const double s = x == 0.0 ? 1.0 : std::sin(pi * a) / (pi * a);
            h[j] = 2.0 * kCutoff * s * w;
            sum += h[j];
        }
        for (int j = 0; j < taps; ++j) h[j] /= sum;
    }
    return t;
}

int threadCount(int requested) {
    if (requested > 0) return requested;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? static_cast<int>(std::min(hw, 16u)) : 4;
}

// Fills out[first, last) for one channel.
void runRange(const double* in, uint32_t frames, const Ratio& r, const std::vector<double>& table, double* out,
              uint32_t first, uint32_t last) {
    const int taps = 2 * kHalfTaps;
    for (uint32_t n = first; n < last; ++n) {
        const uint64_t num = static_cast<uint64_t>(n) * r.M;
        const int64_t i = static_cast<int64_t>(num / r.L);
        const uint32_t p = static_cast<uint32_t>(num % r.L);
        const double* h = table.data() + static_cast<size_t>(p) * taps;
        const int64_t j0 = i - kHalfTaps + 1;
        double acc = 0.0;
        if (j0 >= 0 && j0 + taps <= static_cast<int64_t>(frames)) {
            const double* x = in + j0;
            for (int j = 0; j < taps; ++j) acc += x[j] * h[j];
        } else {
            for (int j = 0; j < taps; ++j) {
                const int64_t k = j0 + j;
                if (k >= 0 && k < static_cast<int64_t>(frames)) acc += in[k] * h[j]; // silence outside the track
            }
        }
        out[n] = acc;
    }
}

} // namespace

uint32_t musicResampledFrames(uint32_t frames, uint32_t inRate, uint32_t outRate) {
    if (!inRate) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(frames) * outRate + inRate - 1) / inRate);
}

bool musicResampleSupported(uint32_t inRate, uint32_t outRate) {
    if (inRate < 8000 || outRate > 48000 || outRate <= inRate) return false;
    return reduce(inRate, outRate).L <= kMaxPhases;
}

std::vector<double> resampleMusicChannel(const double* in, uint32_t frames, uint32_t inRate, uint32_t outRate,
                                         int threads) {
    if (!musicResampleSupported(inRate, outRate)) {
        throw std::invalid_argument("resampleMusicChannel: unsupported rates");
    }
    const Ratio r = reduce(inRate, outRate);
    const std::vector<double> table = buildTable(r);
    const uint32_t outFrames = musicResampledFrames(frames, inRate, outRate);
    std::vector<double> out(outFrames);
    const int n = std::max(1, std::min<int>(threadCount(threads), static_cast<int>(std::max<uint32_t>(1, outFrames / 4096))));
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) {
        const uint32_t first = static_cast<uint32_t>((static_cast<uint64_t>(outFrames) * t) / n);
        const uint32_t last = static_cast<uint32_t>((static_cast<uint64_t>(outFrames) * (t + 1)) / n);
        pool.emplace_back([&, first, last]() { runRange(in, frames, r, table, out.data(), first, last); });
    }
    for (auto& th : pool) th.join();
    return out;
}

std::vector<int16_t> resampleMusic(const int16_t* in, uint32_t frames, uint32_t channels, uint32_t inRate,
                                   uint32_t outRate, int threads, MusicResampleStats* stats) {
    if (channels < 1 || channels > 2) {
        throw std::invalid_argument("resampleMusic: 1 or 2 channels");
    }
    const uint32_t outFrames = musicResampledFrames(frames, inRate, outRate);
    std::vector<int16_t> out(static_cast<size_t>(outFrames) * channels);
    MusicResampleStats st;
    std::vector<double> ch(frames);
    for (uint32_t c = 0; c < channels; ++c) {
        for (uint32_t i = 0; i < frames; ++i) ch[i] = in[static_cast<size_t>(i) * channels + c];
        const std::vector<double> y = resampleMusicChannel(ch.data(), frames, inRate, outRate, threads);
        for (uint32_t i = 0; i < outFrames; ++i) {
            const double v = y[i];
            st.peak = std::max(st.peak, std::fabs(v));
            const double q = std::nearbyint(v); // the default rounding mode: to nearest, ties to even
            int16_t s;
            if (q > 32767.0) {
                s = 32767;
                ++st.clipped;
            } else if (q < -32768.0) {
                s = -32768;
                ++st.clipped;
            } else {
                s = static_cast<int16_t>(q);
            }
            out[static_cast<size_t>(i) * channels + c] = s;
        }
    }
    if (stats) *stats = st;
    return out;
}

} // namespace formats
