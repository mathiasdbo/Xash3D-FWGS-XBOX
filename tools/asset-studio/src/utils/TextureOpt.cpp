#include "TextureOpt.h"
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include "rgbcx.h"

namespace TextureOpt {

    // Bit-interleaving for Morton order (Z-order curve)
    // Inserts a 0 bit after each of the 16 low bits of x
    uint32_t Part1By1(uint32_t x) {
        x &= 0x0000ffff;
        x = (x ^ (x <<  8)) & 0x00ff00ff;
        x = (x ^ (x <<  4)) & 0x0f0f0f0f;
        x = (x ^ (x <<  2)) & 0x33333333;
        x = (x ^ (x <<  1)) & 0x55555555;
        return x;
    }

    // Calculates the 2D Morton code for (x, y)
    uint32_t morton2D(uint32_t x, uint32_t y) {
        return (Part1By1(y) << 1) | Part1By1(x);
    }

    namespace {
        inline void unpack565(uint16_t c, int& r, int& g, int& b) {
            int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
            r = (r5 << 3) | (r5 >> 2);
            g = (g6 << 2) | (g6 >> 4);
            b = (b5 << 3) | (b5 >> 2);
        }

        inline int sqdist(int r0, int g0, int b0, int r1, int g1, int b1) {
            int dr = r0 - r1, dg = g0 - g1, db = b0 - b1;
            return dr * dr + dg * dg + db * db;
        }

        // codex-review (round 2, MUST FIX): compressToDXT's blockNeedsTransparency
        // gate (see its own comment below) only controls whether rgbcx is allowed
        // to use selector 3 for THIS block at all - it doesn't constrain WHICH
        // texels end up on it. rgbcx's own selector search is a block-wide least-
        // squares fit driven by RGB error alone (rgbcx.cpp's bc1_find_sels3_*),
        // with no knowledge of source alpha, so a "mixed" block (some texels
        // really transparent, some really opaque) can still assign selector 3 to
        // an opaque texel whose color happens to fit the transparent group best -
        // confirmed for real on 6 texels in one retail bake (docs/r3d/fork-plan.md,
        // XB section).
        //
        // Fixed here with a verify-then-correct pass, not a blind one: decode the
        // block rgbcx already produced, and for exactly the texels that got
        // selector 3 despite a real (source) alpha >= 128, reassign them to
        // whichever of this block's own already-chosen color0/color1/midpoint is
        // closest by RGB distance - every texel that was already correct (real
        // transparent -> 3, real opaque -> non-3) keeps its existing selector
        // unchanged, so this only touches the specific texels that were wrong.
        // rgbcx.h's own pForce_selectors contract (rgbcx.cpp:1088-1106,
        // 1128-1148 - `memcpy(sels, pForce_selectors, 16)`) writes the forced
        // array to the output verbatim regardless of which endpoint candidate
        // wins the search, so the RE-ENCODE's selector bits are guaranteed to
        // equal what's forced here. What ISN'T independently verified by reading
        // the library is whether forcing a real transparent texel's selector to 3
        // still reliably makes the WINNING endpoint-search candidate emit
        // color0<=color1 (3-color mode; the only mode where index 3 decodes as
        // transparent - in 4-color mode it's a valid opaque blend instead) rather
        // than some other candidate. So the retry's result is decoded again and
        // checked against the exact invariant this function exists to guarantee -
        // never adopted unless verified strictly better than the pre-retry block.
        // A block that fails verification keeps whatever compressToDXT already
        // produced, so this pass can only fix or no-op, never regress.
        void fixOpaqueTexelsPunchedThrough(uint8_t* dxtBlock, const uint8_t blockPixels[64]) {
            uint16_t c0 = static_cast<uint16_t>(dxtBlock[0] | (dxtBlock[1] << 8));
            uint16_t c1 = static_cast<uint16_t>(dxtBlock[2] | (dxtBlock[3] << 8));
            if (c0 > c1) {
                return; // 4-color mode: no selector-3-as-transparent semantics here at all.
            }
            uint32_t selBits = static_cast<uint32_t>(dxtBlock[4]) | (static_cast<uint32_t>(dxtBlock[5]) << 8) |
                                (static_cast<uint32_t>(dxtBlock[6]) << 16) | (static_cast<uint32_t>(dxtBlock[7]) << 24);

            uint8_t forced[16];
            bool anyViolation = false;
            for (int p = 0; p < 16; ++p) {
                uint32_t sel = (selBits >> (p * 2)) & 0x3;
                forced[p] = static_cast<uint8_t>(sel);
                if (sel == 3 && blockPixels[p * 4 + 3] >= 128) {
                    anyViolation = true;
                }
            }
            if (!anyViolation) {
                return;
            }

            int r0, g0, b0, r1, g1, b1;
            unpack565(c0, r0, g0, b0);
            unpack565(c1, r1, g1, b1);
            int rMid = (r0 + r1) / 2, gMid = (g0 + g1) / 2, bMid = (b0 + b1) / 2;

            for (int p = 0; p < 16; ++p) {
                if (forced[p] != 3 || blockPixels[p * 4 + 3] < 128) {
                    continue; // already correct - leave its existing group alone.
                }
                int r = blockPixels[p * 4 + 0], g = blockPixels[p * 4 + 1], b = blockPixels[p * 4 + 2];
                int d0 = sqdist(r, g, b, r0, g0, b0);
                int d1 = sqdist(r, g, b, r1, g1, b1);
                int d2 = sqdist(r, g, b, rMid, gMid, bMid);
                forced[p] = (d0 <= d1 && d0 <= d2) ? 0 : (d1 <= d2 ? 1 : 2);
            }

            uint8_t retry[8];
            rgbcx::encode_bc1(18, retry, blockPixels, true, true, forced);

            uint16_t rc0 = static_cast<uint16_t>(retry[0] | (retry[1] << 8));
            uint16_t rc1 = static_cast<uint16_t>(retry[2] | (retry[3] << 8));
            uint32_t rsel = static_cast<uint32_t>(retry[4]) | (static_cast<uint32_t>(retry[5]) << 8) |
                             (static_cast<uint32_t>(retry[6]) << 16) | (static_cast<uint32_t>(retry[7]) << 24);
            bool verified = (rc0 <= rc1);
            for (int p = 0; verified && p < 16; ++p) {
                uint32_t sel = (rsel >> (p * 2)) & 0x3;
                bool wantsTransparent = blockPixels[p * 4 + 3] < 128;
                if (wantsTransparent != (sel == 3)) {
                    verified = false;
                }
            }
            if (verified) {
                std::memcpy(dxtBlock, retry, 8);
            }
        }
    }

    std::vector<uint8_t> compressToDXT(const uint8_t* rgbaData, int width, int height, bool punchThroughAlpha) {
        int blocksX = width / 4;
        int blocksY = height / 4;
        std::vector<uint8_t> outBuffer;
        outBuffer.reserve(blocksX * blocksY * 8); // 8 bytes per BC1 block

        uint8_t blockPixels[16 * 4];
        uint8_t dxtBlock[8];

        for (int by = 0; by < blocksY; ++by) {
            for (int bx = 0; bx < blocksX; ++bx) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        int srcX = bx * 4 + x;
                        int srcY = by * 4 + y;
                        int srcIdx = (srcY * width + srcX) * 4;
                        int destIdx = (y * 4 + x) * 4;

                        uint8_t r = rgbaData[srcIdx + 0];
                        uint8_t g = rgbaData[srcIdx + 1];
                        uint8_t b = rgbaData[srcIdx + 2];
                        uint8_t a = rgbaData[srcIdx + 3];

                        if (punchThroughAlpha) {
                            if (a < 128) {
                                // Force onto BC1's transparent (selector 3) bucket -
                                // rgbcx's encoder picks 3-color mode + selector 3 for
                                // texels it sees as black when use_transparent_texels_
                                // for_black is set.
                                r = g = b = 0;
                            } else {
                                uint8_t maxc = std::max({r, g, b});
                                if (maxc < 16) {
                                    // Opaque but near-black: lift so this texel can't
                                    // be mistaken for a transparent key by the same
                                    // black-texel detection.
                                    r = std::max<uint8_t>(r, 16);
                                    g = std::max<uint8_t>(g, 16);
                                    b = std::max<uint8_t>(b, 16);
                                }
                            }
                        }

                        blockPixels[destIdx + 0] = r;
                        blockPixels[destIdx + 1] = g;
                        blockPixels[destIdx + 2] = b;
                        blockPixels[destIdx + 3] = a;
                    }
                }

                // rgbcx's solid-color fast path (encode_bc1_solid_block) ties err3
                // against err4 for an exactly-representable color like pure black,
                // and its strict "<" comparison - plus a second branch that always
                // forbids 3-color for an exact solid match - means a block that's
                // ENTIRELY forced-transparent texels never gets 3-color/punch-through
                // treatment from the general encoder, no matter what flags are passed.
                // Large uniform transparent regions (a sprite's background, most of
                // any masked texture) are exactly this case, so hand-encode it
                // directly: any color0<=color1 with every selector set to 3 decodes
                // to (0,0,0,0) everywhere, which is exactly what a fully-transparent
                // block should be.
                bool allForcedBlack = punchThroughAlpha;
                if (allForcedBlack) {
                    for (int p = 0; p < 16 && allForcedBlack; ++p) {
                        allForcedBlack = (blockPixels[p * 4 + 0] == 0 && blockPixels[p * 4 + 1] == 0 && blockPixels[p * 4 + 2] == 0);
                    }
                }

                // asset-pipeline-specialist review, real finding, confirmed by
                // decoding real baked output byte-for-byte: even with every
                // texture's own footprint now block-aligned (PakManager.cpp's
                // own fix), rgbcx's block-WIDE least-squares fit can still
                // absorb a genuinely opaque (source alpha=255) but near-black
                // texel into a 3-color/selector-3 (transparent) result, since
                // the "opaque but near-black: lift" mitigation below only
                // catches maxc<16 - a pixel at exactly 16, or one whose block-
                // mates pull the aggregate fit toward transparent even a bit
                // above that, can still get punched through incorrectly. No
                // fixed per-pixel lift threshold can guarantee this can't
                // happen, because the decision is fundamentally block-wide,
                // not per-pixel - so guard it at the only place that CAN be
                // exact: per block, only ever ask the encoder for punch-
                // through mode if this SPECIFIC block contains at least one
                // texel whose real source alpha is actually low. A block with
                // no real transparency need literally cannot be offered a
                // transparent selector, so it cannot be miscoded as one -
                // this eliminates the whole defect class for non-masked
                // (or non-edge) blocks, rather than reducing its odds.
                bool blockNeedsTransparency = false;
                if (punchThroughAlpha && !allForcedBlack) {
                    for (int p = 0; p < 16; ++p) {
                        if (blockPixels[p * 4 + 3] < 128) {
                            blockNeedsTransparency = true;
                            break;
                        }
                    }
                }

                if (allForcedBlack) {
                    dxtBlock[0] = 0x00; dxtBlock[1] = 0x00; // color0 = 0
                    dxtBlock[2] = 0x01; dxtBlock[3] = 0x00; // color1 = 1 (color0 <= color1 -> 3-color mode)
                    dxtBlock[4] = dxtBlock[5] = dxtBlock[6] = dxtBlock[7] = 0xFF; // all 16 selectors = 3 (transparent)
                } else {
                    // level 18 (highest quality); allow_3color always true (needed
                    // for punch-through mode; harmless extra option otherwise);
                    // use_transparent_texels_for_black only when THIS BLOCK
                    // actually contains a real transparent-key texel, so a block
                    // with no genuine masking need is never even offered the
                    // option to encode as transparent, regardless of how dark
                    // its real opaque content is.
                    rgbcx::encode_bc1(18, dxtBlock, blockPixels, true, blockNeedsTransparency);
                    if (blockNeedsTransparency) {
                        fixOpaqueTexelsPunchedThrough(dxtBlock, blockPixels);
                    }
                }
                outBuffer.insert(outBuffer.end(), dxtBlock, dxtBlock + 8);
            }
        }
        return outBuffer;
    }

    std::vector<uint8_t> swizzleNV2A(const std::vector<uint8_t>& linearData, int width, int height, int bytesPerBlock) {
        int expectedSize = width * height * bytesPerBlock;
        if (linearData.size() != expectedSize) {
            throw std::runtime_error("Linear data size mismatch for NV2A swizzle");
        }

        std::vector<uint8_t> swizzled(expectedSize);
        
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                // Calculate standard linear index vs Z-order index
                uint32_t linearIdx = (y * width + x) * bytesPerBlock;
                uint32_t swizzledIdx = morton2D(x, y) * bytesPerBlock;
                
                // Copy the block (8 bytes for DXT1, 16 for DXT5)
                for (int b = 0; b < bytesPerBlock; ++b) {
                    swizzled[swizzledIdx + b] = linearData[linearIdx + b];
                }
            }
        }

        return swizzled;
    }

} // namespace TextureOpt
