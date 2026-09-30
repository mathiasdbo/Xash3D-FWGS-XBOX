// Unit test for the DXT1 skybox bake (X9-X item 14 step 10,
// docs/r3d/divergences.md #137, UiPacker::bakeSkyArt): the properties the
// engine and ref/nv2a need for a baked set to replace the retail TGAs.
//
// - Only complete six-side sets are written, under the engine's names
//   (<name>{rt,bk,lf,ft,up,dn}.dds, ref_common.c:33): CheckSkybox picks one
//   extension for all six sides (ref_common.c:71-100).
// - Rows come out top-down, as the engine's TGA loader gives them
//   (img_tga.c:138-146) - the fixture is a standard bottom-up TGA (the
//   descriptor's bit 5 clear, as retail's are), red on top, blue below.
// - Every side is a power-of-two size (ref/nv2a sizes DXT by log2
//   BASE_SIZE); a set that is not is left out.
// - The DDS is the header Image_LoadDDS accepts (menu_art_bake_test checks
//   the header itself; here only that it is the one makeDxt1Dds writes).
#include "formats/UiPacker.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

const char* const kSides[6] = {"rt", "bk", "lf", "ft", "up", "dn"};

// A 24-bit uncompressed TGA, bottom-up (descriptor 0): the first stored row
// is the bottom of the image. Top half red, bottom half blue.
void writeTgaRedOverBlue(const fs::path& path, int w, int h) {
    std::vector<uint8_t> file(18, 0);
    file[2] = 2;
    file[12] = static_cast<uint8_t>(w & 0xFF); file[13] = static_cast<uint8_t>(w >> 8);
    file[14] = static_cast<uint8_t>(h & 0xFF); file[15] = static_cast<uint8_t>(h >> 8);
    file[16] = 24;
    file[17] = 0;
    for (int storedRow = 0; storedRow < h; ++storedRow) {
        const int imageRow = h - 1 - storedRow;
        const bool top = imageRow < h / 2;
        for (int x = 0; x < w; ++x) {
            file.push_back(top ? 0 : 255); // B
            file.push_back(0);             // G
            file.push_back(top ? 255 : 0); // R
        }
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

std::vector<uint8_t> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint16_t u16(const std::vector<uint8_t>& b, size_t off) { return static_cast<uint16_t>(b[off] | (b[off + 1] << 8)); }

// Both endpoint colours of a solid block are that colour in RGB565.
bool blockIsSolid565(const std::vector<uint8_t>& dds, size_t blockIndex, uint16_t c565) {
    const size_t off = 128 + blockIndex * 8;
    return u16(dds, off) == c565 && (u16(dds, off + 2) == c565 || u16(dds, off + 4) == 0);
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "r3d_sky_art_bake_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path env = root / "game" / "gfx" / "env";
    fs::create_directories(env);

    for (int s = 0; s < 6; ++s) writeTgaRedOverBlue(env / (std::string("good") + kSides[s] + ".tga"), 32, 16);
    for (int s = 0; s < 6; ++s) writeTgaRedOverBlue(env / (std::string("npot") + kSides[s] + ".tga"), 12, 12);
    for (int s = 0; s < 5; ++s) writeTgaRedOverBlue(env / (std::string("part") + kSides[s] + ".tga"), 16, 16);

    int sets = -1;
    const bool ok = formats::UiPacker::bakeSkyArt((root / "game").string(), (root / "out").string(), sets);
    CHECK(ok, "bake succeeds");
    CHECK(sets == 1, "exactly the one complete power-of-two set is baked");

    const fs::path outEnv = root / "out" / "gfx" / "env";
    for (int s = 0; s < 6; ++s) {
        const fs::path good = outEnv / (std::string("good") + kSides[s] + ".dds");
        CHECK(fs::exists(good), "every side of the good set is written under the engine's name");
        CHECK(!fs::exists(outEnv / (std::string("npot") + kSides[s] + ".dds")), "a non-power-of-two set is left out");
        CHECK(!fs::exists(outEnv / (std::string("part") + kSides[s] + ".dds")), "an incomplete set is left out");
    }

    const std::vector<uint8_t> dds = readAll(outEnv / "goodrt.dds");
    // 32x16: 8x4 blocks of 8 bytes.
    CHECK(dds.size() == 128 + 8 * 4 * 8, "size = header + 32x16 DXT1 level 0");
    if (dds.size() == 128 + 8 * 4 * 8) {
        std::vector<uint8_t> blocks(dds.begin() + 128, dds.end());
        CHECK(dds == formats::UiPacker::makeDxt1Dds(32, 16, blocks), "header is makeDxt1Dds's");
        // Block rows 0-1 are the image's top 8 rows (red 0xF800), rows 2-3
        // the bottom (blue 0x001F): top-down, although the TGA is stored
        // bottom-up.
        CHECK(blockIsSolid565(dds, 0, 0xF800), "first block is red: rows top-down");
        CHECK(blockIsSolid565(dds, 8 * 3 + 7, 0x001F), "last block is blue");
    }

    int sets2 = -1;
    CHECK(!formats::UiPacker::bakeSkyArt((root / "missing").string(), (root / "out2").string(), sets2), "no gfx/env is an error");

    fs::remove_all(root, ec);
    if (failures) {
        std::printf("sky_art_bake_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sky_art_bake_test: all checks passed\n");
    return 0;
}
