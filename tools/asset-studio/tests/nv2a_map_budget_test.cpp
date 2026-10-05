// Unit test for Nv2aMapBudget.h's bake-time mirror of ref/nv2a's per-map
// memory ledger (r_context.c NV2A_MAP_BUDGET_BYTES,
// r_context.c nv2a_load_map_geometry the ledger formula and the strict `>` compare).
// Pins the formula term by term, since a single end-to-end example could
// hide a term with the wrong coefficient behind one with the opposite
// error.
#include "formats/Nv2aMapBudget.h"
#include "formats/Nv2aMapFit.h"
#include "formats/Nv2aWater.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

MpakHeader6 zeroHeader() {
    // Every len*/count field defaults to 0 already (MpakHeader6's own
    // NSDMIs) - only the fields this test cares about are set per case.
    return MpakHeader6{};
}

template <typename T>
void put(std::vector<uint8_t>& v, T x) {
    const size_t at = v.size();
    v.resize(at + sizeof(T));
    std::memcpy(v.data() + at, &x, sizeof(T));
}

// R5: a tiny BSP30 lump set - one 128x128 square at the origin (4
// vertices, edges 1..4, surfedges 1..4) under each of several texinfos.
struct WaterBsp {
    std::vector<uint8_t> faces, surfedges, edges, vertexes, texinfo, miptex;

    explicit WaterBsp(const std::vector<const char*>& names) {
        const float corners[4][3] = { { 0, 0, 0 }, { 128, 0, 0 }, { 128, 128, 0 }, { 0, 128, 0 } };
        for (const auto& c : corners)
            for (float f : c) put(vertexes, f);
        const uint16_t e[5][2] = { { 0, 0 }, { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 } };
        for (const auto& pair : e) { put(edges, pair[0]); put(edges, pair[1]); }
        for (int32_t i = 1; i <= 4; i++) put(surfedges, i);
        // The miptex lump: count, offsets, then a 40-byte miptex_t each.
        put(miptex, static_cast<int32_t>(names.size()));
        for (size_t i = 0; i < names.size(); i++)
            put(miptex, static_cast<int32_t>(4 + 4 * names.size() + 40 * i));
        for (const char* n : names) {
            char rec[40] = {};
            std::strncpy(rec, n, 15);
            miptex.insert(miptex.end(), rec, rec + 40);
        }
        for (size_t i = 0; i < names.size(); i++) {
            for (int k = 0; k < 8; k++) put(texinfo, 0.0f);
            put(texinfo, static_cast<int32_t>(i));
            put(texinfo, static_cast<int32_t>(0));
        }
    }

    void addFace(uint16_t texinfoIndex, uint16_t numedges = 4) {
        put(faces, static_cast<uint16_t>(0));    // planenum
        put(faces, static_cast<uint16_t>(0));    // side
        put(faces, static_cast<int32_t>(0));     // firstedge
        put(faces, numedges);
        put(faces, texinfoIndex);
        put(faces, static_cast<uint32_t>(0));    // styles
        put(faces, static_cast<int32_t>(-1));    // lightofs
    }

    Nv2aWaterLumps lumps() const {
        Nv2aWaterLumps l;
        l.faces = faces.data(); l.facesLen = faces.size();
        l.surfedges = surfedges.data(); l.surfedgesLen = surfedges.size();
        l.edges = edges.data(); l.edgesLen = edges.size();
        l.vertexes = vertexes.data(); l.vertexesLen = vertexes.size();
        l.texinfo = texinfo.data(); l.texinfoLen = texinfo.size();
        l.miptex = miptex.data(); l.miptexLen = miptex.size();
        return l;
    }
};

} // namespace

int main() {
    static_assert(kNv2aMapBudgetBytes == 25165824, "24 MiB must equal ref/nv2a/r_context.c nv2a_load_map_geometry's NV2A_MAP_BUDGET_BYTES");

    // --- exactly-at-budget passes; one byte over fails (pins the ref's
    //     own strict `>` compare, r_context.c nv2a_load_map_geometry) ---
    {
        MpakHeader6 h = zeroHeader();
        // Everything else 0, so the lightmap alone carries the whole ledger.
        h.lenLightmapAtlas = static_cast<uint32_t>(kNv2aMapBudgetBytes);
        const uint64_t exact = nv2aMapLedgerBytes(h);
        CHECK(exact == kNv2aMapBudgetBytes, "exact-budget ledger must equal the cap");
        CHECK(nv2aMapWithinBudget(exact), "exactly-at-budget must PASS (strict > in the ref, not >=)");

        h.lenLightmapAtlas = static_cast<uint32_t>(kNv2aMapBudgetBytes + 1);
        const uint64_t overByOne = nv2aMapLedgerBytes(h);
        CHECK(overByOne == kNv2aMapBudgetBytes + 1, "one-byte-over ledger must equal cap+1");
        CHECK(!nv2aMapWithinBudget(overByOne), "one byte over budget must FAIL");
    }

    // --- odd iCount rounding: dword_count = (iCount+1)/2, idxBytes = that * 4 ---
    {
        MpakHeader6 h = zeroHeader();
        h.iCount = 3; // (3+1)/2 = 2 dwords -> 8 bytes
        CHECK(nv2aMapLedgerBytes(h) == 8, "iCount=3 must give 8 index bytes");

        h.iCount = 0; // (0+1)/2 = 0 dwords -> 0 bytes
        CHECK(nv2aMapLedgerBytes(h) == 0, "iCount=0 must give 0 index bytes");
    }

    // --- faceDrawCmds/8 truncation (the per-face PVS visibility table) ---
    {
        MpakHeader6 h = zeroHeader();
        h.lenFaceDrawCmds = 15; // 15/8 = 1 face: 1*24 (faces term) + 15/8 (vis term, truncates to 1)
        // MPAK v12: plus the 15-byte lump itself, read in for the load.
        // X9-X step 8: plus one face's 9 bytes of lightmap tables.
        // R3: plus its 5 bytes of visible-face list, plane reference and listable flag.
        CHECK(kNv2aLightmapFaceBytes == 9, "r_lightmap.h NV2A_LM_FACE_BYTES: 6-byte block + held byte + 2-byte styled entry");
        CHECK(kNv2aPvsListFaceBytes == 5, "r_context.c NV2A_PVS_LIST_FACE_BYTES: a uint16_t list entry + a uint16_t plane reference + a listable byte");
        CHECK(nv2aMapLedgerBytes(h) == 1 * 24 + 1 + 9 + 5 + 15, "lenFaceDrawCmds=15 must give one 24-byte face, vis=1 (integer truncation), 9 lightmap-table bytes, 5 list bytes and its 15-byte load copy");
    }

    // --- the vertex term uses kNv2aLitVertexBytes (28), the RUNTIME nv2a_lit_vertex_t
    //     size, never sizeof(Vertex)/mpak_vertex_t, the smaller on-disk record ---
    {
        MpakHeader6 h = zeroHeader();
        h.vCount = 1;
        CHECK(kNv2aLitVertexBytes == 28, "nv2a_lit_vertex_t must be 28 bytes (packed float pos[3], uv0[2], uv1[2])");
        CHECK(nv2aMapLedgerBytes(h) == 28, "vCount=1 must contribute exactly 28 bytes of GPU buffer (outweighing its 12-byte positions copy)");
        // X9-T: GPU buffers and the load temporaries never coexist - the
        // larger counts, not the sum.
        h.lenTextureDir = 100;
        CHECK(nv2aMapLedgerBytes(h) == 100 + 12, "a 100-byte texture directory + 12 bytes of positions outweigh 28 bytes of GPU buffers: max, not sum");
        h.lenLightmapAtlas = 200;
        CHECK(nv2aMapLedgerBytes(h) == 28 + 200, "228 bytes of GPU buffers outweigh a 112-byte load");
        h.lenLightmapAtlas = 0;
        h.lenTextureDir = 0;
    }

    // --- no 32-bit overflow: every term is computed in uint64, unlike the
    //     ref's own uint32 dword_count (Nv2aMapBudget.h's own comment on
    //     why that divergence is harmless for any real map) ---
    {
        MpakHeader6 h = zeroHeader();
        h.vCount = std::numeric_limits<uint32_t>::max();
        const uint64_t expected = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) * kNv2aLitVertexBytes;
        CHECK(nv2aMapLedgerBytes(h) == expected, "vCount=UINT32_MAX must not overflow the vertex term");
        CHECK(!nv2aMapWithinBudget(nv2aMapLedgerBytes(h)), "a UINT32_MAX vertex count must be far over budget");
    }

    // --- MPAK v12: the engine's view is the first sector plus every
    //     classic lump padded to a sector; renderer lumps are not in it ---
    {
        MpakHeader6 h = zeroHeader();
        CHECK(mpakClassicPrefixBytes(h) == 2048, "an archive with no classic lumps is just its first sector");
        h.lenEntities = 1;
        h.lenMiptex = 2049;
        h.lenLightmapAtlas = 4096;
        h.lenFaceDrawCmds = 8;
        CHECK(mpakClassicPrefixBytes(h) == 2048 + 2048 + 4096, "classic lumps pad to whole sectors; renderer lumps do not count");
        // The renderer-only temporaries the load reads in count once.
        MpakHeader6 t = zeroHeader();
        t.lenTextureDir = 16;
        t.lenBatches = 20;
        t.lenFaceVertexBase = 4;
        CHECK(nv2aMapLedgerBytes(t) == 16 + 20 + 4, "the texture directory, batches and vertex bases are load temporaries");
    }

    // --- the resident tables, shared by the ledger and PakManager's fit check ---
    {
        MpakHeader6 h = zeroHeader();
        h.lenFaceDrawCmds = 8 * 1000; // 1000 faces
        h.iCount = 6;                 // 3 dwords = 12 bytes
        h.lenFaceRemap = 10;          // 20 bytes
        const uint64_t expected = 12 + 1000 * kNv2aMapFaceBytes + 1000 + 20
            + 1000 * kNv2aLightmapFaceBytes + 1000 * kNv2aPvsListFaceBytes;
        CHECK(nv2aMapTableBytes(h) == expected, "the tables include the lightmap tables and the visible-face list");
        // Only temporaries otherwise: the ledger is the tables plus the load.
        CHECK(nv2aMapLedgerBytes(h) == expected + 8 * 1000 + 10, "the ledger counts exactly nv2aMapTableBytes as its tables");

        // Codex review of R3: the whole-console fit check must count the same
        // tables. A map whose GPU buffers leave exactly room for the tables
        // without the lightmap and list bytes must fail once they count.
        Nv2aWorldLumps lumps;
        lumps.faces = 1000 * 20;
        const uint64_t withoutNew = expected - 1000 * (kNv2aLightmapFaceBytes + kNv2aPvsListFaceBytes);
        const uint64_t room = kXboxFreeAtWorldLoadBytes - nv2aEngineWorldBytes(lumps) - kXboxPrecacheBytes - kXboxSteadyMarginBytes;
        const uint64_t gpu = room - withoutNew;
        CHECK(nv2aCheckArchive(lumps, 2048, withoutNew, 0, gpu).ok, "boundary: fits when the lightmap and list bytes are left out");
        CHECK(!nv2aCheckArchive(lumps, 2048, nv2aMapTableBytes(h), 0, gpu).ok, "boundary: fails once the resident tables are counted in full");
    }

    // --- R5 (divergences.md #147): the water's static vertex buffer ---
    {
        // SubdividePolygon_r cuts the 128x128 square into four 64x64
        // quads (the ref/nv2a host test's own case): 16 vertices, 20 bytes
        // each (nv2a_warp_gpu_vertex_t). Only water names count, by the
        // engine's rule (Mod_LooksLikeWaterTexture) on the name the engine
        // lowercased first (Mod_LoadTexture): '!', '*' but not "*default",
        // "water", "laser".
        WaterBsp bsp({ "!water", "wall", "*DEFAULT", "LASER1", "Water2" });
        // A sixth texinfo naming a miptex past the lump: the engine's
        // Mod_LoadTexInfo gives it texture 0, "!water".
        for (int k = 0; k < 8; k++) put(bsp.texinfo, 0.0f);
        put(bsp.texinfo, static_cast<int32_t>(99));
        put(bsp.texinfo, static_cast<int32_t>(0));
        bsp.addFace(0);     // water: 16
        bsp.addFace(1);     // not water
        bsp.addFace(2);     // "*default" once lowercased: the engine's default texture
        bsp.addFace(3);     // laser: 16
        bsp.addFace(4);     // "water2" once lowercased: 16
        bsp.addFace(5);     // texture 0: 16
        bsp.addFace(0, 2);  // two edges: the renderer refuses it
        bsp.addFace(9);     // a texinfo past the lump: skipped, not read
        const Nv2aWaterCount wc = nv2aWaterCount(bsp.lumps());
        CHECK(wc.vertices == 64 && nv2aWaterVertexCount(bsp.lumps()) == 64, "four water faces of four quads each");
        // The CPU pieces: per face a 16-byte header, 4 piece counts and 16
        // vertices of 12 bytes = 212 bytes, a 248-byte zone chunk.
        CHECK(wc.polyBytes == 4 * nv2aZoneChunk(212) && nv2aZoneChunk(212) == 248, "the pieces' zone chunks");
        Nv2aWorldLumps withWater;
        withWater.waterPolyBytes = wc.polyBytes;
        CHECK(nv2aEngineWorldBytes(withWater) == nv2aEngineWorldBytes(Nv2aWorldLumps{}) + 4 * 248, "the pieces are engine world bytes");
        CHECK(nv2aWaterGpuBytes(32) == 32 * 20, "20 bytes a vertex (nv2a_warp_gpu_vertex_t)");
        CHECK(nv2aWaterGpuBytes(100000) == 65535ull * 20, "capped at the buffer's 65535 vertices, as the renderer caps it");
        CHECK(nv2aWaterVertexCount(Nv2aWaterLumps{}) == 0, "no lumps, no water");

        // The caps, in the renderer's order (Codex review of R5): 5000
        // water faces of 212 bytes and 16 vertices each. The 1 MiB piece
        // cap keeps 4946 faces (r_warp.c R_WarpSubdivideSurface); the faces
        // past it have no pieces and so no GPU place. Of the kept ones,
        // R_WarpGpuPlace fits 4095 faces (65520 vertices) under 65535.
        WaterBsp many({ "!water" });
        for (int k = 0; k < 5000; k++) many.addFace(0);
        const Nv2aWaterCount capped = nv2aWaterCount(many.lumps());
        CHECK(capped.polyBytes == 4946ull * 248, "the piece cap: only faces under 1 MiB keep pieces");
        CHECK(capped.vertices == 4095u * 16, "the GPU buffer: only kept faces, placed as the renderer places them");

        // A GPU-side term of the ledger: added to the vertex buffer,
        // texture data and lightmap, then the max against the load.
        MpakHeader6 h = zeroHeader();
        h.vCount = 10;              // 280 bytes of GPU buffer
        h.lenTextureDir = 400;      // a 400-byte load, which outweighs it alone
        CHECK(nv2aMapLedgerBytes(h) == 400 + 120, "without water the load is the larger side");
        CHECK(nv2aMapLedgerBytes(h, 640) == 280 + 640, "with water the GPU side is the larger one");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
