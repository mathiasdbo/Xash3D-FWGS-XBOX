// Unit tests for formats::lightmap_atlas::LightmapAtlasPacker (X9-R):
// tallest-first allocation, the trimmed atlas height the baker ships, and
// flatten()'s trimmed buffer. Expected values are worked out by hand from
// the packer's documented rules, never read back from the code under test.
#include "formats/LightmapAtlasPacker.h"

#include <cstdio>
#include <cstdlib>

using namespace formats::lightmap_atlas;

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// Page 0 starts with the ramp strip reserved: cursorX = 260, one row 4 high.
static void testTallestFirstAndStableTies() {
    LightmapAtlasPacker p;
    // Unbordered sizes; each block is placed bordered (+2 on each axis).
    // Request order: short, tall-narrow, tall-wide, short-again.
    std::vector<std::pair<uint32_t, uint32_t>> sizes = {{2, 2}, {2, 10}, {3, 10}, {2, 2}};
    std::vector<LightmapAllocation> a = p.allocateSorted(sizes);
    CHECK(a.size() == 4);
    // Sorted: {3,10} (tallest, wider), {2,10}, then the two {2,2} in request order.
    CHECK(a[2].blockX == 260 && a[2].blockY == 0); // 3+2 = 5 wide
    CHECK(a[1].blockX == 265 && a[1].blockY == 0); // 2+2 = 4 wide
    CHECK(a[0].blockX == 269 && a[0].blockY == 0); // first {2,2}
    CHECK(a[3].blockX == 273 && a[3].blockY == 0); // second {2,2}, after the first
    for (const auto& al : a) CHECK(al.page == 0);
    // Tallest bordered block is 12 rows; 12 is already a multiple of 4.
    CHECK(p.atlasHeight() == 12);
}

static void testHeightRoundsUpToMultipleOf4() {
    LightmapAtlasPacker p;
    // One block 399 luxels tall -> 401 bordered rows -> usedHeight 401 -> 404.
    // (LM_MAX_LUXELS is enforced by the caller, not the packer.)
    p.allocateSorted({{10, 399}});
    CHECK(p.atlasHeight() == 404);
}

static void testEmptyAtlasKeepsTheRamp() {
    LightmapAtlasPacker p;
    // No blocks: only the 4-row ramp strip, which the height must keep.
    CHECK(p.atlasHeight() == LM_RAMP_HEIGHT);
    uint32_t h = 0;
    std::vector<uint8_t> rgba = p.flatten(h);
    CHECK(h == LM_RAMP_HEIGHT);
    CHECK(rgba.size() == static_cast<size_t>(LM_PAGE_SIZE) * LM_RAMP_HEIGHT * 4);
    // The ramp's gradient: texel x on row 0 holds grey value x.
    CHECK(rgba[(0 * LM_PAGE_SIZE + 0) * 4] == 0);
    CHECK(rgba[(0 * LM_PAGE_SIZE + 200) * 4] == 200);
}

static void testFlattenTrimsToAtlasHeight() {
    LightmapAtlasPacker p;
    std::vector<LightmapAllocation> a = p.allocateSorted({{4, 4}});
    const uint8_t rgb[4 * 4 * 3] = {};
    p.writeBlock(a[0], 4, 4, rgb);
    uint32_t h = 0;
    std::vector<uint8_t> out = p.flatten(h);
    CHECK(h == 8); // 4 + 2 border = 6 rows -> rounded up to 8
    CHECK(out.size() == static_cast<size_t>(LM_PAGE_SIZE) * 8 * 4);
    // The block's first data luxel (black, from rgb) is inside the trimmed rows.
    const size_t off = (static_cast<size_t>(a[0].blockY + 1) * LM_PAGE_SIZE + a[0].blockX + 1) * 4;
    CHECK(out[off] == 0 && out[off + 3] == 255);
}

static void testMultiPageKeepsFullHeight() {
    LightmapAtlasPacker p;
    // 64x64 blocks are 66x66 bordered: 15 per 1024-wide row (the first row
    // starts after the 260-wide ramp, so 11), 15 rows per page. 300 blocks
    // cannot fit one page, so a second page opens.
    std::vector<std::pair<uint32_t, uint32_t>> sizes(300, {64, 64});
    std::vector<LightmapAllocation> a = p.allocateSorted(sizes);
    CHECK(p.pageCount() >= 2);
    CHECK(a.back().page >= 1);
    CHECK(p.atlasHeight() == LM_PAGE_SIZE);
    uint32_t h = 0;
    std::vector<uint8_t> out = p.flatten(h);
    CHECK(h == LM_PAGE_SIZE);
    CHECK(out.size() == static_cast<size_t>(p.pageCount()) * LM_PAGE_SIZE * LM_PAGE_SIZE * 4);
}

int main() {
    testTallestFirstAndStableTies();
    testHeightRoundsUpToMultipleOf4();
    testEmptyAtlasKeepsTheRamp();
    testFlattenTrimsToAtlasHeight();
    testMultiPageKeepsFullHeight();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("lightmap_atlas_packer_test: all passed");
    return EXIT_SUCCESS;
}
