#include "PakManager.h"
#include "BspParser.h" // BspLumpType - the dheader_t lump indices
#include "Nv2aMapBudget.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include "../utils/rgbcx.h"
#include "../utils/TextureOpt.h"
#include "mpak_texture_dir.h" // filesystem/mpak_texture_dir.h - the v10 texture-directory rule,
                              // shared with ref/nv2a and the PC tests rather than duplicated
                              // here (see that header's own file comment)

// Ties MpakTextureInfo (this file's own on-disk directory-entry struct) to
// mpak_texture_dir.h's byte-offset constants, the same way
// filesystem/mpak_layout.h's own STATIC_ASSERT ties mpak_texture_info_t to
// them - both structs must independently agree with the ONE set of
// offsets Mpak_ValidateTextureDirectory() reads by hand.
static_assert(sizeof(formats::MpakTextureInfo) == MPAK_TEXTURE_INFO_SIZE,
              "MpakTextureInfo must match MPAK_TEXTURE_INFO_SIZE (filesystem/mpak_texture_dir.h)");

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

    // v10: one 2x2 box-filter step, POT in and out, either axis allowed to
    // be 1 already (a 64x1 level halves to 32x1). For a masked texture the
    // colour averages only the opaque (alpha >= 128) source texels, so the
    // transparent key colour never bleeds into a cutout's edge at distance;
    // alpha itself is the plain average, which compressToDXT then
    // thresholds at 128 exactly as it does for level 0.
    void downsampleRgba(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                        std::vector<uint8_t>& dst, uint32_t dstW, uint32_t dstH, bool masked) {
        dst.assign(static_cast<size_t>(dstW) * dstH * 4, 0);
        const uint32_t fx = srcW / dstW; // 1 or 2
        const uint32_t fy = srcH / dstH;
        for (uint32_t y = 0; y < dstH; ++y) {
            for (uint32_t x = 0; x < dstW; ++x) {
                uint32_t sum[4] = {0, 0, 0, 0}, opaque[3] = {0, 0, 0};
                uint32_t n = 0, nOpaque = 0;
                for (uint32_t oy = 0; oy < fy; ++oy) {
                    for (uint32_t ox = 0; ox < fx; ++ox) {
                        const uint8_t* t = &src[((static_cast<size_t>(y) * fy + oy) * srcW + (x * fx + ox)) * 4];
                        for (int c = 0; c < 4; ++c) sum[c] += t[c];
                        if (t[3] >= 128) {
                            for (int c = 0; c < 3; ++c) opaque[c] += t[c];
                            ++nOpaque;
                        }
                        ++n;
                    }
                }
                uint8_t* d = &dst[(static_cast<size_t>(y) * dstW + x) * 4];
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>((masked && nOpaque > 0) ? (opaque[c] + nOpaque / 2) / nOpaque : (sum[c] + n / 2) / n);
                d[3] = static_cast<uint8_t>((sum[3] + n / 2) / n);
            }
        }
    }

    // CRC-32 (ISO 3309 / IEEE 802.3, poly 0xEDB88320 reflected, init/final
    // 0xFFFFFFFF) - the same algorithm zlib::crc32 and Python's stdlib
    // zlib.crc32/binascii.crc32 implement, a standard rather than an
    // invented-here hash (checked against the standard "123456789" ->
    // 0xCBF43926 test vector). Used only to bind one dump record to the
    // EXACT compressed bytes it was measured against - see buildDxt1MipChain's
    // dumpStream doc comment below - not for any integrity/storage purpose,
    // so the simple bit-by-bit form is fine: it runs once per mip level at
    // bake time, not a per-texel hot path.
    uint32_t crc32Ieee(const uint8_t* data, size_t len) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                uint32_t mask = 0u - (crc & 1u);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }
} // namespace

namespace formats {

    // v10: the full DXT1 mip chain for one POT texture, levels back to back
    // from base size down to 1x1 (max(log2w, log2h) + 1 levels - NV2A's
    // MIPMAP_LEVELS counts every level, and a chain ending early would
    // leave the smallest LODs undefined). A level narrower than 4 texels
    // still occupies whole 4x4 blocks (ceil(w/4) * ceil(h/4) * 8 bytes, the
    // same size NV2A addresses for it): it is edge-replicated up to 4x4
    // before compressToDXT, which only walks complete blocks.
    //
    // Moved out of this file's own anonymous namespace (X9 v10 follow-up)
    // so tests/dxt1_mip_chain_test.cpp can call it and cross-check its
    // output size against Mpak_Dxt1ChainLength (filesystem/
    // mpak_texture_dir.h) - no behaviour change, downsampleRgba stays
    // internal since nothing outside this file needs it directly.
    //
    // dumpStream/dumpTextureIndex (both optional, default off - the
    // defaults live on the declaration in PakManager.h): when set,
    // this writes one record per level - the EXACT buffer about to be
    // compressed (*toCompress`, i.e. already edge-replicated/padded when the
    // level is under 4 texels), plus a CRC-32 of the compressed bytes this
    // same call produces - to a side channel tools/verify-atlas-alpha.py
    // decodes and compares against the archive's own compressed bytes.
    // fixOpaqueTexelsPunchedThrough (TextureOpt.cpp) can leave a block's
    // encode unfixed if its retry fails verification, so whether the punch-
    // through invariant (divergences.md #99) actually holds is an empirical
    // property of the compressed bytes, not something derivable from the
    // archive alone - this is the only place that still has the real,
    // pre-compression source to check it against. The digest exists so the
    // verifier can tell a dump taken from THIS bake apart from a stale one
    // left over from an earlier bake with matching texture dimensions
    // (codex-review finding, X9-V10-tooling QA round 1: dimension-only
    // matching let a stale dump silently pass against a different archive).
    std::vector<uint8_t> buildDxt1MipChain(const std::vector<uint8_t>& base, uint32_t w, uint32_t h, bool masked,
                                            std::ofstream* dumpStream, uint32_t dumpTextureIndex) {
        std::vector<uint8_t> chain;
        std::vector<uint8_t> level = base, next, padded;
        uint32_t lw = w, lh = h;
        uint32_t dumpLevel = 0;
        for (;;) {
            const uint32_t pw = std::max<uint32_t>(lw, 4), ph = std::max<uint32_t>(lh, 4);
            const std::vector<uint8_t>* toCompress = &level;
            if (pw != lw || ph != lh) {
                padded.assign(static_cast<size_t>(pw) * ph * 4, 0);
                for (uint32_t y = 0; y < ph; ++y)
                    for (uint32_t x = 0; x < pw; ++x)
                        std::memcpy(&padded[(static_cast<size_t>(y) * pw + x) * 4],
                                    &level[(static_cast<size_t>(std::min(y, lh - 1)) * lw + std::min(x, lw - 1)) * 4], 4);
                toCompress = &padded;
            }
            auto dxt = TextureOpt::compressToDXT(toCompress->data(), static_cast<int>(pw), static_cast<int>(ph), masked);
            if (dumpStream != nullptr) {
                uint32_t digest = crc32Ieee(dxt.data(), dxt.size());
                uint32_t rec[5] = {dumpTextureIndex, dumpLevel, pw, ph, digest};
                dumpStream->write(reinterpret_cast<const char*>(rec), sizeof(rec));
                dumpStream->write(reinterpret_cast<const char*>(toCompress->data()), static_cast<std::streamsize>(toCompress->size()));
            }
            chain.insert(chain.end(), dxt.begin(), dxt.end());
            if (lw == 1 && lh == 1)
                break;
            const uint32_t nw = std::max<uint32_t>(lw / 2, 1), nh = std::max<uint32_t>(lh / 2, 1);
            downsampleRgba(level, lw, lh, next, nw, nh, masked);
            level.swap(next);
            lw = nw;
            lh = nh;
            ++dumpLevel;
        }
        return chain;
    }

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



    uint32_t PakManager::beginFaceVertices(uint32_t faceVertexCount) {
        // v11: per-face lightmap UVs (lu/lv/lightmap_layer) mean vertices are
        // barely shared across faces, so a large map's vertex count approaches
        // its raw face-vertex sum and can outgrow u16 indices. Rather than
        // wrap (corrupt geometry) or abort (four retail maps used to), start a
        // new chunk when this face would not fit in the current one - worst
        // case, assuming none of its vertices dedupe. A face never spans two
        // chunks, so its u16 indices are always relative to one base.
        const uint64_t used = static_cast<uint64_t>(mVertices.size()) - mVertexChunkBase;
        if (used + faceVertexCount > 65536u) {
            mVertexChunkBase = static_cast<uint32_t>(mVertices.size());
            mVertexMap.clear(); // dedup never reaches back into an earlier chunk
            ++mVertexChunkCount;
        }
        return mVertexChunkBase;
    }

    uint16_t PakManager::addVertex(const Vertex& v) {
        auto it = mVertexMap.find(v);
        if (it != mVertexMap.end()) {
            return it->second;
        }

        // beginFaceVertices() already guaranteed room for this face; only a
        // caller that skipped it (or a single face over 65,536 vertices, which
        // BSP30 cannot express) could get here with the chunk full.
        if (mVertices.size() - mVertexChunkBase >= 65536) {
            std::cerr << "FATAL: vertex chunk overflow - addVertex called without a "
                         "matching beginFaceVertices() for this face.\n";
            std::abort();
        }

        uint16_t index = static_cast<uint16_t>(mVertices.size() - mVertexChunkBase);
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
            // v11: only a multi-chunk map ships the per-face base table.
            header.lenFaceVertexBase = mVertexChunkCount > 1 ? static_cast<uint32_t>(mFaceVertexBase.size() * sizeof(uint32_t)) : 0;
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
            // NV2A_MAP_MAX_BATCHES, both 512, r_context.c NV2A_MAP_MAX_TEXTURES) before
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
            constexpr uint32_t kNv2aMapMaxTextures = 512; // ref/nv2a/r_context.c NV2A_MAP_MAX_TEXTURES
            constexpr uint32_t kNv2aMapMaxBatches = 512;  // ref/nv2a/r_context.c NV2A_MAP_MAX_BATCHES
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

            // Pre-compression RGBA source dump, successor to the R3D_DUMP_ATLAS_RGBA
            // hook removed in 1604d2e7 (#127) when the shared atlas went away in v9.
            // Gated on this exact branch (nv2a && mIsXboxTarget): only here does
            // buildDxt1MipChain ever run, so only here is there anything to dump.
            // See buildDxt1MipChain's own doc comment for why this needs to be the
            // real pre-compression buffer rather than anything reconstructible from
            // the archive alone.
            std::ofstream dumpFile;
            std::ofstream* dumpStream = nullptr;
            if (const char* dumpPath = std::getenv("R3D_DUMP_TEXTURE_RGBA")) {
                dumpFile.open(dumpPath, std::ios::binary | std::ios::trunc);
                if (!dumpFile.is_open()) {
                    std::cerr << "ERROR: R3D_DUMP_TEXTURE_RGBA is set to " << dumpPath
                              << " but that file could not be opened for writing - failing the bake "
                                 "rather than silently skipping the dump.\n";
                    return false;
                }
                const char magic[4] = {'R', 'T', 'X', 'D'};
                // v2: each record also carries a CRC-32 of the compressed bytes
                // it was measured against (buildDxt1MipChain's dumpStream doc
                // comment) - v1 had dimensions only, which let a stale dump from
                // a different bake pair silently with today's archive.
                const uint32_t dumpVersion = 2;
                dumpFile.write(magic, sizeof(magic));
                dumpFile.write(reinterpret_cast<const char*>(&dumpVersion), sizeof(dumpVersion));
                dumpStream = &dumpFile;
            }

            textureDir.reserve(mXboxTextures.size());
            for (size_t j = 0; j < mXboxTextures.size(); ++j) {
                // v10: the whole mip chain (buildDxt1MipChain), then zero
                // padding to the next 128 bytes, Xbox D3D's own texture
                // base alignment. v9's single levels were always 128-aligned
                // by accident: GoldSrc miptex sizes are multiples of 16, so
                // every POT texture was at least 16x16 = 128 bytes of DXT1.
                auto dxt = buildDxt1MipChain(mXboxTextures[j],
                                             mXboxTextureWidths[j],
                                             mXboxTextureHeights[j],
                                             mXboxTexturesMasked[j],
                                             dumpStream, static_cast<uint32_t>(j));
                MpakTextureInfo info{};
                info.width = mXboxTextureWidths[j];
                info.height = mXboxTextureHeights[j];
                info.log2Width = log2OfPowerOfTwo(info.width);
                info.log2Height = log2OfPowerOfTwo(info.height);
                info.dataLength = static_cast<uint32_t>(dxt.size());
                textureDir.push_back(info);
                textureDataBlob.insert(textureDataBlob.end(), dxt.begin(), dxt.end());
                textureDataBlob.resize((textureDataBlob.size() + 127) & ~static_cast<size_t>(127), 0);
            }

            if (dumpStream != nullptr) {
                dumpStream->flush();
                if (!dumpStream->good()) {
                    std::cerr << "ERROR: writing the R3D_DUMP_TEXTURE_RGBA dump failed partway through - "
                                 "failing the bake rather than leaving a truncated dump a verifier would "
                                 "misread as complete.\n";
                    return false;
                }
            }

            header.textureCount = static_cast<uint32_t>(textureDir.size());
            header.lenTextureDir = static_cast<uint32_t>(textureDir.size() * sizeof(MpakTextureInfo));
            header.lenTextureData = static_cast<uint32_t>(textureDataBlob.size());

            header.batchCount = static_cast<uint32_t>(mTextureBatches.size());
            header.lenBatches = static_cast<uint32_t>(mTextureBatches.size() * sizeof(MpakTextureBatch));
        } else if (std::getenv("R3D_DUMP_TEXTURE_RGBA") != nullptr) {
            // Nothing calls buildDxt1MipChain outside the nv2a+Xbox branch above,
            // so there is nothing to dump for this target/profile - say so once,
            // and write no file at all, so tools/verify-atlas-alpha.py fails on a
            // missing file (loud) rather than an empty-but-well-formed one (quiet).
            std::cerr << "WARNING: R3D_DUMP_TEXTURE_RGBA is set but this bake is not "
                         "--target xbox --profile nv2a - no texture chains are baked for "
                         "this target/profile, so no dump is written.\n";
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
        if (header.lenFaceVertexBase > 0) writePadded(mFaceVertexBase.data(), mFaceVertexBase.size() * sizeof(uint32_t));

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

        // Bake-time mirror of ref/nv2a's own per-map memory ledger
        // (Nv2aMapBudget.h, ref/nv2a/r_context.c nv2a_load_map_geometry) - runs after
        // verifyFile, not before: the check needs the archive's real
        // on-disk size (the ref's own `buffersize` term is the literal
        // FS_LoadFile length, engine/common/model.c:330,387), and a
        // pre-write estimate would have to re-derive the 2048-byte sector
        // padding this function's own writer already applies. Gated the
        // same way as the two 512 caps above: a PC-target archive has no
        // nv2a block at all (the ref itself returns before its ledger at
        // r_context.c nv2a_load_map_geometry when textureDir/textureData/batches are
        // empty), so there is nothing to check outside the Xbox nv2a
        // profile.
        //
        // This mirrors the ref's own per-map ledger and nothing more - see
        // Nv2aMapBudget.h's own comment for what it does NOT cover (the
        // engine's static state, pbkit's buffers, every other subsystem,
        // and fragmentation; NV2A_MAP_BUDGET_BYTES's own comment names the
        // same limits).
        //
        // Post-write cleanup, not a transactional temp-path write: the
        // check needs the real on-disk size (see above), and verifyFile -
        // the other gate that would have to move into a temp-path scheme
        // too - is owned by the concurrent x9-v10-texture-validation topic
        // and out of scope to touch here. Every failure branch below that
        // runs after the archive already exists on disk must therefore
        // remove it itself, and must check that the removal actually
        // succeeded (round-1 review finding: a failed stat or a failed
        // remove() both silently left a full-size, unchecked .mpak behind,
        // newer than its .bsp, which any later non-forced re-stage could
        // then treat as current).
        if (nv2a && mIsXboxTarget) {
            std::error_code ec;
            const uint64_t fileSize = std::filesystem::file_size(outputPath, ec);
            if (ec) {
                std::cerr << "ERROR: could not stat " << outputPath << " to compute its nv2a map budget: "
                          << ec.message() << "\n";
                // The archive is already fully written and verifyFile-passed
                // at this point, so leaving it behind unchecked is exactly
                // the stale-archive danger the over-budget branch's own
                // remove() below exists to prevent - a stat failure earns
                // the same cleanup, not a silent skip.
                std::error_code removeEc;
                const bool removed = std::filesystem::remove(outputPath, removeEc);
                if (removeEc || !removed) {
                    std::cerr << "ERROR: also failed to remove " << outputPath
                              << " after the stat failure above: "
                              << (removeEc ? removeEc.message() : "no such file") << "\n";
                }
                return false;
            }

            const uint64_t vbBytes = static_cast<uint64_t>(header.vCount) * kNv2aLitVertexBytes;
            const uint64_t idxBytes = ((static_cast<uint64_t>(header.iCount) + 1) / 2) * 4;
            const uint64_t facesBytes = static_cast<uint64_t>(header.lenFaceDrawCmds / 8) * kNv2aMapFaceBytes;
            const uint64_t visBytes = static_cast<uint64_t>(header.lenFaceDrawCmds) / 8;
            const uint64_t remapBytes = static_cast<uint64_t>(header.lenFaceRemap) * 2;
            const uint64_t texBytes = static_cast<uint64_t>(header.lenTextureData);
            const uint64_t lmBytes = static_cast<uint64_t>(header.lenLightmapAtlas);
            const uint64_t total = nv2aMapLedgerBytes(header, fileSize);
            const double pct = 100.0 * static_cast<double>(total) / static_cast<double>(kNv2aMapBudgetBytes);

            if (!nv2aMapWithinBudget(total)) {
                std::cerr << "ERROR: nv2a map ledger " << total
                          << " bytes exceeds ref/nv2a/r_context.c's NV2A_MAP_BUDGET_BYTES ("
                          << kNv2aMapBudgetBytes
                          << ") - this archive would not draw on the Xbox target. Failing the bake.\n"
                          << "  [vb=" << vbBytes << " idx=" << idxBytes << " faces=" << facesBytes
                          << " vis=" << visBytes << " remap=" << remapBytes << " tex=" << texBytes
                          << " lm=" << lmBytes << " file=" << fileSize << "]\n";
                // The 512 caps above fail before any section is written, so
                // they only ever leave a 2048-byte stub behind. This check
                // runs after the full archive is written and verified, so
                // without an explicit remove a left-over full-size .mpak
                // would sit there newer than its source .bsp - main.cpp
                // writes <map>.assets.txt BEFORE calling save() (main.cpp
                // ~520 vs ~539), so a later non-forced re-stage could treat
                // this failed bake as already current, and ship an archive
                // that cannot draw
                // its real geometry on the console.
                //
                // Check the removal itself rather than firing and
                // forgetting: a real Windows file-lock/AV/permission
                // failure here would leave the over-budget archive behind
                // despite the ERROR above and the exit 1 in main.cpp,
                // reproducing the exact same stale-archive danger with no
                // trace in the log (round-1 review finding).
                std::error_code removeEc;
                const bool removed = std::filesystem::remove(outputPath, removeEc);
                if (removeEc || !removed) {
                    std::cerr << "ERROR: also failed to remove the over-budget archive " << outputPath
                              << ": " << (removeEc ? removeEc.message() : "no such file") << "\n";
                }
                return false;
            }

            std::cout << std::fixed << std::setprecision(1)
                      << "nv2a map budget: " << total << " / " << kNv2aMapBudgetBytes
                      << " bytes (" << pct << "%) [vb=" << vbBytes << " idx=" << idxBytes
                      << " faces=" << facesBytes << " vis=" << visBytes << " remap=" << remapBytes
                      << " tex=" << texBytes << " lm=" << lmBytes << " file=" << fileSize << "]\n"
                      << std::defaultfloat;
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
                desc.lightmapWidth || desc.lightmapHeight || desc.lightmapLayers || desc.lenExtraLightBlocks || desc.lenFaceVertexBase ||
                desc.textureCount || desc.lenTextureDir || desc.lenTextureData ||
                desc.batchCount || desc.lenBatches || desc.lenFaceRemap)
                fail("core profile archive carries baked-block lengths");
        } else {
            if (desc.vCount != mVertices.size()) fail("vCount " + std::to_string(desc.vCount) + " != baked " + std::to_string(mVertices.size()));
            if (desc.iCount != mIndices.size()) fail("iCount " + std::to_string(desc.iCount) + " != baked " + std::to_string(mIndices.size()));
            if (desc.lenFaceDrawCmds != mFaceDrawCmds.size() * sizeof(FaceDrawCmd)) fail("lenFaceDrawCmds != baked");
            if (desc.lenFaceInfo != mFaceInfos.size() * sizeof(MpakFaceInfo)) fail("lenFaceInfo != baked");
            if (desc.lenExtraLightBlocks != mExtraLightBlocks.size() * sizeof(MpakExtraLightBlock)) fail("lenExtraLightBlocks != baked");
            if (desc.lenFaceVertexBase != (mVertexChunkCount > 1 ? mFaceVertexBase.size() * sizeof(uint32_t) : 0)) fail("lenFaceVertexBase != baked");
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
            uint64_t textureDirAt = 0, textureDataAt = 0;
            if (xbox) {
                textureDirAt = place(desc.lenTextureDir);
                textureDataAt = place(desc.lenTextureData);
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
            const uint64_t faceVertexBaseAt = place(desc.lenFaceVertexBase);

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
            // archive with a flipped vertex byte verified clean. The PC DXT
            // texture arrays/atlas remain structural-only - their bytes are
            // produced inside save() and not retained. The Xbox per-texture
            // directory/data are no longer structural-only either (v10
            // follow-up, below): Mpak_ValidateTextureDirectory checks the
            // directory's real content (chain lengths, 128-byte padding,
            // WITH the padding-content check on, unlike ref/nv2a's own
            // load-time call) against the texture-data lump's real bytes,
            // and each entry's width/height is compared to the retained,
            // already-POT-resampled bake sources below. What neither can see
            // is whether the DXT encoder kept every opaque texel opaque (the
            // punch-through-alpha invariant TextureOpt.cpp's encoder guard can
            // leave unfixed on a block): tools/verify-atlas-alpha.py checks
            // that, per texture and per mip level, against an
            // R3D_DUMP_TEXTURE_RGBA dump, because this in-memory verify has no
            // access to the pre-compression source once save() returns.
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
                checkBaked("face vertex bases", faceVertexBaseAt, mFaceVertexBase.data(), desc.lenFaceVertexBase);
                // v9: retained (unlike the per-texture DXT directory/data,
                // which - like the old atlas - are produced inside save()
                // and not kept around) so these get the same full byte
                // comparison as face draw cmds/face info above.
                if (xbox) checkBaked("texture batches", batchesAt, mTextureBatches.data(), static_cast<uint64_t>(mTextureBatches.size()) * sizeof(MpakTextureBatch));
                checkBaked("face remap", faceRemapAt, mFaceRemap.data(), static_cast<uint64_t>(mFaceRemap.size()) * sizeof(uint32_t));

                // v10 follow-up: the texture directory's own CONTENT (chain
                // lengths, 128-byte padding) used to be checked only by
                // ref/nv2a's real map loader - never here, and never by the
                // PC tests. Mpak_ValidateTextureDirectory
                // (filesystem/mpak_texture_dir.h) is the exact same rule
                // ref/nv2a now calls, but with the padding-content check ON
                // (ref/nv2a passes data = NULL there on purpose - see that
                // function's own doc comment for why that is safe and does
                // not need mirroring here).
                if (xbox) {
                    if (textureDirAt + desc.lenTextureDir > static_cast<uint64_t>(fileSize)) {
                        fail("texture directory: section runs past the end of the file");
                    } else if (textureDataAt + desc.lenTextureData > static_cast<uint64_t>(fileSize)) {
                        fail("texture data: section runs past the end of the file");
                    } else {
                        uint32_t bad = 0;
                        mpak_texdir_status_t status = Mpak_ValidateTextureDirectory(
                            data.data() + textureDirAt, desc.lenTextureDir, desc.textureCount,
                            data.data() + textureDataAt, desc.lenTextureData, &bad);
                        if (status != MPAK_TEXDIR_OK) {
                            fail("texture directory: " + std::string(Mpak_TextureDirStatusString(status)) +
                                 " at entry " + std::to_string(bad));
                        }
                    }

                    // Compare each entry's own width/height against the
                    // retained, already-POT-resampled bake sources
                    // (mXboxTextureWidths/mXboxTextureHeights, PakManager.h)
                    // - the same "compare to the bake" idea checkBaked
                    // already applies to every retained section above.
                    // Skipped (not failed) when the counts already
                    // disagree - the textureCount check earlier in this
                    // function already reports that mismatch.
                    if (desc.textureCount == mXboxTextures.size()) {
                        for (uint32_t j = 0; j < desc.textureCount; ++j) {
                            const uint64_t base = textureDirAt + static_cast<uint64_t>(j) * sizeof(MpakTextureInfo);
                            if (base + sizeof(MpakTextureInfo) > static_cast<uint64_t>(fileSize))
                                break; // already reported by the bounds check above
                            MpakTextureInfo info{};
                            std::memcpy(&info, data.data() + base, sizeof(info));
                            if (info.width != mXboxTextureWidths[j] || info.height != mXboxTextureHeights[j])
                                fail("texture directory entry " + std::to_string(j) + ": " +
                                     std::to_string(info.width) + "x" + std::to_string(info.height) +
                                     " != baked " + std::to_string(mXboxTextureWidths[j]) + "x" + std::to_string(mXboxTextureHeights[j]));
                        }
                    }
                }
            }
        }

        return ok;
    }

} // namespace formats
