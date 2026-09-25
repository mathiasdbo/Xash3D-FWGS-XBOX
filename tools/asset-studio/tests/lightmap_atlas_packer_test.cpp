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

static void testTallPageInsteadOfSecondPage() {
    LightmapAtlasPacker p;
    // 64x64 blocks are 66x66 bordered: 15 per 1024-wide row (the first row
    // starts after the 260-wide ramp, so 11). 300 blocks need 21 rows of 66 =
    // 1386 rows (row 0 holds 11 after the ramp, then 15 a row), trimmed to 1388 - more than the old 1024-row page held. X9-T: they stay
    // on page 0, which grows and is trimmed to the rows used.
    std::vector<std::pair<uint32_t, uint32_t>> sizes(300, {64, 64});
    std::vector<LightmapAllocation> a = p.allocateSorted(sizes);
    CHECK(p.pageCount() == 1);
    CHECK(a.back().page == 0);
    CHECK(p.atlasHeight() == 1388);
    uint32_t h = 0;
    std::vector<uint8_t> out = p.flatten(h);
    CHECK(h == 1388);
    CHECK(out.size() == static_cast<size_t>(LM_PAGE_SIZE) * 1388 * 4);
}

static void testMultiPageKeepsFullHeight() {
    LightmapAtlasPacker p;
    // 62 rows of 66 luxels fill 4092 of the 4096 rows; a 63rd row opens
    // a second page, and every layer then keeps the full height.
    std::vector<std::pair<uint32_t, uint32_t>> sizes(15 * 63, {64, 64});
    std::vector<LightmapAllocation> a = p.allocateSorted(sizes);
    CHECK(p.pageCount() == 2);
    CHECK(a.back().page == 1);
    CHECK(p.atlasHeight() == LM_PAGE_MAX_HEIGHT);
    uint32_t h = 0;
    std::vector<uint8_t> out = p.flatten(h);
    CHECK(h == LM_PAGE_MAX_HEIGHT);
    CHECK(out.size() == static_cast<size_t>(p.pageCount()) * LM_PAGE_SIZE * LM_PAGE_MAX_HEIGHT * 4);
}

static void testTightPackingHasNoBorder() {
    LightmapAtlasPacker p(false);
    CHECK(p.border() == 0);
    // 4x2 luxels take exactly 4x2 tight (6x4 bordered), placed right after
    // the 260-wide ramp; the block's first data luxel is its origin.
    std::vector<LightmapAllocation> a = p.allocateSorted({{4, 2}, {4, 2}});
    CHECK(a[0].blockX == LM_RESERVED_MARGIN && a[0].blockY == 0);
    CHECK(a[1].blockX == LM_RESERVED_MARGIN + 4 && a[1].blockY == 0);
    uint8_t rgb[4 * 2 * 3];
    for (size_t i = 0; i < sizeof(rgb); ++i) rgb[i] = 7;
    p.writeBlock(a[0], 4, 2, rgb);
    uint32_t h = 0;
    std::vector<uint8_t> out = p.flatten(h);
    CHECK(h == 4); // the ramp's 4 rows; the 2-row blocks fit inside them
    const size_t first = (static_cast<size_t>(0) * LM_PAGE_SIZE + a[0].blockX) * 4;
    const size_t after = (static_cast<size_t>(0) * LM_PAGE_SIZE + a[0].blockX + 4) * 4;
    CHECK(out[first] == 7 && out[first + 3] == 255);
    CHECK(out[after] == 255); // the neighbour's origin is untouched (page starts white): no border written
    CHECK(LightmapAtlasPacker().border() == 1);
}

int main() {
    testTallestFirstAndStableTies();
    testHeightRoundsUpToMultipleOf4();
    testEmptyAtlasKeepsTheRamp();
    testFlattenTrimsToAtlasHeight();
    testTallPageInsteadOfSecondPage();
    testMultiPageKeepsFullHeight();
    testTightPackingHasNoBorder();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("lightmap_atlas_packer_test: all passed");
    return EXIT_SUCCESS;
}
