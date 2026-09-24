// Unit test for formats::buildDxt1MipChain (PakManager.cpp) - the writer
// whose output size filesystem/mpak_texture_dir.h's Mpak_Dxt1ChainLength
// predicts. Moved out of PakManager.cpp's own anonymous namespace (X9 v10
// follow-up, docs/r3d/fork-plan.md's X9 item 2) specifically so this test
// can call it directly.
//
// Every expected byte count below is a hand-computed literal (worked out
// independently, the same table filesystem/tests/mpak_layout.c's own
// TestDxt1ChainLength uses) - never a value read back from
// Mpak_Dxt1ChainLength itself, even though the test also cross-checks
// against that function as a second, independent assertion.
#include "formats/PakManager.h"
#include "mpak_texture_dir.h"
#include "utils/rgbcx.h"
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// A non-uniform RGBA8 base image: every texel gets its own color/alpha
// derived from its coordinates, so this is never a degenerate flat block
// compressToDXT (or buildDxt1MipChain's own downsample step) could special-case.
std::vector<uint8_t> MakeBaseImage(uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>((x * 37 + y * 11) & 0xFF);
            p[1] = static_cast<uint8_t>((x * 53 + y * 29) & 0xFF);
            p[2] = static_cast<uint8_t>((x * 7 + y * 101) & 0xFF);
            p[3] = static_cast<uint8_t>(128 + ((x + y) & 0x7F)); // never below 128: no accidental all-transparent block
        }
    }
    return rgba;
}

} // namespace

int main() {
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);

    struct Case { uint32_t w, h, log2w, log2h; uint64_t expectedBytes; uint32_t expectedLevels; };
    static const Case cases[] = {
        { 4,   4,  2, 2, 24,   3 },
        { 16,  16, 4, 4, 184,  5 },
        { 64,  4,  6, 2, 264,  7 },
        { 4,   64, 2, 6, 264,  7 },
        { 256, 16, 8, 4, 2824, 9 },
    };

    for (const Case& c : cases) {
        std::vector<uint8_t> base = MakeBaseImage(c.w, c.h);

        auto chain = formats::buildDxt1MipChain(base, c.w, c.h, false);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%ux%u: buildDxt1MipChain size must match the hand-computed chain length", c.w, c.h);
        CHECK(chain.size() == c.expectedBytes, msg);

        uint32_t levels = 0;
        uint64_t predicted = Mpak_Dxt1ChainLength(c.log2w, c.log2h, &levels);
        std::snprintf(msg, sizeof(msg), "%ux%u: buildDxt1MipChain size must match Mpak_Dxt1ChainLength", c.w, c.h);
        CHECK(chain.size() == predicted, msg);
        std::snprintf(msg, sizeof(msg), "%ux%u: Mpak_Dxt1ChainLength level count must match the hand-computed table", c.w, c.h);
        CHECK(levels == c.expectedLevels, msg);
    }

    // masked = true must produce the SAME sizes (punch-through alpha only
    // changes which BC1 mode a block picks, never the block count) - checked
    // for the two most aspect-skewed cases (4x64, 256x16), where a level-count
    // or block-count bug would be most likely to show up.
    {
        std::vector<uint8_t> base64x4 = MakeBaseImage(4, 64);
        auto chain = formats::buildDxt1MipChain(base64x4, 4, 64, true);
        CHECK(chain.size() == 264, "4x64 masked: size must match the unmasked hand-computed chain length");

        std::vector<uint8_t> base256x16 = MakeBaseImage(256, 16);
        auto chain2 = formats::buildDxt1MipChain(base256x16, 256, 16, true);
        CHECK(chain2.size() == 2824, "256x16 masked: size must match the unmasked hand-computed chain length");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
