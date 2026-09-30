// Unit test for Nv2aMapBudget.h's bake-time mirror of ref/nv2a's per-map
// memory ledger (r_context.c NV2A_MAP_BUDGET_BYTES,
// r_context.c nv2a_load_map_geometry the ledger formula and the strict `>` compare).
// Pins the formula term by term, since a single end-to-end example could
// hide a term with the wrong coefficient behind one with the opposite
// error.
#include "formats/Nv2aMapBudget.h"
#include <cstdio>
#include <cstdint>
#include <limits>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

MpakHeader6 zeroHeader() {
    // Every len*/count field defaults to 0 already (MpakHeader6's own
    // NSDMIs) - only the fields this test cares about are set per case.
    return MpakHeader6{};
}

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
        CHECK(kNv2aLightmapFaceBytes == 9, "r_lightmap.h NV2A_LM_FACE_BYTES: 6-byte block + held byte + 2-byte styled entry");
        CHECK(nv2aMapLedgerBytes(h) == 1 * 24 + 1 + 9 + 15, "lenFaceDrawCmds=15 must give one 24-byte face, vis=1 (integer truncation), 9 lightmap-table bytes and its 15-byte load copy");
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

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
