// music_resample_test - the music bake's offline resampler (docs/r3d/divergences.md #173).
//
// Measures formats::resampleMusicChannel, 44100 -> 48000 Hz (and two
// other source rates), on synthetic signals:
// - lengths: ceil(frames * out / in), the engine's ToFileFrames;
// - frequency response: a tone anywhere in 20 Hz - 20 kHz comes out at
//   unit gain within 0.0005 dB and zero phase, and what is left after
//   removing the fitted sine - images and aliases of the tone, ripple - is
//   at least 110 dB under it;
// - the transition band: at 21.025 kHz (the cutoff) about -6 dB, and for
//   tones above it the leftover is still 110 dB under a full tone;
// - alignment: an impulse at input frame j peaks at output frame
//   round(j * 160 / 147); a constant stays exactly constant;
// - the int16 path: the double path rounded, clipping counted, and the
//   result independent of the thread count.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "formats/music_resample.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

const double kPi = 3.14159265358979323846;

struct ToneFit {
    double gainDb = 0.0;     // the fitted sine's amplitude, dB re the input's
    double phase = 0.0;      // its phase error, radians
    double residualDb = 0.0; // what is left, dB re a full (unit) tone
};

// A unit tone at `f` Hz, `seconds` long, resampled; the output's middle
// (away from the ends' zero padding) fitted with a*sin + b*cos at the
// ideal output time n / outRate.
ToneFit tone(double f, uint32_t inRate, uint32_t outRate, double seconds = 1.0) {
    const uint32_t frames = static_cast<uint32_t>(inRate * seconds);
    std::vector<double> x(frames);
    const double phi = 0.3;
    for (uint32_t i = 0; i < frames; ++i) x[i] = std::sin(2.0 * kPi * f * i / inRate + phi);
    const std::vector<double> y = formats::resampleMusicChannel(x.data(), frames, inRate, outRate, 3);
    const size_t margin = 1000;
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t n = margin; n + margin < y.size(); ++n) {
        const double t = 2.0 * kPi * f * n / outRate + phi;
        const double s = std::sin(t), c = std::cos(t);
        ss += s * s; sc += s * c; cc += c * c; ys += y[n] * s; yc += y[n] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double res = 0, ref = 0;
    for (size_t n = margin; n + margin < y.size(); ++n) {
        const double t = 2.0 * kPi * f * n / outRate + phi;
        const double e = y[n] - (a * std::sin(t) + b * std::cos(t));
        res += e * e;
        ref += std::sin(t) * std::sin(t);
    }
    ToneFit r;
    r.gainDb = 20.0 * std::log10(std::sqrt(a * a + b * b));
    r.phase = std::atan2(b, a);
    r.residualDb = res > 0 ? 10.0 * std::log10(res / ref) : -400.0;
    return r;
}

} // namespace

int main() {
    // lengths: the engine's Xbox_MusicXwav_ToFileFrames of the source
    CHECK(formats::musicResampledFrames(0, 44100, 48000) == 0);
    CHECK(formats::musicResampledFrames(1, 44100, 48000) == 2);
    CHECK(formats::musicResampledFrames(147, 44100, 48000) == 160);
    CHECK(formats::musicResampledFrames(148, 44100, 48000) == 162);
    CHECK(formats::musicResampledFrames(44100, 44100, 48000) == 48000);
    CHECK(formats::musicResampleSupported(44100, 48000) && formats::musicResampleSupported(22050, 48000) &&
          formats::musicResampleSupported(11025, 48000));
    CHECK(!formats::musicResampleSupported(48000, 44100) && !formats::musicResampleSupported(44100, 44100) &&
          !formats::musicResampleSupported(44100, 96000));
    {
        bool threw = false;
        try {
            int16_t s = 0;
            formats::resampleMusic(&s, 1, 1, 48000, 44100, 1, nullptr);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // the passband: 20 Hz - 20 kHz
    double worstGain = 0.0, worstResidual = -400.0;
    for (double f : {20.0, 100.0, 1000.0, 4567.0, 10000.0, 15000.0, 18000.0, 19000.0, 19800.0, 20000.0}) {
        const ToneFit r = tone(f, 44100, 48000);
        worstGain = std::max(worstGain, std::fabs(r.gainDb));
        worstResidual = std::max(worstResidual, r.residualDb);
        CHECK(std::fabs(r.gainDb) < 0.0005);
        CHECK(std::fabs(r.phase) < 1e-5);
        CHECK(r.residualDb < -110.0);
    }
    std::printf("passband 20 Hz-20 kHz: worst gain error %.6f dB, worst leftover %.1f dB\n", worstGain, worstResidual);

    // the transition band and above: the gain falls, and whatever passes
    // is the tone itself - images and aliases stay 110 dB under a full one
    const ToneFit cut = tone(21025.0, 44100, 48000);
    CHECK(cut.gainDb > -7.0 && cut.gainDb < -5.0);
    for (double f : {20500.0, 21025.0, 21500.0, 21900.0, 22000.0, 22040.0}) {
        const ToneFit r = tone(f, 44100, 48000);
        std::printf("  %.0f Hz: gain %.2f dB, leftover %.1f dB\n", f, r.gainDb, r.residualDb);
        CHECK(r.residualDb < -110.0);
    }
    CHECK(tone(22040.0, 44100, 48000).gainDb < -100.0); // the input's Nyquist edge is stopped

    // other source rates: the cutoff scales with the input rate
    {
        const ToneFit a = tone(4000.0, 11025, 48000, 2.0), b = tone(9000.0, 22050, 48000);
        CHECK(std::fabs(a.gainDb) < 0.0005 && a.residualDb < -110.0);
        CHECK(std::fabs(b.gainDb) < 0.0005 && b.residualDb < -110.0);
    }

    // alignment: output frame n is input time n * 147 / 160
    {
        std::vector<double> x(20000, 0.0);
        x[5000] = 1.0;
        const std::vector<double> y = formats::resampleMusicChannel(x.data(), 20000, 44100, 48000, 2);
        size_t peak = 0;
        for (size_t n = 1; n < y.size(); ++n) {
            if (std::fabs(y[n]) > std::fabs(y[peak])) peak = n;
        }
        CHECK(peak == 5442); // 5000 * 160 / 147 = 5442.18
        // the response is the kernel sampled where each output falls: at
        // the peak (input time 4999.83) a little under its centre value,
        // twice the cutoff (2 * 21025 / 44100 = 0.9535)
        CHECK(y[peak] > 0.85 && y[peak] < 2.0 * 21025.0 / 44100.0);
    }

    // a constant stays exactly constant away from the ends
    {
        std::vector<double> x(10000, 1234.5);
        const std::vector<double> y = formats::resampleMusicChannel(x.data(), 10000, 44100, 48000, 4);
        double worst = 0.0;
        for (size_t n = 300; n + 300 < y.size(); ++n) worst = std::max(worst, std::fabs(y[n] - 1234.5));
        CHECK(worst < 1e-9);
    }

    // int16: the double path rounded to nearest, per channel; the thread
    // count does not change a sample; full scale is clamped and counted
    {
        const uint32_t frames = 30000;
        std::vector<int16_t> st(frames * 2);
        std::vector<double> l(frames), r(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            l[i] = std::lrint(12000.0 * std::sin(2.0 * kPi * 440.0 * i / 44100));
            r[i] = std::lrint(9000.0 * std::sin(2.0 * kPi * 3000.0 * i / 44100 + 1.0));
            st[2 * i] = static_cast<int16_t>(l[i]);
            st[2 * i + 1] = static_cast<int16_t>(r[i]);
        }
        formats::MusicResampleStats s1, s7;
        const std::vector<int16_t> a = formats::resampleMusic(st.data(), frames, 2, 44100, 48000, 1, &s1);
        const std::vector<int16_t> b = formats::resampleMusic(st.data(), frames, 2, 44100, 48000, 7, &s7);
        CHECK(a == b && s1.clipped == 0 && s7.clipped == 0);
        CHECK(a.size() == static_cast<size_t>(formats::musicResampledFrames(frames, 44100, 48000)) * 2);
        const std::vector<double> dl = formats::resampleMusicChannel(l.data(), frames, 44100, 48000, 1);
        const std::vector<double> dr = formats::resampleMusicChannel(r.data(), frames, 44100, 48000, 1);
        bool same = true;
        for (size_t n = 0; n < dl.size(); ++n) {
            same = same && a[2 * n] == static_cast<int16_t>(std::nearbyint(dl[n])) &&
                   a[2 * n + 1] == static_cast<int16_t>(std::nearbyint(dr[n]));
        }
        CHECK(same);

        // a full-scale square overshoots (Gibbs): clamped, counted
        std::vector<int16_t> sq(frames);
        for (uint32_t i = 0; i < frames; ++i) sq[i] = ((i / 50) & 1) ? -32768 : 32767;
        formats::MusicResampleStats sc;
        const std::vector<int16_t> c = formats::resampleMusic(sq.data(), frames, 1, 44100, 48000, 2, &sc);
        CHECK(sc.clipped > 0 && sc.peak > 32767.0);
        std::printf("full-scale square: %u of %zu samples clamped, peak %.0f\n", sc.clipped, c.size(), sc.peak);
    }

    if (failures) {
        std::printf("music_resample_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("music_resample_test: all passed\n");
    return 0;
}
