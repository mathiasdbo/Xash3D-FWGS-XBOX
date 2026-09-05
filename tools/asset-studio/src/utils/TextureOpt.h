#ifndef TEXTUREOPT_H
#define TEXTUREOPT_H

#include <cstdint>
#include <vector>

namespace TextureOpt {

    /**
     * @brief Compress a raw RGBA8888 image to BC1/DXT1 via rgbcx.
     * @param rgbaData Pointer to raw RGBA bytes.
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @param punchThroughAlpha If true, encodes with BC1's 3-color/punch-through-alpha
     *   mode: input texels with alpha < 128 are remapped to black pre-encode (forcing
     *   them onto BC1's transparent selector), and opaque-but-near-black texels are
     *   lifted so they can't be mistaken for a transparent key by the encoder's
     *   black-texel detection. If false, encodes plain 4-color opaque BC1 (alpha is
     *   always ignored by rgbcx's encoder either way - this flag only controls
     *   whether transparency can be *expressed* through the selector trick).
     * @return Compressed binary blob (8 bytes/block).
     */
    std::vector<uint8_t> compressToDXT(const uint8_t* rgbaData, int width, int height, bool punchThroughAlpha);

    /**
     * @brief Applies NV2A Morton/Z-order swizzling to a linear buffer (e.g., DXT blocks).
     * @param linearData The linear texture or DXT block data.
     * @param width Width of the texture (in pixels or blocks).
     * @param height Height of the texture (in pixels or blocks).
     * @param bytesPerBlock Size of a single element/block (e.g., 8 for DXT1, 16 for DXT5).
     * @return Swizzled binary blob.
     */
    std::vector<uint8_t> swizzleNV2A(const std::vector<uint8_t>& linearData, int width, int height, int bytesPerBlock);

} // namespace TextureOpt

#endif // TEXTUREOPT_H
