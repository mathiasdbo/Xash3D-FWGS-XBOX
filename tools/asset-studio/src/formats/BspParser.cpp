#include "BspParser.h"
#include "WadParser.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <numeric>

namespace formats {

#pragma pack(push, 1)
    struct dmiptexlump_t {
        int32_t nummiptex;
        int32_t dataofs[1];
    };
#pragma pack(pop)

    // GoldSrc BSP30 texinfo flag: sky or liquid, no lightmap. See xash3d-fwgs
    // common/bspfile.h TEX_SPECIAL.
    constexpr uint32_t TEX_SPECIAL = 1u;

    namespace {
        constexpr uint32_t LM_PAGE_SIZE = 1024;   // luxels per atlas page (matches BLOCK_SIZE_MAX)
        constexpr uint32_t LM_SAMPLE_SIZE = 16;   // GoldSrc's fixed lightmap luxel granularity
        constexpr uint32_t LM_MAX_LUXELS = 64;    // reject pathological face extents
        // Faces with no real lightmap (sky/liquid/fullbright/anomalous) sample the
        // white end of the ramp strip below instead of getting their own allocation.
        // Width covers the 256-value gradient plus a small white guard band; height
        // is a few rows so a sampled point can be centered away from row edges.
        constexpr uint32_t LM_RAMP_GRADIENT_WIDTH = 256; // one luxel per 0..255 grayscale value
        constexpr uint32_t LM_RAMP_WHITE_WIDTH = 4;       // pure-white guard band after the gradient
        constexpr uint32_t LM_RESERVED_MARGIN = LM_RAMP_GRADIENT_WIDTH + LM_RAMP_WHITE_WIDTH; // 260
        constexpr uint32_t LM_RAMP_HEIGHT = 4;

        struct LightmapPage {
            std::vector<uint8_t> rgba;
            uint32_t cursorX = 0;
            uint32_t cursorY = 0;
            uint32_t rowHeight = 0;
            uint32_t usedHeight = 0; // highest Y written to, for atlas height trimming

            LightmapPage() : rgba(static_cast<size_t>(LM_PAGE_SIZE) * LM_PAGE_SIZE * 4, 255) {}
        };

        struct LightmapAllocation {
            uint32_t page = 0;
            uint32_t blockX = 0; // origin of the (smax+2) x (tmax+2) BORDERED block
            uint32_t blockY = 0;
        };

        // Shelf-packs bordered lightmap blocks into 1024x1024-luxel RGBA8 pages,
        // mirroring the row-cursor packing already used for Xbox texture atlases in
        // PakManager::addTexture. Each block gets a 1-luxel replicated border so
        // bilinear sampling never bleeds across faces (see B4's sampler notes).
        class LightmapAtlasPacker {
        public:
            LightmapAtlasPacker() {
                mPages.emplace_back();

                // Bake a luminance ramp strip into page 0's top-left corner: a
                // 0..255 grayscale gradient (one texel per value) followed by a
                // white guard band. This does double duty:
                //  - It replaces the old flat white reserved corner as the
                //    no-lightmap-data fallback sample point (in the white band).
                //  - Per-vertex studio-model lighting (later phases) can encode an
                //    illumination value by sampling into the gradient instead of
                //    needing a shader change - the existing x2 overbright multiply
                //    stays correct because the gradient's own values already carry
                //    the intended brightness (e.g. a flat-shaded model samples
                //    x=96 for its ~192/510 GoldSrc ambient-only illumination).
                // The page starts fully white (LightmapPage's ctor), so only the
                // gradient portion (not the white band after it) needs writing.
                for (uint32_t y = 0; y < LM_RAMP_HEIGHT; ++y) {
                    for (uint32_t x = 0; x < LM_RAMP_GRADIENT_WIDTH; ++x) {
                        uint8_t value = static_cast<uint8_t>(x);
                        size_t off = (static_cast<size_t>(y) * LM_PAGE_SIZE + x) * 4;
                        mPages[0].rgba[off + 0] = value;
                        mPages[0].rgba[off + 1] = value;
                        mPages[0].rgba[off + 2] = value;
                        mPages[0].rgba[off + 3] = 255;
                    }
                }

                // Reserve the ramp's full footprint (gradient + white band) so the
                // shelf packer never allocates a real lightmap block over it.
                mPages[0].cursorX = LM_RESERVED_MARGIN;
                mPages[0].rowHeight = LM_RAMP_HEIGHT;
                mPages[0].usedHeight = LM_RAMP_HEIGHT;
            }

            LightmapAllocation allocate(uint32_t smax, uint32_t tmax) {
                uint32_t paddedW = smax + 2;
                uint32_t paddedH = tmax + 2;

                if (mPages.back().cursorX + paddedW > LM_PAGE_SIZE) {
                    mPages.back().cursorY += mPages.back().rowHeight;
                    mPages.back().cursorX = 0;
                    mPages.back().rowHeight = 0;
                }
                if (mPages.back().cursorY + paddedH > LM_PAGE_SIZE) {
                    mPages.emplace_back();
                }

                LightmapPage& page = mPages.back();
                LightmapAllocation alloc{static_cast<uint32_t>(mPages.size() - 1), page.cursorX, page.cursorY};

                page.cursorX += paddedW;
                if (paddedH > page.rowHeight) page.rowHeight = paddedH;
                uint32_t bottom = page.cursorY + page.rowHeight;
                if (bottom > page.usedHeight) page.usedHeight = bottom;

                return alloc;
            }

            // Writes an smax x tmax RGB24 block into the page at alloc's bordered
            // origin, then replicates a 1-luxel border around it.
            void writeBlock(const LightmapAllocation& alloc, uint32_t smax, uint32_t tmax, const uint8_t* rgb24) {
                LightmapPage& page = mPages[alloc.page];
                uint32_t destX = alloc.blockX + 1;
                uint32_t destY = alloc.blockY + 1;

                for (uint32_t y = 0; y < tmax; ++y) {
                    for (uint32_t x = 0; x < smax; ++x) {
                        size_t destOffset = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + x)) * 4;
                        const uint8_t* src = &rgb24[(y * smax + x) * 3];
                        page.rgba[destOffset + 0] = src[0];
                        page.rgba[destOffset + 1] = src[1];
                        page.rgba[destOffset + 2] = src[2];
                        page.rgba[destOffset + 3] = 255;
                    }
                }

                // Replicate left/right edges, then top/bottom (incl. corners).
                for (uint32_t y = 0; y < tmax; ++y) {
                    size_t rowLeft = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX - 1)) * 4;
                    size_t rowLeftSrc = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + destX) * 4;
                    std::memcpy(&page.rgba[rowLeft], &page.rgba[rowLeftSrc], 4);

                    size_t rowRight = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + smax)) * 4;
                    size_t rowRightSrc = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + smax - 1)) * 4;
                    std::memcpy(&page.rgba[rowRight], &page.rgba[rowRightSrc], 4);
                }
                for (uint32_t x = 0; x < smax + 2; ++x) {
                    size_t top = (static_cast<size_t>(alloc.blockY) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                    size_t topSrc = (static_cast<size_t>(destY) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                    std::memcpy(&page.rgba[top], &page.rgba[topSrc], 4);

                    size_t bottom = (static_cast<size_t>(destY + tmax) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                    size_t bottomSrc = (static_cast<size_t>(destY + tmax - 1) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                    std::memcpy(&page.rgba[bottom], &page.rgba[bottomSrc], 4);
                }
            }

            uint32_t pageCount() const { return static_cast<uint32_t>(mPages.size()); }

            // Flattens all pages into one layer-major RGBA8 buffer.
            //
            // outHeight is ALWAYS the full LM_PAGE_SIZE, never trimmed to a
            // page's actually-used extent. Every per-vertex `lv` this class's
            // caller bakes is `(... ) / LM_PAGE_SIZE` - a fixed divisor,
            // computed per-face before any page's final used height is even
            // known (allocate() only grows page.usedHeight, doesn't finalize
            // it until baking is done). If this function ever hands back a
            // shorter height, the caller uploads a texture of that shorter
            // height, and the GPU's normalized-UV sampling rescales every
            // `lv` by (thatHeight / LM_PAGE_SIZE) - a uniform per-face
            // vertical compression that lands most faces on a WRONG row
            // (typically a different face's data entirely, or the reserved
            // margin), independent of anything about that specific face.
            // This isn't a hypothetical: an earlier version of this function
            // trimmed to `usedHeight` to save upload bandwidth, and it
            // silently scrambled the lightmap on every map whose used height
            // wasn't exactly 1024 (i.e. every real map) - confirmed by
            // comparing against retail engine screenshots of the same room.
            // Fixing the mismatch correctly (rescaling `lv` post-hoc, or a
            // two-pass bake that defers `lv` until the true final height is
            // known) is real future memory-bandwidth work, not a quick
            // patch - tracked as a P14+ optimization, not attempted here.
            std::vector<uint8_t> flatten(uint32_t& outHeight) const {
                outHeight = LM_PAGE_SIZE;

                std::vector<uint8_t> out;
                out.reserve(static_cast<size_t>(mPages.size()) * outHeight * LM_PAGE_SIZE * 4);
                for (const auto& p : mPages) {
                    out.insert(out.end(), p.rgba.begin(), p.rgba.end());
                }
                return out;
            }

        private:
            std::vector<LightmapPage> mPages;
        };

        // Mirrors the real per-face classification, Mod_LoadSurfaces
        // (mod_bmodel.c:3309-3378, canonical Xash3D-FWGS clone) - name-prefix
        // checks against the face's OWN texture name, not texinfo bits, for
        // every flag except SURF_DRAWTILED. The reference sets these via
        // independent (not else-if) conditions, but every real texture name
        // matches at most one of these prefixes ("{scroll"/"{"/"scroll"/
        // "sky"/"*"/"!" don't overlap except where "{scroll" also implies
        // "{"), so collapsing them into one if/else-if chain here - testing
        // the more specific "{scroll" before the bare "{"/"scroll" it's a
        // prefix of - produces byte-identical results without duplicating
        // the reference's control-flow shape.
        //
        // Two real branches are deliberately not ported: the non-Quake-compat
        // "water"/"laser" name fallback and the texinfo TEX_SCROLL bit
        // (mod_bmodel.c:3363-3364) are both Xash-only extensions - no
        // original GoldSrc compiler emits TEX_SCROLL, and retail HL1 content
        // never needs the name fallback since it always uses "scroll"-prefixed
        // names. A simplification, not an oversight: if this project ever
        // bakes non-retail/Xash-authored maps, revisit both.
        uint32_t classifySurfaceFlags(const std::string& nameLower, bool planeback, bool texinfoSpecial) {
            uint32_t flags = 0;
            if (planeback) flags |= formats::SURF_PLANEBACK;
            if (texinfoSpecial) flags |= formats::SURF_DRAWTILED;

            if (nameLower.rfind("{scroll", 0) == 0) {
                flags |= formats::SURF_CONVEYOR | formats::SURF_TRANSPARENT;
            } else if (!nameLower.empty() && nameLower[0] == '{') {
                flags |= formats::SURF_TRANSPARENT;
            } else if (nameLower.rfind("scroll", 0) == 0) {
                flags |= formats::SURF_CONVEYOR;
            } else if (nameLower.rfind("sky", 0) == 0) {
                flags |= formats::SURF_DRAWSKY;
            } else if (!nameLower.empty() && (nameLower[0] == '*' || nameLower[0] == '!')) {
                flags |= formats::SURF_DRAWTURB;
            }
            return flags;
        }

        // The size of one embedded miptex entry the way the engine measures
        // it: Mod_CalculateMipTexSize (mod_bmodel.c:664) - header, the four
        // mips (w*h*85/64), and 770 more bytes (uint16 count + 256*3 palette)
        // when Mod_CalcMipTexUsesCustomPalette (:673) finds room for them
        // before the next entry's dataofs (or the lump's end).
        size_t miptexPalettelessSize(const miptex_t& mt) {
            return sizeof(miptex_t) + ((static_cast<size_t>(mt.width) * mt.height * 85) >> 6);
        }

    } // namespace

    // Rebuilds LUMP_TEXTURES as the embedded-miptex lump compilers emit for
    // -nowadtextures, so the engine's unmodified Mod_LoadTextures path
    // (Mod_GetMipTexForTexture :605, Mod_LoadTextureData's "internal texture"
    // branch :2860-2868) loads names, dimensions and 8-bit pixels with no WAD
    // on the console at all. Per miptex, in the source's own index order
    // (texinfo.miptex indexes it), in the REFERENCE's own precedence -
    // r_wadtextures defaults to 0 (model.c:32) and Mod_LoadTextureData only
    // tries a WAD when that is set or the miptex has no pixels of its own
    // (:2834-2835), found in review: the first version took the WAD first,
    // which would have baked different pixels than the engine's default for
    // a map carrying customised embedded art under a WAD texture's name:
    //  - embedded pixels, complete: the embedded bytes, sized the engine's
    //    way (Mod_CalculateMipTexSize :664 plus the palette iff
    //    Mod_CalcMipTexUsesCustomPalette :673 would find room for it);
    //  - otherwise, a same-named WAD lump: its raw bytes, verbatim (a WAD3
    //    lump is exactly miptex_t + mips + count + palette);
    //  - otherwise the 40-byte header alone, offsets zero - the engine keeps
    //    the NAME and dimensions (Mod_LoadTexture :2953-2956) and falls back
    //    to its default image (:2871-2874), exactly what a real .bsp with a
    //    missing WAD gets. Names decide sky/water/conveyor/transparency flags
    //    (:3354-3371), bevel contents (:1451) and animation linkage
    //    (:3042-3055), so they are never substituted; an embedded payload the
    //    lump is too short to hold lands here too, rather than as a short
    //    copy the engine would then read past;
    //  - no header at all (dataofs out of range): dataofs -1, the engine
    //    creates its default texture (Mod_CreateDefaultTexture :722).
    // Every offset is bounded by the input buffer (found in review: a later
    // directory entry pointing past the lump used to become the copy limit).
    // Unit-tested in tests/miptex_lump_test.cpp.
    std::vector<uint8_t> buildEmbeddedMiptexLump(const std::vector<uint8_t>& texData,
                                                 const std::unordered_map<std::string, const WadLump*>& externalWadTextures) {
        std::vector<uint8_t> out;
        if (texData.size() < sizeof(int32_t)) {
            // No texture lump at all: an empty dmiptexlump_t (nummiptex 0)
            // keeps Mod_LoadTextures on its "no textures" path (:3135).
            out.resize(sizeof(int32_t), 0);
            return out;
        }

        int32_t nummiptex = 0;
        std::memcpy(&nummiptex, texData.data(), sizeof(int32_t));
        const size_t dirBytes = sizeof(int32_t) + static_cast<size_t>(std::max(nummiptex, 0)) * sizeof(int32_t);
        if (nummiptex <= 0 || dirBytes > texData.size()) {
            out.resize(sizeof(int32_t), 0);
            return out;
        }

        auto srcOfsAt = [&](int32_t i) {
            int32_t v = -1;
            std::memcpy(&v, texData.data() + sizeof(int32_t) + static_cast<size_t>(i) * sizeof(int32_t), sizeof(int32_t));
            return v;
        };
        auto headerFits = [&](int32_t ofs) {
            return ofs >= 0 && static_cast<size_t>(ofs) + sizeof(miptex_t) <= texData.size();
        };

        out.resize(dirBytes, 0);
        std::memcpy(out.data(), &nummiptex, sizeof(int32_t));

        for (int32_t i = 0; i < nummiptex; ++i) {
            const int32_t offset = srcOfsAt(i);
            if (!headerFits(offset)) {
                int32_t none = -1;
                std::memcpy(out.data() + sizeof(int32_t) + static_cast<size_t>(i) * sizeof(int32_t), &none, sizeof(int32_t));
                continue;
            }

            const miptex_t* src = reinterpret_cast<const miptex_t*>(texData.data() + offset);
            char cleanName[17] = {0};
            std::memcpy(cleanName, src->name, 16);
            std::string nameLower(cleanName);
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c){ return std::tolower(c); });

            const uint8_t* entry = texData.data() + offset;
            size_t entrySize = sizeof(miptex_t);
            bool headerOnly = true;

            if (src->offsets[0] >= sizeof(miptex_t)) {
                // Embedded pixels. This entry's bytes end at the next VALID
                // directory offset after it, or at the lump's end - never past
                // the buffer, whatever the directory claims.
                size_t limit = texData.size();
                for (int32_t j = i + 1; j < nummiptex; ++j) {
                    const int32_t ofsJ = srcOfsAt(j);
                    if (ofsJ > offset && static_cast<size_t>(ofsJ) < limit)
                        limit = static_cast<size_t>(ofsJ);
                }
                const size_t available = limit - static_cast<size_t>(offset);
                const size_t need = miptexPalettelessSize(*src);
                if (need <= available && static_cast<size_t>(src->offsets[0]) + static_cast<size_t>(src->width) * src->height <= available) {
                    entrySize = need + ((available - need >= 770) ? 770 : 0);
                    headerOnly = false;
                }
                // else: truncated - fall through to the WAD, then to header-only
            }

            if (headerOnly) {
                auto it = externalWadTextures.find(nameLower);
                if (it != externalWadTextures.end() && it->second->data.size() >= sizeof(miptex_t)) {
                    entry = it->second->data.data();
                    entrySize = it->second->data.size();
                    headerOnly = false;
                }
            }

            // 4-byte-align each entry; the engine reads mip_t via memcpy and
            // pixels as bytes, so this is only tidiness, and any extra bytes
            // before the next dataofs only ever make the palette-room test
            // more permissive, never less.
            while (out.size() % 4 != 0) out.push_back(0);
            const int32_t entryOfs = static_cast<int32_t>(out.size());
            out.insert(out.end(), entry, entry + entrySize);
            std::memcpy(out.data() + sizeof(int32_t) + static_cast<size_t>(i) * sizeof(int32_t), &entryOfs, sizeof(int32_t));

            if (headerOnly) {
                // Whatever the source header's offsets said, this entry carries
                // no pixels: zero them so Mod_LoadTextureData's
                // `mipTex.offsets[0] > 0` internal-texture branch is not taken.
                miptex_t* written = reinterpret_cast<miptex_t*>(out.data() + entryOfs);
                std::memset(written->offsets, 0, sizeof(written->offsets));
            }
        }

        return out;
    }

    void indexWadLumpsForCore(std::unordered_map<std::string, const WadLump*>& index,
                              const std::vector<WadLump>& lumps) {
        for (const auto& lump : lumps) {
            char cleanName[17] = {0};
            std::memcpy(cleanName, lump.name, 16);
            std::string nameLower(cleanName);
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c){ return std::tolower(c); });
            index[nameLower] = &lump;
        }
    }

    bool BspParser::load(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open BSP file: " << filePath << "\n";
            return false;
        }

        file.read(reinterpret_cast<char*>(&mHeader.version), sizeof(uint32_t));
        if (mHeader.version != 30) {
            std::cerr << "Unsupported BSP version: " << mHeader.version << ". Only BSP30 is currently supported.\n";
            return false;
        }

        for (int i = 0; i < BSP_LUMPS; ++i) {
            file.read(reinterpret_cast<char*>(&mHeader.lumps[i].offset), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&mHeader.lumps[i].length), sizeof(uint32_t));
        }

        mLumpsData.resize(BSP_LUMPS);
        for (int i = 0; i < BSP_LUMPS; ++i) {
            file.seekg(mHeader.lumps[i].offset, std::ios::beg);
            mLumpsData[i].resize(mHeader.lumps[i].length);
            file.read(reinterpret_cast<char*>(mLumpsData[i].data()), mHeader.lumps[i].length);
        }

        return true;
    }

    const std::vector<uint8_t>& BspParser::getLumpData(BspLumpType type) const {
        return mLumpsData[static_cast<int>(type)];
    }

    void BspParser::exportToPak(PakManager& pak, const VfsIndexer& vfs) const {
        const auto& entityData = getLumpData(LUMP_ENTITIES);
        std::vector<std::string> priorityWadNames;

        if (!entityData.empty()) {
            std::string_view entityStr(reinterpret_cast<const char*>(entityData.data()), entityData.size());
            size_t wadKeyPos = entityStr.find("\"wad\"");
            if (wadKeyPos != std::string_view::npos) {
                size_t valStart = entityStr.find('"', wadKeyPos + 5);
                if (valStart != std::string_view::npos) {
                    size_t valEnd = entityStr.find('"', valStart + 1);
                    if (valEnd != std::string_view::npos) {
                        std::string_view wadVal = entityStr.substr(valStart + 1, valEnd - valStart - 1);
                        size_t pos = 0;
                        while ((pos = wadVal.find(';')) != std::string_view::npos) {
                            std::string_view item = wadVal.substr(0, pos);
                            wadVal.remove_prefix(pos + 1);
                            
                            while (!item.empty() && std::isspace(item.front())) item.remove_prefix(1);
                            while (!wadVal.empty() && std::isspace(wadVal.front())) wadVal.remove_prefix(1);
                            
                            if (!item.empty()) {
                                std::filesystem::path p(item);
                                std::string fname = p.filename().string();
                                if (!fname.empty()) priorityWadNames.push_back(fname);
                            }
                        }
                        if (!wadVal.empty()) {
                            std::filesystem::path p(wadVal);
                            std::string fname = p.filename().string();
                            if (!fname.empty()) priorityWadNames.push_back(fname);
                        }
                    }
                }
            }
        }

        std::cout << "Worldspawn WAD Metadata parsed (" << priorityWadNames.size() << " requested WADs):\n";
        for (const auto& w : priorityWadNames) {
            std::cout << " - " << w << "\n";
        }

        std::unordered_map<std::string, const WadLump*> externalWadTextures;
        std::vector<WadParser> loadedWads;
        std::unordered_map<std::string, bool> loadedWadPaths;

        for (const auto& wadName : priorityWadNames) {
            std::string fullPath = vfs.findFileByName(wadName);
            if (!fullPath.empty() && loadedWadPaths.find(fullPath) == loadedWadPaths.end()) {
                WadParser wad;
                if (wad.load(fullPath)) {
                    std::cout << "VFS Loaded Priority WAD: " << fullPath << "\n";
                    loadedWads.push_back(std::move(wad));
                    loadedWadPaths[fullPath] = true;
                }
            }
        }
        const size_t listedWadsLoaded = loadedWads.size(); // everything after this index is unlisted

        for (const auto& entry : vfs.getEntries()) {
            std::filesystem::path p(entry.fileName);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });

            if (ext == ".wad" && loadedWadPaths.find(entry.fullPath) == loadedWadPaths.end()) {
                WadParser wad;
                if (wad.load(entry.fullPath)) {
                    std::cout << "VFS Loaded Auxiliary WAD: " << entry.relativePath << "\n";
                    loadedWads.push_back(std::move(wad));
                    loadedWadPaths[entry.fullPath] = true;
                }
            }
        }

        // loadedWads holds the worldspawn-listed WADs first, in list order,
        // then every other WAD the VFS could see. The nv2a block's DXT path
        // below resolves from ALL of them (its documented, X9-owned choice);
        // the core miptex lump resolves ONLY from the listed ones, later
        // listed overriding earlier, which is the reference's own precedence
        // (Mod_LoadTextureFromWadList walks world.wadlist in reverse,
        // mod_bmodel.c:619-655) - found in review: feeding it every WAD on
        // disk let an unrelated WAD sharing a name bake its pixels into the
        // map permanently.
        std::unordered_map<std::string, const WadLump*> coreWadTextures;
        for (size_t i = 0; i < listedWadsLoaded; ++i)
            indexWadLumpsForCore(coreWadTextures, loadedWads[i].getLumps());

        for (const auto& wad : loadedWads) {
            for (const auto& lump : wad.getLumps()) {
                char cleanName[17] = {0};
                std::memcpy(cleanName, lump.name, 16);
                std::string nameLower(cleanName);
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c){ return std::tolower(c); });
                externalWadTextures[nameLower] = &lump;
            }
        }
        std::cout << "Total unique WAD texture lumps registered: " << externalWadTextures.size()
                  << " (" << coreWadTextures.size() << " from the " << listedWadsLoaded << " worldspawn-listed WAD(s), used for the core miptex lump)\n";

        const auto& faceData = getLumpData(LUMP_FACES);
        const auto& vertexData = getLumpData(LUMP_VERTICES);
        const auto& edgeData = getLumpData(LUMP_EDGES);
        const auto& surfedgeData = getLumpData(LUMP_SURFEDGES);
        const auto& texinfoData = getLumpData(LUMP_TEXINFO);
        const auto& texData = getLumpData(LUMP_TEXTURES);

        const dface_t* faces = reinterpret_cast<const dface_t*>(faceData.data());
        size_t numFaces = faceData.size() / sizeof(dface_t);

        const dvertex_t* vertices = reinterpret_cast<const dvertex_t*>(vertexData.data());
        const dedge_t* edges = reinterpret_cast<const dedge_t*>(edgeData.data());
        const int32_t* surfedges = reinterpret_cast<const int32_t*>(surfedgeData.data());
        const dtexinfo_t* texinfos = reinterpret_cast<const dtexinfo_t*>(texinfoData.data());
        size_t numTexinfos = texinfoData.size() / sizeof(dtexinfo_t);

        // Everything from here to the lightmap atlas is the nv2a block:
        // DXT-resolved textures, per-face baked Vertex/index/FaceDrawCmd/
        // FaceInfo, the lightmap atlas. The core profile stores the original
        // geometry lumps instead and never reads any of this - found in
        // review: running it anyway meant addVertex's 65,536-vertex abort
        // (PakManager.cpp) could refuse a core bake of a map whose expanded
        // per-face vertex count exceeds a 16-bit index buffer the core
        // archive does not even contain. Skipping it is also most of a core
        // bake's time.
        if (pak.isProfileNv2a()) {
        std::vector<TextureLocation> texLocations;
        // A distinct, unmistakable placeholder for any texture that fails to
        // resolve (WAD lookup miss, embedded decode failure, or no data
        // offset at all) - lazily baked at most once. Without this, a failed
        // lookup left `texLocations[i]` at its default-constructed
        // TextureLocation{} (array_index=0, layer=0), silently ALIASING
        // whatever real texture happened to be baked first into atlas slot
        // 0 - every face referencing a failed texture then rendered with
        // that unrelated texture instead, which is indistinguishable from a
        // real (if wrong-looking) texture and gives no signal that
        // anything failed. Bright magenta/black checkerboard, the
        // conventional "missing texture" pattern used by most modern
        // engines (Source, Unity, Unreal) specifically so it's never
        // mistaken for real level art.
        bool haveMissingTexturePlaceholder = false;
        TextureLocation missingTexturePlaceholder{};
        auto missingTextureLocation = [&]() -> TextureLocation {
            if (!haveMissingTexturePlaceholder) {
                constexpr int kSize = 16;
                std::vector<uint8_t> rgba(static_cast<size_t>(kSize) * kSize * 4);
                for (int y = 0; y < kSize; ++y) {
                    for (int x = 0; x < kSize; ++x) {
                        bool magenta = ((x / 4) + (y / 4)) % 2 == 0;
                        uint8_t* px = &rgba[(static_cast<size_t>(y) * kSize + x) * 4];
                        px[0] = magenta ? 255 : 0;
                        px[1] = 0;
                        px[2] = magenta ? 255 : 0;
                        px[3] = 255;
                    }
                }
                missingTexturePlaceholder = pak.addTexture(kSize, kSize, rgba.data());
                haveMissingTexturePlaceholder = true;
            }
            return missingTexturePlaceholder;
        };

        // Lower-cased texture name per miptex index, kept alongside texLocations
        // for the face loop's SURF_* classification below (Mod_LoadSurfaces,
        // mod_bmodel.c:3309-3378, classifies by name prefix - "sky", water-like,
        // "scroll", "{" - not by any bit already on texinfo). Left empty for an
        // unresolved miptex (no embedded header to read a name from); an empty
        // name matches none of the prefix checks, which is the correct "ordinary
        // opaque surface" default for a texture that failed to resolve anyway.
        std::vector<std::string> mipNamesLower;

        if (!texData.empty()) {
            const dmiptexlump_t* mipData = reinterpret_cast<const dmiptexlump_t*>(texData.data());
            texLocations.resize(mipData->nummiptex);
            mipNamesLower.resize(mipData->nummiptex);

            uint32_t matchedWadCount = 0;
            uint32_t matchedEmbeddedCount = 0;
            uint32_t unresolvedCount = 0;

            for (int32_t i = 0; i < mipData->nummiptex; ++i) {
                int32_t offset = mipData->dataofs[i];
                if (offset < 0 || static_cast<size_t>(offset) >= texData.size()) {
                    // No embedded miptex_t header to read a name from at all
                    // (a purely-external WAD reference some compilers emit) -
                    // there's no name here to look a WAD entry up by, so this
                    // can only ever be the missing-texture placeholder, not a
                    // silent slot-0 alias.
                    texLocations[i] = missingTextureLocation();
                    ++unresolvedCount;
                    continue;
                }

                const miptex_t* mipHeader = reinterpret_cast<const miptex_t*>(texData.data() + offset);
                char cleanBspName[17] = {0};
                std::memcpy(cleanBspName, mipHeader->name, 16);
                std::string texNameLower(cleanBspName);
                std::transform(texNameLower.begin(), texNameLower.end(), texNameLower.begin(), [](unsigned char c){ return std::tolower(c); });
                mipNamesLower[i] = texNameLower;

                DecodedTexture tex;
                bool decoded = false;

                // 1. ALWAYS check external WAD files FIRST by texture name!
                auto it = externalWadTextures.find(texNameLower);
                if (it != externalWadTextures.end()) {
                    decoded = WadParser::decodeMiptex(it->second->data.data(), it->second->data.size(), tex);
                    if (decoded) matchedWadCount++;
                }

                // 2. If not found in external WADs, decode embedded BSP miptex payload
                if (!decoded) {
                    decoded = WadParser::decodeMiptex(texData.data() + offset, texData.size() - offset, tex);
                    if (decoded) matchedEmbeddedCount++;
                }

                if (decoded) {
                    // `masked` must match decodeMiptex's own isTransparentTex
                    // check (WadParser.cpp:91 - name starts with '{') so
                    // TextureOpt::compressToDXT's punch-through path actually
                    // preserves the alpha channel decodeMiptex already
                    // computed (WadParser.cpp:98). Without it, DXT1
                    // compression silently drops alpha and the raw palette
                    // RGB at index 255 survives into the baked texture
                    // instead of being cut out. The real engine doesn't
                    // special-case any particular color there either - it
                    // just zeroes whatever sits at index 255 unconditionally
                    // (`Image_SetPalette`, `img_utils.c:306-310`) - artists
                    // conventionally paint that palette slot bright blue so
                    // it's recognizable in an editor, which is exactly the
                    // color that leaks through uncut: every grate, fence,
                    // ladder and vent rendered with a visible blue cutout.
                    bool masked = !texNameLower.empty() && texNameLower[0] == '{';
                    texLocations[i] = pak.addTexture(tex.width, tex.height, tex.rgba.data(), masked);
                } else {
                    std::cerr << "BspParser: texture \"" << cleanBspName << "\" failed to resolve (not in any WAD, embedded decode failed) - using the missing-texture placeholder\n";
                    texLocations[i] = missingTextureLocation();
                    ++unresolvedCount;
                }
            }

            std::cout << "Texture Resolution Summary: " << matchedWadCount << " from WADs, " << matchedEmbeddedCount << " embedded/procedural, " << unresolvedCount << " unresolved (placeholder).\n";
        }

        uint32_t currentIndexCount = 0;

        const auto& lightingData = getLumpData(LUMP_LIGHTING);
        LightmapAtlasPacker lmPacker;
        // Centered in the white guard band that follows the gradient (not the
        // gradient itself), so a face with no real lightmap data always samples
        // pure white regardless of any future change to the gradient's own values.
        const float kFallbackLu = (static_cast<float>(LM_RAMP_GRADIENT_WIDTH) + LM_RAMP_WHITE_WIDTH * 0.5f) / static_cast<float>(LM_PAGE_SIZE);
        const float kFallbackLv = (LM_RAMP_HEIGHT * 0.5f) / static_cast<float>(LM_PAGE_SIZE);
        uint32_t lmBaked = 0, lmSkipped = 0, lmRejected = 0;

        // v9: per-texture Xbox draws sort faces by (textureIndex, PVS leaf)
        // before anything is emitted - see MpakHeader.h's v8 -> v9 note. Pass
        // 1 below computes every face's baked data (lightmap allocation
        // included - allocation order affects only atlas packing layout, not
        // correctness) into a temporary per-face record instead of emitting
        // it immediately; pass 2, after sorting, emits MpakFaceInfo/indices/
        // FaceDrawCmd in the new order and builds the MpakTextureBatch list
        // and the originalToSortedFaceIndex[] remap. Vertices are still
        // added to `pak` directly in pass 1 below (not deferred): the vertex
        // buffer is globally deduplicated and shared across faces, so its
        // own content is independent of face emission order - only the INDEX
        // buffer's order needs to follow the sort, via polyIndices below.
        struct FaceBuildRecord {
            MpakFaceInfo faceInfo{};
            std::vector<uint16_t> polyIndices;
            std::vector<MpakExtraLightBlock> extraBlocks; // faceIndex filled in during pass 2
            uint32_t textureIndex = 0;
            uint32_t pvsCluster = 0xFFFFFFFFu; // no owning leaf found - sorts last within its texture group
        };
        std::vector<FaceBuildRecord> records(numFaces);

        // face -> "owning" leaf, for the sort's secondary key only (spatial
        // locality within one texture's batch, not real PVS - correctness of
        // runtime PVS marking comes entirely from the originalToSortedFaceIndex[]
        // remap built below, which is correct regardless of which leaf a
        // straddling face happens to pick here). A face can appear in
        // several leaves' marksurfaces lists (it straddles a BSP split); the
        // FIRST leaf found while walking leaves in ascending index order
        // wins, deterministically. Same leaf/marksurface relationship
        // ref/nv2a's own nv2a_mark_leaves() walks at runtime (r_context.c) -
        // see MpakHeader.h's v8 -> v9 note.
        {
            const auto& leavesData = getLumpData(LUMP_LEAVES);
            const auto& marksurfacesData = getLumpData(LUMP_MARKSURFACES);
            const dleaf_t* leaves = reinterpret_cast<const dleaf_t*>(leavesData.data());
            size_t numLeaves = leavesData.size() / sizeof(dleaf_t);
            const uint16_t* marksurfaces = reinterpret_cast<const uint16_t*>(marksurfacesData.data());
            size_t numMarksurfaces = marksurfacesData.size() / sizeof(uint16_t);

            for (size_t leafIdx = 0; leafIdx < numLeaves; ++leafIdx) {
                const dleaf_t& leaf = leaves[leafIdx];
                size_t first = leaf.firstmarksurface;
                size_t count = leaf.nummarksurfaces;
                for (size_t k = 0; k < count; ++k) {
                    size_t msIdx = first + k;
                    if (msIdx >= numMarksurfaces) break; // truncated/corrupt lump - stop this leaf's walk
                    uint16_t faceIdx = marksurfaces[msIdx];
                    if (faceIdx < numFaces && records[faceIdx].pvsCluster == 0xFFFFFFFFu)
                        records[faceIdx].pvsCluster = static_cast<uint32_t>(leafIdx);
                }
            }
        }

        for (size_t i = 0; i < numFaces; ++i) {
            FaceBuildRecord& rec = records[i];
            const dface_t& face = faces[i];
            const dtexinfo_t& texinfo = (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < numTexinfos) ? texinfos[face.texinfo] : dtexinfo_t{};

            TextureLocation loc{};
            if (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < numTexinfos) {
                uint32_t mipIdx = texinfo.miptex;
                if (mipIdx < texLocations.size()) {
                    loc = texLocations[mipIdx];
                }
            }
            rec.textureIndex = loc.textureIndex;

            // --- Lightmap extents pre-pass (GoldSrc BSP30 / xash3d-fwgs
            // Mod_CalcSurfaceExtents semantics) ---
            // texturemins/extents are computed from the SAME rawU/rawV dot products
            // the per-vertex loop below uses for diffuse UVs, over float mins/maxs.
            //
            // NOTE: only lightstyle 0 (face.styles[0]) is baked. GoldSrc faces can
            // carry up to 4 styles (styles[1..3]) for flickering/animated lights,
            // each its own smax*tmax*3-byte block immediately following this one in
            // LUMP_LIGHTING - this baker only reads the first and drops the rest, so
            // animated lightstyle faces bake as their base (non-flickering) light
            // level. Reasonable for a static baked atlas; revisit if per-style
            // dynamic relighting is ever wanted.
            bool hasLightmap = face.numedges > 0
                && face.lightofs >= 0
                && face.styles[0] != 255
                && (texinfo.flags & TEX_SPECIAL) == 0;

            float mins[2] = { 999999.0f, 999999.0f };
            float maxs[2] = { -999999.0f, -999999.0f };
            if (hasLightmap) {
                for (int16_t j = 0; j < face.numedges; ++j) {
                    int32_t surfedge = surfedges[face.firstedge + j];
                    uint16_t vertexIndex = (surfedge >= 0) ? edges[surfedge].v[0] : edges[-surfedge].v[1];
                    const dvertex_t& v = vertices[vertexIndex];

                    float s = v.point[0] * texinfo.vecs[0][0] + v.point[1] * texinfo.vecs[0][1] + v.point[2] * texinfo.vecs[0][2] + texinfo.vecs[0][3];
                    float t = v.point[0] * texinfo.vecs[1][0] + v.point[1] * texinfo.vecs[1][1] + v.point[2] * texinfo.vecs[1][2] + texinfo.vecs[1][3];
                    mins[0] = std::min(mins[0], s); maxs[0] = std::max(maxs[0], s);
                    mins[1] = std::min(mins[1], t); maxs[1] = std::max(maxs[1], t);
                }
            }

            int32_t texturemins[2] = {0, 0};
            uint32_t smax = 0, tmax = 0;
            if (hasLightmap) {
                int32_t bmins[2], bmaxs[2];
                for (int k = 0; k < 2; ++k) {
                    bmins[k] = static_cast<int32_t>(std::floor(mins[k] / LM_SAMPLE_SIZE));
                    bmaxs[k] = static_cast<int32_t>(std::ceil(maxs[k] / LM_SAMPLE_SIZE));
                    texturemins[k] = bmins[k] * static_cast<int32_t>(LM_SAMPLE_SIZE);
                }
                smax = static_cast<uint32_t>((bmaxs[0] - bmins[0])) + 1;
                tmax = static_cast<uint32_t>((bmaxs[1] - bmins[1])) + 1;

                if (smax > LM_MAX_LUXELS || tmax > LM_MAX_LUXELS) {
                    std::cerr << "Lightmap: face " << i << " extents " << smax << "x" << tmax
                              << " exceed " << LM_MAX_LUXELS << " luxels - using white fallback\n";
                    hasLightmap = false;
                    lmRejected++;
                } else {
                    size_t required = static_cast<size_t>(face.lightofs) + static_cast<size_t>(smax) * tmax * 3;
                    if (required > lightingData.size()) {
                        std::cerr << "Lightmap: face " << i << " lightofs " << face.lightofs
                                  << " + " << (smax * tmax * 3) << " bytes exceeds LUMP_LIGHTING size "
                                  << lightingData.size() << " - using white fallback\n";
                        hasLightmap = false;
                        lmRejected++;
                    }
                }
            } else {
                lmSkipped++;
            }

            LightmapAllocation lmAlloc; // defaults to {page 0, 0, 0} - the reserved white corner
            if (hasLightmap) {
                lmAlloc = lmPacker.allocate(smax, tmax);
                lmPacker.writeBlock(lmAlloc, smax, tmax, &lightingData[face.lightofs]);
                lmBaked++;

                // MpakHeader.h's v7 -> v8: styles[1..3], each its own
                // smax*tmax*3-byte block immediately following style 0's in
                // LUMP_LIGHTING (same packed-no-gaps convention the
                // hasLightmap/extents computation above already assumes -
                // see this file's own comment there). Same extents as style
                // 0 (all styles of one face share lightmapmins/extents; only
                // the sample VALUES differ), so a fresh allocate() per extra
                // style, bounds-checked the same way style 0's own block was
                // above (a truncated LUMP_LIGHTING silently drops the extra
                // block rather than reading past the end).
                const size_t blockBytes = static_cast<size_t>(smax) * tmax * 3;
                for (int styleSlot = 1; styleSlot < 4; ++styleSlot) {
                    if (face.styles[styleSlot] == 255)
                        break; // packed convention: no gaps, first 255 ends the real styles
                    size_t extraOfs = static_cast<size_t>(face.lightofs) + static_cast<size_t>(styleSlot) * blockBytes;
                    if (extraOfs + blockBytes > lightingData.size()) {
                        std::cerr << "Lightmap: face " << i << " style slot " << styleSlot
                                  << " offset " << extraOfs << " + " << blockBytes
                                  << " bytes exceeds LUMP_LIGHTING size " << lightingData.size()
                                  << " - dropping this extra style block\n";
                        break; // a truncated table can't have later slots either
                    }
                    LightmapAllocation extraAlloc = lmPacker.allocate(smax, tmax);
                    lmPacker.writeBlock(extraAlloc, smax, tmax, &lightingData[extraOfs]);

                    // faceIndex is filled in during pass 2, once this face's
                    // SORTED position is known - MpakExtraLightBlock::faceIndex
                    // must reference this archive's own (sorted) face order,
                    // not the original BSP index `i` (see its own doc comment
                    // in MpakHeader.h).
                    MpakExtraLightBlock extra{};
                    extra.styleSlot = static_cast<uint8_t>(styleSlot);
                    extra.style = face.styles[styleSlot];
                    extra.page = extraAlloc.page;
                    extra.blockX = extraAlloc.blockX;
                    extra.blockY = extraAlloc.blockY;
                    rec.extraBlocks.push_back(extra);
                }
            }

            // Per-face metadata the reference computes at load time and keeps
            // on msurface_t (see MpakHeader.h's MpakFaceInfo doc comment) -
            // this baker computed the equivalent transiently above and
            // discarded it every time until now.
            MpakFaceInfo& faceInfo = rec.faceInfo;
            faceInfo.planenum = face.planenum;
            faceInfo.texinfo = (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < numTexinfos) ? face.texinfo : -1;
            std::memcpy(faceInfo.styles, face.styles, 4);
            faceInfo.flags = classifySurfaceFlags(
                (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < numTexinfos && texinfo.miptex < mipNamesLower.size())
                    ? mipNamesLower[texinfo.miptex] : std::string{},
                face.side != 0,
                (texinfo.flags & TEX_SPECIAL) != 0);
            if (hasLightmap) {
                faceInfo.lightmapmins[0] = texturemins[0];
                faceInfo.lightmapmins[1] = texturemins[1];
                faceInfo.extents[0] = static_cast<int32_t>((smax - 1) * LM_SAMPLE_SIZE);
                faceInfo.extents[1] = static_cast<int32_t>((tmax - 1) * LM_SAMPLE_SIZE);
                faceInfo.lightmapPage = lmAlloc.page;
                faceInfo.lightmapBlockX = lmAlloc.blockX;
                faceInfo.lightmapBlockY = lmAlloc.blockY;
            }

            std::vector<uint16_t>& polyIndices = rec.polyIndices;

            for (int16_t j = 0; j < face.numedges; ++j) {
                int32_t surfedge = surfedges[face.firstedge + j];
                uint16_t vertexIndex;

                if (surfedge >= 0) {
                    vertexIndex = edges[surfedge].v[0];
                } else {
                    vertexIndex = edges[-surfedge].v[1];
                }

                const dvertex_t& v = vertices[vertexIndex];

                float rawU = v.point[0] * texinfo.vecs[0][0] + v.point[1] * texinfo.vecs[0][1] + v.point[2] * texinfo.vecs[0][2] + texinfo.vecs[0][3];
                float rawV = v.point[0] * texinfo.vecs[1][0] + v.point[1] * texinfo.vecs[1][1] + v.point[2] * texinfo.vecs[1][2] + texinfo.vecs[1][3];

                // Value-initialize (not default-initialize): the trailing _pad/_pad2
                // fields participate in VertexHasher's memcmp-based dedup, so leaving
                // them as indeterminate stack garbage would silently defeat dedup.
                Vertex outVert{};
                // Native GoldSrc units end-to-end: LUMP_PLANES/CLIPNODES/MODELS (hull
                // collision, PVS) and entity origins from LUMP_ENTITIES are already
                // unscaled, and every pm_shared constant (speed 320, stepsize 18,
                // jump 45, gravity 800) is defined in these units. Scaling only the
                // render vertices (as this used to do) put geometry and collision in
                // different coordinate spaces.
                outVert.x = v.point[0];
                outVert.y = v.point[1];
                outVert.z = v.point[2];

                outVert.u = rawU / static_cast<float>(loc.width > 0 ? loc.width : 64);
                outVert.v = rawV / static_cast<float>(loc.height > 0 ? loc.height : 64);

                if (hasLightmap) {
                    // (blockX, blockY) is the origin of the BORDERED (smax+2)x(tmax+2)
                    // block, so +1 lands on the first data luxel; the +0.5 centers the
                    // sample in that luxel. Bilinear then reaches at most 0.5 texel
                    // into the replicated border - never across into a neighboring face.
                    float lu = (static_cast<float>(lmAlloc.blockX) + 1.0f + (rawU - texturemins[0]) / static_cast<float>(LM_SAMPLE_SIZE) + 0.5f) / static_cast<float>(LM_PAGE_SIZE);
                    float lv = (static_cast<float>(lmAlloc.blockY) + 1.0f + (rawV - texturemins[1]) / static_cast<float>(LM_SAMPLE_SIZE) + 0.5f) / static_cast<float>(LM_PAGE_SIZE);
                    outVert.lu = lu;
                    outVert.lv = lv;
                    outVert.lightmap_layer = lmAlloc.page;
                } else {
                    outVert.lu = kFallbackLu;
                    outVert.lv = kFallbackLv;
                    outVert.lightmap_layer = 0;
                }

                outVert.texture_layer = loc.layer;
                outVert.array_index = loc.array_index;

                uint16_t idx = pak.addVertex(outVert);
                polyIndices.push_back(idx);
            }
        } // end pass 1

        // Sort ALL faces by (textureIndex, pvsCluster) - stable, so faces
        // sharing both keys (most commonly: faces with no owning leaf found,
        // or every face of a texture used in only one leaf) keep their
        // original relative order rather than an arbitrary one.
        std::vector<uint32_t> order(numFaces);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&records](uint32_t a, uint32_t b) {
            if (records[a].textureIndex != records[b].textureIndex) return records[a].textureIndex < records[b].textureIndex;
            return records[a].pvsCluster < records[b].pvsCluster;
        });

        // originalToSortedFaceIndex[originalFaceIndex] = sortedPosition - the
        // inverse permutation ref/nv2a's nv2a_mark_leaves() needs to
        // translate a classic-lump-order face index into this archive's own
        // sorted order (MpakHeader.h's v8 -> v9 note). Mandatory, not an
        // optimization: without it, PVS visibility marks would land on
        // whatever face happens to now sit at that original index, not the
        // face that originally had it.
        std::vector<uint32_t> faceRemap(numFaces);
        for (uint32_t sortedPos = 0; sortedPos < static_cast<uint32_t>(order.size()); ++sortedPos) {
            faceRemap[order[sortedPos]] = sortedPos;
        }

        // Pass 2: emit MpakFaceInfo/indices/FaceDrawCmd/extra-light-blocks in
        // the SORTED order (vertices were already emitted, order-independently,
        // in pass 1 above), and build the contiguous per-texture
        // MpakTextureBatch list as we go - architecturally one run per used
        // texture, since textureIndex is the sort's primary key.
        //
        // codex review, real finding (both the baker and renderer halves of
        // this parallel implementation missed it): ref/nv2a's per-face AND
        // whole-batch draw-call submission both snap a [start,end) LOGICAL
        // index range OUTWARD to the nearest 6-index/3-dword boundary
        // (r_context.c's own nv2a_map_face_t/nv2a_batch_dword_range
        // declaration comments - 6 = lcm(2,3), since indices pack two per
        // dword and triangles are 3 indices). That snap was harmless before
        // X9-K, when one shared atlas covered the whole map and an
        // over-snap just redrew a triangle of the SAME texture - but now
        // every batch below binds its OWN independent texture, so a batch
        // boundary that doesn't already land on a 6-index line lets the
        // snap read 1-2 triangles across into the NEXT batch and draw them
        // with the WRONG (that batch's) texture, a visible mistexturing
        // seam at nearly every texture-batch boundary on a real
        // multi-textured map. Faces always contribute a multiple of 3
        // indices (one whole triangle at a time - the fan-triangulation
        // loop below), so a batch's own running indexCount can only ever
        // land 0 or 3 short of a multiple of 6; the fix is to top it up to
        // the next multiple of 6 the MOMENT a batch is known to be
        // complete (this face is the last one using this texture), with a
        // real degenerate (zero-area) triangle - three copies of an index
        // this SAME batch already used, so it repeats real geometry rather
        // than reading unrelated vertex data - absorbed into the LAST real
        // face's own FaceDrawCmd.index_count via padLastFaceDrawCmd()
        // rather than a new fake face, so FaceDrawCmds stays an exact,
        // gapless partition of the index buffer (the invariant
        // ref/nv2a/r_context.c's own load-time tiling validation checks).
        // Since batches are contiguous by construction, padding every
        // batch's own end also aligns the very next batch's own start -
        // one fix at the source closes the gap for every batch boundary in
        // the archive, on both the per-face and whole-batch draw paths.
        std::vector<MpakTextureBatch> batches;
        uint16_t lastRealVertexIndex = 0; // padding fallback if a batch's own last face somehow emitted 0 real indices - see below
        for (uint32_t sortedPos = 0; sortedPos < static_cast<uint32_t>(order.size()); ++sortedPos) {
            FaceBuildRecord& rec = records[order[sortedPos]];

            uint32_t startIndex = currentIndexCount;
            uint32_t addedIndices = 0;
            for (size_t j = 2; j < rec.polyIndices.size(); ++j) {
                pak.addIndex(rec.polyIndices[0]);
                pak.addIndex(rec.polyIndices[j - 1]);
                pak.addIndex(rec.polyIndices[j]);
                addedIndices += 3;
            }
            if (!rec.polyIndices.empty()) {
                lastRealVertexIndex = rec.polyIndices[0];
            }

            pak.addFaceDrawCmd(startIndex, addedIndices);
            pak.addFaceInfo(rec.faceInfo);
            currentIndexCount += addedIndices;

            for (MpakExtraLightBlock extra : rec.extraBlocks) {
                extra.faceIndex = sortedPos;
                pak.addExtraLightBlock(extra);
            }

            if (batches.empty() || batches.back().textureIndex != rec.textureIndex) {
                MpakTextureBatch batch{};
                batch.textureIndex = rec.textureIndex;
                batch.startFace = sortedPos;
                batch.faceCount = 1;
                batch.startIndex = startIndex;
                batch.indexCount = addedIndices;
                batches.push_back(batch);
            } else {
                batches.back().faceCount += 1;
                batches.back().indexCount += addedIndices;
            }

            // This batch is complete once the NEXT face (if any) uses a
            // different texture, or this is the very last face overall -
            // pad it now, before any later batch's own startIndex is
            // computed from currentIndexCount.
            bool isLastFaceOfBatch = (sortedPos + 1 == order.size())
                || (records[order[sortedPos + 1]].textureIndex != rec.textureIndex);
            if (isLastFaceOfBatch) {
                uint32_t remainder = batches.back().indexCount % 6;
                if (remainder != 0) {
                    uint32_t pad = 6 - remainder;
                    for (uint32_t p = 0; p < pad; ++p) {
                        pak.addIndex(lastRealVertexIndex);
                    }
                    pak.padLastFaceDrawCmd(pad);
                    batches.back().indexCount += pad;
                    currentIndexCount += pad;
                }
            }
        }

        pak.setTextureBatches(std::move(batches));
        pak.setFaceRemap(std::move(faceRemap));

        std::cout << "Lightmap Atlas: " << lmBaked << " faces baked, " << lmSkipped
                  << " skipped (sky/liquid/fullbright), " << lmRejected << " rejected (anomalous), "
                  << lmPacker.pageCount() << " page(s)\n";

        uint32_t atlasHeight = 0;
        std::vector<uint8_t> atlasRgba = lmPacker.flatten(atlasHeight);
        pak.setLightmapAtlas(LM_PAGE_SIZE, atlasHeight, lmPacker.pageCount(), 0, std::move(atlasRgba));
        } // nv2a block

        pak.setVisibilityData(getLumpData(LUMP_VISIBILITY));
        pak.setNodesData(getLumpData(LUMP_NODES));
        pak.setLeavesData(getLumpData(LUMP_LEAVES));
        pak.setMarksurfacesData(getLumpData(LUMP_MARKSURFACES));
        pak.setPlanesData(getLumpData(LUMP_PLANES));
        pak.setClipnodesData(getLumpData(LUMP_CLIPNODES));
        pak.setModelsData(getLumpData(LUMP_MODELS));
        pak.setEntitiesData(getLumpData(LUMP_ENTITIES));
        pak.setTexinfoData(getLumpData(LUMP_TEXINFO));

        // v7 classic core (MpakHeader.h's v6 -> v7 note): the raw lumps the
        // engine's own loaders consume unchanged - ref/soft scan-converts from
        // SURFEDGES/EDGES every frame, the server and both renderers rebuild
        // lighting per lightstyle from LIGHTING samples - plus TEXTURES rebuilt
        // as embedded miptex so texture names, dimensions and 8-bit pixels
        // all reach the engine in the one shape it already parses.
        pak.setVertexesData(vertexData);
        pak.setEdgesData(edgeData);
        pak.setSurfedgesData(surfedgeData);
        pak.setFacesData(faceData);
        pak.setLightingData(getLumpData(LUMP_LIGHTING));
        pak.setMiptexData(buildEmbeddedMiptexLump(texData, coreWadTextures));
    }

} // namespace formats
