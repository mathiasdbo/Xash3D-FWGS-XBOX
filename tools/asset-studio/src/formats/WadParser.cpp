#include "WadParser.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace formats {

    bool WadParser::load(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open WAD file: " << filePath << "\n";
            return false;
        }

        char magic[4];
        file.read(magic, 4);
        if (strncmp(magic, "WAD3", 4) != 0) {
            std::cerr << "Invalid WAD3 magic number for file: " << filePath << "\n";
            return false;
        }

        uint32_t numLumps = 0, dirOffset = 0;
        file.read(reinterpret_cast<char*>(&numLumps), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&dirOffset), sizeof(uint32_t));

        file.seekg(dirOffset, std::ios::beg);
        
        mLumps.clear();
        mLumps.reserve(numLumps);

        for (uint32_t i = 0; i < numLumps; ++i) {
            WadLump lump;
            file.read(reinterpret_cast<char*>(&lump.offset), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&lump.compressedSize), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&lump.fullSize), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&lump.type), sizeof(uint8_t));
            file.read(reinterpret_cast<char*>(&lump.compression), sizeof(uint8_t));
            
            uint16_t pad;
            file.read(reinterpret_cast<char*>(&pad), sizeof(uint16_t));
            file.read(lump.name, 16);
            mLumps.push_back(lump);
        }

        for (auto& lump : mLumps) {
            file.seekg(lump.offset, std::ios::beg);
            lump.data.resize(lump.fullSize);
            file.read(reinterpret_cast<char*>(lump.data.data()), lump.fullSize);
        }

        return true;
    }

    bool WadParser::decodeMiptex(const uint8_t* data, size_t size, DecodedTexture& outTex) {
        if (size < sizeof(miptex_t)) return false;

        const miptex_t* header = reinterpret_cast<const miptex_t*>(data);
        outTex.name = std::string(header->name);
        outTex.width = (header->width > 0) ? header->width : 64;
        outTex.height = (header->height > 0) ? header->height : 64;

        size_t pixelCount = outTex.width * outTex.height;

        // If embedded pixels exist
        if (header->offsets[0] > 0 && header->offsets[0] + pixelCount <= size) {
            size_t mip0Offset = header->offsets[0];
            size_t mip1Size = (outTex.width / 2) * (outTex.height / 2);
            size_t mip2Size = (outTex.width / 4) * (outTex.height / 4);
            size_t mip3Size = (outTex.width / 8) * (outTex.height / 8);

            size_t paletteOffset = mip0Offset + pixelCount + mip1Size + mip2Size + mip3Size + sizeof(uint16_t);

            const uint8_t* pixels = data + mip0Offset;
            const uint8_t* palette = nullptr;
            std::vector<uint8_t> fallbackPalette;

            if (paletteOffset + 768 <= size) {
                palette = data + paletteOffset;
            } else {
                // Grayscale fallback palette for BSP textures missing embedded palette
                fallbackPalette.resize(768);
                for (int i = 0; i < 256; ++i) {
                    fallbackPalette[i * 3 + 0] = static_cast<uint8_t>(i);
                    fallbackPalette[i * 3 + 1] = static_cast<uint8_t>(i);
                    fallbackPalette[i * 3 + 2] = static_cast<uint8_t>(i);
                }
                palette = fallbackPalette.data();
            }

            outTex.rgba.resize(pixelCount * 4);
            bool isTransparentTex = (header->name[0] == '{');

            for (size_t i = 0; i < pixelCount; ++i) {
                uint8_t idx = pixels[i];
                outTex.rgba[i * 4 + 0] = palette[idx * 3 + 0];
                outTex.rgba[i * 4 + 1] = palette[idx * 3 + 1];
                outTex.rgba[i * 4 + 2] = palette[idx * 3 + 2];
                outTex.rgba[i * 4 + 3] = (isTransparentTex && idx == 255) ? 0 : 255;
            }
            return true;
        }

        // Procedural grid fallback if texture has no pixel payload at all
        outTex.rgba.resize(pixelCount * 4);
        uint32_t hash = 5381;
        for (char c : outTex.name) {
            hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
        }

        uint8_t r = static_cast<uint8_t>(80 + ((hash) % 150));
        uint8_t g = static_cast<uint8_t>(80 + ((hash >> 8) % 150));
        uint8_t b = static_cast<uint8_t>(80 + ((hash >> 16) % 150));

        for (uint32_t y = 0; y < outTex.height; ++y) {
            for (uint32_t x = 0; x < outTex.width; ++x) {
                size_t idx = (y * outTex.width + x) * 4;
                bool isBorder = (x < 2 || x >= outTex.width - 2 || y < 2 || y >= outTex.height - 2);
                bool isGrid = ((x / 16) + (y / 16)) % 2 == 0;

                outTex.rgba[idx + 0] = isBorder ? 20 : (isGrid ? r : (r * 3 / 4));
                outTex.rgba[idx + 1] = isBorder ? 20 : (isGrid ? g : (g * 3 / 4));
                outTex.rgba[idx + 2] = isBorder ? 20 : (isGrid ? b : (b * 3 / 4));
                outTex.rgba[idx + 3] = 255;
            }
        }

        return true;
    }

} // namespace formats
