#include "SprParser.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace formats {

bool SprParser::load(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "SprParser: Failed to open file " << filePath << "\n";
        return false;
    }

    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < sizeof(msprite_t)) {
        std::cerr << "SprParser: File too small\n";
        return false;
    }

    mRawData.resize(size);
    if (!file.read(reinterpret_cast<char*>(mRawData.data()), size)) {
        std::cerr << "SprParser: Failed to read data\n";
        return false;
    }

    std::memcpy(&mHeader, mRawData.data(), sizeof(msprite_t));

    // "IDSP" == 0x50534449
    if (mHeader.id != 0x50534449) {
        std::cerr << "SprParser: Invalid SPR magic number (not IDSP)\n";
        return false;
    }

    // Palette follows header
    const uint8_t* ptr = mRawData.data() + sizeof(msprite_t);
    const uint8_t* palette = ptr;
    ptr += mHeader.paletteSize * 3;

    mFrames.clear();
    for (int i = 0; i < mHeader.numFrames; ++i) {
        if (ptr + sizeof(int32_t) > mRawData.data() + mRawData.size()) {
             break;
        }
        
        int32_t frameType = *reinterpret_cast<const int32_t*>(ptr);
        ptr += sizeof(int32_t);
        
        if (frameType != 0) {
            std::cerr << "SprParser: Frame groups are not fully supported yet.\n";
            break;
        }
        
        if (ptr + sizeof(mspriteframe_t) > mRawData.data() + mRawData.size()) {
             break;
        }
        
        mspriteframe_t frameInfo;
        std::memcpy(&frameInfo, ptr, sizeof(mspriteframe_t));
        ptr += sizeof(mspriteframe_t);

        SprFrame frame;
        frame.width = frameInfo.width;
        frame.height = frameInfo.height;
        frame.origin[0] = frameInfo.origin[0];
        frame.origin[1] = frameInfo.origin[1];
        
        frame.rgba.resize(frame.width * frame.height * 4);
        for (int p = 0; p < frame.width * frame.height; ++p) {
            uint8_t index = ptr[p];
            frame.rgba[p * 4 + 0] = palette[index * 3 + 0];
            frame.rgba[p * 4 + 1] = palette[index * 3 + 1];
            frame.rgba[p * 4 + 2] = palette[index * 3 + 2];
            // SPR_ALPHTEST (3): index 255 is the binary transparency key. Was
            // incorrectly checking texFormat==1 (SPR_ADDITIVE) - see sprite.h's
            // drawtype_t enum (SPR_NORMAL=0, SPR_ADDITIVE=1, SPR_INDEXALPHA=2,
            // SPR_ALPHTEST=3).
            frame.rgba[p * 4 + 3] = (mHeader.texFormat == SPR_ALPHTEST && index == 255) ? 0 : 255;
        }
        ptr += (frame.width * frame.height);
        
        mFrames.push_back(frame);
    }

    // Do not clear mRawData
    return true;
}

std::vector<uint8_t> SprParser::getPatchedData(const std::vector<TextureLocation>& locs) {
    std::vector<uint8_t> patched;
    if (locs.size() != mFrames.size()) {
        std::cerr << "SprParser: TextureLocation count mismatch\n";
        return patched;
    }

    patched.insert(patched.end(), reinterpret_cast<const uint8_t*>(&mHeader), reinterpret_cast<const uint8_t*>(&mHeader) + sizeof(msprite_t));

    // msprite_t is 42 bytes (#pragma pack(1)), which leaves every frame record -
    // and every TextureLocation inside it, a 4-byte-aligned Pod type - starting on
    // an odd 4-byte boundary. 2 bytes of zero padding here makes the frame table
    // start at byte 44, a multiple of 4. Readers MUST use this constant, never
    // sizeof(SpriteHeader)/sizeof(msprite_t), as the frame-data offset.
    patched.insert(patched.end(), 2, 0);

    for (size_t i = 0; i < mFrames.size(); ++i) {
        mspriteframe_t frameInfo;
        frameInfo.origin[0] = mFrames[i].origin[0];
        frameInfo.origin[1] = mFrames[i].origin[1];
        frameInfo.width = mFrames[i].width;
        frameInfo.height = mFrames[i].height;
        patched.insert(patched.end(), reinterpret_cast<const uint8_t*>(&frameInfo), reinterpret_cast<const uint8_t*>(&frameInfo) + sizeof(mspriteframe_t));
        patched.insert(patched.end(), reinterpret_cast<const uint8_t*>(&locs[i]), reinterpret_cast<const uint8_t*>(&locs[i]) + sizeof(TextureLocation));
    }
    
    return patched;
}

} // namespace formats
