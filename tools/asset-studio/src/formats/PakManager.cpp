#include "PakManager.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include "../utils/rgbcx.h"
#include "../utils/TextureOpt.h"

namespace formats {

    PakManager::PakManager() {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        // Xbox Atlas initialization
        mAtlasRgba.assign(mAtlasWidth * mAtlasHeight * 4, 0); // Black transparent padding
    }

    TextureLocation PakManager::addTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData, bool masked) {
        TextureLocation loc{};
        loc.width = width;
        loc.height = height;

        if (mIsXboxTarget) {
            uint32_t paddedW = width + 2;
            uint32_t paddedH = height + 2;

            if (mAtlasCurrentX + paddedW > mAtlasWidth) {
                mAtlasCurrentX = 0;
                mAtlasCurrentY += mAtlasMaxRowHeight;
                mAtlasMaxRowHeight = 0;
            }

            if (mAtlasCurrentY + paddedH > mAtlasHeight) {
                std::cerr << "WARNING: Atlas overflow! Texture won't fit.\n";
                return loc; 
            }

            uint32_t destX = mAtlasCurrentX + 1;
            uint32_t destY = mAtlasCurrentY + 1;

            for (uint32_t y = 0; y < height; ++y) {
                size_t srcOffset = y * width * 4;
                size_t destOffset = ((destY + y) * mAtlasWidth + destX) * 4;
                std::memcpy(&mAtlasRgba[destOffset], &rgbaData[srcOffset], width * 4);
                
                std::memcpy(&mAtlasRgba[((destY + y) * mAtlasWidth + destX - 1) * 4], &rgbaData[srcOffset], 4);
                std::memcpy(&mAtlasRgba[((destY + y) * mAtlasWidth + destX + width) * 4], &rgbaData[srcOffset + (width - 1) * 4], 4);
            }
            
            std::memcpy(&mAtlasRgba[(mAtlasCurrentY * mAtlasWidth + destX - 1) * 4], &mAtlasRgba[(destY * mAtlasWidth + destX - 1) * 4], paddedW * 4);
            std::memcpy(&mAtlasRgba[((destY + height) * mAtlasWidth + destX - 1) * 4], &mAtlasRgba[((destY + height - 1) * mAtlasWidth + destX - 1) * 4], paddedW * 4);

            loc.array_index = 0;
            loc.layer = 0;
            loc.rect[0] = static_cast<float>(destX) / static_cast<float>(mAtlasWidth);
            loc.rect[1] = static_cast<float>(destY) / static_cast<float>(mAtlasHeight);
            loc.rect[2] = static_cast<float>(width) / static_cast<float>(mAtlasWidth);
            loc.rect[3] = static_cast<float>(height) / static_cast<float>(mAtlasHeight);

            mAtlasCurrentX += paddedW;
            if (paddedH > mAtlasMaxRowHeight) mAtlasMaxRowHeight = paddedH;

        } else {
            uint32_t maxDim = std::max(width, height);
            
            uint32_t binSize = 64;
            if (maxDim > 64) binSize = 256;
            if (maxDim > 256) binSize = 512;
            
            std::vector<std::vector<uint8_t>>* targetArray = nullptr;
            std::vector<bool>* targetMaskedArray = nullptr;
            if (binSize == 64) { targetArray = &mArray64; targetMaskedArray = &mArray64Masked; loc.array_index = 0; }
            else if (binSize == 256) { targetArray = &mArray256; targetMaskedArray = &mArray256Masked; loc.array_index = 1; }
            else { targetArray = &mArray512; targetMaskedArray = &mArray512Masked; loc.array_index = 2; }

            loc.width = width;
            loc.height = height;
            loc.binSize = binSize;
            loc.layer = static_cast<uint32_t>(targetArray->size());
            loc.rect[0] = 0.0f;
            loc.rect[1] = 0.0f;
            loc.rect[2] = 1.0f;
            loc.rect[3] = 1.0f;

            std::vector<uint8_t> slice(binSize * binSize * 4, 0);

            for (uint32_t y = 0; y < binSize; ++y) {
                float v_ratio = (height > 1) ? (static_cast<float>(y) / static_cast<float>(binSize - 1)) * static_cast<float>(height - 1) : 0.0f;
                uint32_t y0 = static_cast<uint32_t>(v_ratio);
                uint32_t y1 = std::min(y0 + 1, height - 1);
                float fy = v_ratio - y0;

                for (uint32_t x = 0; x < binSize; ++x) {
                    float u_ratio = (width > 1) ? (static_cast<float>(x) / static_cast<float>(binSize - 1)) * static_cast<float>(width - 1) : 0.0f;
                    uint32_t x0 = static_cast<uint32_t>(u_ratio);
                    uint32_t x1 = std::min(x0 + 1, width - 1);
                    float fx = u_ratio - x0;

                    if (masked) {
                        // Nearest-neighbor: bilinear would blend opaque texel color
                        // into the transparent key at masked edges, smearing a
                        // fringe that punch-through BC1 can't hide.
                        uint32_t nx = (fx < 0.5f) ? x0 : x1;
                        uint32_t ny = (fy < 0.5f) ? y0 : y1;
                        for (int c = 0; c < 4; ++c) {
                            slice[(y * binSize + x) * 4 + c] = rgbaData[(ny * width + nx) * 4 + c];
                        }
                        continue;
                    }

                    for (int c = 0; c < 4; ++c) {
                        float p00 = rgbaData[(y0 * width + x0) * 4 + c];
                        float p10 = rgbaData[(y0 * width + x1) * 4 + c];
                        float p01 = rgbaData[(y1 * width + x0) * 4 + c];
                        float p11 = rgbaData[(y1 * width + x1) * 4 + c];

                        float val = p00 * (1.0f - fx) * (1.0f - fy) +
                                    p10 * fx * (1.0f - fy) +
                                    p01 * (1.0f - fx) * fy +
                                    p11 * fx * fy;

                        slice[(y * binSize + x) * 4 + c] = static_cast<uint8_t>(val);
                    }
                }
            }
            targetArray->push_back(std::move(slice));
            targetMaskedArray->push_back(masked);
        }

        return loc;
    }



    uint16_t PakManager::addVertex(const Vertex& v) {
        auto it = mVertexMap.find(v);
        if (it != mVertexMap.end()) {
            return it->second;
        }

        // Per-face lightmap UVs (lu/lv/lightmap_layer) mean vertices are no longer
        // shared across faces the way they were with the old placeholder UVs, so
        // vertex counts on large maps can approach the raw edge count. Indices are
        // u16: silently wrapping past 65535 would corrupt geometry, so fail loudly
        // instead - the fix is chunking draws with base_vertex (tracked for a later
        // phase), not truncating data here.
        if (mVertices.size() >= 65536) {
            std::cerr << "FATAL: vertex count exceeded 65536 (u16 index limit). "
                         "This map needs base_vertex-chunked draws, not yet implemented.\n";
            std::abort();
        }

        uint16_t index = static_cast<uint16_t>(mVertices.size());
        mVertices.push_back(v);
        mVertexMap[v] = index;
        return index;
    }

    void PakManager::addIndex(uint16_t idx) {
        mIndices.push_back(idx);
    }

    void PakManager::addFaceDrawCmd(uint32_t start_index, uint32_t index_count) {
        mFaceDrawCmds.push_back({start_index, index_count});
    }

    void PakManager::addAsset(const std::string& name, const std::vector<uint8_t>& data) {
        // Ensure the offset starts at a 2048-byte aligned boundary
        size_t currentOffset = mAssetData.size();
        size_t remainder = currentOffset % 2048;
        if (remainder != 0) {
            size_t padding = 2048 - remainder;
            mAssetData.insert(mAssetData.end(), padding, 0);
        }

        AssetEntry entry{};
        std::strncpy(entry.name, name.c_str(), sizeof(entry.name) - 1);
        entry.offset = static_cast<uint32_t>(mAssetData.size());
        entry.length = static_cast<uint32_t>(data.size());
        mAssetDict.push_back(entry);
        
        mAssetData.insert(mAssetData.end(), data.begin(), data.end());
    }

    bool PakManager::save(const std::string& outputPath) {
        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open output file for MPAK serialization: " << outputPath << "\n";
            return false;
        }

        // Helper lambda for sector-aligned writing
        auto writePadded = [&file](const void* data, size_t size) {
            file.write(reinterpret_cast<const char*>(data), size);
            size_t remainder = size % 2048;
            if (remainder != 0) {
                size_t padding = 2048 - remainder;
                std::vector<char> padData(padding, 0);
                file.write(padData.data(), padding);
            }
        };

        MpakHeader6 header{};
        header.headerSize = static_cast<uint32_t>(sizeof(MpakHeader6));
        header.flags = mIsXboxTarget ? 1 : 0;
        header.vCount = static_cast<uint32_t>(mVertices.size());
        header.iCount = static_cast<uint32_t>(mIndices.size());

        header.lenVisibility = static_cast<uint32_t>(mVisibilityData.size());
        header.lenNodes = static_cast<uint32_t>(mNodesData.size());
        header.lenLeaves = static_cast<uint32_t>(mLeavesData.size());
        header.lenMarksurfaces = static_cast<uint32_t>(mMarksurfacesData.size());
        header.lenFaceDrawCmds = static_cast<uint32_t>(mFaceDrawCmds.size() * sizeof(FaceDrawCmd));
        header.lenPlanes = static_cast<uint32_t>(mPlanesData.size());
        header.lenClipnodes = static_cast<uint32_t>(mClipnodesData.size());
        header.lenModels = static_cast<uint32_t>(mModelsData.size());
        header.lenAssetDict = static_cast<uint32_t>(mAssetDict.size() * sizeof(AssetEntry));
        header.lenAssetData = static_cast<uint32_t>(mAssetData.size());

        header.lenEntities = static_cast<uint32_t>(mEntitiesData.size());
        header.lightmapWidth = mLightmapWidth;
        header.lightmapHeight = mLightmapHeight;
        header.lightmapLayers = mLightmapLayers;
        header.lightmapFormat = mLightmapFormat;
        header.lenLightmapAtlas = static_cast<uint32_t>(mLightmapAtlasData.size());

        header.lenFaceInfo = static_cast<uint32_t>(mFaceInfos.size() * sizeof(MpakFaceInfo));
        header.lenTexinfo = static_cast<uint32_t>(mTexinfoData.size());

        std::vector<uint8_t> dxtAtlas;
        if (mIsXboxTarget) {
            header.atlasWidth = mAtlasWidth;
            header.atlasHeight = mAtlasHeight;
            // Xbox atlas path: per-texture masked tracking doesn't exist here (many
            // textures share one shelf-packed atlas with no per-region alpha mode),
            // so punch-through is deferred - pass false, preserving the exact
            // (alpha-less) output this call always produced before A3 landed.
            dxtAtlas = TextureOpt::compressToDXT(mAtlasRgba.data(), mAtlasWidth, mAtlasHeight, false);
            header.lenAtlas = static_cast<uint32_t>(dxtAtlas.size());
        }

        std::vector<uint8_t> flat64, flat256, flat512;
        if (!mIsXboxTarget) {
            header.count64 = static_cast<uint32_t>(mArray64.size());
            for (size_t j = 0; j < mArray64.size(); ++j) {
                auto dxt = TextureOpt::compressToDXT(mArray64[j].data(), 64, 64, mArray64Masked[j]);
                flat64.insert(flat64.end(), dxt.begin(), dxt.end());
            }
            header.lenArray64 = static_cast<uint32_t>(flat64.size());

            header.count256 = static_cast<uint32_t>(mArray256.size());
            for (size_t j = 0; j < mArray256.size(); ++j) {
                auto dxt = TextureOpt::compressToDXT(mArray256[j].data(), 256, 256, mArray256Masked[j]);
                flat256.insert(flat256.end(), dxt.begin(), dxt.end());
            }
            header.lenArray256 = static_cast<uint32_t>(flat256.size());

            header.count512 = static_cast<uint32_t>(mArray512.size());
            for (size_t j = 0; j < mArray512.size(); ++j) {
                auto dxt = TextureOpt::compressToDXT(mArray512[j].data(), 512, 512, mArray512Masked[j]);
                flat512.insert(flat512.end(), dxt.begin(), dxt.end());
            }
            header.lenArray512 = static_cast<uint32_t>(flat512.size());
        }

        // Write Header
        writePadded(&header, sizeof(MpakHeader6));

        // Write Geometry
        writePadded(mVertices.data(), mVertices.size() * sizeof(Vertex));
        writePadded(mIndices.data(), mIndices.size() * sizeof(uint16_t));
        
        // Write BSP Lumps
        if (header.lenVisibility > 0) writePadded(mVisibilityData.data(), mVisibilityData.size());
        if (header.lenNodes > 0) writePadded(mNodesData.data(), mNodesData.size());
        if (header.lenLeaves > 0) writePadded(mLeavesData.data(), mLeavesData.size());
        if (header.lenMarksurfaces > 0) writePadded(mMarksurfacesData.data(), mMarksurfacesData.size());
        if (header.lenFaceDrawCmds > 0) writePadded(mFaceDrawCmds.data(), mFaceDrawCmds.size() * sizeof(FaceDrawCmd));
        if (header.lenPlanes > 0) writePadded(mPlanesData.data(), mPlanesData.size());
        if (header.lenClipnodes > 0) writePadded(mClipnodesData.data(), mClipnodesData.size());
        if (header.lenModels > 0) writePadded(mModelsData.data(), mModelsData.size());

        // Write Textures (DXT compressed)
        if (mIsXboxTarget) {
            if (header.lenAtlas > 0) writePadded(dxtAtlas.data(), dxtAtlas.size());
        } else {
            if (header.lenArray64 > 0) writePadded(flat64.data(), flat64.size());
            if (header.lenArray256 > 0) writePadded(flat256.data(), flat256.size());
            if (header.lenArray512 > 0) writePadded(flat512.data(), flat512.size());
        }

        // Write Assets
        if (header.lenAssetDict > 0) writePadded(mAssetDict.data(), mAssetDict.size() * sizeof(AssetEntry));
        if (header.lenAssetData > 0) writePadded(mAssetData.data(), mAssetData.size());

        // MPAKv4+ additions - appended last, mirroring header field order. The Rust
        // reader has no offset table, so this write order must never change without
        // also updating ferrum-vfs/src/mpak.rs in lockstep.
        if (header.lenEntities > 0) writePadded(mEntitiesData.data(), mEntitiesData.size());
        if (header.lenLightmapAtlas > 0) writePadded(mLightmapAtlasData.data(), mLightmapAtlasData.size());

        // MPAKv6 additions.
        if (header.lenFaceInfo > 0) writePadded(mFaceInfos.data(), mFaceInfos.size() * sizeof(MpakFaceInfo));
        if (header.lenTexinfo > 0) writePadded(mTexinfoData.data(), mTexinfoData.size());

        std::cout << "Successfully serialized MPAKv6 archive to " << outputPath << "\n";
        return true;
    }

} // namespace formats
