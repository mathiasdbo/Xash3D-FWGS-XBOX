// Dumps MpakHeader6's real fields from a baked .mpak archive - the
// actual struct, not hand-counted byte offsets, so there is never a risk
// of miscounting fields the way a manual hex dump can. Written while
// verifying MPAK v8 (the multi-lightstyle atlas blocks), kept as a
// small, permanent diagnostic: the same "what does this archive's
// header actually say" question comes up every time the format grows a
// field, and re-deriving hand offsets each time is exactly the kind of
// mistake this tool exists to avoid.
//
// Validates before printing - found in review, round 1: the first
// version read whatever bytes were at MPAK_DESCRIPTOR_OFFSET and
// printed them unconditionally, exiting 0 regardless, so a plain .bsp
// or a corrupted archive produced a normal-looking report and a clean
// exit status, indistinguishable from a real one to anything scripting
// this tool. Round 1's fix checked magic/version/headerSize and two
// lumps' divisibility, but never compared declared section lengths
// against the real file size - found in review, round 2: truncating a
// valid archive right after its first sector still passed, since every
// declared section was simply never walked. Fixed for real this time:
// walks every section in the same write order PakManager.cpp's save()
// uses (mirroring that file's own verifyFile() cursor walk, not
// re-derived from scratch), sector-rounding each declared length the
// same way, and requires the final cursor to land exactly on the real
// file size - the same "does the archive actually end where the header
// says it should" check that function's own comment calls out as
// catching exactly this shape of bug.
//
// Usage: inspect_mpak <file.mpak>
#include "formats/MpakHeader.h"
#include "formats/PakManager.h" // Vertex, for vCount's real per-record size
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>

using namespace formats;

namespace {

constexpr uint64_t SECTOR = 2048;

bool CheckDivisible(const char* name, uint32_t len, size_t recordSize) {
    if (recordSize > 0 && len % recordSize != 0) {
        std::fprintf(stderr, "INVALID: %s length %u is not a multiple of its %zu-byte record size\n",
            name, len, recordSize);
        return false;
    }
    return true;
}

// Advances cursor by length's sector-rounded extent (0 if length is 0 -
// PakManager.cpp's save() writes nothing, not even a placeholder sector,
// for an empty section - same convention filesystem/mpak_layout.c's own
// Mpak_PlaceLump documents).
void Place(uint64_t& cursor, uint64_t length) {
    if (length > 0)
        cursor += ((length + SECTOR - 1) / SECTOR) * SECTOR;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: inspect_mpak <file.mpak>\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
    if (fileSize < 2048) { std::fprintf(stderr, "INVALID: file is smaller than one sector\n"); return 1; }
    f.seekg(MPAK_DESCRIPTOR_OFFSET);
    MpakHeader6 h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f) { std::fprintf(stderr, "INVALID: short read - file too small to hold a descriptor\n"); return 1; }

    bool ok = true;
    if (std::memcmp(h.magic, "MPAK", 4) != 0) {
        std::fprintf(stderr, "INVALID: no MPAK magic at offset %u (this is not an MPAK archive)\n", MPAK_DESCRIPTOR_OFFSET);
        ok = false;
    }
    // This tool is built alongside the writer, so MpakHeader6{}'s own
    // default IS "the version this build's format currently is" - the
    // same source of truth PakManager::verifyFile() already trusts
    // (`desc.version != MpakHeader6{}.version`), not a hardcoded number
    // that would silently go stale on the next bump.
    if (h.version != MpakHeader6{}.version) {
        std::fprintf(stderr, "INVALID: version %u, this build only knows version %u\n", h.version, MpakHeader6{}.version);
        ok = false;
    }
    if (h.headerSize != sizeof(MpakHeader6)) {
        std::fprintf(stderr, "INVALID: headerSize %u, this build's MpakHeader6 is %zu bytes\n", h.headerSize, sizeof(MpakHeader6));
        ok = false;
    }
    ok &= CheckDivisible("lenFaceInfo", h.lenFaceInfo, sizeof(MpakFaceInfo));
    ok &= CheckDivisible("lenExtraLightBlocks", h.lenExtraLightBlocks, sizeof(MpakExtraLightBlock));
    ok &= CheckDivisible("lenFaceVertexBase", h.lenFaceVertexBase, sizeof(uint32_t));
    ok &= CheckDivisible("lenTextureDir", h.lenTextureDir, sizeof(MpakTextureInfo));
    ok &= CheckDivisible("lenBatches", h.lenBatches, sizeof(MpakTextureBatch));
    ok &= CheckDivisible("lenFaceRemap", h.lenFaceRemap, sizeof(uint32_t));

    // Only walk sections once the header itself is trustworthy - a bad
    // version/headerSize means the field offsets above may not even be
    // the real ones, so re-deriving a byte range from them would be
    // building on a foundation already known to be wrong.
    if (ok) {
        const bool xbox = (h.flags & MPAK_FLAG_XBOX) != 0;
        const bool nv2a = (h.flags & MPAK_FLAG_NV2A_BLOCK) != 0;
        uint64_t cursor = 2048; // first sector: dheader_t + descriptor

        // Exact write order from PakManager.cpp's save() (mirrored by
        // that file's own verifyFile(), not re-derived independently).
        // v12: every classic BSP lump first, then the renderer sections.
        Place(cursor, h.lenVisibility);
        Place(cursor, h.lenNodes);
        Place(cursor, h.lenLeaves);
        Place(cursor, h.lenMarksurfaces);
        Place(cursor, h.lenPlanes);
        Place(cursor, h.lenClipnodes);
        Place(cursor, h.lenModels);
        Place(cursor, h.lenEntities);
        Place(cursor, h.lenTexinfo);
        Place(cursor, h.lenVertexes);
        Place(cursor, h.lenEdges);
        Place(cursor, h.lenSurfedges);
        Place(cursor, h.lenFaces);
        Place(cursor, h.lenLighting);
        Place(cursor, h.lenMiptex);
        Place(cursor, nv2a ? static_cast<uint64_t>(h.vCount) * sizeof(Vertex) : 0);
        Place(cursor, nv2a ? static_cast<uint64_t>(h.iCount) * sizeof(uint16_t) : 0);
        Place(cursor, h.lenFaceDrawCmds);
        // v9: texture directory/data/batches (Xbox only), then the face
        // remap (both targets), then the PC arrays (PC only) - PakManager.cpp
        // save()'s own write order, which matches the header's field order
        // (MpakHeader.h's v8 -> v9 note).
        if (xbox) {
            Place(cursor, h.lenTextureDir);
            Place(cursor, h.lenTextureData);
            Place(cursor, h.lenBatches);
        }
        Place(cursor, h.lenFaceRemap);
        if (!xbox) {
            Place(cursor, h.lenArray64);
            Place(cursor, h.lenArray256);
            Place(cursor, h.lenArray512);
        }
        Place(cursor, h.lenAssetDict);
        Place(cursor, h.lenAssetData);
        Place(cursor, h.lenLightmapAtlas);
        Place(cursor, h.lenFaceInfo);
        Place(cursor, h.lenExtraLightBlocks);
        Place(cursor, h.lenFaceVertexBase);

        if (cursor != fileSize) {
            std::fprintf(stderr,
                "INVALID: declared sections end at %llu bytes, but the file is %llu bytes"
                " (truncated, corrupted, or a header/writer version mismatch)\n",
                static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(fileSize));
            ok = false;
        }
    }

    if (!ok) {
        std::fprintf(stderr, "%s is not a valid MPAK archive this build recognises\n", argv[1]);
        return 1;
    }

    std::printf("magic: %.4s\n", h.magic);
    std::printf("version: %u\n", h.version);
    std::printf("headerSize: %u (this build's sizeof(MpakHeader6): %zu)\n", h.headerSize, sizeof(MpakHeader6));
    std::printf("bakerBuildId: %u\n", h.bakerBuildId);
    std::printf("flags: %u%s%s\n", h.flags,
        (h.flags & MPAK_FLAG_XBOX) ? " [XBOX]" : " [PC]",
        (h.flags & MPAK_FLAG_NV2A_BLOCK) ? " [NV2A_BLOCK]" : " [core]");
    std::printf("vCount: %u, iCount: %u\n", h.vCount, h.iCount);
    std::printf("lenVisibility: %u, lenNodes: %u, lenLeaves: %u, lenMarksurfaces: %u\n",
        h.lenVisibility, h.lenNodes, h.lenLeaves, h.lenMarksurfaces);
    std::printf("lenFaceDrawCmds: %u, lenPlanes: %u, lenClipnodes: %u, lenModels: %u\n",
        h.lenFaceDrawCmds, h.lenPlanes, h.lenClipnodes, h.lenModels);
    std::printf("textures: count %u, lenTextureDir %u, lenTextureData %u\n",
        h.textureCount, h.lenTextureDir, h.lenTextureData);
    std::printf("batches: count %u, lenBatches %u\n", h.batchCount, h.lenBatches);
    std::printf("lenFaceRemap: %u (%zu original-order face(s))\n", h.lenFaceRemap, h.lenFaceRemap / sizeof(uint32_t));
    std::printf("array64: count %u len %u, array256: count %u len %u, array512: count %u len %u\n",
        h.count64, h.lenArray64, h.count256, h.lenArray256, h.count512, h.lenArray512);
    std::printf("lenAssetDict: %u, lenAssetData: %u (always 0 since the X2 scope narrowing)\n",
        h.lenAssetDict, h.lenAssetData);
    std::printf("lenEntities: %u\n", h.lenEntities);
    std::printf("lightmap: %ux%u, %u layer(s), format %u, lenLightmapAtlas: %u\n",
        h.lightmapWidth, h.lightmapHeight, h.lightmapLayers, h.lightmapFormat, h.lenLightmapAtlas);
    std::printf("lenFaceInfo: %u (%zu faces)\n", h.lenFaceInfo, h.lenFaceInfo / sizeof(MpakFaceInfo));
    std::printf("lenTexinfo: %u\n", h.lenTexinfo);
    std::printf("lenVertexes: %u, lenEdges: %u, lenSurfedges: %u, lenFaces: %u\n",
        h.lenVertexes, h.lenEdges, h.lenSurfedges, h.lenFaces);
    std::printf("lenLighting: %u, lenMiptex: %u\n", h.lenLighting, h.lenMiptex);
    std::printf("lenExtraLightBlocks: %u (%zu record(s))\n", h.lenExtraLightBlocks,
        h.lenExtraLightBlocks / sizeof(MpakExtraLightBlock));
    return 0;
}
