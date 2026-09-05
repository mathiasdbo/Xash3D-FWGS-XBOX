#include "MdlParser.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace formats {

namespace {

    // Reads a whole file into memory; returns empty on any failure (missing file,
    // read error). Used both for the main .mdl and an optional <stem>T.mdl sibling.
    std::vector<uint8_t> readWholeFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return {};

        file.seekg(0, std::ios::end);
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size <= 0) return {};

        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(data.data()), size)) return {};
        return data;
    }

    // <dir>/<stem>.mdl -> <dir>/<stem>T.mdl, resolved case-insensitively against
    // whatever actually sits next to the main file (GoldSrc content ships this as
    // e.g. "playerT.mdl" but filesystems vary in case sensitivity/convention).
    std::string findTexModelSibling(const std::string& mainFilePath) {
        std::filesystem::path mainPath(mainFilePath);
        std::string stem = mainPath.stem().string(); // "player" from "player.mdl"
        std::string wantLower = stem + "t.mdl";
        std::transform(wantLower.begin(), wantLower.end(), wantLower.begin(), [](unsigned char c){ return std::tolower(c); });

        std::error_code ec;
        std::filesystem::path dir = mainPath.parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string nameLower = entry.path().filename().string();
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c){ return std::tolower(c); });
            if (nameLower == wantLower) {
                return entry.path().string();
            }
        }
        return "";
    }

} // namespace

void MdlParser::parseTextures(const std::vector<uint8_t>& rawData, const studiohdr_t& hdr, std::vector<MdlTexture>& outTextures) {
    outTextures.clear();
    if (hdr.numtextures <= 0) return;

    size_t texHeadersEnd = static_cast<size_t>(hdr.textureindex) + static_cast<size_t>(hdr.numtextures) * sizeof(mstudiotexture_t);
    if (hdr.textureindex < 0 || texHeadersEnd > rawData.size()) {
        std::cerr << "MdlParser: texture header table out of bounds (index=" << hdr.textureindex
                  << ", count=" << hdr.numtextures << ") - treating as 0 textures\n";
        return;
    }

    const mstudiotexture_t* texHeaders = reinterpret_cast<const mstudiotexture_t*>(rawData.data() + hdr.textureindex);

    for (int i = 0; i < hdr.numtextures; ++i) {
        MdlTexture outTex;
        outTex.name.assign(texHeaders[i].name, strnlen(texHeaders[i].name, sizeof(texHeaders[i].name)));
        outTex.width = static_cast<uint32_t>(texHeaders[i].width);
        outTex.height = static_cast<uint32_t>(texHeaders[i].height);
        outTex.flags = texHeaders[i].flags;

        size_t pixelBytes = static_cast<size_t>(outTex.width) * outTex.height;
        constexpr size_t kPaletteBytes = 768; // 256 entries x RGB888
        size_t dataEnd = static_cast<size_t>(texHeaders[i].index) + pixelBytes + kPaletteBytes;

        outTex.rgba.resize(pixelBytes * 4);

        bool inBounds = texHeaders[i].index >= 0 && outTex.width > 0 && outTex.height > 0 && dataEnd <= rawData.size();
        if (!inBounds) {
            std::cerr << "MdlParser: texture '" << outTex.name << "' pixel/palette data out of bounds - using placeholder\n";
            for (size_t p = 0; p < pixelBytes; ++p) {
                outTex.rgba[p * 4 + 0] = 255;
                outTex.rgba[p * 4 + 1] = 0;
                outTex.rgba[p * 4 + 2] = 255;
                outTex.rgba[p * 4 + 3] = 255;
            }
        } else {
            const uint8_t* pixels = rawData.data() + texHeaders[i].index;
            const uint8_t* palette = pixels + pixelBytes;
            bool masked = (outTex.flags & STUDIO_NF_MASKED) != 0;
            for (size_t p = 0; p < pixelBytes; ++p) {
                uint8_t index = pixels[p];
                outTex.rgba[p * 4 + 0] = palette[index * 3 + 0];
                outTex.rgba[p * 4 + 1] = palette[index * 3 + 1];
                outTex.rgba[p * 4 + 2] = palette[index * 3 + 2];
                outTex.rgba[p * 4 + 3] = (masked && index == 255) ? 0 : 255;
            }
        }

        outTextures.push_back(std::move(outTex));
    }
}

void MdlParser::parseSkinTable(const std::vector<uint8_t>& rawData, const studiohdr_t& hdr, uint32_t numTextures, std::vector<int16_t>& outSkinRefs, int32_t& outNumSkinFamilies, int32_t& outNumSkinRef) {
    outSkinRefs.clear();

    size_t tableEntries = static_cast<size_t>(hdr.numskinfamilies) * static_cast<size_t>(hdr.numskinref);
    size_t tableEnd = static_cast<size_t>(hdr.skinindex) + tableEntries * sizeof(int16_t);

    bool inBounds = numTextures > 0 && hdr.numskinfamilies > 0 && hdr.numskinref > 0
        && hdr.skinindex >= 0 && tableEnd <= rawData.size();

    if (inBounds) {
        const int16_t* table = reinterpret_cast<const int16_t*>(rawData.data() + hdr.skinindex);
        outSkinRefs.assign(table, table + tableEntries);
        outNumSkinFamilies = hdr.numskinfamilies;
        outNumSkinRef = hdr.numskinref;
        return;
    }

    if (numTextures > 0) {
        std::cerr << "MdlParser: skin ref table missing/out of bounds - synthesizing identity table\n";
    }
    outSkinRefs.resize(numTextures);
    for (uint32_t i = 0; i < numTextures; ++i) outSkinRefs[i] = static_cast<int16_t>(i);
    outNumSkinFamilies = numTextures > 0 ? 1 : 0;
    outNumSkinRef = static_cast<int32_t>(numTextures);
}

bool MdlParser::load(const std::string& filePath) {
    mRawData = readWholeFile(filePath);
    if (mRawData.empty()) {
        std::cerr << "MdlParser: Failed to open/read file " << filePath << "\n";
        return false;
    }

    if (mRawData.size() < sizeof(studiohdr_t)) {
        std::cerr << "MdlParser: File too small to be a valid MDL\n";
        return false;
    }

    std::memcpy(&mHeader, mRawData.data(), sizeof(studiohdr_t));

    // "IDST" == 0x54534449
    if (mHeader.id != 0x54534449) {
        std::cerr << "MdlParser: Invalid MDL magic number (not IDST)\n";
        return false;
    }

    if (mHeader.version != 10) {
        std::cerr << "MdlParser: Warning: MDL version is " << mHeader.version << ", expected 10.\n";
    }

    if (mHeader.numtextures > 0) {
        // Embedded textures: parse from the main file, patched output truncates
        // right before the original texture header table.
        mIsSpliced = false;
        mOriginalTextureIndex = mHeader.textureindex;
        parseTextures(mRawData, mHeader, mTextures);
        parseSkinTable(mRawData, mHeader, static_cast<uint32_t>(mTextures.size()), mSkinRefs, mNumSkinFamilies, mNumSkinRef);
    } else {
        // No embedded textures - GoldSrc convention is a <stem>T.mdl sibling
        // (mod_studio.c's Mod_StudioTexName: strip extension, append "T.mdl").
        mIsSpliced = true;
        std::string texModelPath = findTexModelSibling(filePath);
        if (texModelPath.empty()) {
            std::cerr << "MdlParser: '" << filePath << "' has 0 embedded textures and no "
                         "T.mdl sibling was found - model will bake untextured\n";
        } else {
            std::vector<uint8_t> texRaw = readWholeFile(texModelPath);
            if (texRaw.size() < sizeof(studiohdr_t)) {
                std::cerr << "MdlParser: T.mdl sibling '" << texModelPath << "' unreadable/too small\n";
            } else {
                studiohdr_t texHdr;
                std::memcpy(&texHdr, texRaw.data(), sizeof(studiohdr_t));
                if (texHdr.id != 0x54534449) {
                    std::cerr << "MdlParser: T.mdl sibling '" << texModelPath << "' has bad magic\n";
                } else {
                    parseTextures(texRaw, texHdr, mTextures);
                    parseSkinTable(texRaw, texHdr, static_cast<uint32_t>(mTextures.size()), mSkinRefs, mNumSkinFamilies, mNumSkinRef);
                }
            }
        }
    }

    // Do not clear mRawData, we need it for patching

    return true;
}

std::vector<uint8_t> MdlParser::getPatchedData(const std::vector<TextureLocation>& locs) {
    if (mTextures.empty() || locs.size() != mTextures.size()) {
        return mRawData;
    }

    // Embedded case: cut right before the original texture header table (which
    // precedes its own pixel data by construction in every real GoldSrc MDL),
    // discarding old headers/skin table/pixel+palette data in one truncation -
    // all three are rebuilt fresh below. Spliced case: the main file never had any
    // texture-related trailing data (numtextures was 0), so nothing is truncated;
    // everything appends after the file's real content.
    uint32_t truncateAt = mIsSpliced ? static_cast<uint32_t>(mRawData.size()) : static_cast<uint32_t>(mOriginalTextureIndex);
    mRawData.resize(truncateAt);

    auto pad4 = [](std::vector<uint8_t>& buf) {
        size_t rem = buf.size() % 4;
        if (rem != 0) buf.insert(buf.end(), 4 - rem, 0);
    };

    // Fresh mstudiotexture_t[] - name/flags/width/height preserved, index dead
    // (pixel data no longer travels with the model; TextureLocation below is what
    // the renderer actually uses).
    uint32_t newTextureIndex = static_cast<uint32_t>(mRawData.size());
    for (const auto& tex : mTextures) {
        mstudiotexture_t th{};
        std::strncpy(th.name, tex.name.c_str(), sizeof(th.name) - 1);
        th.flags = tex.flags;
        th.width = static_cast<int32_t>(tex.width);
        th.height = static_cast<int32_t>(tex.height);
        th.index = 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&th);
        mRawData.insert(mRawData.end(), bytes, bytes + sizeof(mstudiotexture_t));
    }
    pad4(mRawData);

    // Fresh skin ref table (numskinfamilies x numskinref int16, row-major families).
    uint32_t newSkinIndex = static_cast<uint32_t>(mRawData.size());
    const uint8_t* skinBytes = reinterpret_cast<const uint8_t*>(mSkinRefs.data());
    mRawData.insert(mRawData.end(), skinBytes, skinBytes + mSkinRefs.size() * sizeof(int16_t));
    pad4(mRawData);

    // TextureLocation[numtextures] - 4-byte aligned Pod, already aligned by pad4 above.
    uint32_t newTextureDataIndex = static_cast<uint32_t>(mRawData.size());
    const uint8_t* locsBytes = reinterpret_cast<const uint8_t*>(locs.data());
    mRawData.insert(mRawData.end(), locsBytes, locsBytes + locs.size() * sizeof(TextureLocation));

    studiohdr_t* hdr = reinterpret_cast<studiohdr_t*>(mRawData.data());
    hdr->numtextures = static_cast<int32_t>(mTextures.size());
    hdr->textureindex = static_cast<int32_t>(newTextureIndex);
    hdr->texturedataindex = static_cast<int32_t>(newTextureDataIndex);
    hdr->numskinref = mNumSkinRef;
    hdr->numskinfamilies = mNumSkinFamilies;
    hdr->skinindex = static_cast<int32_t>(newSkinIndex);
    hdr->length = static_cast<int32_t>(mRawData.size());

    return mRawData;
}

} // namespace formats
