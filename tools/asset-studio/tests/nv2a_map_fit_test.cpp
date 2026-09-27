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
    const uint32_t base = 2 + kSdkWorldEdicts; // the world, one client, soundent + bodyque
    CHECK(peak("{\"classname\" \"worldspawn\"}") == base, "worldspawn alone: the world, one client and the world's own edicts");
    CHECK(peak("{\"classname\" \"worldspawn\" \"netname\" \"#C1A0\"}") == base + 1, "a chapter title adds CWorld's env_message");
    CHECK(peak("{\"classname\" \"worldspawn\"}{\"classname\" \"func_wall\"}") == base + 1, "a func_wall holds a slot");
    CHECK(peak("{\"classname\" \"light\"}{\"classname\" \"light_spot\"}{\"classname\" \"light_environment\"}") == base,
          "unnamed lights remove themselves in CLight::Spawn");
    CHECK(peak("{\"classname\" \"light\" \"targetname\" \"sw1\"}") == base + 1, "a named light stays (it can be switched)");
    CHECK(peak("{\"classname\" \"info_null\"}{\"classname\" \"info_node\"}{\"classname\" \"info_node_air\"}") == base,
          "info_null and nodes remove themselves in Spawn");
    CHECK(peak("{\"classname\" \"infodecal\"}") == base + 1, "infodecal holds its slot until its first think");
    CHECK(peak("{\"origin\" \"0 0 0\"}") == base, "a block with no classname takes no slot");
    CHECK(peak("{\"classname\" \"env_message\" \"message\" \"a } b {\"}{\"classname\" \"func_wall\"}") == base + 2,
          "braces inside a quoted value do not end or start a block");

    // --- ref/nv2a's session-resident allocations come out of the measured free memory ---
    // The studio skin cache is the one bound on every map's skin memory
    // (Nv2aMapFit.h, kXboxStudioSkinCacheBytes); dropping it from the
    // resident sum is exactly the bug that approved c4a3 and disposal.
    CHECK(kXboxStudioSkinCacheBytes == 2ull * kMiB, "skin cache term mirrors NV2A_STUDIO_SKIN_CACHE_BYTES (2 MiB)");
    CHECK(kXboxRefNv2aResidentBytes == kXboxTextureArenaBytes + kXboxVertexRingBytes + kXboxTextureTableBytes
                                           + kXboxStudioStateBytes + kXboxStudioSkinCacheBytes,
          "every resident term, including the skin cache, is in the resident sum");
    CHECK(kXboxFreeAtWorldLoadBytes == 36ull * kMiB + kMiB / 4 - kXboxRefNv2aResidentBytes,
          "free-at-world-load is the 36.25 MiB measurement minus everything resident");

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
        CHECK(nv2aCheckBeforeBake(small, 900 - kXboxEdictHeadroom, 900).ok, "exactly the headroom left fits");
        const Nv2aFit tooMany = nv2aCheckBeforeBake(small, 901 - kXboxEdictHeadroom, 900);
        CHECK(!tooMany.ok && tooMany.reason.find("entities") != std::string::npos, "one slot into the headroom fails, and says why");

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

        // --- X9-W hosting: the baked PHS and the listen server's buffers ---
        const Nv2aFit noPhs = nv2aCheckHosting(small, 2048, 1 * kMiB, 1 * kMiB, 4 * kMiB, 0, 0, 100, 900, 2);
        CHECK(!noPhs.ok && noPhs.reason.find("no baked PHS") != std::string::npos, "a map without a baked PHS cannot be hosted");
        CHECK(nv2aCheckHosting(small, 2048, 1 * kMiB, 1 * kMiB, 4 * kMiB, 1000, 200 * 1024, 100, 900, 2).ok, "a small map hosts 2 players");

        // 2 players over single-player: server packet entities 2 x 64 x 64 x 340
        // - 4 x 64 x 340, one more sv_client_t, client packet entities 64 x 64 x 340
        // - 4 x 64 x 340, frames 2 x 64 x 6128 - 4 x 6128 (~4.3 MiB in all).
        const Nv2aHostingCost two = nv2aHostingCost(2);
        CHECK(two.beforeWorld == 2 * 64 * 64 * 340 - 4 * 64 * 340 + nv2aZoneChunk(91296), "2 players: server buffers before the world");
        CHECK(two.afterWorld == 64 * 64 * 340 - 4 * 64 * 340 + 2 * 64 * 6128 - 4 * 6128, "2 players: client buffers and frames");
        const Nv2aHostingCost three = nv2aHostingCost(3);
        const uint64_t perPlayer = (three.beforeWorld + three.afterWorld) - (two.beforeWorld + two.afterWorld);
        CHECK(perPlayer == 64 * 64 * 340 + nv2aZoneChunk(91296) + 64 * 6128, "each further player adds packet entities, a client slot and frames");

        const uint64_t rows = 1000, data = 100 * 1024;
        const uint64_t resident = nv2aZoneChunk(data) + nv2aZoneChunk(4 * rows);
        const uint64_t hostRoom = room - resident - two.beforeWorld - two.afterWorld;
        CHECK(nv2aCheckHosting(small, 2048, 0, hostRoom, hostRoom, rows, data, 100, 900, 2).ok,
              "the PHS and the server's buffers take exactly their size out of C3's room");
        const Nv2aFit tight = nv2aCheckHosting(small, 2048, 0, hostRoom + 1, hostRoom + 1, rows, data, 100, 900, 2);
        CHECK(!tight.ok && tight.reason.find("hosting C3") != std::string::npos, "one byte more than that fails hosting C3");
        const Nv2aFit bigPhs = nv2aCheckHosting(small, 2048, 0, 0, 0, rows, 40 * kMiB, 100, 900, 2);
        CHECK(!bigPhs.ok && bigPhs.reason.find("hosting C1") != std::string::npos, "a PHS too big to read at world load fails hosting C1");

        // Edicts: the first player's slot is in the peak; every player holds
        // up to kHostEdictsPerPlayer with its weapons.
        const uint32_t fullAt2 = 900 - kXboxEdictHeadroom - (2 * kHostEdictsPerPlayer - 1);
        CHECK(nv2aCheckHosting(small, 2048, 0, 0, 0, rows, data, fullAt2, 900, 2).ok, "exactly enough edict slots for 2 armed players");
        const Nv2aFit edicts = nv2aCheckHosting(small, 2048, 0, 0, 0, rows, data, fullAt2 + 1, 900, 2);
        CHECK(!edicts.ok && edicts.reason.find("hosting entities") != std::string::npos, "one slot short fails, and says why");

        std::string limit;
        CHECK(nv2aMaxHostPlayers(small, 2048, 0, 0, 0, rows, data, fullAt2, 900, limit) == 2 && !limit.empty(),
              "the player count stops where the edicts run out, with the reason");
        CHECK(nv2aMaxHostPlayers(small, 2048, 0, 0, 0, 0, 0, 100, 900, limit) == 0, "no PHS: not even two players");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
