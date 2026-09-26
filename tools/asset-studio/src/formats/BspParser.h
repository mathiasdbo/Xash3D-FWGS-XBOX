#ifndef BSPPARSER_H
#define BSPPARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "PakManager.h"
#include "VfsIndexer.h"
#include "WadParser.h"

namespace formats {

    // Rebuilds LUMP_TEXTURES as the embedded-miptex lump the engine's
    // unmodified Mod_LoadTextures path parses (see the definition's own
    // comment for the per-texture policy). Public so it can be unit-tested
    // against hand-built lumps without a retail install.
    // headersOnly (the nv2a profile, MPAK v12): every entry is the 40-byte
    // header alone - see the definition's comment.
    std::vector<uint8_t> buildEmbeddedMiptexLump(const std::vector<uint8_t>& texData,
                                                 const std::unordered_map<std::string, const WadLump*>& externalWadTextures,
                                                 bool headersOnly = false);

    // Indexes one WAD's lumps by lower-cased name into the lookup the core
    // miptex builder consumes, later calls overriding earlier ones. Called
    // ONLY for the WADs the map's worldspawn "wad" key lists, in list order,
    // so the last listed WAD wins - the reference's own precedence
    // (Mod_LoadTextureFromWadList walks world.wadlist in reverse,
    // mod_bmodel.c:619-655). Unlisted WADs on disk must never reach this
    // index: found in review, the first version fed every WAD the VFS
    // could see, so an unrelated WAD sharing a name would have been baked
    // into the map permanently.
    void indexWadLumpsForCore(std::unordered_map<std::string, const WadLump*>& index,
                              const std::vector<WadLump>& lumps);

    // X9-X Part A: strips every "wad" key/value pair from the FIRST
    // entity block only (worldspawn) of a LUMP_ENTITIES buffer, matched
    // case-insensitively - the same scope and the same comparison the
    // engine's own parser uses (Mod_LoadEntities only ever reads "wad"
    // from the first entity, returning right after that block; the key
    // compare is Q_stricmp, engine/common/mod_bmodel.c:2439-2491,
    // key match at :2462). Used only for the Xbox nv2a bake (see the
    // definition's own comment for why). Leaves every other byte
    // identical, including a later entity's own "wad" key if any, and
    // returns the input completely unchanged for anything that is not a
    // well-formed leading `{...}` block rather than guess at a partial
    // strip. Public so it can be unit-tested against hand-built entity
    // lumps without a retail install.
    std::vector<uint8_t> stripWorldspawnWadKey(const std::vector<uint8_t>& entityData);

    enum BspLumpType {
        LUMP_ENTITIES = 0,
        LUMP_PLANES = 1,
        LUMP_TEXTURES = 2,
        LUMP_VERTICES = 3,
        LUMP_VISIBILITY = 4,
        LUMP_NODES = 5,
        LUMP_TEXINFO = 6,
        LUMP_FACES = 7,
        LUMP_LIGHTING = 8,
        LUMP_CLIPNODES = 9,
        LUMP_LEAVES = 10,
        LUMP_MARKSURFACES = 11,
        LUMP_EDGES = 12,
        LUMP_SURFEDGES = 13,
        LUMP_MODELS = 14,
        BSP_LUMPS = 15
    };

    struct BspHeader {
        uint32_t version;
        struct {
            uint32_t offset;
            uint32_t length;
        } lumps[BSP_LUMPS];
    };

#pragma pack(push, 1)
    struct dvertex_t {
        float point[3];
    };

    struct dedge_t {
        uint16_t v[2];
    };

    struct dface_t {
        int16_t planenum;
        int16_t side;
        int32_t firstedge;
        int16_t numedges;
        int16_t texinfo;
        uint8_t styles[4];
        int32_t lightofs;
    };

    struct dtexinfo_t {
        float vecs[2][4]; // [s/t][x,y,z,offset]
        uint32_t miptex;
        uint32_t flags;
    };

    struct dnode_t {
        uint32_t planenum;
        int16_t children[2]; // negative numbers are -(leafs + 1), not nodes
        int16_t mins[3];
        int16_t maxs[3];
        uint16_t firstface;
        uint16_t numfaces;
    };

    struct dleaf_t {
        int32_t contents;
        int32_t visofs; // -1 if no visibility info
        int16_t mins[3];
        int16_t maxs[3];
        uint16_t firstmarksurface;
        uint16_t nummarksurfaces;
        uint8_t ambient_level[4];
    };
#pragma pack(pop)

    class BspParser {
    public:
        BspParser() = default;
        bool load(const std::string& filePath);
        const std::vector<uint8_t>& getLumpData(BspLumpType type) const;
        void exportToPak(PakManager& pak, const VfsIndexer& vfs) const;

    private:
        BspHeader mHeader;
        std::vector<std::vector<uint8_t>> mLumpsData;
    };

} // namespace formats

#endif // BSPPARSER_H
