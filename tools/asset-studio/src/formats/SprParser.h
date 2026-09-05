#ifndef SPRPARSER_H
#define SPRPARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include "PakManager.h"

namespace formats {

    // GoldSrc sprite.h drawtype_t (texFormat). SPR_ALPHTEST uses palette index 255
    // as a binary transparency key ("masked.pal"); the others decode opaque (or, for
    // SPR_INDEXALPHA, a graduated alpha this parser does not yet implement).
    constexpr int32_t SPR_NORMAL = 0;
    constexpr int32_t SPR_ADDITIVE = 1;
    constexpr int32_t SPR_INDEXALPHA = 2;
    constexpr int32_t SPR_ALPHTEST = 3;

#pragma pack(push, 1)
    struct msprite_t {
        int32_t id;             // "IDSP"
        int32_t version;
        int32_t type;
        int32_t texFormat;
        float boundingRadius;
        int32_t maxWidth;
        int32_t maxHeight;
        int32_t numFrames;
        float beamLength;
        int32_t syncType;
        int16_t paletteSize;
    };
    
    struct mspriteframe_t {
        int32_t origin[2];
        int32_t width;
        int32_t height;
    };
#pragma pack(pop)

    struct SprFrame {
        int32_t width;
        int32_t height;
        int32_t origin[2];
        std::vector<uint8_t> rgba; // 32-bit decoded frame
    };

    class SprParser {
    public:
        SprParser() = default;
        bool load(const std::string& filePath);
        
        const msprite_t& getHeader() const { return mHeader; }
        const std::vector<SprFrame>& getFrames() const { return mFrames; }
        
        std::vector<uint8_t> getPatchedData(const std::vector<TextureLocation>& locs);

    private:
        msprite_t mHeader;
        std::vector<SprFrame> mFrames;
        std::vector<uint8_t> mRawData;
    };

} // namespace formats

#endif // SPRPARSER_H
