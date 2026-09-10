// Dumps MpakHeader6's real fields from a baked .mpak archive - the
// actual struct, not hand-counted byte offsets, so there is never a risk
// of miscounting fields the way a manual hex dump can. Written while
// verifying MPAK v8 (the multi-lightstyle atlas blocks), kept as a
// small, permanent diagnostic: the same "what does this archive's
// header actually say" question comes up every time the format grows a
// field, and re-deriving hand offsets each time is exactly the kind of
// mistake this tool exists to avoid.
//
// Validates before printing - found in review: the first version read
// whatever bytes were at MPAK_DESCRIPTOR_OFFSET and printed them
// unconditionally, exiting 0 regardless, so a plain .bsp or a corrupted
// archive produced a normal-looking report and a clean exit status,
// indistinguishable from a real one to anything scripting this tool.
// This checks magic/version/headerSize (the same invariants
// filesystem/mpak_layout.c's Mpak_ValidateHeader enforces on the engine
// side - re-checked here rather than shared, since this is a tiny,
// standalone C++ tool and that function is C, gated behind a real
// g_engfuncs the engine provides) and every lump's declared length
// against a real record size where one applies, printing a clear error
// and returning 1 on the first problem found.
//
// Usage: inspect_mpak <file.mpak>
#include "formats/MpakHeader.h"
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace formats;

namespace {

bool CheckDivisible(const char* name, uint32_t len, size_t recordSize) {
    if (recordSize > 0 && len % recordSize != 0) {
        std::fprintf(stderr, "INVALID: %s length %u is not a multiple of its %zu-byte record size\n",
            name, len, recordSize);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: inspect_mpak <file.mpak>\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
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
    std::printf("atlas: %ux%u, lenAtlas: %u\n", h.atlasWidth, h.atlasHeight, h.lenAtlas);
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
