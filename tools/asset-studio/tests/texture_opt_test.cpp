// Unit test for TextureOpt::compressToDXT's punch-through alpha path
// (TextureOpt.cpp), specifically the invariant fixOpaqueTexelsPunchedThrough()
// exists to guarantee: a real transparent-source texel (alpha < 128) must
// always decode to BC1 selector 3, and a real opaque-source texel (alpha
// >= 128) must never decode to selector 3, regardless of how rgbcx's own
// block-wide least-squares fit would otherwise place it.
//
// This pins the exact defect asset-pipeline-specialist review found and
// codex-review round 2 refused to accept as an undocumented residual (see
// docs/r3d/fork-plan.md's XB section): a block containing both real
// transparency and a real opaque texel whose color is far from the fitted
// endpoints (nearer the free (0,0,0,0) transparent anchor than either
// endpoint) can get that opaque texel misclassified as transparent unless
// corrected.
#include "utils/TextureOpt.h"
#include "utils/rgbcx.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Decodes a single 8-byte BC1 block's mode (3-color vs 4-color) and its
// 16 2-bit selectors, matching the byte layout rgbcx.h's bc1_block uses
// (low color LE u16, high color LE u16, then 16 selectors packed LSB-first
// across 4 bytes) - the same layout this project's own scratchpad
// verify_atlas_roundtrip.py decodes against real baked output.
void decodeBlock(const uint8_t* block, bool& threeColorMode, uint8_t selectors[16]) {
    uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    threeColorMode = (c0 <= c1);
    uint32_t selBits = static_cast<uint32_t>(block[4]) | (static_cast<uint32_t>(block[5]) << 8) |
                        (static_cast<uint32_t>(block[6]) << 16) | (static_cast<uint32_t>(block[7]) << 24);
    for (int p = 0; p < 16; ++p) {
        selectors[p] = static_cast<uint8_t>((selBits >> (p * 2)) & 0x3);
    }
}

} // namespace

int main() {
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);

    // --- a block that reproduces the real defect, found by brute-force
    //     search over random blocks against this file's pre-fix behavior
    //     (one real transparent texel at index 12; fifteen varied opaque
    //     texels). Before fixOpaqueTexelsPunchedThrough existed, rgbcx's
    //     block-wide least-squares selector search assigned selector 3
    //     (transparent) to the real-opaque texel at index 9 as well,
    //     because its color (1,38,112) happened to sit closer to the
    //     block's free (0,0,0,0) transparent anchor than to either fitted
    //     endpoint - confirmed by disabling the fix locally and re-running
    //     this exact data, which fails without it. ---
    {
        static const uint8_t kPixels[16][4] = {
            {128,97,207,255}, {179,24,124,255}, {56,106,66,255},  {163,119,194,255},
            {117,142,181,255},{177,45,233,255}, {136,6,42,255},   {214,196,98,255},
            {237,76,156,255}, {1,38,112,255},    {125,188,96,255},{96,217,126,255},
            {0,0,0,0},         {233,3,98,255},    {63,80,88,255},  {145,203,48,255},
        };
        std::vector<uint8_t> pixels(4 * 4 * 4, 0);
        for (int i = 0; i < 16; ++i) {
            pixels[i * 4 + 0] = kPixels[i][0]; pixels[i * 4 + 1] = kPixels[i][1];
            pixels[i * 4 + 2] = kPixels[i][2]; pixels[i * 4 + 3] = kPixels[i][3];
        }

        auto dxt = TextureOpt::compressToDXT(pixels.data(), 4, 4, true);
        CHECK(dxt.size() == 8, "engineered block: one BC1 block out");

        bool threeColor = false;
        uint8_t sel[16] = {};
        decodeBlock(dxt.data(), threeColor, sel);
        CHECK(threeColor, "engineered block: real transparency present, must stay 3-color mode");

        for (int i = 0; i < 16; ++i) {
            bool sourceTransparent = (kPixels[i][3] < 128);
            char msg[80];
            std::snprintf(msg, sizeof(msg), "engineered block: texel %d selector must match its real source alpha", i);
            CHECK((sel[i] == 3) == sourceTransparent, msg);
        }
    }

    // --- a uniform fully-opaque block must never be offered a transparent
    //     selector at all, even with punchThroughAlpha=true (the
    //     blockNeedsTransparency gate: no real alpha<128 texel exists) ---
    {
        std::vector<uint8_t> pixels(4 * 4 * 4, 0);
        for (int i = 0; i < 16; ++i) {
            pixels[i * 4 + 0] = 10; pixels[i * 4 + 1] = 10; pixels[i * 4 + 2] = 10; pixels[i * 4 + 3] = 255;
        }
        auto dxt = TextureOpt::compressToDXT(pixels.data(), 4, 4, true);
        bool threeColor = false;
        uint8_t sel[16] = {};
        decodeBlock(dxt.data(), threeColor, sel);
        for (int i = 0; i < 16; ++i) {
            CHECK(sel[i] != 3, "all-opaque block: no texel may be selector 3 without any real transparency");
        }
    }

    // --- a uniform fully-transparent block must decode as (0,0,0,0)
    //     everywhere (the allForcedBlack hand-coded path) ---
    {
        std::vector<uint8_t> pixels(4 * 4 * 4, 0); // alpha 0 throughout
        auto dxt = TextureOpt::compressToDXT(pixels.data(), 4, 4, true);
        bool threeColor = false;
        uint8_t sel[16] = {};
        decodeBlock(dxt.data(), threeColor, sel);
        CHECK(threeColor, "all-transparent block: must be 3-color mode");
        for (int i = 0; i < 16; ++i) {
            CHECK(sel[i] == 3, "all-transparent block: every texel must be selector 3");
        }
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
