#include "BspParser.h"
#include "WadParser.h"
#include "LightmapAtlasPacker.h"
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
        using namespace lightmap_atlas; // X9-R: LightmapAtlasPacker.h

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
    //
    // headersOnly (the nv2a profile, X9-T): every entry is the header alone,
    // offsets zero - what a retail .bsp whose textures live in WADs carries.
    // ref/nv2a draws from the archive's own DXT1 directory and discards the
    // pixels the engine hands it (its GL_LoadTexture returns 0), so the 8-bit
    // copies only cost memory: they are 4.5 MB of rocket_frenzy's classic
    // prefix, which the engine holds whole while it builds the world model.
    // The engine then takes its own no-pixels path (a WAD lookup, then its
    // default texture), and names and dimensions are kept, for the same
    // reasons as the header-only case above.
    // Unit-tested in tests/miptex_lump_test.cpp.
    std::vector<uint8_t> buildEmbeddedMiptexLump(const std::vector<uint8_t>& texData,
                                                 const std::unordered_map<std::string, const WadLump*>& externalWadTextures,
                                                 bool headersOnly) {
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

            if (!headersOnly && src->offsets[0] >= sizeof(miptex_t)) {
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

            if (headerOnly && !headersOnly) {
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

    // X9-X Part A, gap 1: on Xbox nv2a every world texture takes the
    // WAD branch regardless of this baker's own miptex policy (#112 -
    // the archive's directory offsets are already zero) because
    // Mod_LoadTextureData falls back to a WAD lookup whenever offsets[0]
    // is <= 0 (engine/common/mod_bmodel.c:2911-2922), and the Xbox disc
    // stages the whole retail valve dir, WADs included
    // (tools/xbox-xiso.sh:230). That WAD lookup only ever runs for a
    // texture whose containing map's worldspawn "wad" key names a WAD on
    // disk (Mod_LoadTextureFromWadList walks world.wadlist, itself built
    // only from that key, mod_bmodel.c:2439-2491). Dropping the key here
    // makes wadcount 0, so every world texture instead falls straight to
    // R_GetBuiltinTexture(REF_DEFAULT_TEXTURE) (mod_bmodel.c:2946-2951)
    // with nothing decoded or uploaded - ref/nv2a never draws a world
    // texture this way regardless (it samples the archive's own DXT1
    // atlas), so the engine doing real WAD decode work for a texture
    // no renderer will ever sample was pure waste: median 2.73 MiB,
    // max 13.23 MiB of RGBA32 decodes per map (measured over the retail
    // WAD set and all 125 maps).
    std::vector<uint8_t> stripWorldspawnWadKey(const std::vector<uint8_t>& entityData) {
        std::string_view text(reinterpret_cast<const char*>(entityData.data()), entityData.size());

        size_t openBrace = text.find_first_not_of(" \t\r\n");
        if (openBrace == std::string_view::npos || text[openBrace] != '{')
            return entityData; // malformed: no leading entity block

        size_t closeBrace = text.find('}', openBrace + 1);
        if (closeBrace == std::string_view::npos)
            return entityData; // malformed: unterminated first block

        // Byte ranges within the first block to drop, in the order found.
        std::vector<std::pair<size_t, size_t>> removals;
        size_t pos = openBrace + 1;

        while (pos < closeBrace) {
            size_t scan = text.find_first_not_of(" \t\r\n", pos);
            if (scan == std::string_view::npos || scan >= closeBrace)
                break; // only trailing whitespace left before the closing brace

            if (text[scan] != '"')
                return entityData; // malformed: expected a quoted key

            const size_t keyStart = scan;
            const size_t keyEnd = text.find('"', keyStart + 1);
            if (keyEnd == std::string_view::npos || keyEnd >= closeBrace)
                return entityData; // malformed: unterminated key

            const size_t valScan = text.find_first_not_of(" \t\r\n", keyEnd + 1);
            if (valScan == std::string_view::npos || valScan >= closeBrace || text[valScan] != '"')
                return entityData; // malformed: expected a quoted value

            const size_t valEnd = text.find('"', valScan + 1);
            if (valEnd == std::string_view::npos || valEnd >= closeBrace)
                return entityData; // malformed: unterminated value

            const size_t pairEnd = valEnd + 1;
            const std::string_view key = text.substr(keyStart + 1, keyEnd - keyStart - 1);
            const bool isWad = key.size() == 3
                && std::tolower(static_cast<unsigned char>(key[0])) == 'w'
                && std::tolower(static_cast<unsigned char>(key[1])) == 'a'
                && std::tolower(static_cast<unsigned char>(key[2])) == 'd';

            if (isWad) {
                size_t removeEnd = pairEnd;
                // Absorb one trailing line break so the stripped line
                // doesn't leave a blank one behind - cosmetic only, the
                // engine's own parser (COM_ParseFile) skips whitespace
                // freely either way.
                if (removeEnd < text.size() && text[removeEnd] == '\r') ++removeEnd;
                if (removeEnd < text.size() && text[removeEnd] == '\n') ++removeEnd;
                removals.emplace_back(keyStart, removeEnd);
            }

            pos = pairEnd;
        }

        if (removals.empty())
            return entityData;

        std::vector<uint8_t> out;
        out.reserve(entityData.size());
        size_t cursor = 0;
        for (const auto& [start, end] : removals) {
            out.insert(out.end(), entityData.begin() + static_cast<long>(cursor), entityData.begin() + static_cast<long>(start));
            cursor = end;
        }
        out.insert(out.end(), entityData.begin() + static_cast<long>(cursor), entityData.end());
        return out;
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
        // X9-U: the Xbox atlas is packed tight, like the reference's, and
        // holds style 0 only - ref/nv2a draws no lightstyle but the first
        // (MpakExtraLightBlock is never read), so the extra-style blocks
        // only cost console memory. The PC bake keeps both.
        const bool lmBordered = !pak.isXboxTarget();
        const bool lmExtraStyles = !pak.isXboxTarget();
        LightmapAtlasPacker lmPacker(lmBordered);
        // Centered in the white guard band that follows the gradient (not the
        // gradient itself), so a face with no real lightmap data always samples
        // pure white regardless of any future change to the gradient's own values.
        const float kFallbackLu = (static_cast<float>(LM_RAMP_GRADIENT_WIDTH) + LM_RAMP_WHITE_WIDTH * 0.5f) / static_cast<float>(LM_PAGE_SIZE);
        float kFallbackLv = 0.0f;       // X9-R: set once the atlas height is known (pre-pass below)
        float lmAtlasHeight = static_cast<float>(LM_PAGE_SIZE); // X9-R: lv's divisor, likewise
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
            std::vector<uint16_t> polyIndices; // v11: relative to vertexBase
            uint32_t vertexBase = 0;           // v11: this face's vertex-chunk base (PakManager::beginFaceVertices)
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

        // --- Lightmap extents (GoldSrc BSP30 / xash3d-fwgs
        // Mod_CalcSurfaceExtents semantics) ---
        // texturemins/extents are computed from the SAME rawU/rawV dot products
        // the per-vertex loop below uses for diffuse UVs, over float mins/maxs.
        //
        // X9-R: lifted into a lambda so the allocation pre-pass and the main
        // face loop compute exactly the same thing. `reject` reports why a
        // face has no lightmap: 0 = it has one, 1 = skipped (sky, liquid, no
        // styles), 2 = extents over LM_MAX_LUXELS, 3 = LUMP_LIGHTING too short.
        struct LmExtents {
            bool hasLightmap = false;
            int32_t texturemins[2] = {0, 0};
            uint32_t smax = 0, tmax = 0;
            int reject = 1;
        };
        auto computeLmExtents = [&](const dface_t& face, const dtexinfo_t& texinfo) {
            LmExtents e;
            e.hasLightmap = face.numedges > 0
                && face.lightofs >= 0
                && face.styles[0] != 255
                && (texinfo.flags & TEX_SPECIAL) == 0;
            if (!e.hasLightmap)
                return e;

            float mins[2] = { 999999.0f, 999999.0f };
            float maxs[2] = { -999999.0f, -999999.0f };
            for (int16_t j = 0; j < face.numedges; ++j) {
                int32_t surfedge = surfedges[face.firstedge + j];
                uint16_t vertexIndex = (surfedge >= 0) ? edges[surfedge].v[0] : edges[-surfedge].v[1];
                const dvertex_t& v = vertices[vertexIndex];

                float s = v.point[0] * texinfo.vecs[0][0] + v.point[1] * texinfo.vecs[0][1] + v.point[2] * texinfo.vecs[0][2] + texinfo.vecs[0][3];
                float t = v.point[0] * texinfo.vecs[1][0] + v.point[1] * texinfo.vecs[1][1] + v.point[2] * texinfo.vecs[1][2] + texinfo.vecs[1][3];
                mins[0] = std::min(mins[0], s); maxs[0] = std::max(maxs[0], s);
                mins[1] = std::min(mins[1], t); maxs[1] = std::max(maxs[1], t);
            }

            int32_t bmins[2], bmaxs[2];
            for (int k = 0; k < 2; ++k) {
                bmins[k] = static_cast<int32_t>(std::floor(mins[k] / LM_SAMPLE_SIZE));
                bmaxs[k] = static_cast<int32_t>(std::ceil(maxs[k] / LM_SAMPLE_SIZE));
                e.texturemins[k] = bmins[k] * static_cast<int32_t>(LM_SAMPLE_SIZE);
            }
            e.smax = static_cast<uint32_t>((bmaxs[0] - bmins[0])) + 1;
            e.tmax = static_cast<uint32_t>((bmaxs[1] - bmins[1])) + 1;

            if (e.smax > LM_MAX_LUXELS || e.tmax > LM_MAX_LUXELS) {
                e.hasLightmap = false;
                e.reject = 2;
            } else {
                size_t required = static_cast<size_t>(face.lightofs) + static_cast<size_t>(e.smax) * e.tmax * 3;
                if (required > lightingData.size()) {
                    e.hasLightmap = false;
                    e.reject = 3;
                } else {
                    e.reject = 0;
                }
            }
            return e;
        };

        // X9-R: allocation pre-pass. Every lightmap block this map will bake -
        // each face's style 0 plus its extra style blocks, with the same
        // bounds rules the main loop applies - is sized first and packed
        // tallest-first (LightmapAtlasPacker::allocateSorted), so the atlas's
        // final height is known before any vertex's lv is computed. The main
        // loop below takes its blocks from lmFaceAllocs instead of calling
        // allocate() itself.
        std::vector<std::vector<LightmapAllocation>> lmFaceAllocs(numFaces);
        std::vector<LmExtents> lmExtentsByFace(numFaces); // computed once here, reused by the main loop
        {
            std::vector<std::pair<uint32_t, uint32_t>> sizes;
            std::vector<std::pair<size_t, size_t>> owner; // (face, slot) per request
            for (size_t i = 0; i < numFaces; ++i) {
                const dface_t& face = faces[i];
                const dtexinfo_t& texinfo = (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < numTexinfos) ? texinfos[face.texinfo] : dtexinfo_t{};
                lmExtentsByFace[i] = computeLmExtents(face, texinfo);
                const LmExtents& e = lmExtentsByFace[i];
                if (!e.hasLightmap)
                    continue;
                sizes.emplace_back(e.smax, e.tmax);
                owner.emplace_back(i, 0);
                const size_t blockBytes = static_cast<size_t>(e.smax) * e.tmax * 3;
                for (int styleSlot = 1; lmExtraStyles && styleSlot < 4; ++styleSlot) {
                    if (face.styles[styleSlot] == 255)
                        break;
                    size_t extraOfs = static_cast<size_t>(face.lightofs) + static_cast<size_t>(styleSlot) * blockBytes;
                    if (extraOfs + blockBytes > lightingData.size())
                        break;
                    sizes.emplace_back(e.smax, e.tmax);
                    owner.emplace_back(i, static_cast<size_t>(styleSlot));
                }
            }
            const std::vector<LightmapAllocation> allocs = lmPacker.allocateSorted(sizes);
            for (size_t k = 0; k < allocs.size(); ++k) {
                std::vector<LightmapAllocation>& list = lmFaceAllocs[owner[k].first];
                if (list.size() <= owner[k].second)
                    list.resize(owner[k].second + 1);
                list[owner[k].second] = allocs[k];
            }
            lmAtlasHeight = static_cast<float>(lmPacker.atlasHeight());
            kFallbackLv = (LM_RAMP_HEIGHT * 0.5f) / lmAtlasHeight;
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

            // Lightmap extents: computed once by the allocation pre-pass
            // above and reused here, so the blocks this loop writes are by
            // construction the ones the pre-pass sized and placed.
            const LmExtents& lmExt = lmExtentsByFace[i];
            bool hasLightmap = lmExt.hasLightmap;
            int32_t texturemins[2] = { lmExt.texturemins[0], lmExt.texturemins[1] };
            uint32_t smax = lmExt.smax, tmax = lmExt.tmax;
            if (lmExt.reject == 2) {
                std::cerr << "Lightmap: face " << i << " extents " << smax << "x" << tmax
                          << " exceed " << LM_MAX_LUXELS << " luxels - using white fallback\n";
                lmRejected++;
            } else if (lmExt.reject == 3) {
                std::cerr << "Lightmap: face " << i << " lightofs " << face.lightofs
                          << " + " << (smax * tmax * 3) << " bytes exceeds LUMP_LIGHTING size "
                          << lightingData.size() << " - using white fallback\n";
                lmRejected++;
            } else if (lmExt.reject == 1) {
                lmSkipped++;
            }

            LightmapAllocation lmAlloc; // defaults to {page 0, 0, 0} - the reserved white corner
            if (hasLightmap) {
                lmAlloc = lmFaceAllocs[i][0]; // X9-R: allocated in the pre-pass
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
                for (int styleSlot = 1; lmExtraStyles && styleSlot < 4; ++styleSlot) {
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
                    LightmapAllocation extraAlloc = lmFaceAllocs[i][styleSlot]; // X9-R: pre-pass
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
            rec.vertexBase = pak.beginFaceVertices(face.numedges > 0 ? static_cast<uint32_t>(face.numedges) : 0u);

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
                    // (blockX, blockY) is the block's origin; + border() lands on the
                    // first data luxel (1 bordered, 0 tight) and the +0.5 centers the
                    // sample in that luxel. Bordered, bilinear reaches at most 0.5 texel
                    // into the replicated border; tight (Xbox, X9-U), half a luxel into
                    // the neighbouring block, exactly as ref/gl's own
                    // (light_s + (s - mins) / 16 + 0.5) / BLOCK_SIZE does.
                    const float border = static_cast<float>(lmPacker.border());
                    float lu = (static_cast<float>(lmAlloc.blockX) + border + (rawU - texturemins[0]) / static_cast<float>(LM_SAMPLE_SIZE) + 0.5f) / static_cast<float>(LM_PAGE_SIZE);
                    float lv = (static_cast<float>(lmAlloc.blockY) + border + (rawV - texturemins[1]) / static_cast<float>(LM_SAMPLE_SIZE) + 0.5f) / lmAtlasHeight; // X9-R: final (trimmed) height
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
            // v11: then by vertex chunk, so one texture's faces form at most
            // one run per chunk - a batch below never spans two chunks.
            if (records[a].vertexBase != records[b].vertexBase) return records[a].vertexBase < records[b].vertexBase;
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
            pak.addFaceVertexBase(rec.vertexBase); // v11
            pak.addFaceInfo(rec.faceInfo);
            currentIndexCount += addedIndices;

            for (MpakExtraLightBlock extra : rec.extraBlocks) {
                extra.faceIndex = sortedPos;
                pak.addExtraLightBlock(extra);
            }

            // v11: a batch is a run of one texture WITHIN one vertex chunk,
            // so a batch drawn as a whole (ref/nv2a's per-batch fallback)
            // needs a single base vertex.
            const bool newBatch = batches.empty() || batches.back().textureIndex != rec.textureIndex
                || (sortedPos > 0 && records[order[sortedPos - 1]].vertexBase != rec.vertexBase);
            if (newBatch) {
                // v11: the padding fallback index must belong to this batch's
                // own chunk; 0 is always a real vertex of it.
                lastRealVertexIndex = 0;
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
                || (records[order[sortedPos + 1]].textureIndex != rec.textureIndex)
                || (records[order[sortedPos + 1]].vertexBase != rec.vertexBase); // v11
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
        // X9-U: only textures a batch draws reach the Xbox archive -
        // ref/nv2a binds textures through the batch table alone, and the
        // unused ones were 0.56 MiB of rocket_frenzy's GPU texture data.
        if (pak.isXboxTarget() && pak.isProfileNv2a()) {
            const size_t dropped = pak.pruneUnusedXboxTextures();
            std::cout << "Textures: dropped " << dropped << " no face draws\n";
        }
        pak.setFaceRemap(std::move(faceRemap));

        std::cout << "Vertex chunks: " << pak.vertexChunkCount() << "\n"; // v11 - 1 unless the map outgrows u16 indices
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
        if (pak.isProfileNv2a() && pak.isXboxTarget()) {
            // X9-X Part A: see stripWorldspawnWadKey's own comment - this
            // is what makes the ENGINE take the no-pixels path too,
            // instead of only ref/nv2a discarding what the engine already
            // decoded from a WAD it never needed to open.
            pak.setEntitiesData(stripWorldspawnWadKey(getLumpData(LUMP_ENTITIES)));
        } else {
            pak.setEntitiesData(getLumpData(LUMP_ENTITIES));
        }
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
        // Headers only for the Xbox nv2a bake, the one ref/nv2a draws: a PC
        // nv2a-profile bake keeps its pixels for the PC renderers.
        pak.setMiptexData(buildEmbeddedMiptexLump(texData, coreWadTextures, pak.isProfileNv2a() && pak.isXboxTarget()));
    }

} // namespace formats
