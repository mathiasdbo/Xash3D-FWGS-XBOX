// Unit test for the DXT1 menu-background bake (docs/r3d/divergences.md
// #131, UiPacker::bakeBackgroundArt's dxt1 mode): the two pieces that have
// to be exactly right for ref/nv2a to draw the tiles at all.
//
// - potTileSpans: every tile must be a power of two on both axes, because
//   ref/nv2a sizes a DXT texture only by the FORMAT word's log2
//   BASE_SIZE_U/V fields and refuses anything else
//   (r_image.c nv2a_tex_upload_compressed); and the spans must cover the
//   canvas exactly, or the layout file places tiles on gaps/overlaps.
// - makeDxt1Dds: the header must be the one engine/common/imagelib/
//   img_dds.c Image_LoadDDS accepts, or PIC_Load fails and mainui drops
//   the whole background (BackgroundBitmap.cpp LoadSteamBackground aborts
//   on the first tile that does not load).
#include "formats/UiPacker.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

uint32_t u32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v;
    std::memcpy(&v, &b[off], 4);
    return v;
}

bool isPot(int v) { return v > 0 && (v & (v - 1)) == 0; }

void testSpans() {
    using formats::UiPacker;
    std::vector<int> w = UiPacker::potTileSpans(640, 256);
    CHECK(w.size() == 3 && w[0] == 256 && w[1] == 256 && w[2] == 128, "640 -> 256+256+128");
    std::vector<int> h = UiPacker::potTileSpans(480, 256);
    CHECK(h.size() == 4 && h[0] == 256 && h[1] == 128 && h[2] == 64 && h[3] == 32, "480 -> 256+128+64+32");

    for (int total = 4; total <= 2048; total += 4) {
        std::vector<int> s = UiPacker::potTileSpans(total, 256);
        int sum = 0;
        bool allPot = true, allBlocks = true;
        for (int v : s) {
            sum += v;
            allPot = allPot && isPot(v) && v <= 256;
            allBlocks = allBlocks && v >= 4;
        }
        if (sum != total || !allPot || !allBlocks) {
            std::printf("FAIL: spans of %d do not cover it with whole-block power-of-two tiles\n", total);
            ++failures;
            break;
        }
    }

    CHECK(UiPacker::potTileSpans(0, 256).empty(), "empty canvas -> no spans");
    CHECK(UiPacker::potTileSpans(640, 200).empty(), "non-power-of-two tile size is refused");
}

void testDds() {
    std::vector<uint8_t> blocks(64 * 32 / 16 * 8, 0xAB); // a 64x32 DXT1 image
    std::vector<uint8_t> dds = formats::UiPacker::makeDxt1Dds(64, 32, blocks);

    CHECK(dds.size() == 128 + blocks.size(), "file = 128-byte header + blocks");
    CHECK(std::memcmp(&dds[0], "DDS ", 4) == 0, "ident");
    CHECK(u32(dds, 4) == 124, "dwSize == sizeof(dds_t) - ident (img_dds.c header check)");
    CHECK(u32(dds, 12) == 32 && u32(dds, 16) == 64, "height, width");
    CHECK((u32(dds, 8) & 0x80000) != 0 && u32(dds, 20) == blocks.size(), "DDSD_LINEARSIZE and its size (Image_DXTCalcSize)");
    CHECK((u32(dds, 8) & 0x20000) == 0, "no DDSD_MIPMAPCOUNT: one level");
    CHECK(u32(dds, 32) == 0, "dwReserved1[0] (image.encode) is DXT_ENCODE_DEFAULT");
    CHECK(u32(dds, 76) == 32, "pixel format dwSize == sizeof(dds_pixf_t)");
    CHECK(u32(dds, 80) == 0x4 && std::memcmp(&dds[84], "DXT1", 4) == 0, "DDPF_FOURCC, DXT1");
    CHECK(std::memcmp(&dds[128], blocks.data(), blocks.size()) == 0, "blocks copied verbatim");
}

} // namespace

int main() {
    testSpans();
    testDds();
    if (failures == 0) {
        std::printf("menu_art_bake_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
