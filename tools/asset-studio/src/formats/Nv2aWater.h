#pragma once
#include <cstddef>
#include <cstdint>

namespace formats {

    // R5 (docs/r3d/divergences.md #147): the bytes ref/nv2a's static water
    // vertex buffer takes for a map - r_warp.c R_WarpNewMap builds it from
    // every SURF_DRAWTURB face's subdivided pieces, and r_context.c's per-map
    // ledger adds R_WarpGpuMapBytes for it. The count here runs the
    // renderer's own code on the map's classic lumps, so the two cannot
    // drift: ref/nv2a/nv2a_warp_util.c, compiled into this library
    // (CMakeLists.txt) - nv2a_warp_looks_like_water (the engine's
    // Mod_LooksLikeWaterTexture, which picks the faces the engine
    // subdivides), nv2a_warp_subdivide (GL_SubdivideSurface's
    // SubdividePolygon_r, which the engine runs on each of them at load)
    // and nv2a_warp_gpu_bytes (the buffer's size, capped as the renderer
    // caps it).
    //
    // The engine lowercases every texture name before it tests it (Q_strnlwr
    // in Mod_LoadTexture, mod_bmodel.c:2953 in the root clone) and gives a
    // face whose texinfo names a miptex past the lump texture 0
    // (mod_bmodel.c:3275-3276); the count does both. The vertex count is an
    // upper bound on the runtime's: a face the renderer cannot subdivide
    // (more than NV2A_WARP_MAX_VERTS edges, or a piece past the limit) is
    // left out on both sides, and a face past the renderer's 1 MiB
    // positions cap (r_warp.c NV2A_WARP_MAX_MAP_BYTES) or the buffer's
    // vertex cap only at run time.

    // The classic BSP30 lumps the count reads, as raw bytes (the archive's
    // own: faces, surfedges, edges, vertexes, texinfo, and the miptex lump,
    // whose header-only Xbox form keeps every name).
    struct Nv2aWaterLumps {
        const uint8_t* faces = nullptr;     size_t facesLen = 0;
        const uint8_t* surfedges = nullptr; size_t surfedgesLen = 0;
        const uint8_t* edges = nullptr;     size_t edgesLen = 0;
        const uint8_t* vertexes = nullptr;  size_t vertexesLen = 0;
        const uint8_t* texinfo = nullptr;   size_t texinfoLen = 0;
        const uint8_t* miptex = nullptr;    size_t miptexLen = 0;
    };

    struct Nv2aWaterCount {
        // The vertices every water face subdivides into: the static GPU
        // buffer's (nv2aWaterGpuBytes).
        uint32_t vertices = 0;
        // The CPU-side pieces (r_warp.c R_WarpSubdivideSurface): one
        // Mem_Calloc per face in the world model's pool - a 16-byte header
        // (32-bit pointers), the piece counts rounded up to 4 and 12 bytes
        // a vertex - each a zone chunk (nv2aZoneChunk), faces past the
        // renderer's 1 MiB cap on the payload left out as it leaves them
        // out. Allocated while the world loads (C1) and resident (C3):
        // an engine-side term, nv2aEngineWorldBytes' waterPolyBytes.
        uint64_t polyBytes = 0;
    };

    // A malformed index (an edge, vertex or texinfo past its lump) skips
    // that face.
    Nv2aWaterCount nv2aWaterCount(const Nv2aWaterLumps& l);
    uint32_t nv2aWaterVertexCount(const Nv2aWaterLumps& l);

    // nv2a_warp_gpu_bytes: the buffer's bytes for that many vertices.
    uint64_t nv2aWaterGpuBytes(uint32_t vertices);

} // namespace formats
