#include "TextureOpt.h"
#include <stdexcept>
#include <algorithm>
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

                if (allForcedBlack) {
                    dxtBlock[0] = 0x00; dxtBlock[1] = 0x00; // color0 = 0
                    dxtBlock[2] = 0x01; dxtBlock[3] = 0x00; // color1 = 1 (color0 <= color1 -> 3-color mode)
                    dxtBlock[4] = dxtBlock[5] = dxtBlock[6] = dxtBlock[7] = 0xFF; // all 16 selectors = 3 (transparent)
                } else {
                    // level 18 (highest quality); allow_3color always true (needed
                    // for punch-through mode; harmless extra option otherwise);
                    // use_transparent_texels_for_black only when this slice is
                    // masked, so legitimately dark opaque world textures stay pure
                    // 4-color.
                    rgbcx::encode_bc1(18, dxtBlock, blockPixels, true, punchThroughAlpha);
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
