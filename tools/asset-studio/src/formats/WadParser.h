#ifndef WADPARSER_H
#define WADPARSER_H

#include <string>
#include <cstdint>
#include <vector>

namespace formats {

#pragma pack(push, 1)
    struct miptex_t {
        char name[16];
        uint32_t width, height;
        uint32_t offsets[4];
    };
#pragma pack(pop)

    struct DecodedTexture {
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba; // 32-bit RGBA data
    };

    struct WadLump {
        uint32_t offset;
        uint32_t compressedSize;
        uint32_t fullSize;
        uint8_t type;
        uint8_t compression;
        char name[16];
        std::vector<uint8_t> data;
    };

    class WadParser {
    public:
        WadParser() = default;
        bool load(const std::string& filePath);
        const std::vector<WadLump>& getLumps() const { return mLumps; }

        static bool decodeMiptex(const uint8_t* data, size_t size, DecodedTexture& outTex);

    private:
        std::vector<WadLump> mLumps;
    };

} // namespace formats

#endif // WADPARSER_H
