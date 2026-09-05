#ifndef BSPPARSER_H
#define BSPPARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include "PakManager.h"
#include "VfsIndexer.h"

namespace formats {

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
