// Unit test for Nv2aMapFit.h, the bake-time model of whether a map fits
// the Original Xbox: the edict-slot count, the per-allocation overhead,
// the engine-structure and prefix terms, and both verdicts.
#include "formats/Nv2aMapFit.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

uint32_t peak(const std::string& ents) { return nv2aPeakEdicts(ents.data(), ents.size()); }

} // namespace

int main() {
    // --- edict slots: world + one client + entities that survive Spawn ---
    CHECK(peak("{\"classname\" \"worldspawn\"}") == 2, "worldspawn alone: the world and one client");
    CHECK(peak("{\"classname\" \"worldspawn\"}{\"classname\" \"func_wall\"}") == 3, "a func_wall holds a slot");
    CHECK(peak("{\"classname\" \"light\"}{\"classname\" \"light_spot\"}{\"classname\" \"light_environment\"}") == 2,
          "unnamed lights remove themselves in CLight::Spawn");
    CHECK(peak("{\"classname\" \"light\" \"targetname\" \"sw1\"}") == 3, "a named light stays (it can be switched)");
    CHECK(peak("{\"classname\" \"info_null\"}{\"classname\" \"info_node\"}{\"classname\" \"info_node_air\"}") == 2,
          "info_null and nodes remove themselves in Spawn");
    CHECK(peak("{\"classname\" \"infodecal\"}") == 3, "infodecal holds its slot until its first think");
    CHECK(peak("{\"origin\" \"0 0 0\"}") == 2, "a block with no classname takes no slot");

    // --- per-allocation overhead: 24-byte header + sentinel, dlmalloc chunk ---
    CHECK(nv2aZoneChunk(0) == 32, "an empty allocation still costs a header and a chunk");
    CHECK(nv2aZoneChunk(100) == 136, "100 bytes -> 100 + 29, rounded up to 8");

    // --- classic prefix: first sector plus each lump padded to a sector ---
    {
        Nv2aWorldLumps l;
        CHECK(nv2aClassicPrefixBytes(l) == 2048, "no lumps: just the first sector");
        l.entities = 1;
        l.miptex = nv2aHeaderOnlyMiptexBytes(3); // 4 + 44*3 = 136
        CHECK(nv2aHeaderOnlyMiptexBytes(3) == 136, "headers-only miptex: count + directory + 40-byte headers");
        CHECK(nv2aClassicPrefixBytes(l) == 2048 * 3, "each lump rounds up to its own sector");
    }

    // --- engine structures: one term pinned (faces: 92 + 112 + 28 per face in three blocks) ---
    {
        Nv2aWorldLumps a, b;
        b.faces = 20 * 1000; // 1,000 dface_t
        const uint64_t delta = nv2aEngineWorldBytes(b) - nv2aEngineWorldBytes(a);
        const uint64_t expected = (nv2aZoneChunk(92000) - nv2aZoneChunk(0)) + (nv2aZoneChunk(112000) - nv2aZoneChunk(0))
            + (nv2aZoneChunk(28000) - nv2aZoneChunk(0));
        CHECK(delta == expected, "1,000 faces cost msurface_t + mextrasurf_t + mfacebevel_t, one block each");
    }

    // --- verdicts ---
    {
        Nv2aWorldLumps small;
        small.faces = 20 * 1000;
        CHECK(nv2aCheckBeforeBake(small, 500, 900).ok, "a small map with 500 slots fits");
        const Nv2aFit tooMany = nv2aCheckBeforeBake(small, 901, 900);
        CHECK(!tooMany.ok && tooMany.reason.find("entities") != std::string::npos, "901 slots of 900 fails, and says why");

        Nv2aWorldLumps huge;
        huge.lighting = 40ull * kMiB; // the engine's lighting copy alone is over budget
        const Nv2aFit c1 = nv2aCheckBeforeBake(huge, 10, 900);
        CHECK(!c1.ok && c1.reason.find("C1") != std::string::npos, "an engine load over the free memory fails C1");

        CHECK(nv2aCheckArchive(small, 2048, 1 * kMiB, 1 * kMiB, 4 * kMiB).ok, "a small map's renderer fits");
        const Nv2aFit c3 = nv2aCheckArchive(small, 2048, 1 * kMiB, 1 * kMiB, 30 * kMiB);
        CHECK(!c3.ok && c3.reason.find("C3") != std::string::npos, "30 MiB of GPU buffers fails C3");
        // The renderer's peak is tables + the larger of its temporaries and GPU
        // buffers - they never coexist.
        const uint64_t room = kXboxFreeAtWorldLoadBytes - nv2aEngineWorldBytes(small) - kXboxPrecacheBytes - kXboxSteadyMarginBytes;
        CHECK(nv2aCheckArchive(small, 2048, 0, room, room).ok, "temporaries and GPU buffers of exactly the room left fit: max, not sum");
        CHECK(!nv2aCheckArchive(small, 2048, 0, room, room + 1).ok, "one byte over fails");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
