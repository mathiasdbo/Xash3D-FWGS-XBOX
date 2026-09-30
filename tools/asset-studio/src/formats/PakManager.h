#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <iosfwd>

#include <unordered_map>
#include <cstring>

#include "MpakHeader.h"
#include "PhsBuilder.h"

namespace formats {

    struct Vertex {
        float x, y, z;
        float u, v;
        float lu, lv;
        uint32_t lightmap_layer;
        // rect[4] (the old atlas-placement remap, normalized ATLAS-space
        // [x,y,w,h]) is GONE as of MPAKv9 - see MpakHeader.h's v8 -> v9 note.
        // The Xbox path now addresses each texture independently with
        // hardware WRAP, selected per MpakTextureBatch rather than per
        // vertex, so there is nothing left for a per-vertex remap to do.
        uint32_t texture_layer; // Texture2DArray index (PC)
        uint32_t array_index; // 0=64, 1=256, 2=512 (PC)
        uint32_t _pad;
        uint32_t _pad2; // Pads 40 -> 48 bytes: still a clean multiple of 16 for NV2A DMA.

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
        // NATIVE (pre-resample) pixel dimensions of the source texture - LOAD
        // BEARING: BspParser.cpp normalizes its raw texture-space UVs by
        // these, not by whatever POT size the Xbox path resamples to
        // internally, so world-space UV resolution stays independent of any
        // later resample (see PakManager::addTexture's own doc comment).
        uint32_t width = 64;
        uint32_t height = 64;
        uint32_t binSize = 64; // PC path only: which fixed bin (64/256/512) this texture landed in
        uint32_t array_index; // PC: 0=64, 1=256, 2=512. Unused (0) on the Xbox path.
        uint32_t layer;       // PC: Texture2DArray layer. Unused (0) on the Xbox path.
        // v9, Xbox path only: index into PakManager's own per-texture list -
        // BspParser.cpp needs this to build MpakTextureBatch (see
        // MpakHeader.h's v8 -> v9 note). Unused (0) on the PC path, which has
        // no per-texture directory.
        uint32_t textureIndex = 0;
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

    // v10: the full DXT1 mip chain for one POT RGBA8 texture, base level
    // down to 1x1 - see PakManager.cpp's own definition comment for the
    // level-count/block-size details. Declared here (not just defined in
    // the .cpp's anonymous namespace, as it used to be) so
    // tests/dxt1_mip_chain_test.cpp can call it directly without linking a
    // copy of its own - the same reasoning downsampleRgba/resampleRgba
    // don't need (nothing outside this .cpp calls them).
    // dumpStream/dumpTextureIndex: optional per-level RGBA dump for
    // tools/verify-atlas-alpha.py (see the definition in PakManager.cpp).
    std::vector<uint8_t> buildDxt1MipChain(const std::vector<uint8_t>& baseRgba, uint32_t w, uint32_t h, bool masked,
                                           std::ofstream* dumpStream = nullptr, uint32_t dumpTextureIndex = 0);

    class PakManager {
    public:
        PakManager();
        

        // masked: true for keyed/alpha-tested textures (STUDIO_NF_MASKED models,
        // SPR_ALPHTEST sprites) - upscales with nearest-neighbor instead of bilinear
        // (bilinear would smear opaque color into the transparent key at edges) and
        // marks the slice for punch-through BC1 alpha at save() time.
        TextureLocation addTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData, bool masked = false);

        // v11: call once per face before its addVertex() calls, with the
        // face's vertex count. If the face would not fit in the current
        // 65,536-vertex chunk, a new chunk starts (and vertex dedup restarts
        // with it). Returns the chunk's base - the absolute index of its
        // first vertex - which the face's u16 indices are relative to.
        uint32_t beginFaceVertices(uint32_t faceVertexCount);
        // Returns the vertex's index RELATIVE to the current chunk (v11).
        uint16_t addVertex(const Vertex& v);
        // v11: one per sorted face, in the same order as addFaceDrawCmd().
        void addFaceVertexBase(uint32_t base) { mFaceVertexBase.push_back(base); }
        uint32_t vertexChunkCount() const { return mVertexChunkCount; }
        void addIndex(uint16_t idx);
        void addFaceDrawCmd(uint32_t start_index, uint32_t index_count);
        // v9: grows the MOST RECENTLY added FaceDrawCmd's own index_count by
        // extraIndexCount, with no matching new FaceDrawCmd entry - used only
        // to absorb the degenerate-triangle padding BspParser.cpp appends at
        // the end of each texture-draw batch so every batch's own index
        // range lands on a dword-aligned boundary (ref/nv2a's per-face/
        // per-batch draw-call snap needs this - see MpakTextureBatch's own
        // doc comment and ref/nv2a/r_context.c's nv2a_batch_dword_range()
        // declaration comment for the full "why" - a texture-batch-boundary
        // mistexturing bug this padding fixes at the source). No-op if no
        // FaceDrawCmd has been added yet (nothing to grow).
        void padLastFaceDrawCmd(uint32_t extraIndexCount) {
            if (!mFaceDrawCmds.empty()) mFaceDrawCmds.back().index_count += extraIndexCount;
        }
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
        // v13: the offline PHS this archive carries - Xbox nv2a bakes of a
        // map PhsBuilder.h can reproduce; empty otherwise, with `why` set.
        // Built from the retained lumps, so save() and a standalone
        // verifyFile() (--verify) agree on it.
        PhsData buildArchivePhs(std::string* why) const;
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
        // X9-W: the map's edict slots at the load peak and the game's
        // max_edicts (main.cpp), for the advisory hosting check.
        void setEdictBudget(uint32_t peak, uint32_t maxEdicts) { mPeakEdicts = peak; mMaxEdicts = maxEdicts; }
        bool isXboxTarget() const { return mIsXboxTarget; }

        void addArray64(const std::vector<uint8_t>& data) { mArray64.push_back(data); }
        void addArray256(const std::vector<uint8_t>& data) { mArray256.push_back(data); }

        // v9: BspParser.cpp builds both of these once, after sorting all
        // faces by (textureIndex, PVS leaf) - see MpakHeader.h's v8 -> v9
        // note. Batches are meaningful only on the Xbox path (save() writes
        // batchCount/lenBatches only when isXboxTarget()); the face remap is
        // written whenever the nv2a profile is active, since the face sort
        // itself is unconditional in BspParser's shared face loop.
        void setTextureBatches(std::vector<MpakTextureBatch> batches) { mTextureBatches = std::move(batches); }
        // X9-U: drops every Xbox texture no batch draws - every miptex the
        // map's WAD set names gets baked, used or not - and remaps the
        // batches' textureIndex to the compacted order. Call after
        // setTextureBatches(). Returns the number of textures dropped.
        //
        // X9-X item 14 step 9: appendAfter names textures (pre-prune
        // textureIndex values) to append after the compacted set, one new
        // entry each and in the order given, whether or not a batch draws
        // them - the animation frames no face references
        // (AnimTextureFrames.h). They must follow the drawn textures, in
        // this order, because ref/nv2a finds them there by position.
        size_t pruneUnusedXboxTextures(const std::vector<uint32_t>& appendAfter = {});
        void setFaceRemap(std::vector<uint32_t> remap) { mFaceRemap = std::move(remap); }

        // Read-only views for tests/anim_texture_frames_test.cpp.
        size_t xboxTextureCountForTest() const { return mXboxTextures.size(); }
        uint32_t xboxTextureWidthForTest(size_t i) const { return mXboxTextureWidths.at(i); }
        uint32_t xboxTextureHeightForTest(size_t i) const { return mXboxTextureHeights.at(i); }
        uint32_t textureBatchIndexForTest(size_t i) const { return mTextureBatches.at(i).textureIndex; }

    private:
        std::vector<Vertex> mVertices;
        std::unordered_map<Vertex, uint16_t, VertexHasher> mVertexMap; // current chunk only, chunk-relative indices
        uint32_t mVertexChunkBase = 0;   // v11: absolute index of the current chunk's first vertex
        uint32_t mPeakEdicts = 0;        // X9-W: setEdictBudget, 0 = unknown
        uint32_t mMaxEdicts = 0;
        uint32_t mVertexChunkCount = 1;  // v11: chunks used so far (a map always has at least one)
        std::vector<uint32_t> mFaceVertexBase; // v11: per sorted face, written only when mVertexChunkCount > 1
        std::vector<uint16_t> mIndices;
        std::vector<FaceDrawCmd> mFaceDrawCmds;
        std::vector<MpakFaceInfo> mFaceInfos;
        std::vector<MpakExtraLightBlock> mExtraLightBlocks;
        // v9: built by BspParser.cpp's sort, consumed by save() - see
        // MpakHeader.h's v8 -> v9 note and setTextureBatches()/setFaceRemap()'s
        // own doc comments above.
        std::vector<MpakTextureBatch> mTextureBatches;
        std::vector<uint32_t> mFaceRemap;

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

        // Xbox Target (v9: independent per-texture list, no shared atlas -
        // see MpakHeader.h's v8 -> v9 note). Each of the four vectors below
        // is parallel, indexed by TextureLocation::textureIndex; mXboxTextures
        // holds the already-POT-resampled RGBA8 source save() compresses to
        // DXT1 per-entry.
        std::vector<std::vector<uint8_t>> mXboxTextures;
        std::vector<bool> mXboxTexturesMasked;
        std::vector<uint32_t> mXboxTextureWidths;  // POT, post-resample
        std::vector<uint32_t> mXboxTextureHeights; // POT, post-resample

        // Set by addTexture() the moment a source texture's native
        // width/height exceeds NV2A's 4096 real texture-edge maximum -
        // same "warn now, save() refuses to write" discipline the old
        // atlas's own mAtlasOverflowed flag used (removed with the atlas
        // itself, v9). Without this, nearestPowerOfTwo()'s own 4096 ceiling
        // would silently clamp an out-of-range texture instead of failing
        // the bake loudly.
        bool mXboxTextureOversized = false;
    };

} // namespace formats
