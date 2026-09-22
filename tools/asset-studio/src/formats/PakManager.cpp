#include "PakManager.h"
#include "BspParser.h" // BspLumpType - the dheader_t lump indices
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
            // asset-pipeline-specialist review, real finding, confirmed by
            // decoding a real bake byte-for-byte: DXT1/BC1 compresses this
            // whole atlas in one fixed 4x4-pixel grid (TextureOpt.cpp's own
            // compressToDXT), but this shelf packer never aligned a
            // texture's own placement to that grid - GoldSrc miptex widths
            // are always multiples of 16, so `width+2` (the old paddedW)
            // is always ≡2 (mod 4), and packing them back-to-back walks
            // mAtlasCurrentX through every residue EXCEPT a clean multiple
            // of 4. A block that isn't 4-aligned can span this texture's
            // own real edge pixels AND either a neighbouring texture's
            // pixels or the atlas's own uninitialised (0,0,0,0) void -
            // harmless colour bleed before punch-through alpha existed
            // (rgbcx's block-wide least-squares fit just blended two
            // similar opaque colours), but a real, measured defect once a
            // block's own aggregate fit can pick BC1's transparent
            // selector for a texel whose own true alpha was 255 (10 such
            // texels confirmed in one real retail bake, two of them in a
            // texture with no masking involved at all - the void beyond
            // its own shelf row, not another texture, but the same root
            // cause). Fixed by reserving each texture its own whole
            // number of 4x4 blocks (`blockW`x`blockH`, rounded up from the
            // existing 1px-bordered footprint) and filling the ENTIRE
            // reserved footprint - not just the original 1px border - by
            // repeating each row/column's own real edge pixel outward.
            // mAtlasCurrentX/Y stay exact multiples of 4 by construction
            // (both start at 0 and only ever advance by a `block*`
            // value), so every block inside one texture's own reserved
            // footprint now contains only that texture's own real content
            // or its own repeated edge - never a neighbour's, and never
            // unrelated void.
            uint32_t paddedW = width + 2;
            uint32_t paddedH = height + 2;
            uint32_t blockW = ((paddedW + 3) / 4) * 4;
            uint32_t blockH = ((paddedH + 3) / 4) * 4;

            if (mAtlasCurrentX + blockW > mAtlasWidth) {
                mAtlasCurrentX = 0;
                mAtlasCurrentY += mAtlasMaxRowHeight;
                mAtlasMaxRowHeight = 0;
            }

            if (mAtlasCurrentY + blockH > mAtlasHeight) {
                std::cerr << "ERROR: Atlas overflow! Texture won't fit - failing this bake (save() will refuse to write).\n";
                mAtlasOverflowed = true;
                return loc;
            }

            uint32_t destX = mAtlasCurrentX + 1;
            uint32_t destY = mAtlasCurrentY + 1;
            // How far the reserved footprint extends past the real 1px
            // border on the right/bottom - 0..3 pixels, whatever rounding
            // up to a block boundary added.
            uint32_t extraRight = (mAtlasCurrentX + blockW) - (destX + width + 1);
            uint32_t extraBottom = (mAtlasCurrentY + blockH) - (destY + height + 1);

            for (uint32_t y = 0; y < height; ++y) {
                size_t srcOffset = y * width * 4;
                size_t destOffset = ((destY + y) * mAtlasWidth + destX) * 4;
                std::memcpy(&mAtlasRgba[destOffset], &rgbaData[srcOffset], width * 4);

                // Real 1px border (left/right), matching the original
                // logic exactly, then repeat the SAME right-edge pixel
                // across any extra block-alignment columns so every pixel
                // out to the reserved footprint's own right edge is this
                // texture's own real edge colour/alpha, never void. The
                // left side needs no such extension - destX-1 already
                // sits exactly on this texture's own reserved footprint's
                // left edge, since mAtlasCurrentX is always block-aligned.
                const uint8_t* rightPx = &rgbaData[srcOffset + (width - 1) * 4];
                std::memcpy(&mAtlasRgba[((destY + y) * mAtlasWidth + destX - 1) * 4], &rgbaData[srcOffset], 4);
                for (uint32_t bx = 0; bx <= extraRight; ++bx) {
                    std::memcpy(&mAtlasRgba[((destY + y) * mAtlasWidth + destX + width + bx) * 4], rightPx, 4);
                }
            }

            // Top/bottom border rows across the full reserved footprint
            // width (destX-1 .. destX+width+extraRight - includes the
            // right-edge extension already written above), by repeating
            // the nearest real content row's own already-bordered pixels.
            // Top is a single real border row (matches the original logic
            // exactly - the top has no alignment gap of its own, since
            // destY sits exactly on this texture's reserved footprint's
            // top edge for the same reason destX does). Bottom repeats
            // for both the real 1px border row (by=0) and any extra
            // alignment rows beyond it (by=1..extraBottom).
            uint32_t footprintW = 1 + width + 1 + extraRight; // left border + content + right border(+extra)
            std::memcpy(&mAtlasRgba[(mAtlasCurrentY * mAtlasWidth + destX - 1) * 4],
                        &mAtlasRgba[(destY * mAtlasWidth + destX - 1) * 4], footprintW * 4);
            for (uint32_t by = 0; by <= extraBottom; ++by) {
                std::memcpy(&mAtlasRgba[((destY + height + by) * mAtlasWidth + destX - 1) * 4],
                            &mAtlasRgba[((destY + height - 1) * mAtlasWidth + destX - 1) * 4], footprintW * 4);
            }

            loc.array_index = 0;
            loc.layer = 0;
            loc.rect[0] = static_cast<float>(destX) / static_cast<float>(mAtlasWidth);
            loc.rect[1] = static_cast<float>(destY) / static_cast<float>(mAtlasHeight);
            loc.rect[2] = static_cast<float>(width) / static_cast<float>(mAtlasWidth);
            loc.rect[3] = static_cast<float>(height) / static_cast<float>(mAtlasHeight);

            mAtlasCurrentX += blockW;
            if (blockH > mAtlasMaxRowHeight) mAtlasMaxRowHeight = blockH;

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
        if (mAtlasOverflowed) {
            std::cerr << "Failed to save MPAK archive: the Xbox atlas overflowed during baking "
                         "(see the earlier \"Atlas overflow\" error) - refusing to write a file with "
                         "faces silently aliased onto atlas slot 0.\n";
            return false;
        }

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
        }
        header.lenTexinfo = static_cast<uint32_t>(mTexinfoData.size());

        header.lenVertexes = static_cast<uint32_t>(mVertexesData.size());
        header.lenEdges = static_cast<uint32_t>(mEdgesData.size());
        header.lenSurfedges = static_cast<uint32_t>(mSurfedgesData.size());
        header.lenFaces = static_cast<uint32_t>(mFacesData.size());
        header.lenLighting = static_cast<uint32_t>(mLightingData.size());
        header.lenMiptex = static_cast<uint32_t>(mMiptexData.size());

        std::vector<uint8_t> dxtAtlas;
        if (nv2a && mIsXboxTarget) {
            header.atlasWidth = mAtlasWidth;
            header.atlasHeight = mAtlasHeight;
            // Xbox atlas path: an earlier version of this call passed
            // punchThroughAlpha=false, reasoning that per-texture masked
            // tracking "doesn't exist here" since many textures share one
            // shelf-packed atlas with no per-region alpha mode - true, but not
            // actually needed. compressToDXT's own punch-through decision is
            // PER-PIXEL (a<128 within each 4x4 block, TextureOpt.cpp:47-65),
            // not per-region, and the alpha channel already IS correct on a
            // per-pixel basis across the whole atlas before it ever reaches
            // this call: WadParser::decodeMiptex (WadParser.cpp:98) sets
            // alpha=255 for every texel except a masked ('{'-prefixed)
            // texture's own transparent-key palette index (255), which gets
            // alpha=0 - for EVERY texture the atlas packs, not just masked
            // ones - and BspParser's own missing-texture placeholder
            // (BspParser.cpp:566) is alpha=255 throughout too. So there is
            // nothing this call needs to know about WHICH source texture a
            // given block came from; the alpha already carried into
            // mAtlasRgba by addTexture()'s own full 4-byte-per-pixel copy
            // (this file, ~line 44) tells compressToDXT everything it needs,
            // block by block. Turning this on is what actually makes masked
            // ('{'-prefixed) Xbox textures render as masked at all - X9's own
            // native-alpha-test slice (fork-plan.md's X9 section) was blocked
            // on exactly this being a no-op before now.
            if (const char* dbg = std::getenv("R3D_DUMP_ATLAS_RGBA")) {
                std::ofstream dbgf(dbg, std::ios::binary);
                dbgf.write(reinterpret_cast<const char*>(mAtlasRgba.data()), mAtlasRgba.size());
            }
            dxtAtlas = TextureOpt::compressToDXT(mAtlasRgba.data(), mAtlasWidth, mAtlasHeight, true);
            header.lenAtlas = static_cast<uint32_t>(dxtAtlas.size());
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

        // Write Textures (DXT compressed, nv2a profile only)
        if (nv2a) {
            if (mIsXboxTarget) {
                if (header.lenAtlas > 0) writePadded(dxtAtlas.data(), dxtAtlas.size());
            } else {
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
                desc.lenAtlas || desc.lenArray64 || desc.lenArray256 || desc.lenArray512 ||
                desc.lightmapWidth || desc.lightmapHeight || desc.lightmapLayers || desc.lenExtraLightBlocks)
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
            if (xbox && (desc.lenArray64 || desc.lenArray256 || desc.lenArray512)) fail("xbox target carries PC texture arrays");
            if (!xbox && desc.lenAtlas) fail("pc target carries an xbox atlas");
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
            if (xbox) {
                place(desc.lenAtlas);
            } else {
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
            }
        }

        return ok;
    }

} // namespace formats
