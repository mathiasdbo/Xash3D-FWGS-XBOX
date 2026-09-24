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
        // Everything else 0, so fileSize alone carries the whole ledger.
        const uint64_t exact = nv2aMapLedgerBytes(h, kNv2aMapBudgetBytes);
        CHECK(exact == kNv2aMapBudgetBytes, "exact-budget ledger must equal the cap");
        CHECK(nv2aMapWithinBudget(exact), "exactly-at-budget must PASS (strict > in the ref, not >=)");

        const uint64_t overByOne = nv2aMapLedgerBytes(h, kNv2aMapBudgetBytes + 1);
        CHECK(overByOne == kNv2aMapBudgetBytes + 1, "one-byte-over ledger must equal cap+1");
        CHECK(!nv2aMapWithinBudget(overByOne), "one byte over budget must FAIL");
    }

    // --- odd iCount rounding: dword_count = (iCount+1)/2, idxBytes = that * 4 ---
    {
        MpakHeader6 h = zeroHeader();
        h.iCount = 3; // (3+1)/2 = 2 dwords -> 8 bytes
        CHECK(nv2aMapLedgerBytes(h, 0) == 8, "iCount=3 must give 8 index bytes");

        h.iCount = 0; // (0+1)/2 = 0 dwords -> 0 bytes
        CHECK(nv2aMapLedgerBytes(h, 0) == 0, "iCount=0 must give 0 index bytes");
    }

    // --- faceDrawCmds/8 truncation (the per-face PVS visibility table) ---
    {
        MpakHeader6 h = zeroHeader();
        h.lenFaceDrawCmds = 15; // 15/8 = 1 face: 1*52 (faces term) + 15/8 (vis term, truncates to 1)
        CHECK(nv2aMapLedgerBytes(h, 0) == 1 * 52 + 1, "lenFaceDrawCmds=15 must give one 52-byte face and vis=1 (integer truncation)");
    }

    // --- the vertex term uses kNv2aLitVertexBytes (28), the RUNTIME nv2a_lit_vertex_t
    //     size, never sizeof(Vertex)/mpak_vertex_t, the smaller on-disk record ---
    {
        MpakHeader6 h = zeroHeader();
        h.vCount = 1;
        CHECK(kNv2aLitVertexBytes == 28, "nv2a_lit_vertex_t must be 28 bytes (packed float pos[3], uv0[2], uv1[2])");
        CHECK(nv2aMapLedgerBytes(h, 100) == 28 + 100, "vCount=1 must contribute exactly 28 bytes, plus fileSize");
    }

    // --- no 32-bit overflow: every term is computed in uint64, unlike the
    //     ref's own uint32 dword_count (Nv2aMapBudget.h's own comment on
    //     why that divergence is harmless for any real map) ---
    {
        MpakHeader6 h = zeroHeader();
        h.vCount = std::numeric_limits<uint32_t>::max();
        const uint64_t expected = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) * kNv2aLitVertexBytes;
        CHECK(nv2aMapLedgerBytes(h, 0) == expected, "vCount=UINT32_MAX must not overflow the vertex term");
        CHECK(!nv2aMapWithinBudget(nv2aMapLedgerBytes(h, 0)), "a UINT32_MAX vertex count must be far over budget");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
