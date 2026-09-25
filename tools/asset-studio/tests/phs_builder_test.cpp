// X9-W: PhsBuilder.h against hand-worked expectations. The byte-for-byte
// proof against the engine's own Mod_CalcPHS over the retail maps is
// tools/test-phs.sh; these cases pin the transcribed rules on inputs small
// enough to check by hand, including the ones that look like accidents.
#include "../src/formats/PhsBuilder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace formats;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); ++failures; } } while (0)

// A models lump whose world model has `visleafs`.
static std::vector<uint8_t> modelsLump(int32_t visleafs) {
    std::vector<uint8_t> m(kDmodelBytes, 0);
    std::memcpy(m.data() + kDmodelVisleafs, &visleafs, sizeof(visleafs));
    return m;
}

// A leaves lump with one record per visofs.
static std::vector<uint8_t> leavesLump(const std::vector<int32_t>& visofs) {
    std::vector<uint8_t> l(visofs.size() * kDleafBytes, 0);
    for (size_t i = 0; i < visofs.size(); i++)
        std::memcpy(l.data() + i * kDleafBytes + kDleafVisofs, &visofs[i], sizeof(int32_t));
    return l;
}

static void testCompressRoundTrip() {
    const uint8_t row[] = { 0x05, 0, 0, 0, 0x80, 0 };
    std::vector<uint8_t> c;
    phsCompressRow(c, row, sizeof(row));
    const std::vector<uint8_t> want = { 0x05, 0, 3, 0x80, 0, 1 };
    CHECK(c == want, "compress: zero runs become {0, n}");
    uint8_t back[sizeof(row)] = {};
    CHECK(phsDecompressRow(back, c.data(), c.data() + c.size(), sizeof(row)), "decompress succeeds");
    CHECK(std::memcmp(back, row, sizeof(row)) == 0, "decompress inverts compress");

    std::vector<uint8_t> zeros(600, 0), cz;
    phsCompressRow(cz, zeros.data(), zeros.size());
    const std::vector<uint8_t> wantZ = { 0, 255, 0, 255, 0, 90 };
    CHECK(cz == wantZ, "compress: runs cap at 255");
}

static void testDecompressEdges() {
    uint8_t out[3] = {};
    CHECK(phsDecompressRow(out, nullptr, nullptr, 3) && out[0] == 0xFF && out[2] == 0xFF, "null input: all visible");
    const uint8_t runOff[] = { 0 };
    CHECK(!phsDecompressRow(out, runOff, runOff + 1, 3), "a run with no count byte is refused");
    const uint8_t zeroRun[] = { 0, 0, 7, 0, 5 };
    uint8_t z[3] = { 9, 9, 9 };
    CHECK(phsDecompressRow(z, zeroRun, zeroRun + sizeof(zeroRun), 3), "a zero-length run just consumes its bytes");
    CHECK(z[0] == 7 && z[1] == 0 && z[2] == 0, "zero-length run then data");
    const uint8_t longRun[] = { 0, 200 };
    uint8_t l[3] = { 9, 9, 9 };
    CHECK(phsDecompressRow(l, longRun, longRun + 2, 3) && l[0] == 0 && l[2] == 0, "a run is clamped to the row");
}

// Three visible leaves in a chain: 1 sees 2, 2 sees 1 and 3, 3 sees 2.
// PVS bit n is leaf n + 1, so leaf 1's row has bit 1 (leaf 2) set, etc.
// PHS(1) = PVS(1) | PVS(2) = bits {0,1,2}; PHS(3) likewise; PHS(2) =
// PVS(2) | PVS(1) | PVS(3) = bits {0,1,2}. Leaf 0 (visofs -1) is all 0xFF
// in its visbytes and its own OR only reaches leaves < count.
static void testChain() {
    // visleafs 3: visbytes 1, rowbytes 4, count 4.
    const std::vector<uint8_t> vis = {
        0x02,                 // leaf 1: sees leaf 2 (bit 1)
        0x05,                 // leaf 2: sees leaves 1 and 3 (bits 0, 2)
        0x02,                 // leaf 3: sees leaf 2
    };
    PhsData phs;
    std::string why;
    CHECK(buildPhs(vis, leavesLump({ -1, 0, 1, 2 }), modelsLump(3), phs, why), why.c_str());
    CHECK(phs.visleafs == 3 && phs.visbytes == 1 && phs.rowbytes == 4, "row geometry");
    CHECK(phs.offsets.size() == 4, "one offset per leaf, leaf 0 included");
    // Leaf 0: PVS 0xFF in byte 0, padding 0. Its set bits 0..2 OR in
    // leaves 1..3; bits 3..7 name leaves >= count and are skipped. Row:
    // ff 00 00 00 -> compressed ff 00 03.
    // Leaves 1-3: PHS 0x07 00 00 00 -> 07 00 03.
    const std::vector<uint8_t> want = { 0xFF, 0x00, 0x03, 0x07, 0x00, 0x03, 0x07, 0x00, 0x03, 0x07, 0x00, 0x03 };
    CHECK(phs.data == want, "chain PHS rows");
    CHECK(phs.offsets[0] == 0 && phs.offsets[1] == 3 && phs.offsets[2] == 6 && phs.offsets[3] == 9, "offsets are cumulative");
    CHECK(phsResidentBytes(phs) == 4 * 4 + want.size(), "resident bytes: 4-byte offsets plus rows");
}

static void testRefusals() {
    PhsData phs;
    std::string why;
    CHECK(!buildPhs({}, leavesLump({ -1, 0 }), modelsLump(1), phs, why), "no vis data: no PHS");
    CHECK(!buildPhs({ 0x01 }, leavesLump({ -1 }), modelsLump(1), phs, why), "visleafs + 1 leaves must exist");
    CHECK(!buildPhs({ 0x01 }, leavesLump({ -1, 5 }), modelsLump(1), phs, why), "visofs past the lump is refused");
    CHECK(!buildPhs({ 0x00 }, leavesLump({ -1, 0 }), modelsLump(1), phs, why), "a row running off the lump is refused");
    CHECK(phs.offsets.empty() && phs.data.empty(), "a refusal leaves no partial PHS");
}

int main() {
    testCompressRoundTrip();
    testDecompressEdges();
    testChain();
    testRefusals();
    if (failures) { std::printf("phs_builder_test: %d failure(s)\n", failures); return 1; }
    std::printf("phs_builder_test: all passed\n");
    return 0;
}
