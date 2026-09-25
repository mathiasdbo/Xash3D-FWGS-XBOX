#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace formats {

    // X9-W: the map's PHS (potentially hearable set), built offline.
    //
    // A multiplayer server builds the PHS at world load (Mod_CalcPHS,
    // engine/common/mod_bmodel.c:3735; upstream xash3d-fwgs
    // engine/common/mod_bmodel.c:3651). On a 64 MB console that load-time
    // build is what stops an Xbox from hosting the large maps: it holds
    // 2 x rowbytes x (visleafs + 1) of uncompressed rows at once (~9-12 MiB
    // on disposal/pool_party). The baker builds the same result here and
    // stores it in the archive (MPAK v13); the Xbox engine loads it
    // instead of calling Mod_CalcPHS (docs/r3d/divergences.md #117).
    //
    // The output must match Mod_CalcPHS byte for byte - world.phsofs and
    // world.compressed_phs - because Mod_FatPVS reads it with the engine's
    // own Mod_DecompressPVS. tools/test-phs.sh proves that on every retail
    // map with the PC test engine's test_phs command. Every step below is
    // a transcription of the engine function named next to it, including
    // the parts that look like accidents:
    //  - rows are ALIGN(visbytes, 4) bytes wide, but only visbytes are
    //    decompressed into them; the padding stays zero;
    //  - a leaf with visofs -1 decompresses to visbytes of 0xFF (no vis
    //    info: everything visible), leaf 0 (solid) included;
    //  - the OR loop scans the whole padded row, and a bit whose leaf
    //    index is >= visleafs + 1 is skipped;
    //  - each PHS row is compressed from all rowbytes, padding included.
    //
    // Where the engine would read outside the visibility lump (a visofs
    // past its end, or a compressed row that runs off it), its result
    // depends on whatever memory follows the lump and cannot be
    // reproduced offline. build() then returns false with a reason, the
    // map ships without a PHS, and an Xbox host falls back to
    // Mod_CalcPHS.

    // Raw BSP30 record layouts this reads (common/bspfile.h).
    constexpr size_t kDleafBytes = 28;       // dleaf_t: contents, visofs, mins[3], maxs[3], firstmarksurface, nummarksurfaces, ambient[4]
    constexpr size_t kDleafVisofs = 4;       // offsetof(dleaf_t, visofs)
    constexpr size_t kDmodelBytes = 64;      // dmodel_t
    constexpr size_t kDmodelVisleafs = 52;   // offsetof(dmodel_t, visleafs)
    constexpr size_t kMaxMapLeafs = 32767;   // MAX_MAP_LEAFS (common/bspfile.h), sizes the engine's per-row scratch

    struct PhsData {
        std::vector<uint32_t> offsets;  // world.phsofs: visleafs + 1 byte offsets into `data`
        std::vector<uint8_t> data;      // world.compressed_phs
        uint32_t visleafs = 0;
        uint32_t visbytes = 0;
        uint32_t rowbytes = 0;
    };

    // Mod_DecompressPVS (mod_bmodel.c). `in` may be null (no vis info).
    // Returns false where the engine would read past `inEnd`.
    inline bool phsDecompressRow(uint8_t* out, const uint8_t* in, const uint8_t* inEnd, size_t visbytes) {
        if (!in) {
            std::memset(out, 0xFF, visbytes);
            return true;
        }
        uint8_t* dst = out;
        uint8_t* const end = out + visbytes;
        while (dst < end) {
            if (in >= inEnd) return false;
            if (*in) {
                *dst++ = *in++;
            } else {
                if (in + 1 >= inEnd) return false;
                size_t c = in[1];
                if (c > static_cast<size_t>(end - dst)) c = static_cast<size_t>(end - dst);
                std::memset(dst, 0, c);
                in += 2;
                dst += c; // a run of 0 just consumes its two bytes, as in the engine
            }
        }
        return true;
    }

    // Mod_CompressPVS (mod_bmodel.c): zero bytes become {0, run length},
    // runs capped at 255. Appends to `out`.
    inline void phsCompressRow(std::vector<uint8_t>& out, const uint8_t* in, size_t inbytes) {
        for (size_t i = 0; i < inbytes; i++) {
            size_t j = i + 1, rep = 1;
            out.push_back(in[i]);
            if (in[i]) continue;
            for (; j < inbytes && rep != 255; j++, rep++) {
                if (in[j]) break;
            }
            out.push_back(static_cast<uint8_t>(rep));
            i = j - 1;
        }
    }

    // Mod_CalcPHS (mod_bmodel.c:3735), from the raw LUMP_VISIBILITY,
    // LUMP_LEAFS and LUMP_MODELS bytes. Returns false with `why` set when
    // the map has no PHS to bake (no vis data) or the engine's result
    // cannot be reproduced (see the header comment).
    inline bool buildPhs(const std::vector<uint8_t>& vis, const std::vector<uint8_t>& leafs,
                         const std::vector<uint8_t>& models, PhsData& out, std::string& why) {
        out = PhsData{};
        // Mod_LoadVisibility: no lump, no visdata, and Mod_CalcPHS returns early.
        if (vis.empty()) { why = "no visibility data"; return false; }
        if (models.size() < kDmodelBytes) { why = "no world model"; return false; }

        int32_t visleafs = 0;
        std::memcpy(&visleafs, models.data() + kDmodelVisleafs, sizeof(visleafs));
        const size_t numLeafRecords = leafs.size() / kDleafBytes;
        // Mod_LoadLeafs sets world.visbytes from submodels[0].visleafs and
        // Mod_SetupSubmodels sets the world's numleafs to it; Mod_CalcPHS
        // walks visleafs + 1 leaves, which must exist in the lump.
        if (visleafs < 0 || static_cast<size_t>(visleafs) + 1 > numLeafRecords || visleafs > static_cast<int32_t>(kMaxMapLeafs)) {
            why = "visleafs " + std::to_string(visleafs) + " does not fit " + std::to_string(numLeafRecords) + " leaves";
            return false;
        }

        const size_t count = static_cast<size_t>(visleafs) + 1;
        const size_t visbytes = (static_cast<size_t>(visleafs) + 7) >> 3;
        const size_t rowbytes = (visbytes + 3) & ~static_cast<size_t>(3); // ALIGN( world.visbytes, 4 )
        out.visleafs = static_cast<uint32_t>(visleafs);
        out.visbytes = static_cast<uint32_t>(visbytes);
        out.rowbytes = static_cast<uint32_t>(rowbytes);

        std::vector<uint8_t> pvs(rowbytes * count, 0), phs(rowbytes * count, 0);
        const uint8_t* const visEnd = vis.data() + vis.size();

        // Uncompress the PVS. Mod_LoadLeafs' final word on compressed_vis
        // is `p == -1 ? NULL : visdata + p` for every leaf, leaf 0 included.
        for (size_t i = 0; i < count; i++) {
            int32_t visofs = 0;
            std::memcpy(&visofs, leafs.data() + i * kDleafBytes + kDleafVisofs, sizeof(visofs));
            const uint8_t* in = nullptr;
            if (visofs != -1) {
                if (visofs < 0 || static_cast<size_t>(visofs) >= vis.size()) {
                    why = "leaf " + std::to_string(i) + " visofs " + std::to_string(visofs) + " outside the visibility lump";
                    return false;
                }
                in = vis.data() + visofs;
            }
            if (!phsDecompressRow(&pvs[rowbytes * i], in, visEnd, visbytes)) {
                why = "leaf " + std::to_string(i) + " PVS row runs past the visibility lump";
                return false;
            }
        }

        // Create the PHS: each row is its PVS row ORed with the PVS row of
        // every leaf it sees (+1: PVS bit n is leaf n + 1).
        for (size_t i = 0; i < count; i++) {
            const uint8_t* scan = &pvs[rowbytes * i];
            uint8_t* dst = &phs[rowbytes * i];
            std::memcpy(dst, scan, rowbytes);
            for (size_t j = 0; j < rowbytes; j++) {
                const unsigned bitbyte = scan[j];
                if (bitbyte == 0) continue;
                for (size_t k = 0; k < 8; k++) {
                    if (!(bitbyte & (1u << k))) continue;
                    const size_t index = j * 8 + k + 1;
                    if (index >= count) continue;
                    const uint8_t* src = &pvs[rowbytes * index];
                    for (size_t b = 0; b < rowbytes; b++) dst[b] |= src[b]; // Q_memor
                }
            }
        }

        // Compress, in leaf order, recording each row's offset.
        out.offsets.reserve(count);
        for (size_t i = 0; i < count; i++) {
            out.offsets.push_back(static_cast<uint32_t>(out.data.size()));
            phsCompressRow(out.data, &phs[rowbytes * i], rowbytes);
        }
        return true;
    }

    // The engine-side memory a baked PHS occupies while a server runs:
    // world.phsofs is a size_t per row (4 bytes on the Xbox) plus the
    // compressed rows. Nv2aMapFit.h's hosting check uses this.
    inline uint64_t phsResidentBytes(const PhsData& phs) {
        return static_cast<uint64_t>(phs.offsets.size()) * 4u + phs.data.size();
    }

} // namespace formats
