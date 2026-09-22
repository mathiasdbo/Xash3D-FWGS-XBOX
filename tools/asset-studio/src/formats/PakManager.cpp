#include "PakManager.h"
#include "BspParser.h" // BspLumpType - the dheader_t lump indices
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include "../utils/rgbcx.h"
#include "../utils/TextureOpt.h"

namespace {

    // Bilinear resize of an RGBA8 image into an arbitrary `dstW`x`dstH`
    // buffer, with a nearest-neighbor special case for `masked` (keyed/
    // alpha-tested) textures - bilinear would blend opaque texel color into
    // the transparent key at masked edges, smearing a fringe punch-through
    // BC1 can't hide. This is the PC array-binning path's own original
    // inline resize loop, factored out so the Xbox per-texture path (v9,
    // MpakHeader.h's v8 -> v9 note) can reuse the exact same algorithm
    // instead of reinventing it - both call sites need identical resample
    // behavior, not just similar-looking code.
    void resampleRgba(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                       std::vector<uint8_t>& dst, uint32_t dstW, uint32_t dstH, bool masked) {
        dst.assign(static_cast<size_t>(dstW) * dstH * 4, 0);

        for (uint32_t y = 0; y < dstH; ++y) {
            float v_ratio = (srcH > 1) ? (static_cast<float>(y) / static_cast<float>(dstH - 1)) * static_cast<float>(srcH - 1) : 0.0f;
            uint32_t y0 = static_cast<uint32_t>(v_ratio);
            uint32_t y1 = std::min(y0 + 1, srcH - 1);
            float fy = v_ratio - y0;

            for (uint32_t x = 0; x < dstW; ++x) {
                float u_ratio = (srcW > 1) ? (static_cast<float>(x) / static_cast<float>(dstW - 1)) * static_cast<float>(srcW - 1) : 0.0f;
                uint32_t x0 = static_cast<uint32_t>(u_ratio);
                uint32_t x1 = std::min(x0 + 1, srcW - 1);
                float fx = u_ratio - x0;

                if (masked) {
                    uint32_t nx = (fx < 0.5f) ? x0 : x1;
                    uint32_t ny = (fy < 0.5f) ? y0 : y1;
                    for (int c = 0; c < 4; ++c) {
                        dst[(y * dstW + x) * 4 + c] = src[(ny * srcW + nx) * 4 + c];
                    }
                    continue;
                }

                for (int c = 0; c < 4; ++c) {
                    float p00 = src[(y0 * srcW + x0) * 4 + c];
                    float p10 = src[(y0 * srcW + x1) * 4 + c];
                    float p01 = src[(y1 * srcW + x0) * 4 + c];
                    float p11 = src[(y1 * srcW + x1) * 4 + c];

                    float val = p00 * (1.0f - fx) * (1.0f - fy) +
                                p10 * fx * (1.0f - fy) +
                                p01 * (1.0f - fx) * fy +
                                p11 * fx * fy;

                    dst[(y * dstW + x) * 4 + c] = static_cast<uint8_t>(val);
                }
            }
        }
    }

    // Always rounds UP to the next power-of-two - MpakTextureInfo::width/
    // height's own contract (MpakHeader.h). Deliberately NOT true-nearest
    // (round-to-closer, which can round DOWN): a real, measured codex-review
    // finding on this exact function found true-nearest silently loses
    // resolution on common GoldSrc sizes (miptex widths/heights are only
    // guaranteed multiples of 16, not POT - e.g. 80px would round DOWN to 64,
    // a 20% loss on that axis), which the PC array-bin path's own
    // always-round-up bucketing (the code this was adapted from, see
    // resampleRgba's own history) never risked. Round-up never loses native
    // resolution, at the cost of some texels of padding a WRAP-addressed
    // texture doesn't otherwise need - GPU memory, not visual quality, so
    // this is the safer default for a retail port. Clamped to a 4-pixel
    // floor so TextureOpt::compressToDXT (which walks the image in 4x4
    // blocks) always divides evenly - every real GoldSrc miptex is far
    // larger than that floor, so the floor is a defensive backstop, not a
    // case this baker expects to hit. Also clamped to a 4096-pixel ceiling,
    // matching NV2A's real maximum texture edge (ref/nv2a/r_context.c's own
    // atlas-era validation already established this same 4096 limit,
    // divergences.md #100) and ref/nv2a/r_context.c's MpakTextureInfo
    // validation (log2Width/log2Height <= 12) - callers must check
    // isNativeSizeRepresentable() first and skip/fail the texture rather
    // than silently ship one this ceiling had to clamp, since a real GoldSrc
    // texture is never anywhere near 4096px and hitting this means
    // something is wrong upstream, not a size to render best-effort.
    uint32_t nearestPowerOfTwo(uint32_t value) {
        if (value <= 4) return 4;
        uint32_t upper = 1;
        while (upper < value) upper <<= 1;
        if (upper > 4096) upper = 4096;
        return upper;
    }

    // True only when value's rounded-up POT (nearestPowerOfTwo's own return)
    // is achievable without exceeding NV2A's 4096 texture-edge ceiling -
    // i.e. value itself is <= 4096. No real GoldSrc content is expected to
    // trip this; it exists so a future malformed/unusual WAD fails the bake
    // loudly with a clear diagnostic instead of silently shipping a
    // quietly-downscaled 4096px texture that would look wrong with no
    // warning anywhere.
    bool isNativeSizeRepresentable(uint32_t value) {
        return value <= 4096;
    }

    // log2 of a value already known to be an exact power of two - the
    // BASE_SIZE_U/BASE_SIZE_V value NV097_SET_TEXTURE_FORMAT expects
    // (ref/nv2a/r_context.c's stage-0 texture setup).
    uint32_t log2OfPowerOfTwo(uint32_t value) {
        uint32_t log2 = 0;
        while ((1u << log2) < value) ++log2;
        return log2;
    }

} // namespace

namespace formats {

    PakManager::PakManager() {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    }

    TextureLocation PakManager::addTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData, bool masked) {
        TextureLocation loc{};
        // NATIVE (pre-resample) dimensions - see TextureLocation's own doc
        // comment for why neither branch below may overwrite these.
        loc.width = width;
        loc.height = height;

        if (mIsXboxTarget) {
            // v9: no shared atlas. Each source texture is independently
            // resampled to the nearest power-of-two PER AXIS and hardware
            // WRAP-addressed at render time (r_context.c's stage-0 texture
            // setup) instead of being shelf-packed into a shared canvas with
            // a per-vertex placement remap - see MpakHeader.h's v8 -> v9 note
            // for why the old atlas + remap could not represent a tiling
            // texture (frac(u) collapsed every repeat past the first onto
            // the same fractional value).
            if (!isNativeSizeRepresentable(width) || !isNativeSizeRepresentable(height)) {
                std::cerr << "ERROR: texture " << width << "x" << height << " exceeds NV2A's "
                             "4096px texture-edge maximum - flagging this bake to fail rather than "
                             "silently ship a clamped texture.\n";
                mXboxTextureOversized = true;
            }

            uint32_t potW = nearestPowerOfTwo(width);
            uint32_t potH = nearestPowerOfTwo(height);

            std::vector<uint8_t> resampled;
            resampleRgba(rgbaData, width, height, resampled, potW, potH, masked);

            loc.textureIndex = static_cast<uint32_t>(mXboxTextures.size());
            loc.array_index = 0;
            loc.layer = 0;
            loc.binSize = 0;

            mXboxTextures.push_back(std::move(resampled));
            mXboxTexturesMasked.push_back(masked);
            mXboxTextureWidths.push_back(potW);
            mXboxTextureHeights.push_back(potH);

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

            loc.binSize = binSize;
            loc.layer = static_cast<uint32_t>(targetArray->size());

            std::vector<uint8_t> slice;
            resampleRgba(rgbaData, width, height, slice, binSize, binSize, masked);
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

        // Helper lambda for sector-aligned writing. Returns the absolute
        // offset the section started at, so the dheader_t lump table below
        // is filled from the SAME cursor that laid the bytes out - it cannot
        // disagree with the descriptor's lengths by construction, and
        // filesystem/mpak_layout.c checks that it doesn't anyway.
        auto writePadded = [&file](const void* data, size_t size) -> uint32_t {
            uint32_t startOffset = static_cast<uint32_t>(file.tellp());
            file.write(reinterpret_cast<const char*>(data), size);
            size_t remainder = size % 2048;
            if (remainder != 0) {
                size_t padding = 2048 - remainder;
                std::vector<char> padData(padding, 0);
                file.write(padData.data(), padding);
            }
            return startOffset;
        };

        // v7 first sector: a real dheader_t at 0 and the descriptor at
        // MPAK_DESCRIPTOR_OFFSET (MpakHeader.h's v7 first-sector note). Both
        // depend on offsets that are only known once the sections are laid
        // out, so the sector is reserved now and rewritten at the end.
        MpakBspHeader bsp{};
        auto lump = [&bsp](BspLumpType which, uint32_t offset, uint32_t length) {
            // A lump with no bytes gets {0, 0}: Mod_LoadLump treats fileofs
            // 0 as "lump absent" and applies its own mincount rules to it.
            bsp.lumps[which].fileofs = length > 0 ? static_cast<int32_t>(offset) : 0;
            bsp.lumps[which].filelen = static_cast<int32_t>(length);
        };
        {
            std::vector<char> sector(2048, 0);
            file.write(sector.data(), sector.size());
        }

        // Core profile (the default): only the classic lumps are written and
        // every length in the baked block is 0 - see MpakHeader.h's v6 -> v7
        // note for why nothing before the X9 renderer can consume that block.
        // The block is still built in memory by BspParser::exportToPak either
        // way (it is cheap, and keeping one code path there is simpler than
        // two); the profile decides only what reaches the file.
        const bool nv2a = mProfileNv2a;

        MpakHeader6 header{};
        header.headerSize = static_cast<uint32_t>(sizeof(MpakHeader6));
        header.flags = (mIsXboxTarget ? MPAK_FLAG_XBOX : 0) | (nv2a ? MPAK_FLAG_NV2A_BLOCK : 0);
        header.vCount = nv2a ? static_cast<uint32_t>(mVertices.size()) : 0;
        header.iCount = nv2a ? static_cast<uint32_t>(mIndices.size()) : 0;

        header.lenVisibility = static_cast<uint32_t>(mVisibilityData.size());
        header.lenNodes = static_cast<uint32_t>(mNodesData.size());
        header.lenLeaves = static_cast<uint32_t>(mLeavesData.size());
        header.lenMarksurfaces = static_cast<uint32_t>(mMarksurfacesData.size());
        header.lenFaceDrawCmds = nv2a ? static_cast<uint32_t>(mFaceDrawCmds.size() * sizeof(FaceDrawCmd)) : 0;
        header.lenPlanes = static_cast<uint32_t>(mPlanesData.size());
        header.lenClipnodes = static_cast<uint32_t>(mClipnodesData.size());
        header.lenModels = static_cast<uint32_t>(mModelsData.size());
        header.lenAssetDict = static_cast<uint32_t>(mAssetDict.size() * sizeof(AssetEntry));
        header.lenAssetData = static_cast<uint32_t>(mAssetData.size());

        header.lenEntities = static_cast<uint32_t>(mEntitiesData.size());
        if (nv2a) {
            header.lightmapWidth = mLightmapWidth;
            header.lightmapHeight = mLightmapHeight;
            header.lightmapLayers = mLightmapLayers;
            header.lightmapFormat = mLightmapFormat;
            header.lenLightmapAtlas = static_cast<uint32_t>(mLightmapAtlasData.size());
            header.lenFaceInfo = static_cast<uint32_t>(mFaceInfos.size() * sizeof(MpakFaceInfo));
            header.lenExtraLightBlocks = static_cast<uint32_t>(mExtraLightBlocks.size() * sizeof(MpakExtraLightBlock));
            // v9: BspParser's face sort is unconditional in the shared nv2a
            // face loop (both targets), so the remap is written for both -
            // see MpakHeader.h's v8 -> v9 note and setFaceRemap()'s own doc.
            header.lenFaceRemap = static_cast<uint32_t>(mFaceRemap.size() * sizeof(uint32_t));
        }
        header.lenTexinfo = static_cast<uint32_t>(mTexinfoData.size());

        header.lenVertexes = static_cast<uint32_t>(mVertexesData.size());
        header.lenEdges = static_cast<uint32_t>(mEdgesData.size());
        header.lenSurfedges = static_cast<uint32_t>(mSurfedgesData.size());
        header.lenFaces = static_cast<uint32_t>(mFacesData.size());
        header.lenLighting = static_cast<uint32_t>(mLightingData.size());
        header.lenMiptex = static_cast<uint32_t>(mMiptexData.size());

        // v9: one DXT1 blob per texture, concatenated in directory order -
        // replaces the old shared-atlas compress call. Punch-through alpha is
        // still unconditionally on (same reasoning the old atlas call already
        // established: WadParser::decodeMiptex sets alpha=0 only on a masked
        // texture's own transparent-key palette index and 255 everywhere
        // else, so compressToDXT's per-pixel punch-through decision is
        // correct whether or not THIS particular texture is masked - passing
        // `mXboxTexturesMasked[j]` costs nothing and documents intent, but
        // functionally `true` unconditionally would produce identical bytes).
        // Ships linear/unswizzled, exactly like the old atlas did -
        // TextureOpt::swizzleNV2A is NOT called here; its one real call site
        // stays UiPacker.cpp's unrelated menu-background archive.
        std::vector<uint8_t> textureDataBlob;
        std::vector<MpakTextureInfo> textureDir;
        if (nv2a && mIsXboxTarget) {
            if (mXboxTextureOversized) {
                std::cerr << "ERROR: at least one texture exceeded NV2A's 4096px maximum "
                             "(see the addTexture() warning above) - refusing to write this archive.\n";
                return false;
            }

            // codex-review finding: nothing checked mXboxTextures.size()/
            // mTextureBatches.size() against ref/nv2a/r_context.c's own
            // fixed-capacity load-time arrays (NV2A_MAP_MAX_TEXTURES/
            // NV2A_MAP_MAX_BATCHES, both 512, r_context.c:226,244) before
            // this v9 rewrite removed the old atlas's own overflow guard
            // (mAtlasOverflowed) - a map with more unique used textures than
            // that cap would bake "successfully", pass verifyFile(), and
            // only fail to LOAD on real Xbox hardware. No shared header
            // exists between this C++ baker and the C engine for that
            // constant, so the literal 512 is duplicated here on purpose,
            // with this cross-reference comment as the thing that keeps them
            // in sync - if r_context.c's caps ever change, update this too.
            // Measured real headroom on the two staged retail maps (Gate
            // phase, this same v9 pass): c1a0 164/512 textures (32%),
            // crossfire 120/512 (23%) - comfortable margin, not a case this
            // baker expects to hit, but failing the BAKE loudly beats
            // shipping an archive that only fails on the console.
            constexpr uint32_t kNv2aMapMaxTextures = 512; // ref/nv2a/r_context.c:226 NV2A_MAP_MAX_TEXTURES
            constexpr uint32_t kNv2aMapMaxBatches = 512;  // ref/nv2a/r_context.c:244 NV2A_MAP_MAX_BATCHES
            if (mXboxTextures.size() > kNv2aMapMaxTextures) {
                std::cerr << "ERROR: " << mXboxTextures.size() << " unique textures exceeds "
                             "ref/nv2a/r_context.c's NV2A_MAP_MAX_TEXTURES (" << kNv2aMapMaxTextures
                          << ") - this archive would fail to load on the Xbox target. Failing the bake.\n";
                return false;
            }
            if (mTextureBatches.size() > kNv2aMapMaxBatches) {
                std::cerr << "ERROR: " << mTextureBatches.size() << " texture batches exceeds "
                             "ref/nv2a/r_context.c's NV2A_MAP_MAX_BATCHES (" << kNv2aMapMaxBatches
                          << ") - this archive would fail to load on the Xbox target. Failing the bake.\n";
                return false;
            }

            textureDir.reserve(mXboxTextures.size());
            for (size_t j = 0; j < mXboxTextures.size(); ++j) {
                auto dxt = TextureOpt::compressToDXT(mXboxTextures[j].data(),
                                                      static_cast<int>(mXboxTextureWidths[j]),
                                                      static_cast<int>(mXboxTextureHeights[j]),
                                                      mXboxTexturesMasked[j]);
                MpakTextureInfo info{};
                info.width = mXboxTextureWidths[j];
                info.height = mXboxTextureHeights[j];
                info.log2Width = log2OfPowerOfTwo(info.width);
                info.log2Height = log2OfPowerOfTwo(info.height);
                info.dataLength = static_cast<uint32_t>(dxt.size());
                textureDir.push_back(info);
                textureDataBlob.insert(textureDataBlob.end(), dxt.begin(), dxt.end());
            }
            header.textureCount = static_cast<uint32_t>(textureDir.size());
            header.lenTextureDir = static_cast<uint32_t>(textureDir.size() * sizeof(MpakTextureInfo));
            header.lenTextureData = static_cast<uint32_t>(textureDataBlob.size());

            header.batchCount = static_cast<uint32_t>(mTextureBatches.size());
            header.lenBatches = static_cast<uint32_t>(mTextureBatches.size() * sizeof(MpakTextureBatch));
        }

        std::vector<uint8_t> flat64, flat256, flat512;
        // Found by verifyFile() on the very first core bake: this block used
        // to run for every profile, so a core archive's descriptor claimed
        // 2.3 MB of texture arrays whose bytes were (correctly) never written.
        if (nv2a && !mIsXboxTarget) {
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

        // Write Geometry (nv2a profile only - header.vCount/iCount are 0 otherwise,
        // and writePadded with size 0 writes nothing)
        writePadded(mVertices.data(), header.vCount * sizeof(Vertex));
        writePadded(mIndices.data(), header.iCount * sizeof(uint16_t));

        // Write BSP Lumps. The write order below is the format - the descriptor
        // has no offset table and filesystem/mpak_layout.c walks these sections
        // in exactly this order; never change one without the other.
        if (header.lenVisibility > 0) lump(LUMP_VISIBILITY, writePadded(mVisibilityData.data(), mVisibilityData.size()), header.lenVisibility);
        if (header.lenNodes > 0) lump(LUMP_NODES, writePadded(mNodesData.data(), mNodesData.size()), header.lenNodes);
        if (header.lenLeaves > 0) lump(LUMP_LEAVES, writePadded(mLeavesData.data(), mLeavesData.size()), header.lenLeaves);
        if (header.lenMarksurfaces > 0) lump(LUMP_MARKSURFACES, writePadded(mMarksurfacesData.data(), mMarksurfacesData.size()), header.lenMarksurfaces);
        if (header.lenFaceDrawCmds > 0) writePadded(mFaceDrawCmds.data(), mFaceDrawCmds.size() * sizeof(FaceDrawCmd));
        if (header.lenPlanes > 0) lump(LUMP_PLANES, writePadded(mPlanesData.data(), mPlanesData.size()), header.lenPlanes);
        if (header.lenClipnodes > 0) lump(LUMP_CLIPNODES, writePadded(mClipnodesData.data(), mClipnodesData.size()), header.lenClipnodes);
        if (header.lenModels > 0) lump(LUMP_MODELS, writePadded(mModelsData.data(), mModelsData.size()), header.lenModels);

        // Write Textures (DXT compressed, nv2a profile only). Write order
        // matches the header's own field order (MpakHeader.h's v8 -> v9
        // note): texture directory, texture data, batches, then face remap -
        // all four independently skipped (0 bytes) when this bake doesn't
        // produce them, so an Xbox archive's PC array section below
        // contributes nothing and vice versa.
        if (nv2a) {
            if (mIsXboxTarget) {
                if (header.lenTextureDir > 0) writePadded(textureDir.data(), textureDir.size() * sizeof(MpakTextureInfo));
                if (header.lenTextureData > 0) writePadded(textureDataBlob.data(), textureDataBlob.size());
                if (header.lenBatches > 0) writePadded(mTextureBatches.data(), mTextureBatches.size() * sizeof(MpakTextureBatch));
            }
            if (header.lenFaceRemap > 0) writePadded(mFaceRemap.data(), mFaceRemap.size() * sizeof(uint32_t));
            if (!mIsXboxTarget) {
                if (header.lenArray64 > 0) writePadded(flat64.data(), flat64.size());
                if (header.lenArray256 > 0) writePadded(flat256.data(), flat256.size());
                if (header.lenArray512 > 0) writePadded(flat512.data(), flat512.size());
            }
        }

        // Write Assets (always empty since the X2 scope narrowing)
        if (header.lenAssetDict > 0) writePadded(mAssetDict.data(), mAssetDict.size() * sizeof(AssetEntry));
        if (header.lenAssetData > 0) writePadded(mAssetData.data(), mAssetData.size());

        // MPAKv4+ additions - appended in header field order.
        if (header.lenEntities > 0) lump(LUMP_ENTITIES, writePadded(mEntitiesData.data(), mEntitiesData.size()), header.lenEntities);
        if (header.lenLightmapAtlas > 0) writePadded(mLightmapAtlasData.data(), mLightmapAtlasData.size());

        // MPAKv6 additions.
        if (header.lenFaceInfo > 0) writePadded(mFaceInfos.data(), mFaceInfos.size() * sizeof(MpakFaceInfo));
        if (header.lenTexinfo > 0) lump(LUMP_TEXINFO, writePadded(mTexinfoData.data(), mTexinfoData.size()), header.lenTexinfo);

        // MPAKv7 additions: the classic core (MpakHeader.h's v6 -> v7 note).
        if (header.lenVertexes > 0) lump(LUMP_VERTICES, writePadded(mVertexesData.data(), mVertexesData.size()), header.lenVertexes);
        if (header.lenEdges > 0) lump(LUMP_EDGES, writePadded(mEdgesData.data(), mEdgesData.size()), header.lenEdges);
        if (header.lenSurfedges > 0) lump(LUMP_SURFEDGES, writePadded(mSurfedgesData.data(), mSurfedgesData.size()), header.lenSurfedges);
        if (header.lenFaces > 0) lump(LUMP_FACES, writePadded(mFacesData.data(), mFacesData.size()), header.lenFaces);
        if (header.lenLighting > 0) lump(LUMP_LIGHTING, writePadded(mLightingData.data(), mLightingData.size()), header.lenLighting);
        if (header.lenMiptex > 0) lump(LUMP_TEXTURES, writePadded(mMiptexData.data(), mMiptexData.size()), header.lenMiptex);

        // MPAKv8 addition (MpakHeader.h's v7 -> v8 note) - not a classic BSP
        // lump, same shape as lenFaceInfo/lenLightmapAtlas above.
        if (header.lenExtraLightBlocks > 0) writePadded(mExtraLightBlocks.data(), mExtraLightBlocks.size() * sizeof(MpakExtraLightBlock));

        // Now the first sector, with every offset known.
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&bsp), sizeof(bsp));
        file.seekp(MPAK_DESCRIPTOR_OFFSET);
        file.write(reinterpret_cast<const char*>(&header), sizeof(MpakHeader6));
        file.flush();
        if (!file.good()) {
            std::cerr << "Failed writing MPAK archive: " << outputPath << "\n";
            return false;
        }
        file.close();

        // A gate that cannot fail is not a gate: re-read what was just written
        // and compare it, lump by lump, against the sources.
        if (!verifyFile(outputPath)) {
            std::cerr << "Verification of the archive just written FAILED: " << outputPath << "\n";
            return false;
        }

        // header.version, not a literal "v7"/"v8": found while adding v8 -
        // this message had hardcoded "MPAKv7" through the v6->v7 bump too,
        // silently stale the moment version bumped again. Reading it back
        // from the struct that already carries it means this can't drift
        // out of sync with a future version bump either.
        std::cout << "Successfully serialized and verified MPAKv" << header.version << " archive ("
                  << (nv2a ? "nv2a" : "core") << " profile) to " << outputPath << "\n";
        return true;
    }

    bool PakManager::verifyFile(const std::string& path) const {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            std::cerr << "verify: cannot open " << path << "\n";
            return false;
        }
        const std::streamoff fileSize = in.tellg();
        if (fileSize < 2048) {
            std::cerr << "verify: " << path << " is smaller than one sector\n";
            return false;
        }
        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(data.data()), fileSize);
        if (!in.good()) {
            std::cerr << "verify: short read of " << path << "\n";
            return false;
        }

        MpakBspHeader bsp{};
        MpakHeader6 desc{};
        std::memcpy(&bsp, data.data(), sizeof(bsp));
        std::memcpy(&desc, data.data() + MPAK_DESCRIPTOR_OFFSET, sizeof(desc));

        bool ok = true;
        auto fail = [&ok](const std::string& what) { std::cerr << "verify: " << what << "\n"; ok = false; };

        if (bsp.version != static_cast<int32_t>(MPAK_BSP_VERSION)) fail("dheader_t version is not 30");
        if (std::memcmp(desc.magic, "MPAK", 4) != 0) fail("no MPAK descriptor at offset 128");
        if (desc.version != MpakHeader6{}.version) fail("descriptor version mismatch");
        if (desc.headerSize != sizeof(MpakHeader6)) fail("descriptor headerSize mismatch");
        {
            uint32_t probe = 0;
            std::memcpy(&probe, data.data() + sizeof(MpakBspHeader), sizeof(probe));
            if (probe != 0) fail("bytes after the dheader_t are not zero");
        }

        // Every classic lump: the file's dheader_t says where, the in-memory
        // source says what. The descriptor's own length field must agree too.
        struct Entry { BspLumpType lump; const std::vector<uint8_t>* src; uint32_t descLen; const char* name; };
        const Entry table[] = {
            { LUMP_ENTITIES,     &mEntitiesData,     desc.lenEntities,     "entities" },
            { LUMP_PLANES,       &mPlanesData,       desc.lenPlanes,       "planes" },
            { LUMP_TEXTURES,     &mMiptexData,       desc.lenMiptex,       "textures (miptex)" },
            { LUMP_VERTICES,     &mVertexesData,     desc.lenVertexes,     "vertexes" },
            { LUMP_VISIBILITY,   &mVisibilityData,   desc.lenVisibility,   "visibility" },
            { LUMP_NODES,        &mNodesData,        desc.lenNodes,        "nodes" },
            { LUMP_TEXINFO,      &mTexinfoData,      desc.lenTexinfo,      "texinfo" },
            { LUMP_FACES,        &mFacesData,        desc.lenFaces,        "faces" },
            { LUMP_LIGHTING,     &mLightingData,     desc.lenLighting,     "lighting" },
            { LUMP_CLIPNODES,    &mClipnodesData,    desc.lenClipnodes,    "clipnodes" },
            { LUMP_LEAVES,       &mLeavesData,       desc.lenLeaves,       "leaves" },
            { LUMP_MARKSURFACES, &mMarksurfacesData, desc.lenMarksurfaces, "marksurfaces" },
            { LUMP_EDGES,        &mEdgesData,        desc.lenEdges,        "edges" },
            { LUMP_SURFEDGES,    &mSurfedgesData,    desc.lenSurfedges,    "surfedges" },
            { LUMP_MODELS,       &mModelsData,       desc.lenModels,       "models" },
        };

        for (const Entry& e : table) {
            const MpakBspLump& l = bsp.lumps[e.lump];
            const size_t srcSize = e.src->size();
            if (e.descLen != srcSize) {
                fail(std::string(e.name) + ": descriptor length " + std::to_string(e.descLen) + " != source " + std::to_string(srcSize));
                continue;
            }
            if (srcSize == 0) {
                if (l.fileofs != 0 || l.filelen != 0) fail(std::string(e.name) + ": empty lump but dheader_t entry is not {0, 0}");
                continue;
            }
            if (static_cast<size_t>(l.filelen) != srcSize) {
                fail(std::string(e.name) + ": dheader_t length " + std::to_string(l.filelen) + " != source " + std::to_string(srcSize));
                continue;
            }
            if (l.fileofs < 2048 || l.fileofs % 2048 != 0 ||
                static_cast<uint64_t>(l.fileofs) + srcSize > static_cast<uint64_t>(fileSize)) {
                fail(std::string(e.name) + ": dheader_t offset " + std::to_string(l.fileofs) + " is not a sector offset inside the file");
                continue;
            }
            if (std::memcmp(data.data() + l.fileofs, e.src->data(), srcSize) != 0) {
                // Say where: the first differing byte is the whole story for a
                // corrupted copy (the negative controls) and a strong hint for
                // a writer bug.
                size_t at = 0;
                while (at < srcSize && data[l.fileofs + at] == (*e.src)[at]) ++at;
                fail(std::string(e.name) + ": byte " + std::to_string(at) + " of " + std::to_string(srcSize) + " differs from the source");
            }
        }

        // The baked block. Its DXT/atlas bytes are not retained after save
        // (see PakManager.h), so those are checked structurally (the
        // retained sections are byte-compared further down): every count
        // and length the descriptor declares must match what this bake holds,
        // and - found in review: an nv2a archive with vCount rewritten to
        // 67,108,864 (a 4 GiB vertex block) used to verify clean here while
        // the C reader rejected it - the descriptor's whole section walk must
        // land the fifteen classic lumps exactly where the file's dheader_t
        // says they are. Any wrong length or flag anywhere in the walk shifts
        // every classic lump after it, so this ties the baked block's extents
        // to the same truth the classic bytes were just checked against.
        const bool nv2a = (desc.flags & MPAK_FLAG_NV2A_BLOCK) != 0;
        const bool xbox = (desc.flags & MPAK_FLAG_XBOX) != 0;
        if (nv2a != mProfileNv2a) fail("descriptor profile flag does not match this bake's profile");
        if (xbox != mIsXboxTarget) fail("descriptor target flag does not match this bake's target");
        if (!nv2a) {
            if (desc.vCount || desc.iCount || desc.lenFaceDrawCmds || desc.lenFaceInfo || desc.lenLightmapAtlas ||
                desc.lenArray64 || desc.lenArray256 || desc.lenArray512 ||
                desc.lightmapWidth || desc.lightmapHeight || desc.lightmapLayers || desc.lenExtraLightBlocks ||
                desc.textureCount || desc.lenTextureDir || desc.lenTextureData ||
                desc.batchCount || desc.lenBatches || desc.lenFaceRemap)
                fail("core profile archive carries baked-block lengths");
        } else {
            if (desc.vCount != mVertices.size()) fail("vCount " + std::to_string(desc.vCount) + " != baked " + std::to_string(mVertices.size()));
            if (desc.iCount != mIndices.size()) fail("iCount " + std::to_string(desc.iCount) + " != baked " + std::to_string(mIndices.size()));
            if (desc.lenFaceDrawCmds != mFaceDrawCmds.size() * sizeof(FaceDrawCmd)) fail("lenFaceDrawCmds != baked");
            if (desc.lenFaceInfo != mFaceInfos.size() * sizeof(MpakFaceInfo)) fail("lenFaceInfo != baked");
            if (desc.lenExtraLightBlocks != mExtraLightBlocks.size() * sizeof(MpakExtraLightBlock)) fail("lenExtraLightBlocks != baked");
            if (desc.lenLightmapAtlas != mLightmapAtlasData.size() || desc.lightmapWidth != mLightmapWidth ||
                desc.lightmapHeight != mLightmapHeight || desc.lightmapLayers != mLightmapLayers ||
                desc.lightmapFormat != mLightmapFormat)
                fail("lightmap atlas fields != baked");
            // v9: the face sort/remap is unconditional in BspParser's shared
            // nv2a face loop, so lenFaceRemap is checked for both targets;
            // the per-texture directory/data/batches are Xbox-only (see
            // MpakHeader.h's v8 -> v9 note and save()'s own gating).
            if (desc.lenFaceRemap != mFaceRemap.size() * sizeof(uint32_t)) fail("lenFaceRemap != baked");
            if (xbox && (desc.lenArray64 || desc.lenArray256 || desc.lenArray512)) fail("xbox target carries PC texture arrays");
            if (!xbox && (desc.lenTextureDir || desc.lenTextureData || desc.lenBatches))
                fail("pc target carries xbox per-texture data");
            if (xbox) {
                if (desc.textureCount != mXboxTextures.size())
                    fail("textureCount " + std::to_string(desc.textureCount) + " != baked " + std::to_string(mXboxTextures.size()));
                if (desc.lenTextureDir != desc.textureCount * sizeof(MpakTextureInfo)) fail("lenTextureDir != textureCount * sizeof(MpakTextureInfo)");
                if (desc.batchCount != mTextureBatches.size())
                    fail("batchCount " + std::to_string(desc.batchCount) + " != baked " + std::to_string(mTextureBatches.size()));
                if (desc.lenBatches != mTextureBatches.size() * sizeof(MpakTextureBatch)) fail("lenBatches != baked");
            }
        }

        // Re-derive every section's offset from the descriptor, in write
        // order, exactly the way filesystem/mpak_layout.c does (and this
        // file's save() lays them out), then compare the classic lumps'
        // positions against the dheader_t.
        {
            uint64_t cursor = 2048;
            auto place = [&](uint64_t length) -> uint64_t {
                uint64_t at = cursor;
                if (length > 0) cursor += ((length + 2047) / 2048) * 2048;
                return at;
            };
            uint64_t at[MPAK_BSP_LUMPS] = {};
            const uint64_t vertexAt = place(static_cast<uint64_t>(desc.vCount) * sizeof(Vertex));
            const uint64_t indexAt = place(static_cast<uint64_t>(desc.iCount) * sizeof(uint16_t));
            at[LUMP_VISIBILITY] = place(desc.lenVisibility);
            at[LUMP_NODES] = place(desc.lenNodes);
            at[LUMP_LEAVES] = place(desc.lenLeaves);
            at[LUMP_MARKSURFACES] = place(desc.lenMarksurfaces);
            const uint64_t drawCmdAt = place(desc.lenFaceDrawCmds);
            at[LUMP_PLANES] = place(desc.lenPlanes);
            at[LUMP_CLIPNODES] = place(desc.lenClipnodes);
            at[LUMP_MODELS] = place(desc.lenModels);
            // v9: texture directory/data/batches (Xbox only), then the face
            // remap (both targets), then the PC arrays (PC only) - matches
            // save()'s own write order, which matches the header's field
            // order (MpakHeader.h's v8 -> v9 note).
            uint64_t batchesAt = 0;
            if (xbox) {
                place(desc.lenTextureDir);
                place(desc.lenTextureData);
                batchesAt = place(desc.lenBatches);
            }
            const uint64_t faceRemapAt = place(desc.lenFaceRemap);
            if (!xbox) {
                place(desc.lenArray64);
                place(desc.lenArray256);
                place(desc.lenArray512);
            }
            place(desc.lenAssetDict);
            place(desc.lenAssetData);
            at[LUMP_ENTITIES] = place(desc.lenEntities);
            const uint64_t lightmapAt = place(desc.lenLightmapAtlas);
            const uint64_t faceInfoAt = place(desc.lenFaceInfo);
            at[LUMP_TEXINFO] = place(desc.lenTexinfo);
            at[LUMP_VERTICES] = place(desc.lenVertexes);
            at[LUMP_EDGES] = place(desc.lenEdges);
            at[LUMP_SURFEDGES] = place(desc.lenSurfedges);
            at[LUMP_FACES] = place(desc.lenFaces);
            at[LUMP_LIGHTING] = place(desc.lenLighting);
            at[LUMP_TEXTURES] = place(desc.lenMiptex);
            const uint64_t extraLightBlocksAt = place(desc.lenExtraLightBlocks);

            if (cursor != static_cast<uint64_t>(fileSize))
                fail("descriptor sections end at " + std::to_string(cursor) + " but the file is " + std::to_string(fileSize) + " bytes");

            for (const Entry& e : table) {
                const MpakBspLump& l = bsp.lumps[e.lump];
                const uint64_t expectedOfs = e.src->empty() ? 0 : at[e.lump];
                if (static_cast<uint64_t>(l.fileofs) != expectedOfs)
                    fail(std::string(e.name) + ": dheader_t offset " + std::to_string(l.fileofs) +
                         " but the descriptor's section walk puts it at " + std::to_string(expectedOfs));
            }

            // The baked sections this object still holds are compared byte
            // for byte at the offsets the walk just derived. Found by a
            // negative control: with only the count checks above, an nv2a
            // archive with a flipped vertex byte verified clean. The DXT
            // texture arrays/atlas remain structural-only - their bytes are
            // produced inside save() and not retained.
            auto checkBaked = [&](const char* name, uint64_t ofs, const void* src, uint64_t size) {
                if (size == 0) return;
                if (ofs + size > static_cast<uint64_t>(fileSize)) {
                    fail(std::string(name) + ": section runs past the end of the file");
                    return;
                }
                if (std::memcmp(data.data() + ofs, src, static_cast<size_t>(size)) != 0) {
                    const uint8_t* s = static_cast<const uint8_t*>(src);
                    uint64_t bad = 0;
                    while (bad < size && data[static_cast<size_t>(ofs + bad)] == s[bad]) ++bad;
                    fail(std::string(name) + ": byte " + std::to_string(bad) + " of " + std::to_string(size) + " differs from the baked data");
                }
            };
            if (nv2a) {
                checkBaked("baked vertices", vertexAt, mVertices.data(), static_cast<uint64_t>(mVertices.size()) * sizeof(Vertex));
                checkBaked("baked indices", indexAt, mIndices.data(), static_cast<uint64_t>(mIndices.size()) * sizeof(uint16_t));
                checkBaked("face draw cmds", drawCmdAt, mFaceDrawCmds.data(), static_cast<uint64_t>(mFaceDrawCmds.size()) * sizeof(FaceDrawCmd));
                checkBaked("lightmap atlas", lightmapAt, mLightmapAtlasData.data(), mLightmapAtlasData.size());
                checkBaked("face info", faceInfoAt, mFaceInfos.data(), static_cast<uint64_t>(mFaceInfos.size()) * sizeof(MpakFaceInfo));
                checkBaked("extra light blocks", extraLightBlocksAt, mExtraLightBlocks.data(), static_cast<uint64_t>(mExtraLightBlocks.size()) * sizeof(MpakExtraLightBlock));
                // v9: retained (unlike the per-texture DXT directory/data,
                // which - like the old atlas - are produced inside save()
                // and not kept around) so these get the same full byte
                // comparison as face draw cmds/face info above.
                if (xbox) checkBaked("texture batches", batchesAt, mTextureBatches.data(), static_cast<uint64_t>(mTextureBatches.size()) * sizeof(MpakTextureBatch));
                checkBaked("face remap", faceRemapAt, mFaceRemap.data(), static_cast<uint64_t>(mFaceRemap.size()) * sizeof(uint32_t));
            }
        }

        return ok;
    }

} // namespace formats
