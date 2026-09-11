#pragma once
#include <vector>
#include <string>
#include <cstdint>

#include <unordered_map>
#include <cstring>

#include "MpakHeader.h"

namespace formats {

    struct Vertex {
        float x, y, z;
        float u, v;
        float lu, lv;
        uint32_t lightmap_layer;
        float rect[4]; // [atlas_x, atlas_y, atlas_w, atlas_h] normalized (Xbox)
        uint32_t texture_layer; // Texture2DArray index (PC)
        uint32_t array_index; // 0=64, 1=256, 2=512 (PC)
        uint32_t _pad;
        uint32_t _pad2; // Pads 60 -> 64 bytes: two clean 32-byte NV2A DMA cache lines.

        bool operator==(const Vertex& other) const {
            return std::memcmp(this, &other, sizeof(Vertex)) == 0;
        }
    };

    struct VertexHasher {
        std::size_t operator()(const Vertex& v) const {
            std::size_t hash = 0;
            const uint32_t* data = reinterpret_cast<const uint32_t*>(&v);
            for (size_t i = 0; i < sizeof(Vertex) / sizeof(uint32_t); ++i) {
                hash ^= data[i] + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    struct TextureLocation {
        uint32_t width = 64;
        uint32_t height = 64;
        uint32_t binSize = 64;
        uint32_t array_index; // 0=atlas/64, 1=256, 2=512
        uint32_t layer;
        float rect[4];
    };

    struct FaceDrawCmd {
        uint32_t start_index;
        uint32_t index_count;
    };

    struct AssetEntry {
        char name[64];
        uint32_t offset;
        uint32_t length;
    };

    class PakManager {
    public:
        PakManager();
        

        // masked: true for keyed/alpha-tested textures (STUDIO_NF_MASKED models,
        // SPR_ALPHTEST sprites) - upscales with nearest-neighbor instead of bilinear
        // (bilinear would smear opaque color into the transparent key at edges) and
        // marks the slice for punch-through BC1 alpha at save() time.
        TextureLocation addTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData, bool masked = false);

        uint16_t addVertex(const Vertex& v);
        void addIndex(uint16_t idx);
        void addFaceDrawCmd(uint32_t start_index, uint32_t index_count);
        void addFaceInfo(const MpakFaceInfo& info) { mFaceInfos.push_back(info); }
        // v8: one call per face per extra (non-style-0) lightstyle it uses -
        // see MpakHeader.h's v7 -> v8 note. Most faces never call this.
        void addExtraLightBlock(const MpakExtraLightBlock& block) { mExtraLightBlocks.push_back(block); }
        bool save(const std::string& outputPath);

        // Re-reads an archive and byte-compares every classic lump the file's
        // own dheader_t points at against the in-memory source it was built
        // from (docs/r3d/mpak-loading-design.md, §8 step 4) - independent of
        // the writer: offsets come from the file, bytes from the sources.
        // save() runs it on what it just wrote; main.cpp's --verify runs it
        // against an existing archive after baking the same map in memory,
        // which is what makes the negative controls (a flipped lighting,
        // palette or index byte in a copy) possible. The nv2a block's bytes
        // are outside this check - only its lengths are - see the doc.
        bool verifyFile(const std::string& path) const;
        void addAsset(const std::string& name, const std::vector<uint8_t>& data);

        void setVisibilityData(const std::vector<uint8_t>& data) { mVisibilityData = data; }
        void setNodesData(const std::vector<uint8_t>& data) { mNodesData = data; }
        void setLeavesData(const std::vector<uint8_t>& data) { mLeavesData = data; }
        void setMarksurfacesData(const std::vector<uint8_t>& data) { mMarksurfacesData = data; }
        void setPlanesData(const std::vector<uint8_t>& data) { mPlanesData = data; }
        void setClipnodesData(const std::vector<uint8_t>& data) { mClipnodesData = data; }
        void setModelsData(const std::vector<uint8_t>& data) { mModelsData = data; }
        void setEntitiesData(const std::vector<uint8_t>& data) { mEntitiesData = data; }
        // Raw LUMP_TEXINFO passthrough - MpakFaceInfo::texinfo indexes into this.
        void setTexinfoData(const std::vector<uint8_t>& data) { mTexinfoData = data; }

        // v7 classic core (MpakHeader.h's v6 -> v7 note): raw passthroughs of
        // the source lumps the engine's own loaders consume unchanged, plus
        // the embedded-miptex TEXTURES lump BspParser rebuilds from the WADs.
        void setVertexesData(const std::vector<uint8_t>& data) { mVertexesData = data; }
        void setEdgesData(const std::vector<uint8_t>& data) { mEdgesData = data; }
        void setSurfedgesData(const std::vector<uint8_t>& data) { mSurfedgesData = data; }
        void setFacesData(const std::vector<uint8_t>& data) { mFacesData = data; }
        void setLightingData(const std::vector<uint8_t>& data) { mLightingData = data; }
        void setMiptexData(std::vector<uint8_t> data) { mMiptexData = std::move(data); }

        // Profile: core (default) writes only the classic lumps; nv2a also
        // writes the baked block and sets MPAK_FLAG_NV2A_BLOCK.
        void setProfileNv2a(bool nv2a) { mProfileNv2a = nv2a; }
        bool isProfileNv2a() const { return mProfileNv2a; }

        // rgba must be exactly width*height*4*layers bytes, layer-major (all of page 0,
        // then all of page 1, ...). format is an MpakHeader6::lightmapFormat value
        // (0 = RGBA8 today).
        void setLightmapAtlas(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, std::vector<uint8_t> rgba) {
            mLightmapWidth = width;
            mLightmapHeight = height;
            mLightmapLayers = layers;
            mLightmapFormat = format;
            mLightmapAtlasData = std::move(rgba);
        }

        void setTargetXbox(bool xbox) { mIsXboxTarget = xbox; }
        bool isXboxTarget() const { return mIsXboxTarget; }

        void addArray64(const std::vector<uint8_t>& data) { mArray64.push_back(data); }
        void addArray256(const std::vector<uint8_t>& data) { mArray256.push_back(data); }

        uint32_t getAtlasWidth() const { return mAtlasWidth; }
        uint32_t getAtlasHeight() const { return mAtlasHeight; }

    private:
        std::vector<Vertex> mVertices;
        std::unordered_map<Vertex, uint16_t, VertexHasher> mVertexMap;
        std::vector<uint16_t> mIndices;
        std::vector<FaceDrawCmd> mFaceDrawCmds;
        std::vector<MpakFaceInfo> mFaceInfos;
        std::vector<MpakExtraLightBlock> mExtraLightBlocks;

        std::vector<uint8_t> mVisibilityData;
        std::vector<uint8_t> mNodesData;
        std::vector<uint8_t> mLeavesData;
        std::vector<uint8_t> mMarksurfacesData;
        std::vector<uint8_t> mPlanesData;
        std::vector<uint8_t> mClipnodesData;
        std::vector<uint8_t> mModelsData;
        std::vector<uint8_t> mEntitiesData;
        std::vector<uint8_t> mTexinfoData;

        std::vector<uint8_t> mVertexesData;
        std::vector<uint8_t> mEdgesData;
        std::vector<uint8_t> mSurfedgesData;
        std::vector<uint8_t> mFacesData;
        std::vector<uint8_t> mLightingData;
        std::vector<uint8_t> mMiptexData;
        bool mProfileNv2a = false;

        std::vector<uint8_t> mLightmapAtlasData;
        uint32_t mLightmapWidth = 0;
        uint32_t mLightmapHeight = 0;
        uint32_t mLightmapLayers = 0;
        uint32_t mLightmapFormat = 0;

        std::vector<AssetEntry> mAssetDict;
        std::vector<uint8_t> mAssetData;

        bool mIsXboxTarget = false;

        // PC Target (Texture2DArrays). mArrayNMasked[i] is parallel to mArrayN[i]:
        // true if that slice needs punch-through (1-bit) alpha in its BC1 encode -
        // set per-texture at addTexture() time, consumed in save()'s compress loop.
        std::vector<std::vector<uint8_t>> mArray64;
        std::vector<bool> mArray64Masked;
        std::vector<std::vector<uint8_t>> mArray256;
        std::vector<bool> mArray256Masked;
        std::vector<std::vector<uint8_t>> mArray512;
        std::vector<bool> mArray512Masked;

        // Xbox Target (Atlas)
        uint32_t mAtlasCurrentX = 0;
        uint32_t mAtlasCurrentY = 0;
        uint32_t mAtlasMaxRowHeight = 0;
        uint32_t mAtlasWidth = 2048;
        uint32_t mAtlasHeight = 2048;
        std::vector<uint8_t> mAtlasRgba;
    };

} // namespace formats
