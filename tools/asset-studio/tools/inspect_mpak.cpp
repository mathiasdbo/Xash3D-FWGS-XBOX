// Dumps MpakHeader6's real fields from a baked .mpak archive - the
// actual struct, not hand-counted byte offsets, so there is never a risk
// of miscounting fields the way a manual hex dump can. Written while
// verifying MPAK v8 (the multi-lightstyle atlas blocks), kept as a
// small, permanent diagnostic: the same "what does this archive's
// header actually say" question comes up every time the format grows a
// field, and re-deriving hand offsets each time is exactly the kind of
// mistake this tool exists to avoid.
//
// Usage: inspect_mpak <file.mpak>
#include "formats/MpakHeader.h"
#include <cstdio>
#include <fstream>

using namespace formats;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: inspect_mpak <file.mpak>\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    f.seekg(MPAK_DESCRIPTOR_OFFSET);
    MpakHeader6 h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f) { std::fprintf(stderr, "short read - file too small to hold a descriptor\n"); return 1; }

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
