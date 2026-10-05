#include "Nv2aWater.h"
#include "Nv2aMapFit.h"     // nv2aZoneChunk
#include "nv2a_warp_util.h" // ref/nv2a, the renderer's own subdivision and water rule
#include <cstring>

namespace formats {

namespace {

    template <typename T>
    T readAt(const uint8_t* base, size_t offset) {
        T v;
        std::memcpy(&v, base + offset, sizeof(T));
        return v;
    }

    struct FaceCount { uint32_t pieces = 0, verts = 0; };

    void countPiece(void* ctx, int numverts, const float* /*verts*/) {
        FaceCount* c = static_cast<FaceCount*>(ctx);
        c->pieces++;
        c->verts += static_cast<uint32_t>(numverts);
    }

    // r_warp.c NV2A_WARP_MAX_MAP_BYTES and sizeof(nv2a_warp_polys_t) on the
    // Xbox (two uint16_t, two pointers, a uint32_t).
    constexpr uint64_t kWarpMaxMapBytes = 1024ull * 1024ull;
    constexpr uint64_t kWarpPolysHeaderBytes = 16;

} // namespace

Nv2aWaterCount nv2aWaterCount(const Nv2aWaterLumps& l) {
    Nv2aWaterCount out;
    uint64_t payload = 0; // the runtime cap's own sum (no zone headers)

    // dface_t (20 bytes): planenum u16, side u16, firstedge i32,
    // numedges u16, texinfo u16, styles[4], lightofs i32. texinfo_t (40):
    // vecs[2][4], miptex i32, flags i32. dedge_t: v[2] u16. dvertex_t:
    // point[3]. The miptex lump: nummiptex i32, dataofs[nummiptex] i32,
    // each a miptex_t whose first 16 bytes are the name (common/bspfile.h).
    if (l.miptexLen < 4)
        return out;
    const int32_t numMiptex = readAt<int32_t>(l.miptex, 0);
    if (numMiptex <= 0 || static_cast<uint64_t>(numMiptex) * 4 + 4 > l.miptexLen)
        return out;

    const size_t faces = l.facesLen / 20, texinfos = l.texinfoLen / 40;
    const size_t surfedges = l.surfedgesLen / 4, edges = l.edgesLen / 4, vertexes = l.vertexesLen / 12;

    for (size_t f = 0; f < faces; f++) {
        const uint8_t* face = l.faces + f * 20;
        const int32_t firstedge = readAt<int32_t>(face, 4);
        const uint16_t numedges = readAt<uint16_t>(face, 8);
        const uint16_t texinfo = readAt<uint16_t>(face, 10);
        if (texinfo >= texinfos)
            continue;
        int32_t miptex = readAt<int32_t>(l.texinfo + static_cast<size_t>(texinfo) * 40, 32);
        if (miptex < 0 || miptex >= numMiptex)
            miptex = 0; // Mod_LoadTexInfo: an out-of-range miptex is texture 0
        const int32_t ofs = readAt<int32_t>(l.miptex, 4 + static_cast<size_t>(miptex) * 4);
        if (ofs < 0 || static_cast<uint64_t>(ofs) + 16 > l.miptexLen)
            continue; // a missing texture: the engine's default, not water
        char name[17];
        std::memcpy(name, l.miptex + ofs, 16);
        name[16] = '\0';
        for (char& ch : name) // Q_strnlwr, as Mod_LoadTexture does before the test
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        if (!nv2a_warp_looks_like_water(name))
            continue;

        // GL_SubdivideSurface's own refusals (r_warp.c R_WarpSubdivideSurface).
        if (numedges < 3 || numedges > NV2A_WARP_MAX_VERTS || firstedge < 0
            || static_cast<uint64_t>(firstedge) + numedges > surfedges)
            continue;

        // R_GetEdgePosition, gl_rsurf.c:163 (BSP30's 16-bit edges).
        float verts[NV2A_WARP_SUBDIVIDE_SIZE][3];
        bool ok = true;
        for (uint16_t i = 0; i < numedges && ok; i++) {
            const int32_t lindex = readAt<int32_t>(l.surfedges, (static_cast<size_t>(firstedge) + i) * 4);
            const int64_t e = lindex > 0 ? lindex : -static_cast<int64_t>(lindex);
            if (static_cast<uint64_t>(e) >= edges) { ok = false; break; }
            const uint16_t v = readAt<uint16_t>(l.edges, static_cast<size_t>(e) * 4 + (lindex > 0 ? 0 : 2));
            if (v >= vertexes) { ok = false; break; }
            std::memcpy(verts[i], l.vertexes + static_cast<size_t>(v) * 12, 12);
        }
        if (!ok)
            continue;

        FaceCount pieces;
        // r_warp.c R_WarpSubdivideSurface's refusals: the renderer drops
        // the whole face.
        if (!nv2a_warp_subdivide(numedges, verts[0], countPiece, &pieces)
            || pieces.pieces == 0 || pieces.pieces > 0xFFFF || pieces.verts > 0xFFFF)
            continue;

        const uint64_t bytes = kWarpPolysHeaderBytes + ((static_cast<uint64_t>(pieces.pieces) + 3) & ~3ull) + 12ull * pieces.verts;
        if (payload + bytes > kWarpMaxMapBytes)
            continue; // r_warp.c: past the cap the face stays unwarped, with no pieces
        payload += bytes;
        out.polyBytes += nv2aZoneChunk(bytes);

        // r_warp.c R_WarpGpuPlace: only faces that kept their pieces get a
        // place in the GPU buffer, in face order, and a face that would
        // overflow NV2A_WARP_GPU_MAX_VERTS is skipped while later, smaller
        // ones can still fit.
        if (out.vertices + pieces.verts <= NV2A_WARP_GPU_MAX_VERTS)
            out.vertices += pieces.verts;
    }
    return out;
}

uint32_t nv2aWaterVertexCount(const Nv2aWaterLumps& l) {
    return nv2aWaterCount(l).vertices;
}

uint64_t nv2aWaterGpuBytes(uint32_t vertices) {
    return nv2a_warp_gpu_bytes(vertices);
}

} // namespace formats
